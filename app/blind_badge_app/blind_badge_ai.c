/****************************************************************************
 * Contest 2026 team 024 - BlindBadge AI adapter
 ****************************************************************************/

#include "blind_badge_ai.h"

#include "blind_badge_policy.h"

#include <nuttx/config.h>

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "infra/config_store.h"
#include "infra/network_manager.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"

#include <pthread.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
#include <time.h>
#endif

#define BLIND_BADGE_AGENT_CHANNEL "blind_badge"
#define BLIND_BADGE_AGENT_CHAT_ID "badge_event"
#define BLIND_BADGE_AGENT_TIMEOUT_MS 75000

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
struct blind_badge_agent_waiter
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int done;
  char response[512];
};

static void blind_badge_agent_tap(const agent_msg_t *msg, void *cookie)
{
  struct blind_badge_agent_waiter *waiter = cookie;

  pthread_mutex_lock(&waiter->lock);
  if (msg->content != NULL)
    {
      strncpy(waiter->response, msg->content, sizeof(waiter->response) - 1);
      waiter->response[sizeof(waiter->response) - 1] = '\0';
    }

  waiter->done = 1;
  pthread_cond_signal(&waiter->cond);
  pthread_mutex_unlock(&waiter->lock);
}

static void add_timeout_ms(struct timespec *ts, int timeout_ms)
{
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += timeout_ms / 1000;
  ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
    }
}
#endif

