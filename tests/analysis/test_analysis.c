/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cb_analyse and the analysis module's edges, beside the beat cases
 * (test_beats.c), the key (test_key.c) and the waveforms (test_waveform.c):
 *
 *  - a float WAV with NaN and infinite samples, decoded as the application
 *    decodes and analysed, gives exactly what the same file with those
 *    samples at 0 gives (the decoder makes them silence, src/audio/
 *    decode.c): the key, the waveforms and the grid, to the bit, and the
 *    key of the file without them. Before, the key came out "" (the file
 *    without them is "Am") and the waveforms and even the grid differed;
 *  - the onsets of a few frames: on silence the band alignment took the
 *    first lag it tried, 8 frames back, and below 8 frames its shift wrote
 *    outside the band ("free(): invalid size"); a band over a silent
 *    reference stays where it is, and a band 3 frames early is still put
 *    on the reference;
 *  - the first loud onset (dj's rule), selected in place, against a sorted
 *    copy;
 *  - the automatic cue of a track whose length is not known yet (0): the
 *    downbeat it gets with a length, from a grid expanded only that far
 *    (it was a million seconds: the process grew by 45 MB).
 *
 * Media are written under $CB_TEST_DIR. */

#include "cyanbox/analysis.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "src/analysis/beats_internal.h"

static int fails;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            fails++;                                                        \
        }                                                                   \
    } while (0)

/* ------------------------------------------------------- non-finite input */

#define RATE    44100
#define SECONDS 12

/* An A minor chord over its root, and a kick on every beat at 124 BPM. */
static float *make_track(int64_t *n)
{
    *n = (int64_t)SECONDS * RATE;
    float *x = calloc(*n, sizeof(*x));
    if (!x)
        return NULL;
    static const double notes[] = { 110.0, 220.0, 261.63, 329.63 };
    for (int64_t i = 0; i < *n; i++) {
        double t = (double)i / RATE, v = 0.0;
        for (size_t k = 0; k < CB_ARRAY_ELEMS(notes); k++)
            v += 0.1 * sin(2.0 * M_PI * notes[k] * t);
        x[i] = (float)v;
    }
    for (double at = 0.25; at < SECONDS; at += 60.0 / 124.0) {
        int64_t s0 = (int64_t)llround(at * RATE);
        double ph = 0.0;
        for (int i = 0; i < RATE / 4 && s0 + i < *n; i++) {
            double t = (double)i / RATE;
            ph += 2.0 * M_PI * (50.0 + 120.0 * exp(-t * 40.0)) / RATE;
            x[s0 + i] += (float)(0.6 * sin(ph) * exp(-t * 12.0));
        }
    }
    return x;
}

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* A mono IEEE float WAV, 32 or 64 bits a sample */
static int write_float_wav(const char *path, const float *x, int64_t n, int bits,
                           const int64_t *at, const double *v, int nb)
{
    int bps = bits / 8;
    uint32_t data = (uint32_t)(n * bps);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + data, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 3, 2);                   /* WAVE_FORMAT_IEEE_FLOAT */
    put_le(h + 22, 1, 2);
    put_le(h + 24, RATE, 4);
    put_le(h + 28, (uint32_t)(RATE * bps), 4);
    put_le(h + 32, (uint32_t)bps, 2);
    put_le(h + 34, (uint32_t)bits, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data, 4);

    uint8_t *buf = malloc((size_t)data);
    if (!buf)
        return -ENOMEM;
    for (int64_t i = 0; i < n; i++) {
        double s = x[i];
        for (int k = 0; k < nb; k++) {
            if (at[k] == i)
                s = v[k];
        }
        if (bits == 32) {
            float f = (float)s;
            memcpy(buf + i * 4, &f, 4);
        } else {
            memcpy(buf + i * 8, &s, 8);
        }
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(h, 1, sizeof(h), f) == sizeof(h) &&
              fwrite(buf, 1, data, f) == data;
    if (f && fclose(f))
        ok = false;
    free(buf);
    return ok ? 0 : -EIO;
}

typedef struct Lines {
    int  n;
    char last[256];
} Lines;

static void count_line(void *opaque, enum CBLogLevel level, const char *line)
{
    Lines *l = opaque;
    if (level == CB_LOG_WARN && strstr(line, "not finite")) {
        l->n++;
        snprintf(l->last, sizeof(l->last), "%s", line);
    }
}

