// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file eval.c
 * @brief Calculated values and control logic (eval sections).
 *
 * Configuration runs in two passes over all evals: the first one parses
 * the sections and creates the dispatchers of all non-local values (so
 * evals can read values of evals defined later), the second one compiles
 * the expressions and records the dependencies of each value by walking
 * the compiled expression trees.  The outside values a value depends on
 * include those of the other values of the eval it reads (closure), so a
 * value reading the previous state of a value further down is invalid
 * when that value's inputs are invalid.  Computed once at config load, so
 * feedback between values (a reads b, b reads a) cannot block recovery.
 *
 * tinyexpr binds each name to the address of a double.  Each eval has an
 * array of these "slots": @c dt, one per own value and one per dispatcher
 * name usable in expressions (only the ones actually read are refreshed).
 *
 * One thread evaluates all evals.  Trigger callbacks (running in the
 * thread of the source) only set the @c pending flag of the eval, so
 * several triggers arriving together cause one evaluation.  Results are
 * published without holding the module lock.
 */
#include "eval.h"
#include "utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <syslog.h>

#define SLOT_DT 0

static int evals_count;
static EVAL_T *evals;

static pthread_mutex_t lock;
static pthread_cond_t cond;
static pthread_t thread;
static bool thread_running;

static double fn_if(double c, double a, double b);
static double fn_min(double a, double b);
static double fn_max(double a, double b);
static double fn_clamp(double x, double lo, double hi);
static double fn_hyst(double prev, double x, double on, double off);

/**
 * @brief Expression functions added by uvrgw.
 */
static const te_variable functions[] = {
  { "if", fn_if, TE_FUNCTION3 | TE_FLAG_PURE, NULL },
  { "min", fn_min, TE_FUNCTION2 | TE_FLAG_PURE, NULL },
  { "max", fn_max, TE_FUNCTION2 | TE_FLAG_PURE, NULL },
  { "clamp", fn_clamp, TE_FUNCTION3 | TE_FLAG_PURE, NULL },
  { "hyst", fn_hyst, TE_FUNCTION4 | TE_FLAG_PURE, NULL },
};

#define FUNCTIONS_COUNT ((int) (sizeof(functions) / sizeof(functions[0])))

/**
 * @brief Context of the dependency walk of one value.
 */
typedef struct {
  EVAL_T *e;                          /**< Eval. */
  EVAL_VAL_T *v;                      /**< Value whose expression is walked. */
  UVRGW_CONF_VAL_DISPATCH_T **cands;  /**< Dispatcher of each outside value slot. */
  int *cand_ins;                      /**< Index into @c e->ins of each outside value slot; -1 = not read yet. */
} DEP_CTX_T;

static int eval_configure_one(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static int compile_eval(EVAL_T *e);
static void compile_error(EVAL_T *e, EVAL_VAL_T *v, const te_variable *vars, int vars_count, int err);
static int walk(DEP_CTX_T *ctx, const te_expr *n);
static int add_index(int **list, int *count, int idx);
static int close_inputs(EVAL_T *e);
static int setup_triggers(EVAL_T *e, cfg_t *cfg);
static int check_cycles(void);
static int visit(EVAL_T *e, EVAL_T **path, int depth);
static bool triggered_by(EVAL_T *b, EVAL_T *a);
static EVAL_T *eval_of(UVRGW_CONF_VAL_DISPATCH_T *dp);
static bool valid_name(const char *name);
static bool is_function(const char *name);
static EVAL_VAL_T *find_value(EVAL_T *e, const char *name);
static UVRGW_CONF_VAL_DISPATCH_T *find_dispatcher(const char *name);
static int trigger_update(void *v, double f, bool valid);
static void *eval_thread(void *ptr);
static void evaluate(EVAL_T *e);

void eval_init(void) {
  pthread_condattr_t attr;

  evals_count = 0;
  evals = NULL;
  thread_running = false;

  pthread_mutex_init(&lock, NULL);
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&cond, &attr);
  pthread_condattr_destroy(&attr);
}

