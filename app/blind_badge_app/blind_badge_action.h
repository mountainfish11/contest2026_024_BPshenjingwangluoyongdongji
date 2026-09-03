/****************************************************************************
 * Contest 2026 team 024 - BlindBadge action simulation
 ****************************************************************************/

#pragma once

#include "blind_badge_event.h"

void blind_badge_print_actions(const struct blind_badge_event *event,
                               const char *final_reminder,
                               int used_fallback);

