/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Writing a stick, rekordbox's sync-manager model (rxbox/export.h): the
 * settings, the chosen playlists (all by default) and the union of their
 * tracks, or the whole library. A port of dj's export.py write_stick with
 * the checks docs/OVERVIEW.md 3.5 asks for.
 *
 * Order matters for a stick that is unplugged half way: audio first, then
 * the ANLZ files, artwork and settings, and the databases last, so a
 * database never points at files that are not there yet (dj). Audio
 * already on the stick with the same size and a modification time within
 * FAT's two seconds is skipped (dj's _same_file); the analysis files and
 * artwork are rebuilt and written where the stick's differ; the settings
 * files and databases every time. Everything is written in place, the
 * audio straight through to the stick (O_DIRECT), with one sync of the
 * stick at the end: no temporaries, no fsync per file (a tenth of a second
 * each on a stick mounted with flush, three files a track).
 *
 * What is replaced and what is kept: export.pdb, exportExt.pdb and
 * exportLibrary.db are rebuilt (the history a player appended to them goes
 * with them, which is why a stick with unsynced history blocks the
 * export), exportLibrary.db-wal/-shm and a stale exportExt.pdb are removed
 * (docs/pdb.md 1.1). Files players write and rxbox does not (export.pdb.bak,
 * RBFLTR.DAT, PIONEER/CDJ, LOG, MPJ ...) are never touched. With `prune`,
 * audio, ANLZ and artwork files that are not part of this export are
 * deleted; without it they are only reported.
 *
 * Analysis is the caller's (force_analysis included): a track without a
 * grid is reported and left out, one without waveforms gets ANLZ files
 * without waveform tags, both through the callback.
 *
 * Progress counts are 1-based ("copy" 1 of 58 is the first file). A dry
 * run writes nothing anywhere: the transcodes it would make are listed as
 * "transcode" items (and the plan describes them from the originals), and
 * every file it would write as "copy", "skip" or "rewrite". Real
 * transcodes run on a pool of half the CPUs before the plan is made.
 *
 * What keeps the stick and the library safe:
 *  - the databases are built during the checks, so a library they cannot
 *    hold (a name no DeviceSQL page fits) stops the export before anything
 *    is written;
 *  - every file written, read back or deleted is resolved below the
 *    stick's root with no symbolic link followed and no other file system
 *    entered (rx_stick_open; FAT has no links, a staging directory may,
 *    at any depth), and the checks name such a path before anything is
 *    written, so nothing is written or deleted out of the stick;
 *  - the old sync record is removed before the databases are replaced and
 *    the new one written after them, so a record never describes other
 *    databases than the stick's;
 *  - prune never deletes a file that is, by the file system's own lookup
 *    of the plan's spelling and the file found, back to back as it
 *    deletes, one the plan writes, nor a library track's file or anything
 *    under the library's root, wherever those are; its walk follows no
 *    link and does not leave the stick's file system. */

#include "rxbox/export.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "rxbox/thread.h"
#include "src/export/export_priv.h"
#include "src/formats/formats_priv.h"

#define DB_RESERVE (64ll << 20)

void rx_export_options_default(RXExportOptions *o)
{
    *o = (RXExportOptions) { 0 };
}

typedef struct Ctx {
    RXLibrary              *lib;
    const char             *dest;
    /* The stick's root (rx_stick_root): every file written, read back or
     * deleted is resolved below it (export_priv.h), and the device it is
     * on, which prune's walk does not leave. */
    int                     root;
    dev_t                   dev;
    /* A real run past its checks has written to the stick (and syncs it
     * whatever happens next); everything is written (a cancel changes
     * nothing any more). */
    bool                    writing, finished;
    const RXExportOptions  *opt;
    rx_progress_cb          cb;
    void                   *opaque;
    bool                    cancelled;
    /* The databases, built by the checks and written last. */
    uint8_t                *pdb, *ext, *olib;
    size_t                  pdb_size, ext_size, olib_size;
    /* The copy's throughput for opt->stats: bytes so far, and samples of
     * (time, bytes) a quarter second apart, the rate taken over them. */
    int64_t                 bytes_total, bytes_done;
    int64_t                 t_sample[16], b_sample[16];
    int                     nb_samples, head;
    /* The small files' writes and the audio files' closes, in parallel
     * (PutJob); the jobs of the stage, checked by puts_wait. */
    RXThreadPool           *pool;
    RXArray                 puts;       /* PutJob * */
} Ctx;

static void reportf(Ctx *c, const char *stage, const char *fmt, ...) RX_PRINTF(3, 4);

/* A stick mounted with flush (udisks' default for removable FAT) sleeps a
 * tenth of a second in the kernel on every close of a file written
 * (fs/fat/file.c, fat_file_release), whatever its size: two hundred
 * analysis and artwork files written one after another cost twenty
 * seconds, sixteen at a time a little over one. So the small files are
 * written, and the copied audio files closed, on a pool, and a stage
 * waits for its jobs at its end. The job frees the bytes it owns as soon
 * as they are written, so what waits in the queue is bounded. */
#define PUT_THREADS 16
#define PUT_QUEUE   64

typedef struct PutJob {
    int      root;              /* the stick's (Ctx.root) */
    char    *path;              /* the stick path; NULL for a close */
    uint8_t *data;
    size_t   size;
    bool     owns;              /* free the data once written */
    int      fd;                /* the audio file to close, or -1 */
    int      ret;
} PutJob;

static void put_job(void *arg)
{
    PutJob *j = arg;
    if (j->fd >= 0) {
        j->ret = close(j->fd) < 0 ? RX_ERROR(errno) : 0;
        j->fd = -1;
    } else {
        j->ret = rx_stick_write(j->root, j->path, j->data, j->size);
    }
    if (j->owns)
        free(j->data);
    j->data = NULL;
}

static void put_free(PutJob *j)
{
    if (!j)
        return;
    if (j->fd >= 0)
        close(j->fd);
    if (j->owns)
        free(j->data);
    free(j->path);
    free(j);
}

/* Queues a write (`path`, `data`) or a close (`fd`). The data is the
 * job's when `owns`, freed by it; the path is copied. On failure the data
 * and fd are still the caller's. */
static int put_submit(Ctx *c, const char *path, uint8_t *data, size_t size, bool owns, int fd)
{
    int ret = 0;
    if (!c->pool && (ret = rx_pool_create(&c->pool, PUT_THREADS)) < 0)
        return ret;
    PutJob *j = calloc(1, sizeof(*j));
    char *p = path ? rx_strdup(path) : NULL;
    if (!j || (path && !p)) {
        free(j);
        free(p);
        return RX_ERROR(ENOMEM);
    }
    *j = (PutJob) { .root = c->root, .path = p, .data = data, .size = size, .owns = owns, .fd = fd };
    if (!rx_array_push(&c->puts, &j)) {
        j->owns = false;
        j->fd = -1;
        put_free(j);
        return RX_ERROR(ENOMEM);
    }
    /* No more than a queue's worth ahead of the writers. */
    while (rx_pool_pending(c->pool) >= PUT_QUEUE)
        nanosleep(&(struct timespec) { .tv_nsec = 2000000 }, NULL);
    if ((ret = rx_pool_submit(c->pool, put_job, j)) < 0) {
        c->puts.nb--;
        j->owns = false;
        j->fd = -1;
        put_free(j);
    }
    return ret;
}

/* The stage's jobs done; the first failure reported under `stage`. */
static int puts_wait(Ctx *c, const char *stage)
{
    if (c->pool)
        rx_pool_wait(c->pool);
    int ret = 0;
    for (size_t i = 0; i < c->puts.nb; i++) {
        PutJob *j = *(PutJob **)rx_array_at(&c->puts, i);
        if (j->ret < 0) {
            reportf(c, stage, "cannot write %s: %s", j->path ? j->path : "an audio file", strerror(-j->ret));
            ret = ret ? ret : j->ret;
        }
        put_free(j);
    }
    c->puts.nb = 0;
    return ret;
}

