#include "volc_asr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#define VOLC_HOST "openspeech.bytedance.com"
#define VOLC_URL "https://openspeech.bytedance.com"
#define VOLC_V2_PATH "/api/v2/asr"
#define VOLC_V3_PATH "/api/v3/sauc/bigmodel_nostream"
#define VOLC_V3_STREAM_PATH "/api/v3/sauc/bigmodel_async"
#define VOLC_NAMESPACE "volc_asr"
#define VOLC_PCM_CHUNK_BYTES 3200
#define VOLC_WS_BUFFER_SIZE 8192
#define VOLC_UUID_SIZE 37

#define VOLC_PROTOCOL_HEADER 0x11
#define VOLC_MSG_FULL_REQUEST 0x10
#define VOLC_MSG_AUDIO 0x20
#define VOLC_MSG_AUDIO_LAST 0x22
#define VOLC_SERIAL_JSON 0x10
#define VOLC_SERIAL_RAW 0x00

#define WS_FIN 0x80
#define WS_MASK 0x80
#define WS_BINARY 0x02
#define WS_CLOSE 0x08
#define WS_PING 0x09
#define WS_PONG 0x0a

typedef struct {
    esp_tls_t *tls;
    bool use_v3;
    int32_t sequence;
} volc_connection_t;

struct volc_asr_stream {
    volc_connection_t connection;
    bool ready;
    bool audio_sent;
};

static const char *TAG = "VOLC_ASR";
static bool s_time_sync_started;

static bool system_time_valid(void)
{
    time_t now = time(NULL);
    struct tm utc;

    return gmtime_r(&now, &utc) != NULL && utc.tm_year + 1900 >= 2024;
}

static bool is_v3_resource(const char *resource)
{
    return strncmp(resource, "volc.bigasr.sauc.", 17) == 0 ||
           strncmp(resource, "volc.seedasr.sauc.", 18) == 0;
}

static void generate_uuid(char *output, size_t capacity)
{
    uint8_t random[16];

    esp_fill_random(random, sizeof(random));
    random[6] = (random[6] & 0x0f) | 0x40;
    random[8] = (random[8] & 0x3f) | 0x80;
    snprintf(output, capacity,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x-%02x%02x%02x%02x%02x%02x",
             random[0], random[1], random[2], random[3],
             random[4], random[5], random[6], random[7],
             random[8], random[9], random[10], random[11],
             random[12], random[13], random[14], random[15]);
}

static int tls_write_all(esp_tls_t *tls, const void *data, size_t length)
{
    const uint8_t *cursor = data;
    size_t written = 0;

    while (written < length) {
        int ret = esp_tls_conn_write(tls, cursor + written,
                                     length - written);
        if (ret > 0) {
            written += ret;
        } else if (ret == ESP_TLS_ERR_SSL_WANT_READ ||
                   ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            ESP_LOGE(TAG, "TLS write failed: %d", ret);
            return -EIO;
        }
    }
    return 0;
}

static int tls_read_all(esp_tls_t *tls, void *data, size_t length)
{
    uint8_t *cursor = data;
    size_t received = 0;

    while (received < length) {
        int ret = esp_tls_conn_read(tls, cursor + received,
                                    length - received);
        if (ret > 0) {
            received += ret;
        } else if (ret == ESP_TLS_ERR_SSL_WANT_READ ||
                   ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            ESP_LOGE(TAG, "TLS read failed: %d", ret);
            return ret == 0 ? -ECONNRESET : -EIO;
        }
    }
    return 0;
}

