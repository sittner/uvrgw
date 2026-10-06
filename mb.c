/**
 * @file mb.c
 * @brief Modbus master implementation (RTU and TCP).
 *
 * Provides polling threads for all configured Modbus masters.  Each
 * thread wakes every MASTER_THREAD_PERIOD_US microseconds and calls
 * master_task(), which advances through slaves and blocks in round-robin
 * order, reads register blocks, dispatches values and flushes queued writes.
 *
 * Scale-factor support: an input block can declare one register as the
 * scale factor source for another register.  The final dispatched value
 * is raw × scale × 10^exponent + offset.
 *
 * Bitmask support: multiple values can share the same register; each
 * extracts a different bit.  Bitmask outputs of one register share a local
 * register image (@c valbuf), which starts at 0 and is not read back from
 * the device, so all relevant bits of a written register must be mapped.
 */
#include "mb.h"
#include "mqtt.h"
#include "can.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <asm/ioctls.h>
#include <modbus/modbus.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <pthread.h>
#include <math.h>

#define MASTER_THREAD_PERIOD_US 10000
#define MASTER_RECONNECT_DELAY_MS 1000

static int rtu_masters_count;
static MB_RTU_MASTER_T *rtu_masters;

static int tcp_masters_count;
static MB_TCP_MASTER_T *tcp_masters;

static int master_rtu_configure(cfg_t *cfg, void *ctx, void *child);
static int master_tcp_configure(cfg_t *cfg, void *ctx, void *child);
static int master_configure(cfg_t *cfg, MB_MASTER_T *master);
static int slave_configure(cfg_t *cfg, void *ctx, void *child);
static int block_configure(cfg_t *cfg, void *ctx, void *child);
static MB_SLAVE_VAL_T *find_block_value(MB_BLOCK_T *blk, const char *name);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static void register_disp_cbs_master(MB_MASTER_T *master);
static void unconfigure_master(MB_MASTER_T *master);
static int master_rtu_startup(MB_RTU_MASTER_T *rtu_master);
static int master_tcp_startup(MB_TCP_MASTER_T *tcp_master);
static int master_startup(MB_MASTER_T *master);
static void master_shutdown(MB_MASTER_T *master);
static void *master_thread(void *ptr);
static int master_task(MB_MASTER_T *master, int64_t now);
static int slave_task_read(MB_SLAVE_T *slave, int64_t now);
static int slave_task_write(MB_SLAVE_T *slave, int64_t now);
static int write_schedule(void *v, double f, bool valid);
static int write_execute(MB_SLAVE_VAL_T *val);
static int read_block_bits(MB_BLOCK_T *blk);
static int read_block_registers(MB_BLOCK_T *blk);
static void link_error(MB_MASTER_T *master);
static void dispatch_value(MB_SLAVE_VAL_T *dpval, const uint16_t *regs, double sf);
static int value_reg_count(int type);
static uint32_t get_u32(const MB_SLAVE_VAL_T *val, const uint16_t *regs);
static void set_u32(const MB_SLAVE_VAL_T *val, uint16_t *regs, uint32_t v);
static bool sunspec_is_na(const MB_SLAVE_VAL_T *val, const uint16_t *regs);

void mb_init(void) {
  rtu_masters_count = 0;
  rtu_masters = NULL;

  tcp_masters_count = 0;
  tcp_masters = NULL;
}

int mb_configure(cfg_t *cfg) {
  int err;

  err = uvrgw_conf_config_childs(cfg, "modbus_rtu", &rtu_masters_count, (void **) &rtu_masters, sizeof(MB_RTU_MASTER_T), NULL, master_rtu_configure);
  if (err < 0) {
    return err;
  }

  err = uvrgw_conf_config_childs(cfg, "modbus_tcp", &tcp_masters_count, (void **) &tcp_masters, sizeof(MB_TCP_MASTER_T), NULL, master_tcp_configure);
  if (err < 0) {
    return err;
  }

  return 0;
}

static int master_rtu_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_RTU_MASTER_T *master = (MB_RTU_MASTER_T *) child;

  master->interface = uvrgw_conf_strdup(cfg_getstr(cfg, "interface"));
  master->baud = cfg_getint(cfg, "baud");
  master->parity = cfg_getint(cfg, "parity");
  master->data_bits = cfg_getint(cfg, "data_bits");
  master->stop_bits = cfg_getint(cfg, "stop_bits");
  master->mode = cfg_getint(cfg, "mode");
  master->rts = cfg_getint(cfg, "rts");
  master->rts_delay = cfg_getint(cfg, "rts_delay");

  if (master->interface == NULL) {
    syslog(LOG_ERR, "modbus_rtu inteface name not given.");
    return -1;
  }

  return master_configure(cfg, (MB_MASTER_T *) master);
}

static int master_tcp_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_TCP_MASTER_T *master = (MB_TCP_MASTER_T *) child;

  master->ip = uvrgw_conf_strdup(cfg_getstr(cfg, "ip"));
  master->port = cfg_getint(cfg, "port");
  master->master.tcp = true;

  if (master->ip == NULL) {
    syslog(LOG_ERR, "modbus_tcp ip not given.");
    return -1;
  }

  return master_configure(cfg, (MB_MASTER_T *) master);
}

