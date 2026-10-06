/**
 * @file sunspec.c
 * @brief SunSpec smart meter emulation (Modbus TCP server).
 *
 * Each server runs a dedicated thread that accepts Modbus TCP clients and
 * answers "read holding registers" requests from a per-meter register
 * image, which is rebuilt from the current dispatcher values on each
 * request.
 *
 * Register layout (base address 40000):
 *  - 40000: "SunS" marker (2 registers)
 *  - 40002: common model 1 (ID, L = 65, Mn, Md, Opt, Vr, SN, DA)
 *  - 40069: float three-phase meter model 213 (ID, L = 124, 61 float32
 *           values, event bitfield32), float32 high word first
 *  - 40195: end model (ID = 0xffff, L = 0)
 */
#include "sunspec.h"
#include "utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <syslog.h>
#include <sys/select.h>
#include <sys/socket.h>

#define SERVER_SELECT_TIMEOUT_US 100000

#define MAP_START_ADDR 40000
#define MAP_REGS       197

#define COMMON_ID     1
#define COMMON_LEN    65
#define METER_ID      213
#define METER_LEN     124
#define END_ID        0xffff
#define END_LEN       0

#define PHASE_SHIFT_RAD (120.0 * M_PI / 180.0)

static int servers_count;
static SUNSPEC_SERVER_T *servers;

static const char *phase_keys[SUNSPEC_PH_COUNT] = {
  "current", "voltage", "power", "pf", "energy_import", "energy_export"
};

/** Values of one phase (or the totals) for the register image. */
typedef struct {
  double current;
  double voltage;
  double voltage_pp;
  double power;
  double apparent_power;
  double reactive_power;
  double pf;
  double energy_imp;
  double energy_exp;
} PHASE_VALS_T;

static int server_configure(cfg_t *cfg, void *ctx, void *child);
static int meter_configure(cfg_t *cfg, void *ctx, void *child);
static int src_configure(cfg_t *cfg, const char *key, SUNSPEC_METER_T *meter, SUNSPEC_SRC_T *src, bool power);
static int str_configure(cfg_t *cfg, const char *key, int max_len, const char **str);
static int server_startup(SUNSPEC_SERVER_T *server);
static void server_shutdown(SUNSPEC_SERVER_T *server);
static void *server_thread(void *ptr);
static void accept_client(SUNSPEC_SERVER_T *server);
static void close_client(SUNSPEC_SERVER_T *server, int idx);
static void handle_client(SUNSPEC_SERVER_T *server, int idx);
static SUNSPEC_METER_T *find_meter(SUNSPEC_SERVER_T *server, int unit_id);
static int update_image(SUNSPEC_METER_T *meter);
static bool src_get(SUNSPEC_SRC_T *src, double *f, const char **invalid);
static double phase_to_phase(double v1, double v2);
static int put_str(uint16_t *regs, const char *s, int len);
static int put_f32(uint16_t *regs, double f);

void sunspec_init(void) {
  servers_count = 0;
  servers = NULL;
}

int sunspec_configure(cfg_t *cfg) {
  return uvrgw_conf_config_childs(cfg, "sunspec_server", &servers_count, (void **) &servers, sizeof(SUNSPEC_SERVER_T), NULL, server_configure);
}

static int server_configure(cfg_t *cfg, void *ctx, void *child) {
  SUNSPEC_SERVER_T *server = (SUNSPEC_SERVER_T *) child;
  SUNSPEC_METER_T *meter, *cmp;
  int meter_idx, cmp_idx;
  int i;

  server->bind = uvrgw_conf_strdup(cfg_getstr(cfg, "bind"));
  server->port = cfg_getint(cfg, "port");

  server->listen_fd = -1;
  for (i = 0; i < SUNSPEC_MAX_CLIENTS; i++) {
    server->client_fds[i] = -1;
  }

  if (server->port <= 0 || server->port > 65535) {
    syslog(LOG_ERR, "sunspec_server port invalid.");
    return -1;
  }

  if (uvrgw_conf_config_childs(cfg, "meter", &server->meters_count, (void **) &server->meters, sizeof(SUNSPEC_METER_T), server, meter_configure) < 0) {
    return -1;
  }

  // check for unique unit IDs
  for (meter = server->meters, meter_idx = 0; meter_idx < server->meters_count; meter++, meter_idx++) {
    for (cmp = server->meters, cmp_idx = 0; cmp_idx < meter_idx; cmp++, cmp_idx++) {
      if (cmp->unit_id == meter->unit_id) {
        syslog(LOG_ERR, "sunspec meter '%s' uses unit_id %d of meter '%s'.", meter->name, meter->unit_id, cmp->name);
        return -1;
      }
    }
  }

  return 0;
}

