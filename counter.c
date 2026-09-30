/**
 * @file counter.c
 * @brief Persistent energy counters.
 *
 * Source values arrive via dispatcher callbacks (from any input thread).
 * A dedicated thread advances power integration counters every
 * COUNTER_TICK_MS and saves changed states every COUNTER_SAVE_INTERVAL_MS.
 *
 * Counter values are dispatched while holding the counter lock, so the
 * published values are strictly ordered.  This is deadlock free, because
 * the source of a counter must not be another counter.
 */
#include "counter.h"
#include "utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <syslog.h>
#include <sys/stat.h>

#define COUNTER_THREAD_PERIOD_US 100000
#define COUNTER_TICK_MS 1000
#define COUNTER_SAVE_INTERVAL_MS 300000
#define COUNTER_SUBDIR "counters"

static int counters_count;
static COUNTER_T *counters;

static char *counter_dir;
static pthread_t thread;
static bool thread_running;

static int counter_configure_one(cfg_t *cfg, void *ctx, void *child);
static bool valid_name(const char *name);
static COUNTER_T *find_counter(const char *name);
static int source_update(void *v, double f);
static bool device_update(COUNTER_T *c, double raw, int64_t now);
static void power_update(COUNTER_T *c, double power, int64_t now);
static void integrate(COUNTER_T *c, int64_t now);
static bool power_fresh(COUNTER_T *c, int64_t now);
static void *counter_thread(void *ptr);
static char *state_path(const COUNTER_T *c, const char *suffix);
static void load_state(COUNTER_T *c);
static void save_states(void);
static int save_state(COUNTER_T *c, double accum, double last_read);

void counter_init(void) {
  counters_count = 0;
  counters = NULL;
  counter_dir = NULL;
  thread_running = false;
}

int counter_configure(cfg_t *cfg) {
  COUNTER_T *c;
  int idx;

  if (uvrgw_conf_config_childs(cfg, "counter", &counters_count, (void **) &counters, sizeof(COUNTER_T), NULL, counter_configure_one) < 0) {
    return -1;
  }

  // chained counters are not supported (see file comment)
  for (c = counters, idx = 0; idx < counters_count; c++, idx++) {
    if (find_counter(c->source) != NULL) {
      syslog(LOG_ERR, "counter '%s': source '%s' must not be another counter.", c->name, c->source);
      return -1;
    }
  }

  return 0;
}

static int counter_configure_one(cfg_t *cfg, void *ctx, void *child) {
  COUNTER_T *c = (COUNTER_T *) child;

  c->name = uvrgw_conf_strdup(cfg_title(cfg));
  c->source = uvrgw_conf_strdup(cfg_getstr(cfg, "source"));
  c->integrate_power = cfg_getbool(cfg, "integrate_power");
  c->max_power = cfg_getfloat(cfg, "max_power");
  c->max_gap = cfg_getint(cfg, "max_gap");
  pthread_mutex_init(&c->lock, NULL);

  if (!valid_name(c->name)) {
    syslog(LOG_ERR, "counter name '%s' invalid (allowed: A-Z a-z 0-9 _ . -, not starting with '.').", c->name);
    return -1;
  }

  if (c->source == NULL) {
    syslog(LOG_ERR, "counter '%s': source not given.", c->name);
    return -1;
  }

  if (strcmp(c->name, c->source) == 0) {
    syslog(LOG_ERR, "counter '%s': source must have a different name.", c->name);
    return -1;
  }

  if (c->integrate_power) {
    if (c->max_gap <= 0) {
      syslog(LOG_ERR, "counter '%s': max_gap invalid.", c->name);
      return -1;
    }
    if (c->max_power != 0.0) {
      syslog(LOG_ERR, "counter '%s': max_power is not allowed with integrate_power.", c->name);
      return -1;
    }
  } else if (c->max_power < 0.0) {
    syslog(LOG_ERR, "counter '%s': max_power invalid.", c->name);
    return -1;
  }

  c->src_disp = uvrgw_conf_get_dispatcher(c->source, true);
  c->disp = uvrgw_conf_get_dispatcher(c->name, false);
  if (c->src_disp == NULL || c->disp == NULL) {
    return -1;
  }

  return 0;
}

