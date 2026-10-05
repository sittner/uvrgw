/**
 * @file mqtt_logger.c
 * @brief Periodic value snapshots published as JSON via MQTT (for logging).
 *
 * One thread serves all loggers: it takes the snapshots at the aligned
 * wall clock times and publishes them.
 */
#include "mqtt_logger.h"
#include "mqtt.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <syslog.h>
#include <sys/timex.h>

#define LOGGER_THREAD_PERIOD_US 100000
#define LOGGER_NUM_LEN 40

static bool thread_running;
static pthread_t thread;
static MQTT_CONN_T *log_conns;
static int log_conns_count;

static int logger_configure(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static bool valid_field(const char *field);
static void *logger_thread(void *ptr);
static void logger_task(MQTT_LOGGER_T *logger, time_t now);
static void take_snapshot(MQTT_LOGGER_T *logger, time_t t);
static char *build_json(MQTT_LOGGER_T *logger, time_t t);
static bool clock_synced(void);

int mqtt_logger_configure(cfg_t *cfg, MQTT_CONN_T *conn) {
  return uvrgw_conf_config_childs(cfg, "logger", &conn->loggers_count, (void **) &conn->loggers, sizeof(MQTT_LOGGER_T), conn, logger_configure);
}

static int logger_configure(cfg_t *cfg, void *ctx, void *child) {
  MQTT_LOGGER_T *logger = (MQTT_LOGGER_T *) child;
  MQTT_LOGGER_VAL_T *val, *cmp;
  int val_idx, cmp_idx;

  logger->conn = (MQTT_CONN_T *) ctx;

  logger->name = uvrgw_conf_strdup(cfg_title(cfg));
  logger->topic = uvrgw_conf_strdup(cfg_getstr(cfg, "topic"));
  logger->interval = cfg_getint(cfg, "interval");
  logger->qos = cfg_getint(cfg, "qos");

  if (logger->topic == NULL) {
    syslog(LOG_ERR, "mqtt logger '%s': topic not given.", logger->name);
    return -1;
  }

  // the interval must divide a day, so snapshots are aligned to the clock
  if (logger->interval <= 0 || 86400 % logger->interval != 0) {
    syslog(LOG_ERR, "mqtt logger '%s': interval %d invalid (must divide 86400 s).", logger->name, logger->interval);
    return -1;
  }

  if (logger->qos < 0 || logger->qos > 2) {
    syslog(LOG_ERR, "mqtt logger '%s': qos invalid.", logger->name);
    return -1;
  }

  if (uvrgw_conf_config_childs(cfg, "value", &logger->values_count, (void **) &logger->values, sizeof(MQTT_LOGGER_VAL_T), logger, value_configure) < 0) {
    return -1;
  }

  if (logger->values_count == 0) {
    syslog(LOG_ERR, "mqtt logger '%s': no values given.", logger->name);
    return -1;
  }

  // field names must be unique (value names are unique by config parser)
  for (val = logger->values, val_idx = 0; val_idx < logger->values_count; val++, val_idx++) {
    for (cmp = logger->values, cmp_idx = 0; cmp_idx < val_idx; cmp++, cmp_idx++) {
      if (strcmp(val->field, cmp->field) == 0) {
        syslog(LOG_ERR, "mqtt logger '%s': field '%s' used twice.", logger->name, val->field);
        return -1;
      }
    }
  }

  return 0;
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  MQTT_LOGGER_VAL_T *val = (MQTT_LOGGER_VAL_T *) child;
  MQTT_LOGGER_T *logger = (MQTT_LOGGER_T *) ctx;
  const char *field;

  val->name = uvrgw_conf_strdup(cfg_title(cfg));
  field = cfg_getstr(cfg, "field");
  val->field = uvrgw_conf_strdup(field != NULL ? field : val->name);
  val->scale = cfg_getfloat(cfg, "scale");

  if (!valid_field(val->field)) {
    syslog(LOG_ERR, "mqtt logger '%s': field name '%s' invalid (allowed: A-Z a-z 0-9 _, not 'time').", logger->name, val->field);
    return -1;
  }

  val->disp = uvrgw_conf_get_dispatcher(val->name, false);
  if (val->disp == NULL) {
    return -1;
  }

  return 0;
}

static bool valid_field(const char *field) {
  const char *p;

  if (field == NULL || *field == 0 || strcmp(field, "time") == 0) {
    return false;
  }

  for (p = field; *p != 0; p++) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
      return false;
    }
  }

  return true;
}

void mqtt_logger_unconfigure(MQTT_CONN_T *conn) {
  MQTT_LOGGER_T *logger;
  int logger_idx;
  MQTT_LOGGER_VAL_T *val;
  int val_idx;

  for (logger = conn->loggers, logger_idx = 0; logger_idx < conn->loggers_count; logger++, logger_idx++) {
    for (val = logger->values, val_idx = 0; val_idx < logger->values_count; val++, val_idx++) {
      free((void *) val->name);
      free((void *) val->field);
    }
    free(logger->values);
    free((void *) logger->name);
    free((void *) logger->topic);
  }
  free(conn->loggers);
}

int mqtt_logger_startup(MQTT_CONN_T *conns, int conns_count) {
  MQTT_CONN_T *conn;
  int conn_idx;
  MQTT_LOGGER_T *logger;
  int logger_idx;
  bool any = false;
  time_t now = time(NULL);

  log_conns = conns;
  log_conns_count = conns_count;

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    for (logger = conn->loggers, logger_idx = 0; logger_idx < conn->loggers_count; logger++, logger_idx++) {
      logger->next_time = (now / logger->interval + 1) * logger->interval;
      any = true;
    }
  }

  if (!any) {
    return 0;
  }

  thread_running = true;
  if (pthread_create(&thread, NULL, logger_thread, NULL) != 0) {
    thread_running = false;
    syslog(LOG_ERR, "failed to start mqtt logger thread");
    return -1;
  }

  return 0;
}

