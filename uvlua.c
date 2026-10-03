/**
 * @file uvlua.c
 * @brief Embedded Lua interpreter: derived values (@c calc).
 *
 * One Lua state, used at config load (compile) and afterwards only by the
 * Lua thread.  Operand callbacks run in the threads of the sources: they
 * only set the dirty flag of their calc and wake the Lua thread.
 *
 * Calcs are evaluated in dependency order (operands that are calcs first),
 * so a calc depending on another calc is evaluated once per change.  The
 * results are dispatched from the Lua thread without holding the module
 * lock, so calcs and counters can use each other as sources.
 *
 * Sandbox: only the math library is loaded.  Expressions run in a shared
 * environment table with the provided functions; its metatable resolves
 * operand names to their current values and forbids global assignments.
 * Each evaluation is limited to UVLUA_MAX_INSTRUCTIONS, the whole state to
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
#define CALC_STATE_STALE   1
#define CALC_STATE_ERROR   2
#define CALC_STATE_INVALID 3

#define ORDER_NONE   0
#define ORDER_ACTIVE 1
#define ORDER_DONE   2

static const char * const keywords[] = {
  "and", "break", "do", "else", "elseif", "end", "false", "for",
  "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
  "return", "then", "true", "until", "while", "_ENV", NULL
};

static const char * const provided[] = {
  "math", "min", "max", "abs", "floor", "ceil", "clamp", NULL
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
static UVLUA_OPERAND_T *abort_operand;
static bool abort_stale;
static char abort_marker;

static int calc_configure_one(cfg_t *cfg, void *ctx, void *child);
static bool is_name_start(char c);
static bool is_name_char(char c);
static bool in_list(const char * const *list, const char *s, size_t len);
static bool valid_name(const char *name);
static int lex_operands(UVLUA_CALC_T *c);
static int long_bracket_level(const char *p);
static const char *skip_long_bracket(const char *p, int level);
static int add_operand(UVLUA_CALC_T *c, const char *s, size_t len);
static UVLUA_CALC_T *find_calc(const char *name);
static int sort_calc(UVLUA_CALC_T *c, int *pos);
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
static int l_panic(lua_State *L);
static int create_state(void);
static int compile_calc(UVLUA_CALC_T *c);
static int l_index(lua_State *L);
static int l_newindex(lua_State *L);
static int l_clamp(lua_State *L);
static void instruction_hook(lua_State *L, lua_Debug *ar);
static int operand_update(void *v, double f);
static void *uvlua_thread(void *ptr);
static void eval(UVLUA_CALC_T *c);
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

  pthread_mutex_init(&lock, NULL);
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&cond, &attr);
  pthread_condattr_destroy(&attr);
}

int uvlua_configure(cfg_t *cfg) {
  UVLUA_CALC_T *c;
  int idx;
  int pos;

  if (uvrgw_conf_config_childs(cfg, "calc", &calcs_count, (void **) &calcs, sizeof(UVLUA_CALC_T), NULL, calc_configure_one) < 0) {
    return -1;
  }

  if (calcs_count == 0) {
    return 0;
  }

  // calc and counter would both publish the value
  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (counter_exists(c->name)) {
      syslog(LOG_ERR, "calc '%s': a counter with the same name exists.", c->name);
      return -1;
    }
  }

  // dependency order, detects cycles
  calc_order = calloc(calcs_count, sizeof(UVLUA_CALC_T *));
  if (calc_order == NULL) {
    syslog(LOG_ERR, "Failed to allocate calc order.");
    return -1;
  }
  pos = 0;
  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (sort_calc(c, &pos) < 0) {
      return -1;
    }
  }

  if (create_state() < 0) {
    return -1;
  }

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (compile_calc(c) < 0) {
      return -1;
    }
  }

  return 0;
}

static int calc_configure_one(cfg_t *cfg, void *ctx, void *child) {
  UVLUA_CALC_T *c = (UVLUA_CALC_T *) child;
  UVLUA_OPERAND_T *op;
  int op_idx;

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

  if (lex_operands(c) < 0) {
    return -1;
  }

  if (c->operands_count == 0) {
    syslog(LOG_ERR, "calc '%s': expression has no operands.", c->name);
    return -1;
  }

  c->disp = uvrgw_conf_get_dispatcher(c->name, false);
  if (c->disp == NULL) {
    return -1;
  }

  for (op = c->operands, op_idx = 0; op_idx < c->operands_count; op++, op_idx++) {
    op->calc = c;
    op->disp = uvrgw_conf_get_dispatcher(op->name, true);
    if (op->disp == NULL) {
      return -1;
    }
  }

  return 0;
}

static bool is_name_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c) {
  return is_name_start(c) || (c >= '0' && c <= '9');
}

static bool in_list(const char * const *list, const char *s, size_t len) {
  for (; *list != NULL; list++) {
    if (strlen(*list) == len && strncmp(*list, s, len) == 0) {
      return true;
    }
  }

  return false;
}

static bool valid_name(const char *name) {
  const char *p;

  if (name == NULL || !is_name_start(*name)) {
    return false;
  }

  for (p = name; *p != 0; p++) {
    if (!is_name_char(*p)) {
      return false;
    }
  }

  return !in_list(keywords, name, strlen(name)) && !in_list(provided, name, strlen(name));
}

/**
 * @brief Collect the operands of an expression.
 *
 * Small Lua lexer: skips comments, strings and numbers; identifiers that
 * are not keywords, not provided functions and not field names (after a
 * single '.' or ':') are operands.
 *
 * @param c  Calc.
 * @return   0 on success, -1 on allocation error.
 */