int eval_configure(cfg_t *cfg) {
  EVAL_T *e;
  EVAL_VAL_T *v;
  int idx, val_idx;

  // pass 1: parse, create dispatchers of non-local values
  if (uvrgw_conf_config_childs(cfg, "eval", &evals_count, (void **) &evals, sizeof(EVAL_T), NULL, eval_configure_one) < 0) {
    return -1;
  }

  // pass 2: all names are known now
  for (e = evals, idx = 0; idx < evals_count; e++, idx++) {
    for (v = e->values, val_idx = 0; val_idx < e->values_count; v++, val_idx++) {
      if (v->local && find_dispatcher(v->name) != NULL) {
        syslog(LOG_ERR, "eval '%s': local value '%s' is also used as value name outside of the eval.", e->name, v->name);
        return -1;
      }
    }

    if (compile_eval(e) < 0) {
      return -1;
    }

    if (setup_triggers(e, cfg_getnsec(cfg, "eval", idx)) < 0) {
      return -1;
    }
  }

  return check_cycles();
}

static int eval_configure_one(cfg_t *cfg, void *ctx, void *child) {
  EVAL_T *e = (EVAL_T *) child;
  bool has_period;

  e->name = uvrgw_conf_strdup(cfg_title(cfg));

  has_period = cfg_size(cfg, "period") > 0;
  if (has_period) {
    e->period = cfg_getint(cfg, "period");
    if (e->period <= 0) {
      syslog(LOG_ERR, "eval '%s': period invalid.", e->name);
      return -1;
    }
    if (cfg_size(cfg, "triggers") > 0) {
      syslog(LOG_ERR, "eval '%s': period and triggers must not be given together.", e->name);
      return -1;
    }
  }

  if (uvrgw_conf_config_childs(cfg, "value", &e->values_count, (void **) &e->values, sizeof(EVAL_VAL_T), e, value_configure) < 0) {
    return -1;
  }

  if (e->values_count == 0) {
    syslog(LOG_ERR, "eval '%s': no value given.", e->name);
    return -1;
  }

  return 0;
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  EVAL_VAL_T *v = (EVAL_VAL_T *) child;
  EVAL_T *e = (EVAL_T *) ctx;

  v->eval = e;
  v->name = uvrgw_conf_strdup(cfg_title(cfg));
  v->expr_str = uvrgw_conf_strdup(cfg_getstr(cfg, "expr"));
  v->local = cfg_getbool(cfg, "local");
  v->init_value = cfg_getfloat(cfg, "init_value");

  if (!valid_name(v->name)) {
    syslog(LOG_ERR, "eval '%s': value name '%s' invalid (allowed: a letter, then letters, digits and _).", e->name, v->name);
    return -1;
  }

  if (strcmp(v->name, "dt") == 0 || is_function(v->name)) {
    syslog(LOG_ERR, "eval '%s': value name '%s' is reserved (dt or expression function).", e->name, v->name);
    return -1;
  }

  if (v->expr_str == NULL) {
    syslog(LOG_ERR, "eval '%s': value '%s': expr not given.", e->name, v->name);
    return -1;
  }

  if (!isfinite(v->init_value)) {
    syslog(LOG_ERR, "eval '%s': value '%s': init_value invalid.", e->name, v->name);
    return -1;
  }

  if (!v->local) {
    v->disp = uvrgw_conf_get_dispatcher(v->name, false);
    if (v->disp == NULL) {
      return -1;
    }
    if (uvrgw_conf_set_producer(v->disp, "eval", e->name, e, false, v->init_value, 0) < 0) {
      return -1;
    }
  }

  return 0;
}

/**
 * @brief Compile the expressions of an eval and record the dependencies.
 *
 * @param e  Eval.
 * @return   0 on success, -1 on error.
 */
