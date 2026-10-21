#include "uart_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#define UART_LINK_PCM_PORT       45680
#define UART_LINK_LINE_SIZE      256
#define UART_LINK_IDLE_TIMEOUT_S 25
#define UART_LINK_CONTROL_QUEUE  "blind_badge_link"
#define UART_LINK_CONTROL_PATH   "/var/mqueue/blind_badge_link"
#define UART_LINK_FLAG_SIZE      32
#define UART_LINK_PCM_MAGIC      0x42425043
#define UART_LINK_PCM_VERSION    1
#define UART_LINK_PCM_MAX_BYTES  (4 * 16000 * 2)
#define UART_LINK_PCM_CHUNK_SIZE 64

static pthread_t g_thread;
static volatile int g_running;
static volatile int g_connected;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static struct sockaddr_in g_client_addr;
static int g_client_addr_valid;
static unsigned int g_tx_seq;
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;
static uart_link_event_handler_t g_event_handler;
static void *g_event_handler_arg;

static int valid_flag(const char *flag, size_t *flag_len)
{
  size_t i;
  if (flag == NULL || flag[0] == '\0') return 0;
  for (i = 0; flag[i] != '\0'; i++)
    {
      char ch = flag[i];
      if (i >= UART_LINK_FLAG_SIZE - 1 ||
          !((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'))
        return 0;
    }
  if (flag_len) *flag_len = i;
  return 1;
}

static int send_all(int fd, const void *data, size_t len)
{
  const uint8_t *cursor = data;

  while (len > 0)
    {
      ssize_t n = send(fd, cursor, len, 0);
      if (n <= 0) return -1;
      cursor += n;
      len -= n;
    }
  return 0;
}

static int recv_all(int fd, void *data, size_t len)
{
  uint8_t *cursor = data;

  while (len > 0)
    {
      ssize_t n = recv(fd, cursor, len, 0);
      if (n <= 0)
        {
          return -1;
        }

      cursor += n;
      len -= n;
    }

  return 0;
}

static int send_pcm_chunks(int fd, const uint8_t *data, size_t len)
{
  while (len > 0)
    {
      size_t chunk = len > UART_LINK_PCM_CHUNK_SIZE ?
                     UART_LINK_PCM_CHUNK_SIZE : len;

      if (send_all(fd, data, chunk) != 0)
        {
          return -1;
        }

      data += chunk;
      len -= chunk;
      usleep(3000);
    }

  return 0;
}

static uint32_t pcm_crc32(const uint8_t *data, size_t len)
{
  uint32_t crc = UINT32_MAX;
  size_t i;

  for (i = 0; i < len; i++)
    {
      uint32_t value = crc ^ data[i];
      int bit;

      for (bit = 0; bit < 8; bit++)
        {
          value = (value >> 1) ^
                  (0xedb88320u & (uint32_t)-(int32_t)(value & 1));
        }

      crc = value;
    }

  return ~crc;
}

static void close_client(void)
{
  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0)
    {
      close(g_client_fd);
      g_client_fd = -1;
    }
  g_client_addr_valid = 0;
  g_connected = 0;
  pthread_mutex_unlock(&g_client_lock);
}

static int send_client_data(const char *data, size_t len)
{
  int ret = -1;
  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0) ret = send_all(g_client_fd, data, len);
  pthread_mutex_unlock(&g_client_lock);
  return ret;
}

static int send_flag_to_client(const char *flag)
{
  char frame[UART_LINK_LINE_SIZE];
  unsigned int seq;
  size_t flag_len;
  int n, ret = -1;

  if (!valid_flag(flag, &flag_len)) return -1;

  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0)
    {
      seq = ++g_tx_seq;
      n = snprintf(frame, sizeof(frame), "UART_LINK/1 COMMAND %u %u %s\n",
                   seq, (unsigned int)flag_len, flag);
      if (n > 0 && (size_t)n < sizeof(frame))
        {
          ret = send_all(g_client_fd, frame, (size_t)n);
          if (ret == 0)
            printf("[UART_LINK] command sent: seq=%u flag=%s\n", seq, flag);
        }
    }
  pthread_mutex_unlock(&g_client_lock);
  return ret;
}

