/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The thread pool and mailbox (src/common/thread.c): every job runs once,
 * wait and destroy drain the queue, the mailbox loses nothing under
 * several producers, and its timed wait times out. */

#include "rxbox/thread.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++;                                                          \
    }                                                                        \
} while (0)

static _Atomic int counter;
static int slots[10000];

static void count_job(void *arg)
{
    __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add((int *)arg, 1, __ATOMIC_RELAXED);
}

static void slow_job(void *arg)
{
    usleep(2000);
    __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED);
}

static void test_pool(void)
{
    RXThreadPool *pool;
    CHECK(rx_pool_create(&pool, 4) == 0);
    CHECK(rx_pool_threads(pool) == 4);
    CHECK(rx_pool_pending(pool) == 0);

    counter = 0;
    for (int i = 0; i < 10000; i++)
        CHECK(rx_pool_submit(pool, count_job, &slots[i]) == 0);
    rx_pool_wait(pool);
    CHECK(counter == 10000);
    CHECK(rx_pool_pending(pool) == 0);
    int wrong = 0;
    for (int i = 0; i < 10000; i++)
        wrong += slots[i] != 1;
    CHECK(wrong == 0);
    CHECK(rx_pool_submit(pool, NULL, NULL) == RX_ERROR(EINVAL));

    /* Pending counts queued and running jobs. */
    counter = 0;
    for (int i = 0; i < 40; i++)
        rx_pool_submit(pool, slow_job, NULL);
    CHECK(rx_pool_pending(pool) > 0);
    rx_pool_wait(pool);
    CHECK(counter == 40 && rx_pool_pending(pool) == 0);

    /* Destroy runs what is still queued before joining. */
    counter = 0;
    for (int i = 0; i < 40; i++)
        rx_pool_submit(pool, slow_job, NULL);
    rx_pool_destroy(pool);
    CHECK(counter == 40);

    /* <= 0 threads: one per CPU. */
    CHECK(rx_pool_create(&pool, 0) == 0);
    CHECK(rx_pool_threads(pool) == (int)sysconf(_SC_NPROCESSORS_ONLN));
    rx_pool_destroy(pool);
    rx_pool_destroy(NULL);
}

#define PRODUCERS 4
#define ITEMS 20000

typedef struct Producer {
    RXMailbox *mb;
    int index;
} Producer;

static void *produce(void *arg)
{
    Producer *p = arg;
    for (intptr_t i = 0; i < ITEMS; i++)
        rx_mailbox_post(p->mb, (void *)(intptr_t)(1 + p->index * ITEMS + i));
    return NULL;
}

static void *post_later(void *arg)
{
    usleep(50000);
    rx_mailbox_post(arg, (void *)(intptr_t)42);
    return NULL;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_mailbox(void)
{
    RXMailbox *mb;
    CHECK(rx_mailbox_create(&mb) == 0);
    CHECK(rx_mailbox_take(mb) == NULL);
    CHECK(rx_mailbox_post(mb, NULL) == RX_ERROR(EINVAL));

    /* FIFO from one producer. */
    for (intptr_t i = 1; i <= 100; i++)
        rx_mailbox_post(mb, (void *)i);
    bool order = true;
    for (intptr_t i = 1; i <= 100; i++)
        order &= rx_mailbox_take(mb) == (void *)i;
    CHECK(order);

    /* Several producers, one consumer: everything arrives exactly once, in
     * order per producer. */
    pthread_t th[PRODUCERS];
    Producer prod[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) {
        prod[i] = (Producer) { .mb = mb, .index = i };
        pthread_create(&th[i], NULL, produce, &prod[i]);
    }
    static unsigned char seen[PRODUCERS * ITEMS + 1];
    intptr_t last[PRODUCERS] = { 0 };
    int got = 0;
    bool in_order = true;
    while (got < PRODUCERS * ITEMS) {
        void *item = rx_mailbox_wait(mb, 1000);
        if (!item)
            break;
        intptr_t v = (intptr_t)item;
        int p = (int)((v - 1) / ITEMS);
        in_order &= v > last[p];
        last[p] = v;
        seen[v]++;
        got++;
    }
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(th[i], NULL);
    CHECK(got == PRODUCERS * ITEMS);
    CHECK(in_order);
    int dup = 0;
    for (int i = 1; i <= PRODUCERS * ITEMS; i++)
        dup += seen[i] != 1;
    CHECK(dup == 0);

    /* Timed wait: times out empty, wakes on a post. */
    int64_t t0 = now_ms();
    CHECK(rx_mailbox_wait(mb, 60) == NULL);
    int64_t waited = now_ms() - t0;
    CHECK(waited >= 55 && waited < 1000);
    CHECK(rx_mailbox_wait(mb, 0) == NULL);

    pthread_t later;
    pthread_create(&later, NULL, post_later, mb);
    CHECK(rx_mailbox_wait(mb, -1) == (void *)(intptr_t)42);
    pthread_join(later, NULL);
    rx_mailbox_destroy(mb);

    RXCancel c = { 0 };
    CHECK(!rx_cancelled(&c));
    rx_cancel(&c);
    CHECK(rx_cancelled(&c) && !rx_cancelled(NULL));
}

int main(void)
{
    test_pool();
    test_mailbox();
    if (failures) {
        fprintf(stderr, "common_thread: %d checks failed\n", failures);
        return 1;
    }
    printf("common_thread: all checks passed\n");
    return 0;
}