static int compile_eval(EVAL_T *e) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;
  UVRGW_CONF_VAL_DISPATCH_T **cands = NULL;
  int *cand_ins = NULL;
  te_variable *vars = NULL;
  int vars_count = 0;
  int cands_count = 0;
  EVAL_VAL_T *v;
  int val_idx, idx, err;
  DEP_CTX_T ctx;
  int ret = -1;

  for (dp = uvrgw_conf_get_dispatchers(); dp != NULL; dp = dp->next) {
    cands_count++;
  }

  // slots: dt, own values, one per dispatcher name
  e->slots_count = 1 + e->values_count + cands_count;
  e->slots = calloc(e->slots_count, sizeof(double));
  e->ins = calloc(cands_count > 0 ? cands_count : 1, sizeof(EVAL_IN_T));
  cands = calloc(cands_count > 0 ? cands_count : 1, sizeof(UVRGW_CONF_VAL_DISPATCH_T *));
  cand_ins = calloc(cands_count > 0 ? cands_count : 1, sizeof(int));
  vars = calloc(e->slots_count + FUNCTIONS_COUNT, sizeof(te_variable));
  if (e->slots == NULL || e->ins == NULL || cands == NULL || cand_ins == NULL || vars == NULL) {
    syslog(LOG_ERR, "eval '%s': failed to allocate variables.", e->name);
    goto out;
  }

  // variable list, in lookup order: dt, own values, outside values, functions
  vars[vars_count].name = "dt";
  vars[vars_count].address = &e->slots[SLOT_DT];
  vars_count++;

  for (v = e->values, val_idx = 0; val_idx < e->values_count; v++, val_idx++) {
    v->slot = 1 + val_idx;
    e->slots[v->slot] = v->init_value;
    vars[vars_count].name = v->name;
    vars[vars_count].address = &e->slots[v->slot];
    vars_count++;
  }

  // outside values named like a function or dt would replace them
  for (dp = uvrgw_conf_get_dispatchers(), idx = 0; dp != NULL; dp = dp->next, idx++) {
    cands[idx] = dp;
    cand_ins[idx] = -1;
    if (!valid_name(dp->name) || strcmp(dp->name, "dt") == 0 || is_function(dp->name) || find_value(e, dp->name) != NULL) {
      continue;
    }
    vars[vars_count].name = dp->name;
    vars[vars_count].address = &e->slots[1 + e->values_count + idx];
    vars_count++;
  }

  for (idx = 0; idx < FUNCTIONS_COUNT; idx++) {
    vars[vars_count++] = functions[idx];
  }

  ctx.e = e;
  ctx.cands = cands;
  ctx.cand_ins = cand_ins;

  for (v = e->values, val_idx = 0; val_idx < e->values_count; v++, val_idx++) {
    v->expr = te_compile(v->expr_str, vars, vars_count, &err);
    if (v->expr == NULL) {
      compile_error(e, v, vars, vars_count, err);
      goto out;
    }

    ctx.v = v;
    if (walk(&ctx, v->expr) < 0) {
      syslog(LOG_ERR, "eval '%s': failed to allocate dependencies.", e->name);
      goto out;
    }
  }

  if (close_inputs(e) < 0) {
    syslog(LOG_ERR, "eval '%s': failed to allocate dependencies.", e->name);
    goto out;
  }

  ret = 0;

out:
  free(vars);
  free(cand_ins);
  free(cands);
  return ret;
}

/**
 * @brief Log a compile error, naming the unknown name if there is one.
 *
 * tinyexpr reports the position after the token that failed, which is
 * the 1-based position of its last character.  Unknown names are reported
 * with the 1-based position of their first character.
 *
 * @param e           Eval.
 * @param v           Value.
 * @param vars        Variable list used for compiling.
 * @param vars_count  Number of variables.
 * @param err         Error position reported by te_compile() (-1: allocation failure).
 */
