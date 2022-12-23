#include "can.h"
#include "mqtt.h"
#include "mb.h"
#include "timer.h"
#include "ntp_check.h"

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

#define TIMESTAMP_PERIOD_MS 60000

static int can_fd = -1;
static int timestamp_timer;

#define OUTPUT_BUF_COUNT 32

static struct can_frame output_buf[OUTPUT_BUF_COUNT];
pthread_mutex_t output_buf_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t read_value(const uint8_t *p, int len);
static void write_value(uint8_t *p, int len, uint32_t val);
static double val_limit(double val, double min, double max);
static int send_timestamp(void);

int can_startup(const char *ifname) {
  const IOCONF_CHAN_T *chan;
  struct can_frame *frame, *found;
  int i;

  // setup output buffers
  memset(output_buf, 0, sizeof(output_buf));
  for (chan = ioconf_tab; chan->topic != NULL ; chan++) {
    // check for CAN output mapping
    if (chan->can.can_id == 0 || chan->can.input) {
      continue;
    }

    // search for frame
    for (frame = output_buf, found = NULL, i = 0; frame->can_id != 0; frame++, i++) {
      if (i >= OUTPUT_BUF_COUNT) {
        syslog(LOG_ERR, "Maximum CAN output buffer count exceeded.");
        goto fail0;
      }

      if (frame->can_id == chan->can.can_id) {
        found = frame;
        break;
      }
    }

    // initialize frame, if not found
    if (found == NULL) {
      frame->can_id = chan->can.can_id;
      frame->can_dlc = 8;
    }
  }

  // open socket
  if ((can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
    syslog(LOG_ERR, "Could not create CAN socket");
    goto fail0;
  }

  // get interface index
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
  if (ioctl(can_fd, SIOCGIFINDEX, &ifr) < 0) {
    syslog(LOG_ERR, "Could not set CAN interface name '%s'", ifname);
    goto fail1;
  }

  // bind to socket
  struct sockaddr_can addr;
  memset(&addr, 0, sizeof(addr));
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(can_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    syslog(LOG_ERR, "Could not bind to CAN socket");
    goto fail1;
  }

  // init timestamp send timer
  timestamp_timer = 0;

  return 0;

fail1:
  close(can_fd);
  can_fd = -1;
fail0:
  return -1;
}

void can_shutdown(void) {
  // close socket
  close(can_fd);
  can_fd = -1;
}

int can_task(void) {
  // send timestamp every 60 s
  timestamp_timer += TIMER_PERIOD_MS;
  if (timestamp_timer >= TIMESTAMP_PERIOD_MS) {
    timestamp_timer -= TIMESTAMP_PERIOD_MS;
    if (send_timestamp() < 0) {
      return -1;
    }
  }

  return 0;
}

int can_handler(fd_set *fd_set) {
  struct can_frame rcvd_frame;
  ssize_t count;
  uint8_t *p;
  const IOCONF_CHAN_T *chan;
  double val;

  // check if fd is set
  if (!FD_ISSET(can_fd, fd_set)) {
    return 0;
  }

  // read frame
  count = read(can_fd, &rcvd_frame, sizeof(rcvd_frame));
  if (count != sizeof(struct can_frame)) {
    syslog(LOG_ERR, "Failed to read from CAN socket (error = %d)", errno);
    return -1;
  }

  // process only std frames
  if ((rcvd_frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0) {
    return 0;
  }

  for (chan = ioconf_tab; chan->topic != NULL ; chan++) {
    // check for CAN input mapping
    if (chan->can.can_id == 0 || !chan->can.input) {
      continue;
    }

    // check for matching CAN-ID
    if ((rcvd_frame.can_id & CAN_SFF_MASK) != chan->can.can_id) {
      continue;
    }

    // read value
    if (chan->can.type == IOCONF_CAN_TYPE_BIT) {
      p = &rcvd_frame.data[(chan->can.pos >> 3)];
      val = (*p & (1 << (chan->can.pos & 7))) ? 1.0 : 0.0;
    } else {
      p = &rcvd_frame.data[chan->can.pos];
      switch (chan->can.type) {
        case IOCONF_CAN_TYPE_U8:
          val = (double) read_value(p, 1);
          break;
        case IOCONF_CAN_TYPE_S8:
          val = (double) ((int8_t) read_value(p, 1));
          break;
        case IOCONF_CAN_TYPE_U16:
          val = (double) read_value(p, 2);
          break;
        case IOCONF_CAN_TYPE_S16:
          val = (double) ((int16_t) read_value(p, 2));
          break;
        case IOCONF_CAN_TYPE_U32:
          val = (double) read_value(p, 4);
          break;
        case IOCONF_CAN_TYPE_S32:
          val = (double) ((int32_t) read_value(p, 4));
          break;
        default:
          val = 0.0;
      }
      val = val * chan->can.scale + chan->can.offset;
    }

    // write modbus
    mb_write_chan(chan, val);

    // send MQTT topic
    mqtt_publish_chan(chan, val);
  }

  return 0;
}

void can_update_fds(fd_set *fd_set) {
  FD_SET(can_fd, fd_set);
}

int can_send_chan(const IOCONF_CHAN_T *chan, double val) {
  struct can_frame *frame, *found;
  int i;
  uint8_t *p;
  ssize_t count;

  // check for CAN output mapping
  if (chan->can.can_id == 0 || chan->can.input) {
    return 0;
  }

  pthread_mutex_lock(&output_buf_lock);

  // search for matching frame buffer
  for (frame = output_buf, found = NULL, i = 0; frame->can_id != 0 && i < OUTPUT_BUF_COUNT; frame++, i++) {
    if (frame->can_id == chan->can.can_id) {
      found = frame;
      break;
    }
  }

  // exit, if not found
  if (found == NULL) {
    pthread_mutex_unlock(&output_buf_lock);
    return 0;
  }

  // write value
  if (chan->can.type == IOCONF_CAN_TYPE_BIT) {
    p = &found->data[(chan->can.pos >> 3)];
    if (val > 0.5) {
      *p |= 1 << (chan->can.pos & 7);
    } else {
      *p &= ~(1 << (chan->can.pos & 7));
    }
  } else {
    val = (val - chan->can.offset) / chan->can.scale;
    p = &found->data[chan->can.pos];
    switch (chan->can.type) {
      case IOCONF_CAN_TYPE_U8:
        write_value(p, 1, (uint8_t) val_limit(val, 0.0, UINT8_MAX));
        break;
      case IOCONF_CAN_TYPE_S8:
        write_value(p, 1, (int8_t) val_limit(val, INT8_MIN, INT8_MAX));
        break;
      case IOCONF_CAN_TYPE_U16:
        write_value(p, 2, (uint16_t) val_limit(val, 0.0, UINT16_MAX));
        break;
      case IOCONF_CAN_TYPE_S16:
        write_value(p, 2, (int16_t) val_limit(val, INT16_MIN, INT16_MAX));
        break;
      case IOCONF_CAN_TYPE_U32:
        write_value(p, 4, (uint32_t) val_limit(val, 0.0, UINT32_MAX));
        break;
      case IOCONF_CAN_TYPE_S32:
        write_value(p, 4, (int32_t) val_limit(val, INT32_MIN, INT32_MAX));
        break;
    }
  }

  if (chan->can.send) {
    count = write(can_fd, frame, sizeof(struct can_frame));
    if (count != sizeof(struct can_frame)) {
      pthread_mutex_unlock(&output_buf_lock);
      syslog(LOG_ERR, "Failed to write to CAN socket (error = %d)", errno);
      return -1;
    }
  }

  pthread_mutex_unlock(&output_buf_lock);
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

static double val_limit(double val, double min, double max) {
  if (val < min) {
    return min;
  }

  if (val > max) {
    return max;
  }

  return val;
}

static int send_timestamp(void) {
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
  count = write(can_fd, &frame, sizeof(struct can_frame));
  if (count != sizeof(struct can_frame)) {
    syslog(LOG_ERR, "Failed to write to CAN socket (error = %d)", errno);
    return -1;
  }

  return 1;
}

