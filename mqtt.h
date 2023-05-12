#ifndef _MQTT_H_
#define _MQTT_H_

#include <ioconf.h>

#include <stdbool.h>
#include <sys/select.h>
#include <mosquitto.h>

typedef struct {
  const char *name;
  int dir;
  int type;
  const char *topic;
  const char *fmt;
  int qos;
  bool retain;
} MQTT_VAL_T;

typedef struct {
  const char *host;
  int port;
  const char *client_id;
  const char *user;
  const char *pwd;
  const char *state_topic;
  int qos;
  bool retain;

  int values_count;
  MQTT_VAL_T *values;

  struct mosquitto *mosq;
} MQTT_CONN_T;

int mqtt_startup(const char *host, int port, const char *client_id, const char *username, const char *password);
void mqtt_shutdown(void);

int mqtt_publish_chan(const IOCONF_CHAN_T *chan, double val);

#endif

