#include "mqtt.h"

#include <stdio.h>
#include <syslog.h>
#include <fcntl.h>
#include <mosquitto.h>

#define KEEPALIVE_PERIOD 300
#define RECONNECT_TIME 30

static struct mosquitto *mosq = NULL;
static int reconnect_timer = 0;

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

  if (mosquitto_connect(mosq, host, port, KEEPALIVE_PERIOD)) {
    syslog(LOG_INFO, "initial mqtt connection failed");
  }

  reconnect_timer = 0;
  return 0;

fail2:
  mosquitto_destroy(mosq);
fail1:
  mosquitto_lib_cleanup();
fail0:
  return -1;
}

void mqtt_shutdown(void) {
  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
}

void mqtt_update_fds(fd_set *read_fd_set, fd_set *write_fd_set) {
  // check for valid socket
  int sock = mosquitto_socket(mosq);
  if (sock == -1 || fcntl(sock, F_GETFD) < 0) {
    if (reconnect_timer == 0) {
      reconnect_timer = RECONNECT_TIME;
    }
    return;
  }

  FD_SET(sock, read_fd_set);  
  if (mosquitto_want_write(mosq)) {
    FD_SET(sock, write_fd_set);  
  }
}

int mqtt_handler(fd_set *read_fd_set, fd_set *write_fd_set) {
  int sock = mosquitto_socket(mosq);

  if (FD_ISSET(sock, read_fd_set)) {
    if (mosquitto_loop_read(mosq, 1)) {
      syslog(LOG_INFO, "mosquitto_loop_read failed");
    }
  }

  if (FD_ISSET(sock, write_fd_set)) {
    if (mosquitto_loop_write(mosq, 1)) {
      syslog(LOG_INFO, "mosquitto_loop_write failed");
    }
  }

  return 0;
}

int mqtt_task(void) {
  if (reconnect_timer > 0) {
    reconnect_timer--;
printf("reconnect_timer %d\n", reconnect_timer);
    if (reconnect_timer == 0) {
      syslog(LOG_INFO, "try mqtt reconnect");
      mosquitto_reconnect(mosq);
    }
    return 0;
  }

  if (mosquitto_loop_misc(mosq) != MOSQ_ERR_SUCCESS) {
    syslog(LOG_INFO, "mosquitto_loop_misc failed");
  }

  return 0;
}

