/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_JOBS_H
#define CYANBOX_JOBS_H

/* Background work for the interface and the CLI: analysing tracks,
 * decoding tracks for the decks, cover art, reading sticks and tagging
 * tracks with the model, on a pool,
 * with results handed back to the owner thread through a mailbox. Jobs
 * never touch the owner's library; the owner applies results (library
 * writes, deck load). A stick read, which compares the stick with the
 * library, opens a library handle of its own.
 *
 * Analyses share a memory budget per queue (40 % of the physical memory,
 * or CYANBOX_ANALYSIS_BUDGET_MB, read by cb_jobs_create): before decoding,
 * each reserves an estimate from the file's length and waits while it does
 * not fit; one analysis may always run. A job waiting for memory is still
 * pending, not running, and can be cancelled. */

#include "cyanbox/thread.h"
#include "cyanbox/analysis.h"
#include "cyanbox/audio.h"
#include "cyanbox/export.h"
#include "cyanbox/ml.h"

typedef struct CBJobs CBJobs;

enum CBJobKind {
    CB_JOB_ANALYSE,     /* decode + analyse: result->analysis */
    CB_JOB_DECODE,      /* a track for a deck: result->packets (cb_packets_open), or
                           result->audio for a file whose packets cannot place their
                           samples (Matroska), decoded whole */
    CB_JOB_ARTWORK,     /* cover art RGBA: result->rgba, and result->jpeg */
    CB_JOB_STICK_READ,  /* cb_stick_read + cb_stick_diff: result->stick, result->sync_items */
    CB_JOB_INSPECT,     /* cb_inspect: result->report, result->problems */
    CB_JOB_TAG,         /* the model's tags of one track of the tag batch: result->tags;
                           track_id 0: the batch stopped before its tracks (the model
                           would not open), err says why */
};

typedef struct CBJobResult {
    enum CBJobKind kind;
    uint64_t   track_id;
    int        deck;            /* CB_JOB_DECODE */
    uint64_t   request;         /* the id cb_jobs_* returned */
    int        err;             /* 0 or a negative errno */
    double     seconds;         /* wall time the job took */
    CBAnalysis *analysis;       /* owned by the receiver */
    CBAudio    *audio;          /* owned by the receiver */
    CBPackets  *packets;        /* CB_JOB_DECODE: owned by the receiver */
    uint8_t    *rgba; int width, height;
    uint8_t    *jpeg; size_t jpeg_size;   /* CB_JOB_ARTWORK: the library's 240 px thumbnail
                                             (cb_image_jpeg_square), NULL if it failed */
    CBStickState *stick;        /* CB_JOB_STICK_READ: owned by the receiver (cb_stick_state_free,
                                   then free); set once the read worked, even if the diff failed */
    CBSyncItem *sync_items; int nb_sync_items;  /* CB_JOB_STICK_READ: owned by the receiver */
    char       *report;         /* CB_JOB_INSPECT: the report's lines, also on error; owned by
                                   the receiver */
    int         problems;       /* CB_JOB_INSPECT: cb_inspect's count */
    CBMLTags   *tags;           /* CB_JOB_TAG: owned by the receiver */
} CBJobResult;

int  cb_jobs_create(CBJobs **j, int threads);
void cb_jobs_destroy(CBJobs *j);      /* cancels pending, waits for running */

/* The GPU the analyses' spectral front end runs on (cyanbox/compute.h). The
 * pool makes one compute context, when its first analysis starts, on the
 * device whose name contains `device` (NULL: $CYANBOX_GPU_DEVICE, else the
 * best one), shares it between its workers, and destroys it in
 * cb_jobs_destroy after the last of them. `enable` false, or
 * CYANBOX_ANALYSIS_GPU=0, or no usable device, and the analyses run on the
 * CPU. An analysis whose options carry a context of their own uses that.
 * Default: enabled, no name. Call before the first cb_jobs_analyse. */
void cb_jobs_set_gpu(CBJobs *j, bool enable, const char *device);

