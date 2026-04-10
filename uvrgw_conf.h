/**
 * @file uvrgw_conf.h
 * @brief Configuration file parsing and central value dispatch system.
 *
 * This header exposes:
 *  - Constants for value directions (IN/OUT), MQTT value types,
 *    CAN value types, Modbus value types, register types and parity.
 *  - The @c UVRGW_CONF_VAL_DISPATCH_T and @c UVRGW_CONF_DISPATCH_CB_VAL_T
 *    structures that implement the named value dispatch mechanism.
 *  - Functions for loading/cleaning up the configuration, enumerating
 *    child sections, and registering/firing value dispatch callbacks.
 *
 * The value dispatch system is the glue between all protocol subsystems.
 * Each named value corresponds to a @c UVRGW_CONF_VAL_DISPATCH_T node in a
 * linked list.  Protocol inputs register themselves without a callback;
 * protocol outputs register a send callback.  When an input receives a
 * value it calls uvrgw_conf_disp_val(), which invokes every registered
 * callback whose @c val pointer differs from the source, preventing loopback.
 */

#ifndef _UVRGW_CONF_H_
#define _UVRGW_CONF_H_

#include <confuse.h>
#include <stdbool.h>

/** @defgroup val_dir Value direction constants
 *  @{ */
#define UVRGW_CONF_VAL_DIR_IN  0  /**< Value is an input (received from a protocol). */
#define UVRGW_CONF_VAL_DIR_OUT 1  /**< Value is an output (sent to a protocol). */
/** @} */

/** @defgroup mqtt_types MQTT value type constants
 *  @{ */
#define UVRGW_CONF_MQTT_TYPE_NUMBER  0  /**< Numeric value formatted with a printf format string. */
#define UVRGW_CONF_MQTT_TYPE_SWITCH  1  /**< Boolean switch: "ON" or "OFF". */
#define UVRGW_CONF_MQTT_TYPE_CONTACT 2  /**< Contact state: "OPEN" or "CLOSED". */
/** @} */

/** @defgroup can_types CAN value type constants
 *  @{ */
#define UVRGW_CONF_CAN_TYPE_BIT  0  /**< Single bit within the CAN frame data field. */
#define UVRGW_CONF_CAN_TYPE_U8   1  /**< Unsigned 8-bit integer. */
#define UVRGW_CONF_CAN_TYPE_S8   2  /**< Signed 8-bit integer. */
#define UVRGW_CONF_CAN_TYPE_U16  3  /**< Unsigned 16-bit integer (little-endian). */
#define UVRGW_CONF_CAN_TYPE_S16  4  /**< Signed 16-bit integer (little-endian). */
#define UVRGW_CONF_CAN_TYPE_U32  5  /**< Unsigned 32-bit integer (little-endian). */
#define UVRGW_CONF_CAN_TYPE_S32  6  /**< Signed 32-bit integer (little-endian). */
/** @} */

/** @defgroup mb_val_types Modbus value type constants
 *  @{ */
#define UVRGW_CONF_MB_TYPE_BIT      0  /**< Single bit (coil or discrete input). */
#define UVRGW_CONF_MB_TYPE_SIGNED   1  /**< Signed 16-bit register value. */
#define UVRGW_CONF_MB_TYPE_UNSIGNED 2  /**< Unsigned 16-bit register value. */
#define UVRGW_CONF_MB_TYPE_BITMASK  3  /**< Individual bit extracted from a 16-bit register. */
/** @} */

/** @defgroup mb_reg_types Modbus register type constants
 *  @{ */
#define UVRGW_CONF_MB_REG_TYPE_INBIT 0  /**< Discrete input (read-only bit). */
#define UVRGW_CONF_MB_REG_TYPE_BIT   1  /**< Coil (read/write bit). */
#define UVRGW_CONF_MB_REG_TYPE_INREG 2  /**< Input register (read-only 16-bit). */
#define UVRGW_CONF_MB_REG_TYPE_REG   3  /**< Holding register (read/write 16-bit). */
/** @} */

/** @defgroup mb_parity Modbus RTU parity constants
 *  @{ */
#define UVRGW_CONF_MB_PARITY_NONE ((int) 'N')  /**< No parity. */
#define UVRGW_CONF_MB_PARITY_EVEN ((int) 'E')  /**< Even parity. */
#define UVRGW_CONF_MB_PARITY_ODD  ((int) 'O')  /**< Odd parity. */
/** @} */

/**
 * @brief Callback invoked by uvrgw_conf_config_childs() for each child section.
 *
 * @param cfg    The child libconfuse section.
 * @param ctx    Caller-supplied context pointer.
 * @param child  Pointer to the pre-allocated child data structure to populate.
 * @return       0 on success, negative on error.
 */
typedef int (* UVRGW_CONF_CONFIG_CHILD_CB)(cfg_t *cfg, void *ctx, void *child);

/**
 * @brief Callback invoked when a dispatched value should be forwarded to an output.
 *
 * @param v  Protocol-specific value context pointer (e.g. @c CAN_VAL_T *).
 * @param f  The value as a double.
 * @return   0 on success, negative on error.
 */
