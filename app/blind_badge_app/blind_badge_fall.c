/****************************************************************************
 * Contest 2026 team 024 - BlindBadge fall detection
 *
 * Three-phase fall detection algorithm:
 *
 *   IDLE ──(|a| < free_fall_threshold)──▶ FREE_FALL
 *   FREE_FALL ──(|a| > impact_threshold)──▶ IMPACT
 *   IMPACT ──(|a| ≈ 1g for stillness_ms)──▶ STILLNESS → FIRE + COOLDOWN
 *
 * Any anomaly resets to IDLE.  The algorithm runs in a background
 * pthread so the main CLI stays responsive.
 ****************************************************************************/

#include "blind_badge_fall.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FALL_TASK_STACK_SIZE  16384
#define FALL_SLEEP_SLICE_MS   10

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct qma7981_dev g_sensor;
static struct fall_detect_config g_cfg;
static struct fall_detect_state g_state;
static fall_detect_cb_t g_callback;
static void *g_cb_arg;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_thread_created;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int elapsed_ms(struct timespec *from)
{
  struct timespec now;
  long ms;

  clock_gettime(CLOCK_MONOTONIC, &now);
  ms = (now.tv_sec - from->tv_sec) * 1000L +
       (now.tv_nsec - from->tv_nsec) / 1000000L;
  return (int)ms;
}

static void reset_phase(struct fall_detect_state *st)
{
  st->phase = FALL_PHASE_IDLE;
  st->phase_elapsed_ms = 0;
}

static void update_state(struct fall_detect_state *st,
                         float magnitude, int dt_ms)
{
  st->last_magnitude = magnitude;

  if (st->cooldown_remaining_ms > 0)
    {
      st->cooldown_remaining_ms -= dt_ms;
      if (st->cooldown_remaining_ms < 0)
        {
          st->cooldown_remaining_ms = 0;
        }

      return;
    }

  switch (st->phase)
    {
      case FALL_PHASE_IDLE:
        if (magnitude < g_cfg.free_fall_threshold)
          {
            st->phase = FALL_PHASE_FREE_FALL;
            st->phase_elapsed_ms = dt_ms;
          }
        break;

      case FALL_PHASE_FREE_FALL:
        if (magnitude < g_cfg.free_fall_threshold)
          {
            st->phase_elapsed_ms += dt_ms;
            if (st->phase_elapsed_ms >= g_cfg.free_fall_min_ms)
              {
                st->phase = FALL_PHASE_IMPACT;
                st->phase_elapsed_ms = 0;
              }
          }
        else
          {
            reset_phase(st);
          }
        break;

      case FALL_PHASE_IMPACT:
        if (magnitude > g_cfg.impact_threshold)
          {
            st->phase = FALL_PHASE_STILLNESS;
            st->phase_elapsed_ms = 0;
          }
        else
          {
            st->phase_elapsed_ms += dt_ms;
            if (st->phase_elapsed_ms > g_cfg.impact_window_ms)
              {
                reset_phase(st);
              }
          }
        break;

      case FALL_PHASE_STILLNESS:
        if (magnitude >= g_cfg.stillness_lo_g &&
            magnitude <= g_cfg.stillness_hi_g)
          {
            st->phase_elapsed_ms += dt_ms;
            if (st->phase_elapsed_ms >= g_cfg.stillness_ms)
              {
                /* Fall confirmed */

                st->fall_count++;
                st->cooldown_remaining_ms = g_cfg.cooldown_ms;
                reset_phase(st);

                printf("[FallDetect] FALL DETECTED "
                       "(magnitude=%.2fg, count=%d)\n",
                       magnitude, st->fall_count);

                if (g_callback != NULL)
                  {
                    g_callback(g_cb_arg);
                  }
              }
          }
        else if (magnitude > g_cfg.stillness_hi_g)
          {
            /* Still moving — not a quiet fall, reset */

            reset_phase(st);
          }
        else
          {
            /* Still below normal gravity — extend impact window */

            st->phase_elapsed_ms += dt_ms;
            if (st->phase_elapsed_ms > g_cfg.impact_window_ms * 2)
              {
                reset_phase(st);
              }
          }
        break;

      default:
        reset_phase(st);
        break;
    }
}

static int do_one_cycle(void)
{
  struct qma7981_sample sample;
  int ret;

  ret = qma7981_read_sample(&g_sensor, &sample);
  if (ret < 0)
    {
      return ret;
    }

  pthread_mutex_lock(&g_lock);
  update_state(&g_state, sample.magnitude, g_cfg.sample_interval_ms);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static void *fall_task(void *arg)
{
  struct timespec start;
  int dt_ms;

  (void)arg;
  clock_gettime(CLOCK_MONOTONIC, &start);

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
  cfg->free_fall_threshold   = 0.5f;   /* g */
  cfg->free_fall_min_ms      = 40;     /* 40 ms of free fall */
  cfg->impact_threshold      = 2.5f;   /* g */
  cfg->impact_window_ms      = 300;    /* impact within 300 ms */
  cfg->stillness_lo_g        = 0.8f;
  cfg->stillness_hi_g        = 1.2f;
  cfg->stillness_ms          = 500;    /* 500 ms of stillness */
  cfg->sample_interval_ms    = 50;     /* 20 Hz */
  cfg->cooldown_ms           = 10000;  /* 10 s cooldown */
}

int fall_detect_start(const char *i2c_path,
                      const struct fall_detect_config *cfg,
                      fall_detect_cb_t callback, void *cb_arg)
{
  pthread_attr_t attr;
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

  g_callback = callback;
  g_cb_arg   = cb_arg;

  memset(&g_state, 0, sizeof(g_state));
  g_state.running = 1;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, FALL_TASK_STACK_SIZE);
  ret = pthread_create(&g_thread, &attr, fall_task, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_state.running = 0;
      printf("[FallDetect] error: thread create failed (%d)\n", ret);
      return -ret;
    }

  g_thread_created = 1;
  printf("[FallDetect] started: free_fall<%.1fg %dms → impact>%.1fg "
         "→ stillness %.1f-%.1fg %dms, cooldown=%dms\n",
         g_cfg.free_fall_threshold, g_cfg.free_fall_min_ms,
         g_cfg.impact_threshold,
         g_cfg.stillness_lo_g, g_cfg.stillness_hi_g,
         g_cfg.stillness_ms, g_cfg.cooldown_ms);
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
  int before;
  int ret;

  ret = qma7981_read_sample(&g_sensor, &sample);
  if (ret < 0)
    {
      return -1;
    }

  pthread_mutex_lock(&g_lock);
  before = g_state.fall_count;
  update_state(&g_state, sample.magnitude, g_cfg.sample_interval_ms);
  pthread_mutex_unlock(&g_lock);

  return g_state.fall_count > before ? 1 : 0;
}
