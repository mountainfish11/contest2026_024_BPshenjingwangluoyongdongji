#include "atgm336h.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ATGM336H_LINE_MAX 160
#define ATGM336H_UART_BUFFER_SIZE 2048
#define ATGM336H_TASK_STACK_SIZE 4096
#define ATGM336H_TASK_PRIORITY 6
#define ATGM336H_MAX_FIELDS 24
#define ATGM336H_KNOT_TO_MPS 0.514444

static const char *TAG = "ATGM336H";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static atgm336h_data_t s_data;
static int64_t s_last_data_us;
static volatile uint32_t s_pps_count;
static volatile TickType_t s_last_pps_tick;

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = (char)toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool validate_checksum(char *line)
{
    char *star;
    uint8_t checksum = 0;
    int high;
    int low;

    if (line[0] != '$') {
        return false;
    }
    star = strchr(line, '*');
    if (star == NULL || star[1] == '\0' || star[2] == '\0') {
        return false;
    }
    high = hex_value(star[1]);
    low = hex_value(star[2]);
    if (high < 0 || low < 0) {
        return false;
    }
    for (char *cursor = line + 1; cursor < star; cursor++) {
        checksum ^= (uint8_t)*cursor;
    }
    if (checksum != (uint8_t)((high << 4) | low)) {
        return false;
    }
    *star = '\0';
    return true;
}

