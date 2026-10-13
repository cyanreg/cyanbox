/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Background jobs (rxbox/jobs.h): decoding and analysing tracks, decoding
 * them for a deck, scaling their cover art, reading and inspecting
 * sticks, and tagging tracks with the model, on an RXThreadPool, with the
 * results handed back to the owner thread through an RXMailbox.
 *
 * Ownership and threads:
 *  - A Job is allocated by the owner when it submits, then belongs to the
 *    pool until a worker has run (or dropped) it; the worker frees it. It
 *    stays on the RXJobs list, under `lock`, from submission until the
 *    worker is done with it, which is what lets the owner cancel a job that
 *    has not started: it only flips the state, and the worker that later
 *    pops it frees it without running it.
 *  - A worker allocates the RXJobResult and its payload and posts it; from
 *    then on it belongs to whoever takes it from the mailbox (the owner),
 *    who releases it with rx_job_result_free().
 *  - Workers never see the owner's library: a job carries copies of the
 *    path and the options, nothing else. The stick read needs a library to
 *    compare with, so it opens one of its own from the root path (a second
 *    connection to the database, as the export does) and closes it again.
 *  - The counters and the per-worker active track ids are read without the
 *    lock (atomics), so the status bar can poll them from a const RXJobs.
 *
 * The analyse job decodes mono: every stage of the analysis works on the
 * mean of the channels (dj's decode_player did the same), and a mono buffer
 * halves the memory of the 11 or so decodes running at once. The decode job
 * for a deck decodes nothing: it opens the track's packet store
 * (rx_packets_open, a few milliseconds), which the deck decodes as it plays,
 * at the file's own rate and channels on the players' timeline; only a
 * file the store refuses (Matroska) is decoded whole, as every track was
 * before, which took one to five seconds. The artwork job also makes
 * the 240 px JPEG thumbnail the library stores, so the owner thread never
 * encodes.
 *
 * The memory guard: an hour-long mix decodes to about 635 MB of mono floats
 * at 44.1 kHz and the analyser holds about 180 MB more, so a batch of long
 * mixes on every CPU but one could exhaust the machine. Before it decodes,
 * an analysis reserves its estimate (analysis_estimate) against a budget
 * shared by the queue's workers, 40 % of the physical memory unless
 * RXBOX_ANALYSIS_BUDGET_MB says otherwise, and waits on `budget_cond` while
 * it does not fit. One analysis may always run, so a file larger than the
 * whole budget is still analysed, alone. A waiting job has not started:
 * it is not counted as running, shows no active track, and can be
 * cancelled. Jobs that fit go ahead of one that does not; the queue is
 * finite, so that one runs at the latest when the others are done.
 *
 * The GPU: the analyses' spectral front end runs on one compute context
 * (rxbox/compute.h) that all the workers share. It is made when the first
 * analysis starts, not with the pool, so a pool that never analyses (the
 * interface's I/O pool, a CLI command that only reads) never touches
 * Vulkan; it goes in rx_jobs_destroy, once the pool has joined its last
 * worker. The context serialises its own queue submissions and each
 * analysis has its own command pools, so workers never wait for each
 * other's transforms.
 *
 * Tagging (rxbox/ml.h) is one job for a whole batch of tracks, not one per
 * track: the model takes 3.5 s and 4 GB of the GPU's memory to load, and
 * up to 7 GB while it listens, so it is loaded once, used for every track
 * asked for, one at a time, and unloaded when there is nothing left; and
 * two models on one GPU have made the driver reset it (docs/ML.md). The
 * queue is the RXJobs' own, under `lock`; rx_jobs_tag appends to it and
 * makes the batch's job only when there is none. The job ends only after
 * it has closed the model and found the queue still empty, under the
 * lock: a track asked for while the model closes is tagged by the same
 * job, which opens it again, so a second batch can never start while the
 * first still holds the GPU. Cancelling empties the queue and sets the
 * flag rx_ml_tag watches; a track cancelled half way posts nothing. The
 * model gets the GPU the analyses were given (rx_jobs_set_gpu's name), not
 * their compute context: it keeps a device of its own (src/ml/ml_priv.h). */

#include "rxbox/jobs.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

/* The library's artwork thumbnails (rxbox/library.h). */
#define ARTWORK_THUMB_SIZE 240

/* The memory guard (above): the share of physical memory analyses may hold
 * at once, what the analyser needs on top of the decoded audio (180 MB for
 * the 635 MB of an hour at 44.1 kHz, rounded up), and a fixed part for the
 * rest (the decoder, FFT plans, the waveforms and the result). */
#define ANALYSIS_BUDGET_SHARE 0.4
#define ANALYSIS_OVERHEAD     0.3
#define ANALYSIS_BASE_BYTES   (64LL << 20)

enum JobState {
    JOB_QUEUED = 0,
    JOB_RUNNING,
    JOB_CANCELLED,
};

typedef struct Job {
    RXJobs *j;
    struct Job *prev, *next;        /* on j->head, under j->lock */
    enum JobState state;            /* under j->lock */
    enum RXJobKind kind;
    uint64_t request;
    uint64_t track_id;
    char *path;                     /* the audio file, the stick's mount, or the model's
                                       file (RX_JOB_TAG) */
    char *root;                     /* RX_JOB_STICK_READ: the library */
    int deck;
    int max_size;
    unsigned what;
    RXAnalysisOptions opt;
} Job;

/* A track waiting in the tag queue, with the legend it was asked with (a
 * legend edited meanwhile applies to the tracks asked for after the edit). */
typedef struct TagItem {
    uint64_t track_id;
    char *path;
    char *legend[RX_TRACK_COLORS];
} TagItem;

static void tag_item_free(TagItem *it)
{
    free(it->path);
    for (int i = 0; i < RX_TRACK_COLORS; i++)
        free(it->legend[i]);
    *it = (TagItem) { 0 };
}

typedef struct Slot {
    uint64_t track_id;              /* atomic; 0 when idle */
    int busy;                       /* under j->lock */
} Slot;

struct RXJobs {
    RXThreadPool *pool;
    RXMailbox *results;

    pthread_mutex_t lock;           /* guards the list, the states, the slots' busy flags,
                                       the reservations */
    Job *head;
    uint64_t next_request;

    pthread_cond_t budget_cond;     /* with `lock`: a reservation was released, or a job
                                       cancelled */
    int64_t budget;                 /* bytes analyses may reserve at once */
    int64_t reserved;               /* under lock */
    int nb_reserved;                /* under lock: analyses holding a reservation */

    int pending;                    /* atomic: queued + running, cancelled ones excluded */
    int running;                    /* atomic */
    Slot *slots;
    int nb_slots;

    /* The analyses' GPU (rx_jobs_set_gpu), made by the first analysis
     * under its own lock: making it takes a moment, and `lock` is the
     * owner thread's too. */
    pthread_mutex_t gpu_lock;       /* guards the four below */
    bool gpu_enable;
    bool gpu_tried;
    char *gpu_device;
    RXCompute *gpu;

    /* The tag queue and its batch (rx_jobs_tag): under `lock`, but the
     * cancel flag, which the batch's rx_ml_tag reads as it goes. */
    RXArray tag_queue;              /* TagItem, oldest first */
    Job *tag_job;                   /* the batch's job, queued or running; NULL none */
    int tag_done, tag_total;
    uint64_t tag_current;           /* being tagged; 0 while the model loads */
    RXCancel tag_cancel;
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
static void job_unlink(RXJobs *j, Job *job)
{
    if (job->prev)
        job->prev->next = job->next;
    else
        j->head = job->next;
    if (job->next)
        job->next->prev = job->prev;
    job->prev = job->next = NULL;
}

/* Under j->lock: drops the tracks waiting in the tag queue (not the one
 * being tagged). */
static void tag_drop(RXJobs *j)
{
    for (size_t i = 0; i < j->tag_queue.nb; i++)
        tag_item_free(rx_array_at(&j->tag_queue, i));
    j->tag_total -= (int)j->tag_queue.nb;
    rx_array_clear(&j->tag_queue);
}

/* Under j->lock: no batch any more. */
static void tag_end(RXJobs *j)
{
    j->tag_job = NULL;
    j->tag_done = j->tag_total = 0;
    j->tag_current = 0;
}

/* What an analysis of `path` holds at its peak: the mono decode, a float per
 * sample at the file's own rate (the decoder mixes the channels down packet
 * by packet, so they do not add to it), the analyser's buffers on top, and
 * the fixed part. A file the probe cannot read gets the fixed part only:
 * its decode fails as quickly. */
static int64_t analysis_estimate(const char *path)
{
    RXProbe p;
    int64_t samples = 0;
    if (rx_probe(path, &p) >= 0) {
        samples = RX_MAX(p.total_samples, (int64_t)0);
        rx_probe_free(&p);
    }
    double audio = (double)samples * sizeof(float);
    return ANALYSIS_BASE_BYTES + (int64_t)(audio * (1.0 + ANALYSIS_OVERHEAD));
}

static int64_t analysis_budget(void)
{
    const char *env = getenv("RXBOX_ANALYSIS_BUDGET_MB");
    if (env && env[0]) {
        char *end;
        errno = 0;
        long long mb = strtoll(env, &end, 10);
        if (!errno && !*end && mb > 0 && mb <= INT64_MAX >> 20)
            return (int64_t)mb << 20;
        rx_warn("jobs: RXBOX_ANALYSIS_BUDGET_MB is \"%s\", not a number of MB above 0; "
                "using the default", env);
    }
    long pages = sysconf(_SC_PHYS_PAGES);
    long page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0)
        return (int64_t)((double)pages * page * ANALYSIS_BUDGET_SHARE);
    /* Unknown: what the share is of 16 GB. */
    return (int64_t)((16LL << 30) * ANALYSIS_BUDGET_SHARE);
}

/* Under j->lock: waits until `bytes` fit the budget, or nothing else holds
 * a reservation, or the job is cancelled. Returns false when cancelled. */
static bool reserve(RXJobs *j, Job *job, int64_t bytes)
{
    bool said = false;
    while (job->state != JOB_CANCELLED && j->nb_reserved > 0 && j->reserved + bytes > j->budget) {
        if (!said) {
            rx_debug("jobs: %s waits for %lld MB of memory (%lld of %lld MB reserved)", job->path,
                     (long long)(bytes >> 20), (long long)(j->reserved >> 20),
                     (long long)(j->budget >> 20));
            said = true;
        }
        pthread_cond_wait(&j->budget_cond, &j->lock);
    }
    if (job->state == JOB_CANCELLED)
        return false;
    j->reserved += bytes;
    j->nb_reserved++;
    return true;
}

/* The pool's compute context, made by the first analysis that asks; NULL
 * when there is none (the analyses then run on the CPU). Workers that ask
 * while it is being made wait for it rather than each make their own. */
static RXCompute *jobs_gpu(RXJobs *j)
{
    pthread_mutex_lock(&j->gpu_lock);
    if (!j->gpu_tried && j->gpu_enable) {
        j->gpu_tried = true;
        RXComputeOptions o;
        rx_compute_options_default(&o);
        o.device = j->gpu_device;
        int ret = rx_compute_create(&j->gpu, &o);
        if (ret < 0 && rx_compute_mode() == RX_COMPUTE_REQUIRED)
            rx_err("jobs: RXBOX_ANALYSIS_GPU=1, but there is no GPU (%s); analysing on the CPU",
                   strerror(-ret));
        else if (ret < 0 && rx_compute_mode() != RX_COMPUTE_OFF)
            rx_info("jobs: no GPU for the analyses (%s); they run on the CPU", strerror(-ret));
    }
    RXCompute *gpu = j->gpu;
    pthread_mutex_unlock(&j->gpu_lock);
    return gpu;
}

static int run_analyse(Job *job, RXJobResult *r)
{
    RXAudio audio = { 0 };
    int ret = rx_decode(job->path, 1, 0, &audio);
    if (ret < 0)
        return ret;
    if (!job->opt.compute)
        job->opt.compute = jobs_gpu(job->j);

    RXAnalysis *a = calloc(1, sizeof(*a));
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

static int run_decode(Job *job, RXJobResult *r)
{
    RXPackets *pk;
    int ret = rx_packets_open(job->path, &pk);
    if (ret >= 0) {
        r->packets = pk;
        return 0;
    }
    if (ret != RX_ERROR(ENOTSUP))
        return ret;

    RXAudio *audio = calloc(1, sizeof(*audio));
    if (!audio)
        return RX_ERROR(ENOMEM);

    ret = rx_decode(job->path, 0, 0, audio);
    if (ret < 0) {
        free(audio);
        return ret;
    }

    r->audio = audio;
    return 0;
}

static int run_artwork(Job *job, RXJobResult *r)
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

static int run_stick_read(Job *job, RXJobResult *r)
{
    RXLibrary lib = { 0 };
    int ret = rx_library_open(&lib, job->root);
    if (ret < 0)
        return ret;

    RXStickState *s = calloc(1, sizeof(*s));
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

static int run_inspect(Job *job, RXJobResult *r)
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

static void post(RXJobs *j, RXJobResult *r, const char *what)
{
    if (rx_mailbox_post(j->results, r) < 0) {
        rx_err("jobs: cannot post the result of %s", what);
        rx_job_result_free(r);
    }
}

/* One track of the tag batch: decoded mono at the model's rate, tagged. */
static int tag_one(RXJobs *j, RXML *ml, const TagItem *it, RXJobResult *r)
{
    RXAudio audio = { 0 };
    int ret = rx_decode(it->path, 1, RX_ML_RATE, &audio);
    if (ret < 0)
        return ret;

    RXMLTags *tags = calloc(1, sizeof(*tags));
    if (!tags) {
        rx_audio_free(&audio);
        return RX_ERROR(ENOMEM);
    }
    ret = rx_ml_tag(ml, audio.samples, audio.nb_frames, (const char *const *)it->legend, tags,
                    &j->tag_cancel);
    rx_audio_free(&audio);
    if (ret < 0) {
        free(tags);
        return ret;
    }
    r->tags = tags;
    return 0;
}

/* The tag batch, on one worker, until the queue is empty (see the top of
 * the file). */
static void run_tag(Job *job)
{
    RXJobs *j = job->j;
    RXML *ml = NULL;

    pthread_mutex_lock(&j->gpu_lock);
    char *device = j->gpu_device ? rx_strdup(j->gpu_device) : NULL;
    pthread_mutex_unlock(&j->gpu_lock);

    while (1) {
        TagItem it = { 0 };
        pthread_mutex_lock(&j->lock);
        bool have = !rx_cancelled(&j->tag_cancel) && j->tag_queue.nb;
        if (have) {
            it = *(TagItem *)rx_array_at(&j->tag_queue, 0);
            rx_array_remove(&j->tag_queue, 0);
        }
        j->tag_current = 0;
        pthread_mutex_unlock(&j->lock);

        if (!have) {
            /* Done or cancelled: the model goes before the batch ends. */
            if (ml) {
                rx_ml_close(&ml);
                rx_info("jobs: the model is unloaded");
            }
            pthread_mutex_lock(&j->lock);
            bool more = j->tag_queue.nb > 0;
            if (more)
                __atomic_store_n(&j->tag_cancel.flag, 0, __ATOMIC_RELAXED);
            else
                tag_end(j);
            pthread_mutex_unlock(&j->lock);
            if (more)
                continue;
            break;
        }

        if (!ml) {
            RXMLOptions o;
            rx_ml_options_default(&o);
            o.device = device;
            int64_t t0 = rx_time_us();
            int ret = rx_ml_open(&ml, job->path, &o);
            if (ret < 0) {
                rx_err("jobs: cannot load the model %s: %s", job->path, strerror(-ret));
                tag_item_free(&it);
                /* The failure is posted and the queue dropped under the
                 * lock, so a track asked for after it starts a new batch
                 * and is not lost with this one. */
                RXJobResult *r = calloc(1, sizeof(*r));
                pthread_mutex_lock(&j->lock);
                if (r) {
                    *r = (RXJobResult) {
                        .kind    = RX_JOB_TAG,
                        .request = job->request,
                        .err     = ret,
                    };
                    post(j, r, job->path);
                }
                tag_drop(j);
                tag_end(j);
                pthread_mutex_unlock(&j->lock);
                break;
            }
            rx_info("jobs: the model is loaded on %s in %.1f s", rx_ml_device_name(ml),
                    (rx_time_us() - t0) / 1e6);
        }

        pthread_mutex_lock(&j->lock);
        j->tag_current = it.track_id;
        pthread_mutex_unlock(&j->lock);

        int64_t t0 = rx_time_us();
        RXJobResult *r = calloc(1, sizeof(*r));
        int ret = r ? tag_one(j, ml, &it, r) : RX_ERROR(ENOMEM);
        bool interrupted = ret == RX_ERROR(ECANCELED) && rx_cancelled(&j->tag_cancel);
        if (r && !interrupted) {
            r->kind = RX_JOB_TAG;
            r->track_id = it.track_id;
            r->request = job->request;
            r->err = ret < 0 ? ret : 0;
            r->seconds = (rx_time_us() - t0) / 1e6;
            rx_debug("jobs: tagged %s in %.1f s", it.path, r->seconds);
            post(j, r, it.path);
        } else {
            rx_job_result_free(r);
        }

        pthread_mutex_lock(&j->lock);
        if (interrupted)
            j->tag_total--;
        else
            j->tag_done++;
        pthread_mutex_unlock(&j->lock);

        /* EIO: the model's device stopped answering (a wait failed, the
         * device was lost) or its answer was not a number. It runs nothing
         * more once lost, so it is closed and the next track opens it
         * again: the batch goes on if the GPU recovered, and ends with one
         * message where it cannot be opened, rather than failing every
         * track left. */
        if (ret == RX_ERROR(EIO) && ml) {
            rx_warn("jobs: the model failed on %s; it is loaded again for the next track",
                    it.path);
            rx_ml_close(&ml);
        }
        tag_item_free(&it);
    }

    free(device);
}

/* Every job but the tag batch: one result. */
static RXJobResult *run_one(Job *job)
{
    int64_t t0 = rx_time_us();
    RXJobResult *r = calloc(1, sizeof(*r));
    if (!r) {
        rx_err("jobs: out of memory for the result of %s", job->path);
        return NULL;
    }
    *r = (RXJobResult) {
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
    return r;
}

/* Runs on a pool worker. */
static void run_job(void *arg)
{
    Job *job = arg;
    RXJobs *j = job->j;

    /* An analysis probes for its estimate before taking the lock, unless
     * it was cancelled while queued (a destroyed queue drops its backlog
     * without probing every file in it). */
    int64_t bytes = 0;
    pthread_mutex_lock(&j->lock);
    bool cancelled = job->state == JOB_CANCELLED;
    pthread_mutex_unlock(&j->lock);
    if (job->kind == RX_JOB_ANALYSE && !cancelled)
        bytes = analysis_estimate(job->path);

    pthread_mutex_lock(&j->lock);
    if (job->state == JOB_CANCELLED || (bytes && !reserve(j, job, bytes))) {
        /* rx_jobs_cancel() already took it off the counters, and emptied
         * the queue of a tag batch. */
        if (j->tag_job == job)
            tag_end(j);
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

    /* Post before the counters drop, so an owner that sees pending == 0
     * finds every result already in the mailbox (the tag batch posts each
     * of its own as it goes). */
    if (job->kind == RX_JOB_TAG) {
        run_tag(job);
    } else {
        RXJobResult *r = run_one(job);
        if (r)
            post(j, r, job->path);
    }

    pthread_mutex_lock(&j->lock);
    if (slot >= 0) {
        __atomic_store_n(&j->slots[slot].track_id, 0, __ATOMIC_RELAXED);
        j->slots[slot].busy = 0;
    }
    if (bytes) {
        j->reserved -= bytes;
        j->nb_reserved--;
        pthread_cond_broadcast(&j->budget_cond);
    }
    __atomic_sub_fetch(&j->running, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&j->pending, 1, __ATOMIC_RELAXED);
    job_unlink(j, job);
    pthread_mutex_unlock(&j->lock);

    job_free(job);
}

int rx_jobs_create(RXJobs **out, int threads)
{
    if (threads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        threads = n > 0 ? (int)n : 1;
    }

    RXJobs *j = calloc(1, sizeof(*j));
    if (!j)
        return RX_ERROR(ENOMEM);

    int ret = pthread_mutex_init(&j->lock, NULL);
    if (ret) {
        free(j);
        return RX_ERROR(ret);
    }
    ret = pthread_cond_init(&j->budget_cond, NULL);
    if (ret) {
        pthread_mutex_destroy(&j->lock);
        free(j);
        return RX_ERROR(ret);
    }
    ret = pthread_mutex_init(&j->gpu_lock, NULL);
    if (ret) {
        pthread_cond_destroy(&j->budget_cond);
        pthread_mutex_destroy(&j->lock);
        free(j);
        return RX_ERROR(ret);
    }
    j->gpu_enable = true;
    rx_array_init(&j->tag_queue, sizeof(TagItem));
    j->budget = analysis_budget();
    rx_debug("jobs: analyses may hold %lld MB at once", (long long)(j->budget >> 20));

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
    pthread_mutex_destroy(&j->gpu_lock);
    pthread_cond_destroy(&j->budget_cond);
    pthread_mutex_destroy(&j->lock);
    free(j);
    return ret;
}

void rx_jobs_destroy(RXJobs *j)
{
    if (!j)
        return;

    /* Queued jobs are dropped by the workers that pop them; running ones
     * finish. The pool waits for both before joining. */
    rx_jobs_cancel_all(j);
    rx_pool_destroy(j->pool);

    RXJobResult *r;
    while ((r = rx_mailbox_take(j->results)))
        rx_job_result_free(r);
    rx_mailbox_destroy(j->results);

    /* No worker is left to use the GPU, nor the tag queue. */
    rx_compute_destroy(&j->gpu);
    tag_drop(j);
    rx_array_free(&j->tag_queue);
    free(j->gpu_device);
    pthread_mutex_destroy(&j->gpu_lock);

    pthread_cond_destroy(&j->budget_cond);
    pthread_mutex_destroy(&j->lock);
    free(j->slots);
    free(j);
}

void rx_jobs_set_gpu(RXJobs *j, bool enable, const char *device)
{
    pthread_mutex_lock(&j->gpu_lock);
    if (!j->gpu_tried) {
        j->gpu_enable = enable;
        free(j->gpu_device);
        j->gpu_device = device && device[0] ? rx_strdup(device) : NULL;
    }
    pthread_mutex_unlock(&j->gpu_lock);
}

/* Under j->lock: the job goes on the list, queued, with its request id. */
static void job_link(RXJobs *j, Job *job)
{
    job->j = j;
    job->request = ++j->next_request;
    job->state = JOB_QUEUED;
    job->next = j->head;
    if (j->head)
        j->head->prev = job;
    j->head = job;
    __atomic_add_fetch(&j->pending, 1, __ATOMIC_RELAXED);
}

/* Hands a linked job to the pool; if the pool refuses it, it comes off the
 * list and is freed (a tag batch with its queue: nothing would drain it). */
static int job_start(RXJobs *j, Job *job)
{
    int ret = rx_pool_submit(j->pool, run_job, job);
    if (ret < 0) {
        pthread_mutex_lock(&j->lock);
        if (job->state != JOB_CANCELLED)
            __atomic_sub_fetch(&j->pending, 1, __ATOMIC_RELAXED);
        if (j->tag_job == job) {
            tag_drop(j);
            tag_end(j);
        }
        job_unlink(j, job);
        pthread_mutex_unlock(&j->lock);
        job_free(job);
    }
    return ret;
}

static uint64_t submit(RXJobs *j, Job *job)
{
    pthread_mutex_lock(&j->lock);
    job_link(j, job);
    uint64_t request = job->request;
    pthread_mutex_unlock(&j->lock);

    int ret = job_start(j, job);
    return ret < 0 ? (uint64_t)(int64_t)ret : request;
}

static Job *job_new(enum RXJobKind kind, uint64_t track_id, const char *path)
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

uint64_t rx_jobs_analyse(RXJobs *j, uint64_t track_id, const char *path,
                         const RXAnalysisOptions *opt, unsigned what)
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

uint64_t rx_jobs_decode(RXJobs *j, uint64_t track_id, const char *path, int deck)
{
    if (!path)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_DECODE, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    job->deck = deck;
    return submit(j, job);
}

uint64_t rx_jobs_artwork(RXJobs *j, uint64_t track_id, const char *path, int max_size)
{
    if (!path)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_ARTWORK, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    job->max_size = max_size;
    return submit(j, job);
}

uint64_t rx_jobs_stick_read(RXJobs *j, const char *library_root, const char *mount)
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

uint64_t rx_jobs_inspect(RXJobs *j, const char *mount)
{
    if (!mount)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    Job *job = job_new(RX_JOB_INSPECT, 0, mount);
    if (!job)
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    return submit(j, job);
}

uint64_t rx_jobs_tag(RXJobs *j, const char *model, const char *const *legend, uint64_t track_id,
                     const char *path)
{
    if (!model || !model[0] || !path)
        return (uint64_t)(int64_t)RX_ERROR(EINVAL);
    TagItem it = {
        .track_id = track_id,
        .path     = rx_strdup(path),
    };
    bool copied = true;
    for (int i = 0; i < RX_TRACK_COLORS; i++) {
        const char *v = legend && legend[i] ? legend[i] : rx_config_vibe_default(i + 1);
        copied &= !!(it.legend[i] = rx_strdup(v));
    }
    /* The job a new batch would need, made before the lock is taken. */
    Job *job = job_new(RX_JOB_TAG, 0, model);
    if (!it.path || !copied || !job) {
        tag_item_free(&it);
        job_free(job);
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    }

    pthread_mutex_lock(&j->lock);
    /* Asked for again while it waits or is being tagged: once is enough.
     * (The queue is only ever filled while its batch is there.) */
    bool queued = j->tag_job && j->tag_job->state != JOB_CANCELLED && track_id &&
                  j->tag_current == track_id && !rx_cancelled(&j->tag_cancel);
    for (size_t i = 0; i < j->tag_queue.nb && !queued; i++)
        queued = ((TagItem *)rx_array_at(&j->tag_queue, i))->track_id == track_id;
    if (queued && j->tag_job) {
        uint64_t request = j->tag_job->request;
        pthread_mutex_unlock(&j->lock);
        tag_item_free(&it);
        job_free(job);
        return request;
    }
    if (!rx_array_push(&j->tag_queue, &it)) {
        pthread_mutex_unlock(&j->lock);
        tag_item_free(&it);
        job_free(job);
        return (uint64_t)(int64_t)RX_ERROR(ENOMEM);
    }
    /* A batch cancelled before it started never runs: a new one takes
     * its place. One that runs takes the track, even cancelled (it goes
     * on once it has closed the model). */
    bool start = !j->tag_job || j->tag_job->state == JOB_CANCELLED;
    if (start) {
        job_link(j, job);
        j->tag_job = job;
        j->tag_done = j->tag_total = 0;
        j->tag_current = 0;
        __atomic_store_n(&j->tag_cancel.flag, 0, __ATOMIC_RELAXED);
    }
    j->tag_total++;
    uint64_t request = j->tag_job->request;
    pthread_mutex_unlock(&j->lock);

    if (!start) {
        job_free(job);
        return request;
    }
    int ret = job_start(j, job);
    return ret < 0 ? (uint64_t)(int64_t)ret : request;
}

bool rx_jobs_tag_progress(RXJobs *j, int *done, int *total, uint64_t *current)
{
    if (!j)
        return false;
    pthread_mutex_lock(&j->lock);
    bool active = j->tag_job && j->tag_job->state != JOB_CANCELLED;
    if (done)
        *done = active ? j->tag_done : 0;
    if (total)
        *total = active ? j->tag_total : 0;
    if (current)
        *current = active ? j->tag_current : 0;
    pthread_mutex_unlock(&j->lock);
    return active;
}

/* Under j->lock: the tag batch stops (cancel, cancel_all). */
static void tag_stop(RXJobs *j)
{
    tag_drop(j);
    rx_cancel(&j->tag_cancel);
}

void rx_jobs_cancel(RXJobs *j, uint64_t request)
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
            /* It may be waiting for memory. */
            pthread_cond_broadcast(&j->budget_cond);
        }
        if (job == j->tag_job)
            tag_stop(j);
        break;
    }
    pthread_mutex_unlock(&j->lock);
}

void rx_jobs_cancel_all(RXJobs *j)
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
    if (j->tag_job)
        tag_stop(j);
    pthread_cond_broadcast(&j->budget_cond);
    pthread_mutex_unlock(&j->lock);
}

RXJobResult *rx_jobs_poll(RXJobs *j)
{
    return j ? rx_mailbox_take(j->results) : NULL;
}

RXJobResult *rx_jobs_wait(RXJobs *j, int timeout_ms)
{
    return j ? rx_mailbox_wait(j->results, timeout_ms) : NULL;
}

void rx_job_result_free(RXJobResult *r)
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
    rx_packets_free(r->packets);
    free(r->rgba);
    free(r->jpeg);
    if (r->stick) {
        rx_stick_state_free(r->stick);
        free(r->stick);
    }
    free(r->sync_items);
    free(r->report);
    if (r->tags) {
        rx_ml_tags_free(r->tags);
        free(r->tags);
    }
    free(r);
}

int rx_jobs_pending(const RXJobs *j)
{
    return j ? __atomic_load_n(&j->pending, __ATOMIC_RELAXED) : 0;
}

int rx_jobs_running(const RXJobs *j)
{
    return j ? __atomic_load_n(&j->running, __ATOMIC_RELAXED) : 0;
}

int rx_jobs_active_tracks(const RXJobs *j, uint64_t *ids, int max)
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
