/****************************************************************************
 * Contest 2026 team 024 - blind badge app sample
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "infra/config_store.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"

static int is_direction_valid(const char *direction)
{
  return strcmp(direction, "front") == 0 ||
         strcmp(direction, "left") == 0 ||
         strcmp(direction, "right") == 0;
}

static const char *direction_zh(const char *direction)
{
  if (strcmp(direction, "front") == 0)
    {
      return "前方";
    }

  if (strcmp(direction, "left") == 0)
    {
      return "左侧";
    }

  if (strcmp(direction, "right") == 0)
    {
      return "右侧";
    }

  return "未知方向";
}

static void print_help(void)
{
  printf("BlindBadge usage:\n");
  printf("  blind_badge_app help\n");
  printf("  blind_badge_app status\n");
  printf("  blind_badge_app obstacle <distance_cm> <front|left|right> [--ai|--agent]\n");
  printf("  blind_badge_app step_down <front|left|right> [--ai|--agent]\n");
  printf("  blind_badge_app emergency [--ai|--agent]\n");
  printf("  blind_badge_app demo [--ai|--agent]\n");
  printf("  --ai: call ai_agent LLM router directly and print ai_response\n");
  printf("  --agent: send ai_prompt to running ai_agent and wait for ai_response\n");
}

#define BLIND_BADGE_AGENT_CHANNEL "blind_badge"
#define BLIND_BADGE_AGENT_CHAT_ID "badge_event"
#define BLIND_BADGE_AGENT_TIMEOUT_MS 75000
#define BLIND_BADGE_RESPONSE_SIZE 512

enum blind_badge_ai_mode
{
  BLIND_BADGE_AI_OFF = 0,
  BLIND_BADGE_AI_DIRECT,
  BLIND_BADGE_AI_AGENT
};

struct blind_badge_agent_waiter
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int done;
  char response[BLIND_BADGE_RESPONSE_SIZE];
};

static enum blind_badge_ai_mode get_ai_mode(int argc, char *argv[])
{
  if (argc <= 0)
    {
      return BLIND_BADGE_AI_OFF;
    }

  if (strcmp(argv[argc - 1], "--ai") == 0)
    {
      return BLIND_BADGE_AI_DIRECT;
    }

  if (strcmp(argv[argc - 1], "--agent") == 0)
    {
      return BLIND_BADGE_AI_AGENT;
    }

  return BLIND_BADGE_AI_OFF;
}

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

static int ask_agent(const char *ai_prompt)
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
      printf("[BlindBadge] ai_error: failed to register agent response tap.\n");
      pthread_cond_destroy(&waiter.cond);
      pthread_mutex_destroy(&waiter.lock);
      return 1;
    }

  memset(&msg, 0, sizeof(msg));
  strncpy(msg.channel, BLIND_BADGE_AGENT_CHANNEL, sizeof(msg.channel) - 1);
  strncpy(msg.chat_id, BLIND_BADGE_AGENT_CHAT_ID, sizeof(msg.chat_id) - 1);
  msg.content = strdup(ai_prompt);
  if (msg.content == NULL)
    {
      printf("[BlindBadge] ai_error: out of memory.\n");
      mbus_tap_unregister(BLIND_BADGE_AGENT_CHANNEL);
      pthread_cond_destroy(&waiter.cond);
      pthread_mutex_destroy(&waiter.lock);
      return 1;
    }

  if (message_bus_push_inbound(&msg) != 0)
    {
      printf("[BlindBadge] ai_error: failed to send prompt to ai_agent. "
             "Start ai_agent first.\n");
      free(msg.content);
      mbus_tap_unregister(BLIND_BADGE_AGENT_CHANNEL);
      pthread_cond_destroy(&waiter.cond);
      pthread_mutex_destroy(&waiter.lock);
      return 1;
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

  if (waiter.done)
    {
      printf("[BlindBadge] ai_response: %s\n", waiter.response);
    }
  else
    {
      printf("[BlindBadge] ai_error: timeout waiting for ai_agent response. "
             "Confirm ai_agent is running and network/backend are configured.\n");
    }

  pthread_mutex_unlock(&waiter.lock);
  mbus_tap_unregister(BLIND_BADGE_AGENT_CHANNEL);
  pthread_cond_destroy(&waiter.cond);
  pthread_mutex_destroy(&waiter.lock);

  return waiter.done ? 0 : 1;
}

static int ask_ai_direct(const char *ai_prompt)
{
  const char *system_prompt =
    "你是盲人辅助胸牌的语音提醒模块。只输出一句简短中文提醒，"
    "优先安全，不长篇解释，不承诺绝对安全。"
    "obstacle_near低于50厘米必须先提醒停下，50到99厘米提醒减速绕行，"
    "100厘米及以上只提醒保持注意；step_down必须提醒停下或放慢并确认；"
    "emergency输出适合联系人或附近人员的一句求助信息。";
  char messages_json[768];
  char response[BLIND_BADGE_RESPONSE_SIZE];
  int ret;

  ret = config_store_init();
  if (ret != 0)
    {
      printf("[BlindBadge] ai_error: config_store_init failed.\n");
      return 1;
    }

  ret = llm_proxy_init();
  if (ret != 0)
    {
      printf("[BlindBadge] ai_error: llm_proxy_init failed.\n");
      return 1;
    }

  ret = llm_router_init();
  if (ret != 0)
    {
      printf("[BlindBadge] ai_error: llm_router_init failed.\n");
      return 1;
    }

  ret = llm_router_apply(0);
  if (ret != 0)
    {
      printf("[BlindBadge] ai_error: no router backend at slot 0. "
             "Configure MiMo with ai_agent router_set first.\n");
      return 1;
    }

  snprintf(messages_json, sizeof(messages_json),
           "[{\"role\":\"user\",\"content\":\"%s\"}]", ai_prompt);

  printf("[BlindBadge] ai_status: calling ai_agent llm router...\n");
  memset(response, 0, sizeof(response));
  ret = llm_chat(system_prompt, messages_json, response, sizeof(response));
  if (ret != 0)
    {
      printf("[BlindBadge] ai_error: %s\n",
             response[0] ? response : "llm_chat failed.");
      return 1;
    }

  printf("[BlindBadge] ai_response: %s\n", response);
  return 0;
}

static int run_ai_mode(enum blind_badge_ai_mode mode, const char *ai_prompt)
{
  if (mode == BLIND_BADGE_AI_DIRECT)
    {
      return ask_ai_direct(ai_prompt);
    }

  if (mode == BLIND_BADGE_AI_AGENT)
    {
      return ask_agent(ai_prompt);
    }

  return 0;
}

static int handle_status(enum blind_badge_ai_mode mode)
{
  const char *ai_prompt =
    "你是盲人辅助胸牌。设备状态正常，请生成一句简短的系统就绪提示。";

  printf("[BlindBadge] event: status\n");
  printf("[BlindBadge] status: running\n");
  printf("[BlindBadge] mode: qemu_event_simulation\n");
  printf("[BlindBadge] features: obstacle, step_down, emergency\n");
  printf("[BlindBadge] suggestion: 系统正在运行，可继续模拟障碍、台阶或求助事件。\n");
  printf("[BlindBadge] ai_prompt: %s\n", ai_prompt);

  return run_ai_mode(mode, ai_prompt);
}

static int handle_obstacle(int argc, char *argv[],
                           enum blind_badge_ai_mode mode)
{
  int distance_cm;
  const char *direction;
  char ai_prompt[384];
  const char *ai_action;
  int has_ai_flag = mode != BLIND_BADGE_AI_OFF;

  if (argc != (has_ai_flag ? 5 : 4))
    {
      printf("[BlindBadge] error: obstacle needs distance and direction.\n");
      print_help();
      return 1;
    }

  distance_cm = atoi(argv[2]);
  direction = argv[3];

  if (distance_cm <= 0)
    {
      printf("[BlindBadge] error: invalid distance_cm.\n");
      return 1;
    }

  if (!is_direction_valid(direction))
    {
      printf("[BlindBadge] error: invalid direction.\n");
      print_help();
      return 1;
    }

  printf("[BlindBadge] event: obstacle_near\n");
  printf("[BlindBadge] distance_cm: %d\n", distance_cm);
  printf("[BlindBadge] direction: %s\n", direction);

  if (distance_cm < 50)
    {
      ai_action = "请先停下确认";
      printf("[BlindBadge] suggestion: %s%d厘米有障碍，请立即停下确认。\n",
             direction_zh(direction), distance_cm);
    }
  else if (distance_cm < 100)
    {
      ai_action = "请减速并绕行";
      printf("[BlindBadge] suggestion: %s%d厘米有障碍，请减速并绕行。\n",
             direction_zh(direction), distance_cm);
    }
  else
    {
      ai_action = "请保持注意";
      printf("[BlindBadge] suggestion: %s检测到较远障碍，请保持注意。\n",
             direction_zh(direction));
    }

  snprintf(ai_prompt, sizeof(ai_prompt),
           "BlindBadge event=obstacle_near distance_cm=%d direction=%s。"
           "你是盲人辅助胸牌。检测到%s%d厘米有障碍，请生成一句简短安全提醒。"
           "必须包含动作：%s。只输出一句提醒，优先安全，不长篇解释，不承诺绝对安全。",
           distance_cm, direction, direction_zh(direction), distance_cm,
           ai_action);
  printf("[BlindBadge] ai_prompt: %s\n", ai_prompt);

  return run_ai_mode(mode, ai_prompt);
}

static int handle_step_down(int argc, char *argv[],
                            enum blind_badge_ai_mode mode)
{
  const char *direction;
  char ai_prompt[384];
  int has_ai_flag = mode != BLIND_BADGE_AI_OFF;

  if (argc != (has_ai_flag ? 4 : 3))
    {
      printf("[BlindBadge] error: step_down needs direction.\n");
      print_help();
      return 1;
    }

  direction = argv[2];

  if (!is_direction_valid(direction))
    {
      printf("[BlindBadge] error: invalid direction.\n");
      print_help();
      return 1;
    }

  printf("[BlindBadge] event: step_down\n");
  printf("[BlindBadge] direction: %s\n", direction);
  printf("[BlindBadge] suggestion: %s可能有下行台阶，请停一下，用手杖或脚尖确认。\n",
         direction_zh(direction));
  snprintf(ai_prompt, sizeof(ai_prompt),
           "BlindBadge event=step_down direction=%s。"
           "你是盲人辅助胸牌。检测到%s可能有下行台阶，请生成一句简短安全提醒。"
           "必须提醒停下或放慢并确认。只输出一句提醒，优先安全，不长篇解释，不承诺绝对安全。",
           direction, direction_zh(direction));
  printf("[BlindBadge] ai_prompt: %s\n", ai_prompt);

  return run_ai_mode(mode, ai_prompt);
}

static int handle_emergency(int argc, enum blind_badge_ai_mode mode)
{
  const char *ai_prompt =
    "BlindBadge event=emergency。"
    "你是盲人辅助胸牌。用户触发了求助按钮，请生成一句适合发给紧急联系人的求助信息。"
    "只输出一句简短求助信息，不长篇解释，不承诺绝对安全。";

  int has_ai_flag = mode != BLIND_BADGE_AI_OFF;

  if (argc != (has_ai_flag ? 3 : 2))
    {
      printf("[BlindBadge] error: emergency takes no arguments except --ai or --agent.\n");
      print_help();
      return 1;
    }

  printf("[BlindBadge] event: emergency\n");
  printf("[BlindBadge] suggestion: 已触发求助。建议发送：我需要帮助，请联系我或前往我的当前位置。\n");
  printf("[BlindBadge] ai_prompt: %s\n", ai_prompt);

  return run_ai_mode(mode, ai_prompt);
}

static int demo_run_obstacle(int distance_cm, const char *direction,
                             enum blind_badge_ai_mode mode)
{
  char distance_arg[16];
  char *argv_local[] =
    {
      "blind_badge_app",
      "obstacle",
      distance_arg,
      (char *)direction
    };
  char *argv_ai[] =
    {
      "blind_badge_app",
      "obstacle",
      distance_arg,
      (char *)direction,
      mode == BLIND_BADGE_AI_DIRECT ? "--ai" : "--agent"
    };

  snprintf(distance_arg, sizeof(distance_arg), "%d", distance_cm);
  return handle_obstacle(mode == BLIND_BADGE_AI_OFF ? 4 : 5,
                         mode == BLIND_BADGE_AI_OFF ? argv_local : argv_ai,
                         mode);
}

static int demo_run_step_down(const char *direction,
                              enum blind_badge_ai_mode mode)
{
  char *argv_local[] =
    {
      "blind_badge_app",
      "step_down",
      (char *)direction
    };
  char *argv_ai[] =
    {
      "blind_badge_app",
      "step_down",
      (char *)direction,
      mode == BLIND_BADGE_AI_DIRECT ? "--ai" : "--agent"
    };

  return handle_step_down(mode == BLIND_BADGE_AI_OFF ? 3 : 4,
                          mode == BLIND_BADGE_AI_OFF ? argv_local : argv_ai,
                          mode);
}

static int demo_run_emergency(enum blind_badge_ai_mode mode)
{
  return handle_emergency(mode == BLIND_BADGE_AI_OFF ? 2 : 3, mode);
}

static int handle_demo(int argc, enum blind_badge_ai_mode mode)
{
  int expected_argc = mode == BLIND_BADGE_AI_OFF ? 2 : 3;
  int ret = 0;

  if (argc != expected_argc)
    {
      printf("[BlindBadge] error: demo takes no arguments except --ai or --agent.\n");
      print_help();
      return 1;
    }

  printf("[BlindBadge] demo: proactive_hazard_alert\n");
  printf("[BlindBadge] demo_stage: obstacle approaching from 120cm to 40cm\n");

  if (demo_run_obstacle(120, "front", mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: obstacle warning threshold 80cm\n");
  if (demo_run_obstacle(80, "front", mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: urgent obstacle threshold 40cm\n");
  if (demo_run_obstacle(40, "front", mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: downward step detected\n");
  if (demo_run_step_down("front", mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: emergency help message\n");
  if (demo_run_emergency(mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo: completed\n");
  return ret;
}

int main(int argc, char *argv[])
{
  enum blind_badge_ai_mode mode = get_ai_mode(argc, argv);

  if (argc < 2)
    {
      printf("BlindBadge started.\n");
      print_help();
      return 0;
    }

  if (strcmp(argv[1], "help") == 0)
    {
      print_help();
      return 0;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      return handle_status(mode);
    }

  if (strcmp(argv[1], "obstacle") == 0)
    {
      return handle_obstacle(argc, argv, mode);
    }

  if (strcmp(argv[1], "step_down") == 0)
    {
      return handle_step_down(argc, argv, mode);
    }

  if (strcmp(argv[1], "emergency") == 0)
    {
      return handle_emergency(argc, mode);
    }

  if (strcmp(argv[1], "demo") == 0)
    {
      return handle_demo(argc, mode);
    }

  printf("[BlindBadge] error: unknown command: %s\n", argv[1]);
  print_help();

  return 1;
}