static int lex_operands(UVLUA_CALC_T *c) {
  const char *p = c->expr;
  const char *start;
  const char *exp_chars;
  bool field = false;
  int level;
  char quote;

  while (*p != 0) {
    // whitespace and comments are no tokens (field state is kept)
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\f' || *p == '\v') {
      p++;
      continue;
    }

    if (p[0] == '-' && p[1] == '-') {
      p += 2;
      level = long_bracket_level(p);
      if (level >= 0) {
        p = skip_long_bracket(p, level);
      } else {
        while (*p != 0 && *p != '\n') {
          p++;
        }
      }
      continue;
    }

    // long string
    if (*p == '[' && (level = long_bracket_level(p)) >= 0) {
      p = skip_long_bracket(p, level);
      field = false;
      continue;
    }

    // short string
    if (*p == '"' || *p == '\'') {
      quote = *p++;
      while (*p != 0 && *p != quote) {
        if (*p == '\\' && p[1] != 0) {
          p++;
        }
        p++;
      }
      if (*p != 0) {
        p++;
      }
      field = false;
      continue;
    }

    // number (incl. hex, fraction and signed exponent)
    if ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9')) {
      exp_chars = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? "pP" : "eE";
      while (is_name_char(*p) || *p == '.' || ((*p == '+' || *p == '-') && strchr(exp_chars, p[-1]) != NULL)) {
        p++;
      }
      field = false;
      continue;
    }

    if (is_name_start(*p)) {
      start = p;
      while (is_name_char(*p)) {
        p++;
      }
      if (!field && !in_list(keywords, start, p - start) && !in_list(provided, start, p - start)) {
        if (add_operand(c, start, p - start) < 0) {
          return -1;
        }
      }
      field = false;
      continue;
    }

    // '.' (field) vs. '..' (concatenation) / '...'
    if (*p == '.') {
      if (p[1] == '.') {
        while (*p == '.') {
          p++;
        }
        field = false;
      } else {
        p++;
        field = true;
      }
      continue;
    }

    // ':' (method) vs. '::' (label)
    if (*p == ':') {
      if (p[1] == ':') {
        p += 2;
        field = false;
      } else {
        p++;
        field = true;
      }
      continue;
    }

    p++;
    field = false;
  }

  return 0;
}

/**
 * @brief Get the level of a long bracket ("[[", "[=[", ...).
 *
 * @param p  Position of the opening '['.
 * @return   Number of '=', or -1 if not a long bracket.
 */
static int long_bracket_level(const char *p) {
  int level = 0;

  if (*p != '[') {
    return -1;
  }

  for (p++; *p == '='; p++) {
    level++;
  }

  return *p == '[' ? level : -1;
}

/**
 * @brief Skip a long bracket string or comment.
 *
 * @param p      Position of the opening '['.
 * @param level  Level from long_bracket_level().
 * @return       Position after the closing bracket (or end of string).
 */
