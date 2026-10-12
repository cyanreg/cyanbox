/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The beat-grid analyser on synthetic tracks whose tempo, phase, downbeat
 * and sections are known exactly. Each case asserts exact answers: tempo
 * within 0.05 BPM, phase within 3 ms, the right downbeat, the right
 * sections and flags. The tracks are built from the same ingredients as
 * dj's tests/synth.py (a pitch-swept sine kick, noise hats, chord pads and
 * a bass that changes every bar, a new element every eight bars) plus
 * snares, off-beat bass, live-drum jitter, breakdowns, half-time bars and
 * tempo changes. All noise comes from a fixed LCG: the tracks, and so the
 * results, are the same on every run.
 *
 *   test_beats            run every case
 *   test_beats -v         print the details of every case
 *   test_beats -w N FILE  write case N as raw 44.1 kHz mono f32 (for
 *                         rxbox-eval --dump-onsets) */

#include "rxbox/analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100

/* ------------------------------------------------------------ the synth */

typedef struct Lcg {
    uint64_t s;
} Lcg;

static double lcg_uniform(Lcg *l)
{
    l->s = l->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((l->s >> 11) + 0.5) / 9007199254740992.0;
}

static double lcg_gauss(Lcg *l)
{
    double u = lcg_uniform(l), v = lcg_uniform(l);
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

typedef struct Track {
    float  *x;
    int64_t n;
} Track;

static int track_alloc(Track *t, double seconds)
{
    t->n = (int64_t)(seconds * RATE) + RATE;
    t->x = calloc(t->n, sizeof(*t->x));
    return t->x ? 0 : -1;
}

/* dj's kick: a sine falling from 170 to 50 Hz, 250 ms. */
static void add_kick(Track *t, double at, double gain)
{
    int64_t s0 = (int64_t)llround(at * RATE);
    double ph = 0.0;
    for (int i = 0; i < RATE / 4; i++) {
        double tt = (double)i / RATE;
        double f = 50.0 + 120.0 * exp(-tt * 40.0);
        ph += 2.0 * M_PI * f / RATE;
        int64_t j = s0 + i;
        if (j >= 0 && j < t->n)
            t->x[j] += (float)(gain * sin(ph) * exp(-tt * 12.0));
    }
}

/* Noise burst: hats (bright, 40 ms) or snares (a tone plus noise, 150 ms). */
static void add_hat(Track *t, double at, double gain, Lcg *l)
{
    int64_t s0 = (int64_t)llround(at * RATE);
    double prev = 0.0;
    for (int i = 0; i < (int)(0.04 * RATE); i++) {
        double w = lcg_gauss(l);
        /* First difference: a hat has no low end. */
        double v = w - prev;
        prev = w;
        int64_t j = s0 + i;
        if (j >= 0 && j < t->n)
            t->x[j] += (float)(gain * 0.1 * v * exp(-(double)i / RATE * 120.0));
    }
}

static void add_snare(Track *t, double at, double gain, Lcg *l)
{
    int64_t s0 = (int64_t)llround(at * RATE);
    for (int i = 0; i < (int)(0.15 * RATE); i++) {
        double tt = (double)i / RATE;
        double v = 0.5 * sin(2.0 * M_PI * 190.0 * tt) * exp(-tt * 30.0) +
                   0.6 * lcg_gauss(l) * exp(-tt * 25.0);
        int64_t j = s0 + i;
        if (j >= 0 && j < t->n)
            t->x[j] += (float)(gain * 0.4 * v);
    }
}

static double midi_hz(int m)
{
    return 440.0 * pow(2.0, (m - 69) / 12.0);
}

/* A tone (fundamental + 0.3 of the octave) with soft 10 ms edges. */
static void add_tone(Track *t, double at, double len, double hz, double gain)
{
    int64_t s0 = (int64_t)llround(at * RATE), n = (int64_t)(len * RATE);
    int edge = RATE / 100;
    for (int64_t i = 0; i < n; i++) {
        double tt = (double)i / RATE;
        double env = 1.0;
        if (i < edge)
            env = (double)i / edge;
        if (n - i < edge)
            env = (double)(n - i) / edge;
        int64_t j = s0 + i;
        if (j >= 0 && j < t->n)
            t->x[j] += (float)(gain * env * (sin(2.0 * M_PI * hz * tt) +
                                             0.3 * sin(4.0 * M_PI * hz * tt)));
    }
}

/* What plays in each bar. */
enum {
    BAR_FULL,           /* kick on every beat (or the style's pattern), hats, pads, bass */
    BAR_BREAK,          /* pads only: no drums at all */
    BAR_HALF,           /* half time: kick on 1, snare on 3 */
    BAR_INTRO,          /* pads and a quiet hat, no kick */
};

enum {
    STYLE_HOUSE = 1,    /* four-on-the-floor kick, off-beat hats, claps on 2 and 4 */
    STYLE_DNB,          /* kick on 1 and 3-and, snare on 2 and 4, hats in eighths */
    STYLE_HIPHOP,       /* kick 1 and 3, snare 2 and 4, eighth hats; slow */
    STYLE_TECHHOUSE,    /* four-on-the-floor, but loud open hats and bass on the off-beats */
};

typedef struct Spec {
    const char *name;
    int    style;
    double bpm;
    double first_downbeat;      /* seconds */
    int    bars;
    /* An optional change: from bar `change_bar` on, tempo bpm2, and the
     * new part's first downbeat `change_gap` seconds after where the old
     * grid's bar would have been (0: continuous). */
    int    change_bar;
    double bpm2;
    double change_gap;
    int    style2;                  /* the style after the change (0: the same) */
    int    break_from, break_to;    /* BAR_BREAK bars [from, to) */
    int    half_from, half_to;      /* BAR_HALF bars [from, to) */
    int    intro_bars;              /* BAR_INTRO bars at the start */
    double jitter;                  /* Gaussian timing jitter of every hit, seconds */
    uint64_t seed;
} Spec;

typedef struct Truth {
    int    nb_secs;
    double start[4];        /* downbeat time of each section's first bar */
    double bpm[4];
} Truth;

static int synth(const Spec *sp, Track *t, Truth *truth)
{
    double p1 = 60.0 / sp->bpm, p2 = sp->change_bar ? 60.0 / sp->bpm2 : p1;
    double dur = sp->first_downbeat + 4.0 * p1 * (sp->change_bar ? sp->change_bar : sp->bars) + 2.0;
    if (sp->change_bar)
        dur += sp->change_gap + 4.0 * p2 * (sp->bars - sp->change_bar);
    if (track_alloc(t, dur) < 0)
        return -1;
    Lcg l = { .s = sp->seed * 7919 + 17 };
    static const int prog[4][3] = { { 0, 3, 7 }, { 5, 8, 12 }, { 7, 11, 14 }, { 0, 3, 7 } };
    const int tonic = 9;

    truth->nb_secs = sp->change_bar ? 2 : 1;
    truth->start[0] = sp->first_downbeat;
    truth->bpm[0] = sp->bpm;
    if (sp->change_bar) {
        truth->start[1] = sp->first_downbeat + 4.0 * p1 * sp->change_bar + sp->change_gap;
        truth->bpm[1] = sp->bpm2;
    }

    for (int b = 0; b < sp->bars; b++) {
        bool second = sp->change_bar && b >= sp->change_bar;
        double p = second ? p2 : p1;
        double tb = second ? truth->start[1] + 4.0 * p * (b - sp->change_bar)
                           : sp->first_downbeat + 4.0 * p * b;
        int kind = BAR_FULL;
        if (b < sp->intro_bars)
            kind = BAR_INTRO;
        else if (b >= sp->break_from && b < sp->break_to)
            kind = BAR_BREAK;
        else if (b >= sp->half_from && b < sp->half_to)
            kind = BAR_HALF;

        /* Pads and bass: a chord change every bar, louder every other
         * eight bars, the bass from bar 8: structure on the bar line. */
        const int *ch = prog[b % 4];
        double pad = ((b / 8) % 2) ? 0.06 : 0.10;
        for (int k = 0; k < 3; k++)
            add_tone(t, tb, 4.0 * p, midi_hz(48 + tonic + ch[k]), pad / 3.0);
        if (b >= 8 && kind != BAR_INTRO) {
            if (sp->style == STYLE_TECHHOUSE) {
                for (int q = 0; q < 4; q++)
                    add_tone(t, tb + (q + 0.5) * p, 0.4 * p, midi_hz(36 + tonic + ch[0]), 0.25);
            } else {
                add_tone(t, tb, 4.0 * p, midi_hz(36 + tonic + ch[0]), 0.10);
            }
        }

        if (kind == BAR_BREAK)
            continue;
        /* Every hit jittered the same way (a drummer plays the whole kit
         * early or late on a beat), per beat. */
        for (int q = 0; q < 4; q++) {
            double tq = tb + q * p + (sp->jitter > 0.0 ? sp->jitter * lcg_gauss(&l) : 0.0);
            double th = tq + 0.5 * p;
            if (kind == BAR_INTRO) {
                add_hat(t, th, 0.4, &l);
                continue;
            }
            int style = second && sp->style2 ? sp->style2 : sp->style;
            switch (kind == BAR_HALF ? -1 : style) {
            case -1:
                if (q == 0)
                    add_kick(t, tq, 1.0);
                if (q == 2)
                    add_snare(t, tq, 1.0, &l);
                add_hat(t, tq, 0.6, &l);
                add_hat(t, th, 0.6, &l);
                break;
            case STYLE_HOUSE:
                add_kick(t, tq, 1.0);
                if (q == 1 || q == 3)
                    add_snare(t, tq, 0.5, &l);
                if (b >= 2)
                    add_hat(t, th, 1.0, &l);
                break;
            case STYLE_TECHHOUSE:
                add_kick(t, tq, 0.8);
                add_hat(t, th, 3.0, &l);
                break;
            case STYLE_DNB:
                if (q == 0)
                    add_kick(t, tq, 1.0);
                if (q == 2)
                    add_kick(t, th, 0.9);
                if (q == 1 || q == 3)
                    add_snare(t, tq, 1.0, &l);
                add_hat(t, tq, 0.5, &l);
                add_hat(t, th, 0.5, &l);
                break;
            case STYLE_HIPHOP:
                if (q == 0 || q == 2)
                    add_kick(t, tq, 1.0);
                if (q == 1 || q == 3)
                    add_snare(t, tq, 1.0, &l);
                add_hat(t, tq, 0.5, &l);
                add_hat(t, th, 0.5, &l);
                break;
            }
        }
    }

    float peak = 0.0f;
    for (int64_t i = 0; i < t->n; i++)
        peak = fmaxf(peak, fabsf(t->x[i]));
    for (int64_t i = 0; i < t->n && peak > 0.0f; i++)
        t->x[i] *= 0.8f / peak;
    return 0;
}

/* ------------------------------------------------------------- the cases */

static const Spec cases[] = {
    { .name = "house 124, downbeat 0.3 s", .style = STYLE_HOUSE, .bpm = 124.0,
      .first_downbeat = 0.3, .bars = 48, .seed = 1 },
    { .name = "house 128, 16-bar beatless intro, 8-bar breakdown", .style = STYLE_HOUSE,
      .bpm = 128.0, .first_downbeat = 0.7, .bars = 64, .intro_bars = 16,
      .break_from = 36, .break_to = 44, .seed = 2 },
    { .name = "tech house 126, off-beat hats and bass louder than the kick",
      .style = STYLE_TECHHOUSE, .bpm = 126.0, .first_downbeat = 1.1, .bars = 48, .seed = 3 },
    { .name = "drum & bass 174 (two-step)", .style = STYLE_DNB, .bpm = 174.0,
      .first_downbeat = 0.25, .bars = 64, .seed = 4 },
    { .name = "drum & bass 172 with 8 half-time bars", .style = STYLE_DNB, .bpm = 172.0,
      .first_downbeat = 0.5, .bars = 64, .half_from = 24, .half_to = 32, .seed = 5 },
    { .name = "hip-hop 90", .style = STYLE_HIPHOP, .bpm = 90.0, .first_downbeat = 0.4,
      .bars = 40, .seed = 6 },
    { .name = "live drums 118, 8 ms jitter", .style = STYLE_HOUSE, .bpm = 118.0,
      .first_downbeat = 0.6, .bars = 48, .jitter = 0.008, .seed = 7 },
    { .name = "DJ edit 124 -> 130 at bar 32", .style = STYLE_HOUSE, .bpm = 124.0,
      .first_downbeat = 0.3, .bars = 64, .change_bar = 32, .bpm2 = 130.0, .seed = 8 },
    { .name = "phase cut: 126, re-enters 0.2 s late at bar 32", .style = STYLE_HOUSE,
      .bpm = 126.0, .first_downbeat = 0.45, .bars = 64, .change_bar = 32, .bpm2 = 126.0,
      .change_gap = 0.2, .seed = 9 },
    { .name = "DJ edit house 128 -> drum & bass 174 at bar 32", .style = STYLE_HOUSE,
      .bpm = 128.0, .first_downbeat = 0.35, .bars = 72, .change_bar = 32, .bpm2 = 174.0,
      .style2 = STYLE_DNB, .seed = 10 },
};

typedef struct Want {
    unsigned flags_set;         /* must be set */
    unsigned flags_clear;       /* must not be set */
} Want;

static const Want wants[] = {
    { 0, RX_GRID_FLAG_TEMPO_CHANGE | RX_GRID_FLAG_UNSTEADY | RX_GRID_FLAG_LOW_CONFIDENCE },
    { 0, RX_GRID_FLAG_TEMPO_CHANGE | RX_GRID_FLAG_UNSTEADY },
    { 0, RX_GRID_FLAG_TEMPO_CHANGE | RX_GRID_FLAG_UNSTEADY },
    { 0, RX_GRID_FLAG_TEMPO_CHANGE | RX_GRID_FLAG_UNSTEADY },
    { 0, RX_GRID_FLAG_TEMPO_CHANGE | RX_GRID_FLAG_UNSTEADY },
    { 0, RX_GRID_FLAG_TEMPO_CHANGE | RX_GRID_FLAG_UNSTEADY },
    { RX_GRID_FLAG_UNSTEADY, RX_GRID_FLAG_TEMPO_CHANGE },
    { RX_GRID_FLAG_TEMPO_CHANGE, RX_GRID_FLAG_UNSTEADY },
    { RX_GRID_FLAG_TEMPO_CHANGE, RX_GRID_FLAG_UNSTEADY },
    { RX_GRID_FLAG_TEMPO_CHANGE, RX_GRID_FLAG_UNSTEADY },
};

/* ----------------------------------------------------------- the checks */

static bool verbose;

static int check(int idx)
{
    const Spec *sp = &cases[idx];
    Track t;
    Truth truth;
    if (synth(sp, &t, &truth) < 0)
        return 1;
    RxAnalysisOptions opt;
    rx_analysis_options_default(&opt);
    RxBeatResult r;
    int ret = rx_analyse_beats(t.x, t.n, RATE, &opt, &r);
    free(t.x);
    if (ret < 0) {
        printf("FAIL %s: analysis returned %d\n", sp->name, ret);
        return 1;
    }

    int fails = 0;
    char why[512] = "";
#define FAILF(...) do { \
        size_t l_ = strlen(why); \
        snprintf(why + l_, sizeof(why) - l_, __VA_ARGS__); \
        fails++; \
    } while (0)

    if (r.grid.nb_sections != truth.nb_secs)
        FAILF(" sections %d (want %d)", r.grid.nb_sections, truth.nb_secs);
    for (int s = 0; s < RX_MIN(r.grid.nb_sections, truth.nb_secs); s++) {
        const RxTempoSection *g = &r.grid.sections[s];
        if (fabs(g->bpm - truth.bpm[s]) > 0.05)
            FAILF(" bpm[%d] %.4f (want %g)", s, g->bpm, truth.bpm[s]);
        double p = 60.0 / truth.bpm[s];
        /* Phase: the grid's beats against the true beats of the section. */
        double d = fmod(g->start - truth.start[s], p);
        if (d > p / 2)
            d -= p;
        if (d < -p / 2)
            d += p;
        if (fabs(d) > 0.003)
            FAILF(" phase[%d] off by %.2f ms", s, d * 1e3);
        /* Downbeat: the first section's start is a downbeat; a later
         * section's start must be its own first downbeat when it numbers
         * itself (beat 1), or continue the count onto it (beat 0). */
        double bars = (g->start - truth.start[s]) / (4.0 * p);
        bool on_bar = fabs(bars - round(bars)) * 4.0 * p < 0.010;
        if (s == 0 && !on_bar)
            FAILF(" downbeat off by %.2f beats", (bars - round(bars)) * 4.0);
        if (s > 0) {
            /* The boundary itself: at the new part's first beat, which is
             * its downbeat, numbered 1 or counted on to 1. */
            if (fabs(g->start - truth.start[s]) > 0.003)
                FAILF(" boundary at %.4f (want %.4f)", g->start, truth.start[s]);
            int num = g->beat;
            if (!num) {
                const RxTempoSection *pg = &r.grid.sections[s - 1];
                int pnum = pg->beat ? pg->beat : 1;
                long beats = lrint((g->start - pg->start) * pg->bpm / 60.0);
                num = (int)((pnum - 1 + beats) % 4) + 1;
            }
            if (num != 1)
                FAILF(" boundary is beat %d of its bar (beat field %d)", num, g->beat);
        }
    }
    const Want *w = &wants[idx];
    if ((r.flags & w->flags_set) != w->flags_set)
        FAILF(" flags 0x%x lack 0x%x", r.flags, w->flags_set);
    if (r.flags & w->flags_clear)
        FAILF(" flags 0x%x include 0x%x", r.flags, r.flags & w->flags_clear);

    printf("%s %s%s\n", fails ? "FAIL" : "ok  ", sp->name, why);
    if (verbose || fails) {
        printf("      conf %.2f flags 0x%02x octave %.2f phase %.2f 2nd %.2f first %.3f\n",
               r.confidence, r.flags, r.octave_margin, r.phase_margin, r.bpm_second_best,
               r.first_onset);
        for (int s = 0; s < r.grid.nb_sections; s++)
            printf("      section %d: start %.5f bpm %.4f beat %d\n", s, r.grid.sections[s].start,
                   r.grid.sections[s].bpm, r.grid.sections[s].beat);
    }
    rx_beat_result_free(&r);
    return fails ? 1 : 0;
}

