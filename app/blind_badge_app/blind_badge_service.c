/****************************************************************************
 * Contest 2026 team 024 - BlindBadge menu services
 ****************************************************************************/

#include "blind_badge_service.h"
#include "blind_badge_ai.h"
#include "blind_badge_vision_server.h"
#include "uart_link.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BLIND_BADGE_VISION_PORT 80
#define BLIND_BADGE_VISION_STACK_SIZE 16384
#define BLIND_BADGE_VISION_DISPLAY_SETTLE_US 500000
#define BLIND_BADGE_VOICE_STACK_SIZE 16384
#define BLIND_BADGE_VOICE_PRIORITY 100
#define BLIND_BADGE_VOICE_TEXT_SIZE 96
#define BLIND_BADGE_VOICE_REPLY_SIZE 256
#define BLIND_BADGE_VOICE_DETAIL_SIZE 40
#define BLIND_BADGE_VOICE_SEND_RETRIES 60
#define BLIND_BADGE_VOICE_REPLY_RETRIES 300
#define BLIND_BADGE_VOICE_REPLY_TIMEOUT_MS 25000
#define BLIND_BADGE_VOICE_SEND_RETRY_US 100000
#define BLIND_BADGE_TEST_STACK_SIZE 16384
#define BLIND_BADGE_TEST_PRIORITY 100
#define BLIND_BADGE_TEST_REPLY_SIZE 256
#define BLIND_BADGE_TEST_DETAIL_SIZE 40

static const char *const g_service_labels[] =
{
  "VISION",
  "VOICE",
  "HEALTH",
  "ALERT",
  "SETTINGS",
  "TEST"
};

static pthread_mutex_t g_service_lock = PTHREAD_MUTEX_INITIALIZER;
static enum blind_badge_service_state g_service_state =
  BLIND_BADGE_SERVICE_IDLE;
static enum blind_badge_menu_id g_service_active = BLIND_BADGE_MENU_VISION;
static bool g_vision_thread_active;
static bool g_vision_thread_joinable;
static pthread_t g_vision_thread;
static bool g_vision_cancel_requested;
static int g_vision_result;
static int g_alert_result;
static pthread_cond_t g_voice_cond = PTHREAD_COND_INITIALIZER;
static bool g_voice_task_active;
static pid_t g_voice_task_pid = -1;
static bool g_voice_cancel_requested;
static bool g_voice_text_pending;
static bool g_voice_sub_idle;
static unsigned int g_voice_generation;
static int g_voice_result;
static char g_voice_text[BLIND_BADGE_VOICE_TEXT_SIZE];
static char g_voice_detail[BLIND_BADGE_VOICE_DETAIL_SIZE];
static bool g_test_task_active;
static bool g_test_cancel_requested;
static pid_t g_test_task_pid = -1;
static unsigned int g_test_generation;
static int g_test_result;
static char g_test_detail[BLIND_BADGE_TEST_DETAIL_SIZE];

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

  g_vision_thread = thread;
  g_vision_thread_joinable = true;
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

static int voice_send_reply(const char *text, unsigned int generation)
{
  struct timespec started;
  int retry;

  clock_gettime(CLOCK_MONOTONIC, &started);
  for (retry = 0; retry < BLIND_BADGE_VOICE_REPLY_RETRIES; retry++)
    {
      struct timespec now;
      bool cancelled;

      pthread_mutex_lock(&g_service_lock);
      cancelled = g_voice_cancel_requested ||
                  generation != g_voice_generation;
      pthread_mutex_unlock(&g_service_lock);
      if (cancelled)
        {
          return -ECANCELED;
        }

      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
          ((now.tv_sec - started.tv_sec) * 1000L +
           (now.tv_nsec - started.tv_nsec) / 1000000L) >=
          BLIND_BADGE_VOICE_REPLY_TIMEOUT_MS)
        {
          break;
        }

      if (UART_LINK_IsConnected())
        {
          int send_ret = UART_LINK_SendSpeech(text);

          if (send_ret == 0)
            {
              return 0;
            }
        }

      usleep(BLIND_BADGE_VOICE_SEND_RETRY_US);
    }

  return -EIO;
}

