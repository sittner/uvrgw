#ifndef _MQTT_H_
#define _MQTT_H_

#include <stdint.h>
#include <sys/select.h>

#define MQTT_BITNAMES_EOL "<EOL>"

int mqtt_startup(const char *host, int port, const char *client_id, const char *username, const char *password);
void mqtt_shutdown(void);
int mqtt_publish(const char *topic, const char *payload);
int mqtt_publish_float(const char *topic, const char *fmt, double val);
int mqtt_publish_scaled16(const char *topic, const char *fmt, int16_t val, double offset, double scale);
int mqtt_publish_bitmask(const char *topic_fmt, uint32_t val, int bitstart, int bitlen);
int mqtt_publish_bitnames(const char *topic_fmt, uint32_t val, int bitstart, const char *names);

#endif

