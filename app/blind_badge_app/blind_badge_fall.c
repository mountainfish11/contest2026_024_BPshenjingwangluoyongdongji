/****************************************************************************
 * Contest 2026 team 024 - BlindBadge fall detection
 *
 * Passive emergency trigger.  It calibrates the resting QMA7981 X axis at
 * startup, then detects a short, strong acceleration toward the bottom edge
 * of the LCD.  Hardware measurements map that direction to sensor +X.
 * The callback only posts a request; service teardown and ALERT startup run
 * in the menu/button thread.
 ****************************************************************************/

#include "blind_badge_fall.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FALL_TASK_STACK_SIZE  16384
#define FALL_SLEEP_SLICE_MS   10
#define FALL_CALIBRATION_SAMPLES 20
#define FALL_CALIBRATION_INTERVAL_US 20000
#define FALL_DOWNWARD_DELTA_G 0.90f

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct qma7981_dev g_sensor;
static struct fall_detect_config g_cfg;
static struct fall_detect_state g_state;
static fall_detect_cb_t g_callback;
static void *g_cb_arg;
static float g_baseline_x;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_thread_created;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void reset_phase(struct fall_detect_state *st)
{
  st->phase = FALL_PHASE_IDLE;
  st->phase_elapsed_ms = 0;
}

static bool update_state(struct fall_detect_state *st,
                         float downward_delta, int dt_ms)
{
  if (st->cooldown_remaining_ms > 0)
    {
      st->cooldown_remaining_ms -= dt_ms;
      if (st->cooldown_remaining_ms < 0)
        {
          st->cooldown_remaining_ms = 0;
        }

      return false;
    }

  switch (st->phase)
    {
      case FALL_PHASE_IDLE:
        if (downward_delta >= g_cfg.downward_delta_threshold)
          {
            st->phase = FALL_PHASE_DOWNWARD;
            st->phase_elapsed_ms = dt_ms;
            if (st->phase_elapsed_ms >= g_cfg.downward_min_ms)
              {
                st->fall_count++;
                st->cooldown_remaining_ms = g_cfg.cooldown_ms;
                reset_phase(st);
                return true;
              }
          }
        break;

      case FALL_PHASE_DOWNWARD:
        if (downward_delta >= g_cfg.downward_delta_threshold)
          {
            st->phase_elapsed_ms += dt_ms;
            if (st->phase_elapsed_ms >= g_cfg.downward_min_ms)
              {
                st->fall_count++;
                st->cooldown_remaining_ms = g_cfg.cooldown_ms;
                reset_phase(st);
                return true;
              }
          }
        else
          {
            reset_phase(st);
          }
        break;

      default:
        reset_phase(st);
        break;
    }

  return false;
}

static int do_one_cycle(void)
{
  struct qma7981_sample sample;
  float downward_delta;
  bool triggered;
  int ret;

  ret = qma7981_read_sample(&g_sensor, &sample);
  if (ret < 0)
    {
      return ret;
    }

  /* Screen bottom is sensor +X.  A positive delta therefore means the
   * current X sample rose above its resting baseline.  The later -X spike
   * seen in a hand swing is braking at the bottom and must not trigger.
   */

  downward_delta = sample.x - g_baseline_x;

  pthread_mutex_lock(&g_lock);
  g_state.last_magnitude = sample.magnitude;
  g_state.last_x = sample.x;
  g_state.last_downward_delta = downward_delta;
  triggered = update_state(&g_state, downward_delta,
                           g_cfg.sample_interval_ms);
  pthread_mutex_unlock(&g_lock);

  if (triggered)
    {
      printf("[FallDetect] RAPID DROP DETECTED "
             "(x=%.2fg, baseline=%.2fg, delta=%.2fg, threshold=%.2fg)\n",
             (double)sample.x, (double)g_baseline_x,
             (double)downward_delta,
             (double)g_cfg.downward_delta_threshold);
      if (g_callback != NULL)
        {
          g_callback(g_cb_arg);
        }
    }

  return 0;
}

