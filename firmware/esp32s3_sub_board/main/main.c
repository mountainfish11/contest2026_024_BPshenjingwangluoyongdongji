/**
 * @file main.c
 * @brief ESP32-S3 Sub Board main entry point
 *
 * This is the main entry for the sub board ESP32-S3.
 * It communicates with the main board (ESP32-S3-EYE) via WiFi.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "atgm336h.h"
#include "m100m.h"
#include "max30102.h"
#include "vibration_motor.h"
#include "wifi.h"
#include "uart_link.h"
#include "speech_link.h"
#include "speech_output.h"
#include "volc_asr.h"

static const char *TAG = "SUB_BOARD";
static uint32_t s_m100_alarm_seq;
static TaskHandle_t s_fy_recognition_task;
static volatile bool s_voice_session_active;
static volatile bool s_voice_start_pending;
static volatile bool s_voice_stop_pending;
static volatile uint16_t s_voice_generation;
static TaskHandle_t s_health_task;
static volatile bool s_health_session_active;
static volatile uint16_t s_health_generation;
static size_t s_menu_speech_index;

static const char *const s_menu_speech_labels[] = {
    "视觉识别",
    "语音助手",
    "健康监测",
    "紧急报警",
    "系统设置",
    "功能测试",
};

#define FY_RECOGNITION_VOICE_FLAG 0x100U
#define FY_RECOGNITION_GENERATION_SHIFT 16U
#define VOICE_CHUNK_MS 100U
#define VOICE_START_CHUNKS 2U
#define VOICE_STOP_SILENCE_CHUNKS 7U
#define VOICE_MAX_DURATION_MS 10000U
#define VOICE_MEAN_ABS_THRESHOLD 500U
#define VOICE_PEAK_THRESHOLD 2500U
#define VOICE_SPEAKER_RECOVERY_MS 2500U
#define VOICE_SPEAKER_RECOVERY_POLL_MS 100U
#define MENU_AUDIO_RELEASE_MS 3000U
#define MENU_AUDIO_RELEASE_POLL_MS 20U
#define HEALTH_POLL_MS 250U
#define HEALTH_VALID_POLLS 4U
#define HEALTH_TASK_STACK_SIZE 4096U
#define HEALTH_TASK_PRIORITY 5U
#define HEALTH_SPEECH_WAIT_MS 15000U
#define VISION_VIBRATION_MS 120U
#define ALERT_VIBRATION_MS 120U
#define ALERT_VIBRATION_COUNT 3U
#define LOCAL_SPEECH_SEQUENCE_MASK UINT64_C(0x8000000000000000)

typedef struct {
    volc_asr_stream_t *stream;
    uint16_t generation;
    uint16_t speech_chunks;
    uint16_t silence_chunks;
    bool speech_started;
    bool cancelled;
    int send_error;
} voice_vad_context_t;

static void fy_recognition_task(void *arg);
static void health_monitor_task(void *arg);

static bool start_voice_recognition_task(uint16_t generation)
{
    uint32_t options = 3U | FY_RECOGNITION_VOICE_FLAG |
                       ((uint32_t)generation <<
                        FY_RECOGNITION_GENERATION_SHIFT);

    if (xTaskCreate(fy_recognition_task, "voice_recognition", 16384,
                    (void *)(uintptr_t)options, 5,
                    &s_fy_recognition_task) != pdPASS) {
        s_fy_recognition_task = NULL;
        return false;
    }

    return true;
}

static void stop_voice_session(bool notify_idle)
{
    bool task_active = s_fy_recognition_task != NULL;

    s_voice_session_active = false;
    UART_LINK_SetHeartbeatPaused(false);
    s_voice_start_pending = false;
    s_voice_generation++;

    if (task_active) {
        /* The streaming callback checks the generation every 100 ms and
         * exits promptly.  Preserve an earlier VOICE_STOP notification if a
         * following mode switch arrives before the task has unwound.
         */
        if (notify_idle) {
            s_voice_stop_pending = true;
        }
    } else {
        bool send_idle = notify_idle || s_voice_stop_pending;

        s_voice_stop_pending = false;
        if (send_idle) {
            (void)UART_LINK_SendVoiceIdle();
        }
    }
}

