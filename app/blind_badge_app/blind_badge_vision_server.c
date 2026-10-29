/****************************************************************************
 * Contest 2026 team 024 - BlindBadge pull-based vision server
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "blind_badge_vision_server.h"

#include "infra/network_manager.h"

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
#include <netinet/tcp.h>
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
#  define VISION_STA_IFNAME "wlan0"
#endif
#define VISION_AP_SSID "BlindBadge-Vision"
#define VISION_AP_ADDR "192.168.4.1"
#define VISION_AP_NETMASK "255.255.255.0"
#define VISION_HTTP_DEFAULT_PORT 80
#define VISION_HTTP_BACKLOG 2
#define VISION_STA_RETRY_DELAY_SEC 2
#define VISION_HTTP_REQUEST_MAX 512
#define VISION_HTTP_IO_TIMEOUT_MS 1500
#define VISION_HTTP_SEND_CHUNK 1400
#define VISION_HTTP_SEND_DELAY_US 1000
#define VISION_STREAM_STACK_SIZE 8192
#define VISION_STREAM_PRIORITY 254
#define VISION_CLIENT_DETACHED 1
#define VISION_FRAME_TIMEOUT_MS 1500
#define VISION_FRAME_RETRY_US 10000
#define VISION_NET_BENCH_BLOCK_SIZE 12288
#define VISION_NET_BENCH_BLOCKS 10
#define VISION_DEVICE "/dev/video0"
#define VISION_WIDTH 640
#define VISION_HEIGHT 480
#define VISION_JPEG_MAX (128 * 1024)
#define VISION_CAMERA_BUFFERS 3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vision_camera_s
{
  int fd;
  uint8_t *frames[VISION_CAMERA_BUFFERS];
  unsigned int buffer_count;
  enum v4l2_buf_type type;
  bool streaming;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_server_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_stream_cond = PTHREAD_COND_INITIALIZER;
static volatile bool g_server_running;
static volatile bool g_server_stop;
static int g_listen_fd = -1;
static int g_stream_fd = -1;
static struct vision_camera_s g_server_camera;
static bool g_server_camera_ready;
static bool g_stream_active;
static int g_server_port = VISION_HTTP_DEFAULT_PORT;
static unsigned long g_frames;
static unsigned long g_clients;
static int g_last_error;
#ifdef CONFIG_ESP32S3_WIFI_SOFTAP
static bool g_ap_running;
#endif
static char g_network_addr[INET_ADDRSTRLEN] = "0.0.0.0";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t vision_monotonic_us(void);

static void vision_count_frame(void)
{
  pthread_mutex_lock(&g_server_lock);
  g_frames++;
  pthread_mutex_unlock(&g_server_lock);
}

static void vision_log_heap(const char *stage)
{
  struct mallinfo info = mallinfo();

  printf("[BlindBadge] heap: %s free=%u largest=%u used=%u\n",
         stage, info.fordblks, info.mxordblk, info.uordblks);
}

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

static int vision_network_start(void)
{
#ifdef CONFIG_ESP32S3_WIFI_SOFTAP
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
  strlcpy(g_network_addr, VISION_AP_ADDR, sizeof(g_network_addr));
  return OK;

errout:
  dhcpd_stop();
  netlib_ifdown(VISION_AP_IFNAME);
  g_ap_running = false;
  return ret;
#else
  struct in_addr address;
  unsigned int attempt = 0;
  int ret;

  while (!g_server_stop)
    {
      attempt++;
      if (network_is_connected())
        {
          ret = netlib_get_ipv4addr(VISION_STA_IFNAME, &address);
          if (ret == OK && address.s_addr != INADDR_ANY &&
              inet_ntop(AF_INET, &address, g_network_addr,
                        sizeof(g_network_addr)) != NULL)
            {
              printf("[BlindBadge] vision_server: STA ready at %s\n",
                     g_network_addr);
              return OK;
            }
        }

      if (attempt == 1 || attempt % 10 == 0)
        {
          printf("[BlindBadge] vision_server: waiting for STA IPv4 "
                 "attempt=%u\n", attempt);
        }

      sleep(VISION_STA_RETRY_DELAY_SEC);
    }

  return -ECANCELED;
#endif
}

static void vision_network_stop(void)
{
#ifdef CONFIG_ESP32S3_WIFI_SOFTAP
  if (g_ap_running)
    {
      dhcpd_stop();
      netlib_ifdown(VISION_AP_IFNAME);
      g_ap_running = false;
      printf("[BlindBadge] vision_server: AP stopped\n");
    }
#endif
}

static int vision_camera_open(struct vision_camera_s *camera)
{
  struct v4l2_requestbuffers request;
  struct v4l2_format format;
  struct v4l2_buffer buffer;
  unsigned int i;
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
  request.count = VISION_CAMERA_BUFFERS;
  request.mode = V4L2_BUF_MODE_RING;
  ret = ioctl(camera->fd, VIDIOC_REQBUFS, (uintptr_t)&request);
  if (ret < 0)
    {
      ret = -errno;
      printf("[BlindBadge] camera: REQBUFS failed (%d)\n", ret);
      goto error;
    }

  if (request.count != VISION_CAMERA_BUFFERS)
    {
      ret = -ENOMEM;
      printf("[BlindBadge] camera: REQBUFS returned %u, need %u\n",
             request.count, VISION_CAMERA_BUFFERS);
      goto error;
    }

  camera->buffer_count = request.count;
  printf("[BlindBadge] camera: REQBUFS OK count=%u\n",
         camera->buffer_count);

  for (i = 0; i < camera->buffer_count; i++)
    {
      camera->frames[i] = memalign(64, VISION_JPEG_MAX);
      if (camera->frames[i] == NULL)
        {
          ret = -ENOMEM;
          goto error;
        }

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = camera->type;
      buffer.memory = V4L2_MEMORY_USERPTR;
      buffer.index = i;
      buffer.m.userptr = (uintptr_t)camera->frames[i];
      buffer.length = VISION_JPEG_MAX;
      ret = ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer);
      if (ret < 0)
        {
          ret = -errno;
          printf("[BlindBadge] camera: QBUF %u failed (%d)\n", i, ret);
          goto error;
        }
    }

  printf("[BlindBadge] camera: QBUF OK count=%u bytes_each=%u\n",
         camera->buffer_count, (unsigned)VISION_JPEG_MAX);

  return OK;

error:
  for (i = 0; i < VISION_CAMERA_BUFFERS; i++)
    {
      free(camera->frames[i]);
      camera->frames[i] = NULL;
    }

  camera->buffer_count = 0;
  if (camera->fd >= 0)
    {
      close(camera->fd);
      camera->fd = -1;
    }

  return ret;
}

static void vision_camera_close(struct vision_camera_s *camera)
{
  uint64_t close_started;
  uint64_t streamoff_done;
  unsigned int i;
  int streamoff_ret = OK;

  close_started = vision_monotonic_us();
  if (camera->fd >= 0 && camera->streaming)
    {
      streamoff_ret = ioctl(camera->fd, VIDIOC_STREAMOFF,
                            (uintptr_t)&camera->type);
      streamoff_done = vision_monotonic_us();
      printf("[BlindBadge] camera: close streamoff_ret=%d "
             "streamoff_ms=%lu\n",
             streamoff_ret,
             (unsigned long)((streamoff_done - close_started) / 1000));
      camera->streaming = false;
    }

  if (camera->fd >= 0)
    {
      close(camera->fd);
      camera->fd = -1;
    }

  for (i = 0; i < VISION_CAMERA_BUFFERS; i++)
    {
      free(camera->frames[i]);
      camera->frames[i] = NULL;
    }

  camera->buffer_count = 0;
}

static int vision_camera_start(struct vision_camera_s *camera)
{
  struct v4l2_buffer buffer;
  int ret;

  if (camera->streaming)
    {
      return OK;
    }

  /* Drain completed frames left by an earlier start attempt.  Requeueing
   * them before STREAMON gives V4L2 a clean set of writable containers.
   */

  for (; ; )
    {
      memset(&buffer, 0, sizeof(buffer));
      buffer.type = camera->type;
      buffer.memory = V4L2_MEMORY_USERPTR;
      ret = ioctl(camera->fd, VIDIOC_DQBUF, (uintptr_t)&buffer);
      if (ret < 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              break;
            }

          return -errno;
        }

      ret = ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer);
      if (ret < 0)
        {
          return -errno;
        }
    }

  ret = ioctl(camera->fd, VIDIOC_STREAMON, (uintptr_t)&camera->type);
  if (ret < 0)
    {
      printf("[BlindBadge] camera: STREAMON failed errno=%d\n", errno);
      return -errno;
    }

  camera->streaming = true;
  printf("[BlindBadge] camera: STREAMON OK\n");
  return OK;
}

