/****************************************************************************
 * Contest 2026 team 024 - BlindBadge main-board state machine
 ****************************************************************************/

#ifndef BLIND_BADGE_STATE_H
#define BLIND_BADGE_STATE_H

enum blind_badge_state
{
  BLIND_BADGE_STATE_IDLE = 0,
  BLIND_BADGE_STATE_LISTENING,
  BLIND_BADGE_STATE_RECORDING,
  BLIND_BADGE_STATE_CAPTURING,
  BLIND_BADGE_STATE_PROCESSING,
  BLIND_BADGE_STATE_RESULT,
  BLIND_BADGE_STATE_OFFLINE,
  BLIND_BADGE_STATE_ERROR,
  BLIND_BADGE_STATE_EMERGENCY
};

enum blind_badge_state blind_badge_state_get(void);
const char *blind_badge_state_name(enum blind_badge_state state);
int blind_badge_state_parse(const char *name, enum blind_badge_state *state);
int blind_badge_state_set(enum blind_badge_state state, const char *detail);
int blind_badge_state_show(void);

#endif /* BLIND_BADGE_STATE_H */