/* Decodes path as the application does (mono, its own rate) and
 * analyses it; the warnings about samples that were not finite are
 * counted in *lines. */
static int analyse_file(const char *path, CBAnalysis *a, CBAudio *au, Lines *lines)
{
    *lines = (Lines) { 0 };
    cb_log_set_sink(count_line, lines);
    int ret = cb_decode(path, 1, 0, au);
    cb_log_set_sink(NULL, NULL);
    if (ret < 0) {
        memset(a, 0, sizeof(*a));
        return ret;
    }
    return cb_analyse(au, NULL, CB_ANALYSE_ALL, a);
}

static bool same_waves(const CBWaveforms *a, const CBWaveforms *b)
{
    uint8_t *pa = NULL, *pb = NULL;
    size_t na = 0, nb = 0;
    bool same = cb_waveforms_pack(a, &pa, &na) >= 0 && cb_waveforms_pack(b, &pb, &nb) >= 0 &&
                na == nb && !memcmp(pa, pb, na);
    free(pa);
    free(pb);
    return same;
}

static bool same_grid(const CBGrid *a, const CBGrid *b)
{
    if (a->nb_sections != b->nb_sections || a->beats_per_bar != b->beats_per_bar)
        return false;
    for (int i = 0; i < a->nb_sections; i++) {
        const CBTempoSection *p = &a->sections[i], *q = &b->sections[i];
        if (p->start != q->start || p->bpm != q->bpm || p->beat != q->beat ||
            p->measured != q->measured)
            return false;
    }
    return true;
}

static void test_nonfinite(const char *dir)
{
    int fails0 = fails;
    int64_t n;
    float *x = make_track(&n);
    if (!x) {
        fails++;
        return;
    }
    /* Three bad samples: in the intro, mid-bar, and near the end */
    const int64_t at[3] = { 3 * RATE, 6 * RATE + RATE / 2, 10 * RATE + 7 };
    const double zero[3] = { 0.0, 0.0, 0.0 };
    const double bad32[3] = { NAN, INFINITY, -INFINITY };
    /* As double: NaN, a value float cannot hold (infinite once converted),
     * and minus infinity */
    const double bad64[3] = { NAN, 1e300, -INFINITY };
    struct {
        const char *name;
        int bits;
        const double *v;
        int bad;
    } files[] = {
        { "clean32.wav", 32, NULL,  0 },
        { "zero32.wav",  32, zero,  0 },
        { "bad32.wav",   32, bad32, 3 },
        { "zero64.wav",  64, zero,  0 },
        { "bad64.wav",   64, bad64, 3 },
    };
    CBAnalysis an[CB_ARRAY_ELEMS(files)];
    CBAudio au[CB_ARRAY_ELEMS(files)];
    memset(an, 0, sizeof(an));
    memset(au, 0, sizeof(au));
    for (size_t f = 0; f < CB_ARRAY_ELEMS(files); f++) {
        char *path = cb_asprintf("%s/%s", dir, files[f].name);
        CHECK(path && write_float_wav(path, x, n, files[f].bits, at, files[f].v,
                                      files[f].v ? 3 : 0) == 0, "writing %s", files[f].name);
        Lines lines;
        int ret = path ? analyse_file(path, &an[f], &au[f], &lines) : -ENOMEM;
        CHECK(ret == 0 && an[f].done == CB_ANALYSE_ALL, "%s: ret %d, done 0x%x", files[f].name,
              ret, an[f].done);
        /* One warning for the file, with the count */
        char want[32];
        snprintf(want, sizeof(want), " %d samples were", files[f].bad);
        CHECK(lines.n == (files[f].bad > 0) && (!files[f].bad || strstr(lines.last, want)),
              "%s: %d warnings, the last \"%s\"", files[f].name, lines.n, lines.last);
        bool finite = true;
        for (int64_t i = 0; i < au[f].nb_frames; i++)
            finite &= isfinite(au[f].samples[i]);
        CHECK(finite, "%s: decoded samples that are not finite", files[f].name);
        free(path);
    }

    /* Each bad file against its zeroed twin: the same samples, so the same
     * results to the bit */
    for (int k = 0; k < 2; k++) {
        int z = k ? 3 : 1, b = z + 1;
        const char *name = files[b].name;
        CHECK(au[z].nb_frames == au[b].nb_frames && au[b].samples &&
              !memcmp(au[z].samples, au[b].samples, (size_t)au[b].nb_frames * sizeof(float)),
              "%s: decoded differently from %s", name, files[z].name);
        CHECK(!strcmp(an[b].key.key, an[z].key.key) &&
              an[b].key.confidence == an[z].key.confidence &&
              an[b].key.tuning_cents == an[z].key.tuning_cents,
              "%s: key %s %.17g %.17g, %s %s %.17g %.17g", name, an[b].key.key,
              an[b].key.confidence, an[b].key.tuning_cents, files[z].name, an[z].key.key,
              an[z].key.confidence, an[z].key.tuning_cents);
        CHECK(same_waves(&an[b].waves, &an[z].waves), "%s: waveforms differ from %s", name,
              files[z].name);
        CHECK(same_grid(&an[b].beats.grid, &an[z].beats.grid) &&
              an[b].beats.confidence == an[z].beats.confidence &&
              an[b].beats.music_start == an[z].beats.music_start,
              "%s: grid differs from %s", name, files[z].name);
        /* And the key is the clean file's */
        CHECK(an[b].key.key[0] && !strcmp(an[b].key.key, an[0].key.key),
              "%s: key \"%s\", without the bad samples \"%s\"", name, an[b].key.key,
              an[0].key.key);
    }
    CHECK(!strcmp(an[0].key.key, "Am"), "the chord's key is %s", an[0].key.key);
    if (fails == fails0)
        printf("ok   NaN and infinite samples: key %s %.3f, %d waveform columns, as with zeros\n",
               an[2].key.key, an[2].key.confidence, an[2].waves.nb_detail);

    for (size_t f = 0; f < CB_ARRAY_ELEMS(files); f++) {
        cb_analysis_free(&an[f]);
        cb_audio_free(&au[f]);
    }
    free(x);
}

