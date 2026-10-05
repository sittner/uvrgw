/**
 * @file mqtt.c
 * @brief MQTT client implementation using libmosquitto.
 *
 * Each configured MQTT connection creates a libmosquitto instance with its
 * own background thread.  The module registers three libmosquitto callbacks:
 *  - connect_callback: marks the session as connected, publishes "ON" to
 *    the state topic and subscribes to all IN-direction value topics.
 *  - disconnect_callback: marks the session as disconnected.
 *  - message_callback: receives messages on subscribed topics, converts
 *    the payload to a double and dispatches it via the value system.
 *
 * Output values are published by send_value(), which is registered as a
 * dispatch callback for all OUT-direction values.
 */
#include "mqtt.h"
#include "mqtt_logger.h"
#include "can.h"
#include "mb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <syslog.h>
#include <fcntl.h>

#define CONST_STR_PAYLOAD(s) (sizeof(s) - 1), s

static int conns_count;
static MQTT_CONN_T *conns;

static int conn_configure(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static int send_value(void *v, double f, bool valid);
static bool parse_payload(const MQTT_VAL_T *val, const char *buf, double *f);

static int conn_startup(MQTT_CONN_T *conn);
static void conn_stop(MQTT_CONN_T *conn);
static void conn_destroy(MQTT_CONN_T *conn);

/**
 * @brief libmosquitto connection callback.
 *
 * Called by the mosquitto background thread when a broker connection is
 * established.  Publishes "ON" to the state topic and subscribes to all
 * IN-direction value topics.
 *
 * @param mosq    libmosquitto handle.
 * @param obj     User data pointer (@c MQTT_CONN_T *).
 * @param result  Connection result code (0 = success).
 */
static void connect_callback(struct mosquitto *mosq, void *obj, int result) {
  MQTT_CONN_T *conn = (MQTT_CONN_T *) obj;
  MQTT_VAL_T *val;
  int val_idx;

  if (result != 0) {
    syslog(LOG_WARNING, "mqtt connection to %s:%d refused: %s", conn->host, conn->port, mosquitto_connack_string(result));
    return;
  }

  syslog(LOG_INFO, "mqtt connected to %s:%d", conn->host, conn->port);
  conn->connected = true;

  if (conn->state_topic != NULL) {
    mosquitto_publish(mosq, NULL, conn->state_topic, CONST_STR_PAYLOAD("ON"), conn->qos, conn->retain);
  }

  for (val = conn->values, val_idx = 0; val_idx < conn->values_count; val++, val_idx++) {
    if (val->dir == UVRGW_CONF_VAL_DIR_IN) {
      mosquitto_subscribe(mosq, NULL, val->topic, 0);
    }
  }
}

/**
 * @brief libmosquitto disconnection callback.
 *
 * Called when the broker connection is lost or deliberately closed.
 *
 * @param mosq    libmosquitto handle.
 * @param obj     User data pointer (@c MQTT_CONN_T *).
 * @param result  Disconnect reason code.
 */
static void disconnect_callback(struct mosquitto *mosq, void *obj, int result) {
  MQTT_CONN_T *conn = (MQTT_CONN_T *) obj;

  if (conn->connected && result != 0) {
    syslog(LOG_WARNING, "mqtt connection to %s:%d lost: %s", conn->host, conn->port, mosquitto_strerror(result));
  }
  conn->connected = false;
}

/**
 * @brief libmosquitto message received callback.
 *
 * Called when a subscribed topic delivers a message.  Searches for a
 * matching IN-direction value, converts the payload to a double
 * (interpreting "ON"/"CLOSED" as 1.0 for switch/contact types) and
 * dispatches it via the value system.
 *
 * @param mosq  libmosquitto handle.
 * @param obj   User data pointer (@c MQTT_CONN_T *).
 * @param msg   Received message (topic, payload, payloadlen, …).
 */
static void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
  MQTT_CONN_T *conn = (MQTT_CONN_T *) obj;
  MQTT_VAL_T *val;
  int val_idx;
  char buf[32];
  double f;

  // check for maximum payload length (incl. terminating NUL)
  if (msg->payloadlen < 0 || msg->payloadlen >= (int) sizeof(buf)) {
    return;
  }

  // search for topic
  for (val = conn->values, val_idx = 0; val_idx < conn->values_count; val++, val_idx++) {
    if (val->dir == UVRGW_CONF_VAL_DIR_IN && strcmp(val->topic, msg->topic) == 0) {
      // get payload as string
      memcpy(buf, msg->payload, msg->payloadlen);
      buf[msg->payloadlen] = 0;

      // drop invalid payloads (e.g. "unknown", "unavailable"), so the
      // value becomes stale instead of wrong
      if (!parse_payload(val, buf, &f)) {
        if (!val->invalid) {
          syslog(LOG_WARNING, "mqtt value '%s': invalid payload '%s' on topic '%s' ignored.", val->name, buf, val->topic);
          val->invalid = true;
        }
        return;
      }
      if (val->invalid) {
        syslog(LOG_INFO, "mqtt value '%s': valid payload again.", val->name);
        val->invalid = false;
      }

      // dispatch value
      uvrgw_conf_disp_val(val->disp, val, f, true);
      return;
    }
  }
}