static int websocket_send(volc_connection_t *connection, uint8_t opcode,
                          const uint8_t *payload, size_t payload_len)
{
    uint8_t header[14];
    uint8_t mask[4];
    size_t header_len;
    size_t sent = 0;

    header[0] = WS_FIN | opcode;
    if (payload_len < 126) {
        header[1] = WS_MASK | payload_len;
        header_len = 2;
    } else if (payload_len <= UINT16_MAX) {
        header[1] = WS_MASK | 126;
        header[2] = payload_len >> 8;
        header[3] = payload_len;
        header_len = 4;
    } else {
        header[1] = WS_MASK | 127;
        memset(header + 2, 0, 4);
        header[6] = payload_len >> 24;
        header[7] = payload_len >> 16;
        header[8] = payload_len >> 8;
        header[9] = payload_len;
        header_len = 10;
    }

    esp_fill_random(mask, sizeof(mask));
    memcpy(header + header_len, mask, sizeof(mask));
    header_len += sizeof(mask);
    if (tls_write_all(connection->tls, header, header_len) != 0) {
        return -EIO;
    }

    while (sent < payload_len) {
        uint8_t block[512];
        size_t block_len = payload_len - sent;
        if (block_len > sizeof(block)) {
            block_len = sizeof(block);
        }
        for (size_t i = 0; i < block_len; i++) {
            block[i] = payload[sent + i] ^ mask[(sent + i) % sizeof(mask)];
        }
        if (tls_write_all(connection->tls, block, block_len) != 0) {
            return -EIO;
        }
        sent += block_len;
    }
    return 0;
}

static int websocket_receive(volc_connection_t *connection, uint8_t *buffer,
                             size_t capacity, size_t *length, int *opcode)
{
    uint8_t header[2];
    uint8_t mask[4];
    size_t payload_len;
    bool masked;
    int ret;

    ret = tls_read_all(connection->tls, header, sizeof(header));
    if (ret != 0) {
        return ret;
    }
    *opcode = header[0] & 0x0f;
    masked = (header[1] & WS_MASK) != 0;
    payload_len = header[1] & 0x7f;
    if (payload_len == 126) {
        uint8_t extended[2];
        if (tls_read_all(connection->tls, extended, sizeof(extended)) != 0) {
            return -EIO;
        }
        payload_len = ((size_t)extended[0] << 8) | extended[1];
    } else if (payload_len == 127) {
        uint8_t extended[8];
        if (tls_read_all(connection->tls, extended, sizeof(extended)) != 0) {
            return -EIO;
        }
        if (extended[0] || extended[1] || extended[2] || extended[3]) {
            return -EOVERFLOW;
        }
        payload_len = ((size_t)extended[4] << 24) |
                      ((size_t)extended[5] << 16) |
                      ((size_t)extended[6] << 8) | extended[7];
    }
    if (payload_len > capacity) {
        ESP_LOGE(TAG, "WebSocket frame too large: %u", (unsigned)payload_len);
        return -EOVERFLOW;
    }
    if (masked && tls_read_all(connection->tls, mask, sizeof(mask)) != 0) {
        return -EIO;
    }
    if (payload_len > 0 &&
        tls_read_all(connection->tls, buffer, payload_len) != 0) {
        return -EIO;
    }
    if (masked) {
        for (size_t i = 0; i < payload_len; i++) {
            buffer[i] ^= mask[i % sizeof(mask)];
        }
    }
    *length = payload_len;
    return 0;
}

