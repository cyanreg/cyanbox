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

typedef struct RXJobs RXJobs;

enum RXJobKind {
    RX_JOB_ANALYSE,     /* decode + analyse: result->analysis */
    RX_JOB_DECODE,      /* decode for a deck: result->audio */
    RX_JOB_ARTWORK,     /* cover art RGBA: result->rgba, and result->jpeg */
    RX_JOB_STICK_READ,  /* rx_stick_read + rx_stick_diff: result->stick, result->sync_items */
    RX_JOB_INSPECT,     /* rx_inspect: result->report, result->problems */
};

typedef struct RXJobResult {
    enum RXJobKind kind;
    uint64_t   track_id;
    int        deck;            /* RX_JOB_DECODE */
    uint64_t   request;         /* the id rx_jobs_* returned */
    int        err;             /* 0 or a negative errno */
    double     seconds;         /* wall time the job took */
    RXAnalysis *analysis;       /* owned by the receiver */
    RXAudio    *audio;          /* owned by the receiver */
    uint8_t    *rgba; int width, height;
    uint8_t    *jpeg; size_t jpeg_size;   /* RX_JOB_ARTWORK: the library's 240 px thumbnail
                                             (rx_image_jpeg_square), NULL if it failed */
    RXStickState *stick;        /* RX_JOB_STICK_READ: owned by the receiver (rx_stick_state_free,
                                   then free); set once the read worked, even if the diff failed */
    RXSyncItem *sync_items; int nb_sync_items;  /* RX_JOB_STICK_READ: owned by the receiver */
    char       *report;         /* RX_JOB_INSPECT: the report's lines, also on error; owned by
                                   the receiver */
    int         problems;       /* RX_JOB_INSPECT: rx_inspect's count */
} RXJobResult;

int  rx_jobs_create(RXJobs **j, int threads);
void rx_jobs_destroy(RXJobs *j);      /* cancels pending, waits for running */

/* The GPU the analyses' spectral front end runs on (rxbox/compute.h). The
 * pool makes one compute context, when its first analysis starts, on the
 * device whose name contains `device` (NULL: $RXBOX_GPU_DEVICE, else the
 * best one), shares it between its workers, and destroys it in
 * rx_jobs_destroy after the last of them. `enable` false, or
 * RXBOX_ANALYSIS_GPU=0, or no usable device, and the analyses run on the
 * CPU. An analysis whose options carry a context of their own uses that.
 * Default: enabled, no name. Call before the first rx_jobs_analyse. */
void rx_jobs_set_gpu(RXJobs *j, bool enable, const char *device);

/* Each returns a request id > 0 or a negative errno. */
uint64_t rx_jobs_analyse(RXJobs *j, uint64_t track_id, const char *path,
                         const RXAnalysisOptions *opt, unsigned what);
uint64_t rx_jobs_decode(RXJobs *j, uint64_t track_id, const char *path, int deck);
uint64_t rx_jobs_artwork(RXJobs *j, uint64_t track_id, const char *path, int max_size);
/* Reads the stick mounted at `mount` and compares it with the library at
 * `library_root`, which the worker opens as a second connection to the
 * same database (so the owner saves pending edits first). */
uint64_t rx_jobs_stick_read(RXJobs *j, const char *library_root, const char *mount);
uint64_t rx_jobs_inspect(RXJobs *j, const char *mount);
/* Drops a request not yet started; a running one finishes and is reported. */
void     rx_jobs_cancel(RXJobs *j, uint64_t request);
void     rx_jobs_cancel_all(RXJobs *j);

/* Owner thread: takes the next finished job (NULL when none). The result
 * and its payload belong to the caller; rx_job_result_free releases what
 * was not taken over. */
RXJobResult *rx_jobs_poll(RXJobs *j);
RXJobResult *rx_jobs_wait(RXJobs *j, int timeout_ms);
void         rx_job_result_free(RXJobResult *r);

/* Queue state for the status bar. */
int  rx_jobs_pending(const RXJobs *j);      /* queued (or waiting for memory) + running */
int  rx_jobs_running(const RXJobs *j);
/* The track currently being analysed on each worker, 0 when idle. */
int  rx_jobs_active_tracks(const RXJobs *j, uint64_t *ids, int max);

#endif /* RXBOX_JOBS_H */
