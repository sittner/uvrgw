#include "mb.h"
#include "mqtt.h"
#include "can.h"
#include "timer.h"
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

static int masters_count;
static MB_RTU_MASTER_T *masters;

static int master_configure(cfg_t *cfg, void *ctx, void *child);
static int slave_configure(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static int master_startup(MB_RTU_MASTER_T *master);
static void *master_thread(void *ptr);
static int master_task(MB_RTU_MASTER_T *master, int64_t now);
static void slave_task_init(MB_RTU_SLAVE_T *slave, int64_t now);
static int slave_task_read(MB_RTU_SLAVE_T *slave, int64_t now);
static int slave_task_write(MB_RTU_SLAVE_T *slave, int64_t now);
static int write_schedule(void *v, double f);
static int write_execute(MB_RTU_SLAVE_VAL_T *val);
static int read_bits(MB_RTU_SLAVE_VAL_T *grp);
static int read_registers(MB_RTU_SLAVE_VAL_T *grp);

void mb_init(void) {
  masters_count = 0;
  masters = NULL;
}

int mb_configure(cfg_t *cfg) {
  return uvrgw_conf_config_childs(cfg, "modbus_rtu", &masters_count, (void **) &masters, sizeof(MB_RTU_MASTER_T), NULL, master_configure);
}

static int master_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_RTU_MASTER_T *master = (MB_RTU_MASTER_T *) child;

  master->interface = uvrgw_conf_strdup(cfg_getstr(cfg, "interface"));
  master->baud = cfg_getint(cfg, "baud");
  master->parity = cfg_getint(cfg, "parity");
  master->data_bits = cfg_getint(cfg, "data_bits");
  master->stop_bits = cfg_getint(cfg, "stop_bits");
  master->separation_time = cfg_getint(cfg, "separation_time");
  master->timeout = cfg_getint(cfg, "timeout");
  master->mode = cfg_getint(cfg, "mode");
  master->rts = cfg_getint(cfg, "rts");
  master->rts_delay = cfg_getint(cfg, "rts_delay");

  if (master->interface == NULL) {
    syslog(LOG_ERR, "modbus_rtu inteface name not given.");
    return -1;
  }

  pthread_mutex_init(&master->write_lock, NULL);

  return uvrgw_conf_config_childs(cfg, "slave", &master->slaves_count, (void **) &master->slaves, sizeof(MB_RTU_SLAVE_T), master, slave_configure);
}