static void compile_error(EVAL_T *e, EVAL_VAL_T *v, const te_variable *vars, int vars_count, int err) {
  const char *s = v->expr_str;
  char *name;
  int start, idx;

  if (err < 0) {
    syslog(LOG_ERR, "eval '%s': value '%s': failed to compile expression.", e->name, v->name);
    return;
  }

  // identifier ending at the error position?
  start = err;
  while (start > 0 && ((s[start - 1] >= 'A' && s[start - 1] <= 'Z') || (s[start - 1] >= 'a' && s[start - 1] <= 'z') ||
      (s[start - 1] >= '0' && s[start - 1] <= '9') || s[start - 1] == '_')) {
    start--;
  }
  // skip digits (part of a number, not of a name)
  while (start < err && s[start] >= '0' && s[start] <= '9') {
    start++;
  }

  if (start < err && !(start > 0 && s[start - 1] == '.')) {
    name = strndup(s + start, err - start);
    if (name != NULL && !is_function(name)) {
      for (idx = 0; idx < vars_count; idx++) {
        if (strcmp(vars[idx].name, name) == 0) {
          break;
        }
      }
      if (idx == vars_count) {
        syslog(LOG_ERR, "eval '%s': value '%s': unknown name '%s' at position %d of expression \"%s\".", e->name, v->name, name, start + 1, s);
        free(name);
        return;
      }
    }
    free(name);
  }

  syslog(LOG_ERR, "eval '%s': value '%s': syntax error at position %d of expression \"%s\".", e->name, v->name, err, s);
}

/**
 * @brief Walk a compiled expression and record the values it reads.
 *
 * Records the outside values and the other own values read.  Own values
 * defined above the value carry the result of the current evaluation,
 * those read from the same or a later position the previous state.
 *
 * @param ctx  Walk context.
 * @param n    Expression node.
 * @return     0 on success, -1 on allocation failure.
 */
static int walk(DEP_CTX_T *ctx, const te_expr *n) {
  EVAL_T *e = ctx->e;
  int slot, own, cand, idx;

  if ((n->type & 0x1f) == TE_VARIABLE) {
    slot = n->bound - e->slots;

    if (slot == SLOT_DT) {
      return 0;
    }

    // own value (the value itself has no outside values of its own)
    if (slot <= e->values_count) {
      own = slot - 1;
      if (own == ctx->v - e->values) {
        return 0;
      }
      return add_index(&ctx->v->refs, &ctx->v->refs_count, own);
    }

    // outside value
    cand = slot - 1 - e->values_count;
    if (ctx->cand_ins[cand] < 0) {
      ctx->cand_ins[cand] = e->ins_count;
      e->ins[e->ins_count].disp = ctx->cands[cand];
      e->ins[e->ins_count].slot = slot;
      e->ins_count++;
    }
    return add_index(&ctx->v->ins, &ctx->v->ins_count, ctx->cand_ins[cand]);
  }

  // function or closure: walk the parameters
  if (n->type & (TE_FUNCTION0 | TE_CLOSURE0)) {
    for (idx = 0; idx < (n->type & 7); idx++) {
      if (walk(ctx, n->parameters[idx]) < 0) {
        return -1;
      }
    }
  }

  return 0;
}

/**
 * @brief Append an index to a list, unless it is already contained.
 *
 * @param list   List (reallocated).
 * @param count  Number of entries.
 * @param idx    Index to add.
 * @return       0 on success, -1 on allocation failure.
 */
static int add_index(int **list, int *count, int idx) {
  int *p;
  int i;

  for (i = 0; i < *count; i++) {
    if ((*list)[i] == idx) {
      return 0;
    }
  }

  p = realloc(*list, (*count + 1) * sizeof(int));
  if (p == NULL) {
    return -1;
  }
  p[*count] = idx;
  *list = p;
  (*count)++;

  return 0;
}

/**
 * @brief Add the outside values of the own values read to each value
 *        (transitive closure over @c refs).
 *
 * @param e  Eval.
 * @return   0 on success, -1 on allocation failure.
 */
