#ifndef _CAN_H_
#define _CAN_H_

#include <ioconf.h>

#include <linux/can.h>
#include <sys/select.h>

int can_startup(const char *ifname);
void can_shutdown(void);
void can_update_fds(fd_set *fd_set);
int can_handler(fd_set *fd_set);

int can_send_chan(const IOCONF_CHAN_T *chan, double val);

#endif

