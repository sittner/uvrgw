#ifndef _IOCONF_H_
#define _IOCONF_H_

#include <stdint.h>
#include <stdbool.h>

#define IOCONF_CHAN_TYPE_NUMBER  1
#define IOCONF_CHAN_TYPE_SWITCH  2
#define IOCONF_CHAN_TYPE_CONTACT 3

#define IOCONF_MB_TYPE_BIT      1
#define IOCONF_MB_TYPE_SIGNED   2
#define IOCONF_MB_TYPE_UNSIGNED 3
#define IOCONF_MB_TYPE_BITMASK  4

typedef struct {
  int slave;
  int addr;
  bool input_reg;
  int type;
  double offset;
  double scale;
  bool input;
} IOCONF_MB_T;

typedef struct {
  const char *url;
  const char *path;
  const char *user;
  const char *pwd;
  double offset;
  double scale;
} IOCONF_REST_T;

typedef struct {
  const char *topic;
  int type;
  const char *fmt;
  bool subscribe;
  IOCONF_MB_T mb;
  IOCONF_REST_T rest;
} IOCONF_CHAN_T;

extern const IOCONF_CHAN_T ioconf_tab[];

#endif