static int close_inputs(EVAL_T *e) {
  EVAL_VAL_T *v, *r;
  int val_idx, i, j, count;
  bool changed;

  do {
    changed = false;
    for (v = e->values, val_idx = 0; val_idx < e->values_count; v++, val_idx++) {
      for (i = 0; i < v->refs_count; i++) {
        r = &e->values[v->refs[i]];
        for (j = 0; j < r->ins_count; j++) {
          count = v->ins_count;
          if (add_index(&v->ins, &v->ins_count, r->ins[j]) < 0) {
            return -1;
          }
          changed |= (v->ins_count != count);
        }
      }
    }
  } while (changed);

  return 0;
}

/**
 * @brief Resolve the triggers of an eval (explicit or default) and reserve
 *        their callback slots.
 *
 * @param e    Eval.
 * @param cfg  Section of the eval.
 * @return     0 on success, -1 on error.
 */
static int setup_triggers(EVAL_T *e, cfg_t *cfg) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;
  const char *name;
  int count, idx, i;

  if (e->period > 0) {
    return 0;
  }

  count = cfg_size(cfg, "triggers");
  e->explicit_triggers = (count > 0);
  if (!e->explicit_triggers) {
    // default triggers: all outside values read
    count = e->ins_count;
    if (count == 0) {
      syslog(LOG_ERR, "eval '%s': no outside value read, period or triggers required.", e->name);
      return -1;
    }
  }

  e->triggers = calloc(count, sizeof(UVRGW_CONF_VAL_DISPATCH_T *));
  if (e->triggers == NULL) {
    syslog(LOG_ERR, "eval '%s': failed to allocate triggers.", e->name);
    return -1;
  }

  for (idx = 0; idx < count; idx++) {
    if (e->explicit_triggers) {
      name = cfg_getnstr(cfg, "triggers", idx);
      if (find_value(e, name) != NULL) {
        syslog(LOG_ERR, "eval '%s': trigger '%s' is a value of the same eval.", e->name, name);
        return -1;
      }
      dp = find_dispatcher(name);
      if (dp == NULL) {
        syslog(LOG_ERR, "eval '%s': unknown trigger '%s'.", e->name, name);
        return -1;
      }
      for (i = 0; i < e->triggers_count; i++) {
        if (e->triggers[i] == dp) {
          syslog(LOG_ERR, "eval '%s': trigger '%s' given twice.", e->name, name);
          return -1;
        }
      }
    } else {
      dp = e->ins[idx].disp;
    }

    // reserve callback slot
    e->triggers[e->triggers_count++] = uvrgw_conf_get_dispatcher(dp->name, true);
  }

  return 0;
}

/**
 * @brief Check for trigger cycles between evals.
 *
 * Eval A triggers eval B if a trigger of B is a value published by A.  A
 * cycle would evaluate forever.
 *
 * @return  0 if there is no cycle, -1 otherwise.
 */
static int check_cycles(void) {
  EVAL_T **path;
  EVAL_T *e;
  int idx;
  int ret = 0;

  if (evals_count == 0) {
    return 0;
  }

  path = calloc(evals_count, sizeof(EVAL_T *));
  if (path == NULL) {
    syslog(LOG_ERR, "Failed to allocate eval cycle check.");
    return -1;
  }

  for (e = evals, idx = 0; idx < evals_count && ret == 0; e++, idx++) {
    if (e->visit == 0) {
      ret = visit(e, path, 0);
    }
  }

  free(path);
  return ret;
}

/**
 * @brief Depth first search for trigger cycles.
 *
 * @param e      Current eval.
 * @param path   Evals on the current path.
 * @param depth  Position of @p e in @p path.
 * @return       0 if no cycle was found, -1 otherwise (logged).
 */