static int voice_resume_listening(unsigned int generation)
{
  int retry;

  for (retry = 0; retry < BLIND_BADGE_VOICE_SEND_RETRIES; retry++)
    {
      bool cancelled;

      pthread_mutex_lock(&g_service_lock);
      cancelled = g_voice_cancel_requested ||
                  generation != g_voice_generation;
      pthread_mutex_unlock(&g_service_lock);
      if (cancelled)
        {
          return -ECANCELED;
        }

      if (UART_LINK_IsConnected() &&
          UART_LINK_SendFlag("VOICE_START") == 0)
        {
          return 0;
        }

      usleep(BLIND_BADGE_VOICE_SEND_RETRY_US);
    }

  return -EIO;
}

static int voice_service_worker(int argc, char *argv[])
{
  char recognized[BLIND_BADGE_VOICE_TEXT_SIZE];
  char response[BLIND_BADGE_VOICE_REPLY_SIZE];
  unsigned int generation;
  int used_fallback;
  int ret;

  (void)argc;
  (void)argv;
  generation = g_voice_generation;
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
      printf("[BlindBadge] voice_asr: %s\n", recognized);

      /* Never hold the service lock across DNS/TLS/LLM work.  stop() must
       * remain able to mark this generation cancelled and abort the active
       * socket while the request is in progress.
       */

      pthread_mutex_unlock(&g_service_lock);
      memset(response, 0, sizeof(response));
      used_fallback = 0;
      ret = blind_badge_ask_ai(
              BLIND_BADGE_AI_DIRECT, recognized,
              "我听到了，请再说一遍需要我帮什么。",
              response, sizeof(response), &used_fallback);
      pthread_mutex_lock(&g_service_lock);

      if (g_voice_cancel_requested || generation != g_voice_generation)
        {
          break;
        }

      if (ret == 0 && !used_fallback && response[0] != '\0')
        {
          printf("[BlindBadge] voice_ai_result: text=%s\n", response);

          /* Release the lock while sending to keep link callbacks and a
           * simultaneous forced stop independent of this worker.
           */

          pthread_mutex_unlock(&g_service_lock);
          ret = voice_send_reply(response, generation);
          pthread_mutex_lock(&g_service_lock);
        }
      else if (used_fallback)
        {
          /* A fallback sentence must never be presented as an AI answer.
           * Resume VAD without speaking so the next real request remains
           * unambiguous during validation and normal use.
           */

          printf("[BlindBadge] voice_ai_failed: fallback suppressed\n");
          pthread_mutex_unlock(&g_service_lock);
          ret = voice_resume_listening(generation);
          pthread_mutex_lock(&g_service_lock);
          if (ret == 0)
            {
              strlcpy(g_voice_detail, "AI failed; listening",
                      sizeof(g_voice_detail));
            }
        }
      else if (ret == 0)
        {
          ret = -ENODATA;
        }

      if (g_voice_cancel_requested || generation != g_voice_generation)
        {
          break;
        }

      g_voice_result = ret;
      if (ret == 0)
        {
          if (!used_fallback)
            {
              strlcpy(g_voice_detail, "reply sent",
                      sizeof(g_voice_detail));
              printf("[BlindBadge] voice_ai_response: %s\n", response);
            }
        }
      else
        {
          snprintf(g_voice_detail, sizeof(g_voice_detail),
                   "reply failed: %d", ret);
        }
    }

  g_voice_task_active = false;
  g_voice_task_pid = -1;
  if (g_service_active == BLIND_BADGE_MENU_VOICE &&
      g_service_state == BLIND_BADGE_SERVICE_STOPPING &&
      g_voice_sub_idle)
    {
      g_service_state = BLIND_BADGE_SERVICE_IDLE;
    }

  pthread_mutex_unlock(&g_service_lock);
  return 0;
}

