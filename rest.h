#ifndef _REST_H_
#define _REST_H_

#include <ioconf.h>

#define REST_POLL_PERIOD_MS   10000
#define REST_POLL_TIMEOUT_SEC 3

int rest_startup(void);
void rest_shutdown(void);
int rest_task(void);

#endif
