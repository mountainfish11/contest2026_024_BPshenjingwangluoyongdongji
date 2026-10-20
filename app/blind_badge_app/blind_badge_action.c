/****************************************************************************
 * Contest 2026 team 024 - BlindBadge action simulation
 ****************************************************************************/

#include "blind_badge_action.h"
#include "uart_link.h"

#include <stdio.h>

static void send_sub_board_flag(const char *flag)
{
  if (UART_LINK_SendFlag(flag) == 0)
    {
      printf("[BlindBadge] action.sub_board: %s\n", flag);
    }
}

void blind_badge_print_actions(const struct blind_badge_event *event,
                               const char *final_reminder,
                               int used_fallback)
{
  if (event == NULL)
    {
      return;
    }

  printf("[BlindBadge] severity: %s\n",
         blind_badge_severity_name(event->severity));

  switch (event->severity)
    {
      case BLIND_BADGE_SEVERITY_INFO:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: off\n");
        send_sub_board_flag("VIBRATION_OFF");
        break;

      case BLIND_BADGE_SEVERITY_WARN:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: short\n");
        send_sub_board_flag("VIBRATION_SHORT");
        break;

      case BLIND_BADGE_SEVERITY_URGENT:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: strong\n");
        send_sub_board_flag("VIBRATION_STRONG");
        break;

      case BLIND_BADGE_SEVERITY_EMERGENCY:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: strong\n");
        printf("[BlindBadge] action.emergency_send: triggered\n");
        send_sub_board_flag("VIBRATION_STRONG");
        send_sub_board_flag("EMERGENCY_SEND");
        break;

      default:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: short\n");
        send_sub_board_flag("VIBRATION_SHORT");
        break;
    }

  if (used_fallback)
    {
      printf("[BlindBadge] fallback: active\n");
    }
}
