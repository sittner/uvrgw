/**
 * @file mb.h
 * @brief Modbus RTU and TCP master — register polling, read/write and value dispatch.
 *
 * Supports two master types:
 *  - @c MB_RTU_MASTER_T: serial Modbus RTU (RS-232 or RS-485).
 *  - @c MB_TCP_MASTER_T: Modbus TCP over Ethernet.
 *
 * Each master runs a dedicated polling thread that wakes every 10 ms.
 * Slaves are polled round-robin; each slave's blocks are read in turn.
 * Pending output writes are flushed between read transactions.
 *
 * Value types supported:
 *  - bit (coil / discrete input)
 *  - signed 16-bit register
 *  - unsigned 16-bit register
 *  - bitmask (individual bit extracted from a 16-bit register)
 *  - scale_factor: a companion register provides the decimal exponent
 *    for another value (value × 10^exponent).
 */

#ifndef _MB_H_
#define _MB_H_

#include "uvrgw_conf.h"

#include <modbus/modbus.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct MB_SLAVE_VAL;
struct MB_BLOCK;
struct MB_SLAVE;
struct MB_MASTER;
struct MB_RTU_MASTER;
struct MB_TCP_MASTER;

/**
 * @brief A single value definition within a Modbus block.
 */
typedef struct MB_SLAVE_VAL {
  const char *name;          /**< Logical value name (used for dispatch). */
  int offset;                /**< Register index within the block (0-based). */
  int type;                  /**< Value type; one of the UVRGW_CONF_MB_TYPE_* constants. */
  int pos;                   /**< Bit position within the register for bitmask values. */
  double scale;              /**< Scale factor applied after reading: raw × scale + val_offset. */
  double val_offset;         /**< Offset added after scaling. */

  struct MB_BLOCK *block;    /**< Back-pointer to the containing block. */

  struct MB_SLAVE_VAL *value_out_next; /**< Next entry in the slave's output pending list. */

  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher for this value name. */
  uint16_t valbuf;           /**< Raw 16-bit register buffer (used by bitmask values). */

  bool write_pending;        /**< True when a new output value is queued for writing. */
  double write_value;        /**< Queued output value (valid when @c write_pending is true). */

  const char *sf_name;       /**< Name of the scale-factor companion register, or NULL. */
  struct MB_SLAVE_VAL *sf_source; /**< Resolved pointer to the scale-factor source value. */
  struct MB_SLAVE_VAL *bitmask_base; /**< Pointer to the first bitmask value at the same offset (shared valbuf). */

} MB_SLAVE_VAL_T;

/**
 * @brief A contiguous block of Modbus registers or bits on one slave.
 */
typedef struct MB_BLOCK {
  int dir;                   /**< Direction: UVRGW_CONF_VAL_DIR_IN or UVRGW_CONF_VAL_DIR_OUT. */
  int regtype;               /**< Register type; one of the UVRGW_CONF_MB_REG_TYPE_* constants. */
  int addr;                  /**< Starting Modbus register address. */
  int count;                 /**< Number of registers or coils in this block. */

  int values_count;          /**< Number of value definitions in this block. */
  struct MB_SLAVE_VAL *values; /**< Array of value definitions. */

  struct MB_SLAVE *slave;    /**< Back-pointer to the containing slave. */
} MB_BLOCK_T;

/**
 * @brief A Modbus slave device attached to a master.
 */
typedef struct MB_SLAVE {
  int id;                    /**< Modbus slave address (1–247). */
  int interval;              /**< Polling interval in ms. */

  struct MB_MASTER *master;  /**< Back-pointer to the containing master. */

  int blocks_count;          /**< Number of block definitions. */
  struct MB_BLOCK *blocks;   /**< Array of block definitions. */

  int block_read_idx;        /**< Index of the block currently being read (round-robin). */

  struct MB_SLAVE_VAL *value_out_head; /**< Head of the singly-linked output-pending list. */
  struct MB_SLAVE_VAL *value_out_curr; /**< Current position in the output-pending write iterator. */

  int64_t next_poll;         /**< Monotonic time (ms) for the next poll. */
} MB_SLAVE_T;

/**
 * @brief Common Modbus master state shared by RTU and TCP variants.
 */
typedef struct MB_MASTER {
  int separation_time;       /**< Minimum ms between successive Modbus transactions. */
  int timeout;               /**< Transaction timeout in ms. */

  int slaves_count;          /**< Number of slave definitions. */
  struct MB_SLAVE *slaves;   /**< Array of slave definitions. */

  modbus_t *ctx;             /**< libmodbus context (open while thread is running). */
  pthread_mutex_t write_lock; /**< Mutex serialising write_schedule() vs the polling thread. */

  pthread_t thread;          /**< Polling thread handle. */
  bool thread_running;       /**< Set to false to request thread termination. */
  int64_t next_transaction;  /**< Earliest monotonic time (ms) for the next transaction. */

  int slave_curr_idx;        /**< Index of the slave currently being serviced. */
} MB_MASTER_T;

/**
 * @brief Modbus RTU master (serial port).
 *
 * Extends @c MB_MASTER_T with serial port parameters.
 */
typedef struct MB_RTU_MASTER {
  struct MB_MASTER master;   /**< Embedded common master state (must be first). */

  const char *interface;     /**< Serial device path (e.g. "/dev/ttyUSB0"). */
  int baud;                  /**< Baud rate. */
  int parity;                /**< Parity; one of UVRGW_CONF_MB_PARITY_* constants. */
  int data_bits;             /**< Data bits per character (typically 8). */
  int stop_bits;             /**< Stop bits (1 or 2). */
  int mode;                  /**< RS-232 or RS-485 mode (MODBUS_RTU_RS232 / MODBUS_RTU_RS485). */
  int rts;                   /**< RTS control mode (MODBUS_RTU_RTS_NONE/UP/DOWN). */
  int rts_delay;             /**< RTS activation delay in µs; -1 = use libmodbus default. */
} MB_RTU_MASTER_T;

/**
 * @brief Modbus TCP master (network).
 *
 * Extends @c MB_MASTER_T with TCP connection parameters.
 */
typedef struct MB_TCP_MASTER {
  struct MB_MASTER master;   /**< Embedded common master state (must be first). */

  const char *ip;            /**< Remote IP address. */
  int port;                  /**< TCP port (default 502). */
} MB_TCP_MASTER_T;

/**
 * @brief Initialise the Modbus module (reset master lists).
 *
 * Must be called before mb_configure().
 */
void mb_init(void);

/**
 * @brief Parse all @c modbus_rtu{} and @c modbus_tcp{} sections from @p cfg.
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int mb_configure(cfg_t *cfg);

/**
 * @brief Register write callbacks for all OUT-direction Modbus values.
 *
 * Called after the dispatcher callback arrays have been allocated.
 */
void mb_register_disp_cbs(void);

/**
 * @brief Free all resources allocated by mb_configure().
 *
 * Does not shut down running threads — call mb_shutdown() first.
 */
void mb_unconfigure(void);

/**
 * @brief Open all Modbus connections and start per-master polling threads.
 *
 * @return  0 on success, -1 on error.
 */
int mb_startup(void);

/**
 * @brief Stop all polling threads and close all Modbus connections.
 */
void mb_shutdown(void);

#endif

