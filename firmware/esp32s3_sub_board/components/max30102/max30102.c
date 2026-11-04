#include "max30102.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MAX30102_ADDRESS              0x57
#define MAX30102_EXPECTED_PART_ID     0x15
#define MAX30102_REG_INTR_STATUS_1    0x00
#define MAX30102_REG_INTR_STATUS_2    0x01
#define MAX30102_REG_INTR_ENABLE_1    0x02
#define MAX30102_REG_FIFO_WR_PTR      0x04
#define MAX30102_REG_OVF_COUNTER      0x05
#define MAX30102_REG_FIFO_RD_PTR      0x06
#define MAX30102_REG_FIFO_DATA        0x07
#define MAX30102_REG_FIFO_CONFIG      0x08
#define MAX30102_REG_MODE_CONFIG      0x09
#define MAX30102_REG_SPO2_CONFIG      0x0a
#define MAX30102_REG_LED1_PA          0x0c
#define MAX30102_REG_LED2_PA          0x0d
#define MAX30102_REG_REVISION_ID      0xfe
#define MAX30102_REG_PART_ID          0xff

#define MAX30102_MODE_RESET           0x40
#define MAX30102_MODE_SPO2            0x03
#define MAX30102_FIFO_CONFIG_VALUE    0x5f
#define MAX30102_SPO2_CONFIG_VALUE    0x2f
#define MAX30102_INTR_ENABLE_VALUE    0xc0
#define MAX30102_SAMPLE_RATE_HZ       100
#define MAX30102_WINDOW_SAMPLES       600
#define MAX30102_BASELINE_SAMPLES     100
#define MAX30102_SMOOTH_SAMPLES       5
#define MAX30102_MIN_PEAK_DISTANCE    35
#define MAX30102_MAX_PEAK_DISTANCE    150
#define MAX30102_MAX_PEAKS            20
#define MAX30102_MIN_INTERVALS        3
#define MAX30102_MIN_FILTERED_RMS     60.0f
#define MAX30102_MIN_AC_DC_RATIO      0.0004f
#define MAX30102_MAX_IR_MEAN          250000.0f
#define MAX30102_FINGER_RELEASE_RATIO 0.85f
#define MAX30102_TASK_STACK_SIZE      4096
#define MAX30102_TASK_PRIORITY        6
#define MAX30102_I2C_TIMEOUT_MS       100

static const char *TAG = "MAX30102";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static max30102_data_t s_data;
static uint32_t s_red_window[MAX30102_WINDOW_SAMPLES];
static uint32_t s_ir_window[MAX30102_WINDOW_SAMPLES];
static uint32_t s_analysis_red[MAX30102_WINDOW_SAMPLES];
static uint32_t s_analysis_ir[MAX30102_WINDOW_SAMPLES];
static float s_ir_detrended[MAX30102_WINDOW_SAMPLES];
static float s_ir_filtered[MAX30102_WINDOW_SAMPLES];
static size_t s_window_used;
static size_t s_window_head;
static int64_t s_last_sample_us;
static bool s_finger_latched;

static esp_err_t max30102_read(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(s_device, &reg, sizeof(reg), data,
                                       length, MAX30102_I2C_TIMEOUT_MS);
}

static esp_err_t max30102_write(uint8_t reg, uint8_t value)
{
    uint8_t command[2] = {reg, value};
    return i2c_master_transmit(s_device, command, sizeof(command),
                               MAX30102_I2C_TIMEOUT_MS);
}

static void record_i2c_error(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_data.i2c_errors++;
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t max30102_reset(void)
{
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_MODE_CONFIG,
                                      MAX30102_MODE_RESET), TAG,
                        "soft reset failed");

    for (int retry = 0; retry < 20; retry++) {
        uint8_t mode;
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_RETURN_ON_ERROR(max30102_read(MAX30102_REG_MODE_CONFIG, &mode, 1),
                            TAG, "reset status read failed");
        if ((mode & MAX30102_MODE_RESET) == 0) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t max30102_configure(void)
{
    uint8_t status[2];

    ESP_RETURN_ON_ERROR(max30102_reset(), TAG, "reset timeout");
    ESP_RETURN_ON_ERROR(max30102_read(MAX30102_REG_INTR_STATUS_1, status,
                                     sizeof(status)), TAG,
                        "interrupt clear failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_FIFO_WR_PTR, 0), TAG,
                        "FIFO write pointer clear failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_OVF_COUNTER, 0), TAG,
                        "FIFO overflow clear failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_FIFO_RD_PTR, 0), TAG,
                        "FIFO read pointer clear failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_FIFO_CONFIG,
                                      MAX30102_FIFO_CONFIG_VALUE), TAG,
                        "FIFO configuration failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_SPO2_CONFIG,
                                      MAX30102_SPO2_CONFIG_VALUE), TAG,
                        "SpO2 configuration failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_LED1_PA,
                                      CONFIG_MAX30102_LED_AMPLITUDE), TAG,
                        "red LED configuration failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_LED2_PA,
                                      CONFIG_MAX30102_LED_AMPLITUDE), TAG,
                        "IR LED configuration failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_INTR_ENABLE_1,
                                      MAX30102_INTR_ENABLE_VALUE), TAG,
                        "interrupt configuration failed");
    ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_MODE_CONFIG,
                                      MAX30102_MODE_SPO2), TAG,
                        "SpO2 mode start failed");
    return ESP_OK;
}

