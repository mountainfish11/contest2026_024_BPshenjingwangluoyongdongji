/****************************************************************************
 * Contest 2026 team 024 - BlindBadge main-board state machine
 ****************************************************************************/

#include "blind_badge_state.h"
#include "blind_badge_display.h"

#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static enum blind_badge_state g_state = BLIND_BADGE_STATE_IDLE;
static unsigned long g_state_sequence;

const char *blind_badge_state_name(enum blind_badge_state state)
{
  switch (state)
    {
      case BLIND_BADGE_STATE_IDLE:
        return "IDLE";
      case BLIND_BADGE_STATE_LISTENING:
        return "LISTENING";
      case BLIND_BADGE_STATE_RECORDING:
        return "RECORDING";
      case BLIND_BADGE_STATE_CAPTURING:
        return "CAPTURING";
      case BLIND_BADGE_STATE_PROCESSING:
        return "PROCESSING";
      case BLIND_BADGE_STATE_RESULT:
        return "RESULT";
      case BLIND_BADGE_STATE_OFFLINE:
        return "OFFLINE";
      case BLIND_BADGE_STATE_ERROR:
        return "ERROR";
      case BLIND_BADGE_STATE_EMERGENCY:
        return "EMERGENCY";
      default:
        return "UNKNOWN";
    }
}

enum blind_badge_state blind_badge_state_get(void)
{
  enum blind_badge_state state;

  pthread_mutex_lock(&g_state_lock);
  state = g_state;
  pthread_mutex_unlock(&g_state_lock);
  return state;
}

int blind_badge_state_parse(const char *name, enum blind_badge_state *state)
{
  enum blind_badge_state candidate;

  if (name == NULL || state == NULL)
    {
      return -EINVAL;
    }

  for (candidate = BLIND_BADGE_STATE_IDLE;
       candidate <= BLIND_BADGE_STATE_EMERGENCY;
       candidate++)
    {
      if (strcasecmp(name, blind_badge_state_name(candidate)) == 0)
        {
          *state = candidate;
          return 0;
        }
    }

  return -EINVAL;
}

static int blind_badge_state_set_internal(enum blind_badge_state state,
                                          const char *detail,
                                          bool log_transition)
{
  enum blind_badge_state previous;
  unsigned long sequence;
  const char *safe_detail = detail != NULL ? detail : "";

  if (state < BLIND_BADGE_STATE_IDLE ||
      state > BLIND_BADGE_STATE_EMERGENCY)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_state_lock);
  previous = g_state;
  g_state = state;
  sequence = ++g_state_sequence;
  pthread_mutex_unlock(&g_state_lock);

  if (log_transition)
    {
      printf("[BlindBadge] state: #%lu %s -> %s detail=%s\n",
             sequence, blind_badge_state_name(previous),
             blind_badge_state_name(state),
             safe_detail[0] != '\0' ? safe_detail : "none");
    }

  return blind_badge_display_show_lines("MAIN STATE",
                                        blind_badge_state_name(state),
                                        safe_detail, "");
}

int blind_badge_state_set(enum blind_badge_state state, const char *detail)
{
  return blind_badge_state_set_internal(state, detail, true);
}

int blind_badge_state_set_quiet(enum blind_badge_state state,
                                const char *detail)
{
  return blind_badge_state_set_internal(state, detail, false);
}

int blind_badge_state_show(void)
{
  enum blind_badge_state state = blind_badge_state_get();

  printf("[BlindBadge] state.current: %s\n",
         blind_badge_state_name(state));
  return blind_badge_display_show_lines("MAIN STATE",
                                        blind_badge_state_name(state),
                                        "", "");
}