static void stats_store(Ctx *c, int64_t done, int64_t rate, int64_t left)
{
    if (!c->opt->stats)
        return;
    __atomic_store_n(&c->opt->stats->bytes_total, c->bytes_total, __ATOMIC_RELAXED);
    __atomic_store_n(&c->opt->stats->bytes_done, done, __ATOMIC_RELAXED);
    __atomic_store_n(&c->opt->stats->rate, rate, __ATOMIC_RELAXED);
    __atomic_store_n(&c->opt->stats->seconds_left, left, __ATOMIC_RELAXED);
}

static void stats_start(Ctx *c, int64_t bytes_total)
{
    c->bytes_total = bytes_total;
    c->bytes_done = 0;
    c->nb_samples = 1;
    c->head = 0;
    c->t_sample[0] = rx_time_us();
    c->b_sample[0] = 0;
    stats_store(c, 0, 0, -1);
}

/* After a chunk reached the stick: the bytes, and every quarter second a
 * sample; the rate is over the samples kept (sixteen: four seconds at
 * full speed, longer on a slow stick), the time left at that rate. */
static void stats_add(Ctx *c, int64_t bytes)
{
    c->bytes_done += bytes;
    if (!c->opt->stats)
        return;
    int64_t now = rx_time_us();
    int last = (c->head + c->nb_samples - 1) % 16;
    if (now - c->t_sample[last] < 250000) {
        __atomic_store_n(&c->opt->stats->bytes_done, c->bytes_done, __ATOMIC_RELAXED);
        return;
    }
    if (c->nb_samples == 16)
        c->head = (c->head + 1) % 16;
    else
        c->nb_samples++;
    int idx = (c->head + c->nb_samples - 1) % 16;
    c->t_sample[idx] = now;
    c->b_sample[idx] = c->bytes_done;
    int64_t dt = now - c->t_sample[c->head], db = c->bytes_done - c->b_sample[c->head];
    int64_t rate = dt >= 500000 ? db * 1000000 / dt : 0;
    int64_t left = rate > 0 ? (RX_MAX(c->bytes_total - c->bytes_done, 0) + rate - 1) / rate : -1;
    stats_store(c, c->bytes_done, rate, left);
}

static bool progress(Ctx *c, const char *stage, int done, int total, const char *item)
{
    if (c->cancelled)
        return false;
    if (c->cb && !c->cb(c->opaque, stage, done, total, item))
        c->cancelled = true;
    return !c->cancelled;
}

static void reportf(Ctx *c, const char *stage, const char *fmt, ...) RX_PRINTF(3, 4);
static void reportf(Ctx *c, const char *stage, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = rx_vasprintf(fmt, ap);
    va_end(ap);
    progress(c, stage, 0, 0, msg);
    free(msg);
}

/* What the export says at its end, cancelled or not: the callback's answer
 * no longer matters. */
static void notify(Ctx *c, const char *stage, int done, int total, const char *fmt, ...) RX_PRINTF(5, 6);
static void notify(Ctx *c, const char *stage, int done, int total, const char *fmt, ...)
{
    char *msg = NULL;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        msg = rx_vasprintf(fmt, ap);
        va_end(ap);
    }
    if (c->cb)
        c->cb(c->opaque, stage, done, total, msg);
    free(msg);
}

/* "1.7 GB", "320 MB", "12 kB": decimal units, as the rate is shown. */
static void size_text(int64_t bytes, char *buf, size_t size)
{
    double b = (double)RX_MAX(bytes, 0);
    if (b >= 1e9)
        snprintf(buf, size, "%.1f GB", b / 1e9);
    else if (b >= 1e6)
        snprintf(buf, size, "%.0f MB", b / 1e6);
    else
        snprintf(buf, size, "%.0f kB", b / 1e3);
}

/* ------------------------------------------------------------- tracks */

static int cmp_ptr_id(const void *a, const void *b)
{
    uint64_t x = (*(RXTrack *const *)a)->id, y = (*(RXTrack *const *)b)->id;
    return x < y ? -1 : x > y;
}

/* The tracks to export: the union of the playlists' tracks, or the whole
 * library; missing files and unanalysed tracks reported and left out. */
static int select_tracks(Ctx *c, const RXPlaylist *const *pls, int nb_pls, RXTrack ***out, int *nb)
{
    RXLibrary *lib = c->lib;
    RXTrack **v = malloc((size_t)RX_MAX(lib->nb_tracks, 1) * sizeof(*v));
    if (!v)
        return RX_ERROR(ENOMEM);
    int n = 0;
    if (c->opt->whole_library) {
        for (int i = 0; i < lib->nb_tracks; i++)
            v[n++] = lib->tracks[i];
    } else {
        uint8_t *in = calloc((size_t)RX_MAX(lib->nb_tracks, 1), 1);
        if (!in) {
            free(v);
            return RX_ERROR(ENOMEM);
        }
        for (int p = 0; p < nb_pls; p++) {
            for (int k = 0; k < pls[p]->nb; k++) {
                RXTrack *t = rx_library_find_id(lib, pls[p]->track_ids[k]);
                int idx = t ? rx_library_index_of(lib, t) : -1;
                if (idx >= 0 && !in[idx]) {
                    in[idx] = 1;
                    v[n++] = t;
                }
            }
        }
        free(in);
        qsort(v, (size_t)n, sizeof(*v), cmp_ptr_id);
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        char name[512];
        rx_track_display(v[i], name, sizeof(name));
        if (v[i]->missing) {
            reportf(c, "check", "skipped, file missing: %s", name);
            continue;
        }
        if (!rx_track_analysed(v[i])) {
            reportf(c, "check", "skipped, not analysed (no beat grid): %s", name);
            continue;
        }
        if (v[i]->id > UINT32_MAX) {
            reportf(c, "check", "skipped, track id beyond 32 bits: %s", name);
            continue;
        }
        v[k++] = v[i];
    }
    *out = v;
    *nb = k;
    return 0;
}

/* -------------------------------------------------------------- files */

/* dj's _same_file(): same size, mtime within FAT's 2 s resolution. The
 * stick's copy is a regular file reached through no link. */
static bool same_file(int root, const char *src, const char *stick_path)
{
    struct stat a, b;
    if (root < 0 || stat(src, &a) < 0 || rx_stick_stat(root, stick_path, &b) < 0 || !S_ISREG(b.st_mode))
        return false;
    return a.st_size == b.st_size && llabs((long long)a.st_mtime - (long long)b.st_mtime) <= 2;
}

/* Straight onto the stick, in place: no temporary, no fsync. The file is
 * opened O_DIRECT where the file system takes it, so every chunk is on
 * the stick when write() returns and the rate shown is the stick's, not
 * the page cache's filling up and draining at the end (FAT and exFAT
 * write an O_DIRECT chunk through before returning; ext4 and the like
 * want the chunks aligned, so the file's tail goes buffered; tmpfs
 * refuses the flag and gets plain writes). A copy that stops part way
 * leaves a shorter file with today's time, which same_file() does not
 * take for the source. A tick every quarter second (the step again, no
 * item) moves the interface and lets it cancel inside a long file. */
