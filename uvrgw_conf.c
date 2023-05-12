#include <uvrgw_conf.h>

#include "can.h"

#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <modbus/modbus.h>

typedef struct {
  const char *str;
  int val;
} MAP_ITEM_T;

static int parse_map(const MAP_ITEM_T *map, cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_val_dir(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_mqtt_val_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_can_val_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_mb_val_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_mb_reg_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_mb_parity(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_mb_rtu_mode(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_mb_rtu_rts(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_can_id(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);

void static init_dispatcher();

static cfg_opt_t mqtt_val_opts[] = {
  CFG_INT_CB("dir", -1, CFGF_NONE, parse_val_dir),
  CFG_INT_CB("type", -1, CFGF_NONE, parse_mqtt_val_type),
  CFG_STR("topic", NULL, CFGF_NONE),
  CFG_STR("fmt", NULL, CFGF_NONE),
  CFG_INT("qos", 0, CFGF_NONE),
  CFG_BOOL("retain", cfg_false, CFGF_NONE),
  CFG_END()
};

static cfg_opt_t mqtt_opts[] = {
  CFG_STR("host", "localhost", CFGF_NONE),
  CFG_INT("port", 1883, CFGF_NONE),
  CFG_STR("client_id", NULL, CFGF_NONE),
  CFG_STR("user", NULL, CFGF_NONE),
  CFG_STR("pwd", NULL, CFGF_NONE),
  CFG_STR("state_topic", NULL, CFGF_NONE),
  CFG_INT("qos", 0, CFGF_NONE),
  CFG_BOOL("retain", cfg_false, CFGF_NONE),
  CFG_SEC("value", mqtt_val_opts, CFGF_MULTI | CFGF_TITLE),
  CFG_END()
};

static cfg_opt_t json_val_opts[] = {
  CFG_STR("path", NULL, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_FLOAT("offset", 0.0, CFGF_NONE),
  CFG_END()
};

static cfg_opt_t json_opts[] = {
  CFG_STR("url", NULL, CFGF_NONE),
  CFG_INT("interval", 0, CFGF_NONE),
  CFG_STR("user", NULL, CFGF_NONE),
  CFG_STR("pwd", NULL, CFGF_NONE),
  CFG_SEC("value", json_val_opts, CFGF_MULTI | CFGF_TITLE),
  CFG_END()
};

static cfg_opt_t can_frame_val_opts[] = {
  CFG_INT_CB("type", -1, CFGF_NONE, parse_can_val_type),
  CFG_INT("pos", -1, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_FLOAT("offset", 0.0, CFGF_NONE),
  CFG_END()
};

static cfg_opt_t can_frame_opts[] = {
  CFG_INT_CB("can_id", -1, CFGF_NONE, parse_can_id),
  CFG_INT_CB("dir", -1, CFGF_NONE, parse_val_dir),
  CFG_SEC("value", can_frame_val_opts, CFGF_MULTI | CFGF_TITLE),
  CFG_END()
};

static cfg_opt_t can_opts[] = {
  CFG_STR("interface", NULL, CFGF_NONE),
  CFG_INT("timestamp_period", 0, CFGF_NONE),
  CFG_INT("send_timeout", 1000, CFGF_NONE),
  CFG_SEC("frame", can_frame_opts, CFGF_MULTI),
  CFG_END()
};

static cfg_opt_t mb_slave_val_opts[] = {
  CFG_INT_CB("dir", -1, CFGF_NONE, parse_val_dir),
  CFG_INT("addr", -1, CFGF_NONE),
  CFG_INT_CB("type", -1, CFGF_NONE, parse_mb_val_type),
  CFG_INT_CB("regtype", -1, CFGF_NONE, parse_mb_reg_type),
  CFG_INT("pos", -1, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_FLOAT("offset", 0.0, CFGF_NONE),
  CFG_END()
};

static cfg_opt_t mb_rtu_slave_opts[] = {
  CFG_INT("id", -1, CFGF_NONE),
  CFG_INT("interval", 0, CFGF_NONE),
  CFG_BOOL("many_req", cfg_false, CFGF_NONE),
  CFG_SEC("value", mb_slave_val_opts, CFGF_MULTI | CFGF_TITLE),
  CFG_END()
};

static cfg_opt_t mb_rtu_opts[] = {
  CFG_STR("interface", NULL, CFGF_NONE),
  CFG_INT("baud", 9600, CFGF_NONE),
  CFG_INT_CB("parity", UVRGW_CONF_MB_PARITY_NONE, CFGF_NONE, parse_mb_parity),
  CFG_INT("data_bits", 8, CFGF_NONE),
  CFG_INT("stop_bits", 1, CFGF_NONE),
  CFG_INT("timeout", 250, CFGF_NONE),
  CFG_INT_CB("mode", MODBUS_RTU_RS232, CFGF_NONE, parse_mb_rtu_mode),
  CFG_INT_CB("rts", MODBUS_RTU_RTS_NONE, CFGF_NONE, parse_mb_rtu_rts),
  CFG_INT("rts_delay", -1, CFGF_NONE),
  CFG_SEC("slave", mb_rtu_slave_opts, CFGF_MULTI),
  CFG_END()
};

static cfg_opt_t opts[] = {
  CFG_SEC("mqtt", mqtt_opts, CFGF_MULTI),
  CFG_SEC("json", json_opts, CFGF_MULTI),
  CFG_SEC("can", can_opts, CFGF_MULTI),
  CFG_SEC("modbus_rtu", mb_rtu_opts, CFGF_MULTI),
  CFG_END()
};

static const MAP_ITEM_T val_dir_map[] = {
  { "in", UVRGW_CONF_VAL_DIR_IN },
  { "out", UVRGW_CONF_VAL_DIR_OUT },
  { NULL }
};

static const MAP_ITEM_T mqtt_val_type_map[] = {
  { "number", UVRGW_CONF_MQTT_TYPE_NUMBER },
  { "switch", UVRGW_CONF_MQTT_TYPE_SWITCH },
  { "contact", UVRGW_CONF_MQTT_TYPE_CONTACT },
  { NULL }
};

static const MAP_ITEM_T can_val_type_map[] = {
  { "bit", UVRGW_CONF_CAN_TYPE_BIT },
  { "u8", UVRGW_CONF_CAN_TYPE_U8 },
  { "s8", UVRGW_CONF_CAN_TYPE_S8 },
  { "u16", UVRGW_CONF_CAN_TYPE_U16 },
  { "s16", UVRGW_CONF_CAN_TYPE_S16 },
  { "u32", UVRGW_CONF_CAN_TYPE_U32 },
  { "s32", UVRGW_CONF_CAN_TYPE_S32 },
  { NULL }
};

static const MAP_ITEM_T mb_val_type_map[] = {
  { "bit", UVRGW_CONF_MB_TYPE_BIT },
  { "signed", UVRGW_CONF_MB_TYPE_SIGNED },
  { "unsigned", UVRGW_CONF_MB_TYPE_UNSIGNED },
  { "bitmask", UVRGW_CONF_MB_TYPE_BITMASK },
  { NULL }
};

static const MAP_ITEM_T mb_reg_type_map[] = {
  { "inbit", UVRGW_CONF_MB_REG_TYPE_INBIT },
  { "bit", UVRGW_CONF_MB_REG_TYPE_BIT },
  { "inreg", UVRGW_CONF_MB_REG_TYPE_INREG },
  { "reg", UVRGW_CONF_MB_REG_TYPE_REG },
  { NULL }
};

static const MAP_ITEM_T mb_parity_map[] = {
  { "none", UVRGW_CONF_MB_PARITY_NONE },
  { "even", UVRGW_CONF_MB_PARITY_EVEN },
  { "odd", UVRGW_CONF_MB_PARITY_ODD },
  { NULL }
};

static const MAP_ITEM_T mb_rtu_mode_map[] = {
  { "rs232", MODBUS_RTU_RS232 },
  { "rs485", MODBUS_RTU_RS485 },
  { NULL }
};

static const MAP_ITEM_T mb_rtu_rts_map[] = {
  { "none", MODBUS_RTU_RTS_NONE },
  { "up", MODBUS_RTU_RTS_UP },
  { "down", MODBUS_RTU_RTS_DOWN },
  { NULL }
};

static UVRGW_CONF_VAL_DISPATCH_T *disp;

static int parse_map(const MAP_ITEM_T *map, cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  for (; map->str != NULL; map++) {
    if (strcmp(map->str, value) == 0) {
      *((int *) result) = map->val;
      return 0;
    }
  }

  cfg_error(cfg, "Invalid value for option '%s': %s", cfg_opt_name(opt), value);
  return -1;
}

static int parse_val_dir(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(val_dir_map, cfg, opt, value, result);
}

static int parse_mqtt_val_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(mqtt_val_type_map, cfg, opt, value, result);
}

static int parse_can_val_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(can_val_type_map, cfg, opt, value, result);
}

static int parse_mb_val_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(mb_val_type_map, cfg, opt, value, result);
}

static int parse_mb_reg_type(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(mb_reg_type_map, cfg, opt, value, result);
}

static int parse_mb_parity(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(mb_parity_map, cfg, opt, value, result);
}

static int parse_mb_rtu_mode(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(mb_rtu_mode_map, cfg, opt, value, result);
}

static int parse_mb_rtu_rts(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(mb_rtu_rts_map, cfg, opt, value, result);
}

static int parse_can_id(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  int node, chan;
  char *p;

  if (strncmp(value, "ANA:", 4) == 0) {
    value += 4;
    node = strtol(value, &p, 10);
    if (*p != ':') {
      goto fail;
    }
    chan = strtol(p + 1, &p, 10);
    if (*p != 0) {
      goto fail;
    }
    *((int *) result) = 0x200 | (node & 0x3f) | (((chan) & 0x0c) << 5) | (((chan) & 0x10) << 2);
    return 0;
  }

  if (strncmp(value, "DIG:", 4) == 0) {
    value += 4;
    node = strtol(value, &p, 10);
    if (*p != 0) {
      goto fail;
    }
    *((int *) result) = 0x180 | (node & 0x3f);
    return 0;
  }

  if (strncmp(value, "0x", 2) == 0) {
    value += 2;
    *((int *) result) = strtol(value, &p, 16);
    if (*p != 0) {
      goto fail;
    }
    return 0;
  }

  *((int *) result) = strtol(value, &p, 10);
  if (*p != 0) {
    goto fail;
  }
  return 0;

fail:
  cfg_error(cfg, "Invalid value for option '%s': %s", cfg_opt_name(opt), value);
  return -1;
}

int uvrgw_conf_load(const char *file) {
  cfg_t *cfg;
  int err;

  cfg = cfg_init(opts, CFGF_NOCASE);
  if (cfg == NULL) {
    syslog(LOG_ERR, "Failed to create config file parser.");
    goto fail0;
  }

  err = cfg_parse(cfg, file);
  if (err == CFG_FILE_ERROR) {
    syslog(LOG_ERR, "Failed to open config file %s.", file);
    goto fail1;
  }
  if (err == CFG_PARSE_ERROR) {
    syslog(LOG_ERR, "Failed to parse config file %s.", file);
    goto fail1;
  }
  if (err != CFG_SUCCESS) {
    syslog(LOG_ERR, "Unknown error on loading config file %s.", file);
    goto fail1;
  }

  disp = NULL;
  can_init();

  if (can_configure(cfg)) {
    goto fail2;
  }

  init_dispatcher();
  can_register_disp_cbs();

  cfg_free(cfg);
  return 0;

fail2:
  uvrgw_conf_cleanup();
fail1:
  cfg_free(cfg);
fail0:
  return -1;
}

void uvrgw_conf_cleanup(void) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;
  UVRGW_CONF_VAL_DISPATCH_T *next;

  can_unconfigure();

  dp = disp;
  while (dp != NULL) {
    next = dp->next;
    free((void *) dp->name);
    free(dp->value_cbs);
    free(dp);
    dp = next;
  }
}

int uvrgw_conf_config_childs(cfg_t *cfg, const char *name, int *count, void **data, int size, void *ctx, UVRGW_CONF_CONFIG_CHILD_CB cccb) {
  int n, i;
  void *child;

  *count = 0;
  *data = NULL;

  n = cfg_size(cfg, name);
  if (n == 0) {
    return 0;
  }

  child = calloc(n, size);
  if (child == NULL) {
    syslog(LOG_ERR, "Failed to allocate child's data.");
    return -1;
  }

  *count = n;
  *data = child;

  for (i = 0; i < n; i++, child += size) {
    if (cccb(cfg_getnsec(cfg, name, i), ctx, child) < 0) {
      return -1;
    }
  }

  return 0;
}

UVRGW_CONF_VAL_DISPATCH_T *uvrgw_conf_register_val(const char *name) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;
  UVRGW_CONF_VAL_DISPATCH_T *last;

  dp = disp;
  last = NULL;
  while (dp != NULL) {
    if (strcmp(dp->name, name) == 0) {
      break;
    }
    last = dp;
    dp = dp->next;
  }

  if (dp == NULL) {
    dp = calloc(1, sizeof(UVRGW_CONF_VAL_DISPATCH_T));
    dp->name = strdup(name);
  }

  if (last == NULL) {
    disp = dp;
  } else {
    last->next = dp;
  }

  dp->value_count++;
  return dp;
}

void static init_dispatcher() {
  UVRGW_CONF_VAL_DISPATCH_T *dp;

  dp = disp;
  while (dp != NULL) {
    dp->value_cbs_pos = 0;
    dp->value_cbs = calloc(dp->value_count, sizeof(UVRGW_CONF_VAL_DISPATCH_T));
    dp = dp->next;
  }
}

int uvrgw_conf_register_disp_cb(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, UVRGW_CONF_DISPATCH_CB cb) {
  UVRGW_CONF_DISPATCH_CB_VAL_T *cbv;

  if (dp->value_cbs_pos >= dp->value_count) {
    syslog(LOG_ERR, "Callback count exceeded.");
    return -1;
  }

  cbv = &(dp->value_cbs[dp->value_cbs_pos]);
  dp->value_cbs_pos++;
  cbv->val = val;
  cbv->cb = cb;

  return 0;
}

void uvrgw_conf_disp_val(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f) {
  int i;
  UVRGW_CONF_DISPATCH_CB_VAL_T *cbv;

  for (cbv = dp->value_cbs, i = 0; i < dp->value_count; i++, cbv++) {
    if (cbv->cb != NULL && cbv->val != val) {
      // TODO: handle error
      cbv->cb(cbv->val, f);
    }
  }
}

