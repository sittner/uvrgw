/**
 * @file can.c
 * @brief CAN bus interface implementation.
 *
 * Opens SocketCAN raw sockets, starts a per-interface TX thread and
 * handles RX in the main event-loop thread.  Value encoding/decoding
 * supports little-endian integers of 1, 2 and 4 bytes as well as
 * individual bit access within the 8-byte CAN data field.
 *
 * The TX thread wakes every IFACE_THREAD_PERIOD_US microseconds and:
 *  - transmits any pending outbound frame whose @c send_time has elapsed;
 *  - sends an NTP-synced timestamp frame on CAN ID 0x100 when
 *    @c timestamp_period is non-zero and the period has elapsed.
 */
#include "can.h"
#include "mqtt.h"
#include "mb.h"
#include "ntp_check.h"
#include "utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fd.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/types.h>
#include <sys/eventfd.h>
#include <syslog.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

#define IFACE_THREAD_PERIOD_US 20000

static int can_ifaces_count;
static CAN_IFACE_T *can_ifaces;

static int iface_configure(cfg_t *cfg, void *ctx, void *child);
static int frame_configure(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static int iface_startup(CAN_IFACE_T *iface);
static int iface_rx_handler(fd_set *fd_set, CAN_IFACE_T *iface);
static void *iface_thread(void *ptr);
static int iface_task(CAN_IFACE_T *iface);
static uint32_t read_value(const uint8_t *p, int len);
static void write_value(uint8_t *p, int len, uint32_t val);
static int send_value(void *v, double f);
static int send_timestamp(CAN_IFACE_T *iface);

void can_init(void) {
  can_ifaces_count = 0;
  can_ifaces = NULL;
}

int can_configure(cfg_t *cfg) {
  return uvrgw_conf_config_childs(cfg, "can", &can_ifaces_count, (void **) &can_ifaces, sizeof(CAN_IFACE_T), NULL, iface_configure);
}

void can_register_disp_cbs(void) {
  CAN_IFACE_T *iface;
  int iface_idx;
  CAN_FRAME_T *frame;
  int frame_idx;
  CAN_VAL_T *val;
  int val_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    for (frame = iface->frames, frame_idx = 0; frame_idx < iface->frames_count; frame++, frame_idx++) {
      for (val = frame->values, val_idx = 0; val_idx < frame->values_count; val++, val_idx++) {
        if (frame->dir == UVRGW_CONF_VAL_DIR_OUT) {
          uvrgw_conf_register_disp_cb(val->disp, val, send_value);
        }
      }
    }
  }
}

void can_unconfigure(void) {
  CAN_IFACE_T *iface;
  int iface_idx;
  CAN_FRAME_T *frame;
  int frame_idx;
  CAN_VAL_T *val;
  int val_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    for (frame = iface->frames, frame_idx = 0; frame_idx < iface->frames_count; frame++, frame_idx++) {
      for (val = frame->values, val_idx = 0; val_idx < frame->values_count; val++, val_idx++) {
        free((void *) val->name);
      }
      pthread_mutex_destroy(&frame->send_buf_mutex);
      free(frame->values);
    }
    free((void *) iface->interface);
    free(iface->frames);
  }
  free(can_ifaces);
}

int can_startup(void) {
  CAN_IFACE_T *iface;
  int iface_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    if (iface_startup(iface) < 0) {
      goto fail;
    }
  }

  return 0;

fail:
  can_shutdown();
  return -1;
}

void can_shutdown(void) {
  CAN_IFACE_T *iface;
  int iface_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    // stop thread
    if (iface->thread_running) {
      iface->thread_running = false;
      pthread_join(iface->thread, NULL);
    }

    // close socket
    if (iface->can_fd >= 0) {
      close(iface->can_fd);
      iface->can_fd = -1;
    }
  }
}

int can_handler(fd_set *fd_set) {
  CAN_IFACE_T *iface;
  int iface_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    if (iface_rx_handler(fd_set, iface) < 0) {
      return -1;
    }
  }

  return 0;
}

void can_update_fds(fd_set *fd_set, int *max_fd) {
  CAN_IFACE_T *iface;
  int iface_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    if (iface->can_fd >= 0) {
      utl_update_fds(iface->can_fd, fd_set, max_fd);
    }
  }
}

