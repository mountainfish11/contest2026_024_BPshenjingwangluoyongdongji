/****************************************************************************
 * Contest 2026 team 024 - BlindBadge app
 ****************************************************************************/

#include "blind_badge_action.h"
#include "blind_badge_ai.h"
#include "blind_badge_event.h"
#include "blind_badge_policy.h"
#include "blind_badge_skill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLIND_BADGE_TEXT_SIZE 512

static void print_help(void)
{
  printf("BlindBadge usage:\n");
  printf("  blind_badge_app help\n");
  printf("  blind_badge_app status\n");
  printf("  blind_badge_app obstacle <distance_cm> <front|left|right> [--ai|--agent]\n");
  printf("  blind_badge_app step_down <front|left|right> [--ai|--agent]\n");
  printf("  blind_badge_app emergency [--ai|--agent]\n");
  printf("  blind_badge_app demo [--ai|--agent]\n");
  printf("  blind_badge_app install_skill\n");
  printf("  --ai: call ai_agent LLM router directly and print ai_response\n");
  printf("  --agent: send ai_prompt to running ai_agent and wait for ai_response\n");
}

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

static void print_event_header(const struct blind_badge_event *event,
                               const char *suggestion,
                               const char *ai_prompt)
{
  printf("[BlindBadge] event: %s\n", blind_badge_event_name(event->type));

  if (event->type == BLIND_BADGE_EVENT_STATUS)
    {
      printf("[BlindBadge] status: running\n");
      printf("[BlindBadge] mode: qemu_event_simulation\n");
      printf("[BlindBadge] features: obstacle, step_down, emergency, demo\n");
    }

  if (event->type == BLIND_BADGE_EVENT_OBSTACLE)
    {
      printf("[BlindBadge] distance_cm: %d\n", event->distance_cm);
    }

  if (event->type == BLIND_BADGE_EVENT_OBSTACLE ||
      event->type == BLIND_BADGE_EVENT_STEP_DOWN)
    {
      printf("[BlindBadge] direction: %s\n",
             blind_badge_direction_name(event->direction));
    }

  printf("[BlindBadge] suggestion: %s\n", suggestion);
  printf("[BlindBadge] ai_prompt: %s\n", ai_prompt);
}

static void print_event_ai_result(const struct blind_badge_event *event,
                                  const char *ai_response,
                                  int used_fallback)
{
  if (ai_response != NULL && ai_response[0] != '\0')
    {
      printf("[BlindBadge] ai_response: %s\n", ai_response);
      blind_badge_print_actions(event, ai_response, used_fallback);
    }
}

static int run_event(struct blind_badge_event *event,
                     enum blind_badge_ai_mode mode)
{
  char suggestion[BLIND_BADGE_TEXT_SIZE];
  char ai_prompt[BLIND_BADGE_TEXT_SIZE];
  char ai_response[BLIND_BADGE_TEXT_SIZE];
  int used_fallback = 0;

  blind_badge_apply_policy(event);
  blind_badge_build_local_suggestion(event, suggestion, sizeof(suggestion));
  blind_badge_build_ai_prompt(event, ai_prompt, sizeof(ai_prompt));
  print_event_header(event, suggestion, ai_prompt);

  ai_response[0] = '\0';
  if (mode != BLIND_BADGE_AI_OFF)
    {
      blind_badge_ask_ai(mode, ai_prompt, suggestion, ai_response,
                         sizeof(ai_response), &used_fallback);
      print_event_ai_result(event, ai_response, used_fallback);
    }
  return 0;
}

static int handle_status(int argc, enum blind_badge_ai_mode mode)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_STATUS,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };

  if (argc != (mode == BLIND_BADGE_AI_OFF ? 2 : 3))
    {
      printf("[BlindBadge] error: status takes no arguments except --ai or --agent.\n");
      print_help();
      return 1;
    }

  return run_event(&event, mode);
}

static int handle_obstacle(int argc, char *argv[],
                           enum blind_badge_ai_mode mode)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_OBSTACLE,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };
  int expected_argc = mode == BLIND_BADGE_AI_OFF ? 4 : 5;

  if (argc != expected_argc)
    {
      printf("[BlindBadge] error: obstacle needs distance and direction.\n");
      print_help();
      return 1;
    }

  event.distance_cm = atoi(argv[2]);
  if (event.distance_cm <= 0)
    {
      printf("[BlindBadge] error: invalid distance_cm.\n");
      return 1;
    }

  if (blind_badge_parse_direction(argv[3], &event.direction) != 0)
    {
      printf("[BlindBadge] error: invalid direction.\n");
      print_help();
      return 1;
    }

  return run_event(&event, mode);
}

static int handle_step_down(int argc, char *argv[],
                            enum blind_badge_ai_mode mode)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_STEP_DOWN,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };
  int expected_argc = mode == BLIND_BADGE_AI_OFF ? 3 : 4;

  if (argc != expected_argc)
    {
      printf("[BlindBadge] error: step_down needs direction.\n");
      print_help();
      return 1;
    }

  if (blind_badge_parse_direction(argv[2], &event.direction) != 0)
    {
      printf("[BlindBadge] error: invalid direction.\n");
      print_help();
      return 1;
    }

  return run_event(&event, mode);
}

static int handle_emergency(int argc, enum blind_badge_ai_mode mode)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_EMERGENCY,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_EMERGENCY,
      .distance_cm = 0
    };

  if (argc != (mode == BLIND_BADGE_AI_OFF ? 2 : 3))
    {
      printf("[BlindBadge] error: emergency takes no arguments except --ai or --agent.\n");
      print_help();
      return 1;
    }

  return run_event(&event, mode);
}

static int demo_run_obstacle(int distance_cm,
                             enum blind_badge_ai_mode mode)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_OBSTACLE,
      .direction = BLIND_BADGE_DIR_FRONT,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = distance_cm
    };

  return run_event(&event, mode);
}

static int demo_run_step_down(enum blind_badge_ai_mode mode)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_STEP_DOWN,
      .direction = BLIND_BADGE_DIR_FRONT,
      .severity = BLIND_BADGE_SEVERITY_INFO,
      .distance_cm = 0
    };

  return run_event(&event, mode);
}

static int demo_run_emergency(enum blind_badge_ai_mode mode)
{
  struct blind_badge_event event =
    {
      .type = BLIND_BADGE_EVENT_EMERGENCY,
      .direction = BLIND_BADGE_DIR_UNKNOWN,
      .severity = BLIND_BADGE_SEVERITY_EMERGENCY,
      .distance_cm = 0
    };

  return run_event(&event, mode);
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

  if (demo_run_obstacle(120, mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: obstacle warning threshold 80cm\n");
  if (demo_run_obstacle(80, mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: urgent obstacle threshold 40cm\n");
  if (demo_run_obstacle(40, mode) != 0)
    {
      ret = 1;
    }

  printf("[BlindBadge] demo_stage: downward step detected\n");
  if (demo_run_step_down(mode) != 0)
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
      return handle_status(argc, mode);
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

  if (strcmp(argv[1], "install_skill") == 0)
    {
      if (argc != 2)
        {
          printf("[BlindBadge] error: install_skill takes no arguments.\n");
          print_help();
          return 1;
        }

      return blind_badge_install_skill();
    }

  printf("[BlindBadge] error: unknown command: %s\n", argv[1]);
  print_help();
  return 1;
}
