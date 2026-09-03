/****************************************************************************
 * Contest 2026 team 024 - BlindBadge AI adapter
 ****************************************************************************/

#pragma once

#include <stddef.h>

enum blind_badge_ai_mode
{
  BLIND_BADGE_AI_OFF = 0,
  BLIND_BADGE_AI_DIRECT,
  BLIND_BADGE_AI_AGENT
};

int blind_badge_ask_ai(enum blind_badge_ai_mode mode,
                       const char *ai_prompt,
                       const char *local_suggestion,
                       char *response, size_t response_size,
                       int *used_fallback);