static int master_configure(cfg_t *cfg, MB_MASTER_T *master) {
  master->separation_time = cfg_getint(cfg, "separation_time");
  master->timeout = cfg_getint(cfg, "timeout");

  pthread_mutex_init(&master->write_lock, NULL);

  return uvrgw_conf_config_childs(cfg, "slave", &master->slaves_count, (void **) &master->slaves, sizeof(MB_SLAVE_T), master, slave_configure);
}

static int slave_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_SLAVE_T *slave = (MB_SLAVE_T *) child;

  slave->master = (MB_MASTER_T *) ctx;

  slave->id = cfg_getint(cfg, "id");
  slave->interval = cfg_getint(cfg, "interval");
  slave->init_value = cfg_getfloat(cfg, "init_value");
  slave->stale_timeout = cfg_getint(cfg, "stale_timeout");

  if (slave->id < 0 || slave->id > 255) {
    syslog(LOG_ERR, "modbus slave id not given or invalid.");
    return -1;
  }

  return uvrgw_conf_config_childs(cfg, "block", &slave->blocks_count, (void **) &slave->blocks, sizeof(MB_BLOCK_T), slave, block_configure);
}

static int block_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_BLOCK_T *blk = (MB_BLOCK_T *) child;
  MB_SLAVE_T *slave = (MB_SLAVE_T *) ctx;
  MB_SLAVE_VAL_T *val, *sf, *search;
  int val_idx, search_idx;

  blk->slave = slave;
  blk->dir = cfg_getint(cfg, "dir");
  blk->regtype = cfg_getint(cfg, "regtype");
  blk->addr = cfg_getint(cfg, "addr");
  blk->count = cfg_getint(cfg, "count");
  blk->sunspec_na = cfg_getbool(cfg, "sunspec_na");
  blk->expect_reg = cfg_getint(cfg, "expect_reg");
  blk->expect_value = cfg_getint(cfg, "expect_value");

  if (blk->dir < 0) {
    syslog(LOG_ERR, "modbus block dir not given.");
    return -1;
  }

  if (blk->regtype < 0) {
    syslog(LOG_ERR, "modbus block regtype not given.");
    return -1;
  }

  if (blk->addr < 0) {
    syslog(LOG_ERR, "modbus block addr not given.");
    return -1;
  }

  if (blk->count <= 0) {
    syslog(LOG_ERR, "modbus block count not given or invalid.");
    return -1;
  }

  if (blk->dir == UVRGW_CONF_VAL_DIR_OUT &&
      (blk->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || blk->regtype == UVRGW_CONF_MB_REG_TYPE_INREG)) {
    syslog(LOG_ERR, "modbus output block must not use read-only regtype inbit/inreg.");
    return -1;
  }

  // checks are only possible on input register blocks
  if ((blk->sunspec_na || blk->expect_reg >= 0 || blk->expect_value >= 0) &&
      (blk->dir != UVRGW_CONF_VAL_DIR_IN || blk->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || blk->regtype == UVRGW_CONF_MB_REG_TYPE_BIT)) {
    syslog(LOG_ERR, "modbus block: sunspec_na and expect_reg/expect_value are only allowed for input register blocks.");
    return -1;
  }

  if ((blk->expect_reg >= 0) != (blk->expect_value >= 0)) {
    syslog(LOG_ERR, "modbus block: expect_reg and expect_value must be given together.");
    return -1;
  }

  if (blk->expect_reg >= blk->count || blk->expect_value > 0xffff) {
    syslog(LOG_ERR, "modbus block: expect_reg %d / expect_value %d out of range.", blk->expect_reg, blk->expect_value);
    return -1;
  }

  if (blk->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || blk->regtype == UVRGW_CONF_MB_REG_TYPE_BIT) {
    int max_count = (blk->dir == UVRGW_CONF_VAL_DIR_IN) ? MODBUS_MAX_READ_BITS : MODBUS_MAX_WRITE_BITS;
    if (blk->count > max_count) {
      syslog(LOG_ERR, "modbus block count %d exceeds maximum %d for bit registers.", blk->count, max_count);
      return -1;
    }
  } else {
    int max_count = (blk->dir == UVRGW_CONF_VAL_DIR_IN) ? MODBUS_MAX_READ_REGISTERS : MODBUS_MAX_WRITE_REGISTERS;
    if (blk->count > max_count) {
      syslog(LOG_ERR, "modbus block count %d exceeds maximum %d for registers.", blk->count, max_count);
      return -1;
    }
  }

  if (uvrgw_conf_config_childs(cfg, "value", &blk->values_count, (void **) &blk->values, sizeof(MB_SLAVE_VAL_T), blk, value_configure) < 0) {
    return -1;
  }

  // validate register indexes and resolve bitmask_base
  for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
    if (val->reg < 0 || val->reg + value_reg_count(val->type) > blk->count) {
      syslog(LOG_ERR, "modbus value '%s' reg %d (%d register(s)) out of block range [0, %d).", val->name, val->reg, value_reg_count(val->type), blk->count);
      return -1;
    }

    // for bitmask values, find the first value at the same register (shared valbuf)
    if (val->type == UVRGW_CONF_MB_TYPE_BITMASK) {
      val->bitmask_base = val;
      for (search = blk->values, search_idx = 0; search_idx < blk->values_count; search++, search_idx++) {
        if (search->reg == val->reg && search->type == UVRGW_CONF_MB_TYPE_BITMASK) {
          val->bitmask_base = search;
          break;
        }
      }
    }
  }

  // resolve scale factor references
  for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
    if (val->sf_name == NULL) {
      continue;
    }

    sf = find_block_value(blk, val->sf_name);
    if (sf == NULL) {
      syslog(LOG_ERR, "modbus scale_factor value '%s' not found in same block.", val->sf_name);
      return -1;
    }

    if (sf->sf_name != NULL) {
      syslog(LOG_ERR, "modbus scale_factor value '%s' must not itself have a scale_factor.", val->sf_name);
      return -1;
    }

    if (sf->type != UVRGW_CONF_MB_TYPE_S16) {
      syslog(LOG_ERR, "modbus scale_factor value '%s' must be a s16 value.", val->sf_name);
      return -1;
    }

    val->sf_source = sf;
  }

  // build output value list
  if (blk->dir == UVRGW_CONF_VAL_DIR_OUT) {
    for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
      val->value_out_next = slave->value_out_head;
      slave->value_out_head = val;
    }
  }

  return 0;
}

