#include "mqtt.h"

#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <mosquitto.h>

#define KEEPALIVE_PERIOD 300

#define CONST_STR_PAYLOAD(s) (sizeof(s) - 1), s

static struct mosquitto *mosq = NULL;

static void connect_callback(struct mosquitto *mosq, void *obj, int result) {
  //printf("connect callback, rc=%d\n", result);

  mosquitto_publish(mosq, NULL, "uvr/status", CONST_STR_PAYLOAD("ON"), 1, true);

  // TODO
  //mosquitto_subscribe(mosq, NULL, "uvr/#", 0);
}

static void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
/*
  bool match = 0;
  printf("got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

  mosquitto_topic_matches_sub("uvr/#", message->topic, &match);
  if (match) {
    printf("got message for ADC topic\n");
  }
*/
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

  if (mosquitto_will_set(mosq, "uvr/status", CONST_STR_PAYLOAD("OFF"), 1, true)) {
    syslog(LOG_ERR, "Failed to set mosquitto will");
    goto fail2;
  }

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
  mosquitto_publish(mosq, NULL, "uvr/status", CONST_STR_PAYLOAD("OFF"), 1, true);
  mosquitto_disconnect(mosq);
  mosquitto_loop_stop(mosq, false);
  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
}

int mqtt_publish(const char *topic, const char *payload) {
  return mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, 1, true);
}

int mqtt_publish_float(const char *topic, const char *fmt, double val) {
  char buf[16];
  int len;

  len = snprintf(buf, sizeof(buf), fmt, val);
  if (len > sizeof(buf)) {
    return MOSQ_ERR_PAYLOAD_SIZE;
  }

  return mosquitto_publish(mosq, NULL, topic, len, buf, 1, true);
}

int mqtt_publish_scaled16(const char *topic, const char *fmt, int16_t val, double offset, double scale) {
  return mqtt_publish_float(topic, fmt, ((double) val) * scale + offset);
}

int mqtt_publish_bitmask(const char *topic_fmt, uint32_t val, int bitstart, int bitcnt) {
  int i;
  char topic[64];
  int ret;
  char buf;

  val >>= bitstart;
  for (i = 0; i < bitcnt; i++, val >>= 1) {
    ret = snprintf(topic, sizeof(topic), topic_fmt, i);
    if (ret >= sizeof(topic)) {
      return MOSQ_ERR_NOMEM;
    }

    buf = (val & 1) ? '1' : '0';
    ret = mosquitto_publish(mosq, NULL, topic, sizeof(buf), &buf, 1, true);
    if (ret != MOSQ_ERR_SUCCESS) {
      return ret;
    }
  }

  return MOSQ_ERR_SUCCESS;
}

int mqtt_publish_bitnames(const char *topic_fmt, uint32_t val, int bitstart, const char *names) {
  const char *p;
  char topic[64];
  int ret;
  char buf;

  val >>= bitstart;
  for (p = names; strcmp(p, MQTT_BITNAMES_EOL) != 0; p += strlen(p) + 1, val >>= 1) {
    // skip epmty lines
    if (*p == 0) {
      continue;
    }

    ret = snprintf(topic, sizeof(topic), topic_fmt, p);
    if (ret >= sizeof(topic)) {
      return MOSQ_ERR_NOMEM;
    }

    buf = (val & 1) ? '1' : '0';
    ret = mosquitto_publish(mosq, NULL, topic, sizeof(buf), &buf, 1, true);
    if (ret != MOSQ_ERR_SUCCESS) {
      return ret;
    }
  }

  return MOSQ_ERR_SUCCESS;
}