static int meter_configure(cfg_t *cfg, void *ctx, void *child) {
  SUNSPEC_METER_T *meter = (SUNSPEC_METER_T *) child;
  char key[32];
  int ph, q;

  meter->server = (SUNSPEC_SERVER_T *) ctx;

  meter->name = uvrgw_conf_strdup(cfg_title(cfg));
  meter->unit_id = cfg_getint(cfg, "unit_id");

  if (meter->unit_id < 1 || meter->unit_id > 247) {
    syslog(LOG_ERR, "sunspec meter '%s' unit_id not given or invalid.", meter->name);
    return -1;
  }

  if (str_configure(cfg, "manufacturer", 32, &meter->manufacturer) < 0 ||
      str_configure(cfg, "model", 32, &meter->model) < 0 ||
      str_configure(cfg, "options", 16, &meter->options) < 0 ||
      str_configure(cfg, "version", 16, &meter->version) < 0 ||
      str_configure(cfg, "serial", 32, &meter->serial) < 0) {
    syslog(LOG_ERR, "sunspec meter '%s' configuration failed.", meter->name);
    return -1;
  }

  for (ph = 0; ph < SUNSPEC_PHASES; ph++) {
    for (q = 0; q < SUNSPEC_PH_COUNT; q++) {
      snprintf(key, sizeof(key), "%s_l%d", phase_keys[q], ph + 1);
      if (src_configure(cfg, key, meter, &meter->phase[ph][q], q == SUNSPEC_PH_POWER) < 0) {
        return -1;
      }
    }
  }

  if (src_configure(cfg, "power", meter, &meter->power, true) < 0 ||
      src_configure(cfg, "energy_import", meter, &meter->energy_imp, false) < 0 ||
      src_configure(cfg, "energy_export", meter, &meter->energy_exp, false) < 0 ||
      src_configure(cfg, "frequency", meter, &meter->frequency, false) < 0) {
    return -1;
  }

  meter->mapping = modbus_mapping_new_start_address(0, 0, 0, 0, MAP_START_ADDR, MAP_REGS, 0, 0);
  if (meter->mapping == NULL) {
    syslog(LOG_ERR, "Failed to allocate register map for sunspec meter '%s'.", meter->name);
    return -1;
  }

  return 0;
}

static int src_configure(cfg_t *cfg, const char *key, SUNSPEC_METER_T *meter, SUNSPEC_SRC_T *src, bool power) {
  src->name = uvrgw_conf_strdup(cfg_getstr(cfg, key));
  if (src->name == NULL) {
    return 0;
  }

  src->disp = uvrgw_conf_get_dispatcher(src->name, false);
  if (src->disp == NULL) {
    return -1;
  }

  // a frozen power would be served as current power
  if (power && uvrgw_conf_need_timeout(src->disp, "sunspec meter", meter->name) < 0) {
    return -1;
  }

  return 0;
}

static int str_configure(cfg_t *cfg, const char *key, int max_len, const char **str) {
  *str = uvrgw_conf_strdup(cfg_getstr(cfg, key));
  if (*str != NULL && strlen(*str) > (size_t) max_len) {
    syslog(LOG_ERR, "sunspec meter %s exceeds %d characters.", key, max_len);
    return -1;
  }

  return 0;
}