static uint8_t *vision_camera_frame(struct vision_camera_s *camera,
                                    struct v4l2_buffer *buffer)
{
  if (buffer->index >= camera->buffer_count ||
      buffer->m.userptr != (uintptr_t)camera->frames[buffer->index])
    {
      return NULL;
    }

  return camera->frames[buffer->index];
}

static uint64_t vision_monotonic_us(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static int vision_camera_capture(struct vision_camera_s *camera,
                                 struct v4l2_buffer *buffer,
                                 bool log_frame)
{
  uint8_t *frame;
  uint64_t deadline_us;
  unsigned int discarded = 0;
  int ret;

  if (log_frame)
    {
      printf("[BlindBadge] camera: waiting frame timeout=%dms\n",
             VISION_FRAME_TIMEOUT_MS);
    }

  deadline_us = vision_monotonic_us() + VISION_FRAME_TIMEOUT_MS * 1000;
  while (vision_monotonic_us() < deadline_us)
    {
      memset(buffer, 0, sizeof(*buffer));
      buffer->type = camera->type;
      buffer->memory = V4L2_MEMORY_USERPTR;

      ret = ioctl(camera->fd, VIDIOC_DQBUF, (uintptr_t)buffer);
      if (ret < 0)
        {
          if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
              return -errno;
            }

          usleep(VISION_FRAME_RETRY_US);
          continue;
        }

      frame = vision_camera_frame(camera, buffer);
      if (frame != NULL && buffer->bytesused >= 4 &&
          buffer->bytesused <= VISION_JPEG_MAX &&
          frame[0] == 0xff && frame[1] == 0xd8 &&
          frame[buffer->bytesused - 2] == 0xff &&
          frame[buffer->bytesused - 1] == 0xd9)
        {
          if (log_frame)
            {
              printf("[BlindBadge] camera: frame ready bytes=%u "
                     "discarded=%u\n",
                     (unsigned)buffer->bytesused, discarded);
            }

          return OK;
        }

      discarded++;
      ret = ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)buffer);
      if (ret < 0)
        {
          return -errno;
        }
    }

  return -ETIMEDOUT;
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
  struct v4l2_buffer buffer;
  char header[192];
  uint8_t *frame;
  uint8_t *jpeg;
  uint64_t started;
  size_t jpeg_size;
  int length;
  int ret;
  bool stream_active;

  pthread_mutex_lock(&g_server_lock);
  stream_active = g_stream_active;
  pthread_mutex_unlock(&g_server_lock);

  if (!g_server_camera_ready)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera initialization failed\n");
      return -ENODEV;
    }

  if (stream_active)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera busy: stream active\n");
      return -EBUSY;
    }

  started = vision_monotonic_us();
  ret = vision_camera_capture(&g_server_camera, &buffer, true);
  printf("[BlindBadge] capture: camera_capture_ms=%lu ret=%d\n",
         (unsigned long)((vision_monotonic_us() - started) / 1000), ret);
  if (ret < 0)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera capture failed\n");
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
  frame = vision_camera_frame(&g_server_camera, &buffer);
  jpeg_size = buffer.bytesused;
  jpeg = frame == NULL ? NULL : malloc(jpeg_size);
  if (jpeg == NULL)
    {
      vision_camera_release(&g_server_camera, &buffer);
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge JPEG allocation failed\n");
      return -ENOMEM;
    }

  memcpy(jpeg, frame, jpeg_size);
  vision_log_heap("capture-before-close");
  ret = vision_camera_release(&g_server_camera, &buffer);
  if (ret < 0)
    {
      free(jpeg);
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera buffer release failed\n");
      return ret;
    }

  started = vision_monotonic_us();
  ret = vision_send_all(fd, header, length);
  printf("[BlindBadge] capture: header_send_ms=%lu ret=%d\n",
         (unsigned long)((vision_monotonic_us() - started) / 1000), ret);
  if (ret == OK)
    {
      started = vision_monotonic_us();
      ret = vision_send_all(fd, jpeg, jpeg_size);
      printf("[BlindBadge] capture: jpeg_send_ms=%lu ret=%d\n",
             (unsigned long)((vision_monotonic_us() - started) / 1000),
             ret);
    }

  printf("[BlindBadge] capture: send completed ret=%d\n", ret);
  free(jpeg);

  if (ret == OK)
    {
      vision_count_frame();
    }

  return ret;
}