static void stop_health_session(void)
{
    s_health_session_active = false;
    s_health_generation++;

    /* The monitor never blocks in sensor I/O or playback.  It checks the
     * generation at least every HEALTH_POLL_MS and owns no resource after its
     * task handle becomes NULL.
     */

    for (uint32_t waited = 0;
         s_health_task != NULL && waited < HEALTH_POLL_MS * 2U;
         waited += 20U) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool health_session_current(uint16_t generation)
{
    return s_health_session_active && generation == s_health_generation;
}

static bool wait_health_speech(uint16_t generation, uint32_t delivered_before)
{
    for (uint32_t waited = 0; waited < HEALTH_SPEECH_WAIT_MS; waited += 50U) {
        speech_link_status_t status = {0};

        if (!health_session_current(generation)) {
            return false;
        }
        if (SPEECH_LINK_GetStatus(&status) == ESP_OK &&
            status.delivered > delivered_before) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static bool queue_health_and_wait(uint16_t generation, const char *text)
{
    speech_link_status_t before = {0};

    if (SPEECH_LINK_GetStatus(&before) != ESP_OK ||
        SPEECH_LINK_EnqueueHealth(text, 80) != ESP_OK) {
        return false;
    }
    return wait_health_speech(generation, before.delivered);
}

static void health_monitor_task(void *arg)
{
    uint16_t generation = (uint16_t)(uintptr_t)arg;
    max30102_data_t data = {0};
    unsigned int valid_polls = 0;
    int heart_rate_bpm = 0;
    char speech[64];

    if (!MAX30102_IsRunning()) {
        if (queue_health_and_wait(generation, "检测设备不可用") &&
            health_session_current(generation)) {
            (void)UART_LINK_SendHealthResult(0);
        }
        goto done;
    }

    if (!queue_health_and_wait(generation,
                               "开始检测，请将手指放在传感器上")) {
        goto done;
    }

    while (health_session_current(generation)) {
        if (MAX30102_GetData(&data) == ESP_OK && data.running &&
            data.sensor_present && data.finger_present &&
            data.heart_rate_valid && data.last_sample_age_ms < 1000U &&
            data.heart_rate_bpm >= 30.0f && data.heart_rate_bpm <= 220.0f) {
            valid_polls++;
            if (valid_polls >= HEALTH_VALID_POLLS) {
                heart_rate_bpm = (int)(data.heart_rate_bpm + 0.5f);
                break;
            }
        } else {
            valid_polls = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(HEALTH_POLL_MS));
    }

    if (!health_session_current(generation) || heart_rate_bpm <= 0) {
        goto done;
    }

    snprintf(speech, sizeof(speech), "检测完成，心率每分钟%d次",
             heart_rate_bpm);
    if (queue_health_and_wait(generation, speech) &&
        health_session_current(generation)) {
        (void)UART_LINK_SendHealthResult(heart_rate_bpm);
    }

done:
    if (generation == s_health_generation) {
        s_health_session_active = false;
    }
    s_health_task = NULL;
    vTaskDelete(NULL);
}

static bool start_health_session(void)
{
    uint16_t generation;

    stop_health_session();
    generation = ++s_health_generation;
    s_health_session_active = true;
    if (xTaskCreate(health_monitor_task, "health_monitor",
                    HEALTH_TASK_STACK_SIZE,
                    (void *)(uintptr_t)generation, HEALTH_TASK_PRIORITY,
                    &s_health_task) != pdPASS) {
        s_health_task = NULL;
        s_health_session_active = false;
        return false;
    }
    return true;
}

static void wait_menu_audio_resources(void)
{
    for (uint32_t waited = 0; waited < MENU_AUDIO_RELEASE_MS;
         waited += MENU_AUDIO_RELEASE_POLL_MS) {
        speech_output_status_t status = {0};

        if (s_fy_recognition_task == NULL &&
            SPEECH_OUTPUT_GetStatus(&status) == ESP_OK &&
            !status.capturing) {
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_AUDIO_RELEASE_POLL_MS));
    }

    ESP_LOGW(TAG, "Menu audio handoff timed out");
}

static esp_err_t send_m100_emergency(void)
{
    atgm336h_data_t gps = {0};
    max30102_data_t health = {0};
    char frame[256];

    if (ATGM336H_GetData(&gps) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    bool health_available = MAX30102_GetData(&health) == ESP_OK &&
                            health.running && health.sensor_present;
    bool heart_rate_valid = health_available && health.finger_present &&
                            health.heart_rate_valid;
    bool spo2_valid = health_available && health.finger_present &&
                      health.spo2_valid;
    uint32_t sequence = ++s_m100_alarm_seq;
    int length = snprintf(frame, sizeof(frame),
                          "BB1|ALARM|%lu|EMERGENCY|%u|%.7f|%.7f|%s|%s|"
                          "%u|%u|%u|%.1f|%u|%.1f",
                          (unsigned long)sequence, gps.valid ? 1U : 0U,
                          gps.latitude_deg, gps.longitude_deg,
                          gps.utc_time, gps.utc_date,
                          health_available ? 1U : 0U,
                          health.finger_present ? 1U : 0U,
                          heart_rate_valid ? 1U : 0U,
                          heart_rate_valid ? health.heart_rate_bpm : 0.0f,
                          spo2_valid ? 1U : 0U,
                          spo2_valid ? health.spo2_percent : 0.0f);
    if (length < 0 || (size_t)length >= sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = M100M_SendLine(frame);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "M100M alarm sent: seq=%lu gps_fix=%s HR=%s %.1f BPM "
                 "SpO2=%s %.1f%%",
                 (unsigned long)sequence, gps.valid ? "yes" : "no",
                 heart_rate_valid ? "valid" : "invalid",
                 heart_rate_valid ? health.heart_rate_bpm : 0.0f,
                 spo2_valid ? "valid" : "invalid",
                 spo2_valid ? health.spo2_percent : 0.0f);
    }
    return ret;
}

static void handle_main_board_flag(const char *flag, void *arg)
{
    const char *speech = NULL;
    speech_link_mode_t mode;

    (void)arg;
    ESP_LOGI(TAG, "Main-board control flag: %s", flag);

    if (strncmp(flag, "SPEECH_MODE_MENU_", 17) == 0 &&
        flag[17] >= '0' && flag[17] <= '5' && flag[18] == '\0') {
        s_menu_speech_index = (size_t)(flag[17] - '0');
        stop_voice_session(false);
        stop_health_session();
        wait_menu_audio_resources();
        mode = SPEECH_LINK_MODE_MENU;
        (void)SPEECH_LINK_SetMode(mode);
        (void)SPEECH_LINK_EnqueueMenu(
            s_menu_speech_labels[s_menu_speech_index], 60);
        return;
    } else if (strcmp(flag, "SPEECH_MODE_MENU") == 0) {
        stop_voice_session(false);
        stop_health_session();
        wait_menu_audio_resources();
        mode = SPEECH_LINK_MODE_MENU;
        (void)SPEECH_LINK_SetMode(mode);
        (void)SPEECH_LINK_EnqueueMenu(
            s_menu_speech_labels[s_menu_speech_index], 60);
        return;
    } else if (strcmp(flag, "SPEECH_MODE_VISION") == 0) {
        stop_voice_session(false);
        stop_health_session();
        mode = SPEECH_LINK_MODE_VISION;
        (void)SPEECH_LINK_SetMode(mode);
        (void)SPEECH_LINK_EnqueueVision("视觉服务已启动", 80);
        return;
    } else if (strcmp(flag, "SPEECH_MODE_VOICE") == 0) {
        stop_health_session();
        mode = SPEECH_LINK_MODE_VOICE;
        (void)SPEECH_LINK_SetMode(mode);
        return;
    } else if (strcmp(flag, "SPEECH_MODE_HEALTH") == 0) {
        stop_voice_session(false);
        mode = SPEECH_LINK_MODE_HEALTH;
        (void)SPEECH_LINK_SetMode(mode);
        return;
    } else if (strcmp(flag, "SPEECH_MODE_ALERT") == 0) {
        stop_voice_session(false);
        stop_health_session();
        mode = SPEECH_LINK_MODE_ALERT;
        (void)SPEECH_LINK_SetMode(mode);
        return;
    } else if (strcmp(flag, "SPEECH_MODE_SILENT") == 0) {
        stop_voice_session(false);
        stop_health_session();
        mode = SPEECH_LINK_MODE_SILENT;
        (void)SPEECH_LINK_SetMode(mode);
        return;
    } else if (strcmp(flag, "VOICE_STOP") == 0) {
        stop_voice_session(true);
        (void)SPEECH_LINK_SetMode(SPEECH_LINK_MODE_SILENT);
        return;
    } else if (strcmp(flag, "HEALTH_START") == 0) {
        stop_voice_session(false);
        (void)SPEECH_LINK_SetMode(SPEECH_LINK_MODE_HEALTH);
        if (!start_health_session()) {
            ESP_LOGE(TAG, "Health monitor task start failed");
            (void)UART_LINK_SendHealthResult(0);
        }
        return;
    } else if (strcmp(flag, "HEALTH_STOP") == 0) {
        stop_health_session();
        (void)SPEECH_LINK_SetMode(SPEECH_LINK_MODE_SILENT);
        return;
    } else if (strcmp(flag, "VOICE_START") == 0) {
        UART_LINK_SetHeartbeatPaused(false);
        s_voice_generation++;
        s_voice_session_active = true;
        s_voice_stop_pending = false;
        if (s_fy_recognition_task == NULL) {
            if (!start_voice_recognition_task(s_voice_generation)) {
                s_voice_session_active = false;
                ESP_LOGE(TAG, "Voice recognition task start failed");
            }
        } else {
            s_voice_start_pending = true;
        }
        return;
    } else if (strcmp(flag, "EMERGENCY_SEND") == 0) {
        esp_err_t ret = send_m100_emergency();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "M100M emergency trigger failed: %s",
                     esp_err_to_name(ret));
        } else {
            ret = VIBRATION_MOTOR_TriggerPattern(ALERT_VIBRATION_MS,
                                                 ALERT_VIBRATION_COUNT);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Alert vibration trigger failed: %s",
                         esp_err_to_name(ret));
            }
            ret = SPEECH_LINK_EnqueueAlert("紧急报警已发送", 90);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Alert confirmation speech failed: %s",
                         esp_err_to_name(ret));
            }
        }
        return;
    } else if (strcmp(flag, "BUTTON_MENU_NEXT") == 0) {
        s_menu_speech_index++;
        if (s_menu_speech_index >= sizeof(s_menu_speech_labels) /
                                  sizeof(s_menu_speech_labels[0])) {
            s_menu_speech_index = 0;
        }
        speech = s_menu_speech_labels[s_menu_speech_index];
    } else if (strcmp(flag, "MENU_VISION") == 0) {
        speech = "视觉识别";
    } else if (strcmp(flag, "MENU_VOICE") == 0) {
        speech = "语音助手";
    } else if (strcmp(flag, "MENU_HEALTH") == 0) {
        speech = "健康监测";
    } else if (strcmp(flag, "MENU_ALERT") == 0) {
        speech = "紧急报警";
    } else if (strcmp(flag, "MENU_SETTINGS") == 0) {
        speech = "系统设置";
    }

    if (speech != NULL) {
        esp_err_t ret = SPEECH_LINK_EnqueueMenu(speech, 60);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Menu speech queue failed: flag=%s error=%s",
                     flag, esp_err_to_name(ret));
        }
    }
}

