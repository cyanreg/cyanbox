/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The queue of fifo.h, after cyanrip's fifo_template.c: an array of
 * object pointers behind a mutex, one condition for "something was
 * queued" and one for "something left", and a wake count, which a blocked
 * call compares against the count it started with. */

#include "fifo.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <libavutil/refstruct.h>

#include "cyanbox/common.h"

struct CBFifo {
    void   **queued;
    int      num_queued, cap;
    int      max_queued;
    int      block_flags;
    uint64_t wakes;
    int      fd;
    pthread_mutex_t lock;
    pthread_cond_t  cond_in;    /* something was queued */
    pthread_cond_t  cond_out;   /* something left */
};

CBFifo *cb_fifo_create(int max_queued, int block_flags, bool notify)
{
    CBFifo *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->max_queued = max_queued;
    f->block_flags = block_flags;
    f->fd = notify ? eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC) : -1;
    if (notify && f->fd < 0) {
        free(f);
        return NULL;
    }
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->cond_in, NULL);
    pthread_cond_init(&f->cond_out, NULL);
    return f;
}

static void drain_locked(CBFifo *f)
{
    for (int i = 0; i < f->num_queued; i++)
        av_refstruct_unref(&f->queued[i]);
    f->num_queued = 0;
    pthread_cond_broadcast(&f->cond_out);
}

void cb_fifo_free(CBFifo **pf)
{
    CBFifo *f = *pf;
    if (!f)
        return;
    drain_locked(f);
    free(f->queued);
    if (f->fd >= 0)
        close(f->fd);
    pthread_cond_destroy(&f->cond_in);
    pthread_cond_destroy(&f->cond_out);
    pthread_mutex_destroy(&f->lock);
    free(f);
    *pf = NULL;
}

static bool full(const CBFifo *f)
{
    return f->max_queued >= 0 && f->num_queued >= f->max_queued;
}

int cb_fifo_push(CBFifo *f, void *obj)
{
    pthread_mutex_lock(&f->lock);
    int ret = 0;
    if (!f->max_queued)
        goto unlock;
    uint64_t wakes = f->wakes;
    while (full(f)) {
        if (!(f->block_flags & CB_FIFO_BLOCK_MAX_OUTPUT)) {
            ret = CB_ERROR(ENOBUFS);
            goto unlock;
        }
        if (f->wakes != wakes) {
            ret = CB_ERROR(EAGAIN);
            goto unlock;
        }
        pthread_cond_wait(&f->cond_out, &f->lock);
    }
    if (f->num_queued == f->cap) {
        int cap = f->cap ? f->cap * 2 : 16;
        void **q = realloc(f->queued, (size_t)cap * sizeof(*q));
        if (!q) {
            ret = CB_ERROR(ENOMEM);
            goto unlock;
        }
        f->queued = q;
        f->cap = cap;
    }
    f->queued[f->num_queued++] = av_refstruct_ref(obj);
    pthread_cond_signal(&f->cond_in);
    if (f->fd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(f->fd, &one, sizeof(one));
        (void)w;
    }

unlock:
    pthread_mutex_unlock(&f->lock);
    return ret;
}

/* Waits for an object when the flags say so; true when there is one. */
static bool wait_input(CBFifo *f)
{
    uint64_t wakes = f->wakes;
    while (!f->num_queued) {
        if (!(f->block_flags & CB_FIFO_BLOCK_NO_INPUT) || f->wakes != wakes)
            return false;
        pthread_cond_wait(&f->cond_in, &f->lock);
    }
    return true;
}

void *cb_fifo_pop(CBFifo *f)
{
    pthread_mutex_lock(&f->lock);
    void *out = NULL;
    if (wait_input(f)) {
        out = f->queued[0];
        f->num_queued--;
        memmove(&f->queued[0], &f->queued[1], (size_t)f->num_queued * sizeof(*f->queued));
        pthread_cond_signal(&f->cond_out);
    }
    pthread_mutex_unlock(&f->lock);
    return out;
}

void *cb_fifo_peek(CBFifo *f)
{
    pthread_mutex_lock(&f->lock);
    void *out = wait_input(f) ? av_refstruct_ref(f->queued[0]) : NULL;
    pthread_mutex_unlock(&f->lock);
    return out;
}

int cb_fifo_size(CBFifo *f)
{
    pthread_mutex_lock(&f->lock);
    int n = f->num_queued;
    pthread_mutex_unlock(&f->lock);
    return n;
}

bool cb_fifo_is_full(CBFifo *f)
{
    pthread_mutex_lock(&f->lock);
    bool ret = !f->max_queued || full(f);
    pthread_mutex_unlock(&f->lock);
    return ret;
}

void cb_fifo_set_max_queued(CBFifo *f, int max_queued)
{
    pthread_mutex_lock(&f->lock);
    f->max_queued = max_queued;
    pthread_cond_broadcast(&f->cond_out);
    pthread_mutex_unlock(&f->lock);
}

void cb_fifo_set_block_flags(CBFifo *f, int block_flags)
{
    pthread_mutex_lock(&f->lock);
    f->block_flags = block_flags;
    pthread_cond_broadcast(&f->cond_in);
    pthread_cond_broadcast(&f->cond_out);
    pthread_mutex_unlock(&f->lock);
}

void cb_fifo_drain(CBFifo *f)
{
    pthread_mutex_lock(&f->lock);
    drain_locked(f);
    pthread_mutex_unlock(&f->lock);
}

void cb_fifo_wake(CBFifo *f)
{
    pthread_mutex_lock(&f->lock);
    f->wakes++;
    pthread_cond_broadcast(&f->cond_in);
    pthread_cond_broadcast(&f->cond_out);
    pthread_mutex_unlock(&f->lock);
}

int cb_fifo_fd(const CBFifo *f)
{
    return f->fd;
}
