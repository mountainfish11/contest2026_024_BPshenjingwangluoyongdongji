#include "uart_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define UART_LINK_DISCOVERY_PORT 45678
#define UART_LINK_DATA_PORT      45679
#define UART_LINK_LINE_SIZE      256
#define UART_LINK_IDLE_TIMEOUT_S 25

static pthread_t g_thread;
static volatile int g_running;
static volatile int g_connected;
static int g_listen_fd = -1;
static int g_client_fd = -1;

static int send_all(int fd, const char *data, size_t len)
{
  while (len > 0)
    {
      ssize_t n = send(fd, data, len, 0);
      if (n <= 0)
        {
          return -1;
        }
      data += n;
      len -= n;
    }
  return 0;
}

static void close_client(void)
{
  if (g_client_fd >= 0)
    {
      close(g_client_fd);
      g_client_fd = -1;
    }
  g_connected = 0;
}

static int handle_line(const char *line)
{
  unsigned int seq;
  unsigned int len;
  char type[32];
  char payload[UART_LINK_LINE_SIZE];

  payload[0] = '\0';
  if (sscanf(line, "UART_LINK/1 %31s %u %u %255[^\n]",
             type, &seq, &len, payload) < 3)
    {
      return 0;
    }

  if (len > UART_LINK_LINE_SIZE - 1)
    {
      return 0;
    }

  if (strcmp(type, "HELLO") == 0)
    {
      char ack[96];
      int n = snprintf(ack, sizeof(ack),
                       "UART_LINK/1 HELLO_ACK %u 2 OK\n", seq);
      return send_all(g_client_fd, ack, (size_t)n);
    }
  else if (strcmp(type, "PING") == 0)
    {
      char pong[96];
      int n = snprintf(pong, sizeof(pong),
                       "UART_LINK/1 PONG %u 2 OK\n", seq);
      return send_all(g_client_fd, pong, (size_t)n);
    }

  return 0;
}

static void *uart_link_thread(void *arg)
{
  int discovery_fd = -1;
  struct sockaddr_in addr;
  char buffer[UART_LINK_LINE_SIZE];
  size_t used = 0;
  time_t last_activity = 0;
  (void)arg;

  discovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (discovery_fd < 0 || g_listen_fd < 0)
    {
      goto done;
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
          time(NULL) - last_activity > UART_LINK_IDLE_TIMEOUT_S)
        {
          close_client();
          used = 0;
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
              int flags = fcntl(fd, F_GETFL, 0);
              if (flags >= 0)
                {
                  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                }

              close_client();
              g_client_fd = fd;
              g_connected = 1;
              used = 0;
              last_activity = time(NULL);
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
  if (g_listen_fd >= 0) close(g_listen_fd);
  g_listen_fd = -1;
  return NULL;
}

int UART_LINK_Start(void)
{
  if (g_running) return 0;
  g_running = 1;
  if (pthread_create(&g_thread, NULL, uart_link_thread, NULL) != 0)
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
