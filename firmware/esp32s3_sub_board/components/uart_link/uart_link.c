#include "uart_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "wifi.h"

#define UART_LINK_DISCOVERY_PORT 45678
#define UART_LINK_DATA_PORT 45679
#define UART_LINK_PCM_PORT 45680
#define UART_LINK_DISCOVERY_MS 2000
#define UART_LINK_RETRY_MS 3000
#define UART_LINK_VOICE_RETRY_MS 500
#define UART_LINK_CONNECT_TIMEOUT_MS 1000
#define UART_LINK_HEARTBEAT_MS 10000
#define UART_LINK_HEARTBEAT_PAUSE_MAX_MS 300000
#define UART_LINK_EVENT_QUEUE_LEN 8
#define UART_LINK_PCM_MAGIC 0x42425043
#define UART_LINK_PCM_VERSION 1
#define UART_LINK_PCM_MAX_BYTES (4 * 16000 * 2)

typedef struct {
    char type[16];
    char payload[96];
} uart_link_event_t;

static const char *TAG = "UART_LINK";
static TaskHandle_t s_task;
static TaskHandle_t s_pcm_task;
static volatile bool s_connected;
static volatile bool s_heartbeat_paused;
static volatile TickType_t s_heartbeat_paused_at;
static QueueHandle_t s_event_queue;
static uart_link_flag_handler_t s_flag_handler;
static void *s_flag_handler_arg;
static uart_link_speech_handler_t s_speech_handler;
static void *s_speech_handler_arg;
static uart_link_asr_handler_t s_asr_handler;
static void *s_asr_handler_arg;
static volatile size_t s_last_pcm_len;

static void retry_delay(uint32_t normal_ms)
{
    uint32_t delay_ms = s_heartbeat_paused ? UART_LINK_VOICE_RETRY_MS :
                                             normal_ms;

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static int connect_with_timeout(int fd, const struct sockaddr *address,
                                socklen_t address_len)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    if (connect(fd, address, address_len) == 0) {
        (void)fcntl(fd, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) {
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }

    fd_set write_fds;
    struct timeval timeout = {
        .tv_sec = UART_LINK_CONNECT_TIMEOUT_MS / 1000,
        .tv_usec = (UART_LINK_CONNECT_TIMEOUT_MS % 1000) * 1000,
    };

    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);
    int ready = select(fd + 1, NULL, &write_fds, NULL, &timeout);
    if (ready <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                   &error_len) < 0 || socket_error != 0) {
        if (socket_error != 0) {
            errno = socket_error;
        } else if (ready == 0) {
            errno = ETIMEDOUT;
        }
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }

    (void)fcntl(fd, F_SETFL, flags);
    return 0;
}

void UART_LINK_SetHeartbeatPaused(bool paused)
{
    s_heartbeat_paused = paused;
    if (paused) {
        s_heartbeat_paused_at = xTaskGetTickCount();
    }
}

static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *cursor = data;

    while (len > 0) {
        ssize_t n = send(fd, cursor, len, 0);
        if (n <= 0) {
            return -1;
        }
        cursor += n;
        len -= n;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t len)
{
    uint8_t *cursor = data;

    while (len > 0) {
        ssize_t n = recv(fd, cursor, len, 0);
        if (n <= 0) {
            return -1;
        }
        cursor += n;
        len -= n;
    }
    return 0;
}

static uint32_t pcm_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;

    for (size_t i = 0; i < len; i++) {
        uint32_t value = crc ^ data[i];
        for (int bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^
                    (0xedb88320u & (uint32_t)-(int32_t)(value & 1));
        }
        crc = value;
    }
    return ~crc;
}

static int recv_line(int fd, char *line, size_t line_size)
{
    size_t used = 0;

    while (used + 1 < line_size) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n <= 0) {
            return -1;
        }
        if (ch == '\n') {
            line[used] = '\0';
            return 0;
        }
        line[used++] = ch;
    }
    line[0] = '\0';
    return -1;
}