static int websocket_upgrade(volc_connection_t *connection,
                             const char *app_id, const char *token,
                             const char *resource, bool streaming)
{
    uint8_t raw_key[16];
    uint8_t encoded_key[32];
    size_t encoded_len = 0;
    char request_id[VOLC_UUID_SIZE];
    char auth[640];
    char request[1200];
    char response[2048];
    const char *path;
    size_t used = 0;
    int status = 0;
    int auth_len;

    connection->use_v3 = is_v3_resource(resource);
    path = connection->use_v3 ?
           (streaming ? VOLC_V3_STREAM_PATH : VOLC_V3_PATH) : VOLC_V2_PATH;
    generate_uuid(request_id, sizeof(request_id));
    if (connection->use_v3) {
        auth_len = snprintf(auth, sizeof(auth),
                            "X-Api-App-Key: %s\r\n"
                            "X-Api-Access-Key: %s\r\n"
                            "X-Api-Resource-Id: %s\r\n"
                            "X-Api-Request-Id: %s\r\n"
                            "X-Api-Connect-Id: %s\r\n",
                            app_id, token, resource, request_id, request_id);
    } else {
        auth_len = snprintf(auth, sizeof(auth),
                            "Authorization: Bearer;%s\r\n", token);
    }
    if (auth_len <= 0 || auth_len >= sizeof(auth)) {
        return -EOVERFLOW;
    }

    esp_fill_random(raw_key, sizeof(raw_key));
    if (mbedtls_base64_encode(encoded_key, sizeof(encoded_key), &encoded_len,
                              raw_key, sizeof(raw_key)) != 0) {
        return -EIO;
    }
    int request_len = snprintf(request, sizeof(request),
                               "GET %s HTTP/1.1\r\n"
                               "Host: " VOLC_HOST "\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Key: %.*s\r\n"
                               "Sec-WebSocket-Version: 13\r\n"
                               "%s\r\n",
                               path, (int)encoded_len, encoded_key, auth);
    if (request_len <= 0 || request_len >= sizeof(request) ||
        tls_write_all(connection->tls, request, request_len) != 0) {
        return -EIO;
    }

    while (used + 1 < sizeof(response)) {
        int ret = esp_tls_conn_read(connection->tls, response + used,
                                    sizeof(response) - used - 1);
        if (ret > 0) {
            used += ret;
            response[used] = '\0';
            if (strstr(response, "\r\n\r\n") != NULL) {
                break;
            }
        } else if (ret == ESP_TLS_ERR_SSL_WANT_READ ||
                   ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return -EIO;
        }
    }
    if (sscanf(response, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        ESP_LOGE(TAG, "WebSocket upgrade failed: HTTP %d", status);
        return -EPROTO;
    }
    return 0;
}

static int send_volc_frame(volc_connection_t *connection, uint8_t type,
                           uint8_t serialization, const uint8_t *payload,
                           size_t payload_len)
{
    size_t header_len = connection->use_v3 ? 12 : 8;
    uint8_t *frame = malloc(header_len + payload_len);
    size_t size_offset = 4;
    int ret;

    if (frame == NULL) {
        return -ENOMEM;
    }
    frame[0] = VOLC_PROTOCOL_HEADER;
    frame[1] = type;
    frame[2] = serialization;
    frame[3] = 0;
    if (connection->use_v3) {
        uint32_t sequence = ++connection->sequence;
        frame[1] |= 1;
        if (type == VOLC_MSG_AUDIO_LAST) {
            sequence = (uint32_t)-connection->sequence;
        }
        frame[4] = sequence >> 24;
        frame[5] = sequence >> 16;
        frame[6] = sequence >> 8;
        frame[7] = sequence;
        size_offset = 8;
    }
    frame[size_offset] = payload_len >> 24;
    frame[size_offset + 1] = payload_len >> 16;
    frame[size_offset + 2] = payload_len >> 8;
    frame[size_offset + 3] = payload_len;
    if (payload_len > 0) {
        memcpy(frame + header_len, payload, payload_len);
    }
    ret = websocket_send(connection, WS_BINARY, frame,
                         header_len + payload_len);
    free(frame);
    return ret;
}

static int send_client_request(volc_connection_t *connection,
                               const char *app_id, const char *token,
                               const char *resource)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *user;
    cJSON *audio;
    cJSON *request;
    char request_id[VOLC_UUID_SIZE];
    char *json;
    int ret;

    if (root == NULL) {
        return -ENOMEM;
    }
    generate_uuid(request_id, sizeof(request_id));
    if (!connection->use_v3) {
        cJSON *app = cJSON_AddObjectToObject(root, "app");
        cJSON_AddStringToObject(app, "appid", app_id);
        cJSON_AddStringToObject(app, "token", token);
        cJSON_AddStringToObject(app, "cluster", resource);
    }
    user = cJSON_AddObjectToObject(root, "user");
    cJSON_AddStringToObject(user, "uid", "blindbadge-sub-board");
    audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "format",
                            connection->use_v3 ? "pcm" : "raw");
    cJSON_AddNumberToObject(audio, "rate", 16000);
    cJSON_AddNumberToObject(audio, "bits", 16);
    cJSON_AddNumberToObject(audio, "channel", 1);
    cJSON_AddStringToObject(audio, "language", "zh-CN");
    request = cJSON_AddObjectToObject(root, "request");
    if (connection->use_v3) {
        cJSON_AddStringToObject(request, "model_name", "bigmodel");
        cJSON_AddBoolToObject(request, "enable_itn", true);
        cJSON_AddBoolToObject(request, "enable_punc", true);
        cJSON_AddStringToObject(request, "result_type", "full");
    } else {
        cJSON_AddStringToObject(request, "reqid", request_id);
        cJSON_AddStringToObject(request, "workflow",
                                "audio_in,resample,partition,vad,fe,decode");
        cJSON_AddNumberToObject(request, "sequence", 1);
        cJSON_AddNumberToObject(request, "nbest", 1);
        cJSON_AddBoolToObject(request, "show_utterances", false);
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -ENOMEM;
    }
    ret = send_volc_frame(connection, VOLC_MSG_FULL_REQUEST,
                          VOLC_SERIAL_JSON, (const uint8_t *)json,
                          strlen(json));
    free(json);
    return ret;
}

