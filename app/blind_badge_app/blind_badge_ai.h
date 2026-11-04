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

int blind_badge_ai_import_mimo(const char *path);
int blind_badge_ai_ready(void);
void blind_badge_ai_begin_session(void);
void blind_badge_ai_cancel_active(void);
void blind_badge_ai_reset_aborted(void);

