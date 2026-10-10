// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file shim.c
 * @brief LD_PRELOAD shim for the test suite.
 *
 * Redirects syslog() to stderr with a monotonic timestamp in ms, so the
 * tests can follow the log.  With UVRGW_SHIM_PUB set, every
 * mosquitto_publish() is printed to the same stream ("PUB <topic>
 * <payload>") before it is forwarded to libmosquitto, so tests can check
 * the order of log messages and publishes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;

static long long now_ms(void) {
  struct timespec t;

  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void out(int pri, const char *fmt, va_list ap) {
  pthread_mutex_lock(&out_lock);
  fprintf(stderr, "[%lld] <%d> ", now_ms(), pri & 7);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  fflush(stderr);
  pthread_mutex_unlock(&out_lock);
}

void syslog(int pri, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  out(pri, fmt, ap);
  va_end(ap);
}

void __syslog_chk(int pri, int flag, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  out(pri, fmt, ap);
  va_end(ap);
}

void openlog(const char *ident, int option, int facility) {
}

int mosquitto_publish(void *mosq, int *mid, const char *topic, int len, const void *payload, int qos, bool retain) {
  static int (*real)(void *, int *, const char *, int, const void *, int, bool) = NULL;

  if (getenv("UVRGW_SHIM_PUB") != NULL) {
    pthread_mutex_lock(&out_lock);
    fprintf(stderr, "[%lld] PUB %s %.*s\n", now_ms(), topic, len, (const char *) payload);
    fflush(stderr);
    pthread_mutex_unlock(&out_lock);
  }

  if (real == NULL) {
    real = dlsym(RTLD_NEXT, "mosquitto_publish");
  }
  return real(mosq, mid, topic, len, payload, qos, retain);
}