static int copy_file(Ctx *c, const char *src, const char *dst, int step, int steps)
{
    enum { CHUNK = 1 << 20, ALIGN = 4096 };
    int ret = 0;
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return RX_ERROR(errno);
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    int out = rx_stick_open(c->root, dst, flags | O_DIRECT, 0644);
    bool direct = out >= 0;
    if (out == RX_ERROR(EINVAL))
        out = rx_stick_open(c->root, dst, flags, 0644);
    uint8_t *buf = NULL;
    if (out < 0 || posix_memalign((void **)&buf, ALIGN, CHUNK)) {
        ret = out < 0 ? out : RX_ERROR(ENOMEM);
        out = -1;
        buf = NULL;
        goto out;
    }
    int64_t last_tick = rx_time_us();
    while (1) {
        /* A whole chunk; a short one is the end of the file. */
        size_t n = 0;
        while (n < CHUNK) {
            ssize_t r = read(in, buf + n, CHUNK - n);
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0) {
                ret = RX_ERROR(errno);
                goto out;
            }
            if (!r)
                break;
            n += (size_t)r;
        }
        if (!n)
            break;
        if (direct && n % ALIGN) {
            int fl = fcntl(out, F_GETFL);
            if (fl < 0 || fcntl(out, F_SETFL, fl & ~O_DIRECT) < 0) {
                ret = RX_ERROR(errno);
                goto out;
            }
            direct = false;
        }
        for (size_t o = 0; o < n;) {
            ssize_t w = write(out, buf + o, n - o);
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0) {
                ret = RX_ERROR(errno);
                goto out;
            }
            o += (size_t)w;
        }
        stats_add(c, (int64_t)n);
        int64_t now = rx_time_us();
        if (now - last_tick >= 250000) {
            last_tick = now;
            if (!progress(c, "copy", step, steps, NULL)) {
                ret = RX_ERROR(ECANCELED);
                goto out;
            }
        }
        if (n < CHUNK)
            break;
    }
    /* The source's time, for same_file() next time. */
    struct stat st;
    if (fstat(in, &st) == 0) {
        struct timespec ts[2] = { st.st_atim, st.st_mtim };
        futimens(out, ts);
    }
    /* The close on the pool: its tenth of a second (PutJob) overlaps the
     * next file. */
    if (put_submit(c, dst, NULL, 0, false, out) >= 0)
        out = -1;
out:
    if (out >= 0 && close(out) < 0 && !ret)
        ret = RX_ERROR(errno);
    close(in);
    free(buf);
    return ret;
}

/* The databases and settings files: few, and on the stick before the call
 * returns. */
static int write_file(Ctx *c, const char *stick_path, const void *data, size_t size)
{
    int ret = rx_stick_write(c->root, stick_path, data, size);
    if (ret < 0)
        reportf(c, "database", "cannot write %s%s: %s", c->dest, stick_path, strerror(-ret));
    return ret;
}

/* Whether the stick already holds exactly these bytes at `stick_path`. */
static bool unchanged(Ctx *c, const char *stick_path, const void *data, size_t size)
{
    struct stat st;
    if (rx_stick_stat(c->root, stick_path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != (off_t)size)
        return false;
    uint8_t *old;
    size_t n;
    if (rx_stick_read_file(c->root, stick_path, &old, &n) < 0)
        return false;
    bool same = n == size && !memcmp(old, data, size);
    free(old);
    return same;
}

/* A per-track file (analysis, artwork): kept when the stick holds it
 * already, written otherwise. Reading a file back costs milliseconds, a
 * write on a stick far more, three files a track, and a stick is
 * exported to again far more often than its tracks change. `*written`
 * counts the files written. */
static int put_file(Ctx *c, const char *stick_path, uint8_t *data, size_t size, bool owns, int *written)
{
    int ret = 0;
    if (unchanged(c, stick_path, data, size)) {
        if (owns)
            free(data);
    } else if ((ret = put_submit(c, stick_path, data, size, owns, -1)) < 0) {
        if (owns)
            free(data);
    } else {
        (*written)++;
    }
    return ret;
}

static void remove_file(Ctx *c, const char *stick_path)
{
    if (rx_stick_unlink(c->root, stick_path, 0) == 0)
        reportf(c, "database", "removed stale %s", stick_path);
}

/* ---------------------------------------------------------- our files */

/* A stick path and its key under the stick's case rules
 * (rx_plan_fold_case: two paths with one key are one file on FAT and
 * exFAT). */
typedef struct OurPath {
    char *key, *path;
} OurPath;

typedef struct Ours {
    OurPath *v;                  /* sorted by key (ours_sort) */
    int      nb, cap;
} Ours;

static int ours_add(Ours *o, const char *stick_path)
{
    if (o->nb == o->cap) {
        int cap = o->cap ? 2 * o->cap : 256;
        OurPath *v = realloc(o->v, (size_t)cap * sizeof(*v));
        if (!v)
            return RX_ERROR(ENOMEM);
        o->v = v;
        o->cap = cap;
    }
    char *key = rx_plan_fold_case(stick_path), *path = rx_strdup(stick_path);
    if (!key || !path) {
        free(key);
        free(path);
        return RX_ERROR(ENOMEM);
    }
    o->v[o->nb++] = (OurPath) { .key = key, .path = path };
    return 0;
}

static int cmp_our(const void *a, const void *b)
{
    return strcmp(((const OurPath *)a)->key, ((const OurPath *)b)->key);
}

static void ours_sort(Ours *o)
{
    if (o->nb)
        qsort(o->v, (size_t)o->nb, sizeof(*o->v), cmp_our);
}

/* The first path under `key`, or -1. */
static int ours_find(const Ours *o, const char *key)
{
    int lo = 0, hi = o->nb;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strcmp(o->v[mid].key, key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < o->nb && !strcmp(o->v[lo].key, key) ? lo : -1;
}

static void ours_free(Ours *o)
{
    for (int i = 0; i < o->nb; i++) {
        free(o->v[i].key);
        free(o->v[i].path);
    }
    free(o->v);
    *o = (Ours) { 0 };
}

static int collect_ours(const RXPlan *plan, Ours *o)
{
    int ret = 0;
    for (int i = 0; i < plan->nb_tracks && ret >= 0; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        ret = ours_add(o, t->usb_path);
        const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
        for (int k = 0; k < 3 && ret >= 0; k++) {
            char *p = rx_plan_anlz_path(t, ext[k]);
            ret = p ? ours_add(o, p) : RX_ERROR(ENOMEM);
            free(p);
        }
    }
    for (int i = 0; i < plan->nb_artworks && ret >= 0; i++) {
        uint32_t id = plan->artworks[i].id;
        char *dir = rx_plan_artwork_dir(id);
        const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
        for (int k = 0; k < 4 && ret >= 0 && dir; k++) {
            char *p = rx_asprintf(fmt[k], dir, id);
            ret = p ? ours_add(o, p) : RX_ERROR(ENOMEM);
            free(p);
        }
        if (!dir)
            ret = RX_ERROR(ENOMEM);
        free(dir);
    }
    ours_sort(o);
    return ret;
}

/* What prune keeps besides the plan's files: the library's own (its
 * tracks and the transcodes copied) where they are on the stick, by stick
 * path, and everything under the library's root. Inode numbers seen at
 * the start are a last resort that can only keep more: FAT and exFAT make
 * them up as they read inodes in, so a file read in again (the cache full
 * of a big export) has another, and a number seen at the start may be
 * another file's by the time the walk gets there. */
typedef struct Keep {
    dev_t dev;
    ino_t ino;
} Keep;

typedef struct KeepSet {
    Ours    lib;                 /* the library's files on the stick */
    RXArray inodes;              /* Keep, sorted */
    char   *lib_root;            /* the library's root, resolved; NULL if unknown */
} KeepSet;

static int cmp_keep(const void *a, const void *b)
{
    const Keep *x = a, *y = b;
    if (x->dev != y->dev)
        return x->dev < y->dev ? -1 : 1;
    return x->ino < y->ino ? -1 : x->ino > y->ino;
}

static bool keep_has(const KeepSet *k, const struct stat *st)
{
    Keep e = { st->st_dev, st->st_ino };
    return k->inodes.nb && bsearch(&e, k->inodes.data, k->inodes.nb, sizeof(e), cmp_keep);
}

static void keep_free(KeepSet *k)
{
    ours_free(&k->lib);
    rx_array_free(&k->inodes);
    free(k->lib_root);
}

/* A library file: its inode, and its stick path when it is on the stick
 * (`dest` is the stick's root resolved, `n` its length). */
static int keep_add(const Ctx *c, KeepSet *k, const char *dest, size_t n, const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode))
        return 0;
    Keep e = { st.st_dev, st.st_ino };
    if (!rx_array_push(&k->inodes, &e))
        return RX_ERROR(ENOMEM);
    if (!dest || st.st_dev != c->dev)
        return 0;
    char *real = realpath(path, NULL);
    int ret = 0;
    if (real && !strncmp(real, dest, n) && real[n] == '/')
        ret = ours_add(&k->lib, real + n);
    free(real);
    return ret;
}

