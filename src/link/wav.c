/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link's WAVs (wav.h; docs/LINK.md, section 4). A read is mapped onto the
 * frames it covers, they are decoded by the track's packet decoder (a read
 * that follows the last one carries on, any other starts again from
 * enough packets before it: rx_packets_decode), and they are written as
 * 16-bit samples:
 *
 * - A lossless source of 16 bits or fewer goes through exactly: the
 *   decoder's floats are its samples / 32768 (decode.c), rounded back.
 * - Anything else (24-bit, float, lossy) gets triangular dither of one
 *   step, drawn from the sample's place rather than from a running
 *   generator, so that the same bytes come back however often and in
 *   whatever order a player reads them (a CDJ-900 reads 2 kB pieces that
 *   overlap; a jump decodes again).
 * - Mono is doubled. The plan serves nothing of more channels this way.
 *
 * The frames are the decoder's, which are rx_decode's (exactly for MP3 and
 * lossless codecs, within 1e-7 after a jump for Opus and AAC: packets.c),
 * and so the timeline the beat grid and the cues were made on, whatever
 * the codec: a player plays the samples that were analysed.
 *
 * A track's packet store is opened at its first read and kept for the
 * next ones (a player reads a track in thousands of pieces), a few tracks
 * at a time, the least recently read closed first; one is opened again
 * when its file changed. */

#include "wav.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rxbox/audio.h"
#include "rxbox/common.h"

#include "link_priv.h"

#define NB_ENTRIES  8
#define CHUNK       8192        /* frames decoded per call */

typedef struct Entry {
    uint32_t         id;        /* 0: a free slot */
    char            *path;
    struct stat      st;        /* the file's when it was opened */
    RXPackets       *pk;
    RXPacketDecoder *dec;
    int              channels;  /* the file's */
    uint64_t         used;
} Entry;

struct RXLinkWav {
    size_t   max_memory;
    uint64_t clock;
    Entry    e[NB_ENTRIES];
    float   *buf;               /* CHUNK frames at the file's channels */
    size_t   buf_cap;
    uint8_t  pcm[CHUNK * 4];
};

int rx_link_wav_new(RXLinkWav **out, size_t max_memory)
{
    *out = calloc(1, sizeof(**out));
    if (!*out)
        return RX_ERROR(ENOMEM);
    (*out)->max_memory = max_memory;
    return 0;
}

static void entry_close(Entry *e)
{
    rx_packets_decoder_free(e->dec);
    rx_packets_free(e->pk);
    free(e->path);
    *e = (Entry) { 0 };
}

void rx_link_wav_free(RXLinkWav **pw)
{
    RXLinkWav *w = *pw;
    if (!w)
        return;
    for (int i = 0; i < NB_ENTRIES; i++)
        entry_close(&w->e[i]);
    free(w->buf);
    free(w);
    *pw = NULL;
}

/* Closes the least recently read tracks but `keep` until the rest fit. */
static void trim(RXLinkWav *w, const Entry *keep)
{
    while (1) {
        size_t total = 0;
        Entry *old = NULL;
        for (int i = 0; i < NB_ENTRIES; i++) {
            Entry *e = &w->e[i];
            if (!e->id)
                continue;
            total += rx_packets_memory(e->pk);
            if (e != keep && (!old || e->used < old->used))
                old = e;
        }
        if (total <= w->max_memory || !old)
            return;
        entry_close(old);
    }
}

static Entry *entry_of(RXLinkWav *w, const RXPlanTrack *pt, int *err)
{
    struct stat st;
    if (!pt->source || pt->source[0] != '/' || stat(pt->source, &st) < 0 || !S_ISREG(st.st_mode)) {
        *err = RX_ERROR(ENOENT);
        return NULL;
    }
    w->clock++;
    Entry *slot = NULL;
    for (int i = 0; i < NB_ENTRIES; i++) {
        Entry *e = &w->e[i];
        if (!e->id || e->id != pt->id)
            continue;
        if (!strcmp(e->path, pt->source) && e->st.st_dev == st.st_dev &&
            e->st.st_ino == st.st_ino && e->st.st_size == st.st_size &&
            e->st.st_mtim.tv_sec == st.st_mtim.tv_sec &&
            e->st.st_mtim.tv_nsec == st.st_mtim.tv_nsec) {
            e->used = w->clock;
            return e;
        }
        entry_close(e);
        slot = e;
    }
    for (int i = 0; i < NB_ENTRIES && !slot; i++)
        if (!w->e[i].id)
            slot = &w->e[i];
    if (!slot) {
        slot = &w->e[0];
        for (int i = 1; i < NB_ENTRIES; i++)
            if (w->e[i].used < slot->used)
                slot = &w->e[i];
        entry_close(slot);
    }

    Entry e = { .id = pt->id, .st = st, .used = w->clock };
    int ret = rx_packets_open(pt->source, &e.pk);
    if (ret >= 0)
        ret = rx_packets_decoder(e.pk, &e.dec);
    if (ret >= 0) {
        e.channels = rx_packets_channels(e.pk);
        /* The plan's WAV is at the file's rate and of its one or two
         * channels; a file that says otherwise now is not that file. */
        if (rx_packets_rate(e.pk) != pt->sample_rate || e.channels < 1 || e.channels > 2)
            ret = RX_ERROR(EINVAL);
    }
    if (ret >= 0 && !(e.path = rx_strdup(pt->source)))
        ret = RX_ERROR(ENOMEM);
    if (ret < 0) {
        rx_debug("link: %s cannot be served as a WAV: %s", pt->source, strerror(-ret));
        entry_close(&e);
        *err = ret;
        return NULL;
    }
    *slot = e;
    trim(w, slot);
    return slot;
}

