/****************************************************************************
 * Contest 2026 team 024 - BlindBadge menu services
 ****************************************************************************/

#include "blind_badge_service.h"
#include "blind_badge_ai.h"
#include "blind_badge_vision_server.h"
#include "uart_link.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BLIND_BADGE_VISION_PORT 80
#define BLIND_BADGE_VISION_STACK_SIZE 16384
#define BLIND_BADGE_VISION_DISPLAY_SETTLE_US 500000
#define BLIND_BADGE_VOICE_STACK_SIZE 16384
#define BLIND_BADGE_VOICE_TEXT_SIZE 96
#define BLIND_BADGE_VOICE_REPLY_SIZE 256
#define BLIND_BADGE_VOICE_DETAIL_SIZE 40

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
static int g_alert_result;
static pthread_cond_t g_voice_cond = PTHREAD_COND_INITIALIZER;
static bool g_voice_thread_active;
static bool g_voice_cancel_requested;
static bool g_voice_text_pending;
static bool g_voice_sub_idle;
static unsigned int g_voice_generation;
static int g_voice_result;
static char g_voice_text[BLIND_BADGE_VOICE_TEXT_SIZE];
static char g_voice_detail[BLIND_BADGE_VOICE_DETAIL_SIZE];

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

static int alert_service_start(void)
{
  if (!UART_LINK_IsConnected())
    {
      return -ENOTCONN;
    }

  if (UART_LINK_SendFlag("EMERGENCY_SEND") != 0)
    {
      return -EIO;
    }

  return 0;
}

static void voice_keep_first_sentence(char *text)
{
  static const char *const endings[] = {"。", "！", "？", "!", "?", "\n"};
  char *first = NULL;
  size_t ending_size = 0;
  size_t i;

  for (i = 0; i < sizeof(endings) / sizeof(endings[0]); i++)
    {
      char *found = strstr(text, endings[i]);

      if (found != NULL && (first == NULL || found < first))
        {
          first = found;
          ending_size = strlen(endings[i]);
        }
    }

  if (first != NULL)
    {
      first[ending_size] = '\0';
    }
}

static void *voice_service_worker(void *arg)
{
  char recognized[BLIND_BADGE_VOICE_TEXT_SIZE];
  char prompt[384];
  char response[BLIND_BADGE_VOICE_REPLY_SIZE];
  unsigned int generation = (unsigned int)(uintptr_t)arg;
  int used_fallback = 0;
  int ret;

  pthread_mutex_lock(&g_service_lock);
  while (!g_voice_cancel_requested && generation == g_voice_generation)
    {
      while (!g_voice_text_pending && !g_voice_cancel_requested &&
             generation == g_voice_generation)
        {
          pthread_cond_wait(&g_voice_cond, &g_service_lock);
        }

      if (g_voice_cancel_requested || generation != g_voice_generation)
        {
          break;
        }

      strlcpy(recognized, g_voice_text, sizeof(recognized));
      g_voice_text_pending = false;
      strlcpy(g_voice_detail, "AI thinking", sizeof(g_voice_detail));
      pthread_mutex_unlock(&g_service_lock);

      snprintf(prompt, sizeof(prompt), "用户说：%s。你是 BlindBadge "
               "随身语音助手，只回答一句中文，不超过四十个汉字。",
               recognized);
      used_fallback = 0;
      ret = blind_badge_ask_ai(BLIND_BADGE_AI_DIRECT, prompt,
                               "我听到了，请再说一遍需要我帮什么。",
                               response, sizeof(response), &used_fallback);
      voice_keep_first_sentence(response);

      pthread_mutex_lock(&g_service_lock);
      g_voice_result = ret;
      if (g_voice_cancel_requested || generation != g_voice_generation)
        {
          break;
        }

      if (ret == 0 && UART_LINK_SendSpeech(response) == 0)
        {
          strlcpy(g_voice_detail,
                  used_fallback ? "local reply sent" : "AI reply sent",
                  sizeof(g_voice_detail));
        }
      else
        {
          if (ret == 0)
            {
              g_voice_result = -EIO;
            }

          snprintf(g_voice_detail, sizeof(g_voice_detail),
                   "voice failed: %d", g_voice_result);
          g_service_state = BLIND_BADGE_SERVICE_ERROR;
          break;
        }
    }

  g_voice_thread_active = false;
  if (g_service_active == BLIND_BADGE_MENU_VOICE &&
      g_service_state == BLIND_BADGE_SERVICE_STOPPING &&
      g_voice_sub_idle)
    {
      g_service_state = BLIND_BADGE_SERVICE_IDLE;
    }

  pthread_mutex_unlock(&g_service_lock);
  return NULL;
}