static int check_misc(void)
{
    int fails = 0;
    /* Flag names round-trip. */
    for (unsigned f = 1; f <= RX_GRID_FLAG_UNSTEADY; f <<= 1) {
        const char *n = rx_grid_flag_name((enum RxGridFlag)f);
        if (!n || !*n || rx_grid_flag_parse(n) != f) {
            printf("FAIL flag name 0x%x\n", f);
            fails++;
        }
    }
    if (rx_grid_flag_parse("bogus") != 0) {
        printf("FAIL flag parse bogus\n");
        fails++;
    }

    /* Silence and a too-short input give an empty grid, not an error. */
    float *z = calloc(RATE * 20, sizeof(*z));
    RxBeatResult r;
    if (rx_analyse_beats(z, RATE * 20, RATE, NULL, &r) < 0 || r.grid.nb_sections != 0) {
        printf("FAIL silence: %d sections\n", r.grid.nb_sections);
        fails++;
    }
    rx_beat_result_free(&r);
    if (rx_analyse_beats(z, RATE * 2, RATE, NULL, &r) < 0 || r.grid.nb_sections != 0) {
        printf("FAIL short input\n");
        fails++;
    }
    rx_beat_result_free(&r);
    free(z);

    /* Determinism, and another sample rate: the same track at 48 kHz
     * (resampled by the analyser) gives the same grid within 0.5 ms. */
    Track t;
    Truth truth;
    if (synth(&cases[0], &t, &truth) < 0)
        return fails + 1;
    RxBeatResult a, b;
    rx_analyse_beats(t.x, t.n, RATE, NULL, &a);
    rx_analyse_beats(t.x, t.n, RATE, NULL, &b);
    if (a.grid.nb_sections != b.grid.nb_sections || !a.grid.nb_sections ||
        a.grid.sections[0].start != b.grid.sections[0].start ||
        a.grid.sections[0].bpm != b.grid.sections[0].bpm || a.confidence != b.confidence) {
        printf("FAIL determinism\n");
        fails++;
    }
    int64_t n48 = t.n * 48000 / RATE;
    float *x48 = malloc(n48 * sizeof(*x48));
    for (int64_t i = 0; i < n48; i++) {
        /* Linear interpolation is enough for a test of time alignment. */
        double pos = (double)i * RATE / 48000.0;
        int64_t j = (int64_t)pos;
        double f = pos - j;
        x48[i] = (float)(j + 1 < t.n ? t.x[j] * (1 - f) + t.x[j + 1] * f : 0.0);
    }
    RxBeatResult c;
    rx_analyse_beats(x48, n48, 48000, NULL, &c);
    if (!c.grid.nb_sections || fabs(c.grid.sections[0].start - a.grid.sections[0].start) > 0.0005 ||
        fabs(c.grid.sections[0].bpm - a.grid.sections[0].bpm) > 0.01) {
        printf("FAIL 48 kHz: start %.5f bpm %.4f vs %.5f %.4f\n",
               c.grid.nb_sections ? c.grid.sections[0].start : 0.0,
               c.grid.nb_sections ? c.grid.sections[0].bpm : 0.0,
               a.grid.sections[0].start, a.grid.sections[0].bpm);
        fails++;
    } else {
        printf("ok   determinism and 48 kHz input\n");
    }
    rx_beat_result_free(&a);
    rx_beat_result_free(&b);
    rx_beat_result_free(&c);
    free(x48);
    free(t.x);
    return fails;
}

