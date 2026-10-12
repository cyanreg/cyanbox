/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The logger: the one piece of global state. Lines go to stderr by default
 * or to a sink the interface installs (its status bar and log window).
 * Thread safe: a mutex serialises the formatting and the sink call, so a
 * line from a worker never interleaves with one from the main thread. */

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "rxbox/common.h"

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static enum RxLogLevel log_max = RX_LOG_INFO;
static rx_log_sink log_sink;
static void *log_sink_opaque;

static const char *const level_names[] = { "error", "warn", "info", "debug" };

void rx_log_set_level(enum RxLogLevel level)
{
    pthread_mutex_lock(&log_lock);
    log_max = level;
    pthread_mutex_unlock(&log_lock);
}

enum RxLogLevel rx_log_level(void)
{
    return log_max;
}

void rx_log_set_sink(rx_log_sink sink, void *opaque)
{
    pthread_mutex_lock(&log_lock);
    log_sink = sink;
    log_sink_opaque = opaque;
    pthread_mutex_unlock(&log_lock);
}

void rx_vlog(enum RxLogLevel level, const char *fmt, va_list args)
{
    char line[2048];

    if (level > log_max)
        return;

    vsnprintf(line, sizeof(line), fmt, args);

    /* Drop one trailing newline so callers may write either style. */
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n')
        line[n - 1] = '\0';

    pthread_mutex_lock(&log_lock);
    if (log_sink) {
        log_sink(log_sink_opaque, level, line);
    } else {
        fprintf(stderr, "rxbox: %s: %s\n", level_names[RX_CLAMP(level, 0, 3)], line);
    }
    pthread_mutex_unlock(&log_lock);
}

void rx_log(enum RxLogLevel level, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    rx_vlog(level, fmt, args);
    va_end(args);
}