static MB_SLAVE_VAL_T *find_block_value(MB_BLOCK_T *blk, const char *name) {
  MB_SLAVE_VAL_T *val;
  int val_idx;

  for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
    if (strcmp(val->name, name) == 0) {
      return val;
    }
  }

  return NULL;
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_SLAVE_VAL_T *val = (MB_SLAVE_VAL_T *) child;
  MB_MASTER_T *master;
  bool bit_block;
  double init_value;
  int stale_timeout;
  int ret;

  val->block = (MB_BLOCK_T *) ctx;

  val->name = uvrgw_conf_strdup(cfg_title(cfg));
  val->sf_name = uvrgw_conf_strdup(cfg_getstr(cfg, "scale_factor"));
  val->reg = cfg_getint(cfg, "reg");
  val->type = cfg_getint(cfg, "type");
  val->bit = cfg_getint(cfg, "bit");
  val->scale = cfg_getfloat(cfg, "scale");
  val->offset = cfg_getfloat(cfg, "offset");
  val->word_swap = cfg_getbool(cfg, "word_swap");

  // input timeout options: value setting, else section setting
  if (cfg_size(cfg, "init_value") > 0) {
    init_value = cfg_getfloat(cfg, "init_value");
  } else {
    init_value = val->block->slave->init_value;
  }

  if (cfg_size(cfg, "stale_timeout") > 0) {
    stale_timeout = cfg_getint(cfg, "stale_timeout");
  } else {
    stale_timeout = val->block->slave->stale_timeout;
  }

  // check value type against block register type
  if (val->type < 0) {
    syslog(LOG_ERR, "modbus value '%s' type not given.", val->name);
    return -1;
  }

  bit_block = (val->block->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || val->block->regtype == UVRGW_CONF_MB_REG_TYPE_BIT);
  if (bit_block != (val->type == UVRGW_CONF_MB_TYPE_BIT)) {
    syslog(LOG_ERR, "modbus value '%s': type bit is required in (and only allowed in) inbit/bit blocks.", val->name);
    return -1;
  }

  // check bit position
  if (val->type == UVRGW_CONF_MB_TYPE_BITMASK) {
    if (val->bit < 0 || val->bit > 15) {
      syslog(LOG_ERR, "modbus value '%s' bit %d out of range [0, 15].", val->name, val->bit);
      return -1;
    }
  } else if (val->bit >= 0) {
    syslog(LOG_ERR, "modbus value '%s': bit is only allowed for bitmask values.", val->name);
    return -1;
  }

  // check word order option
  if (val->word_swap && value_reg_count(val->type) != 2) {
    syslog(LOG_ERR, "modbus value '%s': word_swap is only allowed for 32-bit values.", val->name);
    return -1;
  }

  // check scale factor
  if (val->sf_name != NULL) {
    if (val->block->dir != UVRGW_CONF_VAL_DIR_IN) {
      syslog(LOG_ERR, "modbus value '%s': scale_factor is only allowed for inputs.", val->name);
      return -1;
    }

    if (val->type != UVRGW_CONF_MB_TYPE_S16 && val->type != UVRGW_CONF_MB_TYPE_U16 &&
        val->type != UVRGW_CONF_MB_TYPE_S32 && val->type != UVRGW_CONF_MB_TYPE_U32) {
      syslog(LOG_ERR, "modbus value '%s': scale_factor is only allowed for integer values.", val->name);
      return -1;
    }
  }

  val->disp = uvrgw_conf_get_dispatcher(val->name, (val->block->dir == UVRGW_CONF_VAL_DIR_OUT));
  if (val->disp == NULL) {
    return -1;
  }

  if (val->block->dir == UVRGW_CONF_VAL_DIR_IN) {
    master = val->block->slave->master;
    if (master->tcp) {
      ret = uvrgw_conf_set_producer(val->disp, "modbus_tcp", ((MB_TCP_MASTER_T *) master)->ip, val, true, init_value, stale_timeout);
    } else {
      ret = uvrgw_conf_set_producer(val->disp, "modbus_rtu", ((MB_RTU_MASTER_T *) master)->interface, val, true, init_value, stale_timeout);
    }
    if (ret < 0) {
      return -1;
    }
  }

  return 0;
}