static int vision_handle_stream(int fd)
{
  static const char stream_header[] =
    "HTTP/1.1 200 OK\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  struct v4l2_buffer buffer;
  char part[128];
  uint8_t *frame;
  uint8_t *jpeg;
  size_t jpeg_size;
  int length;
  int ret;

  if (!g_server_camera_ready)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge camera initialization failed\n");
      return -ENODEV;
    }

  jpeg = malloc(VISION_JPEG_MAX);
  if (jpeg == NULL)
    {
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge stream allocation failed\n");
      return -ENOMEM;
    }

  ret = vision_send_all(fd, stream_header, sizeof(stream_header) - 1);

  while (ret == OK && !g_server_stop)
    {
      ret = vision_camera_capture(&g_server_camera, &buffer, false);
      if (ret < 0)
        {
          break;
        }

      frame = vision_camera_frame(&g_server_camera, &buffer);
      jpeg_size = buffer.bytesused;
      if (frame == NULL)
        {
          vision_camera_release(&g_server_camera, &buffer);
          ret = -EIO;
          break;
        }

      memcpy(jpeg, frame, jpeg_size);
      ret = vision_camera_release(&g_server_camera, &buffer);
      if (ret < 0)
        {
          break;
        }

      length = snprintf(part, sizeof(part),
                        "--frame\r\nContent-Type: image/jpeg\r\n"
                        "Content-Length: %u\r\n\r\n",
                        (unsigned)jpeg_size);
      ret = vision_send_all(fd, part, length);
      if (ret == OK)
        {
          ret = vision_send_all(fd, jpeg, jpeg_size);
        }

      if (ret == OK)
        {
          ret = vision_send_all(fd, "\r\n", 2);
          if (ret == OK)
            {
              vision_count_frame();
            }
        }
    }

  free(jpeg);
  return ret;
}

