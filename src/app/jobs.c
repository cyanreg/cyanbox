/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Background jobs (rxbox/jobs.h): decoding and analysing tracks, decoding
 * them for a deck, scaling their cover art, and reading and inspecting
 * sticks, on an RxThreadPool, with the results handed back to the owner
 * thread through an RxMailbox.
 *
 * Ownership and threads:
 *  - A Job is allocated by the owner when it submits, then belongs to the
 *    pool until a worker has run (or dropped) it; the worker frees it. It
 *    stays on the RxJobs list, under `lock`, from submission until the
 *    worker is done with it, which is what lets the owner cancel a job that
 *    has not started: it only flips the state, and the worker that later
 *    pops it frees it without running it.
 *  - A worker allocates the RxJobResult and its payload and posts it; from
 *    then on it belongs to whoever takes it from the mailbox (the owner),
 *    who releases it with rx_job_result_free().
 *  - Workers never see the owner's library: a job carries copies of the
 *    path and the options, nothing else. The stick read needs a library to
 *    compare with, so it opens one of its own from the root path (a second
 *    connection to the database, as the export does) and closes it again.
 *  - The counters and the per-worker active track ids are read without the
 *    lock (atomics), so the status bar can poll them from a const RxJobs.
 *
 * The analyse job decodes mono: every stage of the analysis works on the
 * mean of the channels (dj's decode_player did the same), and a mono buffer
 * halves the memory of the 11 or so decodes running at once. The decode job
 * for a deck keeps the native rate and channels, so the player gets the
 * file's own samples on the players' timeline. The artwork job also makes
 * the 240 px JPEG thumbnail the library stores, so the owner thread never
 * encodes. */

#include "rxbox/jobs.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

/* The library's artwork thumbnails (rxbox/library.h). */
#define ARTWORK_THUMB_SIZE 240

enum JobState {
    JOB_QUEUED = 0,
    JOB_RUNNING,
    JOB_CANCELLED,
};

typedef struct Job {
    RxJobs *j;
    struct Job *prev, *next;        /* on j->head, under j->lock */
    enum JobState state;            /* under j->lock */
    enum RxJobKind kind;
    uint64_t request;
    uint64_t track_id;
    char *path;                     /* the audio file, or the stick's mount */
    char *root;                     /* RX_JOB_STICK_READ: the library */
    int deck;
    int max_size;
    unsigned what;
    RxAnalysisOptions opt;
} Job;

typedef struct Slot {
    uint64_t track_id;              /* atomic; 0 when idle */
    int busy;                       /* under j->lock */
} Slot;

struct RxJobs {
    RxThreadPool *pool;
    RxMailbox *results;

    pthread_mutex_t lock;           /* guards the list, the states, the slots' busy flags */
    Job *head;
    uint64_t next_request;

    int pending;                    /* atomic: queued + running, cancelled ones excluded */
    int running;                    /* atomic */
    Slot *slots;
    int nb_slots;
};

static void job_free(Job *job)
{
    if (!job)
        return;
    free(job->path);
    free(job->root);
    free(job);
}

/* Under j->lock. */
static void job_unlink(RxJobs *j, Job *job)
{
    if (job->prev)
        job->prev->next = job->next;
    else
        j->head = job->next;
    if (job->next)
        job->next->prev = job->prev;
    job->prev = job->next = NULL;
}

static int run_analyse(Job *job, RxJobResult *r)
{
    RxAudio audio = { 0 };
    int ret = rx_decode(job->path, 1, 0, &audio);
    if (ret < 0)
        return ret;

    RxAnalysis *a = calloc(1, sizeof(*a));
    if (!a) {
        rx_audio_free(&audio);
        return RX_ERROR(ENOMEM);
    }

    ret = rx_analyse(&audio, &job->opt, job->what, a);
    /* The owner updates the track's length from this: the decode is the
     * players' timeline, which the probe only estimates for some files. */
    if (audio.rate > 0)
        a->duration = (double)audio.nb_frames / audio.rate;
    rx_audio_free(&audio);

    if (ret < 0) {
        rx_analysis_free(a);
        free(a);
        return ret;
    }

    r->analysis = a;
    return 0;
}

static int run_decode(Job *job, RxJobResult *r)
{
    RxAudio *audio = calloc(1, sizeof(*audio));
    if (!audio)
        return RX_ERROR(ENOMEM);

    int ret = rx_decode(job->path, 0, 0, audio);
    if (ret < 0) {
        free(audio);
        return ret;
    }

    r->audio = audio;
    return 0;
}

static int run_artwork(Job *job, RxJobResult *r)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    int ret = rx_artwork_rgba(job->path, &rgba, &w, &h);
    if (ret < 0)
        return ret;

    /* The library keeps a square thumbnail, the size and quality dj used
     * for the stick's larger artwork; the owner stores it. Not fatal. */
    ret = rx_image_jpeg_square(rgba, w, h, ARTWORK_THUMB_SIZE, 90, &r->jpeg, &r->jpeg_size);
    if (ret < 0) {
        rx_debug("jobs: no thumbnail for %s: %s", job->path, strerror(-ret));
        r->jpeg = NULL;
        r->jpeg_size = 0;
    }

    if (job->max_size > 0 && (w > job->max_size || h > job->max_size)) {
        uint8_t *scaled = NULL;
        int sw = 0, sh = 0;
        ret = rx_image_scale(rgba, w, h, job->max_size, job->max_size, &scaled, &sw, &sh);
        free(rgba);
        if (ret < 0)
            return ret;
        rgba = scaled;
        w = sw;
        h = sh;
    }

    r->rgba = rgba;
    r->width = w;
    r->height = h;
    return 0;
}

