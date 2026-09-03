/****************************************************************************
 * Contest 2026 team 024 - BlindBadge action simulation
 ****************************************************************************/

#include "blind_badge_action.h"

#include <stdio.h>

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
        break;

      case BLIND_BADGE_SEVERITY_WARN:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: short\n");
        break;

      case BLIND_BADGE_SEVERITY_URGENT:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: strong\n");
        break;

      case BLIND_BADGE_SEVERITY_EMERGENCY:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: strong\n");
        printf("[BlindBadge] action.emergency_send: triggered\n");
        break;

      default:
        printf("[BlindBadge] action.voice: %s\n", final_reminder);
        printf("[BlindBadge] action.vibration: short\n");
        break;
    }

  if (used_fallback)
    {
      printf("[BlindBadge] fallback: active\n");
    }
}