static const char *skip_long_bracket(const char *p, int level) {
  const char *q;
  int n;

  for (p += level + 2; *p != 0; p++) {
    if (*p != ']') {
      continue;
    }
    for (q = p + 1, n = 0; *q == '='; q++) {
      n++;
    }
    if (n == level && *q == ']') {
      return q + 1;
    }
  }

  return p;
}

static int add_operand(UVLUA_CALC_T *c, const char *s, size_t len) {
  UVLUA_OPERAND_T *ops;
  int idx;

  for (idx = 0; idx < c->operands_count; idx++) {
    if (strlen(c->operands[idx].name) == len && strncmp(c->operands[idx].name, s, len) == 0) {
      return 0;
    }
  }

  ops = realloc(c->operands, (c->operands_count + 1) * sizeof(UVLUA_OPERAND_T));
  if (ops == NULL) {
    syslog(LOG_ERR, "Failed to allocate calc operands.");
    return -1;
  }
  c->operands = ops;

  memset(&ops[c->operands_count], 0, sizeof(UVLUA_OPERAND_T));
  ops[c->operands_count].name = strndup(s, len);
  if (ops[c->operands_count].name == NULL) {
    syslog(LOG_ERR, "Failed to allocate calc operand name.");
    return -1;
  }
  c->operands_count++;

  return 0;
}

static UVLUA_CALC_T *find_calc(const char *name) {
  UVLUA_CALC_T *c;
  int idx;

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    if (strcmp(c->name, name) == 0) {
      return c;
    }
  }

  return NULL;
}

/**
 * @brief Append a calc to the evaluation order after its calc operands
 *        (depth first search).
 *
 * @param c    Calc.
 * @param pos  Next position in @c calc_order.
 * @return     0 on success, -1 on cyclic dependency.
 */
