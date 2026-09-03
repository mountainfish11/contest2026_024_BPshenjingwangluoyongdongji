/****************************************************************************
 * Contest 2026 team 024 - BlindBadge event model
 ****************************************************************************/

#pragma once

#include <stddef.h>

enum blind_badge_event_type
{
  BLIND_BADGE_EVENT_STATUS = 0,
  BLIND_BADGE_EVENT_OBSTACLE,
  BLIND_BADGE_EVENT_STEP_DOWN,
  BLIND_BADGE_EVENT_EMERGENCY
};

enum blind_badge_direction
{
  BLIND_BADGE_DIR_FRONT = 0,
  BLIND_BADGE_DIR_LEFT,
  BLIND_BADGE_DIR_RIGHT,
  BLIND_BADGE_DIR_UNKNOWN
};

enum blind_badge_severity
{
  BLIND_BADGE_SEVERITY_INFO = 0,
  BLIND_BADGE_SEVERITY_WARN,
  BLIND_BADGE_SEVERITY_URGENT,
  BLIND_BADGE_SEVERITY_EMERGENCY
};

struct blind_badge_event
{
  enum blind_badge_event_type type;
  enum blind_badge_direction direction;
  enum blind_badge_severity severity;
  int distance_cm;
};

int blind_badge_parse_direction(const char *text,
                                enum blind_badge_direction *direction);
const char *blind_badge_direction_name(enum blind_badge_direction direction);
const char *blind_badge_direction_zh(enum blind_badge_direction direction);
const char *blind_badge_event_name(enum blind_badge_event_type type);
const char *blind_badge_severity_name(enum blind_badge_severity severity);