static bool valid_name(const char *name) {
  const char *p;

  if (name == NULL || *name == 0 || *name == '.') {
    return false;
  }

  for (p = name; *p != 0; p++) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-')) {
      return false;
    }
  }

  return true;
}

static COUNTER_T *find_counter(const char *name) {
  COUNTER_T *c;
  int idx;

  for (c = counters, idx = 0; idx < counters_count; c++, idx++) {
    if (strcmp(c->name, name) == 0) {
      return c;
    }
  }

  return NULL;
}

void counter_register_disp_cbs(void) {
  COUNTER_T *c;
  int idx;

  for (c = counters, idx = 0; idx < counters_count; c++, idx++) {
    uvrgw_conf_register_disp_cb(c->src_disp, c, source_update);
  }
}

void counter_unconfigure(void) {
  COUNTER_T *c;
  int idx;

  for (c = counters, idx = 0; idx < counters_count; c++, idx++) {
    free((void *) c->name);
    free((void *) c->source);
    pthread_mutex_destroy(&c->lock);
  }
  free(counters);
  free(counter_dir);
}

int counter_startup(void) {
  COUNTER_T *c;
  int idx;
  const char *state_dir;

  if (counters_count == 0) {
    return 0;
  }

  // create counter state directory
  state_dir = uvrgw_conf_state_dir();
  counter_dir = malloc(strlen(state_dir) + strlen(COUNTER_SUBDIR) + 2);
  if (counter_dir == NULL) {
    syslog(LOG_ERR, "Failed to allocate counter directory name.");
    return -1;
  }
  sprintf(counter_dir, "%s/%s", state_dir, COUNTER_SUBDIR);

  if (mkdir(counter_dir, 0750) < 0 && errno != EEXIST) {
    syslog(LOG_ERR, "Could not create counter directory '%s': %s", counter_dir, strerror(errno));
    return -1;
  }

  // load states (before any source is started)
  for (c = counters, idx = 0; idx < counters_count; c++, idx++) {
    load_state(c);
  }

  thread_running = true;
  if (pthread_create(&thread, NULL, counter_thread, NULL) != 0) {
    thread_running = false;
    syslog(LOG_ERR, "failed to start counter thread");
    return -1;
  }

  return 0;
}

void counter_shutdown(void) {
  if (thread_running) {
    thread_running = false;
    pthread_join(thread, NULL);
  }

  // final save (sources are already stopped)
  if (counter_dir != NULL) {
    save_states();
  }
}

/**
 * @brief Dispatcher callback: a new source value arrived.
 *
 * @param v  @c COUNTER_T pointer.
 * @param f  Source value (device counter reading or power in W).
 * @return   0.
 */
static int source_update(void *v, double f) {
  COUNTER_T *c = (COUNTER_T *) v;
  int64_t now = utl_get_ticks();
  bool valid = true;

  pthread_mutex_lock(&c->lock);

  if (!c->disabled) {
    if (c->integrate_power) {
      power_update(c, f, now);
    } else {
      valid = device_update(c, f, now);
    }

    if (valid && c->initialized) {
      uvrgw_conf_disp_val(c->disp, c, c->accum);
    }
  }

  pthread_mutex_unlock(&c->lock);

  return 0;
}

/**
 * @brief Process a device counter reading (called with lock held).
 *
 * @param c    Counter.
 * @param raw  Device counter reading.
 * @param now  Current monotonic time (ms).
 * @return     false if the reading is invalid (negative).
 */