void mqtt_logger_shutdown(void) {
  if (thread_running) {
    thread_running = false;
    pthread_join(thread, NULL);
  }
}

/**
 * @brief Logger thread: take and publish snapshots.
 *
 * @param ptr  Unused.
 * @return     NULL.
 */
static void *logger_thread(void *ptr) {
  MQTT_CONN_T *conn;
  int conn_idx;
  MQTT_LOGGER_T *logger;
  int logger_idx;

  while (thread_running) {
    for (conn = log_conns, conn_idx = 0; conn_idx < log_conns_count; conn++, conn_idx++) {
      for (logger = conn->loggers, logger_idx = 0; logger_idx < conn->loggers_count; logger++, logger_idx++) {
        logger_task(logger, time(NULL));
      }
    }
    usleep(LOGGER_THREAD_PERIOD_US);
  }

  return NULL;
}

/**
 * @brief Take and publish a snapshot if due.
 *
 * @param logger  Logger.
 * @param now     Current wall clock time.
 */
static void logger_task(MQTT_LOGGER_T *logger, time_t now) {
  time_t t;

  // clock jumped back (more than one interval): realign
  if (logger->next_time - now > logger->interval) {
    logger->next_time = (now / logger->interval + 1) * logger->interval;
  }

  if (now >= logger->next_time) {
    // snapshot time: the due slot, or the latest slot after a clock jump
    // forward (e.g. first NTP sync), so no burst of old slots is logged
    t = logger->next_time;
    if (now - t >= logger->interval) {
      t = (now / logger->interval) * logger->interval;
    }
    logger->next_time = t + logger->interval;

    if (clock_synced()) {
      if (logger->unsynced) {
        syslog(LOG_INFO, "mqtt logger '%s': clock synchronised, snapshots resumed.", logger->name);
        logger->unsynced = false;
      }
      take_snapshot(logger, t);
    } else if (!logger->unsynced) {
      syslog(LOG_WARNING, "mqtt logger '%s': clock not synchronised, snapshots skipped.", logger->name);
      logger->unsynced = true;
    }
  }
}

/**
 * @brief Build and publish a snapshot.
 *
 * MOSQ_ERR_NO_CONN is not a loss for QoS > 0: libmosquitto keeps the
 * message and delivers it after reconnect.
 *
 * @param logger  Logger.
 * @param t       Snapshot time.
 */
static void take_snapshot(MQTT_LOGGER_T *logger, time_t t) {
  char *json;
  int rc;

  json = build_json(logger, t);
  if (json == NULL) {
    syslog(LOG_ERR, "mqtt logger '%s': failed to allocate snapshot.", logger->name);
    return;
  }

  rc = mosquitto_publish(logger->conn->mosq, NULL, logger->topic, strlen(json), json, logger->qos, false);
  free(json);

  if (rc != MOSQ_ERR_SUCCESS) {
    if (!logger->publish_failed) {
      if (rc == MOSQ_ERR_NO_CONN && logger->qos > 0) {
        syslog(LOG_WARNING, "mqtt logger '%s': broker not connected, snapshots are kept until reconnect.", logger->name);
      } else {
        syslog(LOG_WARNING, "mqtt logger '%s': publish failed (%s), snapshots lost.", logger->name, mosquitto_strerror(rc));
      }
      logger->publish_failed = true;
    }
    return;
  }

  if (logger->publish_failed) {
    syslog(LOG_INFO, "mqtt logger '%s': publishing again.", logger->name);
    logger->publish_failed = false;
  }
}

/**
 * @brief Build the JSON snapshot of all logged values.
 *
 * Invalid values and values that are not finite after @c scale are
 * logged as null.
 *
 * @param logger  Logger.
 * @param t       Snapshot time.
 * @return        Allocated JSON string, or NULL on allocation failure.
 */
static char *build_json(MQTT_LOGGER_T *logger, time_t t) {
  MQTT_LOGGER_VAL_T *val;
  int val_idx;
  size_t size, len;
  char *json;
  char num[LOGGER_NUM_LEN];
  double f;
  bool valid;

  // field name + quotes, colon, comma + number
  size = 32;
  for (val = logger->values, val_idx = 0; val_idx < logger->values_count; val++, val_idx++) {
    size += strlen(val->field) + 4 + LOGGER_NUM_LEN;
  }

  json = malloc(size);
  if (json == NULL) {
    return NULL;
  }

  len = snprintf(json, size, "{\"time\":%lld", (long long) t);

  for (val = logger->values, val_idx = 0; val_idx < logger->values_count; val++, val_idx++) {
    valid = uvrgw_conf_get_val(val->disp, &f, NULL);
    f *= val->scale;

    if (valid && isfinite(f)) {
      // always mark as float, so consumers do not infer an integer type
      snprintf(num, sizeof(num), "%.15g", f);
      if (strpbrk(num, ".eE") == NULL) {
        strcat(num, ".0");
      }
    } else {
      strcpy(num, "null");
    }

    len += snprintf(json + len, size - len, ",\"%s\":%s", val->field, num);
  }

  snprintf(json + len, size - len, "}");

  return json;
}

/**
 * @brief Check whether the system clock is synchronised.
 *
 * Uses the kernel time status, which is maintained by ntpd, chrony and
 * systemd-timesyncd.
 *
 * @return  true if synchronised.
 */
static bool clock_synced(void) {
  struct timex tx;

  memset(&tx, 0, sizeof(tx));
  return adjtimex(&tx) != TIME_ERROR && (tx.status & STA_UNSYNC) == 0;
}