void mb_register_disp_cbs(void) {
  MB_RTU_MASTER_T *rtu_master;
  MB_TCP_MASTER_T *tcp_master;
  int master_idx;

  for (rtu_master = rtu_masters, master_idx = 0; master_idx < rtu_masters_count; rtu_master++, master_idx++) {
    register_disp_cbs_master((MB_MASTER_T *) rtu_master);
  }

  for (tcp_master = tcp_masters, master_idx = 0; master_idx < tcp_masters_count; tcp_master++, master_idx++) {
    register_disp_cbs_master((MB_MASTER_T *) tcp_master);
  }
}

static void register_disp_cbs_master(MB_MASTER_T *master) {
  MB_SLAVE_T *slave;
  int slave_idx;
  MB_BLOCK_T *blk;
  int blk_idx;
  MB_SLAVE_VAL_T *val;
  int val_idx;

  for (slave = master->slaves, slave_idx = 0; slave_idx < master->slaves_count; slave++, slave_idx++) {
    for (blk = slave->blocks, blk_idx = 0; blk_idx < slave->blocks_count; blk++, blk_idx++) {
      for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
        if (blk->dir == UVRGW_CONF_VAL_DIR_OUT) {
          uvrgw_conf_register_disp_cb(val->disp, val, write_schedule);
        }
      }
    }
  }
}

void mb_unconfigure(void) {
  MB_RTU_MASTER_T *rtu_master;
  MB_TCP_MASTER_T *tcp_master;
  int master_idx;

  for (rtu_master = rtu_masters, master_idx = 0; master_idx < rtu_masters_count; rtu_master++, master_idx++) {
    free((void *) rtu_master->interface);
    unconfigure_master((MB_MASTER_T *) rtu_master);
  }
  free(rtu_masters);

  for (tcp_master = tcp_masters, master_idx = 0; master_idx < tcp_masters_count; tcp_master++, master_idx++) {
    free((void *) tcp_master->ip);
    unconfigure_master((MB_MASTER_T *) tcp_master);
  }
  free(tcp_masters);
}

static void unconfigure_master(MB_MASTER_T *master) {
  MB_SLAVE_T *slave;
  int slave_idx;
  MB_BLOCK_T *blk;
  int blk_idx;
  MB_SLAVE_VAL_T *val;
  int val_idx;

  for (slave = master->slaves, slave_idx = 0; slave_idx < master->slaves_count; slave++, slave_idx++) {
    for (blk = slave->blocks, blk_idx = 0; blk_idx < slave->blocks_count; blk++, blk_idx++) {
      for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
        free((void *) val->name);
        free((void *) val->sf_name);
      }
      free(blk->values);
    }
    free(slave->blocks);
  }

  pthread_mutex_destroy(&master->write_lock);
  free(master->slaves);
}

int mb_startup(void) {
  MB_RTU_MASTER_T *rtu_master;
  MB_TCP_MASTER_T *tcp_master;
  int master_idx;

  for (rtu_master = rtu_masters, master_idx = 0; master_idx < rtu_masters_count; rtu_master++, master_idx++) {
    if (master_rtu_startup(rtu_master) < 0) {
      mb_shutdown();
      return -1;
    }
  }

  for (tcp_master = tcp_masters, master_idx = 0; master_idx < tcp_masters_count; tcp_master++, master_idx++) {
    if (master_tcp_startup(tcp_master) < 0) {
      mb_shutdown();
      return -1;
    }
  }

  return 0;
}

static int master_rtu_startup(MB_RTU_MASTER_T *rtu_master) {
  MB_MASTER_T *master = (MB_MASTER_T *) rtu_master;

  master->ctx = modbus_new_rtu(rtu_master->interface, rtu_master->baud, (char) rtu_master->parity, rtu_master->data_bits, rtu_master->stop_bits);
  if (master->ctx == NULL) {
    syslog(LOG_ERR, "Could not create modbus RTU instance");
    goto fail0;
  }

  if (modbus_set_response_timeout(master->ctx, master->timeout / 1000, (master->timeout % 1000) * 1000) < 0) {
    syslog(LOG_ERR, "Could not set modbus response timeout");
    goto fail1;
  }

  // RTU master is connected the whole time, so open here
  if (modbus_connect(master->ctx) < 0) {
    syslog(LOG_ERR, "Could not open modbus device");
    goto fail1;
  }

  if (modbus_rtu_set_serial_mode(master->ctx, rtu_master->mode)) {
    syslog(LOG_ERR, "Could not set modbus RS485 mode");
    goto fail2;
  }

  if (modbus_rtu_set_rts(master->ctx, rtu_master->rts)) {
    syslog(LOG_ERR, "Could not set modbus RTS mode");
    goto fail2;
  }

  if (rtu_master->rts_delay >= 0) {
    if (modbus_rtu_set_rts_delay(master->ctx, rtu_master->rts_delay)) {
      syslog(LOG_ERR, "Could not set modbus RTS delay");
      goto fail2;
    }
  }

  if (master_startup(master) < 0) {
    goto fail2;
  }

  return 0;

fail2:
  modbus_close(master->ctx);
fail1:
  modbus_free(master->ctx);
  master->ctx = NULL;
fail0:
  return -1;
}

