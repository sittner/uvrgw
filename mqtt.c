#include "mqtt.h"
#include "can.h"
#include "mb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <mosquitto.h>

#define KEEPALIVE_PERIOD 300

#define CONST_STR_PAYLOAD(s) (sizeof(s) - 1), s

static struct mosquitto *mosq = NULL;

static void connect_callback(struct mosquitto *mosq, void *obj, int result) {
  const IOCONF_CHAN_T *chan;

  mosquitto_publish(mosq, NULL, "uvr/status", CONST_STR_PAYLOAD("ON"), 1, true);

  for (chan = ioconf_tab; chan->topic != NULL; chan++) {
    if (chan->subscribe) {
      mosquitto_subscribe(mosq, NULL, chan->topic, 0);
    }
  }
}

static void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
  const IOCONF_CHAN_T *chan;
  char buf[32];
  double val;

  // search for topic
  for (chan = ioconf_tab; ; chan++) {
    if (chan->topic == NULL) {
      return;
    }
    if (chan->subscribe && strcmp(chan->topic, msg->topic) == 0) {
      break;
    }
  }

  // check vor maximum payload length
  if (msg->payloadlen >= (sizeof(buf) - 1)) {
    return;
  }

  // get payload as string
  memcpy(buf, msg->payload, msg->payloadlen);
  buf[msg->payloadlen] = 0;


  val = 0.0;
  switch (chan->type) {
    case IOCONF_CHAN_TYPE_SWITCH:
      if (strcmp("ON", buf) == 0) {
        val = 1.0;
      }
      break;
    case IOCONF_CHAN_TYPE_CONTACT:
      if (strcmp("CLOSED", buf) == 0) {
        val = 1.0;
      }
      break;
    case IOCONF_CHAN_TYPE_NUMBER:
      val = strtod(buf, NULL);
      break;
  }

  // send CAN message
  can_send_chan(chan, val);

  // write MODBUS
  mb_write_chan(chan, val);
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

int mqtt_publish_chan(const IOCONF_CHAN_T *chan, double val) {
  char buf[32];
  int len;

  switch (chan->type) {
    case IOCONF_CHAN_TYPE_SWITCH:
      if (val >= 0.5) {
        return mosquitto_publish(mosq, NULL, chan->topic, CONST_STR_PAYLOAD("ON"), 1, true);
      } else {
        return mosquitto_publish(mosq, NULL, chan->topic, CONST_STR_PAYLOAD("OFF"), 1, true);
      }

    case IOCONF_CHAN_TYPE_CONTACT:
      if (val >= 0.5) {
        return mosquitto_publish(mosq, NULL, chan->topic, CONST_STR_PAYLOAD("CLOSED"), 1, true);
      } else {
        return mosquitto_publish(mosq, NULL, chan->topic, CONST_STR_PAYLOAD("OPEN"), 1, true);
      }

    case IOCONF_CHAN_TYPE_NUMBER:
      len = snprintf(buf, sizeof(buf), chan->fmt, val);
      if (len > sizeof(buf)) {
        return MOSQ_ERR_PAYLOAD_SIZE;
      }

      return mosquitto_publish(mosq, NULL, chan->topic, len, buf, 1, true);
  }

  return MOSQ_ERR_UNKNOWN;
}

