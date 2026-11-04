#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPEECH_LINK_PORT 45682
#define SPEECH_LINK_MAX_TEXT_BYTES 480

typedef enum {
    SPEECH_LINK_MODE_SILENT = 0,
    SPEECH_LINK_MODE_MENU,
    SPEECH_LINK_MODE_VISION,
    SPEECH_LINK_MODE_VOICE,
    SPEECH_LINK_MODE_HEALTH,
    SPEECH_LINK_MODE_ALERT,
} speech_link_mode_t;

typedef void (*speech_link_handler_t)(const char *text, uint8_t priority,
                                      uint64_t sequence,
                                      speech_link_mode_t mode, void *arg);

typedef struct {
    bool running;
    uint32_t accepted;
    uint32_t delivered;
    uint32_t duplicates;
    uint32_t rejected;
    uint32_t crc_errors;
    uint32_t queue_full;
    uint32_t queued;
} speech_link_status_t;

void SPEECH_LINK_SetHandler(speech_link_handler_t handler, void *arg);
esp_err_t SPEECH_LINK_Start(void);
esp_err_t SPEECH_LINK_SetMode(speech_link_mode_t mode);
esp_err_t SPEECH_LINK_EnqueueMenu(const char *text, uint8_t priority);
esp_err_t SPEECH_LINK_EnqueueVision(const char *text, uint8_t priority);
esp_err_t SPEECH_LINK_EnqueueVoice(const char *text, uint8_t priority);
esp_err_t SPEECH_LINK_EnqueueHealth(const char *text, uint8_t priority);
esp_err_t SPEECH_LINK_EnqueueAlert(const char *text, uint8_t priority);
esp_err_t SPEECH_LINK_GetStatus(speech_link_status_t *status);

#ifdef __cplusplus
}
#endif
