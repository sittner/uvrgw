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

#define RESPONSE_TIMEOUT_MS 200

#define MAX_REQ_REGS 32

static modbus_t *ctx = NULL;
static const IOCONF_CHAN_T *chan = NULL;

static int read_bits(int count, const IOCONF_CHAN_T *end);
static int read_registers(int count, const IOCONF_CHAN_T *end);
static double val_limit(double val, double min, double max);

int mb_startup(const char *dev, int baud) {

  chan = ioconf_tab;

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
  int count;
  const IOCONF_CHAN_T *first, *curr, *prev;
  int ret;

  // check for wrap around
  if (chan->topic == NULL) {
    chan = ioconf_tab;
  }

  // check for items to process
  for (count = 0, first = NULL, prev = NULL, curr = chan; curr->topic != NULL && count < MAX_REQ_REGS; curr++) {
    // check for MODBUS read mapping
    if (curr->mb.slave == 0 || !curr->mb.input) {
      continue;
    }

    // remember first item with MODBUS mapping
    if (first == NULL) {
      first = curr;
    }

    // stop if slave or register type has changed
    if (curr->mb.slave != first->mb.slave || curr->mb.input_reg != first->mb.input_reg) {
      break;
    }
    if (curr->mb.type == IOCONF_MB_TYPE_BIT && first->mb.type != IOCONF_MB_TYPE_BIT) {
      break;
    }
    if (curr->mb.type != IOCONF_MB_TYPE_BIT && first->mb.type == IOCONF_MB_TYPE_BIT) {
      break;
    }

    if (prev != NULL) {
      // skip items with same address
      if (curr->mb.addr == prev->mb.addr) {
        continue;
      }

      // check for continuous address range
      if (curr->mb.addr != (prev->mb.addr + 1)) {
        break;
      }
    }

    prev = curr;
    count++;
  }

  // check, if we have at last one item found
  if (first == NULL) {
    return 0;
  }

  // skip items without mapping
  chan = first;

  // set slave address
  ret = modbus_set_slave(ctx, chan->mb.slave);
  if (ret < 0) {
    syslog(LOG_WARNING, "Failed to set MODBUS slave address %d", chan->mb.slave);
    chan = curr;
    return 0;
  }

  // read registers
  if (chan->mb.type == IOCONF_MB_TYPE_BIT) {
    ret = read_bits(count, curr);
  } else {
    ret = read_registers(count, curr);
  }
  if (ret < 0) {
    syslog(LOG_WARNING, "Failed to read MODBUS registers of slave %d (start %d, len %d)", chan->mb.slave, chan->mb.addr, count);
    chan = curr;
    return 0;
  }

  return 0;

}

static int read_bits(int count, const IOCONF_CHAN_T *end) {
  uint8_t buf[MAX_REQ_REGS];
  int ret;
  const IOCONF_CHAN_T *prev;
  uint8_t *p;
  double val;

  if (chan->mb.input_reg) {
    ret = modbus_read_input_bits(ctx, chan->mb.addr, count, buf);
  } else {
    ret = modbus_read_bits(ctx, chan->mb.addr, count, buf);
  }
  if (ret < 0) {
    return ret;
  }

  for (prev = NULL, p = buf; chan != end; chan++) {
    // skip items without modbus read data
    if (chan->mb.slave == 0 || !chan->mb.input) {
      continue;
    }

    // increment data pointer, if address has changed
    if (prev != NULL && prev->mb.addr != chan->mb.addr) {
      p++;
    }
    prev = chan;

    // process value
    val = 0.0;
    switch (chan->mb.type) {
      case IOCONF_MB_TYPE_BIT:
        val = *p ? 1.0 : 0.0;
        break;
    }

    // send CAN
    can_send_chan(chan, val);

    // send MQTT topic
    mqtt_publish_chan(chan, val);
  }

  return 0;
}

static int read_registers(int count, const IOCONF_CHAN_T *end) {
  uint16_t buf[MAX_REQ_REGS];
  int ret;
  const IOCONF_CHAN_T *prev;
  uint16_t *p;
  double val;

  if (chan->mb.input_reg) {
    ret = modbus_read_input_registers(ctx, chan->mb.addr, count, buf);
  } else {
    ret = modbus_read_registers(ctx, chan->mb.addr, count, buf);
  }
  if (ret < 0) {
    return ret;
  }

  for (prev = NULL, p = buf; chan != end; chan++) {
    // skip items without modbus read data
    if (chan->mb.slave == 0 || !chan->mb.input) {
      continue;
    }

    // increment data pointer, if address has changed
    if (prev != NULL && prev->mb.addr != chan->mb.addr) {
      p++;
    }
    prev = chan;

    // process value
    val = 0.0;
    switch (chan->mb.type) {
      case IOCONF_MB_TYPE_SIGNED:
        val = ((double) ((int16_t) *p)) *chan->mb.scale + chan->mb.offset;
        break;
      case IOCONF_MB_TYPE_UNSIGNED:
        val = ((double) *p) *chan->mb.scale + chan->mb.offset;
        break;
      case IOCONF_MB_TYPE_BITMASK:
        val = (*p & (1 << ((int) chan->mb.offset))) ? 1.0 : 0.0;
        break;
    }

    // send CAN
    can_send_chan(chan, val);

    // send MQTT topic
    mqtt_publish_chan(chan, val);
  }

  return 0;
}

int mb_write_chan(const IOCONF_CHAN_T *chan, double val) {
  int ret;

  // check for MODBUS write mapping
  if (chan->mb.slave == 0 || chan->mb.input) {
    return 0;
  }

  // input registers are read only
  if (chan->mb.input_reg) {
    return 0;
  }

  // bitmasks are not supported (no atomic write)
  if (chan->mb.type == IOCONF_MB_TYPE_BITMASK) {
    return 0;
  }

  // set slave address
  ret = modbus_set_slave(ctx, chan->mb.slave);
  if (ret < 0) {
    syslog(LOG_WARNING, "Failed to set MODBUS slave address %d", chan->mb.slave);
    return -1;
  }

  if (chan->mb.type == IOCONF_MB_TYPE_BIT) {
    return modbus_write_bit(ctx, chan->mb.addr, (val > 0.5));
  } else {
    val = (val - chan->mb.offset) / chan->mb.scale;
    switch (chan->mb.type) {
      case IOCONF_MB_TYPE_SIGNED:
        return modbus_write_register(ctx, chan->mb.addr, (int16_t) val_limit(val, INT16_MIN, INT16_MAX));
      case IOCONF_MB_TYPE_UNSIGNED:
        return modbus_write_register(ctx, chan->mb.addr, (uint16_t) val_limit(val, 0.0, UINT16_MAX));
    }
  }

  return 0;
}

static double val_limit(double val, double min, double max) {
  if (val < min) {
    return min;
  }

  if (val > max) {
    return max;
  }

  return val;
}

