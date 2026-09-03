/****************************************************************************
 * Contest 2026 team 024 - BlindBadge event model
 ****************************************************************************/

#include "blind_badge_event.h"

#include <string.h>

int blind_badge_parse_direction(const char *text,
                                enum blind_badge_direction *direction)
{
  if (text == NULL || direction == NULL)
    {
      return -1;
    }

  if (strcmp(text, "front") == 0)
    {
      *direction = BLIND_BADGE_DIR_FRONT;
      return 0;
    }

  if (strcmp(text, "left") == 0)
    {
      *direction = BLIND_BADGE_DIR_LEFT;
      return 0;
    }

  if (strcmp(text, "right") == 0)
    {
      *direction = BLIND_BADGE_DIR_RIGHT;
      return 0;
    }

  return -1;
}

const char *blind_badge_direction_name(enum blind_badge_direction direction)
{
  switch (direction)
    {
      case BLIND_BADGE_DIR_FRONT:
        return "front";
      case BLIND_BADGE_DIR_LEFT:
        return "left";
      case BLIND_BADGE_DIR_RIGHT:
        return "right";
      default:
        return "unknown";
    }
}

const char *blind_badge_direction_zh(enum blind_badge_direction direction)
{
  switch (direction)
    {
      case BLIND_BADGE_DIR_FRONT:
        return "前方";
      case BLIND_BADGE_DIR_LEFT:
        return "左侧";
      case BLIND_BADGE_DIR_RIGHT:
        return "右侧";
      default:
        return "未知方向";
    }
}

const char *blind_badge_event_name(enum blind_badge_event_type type)
{
  switch (type)
    {
      case BLIND_BADGE_EVENT_STATUS:
        return "status";
      case BLIND_BADGE_EVENT_OBSTACLE:
        return "obstacle_near";
      case BLIND_BADGE_EVENT_STEP_DOWN:
        return "step_down";
      case BLIND_BADGE_EVENT_EMERGENCY:
        return "emergency";
      default:
        return "unknown";
    }
}

const char *blind_badge_severity_name(enum blind_badge_severity severity)
{
  switch (severity)
    {
      case BLIND_BADGE_SEVERITY_INFO:
        return "info";
      case BLIND_BADGE_SEVERITY_WARN:
        return "warn";
      case BLIND_BADGE_SEVERITY_URGENT:
        return "urgent";
      case BLIND_BADGE_SEVERITY_EMERGENCY:
        return "emergency";
      default:
        return "unknown";
    }
}