int rx_link_wav_check(RXLinkWav *w, const RXPlanTrack *pt)
{
    int err = 0;
    return entry_of(w, pt, &err) ? 0 : err;
}

void rx_link_wav_header(uint8_t h[RX_LINK_WAV_HEADER], int rate, int64_t frames)
{
    uint32_t data = (uint32_t)RX_CLAMP(frames * 4, 0, (int64_t)UINT32_MAX - 36);
    RXWr w = rx_wr(h, RX_LINK_WAV_HEADER);
    rx_wr_bytes(&w, "RIFF", 4);
    rx_wr_u32le(&w, 36 + data);
    rx_wr_bytes(&w, "WAVEfmt ", 8);
    rx_wr_u32le(&w, 16);
    rx_wr_u16le(&w, 1);                         /* PCM */
    rx_wr_u16le(&w, 2);
    rx_wr_u32le(&w, (uint32_t)rate);
    rx_wr_u32le(&w, (uint32_t)rate * 4);
    rx_wr_u16le(&w, 4);
    rx_wr_u16le(&w, 16);
    rx_wr_bytes(&w, "data", 4);
    rx_wr_u32le(&w, data);
}

/* Whether the source's samples are 16-bit ones exactly. */
static bool exact16(const RXPlanTrack *pt)
{
    const RXTrack *t = pt->track;
    if (!t || t->bit_depth <= 0 || t->bit_depth > 16)
        return false;
    return !strcmp(t->codec, "flac") || !strcmp(t->codec, "alac") ||
           (rx_str_starts(t->codec, "pcm_") && !rx_str_starts(t->codec, "pcm_f"));
}

/* Triangular noise of one step, in (-1, 1), for sample i (splitmix64). */
static float tpdf(uint64_t i)
{
    uint64_t z = i + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (float)(z >> 40) * 0x1p-24f - (float)(z & 0xFFFFFF) * 0x1p-24f;
}

static void put16(uint8_t *p, float x, bool exact, uint64_t i)
{
    long v = lrintf(x * 32768.0f + (exact ? 0.0f : tpdf(i)));
    v = RX_CLAMP(v, -32768, 32767);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

ssize_t rx_link_wav_read(RXLinkWav *w, const RXPlanTrack *pt, uint64_t offset, uint8_t *dst,
                         size_t n)
{
    int64_t frames = RX_MAX(pt->total_samples, 0);
    uint64_t total = RX_LINK_WAV_HEADER + (uint64_t)frames * 4;
    if (offset >= total)
        return 0;
    n = (size_t)RX_MIN((uint64_t)n, total - offset);
    size_t done = 0;
    if (offset < RX_LINK_WAV_HEADER) {
        uint8_t h[RX_LINK_WAV_HEADER];
        rx_link_wav_header(h, pt->sample_rate, frames);
        done = RX_MIN(n, (size_t)(RX_LINK_WAV_HEADER - offset));
        memcpy(dst, h + offset, done);
    }
    if (done == n)
        return (ssize_t)n;

    int err = 0;
    Entry *e = entry_of(w, pt, &err);
    if (!e)
        return err;
    size_t need = (size_t)CHUNK * (size_t)e->channels;
    if (need > w->buf_cap) {
        float *b = realloc(w->buf, need * sizeof(*b));
        if (!b)
            return RX_ERROR(ENOMEM);
        w->buf = b;
        w->buf_cap = need;
    }
    bool exact = exact16(pt);
    while (done < n) {
        uint64_t at = offset + done - RX_LINK_WAV_HEADER;   /* in the samples */
        int64_t f0 = (int64_t)(at / 4);
        size_t skip = (size_t)(at % 4);
        int64_t nf = RX_MIN((int64_t)CHUNK, (int64_t)((skip + (n - done) + 3) / 4));
        nf = RX_MIN(nf, frames - f0);
        int64_t got = rx_packets_decode(e->dec, f0, nf, w->buf, 0);
        if (got < 0)
            return done ? (ssize_t)done : (ssize_t)got;
        /* Past where the decoder ends: silence, to the plan's length. */
        if (got < nf)
            memset(w->buf + got * e->channels, 0,
                   (size_t)(nf - got) * (size_t)e->channels * sizeof(float));
        for (int64_t f = 0; f < nf; f++) {
            const float *s = w->buf + f * e->channels;
            uint64_t i = (uint64_t)(f0 + f) * 2;
            put16(w->pcm + f * 4, s[0], exact, i);
            put16(w->pcm + f * 4 + 2, s[e->channels > 1], exact, i + 1);
        }
        size_t take = RX_MIN((size_t)nf * 4 - skip, n - done);
        memcpy(dst + done, w->pcm + skip, take);
        done += take;
    }
    return (ssize_t)done;
}
