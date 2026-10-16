/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The fixed thread pool and the mailbox of cyanbox/thread.h, on pthreads.
 *
 * The pool is a FIFO of jobs behind one mutex and two condition variables:
 * `work` wakes a worker when a job arrives (or the pool shuts down) and
 * `idle` wakes cb_pool_wait() when the last job has finished. `pending`
 * counts queued plus running jobs; it is changed under the mutex but kept
 * atomic so the const accessor can read it without locking. Jobs are
 * allocated per submission: nothing here is on a real-time path.
 *
 * The mailbox is a growable ring of pointers. Its timed wait runs on
 * CLOCK_MONOTONIC so a wall-clock change never stretches a timeout. */

#include "cyanbox/thread.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ pool */

typedef struct CBJob {
    void (*fn)(void *arg);
    void *arg;
    struct CBJob *next;
} CBJob;

struct CBThreadPool {
    pthread_mutex_t lock;
    pthread_cond_t work;
    pthread_cond_t idle;
    CBJob *head, *tail;
    _Atomic int pending;
    bool quit;
    int nb_threads;
    pthread_t *threads;
};

static void *pool_worker(void *opaque)
{
    CBThreadPool *pool = opaque;

    pthread_mutex_lock(&pool->lock);
    while (1) {
        while (!pool->head && !pool->quit)
            pthread_cond_wait(&pool->work, &pool->lock);
        /* Shutting down only once the queue is empty: destroy drains it. */
        if (!pool->head)
            break;

        CBJob *job = pool->head;
        pool->head = job->next;
        if (!pool->head)
            pool->tail = NULL;
        pthread_mutex_unlock(&pool->lock);

        job->fn(job->arg);
        free(job);

        pthread_mutex_lock(&pool->lock);
        if (--pool->pending == 0)
            pthread_cond_broadcast(&pool->idle);
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

int cb_pool_create(CBThreadPool **out, int nb_threads)
{
    *out = NULL;
    if (nb_threads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nb_threads = n > 0 ? (int)CB_MIN(n, 1024) : 1;
    }

    CBThreadPool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return CB_ERROR(ENOMEM);
    pool->threads = calloc((size_t)nb_threads, sizeof(*pool->threads));
    if (!pool->threads) {
        free(pool);
        return CB_ERROR(ENOMEM);
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->work, NULL);
    pthread_cond_init(&pool->idle, NULL);

    int ret = 0;
    for (int i = 0; i < nb_threads; i++) {
        ret = pthread_create(&pool->threads[i], NULL, pool_worker, pool);
        if (ret) {
            ret = CB_ERROR(ret);
            break;
        }
        pool->nb_threads++;
    }

    if (ret < 0) {
        cb_pool_destroy(pool);
        return ret;
    }

    *out = pool;
    return 0;
}

void cb_pool_destroy(CBThreadPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->quit = true;
    pthread_cond_broadcast(&pool->work);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->nb_threads; i++)
        pthread_join(pool->threads[i], NULL);

    /* Only reachable when no worker could be started. */
    while (pool->head) {
        CBJob *job = pool->head;
        pool->head = job->next;
        free(job);
    }

    pthread_cond_destroy(&pool->idle);
    pthread_cond_destroy(&pool->work);
    pthread_mutex_destroy(&pool->lock);
    free(pool->threads);
    free(pool);
}

int cb_pool_submit(CBThreadPool *pool, void (*fn)(void *arg), void *arg)
{
    if (!pool || !fn)
        return CB_ERROR(EINVAL);

    CBJob *job = malloc(sizeof(*job));
    if (!job)
        return CB_ERROR(ENOMEM);
    *job = (CBJob) {
        .fn  = fn,
        .arg = arg,
    };

    pthread_mutex_lock(&pool->lock);
    if (pool->tail)
        pool->tail->next = job;
    else
        pool->head = job;
    pool->tail = job;
    pool->pending++;
    pthread_cond_signal(&pool->work);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void cb_pool_wait(CBThreadPool *pool)
{
    if (!pool)
        return;
    pthread_mutex_lock(&pool->lock);
    while (pool->pending > 0)
        pthread_cond_wait(&pool->idle, &pool->lock);
    pthread_mutex_unlock(&pool->lock);
}

int cb_pool_threads(const CBThreadPool *pool)
{
    return pool ? pool->nb_threads : 0;
}

int cb_pool_pending(const CBThreadPool *pool)
{
    return pool ? __atomic_load_n(&pool->pending, __ATOMIC_ACQUIRE) : 0;
}

/* --------------------------------------------------------------- mailbox */

struct CBMailbox {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    void **items;
    size_t head, nb, cap;
};

int cb_mailbox_create(CBMailbox **out)
{
    *out = NULL;
    CBMailbox *mb = calloc(1, sizeof(*mb));
    if (!mb)
        return CB_ERROR(ENOMEM);

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&mb->cond, &attr);
    pthread_condattr_destroy(&attr);
    pthread_mutex_init(&mb->lock, NULL);

    *out = mb;
    return 0;
}

void cb_mailbox_destroy(CBMailbox *mb)
{
    if (!mb)
        return;
    pthread_cond_destroy(&mb->cond);
    pthread_mutex_destroy(&mb->lock);
    free(mb->items);
    free(mb);
}

/* Called with the lock held. Unwraps the ring into a larger buffer. */
static int mailbox_grow(CBMailbox *mb)
{
    size_t cap = mb->cap ? mb->cap * 2 : 16;
    void **items = malloc(cap * sizeof(*items));
    if (!items)
        return CB_ERROR(ENOMEM);
    for (size_t i = 0; i < mb->nb; i++)
        items[i] = mb->items[(mb->head + i) % mb->cap];
    free(mb->items);
    mb->items = items;
    mb->head = 0;
    mb->cap = cap;
    return 0;
}

int cb_mailbox_post(CBMailbox *mb, void *item)
{
    /* NULL is what take/wait return for "nothing", so it cannot be an item. */
    if (!mb || !item)
        return CB_ERROR(EINVAL);

    int ret = 0;
    pthread_mutex_lock(&mb->lock);
    if (mb->nb == mb->cap)
        ret = mailbox_grow(mb);
    if (ret >= 0) {
        mb->items[(mb->head + mb->nb) % mb->cap] = item;
        mb->nb++;
        pthread_cond_signal(&mb->cond);
    }
    pthread_mutex_unlock(&mb->lock);
    return ret;
}

/* Called with the lock held. */
static void *mailbox_pop(CBMailbox *mb)
{
    if (!mb->nb)
        return NULL;
    void *item = mb->items[mb->head];
    mb->head = (mb->head + 1) % mb->cap;
    mb->nb--;
    return item;
}

void *cb_mailbox_take(CBMailbox *mb)
{
    if (!mb)
        return NULL;
    pthread_mutex_lock(&mb->lock);
    void *item = mailbox_pop(mb);
    pthread_mutex_unlock(&mb->lock);
    return item;
}

void *cb_mailbox_wait(CBMailbox *mb, int timeout_ms)
{
    if (!mb)
        return NULL;

    struct timespec deadline;
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    pthread_mutex_lock(&mb->lock);
    while (!mb->nb) {
        if (timeout_ms < 0)
            pthread_cond_wait(&mb->cond, &mb->lock);
        else if (pthread_cond_timedwait(&mb->cond, &mb->lock, &deadline) == ETIMEDOUT)
            break;
    }
    void *item = mailbox_pop(mb);
    pthread_mutex_unlock(&mb->lock);
    return item;
}