/* ------------------------------------------------------------ the onsets */

/* The onsets of n frames: the kick band's level `kick` (NULL silent) and
 * a full band whose flux is 1 at frame `hit` (-1 none). */
static int onsets_of(int n, const float *kick, int hit, CBOnsets *o)
{
    CBSpectrum s = {
        .nb_frames = n,
        .fps       = (double)CB_BEAT_RATE / CB_BEAT_HOP,
        .kick      = calloc(n, sizeof(float)),
        .flux_full = calloc(n, sizeof(float)),
        .flux_mid  = calloc(n, sizeof(float)),
        .flux_high = calloc(n, sizeof(float)),
    };
    int ret = CB_ERROR(ENOMEM);
    if (s.kick && s.flux_full && s.flux_mid && s.flux_high) {
        if (kick)
            memcpy(s.kick, kick, (size_t)n * sizeof(float));
        if (hit >= 0)
            s.flux_full[hit] = 1.0f;
        ret = cb_onsets_compute(&s, o);
    }
    cb_spectrum_free(&s);
    return ret;
}

static int peak_of(const float *v, int n)
{
    int at = 0;
    for (int i = 1; i < n; i++) {
        if (v[i] > v[at])
            at = i;
    }
    return at;
}

static void test_onsets(void)
{
    int fails0 = fails;
    /* Silence of every short length: nothing moves, nothing is written
     * outside the bands (run under valgrind or ASan to see the latter) */
    static const int lengths[] = { 1, 2, 4, 7, 8, 9, 20 };
    for (size_t k = 0; k < CB_ARRAY_ELEMS(lengths); k++) {
        int n = lengths[k];
        CBOnsets o;
        int ret = onsets_of(n, NULL, -1, &o);
        CHECK(ret == 0, "%d frames of silence: %d", n, ret);
        if (ret < 0)
            continue;
        bool silent = true;
        for (int i = 0; i < n; i++)
            silent &= !o.full[i] && !o.low[i] && !o.mid[i] && !o.high[i] && !o.accent[i];
        CHECK(silent, "%d frames of silence have onsets", n);
        cb_onsets_free(&o);
    }

    /* A kick over a silent full band: the kick band's flux (the rise of
     * frames 25 and 26, two frames later after the log flux's delay)
     * stays where it is; it went 8 frames later */
    enum { N = 60 };
    float kick[N];
    for (int i = 0; i < N; i++)
        kick[i] = i >= 26 ? 1.0f : 0.0f;
    CBOnsets o;
    int ret = onsets_of(N, kick, -1, &o);
    CHECK(ret == 0, "a kick over silence: %d", ret);
    if (ret == 0) {
        CHECK(peak_of(o.low, N) == 27 && o.low[28] > 0.0f && o.low[29] == 0.0f,
              "a kick over silence: the kick band peaks at frame %d", peak_of(o.low, N));
        cb_onsets_free(&o);
    }

    /* The same kick 3 frames before a hit in the full band (flux 1 at 28,
     * 30 after the delay): put on it, as before */
    ret = onsets_of(N, kick, 28, &o);
    CHECK(ret == 0, "a kick before a hit: %d", ret);
    if (ret == 0) {
        CHECK(peak_of(o.full, N) == 30 && peak_of(o.low, N) == 30 && o.low[29] == 0.0f,
              "a kick before a hit: the full band at %d, the kick band at %d", peak_of(o.full, N),
              peak_of(o.low, N));
        cb_onsets_free(&o);
    }
    if (fails == fails0)
        printf("ok   onsets: short silences, a kick over silence, a kick before a hit\n");
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* dj's rule as it was written, on a sorted copy */
static double first_sorted(const CBOnsets *o)
{
    float *tmp = malloc((size_t)o->nb * sizeof(*tmp));
    if (!tmp)
        return NAN;
    memcpy(tmp, o->full, (size_t)o->nb * sizeof(*tmp));
    qsort(tmp, o->nb, sizeof(*tmp), cmp_float);
    double p99 = tmp[(int)CB_MIN((int64_t)(0.99 * (o->nb - 1) + 0.5), o->nb - 1)];
    free(tmp);
    for (int i = 0; i < o->nb; i++) {
        if (o->full[i] > 0.25 * p99)
            return i / o->fps;
    }
    return 0.0;
}

static void test_first_onset(void)
{
    int fails0 = fails;
    uint64_t s = 12345;
    static const int lengths[] = { 1, 2, 3, 100, 101, 1000, 54321 };
    for (size_t k = 0; k < CB_ARRAY_ELEMS(lengths); k++) {
        int n = lengths[k];
        float *v = malloc((size_t)n * sizeof(*v));
        if (!v) {
            fails++;
            return;
        }
        /* Mostly small, a few loud, many exact zeros and repeats */
        for (int i = 0; i < n; i++) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            uint32_t r = (uint32_t)(s >> 33);
            v[i] = r % 3 == 0 ? 0.0f : r % 97 == 0 ? (float)(r % 50) : (float)(r % 1000) / 997.0f;
        }
        CBOnsets o = { .nb = n, .fps = 150.0, .full = v };
        double want = first_sorted(&o), got = cb_onsets_first(&o);
        CHECK(got == want, "%d values: first onset %.6f, sorted %.6f", n, got, want);
        /* All equal, and all zero */
        for (float c = 0.0f; c <= 1.0f; c += 1.0f) {
            for (int i = 0; i < n; i++)
                v[i] = c;
            want = first_sorted(&o);
            got = cb_onsets_first(&o);
            CHECK(got == want, "%d values of %g: first onset %.6f, sorted %.6f", n, c, got, want);
        }
        free(v);
    }
    if (fails == fails0)
        printf("ok   the first loud onset, selected in place, as on a sorted copy\n");
}

