#ifndef _REST_H_
#define _REST_H_

#include "uvrgw_conf.h"

#include <stdint.h>

struct REST_VAL;
struct REST_CONN;

typedef struct REST_VAL {
  const char *name;
  const char *path;
  double scale;
  double offset;

  struct REST_CONN *conn;

  UVRGW_CONF_VAL_DISPATCH_T *disp;

} REST_VAL_T;

typedef struct REST_CONN {
  const char *url;
  int interval;
  int timeout;
  const char *user;
  const char *pwd;

  int values_count;
  struct REST_VAL *values;

  int64_t next_poll;
} REST_CONN_T;

void rest_init(void);
int rest_configure(cfg_t *cfg);
void rest_unconfigure(void);

int rest_startup(void);
void rest_shutdown(void);
int rest_task(void);

#endif