static void *vision_stream_worker(void *arg)
{
  int fd = (int)(intptr_t)arg;
  int ret;

  pthread_setschedprio(pthread_self(), VISION_STREAM_PRIORITY);

  ret = vision_handle_stream(fd);
  shutdown(fd, SHUT_RDWR);
  close(fd);

  pthread_mutex_lock(&g_server_lock);
  g_stream_fd = -1;
  g_stream_active = false;
  if (ret < 0 && ret != -EPIPE && ret != -ECONNRESET &&
      ret != -ENOTCONN)
    {
      g_last_error = ret;
    }

  pthread_cond_broadcast(&g_stream_cond);
  pthread_mutex_unlock(&g_server_lock);
  return NULL;
}

static int vision_start_stream_worker(int fd)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  pthread_mutex_lock(&g_server_lock);
  if (g_stream_active)
    {
      pthread_mutex_unlock(&g_server_lock);
      vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                       "BlindBadge stream already active\n");
      return -EBUSY;
    }

  g_stream_active = true;
  g_stream_fd = fd;
  pthread_mutex_unlock(&g_server_lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, VISION_STREAM_STACK_SIZE);
  ret = pthread_create(&thread, &attr, vision_stream_worker,
                       (void *)(intptr_t)fd);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_server_lock);
      g_stream_fd = -1;
      g_stream_active = false;
      pthread_cond_broadcast(&g_stream_cond);
      pthread_mutex_unlock(&g_server_lock);
      return -ret;
    }

  pthread_detach(thread);
  return VISION_CLIENT_DETACHED;
}

