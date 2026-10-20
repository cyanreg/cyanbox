/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The null backend: the same mix callback as PipeWire, with no device.
 *
 * Two modes. With period >= 0 a thread calls the callback every
 * period/rate seconds on an absolute monotonic schedule and throws the
 * audio away: the clock runs as it would on a device, for headless runs.
 * With period < 0 (manual mode) there is no thread at all and the
 * callback only runs, -period frames at a time, inside
 * cb_player_null_advance() on the owner thread; that keeps the ring's
 * single consumer single and makes tests exact and repeatable. */

#include "player_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "cyanbox/thread.h"

struct CBNullOut {
    pthread_t   thread;
    bool        started;
    atomic_bool stop;
    float      *buf;
    int         period;
};

static void *null_thread(void *arg)
{
    CBPlayer *p = arg;
    cb_thread_name("player:null");
    CBNullOut *n = p->null;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int64_t rem = 0;

    while (!atomic_load_explicit(&n->stop, memory_order_relaxed)) {
        cb_player_mix(p, n->buf, n->period);

        /* Next deadline: period / rate seconds later, the remainder
         * carried so the schedule does not drift. */
        int rate = cb_player_rate(p);
        int64_t ns = (int64_t)n->period * 1000000000 + rem;
        rem = ns % rate;
        next.tv_nsec += ns / rate;
        while (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }

        /* After a long stall (a suspended machine) restart the schedule
         * rather than run a burst of callbacks to catch up. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1)
            next = now;

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

int cb_player_null_open(CBPlayer *p)
{
    CBNullOut *n = calloc(1, sizeof(*n));
    if (!n)
        return CB_ERROR(ENOMEM);
    p->null = n;
    n->period = p->period;
    atomic_init(&n->stop, false);
    n->buf = malloc(sizeof(*n->buf) * 2 * n->period);
    if (!n->buf)
        return CB_ERROR(ENOMEM);

    if (!p->manual) {
        int err = pthread_create(&n->thread, NULL, null_thread, p);
        if (err)
            return CB_ERROR(err);
        n->started = true;
    }

    atomic_store_explicit(&p->out_state, CB_OUT_RUNNING, memory_order_release);
    return 0;
}

void cb_player_null_close(CBPlayer *p)
{
    CBNullOut *n = p->null;
    if (!n)
        return;
    if (n->started) {
        atomic_store_explicit(&n->stop, true, memory_order_relaxed);
        pthread_join(n->thread, NULL);
    }
    free(n->buf);
    free(n);
    p->null = NULL;
}

void cb_player_null_run(CBPlayer *p, int frames)
{
    CBNullOut *n = p->null;
    if (!n)
        return;
    while (frames > 0) {
        int len = CB_MIN(frames, n->period);
        cb_player_mix(p, n->buf, len);
        frames -= len;
    }
}
