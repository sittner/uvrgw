/**
 * @file uvrgw_conf.c
 * @brief Configuration file parsing and value dispatch implementation.
 *
 * Uses libconfuse to parse the configuration file into a tree of sections.
 * Each top-level section (mqtt, json, can, modbus_rtu, modbus_tcp,
 * sunspec_server, counter, eval) is forwarded to the corresponding
 * subsystem's configure() function.  eval is configured last, because it
 * resolves the names used by all other modules.
 *
 * After configuration the dispatcher linked list is fully populated and
 * the per-entry callback arrays are allocated.  The register_disp_cbs
 * functions of each subsystem then fill in the callback arrays for all
 * output values.
 *
 * The watchdog thread resets values whose producer has a stale_timeout
 * and delivered no data within that time (see uvrgw_conf_startup()).
 */
#include <uvrgw_conf.h>

#include "can.h"
#include "mb.h"
#include "mqtt.h"
#include "rest.h"
#include "sunspec.h"
#include "counter.h"
#include "eval.h"
#include "utils.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <modbus/modbus.h>

#define DEFAULT_STATE_DIR "/var/lib/uvrgw"

#define WATCHDOG_THREAD_PERIOD_US 100000
#define WATCHDOG_CHECK_MS 1000

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
static int parse_counter_sign(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);
static int parse_can_id(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);

static char *owner_str(const char *module, const char *instance);
static int check_dispatchers(void);
static int init_dispatcher(void);
static void fire_cbs(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f, bool valid);
static void *watchdog_thread(void *ptr);
static void watchdog_check(UVRGW_CONF_VAL_DISPATCH_T *dp, int64_t now);

static cfg_opt_t mqtt_val_opts[] = {
  CFG_INT_CB("dir", -1, CFGF_NONE, parse_val_dir),
  CFG_INT_CB("type", -1, CFGF_NONE, parse_mqtt_val_type),
  CFG_STR("topic", NULL, CFGF_NONE),
  CFG_STR("fmt", NULL, CFGF_NONE),
  CFG_INT("qos", 0, CFGF_NODEFAULT),
  CFG_BOOL("retain", cfg_false, CFGF_NODEFAULT),
  CFG_FLOAT("init_value", 0.0, CFGF_NODEFAULT),
  CFG_INT("stale_timeout", 0, CFGF_NODEFAULT),
  CFG_END()
};

