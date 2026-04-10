/**
 * @file mqtt.h
 * @brief MQTT client — publish/subscribe, value formatting and last-will state topic.
 *
 * Each @c mqtt{} section in the configuration creates one @c MQTT_CONN_T
 * instance.  libmosquitto manages its own background thread for connection,
 * keep-alive and message delivery.
 *
 * Value types:
 *  - @c number: numeric value formatted with a printf-style format string
 *               (must contain exactly one floating-point conversion specifier).
 *  - @c switch: boolean published as "ON" or "OFF".
 *  - @c contact: boolean published as "CLOSED" or "OPEN".
 *
 * A configurable @c state_topic publishes "ON" on connection and "OFF" as
 * the last-will message, enabling broker-side presence detection.
 */

#ifndef _MQTT_H_
#define _MQTT_H_

#include "uvrgw_conf.h"

#include <stdbool.h>
#include <sys/select.h>
#include <mosquitto.h>

struct MQTT_VAL;
struct MQTT_CONN;

/**
 * @brief A single MQTT publish/subscribe value.
 */
typedef struct MQTT_VAL {
  const char *name;    /**< Logical value name (used for dispatch). */
  int dir;             /**< Direction: UVRGW_CONF_VAL_DIR_IN or UVRGW_CONF_VAL_DIR_OUT. */
  int type;            /**< Value type: one of UVRGW_CONF_MQTT_TYPE_* constants. */
  const char *topic;   /**< MQTT topic string. */
  const char *fmt;     /**< printf format string for number type (e.g. "%.2f"); NULL for switch/contact. */
  int qos;             /**< MQTT QoS level (0, 1 or 2); inherits from connection if not set per-value. */
  bool retain;         /**< Retain flag; inherits from connection if not set per-value. */

  struct MQTT_CONN *conn; /**< Back-pointer to the containing connection. */

  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher for this value name. */

} MQTT_VAL_T;

/**
 * @brief An MQTT broker connection instance.
 */
typedef struct MQTT_CONN {
  const char *host;          /**< Broker hostname or IP address. */
  int port;                  /**< Broker TCP port (default 1883). */
  const char *client_id;     /**< MQTT client identifier; NULL lets libmosquitto auto-generate one. */
  const char *user;          /**< Username for broker authentication; NULL if not required. */
  const char *pwd;           /**< Password for broker authentication; NULL if not required. */
  const char *state_topic;   /**< Topic to publish "ON"/"OFF" as presence indicator; NULL to disable. */
  int keepalive_period;      /**< MQTT keep-alive interval in seconds. */
  int qos;                   /**< Default QoS for all values on this connection. */
  bool retain;               /**< Default retain flag for all values on this connection. */

  int values_count;          /**< Number of value definitions. */
  struct MQTT_VAL *values;   /**< Array of value definitions. */

  struct mosquitto *mosq;    /**< libmosquitto handle; NULL when not started. */

  bool connected;            /**< True while the MQTT session is established. */
} MQTT_CONN_T;

/**
 * @brief Initialise the MQTT module (reset connection list).
 *
 * Must be called before mqtt_configure().
 */
void mqtt_init(void);

/**
 * @brief Parse all @c mqtt{} sections from @p cfg and populate the connection list.
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int mqtt_configure(cfg_t *cfg);

/**
 * @brief Register send callbacks for all OUT-direction MQTT values.
 *
 * Called after the dispatcher callback arrays have been allocated.
 */
void mqtt_register_disp_cbs(void);

/**
 * @brief Free all resources allocated by mqtt_configure().
 *
 * Does not stop running mosquitto instances — call mqtt_shutdown() first.
 */
void mqtt_unconfigure(void);

/**
 * @brief Initialise libmosquitto, create all mosquitto instances and start
 *        their background threads.
 *
 * @return  0 on success, -1 on error.
 */
int mqtt_startup(void);

/**
 * @brief Disconnect all MQTT sessions and clean up libmosquitto.
 *
 * Publishes "OFF" to each connection's @c state_topic before disconnecting.
 */
void mqtt_shutdown(void);

#endif

