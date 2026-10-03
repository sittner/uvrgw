/**
 * @file uvlua.c
 * @brief Embedded Lua interpreter: derived values (@c calc).
 *
 * One Lua state, used at config load (compile) and afterwards only by
 * the Lua thread.  The dispatcher update hook runs in
 * the threads of the sources: it only sets the dirty flags of the calcs
 * depending on the updated value and wakes the Lua thread.
 *
 * Calcs are evaluated in dependency order (calcs read by another calc
 * first), so a calc depending on another calc is evaluated once per
 * change.  The order is recomputed when the dependencies change.  The
 * results are dispatched from the Lua thread without holding the module
 * lock, so calcs and counters can use each other as sources.
 *
 * Sandbox: only the math library is loaded.  Expressions run in a shared
 * environment table with the provided functions; its metatable resolves
 * all other global names to values and forbids global assignments.  Each
 * evaluation is limited to UVLUA_MAX_INSTRUCTIONS, the whole state to
 * UVLUA_MEM_LIMIT bytes.
 */
#include "uvlua.h"
#include "counter.h"
#include "utils.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <syslog.h>

#define UVLUA_WAIT_MS 1000
#define UVLUA_CHECK_INTERVAL_MS 1000
#define UVLUA_NEVER_RECEIVED_MS 600000
#define UVLUA_MAX_INSTRUCTIONS 1000000
#define UVLUA_MEM_LIMIT (16 * 1024 * 1024)

#define CALC_STATE_OK      0
#define CALC_STATE_MISSING 1
#define CALC_STATE_STALE   2
#define CALC_STATE_ERROR   3
#define CALC_STATE_INVALID 4

#define ORDER_NONE   0
#define ORDER_ACTIVE 1
#define ORDER_DONE   2

static const char * const keywords[] = {
  "and", "break", "do", "else", "elseif", "end", "false", "for",
  "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
  "return", "then", "true", "until", "while", "_ENV", NULL
};

static const char * const math_funcs[] = {
  "min", "max", "abs", "floor", "ceil", NULL
};

static int calcs_count;
static UVLUA_CALC_T *calcs;
static UVLUA_CALC_T **calc_order;

static lua_State *L;
static size_t mem_used;
static int env_ref;

static pthread_mutex_t lock;
static pthread_cond_t cond;
static bool pending;
static bool thread_running;
static pthread_t thread;
static int64_t start_ticks;

// evaluation context (Lua thread)
static UVLUA_CALC_T *eval_calc;
static int eval_deps_count;
static int eval_deps_size;
static UVRGW_CONF_VAL_DISPATCH_T **eval_deps;
static UVRGW_CONF_VAL_DISPATCH_T *abort_dp;
static bool abort_stale;
static char abort_marker;

static int calc_configure_one(cfg_t *cfg, void *ctx, void *child);
static bool valid_name(const char *name);
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
static int l_panic(lua_State *L);
static int create_state(void);
static int compile_calc(UVLUA_CALC_T *c);
static int run_calc(UVLUA_CALC_T *c);
static int l_index(lua_State *L);
static int l_newindex(lua_State *L);
static int l_clamp(lua_State *L);
static void instruction_hook(lua_State *L, lua_Debug *ar);
static int add_dep(UVRGW_CONF_VAL_DISPATCH_T *dp);
static bool update_deps(UVLUA_CALC_T *c);
static UVLUA_CALC_T *find_calc_by_disp(UVRGW_CONF_VAL_DISPATCH_T *dp);
static void sort_calcs(void);
static int sort_calc(UVLUA_CALC_T *c, int *pos);
static void value_update(UVRGW_CONF_VAL_DISPATCH_T *dp);
static void *uvlua_thread(void *ptr);
static bool eval(UVLUA_CALC_T *c);
static void check_never_received(int64_t now);