void mqtt_init(void) {
  conns_count = 0;
  conns = NULL;
}

int mqtt_configure(cfg_t *cfg) {
  return uvrgw_conf_config_childs(cfg, "mqtt", &conns_count, (void **) &conns, sizeof(MQTT_CONN_T), NULL, conn_configure);
}

static int conn_configure(cfg_t *cfg, void *ctx, void *child) {
  MQTT_CONN_T *conn = (MQTT_CONN_T *) child;

  conn->host = uvrgw_conf_strdup(cfg_getstr(cfg, "host"));
  conn->port = cfg_getint(cfg, "port");
  conn->client_id = uvrgw_conf_strdup(cfg_getstr(cfg, "client_id"));
  conn->user = uvrgw_conf_strdup(cfg_getstr(cfg, "user"));
  conn->pwd = uvrgw_conf_strdup(cfg_getstr(cfg, "pwd"));
  conn->state_topic = uvrgw_conf_strdup(cfg_getstr(cfg, "state_topic"));
  conn->keepalive_period = cfg_getint(cfg, "keepalive_period");
  conn->qos = cfg_getint(cfg, "qos");
  conn->retain = cfg_getbool(cfg, "retain");
  conn->init_value = cfg_getfloat(cfg, "init_value");
  conn->stale_timeout = cfg_getint(cfg, "stale_timeout");

  if (conn->host == NULL) {
    syslog(LOG_ERR, "mqtt host name not given.");
    return -1;
  }

  if (uvrgw_conf_config_childs(cfg, "value", &conn->values_count, (void **) &conn->values, sizeof(MQTT_VAL_T), conn, value_configure) < 0) {
    return -1;
  }

  return mqtt_logger_configure(cfg, conn);
}

/**
 * @brief Check that a printf format contains exactly one double conversion.
 *
 * Accepts flags, width, precision and the (no-op) @c l modifier followed by
 * one of @c f, @c F, @c e, @c E, @c g, @c G, @c a, @c A.  @c %% is allowed.
 *
 * @param fmt  Format string.
 * @return     0 if valid, -1 otherwise.
 */
