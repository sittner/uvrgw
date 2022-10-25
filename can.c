#include "can.h"

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

  // get flags
//  isEff = (rcvd_frame.can_id & CAN_EFF_FLAG);
//  isRtr = (rcvd_frame.can_id & CAN_RTR_FLAG);
//  isErr = (rcvd_frame.can_id & CAN_ERR_FLAG);
  return 0;
}

void can_update_fds(fd_set *fd_set) {
  FD_SET(can_fd, fd_set);
}

/*
uint16_t u16_RCU_CAN_WriteMsg(const te_RCU_CAN_Node _ce_CanNodeNo, const uint16_t _cu16_MsgObjNo, const ts_RCU_CAN_Msg * const _ps_Msg) {
  ts_RCU_CAN_NODE *node;
  ts_RCU_CAN_Obj *obj;
  struct can_frame send_frame;

  // get node pointer
  if (_ce_CanNodeNo >= e_RCU_CAN_NODE_COUNT) {
    return e_RCU_CAN_ERR_REGFAULT;
  }
  node = &gs_RCU_CAN_SysData.nodes[_ce_CanNodeNo];

  // check for valid fd
  if (node->fd < 0) {
    return e_RCU_CAN_ERR_CAN_OBJ_NOT_AVAILABLE;
  }

  // get object pointer
  if (_cu16_MsgObjNo >= e_RCU_CAN_OBJ_MAX_COUNT_PER_NODE) {
    return e_RCU_COM_ERR_PARAM;
  }
  obj = &node->objs[_cu16_MsgObjNo];

  // check obj type
  if (obj->config.e_MsgType != e_RCU_CAN_TX && obj->config.e_MsgType != e_RCU_CAN_RTR) {
    return e_RCU_CAN_ERR_WRONG_RXTX_TYPE;
  }

  // convert to can_frame
  memset(&send_frame, 0, sizeof(send_frame));
  if (_ps_Msg->e_Xtd == e_RCU_CAN_EXTENDED_ID) {
    send_frame.can_id = (_ps_Msg->u32_ID & CAN_EFF_MASK) | CAN_EFF_FLAG;
  } else {
    send_frame.can_id = _ps_Msg->u32_ID & CAN_SFF_MASK;
  }
  send_frame.can_dlc = _ps_Msg->u8_DLC;
  memcpy(send_frame.data, &_ps_Msg->u_Data, 8);

  // send packet
  if (write(node->fd, &send_frame, sizeof(send_frame)) < 0) {
    return e_RCU_CAN_ERR_TRANSACT;
  }

  return e_NO_ERROR;
}
*/