static uint32_t read_be32(const uint8_t *buffer, size_t offset)
{
    return ((uint32_t)buffer[offset] << 24) |
           ((uint32_t)buffer[offset + 1] << 16) |
           ((uint32_t)buffer[offset + 2] << 8) | buffer[offset + 3];
}

static bool copy_result_text(cJSON *result, char *text_out, size_t text_cap)
{
    bool found = false;

    if (cJSON_IsArray(result)) {
        int count = cJSON_GetArraySize(result);

        for (int index = 0; index < count; index++) {
            cJSON *item = cJSON_GetArrayItem(result, index);
            cJSON *text = cJSON_GetObjectItem(item, "text");

            if (!cJSON_IsString(text) || text->valuestring == NULL ||
                text->valuestring[0] == '\0') {
                continue;
            }
            if (!found) {
                text_out[0] = '\0';
                found = true;
            }
            strlcat(text_out, text->valuestring, text_cap);
        }
        return found;
    }

    if (cJSON_IsObject(result)) {
        cJSON *text = cJSON_GetObjectItem(result, "text");

        if (cJSON_IsString(text) && text->valuestring != NULL &&
            text->valuestring[0] != '\0') {
            strlcpy(text_out, text->valuestring, text_cap);
            return true;
        }
    }
    return false;
}

