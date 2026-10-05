/**
 * @file rest.h
 * @brief REST/JSON client — HTTP GET with dot-separated JSON path extraction.
 *
 * Each @c json{} section in the configuration creates one @c REST_CONN_T
 * instance.  A dedicated thread polls the configured URL at the configured
 * interval, parses the JSON response body and dispatches extracted values.
 *
 * JSON path syntax: dot-separated keys with numeric indices for arrays.
 * Example: @c "sensors.0.temperature" navigates into
 * @c {"sensors":[{"temperature":21.5}]}.
 *
 * REST connections are input-only; there is no write-back mechanism.
 */

#ifndef _REST_H_
#define _REST_H_

#include "uvrgw_conf.h"

#include <stdint.h>
#include <pthread.h>

struct REST_VAL;
struct REST_CONN;

/**
 * @brief A single JSON value to extract from a REST endpoint.
 */
typedef struct REST_VAL {
  const char *name;      /**< Logical value name (used for dispatch). */
  const char *path;      /**< Dot-separated JSON path (e.g. "data.temperature"). */
  double scale;          /**< Scale factor applied after extraction. */
  double offset;         /**< Offset added after scaling. */

  struct REST_CONN *conn; /**< Back-pointer to the containing connection. */

  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher for this value name. */
  bool missing;          /**< Path missing or not a number in the last poll (for state logging). */

} REST_VAL_T;

/**
 * @brief A REST/JSON polling connection.
 */
typedef struct REST_CONN {
  const char *url;       /**< HTTP(S) URL to GET. */
  int interval;          /**< Polling interval in ms. */
  int timeout;           /**< HTTP request timeout in ms. */
  const char *user;      /**< HTTP Basic-Auth username; NULL if not required. */
  const char *pwd;       /**< HTTP Basic-Auth password; NULL if not required. */
  const char *valid_if;  /**< JSON path that must be present and non-zero for a poll to be used; NULL if not used. */
  bool invalid;          /**< Last poll was rejected by @c valid_if (for state logging). */
  bool get_failed;       /**< Last GET failed or returned no JSON object (for state logging). */
  double init_value;     /**< Default @c init_value for all values of this connection. */
  int stale_timeout;     /**< Default @c stale_timeout (ms) for all values of this connection; 0 = never. */

  int values_count;      /**< Number of value definitions. */
  struct REST_VAL *values; /**< Array of value definitions. */

  pthread_t thread;      /**< Polling thread handle. */
  bool thread_running;   /**< Set to false to request thread termination. */
  int64_t next_poll;     /**< Monotonic time (ms) for the next HTTP GET. */
} REST_CONN_T;

/**
 * @brief Initialise the REST module (reset connection list).
 *
 * Must be called before rest_configure().
 */
void rest_init(void);

/**
 * @brief Parse all @c json{} sections from @p cfg and populate the connection list.
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int rest_configure(cfg_t *cfg);

/**
 * @brief Free all resources allocated by rest_configure().
 *
 * Does not stop running threads — call rest_shutdown() first.
 */
void rest_unconfigure(void);

/**
 * @brief Initialise libcurl and start all polling threads.
 *
 * @return  0 on success, -1 on error.
 */
int rest_startup(void);

/**
 * @brief Stop all polling threads and clean up libcurl.
 */
void rest_shutdown(void);

#endif
