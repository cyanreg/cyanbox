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
 * every file it would write as "copy", "skip" or "rewrite". The plan reads
 * no file the library describes already (RX_PLAN_STORED): a file is read
 * once, as it is written, copied or encoded from its decoded samples
 * (rx_encode; RXStickFormat says which), and what the analysis files need
 * of it is learnt then.
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

#include <pthread.h>

#include "rxbox/audio.h"
#include "rxbox/thread.h"
#include "src/export/export_priv.h"
#include "src/formats/buf.h"
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
    /* Changes a player left on the stick that this export would lose
     * (checks); it refuses on them. */
    int                     unsynced;
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

/* --------------------------------------------------- the encoded record */

/* What the stick holds of the files the export encoded (Made), in
 * RX_ENCODED_PATH: a line each, the stick path, how it was encoded, the
 * source's size and time when it was, the file's size and frames, and
 * the FLAC's first frame and the frames its PVB2 tag names (the 400 a
 * player seeks by, docs/anlz.md 4.15). An encoded file is not its
 * source, so "the same size and time" (same_file) cannot tell it is
 * there already; this record can, and lets the analysis files be made
 * again without reading the file back. */
#define RX_ENCODED_PATH "/PIONEER/rxbox/encoded.txt"
#define SEEKS 400

typedef struct Made {
    char    *path;
    char     spec[48];
    int64_t  src_size, src_mtime, size, frames, first;
    int      nb;
    struct { int64_t sample, offset; int block; } seek[SEEKS];
} Made;

typedef struct Record {
    Made *v;
    int   nb, cap;
} Record;

static void record_free(Record *r)
{
    for (int i = 0; i < r->nb; i++)
        free(r->v[i].path);
    free(r->v);
    *r = (Record) { 0 };
}

static Made *record_add(Record *r)
{
    if (r->nb == r->cap) {
        int cap = r->cap ? r->cap * 2 : 64;
        Made *v = realloc(r->v, (size_t)cap * sizeof(*v));
        if (!v)
            return NULL;
        r->v = v;
        r->cap = cap;
    }
    Made *m = &r->v[r->nb++];
    *m = (Made) { 0 };
    return m;
}

static const Made *record_find(const Record *r, const char *path)
{
    for (int i = 0; path && i < r->nb; i++)
        if (!strcmp(r->v[i].path, path))
            return &r->v[i];
    return NULL;
}

static void spec_of(const RXPlanEncode *e, char out[48])
{
    static const char *const names[] = { "flac", "wav", "aiff", "mp3" };
    snprintf(out, 48, "%s/%d/%d/%d/%d", names[RX_CLAMP(e->format, 0, 3)], e->bits, e->max_rate,
             e->channels, e->exact);
}

/* The stick's record; empty when it has none or it does not read. */
static void record_read(int root, Record *r)
{
    *r = (Record) { 0 };
    uint8_t *data;
    size_t size;
    if (root < 0 || rx_stick_read_file(root, RX_ENCODED_PATH, &data, &size) < 0)
        return;
    char *text = realloc(data, size + 1);
    if (!text) {
        free(data);
        return;
    }
    text[size] = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *f[9];
        int nf = 0;
        for (char *t = line; nf < 9; nf++) {
            f[nf] = t;
            char *tab = strchr(t, '\t');
            if (!tab) {
                nf++;
                break;
            }
            *tab = 0;
            t = tab + 1;
        }
        Made *m;
        if (nf < 9 || f[0][0] != '/' || !(m = record_add(r)))
            continue;
        m->path = rx_strdup(f[0]);
        snprintf(m->spec, sizeof(m->spec), "%s", f[1]);
        m->src_size = strtoll(f[2], NULL, 10);
        m->src_mtime = strtoll(f[3], NULL, 10);
        m->size = strtoll(f[4], NULL, 10);
        m->frames = strtoll(f[5], NULL, 10);
        m->first = strtoll(f[6], NULL, 10);
        int nb = (int)RX_CLAMP(strtol(f[7], NULL, 10), 0, SEEKS);
        char *s = f[8];
        for (int k = 0; k < nb && s && *s; k++) {
            long long a, b;
            int bs, used = 0;
            if (sscanf(s, "%lld,%lld,%d%n", &a, &b, &bs, &used) != 3)
                break;
            m->seek[m->nb++] = (typeof(m->seek[0])) { a, b, bs };
            s += used;
            s += *s == ';';
        }
        if (!m->path)
            r->nb--;
    }
    free(text);
}

