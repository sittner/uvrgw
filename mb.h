#ifndef _MB_H_
#define _MB_H_

int mb_startup(const char *dev, int baud);
void mb_shutdown(void);
int mb_task(void);

#endif

