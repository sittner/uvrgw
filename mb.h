#ifndef _MB_H_
#define _MB_H_

#include "uvrgw_conf.h"

#include <modbus/modbus.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct MB_SLAVE_VAL;
struct MB_BLOCK;
struct MB_SLAVE;
struct MB_MASTER;
struct MB_RTU_MASTER;
struct MB_TCP_MASTER;

typedef struct MB_SLAVE_VAL {
  const char *name;
  int offset;
  int type;
  int pos;
  double scale;
  double val_offset;

  struct MB_BLOCK *block;

  struct MB_SLAVE_VAL *value_out_next;

  UVRGW_CONF_VAL_DISPATCH_T *disp;
  uint16_t valbuf;

  bool write_pending;
  double write_value;

  const char *sf_name;
  struct MB_SLAVE_VAL *sf_source;
  struct MB_SLAVE_VAL *bitmask_base;

} MB_SLAVE_VAL_T;

typedef struct MB_BLOCK {
  int dir;
  int regtype;
  int addr;
  int count;

  int values_count;
  struct MB_SLAVE_VAL *values;

  struct MB_SLAVE *slave;
} MB_BLOCK_T;

typedef struct MB_SLAVE {
  int id;
  int interval;

  struct MB_MASTER *master;

  int blocks_count;
  struct MB_BLOCK *blocks;

  int block_read_idx;

  struct MB_SLAVE_VAL *value_out_head;
  struct MB_SLAVE_VAL *value_out_curr;

  int64_t next_poll;
} MB_SLAVE_T;

typedef struct MB_MASTER {
  int separation_time;
  int timeout;

  int slaves_count;
  struct MB_SLAVE *slaves;

  modbus_t *ctx;
  pthread_mutex_t write_lock;

  pthread_t thread;
  bool thread_running;
  int64_t next_transaction;

  int slave_curr_idx;
} MB_MASTER_T;

typedef struct MB_RTU_MASTER {
  struct MB_MASTER master;

  const char *interface;
  int baud;
  int parity;
  int data_bits;
  int stop_bits;
  int mode;
  int rts;
  int rts_delay;
} MB_RTU_MASTER_T;

typedef struct MB_TCP_MASTER {
  struct MB_MASTER master;

  const char *ip;
  int port;
} MB_TCP_MASTER_T;

void mb_init(void);
int mb_configure(cfg_t *cfg);
void mb_register_disp_cbs(void);
void mb_unconfigure(void);

int mb_startup(void);
void mb_shutdown(void);

#endif