void sunspec_unconfigure(void) {
  SUNSPEC_SERVER_T *server;
  int server_idx;
  SUNSPEC_METER_T *meter;
  int meter_idx;
  int ph, q;

  for (server = servers, server_idx = 0; server_idx < servers_count; server++, server_idx++) {
    for (meter = server->meters, meter_idx = 0; meter_idx < server->meters_count; meter++, meter_idx++) {
      free((void *) meter->name);
      free((void *) meter->manufacturer);
      free((void *) meter->model);
      free((void *) meter->options);
      free((void *) meter->version);
      free((void *) meter->serial);
      for (ph = 0; ph < SUNSPEC_PHASES; ph++) {
        for (q = 0; q < SUNSPEC_PH_COUNT; q++) {
          free((void *) meter->phase[ph][q].name);
        }
      }
      free((void *) meter->power.name);
      free((void *) meter->energy_imp.name);
      free((void *) meter->energy_exp.name);
      free((void *) meter->frequency.name);
      if (meter->mapping != NULL) {
        modbus_mapping_free(meter->mapping);
      }
    }
    free((void *) server->bind);
    free(server->meters);
  }
  free(servers);
}

int sunspec_startup(void) {
  SUNSPEC_SERVER_T *server;
  int server_idx;

  for (server = servers, server_idx = 0; server_idx < servers_count; server++, server_idx++) {
    if (server_startup(server) < 0) {
      sunspec_shutdown();
      return -1;
    }
  }

  return 0;
}

static int server_startup(SUNSPEC_SERVER_T *server) {
  server->ctx = modbus_new_tcp(server->bind, server->port);
  if (server->ctx == NULL) {
    syslog(LOG_ERR, "Could not create sunspec modbus TCP instance");
    goto fail0;
  }

  server->listen_fd = modbus_tcp_listen(server->ctx, SUNSPEC_MAX_CLIENTS);
  if (server->listen_fd < 0) {
    syslog(LOG_ERR, "Could not listen on sunspec server port %d: %s", server->port, modbus_strerror(errno));
    goto fail1;
  }

  server->thread_running = true;
  if (pthread_create(&(server->thread), NULL, server_thread, (void *) server) != 0) {
    server->thread_running = false;
    syslog(LOG_ERR, "failed to start sunspec server thread");
    goto fail2;
  }

  return 0;

fail2:
  close(server->listen_fd);
  server->listen_fd = -1;
fail1:
  modbus_free(server->ctx);
  server->ctx = NULL;
fail0:
  return -1;
}

void sunspec_shutdown(void) {
  SUNSPEC_SERVER_T *server;
  int server_idx;

  for (server = servers, server_idx = 0; server_idx < servers_count; server++, server_idx++) {
    server_shutdown(server);
  }
}

static void server_shutdown(SUNSPEC_SERVER_T *server) {
  int i;

  if (server->thread_running) {
    server->thread_running = false;
    pthread_join(server->thread, NULL);
  }

  for (i = 0; i < SUNSPEC_MAX_CLIENTS; i++) {
    close_client(server, i);
  }

  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  if (server->ctx != NULL) {
    modbus_free(server->ctx);
    server->ctx = NULL;
  }
}

/**
 * @brief Server thread: wait for connections and requests.
 *
 * @param ptr  @c SUNSPEC_SERVER_T pointer cast to void *.
 * @return     NULL.
 */
static void *server_thread(void *ptr) {
  SUNSPEC_SERVER_T *server = (SUNSPEC_SERVER_T *) ptr;
  fd_set read_fd_set;
  struct timeval tv;
  int max_fd;
  int i;

  while (server->thread_running) {
    FD_ZERO(&read_fd_set);
    max_fd = 0;
    utl_update_fds(server->listen_fd, &read_fd_set, &max_fd);
    for (i = 0; i < SUNSPEC_MAX_CLIENTS; i++) {
      if (server->client_fds[i] >= 0) {
        utl_update_fds(server->client_fds[i], &read_fd_set, &max_fd);
      }
    }

    // wake up periodically to check for shutdown
    tv.tv_sec = 0;
    tv.tv_usec = SERVER_SELECT_TIMEOUT_US;
    if (select(max_fd + 1, &read_fd_set, NULL, NULL, &tv) < 0) {
      if (errno == EINTR) {
        continue;
      }
      syslog(LOG_ERR, "sunspec server select failed (error %d)", errno);
      break;
    }

    if (FD_ISSET(server->listen_fd, &read_fd_set)) {
      accept_client(server);
    }

    for (i = 0; i < SUNSPEC_MAX_CLIENTS; i++) {
      if (server->client_fds[i] >= 0 && FD_ISSET(server->client_fds[i], &read_fd_set)) {
        handle_client(server, i);
      }
    }
  }

  return NULL;
}