/**
 * @brief Configure one CAN interface from a libconfuse section.
 *
 * Reads the interface name, timestamp period and send timeout, then
 * recurses into each @c frame{} child section via frame_configure().
 *
 * @param cfg    The @c can{} section.
 * @param ctx    Unused (NULL).
 * @param child  Pre-allocated @c CAN_IFACE_T to populate.
 * @return       0 on success, -1 on error.
 */
static int iface_configure(cfg_t *cfg, void *ctx, void *child) {
  CAN_IFACE_T *iface = (CAN_IFACE_T *) child;

  iface->interface = uvrgw_conf_strdup(cfg_getstr(cfg, "interface"));
  iface->timestamp_period = cfg_getint(cfg, "timestamp_period");
  iface->send_timeout = cfg_getint(cfg, "send_timeout");

  if (iface->interface == NULL) {
    syslog(LOG_ERR, "CAN inteface name not given.");
    return -1;
  }

  iface->can_fd = -1;

  return uvrgw_conf_config_childs(cfg, "frame", &iface->frames_count, (void **) &iface->frames, sizeof(CAN_FRAME_T), iface, frame_configure);
}

/**
 * @brief Configure one CAN frame from a libconfuse section.
 *
 * Reads the CAN ID and direction, initialises the TX send buffer and mutex,
 * then recurses into each @c value{} child via value_configure().
 *
 * @param cfg    The @c frame{} section.
 * @param ctx    Parent @c CAN_IFACE_T pointer.
 * @param child  Pre-allocated @c CAN_FRAME_T to populate.
 * @return       0 on success, -1 on error.
 */
static int frame_configure(cfg_t *cfg, void *ctx, void *child) {
  CAN_FRAME_T *frame = (CAN_FRAME_T *) child;

  frame->iface = (CAN_IFACE_T *) ctx;

  frame->can_id = cfg_getint(cfg, "can_id");
  frame->dir = cfg_getint(cfg, "dir");

  frame->send_buf.can_id = frame->can_id;
  frame->send_buf.can_dlc = 8;
  pthread_mutex_init(&frame->send_buf_mutex, NULL);

  return uvrgw_conf_config_childs(cfg, "value", &frame->values_count, (void **) &frame->values, sizeof(CAN_VAL_T), frame, value_configure);
}

/**
 * @brief Configure one CAN value from a libconfuse section.
 *
 * Reads the value name (section title), type, byte/bit position and
 * scale/offset factors.  Validates that the value fits within the
 * 8-byte CAN data field.  Obtains or creates the named dispatcher.
 *
 * @param cfg    The @c value{} section.
 * @param ctx    Parent @c CAN_FRAME_T pointer.
 * @param child  Pre-allocated @c CAN_VAL_T to populate.
 * @return       0 on success, -1 on error.
 */
static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  CAN_VAL_T *val = (CAN_VAL_T *) child;

  val->frame = (CAN_FRAME_T *) ctx;

  val->name = uvrgw_conf_strdup(cfg_title(cfg));
  val->type = cfg_getint(cfg, "type");
  val->pos = cfg_getint(cfg, "pos");
  val->scale = cfg_getfloat(cfg, "scale");
  val->offset = cfg_getfloat(cfg, "offset");

  if (val->pos < 0) {
    syslog(LOG_ERR, "CAN value '%s' has invalid pos %d.", val->name, val->pos);
    return -1;
  }

  switch (val->type) {
    case UVRGW_CONF_CAN_TYPE_BIT:
      if ((val->pos >> 3) >= 8) {
        syslog(LOG_ERR, "CAN value '%s' bit pos %d exceeds frame data boundary.", val->name, val->pos);
        return -1;
      }
      break;
    case UVRGW_CONF_CAN_TYPE_U8:
    case UVRGW_CONF_CAN_TYPE_S8:
      if (val->pos + 1 > 8) {
        syslog(LOG_ERR, "CAN value '%s' pos %d exceeds frame data boundary for 1-byte type.", val->name, val->pos);
        return -1;
      }
      break;
    case UVRGW_CONF_CAN_TYPE_U16:
    case UVRGW_CONF_CAN_TYPE_S16:
      if (val->pos + 2 > 8) {
        syslog(LOG_ERR, "CAN value '%s' pos %d exceeds frame data boundary for 2-byte type.", val->name, val->pos);
        return -1;
      }
      break;
    case UVRGW_CONF_CAN_TYPE_U32:
    case UVRGW_CONF_CAN_TYPE_S32:
      if (val->pos + 4 > 8) {
        syslog(LOG_ERR, "CAN value '%s' pos %d exceeds frame data boundary for 4-byte type.", val->name, val->pos);
        return -1;
      }
      break;
    default:
      syslog(LOG_ERR, "CAN value '%s' has invalid type.", val->name);
      return -1;
  }

  val->disp = uvrgw_conf_get_dispatcher(val->name, (val->frame->dir == UVRGW_CONF_VAL_DIR_OUT));

  return 0;
}

