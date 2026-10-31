/****************************************************************************
 * Contest 2026 team 024 - BlindBadge menu services
 ****************************************************************************/

#include "blind_badge_service.h"
#include "blind_badge_vision_server.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BLIND_BADGE_VISION_PORT 80
#define BLIND_BADGE_VISION_STACK_SIZE 16384
#define BLIND_BADGE_VISION_DISPLAY_SETTLE_US 500000

static const char *const g_service_labels[] =
{
  "VISION",
  "VOICE",
  "HEALTH",
  "ALERT",
  "SETTINGS"
};

static pthread_mutex_t g_service_lock = PTHREAD_MUTEX_INITIALIZER;
static enum blind_badge_service_state g_service_state =
  BLIND_BADGE_SERVICE_IDLE;
static enum blind_badge_menu_id g_service_active = BLIND_BADGE_MENU_VISION;
static bool g_vision_thread_active;
static bool g_vision_cancel_requested;
static int g_vision_result;

static bool service_id_valid(enum blind_badge_menu_id id)
{
  return (size_t)id < sizeof(g_service_labels) /
                      sizeof(g_service_labels[0]);
}

static void *vision_service_worker(void *arg)
{
  bool cancelled;
  int ret;

  (void)arg;
  pthread_mutex_lock(&g_service_lock);
  cancelled = g_vision_cancel_requested;
  pthread_mutex_unlock(&g_service_lock);

  if (!cancelled)
    {
      /* The LCD and camera both use high-throughput ESP32-S3 peripherals.
       * Let the STARTING page finish before camera/DMA initialization.
       */

      usleep(BLIND_BADGE_VISION_DISPLAY_SETTLE_US);

      pthread_mutex_lock(&g_service_lock);
      cancelled = g_vision_cancel_requested;
      pthread_mutex_unlock(&g_service_lock);
    }

  ret = cancelled ? -ECANCELED :
        blind_badge_vision_server_run(BLIND_BADGE_VISION_PORT);

  pthread_mutex_lock(&g_service_lock);
  g_vision_result = ret;
  g_vision_thread_active = false;
  if (g_service_active == BLIND_BADGE_MENU_VISION)
    {
      if (g_vision_cancel_requested ||
          g_service_state == BLIND_BADGE_SERVICE_STOPPING)
        {
          g_service_state = BLIND_BADGE_SERVICE_IDLE;
        }
      else
        {
          g_service_state = BLIND_BADGE_SERVICE_ERROR;
        }
    }

  pthread_mutex_unlock(&g_service_lock);
  return NULL;
}

static int vision_service_start(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (g_vision_thread_active)
    {
      return -EBUSY;
    }

  g_vision_cancel_requested = false;
  g_vision_result = 0;
  g_vision_thread_active = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, BLIND_BADGE_VISION_STACK_SIZE);
  ret = pthread_create(&thread, &attr, vision_service_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      g_vision_thread_active = false;
      g_vision_result = -ret;
      return -ret;
    }

  pthread_detach(thread);
  return 0;
}

int blind_badge_service_start(enum blind_badge_menu_id id)
{
  if (!service_id_valid(id))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_service_lock);
  if (g_service_state != BLIND_BADGE_SERVICE_IDLE)
    {
      pthread_mutex_unlock(&g_service_lock);
      return -EBUSY;
    }

  g_service_active = id;
  if (id == BLIND_BADGE_MENU_VISION)
    {
      g_service_state = BLIND_BADGE_SERVICE_STARTING;
      if (vision_service_start() < 0)
        {
          g_service_state = BLIND_BADGE_SERVICE_ERROR;
          pthread_mutex_unlock(&g_service_lock);
          return g_vision_result;
        }
    }
  else
    {
      /* Other entries remain placeholders until their implementations are
       * connected.
       */

      g_service_state = BLIND_BADGE_SERVICE_RUNNING;
    }

  pthread_mutex_unlock(&g_service_lock);
  return 0;
}

