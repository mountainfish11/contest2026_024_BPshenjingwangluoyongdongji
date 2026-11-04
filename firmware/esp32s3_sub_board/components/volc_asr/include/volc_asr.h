#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct volc_asr_stream volc_asr_stream_t;

esp_err_t VOLC_ASR_Init(void);
esp_err_t VOLC_ASR_SetCredentials(const char *app_id, const char *token,
                                  const char *resource);
bool VOLC_ASR_IsConfigured(void);
int VOLC_ASR_Recognize(const uint8_t *pcm, size_t pcm_len,
                       char *text_out, size_t text_cap);
volc_asr_stream_t *VOLC_ASR_StreamOpen(void);
int VOLC_ASR_StreamSend(volc_asr_stream_t *stream, const uint8_t *pcm,
                        size_t pcm_len);
int VOLC_ASR_StreamFinish(volc_asr_stream_t *stream, char *text_out,
                          size_t text_cap);
void VOLC_ASR_StreamAbort(volc_asr_stream_t *stream);