static int run_stick_read(Job *job, RxJobResult *r)
{
    RxLibrary lib = { 0 };
    int ret = rx_library_open(&lib, job->root);
    if (ret < 0)
        return ret;

    RxStickState *s = calloc(1, sizeof(*s));
    if (!s) {
        rx_library_close(&lib);
        return RX_ERROR(ENOMEM);
    }
    ret = rx_stick_read(&lib, job->path, s);
    if (ret < 0) {
        free(s);
    } else {
        r->stick = s;
        ret = rx_stick_diff(&lib, s, &r->sync_items, &r->nb_sync_items);
    }
    rx_library_close(&lib);
    return ret;
}

static void report_line(void *opaque, const char *line)
{
    fprintf(opaque, "%s\n", line);
}

static int run_inspect(Job *job, RxJobResult *r)
{
    size_t size = 0;
    FILE *f = open_memstream(&r->report, &size);
    if (!f)
        return RX_ERROR(ENOMEM);
    int ret = rx_inspect(job->path, report_line, f);
    if (fclose(f))
        return RX_ERROR(ENOMEM);
    if (ret < 0)
        return ret;
    r->problems = ret;
    return 0;
}

/* Runs on a pool worker. */
static void run_job(void *arg)
{
    Job *job = arg;
    RxJobs *j = job->j;

    pthread_mutex_lock(&j->lock);
    if (job->state == JOB_CANCELLED) {
        /* rx_jobs_cancel() already took it off the counters. */
        job_unlink(j, job);
        pthread_mutex_unlock(&j->lock);
        job_free(job);
        return;
    }
    job->state = JOB_RUNNING;
    int slot = -1;
    for (int i = 0; i < j->nb_slots; i++) {
        if (!j->slots[i].busy) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        j->slots[slot].busy = 1;
        if (job->kind == RX_JOB_ANALYSE)
            __atomic_store_n(&j->slots[slot].track_id, job->track_id, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&j->running, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&j->lock);

    int64_t t0 = rx_time_us();
    RxJobResult *r = calloc(1, sizeof(*r));
    if (r) {
        *r = (RxJobResult) {
            .kind     = job->kind,
            .track_id = job->track_id,
            .deck     = job->deck,
            .request  = job->request,
        };
        int ret;
        switch (job->kind) {
        case RX_JOB_ANALYSE:
            ret = run_analyse(job, r);
            break;
        case RX_JOB_DECODE:
            ret = run_decode(job, r);
            break;
        case RX_JOB_ARTWORK:
            ret = run_artwork(job, r);
            break;
        case RX_JOB_STICK_READ:
            ret = run_stick_read(job, r);
            break;
        case RX_JOB_INSPECT:
            ret = run_inspect(job, r);
            break;
        default:
            ret = RX_ERROR(EINVAL);
            break;
        }
        r->err = ret < 0 ? ret : 0;
        r->seconds = (rx_time_us() - t0) / 1e6;
    } else {
        rx_err("jobs: out of memory for the result of %s", job->path);
    }

    /* Post before the counters drop, so an owner that sees pending == 0
     * finds every result already in the mailbox. */
    if (r && rx_mailbox_post(j->results, r) < 0) {
        rx_err("jobs: cannot post the result of %s", job->path);
        rx_job_result_free(r);
    }

    pthread_mutex_lock(&j->lock);
    if (slot >= 0) {
        __atomic_store_n(&j->slots[slot].track_id, 0, __ATOMIC_RELAXED);
        j->slots[slot].busy = 0;
    }
    __atomic_sub_fetch(&j->running, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&j->pending, 1, __ATOMIC_RELAXED);
    job_unlink(j, job);
    pthread_mutex_unlock(&j->lock);

    job_free(job);
}

int rx_jobs_create(RxJobs **out, int threads)
{
    if (threads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        threads = n > 0 ? (int)n : 1;
    }

    RxJobs *j = calloc(1, sizeof(*j));
    if (!j)
        return RX_ERROR(ENOMEM);

    int ret = pthread_mutex_init(&j->lock, NULL);
    if (ret) {
        free(j);
        return RX_ERROR(ret);
    }

    j->nb_slots = threads;
    j->slots = calloc(threads, sizeof(*j->slots));
    if (!j->slots) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }

    ret = rx_mailbox_create(&j->results);
    if (ret < 0)
        goto fail;

    ret = rx_pool_create(&j->pool, threads);
    if (ret < 0)
        goto fail;

    *out = j;
    return 0;

fail:
    if (j->results)
        rx_mailbox_destroy(j->results);
    free(j->slots);
    pthread_mutex_destroy(&j->lock);
    free(j);
    return ret;
}

