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

#define MASTER_THREAD_PERIOD_US 10000

static int rtu_masters_count;
static MB_RTU_MASTER_T *rtu_masters;

static int tcp_masters_count;
static MB_TCP_MASTER_T *tcp_masters;

static int master_rtu_configure(cfg_t *cfg, void *ctx, void *child);
static int master_tcp_configure(cfg_t *cfg, void *ctx, void *child);
static int master_configure(cfg_t *cfg, MB_MASTER_T *master);
static int slave_configure(cfg_t *cfg, void *ctx, void *child);
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
static int write_schedule(void *v, double f);
static int write_execute(MB_SLAVE_VAL_T *val);
static int read_bits(MB_SLAVE_VAL_T *grp);
static int read_registers(MB_SLAVE_VAL_T *grp);

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

static int value_reg_cmp(MB_SLAVE_VAL_T *a, MB_SLAVE_VAL_T *b) {
  if (a->dir < b->dir) {
    return -1;
  }
  if (a->dir > b->dir) {
    return 1;
  }

  if (a->regtype < b->regtype) {
    return -1;
  }
  if (a->regtype > b->regtype) {
    return 1;
  }

  if (a->addr < b->addr) {
    return -1;
  }
  if (a->addr > b->addr) {
    return 1;
  }

  return 0;
}

