// this code is based on ntpstat (c) 2001 G.Richard Keech

#include "ntp_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <error.h>
#include <unistd.h>
#include <stdbool.h>

#define NTP_PORT  123

/* This program uses an NTP mode 6 control message, which is the
   same as that used by the ntpq command.  The ntpdc command uses
   NTP mode 7, details of which are elusive.
   For details on the format of NTP control message, see
   http://www.eecis.udel.edu/~mills/database/rfc/rfc1305/rfc1305b.ps.
   This document covers NTP version 2, however the control message
   format works with ntpd version 4 and earlier.
   Section 3.2(pp 9 ff) of RFC1305b describes the data formats used by
   this program.
*/

// ------------------------------------------------------------------------

// version and mode
#define REQ_VERSION 2
#define REQ_MODE    6

// read variables command
#define REQ_OPCODE  2

// RFC-1305 NTP control message format
#pragma pack(push, 1)
typedef struct {
  // byte 1
  unsigned char mode : 3;
  unsigned char version : 3;
  unsigned char dummy : 2;

  // byte 2
  unsigned char opcode : 5;
  unsigned char more : 1;
  unsigned char error : 1;
  unsigned char response : 1;

  unsigned short sequence;

  // status 1
  unsigned char clksrc : 6;
  unsigned char leap_indicator : 2;

  // status 2
  unsigned char st_event : 4;
  unsigned char st_count : 4;

  unsigned short association_id;
  unsigned short offset;
  unsigned short count;

  char payload[468];
  char auth[96];
} NTPMSG_T;
#pragma pack(pop)

// Refer RFC-1305, Appendix B, Section 2.2.1
typedef enum {
  clksrc_unspecified = 0,
  clksrc_atomic_clock,
  clksrc_vlf_radio,
  clksrc_hf_radio,
  clksrc_uhf_radio,
  clksrc_local_net,
  clksrc_ntp_server,
  clksrc_udp_time,
  clksrc_wristwatch,
  clksrc_modem,
} CLKSRC_T;

static const char DISP[] = "rootdisp=";
static const char DELAY[] = "rootdelay=";
static const char STRATUM[] = "stratum=";
static const char POLL[] = "tc=";
static const char REFID[] = "refid=";

//-------------------------------------------------------------------------

bool ntp_check(void) {
  int sync_ok = false;           //  return code
  struct sockaddr_in sock;
  struct in_addr address;
  int sd;                        // file descriptor for socket
  fd_set fds;
  struct timeval tv;
  int n;                        // number returned from select call

  NTPMSG_T ntpmsg;

  // initialise timeout value
  tv.tv_sec = 1;
  tv.tv_usec = 0;

  // initialise file descriptor set
  FD_ZERO(&fds);

  inet_aton("127.0.0.1", &address);
  sock.sin_family = AF_INET;
  sock.sin_addr = address;
  sock.sin_port = htons(NTP_PORT);

  //----------------------------------------------------------------
  // Compose the command message

  memset(&ntpmsg, 0, sizeof(ntpmsg));
  ntpmsg.version = REQ_VERSION;
  ntpmsg.mode = REQ_MODE;
  ntpmsg.opcode = REQ_OPCODE;
  ntpmsg.sequence = htons(1);

  //---------------------------------------------------------------------
  // Send the command message
  if ((sd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
    goto fail0;
  }

  if (connect(sd,(struct sockaddr *) &sock, sizeof(sock)) < 0) {
    goto fail1;
  }

  FD_SET(sd, &fds);

  if (send(sd, &ntpmsg, sizeof(ntpmsg), 0) < 0) {
    goto fail1;
  }

  //----------------------------------------------------------------------
  // Receive the reply message
  n = select(sd + 1, &fds,(fd_set *) 0,(fd_set *) 0, &tv);
  if (n <= 0) {
    goto fail1;
  }

  if ((n = recv(sd, &ntpmsg, sizeof(ntpmsg), 0)) < 0) {
    goto fail1;
  }

  //----------------------------------------------------------------------
  // Interpret the received NTP control message
  // the message payload is an ascii string like this:
  /* version="ntpd 4.0.99k Thu Apr  5 14:21:47 EDT 2001(1)",
     processor="i686", system="Linux2.4.2-2", leap=0, stratum=3,
     precision=-17, rootdelay=205.535, rootdispersion=57.997, peer=22924,
     refid=203.21.84.4, reftime=0xbedc2243.820c282c, poll=10,
     clock=0xbedc2310.75708249, state=4, phase=0.787, frequency=19.022,
     jitter=8.992, stability=0.029 */

  // check version and mode
  if (ntpmsg.version != REQ_VERSION || ntpmsg.mode != REQ_MODE) {
    goto fail1;
  }

  // check opcode
  if (ntpmsg.opcode != REQ_OPCODE) {
    goto fail1;
  }

  // we need a response
  if (!ntpmsg.response) {
    goto fail1;
  }

  // check for error bit in reply
  if (ntpmsg.error) {
    goto fail1;
  }

  // check for unexpected more bit
  if (ntpmsg.more) {
    goto fail1;
  }

  // if the leap indicator(LI), which is the two most significant bits
  // in status byte1, are both one, then the clock is not synchronised.
  if (ntpmsg.leap_indicator == 3) {
    goto fail1;
  }

  if (ntpmsg.clksrc == clksrc_ntp_server) {
    // source of sync is another NTP server so check for the IP address
    if (strstr(ntpmsg.payload, REFID) == NULL) {
      goto fail1;
    }
  }

  // check for stratum
  if (strstr(ntpmsg.payload, STRATUM) == NULL) {
    goto fail1;
  }

  // check for accuracy
  if (strstr(ntpmsg.payload, DISP) == NULL || strstr(ntpmsg.payload, DELAY) == NULL) {
    goto fail1;
  }

  // check for poll interval
  if (strstr(ntpmsg.payload, POLL) == NULL) {
    goto fail1;
  }

  sync_ok = true;

fail1:
  close(sd);
fail0:
  return sync_ok;
}