static int collect_keep(Ctx *c, const RXPlan *plan, KeepSet *k)
{
    rx_array_init(&k->inodes, sizeof(Keep));
    k->lib_root = c->lib->root ? realpath(c->lib->root, NULL) : NULL;
    char *dest = realpath(c->dest, NULL);
    size_t n = dest ? strlen(dest) : 0;
    while (n && dest[n - 1] == '/')
        n--;
    int ret = 0;
    for (int i = 0; i < plan->nb_tracks && ret >= 0; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        struct stat st;
        if (rx_stick_stat(c->root, t->usb_path, &st) >= 0 && S_ISREG(st.st_mode)) {
            Keep e = { st.st_dev, st.st_ino };
            if (!rx_array_push(&k->inodes, &e))
                ret = RX_ERROR(ENOMEM);
        }
        if (ret >= 0)
            ret = keep_add(c, k, dest, n, t->source);
    }
    for (int i = 0; i < c->lib->nb_tracks && ret >= 0; i++)
        ret = keep_add(c, k, dest, n, c->lib->tracks[i]->path);
    free(dest);
    if (k->inodes.nb)
        qsort(k->inodes.data, k->inodes.nb, sizeof(Keep), cmp_keep);
    ours_sort(&k->lib);
    return ret;
}

/* Whether the file `st` is, now, the one a path in `o` under `key` names. */
static bool same_as(const Ctx *c, const Ours *o, const char *key, const struct stat *st)
{
    for (int i = ours_find(o, key); i >= 0 && i < o->nb && !strcmp(o->v[i].key, key); i++) {
        struct stat s;
        if (rx_stick_stat(c->root, o->v[i].path, &s) >= 0 && s.st_dev == st->st_dev && s.st_ino == st->st_ino)
            return true;
    }
    return false;
}

/* Whether prune keeps the file `name` in the open directory `dir` (the
 * stick path `stick_path`): a file the plan writes or a library file on
 * the stick, the file system resolving their spelling and the file found
 * back to back. The file is held open while the other is looked up, so
 * its inode number cannot change in between; on a case-sensitive staging
 * directory a name that differs from the plan's in case alone is another
 * file. A file that cannot be looked at is not prune's to judge. */
static bool keep_file(const Ctx *c, const Ours *o, const KeepSet *k, int dir, const char *name,
                      const char *stick_path)
{
    char *key = rx_plan_fold_case(stick_path);
    int fd = openat(dir, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    bool keep = !key || fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_dev != c->dev ||
                same_as(c, o, key, &st) || same_as(c, &k->lib, key, &st) || keep_has(k, &st);
    if (fd >= 0)
        close(fd);
    free(key);
    return keep;
}

/* Whether the open directory `fd` is the library's root (looked up now,
 * with `fd` held open). */
static bool is_lib_root(const KeepSet *k, int fd)
{
    struct stat a, b;
    return k->lib_root && fstat(fd, &a) == 0 && stat(k->lib_root, &b) == 0 && a.st_dev == b.st_dev &&
           a.st_ino == b.st_ino;
}

/* Files under the open directory `fd` (the stick's `stick_dir`; this takes
 * the descriptor over) that this export does not write. Nothing through a
 * link or on another file system mounted on the stick is looked at. */
static int find_extra(const Ctx *c, const Ours *o, const KeepSet *k, int fd, const char *stick_dir,
                      RXArray *extra)
{
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return 0;
    }
    int ret = 0;
    struct dirent *e;
    while (ret >= 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *sub = rx_asprintf("%s/%s", stick_dir, e->d_name);
        struct stat st;
        if (!sub) {
            ret = RX_ERROR(ENOMEM);
        } else if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 || st.st_dev != c->dev) {
            /* Gone meanwhile, unreadable, or another file system: not ours
             * to judge. */
        } else if (S_ISDIR(st.st_mode)) {
            int sfd = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sfd >= 0 && !is_lib_root(k, sfd))
                ret = find_extra(c, o, k, sfd, sub, extra);
            else if (sfd >= 0)
                close(sfd);
        } else if (S_ISREG(st.st_mode) && !keep_file(c, o, k, dirfd(d), e->d_name, sub)) {
            if (rx_array_push(extra, &sub))
                sub = NULL;
            else
                ret = RX_ERROR(ENOMEM);
        }
        free(sub);
    }
    closedir(d);
    return ret;
}

/* Deletes a file find_extra() listed unless, looked at again now, it is
 * one to keep: 1 deleted, 0 kept, or an error. */
static int delete_extra(const Ctx *c, const Ours *o, const KeepSet *k, const char *stick_path)
{
    const char *base = strrchr(stick_path, '/') + 1;
    char *parent = rx_strndup(stick_path, (size_t)(base - 1 - stick_path));
    int dir = parent ? rx_stick_open(c->root, parent, O_PATH | O_DIRECTORY, 0) : RX_ERROR(ENOMEM);
    free(parent);
    if (dir < 0)
        return dir;
    int ret = 0;
    if (!keep_file(c, o, k, dir, base, stick_path))
        ret = unlinkat(dir, base, 0) < 0 ? RX_ERROR(errno) : 1;
    close(dir);
    return ret;
}

/* Removes the directories left empty under the open directory `fd` (not
 * itself; this takes the descriptor over); true when nothing is left in
 * it. */
static bool prune_empty(int fd, dev_t dev)
{
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return false;
    }
    bool empty = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        struct stat st;
        bool removed = false;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode) &&
            st.st_dev == dev) {
            int sub = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            removed = sub >= 0 && prune_empty(sub, dev) && unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR) == 0;
        }
        if (!removed)
            empty = false;
    }
    closedir(d);
    return empty;
}

/* ---------------------------------------------------------------- checks */

/* The directories the export writes into and prunes: real directories
 * on the stick, never links out of it. */
static const char *const stick_dirs[] = {
    "/Contents", "/PIONEER", "/PIONEER/rekordbox", "/PIONEER/USBANLZ", "/PIONEER/Artwork", "/PIONEER/rxbox",
};

static bool stick_dir_ok(const Ctx *c, const char *stick_dir)
{
    struct stat st;
    int ret = rx_stick_stat(c->root, stick_dir, &st);
    return ret < 0 ? ret == RX_ERROR(ENOENT) : S_ISDIR(st.st_mode);
}

static int64_t round_up(int64_t n, int64_t cluster)
{
    return n <= 0 ? 0 : (n + cluster - 1) / cluster * cluster;
}

/* One file written over its old copy in place: only the growth counts,
 * and a smaller one frees space. */
static void account(int64_t new_size, int64_t old_size, int64_t cluster, int64_t *need)
{
    int64_t n = round_up(new_size, cluster), o = round_up(old_size, cluster);
    *need += RX_MAX(n - o, 0);
}

static int64_t stick_size(int root, const char *stick_path)
{
    struct stat st;
    return root >= 0 && rx_stick_stat(root, stick_path, &st) >= 0 && S_ISREG(st.st_mode) ? st.st_size : 0;
}

int64_t rx_export_space_needed(const RXPlan *plan, const char *dest, int64_t cluster)
{
    cluster = RX_MAX(cluster, 1);
    int64_t need = DB_RESERVE;
    /* Without the stick, everything counts as new. */
    int root = rx_stick_root(dest);
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        if (!same_file(root, t->source, t->usb_path))
            account(t->file_size, stick_size(root, t->usb_path), cluster, &need);
        /* The analysis files, counted as if rewritten: the waveform details
         * take 1 + 2 bytes (.EXT) and 3 bytes (.2EX) a column at 150
         * columns a second (docs/anlz.md 4.9-4.12), the beats 8 bytes each
         * (4.3; up to 5 a second here), the previews, PVBR, PVB2 and cue
         * lists a few KiB a file. */
        double dur = RX_MAX(t->duration, 0.0);
        int64_t cols = (int64_t)ceil(dur * 150.0);
        int64_t sizes[3] = {
            12288 + (int64_t)(40 * dur) + 120 * (int64_t)t->nb_cues,
            20480 + 3 * cols + 200 * (int64_t)t->nb_cues,
            8192 + 3 * cols,
        };
        const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
        for (int k = 0; k < 3; k++) {
            char *p = t->anlz_dir ? rx_plan_anlz_path(t, ext[k]) : NULL;
            account(sizes[k], p ? stick_size(root, p) : 0, cluster, &need);
            free(p);
        }
    }
    for (int i = 0; i < plan->nb_artworks; i++) {
        const RXPlanArtwork *a = &plan->artworks[i];
        char *dir = rx_plan_artwork_dir(a->id);
        const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
        for (int k = 0; dir && k < 4; k++) {
            char *p = rx_asprintf(fmt[k], dir, a->id);
            account((int64_t)(k & 1 ? a->size240 : a->size80), p ? stick_size(root, p) : 0, cluster, &need);
            free(p);
        }
        free(dir);
    }
    if (root >= 0)
        close(root);
    return need;
}