static int visit(EVAL_T *e, EVAL_T **path, int depth) {
  EVAL_T *b;
  int idx, i;
  size_t len;
  char *msg;

  path[depth] = e;
  e->visit = 1;

  for (b = evals, idx = 0; idx < evals_count; b++, idx++) {
    if (b->visit == 2 || !triggered_by(b, e)) {
      continue;
    }

    if (b->visit == 0) {
      if (visit(b, path, depth + 1) < 0) {
        return -1;
      }
      continue;
    }

    // b is on the path: cycle from b to e and back to b
    for (i = 0; path[i] != b; i++);
    len = strlen(b->name) + 1;
    for (; i <= depth; i++) {
      len += strlen(path[i]->name) + 4;
    }
    msg = malloc(len);
    if (msg == NULL) {
      syslog(LOG_ERR, "eval trigger cycle.");
      return -1;
    }
    msg[0] = 0;
    for (i = 0; path[i] != b; i++);
    for (; i <= depth; i++) {
      strcat(msg, path[i]->name);
      strcat(msg, " -> ");
    }
    strcat(msg, b->name);
    syslog(LOG_ERR, "eval trigger cycle: %s (each eval publishes a trigger of the next one).", msg);
    free(msg);
    return -1;
  }

  e->visit = 2;
  return 0;
}

/**
 * @brief Check whether eval @p b is triggered by a value of eval @p a.
 */
static bool triggered_by(EVAL_T *b, EVAL_T *a) {
  int idx;

  for (idx = 0; idx < b->triggers_count; idx++) {
    if (eval_of(b->triggers[idx]) == a) {
      return true;
    }
  }

  return false;
}

/**
 * @brief Get the eval publishing a value.
 *
 * @param dp  Dispatcher of the value.
 * @return    Eval, or NULL if the value is not published by an eval.
 */
static EVAL_T *eval_of(UVRGW_CONF_VAL_DISPATCH_T *dp) {
  EVAL_T *e;
  EVAL_VAL_T *v;
  int idx, val_idx;

  for (e = evals, idx = 0; idx < evals_count; e++, idx++) {
    for (v = e->values, val_idx = 0; val_idx < e->values_count; v++, val_idx++) {
      if (v->disp == dp) {
        return e;
      }
    }
  }

  return NULL;
}

/**
 * @brief Check whether a name is usable in expressions: a letter, then
 *        letters, digits and _ (as tinyexpr accepts them).
 */
static bool valid_name(const char *name) {
  const char *p;

  if (name == NULL || !((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z'))) {
    return false;
  }

  for (p = name + 1; *p != 0; p++) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
      return false;
    }
  }

  return true;
}

/**
 * @brief Check whether a name is an expression function (tinyexpr
 *        built-in or uvrgw).
 *
 * A variable with such a name would replace the function in every
 * expression (tinyexpr looks up variables first).
 *
 * @param name  Name.
 */
static bool is_function(const char *name) {
  int idx;

  // te_is_builtin() is added to tinyexpr by uvrgw (see tinyexpr/README.uvrgw)
  if (te_is_builtin(name)) {
    return true;
  }

  for (idx = 0; idx < FUNCTIONS_COUNT; idx++) {
    if (strcmp(functions[idx].name, name) == 0) {
      return true;
    }
  }

  return false;
}

static EVAL_VAL_T *find_value(EVAL_T *e, const char *name) {
  EVAL_VAL_T *v;
  int val_idx;

  for (v = e->values, val_idx = 0; val_idx < e->values_count; v++, val_idx++) {
    if (strcmp(v->name, name) == 0) {
      return v;
    }
  }

  return NULL;
}

static UVRGW_CONF_VAL_DISPATCH_T *find_dispatcher(const char *name) {
  UVRGW_CONF_VAL_DISPATCH_T *dp;

  for (dp = uvrgw_conf_get_dispatchers(); dp != NULL; dp = dp->next) {
    if (strcmp(dp->name, name) == 0) {
      return dp;
    }
  }

  return NULL;
}