typedef int (* UVRGW_CONF_DISPATCH_CB)(void *v, double f);

/**
 * @brief Associates a protocol value with its dispatch callback.
 */
typedef struct UVRGW_CONF_DISPATCH_CB_VAL {
  void *val;               /**< Protocol-specific value pointer; used as a source identifier to prevent loopback. */
  UVRGW_CONF_DISPATCH_CB cb; /**< Callback to invoke when the value should be sent to this output; NULL for inputs. */
} UVRGW_CONF_DISPATCH_CB_VAL_T;

struct UVRGW_CONF_VAL_DISPATCH;

/**
 * @brief Named value dispatcher.
 *
 * Each unique value name in the configuration has one node in the
 * global dispatcher linked list.  Output protocol values register a
 * send callback; input values register without a callback (callback is
 * NULL) to count references so the callback array can be pre-allocated.
 * When an input fires uvrgw_conf_disp_val() all non-NULL callbacks whose
 * @c val differs from the source are invoked.
 */
typedef struct UVRGW_CONF_VAL_DISPATCH {
  const char *name;                       /**< Logical value name shared across protocol sections. */
  int value_count;                        /**< Total number of registered callbacks (outputs). */
  struct UVRGW_CONF_VAL_DISPATCH *next;   /**< Next dispatcher in the linked list. */

  int value_cbs_pos;                      /**< Next free slot in @c value_cbs (used during registration). */
  struct UVRGW_CONF_DISPATCH_CB_VAL *value_cbs; /**< Array of @c value_count callback entries. */
} UVRGW_CONF_VAL_DISPATCH_T;

/**
 * @brief Load and parse the configuration file, then initialise all subsystems.
 *
 * Parses @p file using libconfuse, configures CAN, Modbus, MQTT and REST
 * modules, allocates the dispatcher callback arrays, and registers all
 * dispatch callbacks.
 *
 * @param file  Path to the configuration file.
 * @return      0 on success, -1 on error (messages logged via syslog).
 */
int uvrgw_conf_load(const char *file);

/**
 * @brief Free all resources allocated by uvrgw_conf_load().
 *
 * Unconfigures all subsystems and frees the dispatcher linked list.
 */
void uvrgw_conf_cleanup(void);

/**
 * @brief Iterate over all child sections of a given name and call a callback for each.
 *
 * Allocates an array of @p size × n_children bytes, then calls @p cccb for
 * each child section to fill in the individual elements.
 *
 * @param cfg    Parent libconfuse section.
 * @param name   Name of the child section (e.g. "can", "slave").
 * @param count  Output: number of children found.
 * @param data   Output: pointer to the allocated array.
 * @param size   Size in bytes of each element in the array.
 * @param ctx    Context pointer forwarded to @p cccb.
 * @param cccb   Callback invoked for each child section.
 * @return       0 on success, -1 on allocation or callback error.
 */
int uvrgw_conf_config_childs(cfg_t *cfg, const char *name, int *count, void **data, int size, void *ctx, UVRGW_CONF_CONFIG_CHILD_CB cccb);

/**
 * @brief Duplicate a string, returning NULL if the input is NULL.
 *
 * Thin wrapper around strdup() that handles NULL gracefully.
 *
 * @param s  String to duplicate, or NULL.
 * @return   Newly allocated copy, or NULL if @p s is NULL.
 */
char *uvrgw_conf_strdup(const char *s);

/**
 * @brief Look up (or create) the dispatcher for the given value name.
 *
 * Searches the global dispatcher linked list for an entry matching @p name.
 * If none is found a new entry is allocated and appended.  When @p alloc_cb
 * is true the entry's @c value_count is incremented to reserve a callback
 * slot (used by output registrations during configuration).
 *
 * @param name      Logical value name.
 * @param alloc_cb  If true, increment the output callback counter.
 * @return          Pointer to the dispatcher entry (never NULL unless OOM).
 */
UVRGW_CONF_VAL_DISPATCH_T *uvrgw_conf_get_dispatcher(const char *name, bool alloc_cb);

/**
 * @brief Register a send callback on a dispatcher.
 *
 * Must be called after uvrgw_conf_load() has allocated the callback arrays
 * (i.e. during the register_disp_cbs phase).  Each call fills the next free
 * slot in the dispatcher's @c value_cbs array.
 *
 * @param dp   Dispatcher to register on.
 * @param val  Protocol-specific value pointer (used as source identifier).
 * @param cb   Callback to invoke when the value is dispatched.
 * @return     0 on success, -1 if the preallocated slot count was exceeded.
 */
int uvrgw_conf_register_disp_cb(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, UVRGW_CONF_DISPATCH_CB cb);

/**
 * @brief Dispatch a value to all registered outputs except the source.
 *
 * Iterates over all entries in @p dp->value_cbs and calls every non-NULL
 * callback whose @c val pointer differs from @p val, preventing the source
 * from receiving its own value back.
 *
 * @param dp   Dispatcher to fire.
 * @param val  Source value pointer (excluded from delivery).
 * @param f    Value to dispatch as a double.
 */
void uvrgw_conf_disp_val(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f);

#endif

