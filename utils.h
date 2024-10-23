#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdint.h>
#include <sys/select.h>

double utl_val_limit(double val, double min, double max);
int64_t utl_get_ticks(void);
void utl_update_fds(int fd, fd_set *fd_set, int *max_fd);

#endif

