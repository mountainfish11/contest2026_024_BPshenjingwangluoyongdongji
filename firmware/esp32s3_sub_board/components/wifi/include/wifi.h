/**
 * @file wifi.h
 * @brief WiFi component for sub board communication
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init(void);
esp_err_t wifi_connect(const char *ssid, const char *password);
esp_err_t wifi_wait_connected(uint32_t timeout_ms);
bool wifi_is_connected(void);
esp_err_t wifi_start_ap(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
