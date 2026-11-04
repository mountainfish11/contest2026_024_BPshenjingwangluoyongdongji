/**
 * @file wifi.c
 * @brief WiFi initialization and connection management
 */

#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CONNECT_TIMEOUT_MS 30000

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;
static bool s_initialized;
static bool s_wifi_started;
static esp_netif_t *s_ap_netif;

static esp_err_t validate_credentials(const char *ssid, const char *password)
{
    size_t ssid_len;
    size_t password_len;

    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strnlen(ssid, sizeof(((wifi_sta_config_t *)0)->ssid) + 1);
    password_len = strnlen(password,
                           sizeof(((wifi_sta_config_t *)0)->password) + 1);
    if (ssid_len == 0 || ssid_len > sizeof(((wifi_sta_config_t *)0)->ssid) ||
        password_len > sizeof(((wifi_sta_config_t *)0)->password) - 1 ||
        (password_len > 0 && password_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_retry_num++;
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Reconnect request failed: %s",
                     esp_err_to_name(ret));
        }
        ESP_LOGW(TAG, "Disconnected; reconnect attempt %d", s_retry_num);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init(void)
{
    esp_err_t ret;

    if (s_initialized) {
        return ESP_OK;
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &event_handler, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &event_handler, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized successfully");
    return ESP_OK;
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    esp_err_t ret;
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = validate_credentials(ssid, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Invalid WiFi SSID or password length");
        return ret;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    s_retry_num = 0;
    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    strlcpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password));
    if (password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!s_wifi_started) {
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            return ret;
        }
        s_wifi_started = true;
    } else {
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    ret = wifi_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", ssid);
    } else {
        ESP_LOGW(TAG, "Connection timed out for SSID:%s; retry continues",
                 ssid);
    }
    return ret;
}

esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits;

    if (!s_initialized || s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                               pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    if (!s_initialized || s_wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_start_ap(const char *ssid, const char *password)
{
    esp_err_t ret;
    wifi_config_t wifi_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = validate_credentials(ssid, password);
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memcpy(wifi_config.ap.ssid, ssid, strlen(ssid));
    strlcpy((char *)wifi_config.ap.password, password,
            sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen(ssid);
    if (password[0] == '\0') {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!s_wifi_started) {
        ret = esp_wifi_start();
        if (ret == ESP_OK) {
            s_wifi_started = true;
        }
        return ret;
    }
    return ESP_OK;
}