/* ------------------------------------------------------------- auto cue */

/* The cue of a track of length `duration` (0: not known) */
static double cue_of(const CBGrid *g, double start, double duration)
{
    CBTrack *t = cb_track_new("/test/cue.flac");
    if (!t)
        return -1.0;
    t->duration = duration;
    CBBeatResult br = { .music_start = start };
    if (cb_grid_copy(&br.grid, g) < 0) {
        cb_track_free(t);
        return -1.0;
    }
    int ret = cb_analysis_auto_cues(t, &br);
    double at = ret < 0 ? -1.0 : t->nb_cues == 1 ? t->cues[0].time : NAN;
    cb_grid_free(&br.grid);
    cb_track_free(t);
    return at;
}

/* The process's peak resident memory, MB */
static double peak_mb(void)
{
    struct rusage ru;
    return getrusage(RUSAGE_SELF, &ru) ? 0.0 : ru.ru_maxrss / 1024.0;
}

static void test_auto_cue(void)
{
    int fails0 = fails;
    CBGrid g[3];
    memset(g, 0, sizeof(g));
    bool ok = cb_grid_init(&g[0], 0.5, 120.0) >= 0;
    /* 120 BPM, then 128 BPM from 60 s counting from beat 3 */
    ok = ok && cb_grid_init(&g[1], 0.5, 120.0) >= 0 && cb_grid_add_section(&g[1], 60.0, 128.0, 3) >= 0;
    /* A dynamic stretch: measured beats from 10 s, a little slower each */
    ok = ok && cb_grid_init(&g[2], 0.5, 120.0) >= 0;
    double t = 10.0;
    for (int k = 0; ok && k < 40; k++) {
        double bpm = 120.0 - 0.25 * k;
        ok = cb_grid_add_beat(&g[2], t, bpm, k ? 0 : 1) >= 0;
        t += 60.0 / bpm;
    }
    CHECK(ok, "making the grids");

    static const struct {
        int    grid;
        double start;
        double want;            /* NAN: as with the length */
    } cases[] = {
        { 0,   3.0,  4.5 },
        { 0,   0.0,  0.5 },
        { 1,  61.0, 60.0 + 6 * 60.0 / 128.0 },
        { 2,  12.0, NAN },
        { 2,  40.0, NAN },
        { 0, 500.0, 500.5 },
    };
    for (size_t k = 0; ok && k < CB_ARRAY_ELEMS(cases); k++) {
        const CBGrid *gr = &g[cases[k].grid];
        double peak0 = peak_mb();
        double unknown = cue_of(gr, cases[k].start, 0.0);
        double grew = peak_mb() - peak0;
        double known = cue_of(gr, cases[k].start, 1000.0);
        CHECK(grew < 16.0, "grid %d: the cue without a length took %.0f MB", cases[k].grid, grew);
        CHECK(unknown == known && !isnan(known) && known >= cases[k].start - 0.05,
              "grid %d, music from %.2f s: cue at %.6f without a length, %.6f with",
              cases[k].grid, cases[k].start, unknown, known);
        CHECK(isnan(cases[k].want) || fabs(known - cases[k].want) < 1e-9,
              "grid %d, music from %.2f s: cue at %.6f, want %.6f", cases[k].grid,
              cases[k].start, known, cases[k].want);
    }
    for (int i = 0; i < 3; i++)
        cb_grid_free(&g[i]);
    if (fails == fails0)
        printf("ok   the automatic cue without the track's length\n");
}

