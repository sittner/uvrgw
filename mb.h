#ifndef _MB_H_
#define _MB_H_

#include "ioconf.h"

#include <modbus/modbus.h>
#include <pthread.h>
#include <stdbool.h>

struct MB_RTU_SLAVE_VAL;
struct MB_RTU_SLAVE;
struct MB_RTU_MASTER;

typedef struct MB_RTU_SLAVE_VAL {
  const char *name;
  int dir;
  int addr;
  int type;
  int regtype;
  int pos;
  double scale;
  double offset;

  struct MB_RTU_SLAVE_VAL *prev;
  struct MB_RTU_SLAVE_VAL *next;
} MB_RTU_SLAVE_VAL_T;

typedef struct MB_RTU_SLAVE {
  int id;
  int interval;
  bool many_req;

  int values_count;
  MB_RTU_SLAVE_VAL_T *values;

  MB_RTU_SLAVE_VAL_T *values_head;
  MB_RTU_SLAVE_VAL_T *values_tail;

  int poll_timer;
} MB_RTU_SLAVE_T;

typedef struct MB_RTU_MASTER {
  const char *interface;
  int baud;
  int parity;
  int data_bits;
  int stop_bits;
  int timeout;
  int mode;
  int rts;
  int rts_delay;

  int slaves_count;
  MB_RTU_SLAVE_T *slaves;

  modbus_t *ctx;
  pthread_mutex_t bus_lock;
} MB_RTU_MASTER_T;

int mb_startup(const char *dev, int baud);
void mb_shutdown(void);
int mb_task(void);

int mb_write_chan(const IOCONF_CHAN_T *chan, double val);

#endif