/* Names longer than any export.pdb row holds (docs/pdb.md 2.6: a row and
 * its index entry within one page), reported by name. */
static void report_long_names(Ctx *c, const char *what, const RXPlanName *v, int nb)
{
    for (int i = 0; i < nb; i++) {
        uint8_t *enc;
        size_t n;
        int ret = rx_pdb_string_encode(v[i].name, &enc, &n);
        if (ret >= 0)
            free(enc);
        if (ret < 0 || n > 4000)
            reportf(c, "check", "%s name too long for export.pdb (%zu bytes): %.60s...", what,
                    ret < 0 ? strlen(v[i].name) : n, v[i].name);
    }
}

/* Both databases built now, so nothing is written for a library they
 * cannot hold; write_databases() puts these bytes on the stick. */
static int build_databases(Ctx *c, const RXPlan *plan)
{
    const RXConfig *cfg = &c->lib->cfg;
    int ret = 0;
    if (cfg->export_pdb || !cfg->export_onelibrary) {
        if ((ret = rx_pdb_build(plan, &c->pdb, &c->pdb_size)) < 0) {
            reportf(c, "check", "export.pdb cannot be built: %s", strerror(-ret));
            report_long_names(c, "artist", plan->artists, plan->nb_artists);
            report_long_names(c, "album", plan->albums, plan->nb_albums);
            report_long_names(c, "genre", plan->genres, plan->nb_genres);
            report_long_names(c, "label", plan->labels, plan->nb_labels);
            report_long_names(c, "key", plan->keys, plan->nb_keys);
            for (int i = 0; i < plan->nb_playlists; i++) {
                RXPlanName n = { plan->playlists[i].id, plan->playlists[i].name, 0 };
                report_long_names(c, "playlist", &n, 1);
            }
            for (int i = 0; i < plan->nb_tags; i++) {
                RXPlanName n = { plan->tags[i].id, plan->tags[i].name, 0 };
                report_long_names(c, "My Tag", &n, 1);
            }
            return ret;
        }
        bool tags = false;
        for (int i = 0; i < plan->nb_tags; i++)
            tags |= !plan->tags[i].is_category;
        if (tags && (ret = rx_pdb_ext_build(plan, &c->ext, &c->ext_size)) < 0) {
            reportf(c, "check", "exportExt.pdb cannot be built: %s", strerror(-ret));
            return ret;
        }
    }
    if (cfg->export_onelibrary && (ret = rx_onelibrary_build(plan, &c->olib, &c->olib_size)) < 0)
        reportf(c, "check", "exportLibrary.db cannot be built: %s", strerror(-ret));
    return ret;
}

/* A file the export writes, looked up before anything is written: a link
 * or another file system on its way (rx_stick_open's rules) is refused
 * now rather than half way through. `bad` counts them; the first few are
 * named. */
static void check_path(Ctx *c, const char *stick_path, int *bad)
{
    int fd = rx_stick_open(c->root, stick_path, O_PATH | O_NOFOLLOW, 0);
    struct stat st;
    int ret = fd;
    if (fd >= 0) {
        ret = fstat(fd, &st) < 0 ? RX_ERROR(errno) : S_ISLNK(st.st_mode) ? RX_ERROR(ELOOP) : 0;
        close(fd);
    }
    if (ret != RX_ERROR(ELOOP) && ret != RX_ERROR(EXDEV))
        return;
    if ((*bad)++ < 10)
        reportf(c, "check", "%s%s: %s; not writing through it", c->dest, stick_path,
                ret == RX_ERROR(EXDEV) ? "another file system is mounted on the way" : "a symbolic link on the way");
}

static int check_paths(Ctx *c, const RXPlan *plan)
{
    static const char *const fixed[] = {
        "/PIONEER/MYSETTING.DAT", "/PIONEER/MYSETTING2.DAT", "/PIONEER/DJMMYSETTING.DAT", "/PIONEER/DEVSETTING.DAT",
        "/PIONEER/rekordbox/export.pdb", "/PIONEER/rekordbox/exportExt.pdb", "/PIONEER/rekordbox/exportLibrary.db",
        "/PIONEER/rekordbox/exportLibrary.db-wal", "/PIONEER/rekordbox/exportLibrary.db-shm", RX_SYNC_PATH,
    };
    int bad = 0;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(fixed); i++)
        check_path(c, fixed[i], &bad);
    const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        check_path(c, t->usb_path, &bad);
        for (int k = 0; k < 3; k++) {
            char *p = rx_plan_anlz_path(t, ext[k]);
            if (!p)
                return RX_ERROR(ENOMEM);
            check_path(c, p, &bad);
            free(p);
        }
    }
    const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
    for (int i = 0; i < plan->nb_artworks; i++) {
        uint32_t id = plan->artworks[i].id;
        char *dir = rx_plan_artwork_dir(id);
        for (int k = 0; k < 4; k++) {
            char *p = dir ? rx_asprintf(fmt[k], dir, id) : NULL;
            if (!p) {
                free(dir);
                return RX_ERROR(ENOMEM);
            }
            check_path(c, p, &bad);
            free(p);
        }
        free(dir);
    }
    if (bad > 10)
        reportf(c, "check", "and %d more paths through a link or another file system", bad - 10);
    return bad ? RX_ERROR(ELOOP) : 0;
}

