#include "speech_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#define SPEECH_QUEUE_LEN 8
#define SPEECH_HIGH_PRIORITY 80
#define SPEECH_DEDUP_SLOTS 16
#define SPEECH_SOCKET_TIMEOUT_S 5

typedef struct {
    uint64_t sequence;
    uint32_t generation;
    uint8_t priority;
    speech_link_mode_t mode;
    char text[SPEECH_LINK_MAX_TEXT_BYTES + 1];
} speech_message_t;

static const char *TAG = "SPEECH_LINK";
static TaskHandle_t s_server_task;
static TaskHandle_t s_player_task;
static QueueHandle_t s_high_queue;
static QueueHandle_t s_normal_queue;
static SemaphoreHandle_t s_mode_mutex;
static speech_link_handler_t s_handler;
static void *s_handler_arg;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static speech_link_status_t s_status;
static uint64_t s_recent_sequences[SPEECH_DEDUP_SLOTS];
static size_t s_recent_next;
static uint64_t s_local_sequence = UINT64_C(0x8000000000000000);
static speech_link_mode_t s_mode = SPEECH_LINK_MODE_SILENT;
static uint32_t s_generation = 1;

static uint32_t speech_crc32(const uint8_t *data, size_t len)
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

static bool valid_utf8(const uint8_t *data, size_t len)
{
    size_t i = 0;

    while (i < len) {
        uint8_t first = data[i];
        size_t count;
        uint32_t codepoint;

        if (first == 0) {
            return false;
        } else if (first < 0x80) {
            i++;
            continue;
        } else if ((first & 0xe0) == 0xc0) {
            count = 2;
            codepoint = first & 0x1f;
        } else if ((first & 0xf0) == 0xe0) {
            count = 3;
            codepoint = first & 0x0f;
        } else if ((first & 0xf8) == 0xf0) {
            count = 4;
            codepoint = first & 0x07;
        } else {
            return false;
        }
        if (i + count > len) {
            return false;
        }
        for (size_t j = 1; j < count; j++) {
            if ((data[i + j] & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (data[i + j] & 0x3f);
        }
        if ((count == 2 && codepoint < 0x80) ||
            (count == 3 && codepoint < 0x800) ||
            (count == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) {
            return false;
        }
        i += count;
    }
    return len > 0;
}

static int recv_all(int fd, void *buffer, size_t len)
{
    uint8_t *cursor = buffer;

    while (len > 0) {
        ssize_t received = recv(fd, cursor, len, 0);
        if (received <= 0) {
            return -1;
        }
        cursor += received;
        len -= (size_t)received;
    }
    return 0;
}

static int recv_line(int fd, char *line, size_t line_size)
{
    size_t used = 0;

    while (used + 1 < line_size) {
        char ch;
        ssize_t received = recv(fd, &ch, 1, 0);
        if (received <= 0) {
            return -1;
        }
        if (ch == '\n') {
            line[used] = '\0';
            return 0;
        }
        if (ch != '\r') {
            line[used++] = ch;
        }
    }
    return -1;
}

static void send_ack(int fd, uint64_t sequence, const char *status)
{
    char reply[96];
    int length = snprintf(reply, sizeof(reply),
                          "BBSP/1 ACK %" PRIu64 " %s\n", sequence, status);

    if (length > 0 && (size_t)length < sizeof(reply)) {
        size_t sent = 0;
        while (sent < (size_t)length) {
            ssize_t result = send(fd, reply + sent, (size_t)length - sent, 0);
            if (result <= 0) {
                break;
            }
            sent += (size_t)result;
        }
    }
}

static bool sequence_seen(uint64_t sequence)
{
    bool found = false;

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < SPEECH_DEDUP_SLOTS; i++) {
        if (s_recent_sequences[i] == sequence) {
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static void remember_sequence(uint64_t sequence)
{
    portENTER_CRITICAL(&s_lock);
    s_recent_sequences[s_recent_next] = sequence;
    s_recent_next = (s_recent_next + 1) % SPEECH_DEDUP_SLOTS;
    portEXIT_CRITICAL(&s_lock);
}

static void update_queued_count(void)
{
    uint32_t queued = (uint32_t)uxQueueMessagesWaiting(s_high_queue) +
                      (uint32_t)uxQueueMessagesWaiting(s_normal_queue);
    portENTER_CRITICAL(&s_lock);
    s_status.queued = queued;
    portEXIT_CRITICAL(&s_lock);
}

static void handle_connection(int fd, const struct sockaddr_in *peer)
{
    char header[128];
    char version[16];
    char command[16];
    unsigned long long sequence_value;
    unsigned int priority;
    unsigned int text_len;
    unsigned int expected_crc;
    speech_message_t message = {0};
    QueueHandle_t queue;

    if (recv_line(fd, header, sizeof(header)) != 0 ||
        sscanf(header, "%15s %15s %llu %u %u %x", version, command,
               &sequence_value, &priority, &text_len, &expected_crc) != 6 ||
        strcmp(version, "BBSP/1") != 0 || strcmp(command, "SPEAK") != 0 ||
        sequence_value == 0 || priority > 100 || text_len == 0 ||
        text_len > SPEECH_LINK_MAX_TEXT_BYTES) {
        portENTER_CRITICAL(&s_lock);
        s_status.rejected++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, 0, "BAD_REQUEST");
        return;
    }

    message.sequence = (uint64_t)sequence_value;
    message.priority = (uint8_t)priority;

    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    if (s_mode != SPEECH_LINK_MODE_VISION) {
        xSemaphoreGive(s_mode_mutex);
        portENTER_CRITICAL(&s_lock);
        s_status.rejected++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, message.sequence, "INACTIVE");
        return;
    }
    message.mode = s_mode;
    message.generation = s_generation;
    xSemaphoreGive(s_mode_mutex);

    if (sequence_seen(message.sequence)) {
        portENTER_CRITICAL(&s_lock);
        s_status.duplicates++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, message.sequence, "DUPLICATE");
        return;
    }
    if (recv_all(fd, message.text, text_len) != 0) {
        portENTER_CRITICAL(&s_lock);
        s_status.rejected++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, message.sequence, "INCOMPLETE");
        return;
    }
    message.text[text_len] = '\0';
    if (!valid_utf8((const uint8_t *)message.text, text_len)) {
        portENTER_CRITICAL(&s_lock);
        s_status.rejected++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, message.sequence, "BAD_UTF8");
        return;
    }
    if (speech_crc32((const uint8_t *)message.text, text_len) !=
        expected_crc) {
        portENTER_CRITICAL(&s_lock);
        s_status.crc_errors++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, message.sequence, "BAD_CRC");
        return;
    }

    /* The mode may have changed while the TCP body was being received.  Check
     * it again while holding the same mutex used by mode transitions, so an
     * old Vision message is never acknowledged as queued for a new task.
     */

    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    if (s_mode != SPEECH_LINK_MODE_VISION ||
        message.generation != s_generation) {
        xSemaphoreGive(s_mode_mutex);
        portENTER_CRITICAL(&s_lock);
        s_status.rejected++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, message.sequence, "INACTIVE");
        return;
    }

    queue = priority >= SPEECH_HIGH_PRIORITY ? s_high_queue : s_normal_queue;
    if (xQueueSend(queue, &message, 0) != pdTRUE) {
        xSemaphoreGive(s_mode_mutex);
        portENTER_CRITICAL(&s_lock);
        s_status.queue_full++;
        portEXIT_CRITICAL(&s_lock);
        send_ack(fd, message.sequence, "BUSY");
        return;
    }
    xSemaphoreGive(s_mode_mutex);

    remember_sequence(message.sequence);
    portENTER_CRITICAL(&s_lock);
    s_status.accepted++;
    portEXIT_CRITICAL(&s_lock);
    update_queued_count();
    send_ack(fd, message.sequence, "QUEUED");
    ESP_LOGI(TAG, "queued seq=%" PRIu64 " priority=%u bytes=%u from %s",
             message.sequence, message.priority, text_len,
             inet_ntoa(peer->sin_addr));
}

