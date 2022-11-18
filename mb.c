#include "mb.h"
#include "mqtt.h"
#include "can.h"

#include <stdio.h>
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

#define RESPONSE_TIMEOUT_MS 2000

#define MAX_REQ_REGS 32

#define FMT_BITMASK "bitmask"

typedef struct {
  struct can_frame *frame;
  int mul;
  int div;
  int offset;
  int pos;
  bool send;
} MB_CAN_T;

typedef struct {
  int slave;
  int addr;
  bool input_reg;
  double offset;
  double scale;
  const char *mqtt_topic;
  const char *mqtt_fmt;
  MB_CAN_T can;
} MB_REG_T;

static struct can_frame canbuf_hp_energy = { .can_id = 0x202, .can_dlc = 8, .data = { 0 } };

static const MB_REG_T input_regs[] = {
  // FOGO gen set
  { 10, 1000, true, 0.0, 1.0, "uvr/fogo/rpm", "%.0f" },
  { 10, 1016, true, 0.0, 0.1, "uvr/fogo/load_p", "%.1f" },
  { 10, 1020, true, 0.0, 0.1, "uvr/fogo/load_q", "%.1f" },
  { 10, 1024, true, 0.0, 0.1, "uvr/fogo/load_s", "%.1f" },
  { 10, 1028, true, 0.0, 0.01, "uvr/fogo/pwrfact", "%.2f" },
  { 10, 1032, true, 0.0, 0.1, "uvr/fogo/freq", "%.1f" },

  { 10, 1033, true, 0.0, 1.0, "uvr/fogo/volt_l1-n", "%.0f" },
  { 10, 1034, true, 0.0, 1.0, "uvr/fogo/volt_l2-n", "%.0f" },
  { 10, 1035, true, 0.0, 1.0, "uvr/fogo/volt_l3-n", "%.0f" },
  { 10, 1036, true, 0.0, 1.0, "uvr/fogo/volt_l1-l1", "%.0f" },
  { 10, 1037, true, 0.0, 1.0, "uvr/fogo/volt_l2-l2", "%.0f" },
  { 10, 1038, true, 0.0, 1.0, "uvr/fogo/volt_l3-l1", "%.0f" },
  { 10, 1039, true, 0.0, 1.0, "uvr/fogo/curr_l1", "%.0f" },
  { 10, 1040, true, 0.0, 1.0, "uvr/fogo/curr_l2", "%.0f" },
  { 10, 1041, true, 0.0, 1.0, "uvr/fogo/curr_l3", "%.0f" },

  { 10, 1051, true, 0.0, 0.1, "uvr/fogo/volt_bat", "%.1f" },
  { 10, 1052, true, 0.0, 0.1, "uvr/fogo/volt_alt", "%.1f" },

  { 10, 1053, true, 0.0, 1.0, "uvr/fogo/temp_coolant", "%.0f" },
  { 10, 1054, true, 0.0, 1.0, "uvr/fogo/temp_canopy", "%.0f" },
  { 10, 1055, true, 0.0, 1.0, "uvr/fogo/level_fuel", "%.0f" },
  { 10, 1056, true, 0.0, 1.0, "uvr/fogo/temp_exhaust", "%.0f" },

  { 10, 1057, true, 0.0, 8.0, "uvr/fogo/dio/i-%02d", FMT_BITMASK },
  { 10, 1058, true, 0.0, 1.0, "uvr/fogo/emerg_stop", "%.0f" },
  { 10, 1059, true, 0.0, 8.0, "uvr/fogo/dio/o-%02d", FMT_BITMASK },

  { 10, 4214, true, 0.0, 1.0, "uvr/fogo/cnt_alarms", "%.0f" },

  // TODO: start: modbus_write_bit(ctx, 4700, TRUE);
  // TODO: stop: modbus_write_bit(ctx, 4700, FALSE);

  // DAIKIN heat pump
  { 11, 40, true, 0.0, 0.01, "uvr/daikin/temp_heat_exchanger", "%.2f",
    .can = { &canbuf_hp_energy, 1, 10, 0, 0, false } },
  { 11, 41, true, 0.0, 0.01, "uvr/daikin/temp_backup_heater", "%.2f" },
  { 11, 42, true, 0.0, 0.01, "uvr/daikin/temp_return", "%.2f",
    .can = { &canbuf_hp_energy, 1, 10, 0, 1, false } },
  { 11, 43, true, 0.0, 0.01, "uvr/daikin/temp_warm_water", "%.2f" },
  { 11, 44, true, 0.0, 0.01, "uvr/daikin/temp_outdoor", "%.2f" },
  { 11, 45, true, 0.0, 0.01, "uvr/daikin/temp_refrigerant", "%.2f" },
  { 11, 49, true, 0.0, 0.01, "uvr/daikin/flow", "%.2f",
    .can = { &canbuf_hp_energy, 60, 100, 0, 2, true } },

  { 0 }
};