static int check_fmt(const char *fmt) {
  const char *p;
  int conv_count = 0;

  for (p = fmt; *p != 0; p++) {
    if (*p != '%') {
      continue;
    }

    p++;
    if (*p == '%') {
      continue;
    }

    p += strspn(p, "-+ #0");
    p += strspn(p, "0123456789");
    if (*p == '.') {
      p++;
      p += strspn(p, "0123456789");
    }
    if (*p == 'l') {
      p++;
    }

    if (*p == 0 || strchr("fFeEgGaA", *p) == NULL) {
      return -1;
    }

    conv_count++;
  }

  return (conv_count == 1) ? 0 : -1;
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  MQTT_VAL_T *val = (MQTT_VAL_T *) child;
  double init_value;
  int stale_timeout;

  val->conn = (MQTT_CONN_T *) ctx;

  val->name = uvrgw_conf_strdup(cfg_title(cfg));
  val->dir = cfg_getint(cfg, "dir");
  val->type = cfg_getint(cfg, "type");
  val->topic = uvrgw_conf_strdup(cfg_getstr(cfg, "topic"));
  val->fmt = uvrgw_conf_strdup(cfg_getstr(cfg, "fmt"));

  if (cfg_size(cfg, "qos") > 0) {
    val->qos = cfg_getint(cfg, "qos");
  } else {
    val->qos = val->conn->qos;
  }

  if (cfg_size(cfg, "retain") > 0) {
    val->retain = cfg_getbool(cfg, "retain");
  } else {
    val->retain = val->conn->retain;
  }

  // input timeout options: value setting, else section setting
  if (cfg_size(cfg, "init_value") > 0) {
    init_value = cfg_getfloat(cfg, "init_value");
  } else {
    init_value = val->conn->init_value;
  }

  if (cfg_size(cfg, "stale_timeout") > 0) {
    stale_timeout = cfg_getint(cfg, "stale_timeout");
  } else {
    stale_timeout = val->conn->stale_timeout;
  }

  if (val->topic == NULL) {
    syslog(LOG_ERR, "mqtt value topic not given.");
    return -1;
  }

  if (val->dir == UVRGW_CONF_VAL_DIR_OUT && val->type == UVRGW_CONF_MQTT_TYPE_NUMBER && val->fmt == NULL) {
    syslog(LOG_ERR, "mqtt value fmt not given.");
    return -1;
  }

  if (val->fmt != NULL && check_fmt(val->fmt) < 0) {
    syslog(LOG_ERR, "mqtt value '%s' fmt must contain exactly one double conversion (%%f, %%e, %%g).", val->name);
    return -1;
  }

  val->disp = uvrgw_conf_get_dispatcher(val->name, (val->dir == UVRGW_CONF_VAL_DIR_OUT));
  if (val->disp == NULL) {
    return -1;
  }

  if (val->dir == UVRGW_CONF_VAL_DIR_IN && uvrgw_conf_set_producer(val->disp, "mqtt", val->conn->host, val, true, init_value, stale_timeout) < 0) {
    return -1;
  }

  return 0;
}

void mqtt_register_disp_cbs(void) {
  MQTT_CONN_T *conn;
  int conn_idx;
  MQTT_VAL_T *val;
  int val_idx;

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    for (val = conn->values, val_idx = 0; val_idx < conn->values_count; val++, val_idx++) {
      if (val->dir == UVRGW_CONF_VAL_DIR_OUT) {
        uvrgw_conf_register_disp_cb(val->disp, val, send_value);
      }
    }
  }
}

void mqtt_unconfigure(void) {
  MQTT_CONN_T *conn;
  int conn_idx;
  MQTT_VAL_T *val;
  int val_idx;

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    for (val = conn->values, val_idx = 0; val_idx < conn->values_count; val++, val_idx++) {
      free((void *) val->name);
      free((void *) val->topic);
      free((void *) val->fmt);
    }
    free((void *) conn->host);
    free((void *) conn->client_id);
    free((void *) conn->user);
    free((void *) conn->pwd);
    free((void *) conn->state_topic);
    free(conn->values);
    mqtt_logger_unconfigure(conn);
  }
  free(conns);
}