int eval_register_disp_cbs(void) {
  EVAL_T *e;
  int idx, trig_idx;

  for (e = evals, idx = 0; idx < evals_count; e++, idx++) {
    for (trig_idx = 0; trig_idx < e->triggers_count; trig_idx++) {
      if (uvrgw_conf_register_disp_cb(e->triggers[trig_idx], e, trigger_update) < 0) {
        return -1;
      }
    }
  }

  return 0;
}

void eval_unconfigure(void) {
  EVAL_T *e;
  EVAL_VAL_T *v;
  int idx, val_idx;

  for (e = evals, idx = 0; idx < evals_count; e++, idx++) {
    for (v = e->values, val_idx = 0; val_idx < e->values_count; v++, val_idx++) {
      free((void *) v->name);
      free((void *) v->expr_str);
      te_free(v->expr);
      free(v->ins);
      free(v->refs);
    }
    free(e->values);
    free(e->ins);
    free(e->triggers);
    free(e->slots);
    free((void *) e->name);
  }
  free(evals);
  evals = NULL;
  evals_count = 0;

  pthread_cond_destroy(&cond);
  pthread_mutex_destroy(&lock);
}

int eval_startup(void) {
  EVAL_T *e;
  int idx;
  int64_t now;

  if (evals_count == 0) {
    return 0;
  }

  // evaluate all evals once at startup
  now = utl_get_ticks();
  pthread_mutex_lock(&lock);
  for (e = evals, idx = 0; idx < evals_count; e++, idx++) {
    e->pending = true;
    e->next = now;
  }
  pthread_mutex_unlock(&lock);

  thread_running = true;
  if (pthread_create(&thread, NULL, eval_thread, NULL) != 0) {
    thread_running = false;
    syslog(LOG_ERR, "failed to start eval thread");
    return -1;
  }

  return 0;
}

void eval_shutdown(void) {
  if (thread_running) {
    pthread_mutex_lock(&lock);
    thread_running = false;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
  }
}

/**
 * @brief Dispatcher callback: a trigger value was updated.
 *
 * Runs in the thread of the source; only marks the eval as pending.
 *
 * @param v      @c EVAL_T pointer.
 * @param f      Trigger value (unused, values are read on evaluation).
 * @param valid  Trigger validity (unused, read on evaluation).
 * @return       0.
 */
static int trigger_update(void *v, double f, bool valid) {
  EVAL_T *e = (EVAL_T *) v;

  pthread_mutex_lock(&lock);
  e->pending = true;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);

  return 0;
}

/**
 * @brief Eval thread: evaluate pending and due evals in config order.
 *
 * @param ptr  Unused.
 * @return     NULL.
 */
static void *eval_thread(void *ptr) {
  EVAL_T *e;
  int idx;
  int64_t now, next_due;
  bool due, evaluated;
  struct timespec ts;

  pthread_mutex_lock(&lock);

  while (thread_running) {
    now = utl_get_ticks();
    next_due = INT64_MAX;
    evaluated = false;

    for (e = evals, idx = 0; idx < evals_count; e++, idx++) {
      due = false;
      if (e->period > 0) {
        if (now >= e->next) {
          // fixed schedule, missed evaluations are skipped
          e->next += ((now - e->next) / e->period + 1) * e->period;
          due = true;
        }
        if (e->next < next_due) {
          next_due = e->next;
        }
      } else if (e->pending) {
        e->pending = false;
        due = true;
      }

      if (due) {
        pthread_mutex_unlock(&lock);
        evaluate(e);
        pthread_mutex_lock(&lock);
        evaluated = true;
        now = utl_get_ticks();
      }
    }

    // evals further up may have been triggered meanwhile
    if (evaluated || !thread_running) {
      continue;
    }

    // all pending flags were checked under the lock: nothing is lost
    if (next_due == INT64_MAX) {
      pthread_cond_wait(&cond, &lock);
    } else {
      ts.tv_sec = next_due / 1000;
      ts.tv_nsec = (next_due % 1000) * 1000000;
      pthread_cond_timedwait(&cond, &lock, &ts);
    }
  }

  pthread_mutex_unlock(&lock);

  return NULL;
}

