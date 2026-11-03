#include "uart_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define UART_LINK_DISCOVERY_PORT 45678
#define UART_LINK_DATA_PORT      45679
#define UART_LINK_LINE_SIZE      256
#define UART_LINK_IDLE_TIMEOUT_S 25
#define UART_LINK_CONTROL_QUEUE  "blind_badge_link"
#define UART_LINK_CONTROL_PATH   "/var/mqueue/blind_badge_link"
#define UART_LINK_FLAG_SIZE      32
#define UART_LINK_SPEECH_ACK_MS  5000
#define UART_LINK_ACK_POLL_US    20000
#define UART_LINK_SEND_WAIT_US   200000
#define UART_LINK_SEND_RETRIES   5

/* The UART link must keep working when no USB serial reader is attached.
 * Printing from this network thread can fill the CDC TX buffer and block the
 * protocol after only a few menu changes.  Keep runtime tracing disabled;
 * explicit NSH status commands remain available to the operator.
 */

#define UART_LINK_LOG(...) do { } while (0)

static pthread_t g_thread;
static volatile int g_running;
static volatile int g_connected;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static unsigned int g_tx_seq;
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_speech_ack_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_speech_ack_seq;
static uart_link_event_handler_t g_event_handler;
static void *g_event_handler_arg;

static time_t monotonic_seconds(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
      return 0;
    }

  return now.tv_sec;
}

static int valid_flag(const char *flag, size_t *flag_len)
{
  size_t i;

  if (flag == NULL || flag[0] == '\0')
    {
      return 0;
    }

  for (i = 0; flag[i] != '\0'; i++)
    {
      char ch = flag[i];
      if (i >= UART_LINK_FLAG_SIZE - 1 ||
          !((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '_'))
        {
          return 0;
        }
    }

  if (flag_len != NULL)
    {
      *flag_len = i;
    }
  return 1;
}

static int send_all(int fd, const char *data, size_t len)
{
  int wait_count = 0;

  while (len > 0)
    {
      ssize_t n = send(fd, data, len, 0);

      if (n > 0)
        {
          data += n;
          len -= n;
          wait_count = 0;
          continue;
        }

      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          wait_count++ < UART_LINK_SEND_RETRIES)
        {
          fd_set writefds;
          struct timeval timeout =
            {
              0, UART_LINK_SEND_WAIT_US
            };
          int ready;

          FD_ZERO(&writefds);
          FD_SET(fd, &writefds);
          ready = select(fd + 1, NULL, &writefds, NULL, &timeout);
          if (ready > 0)
            {
              continue;
            }

          if (ready < 0 && errno == EINTR)
            {
              continue;
            }
        }

      return -1;
    }
  return 0;
}

static void close_client(void)
{
  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0)
    {
      close(g_client_fd);
      g_client_fd = -1;
    }
  g_connected = 0;
  pthread_mutex_unlock(&g_client_lock);
}

static int send_client_data(const char *data, size_t len)
{
  int ret = -1;

  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0)
    {
      ret = send_all(g_client_fd, data, len);
      if (ret != 0)
        {
          close(g_client_fd);
          g_client_fd = -1;
          g_connected = 0;
        }
    }
  pthread_mutex_unlock(&g_client_lock);
  return ret;
}

static int send_flag_to_client(const char *flag)
{
  char frame[UART_LINK_LINE_SIZE];
  unsigned int seq;
  size_t flag_len;
  int n;
  int ret = -1;

  if (!valid_flag(flag, &flag_len))
    {
      return -1;
    }

  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0)
    {
      seq = ++g_tx_seq;
      n = snprintf(frame, sizeof(frame),
                   "UART_LINK/1 COMMAND %u %u %s\n", seq,
                   (unsigned int)flag_len, flag);
      if (n > 0 && (size_t)n < sizeof(frame))
        {
          ret = send_all(g_client_fd, frame, (size_t)n);
          if (ret == 0)
            {
              UART_LINK_LOG("[UART_LINK] command sent: seq=%u flag=%s\n",
                            seq, flag);
            }
          else
            {
              close(g_client_fd);
              g_client_fd = -1;
              g_connected = 0;
            }
        }
    }
  pthread_mutex_unlock(&g_client_lock);
  return ret;
}

