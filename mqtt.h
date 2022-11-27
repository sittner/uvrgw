#ifndef _MQTT_H_
#define _MQTT_H_

#include <ioconf.h>

#include <stdint.h>
#include <sys/select.h>

#define MQTT_BITNAMES_EOL "<EOL>"

int mqtt_startup(const char *host, int port, const char *client_id, const char *username, const char *password);
void mqtt_shutdown(void);

int mqtt_publish_chan(const IOCONF_CHAN_T *chan, double val);

#endif