static int vision_handle_net_bench(int fd)
{
  const size_t total = VISION_NET_BENCH_BLOCK_SIZE *
                       VISION_NET_BENCH_BLOCKS;
  uint8_t *block;
  char header[192];
  uint64_t started;
  uint64_t elapsed;
  size_t sent = 0;
  int length;
  int ret;
  int i;

  block = malloc(VISION_NET_BENCH_BLOCK_SIZE);
  if (block == NULL)
    {
      return vision_send_text(fd, 503, "Service Unavailable", "text/plain",
                              "BlindBadge net bench allocation failed\n");
    }

  for (i = 0; i < VISION_NET_BENCH_BLOCK_SIZE; i++)
    {
      block[i] = (uint8_t)i;
    }

  length = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n\r\n",
                    (unsigned int)total);
  if (length < 0 || length >= sizeof(header))
    {
      free(block);
      return -EOVERFLOW;
    }

  ret = vision_send_all(fd, header, length);
  started = vision_monotonic_us();
  for (i = 0; ret == OK && i < VISION_NET_BENCH_BLOCKS; i++)
    {
      ret = vision_send_all(fd, block, VISION_NET_BENCH_BLOCK_SIZE);
      if (ret == OK)
        {
          sent += VISION_NET_BENCH_BLOCK_SIZE;
        }
    }

  elapsed = vision_monotonic_us() - started;
  printf("[BlindBadge] net_bench: bytes=%lu elapsed_ms=%lu "
         "rate_kib_s=%lu ret=%d\n",
         (unsigned long)sent, (unsigned long)(elapsed / 1000),
         elapsed == 0 ? 0 :
         (unsigned long)((uint64_t)sent * 1000000 / elapsed / 1024), ret);
  free(block);
  return ret;
}