static bool device_update(COUNTER_T *c, double raw, int64_t now) {
  double delta, limit;

  // counters are never negative
  if (raw < 0.0) {
    return false;
  }

  // first reading ever: continue at the device counter value
  if (!c->initialized) {
    c->accum = raw;
    c->last_read = raw;
    c->initialized = true;
    c->dirty = true;
    c->has_change = true;
    c->change_ts = now;
    return true;
  }

  if (raw == c->last_read) {
    return true;
  }

  if (raw < c->last_read) {
    // device counter reset: count up from 0
    syslog(LOG_INFO, "counter '%s': source reset detected (%.3f -> %.3f).", c->name, c->last_read, raw);
    delta = raw;
  } else {
    delta = raw - c->last_read;
  }

  // plausibility check against the time since the last change
  // (not possible for the first change after startup)
  if (c->max_power > 0.0 && c->has_change) {
    limit = c->max_power * (double) (now - c->change_ts) / 3600000.0;
    if (delta > limit) {
      syslog(LOG_WARNING, "counter '%s': implausible change (%.3f -> %.3f) ignored, resynchronised.", c->name, c->last_read, raw);
      delta = 0.0;
    }
  }

  c->accum += delta;
  c->last_read = raw;
  c->dirty = true;
  c->has_change = true;
  c->change_ts = now;

  return true;
}

/**
 * @brief Process a power value for integration (called with lock held).
 *
 * @param c      Counter.
 * @param power  Power in W.
 * @param now    Current monotonic time (ms).
 */
static void power_update(COUNTER_T *c, double power, int64_t now) {
  if (!c->initialized) {
    c->accum = 0.0;
    c->last_read = 0.0;
    c->initialized = true;
    c->dirty = true;
  }

  // integrate the previous power value up to now, then hold the new one
  integrate(c, now);
  c->has_power = true;
  c->power = power;
  c->power_ts = now;
  c->integrated_ts = now;
}

/**
 * @brief Integrate the held power value up to @p now, but at most up to
 *        @c max_gap after the last power value (called with lock held).
 *
 * @param c    Counter.
 * @param now  Current monotonic time (ms).
 */
static void integrate(COUNTER_T *c, int64_t now) {
  int64_t end;

  if (!c->has_power) {
    return;
  }

  end = c->power_ts + c->max_gap;
  if (end > now) {
    end = now;
  }

  if (end > c->integrated_ts) {
    if (c->power > 0.0) {
      c->accum += c->power * (double) (end - c->integrated_ts) / 3600000.0;
      c->dirty = true;
    }
    c->integrated_ts = end;
  }
}

static bool power_fresh(COUNTER_T *c, int64_t now) {
  return c->has_power && (now - c->power_ts) <= c->max_gap;
}

/**
 * @brief Counter thread: advance power integration and save states.
 *
 * @param ptr  Unused.
 * @return     NULL.
 */
static void *counter_thread(void *ptr) {
  COUNTER_T *c;
  int idx;
  int64_t now;
  int64_t next_tick = 0;
  int64_t next_save;

  next_save = utl_get_ticks() + COUNTER_SAVE_INTERVAL_MS;

  while (thread_running) {
    now = utl_get_ticks();

    if (now >= next_tick) {
      next_tick = now + COUNTER_TICK_MS;

      for (c = counters, idx = 0; idx < counters_count; c++, idx++) {
        if (!c->integrate_power) {
          continue;
        }

        pthread_mutex_lock(&c->lock);
        if (!c->disabled && c->initialized) {
          integrate(c, now);
          // publish only while the source is fresh (staleness propagates)
          if (power_fresh(c, now)) {
            uvrgw_conf_disp_val(c->disp, c, c->accum);
          }
        }
        pthread_mutex_unlock(&c->lock);
      }
    }

    if (now >= next_save) {
      next_save = now + COUNTER_SAVE_INTERVAL_MS;
      save_states();
    }

    usleep(COUNTER_THREAD_PERIOD_US);
  }

  return NULL;
}

static char *state_path(const COUNTER_T *c, const char *suffix) {
  char *path;

  path = malloc(strlen(counter_dir) + strlen(c->name) + strlen(suffix) + 2);
  if (path != NULL) {
    sprintf(path, "%s/%s%s", counter_dir, c->name, suffix);
  }

  return path;
}

/**
 * @brief Load the state file of a counter.
 *
 * A missing file leaves the counter uninitialised; an unreadable or
 * invalid file disables the counter.
 *
 * @param c  Counter.
 */