static int master_tcp_startup(MB_TCP_MASTER_T *tcp_master) {
  MB_MASTER_T *master = (MB_MASTER_T *) tcp_master;

  master->ctx = modbus_new_tcp(tcp_master->ip, tcp_master->port);
  if (master->ctx == NULL) {
    syslog(LOG_ERR, "Could not create modbus TCP instance");
    goto fail0;
  }

  if (modbus_set_response_timeout(master->ctx, master->timeout / 1000, (master->timeout % 1000) * 1000) < 0) {
    syslog(LOG_ERR, "Could not set modbus response timeout");
    goto fail1;
  }

  // connect on demand in master thread (libmodbus link recovery is not
  // used, as it retries endlessly while the server is unreachable)
  master->reconnect = true;

  if (master_startup(master) < 0) {
    goto fail1;
  }

  return 0;

fail1:
  modbus_free(master->ctx);
  master->ctx = NULL;
fail0:
  return -1;
}

static int master_startup(MB_MASTER_T *master) {
  if (master->slaves_count > 0) {
    master->thread_running = true;
    if (pthread_create(&(master->thread), NULL, master_thread, (void*) master) != 0) {
      master->thread_running = false;
      syslog(LOG_ERR, "failed to start master thread");
      goto fail0;
    }
  }

  return 0;

fail0:
  return -1;
}

void mb_shutdown(void) {
  MB_RTU_MASTER_T *rtu_master;
  MB_TCP_MASTER_T *tcp_master;
  int master_idx;

  for (rtu_master = rtu_masters, master_idx = 0; master_idx < rtu_masters_count; rtu_master++, master_idx++) {
    master_shutdown((MB_MASTER_T *) rtu_master);
  }

  for (tcp_master = tcp_masters, master_idx = 0; master_idx < tcp_masters_count; tcp_master++, master_idx++) {
    master_shutdown((MB_MASTER_T *) tcp_master);
  }
}

static void master_shutdown(MB_MASTER_T *master) {
  if (master->thread_running) {
    master->thread_running = false;
    pthread_join(master->thread, NULL);
  }
  if (master->ctx != NULL) {
    modbus_close(master->ctx);
    modbus_free(master->ctx);
    master->ctx = NULL;
  }
}

/**
 * @brief Polling thread entry point for one Modbus master.
 *
 * Loops at MASTER_THREAD_PERIOD_US intervals, calling master_task()
 * whenever the @c next_transaction timer has expired, until
 * @c thread_running is cleared by master_shutdown().
 *
 * @param ptr  @c MB_MASTER_T pointer cast to void *.
 * @return     NULL.
 */
static void *master_thread(void *ptr) {
  MB_MASTER_T *master = (MB_MASTER_T *) ptr;
  int64_t now;

  while (master->thread_running) {
    now = utl_get_ticks();
    if (master->next_transaction <= now) {
      // keep separation time after the end of every transaction, also on failed ones
      if (master_task(master, now) != 0) {
        master->next_transaction = utl_get_ticks() + master->separation_time;
      }
    }
    usleep(MASTER_THREAD_PERIOD_US);
  }

  return NULL;
}

/**
 * @brief Single poll iteration for one master.
 *
 * Advances through slaves and processes one read and one write
 * per call.  Moves to the next slave once all blocks and pending
 * writes for the current slave are exhausted.
 *
 * @param master  Master to service.
 * @param now     Current monotonic timestamp (ms).
 * @return        Positive if a Modbus transaction was issued,
 *                0 if nothing was done, -1 on error.
 */
static int master_task(MB_MASTER_T *master, int64_t now) {
  MB_SLAVE_T *slave;
  MB_TCP_MASTER_T *tcp_master;
  int ret;

  // (re)connect TCP master on demand
  if (master->reconnect) {
    tcp_master = (MB_TCP_MASTER_T *) master;
    if (modbus_connect(master->ctx) < 0) {
      if (!master->connect_failed) {
        syslog(LOG_WARNING, "Could not connect to MODBUS TCP server %s:%d: %s", tcp_master->ip, tcp_master->port, modbus_strerror(errno));
        master->connect_failed = true;
      }
      master->next_transaction = utl_get_ticks() + MASTER_RECONNECT_DELAY_MS;
      return 0;
    }
    if (master->connect_failed) {
      syslog(LOG_INFO, "Connected to MODBUS TCP server %s:%d", tcp_master->ip, tcp_master->port);
      master->connect_failed = false;
    }
    master->reconnect = false;
  }

  // get current slave
  if (master->slave_curr_idx < master->slaves_count) {
    slave = &(master->slaves[master->slave_curr_idx]);

    // process reads (only one request per timer period)
    ret = slave_task_read(slave, now);
    if (ret != 0) {
      return ret;
    }

    // process writes (only one request per timer period)
    ret = slave_task_write(slave, now);
    if (ret != 0) {
      return ret;
    }

    // all slave IOs done: reset task state and go to next
    slave->block_read_idx = 0;
    slave->value_out_curr = slave->value_out_head;
    (master->slave_curr_idx)++;
  }

  if (master->slave_curr_idx >= master->slaves_count) {
    master->slave_curr_idx = 0;
  }

  return 0;
}