void uvlua_init(void) {
  pthread_condattr_t attr;

  calcs_count = 0;
  calcs = NULL;
  calc_order = NULL;
  L = NULL;
  mem_used = 0;
  pending = false;
  thread_running = false;
  eval_calc = NULL;
  eval_deps_count = 0;
  eval_deps_size = 0;
  eval_deps = NULL;

  pthread_mutex_init(&lock, NULL);
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&cond, &attr);
  pthread_condattr_destroy(&attr);
}

int uvlua_configure(cfg_t *cfg) {
  UVLUA_CALC_T *c;
  int idx;

  if (uvrgw_conf_config_childs(cfg, "calc", &calcs_count, (void **) &calcs, sizeof(UVLUA_CALC_T), NULL, calc_configure_one) < 0) {
    return -1;
  }

  if (calcs_count <= 0) {
    return 0;
  }

  // calc and counter would both publish the value
  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (counter_exists(c->name)) {
      syslog(LOG_ERR, "calc '%s': a counter with the same name exists.", c->name);
      return -1;
    }
  }

  // evaluation order: config order until the dependencies are known
  calc_order = calloc(calcs_count, sizeof(UVLUA_CALC_T *));
  if (calc_order == NULL) {
    syslog(LOG_ERR, "Failed to allocate calc order.");
    return -1;
  }
  for (idx = 0; idx < calcs_count; idx++) {
    calc_order[idx] = &calcs[idx];
  }

  if (create_state() < 0) {
    return -1;
  }

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (compile_calc(c) < 0) {
      return -1;
    }
  }

  uvrgw_conf_set_update_hook(value_update);

  return 0;
}

static int calc_configure_one(cfg_t *cfg, void *ctx, void *child) {
  UVLUA_CALC_T *c = (UVLUA_CALC_T *) child;

  c->name = uvrgw_conf_strdup(cfg_title(cfg));
  c->expr = uvrgw_conf_strdup(cfg_getstr(cfg, "expr"));
  c->max_age = cfg_getint(cfg, "max_age");
  c->state = CALC_STATE_OK;

  if (!valid_name(c->name)) {
    syslog(LOG_ERR, "calc name '%s' invalid (must be a Lua identifier: A-Z a-z 0-9 _, not starting with a digit, no keyword or provided function).", c->name);
    return -1;
  }

  if (c->expr == NULL) {
    syslog(LOG_ERR, "calc '%s': expr not given.", c->name);
    return -1;
  }

  if (c->max_age < 0) {
    syslog(LOG_ERR, "calc '%s': max_age invalid.", c->name);
    return -1;
  }

  c->disp = uvrgw_conf_get_dispatcher(c->name, false);
  if (c->disp == NULL) {
    return -1;
  }

  return 0;
}

/**
 * @brief Check that a calc name can be used in expressions.
 *
 * @param name  Calc name.
 * @return      true if it is a Lua identifier, no keyword and no provided
 *              function.
 */
static bool valid_name(const char *name) {
  const char * const *k;
  const char *p;

  if (name == NULL || !((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') || *name == '_')) {
    return false;
  }

  for (p = name; *p != 0; p++) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
      return false;
    }
  }

  for (k = keywords; *k != NULL; k++) {
    if (strcmp(*k, name) == 0) {
      return false;
    }
  }

  for (k = math_funcs; *k != NULL; k++) {
    if (strcmp(*k, name) == 0) {
      return false;
    }
  }

  return strcmp(name, "math") != 0 && strcmp(name, "clamp") != 0;
}

/**
 * @brief Lua allocator with a limit for the whole state.
 */
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  size_t old = (ptr != NULL) ? osize : 0;
  void *p;

  if (nsize == 0) {
    free(ptr);
    mem_used -= old;
    return NULL;
  }

  if (nsize > old && mem_used + (nsize - old) > UVLUA_MEM_LIMIT) {
    return NULL;
  }

  p = realloc(ptr, nsize);
  if (p == NULL) {
    return NULL;
  }

  mem_used = mem_used - old + nsize;
  return p;
}

