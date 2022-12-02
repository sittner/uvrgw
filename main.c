#include <stdint.h>

#include "can.h"
#include "mb.h"
#include "timer.h"
#include "mqtt.h"
#include "rest.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <syslog.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>

static bool exit_flag;

static void sighandler(int sig)
{
  switch (sig)
  {
    case SIGINT:
    case SIGTERM:
      exit_flag = true;
      break;
    case SIGHUP:
      break;
    default:
      syslog(LOG_DEBUG, "Unhandled signal %d", sig);
  }
}

int main(int argc, char **argv)
{
  int ret = 1;
  int err;
  struct sigaction act;

  // install signal handler
  exit_flag = false;
  memset(&act, 0, sizeof(act));
  act.sa_handler = &sighandler;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
  sigaction(SIGHUP, &act, NULL);

  if (can_startup("can0") < 0) {
    goto fail1;
  }

  if (mb_startup("/dev/ttyAMA0", 9600) < 0) {
    goto fail2;
  }

  if (timer_startup() < 0) {
    goto fail3;
  }

  if (mqtt_startup("10.0.0.2", 1883, "client123", "uvr", "K4HXOT1yKNekMV6d") < 0) {
    goto fail4;
  }

  if (rest_startup() < 0) {
    goto fail5;
  }

  while(!exit_flag) {
    fd_set read_fd_set;
    FD_ZERO(&read_fd_set);
    can_update_fds(&read_fd_set);
    timer_update_fds(&read_fd_set);

    err = select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL);
    if (err < 0) {
      if (errno == EINTR) {
        continue;
      }
      syslog(LOG_ERR, "Failed on socket select (error %d)", errno);
      goto fail6;
    }

    // handle CAN data
    if (can_handler(&read_fd_set) < 0) {
      goto fail6;
    }

    // check task timers
    if (timer_handler(&read_fd_set) < 0) {
      goto fail6;
    }

  }
    
  ret = 0;

fail6:
  rest_shutdown();
fail5:
  mqtt_shutdown();
fail4:
  timer_shutdown();
fail3:
  mb_shutdown();
fail2:
  can_shutdown();
fail1:
  return ret;
}

