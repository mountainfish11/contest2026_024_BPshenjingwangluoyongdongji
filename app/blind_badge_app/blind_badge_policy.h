/****************************************************************************
 * Contest 2026 team 024 - BlindBadge local safety policy
 ****************************************************************************/

#pragma once

#include "blind_badge_event.h"

#include <stddef.h>

void blind_badge_apply_policy(struct blind_badge_event *event);
void blind_badge_build_local_suggestion(const struct blind_badge_event *event,
                                        char *buf, size_t buf_size);
void blind_badge_build_ai_prompt(const struct blind_badge_event *event,
                                 char *buf, size_t buf_size);
void blind_badge_fallback_response(const char *local_suggestion,
                                   char *buf, size_t buf_size);
void blind_badge_normalize_ai_response(const char *input,
                                       const char *local_suggestion,
                                       char *buf, size_t buf_size);