static int l_panic(lua_State *L) {
  syslog(LOG_CRIT, "lua: unprotected error: %s", lua_tostring(L, -1));
  return 0;
}

/**
 * @brief Create the Lua state and the expression environment.
 *
 * @return  0 on success, -1 on error.
 */
static int create_state(void) {
  const char * const *f;

  L = lua_newstate(l_alloc, NULL);
  if (L == NULL) {
    syslog(LOG_ERR, "Failed to create Lua state.");
    return -1;
  }
  lua_atpanic(L, l_panic);

  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 0);

  // environment: provided functions
  lua_newtable(L);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "math");
  for (f = math_funcs; *f != NULL; f++) {
    lua_getfield(L, -2, *f);
    lua_setfield(L, -2, *f);
  }
  lua_pushcfunction(L, l_clamp);
  lua_setfield(L, -2, "clamp");

  // metatable: value resolution, no global assignments
  lua_newtable(L);
  lua_pushcfunction(L, l_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, l_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -2);

  env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_pop(L, 1);

  return 0;
}

/**
 * @brief Compile the expression of a calc with the shared environment.
 *
 * @param c  Calc.
 * @return   0 on success, -1 on syntax error.
 */
static int compile_calc(UVLUA_CALC_T *c) {
  static const char prefix[] = "return ";
  char *code;
  size_t len;
  int err;

  len = strlen(prefix) + strlen(c->expr);
  code = malloc(len + 1);
  if (code == NULL) {
    syslog(LOG_ERR, "Failed to allocate calc code.");
    return -1;
  }
  sprintf(code, "%s%s", prefix, c->expr);

  err = luaL_loadbufferx(L, code, len, "=expr", "t");
  free(code);
  if (err != LUA_OK) {
    syslog(LOG_ERR, "calc '%s': invalid expression: %s", c->name, lua_tostring(L, -1));
    lua_pop(L, 1);
    return -1;
  }

  // the first upvalue of a main chunk is _ENV
  lua_rawgeti(L, LUA_REGISTRYINDEX, env_ref);
  lua_setupvalue(L, -2, 1);

  c->func_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  return 0;
}

/**
 * @brief Evaluate the expression of a calc, recording the values read in
 *        @c eval_deps.
 *
 * Leaves the result or the error object on the stack.
 *
 * @param c  Calc.
 * @return   lua_pcall() result.
 */
static int run_calc(UVLUA_CALC_T *c) {
  int err;

  eval_calc = c;
  eval_deps_count = 0;
  abort_dp = NULL;

  lua_rawgeti(L, LUA_REGISTRYINDEX, c->func_ref);
  lua_sethook(L, instruction_hook, LUA_MASKCOUNT, UVLUA_MAX_INSTRUCTIONS);
  err = lua_pcall(L, 0, 1, 0);
  lua_sethook(L, NULL, 0, 0);

  eval_calc = NULL;
  return err;
}

/**
 * @brief Environment __index: resolve a global name to its current value.
 *
 * Aborts the evaluation (error object @c abort_marker) if the value was
 * never received or is older than @c max_age.
 */
static int l_index(lua_State *L) {
  const char *name = lua_tostring(L, 2);
  UVRGW_CONF_VAL_DISPATCH_T *dp = NULL;
  int i;
  double f;
  int64_t ts;

  if (name == NULL || eval_calc == NULL) {
    return luaL_error(L, "invalid name");
  }

  // values read last time first (avoids walking the dispatcher list)
  for (i = 0; i < eval_calc->deps_count; i++) {
    if (strcmp(eval_calc->deps[i]->name, name) == 0) {
      dp = eval_calc->deps[i];
      break;
    }
  }
  if (dp == NULL) {
    dp = uvrgw_conf_find_dispatcher(name);
  }
  if (dp == NULL) {
    return luaL_error(L, "unknown value '%s'", name);
  }

  if (add_dep(dp) < 0) {
    return luaL_error(L, "not enough memory");
  }

  if (!uvrgw_conf_get_val(dp, &f, &ts)) {
    abort_dp = dp;
    abort_stale = false;
    lua_pushlightuserdata(L, &abort_marker);
    return lua_error(L);
  }

  if (eval_calc->max_age > 0 && utl_get_ticks() - ts > eval_calc->max_age) {
    abort_dp = dp;
    abort_stale = true;
    lua_pushlightuserdata(L, &abort_marker);
    return lua_error(L);
  }

  lua_pushnumber(L, f);
  return 1;
}

