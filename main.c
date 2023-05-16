#include <stdint.h>

#include "uvrgw_conf.h"
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

#define DEFAULT_CFG_FILE "/etc/uvrgw.conf"

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
  const char *cfg_file;
  int err;
  struct sigaction act;

  cfg_file = DEFAULT_CFG_FILE;
  if (argc >= 2) {
    cfg_file = argv[1];
  }

  // install signal handler
  exit_flag = false;
  memset(&act, 0, sizeof(act));
  act.sa_handler = &sighandler;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
  sigaction(SIGHUP, &act, NULL);

printf("config loading\n");
  if (uvrgw_conf_load(cfg_file) < 0) {
    goto fail_conf;
  }
printf("config ok\n");
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

  if (timer_startup() < 0) {
    goto fail_timer;
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
      goto fail_loop;
    }

    // handle CAN data
    if (can_handler(&read_fd_set) < 0) {
      goto fail_loop;
    }

    // check task timers
    if (timer_handler(&read_fd_set) < 0) {
      goto fail_loop;
    }

  }
    
  ret = 0;

fail_loop:
  timer_shutdown();
fail_timer:
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
  return ret;
}

