/****************************************************************************
 * Contest 2026 team 024 - BlindBadge service layer
 *
 * The PLAY key enters the selected menu entry's status page; a double PLAY
 * leaves it and returns to the menu.  VISION owns the real camera HTTP
 * service; the remaining entries are connected incrementally.
 ****************************************************************************/

#ifndef BLIND_BADGE_SERVICE_H
#define BLIND_BADGE_SERVICE_H

#include "blind_badge_menu.h"

#include <stddef.h>

enum blind_badge_service_state
{
  BLIND_BADGE_SERVICE_IDLE = 0,
  BLIND_BADGE_SERVICE_STARTING,
  BLIND_BADGE_SERVICE_RUNNING,
  BLIND_BADGE_SERVICE_STOPPING,
  BLIND_BADGE_SERVICE_ERROR,
  BLIND_BADGE_SERVICE_UNAVAILABLE
};

/* Enter the selected service state. */

int blind_badge_service_start(enum blind_badge_menu_id id);

/* Leave the selected service state and stop its owned task. */

int blind_badge_service_stop(void);

/* Deliver one recognized utterance from the sub-board to the active VOICE
 * service.  Text received outside that service is deliberately ignored. */

int blind_badge_service_submit_asr(const char *text);

/* Complete an asynchronous VOICE stop after the sub-board confirms that its
 * capture/ASR resources are idle. */

void blind_badge_service_voice_idle(void);

/* Complete HEALTH after the sub-board has spoken the measured result. */

int blind_badge_service_health_done(int heart_rate_bpm);

/* Effective state, derived from the service task itself. */

enum blind_badge_service_state blind_badge_service_state(void);

const char *blind_badge_service_state_name(enum blind_badge_service_state s);

/* Menu entry that owns the current service. */

enum blind_badge_menu_id blind_badge_service_active(void);

/* Short uppercase label for a menu entry. */

const char *blind_badge_service_label(enum blind_badge_menu_id id);

/* One line of live status for the service page. */

int blind_badge_service_detail(char *buf, size_t size);

#endif /* BLIND_BADGE_SERVICE_H */
