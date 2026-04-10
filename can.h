/**
 * @file can.h
 * @brief CAN bus interface — SocketCAN RX/TX, value encoding/decoding and
 *        NTP-synced timestamp injection.
 *
 * Each CAN interface maps to a @c CAN_IFACE_T which holds a set of
 * @c CAN_FRAME_T definitions.  Each frame in turn contains a list of
 * @c CAN_VAL_T entries that describe typed values packed inside the
 * 8-byte CAN data field.
 *
 * Reception (IN frames) is handled in the main event-loop thread via
 * can_update_fds() / can_handler().  Transmission (OUT frames) and
 * periodic timestamp sending are handled by a per-interface TX thread
 * that wakes every 20 ms.
 */

#ifndef _CAN_H_
#define _CAN_H_

#include "uvrgw_conf.h"

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <confuse.h>
#include <linux/can.h>
#include <sys/select.h>

struct CAN_VAL;
struct CAN_FRAME;
struct CAN_IFACE;

/**
 * @brief A typed value packed inside a CAN frame's data field.
 */
typedef struct CAN_VAL {
  const char *name;      /**< Logical value name (used for dispatch). */
  int type;              /**< Value type; one of the UVRGW_CONF_CAN_TYPE_* constants. */
  int pos;               /**< Byte offset within the 8-byte data field (bit position for BIT type). */
  double scale;          /**< Scale factor applied on RX: raw × scale + offset = dispatched value. */
  double offset;         /**< Offset applied on RX and reversed on TX. */

  UVRGW_CONF_VAL_DISPATCH_T *disp; /**< Dispatcher for this value name. */

  struct CAN_FRAME *frame; /**< Back-pointer to the parent frame. */
} CAN_VAL_T;

/**
 * @brief A CAN frame definition (one per configured frame section).
 */
typedef struct CAN_FRAME {
  int can_id;            /**< Standard CAN identifier (11-bit). */
  int dir;               /**< Direction: UVRGW_CONF_VAL_DIR_IN or UVRGW_CONF_VAL_DIR_OUT. */

  int values_count;      /**< Number of values in this frame. */
  struct CAN_VAL *values; /**< Array of value definitions. */

  struct CAN_IFACE *iface; /**< Back-pointer to the parent interface. */

  struct can_frame send_buf;        /**< TX frame buffer shared between dispatcher and TX thread. */
  pthread_mutex_t send_buf_mutex;   /**< Mutex protecting @c send_buf and @c send_time. */
  int64_t send_time;                /**< Monotonic time (ms) at which @c send_buf should be transmitted; 0 = idle. */
} CAN_FRAME_T;

/**
 * @brief A CAN interface instance (one per configured can{} section).
 */
typedef struct CAN_IFACE {
  const char *interface;   /**< SocketCAN interface name (e.g. "can0"). */
  int timestamp_period;    /**< Interval in ms between NTP timestamp frames; 0 disables. */
  int send_timeout;        /**< Max ms to coalesce outbound value updates before transmitting. */

  int frames_count;        /**< Number of frame definitions. */
  struct CAN_FRAME *frames; /**< Array of frame definitions. */

  int can_fd;              /**< Raw SocketCAN file descriptor; -1 when not open. */
  pthread_t thread;        /**< TX thread handle. */
  bool thread_running;     /**< Set to false to request TX thread termination. */
  int64_t next_timestamp;  /**< Monotonic time (ms) for next NTP timestamp transmission. */
} CAN_IFACE_T;

/**
 * @brief Initialise the CAN module (reset interface list).
 *
 * Must be called before can_configure().
 */
void can_init(void);

/**
 * @brief Parse all @c can{} sections from @p cfg and populate the interface list.
 *
 * @param cfg  Root libconfuse configuration object.
 * @return     0 on success, -1 on error.
 */
int can_configure(cfg_t *cfg);

/**
 * @brief Register send callbacks for all OUT-direction CAN values.
 *
 * Called after the dispatcher callback arrays have been allocated.
 */
void can_register_disp_cbs(void);

/**
 * @brief Free all resources allocated by can_configure().
 *
 * Does not shut down running threads or sockets — call can_shutdown() first.
 */
void can_unconfigure(void);

/**
 * @brief Open all CAN sockets and start per-interface TX threads.
 *
 * On failure, already-started interfaces are shut down before returning.
 *
 * @return  0 on success, -1 on error.
 */
int can_startup(void);

/**
 * @brief Stop all TX threads and close all CAN sockets.
 */
void can_shutdown(void);

/**
 * @brief Add all open CAN socket file descriptors to @p fd_set.
 *
 * Updates @p max_fd as required by select().
 *
 * @param fd_set  The read fd_set to update.
 * @param max_fd  Pointer to the running maximum fd value.
 */
void can_update_fds(fd_set *fd_set, int *max_fd);

/**
 * @brief Process pending CAN RX data for all interfaces.
 *
 * Called from the main event loop after a successful select().  For each
 * interface whose socket is readable, reads one CAN frame, matches it
 * against configured IN-direction frame definitions and dispatches any
 * matching values.
 *
 * @param fd_set  The read fd_set returned by select().
 * @return        0 on success, -1 on fatal socket error.
 */
int can_handler(fd_set *fd_set);

#endif

