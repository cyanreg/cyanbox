/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Background jobs (cyanbox/jobs.h): decoding and analysing tracks, decoding
 * them for a deck, scaling their cover art, reading and inspecting
 * sticks, and tagging tracks with the model, on an CBThreadPool, with the
 * results handed back to the owner thread through an CBMailbox.
 *
 * Ownership and threads:
 *  - A Job is allocated by the owner when it submits, then belongs to the
 *    pool until a worker has run (or dropped) it; the worker frees it. It
 *    stays on the CBJobs list, under `lock`, from submission until the
 *    worker is done with it, which is what lets the owner cancel a job that
 *    has not started: it only flips the state, and the worker that later
 *    pops it frees it without running it.
 *  - A worker allocates the CBJobResult and its payload and posts it; from
 *    then on it belongs to whoever takes it from the mailbox (the owner),
 *    who releases it with cb_job_result_free().
 *  - Workers never see the owner's library: a job carries copies of the
 *    path and the options, nothing else. The stick read needs a library to
 *    compare with, so it opens one of its own from the root path (a second
 *    connection to the database, as the export does) and closes it again.
 *  - The counters and the per-worker active track ids are read without the
 *    lock (atomics), so the status bar can poll them from a const CBJobs.
 *
 * The analyse job decodes mono: every stage of the analysis works on the
 * mean of the channels (dj's decode_player did the same), and a mono buffer
 * halves the memory of the 11 or so decodes running at once. The decode job
 * for a deck decodes nothing: it opens the track's stream (cb_stream_open,
 * a few milliseconds), which the deck decodes as it plays, at the file's
 * own rate on the players' timeline, where decoding it whole took one to
 * five seconds. The artwork job also makes
 * the 240 px JPEG thumbnail the library stores, so the owner thread never
 * encodes.
 *
 * The memory guard: an hour-long mix decodes to about 635 MB of mono floats
 * at 44.1 kHz and the analyser holds about 180 MB more, so a batch of long
 * mixes on every CPU but one could exhaust the machine. Before it decodes,
 * an analysis reserves its estimate (analysis_estimate) against a budget
 * shared by the queue's workers, 40 % of the physical memory unless
 * CYANBOX_ANALYSIS_BUDGET_MB says otherwise, and waits on `budget_cond` while
 * it does not fit. One analysis may always run, so a file larger than the
 * whole budget is still analysed, alone. A waiting job has not started:
 * it is not counted as running, shows no active track, and can be
 * cancelled. Jobs that fit go ahead of one that does not; the queue is
 * finite, so that one runs at the latest when the others are done.
 *
 * The GPU: the analyses' spectral front end runs on one compute context
 * (cyanbox/compute.h) that all the workers share. It is made when the first
 * analysis starts, not with the pool, so a pool that never analyses (the
 * interface's I/O pool, a CLI command that only reads) never touches
 * Vulkan; it goes in cb_jobs_destroy, once the pool has joined its last
 * worker. The context serialises its own queue submissions and each
 * analysis has its own command pools, so workers never wait for each
 * other's transforms.
 *
 * Tagging (cyanbox/ml.h) is one job for a whole batch of tracks, not one per
 * track: the model takes 3.5 s and 4 GB of the GPU's memory to load, and
 * up to 7 GB while it listens, so it is loaded once, used for every track
 * asked for, one at a time, and unloaded when there is nothing left; and
 * two models on one GPU have made the driver reset it (docs/ML.md). The
 * queue is the CBJobs' own, under `lock`; cb_jobs_tag appends to it and
 * makes the batch's job only when there is none. The job ends only after
 * it has closed the model and found the queue still empty, under the
 * lock: a track asked for while the model closes is tagged by the same
 * job, which opens it again, so a second batch can never start while the
 * first still holds the GPU. Cancelling empties the queue and sets the
 * flag cb_ml_tag watches; a track cancelled half way posts nothing. The
 * model gets the GPU the analyses were given (cb_jobs_set_gpu's name), not
 * their compute context: it keeps a device of its own (src/ml/ml_priv.h).
 * While the model tags a track, a thread of the batch's own decodes the
 * next one in the queue (the first while the model loads): a CPU's 0.3 s
 * a track that the GPU would otherwise wait for.
 *
 * Beat This! (cyanbox/beatthis.h), which the analyses make the grid with
 * (CBAnalysisOptions.beat_net): the pool's one network, from the file
 * cb_jobs_set_beat_model names, opened by the first analysis that runs it,
 * on the GPU the analyses were given, and shared: each run takes `bt_lock`
 * (the network serves one thread at a time; 0.2 s of a 4-minute track
 * against the analysis' seconds of CPU). It is never open while the tag
 * batch holds the GPU, for the same reason as above: the batch closes it
 * when it starts, and an analysis that wants it meanwhile waits on
 * `bt_cond` for the batch to end. A file that cannot be opened is
 * remembered (every analysis would fail alike, each with its own message)
 * until the model is named again. */

#include "cyanbox/jobs.h"

#include <pthread.h>

#include "cyanbox/beatthis.h"
#include <stdio.h>
#include <unistd.h>

/* The library's artwork thumbnails (cyanbox/library.h). */
#define ARTWORK_THUMB_SIZE 240

/* The memory guard (above): the share of physical memory analyses may hold
 * at once, what the analyser needs on top of the decoded audio (180 MB for
 * the 635 MB of an hour at 44.1 kHz, rounded up), and a fixed part for the
 * rest (the decoder, FFT plans, the waveforms and the result). */
#define ANALYSIS_BUDGET_SHARE 0.4
#define ANALYSIS_OVERHEAD     0.3
/* With Beat This!: its 22050 Hz copy beside the decoded audio (half a
 * 44.1 kHz one), which goes before the analyser's buffers come. */
#define ANALYSIS_OVERHEAD_BT  0.5
#define ANALYSIS_BASE_BYTES   (64LL << 20)

enum JobState {
    JOB_QUEUED = 0,
    JOB_RUNNING,
    JOB_CANCELLED,
};

typedef struct Job {
    CBJobs *j;
    struct Job *prev, *next;        /* on j->head, under j->lock */
    enum JobState state;            /* under j->lock */
    enum CBJobKind kind;
    uint64_t request;
    uint64_t track_id;
    char *path;                     /* the audio file, the stick's mount, or the model's
                                       file (CB_JOB_TAG) */
    char *root;                     /* CB_JOB_STICK_READ: the library */
    int deck;
    int max_size;
    unsigned what;
    CBAnalysisOptions opt;
} Job;

/* A track waiting in the tag queue, with the legend it was asked with (a
 * legend edited meanwhile applies to the tracks asked for after the edit). */
typedef struct TagItem {
    uint64_t track_id;
    char *path;
    char *legend[CB_TRACK_COLORS];
} TagItem;

static void tag_item_free(TagItem *it)
{
    free(it->path);
    for (int i = 0; i < CB_TRACK_COLORS; i++)
        free(it->legend[i]);
    *it = (TagItem) { 0 };
}

typedef struct Slot {
    uint64_t track_id;              /* atomic; 0 when idle */
    int busy;                       /* under j->lock */
} Slot;

struct CBJobs {
    CBThreadPool *pool;
    CBMailbox *results;

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

    /* The analyses' GPU (cb_jobs_set_gpu), made by the first analysis
     * under its own lock: making it takes a moment, and `lock` is the
     * owner thread's too. */
    pthread_mutex_t gpu_lock;       /* guards the four below */
    bool gpu_enable;
    bool gpu_tried;
    char *gpu_device;
    CBCompute *gpu;

    /* The tag queue and its batch (cb_jobs_tag): under `lock`, but the
     * cancel flag, which the batch's cb_ml_tag reads as it goes. */
    CBArray tag_queue;              /* TagItem, oldest first */
    Job *tag_job;                   /* the batch's job, queued or running; NULL none */
    int tag_done, tag_total;
    uint64_t tag_current;           /* being tagged; 0 while the model loads */
    CBCancel tag_cancel;

    /* Beat This! for the analyses (above). */
    pthread_mutex_t bt_lock;        /* guards the five below, and each run of bt */
    pthread_cond_t bt_cond;         /* with bt_lock: the tag batch let the GPU go */
    char *bt_model;                 /* cb_jobs_set_beat_model's file; NULL none */
    CBBeatThis *bt;
    int bt_err;                     /* why bt_model would not open; 0 */
    bool tagging;                   /* the tag batch holds the GPU */
    CBBeatNet beat_net;             /* bt for CBAnalysisOptions.beat_net */
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
static void job_unlink(CBJobs *j, Job *job)
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
static void tag_drop(CBJobs *j)
{
    for (size_t i = 0; i < j->tag_queue.nb; i++)
        tag_item_free(cb_array_at(&j->tag_queue, i));
    j->tag_total -= (int)j->tag_queue.nb;
    cb_array_clear(&j->tag_queue);
}

/* Under j->lock: no batch any more. */
static void tag_end(CBJobs *j)
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
static int64_t analysis_estimate(const char *path, bool beat_this)
{
    CBProbe p;
    int64_t samples = 0;
    if (cb_probe(path, &p) >= 0) {
        samples = CB_MAX(p.total_samples, (int64_t)0);
        cb_probe_free(&p);
    }
    double audio = (double)samples * sizeof(float);
    double over = beat_this ? ANALYSIS_OVERHEAD_BT : ANALYSIS_OVERHEAD;
    return ANALYSIS_BASE_BYTES + (int64_t)(audio * (1.0 + over));
}

static int64_t analysis_budget(void)
{
    const char *env = getenv("CYANBOX_ANALYSIS_BUDGET_MB");
    if (env && env[0]) {
        char *end;
        errno = 0;
        long long mb = strtoll(env, &end, 10);
        if (!errno && !*end && mb > 0 && mb <= INT64_MAX >> 20)
            return (int64_t)mb << 20;
        cb_warn("jobs: CYANBOX_ANALYSIS_BUDGET_MB is \"%s\", not a number of MB above 0; "
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
static bool reserve(CBJobs *j, Job *job, int64_t bytes)
{
    bool said = false;
    while (job->state != JOB_CANCELLED && j->nb_reserved > 0 && j->reserved + bytes > j->budget) {
        if (!said) {
            cb_debug("jobs: %s waits for %lld MB of memory (%lld of %lld MB reserved)", job->path,
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
static CBCompute *jobs_gpu(CBJobs *j)
{
    pthread_mutex_lock(&j->gpu_lock);
    if (!j->gpu_tried && j->gpu_enable) {
        j->gpu_tried = true;
        CBComputeOptions o;
        cb_compute_options_default(&o);
        o.device = j->gpu_device;
        int ret = cb_compute_create(&j->gpu, &o);
        if (ret < 0 && cb_compute_mode() == CB_COMPUTE_REQUIRED)
            cb_err("jobs: CYANBOX_ANALYSIS_GPU=1, but there is no GPU (%s); analysing on the CPU",
                   strerror(-ret));
        else if (ret < 0 && cb_compute_mode() != CB_COMPUTE_OFF)
            cb_info("jobs: no GPU for the analyses (%s); they run on the CPU", strerror(-ret));
    }
    CBCompute *gpu = j->gpu;
    pthread_mutex_unlock(&j->gpu_lock);
    return gpu;
}

/* CBBeatNet.run: Beat This! on the pool's one network. */
static int beat_net_run(void *opaque, const float *pcm, int64_t n, float **beat, float **downbeat,
                        int *nb_frames)
{
    CBJobs *j = opaque;
    pthread_mutex_lock(&j->bt_lock);
    while (j->tagging)
        pthread_cond_wait(&j->bt_cond, &j->bt_lock);
    /* Any reason it cannot run is -ENODEV to the analysis (the log has the
     * reason): -ENOENT would read as the track's file missing. */
    int ret = j->bt_err;
    if (!ret && !j->bt) {
        if (!j->bt_model) {
            ret = CB_ERROR(ENODEV);
        } else {
            pthread_mutex_lock(&j->gpu_lock);
            char *device = j->gpu_device ? cb_strdup(j->gpu_device) : NULL;
            pthread_mutex_unlock(&j->gpu_lock);
            CBBeatThisOptions o;
            cb_beatthis_options_default(&o);
            o.device = device;
            int64_t t0 = cb_time_us();
            ret = cb_beatthis_open(&j->bt, j->bt_model, &o);
            free(device);
            if (ret < 0) {
                cb_err("jobs: cannot load Beat This! %s: %s", j->bt_model, strerror(-ret));
                ret = j->bt_err = CB_ERROR(ENODEV);
            } else {
                cb_info("jobs: Beat This! is loaded on %s in %.2f s",
                        cb_beatthis_device_name(j->bt), (cb_time_us() - t0) / 1e6);
            }
        }
    }
    if (ret >= 0) {
        ret = cb_beatthis_run(j->bt, pcm, n, beat, downbeat, nb_frames);
        /* As the tagging model's: lost, it is loaded again for the next. */
        if (ret == CB_ERROR(EIO)) {
            cb_warn("jobs: Beat This! failed; it is loaded again for the next track");
            cb_beatthis_close(&j->bt);
        }
    }
    pthread_mutex_unlock(&j->bt_lock);
    return ret;
}

static int run_analyse(Job *job, CBJobResult *r)
{
    CBAudio audio = { 0 };
    int ret = cb_decode(job->path, 1, 0, &audio);
    if (ret < 0)
        return ret;
    if (!job->opt.compute)
        job->opt.compute = jobs_gpu(job->j);
    if (!job->opt.beat_net)
        job->opt.beat_net = &job->j->beat_net;

    CBAnalysis *a = calloc(1, sizeof(*a));
    if (!a) {
        cb_audio_free(&audio);
        return CB_ERROR(ENOMEM);
    }

    ret = cb_analyse(&audio, &job->opt, job->what, a);
    /* The owner updates the track's length from this: the decode is the
     * players' timeline, which the probe only estimates for some files. */
    if (audio.rate > 0)
        a->duration = (double)audio.nb_frames / audio.rate;
    cb_audio_free(&audio);

    if (ret < 0) {
        cb_analysis_free(a);
        free(a);
        return ret;
    }

    r->analysis = a;
    return 0;
}

static int run_decode(Job *job, CBJobResult *r)
{
    return cb_stream_open(job->path, &r->stream);
}

static int run_artwork(Job *job, CBJobResult *r)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    int ret = cb_artwork_rgba(job->path, &rgba, &w, &h);
    if (ret < 0)
        return ret;

    /* The library keeps a square thumbnail, the size and quality dj used
     * for the stick's larger artwork; the owner stores it. Not fatal. */
    ret = cb_image_jpeg_square(rgba, w, h, ARTWORK_THUMB_SIZE, 90, &r->jpeg, &r->jpeg_size);
    if (ret < 0) {
        cb_debug("jobs: no thumbnail for %s: %s", job->path, strerror(-ret));
        r->jpeg = NULL;
        r->jpeg_size = 0;
    }

    if (job->max_size > 0 && (w > job->max_size || h > job->max_size)) {
        uint8_t *scaled = NULL;
        int sw = 0, sh = 0;
        ret = cb_image_scale(rgba, w, h, job->max_size, job->max_size, &scaled, &sw, &sh);
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

static int run_stick_read(Job *job, CBJobResult *r)
{
    CBLibrary lib = { 0 };
    int ret = cb_library_open(&lib, job->root);
    if (ret < 0)
        return ret;

    CBStickState *s = calloc(1, sizeof(*s));
    if (!s) {
        cb_library_close(&lib);
        return CB_ERROR(ENOMEM);
    }
    ret = cb_stick_read(&lib, job->path, s);
    if (ret < 0) {
        free(s);
    } else {
        r->stick = s;
        ret = cb_stick_diff(&lib, s, &r->sync_items, &r->nb_sync_items);
    }
    cb_library_close(&lib);
    return ret;
}

static void report_line(void *opaque, const char *line)
{
    fprintf(opaque, "%s\n", line);
}

static int run_inspect(Job *job, CBJobResult *r)
{
    size_t size = 0;
    FILE *f = open_memstream(&r->report, &size);
    if (!f)
        return CB_ERROR(ENOMEM);
    int ret = cb_inspect(job->path, report_line, f);
    if (fclose(f))
        return CB_ERROR(ENOMEM);
    if (ret < 0)
        return ret;
    r->problems = ret;
    return 0;
}

static void post(CBJobs *j, CBJobResult *r, const char *what)
{
    if (cb_mailbox_post(j->results, r) < 0) {
        cb_err("jobs: cannot post the result of %s", what);
        cb_job_result_free(r);
    }
}

/* A track of the tag batch decoded ahead, on a thread of its own: mono at
 * the model's rate. */
typedef struct Ahead {
    pthread_t thread;
    bool running;
    char *path;
    CBAudio audio;
    int ret;
} Ahead;

static void *ahead_run(void *arg)
{
    Ahead *a = arg;
    cb_thread_name("jobs:ahead");
    a->ret = cb_decode(a->path, 1, CB_ML_RATE, &a->audio);
    return NULL;
}

/* Waits for the track decoded ahead, and drops it. */
static void ahead_end(Ahead *a)
{
    if (a->running)
        pthread_join(a->thread, NULL);
    cb_audio_free(&a->audio);
    free(a->path);
    *a = (Ahead) { 0 };
}

/* Starts decoding `path` (may be NULL: nothing) unless it already is. */
static void ahead_start(Ahead *a, const char *path)
{
    if (!path || (a->running && !strcmp(a->path, path)))
        return;
    ahead_end(a);
    if (!(a->path = cb_strdup(path)))
        return;
    if (pthread_create(&a->thread, NULL, ahead_run, a)) {
        free(a->path);
        a->path = NULL;
        return;
    }
    a->running = true;
}

/* The audio of `path`: the track decoded ahead if it is that one, else
 * decoded now. */
static int ahead_take(Ahead *a, const char *path, CBAudio *out)
{
    if (a->running && !strcmp(a->path, path)) {
        pthread_join(a->thread, NULL);
        a->running = false;
        int ret = a->ret;
        *out = a->audio;
        a->audio = (CBAudio) { 0 };
        ahead_end(a);
        return ret;
    }
    ahead_end(a);
    return cb_decode(path, 1, CB_ML_RATE, out);
}

/* One track of the tag batch: its audio (decoded ahead, or now), tagged;
 * the next one's decoded meanwhile. */
static int tag_one(CBJobs *j, CBML *ml, const TagItem *it, Ahead *ahead, const char *next,
                   CBJobResult *r)
{
    CBAudio audio = { 0 };
    int64_t t0 = cb_time_us();
    int ret = ahead_take(ahead, it->path, &audio);
    cb_debug("jobs: %s's audio after %.0f ms", it->path, (cb_time_us() - t0) * 1e-3);
    ahead_start(ahead, next);
    if (ret < 0)
        return ret;

    CBMLTags *tags = calloc(1, sizeof(*tags));
    if (!tags) {
        cb_audio_free(&audio);
        return CB_ERROR(ENOMEM);
    }
    ret = cb_ml_tag(ml, audio.samples, audio.nb_frames, (const char *const *)it->legend, tags,
                    &j->tag_cancel);
    cb_audio_free(&audio);
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
    CBJobs *j = job->j;
    CBML *ml = NULL;
    Ahead ahead = { 0 };

    pthread_mutex_lock(&j->gpu_lock);
    char *device = j->gpu_device ? cb_strdup(j->gpu_device) : NULL;
    pthread_mutex_unlock(&j->gpu_lock);

    /* The GPU is the batch's: Beat This! goes until it ends. */
    pthread_mutex_lock(&j->bt_lock);
    j->tagging = true;
    if (j->bt) {
        cb_beatthis_close(&j->bt);
        cb_info("jobs: Beat This! is unloaded while the model tags");
    }
    pthread_mutex_unlock(&j->bt_lock);

    while (1) {
        TagItem it = { 0 };
        char *next = NULL;
        pthread_mutex_lock(&j->lock);
        bool have = !cb_cancelled(&j->tag_cancel) && j->tag_queue.nb;
        if (have) {
            it = *(TagItem *)cb_array_at(&j->tag_queue, 0);
            cb_array_remove(&j->tag_queue, 0);
            if (j->tag_queue.nb)
                next = cb_strdup(((TagItem *)cb_array_at(&j->tag_queue, 0))->path);
        }
        j->tag_current = 0;
        pthread_mutex_unlock(&j->lock);

        if (!have) {
            /* Done or cancelled: the model goes before the batch ends. */
            ahead_end(&ahead);
            if (ml) {
                cb_ml_close(&ml);
                cb_info("jobs: the model is unloaded");
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
            /* The track decodes while the model loads. */
            ahead_start(&ahead, it.path);
            CBMLOptions o;
            cb_ml_options_default(&o);
            o.device = device;
            int64_t t0 = cb_time_us();
            int ret = cb_ml_open(&ml, job->path, &o);
            if (ret < 0) {
                cb_err("jobs: cannot load the model %s: %s", job->path, strerror(-ret));
                ahead_end(&ahead);
                free(next);
                tag_item_free(&it);
                /* The failure is posted and the queue dropped under the
                 * lock, so a track asked for after it starts a new batch
                 * and is not lost with this one. */
                CBJobResult *r = calloc(1, sizeof(*r));
                pthread_mutex_lock(&j->lock);
                if (r) {
                    *r = (CBJobResult) {
                        .kind    = CB_JOB_TAG,
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
            cb_info("jobs: the model is loaded on %s in %.1f s", cb_ml_device_name(ml),
                    (cb_time_us() - t0) / 1e6);
        }

        pthread_mutex_lock(&j->lock);
        j->tag_current = it.track_id;
        pthread_mutex_unlock(&j->lock);

        int64_t t0 = cb_time_us();
        CBJobResult *r = calloc(1, sizeof(*r));
        int ret = r ? tag_one(j, ml, &it, &ahead, next, r) : CB_ERROR(ENOMEM);
        free(next);
        bool interrupted = ret == CB_ERROR(ECANCELED) && cb_cancelled(&j->tag_cancel);
        if (r && !interrupted) {
            r->kind = CB_JOB_TAG;
            r->track_id = it.track_id;
            r->request = job->request;
            r->err = ret < 0 ? ret : 0;
            r->seconds = (cb_time_us() - t0) / 1e6;
            cb_debug("jobs: tagged %s in %.1f s", it.path, r->seconds);
            post(j, r, it.path);
        } else {
            cb_job_result_free(r);
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
        if (ret == CB_ERROR(EIO) && ml) {
            cb_warn("jobs: the model failed on %s; it is loaded again for the next track",
                    it.path);
            cb_ml_close(&ml);
        }
        tag_item_free(&it);
    }

    ahead_end(&ahead);
    free(device);

    pthread_mutex_lock(&j->bt_lock);
    j->tagging = false;
    pthread_cond_broadcast(&j->bt_cond);
    pthread_mutex_unlock(&j->bt_lock);
}

/* Every job but the tag batch: one result. */
static CBJobResult *run_one(Job *job)
{
    int64_t t0 = cb_time_us();
    CBJobResult *r = calloc(1, sizeof(*r));
    if (!r) {
        cb_err("jobs: out of memory for the result of %s", job->path);
        return NULL;
    }
    *r = (CBJobResult) {
        .kind     = job->kind,
        .track_id = job->track_id,
        .deck     = job->deck,
        .request  = job->request,
    };
    int ret;
    switch (job->kind) {
    case CB_JOB_ANALYSE:
        ret = run_analyse(job, r);
        break;
    case CB_JOB_DECODE:
        ret = run_decode(job, r);
        break;
    case CB_JOB_ARTWORK:
        ret = run_artwork(job, r);
        break;
    case CB_JOB_STICK_READ:
        ret = run_stick_read(job, r);
        break;
    case CB_JOB_INSPECT:
        ret = run_inspect(job, r);
        break;
    default:
        ret = CB_ERROR(EINVAL);
        break;
    }
    r->err = ret < 0 ? ret : 0;
    r->seconds = (cb_time_us() - t0) / 1e6;
    return r;
}

/* Runs on a pool worker. */
static void run_job(void *arg)
{
    Job *job = arg;
    CBJobs *j = job->j;

    /* An analysis probes for its estimate before taking the lock, unless
     * it was cancelled while queued (a destroyed queue drops its backlog
     * without probing every file in it). */
    int64_t bytes = 0;
    pthread_mutex_lock(&j->lock);
    bool cancelled = job->state == JOB_CANCELLED;
    pthread_mutex_unlock(&j->lock);
    if (job->kind == CB_JOB_ANALYSE && !cancelled)
        bytes = analysis_estimate(job->path, job->what & CB_ANALYSE_BEATS);

    pthread_mutex_lock(&j->lock);
    if (job->state == JOB_CANCELLED || (bytes && !reserve(j, job, bytes))) {
        /* cb_jobs_cancel() already took it off the counters, and emptied
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
        if (job->kind == CB_JOB_ANALYSE)
            __atomic_store_n(&j->slots[slot].track_id, job->track_id, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&j->running, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&j->lock);

    /* Post before the counters drop, so an owner that sees pending == 0
     * finds every result already in the mailbox (the tag batch posts each
     * of its own as it goes). */
    if (job->kind == CB_JOB_TAG) {
        run_tag(job);
    } else {
        CBJobResult *r = run_one(job);
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

int cb_jobs_create(CBJobs **out, int threads)
{
    if (threads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        threads = n > 0 ? (int)n : 1;
    }

    CBJobs *j = calloc(1, sizeof(*j));
    if (!j)
        return CB_ERROR(ENOMEM);

    int ret = pthread_mutex_init(&j->lock, NULL);
    if (ret) {
        free(j);
        return CB_ERROR(ret);
    }
    ret = pthread_cond_init(&j->budget_cond, NULL);
    if (ret) {
        pthread_mutex_destroy(&j->lock);
        free(j);
        return CB_ERROR(ret);
    }
    ret = pthread_mutex_init(&j->gpu_lock, NULL);
    if (ret) {
        pthread_cond_destroy(&j->budget_cond);
        pthread_mutex_destroy(&j->lock);
        free(j);
        return CB_ERROR(ret);
    }
    ret = pthread_mutex_init(&j->bt_lock, NULL);
    if (ret) {
        pthread_mutex_destroy(&j->gpu_lock);
        pthread_cond_destroy(&j->budget_cond);
        pthread_mutex_destroy(&j->lock);
        free(j);
        return CB_ERROR(ret);
    }
    ret = pthread_cond_init(&j->bt_cond, NULL);
    if (ret) {
        pthread_mutex_destroy(&j->bt_lock);
        pthread_mutex_destroy(&j->gpu_lock);
        pthread_cond_destroy(&j->budget_cond);
        pthread_mutex_destroy(&j->lock);
        free(j);
        return CB_ERROR(ret);
    }
    j->beat_net = (CBBeatNet) { .run = beat_net_run, .opaque = j };
    j->gpu_enable = true;
    cb_array_init(&j->tag_queue, sizeof(TagItem));
    j->budget = analysis_budget();
    cb_debug("jobs: analyses may hold %lld MB at once", (long long)(j->budget >> 20));

    j->nb_slots = threads;
    j->slots = calloc(threads, sizeof(*j->slots));
    if (!j->slots) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }

    ret = cb_mailbox_create(&j->results);
    if (ret < 0)
        goto fail;

    ret = cb_pool_create(&j->pool, threads, "jobs");
    if (ret < 0)
        goto fail;

    *out = j;
    return 0;

fail:
    if (j->results)
        cb_mailbox_destroy(j->results);
    free(j->slots);
    pthread_cond_destroy(&j->bt_cond);
    pthread_mutex_destroy(&j->bt_lock);
    pthread_mutex_destroy(&j->gpu_lock);
    pthread_cond_destroy(&j->budget_cond);
    pthread_mutex_destroy(&j->lock);
    free(j);
    return ret;
}

void cb_jobs_destroy(CBJobs *j)
{
    if (!j)
        return;

    /* Queued jobs are dropped by the workers that pop them; running ones
     * finish. The pool waits for both before joining. */
    cb_jobs_cancel_all(j);
    cb_pool_destroy(j->pool);

    CBJobResult *r;
    while ((r = cb_mailbox_take(j->results)))
        cb_job_result_free(r);
    cb_mailbox_destroy(j->results);

    /* No worker is left to use the GPU, nor the tag queue. */
    cb_beatthis_close(&j->bt);
    free(j->bt_model);
    pthread_cond_destroy(&j->bt_cond);
    pthread_mutex_destroy(&j->bt_lock);
    cb_compute_destroy(&j->gpu);
    tag_drop(j);
    cb_array_free(&j->tag_queue);
    free(j->gpu_device);
    pthread_mutex_destroy(&j->gpu_lock);

    pthread_cond_destroy(&j->budget_cond);
    pthread_mutex_destroy(&j->lock);
    free(j->slots);
    free(j);
}

void cb_jobs_set_gpu(CBJobs *j, bool enable, const char *device)
{
    pthread_mutex_lock(&j->gpu_lock);
    if (!j->gpu_tried) {
        j->gpu_enable = enable;
        free(j->gpu_device);
        j->gpu_device = device && device[0] ? cb_strdup(device) : NULL;
    }
    pthread_mutex_unlock(&j->gpu_lock);
}

void cb_jobs_set_beat_model(CBJobs *j, const char *path)
{
    pthread_mutex_lock(&j->bt_lock);
    if (!(path && j->bt_model && !strcmp(path, j->bt_model))) {
        cb_beatthis_close(&j->bt);
        free(j->bt_model);
        j->bt_model = path && path[0] ? cb_strdup(path) : NULL;
    }
    j->bt_err = 0;
    pthread_mutex_unlock(&j->bt_lock);
}

/* Under j->lock: the job goes on the list, queued, with its request id. */
static void job_link(CBJobs *j, Job *job)
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
static int job_start(CBJobs *j, Job *job)
{
    int ret = cb_pool_submit(j->pool, run_job, job);
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

static uint64_t submit(CBJobs *j, Job *job)
{
    pthread_mutex_lock(&j->lock);
    job_link(j, job);
    uint64_t request = job->request;
    pthread_mutex_unlock(&j->lock);

    int ret = job_start(j, job);
    return ret < 0 ? (uint64_t)(int64_t)ret : request;
}

static Job *job_new(enum CBJobKind kind, uint64_t track_id, const char *path)
{
    if (!path)
        return NULL;
    Job *job = calloc(1, sizeof(*job));
    if (!job)
        return NULL;
    job->kind = kind;
    job->track_id = track_id;
    job->deck = -1;
    job->path = cb_strdup(path);
    if (!job->path) {
        free(job);
        return NULL;
    }
    return job;
}

uint64_t cb_jobs_analyse(CBJobs *j, uint64_t track_id, const char *path,
                         const CBAnalysisOptions *opt, unsigned what)
{
    if (!path || !what)
        return (uint64_t)(int64_t)CB_ERROR(EINVAL);
    Job *job = job_new(CB_JOB_ANALYSE, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);

    if (opt)
        job->opt = *opt;
    else
        cb_analysis_options_default(&job->opt);
    job->what = what;
    return submit(j, job);
}

uint64_t cb_jobs_decode(CBJobs *j, uint64_t track_id, const char *path, int deck)
{
    if (!path)
        return (uint64_t)(int64_t)CB_ERROR(EINVAL);
    Job *job = job_new(CB_JOB_DECODE, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);
    job->deck = deck;
    return submit(j, job);
}

uint64_t cb_jobs_artwork(CBJobs *j, uint64_t track_id, const char *path, int max_size)
{
    if (!path)
        return (uint64_t)(int64_t)CB_ERROR(EINVAL);
    Job *job = job_new(CB_JOB_ARTWORK, track_id, path);
    if (!job)
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);
    job->max_size = max_size;
    return submit(j, job);
}

uint64_t cb_jobs_stick_read(CBJobs *j, const char *library_root, const char *mount)
{
    if (!library_root || !mount)
        return (uint64_t)(int64_t)CB_ERROR(EINVAL);
    Job *job = job_new(CB_JOB_STICK_READ, 0, mount);
    if (!job)
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);
    job->root = cb_strdup(library_root);
    if (!job->root) {
        job_free(job);
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);
    }
    return submit(j, job);
}

uint64_t cb_jobs_inspect(CBJobs *j, const char *mount)
{
    if (!mount)
        return (uint64_t)(int64_t)CB_ERROR(EINVAL);
    Job *job = job_new(CB_JOB_INSPECT, 0, mount);
    if (!job)
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);
    return submit(j, job);
}

uint64_t cb_jobs_tag(CBJobs *j, const char *model, const char *const *legend, uint64_t track_id,
                     const char *path)
{
    if (!model || !model[0] || !path)
        return (uint64_t)(int64_t)CB_ERROR(EINVAL);
    TagItem it = {
        .track_id = track_id,
        .path     = cb_strdup(path),
    };
    bool copied = true;
    for (int i = 0; i < CB_TRACK_COLORS; i++) {
        const char *v = legend && legend[i] ? legend[i] : cb_config_vibe_default(i + 1);
        copied &= !!(it.legend[i] = cb_strdup(v));
    }
    /* The job a new batch would need, made before the lock is taken. */
    Job *job = job_new(CB_JOB_TAG, 0, model);
    if (!it.path || !copied || !job) {
        tag_item_free(&it);
        job_free(job);
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);
    }

    pthread_mutex_lock(&j->lock);
    /* Asked for again while it waits or is being tagged: once is enough.
     * (The queue is only ever filled while its batch is there.) */
    bool queued = j->tag_job && j->tag_job->state != JOB_CANCELLED && track_id &&
                  j->tag_current == track_id && !cb_cancelled(&j->tag_cancel);
    for (size_t i = 0; i < j->tag_queue.nb && !queued; i++)
        queued = ((TagItem *)cb_array_at(&j->tag_queue, i))->track_id == track_id;
    if (queued && j->tag_job) {
        uint64_t request = j->tag_job->request;
        pthread_mutex_unlock(&j->lock);
        tag_item_free(&it);
        job_free(job);
        return request;
    }
    if (!cb_array_push(&j->tag_queue, &it)) {
        pthread_mutex_unlock(&j->lock);
        tag_item_free(&it);
        job_free(job);
        return (uint64_t)(int64_t)CB_ERROR(ENOMEM);
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

bool cb_jobs_tag_progress(CBJobs *j, int *done, int *total, uint64_t *current)
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
static void tag_stop(CBJobs *j)
{
    tag_drop(j);
    cb_cancel(&j->tag_cancel);
}

void cb_jobs_cancel(CBJobs *j, uint64_t request)
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

void cb_jobs_cancel_all(CBJobs *j)
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

CBJobResult *cb_jobs_poll(CBJobs *j)
{
    return j ? cb_mailbox_take(j->results) : NULL;
}

CBJobResult *cb_jobs_wait(CBJobs *j, int timeout_ms)
{
    return j ? cb_mailbox_wait(j->results, timeout_ms) : NULL;
}

void cb_job_result_free(CBJobResult *r)
{
    if (!r)
        return;
    if (r->analysis) {
        cb_analysis_free(r->analysis);
        free(r->analysis);
    }
    cb_stream_free(r->stream);
    free(r->rgba);
    free(r->jpeg);
    if (r->stick) {
        cb_stick_state_free(r->stick);
        free(r->stick);
    }
    free(r->sync_items);
    free(r->report);
    if (r->tags) {
        cb_ml_tags_free(r->tags);
        free(r->tags);
    }
    free(r);
}

int cb_jobs_pending(const CBJobs *j)
{
    return j ? __atomic_load_n(&j->pending, __ATOMIC_RELAXED) : 0;
}

int cb_jobs_running(const CBJobs *j)
{
    return j ? __atomic_load_n(&j->running, __ATOMIC_RELAXED) : 0;
}

int cb_jobs_active_tracks(const CBJobs *j, uint64_t *ids, int max)
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
