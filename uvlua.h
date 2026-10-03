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
 * Global names of the expression that are not provided functions are
 * resolved to the last dispatched value of that name.
 *
 * Dependencies are not parsed from the expression: every evaluation
 * records the values it reads, and an update of one of them marks the
 * calc for re-evaluation.  This is exact also for conditional expressions
 * (a value only read in a branch not taken does not affect the result
 * until the condition changes, which is read itself).
 *
 * Threading: one Lua state and one thread.  The dispatcher update hook
 * only marks the depending calcs as changed and wakes the thread
 * (coalescing, only the latest values matter); the thread evaluates the
 * changed calcs in dependency order and publishes the results.
 *
 * Config load: each expression is compiled (syntax check).  All calcs are
 * evaluated once at startup; unknown value names are evaluation errors, a
 * cyclic dependency (found from the recorded dependencies) disables the
 * calc closing it.
 *
 * Evaluation: if a value read was never received or is older than
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

/**
 * @brief A derived value defined by a Lua expression.
 */
typedef struct UVLUA_CALC {
  const char *name;                    /**< Published value name (Lua identifier). */
  const char *expr;                    /**< Lua expression. */
  int max_age;                         /**< Max. operand age in ms; 0 = no limit. */

  UVRGW_CONF_VAL_DISPATCH_T *disp;     /**< Dispatcher of the result. */
  int func_ref;                        /**< Registry reference of the compiled expression. */

  int deps_count;                      /**< Number of values read by the last evaluation. */
  UVRGW_CONF_VAL_DISPATCH_T **deps;    /**< Values read by the last evaluation; written by the Lua thread under the module lock. */
  bool dirty;                          /**< A dependency changed; protected by the module lock. */

  int order;                           /**< Dependency sort state; Lua thread only. */
  bool disabled;                       /**< Part of a cyclic dependency; not evaluated. */
  int state;                           /**< Last evaluation state (logging); Lua thread only. */
  UVRGW_CONF_VAL_DISPATCH_T *missing;  /**< Value never received that aborted the last evaluation, or NULL. */
} UVLUA_CALC_T;

/**
 * @brief Initialise the module.
 */
void uvlua_init(void);

/**
 * @brief Parse all @c calc{} sections and compile the expressions.
 *
 * Must be called after counter_configure() (calc names are checked against
 * counter names).
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int uvlua_configure(cfg_t *cfg);

/**
 * @brief Free all resources allocated by uvlua_configure().
 *
 * Call uvlua_shutdown() first.
 */
void uvlua_unconfigure(void);

/**
 * @brief Start the Lua thread (evaluates all calcs once).
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
 * other value sources, before the outputs are shut down.  Value updates
 * arriving later are ignored.
 */
void uvlua_shutdown(void);

#endif
