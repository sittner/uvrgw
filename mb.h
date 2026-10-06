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
 *  - s16 / u16: signed / unsigned 16-bit register
 *  - s32 / u32 / f32: signed / unsigned 32-bit integer or IEEE 754 float
 *    in two registers, high word first (as used by SunSpec); @c word_swap
 *    selects low word first
 *  - bitmask (individual bit extracted from a 16-bit register)
 *    Note: bitmask outputs are written from a local register image which
 *    starts at 0 and is not read back from the device, so all bits of a
 *    written register that matter must be mapped as outputs.
 *  - scale_factor: a companion register provides the decimal exponent
 *    for another value (value × 10^exponent).
 *
 * Input register blocks can optionally
 *  - drop SunSpec "not implemented" values (@c sunspec_na): s16 0x8000,
 *    u16/bitmask 0xffff, s32 0x80000000, u32 0xffffffff, and values whose
 *    scale factor register is 0x8000 (f32 NaN is always dropped),
 *  - check a register for an expected value (@c expect_reg,
 *    @c expect_value), e.g. a SunSpec model ID, and ignore the whole block
 *    on mismatch (protects against shifted register maps).
 */

#ifndef _MB_H_
#define _MB_H_

#include "uvrgw_conf.h"

#include <stdatomic.h>
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
  int reg;                   /**< Register index within the block (0-based). */
  int type;                  /**< Value type; one of the UVRGW_CONF_MB_TYPE_* constants. */
  int bit;                   /**< Bit position (0-15) within the register for bitmask values. */
  double scale;              /**< Scale factor applied after reading: raw × scale + offset. */
  double offset;             /**< Offset added after scaling. */
  bool word_swap;            /**< 32-bit types: low word first instead of high word first. */

  struct MB_BLOCK *block;    /**< Back-pointer to the containing block. */

  struct MB_SLAVE_VAL *value_out_next; /**< Next entry in the slave's output pending list. */

  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher for this value name. */
  uint16_t valbuf;           /**< Local register image for bitmask output writes. */

  bool write_pending;        /**< True when a new output value is queued for writing. */
  double write_value;        /**< Queued output value (valid when @c write_pending is true). */
  bool write_failed;         /**< Last write failed (for state logging). */

  const char *sf_name;       /**< Name of the scale-factor companion register, or NULL. */
  struct MB_SLAVE_VAL *sf_source; /**< Resolved pointer to the scale-factor source value. */
  struct MB_SLAVE_VAL *bitmask_base; /**< First bitmask value at the same register (owner of the shared valbuf). */

} MB_SLAVE_VAL_T;

/**
 * @brief A contiguous block of Modbus registers or bits on one slave.
 */
typedef struct MB_BLOCK {
  int dir;                   /**< Direction: UVRGW_CONF_VAL_DIR_IN or UVRGW_CONF_VAL_DIR_OUT. */
  int regtype;               /**< Register type; one of the UVRGW_CONF_MB_REG_TYPE_* constants. */
  int addr;                  /**< Starting Modbus register address. */
  int count;                 /**< Number of registers or coils in this block. */
  bool sunspec_na;           /**< Drop SunSpec "not implemented" values. */
  int expect_reg;            /**< Register index (in block) to check, -1 = no check. */
  int expect_value;          /**< Expected value of @c expect_reg. */
  bool unexpected;           /**< Last read failed the check (for state logging). */
  bool read_failed;          /**< Last read failed (for state logging). */

  int values_count;          /**< Number of value definitions in this block. */
  struct MB_SLAVE_VAL *values; /**< Array of value definitions. */

  struct MB_SLAVE *slave;    /**< Back-pointer to the containing slave. */
} MB_BLOCK_T;

/**
 * @brief A Modbus slave device attached to a master.
 */
typedef struct MB_SLAVE {
  int id;                    /**< Modbus slave address (1–247; TCP also 0 and 255). */
  int interval;              /**< Polling interval in ms (> 0); also the pause after a failed write. */
  double init_value;         /**< Default @c init_value for all input values of this slave. */
  int stale_timeout;         /**< Default @c stale_timeout (ms) for all input values of this slave; 0 = never. */

  struct MB_MASTER *master;  /**< Back-pointer to the containing master. */

  int blocks_count;          /**< Number of block definitions. */
  struct MB_BLOCK *blocks;   /**< Array of block definitions. */

  int block_read_idx;        /**< Index of the block currently being read (round-robin). */

  struct MB_SLAVE_VAL *value_out_head; /**< Head of the singly-linked output-pending list. */
  struct MB_SLAVE_VAL *value_out_curr; /**< Current position in the output-pending write iterator. */

  int64_t next_poll;         /**< Monotonic time (ms) for the next poll. */
  int64_t next_write;        /**< Monotonic time (ms) before which no write is issued (set after a failed write). */
} MB_SLAVE_T;

/**
 * @brief Common Modbus master state shared by RTU and TCP variants.
 */
typedef struct MB_MASTER {
  int separation_time;       /**< Minimum ms between the end of one Modbus transaction and the start of the next. */
  int timeout;               /**< Transaction timeout in ms. */

  int slaves_count;          /**< Number of slave definitions. */
  struct MB_SLAVE *slaves;   /**< Array of slave definitions. */

  modbus_t *ctx;             /**< libmodbus context (open while thread is running). */
  pthread_mutex_t write_lock; /**< Mutex serialising write_schedule() vs the polling thread. */

  pthread_t thread;          /**< Polling thread handle. */
  atomic_bool thread_running; /**< Set to false to request thread termination. */
  int64_t next_transaction;  /**< Earliest monotonic time (ms) for the next transaction. */

  int slave_curr_idx;        /**< Index of the slave currently being serviced. */

  bool tcp;                  /**< True for TCP masters (connection handled on demand). */
  bool reconnect;            /**< TCP only: connection must be (re)established before the next transaction. */
  bool connect_failed;       /**< TCP only: last connect attempt failed (suppresses repeated log messages). */
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
 *
 * @return  0 on success, -1 on error.
 */
int mb_register_disp_cbs(void);

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