static int l_newindex(lua_State *L) {
  return luaL_error(L, "assignment to global '%s' not allowed", lua_tostring(L, 2));
}

/**
 * @brief clamp(x, lo, hi): limit x to [lo, hi].
 */
static int l_clamp(lua_State *L) {
  lua_Number x = luaL_checknumber(L, 1);
  lua_Number lo = luaL_checknumber(L, 2);
  lua_Number hi = luaL_checknumber(L, 3);

  lua_pushnumber(L, utl_val_limit(x, lo, hi));
  return 1;
}

static void instruction_hook(lua_State *L, lua_Debug *ar) {
  luaL_error(L, "instruction limit exceeded");
}

/**
 * @brief Record a value read by the current evaluation.
 *
 * @param dp  Dispatcher of the value.
 * @return    0 on success, -1 on allocation error.
 */
static int add_dep(UVRGW_CONF_VAL_DISPATCH_T *dp) {
  UVRGW_CONF_VAL_DISPATCH_T **deps;
  int i;

  for (i = 0; i < eval_deps_count; i++) {
    if (eval_deps[i] == dp) {
      return 0;
    }
  }

  if (eval_deps_count >= eval_deps_size) {
    deps = realloc(eval_deps, (eval_deps_size + 8) * sizeof(UVRGW_CONF_VAL_DISPATCH_T *));
    if (deps == NULL) {
      return -1;
    }
    eval_deps = deps;
    eval_deps_size += 8;
  }

  eval_deps[eval_deps_count++] = dp;
  return 0;
}

/**
 * @brief Take the values read by the last evaluation as dependencies.
 *
 * @param c  Calc.
 * @return   true if the dependencies changed.
 */
static bool update_deps(UVLUA_CALC_T *c) {
  UVRGW_CONF_VAL_DISPATCH_T **deps = NULL;
  UVRGW_CONF_VAL_DISPATCH_T **old;

  if (eval_deps_count == c->deps_count && (eval_deps_count == 0 || memcmp(eval_deps, c->deps, eval_deps_count * sizeof(UVRGW_CONF_VAL_DISPATCH_T *)) == 0)) {
    return false;
  }

  if (eval_deps_count > 0) {
    deps = malloc(eval_deps_count * sizeof(UVRGW_CONF_VAL_DISPATCH_T *));
    if (deps == NULL) {
      syslog(LOG_ERR, "calc '%s': failed to allocate dependencies.", c->name);
      return false;
    }
    memcpy(deps, eval_deps, eval_deps_count * sizeof(UVRGW_CONF_VAL_DISPATCH_T *));
  }

  pthread_mutex_lock(&lock);
  old = c->deps;
  c->deps = deps;
  c->deps_count = eval_deps_count;
  pthread_mutex_unlock(&lock);

  free(old);
  return true;
}

static UVLUA_CALC_T *find_calc_by_disp(UVRGW_CONF_VAL_DISPATCH_T *dp) {
  UVLUA_CALC_T *c;
  int idx;

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (c->disp == dp) {
      return c;
    }
  }

  return NULL;
}

/**
 * @brief Compute the evaluation order (calcs read by a calc first).
 *
 * On a cyclic dependency the calc closing the cycle is disabled and the
 * order computed again.
 */
