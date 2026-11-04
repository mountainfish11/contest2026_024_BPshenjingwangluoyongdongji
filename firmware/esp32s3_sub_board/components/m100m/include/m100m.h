#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t tx_errors;
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t rx_overflows;
    uint32_t last_tx_age_ms;
    uint32_t last_rx_age_ms;
} m100m_status_t;

esp_err_t M100M_Init(void);
esp_err_t M100M_SendLine(const char *text);
esp_err_t M100M_GetStatus(m100m_status_t *status);

#ifdef __cplusplus
}
#endif