static int voice_service_start(void)
{
  pid_t pid;

  if (!UART_LINK_IsConnected())
    {
      return -ENOTCONN;
    }

  if (!blind_badge_ai_ready())
    {
      strlcpy(g_voice_detail, "AI not configured",
              sizeof(g_voice_detail));
      return -ENOENT;
    }

  blind_badge_ai_begin_session();

  g_voice_generation++;
  g_voice_cancel_requested = false;
  g_voice_text_pending = false;
  g_voice_sub_idle = false;
  g_voice_text[0] = '\0';
  g_voice_result = 0;
  strlcpy(g_voice_detail, "listening", sizeof(g_voice_detail));
  g_voice_task_active = true;

  pid = task_create("bb_voice", BLIND_BADGE_VOICE_PRIORITY,
                    BLIND_BADGE_VOICE_STACK_SIZE,
                    voice_service_worker, NULL);
  if (pid < 0)
    {
      g_voice_task_active = false;
      return -errno;
    }

  g_voice_task_pid = pid;
  if (UART_LINK_SendFlag("VOICE_START") != 0)
    {
      g_voice_cancel_requested = true;
      pthread_cond_signal(&g_voice_cond);
      return -EIO;
    }

  return 0;
}

static int test_service_worker(int argc, char *argv[])
{
  char response[BLIND_BADGE_TEST_REPLY_SIZE] = { 0 };
  unsigned int generation;
  int used_fallback = 0;
  int ret;

  (void)argc;
  (void)argv;
  generation = g_test_generation;
  ret = blind_badge_ask_ai(BLIND_BADGE_AI_DIRECT,
                           "这是独立AI连通测试。请只回复：测试成功。",
                           "测试失败。", response, sizeof(response),
                           &used_fallback);
  if (ret == 0 && used_fallback)
    {
      ret = -EIO;
    }

  pthread_mutex_lock(&g_service_lock);
  g_test_task_active = false;
  g_test_result = ret;
  if (!g_test_cancel_requested && generation == g_test_generation &&
      g_service_active == BLIND_BADGE_MENU_TEST)
    {
      if (ret == 0)
        {
          strlcpy(g_test_detail, "AI request OK", sizeof(g_test_detail));
          g_service_state = BLIND_BADGE_SERVICE_RUNNING;
          printf("[BlindBadge] test_ai_response: %s\n", response);
        }
      else
        {
          snprintf(g_test_detail, sizeof(g_test_detail),
                   "AI request failed: %d", ret);
          g_service_state = BLIND_BADGE_SERVICE_ERROR;
        }
    }

  pthread_mutex_unlock(&g_service_lock);
  return 0;
}