/**
 * @brief Open the SocketCAN socket and start the TX thread for one interface.
 *
 * Creates a PF_CAN / SOCK_RAW socket, resolves the interface index, binds
 * the socket, and starts the iface_thread() TX thread.
 *
 * @param iface  Interface to start.
 * @return       0 on success, -1 on error (socket is closed on failure).
 */
static int iface_startup(CAN_IFACE_T *iface) {
  // open socket
  if ((iface->can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
    syslog(LOG_ERR, "Could not create CAN socket");
    goto fail0;
  }

  // get interface index
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, iface->interface, IFNAMSIZ - 1);
  ifr.ifr_name[IFNAMSIZ - 1] = '\0';
  if (ioctl(iface->can_fd, SIOCGIFINDEX, &ifr) < 0) {
    syslog(LOG_ERR, "Could not set CAN interface name '%s'", iface->interface);
    goto fail1;
  }

  // bind to socket
  struct sockaddr_can addr;
  memset(&addr, 0, sizeof(addr));
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(iface->can_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    syslog(LOG_ERR, "Could not bind to CAN socket");
    goto fail1;
  }

  // start thread
  iface->thread_running = true;
  if (pthread_create(&(iface->thread), NULL, iface_thread, (void*) iface) != 0) {
    iface->thread_running = false;
    syslog(LOG_ERR, "failed to start iface thread");
    goto fail1;
  }

  return 0;

fail1:
  close(iface->can_fd);
  iface->can_fd = -1;
fail0:
  return -1;
}

/**
 * @brief Process a single received CAN frame for one interface.
 *
 * Called from can_handler() when the interface socket is readable.
 * Reads one standard CAN frame, matches it against all IN-direction
 * frame definitions and dispatches matching values after applying
 * scale and offset.  Extended, RTR and error frames are silently ignored.
 *
 * @param fd_set  The read fd_set from select().
 * @param iface   Interface to service.
 * @return        0 on success, -1 on socket read error.
 */
