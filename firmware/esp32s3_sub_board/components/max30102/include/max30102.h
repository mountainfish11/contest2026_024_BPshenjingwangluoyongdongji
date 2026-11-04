#ifndef BLINDBADGE_MAX30102_H
#define BLINDBADGE_MAX30102_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool running;
    bool sensor_present;
    bool finger_present;
    bool heart_rate_valid;
    bool spo2_valid;
    uint8_t part_id;
    uint8_t revision_id;
    uint32_t red_raw;
    uint32_t ir_raw;
    uint32_t samples;
    uint32_t analysis_sequence;
    uint32_t fifo_overflows;
    uint32_t i2c_errors;
    uint32_t last_sample_age_ms;
    float heart_rate_bpm;
    float spo2_percent;
    float ir_mean;
    float ir_ac_rms;
} max30102_data_t;

esp_err_t MAX30102_Init(void);
bool MAX30102_IsRunning(void);
esp_err_t MAX30102_GetData(max30102_data_t *data);

#ifdef __cplusplus
}
#endif

#endif