static void accept_client(SUNSPEC_SERVER_T *server) {
  int fd;
  int i;

  fd = accept(server->listen_fd, NULL, NULL);
  if (fd < 0) {
    syslog(LOG_WARNING, "sunspec server accept failed (error %d)", errno);
    return;
  }

  for (i = 0; i < SUNSPEC_MAX_CLIENTS; i++) {
    if (server->client_fds[i] < 0) {
      server->client_fds[i] = fd;
      return;
    }
  }

  syslog(LOG_WARNING, "sunspec server: too many clients, connection rejected.");
  close(fd);
}

static void close_client(SUNSPEC_SERVER_T *server, int idx) {
  if (server->client_fds[idx] >= 0) {
    close(server->client_fds[idx]);
    server->client_fds[idx] = -1;
  }
}

/**
 * @brief Receive and answer one request of a client.
 *
 * Requests for unknown unit IDs are not answered (like a missing device).
 * Only function 3 (read holding registers) is supported; the address
 * range check is done by modbus_reply().
 *
 * @param server  Server.
 * @param idx     Client index.
 */
static void handle_client(SUNSPEC_SERVER_T *server, int idx) {
  uint8_t req[MODBUS_TCP_MAX_ADU_LENGTH];
  SUNSPEC_METER_T *meter;
  int hdr_len;
  int len;
  int ret;

  modbus_set_socket(server->ctx, server->client_fds[idx]);

  len = modbus_receive(server->ctx, req);
  if (len < 0) {
    // connection closed by client or invalid frame
    close_client(server, idx);
    return;
  }
  if (len == 0) {
    return;
  }

  hdr_len = modbus_get_header_length(server->ctx);
  meter = find_meter(server, req[hdr_len - 1]);
  if (meter == NULL) {
    return;
  }

  if (req[hdr_len] != MODBUS_FC_READ_HOLDING_REGISTERS) {
    ret = modbus_reply_exception(server->ctx, req, MODBUS_EXCEPTION_ILLEGAL_FUNCTION);
  } else if (update_image(meter) < 0) {
    ret = modbus_reply_exception(server->ctx, req, MODBUS_EXCEPTION_SLAVE_OR_SERVER_FAILURE);
  } else {
    ret = modbus_reply(server->ctx, req, len, meter->mapping);
  }

  if (ret < 0) {
    close_client(server, idx);
  }
}

static SUNSPEC_METER_T *find_meter(SUNSPEC_SERVER_T *server, int unit_id) {
  SUNSPEC_METER_T *meter;
  int meter_idx;

  for (meter = server->meters, meter_idx = 0; meter_idx < server->meters_count; meter++, meter_idx++) {
    if (meter->unit_id == unit_id) {
      return meter;
    }
  }

  return NULL;
}

/**
 * @brief Rebuild the register image of a meter from the current values.
 *
 * @param meter  Meter to update.
 * @return       0 on success, -1 if a configured source value is invalid.
 */
