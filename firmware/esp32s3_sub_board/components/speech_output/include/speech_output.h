#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool tts_ready;
    bool usb_host_ready;
    bool speaker_ready;
    bool microphone_ready;
    bool speaking;
    bool capturing;
    uint16_t speaker_vid;
    uint16_t speaker_pid;
    uint16_t microphone_vid;
    uint16_t microphone_pid;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint32_t microphone_sample_rate;
    uint8_t microphone_channels;
    uint8_t microphone_bits_per_sample;
    uint32_t completed;
    uint32_t captures_completed;
    size_t last_capture_bytes;
    uint32_t failed;
    esp_err_t last_error;
} speech_output_status_t;

typedef bool (*speech_output_pcm_handler_t)(const int16_t *samples,
                                            size_t sample_count,
                                            void *arg);

esp_err_t SPEECH_OUTPUT_Init(void);
esp_err_t SPEECH_OUTPUT_Speak(const char *utf8_text);
esp_err_t SPEECH_OUTPUT_CapturePCM(uint8_t *pcm, size_t capacity,
                                   uint32_t duration_ms,
                                   size_t *captured_bytes);
esp_err_t SPEECH_OUTPUT_StreamPCM(uint32_t max_duration_ms,
                                  uint32_t chunk_duration_ms,
                                  speech_output_pcm_handler_t handler,
                                  void *arg, size_t *captured_bytes);
esp_err_t SPEECH_OUTPUT_GetStatus(speech_output_status_t *status);

#ifdef __cplusplus
}
#endif