static void load_state(COUNTER_T *c) {
  char *path;
  FILE *f;
  double accum, last_read;
  char extra;
  int n;

  path = state_path(c, "");
  if (path == NULL) {
    syslog(LOG_ERR, "counter '%s': failed to allocate state file name, counter disabled.", c->name);
    c->disabled = true;
    return;
  }

  f = fopen(path, "r");
  if (f == NULL) {
    if (errno == ENOENT) {
      syslog(LOG_INFO, "counter '%s': no state file, starting new counter.", c->name);
    } else {
      syslog(LOG_ERR, "counter '%s': could not open state file '%s' (%s), counter disabled.", c->name, path, strerror(errno));
      c->disabled = true;
    }
    free(path);
    return;
  }

  n = fscanf(f, "%lf %lf %c", &accum, &last_read, &extra);
  fclose(f);

  if (n != 2 || !isfinite(accum) || !isfinite(last_read) || accum < 0.0 || last_read < 0.0) {
    syslog(LOG_ERR, "counter '%s': invalid state file '%s' (expected \"<accumulated> <last reading>\"), counter disabled.", c->name, path);
    c->disabled = true;
    free(path);
    return;
  }

  c->accum = accum;
  c->last_read = last_read;
  c->initialized = true;

  free(path);
}

/**
 * @brief Save the states of all changed counters.
 */
static void save_states(void) {
  COUNTER_T *c;
  int idx;
  double accum, last_read;
  bool dirty;
  int dir_fd;
  bool saved = false;

  for (c = counters, idx = 0; idx < counters_count; c++, idx++) {
    pthread_mutex_lock(&c->lock);
    dirty = c->dirty;
    accum = c->accum;
    last_read = c->last_read;
    c->dirty = false;
    pthread_mutex_unlock(&c->lock);

    if (!dirty) {
      continue;
    }

    if (save_state(c, accum, last_read) < 0) {
      // retry on next save
      pthread_mutex_lock(&c->lock);
      c->dirty = true;
      pthread_mutex_unlock(&c->lock);
      continue;
    }
    saved = true;
  }

  // make renames durable
  if (saved) {
    dir_fd = open(counter_dir, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
      fsync(dir_fd);
      close(dir_fd);
    }
  }
}

/**
 * @brief Write the state file of a counter atomically (write temporary
 *        file, fsync, rename).
 *
 * @param c          Counter.
 * @param accum      Accumulated value.
 * @param last_read  Last reading.
 * @return           0 on success, -1 on error.
 */
static int save_state(COUNTER_T *c, double accum, double last_read) {
  char *path, *tmp_path;
  FILE *f;
  int ret = -1;

  path = state_path(c, "");
  tmp_path = state_path(c, ".tmp");
  if (path == NULL || tmp_path == NULL) {
    syslog(LOG_ERR, "counter '%s': failed to allocate state file name.", c->name);
    goto out;
  }

  f = fopen(tmp_path, "w");
  if (f == NULL) {
    syslog(LOG_ERR, "counter '%s': could not write state file '%s': %s", c->name, tmp_path, strerror(errno));
    goto out;
  }

  if (fprintf(f, "%.6f %.6f\n", accum, last_read) < 0 || fflush(f) != 0 || fsync(fileno(f)) < 0) {
    syslog(LOG_ERR, "counter '%s': could not write state file '%s': %s", c->name, tmp_path, strerror(errno));
    fclose(f);
    unlink(tmp_path);
    goto out;
  }

  if (fclose(f) != 0) {
    syslog(LOG_ERR, "counter '%s': could not write state file '%s': %s", c->name, tmp_path, strerror(errno));
    unlink(tmp_path);
    goto out;
  }

  if (rename(tmp_path, path) < 0) {
    syslog(LOG_ERR, "counter '%s': could not rename state file to '%s': %s", c->name, path, strerror(errno));
    unlink(tmp_path);
    goto out;
  }

  ret = 0;

out:
  free(path);
  free(tmp_path);
  return ret;
}