static void *fall_task(void *arg)
{
  int dt_ms;

  (void)arg;

  while (g_state.running)
    {
      if (do_one_cycle() < 0)
        {
          printf("[FallDetect] warning: sensor read failed\n");
        }

      /* Sleep in small slices so stop() is responsive */

      dt_ms = 0;
      while (dt_ms < g_cfg.sample_interval_ms && g_state.running)
        {
          usleep(FALL_SLEEP_SLICE_MS * 1000);
          dt_ms += FALL_SLEEP_SLICE_MS;
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void fall_detect_default_config(struct fall_detect_config *cfg)
{
  cfg->downward_delta_threshold = FALL_DOWNWARD_DELTA_G;
  cfg->downward_min_ms       = 20;     /* one 50 Hz sample */
  cfg->impact_threshold      = 2.5f;   /* g */
  cfg->impact_window_ms      = 300;    /* impact within 300 ms */
  cfg->stillness_lo_g        = 0.8f;
  cfg->stillness_hi_g        = 1.2f;
  cfg->stillness_ms          = 500;    /* 500 ms of stillness */
  cfg->sample_interval_ms    = 20;     /* 50 Hz */
  cfg->cooldown_ms           = 15000;  /* suppress repeated alerts */
}

int fall_detect_start(const char *i2c_path,
                      const struct fall_detect_config *cfg,
                      fall_detect_cb_t callback, void *cb_arg)
{
  pthread_attr_t attr;
  struct qma7981_sample sample;
  float baseline_x = 0.0f;
  int valid_samples = 0;
  int i;
  int ret;

  if (g_state.running)
    {
      printf("[FallDetect] already running\n");
      return -EBUSY;
    }

  if (cfg != NULL)
    {
      g_cfg = *cfg;
    }
  else
    {
      fall_detect_default_config(&g_cfg);
    }

  ret = qma7981_init(&g_sensor, i2c_path);
  if (ret < 0)
    {
      printf("[FallDetect] error: sensor init failed (%d)\n", ret);
      return ret;
    }

  for (i = 0; i < FALL_CALIBRATION_SAMPLES; i++)
    {
      ret = qma7981_read_sample(&g_sensor, &sample);
      if (ret == 0 && isfinite(sample.x) &&
          sample.x > -8.0f && sample.x < 8.0f)
        {
          baseline_x += sample.x;
          valid_samples++;
        }

      usleep(FALL_CALIBRATION_INTERVAL_US);
    }

  if (valid_samples < FALL_CALIBRATION_SAMPLES / 2)
    {
      qma7981_set_standby(&g_sensor);
      qma7981_close(&g_sensor);
      printf("[FallDetect] error: X-axis baseline unavailable\n");
      return -ENODATA;
    }

  baseline_x /= valid_samples;

  g_baseline_x = baseline_x;

  g_callback = callback;
  g_cb_arg   = cb_arg;

  memset(&g_state, 0, sizeof(g_state));
  g_state.running = 1;
  g_state.baseline_x = baseline_x;
  g_state.trigger_delta = g_cfg.downward_delta_threshold;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, FALL_TASK_STACK_SIZE);
  ret = pthread_create(&g_thread, &attr, fall_task, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_state.running = 0;
      qma7981_set_standby(&g_sensor);
      qma7981_close(&g_sensor);
      printf("[FallDetect] error: thread create failed (%d)\n", ret);
      return -ret;
    }

  g_thread_created = 1;
  printf("[FallDetect] armed: screen-down=+X baseline=%.3fg "
         "delta>=%.3fg for %dms, "
         "cooldown=%dms\n",
         (double)baseline_x, (double)g_cfg.downward_delta_threshold,
         g_cfg.downward_min_ms, g_cfg.cooldown_ms);
  return 0;
}

int fall_detect_stop(void)
{
  if (!g_state.running)
    {
      return -EAGAIN;
    }

  g_state.running = 0;

  if (g_thread_created)
    {
      pthread_join(g_thread, NULL);
      g_thread_created = 0;
    }

  qma7981_set_standby(&g_sensor);
  qma7981_close(&g_sensor);
  printf("[FallDetect] stopped (total falls: %d)\n", g_state.fall_count);
  return 0;
}

void fall_detect_get_state(struct fall_detect_state *out)
{
  if (out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  *out = g_state;
  pthread_mutex_unlock(&g_lock);
}

int fall_detect_poll_once(void)
{
  struct qma7981_sample sample;
  float downward_delta;
  int before;
  int ret;

  ret = qma7981_read_sample(&g_sensor, &sample);
  if (ret < 0)
    {
      return -1;
    }

  downward_delta = sample.x - g_baseline_x;

  pthread_mutex_lock(&g_lock);
  before = g_state.fall_count;
  g_state.last_magnitude = sample.magnitude;
  g_state.last_x = sample.x;
  g_state.last_downward_delta = downward_delta;
  update_state(&g_state, downward_delta, g_cfg.sample_interval_ms);
  pthread_mutex_unlock(&g_lock);

  return g_state.fall_count > before ? 1 : 0;
}