static int receive_result(volc_connection_t *connection, char *text_out,
                          size_t text_cap)
{
    uint8_t *buffer = heap_caps_malloc(VOLC_WS_BUFFER_SIZE + 1,
                                       MALLOC_CAP_SPIRAM |
                                       MALLOC_CAP_8BIT);
    size_t frame_len;
    int opcode;
    int ret;

    if (buffer == NULL) {
        return -ENOMEM;
    }
    ret = websocket_receive(connection, buffer, VOLC_WS_BUFFER_SIZE,
                            &frame_len, &opcode);
    if (ret != 0) {
        free(buffer);
        return ret;
    }
    if (opcode == WS_PING) {
        ret = websocket_send(connection, WS_PONG, buffer, frame_len);
        free(buffer);
        return ret == 0 ? 0 : ret;
    }
    if (opcode == WS_CLOSE) {
        free(buffer);
        return -ECONNRESET;
    }
    if (opcode != WS_BINARY || frame_len < 8) {
        free(buffer);
        return 0;
    }

    uint8_t type_flags = buffer[1];
    uint8_t message_type = type_flags >> 4;
    bool final = (type_flags & 2) != 0;
    size_t offset = (buffer[0] & 0x0f) * 4;
    if (type_flags & 1) {
        offset += 4;
    }
    if (type_flags & 4) {
        offset += 4;
    }
    if (message_type == 0x0f) {
        if (frame_len >= offset + 8) {
            uint32_t code = read_be32(buffer, offset);
            uint32_t message_len = read_be32(buffer, offset + 4);
            if (message_len > frame_len - offset - 8) {
                message_len = frame_len - offset - 8;
            }
            ESP_LOGE(TAG, "Server error %lu: %.*s", (unsigned long)code,
                     (int)message_len, buffer + offset + 8);
        }
        free(buffer);
        return -EIO;
    }
    if (message_type != 0x09 || frame_len < offset + 4) {
        free(buffer);
        return 0;
    }

    uint32_t payload_len = read_be32(buffer, offset);
    offset += 4;
    if (payload_len > frame_len - offset) {
        free(buffer);
        return -EPROTO;
    }
    buffer[offset + payload_len] = '\0';
    cJSON *root = cJSON_Parse((char *)buffer + offset);
    free(buffer);
    if (root == NULL) {
        return -EPROTO;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (connection->use_v3) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *message = cJSON_GetObjectItem(root, "message");
        cJSON *last_package = cJSON_GetObjectItem(root,
                                                  "is_last_package");
        int code_value = cJSON_IsNumber(code) ? code->valueint : 0;
        const char *message_value = cJSON_IsString(message) &&
                                    message->valuestring != NULL ?
                                    message->valuestring : "";

        if (cJSON_IsBool(last_package)) {
            final = cJSON_IsTrue(last_package);
        } else if (cJSON_IsNumber(last_package)) {
            final = last_package->valueint != 0;
        }
        copy_result_text(result, text_out, text_cap);
        if (code_value != 0 && code_value != 20000000 &&
            code_value != 20000003) {
            ESP_LOGE(TAG, "ASR server rejected request: code=%d message=%s",
                     code_value, message_value);
            cJSON_Delete(root);
            return -EIO;
        }
        cJSON_Delete(root);
        return final ? 1 : 0;
    }

    cJSON *sequence = cJSON_GetObjectItem(root, "sequence");
    int sequence_value = cJSON_IsNumber(sequence) ? sequence->valueint : 0;
    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        cJSON *first = cJSON_GetArrayItem(result, 0);
        cJSON *text = cJSON_GetObjectItem(first, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            strlcpy(text_out, text->valuestring, text_cap);
        }
    }
    cJSON_Delete(root);
    return sequence_value < 0 ? 1 : 0;
}