static int handle_command_frame(int fd, const char *line)
{
    char version[16];
    char type[24];
    char payload[96];
    unsigned int seq;
    unsigned int payload_len;
    char ack[96];
    int ack_len;
    int payload_offset = 0;
    size_t i;

    if (sscanf(line, "%15s %23s %u %u %n", version, type, &seq,
               &payload_len, &payload_offset) != 4 ||
        strcmp(version, "UART_LINK/1") != 0) {
        return 0;
    }
    if (payload_offset <= 0 || strlen(line + payload_offset) >=
        sizeof(payload)) {
        return -1;
    }
    strlcpy(payload, line + payload_offset, sizeof(payload));

    /* PONG is routine transport traffic.  Keep it available for debug builds
     * without continuously overwriting the interactive UART prompt.
     */
    if (strcmp(type, "PONG") == 0) {
        ESP_LOGD(TAG, "RX frame: %s", line);
    } else {
        ESP_LOGI(TAG, "RX frame: %s", line);
    }

    /* COMMAND frame */
    if (strcmp(type, "COMMAND") == 0) {
        if (strlen(payload) != payload_len) {
            ESP_LOGW(TAG, "invalid command length: %s", line);
            return -1;
        }
        for (i = 0; payload[i] != '\0'; i++) {
            char ch = payload[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '_')) {
                ESP_LOGW(TAG, "invalid command flag: %s", payload);
                return -1;
            }
        }

        ack_len = snprintf(ack, sizeof(ack),
                           "UART_LINK/1 COMMAND_ACK %u 2 OK\n", seq);
        if (ack_len <= 0 || (size_t)ack_len >= sizeof(ack) ||
            send_all(fd, ack, (size_t)ack_len) != 0) {
            return -1;
        }

        ESP_LOGI(TAG, "command received: seq=%u flag=%s", seq, payload);
        if (s_flag_handler != NULL) {
            s_flag_handler(payload, s_flag_handler_arg);
        }
        return 1;
    }

    if (strcmp(type, "SPEAK") == 0) {
        if (payload_len == 0 || strlen(payload) != payload_len) {
            return -1;
        }
        ack_len = snprintf(ack, sizeof(ack),
                           "UART_LINK/1 SPEAK_ACK %u 2 OK\n", seq);
        if (ack_len <= 0 || (size_t)ack_len >= sizeof(ack) ||
            send_all(fd, ack, (size_t)ack_len) != 0) {
            return -1;
        }
        if (s_speech_handler != NULL) {
            s_speech_handler(payload, s_speech_handler_arg);
        }
        return 1;
    }

    return 0;
}

static void pcm_connection(int fd, const struct sockaddr_in *peer)
{
    uint32_t header[7];
    uint32_t reply[5];
    uint32_t pcm_len = 0;
    uint32_t expected_crc = 0;
    uint32_t actual_crc = 0;
    uint32_t status = 1;
    uint8_t *pcm = NULL;

    if (recv_all(fd, header, sizeof(header)) != 0 ||
        ntohl(header[0]) != UART_LINK_PCM_MAGIC ||
        ntohl(header[1]) != UART_LINK_PCM_VERSION ||
        ntohl(header[2]) != 16000 || ntohl(header[3]) != 1 ||
        ntohl(header[4]) != 16) {
        ESP_LOGW(TAG, "PCM invalid header from %s", inet_ntoa(peer->sin_addr));
        goto reply;
    }

    pcm_len = ntohl(header[5]);
    expected_crc = ntohl(header[6]);
    if (pcm_len == 0 || pcm_len > UART_LINK_PCM_MAX_BYTES) {
        ESP_LOGW(TAG, "PCM invalid length: %lu", (unsigned long)pcm_len);
        goto reply;
    }

    pcm = heap_caps_malloc(pcm_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        pcm = heap_caps_malloc(pcm_len, MALLOC_CAP_8BIT);
    }
    if (pcm == NULL) {
        status = 2;
        ESP_LOGE(TAG, "PCM allocation failed: %lu bytes",
                 (unsigned long)pcm_len);
        goto reply;
    }

    ESP_LOGI(TAG, "PCM transfer start: %lu bytes from %s crc32=%08lx",
             (unsigned long)pcm_len, inet_ntoa(peer->sin_addr),
             (unsigned long)expected_crc);
    if (recv_all(fd, pcm, pcm_len) != 0) {
        status = 3;
        ESP_LOGW(TAG, "PCM receive failed: errno=%d", errno);
        goto reply;
    }

    actual_crc = pcm_crc32(pcm, pcm_len);
    if (actual_crc != expected_crc) {
        status = 4;
        ESP_LOGW(TAG, "PCM CRC mismatch: expected=%08lx actual=%08lx",
                 (unsigned long)expected_crc, (unsigned long)actual_crc);
        goto reply;
    }

    status = 0;
    s_last_pcm_len = pcm_len;
    ESP_LOGI(TAG, "PCM TRANSFER COMPLETE: bytes=%lu crc32=%08lx",
             (unsigned long)pcm_len, (unsigned long)actual_crc);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, pcm, pcm_len < 16 ? pcm_len : 16,
                             ESP_LOG_INFO);

