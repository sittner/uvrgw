#ifndef _MQTT_H_
#define _MQTT_H_

#include <sys/select.h>

int mqtt_startup(const char *host, int port, const char *client_id, const char *username, const char *password);
void mqtt_shutdown(void);
void mqtt_update_fds(fd_set *read_fd_set, fd_set *write_fd_set);
int mqtt_handler(fd_set *read_fd_set, fd_set *write_fd_set);
int mqtt_task(void);

#endif