static int update_image(SUNSPEC_METER_T *meter) {
  PHASE_VALS_T ph[SUNSPEC_PHASES];
  PHASE_VALS_T tot;
  bool has[SUNSPEC_PHASES][SUNSPEC_PH_COUNT];
  double vals[SUNSPEC_PHASES][SUNSPEC_PH_COUNT];
  double f, frequency;
  const char *invalid = NULL;
  uint16_t *regs;
  int i, q, n, v_count, vpp_count;

  memset(ph, 0, sizeof(ph));
  memset(&tot, 0, sizeof(tot));

  // get source values
  for (i = 0; i < SUNSPEC_PHASES; i++) {
    for (q = 0; q < SUNSPEC_PH_COUNT; q++) {
      vals[i][q] = 0.0;
      has[i][q] = src_get(&meter->phase[i][q], &vals[i][q], &invalid);
    }
  }

  // per phase values
  for (i = 0; i < SUNSPEC_PHASES; i++) {
    ph[i].current = vals[i][SUNSPEC_PH_CURRENT];
    ph[i].voltage = vals[i][SUNSPEC_PH_VOLTAGE];
    ph[i].power = vals[i][SUNSPEC_PH_POWER];
    ph[i].energy_imp = vals[i][SUNSPEC_PH_ENERGY_IMP];
    ph[i].energy_exp = vals[i][SUNSPEC_PH_ENERGY_EXP];

    if (has[i][SUNSPEC_PH_VOLTAGE] && has[i][SUNSPEC_PH_CURRENT]) {
      ph[i].apparent_power = ph[i].voltage * ph[i].current;
      if (has[i][SUNSPEC_PH_POWER]) {
        ph[i].reactive_power = sqrt(fabs(ph[i].apparent_power * ph[i].apparent_power - ph[i].power * ph[i].power));
      }
    }

    if (has[i][SUNSPEC_PH_PF]) {
      ph[i].pf = vals[i][SUNSPEC_PH_PF];
    } else if (ph[i].apparent_power > 0.0) {
      ph[i].pf = ph[i].power / ph[i].apparent_power;
    }
  }

  // phase to phase voltages (L1-L2, L2-L3, L3-L1)
  for (i = 0; i < SUNSPEC_PHASES; i++) {
    n = (i + 1) % SUNSPEC_PHASES;
    if (has[i][SUNSPEC_PH_VOLTAGE] && has[n][SUNSPEC_PH_VOLTAGE]) {
      ph[i].voltage_pp = phase_to_phase(ph[i].voltage, ph[n].voltage);
    }
  }

  // totals / averages
  v_count = 0;
  vpp_count = 0;
  for (i = 0; i < SUNSPEC_PHASES; i++) {
    n = (i + 1) % SUNSPEC_PHASES;
    tot.current += ph[i].current;
    tot.power += ph[i].power;
    tot.apparent_power += ph[i].apparent_power;
    tot.reactive_power += ph[i].reactive_power;
    tot.energy_imp += ph[i].energy_imp;
    tot.energy_exp += ph[i].energy_exp;
    if (has[i][SUNSPEC_PH_VOLTAGE]) {
      tot.voltage += ph[i].voltage;
      v_count++;
    }
    if (has[i][SUNSPEC_PH_VOLTAGE] && has[n][SUNSPEC_PH_VOLTAGE]) {
      tot.voltage_pp += ph[i].voltage_pp;
      vpp_count++;
    }
  }
  if (v_count > 0) {
    tot.voltage /= v_count;
  }
  if (vpp_count > 0) {
    tot.voltage_pp /= vpp_count;
  }

  // explicitly given totals override the sums
  f = 0.0;
  if (src_get(&meter->power, &f, &invalid)) {
    tot.power = f;
  }
  if (src_get(&meter->energy_imp, &f, &invalid)) {
    tot.energy_imp = f;
  }
  if (src_get(&meter->energy_exp, &f, &invalid)) {
    tot.energy_exp = f;
  }
  frequency = 0.0;
  src_get(&meter->frequency, &frequency, &invalid);

  if (tot.apparent_power > 0.0) {
    tot.pf = tot.power / tot.apparent_power;
  }

  // report state changes
  if (invalid != NULL) {
    if (!meter->unavailable) {
      syslog(LOG_WARNING, "sunspec meter '%s' unavailable: value '%s' is invalid.", meter->name, invalid);
      meter->unavailable = true;
    }
    return -1;
  }
  if (meter->unavailable) {
    syslog(LOG_INFO, "sunspec meter '%s' available.", meter->name);
    meter->unavailable = false;
  }

  // build register image
  regs = meter->mapping->tab_registers;
  memset(regs, 0, MAP_REGS * sizeof(uint16_t));

  // SunSpec marker and common model
  regs += put_str(regs, "SunS", 4);
  *(regs++) = COMMON_ID;
  *(regs++) = COMMON_LEN;
  regs += put_str(regs, meter->manufacturer, 32);
  regs += put_str(regs, meter->model, 32);
  regs += put_str(regs, meter->options, 16);
  regs += put_str(regs, meter->version, 16);
  regs += put_str(regs, meter->serial, 32);
  *(regs++) = meter->unit_id;

  // float three phase meter model
  *(regs++) = METER_ID;
  *(regs++) = METER_LEN;

#define PUT_ALL(field) \
  regs += put_f32(regs, tot.field); \
  for (i = 0; i < SUNSPEC_PHASES; i++) { \
    regs += put_f32(regs, ph[i].field); \
  }

  PUT_ALL(current);
  PUT_ALL(voltage);
  PUT_ALL(voltage_pp);
  regs += put_f32(regs, frequency);
  PUT_ALL(power);
  PUT_ALL(apparent_power);
  PUT_ALL(reactive_power);
  PUT_ALL(pf);
  PUT_ALL(energy_exp);
  PUT_ALL(energy_imp);

#undef PUT_ALL

  // not supported: VAh export/import, VArh Q1..Q4 (4 values each)
  regs += 6 * 4 * 2;

  // events (none)
  regs += 2;

  // end model
  *(regs++) = END_ID;
  *(regs++) = END_LEN;

  return 0;
}

