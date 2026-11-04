#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*uart_link_flag_handler_t)(const char *flag, void *arg);
typedef void (*uart_link_speech_handler_t)(const char *text, void *arg);

/* ASR callback: receives PCM data from main board, writes recognized text
 * to text_out, returns 0 on success. */
typedef int (*uart_link_asr_handler_t)(const uint8_t *pcm, size_t pcm_len,
                                       char *text_out, size_t text_cap,
                                       void *arg);

esp_err_t UART_LINK_Start(void);
bool UART_LINK_IsConnected(void);
void UART_LINK_SetHeartbeatPaused(bool paused);
void UART_LINK_SetFlagHandler(uart_link_flag_handler_t handler, void *arg);
void UART_LINK_SetSpeechHandler(uart_link_speech_handler_t handler,
                               void *arg);
void UART_LINK_SetASRHandler(uart_link_asr_handler_t handler, void *arg);

esp_err_t UART_LINK_SendObstacle(int distance_cm, const char *direction,
                                 int confidence);
esp_err_t UART_LINK_SendStepDown(const char *direction, int confidence);
esp_err_t UART_LINK_SendEmergency(const char *source);
esp_err_t UART_LINK_SendASRResult(const char *text);
esp_err_t UART_LINK_SendVoiceIdle(void);
esp_err_t UART_LINK_SendHealthResult(int heart_rate_bpm);
size_t UART_LINK_GetPCMLen(void);