static int test_service_start(void)
{
  pid_t pid;

  if (!blind_badge_ai_ready())
    {
      strlcpy(g_test_detail, "AI not configured", sizeof(g_test_detail));
      return -ENOENT;
    }

  blind_badge_ai_begin_session();
  g_test_generation++;
  g_test_cancel_requested = false;
  g_test_result = 0;
  strlcpy(g_test_detail, "AI requesting", sizeof(g_test_detail));
  g_test_task_active = true;

  pid = task_create("bb_ai_test", BLIND_BADGE_TEST_PRIORITY,
                    BLIND_BADGE_TEST_STACK_SIZE, test_service_worker, NULL);
  if (pid < 0)
    {
      g_test_task_active = false;
      g_test_result = -errno;
      return g_test_result;
    }

  g_test_task_pid = pid;
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
  else if (id == BLIND_BADGE_MENU_TEST)
    {
      g_service_state = BLIND_BADGE_SERVICE_STARTING;
      g_test_result = test_service_start();
      if (g_test_result < 0)
        {
          g_service_state = BLIND_BADGE_SERVICE_ERROR;
          pthread_mutex_unlock(&g_service_lock);
          return g_test_result;
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
  bool vision_joinable = false;
  pthread_t vision_thread = 0;
  pid_t voice_task_pid = -1;
  pid_t test_task_pid = -1;

  pthread_mutex_lock(&g_service_lock);
  active = g_service_active;
  g_service_state = BLIND_BADGE_SERVICE_STOPPING;

  if (active == BLIND_BADGE_MENU_VISION)
    {
      g_vision_cancel_requested = true;
      vision_joinable = g_vision_thread_joinable;
      vision_thread = g_vision_thread;
    }
  else if (active == BLIND_BADGE_MENU_VOICE)
    {
      g_voice_generation++;
      g_voice_cancel_requested = true;
      g_voice_text_pending = false;
      g_voice_text[0] = '\0';
      g_voice_sub_idle = false;
      voice_task_pid = g_voice_task_pid;
      pthread_cond_broadcast(&g_voice_cond);
    }
  else if (active == BLIND_BADGE_MENU_TEST)
    {
      g_test_generation++;
      g_test_cancel_requested = true;
      test_task_pid = g_test_task_pid;
    }

  pthread_mutex_unlock(&g_service_lock);

  if (active == BLIND_BADGE_MENU_VISION)
    {
      /* stop() is also safe while the worker is still in its display
       * settling delay.  The cancel flag prevents a late server start.
       */

      (void)blind_badge_vision_server_stop();
      if (vision_joinable)
        {
          (void)pthread_join(vision_thread, NULL);
        }
    }
  else if (active == BLIND_BADGE_MENU_VOICE)
    {
      int retries;
      bool active_now = true;

      /* The sub-board also treats the following MENU mode change as a hard
       * cancellation.  VOICE_STOP asks it to release microphone/ASR first;
       * loss of that packet must not trap the main board in STOPPING.
       */

      (void)UART_LINK_SendFlag("VOICE_STOP");
      for (retries = 0; retries < 100; retries++)
        {
          blind_badge_ai_cancel_active();
          pthread_mutex_lock(&g_service_lock);
          active_now = g_voice_task_active;
          pthread_mutex_unlock(&g_service_lock);
          if (!active_now)
            {
              break;
            }

          usleep(20000);
        }

      if (active_now && voice_task_pid > 0)
        {
          (void)task_delete(voice_task_pid);
        }

      blind_badge_ai_reset_aborted();
    }
  else if (active == BLIND_BADGE_MENU_TEST)
    {
      int retries;
      bool active_now = true;

      for (retries = 0; retries < 100; retries++)
        {
          blind_badge_ai_cancel_active();
          pthread_mutex_lock(&g_service_lock);
          active_now = g_test_task_active;
          pthread_mutex_unlock(&g_service_lock);
          if (!active_now)
            {
              break;
            }

          usleep(20000);
        }

      if (active_now && test_task_pid > 0)
        {
          (void)task_delete(test_task_pid);
          blind_badge_ai_reset_aborted();
        }
    }

  pthread_mutex_lock(&g_service_lock);
  if (active == BLIND_BADGE_MENU_VISION)
    {
      g_vision_thread_active = false;
      g_vision_thread_joinable = false;
      g_vision_cancel_requested = false;
    }
  else if (active == BLIND_BADGE_MENU_VOICE)
    {
      g_voice_task_active = false;
      g_voice_task_pid = -1;
      g_voice_cancel_requested = false;
      g_voice_text_pending = false;
      g_voice_sub_idle = true;
      g_voice_text[0] = '\0';
      strlcpy(g_voice_detail, "idle", sizeof(g_voice_detail));
    }
  else if (active == BLIND_BADGE_MENU_TEST)
    {
      g_test_task_active = false;
      g_test_task_pid = -1;
      g_test_cancel_requested = false;
      strlcpy(g_test_detail, "idle", sizeof(g_test_detail));
    }

  g_service_state = BLIND_BADGE_SERVICE_IDLE;
  pthread_mutex_unlock(&g_service_lock);

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
      !g_voice_task_active || g_voice_cancel_requested)
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
      if (!g_voice_task_active)
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
  int test_result;
  char voice_detail[BLIND_BADGE_VOICE_DETAIL_SIZE];
  char test_detail[BLIND_BADGE_TEST_DETAIL_SIZE];

  if (buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_service_lock);
  active = g_service_active;
  vision_result = g_vision_result;
  alert_result = g_alert_result;
  test_result = g_test_result;
  strlcpy(voice_detail, g_voice_detail, sizeof(voice_detail));
  strlcpy(test_detail, g_test_detail, sizeof(test_detail));
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

  if (active == BLIND_BADGE_MENU_TEST)
    {
      if (test_detail[0] != '\0')
        {
          snprintf(buf, size, "%s", test_detail);
        }
      else
        {
          snprintf(buf, size, "AI request: %d", test_result);
        }

      return 0;
    }

  snprintf(buf, size, "service pending");
  return 0;
}
