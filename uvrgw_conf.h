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
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

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
#define UVRGW_CONF_MB_TYPE_S16     1  /**< Signed 16-bit register value. */
#define UVRGW_CONF_MB_TYPE_U16     2  /**< Unsigned 16-bit register value. */
#define UVRGW_CONF_MB_TYPE_BITMASK 3  /**< Individual bit extracted from a 16-bit register. */
#define UVRGW_CONF_MB_TYPE_S32     4  /**< Signed 32-bit value (2 registers). */
#define UVRGW_CONF_MB_TYPE_U32     5  /**< Unsigned 32-bit value (2 registers). */
#define UVRGW_CONF_MB_TYPE_F32     6  /**< IEEE 754 single precision float (2 registers). */
/** @} */

/** @name Counter power sign */
/** @{ */
#define UVRGW_CONF_COUNTER_SIGN_POSITIVE  1  /**< Count positive power. */
#define UVRGW_CONF_COUNTER_SIGN_NEGATIVE -1  /**< Count negative power (as positive energy). */
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
 * @param v      Protocol-specific value context pointer (e.g. @c CAN_VAL_T *).
 * @param f      The value as a double (always finite).
 * @param valid  True if @p f is current data, false if it is a start or
 *               reset value or a value computed from invalid data.
 * @return       0 on success, negative on error.
 */
typedef int (* UVRGW_CONF_DISPATCH_CB)(void *v, double f, bool valid);

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
 *
 * At most one module may publish (produce) a name; it registers itself
 * with uvrgw_conf_set_producer() during configuration.
 *
 * The dispatcher also keeps the last dispatched value and its validity,
 * so consumers can read the current value on demand (see
 * uvrgw_conf_get_val()).  The value is always a finite number; it starts
 * at the producer's @c init_value and is invalid until the first data.
 *
 * Locking: @c disp_lock is held from storing a value until all callbacks
 * have returned, so the stored value and the values delivered to the
 * outputs cannot get out of order.  Callbacks may dispatch other names
 * (e.g. a counter dispatching its total from the callback of its source).
 * Locks are thus taken along the data flow, which is deadlock free as
 * long as the synchronous data flow has no cycle (evals break cycles,
 * their callbacks only mark the eval as pending).  A callback must never
 * dispatch the name it was called for.  @c last_lock only protects the
 * stored value for readers, so a slow callback does not block them.
 */
typedef struct UVRGW_CONF_VAL_DISPATCH {
  const char *name;                       /**< Logical value name shared across protocol sections. */
  char *producer;                         /**< Module publishing this value (e.g. "counter 'x'"); NULL if none. */
  int value_count;                        /**< Total number of registered callbacks (outputs). */
  struct UVRGW_CONF_VAL_DISPATCH *next;   /**< Next dispatcher in the linked list. */

  int value_cbs_pos;                      /**< Next free slot in @c value_cbs (used during registration). */
  struct UVRGW_CONF_DISPATCH_CB_VAL *value_cbs; /**< Array of @c value_count callback entries. */

  void *producer_val;                     /**< Producer's value pointer (source identifier for its dispatches); NULL if none. */
  double init_value;                      /**< Start value of the producer. */
  int stale_timeout;                      /**< Max. time (ms) without data before reset to @c init_value; 0 = never. */

  pthread_mutex_t disp_lock;              /**< Held while storing a value and firing the callbacks; protects @c deadline, @c timed_out and @c received. */
  int64_t deadline;                       /**< Monotonic time (ms) at which the watchdog resets the value; 0 = disarmed. */
  bool timed_out;                         /**< Reset by the watchdog, no data since (for state logging). */
  bool received;                          /**< Valid data has been dispatched at least once (for state logging). */
  pthread_mutex_t last_lock;              /**< Protects @c last_value and @c valid. */
  double last_value;                      /**< Last dispatched value (always finite). */
  bool valid;                             /**< True if @c last_value is current data. */
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
 * @brief Register the module publishing a value.
 *
 * Called during configuration by every module for each name it publishes
 * (inputs, counters, evals).  A name may have only one producer; a second
 * one is a configuration error, logged with both owners.
 *
 * The stored value starts at @p init_value (it is not dispatched).
 *
 * @param dp             Dispatcher of the published name.
 * @param module         Module/section type (e.g. "modbus_tcp", "counter").
 * @param instance       Section identification (e.g. IP address, counter name).
 * @param val            Producer's value pointer, the source identifier it
 *                       passes to uvrgw_conf_disp_val().
 * @param init_value     Value before the first data and after a timeout.
 * @param stale_timeout  Max. time (ms) without data before the value is
 *                       reset to @p init_value; 0 = never.
 * @return               0 on success, -1 if the name already has a producer
 *                       or @p init_value / @p stale_timeout is invalid.
 */
int uvrgw_conf_set_producer(UVRGW_CONF_VAL_DISPATCH_T *dp, const char *module, const char *instance,
                            void *val, double init_value, int stale_timeout);

/**
 * @brief Get the head of the dispatcher list.
 *
 * Valid after configuration until uvrgw_conf_cleanup(); follow @c next.
 *
 * @return  First dispatcher, or NULL if there is none.
 */
UVRGW_CONF_VAL_DISPATCH_T *uvrgw_conf_get_dispatchers(void);

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
 * Stores @p f and @p valid as the dispatcher's last value, then iterates
 * over all entries in @p dp->value_cbs and calls every non-NULL callback
 * whose @c val pointer differs from @p val, preventing the source from
 * receiving its own value back.  Holds @c disp_lock meanwhile (see
 * @c UVRGW_CONF_VAL_DISPATCH_T).
 *
 * A non-finite @p f is neither stored nor forwarded; inputs drop invalid
 * readings themselves, this only keeps the stored value finite.
 *
 * @param dp     Dispatcher to fire.
 * @param val    Source value pointer (excluded from delivery).
 * @param f      Value to dispatch as a double.
 * @param valid  True for current data (inputs), false for a reset or a
 *               value computed from invalid data.
 */
void uvrgw_conf_disp_val(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f, bool valid);

/**
 * @brief Get the current value and its validity.
 *
 * Thread-safe; may be called from any thread.
 *
 * @param dp  Dispatcher to read.
 * @param f   Output: current value (the producer's @c init_value before
 *            the first data).
 * @return    true if the value is valid, false otherwise.
 */
bool uvrgw_conf_get_val(UVRGW_CONF_VAL_DISPATCH_T *dp, double *f);

/**
 * @brief Start the watchdog that resets values without current data.
 *
 * Every name whose producer has a @c stale_timeout gets a deadline,
 * armed here and on every dispatch of valid data.  Once a deadline has
 * expired (checked once per second) the value is set to the producer's
 * @c init_value, marked invalid and dispatched once with the producer's
 * value pointer as source, then the deadline stays disarmed until new
 * data arrives.  A name that never delivers is thus reset once at
 * @c stale_timeout after startup.  Reset and next data are logged once.
 *
 * The watchdog dispatches values, so it is started after the outputs and
 * stopped before them.
 *
 * @return  0 on success, -1 if the thread could not be started.
 */
int uvrgw_conf_startup(void);

/**
 * @brief Stop the watchdog thread.
 */
void uvrgw_conf_shutdown(void);

/**
 * @brief Get the state directory for persistent data.
 *
 * Configured by the top-level @c state_dir option; defaults to
 * @c $STATE_DIRECTORY (set by systemd's StateDirectory=) or
 * @c /var/lib/uvrgw.
 *
 * @return  State directory path (valid until uvrgw_conf_cleanup()).
 */
const char *uvrgw_conf_state_dir(void);

#endif

