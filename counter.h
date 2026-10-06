/**
 * @file counter.h
 * @brief Persistent energy counters.
 *
 * A counter listens to a source value and publishes a monotonic,
 * persistent accumulated value under its own name.  Two modes:
 *
 *  - Device counter (default): the source is a counter of a device that
 *    may be reset (e.g. on reboot).  The accumulated value is increased by
 *    the difference to the last reading; if the reading is lower than the
 *    last one, a reset is assumed and the reading itself is added.  With
 *    @c max_power, increases that are implausible for the time since the
 *    last change are treated as glitch: nothing is added and the last
 *    reading is resynchronised.  An invalid source value (reset after
 *    the @c stale_timeout of its input) is ignored, the total stays.
 *
 *  - Power integration (@c integrate_power): the source is a power in W,
 *    which is integrated to Wh.  The last power value is held until the
 *    next one arrives; an invalid source value (reset after the
 *    @c stale_timeout of its input) stops integration until the next
 *    power value.  The total is published every second.  @c sign selects the
 *    counted part: positive power (default), or negative power counted as
 *    positive energy (e.g. separate heating and cooling counters of a heat
 *    pump); the other part is counted as 0.
 *
 * @c scale multiplies the source value first (e.g. 1000 for kW sources).
 *
 * The state (accumulated value and last reading) of each counter is
 * stored in its own file <state_dir>/counters/<name> as
 * "<accumulated> <last reading>".  It is written atomically every
 * COUNTER_SAVE_INTERVAL_MS if changed, and on shutdown.  A missing file
 * starts the counter at the first reading (device counter) or at 0
 * (power integration); an unreadable file disables the counter, so a
 * broken file never silently resets the history.
 */

#ifndef _COUNTER_H_
#define _COUNTER_H_

#include "uvrgw_conf.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief A persistent counter.
 */
typedef struct COUNTER {
  const char *name;          /**< Counter name (published value name and state file name). */
  const char *source;        /**< Source value name. */
  bool integrate_power;      /**< Integrate source power (W) instead of using a device counter. */
  double max_power;          /**< Plausibility limit in W for device counters; 0 = off. */
  double scale;              /**< Multiplier for the source value. */
  int sign;                  /**< Power integration: counted sign (UVRGW_CONF_COUNTER_SIGN_*). */

  UVRGW_CONF_VAL_DISPATCH_T *src_disp; /**< Dispatcher of the source value. */
  UVRGW_CONF_VAL_DISPATCH_T *disp;     /**< Dispatcher of the counter value. */

  pthread_mutex_t lock;      /**< Protects the state below. */
  bool disabled;             /**< State file unreadable: counter is not updated. */
  bool initialized;          /**< State is valid (loaded or initialised by first reading). */
  double accum;              /**< Accumulated value (published). */
  double last_read;          /**< Device counter: last reading. */
  bool dirty;                /**< State changed since last save. */

  bool has_change;           /**< Device counter: @c change_ts is valid. */
  int64_t change_ts;         /**< Device counter: time (ms) of the last change of the reading. */

  bool has_power;            /**< Power integration: a power value has been received. */
  double power;              /**< Power integration: last power value (W). */
  int64_t integrated_ts;     /**< Power integration: time (ms) integrated up to. */
} COUNTER_T;

/**
 * @brief Initialise the module (reset counter list).
 */
void counter_init(void);

/**
 * @brief Parse all @c counter{} sections from @p cfg.
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int counter_configure(cfg_t *cfg);

/**
 * @brief Register the source callbacks of all counters.
 */
void counter_register_disp_cbs(void);

/**
 * @brief Free all resources allocated by counter_configure().
 */
void counter_unconfigure(void);

/**
 * @brief Load the counter states and start the counter thread.
 *
 * @return  0 on success, -1 on error.
 */
int counter_startup(void);

/**
 * @brief Publish the totals of all counters that have one.
 *
 * A counter loaded from its state file has a valid total before its
 * source delivers; this dispatches it once.  Call after the outputs are
 * started.
 */
void counter_publish(void);

/**
 * @brief Stop the counter thread.
 *
 * The thread publishes power integration counters, so it must be stopped
 * together with the other value sources, before the outputs are shut down.
 * The counter states are still updated by source values arriving later
 * (e.g. via MQTT) and saved by counter_shutdown().
 */
void counter_stop(void);

/**
 * @brief Stop the counter thread (if still running) and save the counter
 *        states.
 *
 * Call after all sources are stopped.
 */
void counter_shutdown(void);

#endif
