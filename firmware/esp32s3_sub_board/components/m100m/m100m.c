#include "m100m.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define M100M_UART_BUFFER_SIZE 2048
#define M100M_RX_LINE_SIZE 512
#define M100M_TASK_STACK_SIZE 3072
#define M100M_TASK_PRIORITY 5

static const char *TAG = "M100M";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static m100m_status_t s_status;
static int64_t s_last_tx_us;
static int64_t s_last_rx_us;

static uint32_t age_ms(int64_t timestamp_us)
{
    if (timestamp_us == 0) {
        return UINT32_MAX;
    }

    int64_t age_us = esp_timer_get_time() - timestamp_us;
    if (age_us <= 0) {
        return 0;
    }
    if (age_us / 1000 > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(age_us / 1000);
}

static void count_received_line(const char *line, size_t length)
{
    portENTER_CRITICAL(&s_lock);
    s_status.rx_frames++;
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "RX %u bytes: %.*s", (unsigned)length,
             (int)length, line);
}

static void m100m_rx_task(void *arg)
{
    uint8_t buffer[128];
    char line[M100M_RX_LINE_SIZE];
    size_t used = 0;

    (void)arg;
    while (true) {
        int length = uart_read_bytes(CONFIG_M100M_UART_NUM, buffer,
                                     sizeof(buffer), pdMS_TO_TICKS(500));
        if (length <= 0) {
            continue;
        }

        portENTER_CRITICAL(&s_lock);
        s_status.rx_bytes += (uint32_t)length;
        portEXIT_CRITICAL(&s_lock);

        for (int index = 0; index < length; index++) {
            uint8_t ch = buffer[index];
            if (ch == '\r' || ch == '\n') {
                if (used > 0) {
                    line[used] = '\0';
                    count_received_line(line, used);
                    used = 0;
                }
            } else if (used + 1 < sizeof(line)) {
                line[used++] = (char)ch;
            } else {
                used = 0;
                portENTER_CRITICAL(&s_lock);
                s_status.rx_overflows++;
                portEXIT_CRITICAL(&s_lock);
            }
        }
    }
}

esp_err_t M100M_Init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    const uart_config_t uart_config = {
        .baud_rate = CONFIG_M100M_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(CONFIG_M100M_UART_NUM,
                                        M100M_UART_BUFFER_SIZE,
                                        M100M_UART_BUFFER_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_param_config(CONFIG_M100M_UART_NUM, &uart_config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(CONFIG_M100M_UART_NUM,
                           CONFIG_M100M_UART_TX_GPIO,
                           CONFIG_M100M_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (ret != ESP_OK) {
        uart_driver_delete(CONFIG_M100M_UART_NUM);
        return ret;
    }

    uart_flush_input(CONFIG_M100M_UART_NUM);
    if (xTaskCreate(m100m_rx_task, "m100m_rx", M100M_TASK_STACK_SIZE,
                    NULL, M100M_TASK_PRIORITY, &s_task) != pdPASS) {
        uart_driver_delete(CONFIG_M100M_UART_NUM);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "started UART%d baud=%d RX=%d TX=%d",
             CONFIG_M100M_UART_NUM, CONFIG_M100M_UART_BAUD_RATE,
             CONFIG_M100M_UART_RX_GPIO, CONFIG_M100M_UART_TX_GPIO);
    return ESP_OK;
}

esp_err_t M100M_SendLine(const char *text)
{
    char frame[M100M_RX_LINE_SIZE];

    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t length = strlen(text);
    if (length + 2 > M100M_RX_LINE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(frame, text, length);
    frame[length] = '\r';
    frame[length + 1] = '\n';
    size_t frame_length = length + 2;
    int written = uart_write_bytes(CONFIG_M100M_UART_NUM, frame,
                                   frame_length);
    if (written == (int)frame_length &&
        uart_wait_tx_done(CONFIG_M100M_UART_NUM,
                          pdMS_TO_TICKS(1000)) == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.tx_frames++;
        s_status.tx_bytes += (uint32_t)frame_length;
        s_last_tx_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.tx_errors++;
    portEXIT_CRITICAL(&s_lock);
    return ESP_FAIL;
}

esp_err_t M100M_GetStatus(m100m_status_t *status)
{
    int64_t last_tx_us;
    int64_t last_rx_us;

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    last_tx_us = s_last_tx_us;
    last_rx_us = s_last_rx_us;
    portEXIT_CRITICAL(&s_lock);
    status->last_tx_age_ms = age_ms(last_tx_us);
    status->last_rx_age_ms = age_ms(last_rx_us);
    return ESP_OK;
}