static int checks(Ctx *c, const RXPlan *plan)
{
    int ret = 0;
    char fs[64];

    for (size_t i = 0; i < RX_ARRAY_ELEMS(stick_dirs); i++) {
        if (!stick_dir_ok(c, stick_dirs[i])) {
            reportf(c, "check", "%s%s is not a directory on the stick (a link?); not writing through it",
                    c->dest, stick_dirs[i]);
            ret = ret ? ret : RX_ERROR(ENOTDIR);
        }
    }
    int r = check_paths(c, plan);
    if (r < 0)
        ret = ret ? ret : r;

    if (!rx_stick_is_mounted(c->dest))
        reportf(c, "check", "warning: %s is not a mount point; writing into a directory", c->dest);
    else if (rx_stick_fs_type(c->dest, fs, sizeof(fs)) >= 0 && strcmp(fs, "vfat") && strcmp(fs, "exfat") &&
             strcmp(fs, "msdos") && strcmp(fs, "fat"))
        /* FAT32 on MBR is what every player reads; exFAT only the newer
         * ones (docs/pdb.md G14). */
        reportf(c, "check", "warning: %s is %s; players read FAT32 (and exFAT on the newer ones)",
                c->dest, fs);

    /* Track rows that do not fit a DeviceSQL page (docs/pdb.md 2.6),
     * named rather than failing half way. */
    for (int i = 0; i < plan->nb_tracks; i++) {
        int over = rx_pdb_track_row_overflow(&plan->tracks[i], plan);
        if (over > 0) {
            char name[512];
            rx_track_display(plan->tracks[i].track, name, sizeof(name));
            reportf(c, "check", "too much text for one export.pdb page (%d bytes over): %s", over, name);
            ret = ret ? ret : RX_ERROR(EFBIG);
        }
    }

    /* Stick paths on the stick and unique as FAT and exFAT compare names
     * (rx_plan_fold_case). */
    Ours o = { 0 };
    for (int i = 0; i < plan->nb_tracks; i++) {
        if (!rx_stick_path_safe(plan->tracks[i].usb_path)) {
            reportf(c, "check", "not a path on the stick: %s", plan->tracks[i].usb_path);
            ret = ret ? ret : RX_ERROR(EINVAL);
        }
        if (ours_add(&o, plan->tracks[i].usb_path) < 0)
            ret = ret ? ret : RX_ERROR(ENOMEM);
    }
    ours_sort(&o);
    for (int i = 1; i < o.nb; i++) {
        if (!strcmp(o.v[i].key, o.v[i - 1].key)) {
            reportf(c, "check", "two tracks map to one file on the stick: %s and %s", o.v[i - 1].path,
                    o.v[i].path);
            ret = ret ? ret : RX_ERROR(EEXIST);
        }
    }
    ours_free(&o);

    /* What a player left on the stick and the library does not have yet
     * would be lost: export.pdb and exportLibrary.db are rebuilt, the ANLZ
     * cue lists rewritten. */
    RXStickState s;
    r = rx_stick_read(c->lib, c->dest, &s);
    if (r >= 0) {
        RXSyncItem *items;
        int nb;
        if ((r = rx_stick_diff(c->lib, &s, &items, &nb)) >= 0) {
            if (nb && !c->opt->ignore_unsynced) {
                reportf(c, "check", "stick has %d unsynced change%s (cues, history or settings a player "
                        "wrote); pull them first or export ignoring them", nb, nb == 1 ? "" : "s");
                ret = ret ? ret : RX_ERROR(EBUSY);
            } else if (nb) {
                reportf(c, "check", "warning: overwriting %d unsynced change%s on the stick", nb,
                        nb == 1 ? "" : "s");
            }
            free(items);
        }
        rx_stick_state_free(&s);
    }
    /* Not knowing is not the same as nothing to lose. */
    if (r < 0 && !c->opt->ignore_unsynced) {
        reportf(c, "check", "cannot read what players left on the stick (%s)", strerror(-r));
        ret = ret ? ret : r;
    }

    if (ret >= 0 && (r = build_databases(c, plan)) < 0)
        ret = r;

    struct statvfs vfs;
    int64_t cluster = statvfs(c->dest, &vfs) == 0 && vfs.f_frsize ? (int64_t)vfs.f_frsize : 4096;
    int64_t need = rx_export_space_needed(plan, c->dest, cluster);
    int64_t free_bytes = rx_fs_free_bytes(c->dest);
    char need_text[32], free_text[32];
    size_text(need, need_text, sizeof(need_text));
    size_text(free_bytes, free_text, sizeof(free_text));
    if (free_bytes >= 0 && free_bytes < need) {
        reportf(c, "check", "not enough space on %s: %s free, %s needed", c->dest, free_text, need_text);
        ret = ret ? ret : RX_ERROR(ENOSPC);
    } else if (free_bytes >= 0) {
        reportf(c, "check", "takes %s on the stick (%s free)", need_text, free_text);
    }
    return ret;
}

/* ------------------------------------------------------------- writing */

/* The analysis files a player reads (ANLZ0000.DAT, .EXT, .2EX: the grid,
 * cues and waveforms, docs/anlz.md) are built from what the library holds;
 * nothing is analysed here. A track's step names it when a file of its
 * was written and carries no item otherwise, so the interface counts
 * rewrites, not tracks. */
static int write_anlz(Ctx *c, const RXPlan *plan)
{
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        char name[512];
        rx_track_display(t->track, name, sizeof(name));
        RXWaveforms w = { 0 };
        bool have = rx_library_waveforms_load(c->lib, t->id, &w) >= 0;
        if (!have)
            reportf(c, "analysis files", "no waveforms for %s (analyse it to add them)", name);
        RXAnlzFiles f;
        int ret = rx_anlz_build(t, have ? &w : NULL, &f);
        if (have)
            rx_waveforms_free(&w);
        if (ret < 0)
            return ret;
        /* The buffers go to the jobs. */
        const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
        uint8_t *data[3] = { f.dat, f.ext, f.ex2 };
        size_t size[3] = { f.dat_size, f.ext_size, f.ex2_size };
        int written = 0;
        for (int k = 0; k < 3; k++) {
            char *p = ret >= 0 ? rx_plan_anlz_path(t, ext[k]) : NULL;
            if (p)
                ret = put_file(c, p, data[k], size[k], true, &written);
            else
                free(data[k]);
            if (!p && ret >= 0)
                ret = RX_ERROR(ENOMEM);
            free(p);
        }
        if (ret < 0)
            return ret;
        if (!progress(c, "analysis files", i + 1, plan->nb_tracks, written ? name : NULL))
            return RX_ERROR(ECANCELED);
    }
    return puts_wait(c, "analysis files");
}

/* a<id>.jpg (80 px) and a<id>_m.jpg (240 px) for export.pdb, the same as
 * b<id>... for OneLibrary's image paths (docs/onelibrary.md 3.3). */
static int write_artwork(Ctx *c, const RXPlan *plan)
{
    for (int i = 0; i < plan->nb_artworks; i++) {
        const RXPlanArtwork *a = &plan->artworks[i];
        char *dir = rx_plan_artwork_dir(a->id);
        if (!dir)
            return RX_ERROR(ENOMEM);
        int ret = 0, written = 0;
        const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
        for (int k = 0; k < 4 && ret >= 0; k++) {
            char *p = rx_asprintf(fmt[k], dir, a->id);
            /* The plan's bytes, which outlive the jobs. */
            ret = p ? put_file(c, p, (uint8_t *)(k & 1 ? a->jpeg240 : a->jpeg80),
                               k & 1 ? a->size240 : a->size80, false, &written)
                    : RX_ERROR(ENOMEM);
            free(p);
        }
        free(dir);
        if (ret < 0)
            return ret;
        if (!progress(c, "artwork", i + 1, plan->nb_artworks, written ? a->path80 : NULL))
            return RX_ERROR(ECANCELED);
    }
    return puts_wait(c, "artwork");
}

static int write_databases(Ctx *c, const RXPlan *plan)
{
    const RXConfig *cfg = &c->lib->cfg;
    int ret;

    /* The old sync record first: from here on it would describe databases
     * the stick no longer has, and the sync would trust its ids. Without
     * one the sync only proposes additions. */
    ret = rx_stick_unlink(c->root, RX_SYNC_PATH, 0);
    if (ret < 0 && ret != RX_ERROR(ENOENT)) {
        reportf(c, "database", "cannot replace %s%s: %s", c->dest, RX_SYNC_PATH, strerror(-ret));
        return ret;
    }

    RXSettingsFile files[4];
    int nf = 0;
    if ((ret = rx_settings_from_config(cfg, files, &nf)) < 0) {
        reportf(c, "database", "the My Settings in the library are invalid; see `rxbox settings`");
        return ret;
    }
    for (int i = 0; i < nf; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/%s", files[i].name);
        progress(c, "database", i + 1, nf + 3, path);
        if (ret >= 0)
            ret = write_file(c, path, files[i].data, files[i].size);
        free(files[i].data);
    }
    if (ret < 0)
        return ret;

    /* The databases last (built by the checks). Both by default: one stick
     * works on every player generation (docs/OVERVIEW.md 1); a library left
     * out is removed so no player shows a stale one (docs/pdb.md 1.1, G12). */
    if (c->pdb) {
        progress(c, "database", nf + 1, nf + 3, "export.pdb");
        if ((ret = write_file(c, "/PIONEER/rekordbox/export.pdb", c->pdb, c->pdb_size)) < 0)
            return ret;
        if (c->ext) {
            progress(c, "database", nf + 2, nf + 3, "exportExt.pdb");
            if ((ret = write_file(c, "/PIONEER/rekordbox/exportExt.pdb", c->ext, c->ext_size)) < 0)
                return ret;
        } else {
            remove_file(c, "/PIONEER/rekordbox/exportExt.pdb");
        }
    } else {
        remove_file(c, "/PIONEER/rekordbox/export.pdb");
        remove_file(c, "/PIONEER/rekordbox/exportExt.pdb");
    }
    remove_file(c, "/PIONEER/rekordbox/exportLibrary.db-wal");
    remove_file(c, "/PIONEER/rekordbox/exportLibrary.db-shm");
    if (c->olib) {
        progress(c, "database", nf + 3, nf + 3, "exportLibrary.db");
        if ((ret = write_file(c, "/PIONEER/rekordbox/exportLibrary.db", c->olib, c->olib_size)) < 0)
            return ret;
    } else {
        remove_file(c, "/PIONEER/rekordbox/exportLibrary.db");
    }

    /* The sync record: what the stick holds now, so later changes can be
     * told to be a player's, and whose library wrote it. */
    RXSyncRecord r;
    if ((ret = rx_sync_record_from_plan(plan, &r)) < 0)
        return ret;
    if ((ret = rx_sync_library_key(c->lib, r.library)) >= 0) {
        rx_sync_record_add_settings(&r, c->root);
        ret = rx_sync_record_write(c->root, &r);
    }
    rx_sync_record_free(&r);
    return ret;
}