int mqtt_startup(void) {
  MQTT_CONN_T *conn;
  int conn_idx;

  if (mosquitto_lib_init()) {
    syslog(LOG_ERR, "Failed to init mosquitto lib");
    goto fail0;
  }

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    if (conn_startup(conn) < 0) {
      goto fail1;
    }
  }

  if (mqtt_logger_startup(conns, conns_count) < 0) {
    goto fail1;
  }

  return 0;

fail1:
  mqtt_shutdown();
fail0:
  return -1;
}

/**
 * @brief Create a mosquitto instance, configure callbacks and start its loop thread.
 *
 * Sets up credentials, last-will and callbacks, then calls
 * mosquitto_connect_async() (initial failure is non-fatal; libmosquitto will
 * retry) and mosquitto_loop_start() to launch the background thread.
 *
 * @param conn  Connection to start.
 * @return      0 on success, -1 on error.
 */
static int conn_startup(MQTT_CONN_T *conn) {
  conn->mosq = mosquitto_new(conn->client_id, true, conn);
  if (conn->mosq == NULL) {
    syslog(LOG_ERR, "Failed to create mosquitto instance");
    goto fail1;
  }

  if (conn->user != NULL && conn->pwd != NULL) {
    if (mosquitto_username_pw_set(conn->mosq, conn->user, conn->pwd)) {
      syslog(LOG_ERR, "Failed to set mosquitto credentials");
      goto fail2;
    }
  }

  mosquitto_connect_callback_set(conn->mosq, connect_callback);
  mosquitto_disconnect_callback_set(conn->mosq, disconnect_callback);
  mosquitto_message_callback_set(conn->mosq, message_callback);

  if (conn->state_topic != NULL) {
    if (mosquitto_will_set(conn->mosq, conn->state_topic, CONST_STR_PAYLOAD("OFF"), conn->qos, conn->retain)) {
      syslog(LOG_ERR, "Failed to set mosquitto will");
      goto fail2;
    }
  }

  // start the network thread before connecting: if the first connect
  // fails (e.g. broker not reachable), libmosquitto only retries when the
  // thread is already running
  if (mosquitto_loop_start(conn->mosq)) {
    syslog(LOG_ERR, "Failed to start mosquitto thread");
    goto fail2;
  }

  if (mosquitto_connect_async(conn->mosq, conn->host, conn->port, conn->keepalive_period)) {
    syslog(LOG_INFO, "initial mqtt connection to %s:%d failed, retrying", conn->host, conn->port);
  }

  return 0;

fail2:
  mosquitto_destroy(conn->mosq);
  conn->mosq = NULL;
fail1:
  return -1;
}

void mqtt_shutdown(void) {
  MQTT_CONN_T *conn;
  int conn_idx;

  // stop logger before the connections are destroyed
  mqtt_logger_shutdown();

  // stop all network threads first: a message received on one connection
  // can be dispatched to an output value of another connection, so no
  // instance may be destroyed while any network thread is still running
  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    conn_stop(conn);
  }

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    conn_destroy(conn);
  }

  mosquitto_lib_cleanup();
}

/**
 * @brief Publish "OFF" to the state topic (if configured), disconnect and stop
 *        the network thread of the mosquitto instance.
 *
 * @param conn  Connection to stop.
 */
static void conn_stop(MQTT_CONN_T *conn) {
  if (conn->mosq != NULL) {
    if (conn->connected && conn->state_topic != NULL) {
      mosquitto_publish(conn->mosq, NULL, conn->state_topic, CONST_STR_PAYLOAD("OFF"), conn->qos, conn->retain);
    }
    // always request disconnect: also ends the reconnect attempts of the
    // network thread while the broker is not reachable
    mosquitto_disconnect(conn->mosq);
    mosquitto_loop_stop(conn->mosq, false);
  }
}

/**
 * @brief Destroy the mosquitto instance (network thread must be stopped).
 *
 * @param conn  Connection to destroy.
 */
static void conn_destroy(MQTT_CONN_T *conn) {
  if (conn->mosq != NULL) {
    mosquitto_destroy(conn->mosq);
    conn->mosq = NULL;
  }
}

