/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link's WAVs made as they are read (src/link/wav.c), on files the ffmpeg
 * CLI makes: 16- and 24-bit FLAC (that one at 48 kHz), a 96 kHz and a 5.1
 * FLAC, MP3, a mono WAV, AAC and Opus. A read that is not made yet waits
 * for the pipelines' eventfds and asks again, as the service does.
 *
 *  - Read as a CDJ-900 reads (docs/LINK.md): 2 kB at 0, then 9408-byte
 *    pieces in order. The 44-byte header of a 16-bit WAV at 44.1 kHz of
 *    the plan's frames and the file's channels (mono stays mono, 5.1 is
 *    stereo), then the samples: a 16-bit 44.1 kHz file's exactly, the rest
 *    what a source at 44.1 kHz decodes, within dither and rounding.
 *  - Read again 400 times, any size at any place, while another player
 *    reads another track: the same bytes, but for dither noise (two steps)
 *    and lossy decoders starting cold (four).
 *  - The size is the plan's whatever the decoder makes: nothing past it,
 *    silence where the file ends first.
 *
 * Skips (77) without the ffmpeg CLI. Usage: test_wav (CB_TEST_DIR set). */

#include "cyanbox/audio.h"
#include "cyanbox/export.h"
#include "cyanbox/model.h"

#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/audio/internal.h"
#include "src/link/wav.h"
#include "tests/formats/test.h"

static char dir[512];

static int sh(const char *fmt, ...) CB_PRINTF(1, 2);
static int sh(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *cmd = cb_vasprintf(fmt, ap);
    va_end(ap);
    int r = cmd ? system(cmd) : -1;
    free(cmd);
    return r;
}

/* 6 s of a kick, a hat and a slow sweep, so that every sample differs. */
static int make(const char *file, const char *args)
{
    return sh("ffmpeg -v error -nostdin -y -f lavfi -i \"aevalsrc='0.6*sin(2*PI*55*t)*exp(-14*mod(t,0.5))"
              "+0.1*sin(2*PI*(200+300*t)*t)|0.5*sin(2*PI*58*t)*exp(-12*mod(t,0.5))"
              "+0.05*sin(2*PI*7000*t)':s=44100:d=6\" %s '%s/%s'", args, dir, file);
}

typedef struct Case {
    const char *file, *args, *encoder;
    bool exact;                 /* 16-bit lossless at 44.1 kHz: bytes for bytes */
    double again;               /* steps a read again may differ by */
} Case;

static const Case cases[] = {
    { "a.flac",  "-c:a flac -sample_fmt s16",                   NULL,         true,  0 },
    { "b.flac",  "-c:a flac -sample_fmt s32 -bits_per_raw_sample 24 -ar 48000", NULL, false, 2 },
    { "c.mp3",   "-c:a libmp3lame -b:a 320k",                   "libmp3lame", false, 2 },
    { "d.wav",   "-ac 1 -c:a pcm_s16le",                        NULL,         true,  0 },
    { "e.m4a",   "-c:a aac -b:a 256k",                          NULL,         false, 4 },
    { "f.opus",  "-c:a libopus -b:a 192k",                      "libopus",    false, 4 },
    { "g.flac",  "-af 'pan=5.1|FL=c0|FR=c1|FC=0.5*c0+0.5*c1|LFE=0.1*c0|BL=0.3*c0|BR=0.3*c1' -c:a flac",
                                                                 NULL,         false, 2 },
    { "h.flac",  "-c:a flac -sample_fmt s32 -ar 96000",          NULL,         false, 2 },
};

static bool have(const char *encoder)
{
    return !encoder || !sh("ffmpeg -v error -hide_banner -h encoder=%s 2>/dev/null | grep -q .", encoder);
}

/* The plan track Link's plan makes of the file (src/export/plan.c,
 * dry_source with CB_TRANSCODE_LINK), `extra` frames longer. */
static void plan_track(CBPlanTrack *pt, CBTrack *t, uint32_t id, const char *path,
                       const CBProbe *p, int64_t extra)
{
    *t = (CBTrack) { .id = id, .path = (char *)path, .sample_rate = p->sample_rate,
                     .channels = p->channels, .bit_depth = p->bit_depth,
                     .total_samples = p->total_samples };
    snprintf(t->codec, sizeof(t->codec), "%s", p->codec);
    int ch = CB_MIN(p->channels, 2);
    int64_t frames = (p->total_samples * CB_LINK_RATE + p->sample_rate - 1) / p->sample_rate + extra;
    *pt = (CBPlanTrack) {
        .id = id, .track = t, .source = (char *)path, .pcm = true,
        .sample_rate = CB_LINK_RATE, .total_samples = frames,
        .file_size = CB_LINK_WAV_HEADER + frames * 2 * ch,
    };
    pt->probe.channels = ch;
}