/**
 * @brief Evaluate an eval and publish all values.
 *
 * @param e  Eval.
 */
static void evaluate(EVAL_T *e) {
  EVAL_IN_T *in;
  EVAL_VAL_T *v;
  int idx;
  int64_t now = utl_get_ticks();
  double r;
  bool finite;
  int i;

  // refresh outside values
  for (in = e->ins, idx = 0; idx < e->ins_count; in++, idx++) {
    in->valid = uvrgw_conf_get_val(in->disp, &e->slots[in->slot]);
  }

  for (v = e->values, idx = 0; idx < e->values_count; v++, idx++) {
    e->slots[SLOT_DT] = v->has_last ? (double) (now - v->last_ts) / 1000.0 : 0.0;
    r = te_eval(v->expr);

    finite = isfinite(r);
    if (!finite) {
      // the value keeps its previous number in the slot
      if (!v->not_finite) {
        syslog(LOG_WARNING, "eval '%s': value '%s' invalid: result is not finite.", e->name, v->name);
        v->not_finite = true;
      }
    } else {
      e->slots[v->slot] = r;
      if (v->not_finite) {
        syslog(LOG_INFO, "eval '%s': value '%s' finite again.", e->name, v->name);
        v->not_finite = false;
      }
    }

    // valid if all outside values read (directly or via own values) are
    v->valid = finite;
    for (i = 0; i < v->ins_count && v->valid; i++) {
      v->valid = e->ins[v->ins[i]].valid;
    }

    // dt restarts at 0 after a non-finite result
    v->has_last = finite;
    if (finite) {
      v->last_ts = now;
    }
  }

  // publish all values, also unchanged ones
  for (v = e->values, idx = 0; idx < e->values_count; v++, idx++) {
    if (v->disp != NULL) {
      uvrgw_conf_disp_val(v->disp, e, e->slots[v->slot], v->valid);
    }
  }
}

/**
 * @brief if(c, a, b): @p a if @p c is true (not 0), else @p b.
 *
 * Both arguments are evaluated by tinyexpr; only the result is checked.
 * A NaN condition gives NaN.
 */
static double fn_if(double c, double a, double b) {
  if (isnan(c)) {
    return NAN;
  }

  return (c != 0.0) ? a : b;
}

/**
 * @brief min(a, b): smaller value (NaN if one is NaN).
 */
static double fn_min(double a, double b) {
  if (isnan(a) || isnan(b)) {
    return NAN;
  }

  return (a < b) ? a : b;
}

/**
 * @brief max(a, b): larger value (NaN if one is NaN).
 */
static double fn_max(double a, double b) {
  if (isnan(a) || isnan(b)) {
    return NAN;
  }

  return (a > b) ? a : b;
}

/**
 * @brief clamp(x, lo, hi): @p x limited to [lo, hi].
 */
static double fn_clamp(double x, double lo, double hi) {
  if (isnan(x) || isnan(lo) || isnan(hi)) {
    return NAN;
  }

  return utl_val_limit(x, lo, hi);
}

/**
 * @brief hyst(prev, x, on, off): hysteresis.
 *
 * 1 if @p x >= @p on, 0 if @p x <= @p off, else @p prev as 1/0.  With
 * @p on < @p off inverted: 1 if @p x <= @p on, 0 if @p x >= @p off.
 */
static double fn_hyst(double prev, double x, double on, double off) {
  if (isnan(prev) || isnan(x) || isnan(on) || isnan(off)) {
    return NAN;
  }

  if (on >= off) {
    if (x >= on) {
      return 1.0;
    }
    if (x <= off) {
      return 0.0;
    }
  } else {
    if (x <= on) {
      return 1.0;
    }
    if (x >= off) {
      return 0.0;
    }
  }

  return (prev != 0.0) ? 1.0 : 0.0;
}