static int sort_calc(UVLUA_CALC_T *c, int *pos) {
  UVLUA_OPERAND_T *op;
  int op_idx;
  UVLUA_CALC_T *dep;

  if (c->order == ORDER_DONE) {
    return 0;
  }

  c->order = ORDER_ACTIVE;

  for (op = c->operands, op_idx = 0; op_idx < c->operands_count; op++, op_idx++) {
    dep = find_calc(op->name);
    if (dep == NULL) {
      continue;
    }
    if (dep->order == ORDER_ACTIVE) {
      syslog(LOG_ERR, "calc '%s': cyclic dependency via operand '%s'.", c->name, op->name);
      return -1;
    }
    if (sort_calc(dep, pos) < 0) {
      return -1;
    }
  }

  c->order = ORDER_DONE;
  calc_order[(*pos)++] = c;

  return 0;
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
  static const char * const math_funcs[] = { "min", "max", "abs", "floor", "ceil", NULL };
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

  // metatable: operand resolution, no global assignments
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
 * @brief Environment __index: resolve an operand to its current value.
 *
 * Aborts the evaluation (error object @c abort_marker) if the operand was
 * never received or is older than @c max_age.
 */
static int l_index(lua_State *L) {
  const char *name = lua_tostring(L, 2);
  UVLUA_OPERAND_T *op;
  int op_idx;
  double f;
  int64_t ts;

  if (name == NULL || eval_calc == NULL) {
    return luaL_error(L, "invalid name");
  }

  for (op = eval_calc->operands, op_idx = 0; op_idx < eval_calc->operands_count; op++, op_idx++) {
    if (strcmp(op->name, name) == 0) {
      break;
    }
  }
  if (op_idx >= eval_calc->operands_count) {
    return luaL_error(L, "unknown name '%s'", name);
  }

  if (!uvrgw_conf_get_val(op->disp, &f, &ts)) {
    abort_operand = op;
    abort_stale = false;
    lua_pushlightuserdata(L, &abort_marker);
    return lua_error(L);
  }
  op->received = true;

  if (eval_calc->max_age > 0 && utl_get_ticks() - ts > eval_calc->max_age) {
    abort_operand = op;
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

void uvlua_register_disp_cbs(void) {
  UVLUA_CALC_T *c;
  int idx;
  UVLUA_OPERAND_T *op;
  int op_idx;

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    for (op = c->operands, op_idx = 0; op_idx < c->operands_count; op++, op_idx++) {
      uvrgw_conf_register_disp_cb(op->disp, op, operand_update);
    }
  }
}

void uvlua_unconfigure(void) {
  UVLUA_CALC_T *c;
  int idx;
  int op_idx;

  if (L != NULL) {
    lua_close(L);
    L = NULL;
  }

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    for (op_idx = 0; op_idx < c->operands_count; op_idx++) {
      free((void *) c->operands[op_idx].name);
    }
    free(c->operands);
    free((void *) c->name);
    free((void *) c->expr);
  }
  free(calcs);
  free(calc_order);

  pthread_cond_destroy(&cond);
  pthread_mutex_destroy(&lock);
}

int uvlua_startup(void) {
  if (calcs_count == 0) {
    return 0;
  }

  start_ticks = utl_get_ticks();

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
 * @brief Dispatcher callback of an operand: mark its calc as changed.
 *
 * Runs in the thread of the source, so it only sets flags.
 */
static int operand_update(void *v, double f) {
  UVLUA_OPERAND_T *op = (UVLUA_OPERAND_T *) v;

  pthread_mutex_lock(&lock);
  op->calc->dirty = true;
  pending = true;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);

  return 0;
}

/**
 * @brief Lua thread: evaluate changed calcs, check for missing operands.
 *
 * @param ptr  Unused.
 * @return     NULL.
 */
static void *uvlua_thread(void *ptr) {
  struct timespec deadline;
  bool running;
  bool dirty;
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
    for (idx = 0; idx < calcs_count; idx++) {
      c = calc_order[idx];
      pthread_mutex_lock(&lock);
      dirty = c->dirty;
      c->dirty = false;
      pthread_mutex_unlock(&lock);
      if (dirty) {
        eval(c);
      }
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
 * Problems are logged once per state change.
 *
 * @param c  Calc.
 */
static void eval(UVLUA_CALC_T *c) {
  int err;
  int type;
  double f;
  bool valid;

  eval_calc = c;
  lua_rawgeti(L, LUA_REGISTRYINDEX, c->func_ref);
  lua_sethook(L, instruction_hook, LUA_MASKCOUNT, UVLUA_MAX_INSTRUCTIONS);
  err = lua_pcall(L, 0, 1, 0);
  lua_sethook(L, NULL, 0, 0);
  eval_calc = NULL;

  if (err != LUA_OK) {
    if (lua_touserdata(L, -1) == &abort_marker) {
      // never received operands are reported by check_never_received()
      if (abort_stale && c->state != CALC_STATE_STALE) {
        syslog(LOG_WARNING, "calc '%s': operand '%s' is stale, not published.", c->name, abort_operand->name);
        c->state = CALC_STATE_STALE;
      }
    } else if (c->state != CALC_STATE_ERROR) {
      syslog(LOG_ERR, "calc '%s': evaluation failed: %s", c->name, lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
      c->state = CALC_STATE_ERROR;
    }
    lua_pop(L, 1);
    return;
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
    return;
  }
  lua_pop(L, 1);

  if (c->state != CALC_STATE_OK) {
    syslog(LOG_INFO, "calc '%s': valid again.", c->name);
    c->state = CALC_STATE_OK;
  }

  uvrgw_conf_disp_val(c->disp, c, f);
}

/**
 * @brief Warn once for operands never received within @c max_age (or
 *        UVLUA_NEVER_RECEIVED_MS without max_age) after startup, e.g.
 *        misspelled value names.
 *
 * @param now  Current time (ms).
 */
static void check_never_received(int64_t now) {
  UVLUA_CALC_T *c;
  int idx;
  UVLUA_OPERAND_T *op;
  int op_idx;
  int64_t timeout;
  double f;

  for (c = calcs, idx = 0; idx < calcs_count; c++, idx++) {
    timeout = c->max_age > 0 ? c->max_age : UVLUA_NEVER_RECEIVED_MS;
    for (op = c->operands, op_idx = 0; op_idx < c->operands_count; op++, op_idx++) {
      if (op->received) {
        continue;
      }
      if (uvrgw_conf_get_val(op->disp, &f, NULL)) {
        op->received = true;
      } else if (now - start_ticks > timeout) {
        syslog(LOG_WARNING, "calc '%s': operand '%s' never received.", c->name, op->name);
        op->received = true;
      }
    }
  }
}
