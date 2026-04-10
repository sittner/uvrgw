#include "mqtt.h"
#include "can.h"
#include "mb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>

#define CONST_STR_PAYLOAD(s) (sizeof(s) - 1), s

static int conns_count;
static MQTT_CONN_T *conns;

static int conn_configure(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static int send_value(void *v, double f);

static int conn_startup(MQTT_CONN_T *conn);
static void conn_shutdown(MQTT_CONN_T *conn);

static void connect_callback(struct mosquitto *mosq, void *obj, int result) {
  MQTT_CONN_T *conn = (MQTT_CONN_T *) obj;
  MQTT_VAL_T *val;
  int val_idx;

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

static void disconnect_callback(struct mosquitto *mosq, void *obj, int result) {
  MQTT_CONN_T *conn = (MQTT_CONN_T *) obj;

  conn->connected = false;
}

static void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
  MQTT_CONN_T *conn = (MQTT_CONN_T *) obj;
  MQTT_VAL_T *val;
  int val_idx;
  char buf[32];
  double f;

  // check vor maximum payload length
  if (msg->payloadlen >= (sizeof(buf) - 1)) {
    return;
  }

  // search for topic
  for (val = conn->values, val_idx = 0; val_idx < conn->values_count; val++, val_idx++) {
    if (val->dir == UVRGW_CONF_VAL_DIR_IN && strcmp(val->topic, msg->topic) == 0) {
      // get payload as string
      memcpy(buf, msg->payload, msg->payloadlen);
      buf[msg->payloadlen] = 0;

      f = 0.0;
      switch (val->type) {
        case UVRGW_CONF_MQTT_TYPE_SWITCH:
          if (strcmp("ON", buf) == 0) {
            f = 1.0;
          }
          break;
        case UVRGW_CONF_MQTT_TYPE_CONTACT:
          if (strcmp("CLOSED", buf) == 0) {
            f = 1.0;
          }
          break;
        case UVRGW_CONF_MQTT_TYPE_NUMBER:
          f = strtod(buf, NULL);
          break;
      }

      // dispatch value
      uvrgw_conf_disp_val(val->disp, val, f);
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

  if (conn->host == NULL) {
    syslog(LOG_ERR, "mqtt host name not given.");
    return -1;
  }

  return uvrgw_conf_config_childs(cfg, "value", &conn->values_count, (void **) &conn->values, sizeof(MQTT_VAL_T), conn, value_configure);
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  MQTT_VAL_T *val = (MQTT_VAL_T *) child;

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

  if (val->topic == NULL) {
    syslog(LOG_ERR, "mqtt value topic not given.");
    return -1;
  }

  if (val->dir == UVRGW_CONF_VAL_DIR_OUT && val->type == UVRGW_CONF_MQTT_TYPE_NUMBER && val->fmt == NULL) {
    syslog(LOG_ERR, "mqtt value fmt not given.");
    return -1;
  }

  if (val->fmt != NULL) {
    int conv_count = 0;
    const char *p = val->fmt;
    while (*p) {
      if (*p == '%') {
        p++;
        if (*p == '%') {
          p++;
          continue;
        }
        if (*p == '\0') {
          syslog(LOG_ERR, "mqtt value '%s' fmt contains trailing '%%'.", val->name);
          return -1;
        }
        conv_count++;
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
          p++;
          while (*p >= '0' && *p <= '9') p++;
        }
        /* skip optional length modifiers: h, hh, l, ll, L */
        if (*p == 'h') {
          p++;
          if (*p == 'h') p++;
        } else if (*p == 'l') {
          p++;
          if (*p == 'l') p++;
        } else if (*p == 'L') {
          p++;
        }
        if (*p != 'f' && *p != 'e' && *p != 'E' && *p != 'g' && *p != 'G') {
          syslog(LOG_ERR, "mqtt value '%s' fmt contains non-float conversion specifier.", val->name);
          return -1;
        }
      }
      p++;
    }
    if (conv_count != 1) {
      syslog(LOG_ERR, "mqtt value '%s' fmt must contain exactly one conversion specifier.", val->name);
      return -1;
    }
  }

  val->disp = uvrgw_conf_get_dispatcher(val->name, (val->dir == UVRGW_CONF_VAL_DIR_OUT));

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

  return 0;

fail1:
  mqtt_shutdown();
fail0:
  return -1;
}

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

  if (mosquitto_connect_async(conn->mosq, conn->host, conn->port, conn->keepalive_period)) {
    syslog(LOG_INFO, "initial mqtt connection failed");
  }

  if (mosquitto_loop_start(conn->mosq)) {
    syslog(LOG_ERR, "Failed to start mosquitto thread");
    goto fail3;
  }

  return 0;

fail3:
  mosquitto_disconnect(conn->mosq);
fail2:
  mosquitto_destroy(conn->mosq);
  conn->mosq = NULL;
fail1:
  return -1;
}

void mqtt_shutdown(void) {
  MQTT_CONN_T *conn;
  int conn_idx;

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    conn_shutdown(conn);
  }

  mosquitto_lib_cleanup();
}

static void conn_shutdown(MQTT_CONN_T *conn) {
  if (conn->mosq != NULL) {
    if (conn->connected) {
      if (conn->state_topic != NULL) {
        mosquitto_publish(conn->mosq, NULL, conn->state_topic, CONST_STR_PAYLOAD("OFF"), conn->qos, conn->retain);
      }
      mosquitto_disconnect(conn->mosq);
    }
    mosquitto_loop_stop(conn->mosq, false);
    mosquitto_destroy(conn->mosq);
  }
}

static int send_value(void *v, double f) {
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
      if (len > sizeof(buf)) {
        err = MOSQ_ERR_PAYLOAD_SIZE;
      } else {
        err = mosquitto_publish(conn->mosq, NULL, val->topic, len, buf, val->qos, val->retain);
      }
      break;

    default:
      err = MOSQ_ERR_UNKNOWN;
  }

  if (err != MOSQ_ERR_SUCCESS) {
    syslog(LOG_ERR, "mqtt error %d on send.", err);
    return -1;
  }

  return 0;
}

