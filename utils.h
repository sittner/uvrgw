/**
 * @file utils.h
 * @brief Utility functions used throughout uvrgw.
 *
 * Provides helpers for value clamping, a monotonic millisecond clock,
 * and fd_set management used by the main select() event loop.
 */

#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdint.h>
#include <sys/select.h>

/**
 * @brief Clamp a double value to the closed interval [min, max].
 *
 * @param val  Value to clamp.
 * @param min  Lower bound (inclusive).
 * @param max  Upper bound (inclusive).
 * @return     @p val clamped to [@p min, @p max].
 */
double utl_val_limit(double val, double min, double max);

/**
 * @brief Return a monotonic timestamp in milliseconds.
 *
 * Uses CLOCK_MONOTONIC so the result is unaffected by wall-clock adjustments.
 *
 * @return Monotonic time in milliseconds.
 */
int64_t utl_get_ticks(void);

/**
 * @brief Add a file descriptor to an fd_set and update the maximum fd counter.
 *
 * Convenience wrapper around FD_SET that also keeps @p max_fd up to date,
 * as required by select().
 *
 * @param fd      File descriptor to add.
 * @param fd_set  The fd_set to modify.
 * @param max_fd  Pointer to the current maximum fd value; updated if @p fd is larger.
 */
void utl_update_fds(int fd, fd_set *fd_set, int *max_fd);

#endif