static esp_err_t read_credentials(char *app_id, size_t app_capacity,
                                  char *token, size_t token_capacity,
                                  char *resource, size_t resource_capacity)
{
    nvs_handle_t handle;
    size_t length;
    esp_err_t ret = nvs_open(VOLC_NAMESPACE, NVS_READONLY, &handle);

    if (ret != ESP_OK) {
        return ret;
    }
    length = app_capacity;
    ret = nvs_get_str(handle, "app_id", app_id, &length);
    if (ret == ESP_OK) {
        length = token_capacity;
        ret = nvs_get_str(handle, "token", token, &length);
    }
    if (ret == ESP_OK) {
        length = resource_capacity;
        ret = nvs_get_str(handle, "resource", resource, &length);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t VOLC_ASR_SetCredentials(const char *app_id, const char *token,
                                  const char *resource)
{
    nvs_handle_t handle = 0;
    esp_err_t ret;

    if (app_id == NULL || token == NULL || resource == NULL ||
        app_id[0] == '\0' || token[0] == '\0' || resource[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ret = nvs_open(VOLC_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "app_id", app_id);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "token", token);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "resource", resource);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return ret;
}

esp_err_t VOLC_ASR_Init(void)
{
    if (!s_time_sync_started) {
        esp_sntp_config_t config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t ret = esp_netif_sntp_init(&config);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
        s_time_sync_started = true;
    }
    return ESP_OK;
}

bool VOLC_ASR_IsConfigured(void)
{
    char app_id[128];
    char token[256];
    char resource[128];
    return read_credentials(app_id, sizeof(app_id), token, sizeof(token),
                            resource, sizeof(resource)) == ESP_OK;
}

volc_asr_stream_t *VOLC_ASR_StreamOpen(void)
{
    char app_id[128] = {0};
    char token[256] = {0};
    char resource[128] = {0};
    char acknowledgement[32] = {0};
    esp_tls_cfg_t tls_config = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };
    volc_asr_stream_t *stream = NULL;
    int ret;

    if (read_credentials(app_id, sizeof(app_id), token, sizeof(token),
                         resource, sizeof(resource)) != ESP_OK) {
        ESP_LOGE(TAG, "ASR credentials are not configured");
        return NULL;
    }
    if (!s_time_sync_started ||
        (!system_time_valid() &&
         esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK)) {
        ESP_LOGE(TAG, "System time is not synchronized");
        goto cleanup;
    }

    stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        goto cleanup;
    }
    stream->connection.tls = esp_tls_init();
    if (stream->connection.tls == NULL) {
        goto fail;
    }
    if (esp_tls_conn_http_new_sync(VOLC_URL, &tls_config,
                                   stream->connection.tls) != 1) {
        ESP_LOGE(TAG, "Streaming TLS connection failed");
        goto fail;
    }
    ret = websocket_upgrade(&stream->connection, app_id, token, resource,
                            true);
    if (ret != 0) {
        goto fail;
    }
    ret = send_client_request(&stream->connection, app_id, token, resource);
    if (ret != 0) {
        goto fail;
    }
    if (stream->connection.use_v3) {
        ret = receive_result(&stream->connection, acknowledgement,
                             sizeof(acknowledgement));
        if (ret < 0 || ret == 1) {
            goto fail;
        }
    }
    stream->ready = true;
    memset(token, 0, sizeof(token));
    return stream;

fail:
    if (stream != NULL) {
        if (stream->connection.tls != NULL) {
            esp_tls_conn_destroy(stream->connection.tls);
        }
        free(stream);
    }
    stream = NULL;
cleanup:
    memset(token, 0, sizeof(token));
    return stream;
}

int VOLC_ASR_StreamSend(volc_asr_stream_t *stream, const uint8_t *pcm,
                        size_t pcm_len)
{
    int ret;

    if (stream == NULL || !stream->ready || pcm == NULL || pcm_len == 0) {
        return -EINVAL;
    }
    ret = send_volc_frame(&stream->connection, VOLC_MSG_AUDIO,
                          VOLC_SERIAL_RAW, pcm, pcm_len);
    if (ret == 0) {
        stream->audio_sent = true;
    }
    return ret;
}

int VOLC_ASR_StreamFinish(volc_asr_stream_t *stream, char *text_out,
                          size_t text_cap)
{
    int ret;

    if (stream == NULL || !stream->ready || text_out == NULL ||
        text_cap == 0) {
        VOLC_ASR_StreamAbort(stream);
        return -EINVAL;
    }
    text_out[0] = '\0';
    if (!stream->audio_sent) {
        VOLC_ASR_StreamAbort(stream);
        return -ENODATA;
    }
    ret = send_volc_frame(&stream->connection, VOLC_MSG_AUDIO_LAST,
                          VOLC_SERIAL_RAW, NULL, 0);
    if (ret == 0) {
        for (int attempt = 0; attempt < 100; attempt++) {
            ret = receive_result(&stream->connection, text_out, text_cap);
            if (ret < 0 || ret == 1) {
                break;
            }
        }
        if (ret == 1) {
            ret = text_out[0] == '\0' ? -ENODATA : 0;
        } else if (ret == 0) {
            ret = -ETIMEDOUT;
        }
    }
    esp_tls_conn_destroy(stream->connection.tls);
    stream->ready = false;
    free(stream);
    return ret;
}