static int value_reg_cmp(MB_RTU_SLAVE_VAL_T *a, MB_RTU_SLAVE_VAL_T *b) {
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
  MB_RTU_SLAVE_T *slave = (MB_RTU_SLAVE_T *) child;
  MB_RTU_SLAVE_VAL_T *val, *cmp, *grp;
  int val_idx;
  int res;

  slave->master = (MB_RTU_MASTER_T *) ctx;

  slave->id = cfg_getint(cfg, "id");
  slave->interval = cfg_getint(cfg, "interval");
  slave->max_req_regs = cfg_getint(cfg, "max_req_regs");

  if (slave->max_req_regs > 0) {
    slave->input_buf = calloc(slave->max_req_regs, sizeof(uint16_t));
  }

  if (uvrgw_conf_config_childs(cfg, "value", &slave->values_count, (void **) &slave->values, sizeof(MB_RTU_SLAVE_VAL_T), slave, value_configure) < 0) {
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

  return 0;
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  MB_RTU_SLAVE_VAL_T *val = (MB_RTU_SLAVE_VAL_T *) child;

  val->slave = (MB_RTU_SLAVE_T *) ctx;

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
  MB_RTU_MASTER_T *master;
  int master_idx;
  MB_RTU_SLAVE_T *slave;
  int slave_idx;
  MB_RTU_SLAVE_VAL_T *val;
  int val_idx;

  for (master = masters, master_idx = 0; master_idx < masters_count; master++, master_idx++) {
    for (slave = master->slaves, slave_idx = 0; slave_idx < master->slaves_count; slave++, slave_idx++) {
      for (val = slave->values, val_idx = 0; val_idx < slave->values_count; val++, val_idx++) {
        if (val->dir == UVRGW_CONF_VAL_DIR_OUT) {
          uvrgw_conf_register_disp_cb(val->disp, val, write_schedule);
        }
      }
    }
  }
}

void mb_unconfigure(void) {
  MB_RTU_MASTER_T *master;
  int master_idx;
  MB_RTU_SLAVE_T *slave;
  int slave_idx;
  MB_RTU_SLAVE_VAL_T *val;
  int val_idx;

  for (master = masters, master_idx = 0; master_idx < masters_count; master++, master_idx++) {
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
  free(masters);
}

int mb_startup(void) {
  MB_RTU_MASTER_T *master;
  int master_idx;

  for (master = masters, master_idx = 0; master_idx < masters_count; master++, master_idx++) {
    if (master_startup(master) < 0) {
      return -1;
    }
  }

  return 0;
}

static int master_startup(MB_RTU_MASTER_T *master) {
  int64_t now;
  MB_RTU_SLAVE_T *slave;
  int slave_idx;

  now = utl_get_ticks();

  master->ctx = modbus_new_rtu(master->interface, master->baud, (char) master->parity, master->data_bits, master->stop_bits);
  if (master->ctx == NULL) {
    syslog(LOG_ERR, "Could not create modbus instance");
    goto fail0;
  }

  if (modbus_set_response_timeout(master->ctx, master->timeout / 1000, (master->timeout % 1000) * 1000) < 0) {
    syslog(LOG_ERR, "Could not set modbus response timeout");
    goto fail1;
  }

  if (modbus_connect(master->ctx) < 0) {
    syslog(LOG_ERR, "Could not open modbus device");
    goto fail1;
  }

  if (modbus_rtu_set_serial_mode(master->ctx, master->mode)) {
    syslog(LOG_ERR, "Could not set modbus RS485 mode");
    goto fail2;
  }

  if (modbus_rtu_set_rts(master->ctx, master->rts)) {
    syslog(LOG_ERR, "Could not set modbus RTS mode");
    goto fail2;
  }

  if (master->rts_delay >= 0) {
    if (modbus_rtu_set_rts_delay(master->ctx, master->rts_delay)) {
      syslog(LOG_ERR, "Could not set modbus RTS delay");
      goto fail2;
    }
  }

  // initial slaves task state
  for (slave = master->slaves, slave_idx = 0; slave_idx < master->slaves_count; slave++, slave_idx++) {
    slave_task_init(slave, now);
  }

  if (master->slaves_count > 0) {
    master->thread_running = true;
    if (pthread_create(&(master->thread), NULL, master_thread, (void*) master) != 0) {
      master->thread_running = false;
      syslog(LOG_ERR, "failed to start master thread");
      goto fail2;
    }
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

void mb_shutdown(void) {
  MB_RTU_MASTER_T *master;
  int master_idx;

  for (master = masters, master_idx = 0; master_idx < masters_count; master++, master_idx++) {
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
}

static void *master_thread(void *ptr) {
  MB_RTU_MASTER_T *master = (MB_RTU_MASTER_T *) ptr;
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

static int master_task(MB_RTU_MASTER_T *master, int64_t now) {
  MB_RTU_SLAVE_T *slave;
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
    slave_task_init(slave, now);
    (master->slave_curr_idx)++;
  }

  if (master->slave_curr_idx >= master->slaves_count) {
    master->slave_curr_idx = 0;
  }

  return 0;
}

static void slave_task_init(MB_RTU_SLAVE_T *slave, int64_t now) {
  slave->next_poll =  now + slave->interval;
  slave->in_group_curr = slave->in_group_head;
  slave->value_out_curr = slave->values_head;
}

static int slave_task_read(MB_RTU_SLAVE_T *slave, int64_t now) {
  MB_RTU_SLAVE_VAL_T *grp;

  // check poll timer
  if (slave->next_poll > now) {
    return 0;
  }

  // get current group
  grp = slave->in_group_curr;
  if (grp == NULL) {
    return 0;
  }

  // read registers
  if (grp->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || grp->regtype == UVRGW_CONF_MB_REG_TYPE_BIT) {
    if (read_bits(grp) < 0) {
      syslog(LOG_WARNING, "Failed to read MODBUS bits of slave %d (start %d, len %d)", slave->id, grp->addr, grp->in_group_count);
      return -1;
    }
  } else {
    if (read_registers(grp) < 0) {
      syslog(LOG_WARNING, "Failed to read MODBUS registers of slave %d (start %d, len %d)", slave->id, grp->addr, grp->in_group_count);
      return -1;
    }
  }

  slave->in_group_curr = grp->in_group_next;

  return 1;
}

static int slave_task_write(MB_RTU_SLAVE_T *slave, int64_t now) {
  int ret;
  MB_RTU_SLAVE_VAL_T *val;

  // process next pending value
  while (true) {
    val = slave->value_out_curr;
    if (val == NULL) {
      return 0;
    }
    slave->value_out_curr = val->next;

    // check for output
    if (val->dir != UVRGW_CONF_VAL_DIR_OUT) {
      continue;
    }

    // input registers are read only
    if (val->regtype == UVRGW_CONF_MB_REG_TYPE_INBIT || val->regtype == UVRGW_CONF_MB_REG_TYPE_INREG) {
      continue;
    }

    // execute write, if pending (only one request per timer period)
    ret = write_execute(val);
    if (ret != 0) {
      return ret;
    }
  }
}

static int read_bits(MB_RTU_SLAVE_VAL_T *grp) {
  MB_RTU_SLAVE_T *slave = grp->slave;
  MB_RTU_MASTER_T *master = slave->master;
  MB_RTU_SLAVE_VAL_T *val;
  MB_RTU_SLAVE_VAL_T *dpval;
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

static int read_registers(MB_RTU_SLAVE_VAL_T *grp) {
  MB_RTU_SLAVE_T *slave = grp->slave;
  MB_RTU_MASTER_T *master = slave->master;
  MB_RTU_SLAVE_VAL_T *val;
  MB_RTU_SLAVE_VAL_T *dpval;
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
  MB_RTU_SLAVE_VAL_T *val = (MB_RTU_SLAVE_VAL_T *) v;
  MB_RTU_SLAVE_T *slave = val->slave;
  MB_RTU_MASTER_T *master = slave->master;

  pthread_mutex_lock(&master->write_lock);
  val->write_value = f;
  val->write_pending = true;
  pthread_mutex_unlock(&master->write_lock);

  return 0;
}

static int write_execute(MB_RTU_SLAVE_VAL_T *val) {
  MB_RTU_SLAVE_T *slave = val->slave;
  MB_RTU_MASTER_T *master = slave->master;
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