static void speech_server_task(void *arg)
{
    struct sockaddr_in address = {0};
    int listen_fd;
    int yes = 1;

    (void)arg;
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        goto exit;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(SPEECH_LINK_PORT);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 2) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(listen_fd);
        goto exit;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Windows speech server listening on TCP %d",
             SPEECH_LINK_PORT);
    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        struct timeval timeout = {SPEECH_SOCKET_TIMEOUT_S, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle_connection(fd, &peer);
        close(fd);
    }

exit:
    portENTER_CRITICAL(&s_lock);
    s_status.running = false;
    portEXIT_CRITICAL(&s_lock);
    s_server_task = NULL;
    vTaskDelete(NULL);
}

static void speech_player_task(void *arg)
{
    speech_message_t message;

    (void)arg;
    while (true) {
        BaseType_t received = xQueueReceive(s_high_queue, &message, 0);
        if (received != pdTRUE) {
            received = xQueueReceive(s_normal_queue, &message,
                                     pdMS_TO_TICKS(100));
        }
        if (received != pdTRUE) {
            continue;
        }
        update_queued_count();

        /* Hold the mode mutex through playback.  A mode transition therefore
         * waits for the current sentence to finish, then clears every queued
         * message from the old task before the new task can speak.
         */

        bool delivered = false;

        xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
        if (message.mode == s_mode && message.generation == s_generation &&
            s_handler != NULL) {
            s_handler(message.text, message.priority, message.sequence,
                      message.mode, s_handler_arg);
            delivered = true;
        }
        xSemaphoreGive(s_mode_mutex);
        portENTER_CRITICAL(&s_lock);
        if (delivered) {
            s_status.delivered++;
        } else {
            s_status.rejected++;
        }
        portEXIT_CRITICAL(&s_lock);
    }
}