static int record_write(Ctx *c, const Record *r)
{
    RXBuf b = { 0 };
    for (int i = 0; i < r->nb; i++) {
        const Made *m = &r->v[i];
        char *line = rx_asprintf("%s\t%s\t%lld\t%lld\t%lld\t%lld\t%lld\t%d\t", m->path, m->spec,
                                 (long long)m->src_size, (long long)m->src_mtime, (long long)m->size,
                                 (long long)m->frames, (long long)m->first, m->nb);
        if (!line) {
            rx_buf_free(&b);
            return RX_ERROR(ENOMEM);
        }
        rx_buf_put(&b, line, strlen(line));
        free(line);
        for (int k = 0; k < m->nb; k++) {
            char e[80];
            int n = snprintf(e, sizeof(e), "%s%lld,%lld,%d", k ? ";" : "",
                             (long long)m->seek[k].sample, (long long)m->seek[k].offset, m->seek[k].block);
            rx_buf_put(&b, e, (size_t)n);
        }
        rx_buf_put(&b, "\n", 1);
    }
    if (b.err) {
        rx_buf_free(&b);
        return RX_ERROR(ENOMEM);
    }
    int ret = r->nb ? write_file(c, RX_ENCODED_PATH, b.data, b.len)
                    : rx_stick_unlink(c->root, RX_ENCODED_PATH, 0);
    rx_buf_free(&b);
    return ret == RX_ERROR(ENOENT) ? 0 : ret;
}

/* The frames PVB2 names (anlz.c, pvb2): for each of its 400 entries the
 * last frame starting at or before i * (total / 400). */
static void seeks_of(const RXFlacInfo *fi, Made *m)
{
    m->nb = 0;
    m->first = fi->first_frame_offset;
    int64_t step = fi->total_samples / SEEKS;
    int f = 0;
    for (int i = 0; i < SEEKS && fi->nb_frames; i++) {
        while (f + 1 < fi->nb_frames && fi->frames[f + 1].sample <= (int64_t)i * step)
            f++;
        m->seek[m->nb++] = (typeof(m->seek[0])) { fi->frames[f].sample, fi->frames[f].offset,
                                                   fi->frames[f].block };
    }
}

/* Back from the record: those frames, which give PVB2 what the file's
 * whole table would. */
static int flac_of(const Made *m, int rate, int channels, int bits, RXFlacInfo *fi)
{
    *fi = (RXFlacInfo) {
        .valid = true, .sample_rate = rate, .channels = channels, .bit_depth = bits,
        .total_samples = m->frames, .first_frame_offset = m->first,
    };
    fi->frames = calloc((size_t)RX_MAX(m->nb, 1), sizeof(*fi->frames));
    if (!fi->frames)
        return RX_ERROR(ENOMEM);
    for (int k = 0; k < m->nb; k++)
        fi->frames[k] = (typeof(fi->frames[0])) { m->seek[k].sample, m->seek[k].offset, m->seek[k].block };
    fi->nb_frames = m->nb;
    return 0;
}

/* Whether the stick holds the track's file as this export would write
 * it: a copy by same_file(), an encoded file by the record. */
static bool on_stick(int root, const Record *r, const RXPlanTrack *t)
{
    if (!t->encode.on)
        return same_file(root, t->source, t->usb_path);
    const Made *m = record_find(r, t->usb_path);
    char spec[48];
    spec_of(&t->encode, spec);
    struct stat a, b;
    return m && !strcmp(m->spec, spec) && stat(t->source, &a) == 0 && a.st_size == m->src_size &&
           (int64_t)a.st_mtime == m->src_mtime && rx_stick_stat(root, t->usb_path, &b) == 0 &&
           S_ISREG(b.st_mode) && b.st_size == m->size;
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
        if (rx_stick_stat(c->root, o->v[i].path, &s) >= 0 && s.st_dev == st->st_dev &&
            s.st_ino == st->st_ino)
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
            removed = sub >= 0 && prune_empty(sub, dev) &&
                      unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR) == 0;
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
    Record rec;
    record_read(root, &rec);
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        if (!on_stick(root, &rec, t))
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
    record_free(&rec);
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
                ret == RX_ERROR(EXDEV) ? "another file system is mounted on the way"
                                       : "a symbolic link on the way");
}

