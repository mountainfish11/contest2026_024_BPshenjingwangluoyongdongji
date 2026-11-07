/****************************************************************************
 * Contest 2026 team 024 - BlindBadge fall detection
 *
 * Detects a rapid acceleration toward the bottom edge of the LCD using the
 * QMA7981 accelerometer.  On ESP32-S3-EYE this direction was measured as +X.
 * The callback posts an emergency preemption request to the main app.
 ****************************************************************************/

#ifndef __BLIND_BADGE_FALL_H
#define __BLIND_BADGE_FALL_H

#include "qma7981.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void (*fall_detect_cb_t)(void *arg);

struct fall_detect_config
{
  /* Screen-down phase: X rises this far above its resting baseline (in g) */

  float downward_delta_threshold;
  int downward_min_ms;

  /* Reserved for a future impact-confirmation stage. */

  float impact_threshold;
  int impact_window_ms;

  /* Stillness phase: magnitude stays near 1g for this long */

  float stillness_lo_g;
  float stillness_hi_g;
  int stillness_ms;

  /* Sampling interval in milliseconds (50 ms = 20 Hz) */

  int sample_interval_ms;

  /* Cooldown between triggers (ms) to avoid repeated alarms */

  int cooldown_ms;
};

struct fall_detect_state
{
  enum
  {
    FALL_PHASE_IDLE = 0,
    FALL_PHASE_DOWNWARD,
    FALL_PHASE_IMPACT,
    FALL_PHASE_STILLNESS,
  } phase;

  int phase_elapsed_ms;
  int cooldown_remaining_ms;
  int fall_count;       /* total falls detected since start */
  int running;          /* 1 = task active */
  float last_magnitude;
  float last_x;
  float last_downward_delta;
  float baseline_x;
  float trigger_delta;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * Get the default configuration tuned for a chest-worn badge.
 */
void fall_detect_default_config(struct fall_detect_config *cfg);

/**
 * Start the fall detection task.
 *
 * @param i2c_path  I2C device node, e.g. "/dev/i2c0".
 * @param cfg       Configuration (NULL = defaults).
 * @param callback  Called when a fall is detected (may be NULL).
 * @param cb_arg    Passed to callback.
 * @return 0 on success, negative errno on failure.
 */
int fall_detect_start(const char *i2c_path,
                      const struct fall_detect_config *cfg,
                      fall_detect_cb_t callback, void *cb_arg);

/**
 * Stop the fall detection task and release the sensor.
 */
int fall_detect_stop(void);

/**
 * Query current state (non-blocking).
 */
void fall_detect_get_state(struct fall_detect_state *out);

/**
 * Run one single-shot read + state update (for CLI test without a task).
 * Returns 1 if a fall was detected in this call, 0 otherwise.
 */
int fall_detect_poll_once(void);

#endif /* __BLIND_BADGE_FALL_H */