static int prune(Ctx *c, const RXPlan *plan, bool dry)
{
    Ours o = { 0 };
    KeepSet k = { 0 };
    RXArray extra;
    rx_array_init(&extra, sizeof(char *));
    int ret = collect_ours(plan, &o);
    if (ret >= 0)
        ret = collect_keep(c, plan, &k);
    /* Through no link (checks() refused them already; this keeps it so). */
    const char *roots[3] = { "/Contents", "/PIONEER/USBANLZ", "/PIONEER/Artwork" };
    for (int i = 0; i < 3 && ret >= 0; i++) {
        int fd = rx_stick_open(c->root, roots[i], O_RDONLY | O_DIRECTORY, 0);
        if (fd == RX_ERROR(ENOENT))
            continue;
        if (fd < 0) {
            reportf(c, "prune", "cannot look in %s%s: %s", c->dest, roots[i], strerror(-fd));
            ret = fd;
            break;
        }
        ret = find_extra(c, &o, &k, fd, roots[i], &extra);
    }
    for (size_t i = 0; i < extra.nb && ret >= 0; i++) {
        const char *p = *(char **)rx_array_at(&extra, i);
        /* Decided again as it is deleted, the file system looking up both
         * names at that moment. */
        int r = 0;
        if (!dry && c->opt->prune && !(r = delete_extra(c, &o, &k, p))) {
            rx_info("export: %s on %s is a file this export or the library keeps; not deleted", p, c->dest);
            continue;
        }
        if (!progress(c, "prune", (int)i + 1, (int)extra.nb, p)) {
            ret = RX_ERROR(ECANCELED);
            break;
        }
        if (r < 0)
            reportf(c, "prune", "cannot remove %s: %s", p, strerror(-r));
    }
    if (!dry && c->opt->prune && ret >= 0) {
        for (int i = 0; i < 3; i++) {
            int fd = rx_stick_open(c->root, roots[i], O_RDONLY | O_DIRECTORY, 0);
            if (fd >= 0)
                prune_empty(fd, c->dev);
        }
    } else if (!dry && extra.nb && ret >= 0) {
        reportf(c, "prune", "%zu file%s on the stick %s not part of this export (prune to remove)",
                extra.nb, extra.nb == 1 ? "" : "s", extra.nb == 1 ? "is" : "are");
    }
    for (size_t i = 0; i < extra.nb; i++)
        free(*(char **)rx_array_at(&extra, i));
    rx_array_free(&extra);
    keep_free(&k);
    ours_free(&o);
    return ret;
}

/* Every file a real run would write (or keep, finding it on the stick
 * unchanged), in the order it would. */
static void dry_run_report(Ctx *c, const RXPlan *plan)
{
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        progress(c, same_file(c->root, t->source, t->usb_path) ? "skip" : "copy", i + 1, plan->nb_tracks,
                 t->usb_path);
    }
    RXSettingsFile files[4];
    int nf = 0;
    if (rx_settings_from_config(&c->lib->cfg, files, &nf) < 0)
        nf = 0;
    int total = 3 * plan->nb_tracks + 4 * plan->nb_artworks + nf + !!c->pdb + !!c->ext + !!c->olib + 1;
    int n = 0;
    const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
    for (int i = 0; i < plan->nb_tracks; i++) {
        for (int k = 0; k < 3; k++) {
            char *p = rx_plan_anlz_path(&plan->tracks[i], ext[k]);
            progress(c, "rewrite", ++n, total, p);
            free(p);
        }
    }
    const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
    for (int i = 0; i < plan->nb_artworks; i++) {
        uint32_t id = plan->artworks[i].id;
        char *dir = rx_plan_artwork_dir(id);
        for (int k = 0; dir && k < 4; k++) {
            char *p = rx_asprintf(fmt[k], dir, id);
            progress(c, "rewrite", ++n, total, p);
            free(p);
        }
        free(dir);
    }
    for (int i = 0; i < nf; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/%s", files[i].name);
        progress(c, "rewrite", ++n, total, path);
        free(files[i].data);
    }
    if (c->pdb)
        progress(c, "rewrite", ++n, total, "/PIONEER/rekordbox/export.pdb");
    if (c->ext)
        progress(c, "rewrite", ++n, total, "/PIONEER/rekordbox/exportExt.pdb");
    if (c->olib)
        progress(c, "rewrite", ++n, total, "/PIONEER/rekordbox/exportLibrary.db");
    progress(c, "rewrite", ++n, total, RX_SYNC_PATH);
}

/* ------------------------------------------------------------ transcodes */

typedef struct Transcode {
    const RXTrack *t;
    char          *cache;
    int            max_rate;
    int            ret;
    RXMailbox     *done;
    RXCancel      *cancel;
    int           *finished;
} Transcode;

/* On a pool worker: into a temporary name, renamed into the cache when
 * complete, so a cancelled or failed one leaves nothing that looks made. */
static void transcode_job(void *arg)
{
    Transcode *x = arg;
    char *tmp = rx_asprintf("%s.tmp.flac", x->cache);
    if (rx_cancelled(x->cancel))
        x->ret = RX_ERROR(ECANCELED);
    else if (!tmp)
        x->ret = RX_ERROR(ENOMEM);
    else if ((x->ret = rx_transcode_flac(x->t->path, tmp, x->max_rate)) >= 0 && rename(tmp, x->cache) < 0)
        x->ret = RX_ERROR(errno);
    if (tmp && x->ret < 0)
        unlink(tmp);
    free(tmp);
    rx_mailbox_post(x->done, x);
    __atomic_add_fetch(x->finished, 1, __ATOMIC_RELEASE);
}

/* The transcodes the plan needs and the cache lacks: listed in a dry run,
 * made now otherwise, on half the CPUs (each is single-threaded). */