void VOLC_ASR_StreamAbort(volc_asr_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    if (stream->ready && stream->connection.tls != NULL) {
        esp_tls_conn_destroy(stream->connection.tls);
    }
    stream->ready = false;
    free(stream);
}

int VOLC_ASR_Recognize(const uint8_t *pcm, size_t pcm_len,
                       char *text_out, size_t text_cap)
{
    char app_id[128] = {0};
    char token[256] = {0};
    char resource[128] = {0};
    esp_tls_cfg_t tls_config = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };
    volc_connection_t connection = {0};
    size_t offset = 0;
    int ret;

    if (pcm == NULL || pcm_len == 0 || text_out == NULL || text_cap == 0) {
        return -EINVAL;
    }
    text_out[0] = '\0';
    if (read_credentials(app_id, sizeof(app_id), token, sizeof(token),
                         resource, sizeof(resource)) != ESP_OK) {
        ESP_LOGE(TAG, "ASR credentials are not configured");
        return -ENOENT;
    }
    if (!s_time_sync_started ||
        (!system_time_valid() &&
         esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK)) {
        ESP_LOGE(TAG, "System time is not synchronized");
        return -EHOSTUNREACH;
    }

    connection.tls = esp_tls_init();
    if (connection.tls == NULL) {
        return -ENOMEM;
    }
    if (esp_tls_conn_http_new_sync(VOLC_URL, &tls_config,
                                   connection.tls) != 1) {
        int tls_code = 0;
        int tls_flags = 0;
        esp_tls_error_handle_t error_handle = NULL;
        esp_tls_get_error_handle(connection.tls, &error_handle);
        esp_tls_get_and_clear_last_error(error_handle, &tls_code, &tls_flags);
        ESP_LOGE(TAG, "TLS connection failed: code=-0x%x flags=0x%x",
                 tls_code, tls_flags);
        ret = -ECONNREFUSED;
        goto cleanup;
    }

    ret = websocket_upgrade(&connection, app_id, token, resource, false);
    if (ret != 0) {
        goto cleanup;
    }
    ret = send_client_request(&connection, app_id, token, resource);
    if (ret != 0) {
        goto cleanup;
    }

    /* The first V3 response acknowledges and validates the full request.
     * Consume it before sending timed audio packets so request and audio
     * responses cannot be mistaken for one another.
     */

    if (connection.use_v3) {
        ret = receive_result(&connection, text_out, text_cap);
        if (ret < 0) {
            goto cleanup;
        }
        if (ret == 1) {
            ret = text_out[0] == '\0' ? -ENODATA : 0;
            goto cleanup;
        }
    }

    while (offset < pcm_len) {
        size_t chunk_len = pcm_len - offset;
        bool last;
        if (chunk_len > VOLC_PCM_CHUNK_BYTES) {
            chunk_len = VOLC_PCM_CHUNK_BYTES;
        }
        last = offset + chunk_len == pcm_len;
        ret = send_volc_frame(&connection,
                              last ? VOLC_MSG_AUDIO_LAST : VOLC_MSG_AUDIO,
                              VOLC_SERIAL_RAW, pcm + offset, chunk_len);
        if (ret != 0) {
            goto cleanup;
        }
        offset += chunk_len;
        if (!last) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    for (int attempt = 0; attempt < 100; attempt++) {
        ret = receive_result(&connection, text_out, text_cap);
        if (ret < 0) {
            goto cleanup;
        }
        if (ret == 1) {
            ret = text_out[0] == '\0' ? -ENODATA : 0;
            goto cleanup;
        }
    }
    ret = -ETIMEDOUT;

cleanup:
    esp_tls_conn_destroy(connection.tls);
    memset(token, 0, sizeof(token));
    return ret;
}