reply:
    reply[0] = htonl(UART_LINK_PCM_MAGIC);
    reply[1] = htonl(UART_LINK_PCM_VERSION);
    reply[2] = htonl(status);
    reply[3] = htonl(status == 0 ? pcm_len : 0);
    reply[4] = htonl(status == 0 ? actual_crc : 0);
    send_all(fd, reply, sizeof(reply));

    if (status == 0 && s_asr_handler != NULL) {
        char result[512] = {0};
        int ret = s_asr_handler(pcm, pcm_len, result, sizeof(result),
                                s_asr_handler_arg);
        ESP_LOGI(TAG, "PCM receive handler: ret=%d text=%s", ret, result);
    }
    free(pcm);
}

static void pcm_server_task(void *arg)
{
    struct sockaddr_in addr = {0};
    int listen_fd;
    int yes = 1;

    (void)arg;
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "PCM listen socket failed: %d", errno);
        s_pcm_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(UART_LINK_PCM_PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "PCM listen failed: %d", errno);
        close(listen_fd);
        s_pcm_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "PCM server listening on TCP %d", UART_LINK_PCM_PORT);
    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            ESP_LOGW(TAG, "PCM accept failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        struct timeval timeout = {30, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        pcm_connection(fd, &peer);
        close(fd);
    }
}

static int wait_for_reply(int fd, const char *expected_type,
                          unsigned int expected_seq)
{
    char line[128];

    while (true) {
        char version[16];
        char type[24];
        char payload[96];
        unsigned int seq;
        unsigned int payload_len;
        int command_result;

        if (recv_line(fd, line, sizeof(line)) != 0) {
            return -1;
        }
        command_result = handle_command_frame(fd, line);
        if (command_result < 0) {
            return -1;
        }
        if (command_result > 0) {
            continue;
        }

        if (sscanf(line, "%15s %23s %u %u %95s", version, type, &seq,
                   &payload_len, payload) != 5 ||
            strcmp(version, "UART_LINK/1") != 0 ||
            strcmp(type, expected_type) != 0 || seq != expected_seq ||
            payload_len != strlen(payload) || strcmp(payload, "OK") != 0) {
            ESP_LOGW(TAG, "unexpected reply: %s", line);
            return -1;
        }
        return 0;
    }
}

static int send_frame_and_wait_ack(int fd, const char *type,
                                   const char *payload, unsigned int *seq)
{
    char frame[160];
    unsigned int frame_seq = ++(*seq);
    int len = snprintf(frame, sizeof(frame), "UART_LINK/1 %s %u %u %s\n",
                       type, frame_seq, (unsigned int)strlen(payload), payload);

    if (len <= 0 || (size_t)len >= sizeof(frame) ||
        send_all(fd, frame, (size_t)len) != 0 ||
        wait_for_reply(fd, "EVENT_ACK", frame_seq) != 0) {
        return -1;
    }
    ESP_LOGI(TAG, "event delivered: type=%s seq=%u payload=%s",
             type, frame_seq, payload);
    return 0;
}

