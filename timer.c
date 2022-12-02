#include "timer.h"
#include "mb.h"
#include "rest.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fd.h>
#include <linux/types.h>
#include <sys/timerfd.h>
#include <syslog.h>
#include <errno.h>

static int timer_fd = -1;

static int create_timer(int period_ms) {
  int fd;
  struct itimerspec timspec;

  fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (fd < 0) {
    goto fail0;
  }

  memset(&timspec, 0, sizeof(timspec));
  timspec.it_interval.tv_sec = period_ms / 1000;
  timspec.it_interval.tv_nsec = (period_ms % 1000) * 1000000;
  timspec.it_value.tv_sec = 0;
  timspec.it_value.tv_nsec = 1;

  if (timerfd_settime(fd, 0, &timspec, 0) < 0) {
    goto fail1;
  }

  return fd;

fail1:
  close(fd);
fail0:
  return -1;
}

int timer_startup(void) {
  timer_fd = create_timer(TIMER_PERIOD_MS);
  if (timer_fd < 0) {
    syslog(LOG_ERR, "unable to create periodic timer.");
    goto fail0;
  }

  return 0;

fail0:
  return -1;
}

void timer_shutdown(void) {
  close(timer_fd);
  timer_fd = -1;
}

void timer_update_fds(fd_set *fd_set) {
  FD_SET(timer_fd, fd_set);
}

int timer_handler(fd_set *fd_set) {
  uint64_t u;

  if (!FD_ISSET(timer_fd, fd_set)) {
    return 0;
  }

  if (read(timer_fd, &u, sizeof(u)) != sizeof(u)) {
    syslog(LOG_ERR, "failed to read timer event.");
    return -1;
  }

  if (mb_task() < 0) {
    return -1;
  }

  if (rest_task() < 0) {
    return -1;
  }

  return 0;
}

