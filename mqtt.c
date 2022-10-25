#include "mqtt.h"

#include <stdio.h>
#include <syslog.h>
#include <fcntl.h>
#include <mosquitto.h>

#define KEEPALIVE_PERIOD 300

static struct mosquitto *mosq = NULL;

static void connect_callback(struct mosquitto *mosq, void *obj, int result) {
  printf("connect callback, rc=%d\n", result);

  // TODO
  mosquitto_subscribe(mosq, NULL, "/devices/wb-adc/controls/+", 0);
}

static void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
  bool match = 0;
  printf("got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

  mosquitto_topic_matches_sub("/devices/wb-adc/controls/+", message->topic, &match);
  if (match) {
    printf("got message for ADC topic\n");
  }
}

int mqtt_startup(const char *host, int port, const char *client_id, const char *username, const char *password) {
  if (mosquitto_lib_init()) {
    syslog(LOG_ERR, "Failed to init mosquitto lib");
    goto fail0;
  }

  mosq = mosquitto_new(client_id, true, NULL);
  if (mosq == NULL) {
    syslog(LOG_ERR, "Failed to create mosquitto instance");
    goto fail1;
  }

  if (username != NULL) {
    if (mosquitto_username_pw_set(mosq, username, password)) {
      syslog(LOG_ERR, "Failed to set mosquitto credentials");
      goto fail2;
    }
  }

  mosquitto_connect_callback_set(mosq, connect_callback);
  mosquitto_message_callback_set(mosq, message_callback);

  if (mosquitto_connect_async(mosq, host, port, KEEPALIVE_PERIOD)) {
    syslog(LOG_INFO, "initial mqtt connection failed");
  }

  if (mosquitto_loop_start(mosq)) {
    syslog(LOG_ERR, "Failed to start mosquitto thread");
    goto fail3;
  }

  return 0;

fail3:
  mosquitto_disconnect(mosq);
fail2:
  mosquitto_destroy(mosq);
fail1:
  mosquitto_lib_cleanup();
fail0:
  return -1;
}

void mqtt_shutdown(void) {
  mosquitto_disconnect(mosq);
  mosquitto_loop_stop(mosq, false);
  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
}

