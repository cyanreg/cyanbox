/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A queue of reference-counted objects (libavutil/refstruct.h) between
 * threads: what a pipeline (pipeline.c) hands its chunks over in. After
 * cyanrip's fifo_template.c, by the same author and relicensed for
 * cyanbox: the same queue, limit and blocking flags, for refstruct objects
 * rather than AVFrames, with waits that check their condition again on
 * every wake-up (a spurious one returned an empty queue's first entry
 * there), a wake for a thread blocked in a call another thread wants back,
 * and an eventfd for a poll loop. */

#ifndef CYANBOX_AUDIO_FIFO_H
#define CYANBOX_AUDIO_FIFO_H

#include <stdbool.h>

typedef struct CBFifo CBFifo;

enum CBFifoFlags {
    CB_FIFO_BLOCK_MAX_OUTPUT = 1 << 0,  /* a push waits while the queue is full */
    CB_FIFO_BLOCK_NO_INPUT   = 1 << 1,  /* a pop or peek waits while it is empty */
};

/* max_queued: -1 no limit, 0 nothing is ever queued (a push drops it).
 * notify: an eventfd that each push makes readable (cb_fifo_fd). NULL
 * when out of memory or file descriptors. */
CBFifo *cb_fifo_create(int max_queued, int block_flags, bool notify);
/* Unreferences whatever is queued. No thread may be in a call. */
void cb_fifo_free(CBFifo **f);

/* Queues a new reference to obj (av_refstruct_ref): the caller keeps its
 * own. Blocks while full with CB_FIFO_BLOCK_MAX_OUTPUT, else returns
 * -ENOBUFS; a wake (cb_fifo_wake) returns -EAGAIN. */
int   cb_fifo_push(CBFifo *f, void *obj);
/* The first object, which the caller now owns (av_refstruct_unref), or
 * NULL: empty, or woken, while blocking with CB_FIFO_BLOCK_NO_INPUT. */
void *cb_fifo_pop(CBFifo *f);
/* A new reference to the first object, which stays queued, or NULL. */
void *cb_fifo_peek(CBFifo *f);

int   cb_fifo_size(CBFifo *f);
bool  cb_fifo_is_full(CBFifo *f);
void  cb_fifo_set_max_queued(CBFifo *f, int max_queued);
void  cb_fifo_set_block_flags(CBFifo *f, int block_flags);
/* Unreferences everything queued, which unblocks a push. */
void  cb_fifo_drain(CBFifo *f);
/* Returns every thread blocked in a push, pop or peek (see each). */
void  cb_fifo_wake(CBFifo *f);
/* The eventfd, readable once something was pushed; reading it clears it.
 * -1 without notify. */
int   cb_fifo_fd(const CBFifo *f);

#endif /* CYANBOX_AUDIO_FIFO_H */
