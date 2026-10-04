/**
 * @file eval.h
 * @brief Calculated values and control logic (eval sections).
 *
 * An eval is a group of values defined by expressions over other values
 * (tinyexpr, see tinyexpr/README.uvrgw).  The values of an eval are
 * evaluated in config order, either every @c period ms, when one of the
 * @c triggers is updated, or (neither given) when one of the outside
 * values read by the expressions is updated (default triggers).
 *
 * All names are bound at config load: unknown names are config errors,
 * and the values each expression reads are known.  Validity is decided
 * per value: a value is not evaluated (and not published) if an outside
 * value it reads is missing or older than @c max_age, if a value of the
 * same eval defined above it that it reads was not updated in this
 * evaluation, or if its result is not finite.
 *
 * Non-local values are published via the dispatcher; local values are
 * private to the eval.  State (values read by themselves, @c dt) is kept
 * in memory only and starts at @c init after a restart.
 */

#ifndef _EVAL_H_
#define _EVAL_H_

#include "uvrgw_conf.h"

#include <tinyexpr.h>

#include <stdbool.h>
#include <stdint.h>

struct EVAL;

/**
 * @brief A value of an eval.
 */
typedef struct EVAL_VAL {
  const char *name;          /**< Value name. */
  const char *expr_str;      /**< Expression source. */
  bool local;                /**< Private to the eval (not published). */
  double init;               /**< Value before the first evaluation. */

  struct EVAL *eval;         /**< Back-pointer to the eval. */
  te_expr *expr;             /**< Compiled expression. */
  int slot;                  /**< Index of the value in @c eval->slots. */
  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher (NULL for local values). */

  int ins_count;             /**< Number of outside values read. */
  int *ins;                  /**< Indices into @c eval->ins of the outside values read. */
  int deps_count;            /**< Number of own values above this one read. */
  int *deps;                 /**< Indices into @c eval->values of the own values above read. */

  bool updated;              /**< Updated in the current evaluation. */
  bool has_last;             /**< Updated in the previous evaluation (@c last_ts valid). */
  int64_t last_ts;           /**< Time (ms) of the last update (for @c dt). */
  bool not_finite;           /**< A non-finite result has been logged. */
} EVAL_VAL_T;

/**
 * @brief An outside value read by an eval.
 */
typedef struct EVAL_IN {
  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher of the value. */
  int slot;                  /**< Index of the value in @c eval->slots. */
  bool valid;                /**< Valid in the current evaluation. */
  bool logged;               /**< Invalid state has been logged. */
} EVAL_IN_T;

/**
 * @brief An eval section.
 */
typedef struct EVAL {
  const char *name;          /**< Eval name (for log messages). */
  int period;                /**< Evaluation period in ms; 0 = triggered. */
  int max_age;               /**< Max. age (ms) of outside values; 0 = no limit. */

  int values_count;          /**< Number of values. */
  EVAL_VAL_T *values;        /**< Values in config order. */

  int ins_count;             /**< Number of outside values read. */
  EVAL_IN_T *ins;            /**< Outside values read by any value. */

  int triggers_count;        /**< Number of triggers (explicit or default). */
  UVRGW_CONF_VAL_DISPATCH_T **triggers; /**< Trigger dispatchers. */
  bool explicit_triggers;    /**< Triggers given by @c triggers. */

  int slots_count;           /**< Number of slots. */
  double *slots;             /**< Variables bound into the expressions: dt, own values, outside values. */

  bool pending;              /**< Triggered evaluation pending (protected by the module lock). */
  int64_t next;              /**< Periodic: time (ms) of the next evaluation. */

  int visit;                 /**< Trigger cycle check: 0 = new, 1 = on stack, 2 = done. */
} EVAL_T;

/**
 * @brief Initialise the module (reset eval list).
 */
void eval_init(void);

/**
 * @brief Parse all @c eval{} sections from @p cfg and compile the
 *        expressions.
 *
 * Must be called after all other modules are configured, so all value
 * names are known.
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int eval_configure(cfg_t *cfg);

/**
 * @brief Register the trigger callbacks of all evals.
 */
void eval_register_disp_cbs(void);

/**
 * @brief Free all resources allocated by eval_configure().
 */
void eval_unconfigure(void);

/**
 * @brief Start the eval thread (evaluates all evals once).
 *
 * @return  0 on success, -1 on error.
 */
int eval_startup(void);

/**
 * @brief Stop the eval thread.
 *
 * The thread publishes values, so it must be stopped together with the
 * other value sources, before the outputs are shut down.
 */
void eval_shutdown(void);

#endif
