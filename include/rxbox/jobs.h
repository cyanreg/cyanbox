/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_JOBS_H
#define RXBOX_JOBS_H

/* Background work for the interface and the CLI: analysing tracks,
 * decoding tracks for the decks, cover art, reading sticks and tagging
 * tracks with the model, on a pool,
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
#include "rxbox/ml.h"

typedef struct RXJobs RXJobs;

enum RXJobKind {
    RX_JOB_ANALYSE,     /* decode + analyse: result->analysis */
    RX_JOB_DECODE,      /* a track for a deck: result->packets (rx_packets_open), or
                           result->audio for a file whose packets cannot place their
                           samples (Matroska), decoded whole */
    RX_JOB_ARTWORK,     /* cover art RGBA: result->rgba, and result->jpeg */
    RX_JOB_STICK_READ,  /* rx_stick_read + rx_stick_diff: result->stick, result->sync_items */
    RX_JOB_INSPECT,     /* rx_inspect: result->report, result->problems */
    RX_JOB_TAG,         /* the model's tags of one track of the tag batch: result->tags;
                           track_id 0: the batch stopped before its tracks (the model
                           would not open), err says why */
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
    RXPackets  *packets;        /* RX_JOB_DECODE: owned by the receiver */
    uint8_t    *rgba; int width, height;
    uint8_t    *jpeg; size_t jpeg_size;   /* RX_JOB_ARTWORK: the library's 240 px thumbnail
                                             (rx_image_jpeg_square), NULL if it failed */
    RXStickState *stick;        /* RX_JOB_STICK_READ: owned by the receiver (rx_stick_state_free,
                                   then free); set once the read worked, even if the diff failed */
    RXSyncItem *sync_items; int nb_sync_items;  /* RX_JOB_STICK_READ: owned by the receiver */
    char       *report;         /* RX_JOB_INSPECT: the report's lines, also on error; owned by
                                   the receiver */
    int         problems;       /* RX_JOB_INSPECT: rx_inspect's count */
    RXMLTags   *tags;           /* RX_JOB_TAG: owned by the receiver */
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
/* Tagging with the model (rxbox/ml.h), only ever on request: it holds the
 * GPU for seconds a track, and up to 7 GB of its memory while it is open.
 * rx_jobs_tag appends a track to the pool's one tag queue, and one job
 * drains it: it opens the model (`model`, its GGUF file, on the GPU
 * rx_jobs_set_gpu named), decodes and tags the tracks one at a time in the
 * order they came, takes the tracks appended while it runs into the same
 * batch, and closes the model once the queue is empty or the batch is
 * cancelled, so there is never more than one model open. Each track posts
 * an RX_JOB_TAG result (but one cancelled half way); a model that cannot
 * be opened posts one with track_id 0 and drops the queue. A batch that
 * is running keeps the model it opened. Returns the batch's request id,
 * the same for all its tracks, or a negative errno. */
uint64_t rx_jobs_tag(RXJobs *j, const char *model, uint64_t track_id, const char *path);
/* The tag batch: false when none is queued or running; else the tracks
 * done and asked for in it so far, and the one being tagged (0 while the
 * model loads). Any pointer may be NULL. */
bool     rx_jobs_tag_progress(RXJobs *j, int *done, int *total, uint64_t *current);

/* Drops a request not yet started; a running one finishes and is reported.
 * The tag batch's request is the exception: it stops the batch, the track
 * being tagged included, and empties its queue; cancel_all and destroy do
 * the same to it. */
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
