#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int fix_quality;
    int satellites;
    double latitude_deg;
    double longitude_deg;
    double altitude_m;
    double speed_mps;
    double course_deg;
    double hdop;
    char utc_time[16];
    char utc_date[8];
    uint32_t sentences;
    uint32_t checksum_errors;
    uint32_t parse_errors;
    uint32_t line_overflows;
    uint32_t pps_count;
    uint32_t pps_age_ms;
    uint32_t data_age_ms;
} atgm336h_data_t;

esp_err_t ATGM336H_Init(void);
bool ATGM336H_IsRunning(void);
esp_err_t ATGM336H_GetData(atgm336h_data_t *data);

#ifdef __cplusplus
}
#endif