/* Beat This!'s model file (cyanbox/beatthis.h; NULL: none), which the
 * analyses make the grid with (the pool is their CBAnalysisOptions.beat_net
 * unless they bring their own). The pool opens it when the first of them
 * runs, on the GPU cb_jobs_set_gpu named, shares it, one run at a time,
 * and closes it in cb_jobs_destroy; never while the tag batch holds the
 * GPU (only one model is ever open on it): analyses wait for the batch to
 * end. Without a file, or one that will not open (no GPU that can run
 * it), they fail their beats with -ENODEV (the log says why). Naming it
 * again (the same file too) forgets a failure to open it. */
void cb_jobs_set_beat_model(CBJobs *j, const char *path);

/* Each returns a request id > 0 or a negative errno. */
uint64_t cb_jobs_analyse(CBJobs *j, uint64_t track_id, const char *path,
                         const CBAnalysisOptions *opt, unsigned what);
uint64_t cb_jobs_decode(CBJobs *j, uint64_t track_id, const char *path, int deck);
uint64_t cb_jobs_artwork(CBJobs *j, uint64_t track_id, const char *path, int max_size);
/* Reads the stick mounted at `mount` and compares it with the library at
 * `library_root`, which the worker opens as a second connection to the
 * same database (so the owner saves pending edits first). */
uint64_t cb_jobs_stick_read(CBJobs *j, const char *library_root, const char *mount);
uint64_t cb_jobs_inspect(CBJobs *j, const char *mount);
/* Tagging with the model (cyanbox/ml.h), only ever on request: it holds the
 * GPU for seconds a track, and up to 7 GB of its memory while it is open.
 * cb_jobs_tag appends a track to the pool's one tag queue, with the legend
 * its colour is picked from (CB_TRACK_COLORS descriptions by colour id - 1,
 * copied; NULL: the defaults, cb_config_vibe_default), and one job
 * drains it: it opens the model (`model`, its GGUF file, on the GPU
 * cb_jobs_set_gpu named), decodes and tags the tracks one at a time in the
 * order they came, takes the tracks appended while it runs into the same
 * batch, and closes the model once the queue is empty or the batch is
 * cancelled, so there is never more than one model open. Each track posts
 * an CB_JOB_TAG result (but one cancelled half way); a model that cannot
 * be opened posts one with track_id 0 and drops the queue. A batch that
 * is running keeps the model it opened. Returns the batch's request id,
 * the same for all its tracks, or a negative errno. */
uint64_t cb_jobs_tag(CBJobs *j, const char *model, const char *const *legend, uint64_t track_id,
                     const char *path);
/* The tag batch: false when none is queued or running; else the tracks
 * done and asked for in it so far, and the one being tagged (0 while the
 * model loads). Any pointer may be NULL. */
bool     cb_jobs_tag_progress(CBJobs *j, int *done, int *total, uint64_t *current);

/* Drops a request not yet started; a running one finishes and is reported.
 * The tag batch's request is the exception: it stops the batch, the track
 * being tagged included, and empties its queue; cancel_all and destroy do
 * the same to it. */
void     cb_jobs_cancel(CBJobs *j, uint64_t request);
void     cb_jobs_cancel_all(CBJobs *j);

/* Owner thread: takes the next finished job (NULL when none). The result
 * and its payload belong to the caller; cb_job_result_free releases what
 * was not taken over. */
CBJobResult *cb_jobs_poll(CBJobs *j);
CBJobResult *cb_jobs_wait(CBJobs *j, int timeout_ms);
void         cb_job_result_free(CBJobResult *r);

/* Queue state for the status bar. */
int  cb_jobs_pending(const CBJobs *j);      /* queued (or waiting for memory) + running */
int  cb_jobs_running(const CBJobs *j);
/* The track currently being analysed on each worker, 0 when idle. */
int  cb_jobs_active_tracks(const CBJobs *j, uint64_t *ids, int max);

#endif /* CYANBOX_JOBS_H */
