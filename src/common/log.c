/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The logger: the one piece of global state. Lines go to stderr by default
 * or to a sink the interface installs (its status bar and log window).
 * Thread safe: a mutex serialises the formatting and the sink call, so a
 * line from a worker never interleaves with one from the main thread. */

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "cyanbox/common.h"

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static enum CBLogLevel log_max = CB_LOG_INFO;
static cb_log_sink log_sink;
static void *log_sink_opaque;

static const char *const level_names[] = { "error", "warn", "info", "debug" };

void cb_log_set_level(enum CBLogLevel level)
{
    pthread_mutex_lock(&log_lock);
    log_max = level;
    pthread_mutex_unlock(&log_lock);
}

enum CBLogLevel cb_log_level(void)
{
    return log_max;
}

void cb_log_set_sink(cb_log_sink sink, void *opaque)
{
    pthread_mutex_lock(&log_lock);
    log_sink = sink;
    log_sink_opaque = opaque;
    pthread_mutex_unlock(&log_lock);
}

void cb_vlog(enum CBLogLevel level, const char *fmt, va_list args)
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
        fprintf(stderr, "cyanbox: %s: %s\n", level_names[CB_CLAMP(level, 0, 3)], line);
    }
    pthread_mutex_unlock(&log_lock);
}

void cb_log(enum CBLogLevel level, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    cb_vlog(level, fmt, args);
    va_end(args);
}
