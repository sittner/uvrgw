#include "can.h"
#include "mqtt.h"
#include "mb.h"
#include "timer.h"
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

static int can_ifaces_count;
static CAN_IFACE_T *can_ifaces;

static int iface_configure(cfg_t *cfg, void *ctx, void *child);
static int frame_configure(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);
static int iface_startup(CAN_IFACE_T *iface);
static int iface_rx_handler(fd_set *fd_set, CAN_IFACE_T *iface);
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
    if (iface->can_fd >= 0) {
      // close socket
      close(iface->can_fd);
      iface->can_fd = -1;
    }
  }
}

int can_task(void) {
  CAN_IFACE_T *iface;
  int iface_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    if (iface_task(iface) < 0) {
      return -1;
    }
  }

  return 0;
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

void can_update_fds(fd_set *fd_set) {
  CAN_IFACE_T *iface;
  int iface_idx;

  for (iface = can_ifaces, iface_idx = 0; iface_idx < can_ifaces_count; iface++, iface_idx++) {
    if (iface->can_fd >= 0) {
      FD_SET(iface->can_fd, fd_set);
    }
  }
}

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

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  CAN_VAL_T *val = (CAN_VAL_T *) child;

  val->frame = (CAN_FRAME_T *) ctx;

  val->name = uvrgw_conf_strdup(cfg_title(cfg));
  val->type = cfg_getint(cfg, "type");
  val->pos = cfg_getint(cfg, "pos");
  val->scale = cfg_getfloat(cfg, "scale");
  val->offset = cfg_getfloat(cfg, "offset");

  val->disp = uvrgw_conf_get_dispatcher(val->name, (val->frame->dir == UVRGW_CONF_VAL_DIR_OUT));

  return 0;
}

static int iface_startup(CAN_IFACE_T *iface) {
  // open socket
  if ((iface->can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
    syslog(LOG_ERR, "Could not create CAN socket");
    goto fail0;
  }

  // get interface index
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, iface->interface, IFNAMSIZ);
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

  // init timestamp send timer
  iface->timestamp_timer = 0;

  return 0;

fail1:
  close(iface->can_fd);
  iface->can_fd = -1;
fail0:
  return -1;
}

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

static int iface_task(CAN_IFACE_T *iface) {
  CAN_FRAME_T *frame;
  int frame_idx;
  ssize_t count;

  // send timestamp
  if (iface->timestamp_period > 0) {
    iface->timestamp_timer += TIMER_PERIOD_MS;
    if (iface->timestamp_timer >= iface->timestamp_period) {
      iface->timestamp_timer -= iface->timestamp_period;
      if (send_timestamp(iface) < 0) {
        return -1;
      }
    }
  }

  // check for pending send frames
  for (frame = iface->frames, frame_idx = 0; frame_idx < iface->frames_count; frame++, frame_idx++) {
    if (!(frame->can_id > 0 && frame->dir == UVRGW_CONF_VAL_DIR_OUT)) {
      continue;
    }
    if (!frame->send_pending) {
      continue;
    }

    if (frame->send_timer < iface->send_timeout) {
      frame->send_timer += TIMER_PERIOD_MS;
      continue;
    }
    frame->send_timer = 0;

    pthread_mutex_lock(&frame->send_buf_mutex);
    frame->send_pending = false;

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

static uint32_t read_value(const uint8_t *p, int len) {
  int i;
  uint32_t val;

  for (i = 1, val = 0; i <= len; i++) {
    val <<= 8;
    val |= p[len - i];
  }

  return val;
}

static void write_value(uint8_t *p, int len, uint32_t val) {
  int i;

  for (i = 0; i < len; i++) {
    p[i] = val & 0xff;
    val >>= 8;
  }
}

static int send_value(void *v, double f) {
  CAN_VAL_T *val = (CAN_VAL_T *) v;
  CAN_FRAME_T *frame = val->frame;
  uint8_t *p;

  // check for valid CAN id
  if (frame->can_id < 0) {
    return 0;
  }

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

  frame->send_pending = true;
  pthread_mutex_unlock(&frame->send_buf_mutex);
  return 0;
}

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