static int vision_handle_client(int fd)
{
  char request[VISION_HTTP_REQUEST_MAX];
  char health[256];
  struct timeval timeout;
  unsigned long clients;
  unsigned long frames;
  bool camera_ready;
  bool stream_active;
  int last_error;
  int enabled = 1;
  int flags;
  int length;
  ssize_t count;

  timeout.tv_sec = VISION_HTTP_IO_TIMEOUT_MS / 1000;
  timeout.tv_usec = (VISION_HTTP_IO_TIMEOUT_MS % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                 sizeof(enabled)) < 0)
    {
      printf("[BlindBadge] vision_server: TCP_NODELAY failed errno=%d\n",
             errno);
    }

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
      pthread_mutex_lock(&g_server_lock);
      camera_ready = g_server_camera_ready;
      stream_active = g_stream_active;
      clients = g_clients;
      frames = g_frames;
      last_error = g_last_error;
      pthread_mutex_unlock(&g_server_lock);

      length = snprintf(health, sizeof(health),
                        "{\"ok\":true,\"service\":"
                        "\"blindbadge-vision\",\"camera_ready\":%s,"
                        "\"stream_active\":%s,\"clients\":%lu,"
                        "\"frames\":%lu,\"last_error\":%d}\n",
                        camera_ready ? "true" : "false",
                        stream_active ? "true" : "false",
                        clients, frames, last_error);
      if (length < 0 || length >= sizeof(health))
        {
          return -EOVERFLOW;
        }

      return vision_send_text(fd, 200, "OK", "application/json", health);
    }

  if (strncmp(request, "GET /capture ", 13) == 0)
    {
      return vision_handle_capture(fd);
    }

  if (strncmp(request, "GET /stream ", 12) == 0)
    {
      return vision_start_stream_worker(fd);
    }

  if (strncmp(request, "GET /net_bench ", 15) == 0)
    {
      return vision_handle_net_bench(fd);
    }

  return vision_send_text(fd, 404, "Not Found", "text/plain",
                          "BlindBadge vision: use /health, /capture "
                          "or /stream or /net_bench\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int blind_badge_vision_server_run(int port)
{
  struct sockaddr_in address;
  struct pollfd pollfd;
  struct v4l2_buffer warmup_buffer;
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
  g_stream_fd = -1;
  g_stream_active = false;
  pthread_mutex_unlock(&g_server_lock);

  /* Initialize the camera once before exposing the network server.  Reusing
   * one V4L2
   * session avoids repeating sensor, DMA, and STREAMON/STREAMOFF setup in
   * the request path, matching the persistent-camera strategy used by the
   * reference delivery.
   */

  ret = vision_camera_open(&g_server_camera);
  if (ret < 0)
    {
      printf("[BlindBadge] vision_server: persistent camera open failed "
             "(%d)\n", ret);
      goto finished;
    }

  g_server_camera_ready = true;
  printf("[BlindBadge] vision_server: camera mode=persistent fd=%d\n",
         g_server_camera.fd);

#ifndef CONFIG_ESP32S3_WIFI_SOFTAP
  /* Initialize the sensor before enabling Wi-Fi, but do not start the frame
   * ring while waiting for the operator to connect STA from NSH.
   */

  ret = vision_network_start();
  if (ret < 0)
    {
      goto finished;
    }
#endif

  /* Prime the sensor and keep the V4L2 ring running for the server lifetime.
   * Repeated STREAMON/STREAMOFF transitions proved less reliable than
   * continuously replacing old frames in ring mode.
   */

  ret = vision_camera_start(&g_server_camera);
  if (ret == OK)
    {
      ret = vision_camera_capture(&g_server_camera, &warmup_buffer, false);
    }

  if (ret == OK)
    {
      ret = vision_camera_release(&g_server_camera, &warmup_buffer);
    }

  if (ret < 0)
    {
      printf("[BlindBadge] vision_server: camera warmup failed (%d)\n",
             ret);
      goto finished;
    }

  printf("[BlindBadge] vision_server: camera warmup complete\n");

#ifdef CONFIG_ESP32S3_WIFI_SOFTAP
  ret = vision_network_start();
  if (ret < 0)
    {
      goto finished;
    }
#endif

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
         g_network_addr, port);
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

      pthread_mutex_lock(&g_server_lock);
      g_clients++;
      pthread_mutex_unlock(&g_server_lock);
      ret = vision_handle_client(client);
      if (ret != VISION_CLIENT_DETACHED)
        {
          close(client);
        }

      if (ret < 0 && ret != -EPIPE && ret != -ECONNRESET &&
          ret != -ENOTCONN && ret != -EBUSY)
        {
          printf("[BlindBadge] vision_server: request failed (%d)\n", ret);
          pthread_mutex_lock(&g_server_lock);
          g_last_error = ret;
          pthread_mutex_unlock(&g_server_lock);
        }
    }

  ret = OK;

finished:
  pthread_mutex_lock(&g_server_lock);
  g_server_stop = true;
  if (g_stream_fd >= 0)
    {
      shutdown(g_stream_fd, SHUT_RDWR);
    }

  while (g_stream_active)
    {
      pthread_cond_wait(&g_stream_cond, &g_server_lock);
    }

  pthread_mutex_unlock(&g_server_lock);

  if (g_listen_fd >= 0)
    {
      close(g_listen_fd);
      g_listen_fd = -1;
    }

  if (g_server_camera_ready)
    {
      vision_camera_close(&g_server_camera);
      g_server_camera_ready = false;
    }

  vision_network_stop();

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

  if (g_stream_fd >= 0)
    {
      shutdown(g_stream_fd, SHUT_RDWR);
    }

  pthread_mutex_unlock(&g_server_lock);
  return OK;
}

int blind_badge_vision_server_status(void)
{
  pthread_mutex_lock(&g_server_lock);
  printf("[BlindBadge] vision_server: %s network=%s ip=%s port=%d "
         "stream=%s clients=%lu frames=%lu error=%d\n",
         g_server_running ? "RUNNING" : "STOPPED",
#ifdef CONFIG_ESP32S3_WIFI_SOFTAP
         "softap",
#else
         "station",
#endif
         g_network_addr, g_server_port,
         g_stream_active ? "ACTIVE" : "IDLE",
         g_clients, g_frames, g_last_error);
  pthread_mutex_unlock(&g_server_lock);
  return OK;
}

