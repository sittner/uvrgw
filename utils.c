// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file utils.c
 * @brief Utility function implementations for uvrgw.
 *
 * Implements the helpers declared in utils.h: value clamping,
 * a monotonic millisecond clock, the clock synchronisation check and
 * fd_set helpers.
 */
#include "utils.h"

#include <string.h>
#include <time.h>
#include <sys/timex.h>

/**
 * @brief Clamp @p val to [@p min, @p max].
 */
double utl_val_limit(double val, double min, double max) {
  if (val < min) {
    return min;
  }

  if (val > max) {
    return max;
  }

  return val;
}

/**
 * @brief Return a monotonic timestamp in milliseconds.
 */
int64_t utl_get_ticks(void) {
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return (int64_t) tp.tv_sec * 1000ULL + ((int64_t) tp.tv_nsec / 1000000ULL);
}

/**
 * @brief Check whether the system clock is synchronised (kernel time status).
 */
bool utl_clock_synced(void) {
  struct timex tx;

  memset(&tx, 0, sizeof(tx));
  return adjtimex(&tx) != TIME_ERROR && (tx.status & STA_UNSYNC) == 0;
}

/**
 * @brief Add @p fd to @p fd_set and update @p max_fd.
 */
void utl_update_fds(int fd, fd_set *fd_set, int *max_fd) {
  FD_SET(fd, fd_set);
  if (*max_fd < fd) {
    *max_fd = fd;
  }
}

