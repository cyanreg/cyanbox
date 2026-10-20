/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link's WAVs (wav.h; docs/LINK.md, section 4). Each player reading a
 * track has a reader: a pipeline (cyanbox/audio.h) making the track's
 * 16-bit samples at 44.1 kHz in chunks, a few seconds ahead, and a window
 * of the chunks it made, kept from a second behind the furthest place
 * read. A read is answered from the window, as slices of the chunks
 * themselves (nothing is copied before the socket): the 44-byte header,
 * then the samples, then silence where the file ends before the plan says
 * (nothing past the plan's length). A read the window does not hold yet
 * waits (-EAGAIN) while the pipeline is on its way there; one behind the
 * window, or well past where the pipeline is, seeks it.
 *
 * libswresample converts, mixes and dithers (a 16-bit file at 44.1 kHz
 * comes through as its samples), and a chunk is the same whether the
 * pipeline played into it or seeked to it, but for dither noise, which
 * starts again after a seek. The window covers what the players read
 * again (docs/LINK.md, from cdj-12's traces): the CDJ-900 reads 2 kB at 0
 * and then in order in 53.3 ms pieces, the CDJ-2000NXS2 steps back 13.3 ms
 * at a stream's start and 0.8 s on a hot cue; what they read again much
 * later (a hot cue's pre-read tail) is a seek.
 *
 * A few readers at a time, the least recently read closed first; one is
 * opened again when its file changed. */

#include "wav.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cyanbox/audio.h"
#include "cyanbox/common.h"

#include "link_priv.h"

#define NB_READERS 8
#define CHUNK      4096         /* frames per chunk: 16 kB of stereo, 93 ms */
#define WINDOW     64           /* chunks a reader keeps: 6 s */
#define BEHIND     12           /* of them, behind the furthest read: 1.1 s */
#define JUMP       24           /* a read this far past the window waits rather than seeks */
#define AHEAD      4.0          /* seconds the pipeline queues on top */

typedef struct Reader {
    uint32_t    id, peer;       /* id 0: a free slot */
    char       *path;
    struct stat st;             /* the file's when it was opened */
    CBPipeline *p;
    int         channels;
    int64_t     frames;         /* the plan's */
    uint8_t     header[CB_LINK_WAV_HEADER];
    CBChunk    *win[WINDOW];    /* chunk i in win[i % WINDOW], for base <= i < end */
    int64_t     base, end;
    int64_t     last;           /* the track's last chunk once seen, else -1 */
    int         err;
    uint64_t    used;
} Reader;

struct CBLinkWav {
    uint64_t clock;
    Reader   r[NB_READERS];
    uint8_t  zeros[CHUNK * 4];
};

int cb_link_wav_new(CBLinkWav **out)
{
    *out = calloc(1, sizeof(**out));
    return *out ? 0 : CB_ERROR(ENOMEM);
}

static void drop_window(Reader *r)
{
    for (int64_t i = r->base; i < r->end; i++)
        cb_chunk_unref(&r->win[i % WINDOW]);
    r->base = r->end;
}

static void reader_close(Reader *r)
{
    drop_window(r);
    cb_pipeline_free(&r->p);
    free(r->path);
    *r = (Reader) { 0 };
}

void cb_link_wav_free(CBLinkWav **pw)
{
    CBLinkWav *w = *pw;
    if (!w)
        return;
    for (int i = 0; i < NB_READERS; i++)
        reader_close(&w->r[i]);
    free(w);
    *pw = NULL;
}

void cb_link_wav_header(uint8_t h[CB_LINK_WAV_HEADER], int rate, int channels, int64_t frames)
{
    uint32_t data = (uint32_t)CB_CLAMP(frames * channels * 2, 0, (int64_t)UINT32_MAX - 36);
    CBWr w = cb_wr(h, CB_LINK_WAV_HEADER);
    cb_wr_bytes(&w, "RIFF", 4);
    cb_wr_u32le(&w, 36 + data);
    cb_wr_bytes(&w, "WAVEfmt ", 8);
    cb_wr_u32le(&w, 16);
    cb_wr_u16le(&w, 1);                         /* PCM */
    cb_wr_u16le(&w, (uint16_t)channels);
    cb_wr_u32le(&w, (uint32_t)rate);
    cb_wr_u32le(&w, (uint32_t)rate * (uint32_t)channels * 2);
    cb_wr_u16le(&w, (uint16_t)(channels * 2));
    cb_wr_u16le(&w, 16);
    cb_wr_bytes(&w, "data", 4);
    cb_wr_u32le(&w, data);
}

static size_t chunk_bytes(const Reader *r)
{
    return (size_t)CHUNK * (size_t)r->channels * 2;
}

/* The reader of `peer` for the track, opened (its pipeline starting at the
 * chunk `offset` falls in) when there is none. */
static Reader *reader_of(CBLinkWav *w, const CBPlanTrack *pt, uint32_t peer, uint64_t offset,
                         int *err)
{
    struct stat st;
    if (!pt->source || pt->source[0] != '/' || stat(pt->source, &st) < 0 || !S_ISREG(st.st_mode)) {
        *err = CB_ERROR(ENOENT);
        return NULL;
    }
    w->clock++;
    Reader *slot = NULL;
    for (int i = 0; i < NB_READERS; i++) {
        Reader *r = &w->r[i];
        if (!r->id || r->id != pt->id || r->peer != peer)
            continue;
        if (!strcmp(r->path, pt->source) && r->st.st_dev == st.st_dev &&
            r->st.st_ino == st.st_ino && r->st.st_size == st.st_size &&
            r->st.st_mtim.tv_sec == st.st_mtim.tv_sec &&
            r->st.st_mtim.tv_nsec == st.st_mtim.tv_nsec && r->frames == pt->total_samples) {
            r->used = w->clock;
            return r;
        }
        reader_close(r);
        slot = r;
    }
    for (int i = 0; i < NB_READERS && !slot; i++)
        if (!w->r[i].id)
            slot = &w->r[i];
    if (!slot) {
        slot = &w->r[0];
        for (int i = 1; i < NB_READERS; i++)
            if (w->r[i].used < slot->used)
                slot = &w->r[i];
        reader_close(slot);
    }

    int ch = pt->probe.channels;
    if (ch < 1 || ch > 2 || pt->total_samples < 0) {
        *err = CB_ERROR(EINVAL);
        return NULL;
    }
    Reader r = { .id = pt->id, .peer = peer, .st = st, .channels = ch,
                 .frames = pt->total_samples, .last = -1, .used = w->clock };
    int64_t first = offset > CB_LINK_WAV_HEADER
                    ? (int64_t)((offset - CB_LINK_WAV_HEADER) / chunk_bytes(&r)) : 0;
    CBPipelineOptions o = {
        .format = { .sample = CB_SAMPLE_S16, .rate = CB_LINK_WAV_RATE, .channels = ch },
        .chunk = CHUNK, .ahead = AHEAD, .start = first,
    };
    int ret = cb_pipeline_open(&r.p, pt->source, &o);
    if (ret >= 0 && !(r.path = cb_strdup(pt->source)))
        ret = CB_ERROR(ENOMEM);
    if (ret < 0) {
        cb_debug("link: %s cannot be served as a WAV: %s", pt->source, strerror(-ret));
        reader_close(&r);
        *err = ret;
        return NULL;
    }
    r.base = r.end = first;
    cb_link_wav_header(r.header, CB_LINK_WAV_RATE, ch, r.frames);
    *slot = r;
    return slot;
}

/* What the pipeline made since, into the window while it has room. */
static void pump(Reader *r)
{
    while (r->end - r->base < WINDOW && (r->last < 0 || r->end <= r->last)) {
        CBChunk *c = cb_pipeline_pop(r->p);
        if (!c)
            return;
        if (c->index != r->end) {
            cb_chunk_unref(&c);
            continue;
        }
        if (c->err < 0)
            r->err = c->err;
        if (c->last)
            r->last = c->index;
        r->win[r->end % WINDOW] = c;
        r->end++;
    }
}

/* Drops the chunks before `to`. */
static void drop_before(Reader *r, int64_t to)
{
    while (r->base < r->end && r->base < to)
        cb_chunk_unref(&r->win[r->base++ % WINDOW]);
}

static void seek_to(Reader *r, int64_t chunk)
{
    drop_window(r);
    r->base = r->end = chunk;
    r->err = 0;
    if (r->last >= 0 && chunk > r->last)
        return;
    cb_pipeline_seek(r->p, chunk);
}

static void add(struct iovec *iov, int *niov, const void *p, size_t n)
{
    if (n)
        iov[(*niov)++] = (struct iovec) { .iov_base = (void *)p, .iov_len = n };
}

ssize_t cb_link_wav_read(CBLinkWav *w, const CBPlanTrack *pt, uint32_t peer, uint64_t offset,
                         size_t n, struct iovec *iov, int *niov)
{
    *niov = 0;
    int64_t frames = CB_MAX(pt->total_samples, 0);
    uint64_t total = CB_LINK_WAV_HEADER + (uint64_t)frames * (uint64_t)CB_MAX(pt->probe.channels, 1) * 2;
    if (offset >= total)
        return 0;
    n = (size_t)CB_MIN((uint64_t)n, total - offset);

    int err = 0;
    Reader *r = reader_of(w, pt, peer, offset, &err);
    if (!r)
        return err;
    size_t cb = chunk_bytes(r);
    uint64_t end = offset + n;
    int64_t c0 = 0, c1 = -1;
    if (end > CB_LINK_WAV_HEADER) {
        uint64_t d0 = offset > CB_LINK_WAV_HEADER ? offset - CB_LINK_WAV_HEADER : 0;
        c0 = (int64_t)(d0 / cb);
        c1 = (int64_t)((end - CB_LINK_WAV_HEADER - 1) / cb);
        /* The window keeps BEHIND chunks behind the furthest read */
        drop_before(r, c0 - BEHIND);
    }
    pump(r);
    if (r->err < 0)
        return r->err;
    for (int64_t c = c0; c <= c1; c++) {
        if ((r->last >= 0 && c > r->last) || (c >= r->base && c < r->end))
            continue;
        if (c < r->base || c >= r->end + JUMP)
            seek_to(r, c);
        return CB_ERROR(EAGAIN);
    }

    if (offset < CB_LINK_WAV_HEADER)
        add(iov, niov, r->header + offset, (size_t)CB_MIN((uint64_t)CB_LINK_WAV_HEADER - offset, n));
    for (int64_t c = c0; c <= c1; c++) {
        uint64_t at = CB_LINK_WAV_HEADER + (uint64_t)c * cb;
        size_t from = c == c0 && offset > at ? (size_t)(offset - at) : 0;
        size_t to = (size_t)CB_MIN((uint64_t)cb, end - at);
        size_t have = 0;
        if (r->last < 0 || c <= r->last) {
            const CBChunk *k = r->win[c % WINDOW];
            have = (size_t)k->nb * (size_t)r->channels * 2;
            if (from < have)
                add(iov, niov, k->data[0] + from, CB_MIN(to, have) - from);
        }
        /* Silence where the file ends before the plan does */
        if (to > CB_MAX(from, have))
            add(iov, niov, w->zeros, to - CB_MAX(from, have));
    }
    return (ssize_t)n;
}

int cb_link_wav_fds(const CBLinkWav *w, int *fds, int max)
{
    int n = 0;
    for (int i = 0; i < NB_READERS && n < max; i++)
        if (w->r[i].id && w->r[i].p)
            fds[n++] = cb_pipeline_fd(w->r[i].p);
    return n;
}
