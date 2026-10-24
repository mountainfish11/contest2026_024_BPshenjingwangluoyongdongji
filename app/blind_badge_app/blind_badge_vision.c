/****************************************************************************
 * Contest 2026 team 024 - BlindBadge server-assisted vision
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "blind_badge_vision.h"

#include "blind_badge_display.h"
#include "blind_badge_state.h"

#include <nuttx/config.h>
#include <nuttx/video/v4l2_cap.h>
#include <nuttx/video/video.h>

#include "cJSON.h"

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
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VISION_DEVICE "/dev/video0"
#define VISION_WIDTH 320
#define VISION_HEIGHT 240
#define VISION_FRAME_BYTES (VISION_WIDTH * VISION_HEIGHT * 2)
#define VISION_PIXEL_RGB565_LE 1
#define VISION_DEFAULT_PORT 45681
#define VISION_CONNECT_TIMEOUT_MS 8000
#define VISION_RESPONSE_MAX 2048
#define VISION_HOST_MAX 48
#define VISION_WORKER_STACK_SIZE 16384
#define VISION_INTERVAL_US 500000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vision_camera
{
  int fd;
  uint8_t *frame;
  enum v4l2_buf_type type;
};

struct vision_worker_config
{
  char host[VISION_HOST_MAX];
  int port;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_vision_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_vision_thread;
static int g_vision_running;
static int g_vision_stop;
static int g_vision_thread_valid;
static unsigned int g_vision_seq;
static int g_vision_last_error;
static char g_vision_last_state[32] = "IDLE";
static struct vision_worker_config g_vision_config;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void put_u16(uint8_t *dest, uint16_t value)
{
  value = htons(value);
  memcpy(dest, &value, sizeof(value));
}

static void put_u32(uint8_t *dest, uint32_t value)
{
  value = htonl(value);
  memcpy(dest, &value, sizeof(value));
}

static int send_all(int fd, const void *data, size_t length)
{
  const uint8_t *cursor = data;
  size_t sent = 0;

  while (sent < length)
    {
      ssize_t ret = send(fd, cursor + sent, length - sent, 0);
      if (ret == 0)
        {
          return -EIO;
        }
      else if (ret < 0)
        {
          return -errno;
        }

      sent += ret;
    }

  return OK;
}

static int recv_all(int fd, void *data, size_t length)
{
  uint8_t *cursor = data;
  size_t received = 0;

  while (received < length)
    {
      ssize_t ret = recv(fd, cursor + received, length - received, 0);
      if (ret == 0)
        {
          return -ECONNRESET;
        }
      else if (ret < 0)
        {
          return -errno;
        }

      received += ret;
    }

  return OK;
}

static int vision_connect(const char *host, int port)
{
  struct sockaddr_in address;
  struct pollfd pollfd;
  struct timeval timeout;
  socklen_t error_length;
  int error;
  int flags;
  int fd;
  int ret;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
      return -EINVAL;
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  ret = connect(fd, (struct sockaddr *)&address, sizeof(address));
  if (ret < 0 && errno != EINPROGRESS)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  if (ret < 0)
    {
      pollfd.fd = fd;
      pollfd.events = POLLOUT;
      pollfd.revents = 0;
      ret = poll(&pollfd, 1, VISION_CONNECT_TIMEOUT_MS);
      if (ret == 0)
        {
          close(fd);
          return -ETIMEDOUT;
        }
      else if (ret < 0)
        {
          ret = -errno;
          close(fd);
          return ret;
        }

      error = 0;
      error_length = sizeof(error);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) < 0)
        {
          ret = -errno;
          close(fd);
          return ret;
        }

      if (error != 0)
        {
          close(fd);
          return -error;
        }
    }

  if (fcntl(fd, F_SETFL, flags) < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  timeout.tv_sec = 20;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  return fd;
}

static int vision_camera_open(struct vision_camera *camera)
{
  struct v4l2_requestbuffers request;
  struct v4l2_format format;
  struct v4l2_buffer buffer;
  int ret;

  memset(camera, 0, sizeof(*camera));
  camera->fd = -1;
  camera->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  camera->fd = open(VISION_DEVICE, O_RDWR);
  if (camera->fd < 0)
    {
      return -errno;
    }

  memset(&format, 0, sizeof(format));
  format.type = camera->type;
  format.fmt.pix.width = VISION_WIDTH;
  format.fmt.pix.height = VISION_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  ret = ioctl(camera->fd, VIDIOC_S_FMT, (uintptr_t)&format);
  if (ret < 0)
    {
      ret = -errno;
      goto error;
    }

  memset(&request, 0, sizeof(request));
  request.type = camera->type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = 1;
  request.mode = V4L2_BUF_MODE_FIFO;
  ret = ioctl(camera->fd, VIDIOC_REQBUFS, (uintptr_t)&request);
  if (ret < 0)
    {
      ret = -errno;
      goto error;
    }

  camera->frame = memalign(32, VISION_FRAME_BYTES);
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
  buffer.length = VISION_FRAME_BYTES;
  ret = ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer);
  if (ret < 0)
    {
      ret = -errno;
      goto error;
    }

  ret = ioctl(camera->fd, VIDIOC_STREAMON, (uintptr_t)&camera->type);
  if (ret < 0)
    {
      ret = -errno;
      goto error;
    }

  return OK;

error:
  if (camera->frame != NULL)
    {
      free(camera->frame);
      camera->frame = NULL;
    }

  if (camera->fd >= 0)
    {
      close(camera->fd);
      camera->fd = -1;
    }

  return ret;
}

static void vision_camera_close(struct vision_camera *camera)
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

static int vision_camera_capture(struct vision_camera *camera,
                                 struct v4l2_buffer *buffer)
{
  memset(buffer, 0, sizeof(*buffer));
  buffer->type = camera->type;
  buffer->memory = V4L2_MEMORY_USERPTR;
  if (ioctl(camera->fd, VIDIOC_DQBUF, (uintptr_t)buffer) < 0)
    {
      return -errno;
    }

  if (buffer->bytesused != VISION_FRAME_BYTES)
    {
      ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)buffer);
      return -EMSGSIZE;
    }

  return OK;
}

static int vision_camera_release(struct vision_camera *camera,
                                 struct v4l2_buffer *buffer)
{
  if (ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)buffer) < 0)
    {
      return -errno;
    }

  return OK;
}

static int vision_show_result(const char *json)
{
  const cJSON *item;
  const char *state;
  const char *speech;
  const char *alert;
  cJSON *root;

  root = cJSON_Parse(json);
  if (root == NULL)
    {
      return -EBADMSG;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "ok");
  if (!cJSON_IsTrue(item))
    {
      item = cJSON_GetObjectItemCaseSensitive(root, "error");
      printf("[BlindBadge] vision: server error: %s\n",
             cJSON_IsString(item) ? item->valuestring : "unknown");
      cJSON_Delete(root);
      return -EIO;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "state");
  state = cJSON_IsString(item) ? item->valuestring : "UNKNOWN";
  item = cJSON_GetObjectItemCaseSensitive(root, "speech");
  speech = cJSON_IsString(item) ? item->valuestring : "";
  item = cJSON_GetObjectItemCaseSensitive(root, "alert");
  alert = cJSON_IsString(item) ? item->valuestring : "";

  pthread_mutex_lock(&g_vision_lock);
  snprintf(g_vision_last_state, sizeof(g_vision_last_state), "%s", state);
  pthread_mutex_unlock(&g_vision_lock);

  printf("[BlindBadge] vision: state=%s speech=%s alert=%s\n",
         state, speech[0] ? speech : "(none)",
         alert[0] ? alert : "(none)");
  blind_badge_state_set(BLIND_BADGE_STATE_RESULT, "vision result");
  blind_badge_display_show_lines("VISION", state,
                                 speech[0] ? speech : "scene updated",
                                 alert);
  cJSON_Delete(root);
  return OK;
}

static int vision_exchange(const char *host, int port, unsigned int seq,
                           const uint8_t *frame, size_t frame_len)
{
  uint8_t header[20];
  uint32_t response_len_net = 0;
  uint32_t response_len = 0;
  char *response;
  int fd;
  int ret;

  memcpy(header, "BBV1", 4);
  put_u32(header + 4, seq);
  put_u16(header + 8, VISION_WIDTH);
  put_u16(header + 10, VISION_HEIGHT);
  put_u32(header + 12, VISION_PIXEL_RGB565_LE);
  put_u32(header + 16, frame_len);

  fd = vision_connect(host, port);
  if (fd < 0)
    {
      return fd;
    }

  ret = send_all(fd, header, sizeof(header));
  if (ret == OK)
    {
      ret = send_all(fd, frame, frame_len);
    }

  if (ret == OK)
    {
      ret = recv_all(fd, &response_len_net, sizeof(response_len_net));
    }

  response_len = ntohl(response_len_net);
  if (ret == OK &&
      (response_len == 0 || response_len >= VISION_RESPONSE_MAX))
    {
      ret = -EOVERFLOW;
    }

  response = ret == OK ? malloc(response_len + 1) : NULL;
  if (ret == OK && response == NULL)
    {
      ret = -ENOMEM;
    }

  if (ret == OK)
    {
      ret = recv_all(fd, response, response_len);
    }

  close(fd);
  if (ret == OK)
    {
      response[response_len] = '\0';
      ret = vision_show_result(response);
    }

  free(response);
  return ret;
}

static int vision_process_frame(struct vision_camera *camera,
                                const char *host, int port)
{
  struct v4l2_buffer buffer;
  unsigned int seq;
  int ret;
  int release_ret;

  blind_badge_state_set(BLIND_BADGE_STATE_CAPTURING, "camera active");
  ret = vision_camera_capture(camera, &buffer);
  if (ret < 0)
    {
      return ret;
    }

  pthread_mutex_lock(&g_vision_lock);
  seq = ++g_vision_seq;
  pthread_mutex_unlock(&g_vision_lock);

  blind_badge_state_set(BLIND_BADGE_STATE_PROCESSING, "vision inference");
  printf("[BlindBadge] vision: sending frame #%u, %u bytes to %s:%d\n",
         seq, (unsigned)buffer.bytesused, host, port);
  ret = vision_exchange(host, port, seq,
                        (const uint8_t *)buffer.m.userptr,
                        buffer.bytesused);
  release_ret = vision_camera_release(camera, &buffer);
  return ret < 0 ? ret : release_ret;
}

static void *vision_worker(void *arg)
{
  struct vision_worker_config config = *(struct vision_worker_config *)arg;
  struct vision_camera camera;
  int ret;

  ret = vision_camera_open(&camera);
  if (ret < 0)
    {
      printf("[BlindBadge] vision: camera open failed (%d).\n", ret);
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "camera unavailable");
      goto finished;
    }

  printf("[BlindBadge] vision: started, server=%s:%d.\n",
         config.host, config.port);
  while (1)
    {
      pthread_mutex_lock(&g_vision_lock);
      if (g_vision_stop)
        {
          pthread_mutex_unlock(&g_vision_lock);
          break;
        }

      pthread_mutex_unlock(&g_vision_lock);

      ret = vision_process_frame(&camera, config.host, config.port);
      pthread_mutex_lock(&g_vision_lock);
      g_vision_last_error = ret;
      pthread_mutex_unlock(&g_vision_lock);
      if (ret < 0)
        {
          printf("[BlindBadge] vision: frame failed (%d).\n", ret);
          blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "vision failed");
          blind_badge_display_show_lines("VISION", "ERROR",
                                         "server/camera", "retrying");
          sleep(2);
        }
      else
        {
          usleep(VISION_INTERVAL_US);
        }
    }

  vision_camera_close(&camera);

finished:
  pthread_mutex_lock(&g_vision_lock);
  g_vision_running = 0;
  pthread_mutex_unlock(&g_vision_lock);
  printf("[BlindBadge] vision: stopped.\n");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int blind_badge_vision_start(const char *host, int port)
{
  pthread_attr_t attr;
  int ret;

  if (host == NULL || host[0] == '\0' || strlen(host) >= VISION_HOST_MAX ||
      port <= 0 || port > 65535)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_vision_lock);
  if (g_vision_running)
    {
      pthread_mutex_unlock(&g_vision_lock);
      return -EBUSY;
    }

  snprintf(g_vision_config.host, sizeof(g_vision_config.host), "%s", host);
  g_vision_config.port = port;
  g_vision_stop = 0;
  g_vision_running = 1;
  g_vision_last_error = 0;
  pthread_mutex_unlock(&g_vision_lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, VISION_WORKER_STACK_SIZE);
  ret = pthread_create(&g_vision_thread, &attr, vision_worker,
                       &g_vision_config);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_vision_lock);
      g_vision_running = 0;
      pthread_mutex_unlock(&g_vision_lock);
      return -ret;
    }

  g_vision_thread_valid = 1;
  return OK;
}

int blind_badge_vision_once(const char *host, int port)
{
  struct vision_camera camera;
  int ret;

  pthread_mutex_lock(&g_vision_lock);
  if (g_vision_running)
    {
      pthread_mutex_unlock(&g_vision_lock);
      return -EBUSY;
    }

  pthread_mutex_unlock(&g_vision_lock);

  ret = vision_camera_open(&camera);
  if (ret < 0)
    {
      return ret;
    }

  ret = vision_process_frame(&camera, host, port);
  vision_camera_close(&camera);
  pthread_mutex_lock(&g_vision_lock);
  g_vision_last_error = ret;
  if (ret < 0)
    {
      snprintf(g_vision_last_state, sizeof(g_vision_last_state), "ERROR");
    }

  pthread_mutex_unlock(&g_vision_lock);
  if (ret < 0)
    {
      blind_badge_state_set(BLIND_BADGE_STATE_ERROR, "vision failed");
      blind_badge_display_show_lines("VISION", "ERROR",
                                     "server/camera", "check network");
    }

  return ret;
}

int blind_badge_vision_stop(void)
{
  pthread_t thread;
  int join_thread;

  pthread_mutex_lock(&g_vision_lock);
  if (!g_vision_running)
    {
      pthread_mutex_unlock(&g_vision_lock);
      return -ENOENT;
    }

  g_vision_stop = 1;
  thread = g_vision_thread;
  join_thread = g_vision_thread_valid;
  pthread_mutex_unlock(&g_vision_lock);

  if (join_thread)
    {
      pthread_join(thread, NULL);
      g_vision_thread_valid = 0;
    }

  return OK;
}

int blind_badge_vision_status(void)
{
  pthread_mutex_lock(&g_vision_lock);
  printf("[BlindBadge] vision: %s server=%s:%d seq=%u state=%s error=%d\n",
         g_vision_running ? "running" : "stopped",
         g_vision_config.host[0] ? g_vision_config.host : "(not set)",
         g_vision_config.port ? g_vision_config.port : VISION_DEFAULT_PORT,
         g_vision_seq, g_vision_last_state, g_vision_last_error);
  pthread_mutex_unlock(&g_vision_lock);
  return OK;
}