static cfg_opt_t mqtt_logger_val_opts[] = {
  CFG_STR("field", NULL, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_END()
};

static cfg_opt_t mqtt_logger_opts[] = {
  CFG_STR("topic", NULL, CFGF_NONE),
  CFG_INT("interval", 300, CFGF_NONE),
  CFG_INT("qos", 1, CFGF_NONE),
  CFG_SEC("value", mqtt_logger_val_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_END()
};

static cfg_opt_t mqtt_opts[] = {
  CFG_STR("host", "localhost", CFGF_NONE),
  CFG_INT("port", 1883, CFGF_NONE),
  CFG_STR("client_id", NULL, CFGF_NONE),
  CFG_STR("user", NULL, CFGF_NONE),
  CFG_STR("pwd", NULL, CFGF_NONE),
  CFG_STR("state_topic", NULL, CFGF_NONE),
  CFG_INT("keepalive_period", 300, CFGF_NONE),
  CFG_INT("qos", 0, CFGF_NONE),
  CFG_BOOL("retain", cfg_false, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NONE),
  CFG_INT("stale_timeout", 0, CFGF_NONE),
  CFG_SEC("value", mqtt_val_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_SEC("logger", mqtt_logger_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_END()
};

static cfg_opt_t json_val_opts[] = {
  CFG_STR("path", NULL, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_FLOAT("offset", 0.0, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NODEFAULT),
  CFG_INT("stale_timeout", 0, CFGF_NODEFAULT),
  CFG_END()
};

static cfg_opt_t json_opts[] = {
  CFG_STR("url", NULL, CFGF_NONE),
  CFG_INT("interval", 10000, CFGF_NONE),
  CFG_INT("timeout", 3000, CFGF_NONE),
  CFG_STR("user", NULL, CFGF_NONE),
  CFG_STR("pwd", NULL, CFGF_NONE),
  CFG_STR("valid_if", NULL, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NONE),
  CFG_INT("stale_timeout", 0, CFGF_NONE),
  CFG_SEC("value", json_val_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_END()
};

static cfg_opt_t can_frame_val_opts[] = {
  CFG_INT_CB("type", -1, CFGF_NONE, parse_can_val_type),
  CFG_INT("pos", -1, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_FLOAT("offset", 0.0, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NODEFAULT),
  CFG_INT("stale_timeout", 0, CFGF_NODEFAULT),
  CFG_END()
};

static cfg_opt_t can_frame_opts[] = {
  CFG_INT_CB("can_id", -1, CFGF_NONE, parse_can_id),
  CFG_INT_CB("dir", -1, CFGF_NONE, parse_val_dir),
  CFG_SEC("value", can_frame_val_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_END()
};

static cfg_opt_t can_opts[] = {
  CFG_STR("interface", NULL, CFGF_NONE),
  CFG_INT("timestamp_period", 0, CFGF_NONE),
  CFG_INT("send_timeout", 1000, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NONE),
  CFG_INT("stale_timeout", 0, CFGF_NONE),
  CFG_SEC("frame", can_frame_opts, CFGF_MULTI),
  CFG_END()
};

static cfg_opt_t mb_block_val_opts[] = {
  CFG_INT("reg", -1, CFGF_NONE),
  CFG_INT_CB("type", -1, CFGF_NONE, parse_mb_val_type),
  CFG_INT("bit", -1, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_FLOAT("offset", 0.0, CFGF_NONE),
  CFG_STR("scale_factor", NULL, CFGF_NONE),
  CFG_BOOL("word_swap", cfg_false, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NODEFAULT),
  CFG_INT("stale_timeout", 0, CFGF_NODEFAULT),
  CFG_END()
};

static cfg_opt_t mb_block_opts[] = {
  CFG_INT_CB("dir", -1, CFGF_NONE, parse_val_dir),
  CFG_INT_CB("regtype", -1, CFGF_NONE, parse_mb_reg_type),
  CFG_INT("addr", -1, CFGF_NONE),
  CFG_INT("count", -1, CFGF_NONE),
  CFG_BOOL("sunspec_na", cfg_false, CFGF_NONE),
  CFG_INT("expect_reg", -1, CFGF_NONE),
  CFG_INT("expect_value", -1, CFGF_NONE),
  CFG_SEC("value", mb_block_val_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_END()
};

static cfg_opt_t mb_slave_opts[] = {
  CFG_INT("id", -1, CFGF_NONE),
  CFG_INT("interval", 0, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NONE),
  CFG_INT("stale_timeout", 0, CFGF_NONE),
  CFG_SEC("block", mb_block_opts, CFGF_MULTI),
  CFG_END()
};

static cfg_opt_t mb_rtu_opts[] = {
  CFG_STR("interface", NULL, CFGF_NONE),
  CFG_INT("baud", 9600, CFGF_NONE),
  CFG_INT_CB("parity", UVRGW_CONF_MB_PARITY_NONE, CFGF_NONE, parse_mb_parity),
  CFG_INT("data_bits", 8, CFGF_NONE),
  CFG_INT("stop_bits", 1, CFGF_NONE),
  CFG_INT("separation_time", 0, CFGF_NONE),
  CFG_INT("timeout", 250, CFGF_NONE),
  CFG_INT_CB("mode", MODBUS_RTU_RS232, CFGF_NONE, parse_mb_rtu_mode),
  CFG_INT_CB("rts", MODBUS_RTU_RTS_NONE, CFGF_NONE, parse_mb_rtu_rts),
  CFG_INT("rts_delay", -1, CFGF_NONE),
  CFG_SEC("slave", mb_slave_opts, CFGF_MULTI),
  CFG_END()
};

static cfg_opt_t mb_tcp_opts[] = {
  CFG_STR("ip", NULL, CFGF_NONE),
  CFG_INT("port", MODBUS_TCP_DEFAULT_PORT, CFGF_NONE),
  CFG_INT("separation_time", 0, CFGF_NONE),
  CFG_INT("timeout", 250, CFGF_NONE),
  CFG_SEC("slave", mb_slave_opts, CFGF_MULTI),
  CFG_END()
};

static cfg_opt_t sunspec_meter_opts[] = {
  CFG_INT("unit_id", -1, CFGF_NONE),
  CFG_STR("manufacturer", NULL, CFGF_NONE),
  CFG_STR("model", NULL, CFGF_NONE),
  CFG_STR("options", NULL, CFGF_NONE),
  CFG_STR("version", NULL, CFGF_NONE),
  CFG_STR("serial", NULL, CFGF_NONE),
  CFG_STR("current_l1", NULL, CFGF_NONE),
  CFG_STR("current_l2", NULL, CFGF_NONE),
  CFG_STR("current_l3", NULL, CFGF_NONE),
  CFG_STR("voltage_l1", NULL, CFGF_NONE),
  CFG_STR("voltage_l2", NULL, CFGF_NONE),
  CFG_STR("voltage_l3", NULL, CFGF_NONE),
  CFG_STR("power_l1", NULL, CFGF_NONE),
  CFG_STR("power_l2", NULL, CFGF_NONE),
  CFG_STR("power_l3", NULL, CFGF_NONE),
  CFG_STR("pf_l1", NULL, CFGF_NONE),
  CFG_STR("pf_l2", NULL, CFGF_NONE),
  CFG_STR("pf_l3", NULL, CFGF_NONE),
  CFG_STR("energy_import_l1", NULL, CFGF_NONE),
  CFG_STR("energy_import_l2", NULL, CFGF_NONE),
  CFG_STR("energy_import_l3", NULL, CFGF_NONE),
  CFG_STR("energy_export_l1", NULL, CFGF_NONE),
  CFG_STR("energy_export_l2", NULL, CFGF_NONE),
  CFG_STR("energy_export_l3", NULL, CFGF_NONE),
  CFG_STR("power", NULL, CFGF_NONE),
  CFG_STR("energy_import", NULL, CFGF_NONE),
  CFG_STR("energy_export", NULL, CFGF_NONE),
  CFG_STR("frequency", NULL, CFGF_NONE),
  CFG_END()
};

static cfg_opt_t sunspec_server_opts[] = {
  CFG_STR("bind", "0.0.0.0", CFGF_NONE),
  CFG_INT("port", MODBUS_TCP_DEFAULT_PORT, CFGF_NONE),
  CFG_SEC("meter", sunspec_meter_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_END()
};

static cfg_opt_t counter_opts[] = {
  CFG_STR("source", NULL, CFGF_NONE),
  CFG_BOOL("integrate_power", cfg_false, CFGF_NONE),
  CFG_FLOAT("max_power", 0.0, CFGF_NONE),
  CFG_FLOAT("scale", 1.0, CFGF_NONE),
  CFG_INT_CB("sign", UVRGW_CONF_COUNTER_SIGN_POSITIVE, CFGF_NONE, parse_counter_sign),
  CFG_END()
};

static cfg_opt_t eval_val_opts[] = {
  CFG_STR("expr", NULL, CFGF_NONE),
  CFG_BOOL("local", cfg_false, CFGF_NONE),
  CFG_FLOAT("init_value", 0.0, CFGF_NONE),
  CFG_END()
};

static cfg_opt_t eval_opts[] = {
  CFG_INT("period", 0, CFGF_NODEFAULT),
  CFG_STR_LIST("triggers", NULL, CFGF_NONE),
  CFG_SEC("value", eval_val_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_END()
};

static cfg_opt_t opts[] = {
  CFG_STR("state_dir", NULL, CFGF_NONE),
  CFG_SEC("mqtt", mqtt_opts, CFGF_MULTI),
  CFG_SEC("json", json_opts, CFGF_MULTI),
  CFG_SEC("can", can_opts, CFGF_MULTI),
  CFG_SEC("modbus_rtu", mb_rtu_opts, CFGF_MULTI),
  CFG_SEC("modbus_tcp", mb_tcp_opts, CFGF_MULTI),
  CFG_SEC("sunspec_server", sunspec_server_opts, CFGF_MULTI),
  CFG_SEC("counter", counter_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
  CFG_SEC("eval", eval_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
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
  { "s16", UVRGW_CONF_MB_TYPE_S16 },
  { "u16", UVRGW_CONF_MB_TYPE_U16 },
  { "s32", UVRGW_CONF_MB_TYPE_S32 },
  { "u32", UVRGW_CONF_MB_TYPE_U32 },
  { "f32", UVRGW_CONF_MB_TYPE_F32 },
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

static const MAP_ITEM_T counter_sign_map[] = {
  { "positive", UVRGW_CONF_COUNTER_SIGN_POSITIVE },
  { "negative", UVRGW_CONF_COUNTER_SIGN_NEGATIVE },
  { NULL }
};

static UVRGW_CONF_VAL_DISPATCH_T *disp;
static char *state_dir;

static pthread_t watchdog;
static atomic_bool watchdog_running;

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

static int parse_counter_sign(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  return parse_map(counter_sign_map, cfg, opt, value, result);
}

static int parse_can_id(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result) {
  int node, chan;
  char *p;

  if (strncmp(value, "ANA:", 4) == 0) {
    node = strtol(value + 4, &p, 10);
    if (*p != ':') {
      goto fail;
    }
    chan = strtol(p + 1, &p, 10);
    // chan is the first channel of the frame (4 channels per frame)
    if (*p != 0 || node < 0 || node > 0x3f || chan < 0 || chan > 0x1f || (chan & 3) != 0) {
      goto fail;
    }
    *((int *) result) = 0x200 | (node & 0x3f) | (((chan) & 0x0c) << 5) | (((chan) & 0x10) << 2);
    return 0;
  }

  if (strncmp(value, "DIG:", 4) == 0) {
    node = strtol(value + 4, &p, 10);
    if (*p != 0 || node < 0 || node > 0x3f) {
      goto fail;
    }
    *((int *) result) = 0x180 | (node & 0x3f);
    return 0;
  }

  if (strncmp(value, "0x", 2) == 0) {
    *((int *) result) = strtol(value + 2, &p, 16);
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

/**
 * @brief Load configuration, configure all subsystems and set up value dispatch.
 *
 * @param file  Path to the libconfuse configuration file.
 * @return      0 on success, -1 on error.
 */
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

  // state directory: config, systemd StateDirectory= or default
  state_dir = uvrgw_conf_strdup(cfg_getstr(cfg, "state_dir"));
  if (state_dir == NULL) {
    state_dir = uvrgw_conf_strdup(getenv("STATE_DIRECTORY"));
  }
  if (state_dir == NULL) {
    state_dir = uvrgw_conf_strdup(DEFAULT_STATE_DIR);
  }

  can_init();
  mb_init();
  mqtt_init();
  rest_init();
  sunspec_init();
  counter_init();
  eval_init();

  if (can_configure(cfg)) {
    goto fail2;
  }

  if (mb_configure(cfg)) {
    goto fail2;
  }

  if (mqtt_configure(cfg)) {
    goto fail2;
  }

  if (rest_configure(cfg)) {
    goto fail2;
  }

  if (sunspec_configure(cfg)) {
    goto fail2;
  }

  if (counter_configure(cfg)) {
    goto fail2;
  }

  // evals last: all value names must be known
  if (eval_configure(cfg)) {
    goto fail2;
  }

  if (check_dispatchers()) {
    goto fail2;
  }

  if (init_dispatcher()) {
    goto fail2;
  }

  if (can_register_disp_cbs() || mb_register_disp_cbs() || mqtt_register_disp_cbs() ||
      counter_register_disp_cbs() || eval_register_disp_cbs()) {
    goto fail2;
  }

  cfg_free(cfg);
  return 0;

fail2:
  uvrgw_conf_cleanup();
fail1:
  cfg_free(cfg);
fail0:
  return -1;
}

/**
 * @brief Unconfigure all subsystems and free the dispatcher linked list.
 */
void uvrgw_conf_cleanup(void) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;
  UVRGW_CONF_VAL_DISPATCH_T *next;

  eval_unconfigure();
  counter_unconfigure();
  sunspec_unconfigure();
  rest_unconfigure();
  mqtt_unconfigure();
  can_unconfigure();
  mb_unconfigure();

  dp = disp;
  while (dp != NULL) {
    next = dp->next;
    pthread_mutex_destroy(&dp->disp_lock);
    pthread_mutex_destroy(&dp->last_lock);
    free((void *) dp->name);
    free(dp->producer);
    free(dp->timeout_reader);
    free(dp->value_cbs);
    free(dp);
    dp = next;
  }

  free(state_dir);
  state_dir = NULL;
}

/**
 * @brief Allocate child data and iterate over all child sections, calling @p cccb.
 *
 * @param cfg    Parent section.
 * @param name   Child section name.
 * @param count  Output: number of children.
 * @param data   Output: pointer to allocated array.
 * @param size   Byte size of each element.
 * @param ctx    Context forwarded to @p cccb.
 * @param cccb   Per-child configuration callback.
 * @return       0 on success, -1 on error.
 */
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

/**
 * @brief Duplicate a string, treating NULL input as a valid no-op.
 *
 * @param s  String to duplicate, or NULL.
 * @return   New copy, or NULL if @p s is NULL.
 */
char *uvrgw_conf_strdup(const char *s) {
  if (s == NULL) {
    return NULL;
  }

  return strdup(s);
}

/**
 * @brief Look up or create the dispatcher node for @p name.
 *
 * @param name      Value name.
 * @param alloc_cb  If true, increment the output callback counter.
 * @return          Dispatcher node pointer.
 */
UVRGW_CONF_VAL_DISPATCH_T *uvrgw_conf_get_dispatcher(const char *name, bool alloc_cb) {
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
    if (dp == NULL) {
      syslog(LOG_ERR, "Failed to allocate dispatcher for value '%s'.", name);
      return NULL;
    }
    dp->name = uvrgw_conf_strdup(name);
    if (dp->name == NULL) {
      syslog(LOG_ERR, "Failed to allocate dispatcher for value '%s'.", name);
      free(dp);
      return NULL;
    }
    pthread_mutex_init(&dp->disp_lock, NULL);
    pthread_mutex_init(&dp->last_lock, NULL);

    // append new dispatcher to list
    if (last == NULL) {
      disp = dp;
    } else {
      last->next = dp;
    }
  }

  if (alloc_cb) {
    dp->value_count++;
  }

  return dp;
}

/**
 * @brief Register the producer of a value (see header).
 *
 * @param dp             Dispatcher.
 * @param module         Module/section type.
 * @param instance       Section identification.
 * @param val            Producer's value pointer.
 * @param input          True for input values.
 * @param init_value     Start and reset value.
 * @param stale_timeout  Timeout in ms; 0 = never.
 * @return               0 on success, -1 on duplicate producer or OOM.
 */
int uvrgw_conf_set_producer(UVRGW_CONF_VAL_DISPATCH_T *dp, const char *module, const char *instance,
                            void *val, bool input, double init_value, int stale_timeout) {
  char *owner;

  owner = owner_str(module, instance);
  if (owner == NULL) {
    syslog(LOG_ERR, "Failed to allocate producer of value '%s'.", dp->name);
    return -1;
  }

  if (dp->producer != NULL) {
    syslog(LOG_ERR, "value '%s' of %s is already produced by %s.", dp->name, owner, dp->producer);
    free(owner);
    return -1;
  }

  if (!isfinite(init_value)) {
    syslog(LOG_ERR, "value '%s' of %s: init_value invalid.", dp->name, owner);
    free(owner);
    return -1;
  }

  if (stale_timeout < 0) {
    syslog(LOG_ERR, "value '%s' of %s: stale_timeout invalid.", dp->name, owner);
    free(owner);
    return -1;
  }

  dp->producer = owner;
  dp->producer_val = val;
  dp->producer_input = input;
  dp->init_value = init_value;
  dp->stale_timeout = stale_timeout;
  dp->last_value = init_value;
  return 0;
}

/**
 * @brief Register a reader that needs a value without stale data (see header).
 *
 * Only the first reader is kept, for the error message.
 *
 * @param dp        Dispatcher.
 * @param module    Reader's module/section type.
 * @param instance  Reader's section identification.
 * @return          0 on success, -1 on OOM.
 */
int uvrgw_conf_need_timeout(UVRGW_CONF_VAL_DISPATCH_T *dp, const char *module, const char *instance) {
  if (dp->timeout_reader != NULL) {
    return 0;
  }

  dp->timeout_reader = owner_str(module, instance);
  if (dp->timeout_reader == NULL) {
    syslog(LOG_ERR, "Failed to allocate reader of value '%s'.", dp->name);
    return -1;
  }

  return 0;
}

/**
 * @brief Format a module and instance as "module 'instance'".
 *
 * @param module    Module/section type.
 * @param instance  Section identification.
 * @return          Newly allocated string, or NULL on OOM.
 */
static char *owner_str(const char *module, const char *instance) {
  char *s;

  s = malloc(strlen(module) + strlen(instance) + 4);
  if (s != NULL) {
    sprintf(s, "%s '%s'", module, instance);
  }

  return s;
}

/**
 * @brief Check the producers of all names after configuration.
 *
 * Every name needs a producer, and a name read by a module that needs
 * current data (see uvrgw_conf_need_timeout()) must not be an input
 * without @c stale_timeout.
 *
 * @return  0 if the config is valid, -1 otherwise.
 */
static int check_dispatchers(void) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;

  for (dp = disp; dp != NULL; dp = dp->next) {
    if (dp->producer == NULL) {
      syslog(LOG_ERR, "value '%s' is not produced by any module.", dp->name);
      return -1;
    }

    if (dp->timeout_reader != NULL && dp->producer_input && dp->stale_timeout == 0) {
      syslog(LOG_ERR, "value '%s' of %s has no stale_timeout, but is read by %s.", dp->name, dp->producer, dp->timeout_reader);
      return -1;
    }
  }

  return 0;
}

/**
 * @brief Get the head of the dispatcher list.
 *
 * @return  First dispatcher, or NULL.
 */
UVRGW_CONF_VAL_DISPATCH_T *uvrgw_conf_get_dispatchers(void) {
  return disp;
}

static int init_dispatcher(void) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;

  dp = disp;
  while (dp != NULL) {
    dp->value_cbs_pos = 0;
    if (dp->value_count > 0) {
      dp->value_cbs = calloc(dp->value_count, sizeof(UVRGW_CONF_DISPATCH_CB_VAL_T));
      if (dp->value_cbs == NULL) {
        syslog(LOG_ERR, "Failed to allocate callbacks of value '%s'.", dp->name);
        return -1;
      }
    }
    dp = dp->next;
  }

  return 0;
}

/**
 * @brief Register an output callback in a dispatcher's preallocated slot.
 *
 * @param dp   Dispatcher.
 * @param val  Source value pointer.
 * @param cb   Callback function.
 * @return     0 on success, -1 if slots are exhausted.
 */
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

/**
 * @brief Store the value and fire all output callbacks, excluding the source.
 *
 * Non-finite values are dropped.
 *
 * @param dp     Dispatcher.
 * @param val    Source value pointer (excluded from delivery).
 * @param f      Dispatched value.
 * @param valid  True for current data.
 */
void uvrgw_conf_disp_val(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f, bool valid) {
  // keep the stored value finite
  if (!isfinite(f)) {
    return;
  }

  pthread_mutex_lock(&dp->disp_lock);

  // remember last value
  pthread_mutex_lock(&dp->last_lock);
  dp->last_value = f;
  dp->valid = valid;
  pthread_mutex_unlock(&dp->last_lock);

  if (valid) {
    dp->received = true;

    // re-arm the watchdog
    if (dp->stale_timeout > 0) {
      dp->deadline = utl_get_ticks() + dp->stale_timeout;
    }
    if (dp->timed_out) {
      syslog(LOG_INFO, "value '%s' received again.", dp->name);
      dp->timed_out = false;
    }
  }

  fire_cbs(dp, val, f, valid);

  pthread_mutex_unlock(&dp->disp_lock);
}

/**
 * @brief Call all output callbacks of @p dp except the one of @p val
 *        (called with @c disp_lock held).
 *
 * @param dp     Dispatcher.
 * @param val    Source value pointer (excluded from delivery).
 * @param f      Dispatched value.
 * @param valid  Validity.
 */
static void fire_cbs(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f, bool valid) {
  int i;
  UVRGW_CONF_DISPATCH_CB_VAL_T *cbv;

  for (cbv = dp->value_cbs, i = 0; i < dp->value_count; i++, cbv++) {
    if (cbv->cb != NULL && cbv->val != val) {
      // errors are logged by the callback (once per state change)
      cbv->cb(cbv->val, f, valid);
    }
  }
}

/**
 * @brief Read the current value and its validity.
 *
 * @param dp  Dispatcher.
 * @param f   Output: current value.
 * @return    true if the value is valid.
 */
bool uvrgw_conf_get_val(UVRGW_CONF_VAL_DISPATCH_T *dp, double *f) {
  bool valid;

  pthread_mutex_lock(&dp->last_lock);
  valid = dp->valid;
  *f = dp->last_value;
  pthread_mutex_unlock(&dp->last_lock);

  return valid;
}

/**
 * @brief Arm the timeouts and start the watchdog thread (see header).
 *
 * @return  0 on success, -1 if the thread could not be started.
 */
int uvrgw_conf_startup(void) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;
  int64_t now = utl_get_ticks();
  bool needed = false;

  // names that already delivered data are armed by their dispatch
  for (dp = disp; dp != NULL; dp = dp->next) {
    if (dp->stale_timeout > 0) {
      pthread_mutex_lock(&dp->disp_lock);
      if (dp->deadline == 0) {
        dp->deadline = now + dp->stale_timeout;
      }
      pthread_mutex_unlock(&dp->disp_lock);
      needed = true;
    }
  }

  if (!needed) {
    return 0;
  }

  watchdog_running = true;
  if (pthread_create(&watchdog, NULL, watchdog_thread, NULL) != 0) {
    watchdog_running = false;
    syslog(LOG_ERR, "failed to start watchdog thread");
    return -1;
  }

  return 0;
}

/**
 * @brief Stop the watchdog thread.
 */
void uvrgw_conf_shutdown(void) {
  if (watchdog_running) {
    watchdog_running = false;
    pthread_join(watchdog, NULL);
  }
}

/**
 * @brief Watchdog thread: check all timeouts once per second.
 *
 * @param ptr  Unused.
 * @return     NULL.
 */
static void *watchdog_thread(void *ptr) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;
  int64_t now;
  int64_t next_check = 0;

  while (watchdog_running) {
    now = utl_get_ticks();

    if (now >= next_check) {
      next_check = now + WATCHDOG_CHECK_MS;

      for (dp = disp; dp != NULL; dp = dp->next) {
        if (dp->stale_timeout > 0) {
          watchdog_check(dp, now);
        }
      }
    }

    usleep(WATCHDOG_THREAD_PERIOD_US);
  }

  return NULL;
}

/**
 * @brief Reset a value whose deadline has expired to its @c init_value,
 *        mark it invalid and dispatch the reset once.
 *
 * The deadline is checked under @c disp_lock, so real data dispatched
 * meanwhile wins.
 *
 * @param dp   Dispatcher with a @c stale_timeout.
 * @param now  Current monotonic time (ms).
 */
static void watchdog_check(UVRGW_CONF_VAL_DISPATCH_T *dp, int64_t now) {
  pthread_mutex_lock(&dp->disp_lock);

  if (dp->deadline != 0 && now >= dp->deadline) {
    dp->deadline = 0;
    dp->timed_out = true;

    pthread_mutex_lock(&dp->last_lock);
    dp->last_value = dp->init_value;
    dp->valid = false;
    pthread_mutex_unlock(&dp->last_lock);

    if (dp->received) {
      syslog(LOG_WARNING, "value '%s' timed out, reset to %g.", dp->name, dp->init_value);
    } else {
      syslog(LOG_WARNING, "value '%s' never received, set to %g.", dp->name, dp->init_value);
    }

    fire_cbs(dp, dp->producer_val, dp->init_value, false);
  }

  pthread_mutex_unlock(&dp->disp_lock);
}

/**
 * @brief Get the state directory for persistent data.
 *
 * @return  State directory path.
 */
const char *uvrgw_conf_state_dir(void) {
  return state_dir;
}
