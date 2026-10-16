/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_THREAD_H
#define CYANBOX_THREAD_H

/* A fixed thread pool and a mailbox for handing results back to the owner
 * thread. Neither is real-time safe; the player has its own lock-free
 * ring (cyanbox/player.h). */

#include "cyanbox/common.h"

typedef struct CBThreadPool CBThreadPool;

/* nb_threads <= 0: one per online CPU. */
int  cb_pool_create(CBThreadPool **pool, int nb_threads);
/* Waits for queued work, then joins. */
void cb_pool_destroy(CBThreadPool *pool);
int  cb_pool_submit(CBThreadPool *pool, void (*fn)(void *arg), void *arg);
/* Blocks until every submitted job has run. */
void cb_pool_wait(CBThreadPool *pool);
int  cb_pool_threads(const CBThreadPool *pool);
/* Jobs waiting or running. */
int  cb_pool_pending(const CBThreadPool *pool);

/* Multi-producer, single-consumer mailbox of pointers. */
typedef struct CBMailbox CBMailbox;
int   cb_mailbox_create(CBMailbox **mb);
void  cb_mailbox_destroy(CBMailbox *mb);
int   cb_mailbox_post(CBMailbox *mb, void *item);
/* Non-blocking; NULL when empty. */
void *cb_mailbox_take(CBMailbox *mb);
/* Blocks up to timeout_ms (-1: forever) for an item; NULL on timeout. */
void *cb_mailbox_wait(CBMailbox *mb, int timeout_ms);

/* A cancellation flag shared between the owner and a job. A plain int,
 * read and written only through the __atomic builtins, so the header also
 * parses as C++ (src/ui). */
typedef struct CBCancel { int flag; } CBCancel;
static inline bool cb_cancelled(const CBCancel *c) { return c && __atomic_load_n(&c->flag, __ATOMIC_RELAXED); }
static inline void cb_cancel(CBCancel *c) { __atomic_store_n(&c->flag, 1, __ATOMIC_RELAXED); }

#endif /* CYANBOX_THREAD_H */
