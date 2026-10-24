/****************************************************************************
 * Contest 2026 team 024 - BlindBadge pull-based vision server
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "blind_badge_vision_server.h"

#include <nuttx/config.h>
#include <nuttx/video/v4l2_cap.h>
#include <nuttx/video/video.h>

#include <netutils/dhcpd.h>
#include <netutils/netlib.h>
#include <wireless/wapi.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_WIFI_SOFTAP
#  define VISION_AP_IFNAME "wlan0"
#else
#  define VISION_AP_IFNAME "wlan1"
#  define VISION_STA_IFNAME "wlan0"
#endif
#define VISION_AP_SSID "BlindBadge-Vision"
#define VISION_AP_ADDR "192.168.4.1"
#define VISION_AP_NETMASK "255.255.255.0"
#define VISION_HTTP_DEFAULT_PORT 80
#define VISION_HTTP_BACKLOG 2
#define VISION_HTTP_REQUEST_MAX 512
#define VISION_HTTP_IO_TIMEOUT_MS 5000
#define VISION_HTTP_SEND_CHUNK 1024
#define VISION_HTTP_SEND_DELAY_US 2000
#define VISION_STREAM_SECONDS 20
#define VISION_FRAME_TIMEOUT_MS 5000
#define VISION_FRAME_RETRY_US 10000
#define VISION_DEVICE "/dev/video0"
#define VISION_WIDTH 640
#define VISION_HEIGHT 480
#define VISION_JPEG_MAX (VISION_WIDTH * VISION_HEIGHT)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vision_camera_s
{
  int fd;
  uint8_t *frame;
  enum v4l2_buf_type type;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_server_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_server_running;
static volatile bool g_server_stop;
static int g_listen_fd = -1;
static int g_server_port = VISION_HTTP_DEFAULT_PORT;
static unsigned long g_frames;
static unsigned long g_clients;
static int g_last_error;
static bool g_ap_running;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int vision_send_all(int fd, const void *data, size_t length)
{
  const uint8_t *cursor = data;
  struct pollfd pollfd;
  size_t sent = 0;
  int waitret;

  while (sent < length)
    {
      size_t remaining = length - sent;
      size_t chunk = remaining > VISION_HTTP_SEND_CHUNK ?
                     VISION_HTTP_SEND_CHUNK : remaining;
      ssize_t ret = send(fd, cursor + sent, chunk, 0);
      if (ret == 0)
        {
          return -EPIPE;
        }

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              pollfd.fd = fd;
              pollfd.events = POLLOUT;
              pollfd.revents = 0;
              waitret = poll(&pollfd, 1, VISION_HTTP_IO_TIMEOUT_MS);
              if (waitret > 0)
                {
                  continue;
                }

              return waitret == 0 ? -ETIMEDOUT : -errno;
            }

          return -errno;
        }

      sent += ret;
      if (sent < length)
        {
          usleep(VISION_HTTP_SEND_DELAY_US);
        }
    }

  return OK;
}

static int vision_send_text(int fd, int status, const char *reason,
                            const char *content_type, const char *body)
{
  char header[256];
  int length;
  int ret;

  length = snprintf(header, sizeof(header),
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n\r\n",
                    status, reason, content_type, (unsigned)strlen(body));
  if (length < 0 || length >= sizeof(header))
    {
      return -EOVERFLOW;
    }

  ret = vision_send_all(fd, header, length);
  return ret < 0 ? ret : vision_send_all(fd, body, strlen(body));
}

static int vision_ap_start(void)
{
  struct in_addr address;
  struct in_addr netmask;
  int sock;
  int ret;

  if (inet_pton(AF_INET, VISION_AP_ADDR, &address) != 1 ||
      inet_pton(AF_INET, VISION_AP_NETMASK, &netmask) != 1)
    {
      return -EINVAL;
    }

#ifdef CONFIG_ESP32S3_WIFI_STATION_SOFTAP
  printf("[BlindBadge] vision_server: switching radio from STA to AP\n");
  ret = netlib_ifdown(VISION_STA_IFNAME);
  printf("[BlindBadge] vision_server: station ifdown ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  sleep(1);
#endif

  printf("[BlindBadge] vision_server: setting AP address\n");
  ret = netlib_set_ipv4addr(VISION_AP_IFNAME, &address);
  printf("[BlindBadge] vision_server: set address ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  printf("[BlindBadge] vision_server: setting AP netmask\n");
  ret = netlib_set_ipv4netmask(VISION_AP_IFNAME, &netmask);
  printf("[BlindBadge] vision_server: set netmask ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  printf("[BlindBadge] vision_server: bringing AP interface up\n");
  ret = netlib_ifup(VISION_AP_IFNAME);
  printf("[BlindBadge] vision_server: ifup ret=%d\n", ret);
  if (ret < 0)
    {
      return ret;
    }

  g_ap_running = true;

  printf("[BlindBadge] vision_server: starting DHCP server\n");
  ret = dhcpd_start(VISION_AP_IFNAME);
  printf("[BlindBadge] vision_server: dhcpd ret=%d\n", ret);
  if (ret < 0)
    {
      goto errout;
    }

  printf("[BlindBadge] vision_server: opening WAPI socket\n");
  sock = wapi_make_socket();
  printf("[BlindBadge] vision_server: WAPI socket=%d\n", sock);
  if (sock < 0)
    {
      ret = sock;
      goto errout;
    }

  printf("[BlindBadge] vision_server: setting AP SSID\n");
  ret = wapi_set_essid(sock, VISION_AP_IFNAME, VISION_AP_SSID,
                       WAPI_ESSID_ON);
  printf("[BlindBadge] vision_server: set SSID ret=%d\n", ret);
  close(sock);
  if (ret < 0)
    {
      goto errout;
    }

  printf("[BlindBadge] vision_server: AP %s at %s ready\n",
         VISION_AP_SSID, VISION_AP_ADDR);
  return OK;

errout:
  dhcpd_stop();
  netlib_ifdown(VISION_AP_IFNAME);
  g_ap_running = false;
  return ret;
}

static void vision_ap_stop(void)
{
  if (g_ap_running)
    {
      dhcpd_stop();
      netlib_ifdown(VISION_AP_IFNAME);
      g_ap_running = false;
      printf("[BlindBadge] vision_server: AP stopped\n");
    }
}

static int vision_camera_open(struct vision_camera_s *camera)
{
  struct v4l2_requestbuffers request;
  struct v4l2_format format;
  struct v4l2_buffer buffer;
  int ret;

  memset(camera, 0, sizeof(*camera));
  camera->fd = -1;
  camera->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  camera->fd = open(VISION_DEVICE, O_RDWR | O_NONBLOCK);
  if (camera->fd < 0)
    {
      printf("[BlindBadge] camera: open failed errno=%d\n", errno);
      return -errno;
    }

  printf("[BlindBadge] camera: open OK fd=%d\n", camera->fd);

  memset(&format, 0, sizeof(format));
  format.type = camera->type;
  format.fmt.pix.width = VISION_WIDTH;
  format.fmt.pix.height = VISION_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
  format.fmt.pix.sizeimage = VISION_JPEG_MAX;
  ret = ioctl(camera->fd, VIDIOC_S_FMT, (uintptr_t)&format);
  if (ret < 0)
    {
      ret = -errno;
      printf("[BlindBadge] camera: S_FMT failed (%d)\n", ret);
      goto error;
    }

  printf("[BlindBadge] camera: S_FMT JPEG %ux%u OK\n",
         VISION_WIDTH, VISION_HEIGHT);

  memset(&request, 0, sizeof(request));
  request.type = camera->type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = 1;
  request.mode = V4L2_BUF_MODE_FIFO;
  ret = ioctl(camera->fd, VIDIOC_REQBUFS, (uintptr_t)&request);
  if (ret < 0)
    {
      ret = -errno;
      printf("[BlindBadge] camera: REQBUFS failed (%d)\n", ret);
      goto error;
    }

  printf("[BlindBadge] camera: REQBUFS OK count=%u\n", request.count);

  camera->frame = memalign(64, VISION_JPEG_MAX);
  if (camera->frame == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = camera->type;
  buffer.memory = V4L2_MEMORY_USERPTR;
  buffer.index = 0;
  buffer.m.userptr = (uintptr_t)camera->frame;
  buffer.length = VISION_JPEG_MAX;
  ret = ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer);
  if (ret < 0)
    {
      ret = -errno;
      printf("[BlindBadge] camera: QBUF failed (%d)\n", ret);
      goto error;
    }

  printf("[BlindBadge] camera: QBUF OK bytes=%u\n",
         (unsigned)VISION_JPEG_MAX);

  ret = ioctl(camera->fd, VIDIOC_STREAMON, (uintptr_t)&camera->type);
  if (ret < 0)
    {
      ret = -errno;
      printf("[BlindBadge] camera: STREAMON failed (%d)\n", ret);
      goto error;
    }

  printf("[BlindBadge] camera: STREAMON OK\n");

  return OK;

error:
  free(camera->frame);
  camera->frame = NULL;
  if (camera->fd >= 0)
    {
      close(camera->fd);
      camera->fd = -1;
    }

  return ret;
}

static void vision_camera_close(struct vision_camera_s *camera)
{
  if (camera->fd >= 0)
    {
      ioctl(camera->fd, VIDIOC_STREAMOFF, (uintptr_t)&camera->type);
      close(camera->fd);
      camera->fd = -1;
    }

  free(camera->frame);
  camera->frame = NULL;
}

static int vision_camera_capture(struct vision_camera_s *camera,
                                 struct v4l2_buffer *buffer)
{
  unsigned int elapsed_us = 0;

  printf("[BlindBadge] camera: waiting frame timeout=%dms\n",
         VISION_FRAME_TIMEOUT_MS);
  memset(buffer, 0, sizeof(*buffer));
  buffer->type = camera->type;
  buffer->memory = V4L2_MEMORY_USERPTR;

  while (ioctl(camera->fd, VIDIOC_DQBUF, (uintptr_t)buffer) < 0)
    {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
          printf("[BlindBadge] camera: DQBUF failed errno=%d\n", errno);
          return -errno;
        }

      if (elapsed_us >= VISION_FRAME_TIMEOUT_MS * 1000)
        {
          printf("[BlindBadge] camera: frame timeout\n");
          return -ETIMEDOUT;
        }

      usleep(VISION_FRAME_RETRY_US);
      elapsed_us += VISION_FRAME_RETRY_US;
    }

  printf("[BlindBadge] camera: frame ready bytes=%u\n",
         (unsigned)buffer->bytesused);

  if (buffer->bytesused < 4 || buffer->bytesused > VISION_JPEG_MAX ||
      camera->frame[0] != 0xff || camera->frame[1] != 0xd8 ||
      camera->frame[buffer->bytesused - 2] != 0xff ||
      camera->frame[buffer->bytesused - 1] != 0xd9)
    {
      ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)buffer);
      return -EBADMSG;
    }

  return OK;
}

static int vision_camera_release(struct vision_camera_s *camera,
                                 struct v4l2_buffer *buffer)
{
  if (ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)buffer) < 0)
    {
      return -errno;
    }

  return OK;
}

static int vision_handle_capture(int fd)
{
  struct vision_camera_s camera;
  struct v4l2_buffer buffer;
  char header[192];
  int length;
  int ret;

  ret = vision_camera_open(&camera);
  if (ret < 0)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera initialization failed\n");
      return ret;
    }

  ret = vision_camera_capture(&camera, &buffer);
  if (ret < 0)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera capture failed\n");
      vision_camera_close(&camera);
      return ret;
    }

  length = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n\r\n",
                    (unsigned)buffer.bytesused);
  printf("[BlindBadge] capture: sending JPEG bytes=%u\n",
         (unsigned)buffer.bytesused);
  ret = vision_send_all(fd, header, length);
  if (ret == OK)
    {
      ret = vision_send_all(fd, camera.frame, buffer.bytesused);
    }

  printf("[BlindBadge] capture: send completed ret=%d\n", ret);

  if (ret == OK)
    {
      g_frames++;
    }

  vision_camera_release(&camera, &buffer);
  vision_camera_close(&camera);
  return ret;
}

static int vision_handle_stream(int fd)
{
  static const char stream_header[] =
    "HTTP/1.1 200 OK\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  struct vision_camera_s camera;
  struct v4l2_buffer buffer;
  struct timespec started;
  struct timespec now;
  char part[128];
  int length;
  int ret;

  ret = vision_camera_open(&camera);
  if (ret < 0)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera initialization failed\n");
      return ret;
    }

  ret = vision_send_all(fd, stream_header, sizeof(stream_header) - 1);
  clock_gettime(CLOCK_MONOTONIC, &started);
  while (ret == OK && !g_server_stop)
    {
      ret = vision_camera_capture(&camera, &buffer);
      if (ret < 0)
        {
          break;
        }

      length = snprintf(part, sizeof(part),
                        "--frame\r\nContent-Type: image/jpeg\r\n"
                        "Content-Length: %u\r\n\r\n",
                        (unsigned)buffer.bytesused);
      ret = vision_send_all(fd, part, length);
      if (ret == OK)
        {
          ret = vision_send_all(fd, camera.frame, buffer.bytesused);
        }

      if (ret == OK)
        {
          ret = vision_send_all(fd, "\r\n", 2);
          g_frames++;
        }

      vision_camera_release(&camera, &buffer);
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec - started.tv_sec >= VISION_STREAM_SECONDS)
        {
          break;
        }
    }

  vision_camera_close(&camera);
  return ret;
}

static int vision_handle_client(int fd)
{
  char request[VISION_HTTP_REQUEST_MAX];
  struct timeval timeout;
  int flags;
  ssize_t count;

  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  count = recv(fd, request, sizeof(request) - 1, 0);
  if (count <= 0)
    {
      return count == 0 ? -ECONNRESET : -errno;
    }

  request[count] = '\0';
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      return -errno;
    }

  if (strncmp(request, "GET /health ", 12) == 0)
    {
      return vision_send_text(fd, 200, "OK", "application/json",
                              "{\"ok\":true,\"service\":"
                              "\"blindbadge-vision\"}\n");
    }

  if (strncmp(request, "GET /capture ", 13) == 0)
    {
      return vision_handle_capture(fd);
    }

  if (strncmp(request, "GET /stream ", 12) == 0)
    {
      return vision_handle_stream(fd);
    }

  return vision_send_text(fd, 404, "Not Found", "text/plain",
                          "BlindBadge vision: use /health, /capture "
                          "or /stream\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int blind_badge_vision_server_run(int port)
{
  struct sockaddr_in address;
  struct pollfd pollfd;
  int reuse = 1;
  int client;
  int ret;

  if (port <= 0 || port > 65535)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_server_lock);
  if (g_server_running)
    {
      pthread_mutex_unlock(&g_server_lock);
      return -EBUSY;
    }

  g_server_running = true;
  g_server_stop = false;
  g_server_port = port;
  g_last_error = 0;
  pthread_mutex_unlock(&g_server_lock);

  ret = vision_ap_start();
  if (ret < 0)
    {
      goto finished;
    }

  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listen_fd < 0)
    {
      ret = -errno;
      goto finished;
    }

  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(g_listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
      listen(g_listen_fd, VISION_HTTP_BACKLOG) < 0)
    {
      ret = -errno;
      goto finished;
    }

  printf("[BlindBadge] vision_server: http://%s:%d/stream\n",
         VISION_AP_ADDR, port);
  while (!g_server_stop)
    {
      pollfd.fd = g_listen_fd;
      pollfd.events = POLLIN;
      pollfd.revents = 0;
      ret = poll(&pollfd, 1, 500);
      if (ret == 0)
        {
          continue;
        }

      if (ret < 0)
        {
          ret = errno == EINTR ? OK : -errno;
          if (ret == OK)
            {
              continue;
            }

          break;
        }

      client = accept(g_listen_fd, NULL, NULL);
      if (client < 0)
        {
          if (!g_server_stop)
            {
              ret = -errno;
            }

          continue;
        }

      g_clients++;
      ret = vision_handle_client(client);
      close(client);
      if (ret < 0 && ret != -EPIPE && ret != -ECONNRESET)
        {
          printf("[BlindBadge] vision_server: request failed (%d)\n", ret);
          g_last_error = ret;
        }
    }

  ret = OK;

finished:
  if (g_listen_fd >= 0)
    {
      close(g_listen_fd);
      g_listen_fd = -1;
    }

  vision_ap_stop();

  pthread_mutex_lock(&g_server_lock);
  g_server_running = false;
  g_last_error = ret;
  pthread_mutex_unlock(&g_server_lock);
  printf("[BlindBadge] vision_server: stopped (%d)\n", ret);
  return ret;
}

int blind_badge_vision_server_stop(void)
{
  pthread_mutex_lock(&g_server_lock);
  if (!g_server_running)
    {
      pthread_mutex_unlock(&g_server_lock);
      return -ENOENT;
    }

  g_server_stop = true;
  if (g_listen_fd >= 0)
    {
      shutdown(g_listen_fd, SHUT_RDWR);
    }

  pthread_mutex_unlock(&g_server_lock);
  return OK;
}

int blind_badge_vision_server_status(void)
{
  pthread_mutex_lock(&g_server_lock);
  printf("[BlindBadge] vision_server: %s ap=%s ip=%s port=%d "
         "clients=%lu frames=%lu error=%d\n",
         g_server_running ? "RUNNING" : "STOPPED",
         VISION_AP_SSID, VISION_AP_ADDR, g_server_port,
         g_clients, g_frames, g_last_error);
  pthread_mutex_unlock(&g_server_lock);
  return OK;
}

int blind_badge_vision_camera_test(void)
{
  struct vision_camera_s camera;
  struct v4l2_buffer buffer;
  int ret;

  printf("[BlindBadge] camera_test: opening VGA JPEG camera\n");
  ret = vision_camera_open(&camera);
  if (ret < 0)
    {
      printf("[BlindBadge] camera_test: open failed (%d)\n", ret);
      return ret;
    }

  printf("[BlindBadge] camera_test: waiting for one frame\n");
  ret = vision_camera_capture(&camera, &buffer);
  if (ret == OK)
    {
      printf("[BlindBadge] camera_test: JPEG OK bytes=%lu "
             "start=%02x%02x end=%02x%02x\n",
             (unsigned long)buffer.bytesused,
             camera.frame[0], camera.frame[1],
             camera.frame[buffer.bytesused - 2],
             camera.frame[buffer.bytesused - 1]);
      vision_camera_release(&camera, &buffer);
    }
  else
    {
      printf("[BlindBadge] camera_test: capture failed (%d)\n", ret);
    }

  vision_camera_close(&camera);
  return ret;
}
