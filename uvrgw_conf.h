#ifndef _UVRGW_CONF_H_
#define _UVRGW_CONF_H_

#include <confuse.h>

#define UVRGW_CONF_VAL_DIR_IN  0
#define UVRGW_CONF_VAL_DIR_OUT 1

#define UVRGW_CONF_MQTT_TYPE_NUMBER  0
#define UVRGW_CONF_MQTT_TYPE_SWITCH  1
#define UVRGW_CONF_MQTT_TYPE_CONTACT 2

#define UVRGW_CONF_CAN_TYPE_BIT  0
#define UVRGW_CONF_CAN_TYPE_U8   1
#define UVRGW_CONF_CAN_TYPE_S8   2
#define UVRGW_CONF_CAN_TYPE_U16  3
#define UVRGW_CONF_CAN_TYPE_S16  4
#define UVRGW_CONF_CAN_TYPE_U32  5
#define UVRGW_CONF_CAN_TYPE_S32  6

#define UVRGW_CONF_MB_TYPE_BIT      0
#define UVRGW_CONF_MB_TYPE_SIGNED   1
#define UVRGW_CONF_MB_TYPE_UNSIGNED 2
#define UVRGW_CONF_MB_TYPE_BITMASK  3

#define UVRGW_CONF_MB_REG_TYPE_INBIT 0
#define UVRGW_CONF_MB_REG_TYPE_BIT   1
#define UVRGW_CONF_MB_REG_TYPE_INREG 2
#define UVRGW_CONF_MB_REG_TYPE_REG   3

#define UVRGW_CONF_MB_PARITY_NONE ((int) 'N')
#define UVRGW_CONF_MB_PARITY_EVEN ((int) 'E')
#define UVRGW_CONF_MB_PARITY_ODD  ((int) 'O')

typedef int (* UVRGW_CONF_CONFIG_CHILD_CB)(cfg_t *cfg, void *ctx, void *child);

typedef int (* UVRGW_CONF_DISPATCH_CB)(void *v, double f);

typedef struct {
  void *val;
  UVRGW_CONF_DISPATCH_CB cb;
} UVRGW_CONF_DISPATCH_CB_VAL_T;

struct UVRGW_CONF_VAL_DISPATCH;

typedef struct UVRGW_CONF_VAL_DISPATCH {
  const char *name;
  int value_count;
  struct UVRGW_CONF_VAL_DISPATCH *next;

  int value_cbs_pos;
  UVRGW_CONF_DISPATCH_CB_VAL_T *value_cbs;
} UVRGW_CONF_VAL_DISPATCH_T;

int uvrgw_conf_load(const char *file);
void uvrgw_conf_cleanup(void);

int uvrgw_conf_config_childs(cfg_t *cfg, const char *name, int *count, void **data, int size, void *ctx, UVRGW_CONF_CONFIG_CHILD_CB cccb);

UVRGW_CONF_VAL_DISPATCH_T *uvrgw_conf_register_val(const char *name);
int uvrgw_conf_register_disp_cb(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, UVRGW_CONF_DISPATCH_CB cb);
void uvrgw_conf_disp_val(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f);

#endif