int blind_badge_vision_camera_test(void)
{
  struct vision_camera_s camera;
  struct v4l2_buffer buffer;
  uint8_t *frame;
  int ret;

  printf("[BlindBadge] camera_test: opening VGA JPEG camera\n");
  ret = vision_camera_open(&camera);
  if (ret < 0)
    {
      printf("[BlindBadge] camera_test: open failed (%d)\n", ret);
      return ret;
    }

  printf("[BlindBadge] camera_test: waiting for one frame\n");
  ret = vision_camera_capture(&camera, &buffer, true);
  if (ret == OK)
    {
      frame = vision_camera_frame(&camera, &buffer);
      printf("[BlindBadge] camera_test: JPEG OK bytes=%lu "
             "start=%02x%02x end=%02x%02x\n",
             (unsigned long)buffer.bytesused,
             frame[0], frame[1], frame[buffer.bytesused - 2],
             frame[buffer.bytesused - 1]);
      vision_camera_release(&camera, &buffer);
    }
  else
    {
      printf("[BlindBadge] camera_test: capture failed (%d)\n", ret);
    }

  vision_camera_close(&camera);
  return ret;
}

int blind_badge_vision_camera_bench(unsigned int frames)
{
  struct vision_camera_s camera;
  struct v4l2_buffer buffer;
  uint64_t total_bytes = 0;
  uint64_t started;
  uint64_t elapsed;
  uint64_t capture_elapsed;
  uint64_t capture_total = 0;
  uint64_t capture_min = UINT64_MAX;
  uint64_t capture_max = 0;
  uint64_t qbuf_started;
  uint64_t qbuf_total = 0;
  unsigned int completed = 0;
  int ret;

  if (frames == 0 || frames > 300)
    {
      return -EINVAL;
    }

  printf("[BlindBadge] camera_bench: opening VGA JPEG camera frames=%u\n",
         frames);
  ret = vision_camera_open(&camera);
  if (ret < 0)
    {
      printf("[BlindBadge] camera_bench: open failed (%d)\n", ret);
      return ret;
    }

  ret = vision_camera_capture(&camera, &buffer, false);
  if (ret == OK)
    {
      ret = vision_camera_release(&camera, &buffer);
    }

  if (ret < 0)
    {
      printf("[BlindBadge] camera_bench: warmup failed (%d)\n", ret);
      vision_camera_close(&camera);
      return ret;
    }

  started = vision_monotonic_us();
  while (completed < frames)
    {
      capture_elapsed = vision_monotonic_us();
      ret = vision_camera_capture(&camera, &buffer, false);
      capture_elapsed = vision_monotonic_us() - capture_elapsed;
      if (ret < 0)
        {
          break;
        }

      capture_total += capture_elapsed;
      if (capture_elapsed < capture_min)
        {
          capture_min = capture_elapsed;
        }

      if (capture_elapsed > capture_max)
        {
          capture_max = capture_elapsed;
        }

      total_bytes += buffer.bytesused;
      completed++;
      qbuf_started = vision_monotonic_us();
      ret = vision_camera_release(&camera, &buffer);
      qbuf_total += vision_monotonic_us() - qbuf_started;
      if (ret < 0)
        {
          break;
        }
    }

  elapsed = vision_monotonic_us() - started;
  printf("[BlindBadge] camera_bench: frames=%u elapsed_ms=%lu "
         "fps_x100=%lu jpeg_avg=%lu ret=%d\n",
         completed, (unsigned long)(elapsed / 1000),
         elapsed == 0 ? 0 :
         (unsigned long)((uint64_t)completed * 100000000 / elapsed),
         completed == 0 ? 0 : (unsigned long)(total_bytes / completed), ret);
  printf("[BlindBadge] camera_bench: capture_avg_us=%lu "
         "capture_min_us=%lu capture_max_us=%lu qbuf_avg_us=%lu\n",
         completed == 0 ? 0 :
         (unsigned long)(capture_total / completed),
         completed == 0 ? 0 : (unsigned long)capture_min,
         completed == 0 ? 0 : (unsigned long)capture_max,
         completed == 0 ? 0 : (unsigned long)(qbuf_total / completed));
  vision_camera_close(&camera);
  return completed == frames && ret == OK ? OK : ret;
}