static float calculate_mean(const uint32_t *values, size_t count)
{
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += values[i];
    }
    return (float)(sum / count);
}

static float calculate_rms(const uint32_t *values, size_t count, float mean)
{
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        double value = (double)values[i] - mean;
        sum += value * value;
    }
    return (float)sqrt(sum / count);
}

static int compare_u16(const void *left, const void *right)
{
    uint16_t a = *(const uint16_t *)left;
    uint16_t b = *(const uint16_t *)right;
    return (a > b) - (a < b);
}

static float filter_ir_signal(const uint32_t *ir, size_t count)
{
    double baseline_sum = 0.0;
    double smooth_sum = 0.0;
    double energy = 0.0;
    size_t energy_count = 0;

    for (size_t i = 0; i < count; i++) {
        baseline_sum += ir[i];
        if (i >= MAX30102_BASELINE_SAMPLES) {
            baseline_sum -= ir[i - MAX30102_BASELINE_SAMPLES];
        }

        size_t baseline_count = i + 1;
        if (baseline_count > MAX30102_BASELINE_SAMPLES) {
            baseline_count = MAX30102_BASELINE_SAMPLES;
        }
        s_ir_detrended[i] =
            (float)((double)ir[i] - baseline_sum / baseline_count);

        smooth_sum += s_ir_detrended[i];
        if (i >= MAX30102_SMOOTH_SAMPLES) {
            smooth_sum -= s_ir_detrended[i - MAX30102_SMOOTH_SAMPLES];
        }
        size_t smooth_count = i + 1;
        if (smooth_count > MAX30102_SMOOTH_SAMPLES) {
            smooth_count = MAX30102_SMOOTH_SAMPLES;
        }
        s_ir_filtered[i] = (float)(smooth_sum / smooth_count);

        if (i >= MAX30102_BASELINE_SAMPLES) {
            energy += (double)s_ir_filtered[i] * s_ir_filtered[i];
            energy_count++;
        }
    }

    return energy_count > 0 ? (float)sqrt(energy / energy_count) : 0.0f;
}

static bool calculate_heart_rate(size_t count, float rms, float *bpm)
{
    uint16_t peaks[MAX30102_MAX_PEAKS];
    float peak_values[MAX30102_MAX_PEAKS];
    uint16_t intervals[MAX30102_MAX_PEAKS - 1];
    uint16_t accepted[MAX30102_MAX_PEAKS - 1];
    size_t peak_count = 0;
    size_t interval_count = 0;
    size_t accepted_count = 0;
    float threshold = rms * 0.30f;

    for (size_t i = MAX30102_BASELINE_SAMPLES + 2; i + 2 < count; i++) {
        bool local_peak = s_ir_filtered[i] > threshold &&
                          s_ir_filtered[i] >= s_ir_filtered[i - 1] &&
                          s_ir_filtered[i] > s_ir_filtered[i + 1] &&
                          s_ir_filtered[i] >= s_ir_filtered[i - 2] &&
                          s_ir_filtered[i] > s_ir_filtered[i + 2];
        if (!local_peak) {
            continue;
        }

        if (peak_count > 0 && i - peaks[peak_count - 1] <
                              MAX30102_MIN_PEAK_DISTANCE) {
            if (s_ir_filtered[i] > peak_values[peak_count - 1]) {
                peaks[peak_count - 1] = (uint16_t)i;
                peak_values[peak_count - 1] = s_ir_filtered[i];
            }
            continue;
        }
        if (peak_count < MAX30102_MAX_PEAKS) {
            peaks[peak_count] = (uint16_t)i;
            peak_values[peak_count] = s_ir_filtered[i];
            peak_count++;
        }
    }

    for (size_t i = 1; i < peak_count; i++) {
        uint16_t interval = peaks[i] - peaks[i - 1];
        if (interval >= MAX30102_MIN_PEAK_DISTANCE &&
            interval <= MAX30102_MAX_PEAK_DISTANCE) {
            intervals[interval_count++] = interval;
        }
    }
    if (interval_count < MAX30102_MIN_INTERVALS) {
        return false;
    }

    qsort(intervals, interval_count, sizeof(intervals[0]), compare_u16);
    float median = intervals[interval_count / 2];
    float tolerance = fmaxf(6.0f, median * 0.18f);
    for (size_t i = 0; i < interval_count; i++) {
        if (fabsf((float)intervals[i] - median) <= tolerance) {
            accepted[accepted_count++] = intervals[i];
        }
    }
    if (accepted_count < MAX30102_MIN_INTERVALS) {
        return false;
    }

    qsort(accepted, accepted_count, sizeof(accepted[0]), compare_u16);
    float stable_interval = accepted[accepted_count / 2];
    float spread = accepted[accepted_count - 1] - accepted[0];
    if (spread > stable_interval * 0.22f) {
        return false;
    }

    *bpm = 60.0f * MAX30102_SAMPLE_RATE_HZ / stable_interval;
    return isfinite(*bpm) && *bpm >= 40.0f && *bpm <= 180.0f;
}