int UART_LINK_SendPCM(const uint8_t *pcm, size_t pcm_len)
{
  struct sockaddr_in peer;
  struct timeval timeout = {20, 0};
  uint32_t header[7];
  uint32_t reply[5];
  uint32_t crc;
  int fd = -1;
  int tcp_nodelay = 1;
  int ret = -1;

  if (pcm == NULL || pcm_len == 0 || pcm_len > UART_LINK_PCM_MAX_BYTES)
    {
      printf("[UART_LINK] PCM: invalid length=%u\n",
             (unsigned int)pcm_len);
      return -1;
    }

  memset(&peer, 0, sizeof(peer));
  pthread_mutex_lock(&g_client_lock);
  if (g_client_fd >= 0 && g_client_addr_valid)
    {
      peer = g_client_addr;
      ret = 0;
    }
  pthread_mutex_unlock(&g_client_lock);

  if (ret != 0)
    {
      printf("[UART_LINK] PCM: sub-board control link is disconnected\n");
      return -1;
    }

  ret = -1;
  peer.sin_port = htons(UART_LINK_PCM_PORT);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      printf("[UART_LINK] PCM: socket failed errno=%d\n", errno);
      return -1;
    }

  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay,
             sizeof(tcp_nodelay));
  if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) != 0)
    {
      printf("[UART_LINK] PCM: connect %s:%d failed errno=%d\n",
             inet_ntoa(peer.sin_addr), UART_LINK_PCM_PORT, errno);
      goto done;
    }

  crc = pcm_crc32(pcm, pcm_len);
  header[0] = htonl(UART_LINK_PCM_MAGIC);
  header[1] = htonl(UART_LINK_PCM_VERSION);
  header[2] = htonl(16000);
  header[3] = htonl(1);
  header[4] = htonl(16);
  header[5] = htonl((uint32_t)pcm_len);
  header[6] = htonl(crc);

  printf("[UART_LINK] PCM: sending %u bytes to %s:%d crc32=%08lx\n",
         (unsigned int)pcm_len, inet_ntoa(peer.sin_addr),
         UART_LINK_PCM_PORT, (unsigned long)crc);
  if (send_all(fd, header, sizeof(header)) != 0 ||
      send_pcm_chunks(fd, pcm, pcm_len) != 0 ||
      recv_all(fd, reply, sizeof(reply)) != 0)
    {
      printf("[UART_LINK] PCM: transfer failed errno=%d\n", errno);
      goto done;
    }

  if (ntohl(reply[0]) != UART_LINK_PCM_MAGIC ||
      ntohl(reply[1]) != UART_LINK_PCM_VERSION || ntohl(reply[2]) != 0 ||
      ntohl(reply[3]) != pcm_len || ntohl(reply[4]) != crc)
    {
      printf("[UART_LINK] PCM: invalid acknowledgement status=%lu "
             "bytes=%lu crc32=%08lx\n",
             (unsigned long)ntohl(reply[2]),
             (unsigned long)ntohl(reply[3]),
             (unsigned long)ntohl(reply[4]));
      goto done;
    }

  printf("[UART_LINK] PCM transfer verified: %u bytes crc32=%08lx\n",
         (unsigned int)pcm_len, (unsigned long)crc);
  ret = (int)pcm_len;

done:
  if (fd >= 0)
    {
      close(fd);
  }

  return ret;
}