/**
 * @brief Get a source value, checking its validity.
 *
 * @param src      Source reference.
 * @param f        Output: value (unchanged if not configured or invalid).
 * @param invalid  Output: set to the source name if the value is invalid
 *                 (only if not already set, so the first invalid value is reported).
 * @return         true if the value is configured and valid.
 */
static bool src_get(SUNSPEC_SRC_T *src, double *f, const char **invalid) {
  double v;

  if (src->disp == NULL) {
    return false;
  }

  if (!uvrgw_conf_get_val(src->disp, &v)) {
    if (*invalid == NULL) {
      *invalid = src->name;
    }
    return false;
  }

  *f = v;
  return true;
}

/**
 * @brief Calculate phase-to-phase voltage from two phase-to-neutral voltages.
 *
 * Assumes a phase shift of 120° between the phases.
 *
 * @param v1  Phase-to-neutral voltage of the first phase.
 * @param v2  Phase-to-neutral voltage of the second phase.
 * @return    Phase-to-phase voltage.
 */
static double phase_to_phase(double v1, double v2) {
  double parallel = v1 - v2 * cos(PHASE_SHIFT_RAD);
  double perpendicular = v2 * sin(PHASE_SHIFT_RAD);

  return sqrt(parallel * parallel + perpendicular * perpendicular);
}

/**
 * @brief Write a string into registers (2 chars per register, NUL padded).
 *
 * @param regs  Destination registers.
 * @param s     String (NULL is written as empty string).
 * @param len   Field length in characters (even).
 * @return      Number of registers written.
 */
static int put_str(uint16_t *regs, const char *s, int len) {
  int i, n;

  n = (s != NULL) ? strlen(s) : 0;
  for (i = 0; i < len; i += 2) {
    regs[i / 2] = ((i < n ? (uint8_t) s[i] : 0) << 8) | (i + 1 < n ? (uint8_t) s[i + 1] : 0);
  }

  return len / 2;
}

/**
 * @brief Write a float32 value into two registers (high word first).
 *
 * @param regs  Destination registers.
 * @param f     Value.
 * @return      Number of registers written (2).
 */
static int put_f32(uint16_t *regs, double f) {
  float f32 = (float) f;
  uint32_t u32;

  memcpy(&u32, &f32, sizeof(u32));
  regs[0] = u32 >> 16;
  regs[1] = u32 & 0xffff;

  return 2;
}