static int iface_rx_handler(fd_set *fd_set, CAN_IFACE_T *iface) {
  struct can_frame rcvd_frame;
  ssize_t count;
  uint8_t *p;
  CAN_FRAME_T *frame;
  int frame_idx;
  CAN_VAL_T *val;
  int val_idx;
  double f;

  // check if fd is set
  if (!FD_ISSET(iface->can_fd, fd_set)) {
    return 0;
  }

  // read frame
  count = read(iface->can_fd, &rcvd_frame, sizeof(rcvd_frame));
  if (count != sizeof(struct can_frame)) {
    syslog(LOG_ERR, "Failed to read from CAN socket (error = %d)", errno);
    return -1;
  }

  // process only std frames
  if ((rcvd_frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0) {
    return 0;
  }

  for (frame = iface->frames, frame_idx = 0; frame_idx < iface->frames_count; frame++, frame_idx++) {
    // check for CAN input mapping
    if (!(frame->can_id > 0 && frame->dir == UVRGW_CONF_VAL_DIR_IN)) {
      continue;
    }

    // check for matching CAN-ID
    if ((rcvd_frame.can_id & CAN_SFF_MASK) != frame->can_id) {
      continue;
    }

    for (val = frame->values, val_idx = 0; val_idx < frame->values_count; val++, val_idx++) {
      // read value
      if (val->type == UVRGW_CONF_CAN_TYPE_BIT) {
        p = &rcvd_frame.data[(val->pos >> 3)];
        f = (*p & (1 << (val->pos & 7))) ? 1.0 : 0.0;
      } else {
        p = &rcvd_frame.data[val->pos];
        switch (val->type) {
          case UVRGW_CONF_CAN_TYPE_U8:
            f = (double) read_value(p, 1);
            break;
          case UVRGW_CONF_CAN_TYPE_S8:
            f = (double) ((int8_t) read_value(p, 1));
            break;
          case UVRGW_CONF_CAN_TYPE_U16:
            f = (double) read_value(p, 2);
            break;
          case UVRGW_CONF_CAN_TYPE_S16:
            f = (double) ((int16_t) read_value(p, 2));
            break;
          case UVRGW_CONF_CAN_TYPE_U32:
            f = (double) read_value(p, 4);
            break;
          case UVRGW_CONF_CAN_TYPE_S32:
            f = (double) ((int32_t) read_value(p, 4));
            break;
          default:
            f = 0.0;
        }
      }

      // dispatch value
      uvrgw_conf_disp_val(val->disp, val, f * val->scale + val->offset);
    }
  }

  return 0;
}

/**
 * @brief TX thread entry point for one CAN interface.
 *
 * Loops at IFACE_THREAD_PERIOD_US intervals calling iface_task() until
 * @c thread_running is cleared by can_shutdown().
 *
 * @param ptr  @c CAN_IFACE_T pointer cast to void *.
 * @return     NULL.
 */
static void *iface_thread(void *ptr) {
  CAN_IFACE_T *iface = (CAN_IFACE_T *) ptr;

  while (iface->thread_running) {
    iface_task(iface);
    usleep(IFACE_THREAD_PERIOD_US);
  }

  return NULL;
}

/**
 * @brief Single TX iteration for one CAN interface.
 *
 * Sends the NTP timestamp frame if the period has elapsed, then checks
 * all OUT-direction frames and transmits any whose @c send_time has expired.
 *
 * @param iface  Interface to service.
 * @return       0 on success, -1 on socket write error.
 */
static int iface_task(CAN_IFACE_T *iface) {
  int64_t now;
  CAN_FRAME_T *frame;
  int frame_idx;
  ssize_t count;

  now = utl_get_ticks();

  // send timestamp
  if (iface->timestamp_period > 0 && iface->next_timestamp <= now) {
    iface->next_timestamp = now + iface->timestamp_period;
    if (send_timestamp(iface) < 0) {
      return -1;
    }
  }

  // check for pending send frames
  for (frame = iface->frames, frame_idx = 0; frame_idx < iface->frames_count; frame++, frame_idx++) {
    if (!(frame->can_id > 0 && frame->dir == UVRGW_CONF_VAL_DIR_OUT)) {
      continue;
    }

    pthread_mutex_lock(&frame->send_buf_mutex);

    if (frame->send_time == 0 || frame->send_time > now) {
      pthread_mutex_unlock(&frame->send_buf_mutex);
      continue;
    }

    frame->send_time = 0;

    count = write(iface->can_fd, &(frame->send_buf), sizeof(struct can_frame));
    if (count != sizeof(struct can_frame)) {
      pthread_mutex_unlock(&frame->send_buf_mutex);
      syslog(LOG_ERR, "Failed to write to CAN socket (error = %d)", errno);
      return -1;
    }

    pthread_mutex_unlock(&frame->send_buf_mutex);
  }

  return 0;
}

/**
 * @brief Read a little-endian unsigned integer of @p len bytes from @p p.
 *
 * @param p    Pointer to the first (least-significant) byte.
 * @param len  Number of bytes to read (1, 2 or 4).
 * @return     Unsigned 32-bit value.
 */
static uint32_t read_value(const uint8_t *p, int len) {
  int i;
  uint32_t val;

  for (i = 1, val = 0; i <= len; i++) {
    val <<= 8;
    val |= p[len - i];
  }

  return val;
}

/**
 * @brief Write a little-endian integer of @p len bytes to @p p.
 *
 * @param p    Destination pointer (least-significant byte first).
 * @param len  Number of bytes to write (1, 2 or 4).
 * @param val  Value to encode.
 */
static void write_value(uint8_t *p, int len, uint32_t val) {
  int i;

  for (i = 0; i < len; i++) {
    p[i] = val & 0xff;
    val >>= 8;
  }
}

/**
 * @brief Dispatch callback that encodes a value into the CAN TX buffer.
 *
 * Invoked by the value dispatcher when a value with the same name as an
 * OUT-direction CAN value is received.  Applies the inverse of the
 * scale/offset transform and writes the encoded bytes into the shared
 * @c send_buf.  Sets @c send_time if not already armed, coalescing
 * multiple updates within the @c send_timeout window.
 *
 * @param v  @c CAN_VAL_T pointer.
 * @param f  Dispatched value.
 * @return   0 on success.
 */
static int send_value(void *v, double f) {
  int64_t now;
  CAN_VAL_T *val = (CAN_VAL_T *) v;
  CAN_FRAME_T *frame = val->frame;
  uint8_t *p;

  // check for valid CAN id
  if (frame->can_id < 0) {
    return 0;
  }

  now = utl_get_ticks();

  pthread_mutex_lock(&frame->send_buf_mutex);

  // write value
  if (val->type == UVRGW_CONF_CAN_TYPE_BIT) {
    p = &frame->send_buf.data[(val->pos >> 3)];
    if (f > 0.5) {
      *p |= 1 << (val->pos & 7);
    } else {
      *p &= ~(1 << (val->pos & 7));
    }
  } else {
    f = (f - val->offset) / val->scale;
    p = &frame->send_buf.data[val->pos];
    switch (val->type) {
      case UVRGW_CONF_CAN_TYPE_U8:
        write_value(p, 1, (uint8_t) utl_val_limit(f, 0.0, UINT8_MAX));
        break;
      case UVRGW_CONF_CAN_TYPE_S8:
        write_value(p, 1, (int8_t) utl_val_limit(f, INT8_MIN, INT8_MAX));
        break;
      case UVRGW_CONF_CAN_TYPE_U16:
        write_value(p, 2, (uint16_t) utl_val_limit(f, 0.0, UINT16_MAX));
        break;
      case UVRGW_CONF_CAN_TYPE_S16:
        write_value(p, 2, (int16_t) utl_val_limit(f, INT16_MIN, INT16_MAX));
        break;
      case UVRGW_CONF_CAN_TYPE_U32:
        write_value(p, 4, (uint32_t) utl_val_limit(f, 0.0, UINT32_MAX));
        break;
      case UVRGW_CONF_CAN_TYPE_S32:
        write_value(p, 4, (int32_t) utl_val_limit(f, INT32_MIN, INT32_MAX));
        break;
    }
  }

  if (frame->send_time == 0) {
    frame->send_time = now + frame->iface->send_timeout;
  }

  pthread_mutex_unlock(&frame->send_buf_mutex);
  return 0;
}

/**
 * @brief Build and transmit an NTP-synced timestamp CAN frame.
 *
 * The frame uses CAN ID 0x100 with 6 data bytes:
 *   - bytes 0–3: milliseconds since local midnight (little-endian uint32).
 *   - bytes 4–5: days since 1984-01-01 (little-endian uint16).
 *
 * Only sent when ntp_check() confirms the local clock is synchronised.
 *
 * @param iface  Interface on which to send the frame.
 * @return       1 on successful transmission, 0 if NTP not synced,
 *               -1 on socket or time error.
 */
static int send_timestamp(CAN_IFACE_T *iface) {
  struct timeval tv;
  struct tm lt, tmp;
  uint32_t msecs;
  uint16_t days;
  struct can_frame frame;
  ssize_t count;

  // check for valid NTP time
  if (!ntp_check()) {
    syslog(LOG_WARNING, "NTP daemon not synced, will not send timeframe.");
    return 0;
  }

  // get UTC time
  if (gettimeofday(&tv, NULL) < 0) {
    syslog(LOG_ERR, "Failed to get current time (error = %d)", errno);
    return -1;
  }

  // convert to local time
  localtime_r(&tv.tv_sec, &lt);

  // get milliseconds since 0:00
  memcpy(&tmp, &lt, sizeof(struct tm));
  tmp.tm_sec = 0;
  tmp.tm_min = 0;
  tmp.tm_hour = 0;
  msecs = ((tv.tv_sec - mktime(&tmp)) * 1000) + (tv.tv_usec / 1000);

  // get days since 01-01-1984
  memcpy(&tmp, &lt, sizeof(struct tm));
  tmp.tm_mday = 1;
  tmp.tm_mon = 0;
  tmp.tm_year = 84;
  days = (tv.tv_sec - mktime(&tmp)) / (24 * 60 * 60);

  // build can frame
  memset(&frame, 0, sizeof(frame));
  frame.can_id = 0x100;
  frame.can_dlc = 6;
  frame.data[0] = (msecs >> 0) & 0xff;
  frame.data[1] = (msecs >> 8) & 0xff;
  frame.data[2] = (msecs >> 16) & 0xff;
  frame.data[3] = (msecs >> 24) & 0xff;
  frame.data[4] = (days >> 0) & 0xff;
  frame.data[5] = (days >> 8) & 0xff;

  // send frame
  count = write(iface->can_fd, &frame, sizeof(struct can_frame));
  if (count != sizeof(struct can_frame)) {
    syslog(LOG_ERR, "Failed to write to CAN socket (error = %d)", errno);
    return -1;
  }

  return 1;
}

