#include "speech_output.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

#define TTS_INPUT_RATE_HZ 16000U
#define TTS_SPEED 3U
#define UAC_EVENT_QUEUE_LEN 8
#define UAC_WRITE_TIMEOUT_MS 1500
#define UAC_BUFFER_SIZE 32768
#define UAC_BUFFER_THRESHOLD 4096
#define UAC_MIC_BUFFER_SIZE 32768
#define UAC_MIC_BUFFER_THRESHOLD 3200
#define UAC_MIC_READ_TIMEOUT_MS 1500
#define UAC_MIC_MAX_DURATION_MS 4000
#define UAC_MIC_STREAM_MAX_DURATION_MS 10000
#define UAC_MIC_STREAM_MAX_CHUNK_MS 200
#define UAC_MIC_STREAM_READ_TIMEOUT_MS 250

typedef enum {
    OUTPUT_EVENT_DRIVER,
    OUTPUT_EVENT_DEVICE,
} output_event_type_t;

typedef struct {
    output_event_type_t type;
    union {
        struct {
            uint8_t address;
            uint8_t interface_number;
            uac_host_driver_event_t event;
        } driver;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t event;
        } device;
    } data;
} output_event_t;

static const char *TAG = "SPEECH_OUTPUT";
static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_device_mutex;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static speech_output_status_t s_status;
static uac_host_device_handle_t s_speaker;
static uac_host_device_handle_t s_microphone;
static esp_tts_voice_t *s_voice;
static esp_tts_handle_t s_tts;
static esp_partition_mmap_handle_t s_voice_mmap;
static uint8_t s_capture_raw[UAC_MIC_BUFFER_THRESHOLD];
static int16_t s_stream_mono[(TTS_INPUT_RATE_HZ *
                              UAC_MIC_STREAM_MAX_CHUNK_MS) / 1000];

static void set_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.last_error = error;
    if (error != ESP_OK) {
        s_status.failed++;
    }
    portEXIT_CRITICAL(&s_status_lock);
}

static bool frequency_supported(const uac_host_dev_alt_param_t *alt,
                                uint32_t frequency)
{
    if (alt->sample_freq_type == 0) {
        return frequency >= alt->sample_freq_lower &&
               frequency <= alt->sample_freq_upper;
    }
    for (uint8_t i = 0; i < alt->sample_freq_type; i++) {
        if (alt->sample_freq[i] == frequency) {
            return true;
        }
    }
    return false;
}

static uint32_t choose_frequency(const uac_host_dev_alt_param_t *alt)
{
    static const uint32_t preferred[] = {16000, 48000, 44100, 32000};

    for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
        if (frequency_supported(alt, preferred[i])) {
            return preferred[i];
        }
    }
    if (alt->sample_freq_type > 0) {
        return alt->sample_freq[0];
    }
    return alt->sample_freq_lower;
}