/**
 * @brief Read one input block from @p slave if the poll timer has expired.
 *
 * Reads the next un-read IN-direction block; if all blocks have been
 * read, resets the poll timer.  A failing block does not block the
 * following ones: on Modbus exceptions or protocol errors reading
 * continues with the next block; only if the slave does not respond at
 * all (e.g. timeout) the remaining blocks are skipped until the next poll.
 *
 * @param slave  Slave to service.
 * @param now    Current monotonic timestamp (ms).
 * @return       1 if a read was performed, 0 if nothing was done,
 *               -1 on Modbus error.
 */
static int slave_task_read(MB_SLAVE_T *slave, int64_t now) {
  MB_BLOCK_T *blk;
  int ret;
  int err;

  // check poll timer
  if (slave->next_poll > now) {
    return 0;
  }

  // find next input block
  while (slave->block_read_idx < slave->blocks_count) {
    blk = &slave->blocks[slave->block_read_idx];
    if (blk->dir == UVRGW_CONF_VAL_DIR_IN) {
      break;
    }
    slave->block_read_idx++;
  }

  // all input blocks done
  if (slave->block_read_idx >= slave->blocks_count) {
    slave->next_poll = now + slave->interval;
    return 0;
  }

  // read registers or bits
  if (blk->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || blk->regtype == UVRGW_CONF_MB_REG_TYPE_BIT) {
    ret = read_block_bits(blk);
  } else {
    ret = read_block_registers(blk);
  }

  // always advance, so a failing block does not starve the following ones
  slave->block_read_idx++;

  if (ret < 0) {
    err = errno;
    syslog(LOG_WARNING, "Failed to read MODBUS block of slave %d (start %d, len %d): %s", slave->id, blk->addr, blk->count, modbus_strerror(err));

    // no response from slave (e.g. timeout, connection lost): skip remaining blocks until next poll
    if (err < MODBUS_ENOBASE) {
      slave->next_poll = now + slave->interval;
      link_error(slave->master);
    }
    return -1;
  }

  return 1;
}

/**
 * @brief Write one pending output value from @p slave's pending list.
 *
 * Walks @c value_out_curr until a value with @c write_pending is found,
 * then calls write_execute().
 *
 * @param slave  Slave to service.
 * @param now    Current monotonic timestamp (ms, unused but kept for API consistency).
 * @return       1 if a write was performed, 0 if nothing was pending,
 *               -1 on Modbus error.
 */
static int slave_task_write(MB_SLAVE_T *slave, int64_t now) {
  int ret;
  MB_SLAVE_VAL_T *val;

  // process next pending value
  while (true) {
    val = slave->value_out_curr;
    if (val == NULL) {
      return 0;
    }
    slave->value_out_curr = val->value_out_next;

    // execute write, if pending (only one request per timer period)
    ret = write_execute(val);
    if (ret != 0) {
      return ret;
    }
  }
}

/**
 * @brief Read all coils or discrete inputs of a block and dispatch values.
 *
 * Calls modbus_read_input_bits() or modbus_read_bits() as appropriate
 * and dispatches each value.
 *
 * @param blk  Block to read.
 * @return     0 on success, -1 on Modbus error.
 */
static int read_block_bits(MB_BLOCK_T *blk) {
  MB_SLAVE_T *slave = blk->slave;
  MB_MASTER_T *master = slave->master;
  MB_SLAVE_VAL_T *val;
  int val_idx;
  uint8_t buf[blk->count];
  int ret;

  // set slave address
  ret = modbus_set_slave(master->ctx, slave->id);
  if (ret < 0) {
    return ret;
  }

  if (blk->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT) {
    ret = modbus_read_input_bits(master->ctx, blk->addr, blk->count, buf);
  } else {
    ret = modbus_read_bits(master->ctx, blk->addr, blk->count, buf);
  }

  if (ret < 0) {
    return ret;
  }

  for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
    uvrgw_conf_disp_val(val->disp, val, buf[val->reg] ? 1.0 : 0.0, true);
  }

  return 0;
}

/**
 * @brief Read all holding or input registers of a block and dispatch values.
 *
 * Calls modbus_read_input_registers() or modbus_read_registers() as
 * appropriate, then calls dispatch_value() for each value.  Scale-factor
 * values are applied as a decimal exponent (10^exponent) before dispatch.
 *
 * @param blk  Block to read.
 * @return     0 on success, -1 on Modbus error.
 */
