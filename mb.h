#ifndef _MB_H_
#define _MB_H_

#include "uvrgw_conf.h"

#include <modbus/modbus.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct MB_RTU_SLAVE_VAL;
struct MB_RTU_SLAVE;
struct MB_RTU_MASTER;

typedef struct MB_RTU_SLAVE_VAL {
  const char *name;
  int dir;
  int regtype;
  int addr;
  int type;
  int pos;
  double scale;
  double offset;

  struct MB_RTU_SLAVE *slave;

  struct MB_RTU_SLAVE_VAL *prev;
  struct MB_RTU_SLAVE_VAL *next;
  struct MB_RTU_SLAVE_VAL *same_reg;
  struct MB_RTU_SLAVE_VAL *same_base;

  struct MB_RTU_SLAVE_VAL *in_group_same;
  struct MB_RTU_SLAVE_VAL *in_group_next;
  int in_group_count;
  int in_group_index;

  UVRGW_CONF_VAL_DISPATCH_T *disp;
  uint16_t valbuf;

} MB_RTU_SLAVE_VAL_T;

typedef struct MB_RTU_SLAVE {
  int id;
  int interval;
  int max_req_regs;

  struct MB_RTU_MASTER *master;

  int values_count;
  struct MB_RTU_SLAVE_VAL *values;

  struct MB_RTU_SLAVE_VAL *values_head;
  struct MB_RTU_SLAVE_VAL *values_tail;

  struct MB_RTU_SLAVE_VAL *in_group_head;
  struct MB_RTU_SLAVE_VAL *value_in_curr;

  int poll_timer;
  void *input_buf;
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
  struct MB_RTU_SLAVE *slaves;

  modbus_t *ctx;
  pthread_mutex_t bus_lock;
} MB_RTU_MASTER_T;

void mb_init(void);
int mb_configure(cfg_t *cfg);
void mb_register_disp_cbs(void);
void mb_unconfigure(void);

int mb_startup(void);
void mb_shutdown(void);

int mb_task(void);

#endif