static size_t utf8_prefix_length(const char *text, size_t limit)
{
  size_t len = strlen(text);

  if (len <= limit)
    {
      return len;
    }

  len = limit;
  while (len > 0 && (((unsigned char)text[len] & 0xc0) == 0x80))
    {
      len--;
    }

  return len;
}

int UART_LINK_SendSpeech(const char *text)
{
  char frame[UART_LINK_LINE_SIZE];
  struct timespec started;
  unsigned int seq;
  size_t text_len;
  int n;
  int ret = -1;

  if (text == NULL || text[0] == '\0')
    {
      return -1;
    }

  text_len = utf8_prefix_length(text, 90);
  if (text_len == 0)
    {
      return -1;
    }

  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0)
    {
      seq = ++g_tx_seq;
      n = snprintf(frame, sizeof(frame),
                   "UART_LINK/1 SPEAK %u %u %.*s\n", seq,
                   (unsigned int)text_len, (int)text_len, text);
      if (n > 0 && (size_t)n < sizeof(frame))
        {
          ret = send_all(g_client_fd, frame, (size_t)n);
          if (ret != 0)
            {
              close(g_client_fd);
              g_client_fd = -1;
              g_connected = 0;
            }
        }
    }

  pthread_mutex_unlock(&g_client_lock);
  if (ret != 0)
    {
      return ret;
    }

  /* A successful send() only means that the frame entered the local TCP
   * buffer.  During a Wi-Fi reconnect the old socket can accept the write
   * and then disappear before the sub-board receives it.  Treat SPEAK as
   * delivered only after the peer acknowledges the matching sequence.
   */

  if (clock_gettime(CLOCK_MONOTONIC, &started) != 0)
    {
      return -1;
    }

  for (;;)
    {
      struct timespec now;
      long elapsed_ms;

      pthread_mutex_lock(&g_speech_ack_lock);
      ret = g_speech_ack_seq == seq ? 0 : -1;
      pthread_mutex_unlock(&g_speech_ack_lock);
      if (ret == 0 || !g_connected)
        {
          break;
        }

      if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        {
          ret = -1;
          break;
        }

      elapsed_ms = (now.tv_sec - started.tv_sec) * 1000L +
                   (now.tv_nsec - started.tv_nsec) / 1000000L;
      if (elapsed_ms >= UART_LINK_SPEECH_ACK_MS)
        {
          ret = -1;
          break;
        }

      usleep(UART_LINK_ACK_POLL_US);
    }

  return ret;
}