/* A read, waited for as the service waits: the bytes gathered into dst. */
static ssize_t wav_read(CBLinkWav *w, const CBPlanTrack *pt, uint32_t peer, uint64_t at, size_t n,
                        uint8_t *dst)
{
    for (int tries = 0; tries < 500; tries++) {
        struct iovec iov[CB_LINK_WAV_IOV];
        int niov = 0;
        ssize_t r = cb_link_wav_read(w, pt, peer, at, n, iov, &niov);
        if (r == CB_ERROR(EAGAIN)) {
            int fds[8];
            int nf = cb_link_wav_fds(w, fds, 8);
            struct pollfd p[8];
            for (int i = 0; i < nf; i++)
                p[i] = (struct pollfd) { .fd = fds[i], .events = POLLIN };
            poll(p, (nfds_t)nf, 20);
            for (int i = 0; i < nf; i++) {
                uint64_t v;
                if (p[i].revents & POLLIN)
                    (void)!read(fds[i], &v, sizeof(v));
            }
            continue;
        }
        if (r < 0)
            return r;
        size_t got = 0;
        for (int i = 0; i < niov; i++) {
            memcpy(dst + got, iov[i].iov_base, iov[i].iov_len);
            got += iov[i].iov_len;
        }
        CHECK_EQ((ssize_t)got, r);
        return r;
    }
    return CB_ERROR(ETIMEDOUT);
}

static int16_t s16(const uint8_t *b, int64_t i)
{
    return (int16_t)(b[CB_LINK_WAV_HEADER + 2 * i] | b[CB_LINK_WAV_HEADER + 2 * i + 1] << 8);
}

static uint64_t rnd = 0x1234567;
static uint64_t next(void)
{
    rnd ^= rnd << 13;
    rnd ^= rnd >> 7;
    rnd ^= rnd << 17;
    return rnd;
}

/* The source's samples at 44.1 kHz in float, the file's channels as the
 * WAV has them: what the WAV is within dither. */
static float *reference(const char *path, int ch, int64_t *n)
{
    CBAudioFormat f = { .sample = CB_SAMPLE_FLT, .rate = CB_LINK_RATE, .channels = ch };
    CBSource *s;
    *n = 0;
    if (cb_source_open(&s, path, &f) < 0)
        return NULL;
    size_t cap = (size_t)CB_MAX(cb_source_length(s), (int64_t)4096) + 8192;
    float *x = malloc(cap * ch * sizeof(float));
    while (x && (size_t)*n + 4096 <= cap) {
        uint8_t *d[1] = { (uint8_t *)(x + *n * ch) };
        int got = cb_source_read(s, d, 4096);
        if (got <= 0)
            break;
        *n += got;
    }
    cb_source_free(&s);
    return x;
}