/**
 * @brief Dispatch callback that publishes a value to the MQTT broker.
 *
 * Formats the value according to the value's @c type and @c fmt fields and
 * calls mosquitto_publish().  For switch values publishes "ON"/"OFF"; for
 * contact values publishes "CLOSED"/"OPEN"; for number values uses snprintf
 * with the configured format string.
 *
 * @param v      @c MQTT_VAL_T pointer.
 * @param f      Dispatched value.
 * @param valid  Validity (unused, outputs write every value).
 * @return       0 on success, -1 on publish error.
 */
static int send_value(void *v, double f, bool valid) {
  MQTT_VAL_T *val = (MQTT_VAL_T *) v;
  MQTT_CONN_T *conn = val->conn;
  char buf[32];
  int len;
  int err;

  switch (val->type) {
    case UVRGW_CONF_MQTT_TYPE_SWITCH:
      if (f >= 0.5) {
        err = mosquitto_publish(conn->mosq, NULL, val->topic, CONST_STR_PAYLOAD("ON"), val->qos, val->retain);
      } else {
        err = mosquitto_publish(conn->mosq, NULL, val->topic, CONST_STR_PAYLOAD("OFF"), val->qos, val->retain);
      }
      break;

    case UVRGW_CONF_MQTT_TYPE_CONTACT:
      if (f >= 0.5) {
        err = mosquitto_publish(conn->mosq, NULL, val->topic, CONST_STR_PAYLOAD("CLOSED"), val->qos, val->retain);
      } else {
        err = mosquitto_publish(conn->mosq, NULL, val->topic, CONST_STR_PAYLOAD("OPEN"), val->qos, val->retain);
      }
      break;

    case UVRGW_CONF_MQTT_TYPE_NUMBER:
      len = snprintf(buf, sizeof(buf), val->fmt, f);
      if (len < 0 || len >= (int) sizeof(buf)) {
        err = MOSQ_ERR_PAYLOAD_SIZE;
      } else {
        err = mosquitto_publish(conn->mosq, NULL, val->topic, len, buf, val->qos, val->retain);
      }
      break;

    default:
      err = MOSQ_ERR_UNKNOWN;
  }

  if (err != MOSQ_ERR_SUCCESS) {
    // not connected: already logged as connection state change
    if (err != MOSQ_ERR_NO_CONN) {
      syslog(LOG_ERR, "mqtt send of '%s' failed: %s", val->topic, mosquitto_strerror(err));
    }
    return -1;
  }

  return 0;
}

/**
 * @brief Convert an input payload to a value.
 *
 * @param val  Value descriptor (for the type).
 * @param buf  NUL terminated payload.
 * @param f    Output: value.
 * @return     true if the payload is valid for the type: a finite number
 *             (surrounding whitespace allowed), ON/OFF or CLOSED/OPEN
 *             (case insensitive).
 */
static bool parse_payload(const MQTT_VAL_T *val, const char *buf, double *f) {
  char *end;

  switch (val->type) {
    case UVRGW_CONF_MQTT_TYPE_SWITCH:
      if (strcasecmp("ON", buf) == 0) {
        *f = 1.0;
        return true;
      }
      if (strcasecmp("OFF", buf) == 0) {
        *f = 0.0;
        return true;
      }
      return false;

    case UVRGW_CONF_MQTT_TYPE_CONTACT:
      if (strcasecmp("CLOSED", buf) == 0) {
        *f = 1.0;
        return true;
      }
      if (strcasecmp("OPEN", buf) == 0) {
        *f = 0.0;
        return true;
      }
      return false;

    case UVRGW_CONF_MQTT_TYPE_NUMBER:
      *f = strtod(buf, &end);
      if (end == buf || !isfinite(*f)) {
        return false;
      }
      while (isspace((unsigned char) *end)) {
        end++;
      }
      return *end == 0;

    default:
      return false;
  }
}