void SPEECH_LINK_SetHandler(speech_link_handler_t handler, void *arg)
{
    s_handler = handler;
    s_handler_arg = arg;
}

esp_err_t SPEECH_LINK_Start(void)
{
    if (s_server_task != NULL) {
        return ESP_OK;
    }
    if (s_high_queue == NULL) {
        s_high_queue = xQueueCreate(SPEECH_QUEUE_LEN, sizeof(speech_message_t));
        s_normal_queue = xQueueCreate(SPEECH_QUEUE_LEN,
                                      sizeof(speech_message_t));
        if (s_high_queue == NULL || s_normal_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_mode_mutex == NULL) {
        s_mode_mutex = xSemaphoreCreateMutex();
        if (s_mode_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(speech_player_task, "speech_player", 4096, NULL, 5,
                    &s_player_task) != pdPASS) {
        s_player_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(speech_server_task, "speech_server", 4096, NULL, 5,
                    &s_server_task) != pdPASS) {
        vTaskDelete(s_player_task);
        s_player_task = NULL;
        s_server_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t SPEECH_LINK_SetMode(speech_link_mode_t mode)
{
    if (mode < SPEECH_LINK_MODE_SILENT || mode > SPEECH_LINK_MODE_ALERT ||
        s_mode_mutex == NULL || s_high_queue == NULL ||
        s_normal_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    if (s_mode != mode) {
        s_mode = mode;
        s_generation++;
        xQueueReset(s_high_queue);
        xQueueReset(s_normal_queue);
        update_queued_count();
    }
    xSemaphoreGive(s_mode_mutex);
    return ESP_OK;
}

static esp_err_t enqueue_local(const char *text, uint8_t priority,
                               speech_link_mode_t required_mode)
{
    speech_message_t message = {0};
    QueueHandle_t queue;
    size_t text_len;

    if (text == NULL || priority > 100 || s_high_queue == NULL ||
        s_normal_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    text_len = strlen(text);
    if (text_len == 0 || text_len > SPEECH_LINK_MAX_TEXT_BYTES ||
        !valid_utf8((const uint8_t *)text, text_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    message.sequence = ++s_local_sequence;
    portEXIT_CRITICAL(&s_lock);
    message.priority = priority;
    memcpy(message.text, text, text_len + 1);

    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    if (s_mode != required_mode) {
        xSemaphoreGive(s_mode_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    message.mode = s_mode;
    message.generation = s_generation;

    queue = priority >= SPEECH_HIGH_PRIORITY ? s_high_queue : s_normal_queue;
    if (xQueueSend(queue, &message, 0) != pdTRUE) {
        xSemaphoreGive(s_mode_mutex);
        portENTER_CRITICAL(&s_lock);
        s_status.queue_full++;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mode_mutex);

    portENTER_CRITICAL(&s_lock);
    s_status.accepted++;
    portEXIT_CRITICAL(&s_lock);
    update_queued_count();
    return ESP_OK;
}

esp_err_t SPEECH_LINK_EnqueueMenu(const char *text, uint8_t priority)
{
    return enqueue_local(text, priority, SPEECH_LINK_MODE_MENU);
}

esp_err_t SPEECH_LINK_EnqueueVision(const char *text, uint8_t priority)
{
    return enqueue_local(text, priority, SPEECH_LINK_MODE_VISION);
}

esp_err_t SPEECH_LINK_EnqueueVoice(const char *text, uint8_t priority)
{
    return enqueue_local(text, priority, SPEECH_LINK_MODE_VOICE);
}

esp_err_t SPEECH_LINK_EnqueueHealth(const char *text, uint8_t priority)
{
    return enqueue_local(text, priority, SPEECH_LINK_MODE_HEALTH);
}

esp_err_t SPEECH_LINK_EnqueueAlert(const char *text, uint8_t priority)
{
    return enqueue_local(text, priority, SPEECH_LINK_MODE_ALERT);
}

esp_err_t SPEECH_LINK_GetStatus(speech_link_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
