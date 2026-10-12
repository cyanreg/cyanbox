/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_JOBS_H
#define RXBOX_JOBS_H

/* Background work for the interface and the CLI: analysing tracks,
 * decoding tracks for the decks, cover art and reading sticks, on a pool,
 * with results handed back to the owner thread through a mailbox. Jobs
 * never touch the owner's library; the owner applies results (library
 * writes, deck load). A stick read, which compares the stick with the
 * library, opens a library handle of its own.
 *
 * Analyses share a memory budget per queue (40 % of the physical memory,
 * or RXBOX_ANALYSIS_BUDGET_MB, read by rx_jobs_create): before decoding,
 * each reserves an estimate from the file's length and waits while it does
 * not fit; one analysis may always run. A job waiting for memory is still
 * pending, not running, and can be cancelled. */

#include "rxbox/thread.h"
#include "rxbox/analysis.h"
#include "rxbox/audio.h"
#include "rxbox/export.h"

typedef struct RxJobs RxJobs;

enum RxJobKind {
    RX_JOB_ANALYSE,     /* decode + analyse: result->analysis */
    RX_JOB_DECODE,      /* decode for a deck: result->audio */
    RX_JOB_ARTWORK,     /* cover art RGBA: result->rgba, and result->jpeg */
    RX_JOB_STICK_READ,  /* rx_stick_read + rx_stick_diff: result->stick, result->sync_items */
    RX_JOB_INSPECT,     /* rx_inspect: result->report, result->problems */
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
    RxStickState *stick;        /* RX_JOB_STICK_READ: owned by the receiver (rx_stick_state_free,
                                   then free); set once the read worked, even if the diff failed */
    RxSyncItem *sync_items; int nb_sync_items;  /* RX_JOB_STICK_READ: owned by the receiver */
    char       *report;         /* RX_JOB_INSPECT: the report's lines, also on error; owned by
                                   the receiver */
    int         problems;       /* RX_JOB_INSPECT: rx_inspect's count */
} RxJobResult;

int  rx_jobs_create(RxJobs **j, int threads);
void rx_jobs_destroy(RxJobs *j);      /* cancels pending, waits for running */

/* Each returns a request id > 0 or a negative errno. */
uint64_t rx_jobs_analyse(RxJobs *j, uint64_t track_id, const char *path,
                         const RxAnalysisOptions *opt, unsigned what);
uint64_t rx_jobs_decode(RxJobs *j, uint64_t track_id, const char *path, int deck);
uint64_t rx_jobs_artwork(RxJobs *j, uint64_t track_id, const char *path, int max_size);
/* Reads the stick mounted at `mount` and compares it with the library at
 * `library_root`, which the worker opens as a second connection to the
 * same database (so the owner saves pending edits first). */
uint64_t rx_jobs_stick_read(RxJobs *j, const char *library_root, const char *mount);
uint64_t rx_jobs_inspect(RxJobs *j, const char *mount);
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
int  rx_jobs_pending(const RxJobs *j);      /* queued (or waiting for memory) + running */
int  rx_jobs_running(const RxJobs *j);
/* The track currently being analysed on each worker, 0 when idle. */
int  rx_jobs_active_tracks(const RxJobs *j, uint64_t *ids, int max);

#endif /* RXBOX_JOBS_H */