static esp_err_t choose_alt_setting(uac_host_device_handle_t handle,
                                    uac_host_dev_alt_param_t *selected,
                                    uint32_t *frequency)
{
    uac_host_dev_info_t info;
    esp_err_t ret = uac_host_get_device_info(handle, &info);
    if (ret != ESP_OK) {
        return ret;
    }

    bool found = false;
    uint32_t best_score = UINT32_MAX;
    for (uint8_t index = 1; index <= info.iface_alt_num; index++) {
        uac_host_dev_alt_param_t alt;
        if (uac_host_get_device_alt_param(handle, index, &alt) != ESP_OK ||
            alt.format != 1 || alt.bit_resolution != 16 ||
            alt.subframe_size != 2 || alt.channels == 0 ||
            alt.channels > 2) {
            continue;
        }
        uint32_t candidate = choose_frequency(&alt);
        uint32_t score = candidate == TTS_INPUT_RATE_HZ ? 0 :
                         candidate > TTS_INPUT_RATE_HZ ?
                         candidate - TTS_INPUT_RATE_HZ :
                         TTS_INPUT_RATE_HZ - candidate;
        if (!found || score < best_score) {
            *selected = alt;
            *frequency = candidate;
            best_score = score;
            found = true;
        }
    }
    return found ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static void uac_device_callback(uac_host_device_handle_t handle,
                                const uac_host_device_event_t event, void *arg)
{
    (void)arg;
    output_event_t message = {
        .type = OUTPUT_EVENT_DEVICE,
        .data.device.handle = handle,
        .data.device.event = event,
    };
    (void)xQueueSend(s_event_queue, &message, 0);
}

static void uac_driver_callback(uint8_t address, uint8_t interface_number,
                                const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    output_event_t message = {
        .type = OUTPUT_EVENT_DRIVER,
        .data.driver.address = address,
        .data.driver.interface_number = interface_number,
        .data.driver.event = event,
    };
    (void)xQueueSend(s_event_queue, &message, 0);
}

static void open_speaker(uint8_t address, uint8_t interface_number)
{
    const uac_host_device_config_t config = {
        .addr = address,
        .iface_num = interface_number,
        .buffer_size = UAC_BUFFER_SIZE,
        .buffer_threshold = UAC_BUFFER_THRESHOLD,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    uac_host_device_handle_t handle = NULL;
    uac_host_dev_info_t info;
    uac_host_dev_alt_param_t alt;
    uint32_t frequency = 0;
    esp_err_t ret = uac_host_device_open(&config, &handle);

    if (ret == ESP_OK) {
        ret = uac_host_get_device_info(handle, &info);
    }
    if (ret == ESP_OK) {
        ret = choose_alt_setting(handle, &alt, &frequency);
    }
    if (ret == ESP_OK) {
        const uac_host_stream_config_t stream = {
            .channels = alt.channels,
            .bit_resolution = alt.bit_resolution,
            .sample_freq = frequency,
            .flags = 0,
        };
        ret = uac_host_device_start(handle, &stream);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UAC speaker start failed: %s", esp_err_to_name(ret));
        if (handle != NULL) {
            (void)uac_host_device_close(handle);
        }
        set_error(ret);
        return;
    }

    (void)uac_host_device_set_volume(handle, 100);
    (void)uac_host_device_set_mute(handle, false);
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (s_speaker != NULL) {
        xSemaphoreGive(s_device_mutex);
        ESP_LOGW(TAG, "Ignoring additional UAC speaker interface");
        (void)uac_host_device_close(handle);
        return;
    }
    s_speaker = handle;
    portENTER_CRITICAL(&s_status_lock);
    s_status.speaker_ready = true;
    s_status.speaker_vid = info.VID;
    s_status.speaker_pid = info.PID;
    s_status.sample_rate = frequency;
    s_status.channels = alt.channels;
    s_status.bits_per_sample = alt.bit_resolution;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
    xSemaphoreGive(s_device_mutex);
    ESP_LOGI(TAG, "UAC speaker ready: %" PRIu32 " Hz, %u ch, %u bit",
             frequency, alt.channels, alt.bit_resolution);
}

static void open_microphone(uint8_t address, uint8_t interface_number)
{
    const uac_host_device_config_t config = {
        .addr = address,
        .iface_num = interface_number,
        .buffer_size = UAC_MIC_BUFFER_SIZE,
        .buffer_threshold = UAC_MIC_BUFFER_THRESHOLD,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    uac_host_device_handle_t handle = NULL;
    uac_host_dev_info_t info;
    uac_host_dev_alt_param_t alt;
    uint32_t frequency = 0;
    esp_err_t ret = uac_host_device_open(&config, &handle);

    if (ret == ESP_OK) {
        ret = uac_host_get_device_info(handle, &info);
    }
    if (ret == ESP_OK) {
        ret = choose_alt_setting(handle, &alt, &frequency);
    }
    if (ret == ESP_OK && frequency != TTS_INPUT_RATE_HZ) {
        ESP_LOGE(TAG, "UAC microphone has no 16 kHz mode");
        ret = ESP_ERR_NOT_SUPPORTED;
    }
    if (ret == ESP_OK) {
        const uac_host_stream_config_t stream = {
            .channels = alt.channels,
            .bit_resolution = alt.bit_resolution,
            .sample_freq = frequency,
            .flags = 0,
        };
        ret = uac_host_device_start(handle, &stream);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UAC microphone start failed: %s",
                 esp_err_to_name(ret));
        if (handle != NULL) {
            (void)uac_host_device_close(handle);
        }
        set_error(ret);
        return;
    }

    esp_err_t mute_ret = uac_host_device_set_mute(handle, false);
    esp_err_t volume_ret = uac_host_device_set_volume(handle, 100);

    if (mute_ret != ESP_OK || volume_ret != ESP_OK) {
        ESP_LOGW(TAG, "UAC microphone gain setup: mute=%s volume=%s",
                 esp_err_to_name(mute_ret), esp_err_to_name(volume_ret));
    }

    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (s_microphone != NULL) {
        xSemaphoreGive(s_device_mutex);
        ESP_LOGW(TAG, "Ignoring additional UAC microphone interface");
        (void)uac_host_device_close(handle);
        return;
    }
    s_microphone = handle;
    portENTER_CRITICAL(&s_status_lock);
    s_status.microphone_ready = true;
    s_status.microphone_vid = info.VID;
    s_status.microphone_pid = info.PID;
    s_status.microphone_sample_rate = frequency;
    s_status.microphone_channels = alt.channels;
    s_status.microphone_bits_per_sample = alt.bit_resolution;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
    xSemaphoreGive(s_device_mutex);
    ESP_LOGI(TAG, "UAC microphone ready: %" PRIu32 " Hz, %u ch, %u bit",
             frequency, alt.channels, alt.bit_resolution);
}

static void close_speaker(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (handle == s_speaker) {
        s_speaker = NULL;
        portENTER_CRITICAL(&s_status_lock);
        s_status.speaker_ready = false;
        s_status.speaking = false;
        portEXIT_CRITICAL(&s_status_lock);
    }
    xSemaphoreGive(s_device_mutex);
    (void)uac_host_device_close(handle);
    ESP_LOGW(TAG, "UAC audio interface disconnected");
}

static void close_microphone(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (handle == s_microphone) {
        s_microphone = NULL;
        portENTER_CRITICAL(&s_status_lock);
        s_status.microphone_ready = false;
        s_status.capturing = false;
        portEXIT_CRITICAL(&s_status_lock);
    }
    xSemaphoreGive(s_device_mutex);
    (void)uac_host_device_close(handle);
    ESP_LOGW(TAG, "UAC microphone interface disconnected");
}

static void uac_event_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const uac_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = uac_driver_callback,
        .callback_arg = NULL,
    };
    esp_err_t ret = uac_host_install(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UAC driver install failed: %s", esp_err_to_name(ret));
        set_error(ret);
        vTaskDelete(NULL);
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status.usb_host_ready = true;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "USB Audio Host ready; waiting for FY-SP701");

    output_event_t message;
    while (xQueueReceive(s_event_queue, &message, portMAX_DELAY) == pdTRUE) {
        if (message.type == OUTPUT_EVENT_DRIVER &&
            message.data.driver.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
            open_speaker(message.data.driver.address,
                         message.data.driver.interface_number);
        } else if (message.type == OUTPUT_EVENT_DRIVER &&
                   message.data.driver.event ==
                       UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
            open_microphone(message.data.driver.address,
                            message.data.driver.interface_number);
        } else if (message.type == OUTPUT_EVENT_DEVICE &&
                   message.data.device.event ==
                       UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
            if (message.data.device.handle == s_microphone) {
                close_microphone(message.data.device.handle);
            } else {
                close_speaker(message.data.device.handle);
            }
        } else if (message.type == OUTPUT_EVENT_DEVICE &&
                   message.data.device.event ==
                       UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
            ESP_LOGE(TAG, "UAC transfer error");
            set_error(ESP_FAIL);
        }
    }
}

