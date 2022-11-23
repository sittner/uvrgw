#include "can.h"
#include "mqtt.h"

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

static int can_fd = -1;

typedef struct {
  double offset;
  double scale;
  const char *mqtt_topic;
  const char *mqtt_fmt;
} CAN_ANALOG_OUT_T;

typedef struct {
  int can_id;
  CAN_ANALOG_OUT_T vals[4];
} CAN_ANALOG_GRP_T;

static const CAN_ANALOG_GRP_T analog_outs[] = {
  { 0x201, {
    { 0.0, 0.1, "uvr/temp/store/upper", "%.1f" },
    { 0.0, 0.1, "uvr/temp/store/lower", "%.1f" },
    { 0.0, 0.1, "uvr/temp/radi/send", "%.1f" },
    { 0.0, 0.1, "uvr/temp/radi/return", "%.1f" },
  }},
  { 0x281, {
    { 0.0, 0.1, "uvr/temp/outdoor", "%.1f" },
    { 0.0, 0.1, "uvr/temp/garret", "%.1f" },
    { 0.0, 0.1, "uvr/temp/warm_water", "%.1f" },
    { 0.0, 0.1, "uvr/temp/circ_ret", "%.1f" },
  }},
  { 0x301, {
    { 0.0, 0.1, "uvr/power/radi", "%.2f" },
    { 0.0, 0.1, "uvr/energ/radi", "%.1f" },
    { 0.0, 0.1, "uvr/temp/radi/sp", "%.1f" },
    { 0.0, 1.0, "uvr/flow/radi", "%.0f" },
  }},
  { 0x381, {
    { 0.0, 0.1, "uvr/power/hp", "%.2f" },
    { 0.0, 0.1, "uvr/energ/hp", "%.1f" },
    { 0.0, 0.1, "uvr/temp/hp/sp", "%.1f" },
    { 0.0, 0.0, NULL, NULL },
  }},
  { 0 }
};

int can_startup(const char *ifname) {
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

int can_handler(fd_set *fd_set) {
  struct can_frame rcvd_frame;
  ssize_t count;
  int i;
  uint8_t *p;
  const CAN_ANALOG_GRP_T *agrp;
  const CAN_ANALOG_OUT_T *aout;
  int16_t tmp;

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

  for (agrp = analog_outs; agrp->can_id != 0; agrp++) {
    if ((rcvd_frame.can_id & CAN_SFF_MASK) != agrp->can_id) {
      continue;
    }
    p = rcvd_frame.data;
    for (aout = agrp->vals, i = 0; i < 4; aout++, i++) {
      tmp = *(p++);
      tmp |= ((int16_t) *(p++)) << 8;

      if (aout->mqtt_topic != NULL) {
        mqtt_publish_scaled16(aout->mqtt_topic, aout->mqtt_fmt, tmp, aout->offset, aout->scale);
      }
    }
  }

  return 0;
}

void can_update_fds(fd_set *fd_set) {
  FD_SET(can_fd, fd_set);
}

int can_send(const struct can_frame *frame) {
  ssize_t count;

  count = write(can_fd, frame, sizeof(struct can_frame));
  if (count != sizeof(struct can_frame)) {
    syslog(LOG_ERR, "Failed to write to CAN socket (error = %d)", errno);
    return -1;
  }

  return 0;
}