static void test_case(const Case *k, const CBPlanTrack *other)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", dir, k->file);
    if (make(k->file, k->args)) {
        fprintf(stderr, "test_wav: cannot make %s\n", k->file);
        failures++;
        return;
    }
    CBProbe p;
    CHECK_EQ(cb_probe(path, &p), 0);
    CBTrack t;
    CBPlanTrack pt;
    plan_track(&pt, &t, 7, path, &p, 0);
    int ch = pt.probe.channels;
    size_t size = (size_t)pt.file_size;

    /* As a CDJ-900 reads: 2 kB at 0, then 9408-byte pieces from 44 */
    CBLinkWav *w;
    CHECK_EQ(cb_link_wav_new(&w), 0);
    uint8_t *a = calloc(1, size), *b = malloc(200000);
    CHECK(a && b);
    if (a)
        CHECK_EQ(wav_read(w, &pt, 1, 0, 2048, a), 2048);
    for (size_t at = CB_LINK_WAV_HEADER; a && at < size; at += 9408)
        CHECK_EQ(wav_read(w, &pt, 1, at, 9408, a + at), (ssize_t)CB_MIN((size_t)9408, size - at));
    uint8_t h[CB_LINK_WAV_HEADER];
    cb_link_wav_header(h, CB_LINK_RATE, ch, pt.total_samples);
    CHECK(a && !memcmp(a, h, sizeof(h)) && !memcmp(a, "RIFF", 4) && !memcmp(a + 36, "data", 4) &&
          a[22] == ch && a[24] + (a[25] << 8) + (a[26] << 16) == CB_LINK_RATE);
    uint8_t z[8];
    CHECK_EQ(wav_read(w, &pt, 1, size, sizeof(z), z), 0);
    CHECK_EQ(wav_read(w, &pt, 1, size - 3, sizeof(z), z), 3);

    /* The samples: the file's own, or a 44.1 kHz source's within dither
     * and rounding */
    int64_t rn;
    float *ref = reference(path, ch, &rn);
    CHECK(ref && llabs(rn - pt.total_samples) <= 2);
    double worst = 0;
    int64_t off = 0;
    for (int64_t f = 0; a && ref && f < CB_MIN(rn, pt.total_samples); f++) {
        for (int c = 0; c < ch; c++) {
            double e = fabs(s16(a, f * ch + c) - (double)ref[f * ch + c] * 32768.0);
            worst = CB_MAX(worst, e);
            off += e > (k->exact ? 0.0 : 1.5);
        }
    }
    if (off)
        fprintf(stderr, "test_wav: %s: %lld samples off, worst %.2f steps\n", k->file,
                (long long)off, worst);
    CHECK_EQ(off, 0);
    free(ref);

    /* Again at random places, a player reading another track between:
     * each piece as reading through had it */
    int64_t big = 0;
    double most = 0;
    for (int i = 0; a && b && i < 400; i++) {
        size_t at = (size_t)(next() % size), n = 1 + (size_t)(next() % 65000);
        n = CB_MIN(n, size - at);
        CHECK_EQ(wav_read(w, &pt, 1, at, n, b), (ssize_t)n);
        for (size_t j = 0; j < n; j++) {
            size_t o = at + j;
            if (o < CB_LINK_WAV_HEADER) {
                CHECK_EQ(b[j], a[o]);
                continue;
            }
            /* A sample's low byte, with its high byte in the piece. */
            if ((o - CB_LINK_WAV_HEADER) % 2 || j + 1 >= n)
                continue;
            int va = (int16_t)(a[o] | a[o + 1] << 8), vb = (int16_t)(b[j] | b[j + 1] << 8);
            most = fmax(most, abs(va - vb));
            big += abs(va - vb) > k->again;
        }
        if (other && !(i % 50)) {
            uint8_t scratch[4096];
            CHECK_EQ(wav_read(w, other, 2, 44 + (next() % 1000) * 4, sizeof(scratch), scratch),
                     (ssize_t)sizeof(scratch));
        }
    }
    if (big)
        fprintf(stderr, "test_wav: %s: %lld samples differ by more than %g steps (%g)\n", k->file,
                (long long)big, k->again, most);
    CHECK_EQ(big, 0);
    cb_link_wav_free(&w);

    /* A plan longer than the file: silence after its end, nothing past */
    CBPlanTrack longer;
    CBTrack lt;
    plan_track(&longer, &lt, 9, path, &p, 30000);
    CHECK_EQ(cb_link_wav_new(&w), 0);
    size_t tail = (size_t)longer.file_size - 20000 * 2 * ch;
    bool silent = b && wav_read(w, &longer, 1, tail, 20000 * 2 * ch, b) == 20000 * 2 * ch;
    for (size_t j = 0; silent && j < (size_t)20000 * 2 * ch; j++)
        silent = !b[j];
    CHECK(silent);
    CHECK_EQ(wav_read(w, &longer, 1, (size_t)longer.file_size, 4, z), 0);
    cb_link_wav_free(&w);
    free(a);
    free(b);
    cb_probe_free(&p);
}

int main(void)
{
    const char *base = getenv("CB_TEST_DIR");
    if (!base || sh("ffmpeg -version > /dev/null 2>&1")) {
        fprintf(stderr, "test_wav: no CB_TEST_DIR or no ffmpeg CLI\n");
        return 77;
    }
    snprintf(dir, sizeof(dir), "%s", base);
    cb_mkdir_p(dir);

    /* The track read in between, the first case's file. */
    char first[700];
    snprintf(first, sizeof(first), "%s/%s", dir, cases[0].file);
    CBProbe fp = { 0 };
    CBTrack ft;
    CBPlanTrack fpt;
    bool have_first = false;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(cases); i++) {
        if (!have(cases[i].encoder)) {
            fprintf(stderr, "test_wav: no %s encoder, %s skipped\n", cases[i].encoder, cases[i].file);
            continue;
        }
        test_case(&cases[i], have_first ? &fpt : NULL);
        if (!i && cb_probe(first, &fp) == 0) {
            plan_track(&fpt, &ft, 8, first, &fp, 0);
            have_first = true;
        }
    }

    cb_probe_free(&fp);
    return test_result("test_wav");
}
