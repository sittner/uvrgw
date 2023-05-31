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

  struct MB_RTU_SLAVE_VAL *value_out_next;

  UVRGW_CONF_VAL_DISPATCH_T *disp;
  uint16_t valbuf;

  bool write_pending;
  double write_value;

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
  struct MB_RTU_SLAVE_VAL *in_group_curr;

  struct MB_RTU_SLAVE_VAL *value_out_head;
  struct MB_RTU_SLAVE_VAL *value_out_curr;

  int64_t next_poll;
  void *input_buf;
} MB_RTU_SLAVE_T;

typedef struct MB_RTU_MASTER {
  const char *interface;
  int baud;
  int parity;
  int data_bits;
  int stop_bits;
  int separation_time;
  int timeout;
  int mode;
  int rts;
  int rts_delay;

  int slaves_count;
  struct MB_RTU_SLAVE *slaves;

  modbus_t *ctx;
  pthread_mutex_t write_lock;

  pthread_t thread;
  bool thread_running;
  int64_t next_transaction;

  int slave_curr_idx;
} MB_RTU_MASTER_T;

void mb_init(void);
int mb_configure(cfg_t *cfg);
void mb_register_disp_cbs(void);
void mb_unconfigure(void);

int mb_startup(void);
void mb_shutdown(void);

#endif