static void handle_voice_speech(const char *text, void *arg)
{
    (void)arg;
    if (s_voice_session_active) {
        UART_LINK_SetHeartbeatPaused(false);
        esp_err_t ret = SPEECH_LINK_EnqueueVoice(text, 70);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Voice reply queue failed: %s",
                     esp_err_to_name(ret));
        }
    }
}

static void handle_speech_text(const char *text, uint8_t priority,
                               uint64_t sequence, speech_link_mode_t mode,
                               void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SPEECH TEXT: seq=%llu priority=%u text=%s",
             (unsigned long long)sequence, priority, text);

    /* Only host-originated Vision announcements produce haptic feedback.
     * The one-shot esp_timer turns the motor off asynchronously, so neither
     * speech playback nor the network/queue tasks wait for the pulse.
     */
    if (mode == SPEECH_LINK_MODE_VISION &&
        (sequence & LOCAL_SPEECH_SEQUENCE_MASK) == 0) {
        esp_err_t vibration_ret =
            VIBRATION_MOTOR_Trigger(VISION_VIBRATION_MS);
        if (vibration_ret != ESP_OK) {
            ESP_LOGW(TAG, "Vision vibration trigger failed: %s",
                     esp_err_to_name(vibration_ret));
        }
    }

    esp_err_t ret = SPEECH_OUTPUT_Speak(text);
    if (ret != ESP_OK && s_voice_session_active) {
        /* A transient FY-SP701 USB/UAC reset invalidates the old endpoint
         * handles.  The speech-output event task reopens both interfaces;
         * wait for that bounded recovery and retry this reply once.
         */
        for (uint32_t waited = 0;
             waited < VOICE_SPEAKER_RECOVERY_MS && s_voice_session_active;
             waited += VOICE_SPEAKER_RECOVERY_POLL_MS) {
            speech_output_status_t status = {0};

            vTaskDelay(pdMS_TO_TICKS(VOICE_SPEAKER_RECOVERY_POLL_MS));
            if (SPEECH_OUTPUT_GetStatus(&status) == ESP_OK &&
                status.speaker_ready && !status.speaking &&
                !status.capturing) {
                ret = SPEECH_OUTPUT_Speak(text);
                break;
            }
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Speech playback failed: seq=%llu error=%s",
                 (unsigned long long)sequence, esp_err_to_name(ret));
    }

    /* Playback failure must not leave VOICE permanently RUNNING but deaf.
     * Continue listening after a bounded playback attempt; VOICE_STOP can
     * still cancel the next capture through the generation check.
     */
    if (s_voice_session_active) {
        if (s_fy_recognition_task == NULL) {
            if (!start_voice_recognition_task(s_voice_generation)) {
                s_voice_session_active = false;
                ESP_LOGE(TAG, "Next voice turn start failed");
            }
        } else {
            /* A fast AI reply can finish playback while the previous ASR
             * task is still unwinding after queueing its result.  Preserve
             * the next-turn request so that task starts listening as soon
             * as the old task clears its handle.
             */
            s_voice_start_pending = true;
        }
    }
}