static int handle_line(const char *line)
{
  struct uart_link_event event;
  char frame[UART_LINK_LINE_SIZE];
  char *saveptr;
  char *token;
  char *end;
  unsigned int seq;
  unsigned int len;
  char type[32];
  char payload[UART_LINK_LINE_SIZE];
  int parsed;

  strncpy(frame, line, sizeof(frame) - 1);
  frame[sizeof(frame) - 1] = '\0';
  payload[0] = '\0';

  token = strtok_r(frame, " ", &saveptr);
  if (token == NULL || strcmp(token, "UART_LINK/1") != 0)
    {
      UART_LINK_LOG("[UART_LINK] rejected malformed frame: %s\n", line);
      return 0;
    }

  token = strtok_r(NULL, " ", &saveptr);
  if (token == NULL || strlen(token) >= sizeof(type))
    {
      UART_LINK_LOG("[UART_LINK] rejected malformed frame: %s\n", line);
      return 0;
    }
  strcpy(type, token);

  token = strtok_r(NULL, " ", &saveptr);
  if (token == NULL)
    {
      UART_LINK_LOG("[UART_LINK] rejected malformed frame: %s\n", line);
      return 0;
    }
  seq = (unsigned int)strtoul(token, &end, 10);
  if (*token == '\0' || *end != '\0')
    {
      UART_LINK_LOG("[UART_LINK] rejected malformed sequence: %s\n", line);
      return 0;
    }

  token = strtok_r(NULL, " ", &saveptr);
  if (token == NULL)
    {
      UART_LINK_LOG("[UART_LINK] rejected malformed frame: %s\n", line);
      return 0;
    }
  len = (unsigned int)strtoul(token, &end, 10);
  if (*token == '\0' || *end != '\0')
    {
      UART_LINK_LOG("[UART_LINK] rejected malformed length: %s\n", line);
      return 0;
    }

  while (*saveptr == ' ')
    {
      saveptr++;
    }
  strncpy(payload, saveptr, sizeof(payload) - 1);
  payload[sizeof(payload) - 1] = '\0';

  if (len > UART_LINK_LINE_SIZE - 1)
    {
      UART_LINK_LOG("[UART_LINK] rejected oversized frame: type=%s len=%u\n",
                    type, len);
      return 0;
    }

  if (strlen(payload) != len)
    {
      UART_LINK_LOG("[UART_LINK] rejected length mismatch: type=%s "
                    "declared=%u actual=%u payload=%s\n", type, len,
                    (unsigned int)strlen(payload), payload);
      return 0;
    }

  if (strcmp(type, "HELLO") == 0)
    {
      char ack[96];
      int ret;
      int n = snprintf(ack, sizeof(ack),
                       "UART_LINK/1 HELLO_ACK %u 2 OK\n", seq);
      UART_LINK_LOG("[UART_LINK] RX type=HELLO seq=%u payload=%s\n", seq,
                    payload);
      ret = send_client_data(ack, (size_t)n);
      if (ret == 0)
        {
          g_connected = 1;
        }

      return ret;
    }
  else if (strcmp(type, "PING") == 0)
    {
      char pong[96];
      int n = snprintf(pong, sizeof(pong),
                       "UART_LINK/1 PONG %u 2 OK\n", seq);
      return send_client_data(pong, (size_t)n);
    }
  else if (strcmp(type, "COMMAND_ACK") == 0)
    {
      if (strcmp(payload, "OK") != 0)
        {
          return 0;
        }
      UART_LINK_LOG("[UART_LINK] command acknowledged: seq=%u\n", seq);
      return 0;
    }
  else if (strcmp(type, "SPEAK_ACK") == 0)
    {
      if (strcmp(payload, "OK") == 0)
        {
          pthread_mutex_lock(&g_speech_ack_lock);
          g_speech_ack_seq = seq;
          pthread_mutex_unlock(&g_speech_ack_lock);
        }

      return 0;
    }

  memset(&event, 0, sizeof(event));
  if (strcmp(type, "OBSTACLE") == 0)
    {
      event.type = UART_LINK_EVENT_OBSTACLE;
      parsed = sscanf(payload, "%d %15s %d", &event.distance_cm,
                      event.direction, &event.confidence);
      if (parsed != 3 || event.distance_cm <= 0 || event.confidence < 0 ||
          event.confidence > 100)
        {
          return 0;
        }
    }
  else if (strcmp(type, "STEP_DOWN") == 0)
    {
      event.type = UART_LINK_EVENT_STEP_DOWN;
      parsed = sscanf(payload, "%15s %d", event.direction,
                      &event.confidence);
      if (parsed != 2 || event.confidence < 0 || event.confidence > 100)
        {
          return 0;
        }
    }
  else if (strcmp(type, "EMERGENCY") == 0)
    {
      event.type = UART_LINK_EVENT_EMERGENCY;
      if (sscanf(payload, "%15s", event.source) != 1)
        {
          return 0;
        }
      event.confidence = 100;
    }
  else if (strcmp(type, "ASR_RESULT") == 0)
    {
      event.type = UART_LINK_EVENT_ASR_RESULT;
      if (payload[0] == '\0' || strlen(payload) >= sizeof(event.text))
        {
          return 0;
        }

      strlcpy(event.text, payload, sizeof(event.text));
    }
  else if (strcmp(type, "VOICE_IDLE") == 0)
    {
      event.type = UART_LINK_EVENT_VOICE_IDLE;
      if (strcmp(payload, "OK") != 0)
        {
          return 0;
        }
    }
  else
    {
      return 0;
    }

  {
    char ack[96];
    int n = snprintf(ack, sizeof(ack),
                     "UART_LINK/1 EVENT_ACK %u 2 OK\n", seq);
    if (send_client_data(ack, (size_t)n) != 0)
      {
        return -1;
      }
  }

  UART_LINK_LOG("[UART_LINK] RX type=%s seq=%u payload=%s\n", type, seq,
                payload);
  if (g_event_handler != NULL)
    {
      g_event_handler(&event, g_event_handler_arg);
    }

  return 0;
}