static int fallback_with_reason(const char *reason,
                                const char *local_suggestion,
                                char *response, size_t response_size,
                                int *used_fallback)
{
  printf("[BlindBadge] ai_error: %s\n", reason);
  printf("[BlindBadge] fallback_reason: %s\n", reason);
  blind_badge_fallback_response(local_suggestion, response, response_size);
  if (used_fallback != NULL)
    {
      *used_fallback = 1;
    }

  printf("[BlindBadge] fallback_response: %s\n", response);
  return 0;
}

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
static int ask_agent(const char *ai_prompt,
                     const char *local_suggestion,
                     char *response, size_t response_size,
                     int *used_fallback)
{
  struct blind_badge_agent_waiter waiter;
  agent_msg_t msg;
  struct timespec ts;
  int ret;

  memset(&waiter, 0, sizeof(waiter));
  pthread_mutex_init(&waiter.lock, NULL);
  pthread_cond_init(&waiter.cond, NULL);

  ret = mbus_tap_register(BLIND_BADGE_AGENT_CHANNEL,
                          blind_badge_agent_tap, &waiter);
  if (ret != 0)
    {
      pthread_cond_destroy(&waiter.cond);
      pthread_mutex_destroy(&waiter.lock);
      return fallback_with_reason("failed to register agent response tap",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  memset(&msg, 0, sizeof(msg));
  strncpy(msg.channel, BLIND_BADGE_AGENT_CHANNEL, sizeof(msg.channel) - 1);
  strncpy(msg.chat_id, BLIND_BADGE_AGENT_CHAT_ID, sizeof(msg.chat_id) - 1);
  msg.content = strdup(ai_prompt);
  if (msg.content == NULL)
    {
      mbus_tap_unregister(BLIND_BADGE_AGENT_CHANNEL);
      pthread_cond_destroy(&waiter.cond);
      pthread_mutex_destroy(&waiter.lock);
      return fallback_with_reason("out of memory",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  if (message_bus_push_inbound(&msg) != 0)
    {
      free(msg.content);
      mbus_tap_unregister(BLIND_BADGE_AGENT_CHANNEL);
      pthread_cond_destroy(&waiter.cond);
      pthread_mutex_destroy(&waiter.lock);
      return fallback_with_reason("failed to send prompt to ai_agent",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  printf("[BlindBadge] ai_status: waiting for ai_agent response...\n");

  add_timeout_ms(&ts, BLIND_BADGE_AGENT_TIMEOUT_MS);
  pthread_mutex_lock(&waiter.lock);
  while (!waiter.done)
    {
      ret = pthread_cond_timedwait(&waiter.cond, &waiter.lock, &ts);
      if (ret != 0)
        {
          break;
        }
    }

  pthread_mutex_unlock(&waiter.lock);
  mbus_tap_unregister(BLIND_BADGE_AGENT_CHANNEL);

  if (!waiter.done)
    {
      pthread_cond_destroy(&waiter.cond);
      pthread_mutex_destroy(&waiter.lock);
      return fallback_with_reason("timeout waiting for ai_agent response",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  blind_badge_normalize_ai_response(waiter.response, local_suggestion,
                                    response, response_size);
  pthread_cond_destroy(&waiter.cond);
  pthread_mutex_destroy(&waiter.lock);
  return 0;
}

static int ask_ai_direct(const char *ai_prompt,
                         const char *local_suggestion,
                         char *response, size_t response_size,
                         int *used_fallback)
{
  const char *system_prompt =
    "你是盲人辅助胸牌的语音提醒模块。只输出一句简短中文提醒，"
    "优先安全，不长篇解释，不承诺绝对安全。"
    "obstacle_near低于50厘米必须先提醒停下，50到99厘米提醒减速绕行，"
    "100厘米及以上只提醒保持注意；step_down必须提醒停下或放慢并确认；"
    "emergency输出适合联系人或附近人员的一句求助信息。";
  char messages_json[768];
  char raw_response[512];
  int ret;

  ret = config_store_init();
  if (ret != 0)
    {
      return fallback_with_reason("config_store_init failed",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  ret = llm_proxy_init();
  if (ret != 0)
    {
      return fallback_with_reason("llm_proxy_init failed",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  ret = llm_router_init();
  if (ret != 0)
    {
      return fallback_with_reason("llm_router_init failed",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  ret = llm_router_apply(0);
  if (ret != 0)
    {
      return fallback_with_reason("no router backend at slot 0",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  snprintf(messages_json, sizeof(messages_json),
           "[{\"role\":\"user\",\"content\":\"%s\"}]", ai_prompt);

  printf("[BlindBadge] ai_status: calling ai_agent llm router...\n");
  memset(raw_response, 0, sizeof(raw_response));
  ret = llm_chat(system_prompt, messages_json, raw_response,
                 sizeof(raw_response));
  if (ret != 0)
    {
      return fallback_with_reason(raw_response[0] ? raw_response :
                                  "llm_chat failed",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  blind_badge_normalize_ai_response(raw_response, local_suggestion,
                                    response, response_size);
  if (response[0] == '\0')
    {
      return fallback_with_reason("empty ai response",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }

  return 0;
}
#endif

int blind_badge_ask_ai(enum blind_badge_ai_mode mode,
                       const char *ai_prompt,
                       const char *local_suggestion,
                       char *response, size_t response_size,
                       int *used_fallback)
{
  if (used_fallback != NULL)
    {
      *used_fallback = 0;
    }

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
  if ((mode == BLIND_BADGE_AI_DIRECT || mode == BLIND_BADGE_AI_AGENT) &&
      !network_is_connected())
    {
      return fallback_with_reason("WiFi is not connected",
                                  local_suggestion, response, response_size,
                                  used_fallback);
    }
#endif

  if (mode == BLIND_BADGE_AI_DIRECT)
    {
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
      return ask_ai_direct(ai_prompt, local_suggestion, response,
                           response_size, used_fallback);
#else
      return fallback_with_reason("ai_agent not enabled in this build",
                                  local_suggestion, response, response_size,
                                  used_fallback);
#endif
    }

  if (mode == BLIND_BADGE_AI_AGENT)
    {
#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
      return ask_agent(ai_prompt, local_suggestion, response, response_size,
                       used_fallback);
#else
      return fallback_with_reason("ai_agent not enabled in this build",
                                  local_suggestion, response, response_size,
                                  used_fallback);
#endif
    }

  blind_badge_fallback_response(local_suggestion, response, response_size);
  if (used_fallback != NULL)
    {
      *used_fallback = 1;
    }

  return 0;
}