static bool voice_chunk_has_speech(const int16_t *samples,
                                   size_t sample_count)
{
    uint64_t absolute_sum = 0;
    uint32_t peak = 0;

    for (size_t i = 0; i < sample_count; i++) {
        int32_t value = samples[i];
        uint32_t absolute = value < 0 ? (uint32_t)-value : (uint32_t)value;

        absolute_sum += absolute;
        if (absolute > peak) {
            peak = absolute;
        }
    }
    if (sample_count == 0) {
        return false;
    }
    return absolute_sum / sample_count >= VOICE_MEAN_ABS_THRESHOLD ||
           peak >= VOICE_PEAK_THRESHOLD;
}

static bool voice_stream_chunk(const int16_t *samples, size_t sample_count,
                               void *arg)
{
    voice_vad_context_t *context = arg;
    bool voiced;

    if (!s_voice_session_active ||
        context->generation != s_voice_generation) {
        context->cancelled = true;
        return false;
    }
    voiced = voice_chunk_has_speech(samples, sample_count);
    context->send_error = VOLC_ASR_StreamSend(
        context->stream, (const uint8_t *)samples,
        sample_count * sizeof(int16_t));
    if (context->send_error != 0) {
        return false;
    }

    /* Keep the ASR WebSocket fed while local VAD waits for speech.  The
     * service ends a session if no audio packet arrives for several seconds;
     * sending each 100 ms microphone chunk avoids that timeout without
     * changing the local start/stop decisions.
     */

    if (!context->speech_started) {
        context->speech_chunks = voiced ? context->speech_chunks + 1 : 0;
        if (context->speech_chunks < VOICE_START_CHUNKS) {
            return true;
        }

        context->speech_started = true;
        context->silence_chunks = 0;
        return true;
    }

    context->silence_chunks = voiced ? 0 : context->silence_chunks + 1;
    return context->silence_chunks < VOICE_STOP_SILENCE_CHUNKS;
}

