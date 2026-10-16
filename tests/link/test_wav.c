/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link's WAVs made as they are read (src/link/wav.c), on files the ffmpeg
 * CLI makes: 16- and 24-bit FLAC, MP3, a mono WAV, AAC and Opus.
 *
 *  - Read through in 2 kB pieces, as a CDJ-900 reads: the 44-byte header
 *    of a 16-bit stereo WAV of the plan's frames, then cb_decode's samples
 *    (mono doubled): exactly for the 16-bit lossless sources, within the
 *    one step of dither and the rounding for the others.
 *  - Read again in pieces of any size at any place, in any order, through
 *    a view that keeps one track at a time while another is read between:
 *    the same bytes (within a step for Opus and AAC, whose decoders start
 *    cold after a jump).
 *  - The size is the plan's whatever the decoder makes: nothing past it,
 *    silence where the file ends first.
 *  - A Matroska file is refused (its times are milliseconds), which has
 *    the catalog make the WAV as a file instead.
 *
 * Skips (77) without the ffmpeg CLI. Usage: test_wav (CB_TEST_DIR set). */

#include "cyanbox/audio.h"
#include "cyanbox/model.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    bool exact;                 /* 16-bit lossless: bytes for bytes */
    bool cold;                  /* a jump may differ by a step (Opus, AAC) */
} Case;

static const Case cases[] = {
    { "a.flac",  "-c:a flac -sample_fmt s16",                   NULL,         true,  false },
    { "b.flac",  "-c:a flac -sample_fmt s32 -bits_per_raw_sample 24 -ar 48000", NULL, false, false },
    { "c.mp3",   "-c:a libmp3lame -b:a 320k",                   "libmp3lame", false, false },
    { "d.wav",   "-ac 1 -c:a pcm_s16le",                        NULL,         true,  false },
    { "e.m4a",   "-c:a aac -b:a 256k",                          NULL,         false, true },
    { "f.opus",  "-c:a libopus -b:a 192k",                      "libopus",    false, true },
};

static bool have(const char *encoder)
{
    return !encoder || !sh("ffmpeg -v error -hide_banner -h encoder=%s 2>/dev/null | grep -q .", encoder);
}

/* The plan track Link's plan makes of the file (src/export/plan.c,
 * dry_source with CB_TRANSCODE_LINK). */
static void plan_track(CBPlanTrack *pt, CBTrack *t, uint32_t id, const char *path,
                       const CBProbe *p)
{
    *t = (CBTrack) { .id = id, .path = (char *)path, .sample_rate = p->sample_rate,
                     .channels = p->channels, .bit_depth = p->bit_depth,
                     .total_samples = p->total_samples };
    snprintf(t->codec, sizeof(t->codec), "%s", p->codec);
    *pt = (CBPlanTrack) {
        .id = id, .track = t, .source = (char *)path, .pcm = true,
        .sample_rate = p->sample_rate, .total_samples = p->total_samples,
        .file_size = CB_LINK_WAV_HEADER + p->total_samples * 4,
    };
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
    plan_track(&pt, &t, 7, path, &p);
    size_t size = (size_t)pt.file_size;

    /* Through in 2 kB pieces. */
    CBLinkWav *w;
    CHECK_EQ(cb_link_wav_new(&w, (size_t)1 << 30), 0);
    CHECK_EQ(cb_link_wav_check(w, &pt), 0);
    uint8_t *a = calloc(1, size), *b = malloc(40000);
    CHECK(a && b);
    for (size_t at = 0; a && at < size; at += 2048)
        CHECK_EQ(cb_link_wav_read(w, &pt, at, a + at, 2048), (ssize_t)CB_MIN((size_t)2048, size - at));
    uint8_t h[CB_LINK_WAV_HEADER];
    cb_link_wav_header(h, p.sample_rate, p.total_samples);
    CHECK(a && !memcmp(a, h, sizeof(h)) && !memcmp(a, "RIFF", 4) && !memcmp(a + 36, "data", 4));
    uint8_t z[8];
    CHECK_EQ(cb_link_wav_read(w, &pt, size, z, sizeof(z)), 0);
    CHECK_EQ(cb_link_wav_read(w, &pt, size - 3, z, sizeof(z)), 3);

    /* cb_decode's samples: the same, or within dither and rounding. */
    CBAudio d;
    CHECK_EQ(cb_decode(path, 0, 0, &d), 0);
    int64_t frames = CB_MIN(d.nb_frames, p.total_samples);
    double worst = 0;
    int64_t off = 0;
    for (int64_t f = 0; a && f < frames; f++) {
        for (int c = 0; c < 2; c++) {
            float x = d.samples[f * d.channels + (d.channels > 1 ? c : 0)];
            double e = fabs(s16(a, f * 2 + c) - (double)x * 32768.0);
            worst = CB_MAX(worst, e);
            off += e > (k->exact ? 0.0 : 1.5);
        }
    }
    if (off)
        fprintf(stderr, "test_wav: %s: %lld samples off cb_decode, worst %.2f steps\n", k->file,
                (long long)off, worst);
    CHECK_EQ(off, 0);
    CHECK(d.nb_frames >= p.total_samples - 2048);
    cb_audio_free(&d);
    cb_link_wav_free(&w);

    /* Again at random places, in a view that keeps one track open while
     * another is read in between: each piece as the reading through had
     * it, sample for sample. */
    CHECK_EQ(cb_link_wav_new(&w, 1), 0);
    uint8_t scratch[4096];
    int64_t diff = 0, big = 0;
    for (int i = 0; a && b && i < 400; i++) {
        size_t at = (size_t)(next() % size), n = 1 + (size_t)(next() % 40000);
        n = CB_MIN(n, size - at);
        CHECK_EQ(cb_link_wav_read(w, &pt, at, b, n), (ssize_t)n);
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
            diff += va != vb;
            big += abs(va - vb) > 1;
        }
        if (other && !(i % 50))
            CHECK_EQ(cb_link_wav_read(w, other, 44 + (next() % 1000) * 4, scratch, sizeof(scratch)),
                     (ssize_t)sizeof(scratch));
    }
    if (diff)
        fprintf(stderr, "test_wav: %s: %lld samples differ between the two reads (%lld by more "
                "than a step)\n", k->file, (long long)diff, (long long)big);
    CHECK_EQ(big, 0);
    if (!k->cold)
        CHECK_EQ(diff, 0);
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
            plan_track(&fpt, &ft, 8, first, &fp);
            have_first = true;
        }
    }

    /* A Matroska file: refused, so made as a file instead. */
    if (!make("g.mka", "-c:a flac")) {
        char path[700];
        snprintf(path, sizeof(path), "%s/g.mka", dir);
        CBProbe p;
        CHECK_EQ(cb_probe(path, &p), 0);
        CBTrack t;
        CBPlanTrack pt;
        plan_track(&pt, &t, 9, path, &p);
        CBLinkWav *w;
        CHECK_EQ(cb_link_wav_new(&w, (size_t)1 << 30), 0);
        CHECK(cb_link_wav_check(w, &pt) < 0);
        uint8_t buf[64];
        CHECK(cb_link_wav_read(w, &pt, 44, buf, sizeof(buf)) < 0);
        cb_link_wav_free(&w);
        cb_probe_free(&p);
    }
    cb_probe_free(&fp);
    return test_result("test_wav");
}