static int check_paths(Ctx *c, const RXPlan *plan)
{
    static const char *const fixed[] = {
        "/PIONEER/MYSETTING.DAT", "/PIONEER/MYSETTING2.DAT", "/PIONEER/DJMMYSETTING.DAT",
        "/PIONEER/DEVSETTING.DAT", "/PIONEER/rekordbox/export.pdb", "/PIONEER/rekordbox/exportExt.pdb",
        "/PIONEER/rekordbox/exportLibrary.db", "/PIONEER/rekordbox/exportLibrary.db-wal",
        "/PIONEER/rekordbox/exportLibrary.db-shm", RX_SYNC_PATH, RX_ENCODED_PATH,
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

/* The checks before anything is written; the first failure is returned.
 * Unsynced changes a player left are counted in c->unsynced, not
 * returned: the caller refuses on them, after the dry run's listing. */
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
     * cue lists rewritten. A stick with no database and no settings file
     * (a new one) has nothing to lose. */
    RXStickState s;
    r = rx_stick_read(c->lib, c->dest, &s);
    if (r == RX_ERROR(ENOENT)) {
        r = 0;
    } else if (r >= 0) {
        RXSyncItem *items;
        int nb;
        if ((r = rx_stick_diff(c->lib, &s, &items, &nb)) >= 0) {
            if (nb && !c->opt->ignore_unsynced) {
                reportf(c, "check", "stick has %d unsynced change%s (cues, history or settings a player "
                        "wrote); pull them first or export ignoring them", nb, nb == 1 ? "" : "s");
                c->unsynced = nb;
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

    /* The databases, when nothing else stops the export; a dry run stopped
     * by the unsynced changes alone builds them too, to list all that a
     * real run past them would write. */
    if (ret >= 0 && (!c->unsynced || c->opt->dry_run) && (r = build_databases(c, plan)) < 0)
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
static void dry_run_report(Ctx *c, const RXPlan *plan, const bool *skip)
{
    /* The files that would be encoded, then every file. */
    int nb = 0, listed = 0;
    for (int i = 0; i < plan->nb_tracks; i++)
        nb += plan->tracks[i].encode.on && !skip[i];
    for (int i = 0; i < plan->nb_tracks; i++) {
        if (!plan->tracks[i].encode.on || skip[i])
            continue;
        char name[512];
        rx_track_display(plan->tracks[i].track, name, sizeof(name));
        progress(c, "transcode", ++listed, nb, name);
    }
    for (int i = 0; i < plan->nb_tracks; i++)
        progress(c, skip[i] ? "skip" : "copy", i + 1, plan->nb_tracks, plan->tracks[i].usb_path);
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

/* ------------------------------------------------------------- the audio */

/* The audio goes through two threads: a producer that reads each file to
 * copy, or encodes it (rx_encode, from the decoded samples), into pieces
 * of a MiB at most, and this one, which writes them on the stick and keeps
 * the rate, the time left and the interface going.
 *
 * Straight onto the stick, in place: no temporary, no fsync. A file is
 * opened O_DIRECT where the file system takes it, so every piece is on
 * the stick when write() returns and the rate shown is the stick's, not
 * the page cache's filling up and draining at the end (FAT and exFAT
 * write an O_DIRECT piece through before returning; ext4 and the like
 * want pieces aligned, so a file's tail and a patch go buffered; tmpfs
 * refuses the flag and gets plain writes). A write that stops part way
 * leaves a shorter file with today's time, which same_file() does not
 * take for the source, and no record line, which on_stick() needs. Encoding the next
 * track overlaps writing this one (a FLAC encodes four or five times
 * faster than a USB stick takes it), and memory holds a queue's worth of
 * pieces whatever the files. A muxer going back to its header at the end
 * (FLAC's STREAMINFO, a WAV's or an AIFF's sizes) sends those bytes as a
 * patch, written over what went before, in order. The producer also
 * learns what the analysis files need of each file (its MP3 or FLAC
 * frames), which the plan reads from no file. */
#define PIECE  (1 << 20)
#define QUEUE  32
#define ALIGN  4096

enum PieceKind { P_OPEN, P_DATA, P_PATCH, P_DONE, P_SKIP, P_FAIL, P_END };

typedef struct Piece {
    enum PieceKind kind;
    int      track;
    int64_t  offset;
    uint8_t *data;
    size_t   len;
    int      err;
} Piece;

typedef struct Learnt {
    RXEncoded  enc;             /* encoded */
    RXMp3Info  mp3;
    RXFlacInfo flac;            /* a copy's, or from the record */
    bool       has_mp3, has_flac;
} Learnt;

typedef struct Audio {
    Ctx            *c;
    RXPlan         *plan;
    const bool     *skip;
    const Record   *old;
    Learnt         *learnt;
    pthread_mutex_t lock;
    pthread_cond_t  can_put, can_take;
    Piece           q[QUEUE];
    int             head, nb;
    bool            stop;       /* under lock */
    /* The producer's, for the file it encodes */
    int64_t         pos, buf_off;
    uint8_t        *buf;
    size_t          fill;
    int             track;
} Audio;

static void piece_free(Piece *p)
{
    free(p->data);
    p->data = NULL;
}

/* Into the queue; the data is the queue's from here, also on failure. */
static int put_piece(Audio *a, Piece p)
{
    pthread_mutex_lock(&a->lock);
    while (a->nb == QUEUE && !a->stop)
        pthread_cond_wait(&a->can_put, &a->lock);
    if (a->stop) {
        pthread_mutex_unlock(&a->lock);
        piece_free(&p);
        return RX_ERROR(ECANCELED);
    }
    a->q[(a->head + a->nb++) % QUEUE] = p;
    pthread_cond_signal(&a->can_take);
    pthread_mutex_unlock(&a->lock);
    return 0;
}

/* The next piece: 0, or 1 after a quarter second without one. */
static int take_piece(Audio *a, Piece *p)
{
    pthread_mutex_lock(&a->lock);
    if (!a->nb) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&a->can_take, &a->lock, &ts);
    }
    int got = a->nb > 0;
    if (got) {
        *p = a->q[a->head];
        a->head = (a->head + 1) % QUEUE;
        a->nb--;
        pthread_cond_signal(&a->can_put);
    }
    pthread_mutex_unlock(&a->lock);
    return got ? 0 : 1;
}

static bool stopped(void *opaque)
{
    Audio *a = opaque;
    pthread_mutex_lock(&a->lock);
    bool s = a->stop;
    pthread_mutex_unlock(&a->lock);
    return s;
}

static uint8_t *block(void)
{
    void *b = NULL;
    return posix_memalign(&b, ALIGN, PIECE) ? NULL : b;
}

/* What the analysis files need of a file written as it is. */
static void learn_copy(Learnt *l, const RXPlanTrack *t)
{
    if (!strcmp(t->probe.codec, "mp3"))
        l->has_mp3 = rx_mp3_scan(t->source, &l->mp3) >= 0;
    else if (!strcmp(t->probe.codec, "flac"))
        l->has_flac = rx_flac_scan(t->source, &l->flac) >= 0;
}

static int produce_copy(Audio *a, int i)
{
    const RXPlanTrack *t = &a->plan->tracks[i];
    int fd = open(t->source, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return RX_ERROR(errno);
    int ret = put_piece(a, (Piece) { .kind = P_OPEN, .track = i });
    for (int64_t off = 0; ret >= 0;) {
        uint8_t *b = block();
        if (!b) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        size_t n = 0;
        while (n < PIECE) {
            ssize_t r = read(fd, b + n, PIECE - n);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                if (r < 0)
                    ret = RX_ERROR(errno);
                break;
            }
            n += (size_t)r;
        }
        if (ret < 0 || !n) {
            free(b);
            break;
        }
        ret = put_piece(a, (Piece) { .kind = P_DATA, .track = i, .offset = off, .data = b, .len = n });
        off += (int64_t)n;
        if (n < PIECE)
            break;
    }
    close(fd);
    if (ret >= 0)
        learn_copy(&a->learnt[i], t);
    return ret < 0 ? ret : put_piece(a, (Piece) { .kind = P_DONE, .track = i });
}

/* rx_encode's bytes: appended to the block being filled and sent whole;
 * bytes for a place already sent go as a patch. */
static int enc_write(void *opaque, const uint8_t *b, int n)
{
    Audio *a = opaque;
    while (n > 0) {
        if (a->pos < a->buf_off) {
            int k = (int)RX_MIN((int64_t)n, a->buf_off - a->pos);
            uint8_t *copy = malloc((size_t)k);
            if (!copy)
                return RX_ERROR(ENOMEM);
            memcpy(copy, b, (size_t)k);
            if (put_piece(a, (Piece) { .kind = P_PATCH, .track = a->track, .offset = a->pos,
                                       .data = copy, .len = (size_t)k }) < 0)
                return RX_ERROR(ECANCELED);
            a->pos += k;
            b += k;
            n -= k;
            continue;
        }
        size_t at = (size_t)(a->pos - a->buf_off);
        if (at == PIECE) {
            if (put_piece(a, (Piece) { .kind = P_DATA, .track = a->track, .offset = a->buf_off,
                                       .data = a->buf, .len = PIECE }) < 0) {
                a->buf = NULL;
                return RX_ERROR(ECANCELED);
            }
            a->buf_off += PIECE;
            a->fill = 0;
            if (!(a->buf = block()))
                return RX_ERROR(ENOMEM);
            continue;
        }
        size_t k = RX_MIN((size_t)n, PIECE - at);
        memcpy(a->buf + at, b, k);
        a->fill = RX_MAX(a->fill, at + k);
        a->pos += (int64_t)k;
        b += k;
        n -= (int)k;
    }
    return 0;
}

static int64_t enc_seek(void *opaque, int64_t off, int whence)
{
    Audio *a = opaque;
    if (whence != SEEK_SET || off < 0 || off > a->buf_off + (int64_t)a->fill)
        return RX_ERROR(EINVAL);
    a->pos = off;
    return off;
}

static int produce_encode(Audio *a, int i)
{
    const RXPlanTrack *t = &a->plan->tracks[i];
    const RXPlanEncode *e = &t->encode;
    a->track = i;
    a->pos = a->buf_off = 0;
    a->fill = 0;
    if (!(a->buf = block()))
        return RX_ERROR(ENOMEM);
    int ret = put_piece(a, (Piece) { .kind = P_OPEN, .track = i });
    /* A WAV's or AIFF's size known before it is written: the plan's
     * frames, when they are the source's. */
    const RXTrack *src = t->track;
    bool same_rate = !e->max_rate || !src || src->sample_rate <= e->max_rate;
    RXEncodeOptions o = {
        .format = e->format, .bits = e->bits, .exact = e->exact, .channels = e->channels,
        .max_rate = e->max_rate, .tags = e->format == RX_ENCODE_FLAC,
        .frames = e->format != RX_ENCODE_FLAC && same_rate ? t->total_samples : 0,
        .write = enc_write, .seek = enc_seek, .opaque = a, .cancelled = stopped,
    };
    if (ret >= 0)
        ret = rx_encode(t->source, &o, &a->learnt[i].enc);
    if (ret >= 0 && a->fill) {
        ret = put_piece(a, (Piece) { .kind = P_DATA, .track = i, .offset = a->buf_off,
                                     .data = a->buf, .len = a->fill });
        a->buf = NULL;
    }
    free(a->buf);
    a->buf = NULL;
    return ret < 0 ? ret : put_piece(a, (Piece) { .kind = P_DONE, .track = i });
}

/* A file on the stick already: what the analysis files need of it, from
 * the source (a copy) or the record (an encoded file). */
static int produce_skip(Audio *a, int i)
{
    const RXPlanTrack *t = &a->plan->tracks[i];
    Learnt *l = &a->learnt[i];
    if (!t->encode.on) {
        learn_copy(l, t);
    } else if (t->encode.format == RX_ENCODE_FLAC) {
        const Made *m = record_find(a->old, t->usb_path);
        if (m && flac_of(m, t->probe.sample_rate, t->probe.channels, t->encode.bits, &l->flac) >= 0)
            l->has_flac = true;
    }
    return put_piece(a, (Piece) { .kind = P_SKIP, .track = i });
}

static void *produce(void *arg)
{
    Audio *a = arg;
    for (int i = 0; i < a->plan->nb_tracks && !stopped(a); i++) {
        const RXPlanTrack *t = &a->plan->tracks[i];
        int ret = a->skip[i] ? produce_skip(a, i) : t->encode.on ? produce_encode(a, i)
                                                                  : produce_copy(a, i);
        if (ret < 0) {
            put_piece(a, (Piece) { .kind = P_FAIL, .track = i, .err = ret });
            return NULL;
        }
    }
    put_piece(a, (Piece) { .kind = P_END, .track = -1 });
    return NULL;
}

static int write_at(int fd, bool *direct, const uint8_t *data, size_t len, int64_t off)
{
    if (*direct && (off % ALIGN || len % ALIGN)) {
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_DIRECT) < 0)
            return RX_ERROR(errno);
        *direct = false;
    }
    for (size_t o = 0; o < len;) {
        ssize_t w = pwrite(fd, data + o, len - o, (off_t)(off + (int64_t)o));
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return RX_ERROR(errno);
        o += (size_t)w;
    }
    return 0;
}

/* The audio of every track, skipped, copied or encoded; `*copied` counts
 * the files written. The plan learns each file's real size and frames. */
static int write_audio(Ctx *c, RXPlan *plan, const bool *skip, const Record *old, Learnt *learnt,
                       int *copied)
{
    Audio a = { .c = c, .plan = plan, .skip = skip, .old = old, .learnt = learnt };
    pthread_mutex_init(&a.lock, NULL);
    pthread_cond_init(&a.can_put, NULL);
    pthread_cond_init(&a.can_take, NULL);
    pthread_t th;
    int ret = pthread_create(&th, NULL, produce, &a) ? RX_ERROR(EAGAIN) : 0;
    bool running = ret >= 0;
    int fd = -1, cur = -1, n = plan->nb_tracks;
    bool direct = false;
    int64_t last_tick = rx_time_us();
    while (running) {
        Piece p;
        if (take_piece(&a, &p)) {
            if (!progress(c, cur >= 0 && fd >= 0 ? "copy" : "skip", RX_MAX(cur + 1, 1), n, NULL))
                ret = RX_ERROR(ECANCELED);
        } else {
            const RXPlanTrack *t = p.track >= 0 ? &plan->tracks[p.track] : NULL;
            switch (p.kind) {
            case P_OPEN: {
                cur = p.track;
                if (!progress(c, "copy", cur + 1, n, t->usb_path)) {
                    ret = RX_ERROR(ECANCELED);
                    break;
                }
                int flags = O_WRONLY | O_CREAT | O_TRUNC;
                fd = rx_stick_open(c->root, t->usb_path, flags | O_DIRECT, 0644);
                direct = fd >= 0;
                if (fd == RX_ERROR(EINVAL))
                    fd = rx_stick_open(c->root, t->usb_path, flags, 0644);
                if (fd < 0) {
                    reportf(c, "copy", "cannot write %s%s: %s", c->dest, t->usb_path, strerror(-fd));
                    ret = fd;
                    fd = -1;
                }
                break;
            }
            case P_DATA:
            case P_PATCH:
                if (fd >= 0 && (ret = write_at(fd, &direct, p.data, p.len, p.offset)) < 0)
                    reportf(c, "copy", "cannot write %s%s: %s", c->dest, t->usb_path, strerror(-ret));
                if (p.kind == P_DATA)
                    stats_add(c, (int64_t)p.len);
                break;
            case P_DONE: {
                /* The source's time, for same_file() and the record. */
                struct stat st;
                if (fd >= 0 && stat(t->source, &st) == 0) {
                    struct timespec ts[2] = { st.st_atim, st.st_mtim };
                    futimens(fd, ts);
                }
                /* The close on the pool: its tenth of a second overlaps
                 * the next file (PutJob). */
                if (fd >= 0 && put_submit(c, t->usb_path, NULL, 0, false, fd) < 0)
                    close(fd);
                fd = -1;
                (*copied)++;
                break;
            }
            case P_SKIP:
                progress(c, "skip", p.track + 1, n, t->usb_path);
                break;
            case P_FAIL:
                if (p.err != RX_ERROR(ECANCELED))
                    reportf(c, "copy", "cannot %s %s: %s", t->encode.on ? "encode" : "copy", t->source,
                            strerror(-p.err));
                ret = p.err;
                running = false;
                break;
            case P_END:
                running = false;
                break;
            }
            piece_free(&p);
            int64_t now = rx_time_us();
            if (ret >= 0 && running && now - last_tick >= 250000) {
                last_tick = now;
                if (!progress(c, "copy", RX_MAX(cur + 1, 1), n, NULL))
                    ret = RX_ERROR(ECANCELED);
            }
        }
        if (ret < 0)
            running = false;
    }
    /* Stopped (or done): the producer let go of, what it queued freed. */
    pthread_mutex_lock(&a.lock);
    a.stop = true;
    pthread_cond_broadcast(&a.can_put);
    pthread_mutex_unlock(&a.lock);
    if (ret >= 0 || ret != RX_ERROR(EAGAIN))
        pthread_join(th, NULL);
    while (a.nb) {
        piece_free(&a.q[a.head]);
        a.head = (a.head + 1) % QUEUE;
        a.nb--;
    }
    if (fd >= 0)
        close(fd);
    pthread_cond_destroy(&a.can_put);
    pthread_cond_destroy(&a.can_take);
    pthread_mutex_destroy(&a.lock);
    return ret;
}

/* What the files written taught: each track's size and frames, what its
 * PVB2 or PVBR tag is made of; the record of what is encoded, kept for
 * the next export. */
static int learn(RXPlan *plan, const bool *skip, const Record *old, Learnt *learnt, Record *now)
{
    for (int i = 0; i < plan->nb_tracks; i++) {
        RXPlanTrack *t = &plan->tracks[i];
        Learnt *l = &learnt[i];
        if (l->has_mp3)
            t->probe.mp3 = l->mp3;
        if (l->has_flac) {
            rx_flac_info_free(&t->probe.flac);
            t->probe.flac = l->flac;
            l->flac = (RXFlacInfo) { 0 };
        }
        if (!t->encode.on)
            continue;
        Made *m = record_add(now);
        if (!m)
            return RX_ERROR(ENOMEM);
        const Made *was = skip[i] ? record_find(old, t->usb_path) : NULL;
        if (was) {
            *m = *was;
            m->path = rx_strdup(was->path);
        } else {
            struct stat st;
            const RXEncoded *e = &l->enc;
            *m = (Made) { .path = rx_strdup(t->usb_path), .size = e->size, .frames = e->frames };
            spec_of(&t->encode, m->spec);
            if (stat(t->source, &st) == 0) {
                m->src_size = st.st_size;
                m->src_mtime = st.st_mtime;
            }
            if (t->encode.format == RX_ENCODE_FLAC) {
                seeks_of(&e->flac, m);
                rx_flac_info_free(&t->probe.flac);
                t->probe.flac = e->flac;
                l->enc.flac = (RXFlacInfo) { 0 };
            }
        }
        if (!m->path)
            return RX_ERROR(ENOMEM);
        t->file_size = m->size;
        t->total_samples = t->probe.total_samples = m->frames;
        if (t->sample_rate > 0)
            t->duration = t->probe.duration = (double)m->frames / t->sample_rate;
    }
    return 0;
}

static void learnt_free(Learnt *l, int nb)
{
    for (int i = 0; l && i < nb; i++) {
        rx_flac_info_free(&l[i].flac);
        rx_flac_info_free(&l[i].enc.flac);
    }
    free(l);
}

/* "copy", "flac", "wav", "aiff". */
static const char *const stick_formats[] = { "copy", "flac", "wav", "aiff" };

const char *rx_stick_format_name(int format)
{
    return stick_formats[RX_CLAMP(format, 0, (int)RX_ARRAY_ELEMS(stick_formats) - 1)];
}

int rx_stick_format_parse(const char *name)
{
    for (size_t i = 0; name && i < RX_ARRAY_ELEMS(stick_formats); i++)
        if (!strcmp(name, stick_formats[i]))
            return (int)i;
    return -1;
}

/* The plan's kind of transcode for the options (rxbox/export.h). */
static int stick_kind(const RXExportOptions *opt)
{
    switch (opt->format) {
    case RX_STICK_FLAC: return opt->compat_transcode ? RX_TRANSCODE_STICK_FLAC_COMPAT : RX_TRANSCODE_STICK_FLAC;
    case RX_STICK_WAV:  return RX_TRANSCODE_STICK_WAV;
    case RX_STICK_AIFF: return RX_TRANSCODE_STICK_AIFF;
    default:            return opt->compat_transcode ? RX_TRANSCODE_STICK_COPY_COMPAT : RX_TRANSCODE_STICK_COPY;
    }
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
    Record old = { 0 }, now = { 0 };
    bool *skip = NULL;
    Learnt *learnt = NULL;
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

    /* The plan from what the library stored of each file (no file read
     * but what changed since): what is copied, and how what is not is
     * encoded as it is written. The compatibility option alone decides,
     * not the setting, which is only its default (the dialog's checkbox,
     * --compat and --no-compat start from it). */
    if ((ret = rx_plan_build_cb(lib, tracks, nb_tracks, opt->playlists,
                                opt->playlists ? opt->nb_playlists : 0, opt->device_name,
                                stick_kind(opt), RX_PLAN_STORED, &plan, cb, opaque)) < 0)
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

    /* What the stick holds already, as this export would write it. */
    record_read(c.root, &old);
    skip = calloc((size_t)RX_MAX(plan.nb_tracks, 1), sizeof(*skip));
    learnt = calloc((size_t)RX_MAX(plan.nb_tracks, 1), sizeof(*learnt));
    if (!skip || !learnt) {
        ret = RX_ERROR(ENOMEM);
        goto out;
    }
    for (int i = 0; i < plan.nb_tracks; i++)
        skip[i] = on_stick(c.root, &old, &plan.tracks[i]);

    /* A dry run stopped by unsynced changes alone still lists what it
     * would copy, rewrite and prune, so one question can cover both; the
     * refusal stands. */
    if (opt->dry_run) {
        dry_run_report(&c, &plan, skip);
        ret = prune(&c, &plan, true);
        if (ret >= 0 && c.unsynced)
            ret = RX_ERROR(EBUSY);
        goto out;
    }
    if (c.unsynced) {
        ret = RX_ERROR(EBUSY);
        goto out;
    }

    /* Audio: the bytes to write first, for the rate and time left. */
    int64_t bytes = 0;
    for (int i = 0; i < plan.nb_tracks; i++)
        if (!skip[i])
            bytes += RX_MAX(plan.tracks[i].file_size, 0);
    stats_start(&c, bytes);
    c.writing = true;
    ret = write_audio(&c, &plan, skip, &old, learnt, &copied);
    if (ret >= 0 || ret == RX_ERROR(ECANCELED)) {
        int r = puts_wait(&c, "copy");
        ret = ret < 0 ? ret : r;
    }
    if (ret >= 0 && c.cancelled)
        ret = RX_ERROR(ECANCELED);
    if (ret < 0)
        goto out;

    /* What the files taught (their sizes and frames, encoded ones'
     * records), then the databases again with it. */
    if ((ret = learn(&plan, skip, &old, learnt, &now)) < 0 || (ret = record_write(&c, &now)) < 0)
        goto out;
    free(c.pdb);
    free(c.ext);
    free(c.olib);
    c.pdb = c.ext = c.olib = NULL;
    if ((ret = build_databases(&c, &plan)) < 0)
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
    learnt_free(learnt, plan.nb_tracks);
    free(skip);
    record_free(&old);
    record_free(&now);
    rx_plan_free(&plan);
    free(tracks);
    free(all);
    return ret;
}