static int discover_main(struct sockaddr_in *main_addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval timeout = s_heartbeat_paused ?
        (struct timeval){0, 200000} : (struct timeval){1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in broadcast = {0};
    broadcast.sin_family = AF_INET;
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    broadcast.sin_port = htons(UART_LINK_DISCOVERY_PORT);
    static const char probe[] = "UART_LINK/1 DISCOVER 0 2 OK\n";
    if (sendto(fd, probe, sizeof(probe) - 1, 0,
               (struct sockaddr *)&broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGW(TAG, "global discovery send failed: %d", errno);
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (sta != NULL && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
        uint32_t directed = ip_info.ip.addr | ~ip_info.netmask.addr;
        if (directed != broadcast.sin_addr.s_addr) {
            broadcast.sin_addr.s_addr = directed;
            if (sendto(fd, probe, sizeof(probe) - 1, 0,
                       (struct sockaddr *)&broadcast, sizeof(broadcast)) < 0) {
                ESP_LOGW(TAG, "subnet discovery send failed: %d", errno);
            }
        }
    }

    char reply[96];
    struct sockaddr_in peer = {0};
    socklen_t peer_len = sizeof(peer);
    ssize_t n = recvfrom(fd, reply, sizeof(reply) - 1, 0,
                         (struct sockaddr *)&peer, &peer_len);
    close(fd);
    if (n <= 0 || strncmp(reply, "UART_LINK/1 DISCOVER_ACK", 24) != 0) {
#if defined(CONFIG_SUB_BOARD_MAIN_IP_FALLBACK)
        if (CONFIG_SUB_BOARD_MAIN_IP_FALLBACK[0] != '\0') {
            memset(main_addr, 0, sizeof(*main_addr));
            main_addr->sin_family = AF_INET;
            main_addr->sin_port = htons(UART_LINK_DATA_PORT);
            if (inet_pton(AF_INET, CONFIG_SUB_BOARD_MAIN_IP_FALLBACK,
                          &main_addr->sin_addr) == 1) {
                ESP_LOGD(TAG, "discovery unavailable; trying main board %s",
                         CONFIG_SUB_BOARD_MAIN_IP_FALLBACK);
                return 0;
            }
            ESP_LOGE(TAG, "invalid main-board fallback IP: %s",
                     CONFIG_SUB_BOARD_MAIN_IP_FALLBACK);
        }
#endif
        return -1;
    }

    *main_addr = peer;
    main_addr->sin_port = htons(UART_LINK_DATA_PORT);
    return 0;
}

static void uart_link_task(void *arg)
{
    bool retry_reported = false;

    (void)arg;

    while (true) {
        if (!wifi_is_connected()) {
            s_connected = false;
            retry_delay(UART_LINK_RETRY_MS);
            continue;
        }

        struct sockaddr_in main_addr;
        if (discover_main(&main_addr) != 0) {
            if (!retry_reported) {
                ESP_LOGW(TAG,
                         "main board unavailable; retrying in background");
                retry_reported = true;
            } else {
                ESP_LOGD(TAG, "main board not found; retrying");
            }
            retry_delay(UART_LINK_DISCOVERY_MS);
            continue;
        }

        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (fd < 0 || connect_with_timeout(
                          fd, (struct sockaddr *)&main_addr,
                          sizeof(main_addr)) < 0) {
            if (fd >= 0) {
                close(fd);
            }
            if (!retry_reported) {
                ESP_LOGW(TAG,
                         "main board unavailable; retrying in background: "
                         "ip=%s errno=%d",
                         inet_ntoa(main_addr.sin_addr), errno);
                retry_reported = true;
            } else {
                ESP_LOGD(TAG, "TCP connect failed: %d", errno);
            }
            retry_delay(UART_LINK_RETRY_MS);
            continue;
        }

        static unsigned int seq;
        char frame[128];
        unsigned int hello_seq = ++seq;
        int len = snprintf(frame, sizeof(frame),
                           "UART_LINK/1 HELLO %u 15 SUB_BOARD_READY\n",
                           hello_seq);
        if (send_all(fd, frame, (size_t)len) != 0) {
            close(fd);
            continue;
        }

        struct timeval timeout = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (wait_for_reply(fd, "HELLO_ACK", hello_seq) != 0) {
            if (!retry_reported) {
                ESP_LOGW(TAG,
                         "main board handshake unavailable; retrying in "
                         "background");
                retry_reported = true;
            } else {
                ESP_LOGD(TAG, "HELLO_ACK not received");
            }
            close(fd);
            retry_delay(UART_LINK_RETRY_MS);
            continue;
        }

        s_connected = true;
        retry_reported = false;
        ESP_LOGI(TAG, "connected to main board (%s)",
                 inet_ntoa(main_addr.sin_addr));
        TickType_t last_heartbeat = xTaskGetTickCount();
        while (wifi_is_connected()) {
            uart_link_event_t event;
            if (xQueueReceive(s_event_queue, &event, 0) == pdTRUE &&
                send_frame_and_wait_ack(fd, event.type, event.payload,
                                        &seq) != 0) {
                break;
            }

            fd_set readfds;
            struct timeval poll_timeout = {0, 200000};
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            int ready = select(fd + 1, &readfds, NULL, NULL, &poll_timeout);
            if (ready < 0) {
                break;
            }
            if (ready > 0 && FD_ISSET(fd, &readfds)) {
                char line[128];
                if (recv_line(fd, line, sizeof(line)) != 0 ||
                    handle_command_frame(fd, line) <= 0) {
                    break;
                }
            }

            TickType_t now = xTaskGetTickCount();
            if (s_heartbeat_paused &&
                now - s_heartbeat_paused_at >=
                    pdMS_TO_TICKS(UART_LINK_HEARTBEAT_PAUSE_MAX_MS)) {
                s_heartbeat_paused = false;
                last_heartbeat = now;
            }
            if (!s_heartbeat_paused &&
                now - last_heartbeat >=
                    pdMS_TO_TICKS(UART_LINK_HEARTBEAT_MS)) {
                unsigned int ping_seq = ++seq;
                len = snprintf(frame, sizeof(frame),
                               "UART_LINK/1 PING %u 2 OK\n", ping_seq);
                if (send_all(fd, frame, (size_t)len) != 0 ||
                    wait_for_reply(fd, "PONG", ping_seq) != 0) {
                    break;
                }
                last_heartbeat = now;
            }
        }
        s_connected = false;
        close(fd);
        ESP_LOGW(TAG, "link disconnected; retrying");
        retry_reported = true;
        retry_delay(UART_LINK_RETRY_MS);
    }
}

esp_err_t UART_LINK_Start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_event_queue == NULL) {
        s_event_queue = xQueueCreate(UART_LINK_EVENT_QUEUE_LEN,
                                     sizeof(uart_link_event_t));
        if (s_event_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_pcm_task == NULL &&
        xTaskCreate(pcm_server_task, "pcm_server", 16384, NULL, 5,
                    &s_pcm_task) != pdPASS) {
        s_pcm_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uart_link_task, "uart_link", 4096, NULL, 5, &s_task) !=
        pdPASS) {
        vTaskDelete(s_pcm_task);
        s_pcm_task = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool UART_LINK_IsConnected(void)
{
    return s_connected;
}

void UART_LINK_SetFlagHandler(uart_link_flag_handler_t handler, void *arg)
{
    s_flag_handler = handler;
    s_flag_handler_arg = arg;
}

void UART_LINK_SetSpeechHandler(uart_link_speech_handler_t handler, void *arg)
{
    s_speech_handler = handler;
    s_speech_handler_arg = arg;
}

static esp_err_t queue_event(const char *type, const char *payload)
{
    uart_link_event_t event = {0};

    if (!s_connected || s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlcpy(event.type, type, sizeof(event.type)) >= sizeof(event.type) ||
        strlcpy(event.payload, payload, sizeof(event.payload)) >=
            sizeof(event.payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE ?
           ESP_OK : ESP_ERR_NO_MEM;
}

static bool valid_direction(const char *direction)
{
    return direction != NULL &&
           (strcmp(direction, "front") == 0 ||
            strcmp(direction, "left") == 0 ||
            strcmp(direction, "right") == 0);
}

static void copy_utf8_payload(char *dst, size_t dst_size, const char *src)
{
    size_t in = 0;
    size_t out = 0;

    while (src[in] != '\0' && out + 1 < dst_size) {
        unsigned char first = (unsigned char)src[in];
        size_t bytes = 1;

        if ((first & 0xe0) == 0xc0) {
            bytes = 2;
        } else if ((first & 0xf0) == 0xe0) {
            bytes = 3;
        } else if ((first & 0xf8) == 0xf0) {
            bytes = 4;
        }
        if (out + bytes >= dst_size) {
            break;
        }
        for (size_t i = 1; i < bytes; i++) {
            if (src[in + i] == '\0' ||
                ((unsigned char)src[in + i] & 0xc0) != 0x80) {
                bytes = 1;
                break;
            }
        }
        memcpy(dst + out, src + in, bytes);
        in += bytes;
        out += bytes;
    }
    dst[out] = '\0';
}

esp_err_t UART_LINK_SendObstacle(int distance_cm, const char *direction,
                                 int confidence)
{
    char payload[96];
    if (distance_cm <= 0 || !valid_direction(direction) || confidence < 0 ||
        confidence > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(payload, sizeof(payload), "%d %s %d", distance_cm, direction,
             confidence);
    return queue_event("OBSTACLE", payload);
}

esp_err_t UART_LINK_SendStepDown(const char *direction, int confidence)
{
    char payload[96];
    if (!valid_direction(direction) || confidence < 0 || confidence > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(payload, sizeof(payload), "%s %d", direction, confidence);
    return queue_event("STEP_DOWN", payload);
}

esp_err_t UART_LINK_SendEmergency(const char *source)
{
    if (source == NULL || source[0] == '\0' || strchr(source, ' ') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return queue_event("EMERGENCY", source);
}

esp_err_t UART_LINK_SendASRResult(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[96];
    copy_utf8_payload(buf, sizeof(buf), text);
    return queue_event("ASR_RESULT", buf);
}

esp_err_t UART_LINK_SendVoiceIdle(void)
{
    return queue_event("VOICE_IDLE", "OK");
}

esp_err_t UART_LINK_SendHealthResult(int heart_rate_bpm)
{
    char payload[16];

    if (heart_rate_bpm < 0 || heart_rate_bpm > 250) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(payload, sizeof(payload), "%d", heart_rate_bpm);
    return queue_event("HEALTH_DONE", payload);
}

void UART_LINK_SetASRHandler(uart_link_asr_handler_t handler, void *arg)
{
    s_asr_handler = handler;
    s_asr_handler_arg = arg;
}

size_t UART_LINK_GetPCMLen(void)
{
    return s_last_pcm_len;
}
