#include "vibration_motor.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

static bool s_initialized;
static esp_timer_handle_t s_stop_timer;
static uint32_t s_pattern_transitions;
static bool s_motor_on;

static int motor_level(bool enabled)
{
    return enabled == CONFIG_VIBRATION_MOTOR_ACTIVE_HIGH;
}

static void stop_timer_callback(void *arg)
{
    (void)arg;

    if (s_pattern_transitions == 0) {
        s_motor_on = false;
        (void)gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO,
                             motor_level(false));
        return;
    }

    s_motor_on = !s_motor_on;
    s_pattern_transitions--;
    (void)gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO,
                         motor_level(s_motor_on));
    if (s_pattern_transitions == 0) {
        s_motor_on = false;
        (void)gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO,
                             motor_level(false));
        (void)esp_timer_stop(s_stop_timer);
    }
}

esp_err_t VIBRATION_MOTOR_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_VIBRATION_MOTOR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&config), "VIBRATION_MOTOR",
                        "GPIO configuration failed");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO, motor_level(false)),
        "VIBRATION_MOTOR", "initial output failed");

    const esp_timer_create_args_t timer_args = {
        .callback = stop_timer_callback,
        .name = "motor_stop",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_stop_timer),
                        "VIBRATION_MOTOR", "stop timer creation failed");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t VIBRATION_MOTOR_Set(bool enabled)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO,
                          motor_level(enabled));
}

esp_err_t VIBRATION_MOTOR_Trigger(uint32_t duration_ms)
{
    esp_err_t ret;

    if (!s_initialized || s_stop_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0 || duration_ms > UINT32_MAX / 1000U) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_timer_stop(s_stop_timer);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ESP_RETURN_ON_ERROR(
        gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO, motor_level(true)),
        "VIBRATION_MOTOR", "trigger output failed");
    s_pattern_transitions = 0;
    s_motor_on = true;

    ret = esp_timer_start_once(s_stop_timer, (uint64_t)duration_ms * 1000U);
    if (ret != ESP_OK) {
        s_motor_on = false;
        (void)gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO,
                             motor_level(false));
    }
    return ret;
}

esp_err_t VIBRATION_MOTOR_TriggerPattern(uint32_t pulse_ms,
                                         uint32_t pulse_count)
{
    esp_err_t ret;

    if (!s_initialized || s_stop_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pulse_ms == 0 || pulse_ms > UINT32_MAX / 1000U ||
        pulse_count == 0 || pulse_count > UINT32_MAX / 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_timer_stop(s_stop_timer);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    s_pattern_transitions = pulse_count * 2U - 1U;
    s_motor_on = true;
    ESP_RETURN_ON_ERROR(
        gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO, motor_level(true)),
        "VIBRATION_MOTOR", "pattern output failed");

    ret = esp_timer_start_periodic(s_stop_timer,
                                   (uint64_t)pulse_ms * 1000U);
    if (ret != ESP_OK) {
        s_pattern_transitions = 0;
        s_motor_on = false;
        (void)gpio_set_level(CONFIG_VIBRATION_MOTOR_GPIO,
                             motor_level(false));
    }
    return ret;
}
