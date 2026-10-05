/**
 * @file sunspec.h
 * @brief SunSpec smart meter emulation (Modbus TCP server).
 *
 * Serves one or more virtual three-phase meters (SunSpec common model 1
 * followed by float meter model 213) via Modbus TCP, e.g. as secondary
 * meters for a Fronius inverter.  Each meter is addressed by its own
 * Modbus unit ID.
 *
 * The meter quantities are taken by name from the value dispatcher
 * (last value of any input, e.g. REST or Modbus master).  Derived
 * quantities are calculated on each request:
 *  - apparent power per phase: S = V × I
 *  - reactive power per phase: Q = sqrt(|S² − P²|)
 *  - phase-to-phase voltages from the phase-to-neutral voltages
 *    (assuming 120° phase shift)
 *  - power factor per phase (if not given): P / S
 *  - totals: sum of currents, powers and energies, average voltages,
 *    total power factor P / S
 * Quantities that are neither configured nor derivable are served as 0.
 *
 * If a configured source value is invalid (no data yet, or reset after
 * the @c stale_timeout of its input), requests for that meter are
 * answered with a "server device failure" exception, so the client keeps
 * its last data instead of seeing bogus values (e.g. energy counters
 * dropping to 0).
 */

#ifndef _SUNSPEC_H_
#define _SUNSPEC_H_

#include "uvrgw_conf.h"

#include <modbus/modbus.h>
#include <pthread.h>
#include <stdbool.h>

#define SUNSPEC_PHASES      3
#define SUNSPEC_MAX_CLIENTS 8

/** @name Per-phase source quantities */
/** @{ */
#define SUNSPEC_PH_CURRENT    0
#define SUNSPEC_PH_VOLTAGE    1
#define SUNSPEC_PH_POWER      2
#define SUNSPEC_PH_PF         3
#define SUNSPEC_PH_ENERGY_IMP 4
#define SUNSPEC_PH_ENERGY_EXP 5
#define SUNSPEC_PH_COUNT      6
/** @} */

struct SUNSPEC_SERVER;

/**
 * @brief Reference to a source value in the dispatcher.
 */
typedef struct SUNSPEC_SRC {
  const char *name;                 /**< Value name, or NULL if not configured. */
  UVRGW_CONF_VAL_DISPATCH_T *disp;  /**< Dispatcher of the value, or NULL if not configured. */
} SUNSPEC_SRC_T;

/**
 * @brief A virtual SunSpec meter.
 */
typedef struct SUNSPEC_METER {
  const char *name;          /**< Meter name (section title, used for logging). */
  int unit_id;               /**< Modbus unit ID the meter answers on. */
  const char *manufacturer;  /**< SunSpec common model Mn (max. 32 chars). */
  const char *model;         /**< SunSpec common model Md (max. 32 chars). */
  const char *options;       /**< SunSpec common model Opt (max. 16 chars). */
  const char *version;       /**< SunSpec common model Vr (max. 16 chars). */
  const char *serial;        /**< SunSpec common model SN (max. 32 chars). */

  SUNSPEC_SRC_T phase[SUNSPEC_PHASES][SUNSPEC_PH_COUNT]; /**< Per-phase sources. */
  SUNSPEC_SRC_T power;       /**< Total active power (optional, else sum of phases). */
  SUNSPEC_SRC_T energy_imp;  /**< Total imported energy (optional, else sum of phases). */
  SUNSPEC_SRC_T energy_exp;  /**< Total exported energy (optional, else sum of phases). */
  SUNSPEC_SRC_T frequency;   /**< Grid frequency (optional). */

  struct SUNSPEC_SERVER *server; /**< Back-pointer to the server. */

  modbus_mapping_t *mapping; /**< Register image (starting at 40000). */
  bool unavailable;          /**< Last request failed due to invalid values (for state logging). */
} SUNSPEC_METER_T;

/**
 * @brief A Modbus TCP server serving SunSpec meters.
 */
typedef struct SUNSPEC_SERVER {
  const char *bind;          /**< Local address to listen on. */
  int port;                  /**< TCP port. */

  int meters_count;          /**< Number of meters. */
  struct SUNSPEC_METER *meters; /**< Array of meters. */

  modbus_t *ctx;             /**< libmodbus server context. */
  int listen_fd;             /**< Listening socket, -1 if closed. */
  int client_fds[SUNSPEC_MAX_CLIENTS]; /**< Client sockets, -1 if unused. */
  bool clients_full;         /**< Last connection was rejected, all client slots in use (for state logging). */

  pthread_t thread;          /**< Server thread handle. */
  bool thread_running;       /**< Set to false to request thread termination. */
} SUNSPEC_SERVER_T;

/**
 * @brief Initialise the module (reset server list).
 *
 * Must be called before sunspec_configure().
 */
void sunspec_init(void);

/**
 * @brief Parse all @c sunspec_server{} sections from @p cfg.
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int sunspec_configure(cfg_t *cfg);

/**
 * @brief Free all resources allocated by sunspec_configure().
 */
void sunspec_unconfigure(void);

/**
 * @brief Open the listening sockets and start the server threads.
 *
 * @return  0 on success, -1 on error.
 */
int sunspec_startup(void);

/**
 * @brief Stop the server threads and close all sockets.
 */
void sunspec_shutdown(void);

#endif
