#ifndef _CAN_H_
#define _CAN_H_

#include "uvrgw_conf.h"

#include <stdbool.h>
#include <pthread.h>
#include <confuse.h>
#include <linux/can.h>
#include <sys/select.h>

struct CAN_VAL;
struct CAN_FRAME;
struct CAN_IFACE;

typedef struct CAN_VAL {
  const char *name;
  int type;
  int pos;
  double scale;
  double offset;

  UVRGW_CONF_VAL_DISPATCH_T *disp;

  struct CAN_FRAME *frame;
} CAN_VAL_T;

typedef struct CAN_FRAME {
  int can_id;
  int dir;

  int values_count;
  struct CAN_VAL *values;

  struct CAN_IFACE *iface;

  struct can_frame send_buf;
  pthread_mutex_t send_buf_mutex;
  bool send_pending;
  int send_timer;
} CAN_FRAME_T;

typedef struct CAN_IFACE {
  const char *interface;
  int timestamp_period;
  int send_timeout;

  int frames_count;
  struct CAN_FRAME *frames;

  int can_fd;
  int timestamp_timer;
} CAN_IFACE_T;

void can_init(void);
int can_configure(cfg_t *cfg);
void can_register_disp_cbs(void);
void can_unconfigure(void);

int can_startup(void);
void can_shutdown(void);

int can_task(void);

void can_update_fds(fd_set *fd_set);
int can_handler(fd_set *fd_set);

#endif