static size_t split_fields(char *text, char **fields, size_t capacity)
{
    size_t count = 0;
    char *cursor = text;

    while (count < capacity) {
        fields[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    return count;
}

static bool parse_double(const char *text, double *value)
{
    char *end;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool parse_int(const char *text, int *value)
{
    char *end;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < INT32_MIN || parsed > INT32_MAX) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool parse_coordinate(const char *text, const char *hemisphere,
                             double *degrees)
{
    double raw;

    if (!parse_double(text, &raw) || hemisphere == NULL ||
        hemisphere[0] == '\0') {
        return false;
    }
    double whole_degrees = floor(raw / 100.0);
    double minutes = raw - whole_degrees * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) {
        return false;
    }
    double result = whole_degrees + minutes / 60.0;
    if (hemisphere[0] == 'S' || hemisphere[0] == 'W') {
        result = -result;
    } else if (hemisphere[0] != 'N' && hemisphere[0] != 'E') {
        return false;
    }
    *degrees = result;
    return true;
}

static void copy_field(char *destination, size_t capacity,
                       const char *source)
{
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    strlcpy(destination, source, capacity);
}

static bool parse_rmc(char **fields, size_t count)
{
    atgm336h_data_t update;
    double latitude;
    double longitude;
    double speed_knots = 0.0;
    double course = 0.0;

    if (count < 10 || fields[2][0] == '\0') {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    update = s_data;
    portEXIT_CRITICAL(&s_lock);

    update.valid = fields[2][0] == 'A';
    copy_field(update.utc_time, sizeof(update.utc_time), fields[1]);
    copy_field(update.utc_date, sizeof(update.utc_date), fields[9]);
    if (parse_coordinate(fields[3], fields[4], &latitude) &&
        parse_coordinate(fields[5], fields[6], &longitude)) {
        update.latitude_deg = latitude;
        update.longitude_deg = longitude;
    }
    if (parse_double(fields[7], &speed_knots)) {
        update.speed_mps = speed_knots * ATGM336H_KNOT_TO_MPS;
    }
    if (parse_double(fields[8], &course)) {
        update.course_deg = course;
    }

    portENTER_CRITICAL(&s_lock);
    update.sentences = s_data.sentences;
    update.checksum_errors = s_data.checksum_errors;
    update.parse_errors = s_data.parse_errors;
    update.line_overflows = s_data.line_overflows;
    s_data = update;
    s_last_data_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static bool parse_gga(char **fields, size_t count)
{
    atgm336h_data_t update;
    double latitude;
    double longitude;
    double altitude;
    double hdop;
    int fix_quality;
    int satellites;

    if (count < 10 || !parse_int(fields[6], &fix_quality) ||
        !parse_int(fields[7], &satellites)) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    update = s_data;
    portEXIT_CRITICAL(&s_lock);

    update.valid = fix_quality > 0;
    update.fix_quality = fix_quality;
    update.satellites = satellites;
    copy_field(update.utc_time, sizeof(update.utc_time), fields[1]);
    if (parse_coordinate(fields[2], fields[3], &latitude) &&
        parse_coordinate(fields[4], fields[5], &longitude)) {
        update.latitude_deg = latitude;
        update.longitude_deg = longitude;
    }
    if (parse_double(fields[8], &hdop)) {
        update.hdop = hdop;
    }
    if (parse_double(fields[9], &altitude)) {
        update.altitude_m = altitude;
    }

    portENTER_CRITICAL(&s_lock);
    update.sentences = s_data.sentences;
    update.checksum_errors = s_data.checksum_errors;
    update.parse_errors = s_data.parse_errors;
    update.line_overflows = s_data.line_overflows;
    s_data = update;
    s_last_data_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void parse_sentence(char *line)
{
    char *fields[ATGM336H_MAX_FIELDS];
    size_t count;
    bool parsed = true;

    portENTER_CRITICAL(&s_lock);
    s_data.sentences++;
    portEXIT_CRITICAL(&s_lock);

    if (!validate_checksum(line)) {
        portENTER_CRITICAL(&s_lock);
        s_data.checksum_errors++;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    count = split_fields(line + 1, fields, ATGM336H_MAX_FIELDS);
    if (count == 0) {
        parsed = false;
    } else if (strcmp(fields[0], "GNRMC") == 0 ||
               strcmp(fields[0], "GPRMC") == 0) {
        parsed = parse_rmc(fields, count);
    } else if (strcmp(fields[0], "GNGGA") == 0 ||
               strcmp(fields[0], "GPGGA") == 0) {
        parsed = parse_gga(fields, count);
    }
    if (!parsed) {
        portENTER_CRITICAL(&s_lock);
        s_data.parse_errors++;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    s_pps_count++;
    s_last_pps_tick = xTaskGetTickCountFromISR();
}

static void atgm336h_task(void *arg)
{
    char line[ATGM336H_LINE_MAX];
    size_t used = 0;
    uint8_t buffer[128];

    (void)arg;
    while (true) {
        int length = uart_read_bytes(CONFIG_ATGM336H_UART_NUM, buffer,
                                     sizeof(buffer), pdMS_TO_TICKS(500));
        for (int index = 0; index < length; index++) {
            uint8_t ch = buffer[index];
            if (ch == '\r' || ch == '\n') {
                if (used > 0) {
                    line[used] = '\0';
                    parse_sentence(line);
                    used = 0;
                }
            } else if (used + 1 < sizeof(line)) {
                line[used++] = (char)ch;
            } else {
                used = 0;
                portENTER_CRITICAL(&s_lock);
                s_data.line_overflows++;
                portEXIT_CRITICAL(&s_lock);
            }
        }
    }
}

esp_err_t ATGM336H_Init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ATGM336H_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(CONFIG_ATGM336H_UART_NUM,
                                        ATGM336H_UART_BUFFER_SIZE, 0, 0,
                                        NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_param_config(CONFIG_ATGM336H_UART_NUM, &uart_config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(CONFIG_ATGM336H_UART_NUM,
                           CONFIG_ATGM336H_UART_TX_GPIO,
                           CONFIG_ATGM336H_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (ret != ESP_OK) {
        uart_driver_delete(CONFIG_ATGM336H_UART_NUM);
        return ret;
    }

    const gpio_config_t pps_config = {
        .pin_bit_mask = 1ULL << CONFIG_ATGM336H_PPS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ret = gpio_config(&pps_config);
    if (ret == ESP_OK) {
        ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (ret == ESP_ERR_INVALID_STATE) {
            ret = ESP_OK;
        }
    }
    if (ret == ESP_OK) {
        ret = gpio_isr_handler_add(CONFIG_ATGM336H_PPS_GPIO, pps_isr, NULL);
    }
    if (ret != ESP_OK) {
        uart_driver_delete(CONFIG_ATGM336H_UART_NUM);
        return ret;
    }

    uart_flush_input(CONFIG_ATGM336H_UART_NUM);
    if (xTaskCreate(atgm336h_task, "atgm336h", ATGM336H_TASK_STACK_SIZE,
                    NULL, ATGM336H_TASK_PRIORITY, &s_task) != pdPASS) {
        gpio_isr_handler_remove(CONFIG_ATGM336H_PPS_GPIO);
        uart_driver_delete(CONFIG_ATGM336H_UART_NUM);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started UART%d baud=%d RX=%d TX=%d PPS=%d",
             CONFIG_ATGM336H_UART_NUM, CONFIG_ATGM336H_UART_BAUD_RATE,
             CONFIG_ATGM336H_UART_RX_GPIO, CONFIG_ATGM336H_UART_TX_GPIO,
             CONFIG_ATGM336H_PPS_GPIO);
    return ESP_OK;
}

bool ATGM336H_IsRunning(void)
{
    return s_task != NULL;
}

esp_err_t ATGM336H_GetData(atgm336h_data_t *data)
{
    int64_t last_data_us;
    uint32_t pps_count;
    TickType_t last_pps_tick;

    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *data = s_data;
    last_data_us = s_last_data_us;
    pps_count = s_pps_count;
    last_pps_tick = s_last_pps_tick;
    portEXIT_CRITICAL(&s_lock);

    data->pps_count = pps_count;
    data->data_age_ms = last_data_us == 0 ? UINT32_MAX :
        (uint32_t)((esp_timer_get_time() - last_data_us) / 1000);
    data->pps_age_ms = last_pps_tick == 0 ? UINT32_MAX :
        (uint32_t)((xTaskGetTickCount() - last_pps_tick) * portTICK_PERIOD_MS);
    return ESP_OK;
}
