#include <stdint.h>

#include "uvrgw_conf.h"
#include "utils.h"
#include "can.h"
#include "mb.h"
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
#include <sys/eventfd.h>

#define DEFAULT_CFG_FILE "/etc/uvrgw.conf"

static int exit_fd;

static void sighandler(int sig) {
  uint64_t u = 1;

  switch (sig) {
    case SIGINT:
    case SIGTERM:
      write(exit_fd, &u, sizeof(u));
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
  const char *cfg_file;
  int err;
  struct sigaction act;
  uint64_t u;
  fd_set read_fd_set;
  int max_fd;

  cfg_file = DEFAULT_CFG_FILE;
  if (argc >= 2) {
    cfg_file = argv[1];
  }

  exit_fd = eventfd(0, 0);
  if (exit_fd < 0) {
    syslog(LOG_ERR, "unable to create exit event fd.");
    goto fail_exit_fd;
  }

  // install signal handler
  memset(&act, 0, sizeof(act));
  act.sa_handler = &sighandler;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
  sigaction(SIGHUP, &act, NULL);

  if (uvrgw_conf_load(cfg_file) < 0) {
    goto fail_conf;
  }

  if (can_startup() < 0) {
    goto fail_can;
  }

  if (mb_startup() < 0) {
    goto fail_mb;
  }

  if (mqtt_startup() < 0) {
    goto fail_mqtt;
  }

  if (rest_startup() < 0) {
    goto fail_rest;
  }

  while(true) {
    max_fd = 0;
    FD_ZERO(&read_fd_set);

    utl_update_fds(exit_fd, &read_fd_set, &max_fd);
    can_update_fds(&read_fd_set, &max_fd);

    err = select(max_fd + 1, &read_fd_set, NULL, NULL, NULL);
    if (err < 0) {
      if (errno == EINTR) {
        continue;
      }
      syslog(LOG_ERR, "Failed on socket select (error %d)", errno);
      goto fail_loop;
    }

    // handle exit fd
    if (FD_ISSET(exit_fd, &read_fd_set)) {
      read(exit_fd, &u, sizeof(u));
      break;
    }

    // handle CAN data
    if (can_handler(&read_fd_set) < 0) {
      goto fail_loop;
    }
  }
    
  ret = 0;

fail_loop:
  rest_shutdown();
fail_rest:
  mqtt_shutdown();
fail_mqtt:
  mb_shutdown();
fail_mb:
  can_shutdown();
fail_can:
  uvrgw_conf_cleanup();
fail_conf:
  close(exit_fd);
fail_exit_fd:
  return ret;
}