void rx_jobs_destroy(RxJobs *j)
{
    if (!j)
        return;

    /* Queued jobs are dropped by the workers that pop them; running ones
     * finish. The pool waits for both before joining. */
    rx_jobs_cancel_all(j);
    rx_pool_destroy(j->pool);

    RxJobResult *r;
    while ((r = rx_mailbox_take(j->results)))
        rx_job_result_free(r);
    rx_mailbox_destroy(j->results);

    pthread_mutex_destroy(&j->lock);
    free(j->slots);
    free(j);
}

static uint64_t submit(RxJobs *j, Job *job)
{
    job->j = j;

    pthread_mutex_lock(&j->lock);
    job->request = ++j->next_request;
    job->state = JOB_QUEUED;
    job->next = j->head;
    if (j->head)
        j->head->prev = job;
    j->head = job;
    __atomic_add_fetch(&j->pending, 1, __ATOMIC_RELAXED);
    uint64_t request = job->request;
    pthread_mutex_unlock(&j->lock);

    int ret = rx_pool_submit(j->pool, run_job, job);
    if (ret < 0) {
        pthread_mutex_lock(&j->lock);
        if (job->state != JOB_CANCELLED)
            __atomic_sub_fetch(&j->pending, 1, __ATOMIC_RELAXED);
        job_unlink(j, job);
        pthread_mutex_unlock(&j->lock);
        job_free(job);
        return (uint64_t)(int64_t)ret;
    }

    return request;
}

