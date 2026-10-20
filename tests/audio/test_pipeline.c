/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Sources and pipelines (cyanbox/audio.h, src/audio/source.c and
 * pipeline.c) on the known signal, made here as WAV, FLAC, 24-bit AIFF,
 * ALAC, MP3 (an Info frame rekordbox counts, so a frame of silence first,
 * and the same file with one it does not), Opus, Vorbis, AAC in MP4 and
 * ADTS; and as mono, 5.1, 48, 88.2 and 96 kHz files.
 *
 * - At the file's rate in float, read in order, every one is cb_decode's
 *   output sample for sample (stereo cb_decode's for the 5.1 file, the
 *   mono cb_decode's for mono output), as long as it, and so again after
 *   200 seeks to random places: exactly, but within 1e-4 for Opus, whose
 *   decoder starts cold, and within 0.003 at the start of ADTS (below).
 * - Resampled (the 96, 88.2 and 48 kHz files to 44.1 kHz, the 44.1 kHz one
 *   to 48), 200 seeks give what reading in order gives, sample for sample,
 *   and the clicks land where the rate ratio puts them.
 * - In 16 bits: a 16-bit file at its own rate comes out as its samples,
 *   after seeks too; the 24-bit one is dithered, within two steps of
 *   reading in order after seeks (the noise starts again) and of the float
 *   samples; s16p holds what s16 does, plane by plane.
 * - A pipeline's chunks, popped as its eventfd says, come in order and
 *   are the source's read in order; after a seek the next chunk is the one
 *   asked for, never an older one; the last is marked; a missing file
 *   does not open. (test_pipeline_race.c has the race of a seek.) */

#include "cyanbox/audio.h"

#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>

#include <libavutil/samplefmt.h>

#include "src/audio/internal.h"
#include "util.h"

static int failures;

#define CHUNK 4096
#define SEEKS 200

static double diff(const float *a, const float *b, int64_t n)
{
    double m = 0.0;
    for (int64_t i = 0; i < n; i++)
        m = fmax(m, fabs((double)a[i] - b[i]));
    return m;
}

static unsigned rnd(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return *seed >> 4;
}

/* The whole source in order, in CHUNK reads, as float; frames in *n. */
static float *read_all(const char *path, const CBAudioFormat *f, int64_t *n, int *ch, int *rate)
{
    CBSource *s;
    *n = 0;
    if (cb_source_open(&s, path, f) < 0)
        return NULL;
    *ch = cb_source_channels(s);
    *rate = cb_source_rate(s);
    size_t cap = (size_t)CB_MAX(cb_source_length(s), (int64_t)CHUNK) + 4 * CHUNK;
    float *x = malloc(cap * *ch * sizeof(float));
    while (x) {
        if ((size_t)*n + CHUNK > cap) {
            cap *= 2;
            float *g = realloc(x, cap * *ch * sizeof(float));
            if (!g) {
                free(x);
                x = NULL;
                break;
            }
            x = g;
        }
        uint8_t *dst[1] = { (uint8_t *)(x + *n * *ch) };
        int got = cb_source_read(s, dst, CHUNK);
        if (got <= 0)
            break;
        *n += got;
    }
    cb_source_free(&s);
    return x;
}

/* 200 seeks of up to 3 chunks into `want` (n frames, ch channels, float
 * unless s16); the worst difference, in samples or steps. */
static double seeks(const char *what, const char *path, const CBAudioFormat *f, const void *want,
                    int64_t n, int ch)
{
    CBSource *s;
    if (cb_source_open(&s, path, f) < 0) {
        CHECK(0, "%s: open for seeks", what);
        return INFINITY;
    }
    bool s16 = f->sample == CB_SAMPLE_S16;
    int bps = s16 ? 2 : 4;
    uint8_t *buf = malloc((size_t)3 * CHUNK * ch * bps);
    unsigned seed = 11;
    double worst = 0.0;
    for (int i = 0; buf && i < SEEKS; i++) {
        int64_t at = i == 0 ? n - CHUNK / 2 : i == 1 ? 0 : (int64_t)(rnd(&seed) % (uint64_t)n);
        int len = 1 + (int)(rnd(&seed) % (3 * CHUNK));
        if (cb_source_seek(s, at) < 0) {
            CHECK(0, "%s: seek to %" PRId64, what, at);
            break;
        }
        uint8_t *dst[1] = { buf };
        int got = cb_source_read(s, dst, len);
        int64_t inside = CB_CLAMP(n - at, (int64_t)0, (int64_t)len);
        CHECK(got == inside, "%s: %d frames at %" PRId64 ", want %" PRId64, what, got, at, inside);
        for (int64_t k = 0; k < (int64_t)CB_MAX(got, 0) * ch; k++) {
            double d = s16 ? fabs((double)((const int16_t *)buf)[k] -
                                  ((const int16_t *)want)[at * ch + k])
                           : fabs((double)((const float *)buf)[k] - ((const float *)want)[at * ch + k]);
            worst = fmax(worst, d);
        }
    }
    free(buf);
    cb_source_free(&s);
    return worst;
}

/* cb_decode's samples, read in order and after seeks. Mono from more than
 * two channels is the mean of their stereo mix: cb_decode's stereo, made
 * mono, within rounding (it mixes in two steps, libswresample in one). */
static void check_file(const char *what, const char *path, int channels, double tolerance)
{
    CBAudio ref;
    CBProbe pr;
    bool many = channels == 1 && cb_probe(path, &pr) >= 0 && pr.channels > 2;
    if (many)
        cb_probe_free(&pr);
    int ret = cb_decode(path, many ? 2 : channels, 0, &ref);
    CHECK(ret >= 0, "%s: cb_decode %d", what, ret);
    if (ret < 0)
        return;
    double exact = 0.0;
    if (many) {
        CBAudio st = ref;
        ret = cb_audio_to_mono(&st, &ref);
        cb_audio_free(&st);
        CHECK(ret >= 0, "%s: mono", what);
        if (ret < 0)
            return;
        exact = 1e-6;
        tolerance = fmax(tolerance, exact);
    }
    CBAudioFormat f = { .sample = CB_SAMPLE_FLT, .channels = channels };
    int64_t n;
    int ch, rate;
    CBSource *probe;
    int64_t length = -1;
    if (cb_source_open(&probe, path, &f) >= 0) {
        length = cb_source_length(probe);
        cb_source_free(&probe);
    }
    int64_t t0 = cb_time_us();
    float *x = read_all(path, &f, &n, &ch, &rate);
    int64_t t1 = cb_time_us();
    CHECK(x && n == ref.nb_frames && ch == ref.channels && rate == ref.rate,
          "%s: %" PRId64 " frames %d ch %d Hz, cb_decode %" PRId64 " %d %d", what, n, ch, rate,
          ref.nb_frames, ref.channels, ref.rate);
    /* What the container states: Ogg Vorbis's leading packet makes it 128
     * frames long */
    CHECK(llabs(length - ref.nb_frames) <= 128, "%s: the container says %" PRId64 " frames, %"
          PRId64 " decoded", what, length, ref.nb_frames);
    if (x && n == ref.nb_frames && ch == ref.channels) {
        double seq = diff(x, ref.samples, n * ch);
        CHECK(seq <= exact, "%s: in order, %g off cb_decode", what, seq);
        double worst = seeks(what, path, &f, ref.samples, n, ch);
        CHECK(worst <= tolerance, "%s: %g off after seeks (allowed %g)", what, worst, tolerance);
        printf("%s: %" PRId64 " frames in %.1f ms, %g off after %d seeks\n", what, n,
               (t1 - t0) / 1e3, worst, SEEKS);
    }
    free(x);
    cb_audio_free(&ref);
}

/* Resampled to `rate`: seeks against reading in order; the clicks (every
 * SIG_CLICK frames of the SIG_RATE signal, so every SIG_CLICK * in /
 * SIG_RATE of the file) where the ratio puts them. */
static void check_resampled(const char *what, const char *path, int in_rate, int rate)
{
    CBAudioFormat f = { .sample = CB_SAMPLE_FLT, .rate = rate };
    int64_t n;
    int ch, r;
    float *x = read_all(path, &f, &n, &ch, &r);
    CHECK(x && r == rate && llabs(n - (int64_t)SIG_SECONDS * rate) <= 1,
          "%s: %" PRId64 " frames at %d Hz", what, n, r);
    if (!x)
        return;
    double worst = seeks(what, path, &f, x, n, ch);
    CHECK(worst == 0.0, "%s: %g off reading in order after seeks", what, worst);
    int off = 0;
    for (int k = 1; k < SIG_SECONDS * 2 - 1; k++) {
        double at = (double)k * SIG_CLICK * rate / SIG_RATE;
        int64_t c = (int64_t)llround(at), peak = c;
        for (int64_t i = c - 8; i <= c + 8; i++)
            if (i >= 0 && i < n && fabs(x[i * ch]) > fabs(x[peak * ch]))
                peak = i;
        off = CB_MAX(off, (int)llabs(peak - c));
    }
    CHECK(off <= 1, "%s: a click %d frames from where the ratio puts it", what, off);
    printf("%s: %" PRId64 " frames, seeks %g off, clicks within %d\n", what, n, worst, off);
    free(x);
}

/* The source's 16-bit output against `pcm` (n frames, ch channels):
 * reading in order and after seeks, within `steps`. */
static void check_s16(const char *what, const char *path, int channels, const int16_t *pcm,
                      int64_t pn, double steps)
{
    CBAudioFormat f = { .sample = CB_SAMPLE_S16, .channels = channels };
    CBSource *s;
    if (cb_source_open(&s, path, &f) < 0) {
        CHECK(0, "%s: open", what);
        return;
    }
    int ch = cb_source_channels(s);
    int16_t *x = malloc((size_t)(pn + CHUNK) * ch * 2);
    int64_t n = 0;
    while (x) {
        uint8_t *dst[1] = { (uint8_t *)(x + n * ch) };
        int got = cb_source_read(s, dst, (int)CB_MIN((int64_t)CHUNK, pn + CHUNK - n));
        if (got <= 0)
            break;
        n += got;
    }
    cb_source_free(&s);
    CHECK(n == pn, "%s: %" PRId64 " frames, want %" PRId64, what, n, pn);
    double seq = 0.0;
    for (int64_t i = 0; x && i < CB_MIN(n, pn) * ch; i++)
        seq = fmax(seq, fabs((double)x[i] - pcm[i]));
    CHECK(seq <= steps, "%s: %g steps off in order (allowed %g)", what, seq, steps);
    CHECK(steps == 0.0 || seq > 0.0, "%s: not dithered", what);
    double worst = x ? seeks(what, path, &f, x, n, ch) : INFINITY;
    CHECK(worst <= (steps ? 2.0 : 0.0), "%s: %g steps off after seeks", what, worst);

    /* s16p: the same samples, plane by plane */
    CBAudioFormat fp = { .sample = CB_SAMPLE_S16P, .channels = channels };
    if (x && steps == 0.0 && cb_source_open(&s, path, &fp) >= 0) {
        int16_t *pl = malloc((size_t)2 * CHUNK * 2);
        uint8_t *dst[2] = { (uint8_t *)pl, (uint8_t *)(pl + CHUNK) };
        int got = pl ? cb_source_read(s, dst, CHUNK) : -1;
        bool same = got == CHUNK;
        for (int i = 0; same && i < CHUNK; i++)
            for (int c = 0; c < ch; c++)
                same &= pl[c * CHUNK + i] == x[i * ch + c];
        CHECK(same, "%s: s16p differs from s16", what);
        free(pl);
        cb_source_free(&s);
    }
    printf("%s: 16-bit, %g steps in order, %g after seeks\n", what, seq, worst);
    free(x);
}

static void check_pipeline(const char *path)
{
    CBAudioFormat f = { .sample = CB_SAMPLE_FLT };
    int64_t n;
    int ch, rate;
    float *x = read_all(path, &f, &n, &ch, &rate);
    CBPipeline *p = NULL;
    CBPipelineOptions o = { .format = f, .chunk = CHUNK, .ahead = 1.0 };
    CHECK(x && cb_pipeline_open(&p, path, &o) >= 0, "pipeline: open");
    if (!x || !p) {
        free(x);
        return;
    }
    CHECK(cb_pipeline_channels(p) == ch && cb_pipeline_rate(p) == rate && cb_pipeline_chunk(p) == CHUNK,
          "pipeline: format");

    /* Everything in order, as the eventfd says */
    int64_t frames = 0, next = 0;
    bool in_order = true, last = false, same = true;
    struct pollfd pfd = { .fd = cb_pipeline_fd(p), .events = POLLIN };
    while (!last && poll(&pfd, 1, 5000) > 0) {
        uint64_t v;
        CHECK(read(pfd.fd, &v, sizeof(v)) == sizeof(v), "pipeline: eventfd");
        CBChunk *c;
        while ((c = cb_pipeline_pop(p))) {
            in_order &= c->index == next++ && !c->err;
            same &= frames + c->nb <= n && !memcmp(c->data[0], x + frames * ch,
                                                   (size_t)c->nb * ch * sizeof(float));
            frames += c->nb;
            last = c->last;
            cb_chunk_unref(&c);
        }
    }
    CHECK(last && in_order && same && frames == n, "pipeline: %" PRId64 " frames of %" PRId64
          ", in order %d, the source's %d, last seen %d", frames, n, in_order, same, last);

    /* Seeks: the chunk asked for comes next */
    unsigned seed = 3;
    int bad = 0;
    for (int i = 0; i < 40; i++) {
        int64_t k = (int64_t)(rnd(&seed) % (uint64_t)(n / CHUNK));
        cb_pipeline_seek(p, k);
        CBChunk *c = NULL;
        while (!c && poll(&pfd, 1, 5000) > 0) {
            uint64_t v;
            (void)!read(pfd.fd, &v, sizeof(v));
            c = cb_pipeline_pop(p);
        }
        bad += !c || c->index != k || c->nb != CB_MIN((int64_t)CHUNK, n - k * CHUNK) ||
               memcmp(c->data[0], x + k * CHUNK * ch, (size_t)c->nb * ch * sizeof(float));
        cb_chunk_unref(&c);
    }
    CHECK(bad == 0, "pipeline: %d of 40 seeks gave another chunk", bad);
    /* A chunk outlives its pipeline */
    cb_pipeline_seek(p, 1);
    CBChunk *kept = NULL;
    while (!kept && poll(&pfd, 1, 5000) > 0) {
        uint64_t v;
        (void)!read(pfd.fd, &v, sizeof(v));
        kept = cb_pipeline_pop(p);
    }
    cb_pipeline_free(&p);
    CHECK(kept && kept->index == 1 && !memcmp(kept->data[0], x + CHUNK * ch, CHUNK * ch * sizeof(float)),
          "pipeline: a chunk held after the free");
    cb_chunk_unref(&kept);
    CHECK(cb_pipeline_open(&p, "/nonexistent/cyanbox/x.flac", &o) < 0 && !p, "a missing file opened");
    printf("pipeline: %" PRId64 " frames in order, 40 seeks\n", frames);
    free(x);
}

int main(void)
{
    char *dir = test_dir("pipeline");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, SIG_SECONDS, &n);
    char *wav = test_path(dir, "sig.wav");
    CHECK(write_wav(wav, SIG_RATE, 2, pcm, n) >= 0, "write wav");

    check_file("wav", wav, 0, 0.0);
    check_file("wav, mono output", wav, 1, 0.0);
    check_s16("wav", wav, 0, pcm, n, 0.0);

    char *flac = test_path(dir, "sig.flac");
    CHECK(!ffmpeg("-i", wav, "-c:a", "flac", flac, NULL), "flac encode");
    check_file("flac", flac, 0, 0.0);
    check_s16("flac", flac, 0, pcm, n, 0.0);
    check_pipeline(flac);

    char *aiff = test_path(dir, "sig.aiff");
    CHECK(!ffmpeg("-i", wav, "-c:a", "pcm_s24be", aiff, NULL), "aiff encode");
    check_file("aiff 24-bit", aiff, 0, 0.0);
    /* its samples are the 16-bit ones shifted up: one step of dither */
    check_s16("aiff 24-bit", aiff, 0, pcm, n, 1.0);

    char *alac = test_path(dir, "sig-alac.m4a");
    CHECK(!ffmpeg("-i", wav, "-c:a", "alac", alac, NULL), "alac encode");
    check_file("alac", alac, 0, 0.0);

    int64_t mn;
    int16_t *mono = make_signal(SIG_RATE, 1, SIG_SECONDS, &mn);
    char *mwav = test_path(dir, "mono.wav");
    CHECK(write_wav(mwav, SIG_RATE, 1, mono, mn) >= 0, "write mono wav");
    check_file("mono wav", mwav, 0, 0.0);
    check_s16("mono wav", mwav, 0, mono, mn, 0.0);
    free(mono);

    char *six = test_path(dir, "sig-5.1.flac");
    CHECK(!ffmpeg("-i", wav, "-af", "pan=5.1|FL=c0|FR=c1|FC=0.5*c0+0.5*c1|LFE=0.1*c0|BL=0.3*c0|BR=0.3*c1",
                  "-c:a", "flac", six, NULL), "5.1 encode");
    check_file("5.1 flac, stereo output", six, 2, 0.0);
    check_file("5.1 flac, mono output", six, 1, 0.0);

    struct { const char *file; int rate; const char *fmt; } hi[] = {
        { "sig-96k.flac", 96000, "s32" }, { "sig-88k.flac", 88200, "s32" }, { "sig-48k.wav", 48000, "s16" },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(hi); i++) {
        char *p = test_path(dir, hi[i].file);
        char rate[16];
        snprintf(rate, sizeof(rate), "%d", hi[i].rate);
        CHECK(!ffmpeg("-i", wav, "-af", "aresample=resampler=swr:filter_size=64", "-ar", rate,
                      "-sample_fmt", hi[i].fmt, p, NULL), "%s encode", hi[i].file);
        check_file(hi[i].file, p, 0, 0.0);
        char what[64];
        snprintf(what, sizeof(what), "%s at 44.1 kHz", hi[i].file);
        check_resampled(what, p, hi[i].rate, 44100);
        free(p);
    }
    check_resampled("wav at 48 kHz", wav, SIG_RATE, 48000);

    if (have_encoder("libmp3lame")) {
        /* ffmpeg's Info frame has the audio's channel mode, so rekordbox
         * counts it: a frame of silence first. Flipped, it does not count. */
        char *mp3 = test_path(dir, "sig.mp3");
        char *mp3b = test_path(dir, "sig-restart.mp3");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "128k", mp3, NULL), "mp3 encode");
        CBMp3Info mi;
        uint8_t *d = NULL;
        size_t size = 0;
        if (cb_mp3_scan(mp3, &mi) >= 0 && mi.header_frame && cb_file_read(mp3, &d, &size) >= 0) {
            d[(size_t)mi.first_frame_offset + 3] ^= 0x40;
            CHECK(cb_file_write_atomic(mp3b, d, size) >= 0, "write %s", mp3b);
            check_file(mi.header_counts ? "mp3 (header frame counts)" : "mp3", mp3, 0, 0.0);
            check_file(mi.header_counts ? "mp3" : "mp3 (header frame counts)", mp3b, 0, 0.0);
        } else {
            CHECK(0, "%s: no Info frame", mp3);
        }
        free(d);
        free(mp3);
        free(mp3b);
    }
    if (have_encoder("libopus")) {
        char *opus = test_path(dir, "sig.opus");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libopus", "-b:a", "160k", opus, NULL), "opus encode");
        check_file("opus", opus, 0, 1e-4);
        free(opus);
    }
    if (have_encoder("libvorbis")) {
        char *ogg = test_path(dir, "sig.ogg");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libvorbis", "-q:a", "5", ogg, NULL), "vorbis encode");
        check_file("vorbis", ogg, 0, 0.0);
        free(ogg);
    }
    char *m4a = test_path(dir, "sig.m4a");
    CHECK(!ffmpeg("-i", wav, "-c:a", "aac", "-b:a", "192k", m4a, NULL), "aac encode");
    check_file("aac (mp4)", m4a, 0, 0.0);
    char *adts = test_path(dir, "sig.aac");
    CHECK(!ffmpeg("-i", wav, "-c:a", "aac", "-b:a", "192k", adts, NULL), "adts encode");
    /* libavcodec's AAC flush clears the overlap but keeps the last
     * frame's window state (aacdec.c, flush()), so the first frame after a
     * seek to the start of a stream without priming is windowed apart from
     * a fresh decoder's; later ones settle in the pre-roll */
    check_file("aac (adts)", adts, 0, 0.003);

    free(pcm);
    free(m4a);
    free(adts);
    free(six);
    free(mwav);
    free(alac);
    free(aiff);
    free(flac);
    free(wav);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("pipeline: all tests passed\n");
    return 0;
}
