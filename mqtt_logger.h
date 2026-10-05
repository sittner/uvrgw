/**
 * @file mqtt_logger.h
 * @brief Periodic value snapshots published as JSON via MQTT (for logging).
 *
 * A logger is configured inside an @c mqtt section and publishes a JSON
 * object with a Unix timestamp and the current values of the configured
 * value names every @c interval seconds, aligned to the wall clock
 * (e.g. xx:00, xx:05, ...):
 *
 *   {"time":1790762700,"pv_energy":16852887.05,"office_temp":null}
 *
 * Values are read from the dispatcher (last value); invalid values (no
 * data yet, or reset after the @c stale_timeout of their input) are
 * logged as null.  Numbers always
 * contain a decimal point or exponent, so consumers infer a float type.
 *
 * Snapshots are only taken while the system clock is synchronised
 * (kernel time status, set by ntpd, chrony or systemd-timesyncd).
 *
 * Each snapshot is published once.  With QoS > 0 (default 1) libmosquitto
 * keeps snapshots published while the broker is not connected (although
 * mosquitto_publish() reports MOSQ_ERR_NO_CONN) and delivers them after
 * reconnect, so broker outages do not cause gaps.  This buffer is in
 * memory only (lost on restart).  With QoS 0 snapshots are lost while the
 * broker is not connected.
 */

#ifndef _MQTT_LOGGER_H_
#define _MQTT_LOGGER_H_

#include "uvrgw_conf.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct MQTT_CONN;

/**
 * @brief A logged value.
 */
typedef struct MQTT_LOGGER_VAL {
  const char *name;          /**< Value name (dispatcher). */
  const char *field;         /**< JSON field name (default: value name). */
  double scale;              /**< Multiplier for the value. */
  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher of the value. */
} MQTT_LOGGER_VAL_T;

/**
 * @brief A logger (one JSON snapshot topic).
 */
typedef struct MQTT_LOGGER {
  const char *name;          /**< Logger name (section title, for logging). */
  const char *topic;         /**< Topic to publish to. */
  int interval;              /**< Snapshot interval in s. */
  int qos;                   /**< QoS of the snapshot messages. */

  int values_count;          /**< Number of logged values. */
  struct MQTT_LOGGER_VAL *values; /**< Array of logged values. */

  struct MQTT_CONN *conn;    /**< Back-pointer to the MQTT connection. */

  time_t next_time;          /**< Wall clock time of the next snapshot. */
  bool publish_failed;       /**< Last publish failed (for state logging). */
  bool unsynced;             /**< Clock was not synchronised (for state logging). */
} MQTT_LOGGER_T;

/**
 * @brief Parse the @c logger sections of an @c mqtt section.
 *
 * @param cfg   @c mqtt section.
 * @param conn  MQTT connection the loggers belong to.
 * @return      0 on success, -1 on error.
 */
int mqtt_logger_configure(cfg_t *cfg, struct MQTT_CONN *conn);

/**
 * @brief Free the loggers of a connection.
 *
 * @param conn  MQTT connection.
 */
void mqtt_logger_unconfigure(struct MQTT_CONN *conn);

/**
 * @brief Start the logger thread (if any logger is configured).
 *
 * @param conns        Array of MQTT connections.
 * @param conns_count  Number of connections.
 * @return             0 on success, -1 on error.
 */
int mqtt_logger_startup(struct MQTT_CONN *conns, int conns_count);

/**
 * @brief Stop the logger thread.
 */
void mqtt_logger_shutdown(void);

#endif