static void sort_calcs(void) {
  UVLUA_CALC_T *c;
  int idx;
  int pos;
  int err;

  do {
    for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
      c->order = ORDER_NONE;
    }

    pos = 0;
    err = 0;
    for (c = calcs, idx = 0; idx < calcs_count && err == 0; c++, idx++) {
      err = sort_calc(c, &pos);
    }
  } while (err != 0);
}

/**
 * @brief Append a calc to the evaluation order after the calcs it reads
 *        (depth first search).
 *
 * @param c    Calc.
 * @param pos  Next position in @c calc_order.
 * @return     0 on success, 1 if a calc was disabled (sort again).
 */
static int sort_calc(UVLUA_CALC_T *c, int *pos) {
  UVLUA_CALC_T *dep;
  int i;
  int err;

  if (c->order == ORDER_DONE) {
    return 0;
  }

  c->order = ORDER_ACTIVE;

  // disabled calcs are not evaluated: their dependencies do not matter
  for (i = 0; i < c->deps_count && !c->disabled; i++) {
    dep = find_calc_by_disp(c->deps[i]);
    if (dep == NULL) {
      continue;
    }
    if (dep->order == ORDER_ACTIVE) {
      syslog(LOG_ERR, "calc '%s': cyclic dependency via '%s', disabled.", c->name, dep->name);
      c->disabled = true;
      return 1;
    }
    err = sort_calc(dep, pos);
    if (err != 0) {
      return err;
    }
  }

  c->order = ORDER_DONE;
  calc_order[(*pos)++] = c;

  return 0;
}

/**
 * @brief Dispatcher update hook: mark the calcs reading the value as
 *        changed.
 *
 * Runs in the thread of the source, so it only sets flags.
 *
 * @param dp  Dispatcher of the updated value.
 */
static void value_update(UVRGW_CONF_VAL_DISPATCH_T *dp) {
  UVLUA_CALC_T *c;
  int idx;
  int i;
  bool wake = false;

  pthread_mutex_lock(&lock);
  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (c->dirty) {
      continue;
    }
    for (i = 0; i < c->deps_count; i++) {
      if (c->deps[i] == dp) {
        c->dirty = true;
        wake = true;
        break;
      }
    }
  }
  if (wake) {
    pending = true;
    pthread_cond_signal(&cond);
  }
  pthread_mutex_unlock(&lock);
}

void uvlua_unconfigure(void) {
  UVLUA_CALC_T *c;
  int idx;

  if (L != NULL) {
    lua_close(L);
    L = NULL;
  }

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    free(c->deps);
    free((void *) c->name);
    free((void *) c->expr);
  }
  free(calcs);
  free(calc_order);
  free(eval_deps);

  pthread_cond_destroy(&cond);
  pthread_mutex_destroy(&lock);
}

int uvlua_startup(void) {
  UVLUA_CALC_T *c;
  int idx;

  if (calcs_count == 0) {
    return 0;
  }

  start_ticks = utl_get_ticks();

  // first evaluation of all calcs (values may already be there)
  pthread_mutex_lock(&lock);
  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    c->dirty = true;
  }
  pending = true;
  pthread_mutex_unlock(&lock);

  thread_running = true;
  if (pthread_create(&thread, NULL, uvlua_thread, NULL) != 0) {
    thread_running = false;
    syslog(LOG_ERR, "failed to start lua thread");
    return -1;
  }

  return 0;
}

void uvlua_shutdown(void) {
  bool running;

  pthread_mutex_lock(&lock);
  running = thread_running;
  thread_running = false;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);

  if (running) {
    pthread_join(thread, NULL);
  }
}

/**
 * @brief Lua thread: evaluate changed calcs, check for missing values.
 *
 * @param ptr  Unused.
 * @return     NULL.
 */