static Job *job_new(enum RxJobKind kind, uint64_t track_id, const char *path)
{
    if (!path)
        return NULL;
    Job *job = calloc(1, sizeof(*job));
    if (!job)
        return NULL;
    job->kind = kind;
    job->track_id = track_id;
    job->deck = -1;
    job->path = rx_strdup(path);
    if (!job->path) {
        free(job);
        return NULL;
    }
    return job;
}

uint64_t rx_jobs_analyse(RxJobs *j, uint64_t track_id, const char *path,
                         const RxAnalysisOptions *opt, unsigned what)
{
    if (!path || !what)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_ANALYSE, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);

    if (opt)
        job->opt = *opt;
    else
        rx_analysis_options_default(&job->opt);
    job->what = what;
    return submit(j, job);
}

uint64_t rx_jobs_decode(RxJobs *j, uint64_t track_id, const char *path, int deck)
{
    if (!path)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_DECODE, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    job->deck = deck;
    return submit(j, job);
}

uint64_t rx_jobs_artwork(RxJobs *j, uint64_t track_id, const char *path, int max_size)
{
    if (!path)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_ARTWORK, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    job->max_size = max_size;
    return submit(j, job);
}

uint64_t rx_jobs_stick_read(RxJobs *j, const char *library_root, const char *mount)
{
    if (!library_root || !mount)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_STICK_READ, 0, mount);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    job->root = rx_strdup(library_root);
    if (!job->root) {
        job_free(job);
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    }
    return submit(j, job);
}

uint64_t rx_jobs_inspect(RxJobs *j, const char *mount)
{
    if (!mount)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_INSPECT, 0, mount);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    return submit(j, job);
}

void rx_jobs_cancel(RxJobs *j, uint64_t request)
{
    if (!j || !request)
        return;

    pthread_mutex_lock(&j->lock);
    for (Job *job = j->head; job; job = job->next) {
        if (job->request != request)
            continue;
        if (job->state == JOB_QUEUED) {
            job->state = JOB_CANCELLED;
            __atomic_sub_fetch(&j->pending, 1, __ATOMIC_RELAXED);
        }
        break;
    }
    pthread_mutex_unlock(&j->lock);
}

void rx_jobs_cancel_all(RxJobs *j)
{
    if (!j)
        return;

    pthread_mutex_lock(&j->lock);
    for (Job *job = j->head; job; job = job->next) {
        if (job->state == JOB_QUEUED) {
            job->state = JOB_CANCELLED;
            __atomic_sub_fetch(&j->pending, 1, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_unlock(&j->lock);
}

RxJobResult *rx_jobs_poll(RxJobs *j)
{
    return j ? rx_mailbox_take(j->results) : NULL;
}

RxJobResult *rx_jobs_wait(RxJobs *j, int timeout_ms)
{
    return j ? rx_mailbox_wait(j->results, timeout_ms) : NULL;
}

void rx_job_result_free(RxJobResult *r)
{
    if (!r)
        return;
    if (r->analysis) {
        rx_analysis_free(r->analysis);
        free(r->analysis);
    }
    if (r->audio) {
        rx_audio_free(r->audio);
        free(r->audio);
    }
    free(r->rgba);
    free(r->jpeg);
    if (r->stick) {
        rx_stick_state_free(r->stick);
        free(r->stick);
    }
    free(r->sync_items);
    free(r->report);
    free(r);
}

int rx_jobs_pending(const RxJobs *j)
{
    return j ? __atomic_load_n(&j->pending, __ATOMIC_RELAXED) : 0;
}

int rx_jobs_running(const RxJobs *j)
{
    return j ? __atomic_load_n(&j->running, __ATOMIC_RELAXED) : 0;
}

int rx_jobs_active_tracks(const RxJobs *j, uint64_t *ids, int max)
{
    if (!j)
        return 0;
    int nb = 0;
    for (int i = 0; i < j->nb_slots && nb < max; i++) {
        uint64_t id = __atomic_load_n(&j->slots[i].track_id, __ATOMIC_RELAXED);
        if (id)
            ids[nb++] = id;
    }
    return nb;
}