static void *uart_link_thread(void *arg)
{
  int discovery_fd = -1;
  mqd_t control_mq = (mqd_t)-1;
  struct mq_attr mq_attr;
  struct sockaddr_in addr;
  char buffer[UART_LINK_LINE_SIZE];
  char pending_flag[UART_LINK_FLAG_SIZE] = { 0 };
  size_t used = 0;
  time_t last_activity = 0;
  (void)arg;

  discovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (discovery_fd < 0 || g_listen_fd < 0)
    {
      goto done;
    }

  memset(&mq_attr, 0, sizeof(mq_attr));
  mq_attr.mq_maxmsg = 8;
  mq_attr.mq_msgsize = UART_LINK_FLAG_SIZE;
  mq_unlink(UART_LINK_CONTROL_QUEUE);
  control_mq = mq_open(UART_LINK_CONTROL_QUEUE,
                       O_RDONLY | O_CREAT | O_NONBLOCK, 0666, &mq_attr);
  if (control_mq == (mqd_t)-1)
    {
      UART_LINK_LOG("[UART_LINK] control queue create failed: errno=%d\n",
                    errno);
      goto done;
    }
  if (chmod(UART_LINK_CONTROL_PATH, 0666) != 0)
    {
      UART_LINK_LOG("[UART_LINK] control queue chmod failed: errno=%d\n",
                    errno);
    }

  int yes = 1;
  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(UART_LINK_DISCOVERY_PORT);
  if (bind(discovery_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    goto done;

  addr.sin_port = htons(UART_LINK_DATA_PORT);
  if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(g_listen_fd, 1) < 0)
    goto done;

  while (g_running)
    {
      fd_set readfds;
      int maxfd = discovery_fd > g_listen_fd ? discovery_fd : g_listen_fd;
      FD_ZERO(&readfds);
      FD_SET(discovery_fd, &readfds);
      FD_SET(g_listen_fd, &readfds);
      if (g_client_fd >= 0)
        {
          FD_SET(g_client_fd, &readfds);
          if (g_client_fd > maxfd) maxfd = g_client_fd;
        }

      struct timeval timeout = { 1, 0 };
      if (select(maxfd + 1, &readfds, NULL, NULL, &timeout) < 0)
        {
          if (errno == EINTR) continue;
          break;
        }

      if (g_client_fd >= 0 &&
          monotonic_seconds() - last_activity > UART_LINK_IDLE_TIMEOUT_S)
        {
          close_client();
          used = 0;
        }

      /* Do not consume control commands while the sub-board is offline.
       * Keep the current command pending if a send detects a disconnect, then
       * retry it after the next TCP handshake.  The menu treats a successful
       * mq_send() as accepted ownership, so dropping the command here would
       * otherwise leave the sub-board stuck in SILENT mode indefinitely.
       */

      while (control_mq != (mqd_t)-1 && g_client_fd >= 0 && g_connected)
        {
          if (pending_flag[0] == '\0')
            {
              ssize_t n = mq_receive(control_mq, pending_flag,
                                     sizeof(pending_flag), NULL);
              if (n < 0)
                {
                  break;
                }
              if ((size_t)n >= sizeof(pending_flag))
                {
                  pending_flag[0] = '\0';
                  continue;
                }

              pending_flag[n] = '\0';
            }

          if (send_flag_to_client(pending_flag) != 0)
            {
              UART_LINK_LOG("[UART_LINK] command retained: disconnected "
                            "flag=%s\n", pending_flag);
              break;
            }

          pending_flag[0] = '\0';
        }

      if (FD_ISSET(discovery_fd, &readfds))
        {
          char probe[64];
          struct sockaddr_in peer;
          socklen_t peer_len = sizeof(peer);
          ssize_t n = recvfrom(discovery_fd, probe, sizeof(probe) - 1, 0,
                               (struct sockaddr *)&peer, &peer_len);
          if (n > 0)
            {
              probe[n] = '\0';
              if (strncmp(probe, "UART_LINK/1 DISCOVER", 20) == 0)
                {
                  static const char reply[] = "UART_LINK/1 DISCOVER_ACK 0 2 OK\n";
                  sendto(discovery_fd, reply, sizeof(reply) - 1, 0,
                         (struct sockaddr *)&peer, peer_len);
                }
            }
        }

      /* Keep the current handshake alive. A new connection can become
       * readable in the same select cycle as HELLO on the current client;
       * replacing the client first would drop that HELLO and its ACK. */
      if (g_client_fd < 0 && FD_ISSET(g_listen_fd, &readfds))
        {
          int fd = accept(g_listen_fd, NULL, NULL);
          if (fd >= 0)
            {
              struct timeval send_timeout = { 1, 0 };

              setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                         sizeof(send_timeout));

              close_client();
              pthread_mutex_lock(&g_client_lock);
              g_client_fd = fd;
              /* A TCP accept is not yet a usable protocol connection.
               * Publish readiness only after HELLO has been parsed and its
               * HELLO_ACK sent, otherwise SPEAK/COMMAND can race ahead of
               * the sub-board handshake.
               */

              g_connected = 0;
              pthread_mutex_unlock(&g_client_lock);
              used = 0;
              last_activity = monotonic_seconds();
              UART_LINK_LOG("[UART_LINK] TCP peer connected\n");
            }
        }

      if (g_client_fd >= 0 && FD_ISSET(g_client_fd, &readfds))
        {
          char chunk[64];
          ssize_t n = recv(g_client_fd, chunk, sizeof(chunk), 0);
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
              continue;
            }
          else if (n <= 0)
            {
              close_client();
              used = 0;
            }
          else if (used + (size_t)n >= sizeof(buffer))
            {
              close_client();
              used = 0;
            }
          else
            {
              last_activity = monotonic_seconds();
              memcpy(buffer + used, chunk, (size_t)n);
              used += (size_t)n;
              buffer[used] = '\0';
              char *line;
              while ((line = strchr(buffer, '\n')) != NULL)
                {
                  *line = '\0';
                  if (handle_line(buffer) != 0)
                    {
                      close_client();
                      used = 0;
                      break;
                    }
                  size_t consumed = (size_t)(line - buffer) + 1;
                  memmove(buffer, buffer + consumed, used - consumed);
                  used -= consumed;
                  buffer[used] = '\0';
                }
            }
        }
    }