static void *uvlua_thread(void *ptr) {
  struct timespec deadline;
  bool running;
  bool dirty;
  bool deps_changed;
  UVLUA_CALC_T *c;
  int idx;
  int64_t now;
  int64_t next_check = 0;

  while (true) {
    pthread_mutex_lock(&lock);
    if (thread_running && !pending) {
      clock_gettime(CLOCK_MONOTONIC, &deadline);
      deadline.tv_sec += UVLUA_WAIT_MS / 1000;
      pthread_cond_timedwait(&cond, &lock, &deadline);
    }
    running = thread_running;
    pending = false;
    pthread_mutex_unlock(&lock);

    if (!running) {
      break;
    }

    // dependency order: a calc published here marks its dependents dirty
    // before they are checked
    deps_changed = false;
    for (idx = 0; idx < calcs_count; idx++) {
      c = calc_order[idx];
      pthread_mutex_lock(&lock);
      dirty = c->dirty;
      c->dirty = false;
      pthread_mutex_unlock(&lock);
      if (dirty && !c->disabled) {
        deps_changed |= eval(c);
      }
    }

    if (deps_changed) {
      sort_calcs();
    }

    now = utl_get_ticks();
    if (now >= next_check) {
      next_check = now + UVLUA_CHECK_INTERVAL_MS;
      check_never_received(now);
    }
  }

  return NULL;
}

/**
 * @brief Evaluate a calc and publish the result.
 *
 * Problems are logged once per state change; values never received are
 * reported by check_never_received().
 *
 * @param c  Calc.
 * @return   true if the dependencies changed.
 */
static bool eval(UVLUA_CALC_T *c) {
  bool deps_changed;
  int err;
  int type;
  double f;
  bool valid;

  err = run_calc(c);
  deps_changed = update_deps(c);
  c->missing = NULL;

  if (err != LUA_OK) {
    if (lua_touserdata(L, -1) == &abort_marker) {
      if (!abort_stale) {
        c->missing = abort_dp;
      } else if (c->state != CALC_STATE_STALE) {
        syslog(LOG_WARNING, "calc '%s': value '%s' is stale, not published.", c->name, abort_dp->name);
        c->state = CALC_STATE_STALE;
      }
    } else if (c->state != CALC_STATE_ERROR) {
      syslog(LOG_ERR, "calc '%s': evaluation failed: %s", c->name, lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
      c->state = CALC_STATE_ERROR;
    }
    lua_pop(L, 1);
    return deps_changed;
  }

  type = lua_type(L, -1);
  if (type == LUA_TNUMBER) {
    f = lua_tonumber(L, -1);
    valid = isfinite(f);
  } else if (type == LUA_TBOOLEAN) {
    f = lua_toboolean(L, -1) ? 1.0 : 0.0;
    valid = true;
  } else {
    valid = false;
  }

  if (!valid) {
    if (c->state != CALC_STATE_INVALID) {
      syslog(LOG_WARNING, "calc '%s': result is not a finite number (%s), not published.", c->name, type == LUA_TNUMBER ? "nan/inf" : lua_typename(L, type));
      c->state = CALC_STATE_INVALID;
    }
    lua_pop(L, 1);
    return deps_changed;
  }
  lua_pop(L, 1);

  if (c->state != CALC_STATE_OK) {
    syslog(LOG_INFO, "calc '%s': valid again.", c->name);
    c->state = CALC_STATE_OK;
  }

  uvrgw_conf_disp_val(c->disp, c, f);
  return deps_changed;
}

/**
 * @brief Warn once for calcs waiting for a value never received within
 *        @c max_age (or UVLUA_NEVER_RECEIVED_MS without max_age) after
 *        startup, e.g. a device that is not reachable.
 *
 * @param now  Current time (ms).
 */
static void check_never_received(int64_t now) {
  UVLUA_CALC_T *c;
  int idx;
  int64_t timeout;

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (c->missing == NULL || c->state == CALC_STATE_MISSING) {
      continue;
    }
    timeout = c->max_age > 0 ? c->max_age : UVLUA_NEVER_RECEIVED_MS;
    if (now - start_ticks > timeout) {
      syslog(LOG_WARNING, "calc '%s': value '%s' never received.", c->name, c->missing->name);
      c->state = CALC_STATE_MISSING;
    }
  }
}