static int transcodes(Ctx *c, RXTrack **tracks, int nb_tracks, bool compat)
{
    Transcode *x = calloc((size_t)RX_MAX(nb_tracks, 1), sizeof(*x));
    if (!x)
        return RX_ERROR(ENOMEM);
    int n = 0, ret = 0;
    for (int i = 0; i < nb_tracks && ret >= 0; i++) {
        char *cache;
        int max_rate = 0;
        int r = rx_plan_transcode_path(c->lib, tracks[i], compat, !c->opt->dry_run, &cache, &max_rate);
        if (r < 0)
            ret = r;
        else if (r > 0 && !rx_plan_transcode_fresh(tracks[i], cache))
            x[n++] = (Transcode) { .t = tracks[i], .cache = cache, .max_rate = max_rate };
        else
            free(cache);
    }

    if (ret >= 0 && c->opt->dry_run) {
        for (int i = 0; i < n; i++) {
            char name[512];
            rx_track_display(x[i].t, name, sizeof(name));
            progress(c, "transcode", i + 1, n, name);
        }
    } else if (ret >= 0 && n) {
        RXThreadPool *pool = NULL;
        RXMailbox *mb = NULL;
        RXCancel cancel = { 0 };
        int finished = 0;
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int workers = (int)RX_CLAMP(cpus / 2, 1, n);
        if ((ret = rx_mailbox_create(&mb)) >= 0 && (ret = rx_pool_create(&pool, workers)) >= 0) {
            int queued = 0;
            for (int i = 0; i < n && ret >= 0; i++) {
                x[i].done = mb;
                x[i].cancel = &cancel;
                x[i].finished = &finished;
                if ((ret = rx_pool_submit(pool, transcode_job, &x[i])) >= 0)
                    queued++;
            }
            /* Progress from this thread as each one finishes. */
            for (int k = 0; k < queued;) {
                Transcode *d = rx_mailbox_wait(mb, 250);
                if (!d) {
                    /* All done but a report lost (the mailbox was out of
                     * memory): nothing more will come. */
                    if (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < queued)
                        continue;
                    if (!(d = rx_mailbox_take(mb)))
                        break;
                }
                char name[512];
                rx_track_display(d->t, name, sizeof(name));
                if (!progress(c, "transcode", ++k, n, name))
                    rx_cancel(&cancel);
            }
            rx_pool_wait(pool);
            for (int i = 0; i < queued && ret >= 0; i++) {
                if (x[i].ret < 0 && x[i].ret != RX_ERROR(ECANCELED)) {
                    char name[512];
                    rx_track_display(x[i].t, name, sizeof(name));
                    reportf(c, "transcode", "cannot transcode %s: %s", name, strerror(-x[i].ret));
                    ret = x[i].ret;
                }
            }
            if (ret >= 0 && rx_cancelled(&cancel))
                ret = RX_ERROR(ECANCELED);
        }
        if (pool)
            rx_pool_destroy(pool);
        if (mb)
            rx_mailbox_destroy(mb);
    }
    for (int i = 0; i < n; i++)
        free(x[i].cache);
    free(x);
    return ret;
}

int rx_export(RXLibrary *lib, const char *dest, const RXExportOptions *opt_in, rx_progress_cb cb,
              void *opaque)
{
    RXExportOptions defaults;
    rx_export_options_default(&defaults);
    const RXExportOptions *opt = opt_in ? opt_in : &defaults;
    Ctx c = {
        .lib    = lib,
        .dest   = dest,
        .root   = -1,
        .opt    = opt,
        .cb     = cb,
        .opaque = opaque,
    };
    rx_array_init(&c.puts, sizeof(PutJob *));
    if (!rx_dir_exists(dest)) {
        reportf(&c, "check", "%s is not a directory (mount the stick first)", dest);
        return RX_ERROR(ENOTDIR);
    }

    RXPlaylist **all = NULL;
    RXTrack **tracks = NULL;
    int nb_tracks = 0, copied = 0;
    RXPlan plan = { 0 };
    struct stat st;
    int ret = c.root = rx_stick_root(dest);
    if (ret >= 0)
        ret = fstat(c.root, &st) < 0 ? RX_ERROR(errno) : 0;
    if (ret < 0) {
        reportf(&c, "check", "cannot open %s: %s", dest, strerror(-ret));
        goto out;
    }
    c.dev = st.st_dev;

    /* Every playlist unless some were chosen. */
    const RXPlaylist *const *pls = opt->playlists;
    int nb_pls = opt->nb_playlists;
    if (!pls) {
        int n = rx_library_playlists_flat(lib, NULL, 0);
        all = calloc((size_t)RX_MAX(n, 1), sizeof(*all));
        if (!all) {
            ret = RX_ERROR(ENOMEM);
            goto out;
        }
        nb_pls = rx_library_playlists_flat(lib, all, RX_MAX(n, 0));
    }

    ret = select_tracks(&c, all ? (const RXPlaylist *const *)all : pls, nb_pls, &tracks, &nb_tracks);
    if (ret < 0)
        goto out;

    /* Transcodes first: the plan probes what is copied. The option alone
     * decides: the setting is only its default, which the callers (the
     * dialog's checkbox, --compat and --no-compat) start from, so an
     * export asked for without it gets none. */
    bool compat = opt->compat_transcode;
    if ((ret = transcodes(&c, tracks, nb_tracks, compat)) < 0 || c.cancelled)
        goto out;
    if ((ret = rx_plan_build_cb(lib, tracks, nb_tracks, opt->playlists,
                                opt->playlists ? opt->nb_playlists : 0, opt->device_name, compat,
                                opt->dry_run, &plan, cb, opaque)) < 0)
        goto out;
    if (!opt->device_name && !lib->cfg.device_name) {
        char label[64];
        if (rx_stick_label(dest, label, sizeof(label)) >= 0) {
            free(plan.device_name);
            plan.device_name = rx_strdup(label);
        }
    }

    if ((ret = checks(&c, &plan)) < 0 || c.cancelled)
        goto out;

    if (opt->dry_run) {
        dry_run_report(&c, &plan);
        ret = prune(&c, &plan, true);
        goto out;
    }

    /* Audio: the bytes to copy first, for the rate and time left. */
    int64_t bytes = 0;
    for (int i = 0; i < plan.nb_tracks; i++) {
        const RXPlanTrack *t = &plan.tracks[i];
        if (!same_file(c.root, t->source, t->usb_path))
            bytes += RX_MAX(t->file_size, 0);
    }
    stats_start(&c, bytes);
    c.writing = true;
    for (int i = 0; i < plan.nb_tracks; i++) {
        const RXPlanTrack *t = &plan.tracks[i];
        if (same_file(c.root, t->source, t->usb_path)) {
            progress(&c, "skip", i + 1, plan.nb_tracks, t->usb_path);
        } else if (progress(&c, "copy", i + 1, plan.nb_tracks, t->usb_path)) {
            ret = copy_file(&c, t->source, t->usb_path, i + 1, plan.nb_tracks);
            if (ret < 0 && ret != RX_ERROR(ECANCELED))
                reportf(&c, "copy", "cannot copy %s: %s", t->source, strerror(-ret));
            copied++;
        }
        if (ret < 0)
            goto out;
        if (c.cancelled) {
            ret = RX_ERROR(ECANCELED);
            goto out;
        }
    }
    if ((ret = puts_wait(&c, "copy")) < 0)
        goto out;

    if ((ret = write_anlz(&c, &plan)) < 0 || (ret = write_artwork(&c, &plan)) < 0 ||
        (ret = write_databases(&c, &plan)) < 0)
        goto out;
    if ((ret = prune(&c, &plan, false)) < 0)
        goto out;
    /* All written: a cancel from here on changes nothing. */
    c.finished = true;

out:
    /* Jobs a failed or cancelled stage left: run out (before the sync),
     * their results dropped. */
    if (c.pool)
        rx_pool_destroy(c.pool);
    for (size_t i = 0; i < c.puts.nb; i++)
        put_free(*(PutJob **)rx_array_at(&c.puts, i));
    rx_array_free(&c.puts);
    /* The one sync, whenever a real run wrote anything: what the file
     * system still holds back (the audio went through already), so "done"
     * means on the stick, and a cancelled or failed run does not leave the
     * FAT, analysis files or a new export.pdb in the page cache for the
     * stick to be pulled from under. Its files are whole or cut short, and
     * the databases go last, so what is on the stick is consistent. */
    if (c.writing) {
        notify(&c, "flush", 1, 1, NULL);
        int r = rx_stick_sync(dest);
        if (r < 0) {
            notify(&c, "flush", 0, 0, "cannot write out %s: %s", dest, strerror(-r));
            ret = ret < 0 ? ret : r;
        } else if (!c.finished) {
            rx_info("export to %s %s: what it wrote is written out", dest,
                    c.cancelled || ret == RX_ERROR(ECANCELED) ? "cancelled" : "failed");
            notify(&c, "flush", 0, 0, "what was written so far is on %s", dest);
        }
    }
    if (c.finished && ret >= 0)
        notify(&c, "done", 0, 0, "%d tracks, %d copied, %d playlists, %d artworks", plan.nb_tracks, copied,
               plan.nb_playlists, plan.nb_artworks);
    else if (c.cancelled && ret >= 0)
        ret = RX_ERROR(ECANCELED);
    if (c.root >= 0)
        close(c.root);
    free(c.pdb);
    free(c.ext);
    free(c.olib);
    rx_plan_free(&plan);
    free(tracks);
    free(all);
    return ret;
}