static void analyze_window(void)
{
    size_t start = s_window_head;

    for (size_t i = 0; i < MAX30102_WINDOW_SAMPLES; i++) {
        size_t index = (start + i) % MAX30102_WINDOW_SAMPLES;
        s_analysis_red[i] = s_red_window[index];
        s_analysis_ir[i] = s_ir_window[index];
    }

    float red_mean = calculate_mean(s_analysis_red, MAX30102_WINDOW_SAMPLES);
    float ir_mean = calculate_mean(s_analysis_ir, MAX30102_WINDOW_SAMPLES);
    float red_rms = calculate_rms(s_analysis_red, MAX30102_WINDOW_SAMPLES,
                                  red_mean);
    float ir_rms = calculate_rms(s_analysis_ir, MAX30102_WINDOW_SAMPLES,
                                 ir_mean);
    float filtered_rms = filter_ir_signal(s_analysis_ir,
                                          MAX30102_WINDOW_SAMPLES);
    float release_threshold = CONFIG_MAX30102_FINGER_THRESHOLD *
                              MAX30102_FINGER_RELEASE_RATIO;
    if (s_finger_latched) {
        s_finger_latched = ir_mean >= release_threshold;
    } else {
        s_finger_latched = ir_mean >= CONFIG_MAX30102_FINGER_THRESHOLD;
    }
    bool finger = s_finger_latched && ir_mean < MAX30102_MAX_IR_MEAN;
    bool signal_quality = finger && filtered_rms >=
                          MAX30102_MIN_FILTERED_RMS &&
                          filtered_rms / ir_mean >=
                          MAX30102_MIN_AC_DC_RATIO;
    float bpm = 0.0f;
    bool hr_valid = signal_quality &&
                    calculate_heart_rate(MAX30102_WINDOW_SAMPLES,
                                         filtered_rms, &bpm);
    bool spo2_valid = hr_valid && red_mean > 1.0f && ir_mean > 1.0f &&
                      red_rms > 10.0f && ir_rms > 10.0f;
    float spo2 = 0.0f;
    if (spo2_valid) {
        float ratio = (red_rms / red_mean) / (ir_rms / ir_mean);
        spo2 = 110.0f - 25.0f * ratio;
        if (!isfinite(spo2) || spo2 < 70.0f || spo2 > 100.0f) {
            spo2_valid = false;
            spo2 = 0.0f;
        }
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_data.finger_present = finger;
        s_data.heart_rate_valid = hr_valid;
        s_data.spo2_valid = spo2_valid;
        s_data.heart_rate_bpm = hr_valid ? bpm : 0.0f;
        s_data.spo2_percent = spo2_valid ? spo2 : 0.0f;
        s_data.ir_mean = ir_mean;
        s_data.ir_ac_rms = filtered_rms;
        s_data.analysis_sequence++;
        xSemaphoreGive(s_lock);
    }
}