static int recognize_voice_streaming(uint16_t voice_generation)
{
    voice_vad_context_t *context;
    char text[512] = {0};
    size_t captured_bytes = 0;
    esp_err_t capture_ret;
    int ret;

    if (!VOLC_ASR_IsConfigured()) {
        ESP_LOGE(TAG, "Volcengine ASR is not configured");
        return -ENOENT;
    }
    context = heap_caps_calloc(1, sizeof(*context),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context == NULL) {
        return -ENOMEM;
    }
    context->generation = voice_generation;
    context->stream = VOLC_ASR_StreamOpen();
    if (context->stream == NULL) {
        heap_caps_free(context);
        return -ECONNREFUSED;
    }

    ESP_LOGI(TAG, "Voice listening: VAD 200 ms / silence 700 ms / max 10 s");
    capture_ret = SPEECH_OUTPUT_StreamPCM(
        VOICE_MAX_DURATION_MS, VOICE_CHUNK_MS, voice_stream_chunk, context,
        &captured_bytes);
    if (capture_ret != ESP_OK || context->cancelled ||
        !s_voice_session_active || voice_generation != s_voice_generation) {
        VOLC_ASR_StreamAbort(context->stream);
        ret = context->cancelled ? -ECANCELED : -EIO;
    } else if (context->send_error != 0) {
        VOLC_ASR_StreamAbort(context->stream);
        ret = context->send_error;
    } else if (!context->speech_started) {
        VOLC_ASR_StreamAbort(context->stream);
        ret = -ENODATA;
    } else {
        ret = VOLC_ASR_StreamFinish(context->stream, text, sizeof(text));
        if (ret == 0 && s_voice_session_active &&
            voice_generation == s_voice_generation) {
            ESP_LOGI(TAG, "ASR TEXT: %s", text);
            if (UART_LINK_SendASRResult(text) != ESP_OK) {
                ret = -EIO;
            } else {
                /* The main board can spend a long time in a blocking TLS
                 * handshake.  Do not declare the otherwise healthy local
                 * TCP connection dead while waiting for the AI reply. */
                UART_LINK_SetHeartbeatPaused(true);
            }
        }
    }
    heap_caps_free(context);
    return ret;
}

static int volc_asr_handler(const uint8_t *pcm, size_t pcm_len,
                            char *text_out, size_t text_cap, void *arg)
{
    (void)arg;
    if (!VOLC_ASR_IsConfigured()) {
        ESP_LOGW(TAG, "Volcengine ASR is not configured");
        return -ENOENT;
    }

    int ret = VOLC_ASR_Recognize(pcm, pcm_len, text_out, text_cap);
    if (ret == 0) {
        ESP_LOGI(TAG, "ASR TEXT: %s", text_out);
    } else {
        ESP_LOGE(TAG, "ASR failed: %d", ret);
    }
    return ret;
}