static modbus_t *ctx = NULL;
static const MB_REG_T *reg_pos = NULL;

static void process_bitmask(const MB_REG_T *reg, uint16_t val);
static void process_scaled16(const MB_REG_T *reg, uint16_t val);

int mb_startup(const char *dev, int baud) {

  reg_pos = input_regs;

  ctx = modbus_new_rtu(dev, baud, 'N', 8, 1);
  if (ctx == NULL) {
    syslog(LOG_ERR, "Could not create modbus instance");
    goto fail0;
  }

  if (modbus_set_response_timeout(ctx, RESPONSE_TIMEOUT_MS / 1000, (RESPONSE_TIMEOUT_MS % 1000) * 1000) < 0) {
    syslog(LOG_ERR, "Could not set modbus response timeout");
    goto fail1;
  }

  if (modbus_connect(ctx) < 0) {
    syslog(LOG_ERR, "Could not open modbus device");
    goto fail1;
  }

  if (modbus_rtu_set_serial_mode(ctx, MODBUS_RTU_RS485)) {
    syslog(LOG_ERR, "Could not set modbus RS485 mode");
    goto fail2;
  }

  if (modbus_rtu_set_rts(ctx, MODBUS_RTU_RTS_UP)) {
    syslog(LOG_ERR, "Could not set modbus RTS mode");
    goto fail2;
  }

  return 0;

fail2:
  modbus_close(ctx);
fail1:
  modbus_free(ctx);
  ctx = NULL;
fail0:
  return -1;
}

void mb_shutdown(void) {
  modbus_close(ctx);
  modbus_free(ctx);
  ctx = NULL;
}

int mb_task(void) {
  uint16_t buf[MAX_REQ_REGS];
  int count;
  const MB_REG_T *curr, *prev;
  int ret;
  uint16_t *p;

  // check for wrap around
  if (reg_pos->slave == 0) {
    reg_pos = input_regs;
  }

  // check for continuous address range
  for (count = 0, prev = NULL, curr = reg_pos; count < MAX_REQ_REGS && curr->slave != 0; count++, prev = curr, curr++) {
    if (prev != NULL) {
      if (curr->slave != prev->slave || curr->input_reg != prev->input_reg) {
        break;
      }
      if (curr->addr != (prev->addr + 1)) {
        break;
      }
    }
  }

  // set slave address
  ret = modbus_set_slave(ctx, reg_pos->slave);
  if (ret < 0) {
    syslog(LOG_WARNING, "Failed to set MODBUS slave address %d", reg_pos->slave);
    reg_pos = curr;
    return 0;
  }

  // read registers
  if (reg_pos->input_reg) {
    ret = modbus_read_input_registers(ctx, reg_pos->addr, count, buf);
  } else {
    ret = modbus_read_registers(ctx, reg_pos->addr, count, buf);
  }
  if (ret < 0) {
    syslog(LOG_WARNING, "Failed to read MODBUS registers of slave %d (start %d, len %d)", reg_pos->slave, reg_pos->addr, count);
    reg_pos = curr;
    return 0;
  }

  // process values
  for (p = buf; reg_pos != curr; reg_pos++, p++) {
    if (strcmp(FMT_BITMASK, reg_pos->mqtt_fmt) == 0) {
      process_bitmask(reg_pos, *p);
    } else {
      process_scaled16(reg_pos, *p);
    }
  }

  return 0;
}

static void process_bitmask(const MB_REG_T *reg, uint16_t val) {
  mqtt_publish_bitmask(reg->mqtt_topic, (uint32_t) val, (int) reg->offset, (int) reg->scale);
}

static void process_scaled16(const MB_REG_T *reg, uint16_t val) {
  struct can_frame *send_frame;
  int tmp;

  mqtt_publish_scaled16(reg->mqtt_topic, reg->mqtt_fmt, val, reg->offset, reg->scale);

  send_frame = reg->can.frame;
  if (send_frame != NULL) {
    tmp = (int) val * reg->can.mul / reg->can.div + reg->can.offset;
    send_frame->data[reg->can.pos * 2 + 0] = tmp & 0xff;
    send_frame->data[reg->can.pos * 2 + 1] = (tmp >> 8) & 0xff;
    if (reg->can.send) {
      can_send(send_frame);
    }
  }
}