static void store_sample(uint32_t red, uint32_t ir)
{
    s_red_window[s_window_head] = red;
    s_ir_window[s_window_head] = ir;
    s_window_head = (s_window_head + 1) % MAX30102_WINDOW_SAMPLES;
    if (s_window_used < MAX30102_WINDOW_SAMPLES) {
        s_window_used++;
    }
    s_last_sample_us = esp_timer_get_time();

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_data.red_raw = red;
        s_data.ir_raw = ir;
        s_data.samples++;
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t read_fifo(void)
{
    uint8_t pointers[3];
    ESP_RETURN_ON_ERROR(max30102_read(MAX30102_REG_FIFO_WR_PTR, pointers,
                                     sizeof(pointers)), TAG,
                        "FIFO pointers read failed");
    uint8_t write_pointer = pointers[0] & 0x1f;
    uint8_t overflow = pointers[1] & 0x1f;
    uint8_t read_pointer = pointers[2] & 0x1f;
    uint8_t sample_count = (write_pointer - read_pointer) & 0x1f;

    if (overflow != 0) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_data.fifo_overflows += overflow;
            xSemaphoreGive(s_lock);
        }
        ESP_RETURN_ON_ERROR(max30102_write(MAX30102_REG_OVF_COUNTER, 0), TAG,
                            "FIFO overflow clear failed");
    }
    if (sample_count == 0) {
        return ESP_OK;
    }

    uint8_t fifo[31 * 6];
    size_t length = (size_t)sample_count * 6;
    ESP_RETURN_ON_ERROR(max30102_read(MAX30102_REG_FIFO_DATA, fifo, length),
                        TAG, "FIFO data read failed");
    for (uint8_t i = 0; i < sample_count; i++) {
        size_t offset = (size_t)i * 6;
        uint32_t red = ((uint32_t)fifo[offset] << 16) |
                       ((uint32_t)fifo[offset + 1] << 8) |
                       fifo[offset + 2];
        uint32_t ir = ((uint32_t)fifo[offset + 3] << 16) |
                      ((uint32_t)fifo[offset + 4] << 8) |
                      fifo[offset + 5];
        store_sample(red & 0x3ffff, ir & 0x3ffff);
    }
    return ESP_OK;
}

static void IRAM_ATTR max30102_isr(void *arg)
{
    BaseType_t wake = pdFALSE;
    (void)arg;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &wake);
    }
    if (wake == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void max30102_task(void *arg)
{
    uint32_t next_analysis = MAX30102_WINDOW_SAMPLES;
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        if (read_fifo() != ESP_OK) {
            record_i2c_error();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t samples;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            samples = s_data.samples;
            xSemaphoreGive(s_lock);
        } else {
            continue;
        }
        if (s_window_used == MAX30102_WINDOW_SAMPLES &&
            samples >= next_analysis) {
            analyze_window();
            next_analysis = samples + MAX30102_SAMPLE_RATE_HZ;
        }
    }
}

esp_err_t MAX30102_Init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CONFIG_MAX30102_I2C_PORT,
        .sda_io_num = CONFIG_MAX30102_SDA_GPIO,
        .scl_io_num = CONFIG_MAX30102_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG,
                        "I2C bus creation failed");

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX30102_ADDRESS,
        .scl_speed_hz = CONFIG_MAX30102_I2C_FREQUENCY,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &device_config,
                                                  &s_device), TAG,
                        "I2C device creation failed");

    esp_err_t ret = ESP_FAIL;
    uint8_t part_id = 0;
    for (int retry = 0; retry < 3; retry++) {
        ret = max30102_read(MAX30102_REG_PART_ID, &part_id, 1);
        if (ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor not found at I2C address 0x%02x",
                 MAX30102_ADDRESS);
        return ret;
    }

    uint8_t revision_id = 0;
    ESP_RETURN_ON_ERROR(max30102_read(MAX30102_REG_REVISION_ID,
                                     &revision_id, 1), TAG,
                        "revision ID read failed");
    if (part_id != MAX30102_EXPECTED_PART_ID) {
        ESP_LOGE(TAG, "unexpected part ID: 0x%02x", part_id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(max30102_configure(), TAG,
                        "sensor configuration failed");

    gpio_config_t interrupt_config = {
        .pin_bit_mask = 1ULL << CONFIG_MAX30102_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt_config), TAG,
                        "interrupt GPIO configuration failed");
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(CONFIG_MAX30102_INT_GPIO,
                                             max30102_isr, NULL), TAG,
                        "interrupt handler installation failed");

    s_data.sensor_present = true;
    s_data.part_id = part_id;
    s_data.revision_id = revision_id;
    if (xTaskCreate(max30102_task, "max30102", MAX30102_TASK_STACK_SIZE,
                    NULL, MAX30102_TASK_PRIORITY, &s_task) != pdPASS) {
        gpio_isr_handler_remove(CONFIG_MAX30102_INT_GPIO);
        return ESP_ERR_NO_MEM;
    }
    s_data.running = true;
    ESP_LOGI(TAG,
             "ready: address=0x%02x part=0x%02x revision=0x%02x "
             "SDA=%d SCL=%d INT=%d",
             MAX30102_ADDRESS, part_id, revision_id,
             CONFIG_MAX30102_SDA_GPIO, CONFIG_MAX30102_SCL_GPIO,
             CONFIG_MAX30102_INT_GPIO);
    return ESP_OK;
}

bool MAX30102_IsRunning(void)
{
    return s_data.running;
}

esp_err_t MAX30102_GetData(max30102_data_t *data)
{
    if (data == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *data = s_data;
    if (s_last_sample_us > 0) {
        data->last_sample_age_ms =
            (uint32_t)((esp_timer_get_time() - s_last_sample_us) / 1000);
    } else {
        data->last_sample_age_ms = UINT32_MAX;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