static void usb_host_task(void *arg)
{
    TaskHandle_t event_task = (TaskHandle_t)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t ret = usb_host_install(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(ret));
        set_error(ret);
        vTaskDelete(NULL);
        return;
    }
    xTaskNotifyGive(event_task);
    while (true) {
        uint32_t event_flags = 0;
        (void)usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

static esp_err_t init_tts(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const void *voice_data = NULL;
    esp_err_t ret = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &voice_data,
                                       &s_voice_mmap);
    if (ret != ESP_OK) {
        return ret;
    }
    s_voice = esp_tts_voice_set_init(&esp_tts_voice_template,
                                     (void *)voice_data);
    if (s_voice == NULL) {
        return ESP_FAIL;
    }
    s_tts = esp_tts_create(s_voice);
    if (s_tts == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status.tts_ready = true;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "ESP-TTS ready: 16 kHz mono Chinese synthesis");
    return ESP_OK;
}

static esp_err_t write_resampled(const int16_t *input, size_t input_samples,
                                 uint32_t output_rate, uint8_t channels)
{
    if (output_rate == 0 || channels == 0 || channels > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t output_frames =
        (input_samples * (size_t)output_rate + TTS_INPUT_RATE_HZ - 1) /
        TTS_INPUT_RATE_HZ;
    if (output_frames == 0 || output_frames > SIZE_MAX / channels /
        sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t output_samples = output_frames * channels;
    int16_t *output = malloc(output_samples * sizeof(*output));
    if (output == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t frame = 0; frame < output_frames; frame++) {
        size_t source_index =
            (frame * (size_t)TTS_INPUT_RATE_HZ) / output_rate;
        if (source_index >= input_samples) {
            source_index = input_samples - 1;
        }
        for (uint8_t channel = 0; channel < channels; channel++) {
            output[frame * channels + channel] = input[source_index];
        }
    }
    esp_err_t ret = uac_host_device_write(
        s_speaker, (uint8_t *)output,
        (uint32_t)(output_samples * sizeof(*output)),
        pdMS_TO_TICKS(UAC_WRITE_TIMEOUT_MS));
    free(output);
    return ret;
}

esp_err_t SPEECH_OUTPUT_Init(void)
{
    if (s_status.initialized) {
        return ESP_OK;
    }
    s_event_queue = xQueueCreate(UAC_EVENT_QUEUE_LEN, sizeof(output_event_t));
    s_device_mutex = xSemaphoreCreateMutex();
    if (s_event_queue == NULL || s_device_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = init_tts();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-TTS initialization failed: %s",
                 esp_err_to_name(ret));
        set_error(ret);
        return ret;
    }

    TaskHandle_t event_task = NULL;
    if (xTaskCreate(uac_event_task, "uac_events", 6144, NULL, 5,
                    &event_task) != pdPASS ||
        xTaskCreate(usb_host_task, "usb_host", 4096, event_task, 5,
                    NULL) != pdPASS) {
        set_error(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status.initialized = true;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

esp_err_t SPEECH_OUTPUT_Speak(const char *utf8_text)
{
    if (utf8_text == NULL || utf8_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (s_speaker == NULL || s_tts == NULL) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.speaking = true;
    uint32_t output_rate = s_status.sample_rate;
    uint8_t channels = s_status.channels;
    portEXIT_CRITICAL(&s_status_lock);

    esp_err_t ret = ESP_OK;
    if (!esp_tts_parse_chinese(s_tts, utf8_text)) {
        ret = ESP_ERR_INVALID_ARG;
    } else {
        while (ret == ESP_OK) {
            int samples = 0;
            int16_t *pcm = esp_tts_stream_play(s_tts, &samples, TTS_SPEED);
            if (samples <= 0) {
                break;
            }
            if (pcm == NULL) {
                ret = ESP_FAIL;
                break;
            }
            ret = write_resampled(pcm, (size_t)samples, output_rate,
                                  channels);
        }
    }
    esp_tts_stream_reset(s_tts);
    portENTER_CRITICAL(&s_status_lock);
    s_status.speaking = false;
    if (ret == ESP_OK) {
        s_status.completed++;
        s_status.last_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_status_lock);
    xSemaphoreGive(s_device_mutex);
    if (ret != ESP_OK) {
        set_error(ret);
    }
    return ret;
}

esp_err_t SPEECH_OUTPUT_CapturePCM(uint8_t *pcm, size_t capacity,
                                   uint32_t duration_ms,
                                   size_t *captured_bytes)
{
    uint8_t *raw = s_capture_raw;
    size_t target_bytes;
    size_t output_used = 0;
    uint8_t channels;
    esp_err_t ret;

    if (pcm == NULL || captured_bytes == NULL || duration_ms == 0 ||
        duration_ms > UAC_MIC_MAX_DURATION_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    target_bytes = ((size_t)TTS_INPUT_RATE_HZ * sizeof(int16_t) *
                    duration_ms) / 1000;
    if (capacity < target_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    *captured_bytes = 0;

    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (s_microphone == NULL || s_status.speaking || s_status.capturing) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    channels = s_status.microphone_channels;
    if (s_status.microphone_sample_rate != TTS_INPUT_RATE_HZ ||
        s_status.microphone_bits_per_sample != 16 ||
        (channels != 1 && channels != 2)) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.capturing = true;
    portEXIT_CRITICAL(&s_status_lock);

    /* Keep this FY-SP701 microphone stream active.  Resuming its suspended
     * isochronous interface can fault inside the ESP32-S3 HCD.  Discard a
     * bounded amount of queued audio so each command starts with fresh PCM.
     */

    ret = ESP_OK;
    for (unsigned int i = 0; i < 8; i++) {
        uint32_t discarded = 0;

        if (uac_host_device_read(s_microphone, raw, sizeof(s_capture_raw),
                                 &discarded, 0) != ESP_OK ||
            discarded == 0) {
            break;
        }
    }

    while (ret == ESP_OK && output_used < target_bytes) {
        uint32_t raw_read = 0;
        size_t request = target_bytes - output_used;

        if (channels == 2) {
            request *= 2;
        }
        if (request > sizeof(s_capture_raw)) {
            request = sizeof(s_capture_raw);
        }
        request -= request % (channels * sizeof(int16_t));

        ret = uac_host_device_read(s_microphone, raw, request, &raw_read,
                                   pdMS_TO_TICKS(UAC_MIC_READ_TIMEOUT_MS));
        if (ret != ESP_OK) {
            break;
        }
        raw_read -= raw_read % (channels * sizeof(int16_t));
        if (channels == 1) {
            size_t copy = raw_read;
            if (copy > target_bytes - output_used) {
                copy = target_bytes - output_used;
            }
            memcpy(pcm + output_used, raw, copy);
            output_used += copy;
        } else {
            const int16_t *samples = (const int16_t *)raw;
            int16_t *output = (int16_t *)(pcm + output_used);
            size_t frames = raw_read / (2 * sizeof(int16_t));
            size_t room = (target_bytes - output_used) / sizeof(int16_t);

            if (frames > room) {
                frames = room;
            }
            for (size_t i = 0; i < frames; i++) {
                int32_t left = samples[i * 2];
                int32_t right = samples[i * 2 + 1];
                int32_t mixed = left + right;

                output[i] = (int16_t)(mixed / 2);
            }
            output_used += frames * sizeof(int16_t);
        }
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.capturing = false;
    s_status.last_capture_bytes = output_used;
    if (ret == ESP_OK && output_used == target_bytes) {
        s_status.captures_completed++;
        s_status.last_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_status_lock);
    xSemaphoreGive(s_device_mutex);

    *captured_bytes = output_used;
    if (ret == ESP_OK && output_used != target_bytes) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret != ESP_OK) {
        set_error(ret);
    }
    return ret;
}

esp_err_t SPEECH_OUTPUT_StreamPCM(uint32_t max_duration_ms,
                                  uint32_t chunk_duration_ms,
                                  speech_output_pcm_handler_t handler,
                                  void *arg, size_t *captured_bytes)
{
    size_t target_bytes;
    size_t chunk_bytes;
    size_t output_used = 0;
    size_t chunk_used = 0;
    uint8_t channels;
    esp_err_t ret = ESP_OK;
    bool keep_streaming = true;

    if (handler == NULL || captured_bytes == NULL ||
        max_duration_ms == 0 ||
        max_duration_ms > UAC_MIC_STREAM_MAX_DURATION_MS ||
        chunk_duration_ms == 0 ||
        chunk_duration_ms > UAC_MIC_STREAM_MAX_CHUNK_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    target_bytes = ((size_t)TTS_INPUT_RATE_HZ * sizeof(int16_t) *
                    max_duration_ms) / 1000;
    chunk_bytes = ((size_t)TTS_INPUT_RATE_HZ * sizeof(int16_t) *
                   chunk_duration_ms) / 1000;
    chunk_bytes -= chunk_bytes % sizeof(int16_t);
    *captured_bytes = 0;

    xSemaphoreTake(s_device_mutex, portMAX_DELAY);
    if (s_microphone == NULL || s_status.speaking || s_status.capturing) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    channels = s_status.microphone_channels;
    if (s_status.microphone_sample_rate != TTS_INPUT_RATE_HZ ||
        s_status.microphone_bits_per_sample != 16 ||
        (channels != 1 && channels != 2)) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.capturing = true;
    portEXIT_CRITICAL(&s_status_lock);

    for (unsigned int i = 0; i < 8; i++) {
        uint32_t discarded = 0;

        if (uac_host_device_read(s_microphone, s_capture_raw,
                                 sizeof(s_capture_raw), &discarded, 0) !=
            ESP_OK || discarded == 0) {
            break;
        }
    }

    while (ret == ESP_OK && keep_streaming && output_used < target_bytes) {
        uint32_t raw_read = 0;
        size_t wanted = chunk_bytes - chunk_used;
        size_t request = wanted * channels;

        if (request > sizeof(s_capture_raw)) {
            request = sizeof(s_capture_raw);
        }
        request -= request % (channels * sizeof(int16_t));
        ret = uac_host_device_read(
            s_microphone, s_capture_raw, request, &raw_read,
            pdMS_TO_TICKS(UAC_MIC_STREAM_READ_TIMEOUT_MS));
        if (ret != ESP_OK) {
            break;
        }
        raw_read -= raw_read % (channels * sizeof(int16_t));
        if (channels == 1) {
            size_t copy = raw_read;

            if (copy > wanted) {
                copy = wanted;
            }
            memcpy((uint8_t *)s_stream_mono + chunk_used,
                   s_capture_raw, copy);
            chunk_used += copy;
        } else {
            const int16_t *input = (const int16_t *)s_capture_raw;
            int16_t *output = (int16_t *)((uint8_t *)s_stream_mono +
                                          chunk_used);
            size_t frames = raw_read / (channels * sizeof(int16_t));
            size_t room = wanted / sizeof(int16_t);

            if (frames > room) {
                frames = room;
            }
            for (size_t i = 0; i < frames; i++) {
                int32_t mixed = (int32_t)input[i * 2] + input[i * 2 + 1];

                output[i] = (int16_t)(mixed / 2);
            }
            chunk_used += frames * sizeof(int16_t);
        }

        if (chunk_used == chunk_bytes ||
            output_used + chunk_used >= target_bytes) {
            keep_streaming = handler(s_stream_mono,
                                     chunk_used / sizeof(int16_t), arg);
            output_used += chunk_used;
            chunk_used = 0;
        }
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.capturing = false;
    s_status.last_capture_bytes = output_used;
    if (ret == ESP_OK) {
        s_status.captures_completed++;
        s_status.last_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_status_lock);
    xSemaphoreGive(s_device_mutex);

    *captured_bytes = output_used;
    if (ret != ESP_OK) {
        set_error(ret);
    }
    return ret;
}

esp_err_t SPEECH_OUTPUT_GetStatus(speech_output_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