done:
  g_running = 0;
  close_client();
  if (discovery_fd >= 0) close(discovery_fd);
  if (control_mq != (mqd_t)-1) mq_close(control_mq);
  mq_unlink(UART_LINK_CONTROL_QUEUE);
  if (g_listen_fd >= 0) close(g_listen_fd);
  g_listen_fd = -1;
  return NULL;
}

int UART_LINK_Start(void)
{
  pthread_attr_t attr;
  int ret;

  if (g_running) return 0;
  g_running = 1;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  ret = pthread_create(&g_thread, &attr, uart_link_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      g_running = 0;
      return -1;
    }
  return 0;
}

int UART_LINK_Wait(void)
{
  if (!g_running)
    {
      return 0;
    }

  return pthread_join(g_thread, NULL) == 0 ? 0 : -1;
}

int UART_LINK_Stop(void)
{
  if (!g_running) return 0;
  g_running = 0;
  pthread_join(g_thread, NULL);
  return 0;
}

int UART_LINK_IsConnected(void)
{
  return g_connected;
}

int UART_LINK_SendFlag(const char *flag)
{
  mqd_t mq;
  size_t flag_len;
  int ret;

  if (!valid_flag(flag, &flag_len))
    {
      return -1;
    }

  if (g_running && pthread_equal(pthread_self(), g_thread))
    {
      return send_flag_to_client(flag);
    }

  mq = mq_open(UART_LINK_CONTROL_QUEUE, O_WRONLY | O_NONBLOCK);
  if (mq == (mqd_t)-1)
    {
      UART_LINK_LOG("[UART_LINK] control queue open failed: errno=%d\n",
                    errno);
      return -1;
    }

  ret = mq_send(mq, flag, flag_len, 0);
  if (ret != 0)
    {
      UART_LINK_LOG("[UART_LINK] control queue send failed: errno=%d\n",
                    errno);
    }
  mq_close(mq);
  return ret == 0 ? 0 : -1;
}

void UART_LINK_SetEventHandler(uart_link_event_handler_t handler, void *arg)
{
  g_event_handler = handler;
  g_event_handler_arg = arg;
}