static int handle_line(const char *line)
{
  struct uart_link_event event;
  char frame[UART_LINK_LINE_SIZE];
  char *saveptr, *token, *end;
  unsigned int seq, len;
  char type[32];
  char payload[UART_LINK_LINE_SIZE];
  int parsed;

  strncpy(frame, line, sizeof(frame) - 1);
  frame[sizeof(frame) - 1] = '\0';
  payload[0] = '\0';

  token = strtok_r(frame, " ", &saveptr);
  if (token == NULL || strcmp(token, "UART_LINK/1") != 0) return 0;

  token = strtok_r(NULL, " ", &saveptr);
  if (token == NULL || strlen(token) >= sizeof(type)) return 0;
  strcpy(type, token);

  token = strtok_r(NULL, " ", &saveptr);
  if (token == NULL) return 0;
  seq = (unsigned int)strtoul(token, &end, 10);
  if (*token == '\0' || *end != '\0') return 0;

  token = strtok_r(NULL, " ", &saveptr);
  if (token == NULL) return 0;
  len = (unsigned int)strtoul(token, &end, 10);
  if (*token == '\0' || *end != '\0') return 0;

  while (*saveptr == ' ') saveptr++;
  strncpy(payload, saveptr, sizeof(payload) - 1);
  payload[sizeof(payload) - 1] = '\0';

  if (len > UART_LINK_LINE_SIZE - 1) return 0;
  if (strlen(payload) != len) return 0;

  if (strcmp(type, "HELLO") == 0)
    {
      char ack[96];
      int n = snprintf(ack, sizeof(ack), "UART_LINK/1 HELLO_ACK %u 2 OK\n", seq);
      printf("[UART_LINK] RX type=HELLO seq=%u payload=%s\n", seq, payload);
      return send_client_data(ack, (size_t)n);
    }
  else if (strcmp(type, "PING") == 0)
    {
      char pong[96];
      int n = snprintf(pong, sizeof(pong), "UART_LINK/1 PONG %u 2 OK\n", seq);
      return send_client_data(pong, (size_t)n);
    }
  else if (strcmp(type, "COMMAND_ACK") == 0)
    {
      if (strcmp(payload, "OK") != 0) return 0;
      printf("[UART_LINK] command acknowledged: seq=%u\n", seq);
      return 0;
    }
  memset(&event, 0, sizeof(event));
  if (strcmp(type, "OBSTACLE") == 0)
    {
      event.type = UART_LINK_EVENT_OBSTACLE;
      parsed = sscanf(payload, "%d %15s %d", &event.distance_cm,
                      event.direction, &event.confidence);
      if (parsed != 3 || event.distance_cm <= 0 ||
          event.confidence < 0 || event.confidence > 100) return 0;
    }
  else if (strcmp(type, "STEP_DOWN") == 0)
    {
      event.type = UART_LINK_EVENT_STEP_DOWN;
      parsed = sscanf(payload, "%15s %d", event.direction, &event.confidence);
      if (parsed != 2 || event.confidence < 0 || event.confidence > 100) return 0;
    }
  else if (strcmp(type, "EMERGENCY") == 0)
    {
      event.type = UART_LINK_EVENT_EMERGENCY;
      if (sscanf(payload, "%15s", event.source) != 1) return 0;
      event.confidence = 100;
    }
  else if (strcmp(type, "ASR_RESULT") == 0)
    {
      event.type = UART_LINK_EVENT_ASR_RESULT;
      if (payload[0] == '\0') return 0;
      strlcpy(event.text, payload, sizeof(event.text));
    }
  else
    {
      return 0;
    }

  {
    char ack[96];
    int n = snprintf(ack, sizeof(ack), "UART_LINK/1 EVENT_ACK %u 2 OK\n", seq);
    if (send_client_data(ack, (size_t)n) != 0) return -1;
  }

  printf("[UART_LINK] RX type=%s seq=%u payload=%s\n", type, seq, payload);
  if (g_event_handler != NULL)
    g_event_handler(&event, g_event_handler_arg);

  return 0;
}

