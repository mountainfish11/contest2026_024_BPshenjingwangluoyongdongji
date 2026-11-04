#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t VIBRATION_MOTOR_Init(void);
esp_err_t VIBRATION_MOTOR_Set(bool enabled);
esp_err_t VIBRATION_MOTOR_Trigger(uint32_t duration_ms);
esp_err_t VIBRATION_MOTOR_TriggerPattern(uint32_t pulse_ms,
                                         uint32_t pulse_count);