int blind_badge_service_stop(void)
{
  enum blind_badge_menu_id active;
  bool thread_active;
  int retries;

  pthread_mutex_lock(&g_service_lock);
  active = g_service_active;
  thread_active = g_vision_thread_active;
  if (active == BLIND_BADGE_MENU_VISION && thread_active)
    {
      g_vision_cancel_requested = true;
      g_service_state = BLIND_BADGE_SERVICE_STOPPING;
    }
  else
    {
      g_service_state = BLIND_BADGE_SERVICE_IDLE;
    }

  pthread_mutex_unlock(&g_service_lock);

  if (active == BLIND_BADGE_MENU_VISION && thread_active)
    {
      /* The worker may not have entered run() yet.  Retry briefly so a
       * double PLAY during STARTING cannot leave a late server behind.
       */

      for (retries = 0; retries < 25; retries++)
        {
          int ret = blind_badge_vision_server_stop();

          if (ret == 0)
            {
              break;
            }

          pthread_mutex_lock(&g_service_lock);
          thread_active = g_vision_thread_active;
          pthread_mutex_unlock(&g_service_lock);
          if (!thread_active)
            {
              break;
            }

          usleep(20000);
        }
    }

  return 0;
}

enum blind_badge_service_state blind_badge_service_state(void)
{
  enum blind_badge_service_state state;
  enum blind_badge_menu_id active;
  struct blind_badge_vision_server_snapshot snapshot;

  pthread_mutex_lock(&g_service_lock);
  state = g_service_state;
  active = g_service_active;
  pthread_mutex_unlock(&g_service_lock);

  if (active == BLIND_BADGE_MENU_VISION &&
      state == BLIND_BADGE_SERVICE_STARTING &&
      blind_badge_vision_server_snapshot(&snapshot) == 0 &&
      snapshot.running && snapshot.camera_ready && snapshot.listening)
    {
      pthread_mutex_lock(&g_service_lock);
      if (g_service_state == BLIND_BADGE_SERVICE_STARTING)
        {
          g_service_state = BLIND_BADGE_SERVICE_RUNNING;
        }

      state = g_service_state;
      pthread_mutex_unlock(&g_service_lock);
    }

  return state;
}

const char *blind_badge_service_state_name(
  enum blind_badge_service_state state)
{
  switch (state)
    {
      case BLIND_BADGE_SERVICE_RUNNING:
        return "RUNNING";
      case BLIND_BADGE_SERVICE_STARTING:
        return "STARTING";
      case BLIND_BADGE_SERVICE_STOPPING:
        return "STOPPING";
      case BLIND_BADGE_SERVICE_ERROR:
        return "ERROR";
      case BLIND_BADGE_SERVICE_UNAVAILABLE:
        return "NOT READY";
      case BLIND_BADGE_SERVICE_IDLE:
      default:
        return "IDLE";
    }
}

enum blind_badge_menu_id blind_badge_service_active(void)
{
  enum blind_badge_menu_id id;

  pthread_mutex_lock(&g_service_lock);
  id = g_service_active;
  pthread_mutex_unlock(&g_service_lock);
  return id;
}

const char *blind_badge_service_label(enum blind_badge_menu_id id)
{
  return service_id_valid(id) ? g_service_labels[id] : "UNKNOWN";
}

int blind_badge_service_detail(char *buf, size_t size)
{
  enum blind_badge_menu_id active;
  struct blind_badge_vision_server_snapshot snapshot;
  int vision_result;

  if (buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_service_lock);
  active = g_service_active;
  vision_result = g_vision_result;
  pthread_mutex_unlock(&g_service_lock);

  if (active == BLIND_BADGE_MENU_VISION &&
      blind_badge_vision_server_snapshot(&snapshot) == 0)
    {
      if (snapshot.listening)
        {
          snprintf(buf, size, "%s:%d", snapshot.address,
                   snapshot.port);
        }
      else if (snapshot.running)
        {
          /* Keep the text stable during camera initialization so the menu
           * refresh poll does not trigger concurrent LCD transfers.
           */

          snprintf(buf, size, "starting vision");
        }
      else if (vision_result < 0)
        {
          snprintf(buf, size, "start failed: %d", vision_result);
        }
      else
        {
          snprintf(buf, size, "starting vision");
        }

      return 0;
    }

  snprintf(buf, size, "service pending");
  return 0;
}