static int recognize_from_fy_microphone(uint32_t duration_seconds,
                                        bool voice_session,
                                        uint16_t voice_generation)
{
    const size_t pcm_capacity = duration_seconds * 16000U * sizeof(int16_t);
    uint8_t *pcm = heap_caps_malloc(
        pcm_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t pcm_len = 0;
    char text[512] = {0};

    if (pcm == NULL) {
        ESP_LOGE(TAG,
                 "FY PCM PSRAM allocation failed: need=%u psram_free=%u "
                 "internal_free=%u",
                 (unsigned)pcm_capacity,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return -ENOMEM;
    }

    ESP_LOGI(TAG, "Speech recognition started: %lu s",
             (unsigned long)duration_seconds);
    esp_err_t capture_ret = SPEECH_OUTPUT_CapturePCM(
        pcm, pcm_capacity, duration_seconds * 1000U, &pcm_len);
    if (capture_ret != ESP_OK) {
        ESP_LOGE(TAG, "FY microphone capture failed after %u bytes: %s",
                 (unsigned)pcm_len, esp_err_to_name(capture_ret));
        heap_caps_free(pcm);
        return -EIO;
    }

    const int16_t *samples = (const int16_t *)pcm;
    size_t sample_count = pcm_len / sizeof(int16_t);
    bool has_audio = false;

    for (size_t i = 0; i < sample_count; i++) {
        if (samples[i] != 0) {
            has_audio = true;
            break;
        }
    }
    if (!has_audio) {
        ESP_LOGE(TAG, "FY microphone returned all-zero PCM");
        heap_caps_free(pcm);
        return -ENODATA;
    }

    int ret = volc_asr_handler(pcm, pcm_len, text, sizeof(text), NULL);
    if (ret == 0 && (!voice_session ||
        (s_voice_session_active &&
         voice_generation == s_voice_generation))) {
        esp_err_t send_ret = UART_LINK_SendASRResult(text);
        if (send_ret != ESP_OK) {
            ESP_LOGE(TAG, "Queueing ASR result failed: %s",
                     esp_err_to_name(send_ret));
            ret = -EIO;
        }
    }
    heap_caps_free(pcm);
    return ret;
}

static void fy_recognition_task(void *arg)
{
    uint32_t options = (uint32_t)(uintptr_t)arg;
    uint32_t duration_seconds = options & 0xffU;
    bool voice_session = (options & FY_RECOGNITION_VOICE_FLAG) != 0;
    uint16_t voice_generation =
        (uint16_t)(options >> FY_RECOGNITION_GENERATION_SHIFT);

    int recognition_ret = voice_session ?
        recognize_voice_streaming(voice_generation) :
        recognize_from_fy_microphone(duration_seconds, false, 0);
    s_fy_recognition_task = NULL;
    if (voice_session && s_voice_stop_pending) {
        s_voice_stop_pending = false;
        (void)UART_LINK_SendVoiceIdle();
    }
    if (voice_session && s_voice_start_pending &&
        s_voice_session_active) {
        s_voice_start_pending = false;
        if (!start_voice_recognition_task(s_voice_generation)) {
            s_voice_session_active = false;
            ESP_LOGE(TAG, "Pending voice recognition start failed");
        }
    } else if (voice_session && recognition_ret == -ENODATA &&
               s_voice_session_active) {
        if (!start_voice_recognition_task(s_voice_generation)) {
            s_voice_session_active = false;
            ESP_LOGE(TAG, "Voice listening restart failed");
        }
    }
    vTaskDelete(NULL);
}

static void print_command_help(void)
{
    printf("Sub-board commands:\n");
    printf("  obstacle <distance_cm> <front|left|right> [confidence]\n");
    printf("  step_down <front|left|right> [confidence]\n");
    printf("  emergency\n");
    printf("  status\n");
    printf("  gps_status\n");
    printf("  m100_status\n");
    printf("  m100_send <text>\n");
    printf("  m100_alarm\n");
    printf("  max30102_status\n");
    printf("  vibration_once\n");
    printf("  pcm_status\n");
    printf("  speech_status\n");
    printf("  speech_test <Chinese text>\n");
    printf("  speech_recognize [1-4 seconds, default 3]\n");
    printf("  asr_status\n");
    printf("  asr_set <app_id> <token> <resource>\n");
}

static void handle_console_command(char *line)
{
    int distance_cm;
    int confidence = 100;
    char direction[16];
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (sscanf(line, "obstacle %d %15s %d", &distance_cm, direction,
               &confidence) >= 2) {
        ret = UART_LINK_SendObstacle(distance_cm, direction, confidence);
    } else if (sscanf(line, "step_down %15s %d", direction,
                      &confidence) >= 1) {
        ret = UART_LINK_SendStepDown(direction, confidence);
    } else if (strcmp(line, "emergency") == 0) {
        esp_err_t link_ret = UART_LINK_SendEmergency("console");
        esp_err_t m100_ret = send_m100_emergency();
        if (link_ret != ESP_OK) {
            ESP_LOGW(TAG, "Main-board emergency event rejected: %s",
                     esp_err_to_name(link_ret));
        }
        if (m100_ret != ESP_OK) {
            ESP_LOGW(TAG, "M100M emergency event rejected: %s",
                     esp_err_to_name(m100_ret));
        }
        return;
    } else if (strcmp(line, "status") == 0) {
        ESP_LOGI(TAG, "WiFi: %s, UART_LINK: %s",
                 wifi_is_connected() ? "connected" : "disconnected",
                 UART_LINK_IsConnected() ? "connected" : "disconnected");
        return;
    } else if (strcmp(line, "gps_status") == 0) {
        atgm336h_data_t gps;
        if (ATGM336H_GetData(&gps) != ESP_OK) {
            ESP_LOGE(TAG, "ATGM336H status unavailable");
            return;
        }
        ESP_LOGI(TAG,
                 "GNSS: running=%s fix=%s quality=%d satellites=%d "
                 "lat=%.7f lon=%.7f altitude=%.2fm speed=%.2fm/s "
                 "course=%.2f hdop=%.2f UTC=%s date=%s",
                 ATGM336H_IsRunning() ? "yes" : "no",
                 gps.valid ? "valid" : "invalid", gps.fix_quality,
                 gps.satellites, gps.latitude_deg, gps.longitude_deg,
                 gps.altitude_m, gps.speed_mps, gps.course_deg, gps.hdop,
                 gps.utc_time, gps.utc_date);
        ESP_LOGI(TAG,
                 "GNSS counters: sentences=%lu checksum_errors=%lu "
                 "parse_errors=%lu overflows=%lu data_age_ms=%lu "
                 "pps_count=%lu pps_age_ms=%lu",
                 (unsigned long)gps.sentences,
                 (unsigned long)gps.checksum_errors,
                 (unsigned long)gps.parse_errors,
                 (unsigned long)gps.line_overflows,
                 (unsigned long)gps.data_age_ms,
                 (unsigned long)gps.pps_count,
                 (unsigned long)gps.pps_age_ms);
        return;
    } else if (strcmp(line, "m100_status") == 0) {
        m100m_status_t status;
        if (M100M_GetStatus(&status) != ESP_OK) {
            ESP_LOGE(TAG, "M100M status unavailable");
            return;
        }
        ESP_LOGI(TAG,
                 "M100M: running=%s TX frames=%lu bytes=%lu errors=%lu "
                 "age_ms=%lu RX frames=%lu bytes=%lu overflows=%lu "
                 "age_ms=%lu",
                 status.running ? "yes" : "no",
                 (unsigned long)status.tx_frames,
                 (unsigned long)status.tx_bytes,
                 (unsigned long)status.tx_errors,
                 (unsigned long)status.last_tx_age_ms,
                 (unsigned long)status.rx_frames,
                 (unsigned long)status.rx_bytes,
                 (unsigned long)status.rx_overflows,
                 (unsigned long)status.last_rx_age_ms);
        return;
    } else if (strncmp(line, "m100_send ", 10) == 0) {
        ret = M100M_SendLine(line + 10);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "M100M test line sent: %s", line + 10);
        } else {
            ESP_LOGE(TAG, "M100M send failed: %s", esp_err_to_name(ret));
        }
        return;
    } else if (strcmp(line, "m100_alarm") == 0) {
        ret = send_m100_emergency();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "M100M alarm failed: %s", esp_err_to_name(ret));
        }
        return;
    } else if (strcmp(line, "max30102_status") == 0) {
        max30102_data_t sensor;
        if (MAX30102_GetData(&sensor) != ESP_OK) {
            ESP_LOGE(TAG, "MAX30102 status unavailable");
            return;
        }
        ESP_LOGI(TAG,
                 "MAX30102: running=%s present=%s part=0x%02x rev=0x%02x "
                 "finger=%s RED=%lu IR=%lu samples=%lu age_ms=%lu "
                 "FIFO_overflows=%lu I2C_errors=%lu",
                 sensor.running ? "yes" : "no",
                 sensor.sensor_present ? "yes" : "no", sensor.part_id,
                 sensor.revision_id,
                 sensor.finger_present ? "yes" : "no",
                 (unsigned long)sensor.red_raw,
                 (unsigned long)sensor.ir_raw,
                 (unsigned long)sensor.samples,
                 (unsigned long)sensor.last_sample_age_ms,
                 (unsigned long)sensor.fifo_overflows,
                 (unsigned long)sensor.i2c_errors);
        ESP_LOGI(TAG, "MAX30102 estimate: HR=%s %.1f BPM SpO2=%s %.1f%%",
                 sensor.heart_rate_valid ? "valid" : "invalid",
                 sensor.heart_rate_bpm,
                 sensor.spo2_valid ? "valid" : "invalid",
                 sensor.spo2_percent);
        return;
    } else if (strcmp(line, "vibration_once") == 0) {
        ret = VIBRATION_MOTOR_Trigger(500);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Single vibration test triggered: 500 ms");
        } else {
            ESP_LOGE(TAG, "Single vibration test failed: %s",
                     esp_err_to_name(ret));
        }
        return;
    } else if (strcmp(line, "pcm_status") == 0) {
        ESP_LOGI(TAG, "Last PCM transfer: %u bytes",
                 (unsigned)UART_LINK_GetPCMLen());
        return;
    } else if (strcmp(line, "speech_status") == 0) {
        speech_link_status_t link_status;
        speech_output_status_t output_status;
        if (SPEECH_LINK_GetStatus(&link_status) != ESP_OK ||
            SPEECH_OUTPUT_GetStatus(&output_status) != ESP_OK) {
            ESP_LOGE(TAG, "Speech link status unavailable");
            return;
        }
        ESP_LOGI(TAG,
                 "SPEECH_LINK: running=%s queued=%lu accepted=%lu "
                 "delivered=%lu duplicate=%lu rejected=%lu crc=%lu full=%lu",
                 link_status.running ? "yes" : "no",
                 (unsigned long)link_status.queued,
                 (unsigned long)link_status.accepted,
                 (unsigned long)link_status.delivered,
                 (unsigned long)link_status.duplicates,
                 (unsigned long)link_status.rejected,
                 (unsigned long)link_status.crc_errors,
                 (unsigned long)link_status.queue_full);
        ESP_LOGI(TAG,
                 "SPEECH_OUTPUT: init=%s tts=%s usb=%s speaker=%s "
                 "microphone=%s speaking=%s capturing=%s "
                 "speaker=%04x:%04x %" PRIu32
                 "Hz/%uch/%ubit completed=%lu failed=%lu last=%s",
                 output_status.initialized ? "yes" : "no",
                 output_status.tts_ready ? "yes" : "no",
                 output_status.usb_host_ready ? "yes" : "no",
                 output_status.speaker_ready ? "yes" : "no",
                 output_status.microphone_ready ? "yes" : "no",
                 output_status.speaking ? "yes" : "no",
                 output_status.capturing ? "yes" : "no",
                 output_status.speaker_vid, output_status.speaker_pid,
                 output_status.sample_rate, output_status.channels,
                 output_status.bits_per_sample,
                 (unsigned long)output_status.completed,
                 (unsigned long)output_status.failed,
                 esp_err_to_name(output_status.last_error));
        ESP_LOGI(TAG,
                 "FY_MIC: VID:PID=%04x:%04x %" PRIu32
                 "Hz/%uch/%ubit captures=%lu last_bytes=%u",
                 output_status.microphone_vid,
                 output_status.microphone_pid,
                 output_status.microphone_sample_rate,
                 output_status.microphone_channels,
                 output_status.microphone_bits_per_sample,
                 (unsigned long)output_status.captures_completed,
                 (unsigned)output_status.last_capture_bytes);
        return;
    } else if (strncmp(line, "speech_test ", 12) == 0) {
        ret = SPEECH_OUTPUT_Speak(line + 12);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Speech test failed: %s", esp_err_to_name(ret));
        }
        return;
    } else if (strncmp(line, "speech_recognize", 16) == 0 &&
               (line[16] == '\0' || line[16] == ' ')) {
        unsigned long seconds = 3;
        char *end = NULL;

        if (line[16] == ' ') {
            seconds = strtoul(line + 17, &end, 10);
            if (end == line + 17 || *end != '\0') {
                ESP_LOGW(TAG, "Usage: speech_recognize [1-4 seconds]");
                return;
            }
        }
        if (seconds < 1 || seconds > 4) {
            ESP_LOGW(TAG, "Capture duration must be between 1 and 4 seconds");
            return;
        }

        if (s_fy_recognition_task != NULL) {
            ESP_LOGW(TAG, "FY microphone recognition is already running");
            return;
        }
        if (xTaskCreate(fy_recognition_task, "fy_recognition", 16384,
                        (void *)(uintptr_t)seconds, 5,
                        &s_fy_recognition_task) != pdPASS) {
            s_fy_recognition_task = NULL;
            ESP_LOGE(TAG, "FY microphone recognition task start failed");
        }
        return;
    } else if (strcmp(line, "asr_status") == 0) {
        ESP_LOGI(TAG, "Volcengine ASR: %s",
                 VOLC_ASR_IsConfigured() ? "configured" : "not configured");
        return;
    } else if (strncmp(line, "asr_set ", 8) == 0) {
        char app_id[128];
        char token[256];
        char resource[128];
        if (sscanf(line + 8, "%127s %255s %127s",
                   app_id, token, resource) != 3) {
            ESP_LOGW(TAG, "Usage: asr_set <app_id> <token> <resource>");
            return;
        }
        ret = VOLC_ASR_SetCredentials(app_id, token, resource);
        memset(token, 0, sizeof(token));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Volcengine ASR credentials saved");
        } else {
            ESP_LOGE(TAG, "Saving ASR credentials failed: %s",
                     esp_err_to_name(ret));
        }
        return;
    } else if (strcmp(line, "help") == 0 || line[0] == '\0') {
        print_command_help();
        return;
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", line);
        print_command_help();
        return;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Event rejected: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Event queued: %s", line);
    }
}

