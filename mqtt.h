#ifndef _MQTT_H_
#define _MQTT_H_

#include "uvrgw_conf.h"

#include <stdbool.h>
#include <sys/select.h>
#include <mosquitto.h>

struct MQTT_VAL;
struct MQTT_CONN;

typedef struct MQTT_VAL {
  const char *name;
  int dir;
  int type;
  const char *topic;
  const char *fmt;
  int qos;
  bool retain;

  struct MQTT_CONN *conn;

  UVRGW_CONF_VAL_DISPATCH_T *disp;

} MQTT_VAL_T;

typedef struct MQTT_CONN {
  const char *host;
  int port;
  const char *client_id;
  const char *user;
  const char *pwd;
  const char *state_topic;
  int keepalive_period;
  int qos;
  bool retain;

  int values_count;
  struct MQTT_VAL *values;

  struct mosquitto *mosq;

  bool connected;
} MQTT_CONN_T;

void mqtt_init(void);
int mqtt_configure(cfg_t *cfg);
void mqtt_register_disp_cbs(void);
void mqtt_unconfigure(void);

int mqtt_startup(void);
void mqtt_shutdown(void);

#endif

