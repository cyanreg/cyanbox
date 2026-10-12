/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_JOBS_H
#define RXBOX_JOBS_H

/* Background work for the interface and the CLI: analysing tracks and
 * decoding tracks for the decks, on a pool, with results handed back to
 * the owner thread through a mailbox. Jobs never touch the library; the
 * owner applies results (library writes, deck load). */

#include "rxbox/thread.h"
#include "rxbox/analysis.h"
#include "rxbox/audio.h"

typedef struct RxJobs RxJobs;

enum RxJobKind {
    RX_JOB_ANALYSE,     /* decode + analyse: result->analysis */
    RX_JOB_DECODE,      /* decode for a deck: result->audio */
    RX_JOB_ARTWORK,     /* cover art RGBA: result->rgba, and result->jpeg */
};

typedef struct RxJobResult {
    enum RxJobKind kind;
    uint64_t   track_id;
    int        deck;            /* RX_JOB_DECODE */
    uint64_t   request;         /* the id rx_jobs_* returned */
    int        err;             /* 0 or a negative errno */
    double     seconds;         /* wall time the job took */
    RxAnalysis *analysis;       /* owned by the receiver */
    RxAudio    *audio;          /* owned by the receiver */
    uint8_t    *rgba; int width, height;
    uint8_t    *jpeg; size_t jpeg_size;   /* RX_JOB_ARTWORK: the library's 240 px thumbnail
                                             (rx_image_jpeg_square), NULL if it failed */
} RxJobResult;

int  rx_jobs_create(RxJobs **j, int threads);
void rx_jobs_destroy(RxJobs *j);      /* cancels pending, waits for running */

/* Each returns a request id > 0 or a negative errno. */
uint64_t rx_jobs_analyse(RxJobs *j, uint64_t track_id, const char *path,
                         const RxAnalysisOptions *opt, unsigned what);
uint64_t rx_jobs_decode(RxJobs *j, uint64_t track_id, const char *path, int deck);
uint64_t rx_jobs_artwork(RxJobs *j, uint64_t track_id, const char *path, int max_size);
/* Drops a request not yet started; a running one finishes and is reported. */
void     rx_jobs_cancel(RxJobs *j, uint64_t request);
void     rx_jobs_cancel_all(RxJobs *j);

/* Owner thread: takes the next finished job (NULL when none). The result
 * and its payload belong to the caller; rx_job_result_free releases what
 * was not taken over. */
RxJobResult *rx_jobs_poll(RxJobs *j);
RxJobResult *rx_jobs_wait(RxJobs *j, int timeout_ms);
void         rx_job_result_free(RxJobResult *r);

/* Queue state for the status bar. */
int  rx_jobs_pending(const RxJobs *j);      /* queued + running */
int  rx_jobs_running(const RxJobs *j);
/* The track currently being analysed on each worker, 0 when idle. */
int  rx_jobs_active_tracks(const RxJobs *j, uint64_t *ids, int max);

#endif /* RXBOX_JOBS_H */