/* The beats a measured passage opening a grid gets back to 0 s: the grid is
 * allocated by this count, so a passage starting before 0 s must count none
 * (it counted negative ones once: the grid came out two short and the
 * analysis wrote past it, on a t+pazolite track measured from -1.57 s). */
static void test_back_to_zero(void)
{
    CBBeatObs b[3] = { { .time = 1.30 }, { .time = 1.80 }, { .time = 2.30 } };
    CHECK(cb_beats_back_to_zero(b, 3) == 2, "1.3 s at 0.5 s: %d beats back, want 2",
          cb_beats_back_to_zero(b, 3));
    b[0].time = -1.5672, b[1].time = -1.0668, b[2].time = -0.5664;
    CHECK(cb_beats_back_to_zero(b, 3) == 0, "a passage from -1.57 s: %d beats back, want 0",
          cb_beats_back_to_zero(b, 3));
    b[0].time = 0.0, b[1].time = 0.5;
    CHECK(cb_beats_back_to_zero(b, 3) == 0, "from 0 s: %d, want 0", cb_beats_back_to_zero(b, 3));
    b[0].time = 1.0, b[1].time = 1.0;
    CHECK(cb_beats_back_to_zero(b, 3) == 0, "no interval: %d, want 0", cb_beats_back_to_zero(b, 3));
    CHECK(cb_beats_back_to_zero(b, 1) == 0, "one beat: %d, want 0", cb_beats_back_to_zero(b, 1));
}

int main(void)
{
    const char *base = getenv("CB_TEST_DIR");
    char *dir = cb_asprintf("%s/analysis", base ? base : "build/test-media");
    if (!dir || cb_mkdir_p(dir) < 0) {
        printf("FAIL: cannot make %s\n", dir ? dir : "the test directory");
        free(dir);
        return 1;
    }
    test_onsets();
    test_back_to_zero();
    test_first_onset();
    test_auto_cue();
    test_nonfinite(dir);
    free(dir);
    if (fails) {
        printf("test_analysis: %d failures\n", fails);
        return 1;
    }
    printf("test_analysis: ok\n");
    return 0;
}
