#ifndef _REST_H_
#define _REST_H_

#include <ioconf.h>

#define REST_POLL_PERIOD_MS   10000
#define REST_POLL_TIMEOUT_SEC 3

typedef struct {
  const char *name;
  const char *path;
  double scale;
  double offset;
} REST_VAL_T;

typedef struct {
  const char *url;
  int interval;
  const char *user;
  const char *pwd;

  int values_count;
  REST_VAL_T *values;

  int poll_timer;
} REST_CONN_T;

int rest_startup(void);
void rest_shutdown(void);
int rest_task(void);

#endif
