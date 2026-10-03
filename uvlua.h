/**
 * @file uvlua.h
 * @brief Embedded Lua interpreter: derived values (@c calc).
 *
 * A @c calc section defines a value by a Lua expression over other values
 * and publishes the result under its own name:
 *
 *   calc hp_energy_imp {
 *     expr    = "hp_energy_imp1 + hp_energy_imp2 + hp_energy_imp3"
 *     max_age = 10000   # ms; optional, 0 (default) = no limit
 *   }
 *
 * The operands are the identifiers of the expression that are not Lua
 * keywords, not provided functions and not field names (after '.' or ':').
 * They are collected at config load and resolved at evaluation time to the
 * last dispatched value.
 *
 * Threading: one Lua state and one thread.  Dispatcher callbacks of the
 * operands only mark the calc as changed and wake the thread (coalescing,
 * only the latest values matter); the thread evaluates the changed calcs
 * in dependency order and publishes the results.
 *
 * Evaluation: if an operand was never received or is older than
 * @c max_age, nothing is published (the value becomes stale).  Numbers
 * are published as double, booleans as 1/0; other results, NaN/inf and
 * runtime errors are not published.  Problems are logged once per state
 * change.
 */

#ifndef _UVLUA_H_
#define _UVLUA_H_

#include "uvrgw_conf.h"

#include <stdbool.h>
#include <stdint.h>

struct UVLUA_CALC;

/**
 * @brief An operand of a calc expression.
 *
 * Also used as the dispatcher callback context of the operand.
 */
typedef struct UVLUA_OPERAND {
  const char *name;                    /**< Value name. */
  struct UVLUA_CALC *calc;             /**< Calc using this operand. */
  UVRGW_CONF_VAL_DISPATCH_T *disp;     /**< Dispatcher of the operand value. */
  bool received;                       /**< Value received (or warned as never received); Lua thread only. */
} UVLUA_OPERAND_T;

/**
 * @brief A derived value defined by a Lua expression.
 */
typedef struct UVLUA_CALC {
  const char *name;                    /**< Published value name (Lua identifier). */
  const char *expr;                    /**< Lua expression. */
  int max_age;                         /**< Max. operand age in ms; 0 = no limit. */

  int operands_count;                  /**< Number of operands. */
  UVLUA_OPERAND_T *operands;           /**< Operands (unique names). */

  UVRGW_CONF_VAL_DISPATCH_T *disp;     /**< Dispatcher of the result. */
  int func_ref;                        /**< Registry reference of the compiled expression. */

  int order;                           /**< Config load: dependency sort state. */
  bool dirty;                          /**< An operand changed; protected by the module lock. */
  int state;                           /**< Last evaluation state (logging); Lua thread only. */
} UVLUA_CALC_T;

/**
 * @brief Initialise the module.
 */
void uvlua_init(void);

/**
 * @brief Parse all @c calc{} sections, compile the expressions and create
 *        the dispatchers of operands and results.
 *
 * Must be called after counter_configure() (calc names are checked against
 * counter names).
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int uvlua_configure(cfg_t *cfg);

/**
 * @brief Register the operand callbacks of all calcs.
 */
void uvlua_register_disp_cbs(void);

/**
 * @brief Free all resources allocated by uvlua_configure().
 *
 * Call uvlua_shutdown() first.
 */
void uvlua_unconfigure(void);

/**
 * @brief Start the Lua thread.
 *
 * Call after the output modules are started, so published values reach
 * them.
 *
 * @return  0 on success, -1 on error.
 */
int uvlua_startup(void);

/**
 * @brief Stop the Lua thread.
 *
 * The thread publishes values, so it must be stopped together with the
 * other value sources, before the outputs are shut down.  Operand updates
 * arriving later are ignored.
 */
void uvlua_shutdown(void);

#endif
