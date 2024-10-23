#include "utils.h"

#include <time.h>

double utl_val_limit(double val, double min, double max) {
  if (val < min) {
    return min;
  }

  if (val > max) {
    return max;
  }

  return val;
}

int64_t utl_get_ticks(void) {
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return (int64_t) tp.tv_sec * 1000ULL + ((int64_t) tp.tv_nsec / 1000000ULL);
}

void utl_update_fds(int fd, fd_set *fd_set, int *max_fd) {
  FD_SET(fd, fd_set);
  if (*max_fd < fd) {
    *max_fd = fd;
  }
}