static int read_block_registers(MB_BLOCK_T *blk) {
  MB_SLAVE_T *slave = blk->slave;
  MB_MASTER_T *master = slave->master;
  MB_SLAVE_VAL_T *val;
  int val_idx;
  uint16_t buf[blk->count];
  int ret;
  double sf;

  // set slave address
  ret = modbus_set_slave(master->ctx, slave->id);
  if (ret < 0) {
    return ret;
  }

  if (blk->regtype == UVRGW_CONF_MB_REG_TYPE_INREG) {
    ret = modbus_read_input_registers(master->ctx, blk->addr, blk->count, buf);
  } else {
    ret = modbus_read_registers(master->ctx, blk->addr, blk->count, buf);
  }

  if (ret < 0) {
    return ret;
  }

  // check expected register value (e.g. SunSpec model ID)
  if (blk->expect_reg >= 0) {
    if (buf[blk->expect_reg] != blk->expect_value) {
      if (!blk->unexpected) {
        syslog(LOG_WARNING, "MODBUS block of slave %d (start %d): register %d is %u, expected %d; values ignored.",
          slave->id, blk->addr, blk->addr + blk->expect_reg, buf[blk->expect_reg], blk->expect_value);
        blk->unexpected = true;
      }
      return 0;
    }
    if (blk->unexpected) {
      syslog(LOG_INFO, "MODBUS block of slave %d (start %d): register %d as expected again.", slave->id, blk->addr, blk->addr + blk->expect_reg);
      blk->unexpected = false;
    }
  }

  for (val = blk->values, val_idx = 0; val_idx < blk->values_count; val++, val_idx++) {
    // drop SunSpec "not implemented" values and values with such a scale factor
    if (blk->sunspec_na) {
      if (sunspec_is_na(val, &buf[val->reg]) ||
          (val->sf_source != NULL && buf[val->sf_source->reg] == 0x8000)) {
        continue;
      }
    }

    if (val->sf_source != NULL) {
      sf = pow(10.0, (double) ((int16_t) buf[val->sf_source->reg]));
    } else {
      sf = 1.0;
    }
    dispatch_value(val, &buf[val->reg], sf);
  }

  return 0;
}

/**
 * @brief Check for a SunSpec "not implemented" value.
 *
 * @param val   Value descriptor.
 * @param regs  Raw registers of the value.
 * @return      true if the raw value is the "not implemented" marker of its
 *              type (f32 NaN is dropped by dispatch_value() anyway).
 */
static bool sunspec_is_na(const MB_SLAVE_VAL_T *val, const uint16_t *regs) {
  switch (val->type) {
    case UVRGW_CONF_MB_TYPE_S16:
      return regs[0] == 0x8000;
    case UVRGW_CONF_MB_TYPE_U16:
    case UVRGW_CONF_MB_TYPE_BITMASK:
      return regs[0] == 0xffff;
    case UVRGW_CONF_MB_TYPE_S32:
      return get_u32(val, regs) == 0x80000000;
    case UVRGW_CONF_MB_TYPE_U32:
      return get_u32(val, regs) == 0xffffffff;
    default:
      return false;
  }
}

/**
 * @brief Convert raw register value(s) and dispatch via the value dispatcher.
 *
 * Applies the scale factor @p sf, the value's own @c scale and @c offset
 * for numeric types.  For bitmask values, extracts the configured bit.
 * Non-finite results (a float NaN is "not implemented" in SunSpec) are
 * dropped, so they count as no data.
 *
 * @param dpval  Value descriptor.
 * @param regs   Raw registers of the value (1 or 2, see value_reg_count()).
 * @param sf     Scale-factor multiplier (1.0 if no scale-factor register).
 */
static void dispatch_value(MB_SLAVE_VAL_T *dpval, const uint16_t *regs, double sf) {
  double raw, f;
  uint32_t u32;
  float f32;

  switch (dpval->type) {
    case UVRGW_CONF_MB_TYPE_S16:
      raw = (double) ((int16_t) regs[0]);
      break;
    case UVRGW_CONF_MB_TYPE_U16:
      raw = (double) regs[0];
      break;
    case UVRGW_CONF_MB_TYPE_S32:
      raw = (double) ((int32_t) get_u32(dpval, regs));
      break;
    case UVRGW_CONF_MB_TYPE_U32:
      raw = (double) get_u32(dpval, regs);
      break;
    case UVRGW_CONF_MB_TYPE_F32:
      u32 = get_u32(dpval, regs);
      memcpy(&f32, &u32, sizeof(f32));
      raw = (double) f32;
      break;
    case UVRGW_CONF_MB_TYPE_BITMASK:
      uvrgw_conf_disp_val(dpval->disp, dpval, (regs[0] & (1 << dpval->bit)) ? 1.0 : 0.0, true);
      return;
    default:
      return;
  }

  f = raw * sf * dpval->scale + dpval->offset;
  if (!isfinite(f)) {
    return;
  }

  uvrgw_conf_disp_val(dpval->disp, dpval, f, true);
}

/**
 * @brief Number of registers occupied by a value type.
 *
 * @param type  One of the UVRGW_CONF_MB_TYPE_* constants.
 * @return      2 for 32-bit types, 1 otherwise.
 */
static int value_reg_count(int type) {
  switch (type) {
    case UVRGW_CONF_MB_TYPE_S32:
    case UVRGW_CONF_MB_TYPE_U32:
    case UVRGW_CONF_MB_TYPE_F32:
      return 2;
    default:
      return 1;
  }
}

/**
 * @brief Combine two registers to a 32-bit value.
 *
 * @param val   Value descriptor (for word order).
 * @param regs  Two raw registers.
 * @return      32-bit value (high word first unless @c word_swap is set).
 */
static uint32_t get_u32(const MB_SLAVE_VAL_T *val, const uint16_t *regs) {
  if (val->word_swap) {
    return ((uint32_t) regs[1] << 16) | regs[0];
  }
  return ((uint32_t) regs[0] << 16) | regs[1];
}

