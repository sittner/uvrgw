#ifndef _MB_H_
#define _MB_H_

#include <ioconf.h>

int mb_startup(const char *dev, int baud);
void mb_shutdown(void);
int mb_task(void);

int mb_write_chan(const IOCONF_CHAN_T *chan, double val);

#endif