static int slave_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_SLAVE_T *slave = (MB_SLAVE_T *) child;
  MB_SLAVE_VAL_T *val, *cmp, *grp;
  int val_idx;
  int res;

  slave->master = (MB_MASTER_T *) ctx;

  slave->id = cfg_getint(cfg, "id");
  slave->interval = cfg_getint(cfg, "interval");
  slave->max_req_regs = cfg_getint(cfg, "max_req_regs");

  if (slave->max_req_regs > 0) {
    slave->input_buf = calloc(slave->max_req_regs, sizeof(uint16_t));
  }

  if (uvrgw_conf_config_childs(cfg, "value", &slave->values_count, (void **) &slave->values, sizeof(MB_SLAVE_VAL_T), slave, value_configure) < 0) {
    return -1;
  }

  // order values by dir/regtype/address
  for (val = slave->values, val_idx = 0; val_idx < slave->values_count; val++, val_idx++) {
    // initialize list
    if (slave->values_head == NULL) {
      slave->values_head = slave->values;
      slave->values_tail = slave->values;
      continue;
    }

    // search for place to insert
    for (res = 0, cmp = slave->values_head; cmp != NULL; cmp = cmp->next) {
      res = value_reg_cmp(cmp, val);
      if (res >= 0) {
        break;
      }
    }

    // check for same reg
    if (res == 0) {
      val->same_base = cmp;
      val->same_reg = cmp->same_reg;
      cmp->same_reg = val;
      continue;
    }
    val->same_base = val;

    // insert into ordered list
    if (cmp != NULL) {
      if (cmp->prev == NULL) {
        slave->values_head = val;
      } else {
        cmp->prev->next = val;
      }
      val->prev = cmp->prev;
      val->next = cmp;
      cmp->prev = val;
      continue;
    }

    // append to ordered list
    cmp = slave->values_tail;
    cmp->next = val;
    val->prev = cmp;
    val->next = NULL;
    slave->values_tail = val;
  }

  // build request groups
  for (val_idx = 0, cmp = NULL, grp = NULL, val = slave->values_head; val != NULL; cmp = val, val = val->next, val_idx++) {
    // process inputs only
    if (val->dir != UVRGW_CONF_VAL_DIR_IN) {
      continue;
    }

    // add to group list
    if (grp == NULL || val->regtype != cmp->regtype || val->addr != (cmp->addr + 1) || val_idx >= slave->max_req_regs) {
      val_idx = 0;
      val->in_group_index = 0;
      if (grp == NULL) {
        slave->in_group_head = val;
      } else {
        grp->in_group_next = val;
      }
      grp = val;
      grp->in_group_count = 1;
      continue;
    }

    // add to group
    val->in_group_index = val_idx;
    val->in_group_same = grp->in_group_same;
    grp->in_group_same = val;
    grp->in_group_count++;
  }

  // build output list
  for (val = slave->values_head; val != NULL; val = val->next) {
    // process outputs only
    if (val->dir != UVRGW_CONF_VAL_DIR_OUT) {
      continue;
    }

    // input registers are read only
    if (val->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || val->regtype == UVRGW_CONF_MB_REG_TYPE_INREG) {
      continue;
    }

    // add to list
    val->value_out_next = slave->value_out_head;
    slave->value_out_head = val;
  }

  return 0;
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_SLAVE_VAL_T *val = (MB_SLAVE_VAL_T *) child;

  val->slave = (MB_SLAVE_T *) ctx;

  val->name = uvrgw_conf_strdup(cfg_title(cfg));
  val->dir = cfg_getint(cfg, "dir");
  val->regtype = cfg_getint(cfg, "regtype");
  val->addr = cfg_getint(cfg, "addr");
  val->type = cfg_getint(cfg, "type");
  val->pos = cfg_getint(cfg, "pos");
  val->scale = cfg_getfloat(cfg, "scale");
  val->offset = cfg_getfloat(cfg, "offset");

  val->disp = uvrgw_conf_get_dispatcher(val->name, (val->dir == UVRGW_CONF_VAL_DIR_OUT));

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
  MB_SLAVE_VAL_T *val;
  int val_idx;

  for (slave = master->slaves, slave_idx = 0; slave_idx < master->slaves_count; slave++, slave_idx++) {
    for (val = slave->values, val_idx = 0; val_idx < slave->values_count; val++, val_idx++) {
      if (val->dir == UVRGW_CONF_VAL_DIR_OUT) {
        uvrgw_conf_register_disp_cb(val->disp, val, write_schedule);
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
  MB_SLAVE_VAL_T *val;
  int val_idx;

  for (slave = master->slaves, slave_idx = 0; slave_idx < master->slaves_count; slave++, slave_idx++) {
    for (val = slave->values, val_idx = 0; val_idx < slave->values_count; val++, val_idx++) {
      free((void *) val->name);
    }
    free(slave->values);
    free(slave->input_buf);
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

  // connect/reconnect on demand
  if (modbus_set_error_recovery(master->ctx, MODBUS_ERROR_RECOVERY_LINK) < 0) {
    syslog(LOG_ERR, "Could not set modbus error recovery");
    goto fail1;
  }

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

static void *master_thread(void *ptr) {
  MB_MASTER_T *master = (MB_MASTER_T *) ptr;
  int64_t now;

  while (master->thread_running) {
    now = utl_get_ticks();
    if (master->next_transaction <= now) {
      if (master_task(master, now) > 0) {
        master->next_transaction = now + master->separation_time;
      }
    }
    usleep(MASTER_THREAD_PERIOD_US);
  }

  return NULL;
}

static int master_task(MB_MASTER_T *master, int64_t now) {
  MB_SLAVE_T *slave;
  int ret;

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
    slave->in_group_curr = slave->in_group_head;
    slave->value_out_curr = slave->value_out_head;
    (master->slave_curr_idx)++;
  }

  if (master->slave_curr_idx >= master->slaves_count) {
    master->slave_curr_idx = 0;
  }

  return 0;
}

static int slave_task_read(MB_SLAVE_T *slave, int64_t now) {
  MB_SLAVE_VAL_T *grp;

  // check poll timer
  if (slave->next_poll > now) {
    return 0;
  }

  // get current group
  grp = slave->in_group_curr;
  if (grp == NULL) {
    slave->next_poll =  now + slave->interval;
    return 0;
  }

  // read registers
  if (grp->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || grp->regtype == UVRGW_CONF_MB_REG_TYPE_BIT) {
    if (read_bits(grp) < 0) {
      syslog(LOG_WARNING, "Failed to read MODBUS bits of slave %d (start %d, len %d). Error %d.", slave->id, grp->addr, grp->in_group_count, errno);
      slave->next_poll =  now + slave->interval;
      return -1;
    }
  } else {
    if (read_registers(grp) < 0) {
      syslog(LOG_WARNING, "Failed to read MODBUS registers of slave %d (start %d, len %d). Error %d.", slave->id, grp->addr, grp->in_group_count, errno);
      slave->next_poll =  now + slave->interval;
      return -1;
    }
  }

  slave->in_group_curr = grp->in_group_next;

  return 1;
}

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

static int read_bits(MB_SLAVE_VAL_T *grp) {
  MB_SLAVE_T *slave = grp->slave;
  MB_MASTER_T *master = slave->master;
  MB_SLAVE_VAL_T *val;
  MB_SLAVE_VAL_T *dpval;
  uint8_t *buf = slave->input_buf;
  int ret;
  uint8_t *p;

  // set slave address
  ret = modbus_set_slave(master->ctx, slave->id);
  if (ret < 0) {
    return ret;
  }

  if (grp->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT) {
    ret = modbus_read_input_bits(master->ctx, grp->addr, grp->in_group_count, buf);
  } else {
    ret = modbus_read_bits(master->ctx, grp->addr, grp->in_group_count, buf);
  }

  if (ret < 0) {
    return ret;
  }

  for (val = grp; val != NULL; val = val->in_group_same) {
    p = &buf[val->in_group_index];
    // dispatch value
    for (dpval = val; dpval != NULL; dpval = dpval->same_reg) {
      switch (dpval->type) {
        case UVRGW_CONF_MB_TYPE_BIT:
          uvrgw_conf_disp_val(dpval->disp, dpval, *p ? 1.0 : 0.0);
          break;
      }
    }
  }

  return 0;
}

static int read_registers(MB_SLAVE_VAL_T *grp) {
  MB_SLAVE_T *slave = grp->slave;
  MB_MASTER_T *master = slave->master;
  MB_SLAVE_VAL_T *val;
  MB_SLAVE_VAL_T *dpval;
  uint16_t *buf = slave->input_buf;
  int ret;
  uint16_t *p;

  // set slave address
  ret = modbus_set_slave(master->ctx, slave->id);
  if (ret < 0) {
    return ret;
  }

  if (grp->regtype == UVRGW_CONF_MB_REG_TYPE_INREG) {
    ret = modbus_read_input_registers(master->ctx, grp->addr, grp->in_group_count, buf);
  } else {
    ret = modbus_read_registers(master->ctx, grp->addr, grp->in_group_count, buf);
  }

  if (ret < 0) {
    return ret;
  }

  for (val = grp; val != NULL; val = val->in_group_same) {
    p = &buf[val->in_group_index];
    // dispatch value
    for (dpval = val; dpval != NULL; dpval = dpval->same_reg) {
      switch (dpval->type) {
        case UVRGW_CONF_MB_TYPE_SIGNED:
          uvrgw_conf_disp_val(dpval->disp, dpval, ((double) ((int16_t) *p)) * dpval->scale + dpval->offset);
          break;
        case UVRGW_CONF_MB_TYPE_UNSIGNED:
          uvrgw_conf_disp_val(dpval->disp, dpval, ((double) *p) * dpval->scale + dpval->offset);
          break;
        case UVRGW_CONF_MB_TYPE_BITMASK:
          uvrgw_conf_disp_val(dpval->disp, dpval, (*p & (1 << dpval->pos)) ? 1.0 : 0.0);
          break;
      }
    }
  }

  return 0;
}

static int write_schedule(void *v, double f) {
  MB_SLAVE_VAL_T *val = (MB_SLAVE_VAL_T *) v;
  MB_SLAVE_T *slave = val->slave;
  MB_MASTER_T *master = slave->master;

  pthread_mutex_lock(&master->write_lock);
  val->write_value = f;
  val->write_pending = true;
  pthread_mutex_unlock(&master->write_lock);

  return 0;
}

static int write_execute(MB_SLAVE_VAL_T *val) {
  MB_SLAVE_T *slave = val->slave;
  MB_MASTER_T *master = slave->master;
  bool pending;
  double f;
  uint16_t *buf;

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

  if (val->type == UVRGW_CONF_MB_TYPE_BIT) {
    if (modbus_write_bit(master->ctx, val->addr, (f > 0.5)) < 0) {
      return -1;
    }
  } else {
    switch (val->type) {
      case UVRGW_CONF_MB_TYPE_SIGNED:
        f = (f - val->offset) / val->scale;
        if (modbus_write_register(master->ctx, val->addr, (int16_t) utl_val_limit(f, INT16_MIN, INT16_MAX)) < 0) {
          return -1;
        }
        break;
      case UVRGW_CONF_MB_TYPE_UNSIGNED:
        f = (f - val->offset) / val->scale;
        if (modbus_write_register(master->ctx, val->addr, (uint16_t) utl_val_limit(f, 0.0, UINT16_MAX)) < 0) {
          return -1;
        }
        break;
      case UVRGW_CONF_MB_TYPE_BITMASK:
        buf = &val->same_base->valbuf;
        if (f > 0.5) {
          *buf |= (1 << val->pos);
        } else {
          *buf &= ~(1 << val->pos);
        }
        if (modbus_write_register(master->ctx, val->addr, *buf) < 0) {
          return -1;
        }
        break;
    }
  }

  return 1;
}