typedef struct
{
    char line[640];
    size_t used;
} console_line_t;

static void console_write(const char *text)
{
    (void)uart_write_bytes(UART_NUM_0, text, strlen(text));
}

static void consume_console_byte(console_line_t *input, uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        console_write("\r\n");
        if (input->used > 0) {
            input->line[input->used] = '\0';
            handle_console_command(input->line);
            input->used = 0;
        }
        console_write("sub> ");
    } else if (ch == '\b' || ch == 0x7f) {
        if (input->used > 0) {
            input->used--;
            console_write("\b \b");
        }
    } else if (input->used + 1 < sizeof(input->line)) {
        input->line[input->used++] = (char)ch;
        (void)uart_write_bytes(UART_NUM_0, &ch, 1);
    } else {
        input->used = 0;
        ESP_LOGW(TAG, "Console command too long");
        console_write("\r\nsub> ");
    }
}

static void uart_console_task(void *arg)
{
    console_line_t input = {0};
    (void)arg;

    esp_err_t ret = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART console driver failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    console_write("\r\nsub> ");
    while (true) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        consume_console_byte(&input, ch);
    }
}

void app_main(void)
{
    esp_err_t ret;

    /* Keep normal operation concise while retaining warnings and failures
     * from the third-party protocol and USB audio components.
     */

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("tts_parser", ESP_LOG_WARN);
    esp_log_level_set("uac-host", ESP_LOG_WARN);

    ESP_LOGI(TAG, "ESP32-S3 Sub Board starting...");
    ret = ATGM336H_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ATGM336H initialization failed: %s",
                 esp_err_to_name(ret));
    }

    ret = M100M_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "M100M initialization failed: %s",
                 esp_err_to_name(ret));
    }

    ret = MAX30102_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 initialization failed: %s",
                 esp_err_to_name(ret));
    }

    ret = VIBRATION_MOTOR_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Vibration motor initialization failed: %s",
                 esp_err_to_name(ret));
    }

    ret = wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = wifi_connect(CONFIG_SUB_BOARD_WIFI_SSID,
                       CONFIG_SUB_BOARD_WIFI_PASSWORD);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi is not connected yet; automatic retry remains active");
    }

    ret = VOLC_ASR_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ASR initialization failed: %s", esp_err_to_name(ret));
    }

    ret = SPEECH_OUTPUT_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Speech output initialization failed: %s",
                 esp_err_to_name(ret));
    }

    SPEECH_LINK_SetHandler(handle_speech_text, NULL);
    ret = SPEECH_LINK_Start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPEECH_LINK start failed: %s", esp_err_to_name(ret));
    }

    UART_LINK_SetFlagHandler(handle_main_board_flag, NULL);
    UART_LINK_SetSpeechHandler(handle_voice_speech, NULL);
    UART_LINK_SetASRHandler(volc_asr_handler, NULL);
    ret = UART_LINK_Start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART_LINK start failed: %s", esp_err_to_name(ret));
    }

    if (xTaskCreate(uart_console_task, "uart_event_console", 4096, NULL, 4,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "UART event console start failed");
    }
    print_command_help();
    ESP_LOGI(TAG, "Sub Board initialized successfully");

    while (1) {
        /* Connection transitions and failures are logged by their owning
         * components.  Avoid periodic status output so the interactive
         * console prompt remains usable.
         */
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