/**
 * @brief Split a 32-bit value into two registers.
 *
 * @param val   Value descriptor (for word order).
 * @param regs  Output: two raw registers.
 * @param v     32-bit value.
 */
static void set_u32(const MB_SLAVE_VAL_T *val, uint16_t *regs, uint32_t v) {
  uint16_t hi = v >> 16;
  uint16_t lo = v & 0xffff;

  if (val->word_swap) {
    regs[0] = lo;
    regs[1] = hi;
  } else {
    regs[0] = hi;
    regs[1] = lo;
  }
}

/**
 * @brief Dispatch callback that queues a value for Modbus write.
 *
 * Thread-safe: acquires @c write_lock before updating @c write_value
 * and @c write_pending.  The actual Modbus write is performed by
 * write_execute() in the polling thread.
 *
 * @param v      @c MB_SLAVE_VAL_T pointer.
 * @param f      Value to write.
 * @param valid  Validity (unused, outputs write every value).
 * @return       0.
 */
static int write_schedule(void *v, double f, bool valid) {
  MB_SLAVE_VAL_T *val = (MB_SLAVE_VAL_T *) v;
  MB_SLAVE_T *slave = val->block->slave;
  MB_MASTER_T *master = slave->master;

  pthread_mutex_lock(&master->write_lock);
  val->write_value = f;
  val->write_pending = true;
  pthread_mutex_unlock(&master->write_lock);

  return 0;
}

/**
 * @brief Execute a pending Modbus write for one value.
 *
 * Atomically reads and clears @c write_pending, then issues the
 * appropriate modbus_write_bit() or modbus_write_register() call.
 * For bitmask values the shared @c valbuf is updated before writing.
 *
 * @param val  Value to write.
 * @return     1 if a write was issued, 0 if nothing was pending,
 *             -1 on Modbus error.
 */
static int write_execute(MB_SLAVE_VAL_T *val) {
  MB_SLAVE_T *slave = val->block->slave;
  MB_MASTER_T *master = slave->master;
  int addr = val->block->addr + val->reg;
  bool pending;
  double f;
  uint16_t *buf;
  uint16_t regs[2];
  float f32;
  uint32_t u32;
  int ret;
  int err;

  pthread_mutex_lock(&master->write_lock);
  pending = val->write_pending;
  f = val->write_value;
  val->write_pending = false;
  pthread_mutex_unlock(&master->write_lock);

  // check for pending write
  if (!pending) {
    return 0;
  }

  // set slave address
  if (modbus_set_slave(master->ctx, slave->id) < 0) {
    syslog(LOG_WARNING, "Failed to set MODBUS slave id %d", slave->id);
    return -1;
  }

  switch (val->type) {
    case UVRGW_CONF_MB_TYPE_BIT:
      ret = modbus_write_bit(master->ctx, addr, (f > 0.5));
      break;
    case UVRGW_CONF_MB_TYPE_S16:
      f = (f - val->offset) / val->scale;
      ret = modbus_write_register(master->ctx, addr, (int16_t) utl_val_limit(f, INT16_MIN, INT16_MAX));
      break;
    case UVRGW_CONF_MB_TYPE_U16:
      f = (f - val->offset) / val->scale;
      ret = modbus_write_register(master->ctx, addr, (uint16_t) utl_val_limit(f, 0.0, UINT16_MAX));
      break;
    case UVRGW_CONF_MB_TYPE_S32:
      f = (f - val->offset) / val->scale;
      set_u32(val, regs, (uint32_t) ((int32_t) utl_val_limit(f, INT32_MIN, INT32_MAX)));
      ret = modbus_write_registers(master->ctx, addr, 2, regs);
      break;
    case UVRGW_CONF_MB_TYPE_U32:
      f = (f - val->offset) / val->scale;
      set_u32(val, regs, (uint32_t) utl_val_limit(f, 0.0, UINT32_MAX));
      ret = modbus_write_registers(master->ctx, addr, 2, regs);
      break;
    case UVRGW_CONF_MB_TYPE_F32:
      f32 = (float) ((f - val->offset) / val->scale);
      memcpy(&u32, &f32, sizeof(u32));
      set_u32(val, regs, u32);
      ret = modbus_write_registers(master->ctx, addr, 2, regs);
      break;
    case UVRGW_CONF_MB_TYPE_BITMASK:
      buf = &val->bitmask_base->valbuf;
      if (f > 0.5) {
        *buf |= (1 << val->bit);
      } else {
        *buf &= ~(1 << val->bit);
      }
      ret = modbus_write_register(master->ctx, addr, *buf);
      break;
    default:
      return 0;
  }

  if (ret < 0) {
    err = errno;
    syslog(LOG_WARNING, "Failed to write MODBUS value '%s' to slave %d (addr %d): %s", val->name, slave->id, addr, modbus_strerror(err));
    if (err < MODBUS_ENOBASE) {
      link_error(master);
    }
    return -1;
  }

  return 1;
}

/**
 * @brief Handle a transaction without valid response (timeout, I/O error).
 *
 * For TCP masters the connection is closed, so it is re-established
 * before the next transaction (this also discards late responses).
 *
 * @param master  Master the failed transaction belongs to.
 */
static void link_error(MB_MASTER_T *master) {
  if (master->tcp) {
    modbus_close(master->ctx);
    master->reconnect = true;
  }
}