/* rx_analyse on a stereo 48 kHz track (everything requested), and the
 * automatic memory cue. */
static int check_analyse(void)
{
    int fails = 0;
    Track t;
    Truth truth;
    if (synth(&cases[0], &t, &truth) < 0)
        return 1;
    RxBeatResult mono;
    rx_analyse_beats(t.x, t.n, RATE, NULL, &mono);

    int64_t n48 = t.n * 48000 / RATE;
    RxAudio au = {
        .samples   = malloc(n48 * 2 * sizeof(float)),
        .channels  = 2,
        .rate      = 48000,
        .nb_frames = n48,
    };
    if (!au.samples) {
        free(t.x);
        return 1;
    }
    for (int64_t i = 0; i < n48; i++) {
        double pos = (double)i * RATE / 48000.0;
        int64_t j = (int64_t)pos;
        double f = pos - j;
        float v = (float)(j + 1 < t.n ? t.x[j] * (1 - f) + t.x[j + 1] * f : 0.0);
        au.samples[2 * i] = au.samples[2 * i + 1] = v;
    }
    RxAnalysis a;
    int ret = rx_analyse(&au, NULL, RX_ANALYSE_ALL, &a);
    if (ret < 0 || a.done != RX_ANALYSE_ALL) {
        printf("FAIL rx_analyse: ret %d done 0x%x\n", ret, a.done);
        fails++;
    } else if (!a.beats.grid.nb_sections || !mono.grid.nb_sections ||
               fabs(a.beats.grid.sections[0].start - mono.grid.sections[0].start) > 0.0005 ||
               fabs(a.beats.grid.sections[0].bpm - mono.grid.sections[0].bpm) > 0.01 ||
               a.waves.nb_detail <= 0) {
        printf("FAIL rx_analyse: grid %.5f@%.3f vs %.5f@%.3f, %d detail columns\n",
               a.beats.grid.nb_sections ? a.beats.grid.sections[0].start : 0.0,
               a.beats.grid.nb_sections ? a.beats.grid.sections[0].bpm : 0.0,
               mono.grid.sections[0].start, mono.grid.sections[0].bpm, a.waves.nb_detail);
        fails++;
    } else {
        printf("ok   rx_analyse (stereo 48 kHz): grid, key \"%s\", %d waveform columns\n",
               a.key.key, a.waves.nb_detail);
    }

    /* The cue: the first downbeat at or after the music starts, musical,
     * once; nothing for a track that already has cues. */
    RxTrack *tr = rx_track_new("/test/house124.flac");
    if (tr && a.beats.grid.nb_sections) {
        tr->duration = a.duration;
        double first = a.beats.first_onset;
        rx_analysis_auto_cues(tr, &a.beats);
        rx_analysis_auto_cues(tr, &a.beats);
        double want = truth.start[0];
        double bar = 4.0 * 60.0 / truth.bpm[0];
        while (want < first - 0.05)
            want += bar;
        if (tr->nb_cues != 1 || fabs(tr->cues[0].time - want) > 0.003 ||
            !rx_cue_is_musical(&tr->cues[0]) || rx_cue_is_hot(&tr->cues[0])) {
            printf("FAIL auto cue: %d cues, at %.4f (want %.4f)\n", tr->nb_cues,
                   tr->nb_cues ? tr->cues[0].time : 0.0, want);
            fails++;
        } else {
            printf("ok   auto cue at the first downbeat (%.3f s)\n", tr->cues[0].time);
        }
        /* A track with a cue of its own gets none. */
        RxTrack *t2 = rx_track_new("/test/has-cue.flac");
        if (t2) {
            t2->duration = a.duration;
            RxCue own;
            rx_cue_init(&own, 30.0);
            rx_track_add_cue(t2, &own);
            rx_analysis_auto_cues(t2, &a.beats);
            if (t2->nb_cues != 1) {
                printf("FAIL auto cue added to a track that has cues\n");
                fails++;
            }
            rx_track_free(t2);
        }
    }
    rx_track_free(tr);
    rx_analysis_free(&a);
    rx_beat_result_free(&mono);
    free(au.samples);
    free(t.x);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "-w")) {
        int i = atoi(argv[2]);
        if (i < 0 || i >= (int)RX_ARRAY_ELEMS(cases))
            return 1;
        Track t;
        Truth truth;
        if (synth(&cases[i], &t, &truth) < 0)
            return 1;
        FILE *f = fopen(argv[3], "wb");
        if (!f)
            return 1;
        fwrite(t.x, sizeof(float), t.n, f);
        fclose(f);
        for (int s = 0; s < truth.nb_secs; s++)
            printf("section %d: downbeat %.5f bpm %g\n", s, truth.start[s], truth.bpm[s]);
        free(t.x);
        return 0;
    }
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    int fails = check_misc();
    fails += check_analyse();
    for (size_t i = 0; i < RX_ARRAY_ELEMS(cases); i++)
        fails += check((int)i);
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
