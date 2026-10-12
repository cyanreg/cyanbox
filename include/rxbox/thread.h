/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_THREAD_H
#define RXBOX_THREAD_H

/* A fixed thread pool and a mailbox for handing results back to the owner
 * thread. Neither is real-time safe; the player has its own lock-free
 * ring (rxbox/player.h). */

#include "rxbox/common.h"

typedef struct RxThreadPool RxThreadPool;

/* nb_threads <= 0: one per online CPU. */
int  rx_pool_create(RxThreadPool **pool, int nb_threads);
/* Waits for queued work, then joins. */
void rx_pool_destroy(RxThreadPool *pool);
int  rx_pool_submit(RxThreadPool *pool, void (*fn)(void *arg), void *arg);
/* Blocks until every submitted job has run. */
void rx_pool_wait(RxThreadPool *pool);
int  rx_pool_threads(const RxThreadPool *pool);
/* Jobs waiting or running. */
int  rx_pool_pending(const RxThreadPool *pool);

/* Multi-producer, single-consumer mailbox of pointers. */
typedef struct RxMailbox RxMailbox;
int   rx_mailbox_create(RxMailbox **mb);
void  rx_mailbox_destroy(RxMailbox *mb);
int   rx_mailbox_post(RxMailbox *mb, void *item);
/* Non-blocking; NULL when empty. */
void *rx_mailbox_take(RxMailbox *mb);
/* Blocks up to timeout_ms (-1: forever) for an item; NULL on timeout. */
void *rx_mailbox_wait(RxMailbox *mb, int timeout_ms);

/* A cancellation flag shared between the owner and a job. */
typedef struct RxCancel { _Atomic int flag; } RxCancel;
static inline bool rx_cancelled(const RxCancel *c) { return c && __atomic_load_n(&c->flag, __ATOMIC_RELAXED); }
static inline void rx_cancel(RxCancel *c) { __atomic_store_n(&c->flag, 1, __ATOMIC_RELAXED); }

#endif /* RXBOX_THREAD_H */
