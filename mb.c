#include "mb.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <asm/ioctls.h>
#include <modbus/modbus.h>

#define RESPONSE_TIMEOUT_MS 2000

static modbus_t *ctx = NULL;

int mb_startup(const char *dev, int baud) {
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

/*
FOGO (10):

start: modbus_write_bit(ctx, 4700, TRUE); 
stop: modbus_write_bit(ctx, 4700, FALSE); 


1028: power factor
1016: load P
1020: load Q
1024: load S

1000: rpm
1033-1035: U L1-L3 -> N

1032: freq
1039-1041: I L1-L3

4214: No of alarms
1051: Ubat
1052: D+

1055: Fuel Level
1053: Coolant Temp
1054: Canopy Temp
1056: Exhaust Temp

1058: ESTOP
1057: Binary Inputs
1059: Binary Outputs
*/

int mb_task(void) {
printf("mb_task\n");


  return 0;

/*
  ret = modbus_set_slave(ctx, 1);
  if(ret < 0){
    perror("modbus_set_slave error\n");
    return -1;
  }

    // Read 2 registers from adress 0, copy data to dest.
    ret = modbus_read_registers(ctx, 0, NB_REGS, dest);
    if(ret < 0){
      perror("modbus_read_regs error\n");
      return -1;
    }
*/

}