static int voice_service_start(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (!UART_LINK_IsConnected())
    {
      return -ENOTCONN;
    }

  g_voice_generation++;
  g_voice_cancel_requested = false;
  g_voice_text_pending = false;
  g_voice_sub_idle = false;
  g_voice_text[0] = '\0';
  g_voice_result = 0;
  strlcpy(g_voice_detail, "listening", sizeof(g_voice_detail));
  g_voice_thread_active = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, BLIND_BADGE_VOICE_STACK_SIZE);
  ret = pthread_create(&thread, &attr, voice_service_worker,
                       (void *)(uintptr_t)g_voice_generation);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      g_voice_thread_active = false;
      return -ret;
    }

  pthread_detach(thread);
  if (UART_LINK_SendFlag("VOICE_START") != 0)
    {
      g_voice_cancel_requested = true;
      pthread_cond_signal(&g_voice_cond);
      return -EIO;
    }

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
  else if (id == BLIND_BADGE_MENU_VOICE)
    {
      g_service_state = BLIND_BADGE_SERVICE_STARTING;
      g_voice_result = voice_service_start();
      if (g_voice_result < 0)
        {
          g_service_state = BLIND_BADGE_SERVICE_ERROR;
          pthread_mutex_unlock(&g_service_lock);
          return g_voice_result;
        }

      g_service_state = BLIND_BADGE_SERVICE_RUNNING;
    }
  else if (id == BLIND_BADGE_MENU_ALERT)
    {
      g_service_state = BLIND_BADGE_SERVICE_STARTING;
      g_alert_result = alert_service_start();
      if (g_alert_result < 0)
        {
          g_service_state = BLIND_BADGE_SERVICE_ERROR;
          pthread_mutex_unlock(&g_service_lock);
          return g_alert_result;
        }

      g_service_state = BLIND_BADGE_SERVICE_RUNNING;
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
  else if (active == BLIND_BADGE_MENU_VOICE)
    {
      int stop_ret;

      g_voice_generation++;
      g_voice_cancel_requested = true;
      g_voice_text_pending = false;
      g_voice_text[0] = '\0';
      stop_ret = UART_LINK_SendFlag("VOICE_STOP");
      g_voice_sub_idle = !UART_LINK_IsConnected() || stop_ret != 0;
      if (g_voice_thread_active || !g_voice_sub_idle)
        {
          g_service_state = BLIND_BADGE_SERVICE_STOPPING;
          pthread_cond_signal(&g_voice_cond);
        }
      else
        {
          g_service_state = BLIND_BADGE_SERVICE_IDLE;
        }
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

int blind_badge_service_submit_asr(const char *text)
{
  int ret = 0;

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_service_lock);
  if (g_service_active != BLIND_BADGE_MENU_VOICE ||
      g_service_state != BLIND_BADGE_SERVICE_RUNNING ||
      !g_voice_thread_active || g_voice_cancel_requested)
    {
      ret = -EACCES;
    }
  else if (g_voice_text_pending)
    {
      ret = -EBUSY;
    }
  else
    {
      strlcpy(g_voice_text, text, sizeof(g_voice_text));
      g_voice_text_pending = true;
      strlcpy(g_voice_detail, "ASR received", sizeof(g_voice_detail));
      pthread_cond_signal(&g_voice_cond);
    }

  pthread_mutex_unlock(&g_service_lock);
  return ret;
}

void blind_badge_service_voice_idle(void)
{
  pthread_mutex_lock(&g_service_lock);
  if (g_service_active == BLIND_BADGE_MENU_VOICE &&
      g_service_state == BLIND_BADGE_SERVICE_STOPPING)
    {
      g_voice_sub_idle = true;
      if (!g_voice_thread_active)
        {
          g_service_state = BLIND_BADGE_SERVICE_IDLE;
        }
    }

  pthread_mutex_unlock(&g_service_lock);
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
  int alert_result;
  char voice_detail[BLIND_BADGE_VOICE_DETAIL_SIZE];

  if (buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_service_lock);
  active = g_service_active;
  vision_result = g_vision_result;
  alert_result = g_alert_result;
  strlcpy(voice_detail, g_voice_detail, sizeof(voice_detail));
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

  if (active == BLIND_BADGE_MENU_ALERT)
    {
      if (alert_result == -ENOTCONN)
        {
          snprintf(buf, size, "sub-board offline");
        }
      else if (alert_result < 0)
        {
          snprintf(buf, size, "alert failed: %d", alert_result);
        }
      else
        {
          snprintf(buf, size, "alert requested");
        }

      return 0;
    }

  if (active == BLIND_BADGE_MENU_VOICE)
    {
      snprintf(buf, size, "%s", voice_detail[0] != '\0' ?
               voice_detail : "listening");
      return 0;
    }

  snprintf(buf, size, "service pending");
  return 0;
}