static void *uart_link_thread(void *arg)
{
  int discovery_fd = -1;
  mqd_t control_mq = (mqd_t)-1;
  struct mq_attr mq_attr;
  struct sockaddr_in addr;
  char buffer[UART_LINK_LINE_SIZE];
  size_t used = 0;
  time_t last_activity = 0;
  (void)arg;

  discovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (discovery_fd < 0 || g_listen_fd < 0) goto done;

  memset(&mq_attr, 0, sizeof(mq_attr));
  mq_attr.mq_maxmsg = 8;
  mq_attr.mq_msgsize = UART_LINK_FLAG_SIZE;
  mq_unlink(UART_LINK_CONTROL_QUEUE);
  control_mq = mq_open(UART_LINK_CONTROL_QUEUE,
                       O_RDONLY | O_CREAT | O_NONBLOCK, 0666, &mq_attr);
  if (control_mq == (mqd_t)-1)
    {
      printf("[UART_LINK] control queue create failed: errno=%d\n", errno);
      goto done;
    }
  chmod(UART_LINK_CONTROL_PATH, 0666);

  int yes = 1;
  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(UART_LINK_DISCOVERY_PORT);
  if (bind(discovery_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto done;

  addr.sin_port = htons(UART_LINK_DATA_PORT);
  if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(g_listen_fd, 1) < 0) goto done;

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

      struct timeval timeout = {1, 0};
      if (select(maxfd + 1, &readfds, NULL, NULL, &timeout) < 0)
        {
          if (errno == EINTR) continue;
          break;
        }

      if (g_client_fd >= 0 && time(NULL) - last_activity > UART_LINK_IDLE_TIMEOUT_S)
        {
          close_client();
          used = 0;
        }

      while (control_mq != (mqd_t)-1)
        {
          char flag[UART_LINK_FLAG_SIZE];
          ssize_t n = mq_receive(control_mq, flag, sizeof(flag), NULL);
          if (n < 0) break;
          if ((size_t)n >= sizeof(flag)) continue;
          flag[n] = '\0';
          if (send_flag_to_client(flag) != 0)
            printf("[UART_LINK] command dropped: disconnected flag=%s\n", flag);
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

      if (g_client_fd < 0 && FD_ISSET(g_listen_fd, &readfds))
        {
          struct sockaddr_in accepted_peer;
          socklen_t accepted_peer_len = sizeof(accepted_peer);
          int fd = accept(g_listen_fd, (struct sockaddr *)&accepted_peer,
                          &accepted_peer_len);
          if (fd >= 0)
            {
              int flags = fcntl(fd, F_GETFL, 0);
              if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
              close_client();
              pthread_mutex_lock(&g_client_lock);
              g_client_fd = fd;
              g_client_addr = accepted_peer;
              g_client_addr_valid = 1;
              g_connected = 1;
              pthread_mutex_unlock(&g_client_lock);
              used = 0;
              last_activity = time(NULL);
              printf("[UART_LINK] TCP peer connected\n");
            }
        }

      if (g_client_fd >= 0 && FD_ISSET(g_client_fd, &readfds))
        {
          char chunk[64];
          ssize_t n = recv(g_client_fd, chunk, sizeof(chunk), 0);
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
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
              last_activity = time(NULL);
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
  if (!g_running) return 0;
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

  if (!valid_flag(flag, &flag_len)) return -1;

  if (g_running && pthread_equal(pthread_self(), g_thread))
    return send_flag_to_client(flag);

  mq = mq_open(UART_LINK_CONTROL_QUEUE, O_WRONLY | O_NONBLOCK);
  if (mq == (mqd_t)-1)
    {
      printf("[UART_LINK] control queue open failed: errno=%d\n", errno);
      return -1;
    }
  ret = mq_send(mq, flag, flag_len, 0);
  if (ret != 0)
    printf("[UART_LINK] control queue send failed: errno=%d\n", errno);
  mq_close(mq);
  return ret == 0 ? 0 : -1;
}

void UART_LINK_SetEventHandler(uart_link_event_handler_t handler, void *arg)
{
  g_event_handler = handler;
  g_event_handler_arg = arg;
}
