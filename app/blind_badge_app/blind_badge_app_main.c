/****************************************************************************
 * Contest 2026 team 024 - blind badge app sample
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  printf("  blind_badge_app obstacle <distance_cm> <front|left|right>\n");
  printf("  blind_badge_app step_down <front|left|right>\n");
  printf("  blind_badge_app emergency\n");
}

static int handle_status(void)
{
  printf("[BlindBadge] event: status\n");
  printf("[BlindBadge] status: running\n");
  printf("[BlindBadge] mode: qemu_event_simulation\n");
  printf("[BlindBadge] features: obstacle, step_down, emergency\n");
  printf("[BlindBadge] suggestion: 系统正在运行，可继续模拟障碍、台阶或求助事件。\n");
  printf("[BlindBadge] ai_prompt: 你是盲人辅助胸牌。设备状态正常，请生成一句简短的系统就绪提示。\n");
  return 0;
}

static int handle_obstacle(int argc, char *argv[])
{
  int distance_cm;
  const char *direction;

  if (argc != 4)
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
      printf("[BlindBadge] suggestion: %s%d厘米有障碍，请立即停下确认。\n",
             direction_zh(direction), distance_cm);
    }
  else if (distance_cm < 100)
    {
      printf("[BlindBadge] suggestion: %s%d厘米有障碍，请减速并绕行。\n",
             direction_zh(direction), distance_cm);
    }
  else
    {
      printf("[BlindBadge] suggestion: %s检测到较远障碍，请保持注意。\n",
             direction_zh(direction));
    }

  printf("[BlindBadge] ai_prompt: 你是盲人辅助胸牌。检测到%s%d厘米有障碍，请生成一句简短安全提醒。\n",
         direction_zh(direction), distance_cm);

  return 0;
}

static int handle_step_down(int argc, char *argv[])
{
  const char *direction;

  if (argc != 3)
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
  printf("[BlindBadge] ai_prompt: 你是盲人辅助胸牌。检测到%s可能有下行台阶，请生成一句简短安全提醒。\n",
         direction_zh(direction));

  return 0;
}

static int handle_emergency(void)
{
  printf("[BlindBadge] event: emergency\n");
  printf("[BlindBadge] suggestion: 已触发求助。建议发送：我需要帮助，请联系我或前往我的当前位置。\n");
  printf("[BlindBadge] ai_prompt: 你是盲人辅助胸牌。用户触发了求助按钮，请生成一句适合发给紧急联系人的求助信息。\n");

  return 0;
}

int main(int argc, char *argv[])
{
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
      return handle_status();
    }

  if (strcmp(argv[1], "obstacle") == 0)
    {
      return handle_obstacle(argc, argv);
    }

  if (strcmp(argv[1], "step_down") == 0)
    {
      return handle_step_down(argc, argv);
    }

  if (strcmp(argv[1], "emergency") == 0)
    {
      return handle_emergency();
    }

  printf("[BlindBadge] error: unknown command: %s\n", argv[1]);
  print_help();

  return 1;
}
