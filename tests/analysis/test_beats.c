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
 * Every track starts after digital silence with a hit on beat 1. The cases
 * on where the music starts (docs/BEATS.md) vary that: longer silences,
 * fade-ins linear in dB from -60 dB, a pad fading in before the first hit, a
 * noise floor, a stab on the "and"
 * before bar 1 and a snare pickup on beat 4. For them the grid's first
 * beat, its number and the automatic cue are checked too.
 *
 * Three tracks have a tempo that moves (a ramp, a random walk as a drummer
 * drifts, a rubato dip), each with every beat's true time: for them the
 * grid is checked beat by beat against the truth (the RMS and the largest
 * error, and that the downbeats fall on the bar lines), the dynamic grid
 * against the best straight one (`allow_dynamic` off), which it must beat.
 *
 * The GPU front end (rxbox/compute.h) must give the same results. When a
 * device is there, every case runs on both paths: each must pass on its
 * own, and the two grids must be the same (sections, their tempo and
 * beat numbers, their starts within 0.1 ms, the same flags and start of
 * the music). The front end's arrays are compared directly too: the
 * spectrum's and the envelopes' (see check_frontend for the tolerance).
 * RXBOX_ANALYSIS_GPU=0 runs the CPU path only; =1 the GPU path only, and
 * fails without a device; unset, the GPU half is skipped with a note when
 * there is no device. RXBOX_GPU_VALIDATION=1 runs the GPU half under the
 * Khronos validation layer and fails on any error it reports.
 *
 *   test_beats            run every case
 *   test_beats -v         print the details of every case
 *   test_beats -c N       run case N alone, on the CPU path
 *   test_beats -w N FILE  write case N as raw 44.1 kHz mono f32 (for
 *                         rxbox-eval --dump-onsets) */

#include "rxbox/analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxbox/compute.h"
#include "src/analysis/beats_internal.h"

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
    STYLE_TWOSTEP,      /* UK garage: kick on 1, the "and" of 2 and the "and" of 3, snare on 2
                         * and 4, hats on the "and"s: the kicks lean to the off-beat */
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
    bool   one_line;                /* the change is too small to be a section */
    int    break_from, break_to;    /* BAR_BREAK bars [from, to) */
    int    half_from, half_to;      /* BAR_HALF bars [from, to) */
    int    intro_bars;              /* BAR_INTRO bars at the start */
    double jitter;                  /* Gaussian timing jitter of every hit, seconds */
    uint64_t seed;
    /* Where the music starts. */
    double fade;                    /* a fade-in over this long from the first downbeat, linear
                                     * in dB from -60 dB */
    double pad;                     /* a pad fading in over this long before the first downbeat,
                                     * -60 to -24 dB, no drums under it: the music starts with
                                     * the hit on beat 1 after it */
    double noise_db;                /* white noise through the track at this level (dB RMS) */
    int    pickup;                  /* PICKUP_*: what comes before bar 1 */
    double click;                   /* a one-sample click in the silence at this time */
    bool   start_checks;            /* check the grid's first beat and the automatic cue */
    /* A tempo that moves: a ramp from `bpm` at bar ramp_from to ramp_bpm
     * at bar ramp_to (then held); a random walk of the tempo, walk_sigma
     * per beat in log tempo, held within 10 % of `bpm`; a rubato dip by
     * the fraction `rubato` of the tempo over bars [rubato_from,
     * rubato_to), a half sine. Any of these gives every beat its own
     * time, and the truth is the list of them. */
    double ramp_bpm;
    int    ramp_from, ramp_to;
    double walk_sigma;
    double rubato;
    int    rubato_from, rubato_to;
} Spec;

enum {
    PICKUP_NONE,
    PICKUP_STAB,                    /* a chord stab on the "and" of 4 before bar 1, no kick */
    PICKUP_SNARE,                   /* a snare on beat 4 before bar 1 */
};

typedef struct Truth {
    int    nb_secs;
    double start[4];        /* downbeat time of each section's first bar */
    double bpm[4];
    double first_beat;      /* the grid's first beat: the first sound on a beat */
    int    first_number;    /* and its number */
    /* A tempo that moves: the true time of every beat (malloc'd), bar
     * lines every four; nb_secs is 0 and the grid is judged beat by beat. */
    double *beats;
    int     nb_beats;
} Truth;

static bool curved(const Spec *sp)
{
    return sp->ramp_bpm > 0.0 || sp->walk_sigma > 0.0 || sp->rubato > 0.0;
}

/* The beats of a tempo that moves: 4 * bars + 1 times from the first
 * downbeat, each interval 60 over the tempo at that beat. */
static double *curve_beats(const Spec *sp)
{
    int nb = 4 * sp->bars + 1;
    double *bt = malloc((size_t)nb * sizeof(*bt));
    if (!bt)
        return NULL;
    Lcg l = { .s = sp->seed * 104729 + 3 };
    double logt = 0.0, t = sp->first_downbeat;
    for (int k = 0; k < nb; k++) {
        bt[k] = t;
        double x = k / 4.0, bpm = sp->bpm;
        if (sp->ramp_bpm > 0.0) {
            if (x >= sp->ramp_to)
                bpm = sp->ramp_bpm;
            else if (x > sp->ramp_from)
                bpm += (sp->ramp_bpm - sp->bpm) * (x - sp->ramp_from) / (sp->ramp_to - sp->ramp_from);
        }
        if (sp->rubato > 0.0 && x >= sp->rubato_from && x < sp->rubato_to)
            bpm *= 1.0 - sp->rubato * sin(M_PI * (x - sp->rubato_from) / (sp->rubato_to - sp->rubato_from));
        if (sp->walk_sigma > 0.0) {
            logt += sp->walk_sigma * lcg_gauss(&l);
            logt = RX_CLAMP(logt, log(0.9), log(1.1));
            bpm *= exp(logt);
        }
        t += 60.0 / bpm;
    }
    return bt;
}

static int synth(const Spec *sp, Track *t, Truth *truth)
{
    double p1 = 60.0 / sp->bpm, p2 = sp->change_bar ? 60.0 / sp->bpm2 : p1;
    double dur = sp->first_downbeat + 4.0 * p1 * (sp->change_bar ? sp->change_bar : sp->bars) + 2.0;
    if (sp->change_bar)
        dur += sp->change_gap + 4.0 * p2 * (sp->bars - sp->change_bar);
    double *bt = NULL;
    memset(truth, 0, sizeof(*truth));
    if (curved(sp)) {
        bt = curve_beats(sp);
        if (!bt)
            return -1;
        dur = bt[4 * sp->bars] + 2.0;
    }
    if (track_alloc(t, dur) < 0) {
        free(bt);
        return -1;
    }
    Lcg l = { .s = sp->seed * 7919 + 17 };
    static const int prog[4][3] = { { 0, 3, 7 }, { 5, 8, 12 }, { 7, 11, 14 }, { 0, 3, 7 } };
    const int tonic = 9;

    truth->nb_secs = sp->change_bar && !sp->one_line ? 2 : 1;
    if (bt) {
        truth->nb_secs = 0;
        truth->beats = bt;
        truth->nb_beats = 4 * sp->bars;
    }
    truth->start[0] = sp->first_downbeat;
    truth->bpm[0] = sp->bpm;
    truth->first_beat = sp->first_downbeat - (sp->pickup == PICKUP_SNARE ? p1 : 0.0);
    truth->first_number = sp->pickup == PICKUP_SNARE ? 4 : 1;
    if (sp->pickup == PICKUP_STAB) {
        /* A chord stab, 150 ms, on the "and" of the beat before bar 1. */
        static const int stab[3] = { 0, 3, 7 };
        for (int k = 0; k < 3; k++)
            add_tone(t, sp->first_downbeat - 0.5 * p1, 0.15, midi_hz(60 + 9 + stab[k]), 0.15);
    } else if (sp->pickup == PICKUP_SNARE) {
        add_snare(t, sp->first_downbeat - p1, 1.0, &l);
    }
    double start2 = sp->first_downbeat + 4.0 * p1 * sp->change_bar + sp->change_gap;
    if (sp->change_bar && !sp->one_line) {
        truth->start[1] = start2;
        truth->bpm[1] = sp->bpm2;
    }

    for (int b = 0; b < sp->bars; b++) {
        bool second = sp->change_bar && b >= sp->change_bar;
        double p = second ? p2 : p1;
        double tb = second ? start2 + 4.0 * p * (b - sp->change_bar)
                           : sp->first_downbeat + 4.0 * p * b;
        double bar_len = 4.0 * p;
        if (bt) {
            /* The bar as the tempo curve has it; p is its mean beat. */
            tb = bt[4 * b];
            bar_len = bt[4 * b + 4] - tb;
            p = bar_len / 4.0;
        }
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
            add_tone(t, tb, bar_len, midi_hz(48 + tonic + ch[k]), pad / 3.0);
        if (b >= 8 && kind != BAR_INTRO) {
            if (sp->style == STYLE_TECHHOUSE) {
                for (int q = 0; q < 4; q++)
                    add_tone(t, tb + (q + 0.5) * p, 0.4 * p, midi_hz(36 + tonic + ch[0]), 0.25);
            } else {
                add_tone(t, tb, bar_len, midi_hz(36 + tonic + ch[0]), 0.10);
            }
        }

        if (kind == BAR_BREAK)
            continue;
        /* Every hit jittered the same way (a drummer plays the whole kit
         * early or late on a beat), per beat. */
        for (int q = 0; q < 4; q++) {
            double tq = tb + q * p + (sp->jitter > 0.0 ? sp->jitter * lcg_gauss(&l) : 0.0);
            double th = tq + 0.5 * p;
            if (bt) {
                tq = bt[4 * b + q] + (sp->jitter > 0.0 ? sp->jitter * lcg_gauss(&l) : 0.0);
                th = tq + 0.5 * (bt[4 * b + q + 1] - bt[4 * b + q]);
            }
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
            case STYLE_TWOSTEP:
                if (q == 0)
                    add_kick(t, tq, 1.0);
                if (q == 1 || q == 2)
                    add_kick(t, th, 1.0);
                if (q == 1 || q == 3)
                    add_snare(t, tq, 0.6, &l);
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

    /* A fade-in: the gain rises linearly in dB from -60 dB at the first
     * downbeat to 0 dB `fade` seconds later. */
    if (sp->fade > 0.0) {
        int64_t a = (int64_t)llround(sp->first_downbeat * RATE);
        int64_t b = RX_MIN(a + (int64_t)llround(sp->fade * RATE), t->n);
        for (int64_t i = a; i < b; i++)
            t->x[i] *= (float)pow(10.0, (-60.0 + 60.0 * (i - a) / (sp->fade * RATE)) / 20.0);
    }

    /* A pad before the music: a soft chord fading in, linear in dB from
     * -60 dB to -24 dB over `pad` seconds up to the first downbeat, no
     * drums under it; the music then starts with its hit on beat 1, as
     * pushover's "PASSIVE" does. */
    if (sp->pad > 0.0) {
        int64_t b = (int64_t)llround(sp->first_downbeat * RATE);
        int64_t a = RX_MAX(b - (int64_t)llround(sp->pad * RATE), 0);
        static const double hz[3] = { 220.0, 277.18, 329.63 };
        for (int64_t i = a; i < b; i++) {
            double g = 0.06 * pow(10.0, (-36.0 + 36.0 * (i - a) / (double)(b - a)) / 20.0);
            double v = 0.0;
            for (int k = 0; k < 3; k++)
                v += sin(2.0 * M_PI * hz[k] * i / RATE);
            t->x[i] += (float)(g * v / 3.0);
        }
    }

    float peak = 0.0f;
    for (int64_t i = 0; i < t->n; i++)
        peak = fmaxf(peak, fabsf(t->x[i]));
    for (int64_t i = 0; i < t->n && peak > 0.0f; i++)
        t->x[i] *= 0.8f / peak;

    /* A click in the silence (a CD's pregap has them), at full scale. */
    if (sp->click > 0.0)
        t->x[(int64_t)llround(sp->click * RATE)] = 0.9f;

    /* A noise floor under everything, at an absolute level. */
    if (sp->noise_db < 0.0) {
        double g = pow(10.0, sp->noise_db / 20.0);
        for (int64_t i = 0; i < t->n; i++)
            t->x[i] += (float)(g * lcg_gauss(&l));
    }
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
    { .name = "re-entry 8 ms late at bar 32 (no cut: within the merge tolerance)",
      .style = STYLE_HOUSE, .bpm = 125.0, .first_downbeat = 0.4, .bars = 64, .change_bar = 32,
      .bpm2 = 125.0, .change_gap = 0.008, .one_line = true, .seed = 11 },
    { .name = "0.5 s of silence, a hit on beat 1", .style = STYLE_HOUSE, .bpm = 126.0,
      .first_downbeat = 0.5, .bars = 40, .seed = 12, .start_checks = true },
    { .name = "1.7 s of silence, a hit on beat 1", .style = STYLE_HOUSE, .bpm = 128.0,
      .first_downbeat = 1.7, .bars = 40, .seed = 13, .start_checks = true },
    { .name = "3.3 s of silence, a hit on beat 1", .style = STYLE_TECHHOUSE, .bpm = 124.0,
      .first_downbeat = 3.3, .bars = 40, .seed = 14, .start_checks = true },
    { .name = "fade-in over 2 s from -60 dB", .style = STYLE_HOUSE, .bpm = 125.0,
      .first_downbeat = 0.4, .bars = 40, .seed = 15, .fade = 2.0, .start_checks = true },
    { .name = "fade-in over 4 s from -60 dB", .style = STYLE_HOUSE, .bpm = 130.0,
      .first_downbeat = 0.6, .bars = 40, .seed = 16, .fade = 4.0, .start_checks = true },
    { .name = "fade-in over 8 s from -60 dB", .style = STYLE_DNB, .bpm = 172.0,
      .first_downbeat = 0.35, .bars = 64, .seed = 17, .fade = 8.0, .start_checks = true },
    { .name = "a pad fading in for 2 s, then the hit on beat 1", .style = STYLE_HOUSE, .bpm = 130.0,
      .first_downbeat = 3.6, .bars = 40, .seed = 23, .pad = 2.0, .start_checks = true },
    { .name = "a -75 dB noise floor, then a hit on beat 1", .style = STYLE_HOUSE, .bpm = 127.0,
      .first_downbeat = 1.3, .bars = 40, .seed = 18, .noise_db = -75.0, .start_checks = true },
    { .name = "a click in the silence, then a hit on beat 1", .style = STYLE_HOUSE,
      .bpm = 126.0, .first_downbeat = 1.1, .bars = 40, .seed = 21, .click = 0.3,
      .start_checks = true },
    { .name = "two-step garage 134 after 0.72 s of silence", .style = STYLE_TWOSTEP,
      .bpm = 134.0, .first_downbeat = 0.72, .bars = 48, .seed = 22, .start_checks = true },
    { .name = "a stab on the \"and\" before bar 1", .style = STYLE_HOUSE, .bpm = 124.0,
      .first_downbeat = 0.9, .bars = 40, .seed = 19, .pickup = PICKUP_STAB, .start_checks = true },
    { .name = "a snare pickup on beat 4 before bar 1", .style = STYLE_HOUSE, .bpm = 128.0,
      .first_downbeat = 1.2, .bars = 40, .seed = 20, .pickup = PICKUP_SNARE,
      .start_checks = true },
    /* A tempo that moves. */
    { .name = "ramp 120 -> 132 over bars 8-40, then held", .style = STYLE_HOUSE, .bpm = 120.0,
      .first_downbeat = 0.5, .bars = 56, .seed = 24, .ramp_bpm = 132.0, .ramp_from = 8,
      .ramp_to = 40 },
    { .name = "a drummer's random walk (0.4 % a beat, within 10 %), 6 ms jitter, 122",
      .style = STYLE_HOUSE, .bpm = 122.0, .first_downbeat = 0.4, .bars = 64, .seed = 25,
      .walk_sigma = 0.004, .jitter = 0.006 },
    { .name = "rubato: 120 dipping to 100 over bars 16-24, steady either side",
      .style = STYLE_HIPHOP, .bpm = 120.0, .first_downbeat = 0.6, .bars = 48, .seed = 26,
      .rubato = 0.17, .rubato_from = 16, .rubato_to = 24, .jitter = 0.003 },
};

typedef struct Want {
    unsigned flags_set;         /* must be set */
    unsigned flags_clear;       /* must not be set */
} Want;

/* One section: neither kind of cut. */
#define CUTS (RX_GRID_FLAG_TEMPO_CHANGE | RX_GRID_FLAG_PHASE_CUT)

static const Want wants[] = {
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY | RX_GRID_FLAG_LOW_CONFIDENCE },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { RX_GRID_FLAG_UNSTEADY, CUTS },
    { RX_GRID_FLAG_TEMPO_CHANGE, RX_GRID_FLAG_UNSTEADY | RX_GRID_FLAG_PHASE_CUT },
    { RX_GRID_FLAG_PHASE_CUT, RX_GRID_FLAG_UNSTEADY | RX_GRID_FLAG_TEMPO_CHANGE },
    { RX_GRID_FLAG_TEMPO_CHANGE, RX_GRID_FLAG_UNSTEADY | RX_GRID_FLAG_PHASE_CUT },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { 0, CUTS | RX_GRID_FLAG_UNSTEADY },
    { RX_GRID_FLAG_DYNAMIC, CUTS },
    { RX_GRID_FLAG_DYNAMIC, CUTS },
    { RX_GRID_FLAG_DYNAMIC, CUTS },
};

/* ----------------------------------------------------------- the checks */

/* The grid against the true beats, from the grid's first beat on: the RMS
 * and the largest distance of each true beat from the nearest grid beat,
 * and how many true bar lines have a grid downbeat (number 1) nearest
 * them. */
typedef struct BeatError {
    double rms, max;
    int    nb, down_ok, down;
} BeatError;

static BeatError beat_error(const RXGrid *g, double dur, const double *truth, int nb_truth)
{
    BeatError e = { 0 };
    RXBeat *b = NULL;
    int nb = 0;
    if (rx_grid_beats(g, dur, &b, &nb) < 0 || nb == 0) {
        free(b);
        e.rms = e.max = INFINITY;
        return e;
    }
    double s2 = 0.0;
    for (int k = 0; k < nb_truth; k++) {
        double t = truth[k];
        if (t < b[0].time - 0.001)
            continue;
        int lo = 0, hi = nb - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (b[mid].time < t)
                lo = mid + 1;
            else
                hi = mid;
        }
        int j = lo;
        if (lo > 0 && fabs(b[lo - 1].time - t) < fabs(b[lo].time - t))
            j = lo - 1;
        double d = fabs(b[j].time - t);
        s2 += d * d;
        e.max = fmax(e.max, d);
        e.nb++;
        if (k % 4 == 0) {
            e.down++;
            e.down_ok += b[j].number == 1;
        }
    }
    free(b);
    e.rms = e.nb ? sqrt(s2 / e.nb) : INFINITY;
    return e;
}

static bool verbose;

/* Case idx on the CPU (gpu NULL) or the GPU. The result is kept in *keep
 * for the comparison of the two, when keep is given. */
static int check(int idx, RXCompute *gpu, RXBeatResult *keep)
{
    const Spec *sp = &cases[idx];
    const char *path = gpu ? "[gpu] " : "";
    Track t;
    Truth truth;
    if (synth(sp, &t, &truth) < 0)
        return 1;
    RXAnalysisOptions opt;
    rx_analysis_options_default(&opt);
    opt.compute = gpu;
    RXBeatResult r;
    int ret = rx_analyse_beats(t.x, t.n, RATE, &opt, &r);
    if (ret < 0) {
        printf("FAIL %s%s: analysis returned %d\n", path, sp->name, ret);
        free(t.x);
        return 1;
    }

    int fails = 0;
    char why[512] = "";
#define FAILF(...) do { \
        size_t l_ = strlen(why); \
        snprintf(why + l_, sizeof(why) - l_, __VA_ARGS__); \
        fails++; \
    } while (0)

    double dur = (double)t.n / RATE;
    char curve_note[160] = "";
    if (truth.beats) {
        /* Beat by beat: within 15 ms everywhere (RMS under 6), every bar
         * line a downbeat; and closer than the best straight grid, which
         * is measured on the CPU path only (it is the same on both). */
        BeatError d = beat_error(&r.grid, dur, truth.beats, truth.nb_beats);
        if (!(d.rms <= 0.006))
            FAILF(" rms %.1f ms", d.rms * 1e3);
        if (!(d.max <= 0.015))
            FAILF(" max %.1f ms", d.max * 1e3);
        if (d.down_ok < d.down)
            FAILF(" downbeats %d of %d", d.down_ok, d.down);
        if (!rx_grid_dynamic(&r.grid))
            FAILF(" not a dynamic grid");
        snprintf(curve_note, sizeof(curve_note), "      dynamic: rms %.1f ms, max %.1f ms, "
                 "downbeats %d/%d, %d beats", d.rms * 1e3, d.max * 1e3, d.down_ok, d.down,
                 r.grid.nb_sections);
        if (!gpu) {
            RXAnalysisOptions so = opt;
            so.allow_dynamic = false;
            RXBeatResult sr;
            if (rx_analyse_beats(t.x, t.n, RATE, &so, &sr) == 0) {
                BeatError e = beat_error(&sr.grid, dur, truth.beats, truth.nb_beats);
                size_t l = strlen(curve_note);
                snprintf(curve_note + l, sizeof(curve_note) - l,
                         "; straight: rms %.1f ms, max %.1f ms, %d section%s", e.rms * 1e3,
                         e.max * 1e3, sr.grid.nb_sections, sr.grid.nb_sections == 1 ? "" : "s");
                if (!(d.rms < e.rms))
                    FAILF(" no better than straight (%.1f ms)", e.rms * 1e3);
                rx_beat_result_free(&sr);
            }
        }
    }
    free(t.x);
    if (!truth.beats && r.grid.nb_sections != truth.nb_secs)
        FAILF(" sections %d (want %d)", r.grid.nb_sections, truth.nb_secs);
    for (int s = 0; s < RX_MIN(r.grid.nb_sections, truth.nb_secs); s++) {
        const RXTempoSection *g = &r.grid.sections[s];
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
        /* Downbeat: the first section's first downbeat (its start is its
         * first beat, numbered) is on a bar line; a later section's start
         * must be its own first downbeat when it numbers itself (beat 1),
         * or continue the count onto it (beat 0). */
        double down = g->start;
        if (s == 0)
            down += ((5 - (g->beat ? g->beat : 1)) % 4) * p;
        double bars = (down - truth.start[s]) / (4.0 * p);
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
                const RXTempoSection *pg = &r.grid.sections[s - 1];
                int pnum = pg->beat ? pg->beat : 1;
                long beats = lrint((g->start - pg->start) * pg->bpm / 60.0);
                num = (int)((pnum - 1 + beats) % 4) + 1;
            }
            if (num != 1)
                FAILF(" boundary is beat %d of its bar (beat field %d)", num, g->beat);
        }
    }
    /* The grid as the stick gets it (rxbox/model.h's expansion): no two
     * beats closer than half a period, and every section's first downbeat
     * numbered 1. */
    RXBeat *bt = NULL;
    int nbt = 0;
    if (rx_grid_beats(&r.grid, dur, &bt, &nbt) == 0) {
        for (int i = 1; i < nbt; i++) {
            double gap = bt[i].time - bt[i - 1].time;
            if (gap < 0.5 * 60.0 / fmax(bt[i].bpm, bt[i - 1].bpm)) {
                FAILF(" beats %.4f and %.4f only %.1f ms apart", bt[i - 1].time, bt[i].time,
                      gap * 1e3);
                break;
            }
        }
        for (int s = 1; s < truth.nb_secs; s++) {
            for (int i = 0; i < nbt; i++) {
                if (fabs(bt[i].time - truth.start[s]) < 0.010 && bt[i].number != 1)
                    FAILF(" section %d's downbeat is numbered %d", s, bt[i].number);
            }
        }
        free(bt);
    }
    /* Where the music starts: the grid's first beat is the first sound on
     * a beat, numbered as such, nothing before it (no beat to snap to, no
     * bar), and the automatic cue on the first downbeat. */
    if (sp->start_checks) {
        double p = 60.0 / truth.bpm[0];
        if (rx_grid_beats(&r.grid, dur, &bt, &nbt) == 0 && nbt > 0) {
            if (fabs(bt[0].time - truth.first_beat) > 0.005)
                FAILF(" first beat %.4f (want %.4f)", bt[0].time, truth.first_beat);
            if (bt[0].number != truth.first_number)
                FAILF(" first beat numbered %d (want %d)", bt[0].number, truth.first_number);
            free(bt);
        } else {
            FAILF(" no beats");
        }
        double before = truth.first_beat - 1.5 * p;
        int bar = -1, beat = -1;
        rx_grid_bar_of(&r.grid, before, &bar, &beat);
        if (rx_grid_quantize(&r.grid, before) < truth.first_beat - 0.005 || bar || beat)
            FAILF(" a beat before the first (bar %d.%d)", bar, beat);
        RXTrack *tr = rx_track_new("/test/start.flac");
        if (tr) {
            tr->duration = dur;
            rx_analysis_auto_cues(tr, &r);
            if (tr->nb_cues != 1 || fabs(tr->cues[0].time - truth.start[0]) > 0.005)
                FAILF(" auto cue at %.4f (want %.4f)", tr->nb_cues ? tr->cues[0].time : NAN,
                      truth.start[0]);
            rx_track_free(tr);
        }
    }
    const Want *w = &wants[idx];
    if ((r.flags & w->flags_set) != w->flags_set)
        FAILF(" flags 0x%x lack 0x%x", r.flags, w->flags_set);
    if (r.flags & w->flags_clear)
        FAILF(" flags 0x%x include 0x%x", r.flags, r.flags & w->flags_clear);

    printf("%s %s%s%s\n", fails ? "FAIL" : "ok  ", path, sp->name, why);
    if (curve_note[0])
        printf("%s\n", curve_note);
    free(truth.beats);
    if (verbose || fails) {
        printf("      conf %.2f flags 0x%02x octave %.2f phase %.2f 2nd %.2f start %.4f (%d)\n",
               r.confidence, r.flags, r.octave_margin, r.phase_margin, r.bpm_second_best,
               r.music_start, (int)r.start_kind);
        for (int s = 0; s < r.grid.nb_sections; s++)
            printf("      section %d: start %.5f bpm %.4f beat %d\n", s, r.grid.sections[s].start,
                   r.grid.sections[s].bpm, r.grid.sections[s].beat);
    }
    if (keep)
        *keep = r;
    else
        rx_beat_result_free(&r);
    return fails ? 1 : 0;
}

/* The largest differences between the CPU's and the GPU's grids. */
static double max_dstart, max_dbpm, max_dconf;

/* The same grid on both paths: what a player gets from it (the sections,
 * their tempo, beat numbers and starts), the flags and where the music
 * starts. The starts may differ by float rounding carried through the line
 * fits; 0.1 ms is a tenth of what anyone could hear. */
static int compare(int idx, const RXBeatResult *a, const RXBeatResult *b)
{
    const char *name = cases[idx].name;
    int fails = 0;
    if (a->grid.nb_sections != b->grid.nb_sections || a->flags != b->flags ||
        a->start_kind != b->start_kind || fabs(a->music_start - b->music_start) > 1e-9) {
        printf("FAIL cpu/gpu %s: %d vs %d sections, flags 0x%x vs 0x%x, start %.6f (%d) vs "
               "%.6f (%d)\n", name, a->grid.nb_sections, b->grid.nb_sections, a->flags, b->flags,
               a->music_start, (int)a->start_kind, b->music_start, (int)b->start_kind);
        return 1;
    }
    for (int s = 0; s < a->grid.nb_sections; s++) {
        const RXTempoSection *x = &a->grid.sections[s], *y = &b->grid.sections[s];
        double ds = fabs(x->start - y->start), db = fabs(x->bpm - y->bpm);
        max_dstart = fmax(max_dstart, ds);
        max_dbpm = fmax(max_dbpm, db);
        if (ds > 1e-4 || db > 1e-4 || x->beat != y->beat) {
            printf("FAIL cpu/gpu %s: section %d at %.6f, %.5f BPM, beat %d vs %.6f, %.5f, %d\n",
                   name, s, x->start, x->bpm, x->beat, y->start, y->bpm, y->beat);
            fails++;
        }
    }
    max_dconf = fmax(max_dconf, fabs(a->confidence - b->confidence));
    return fails ? 1 : 0;
}

/* The largest difference between two arrays, relative to the first one's
 * largest magnitude. */
static double rel_diff(const float *a, const float *b, size_t n)
{
    double top = 0.0, d = 0.0;
    for (size_t i = 0; i < n; i++) {
        top = fmax(top, fabs(a[i]));
        d = fmax(d, fabs((double)a[i] - b[i]));
    }
    return top > 0.0 ? d / top : d;
}

/* The front end's arrays on both paths, on a few of the tracks: the
 * spectrum's (the profile bands, the kick band, the three fluxes) and the
 * envelopes around every true beat (all three bands).
 *
 * The tolerance is 1e-5 of each array's largest value. An fp32 FFT of 2^n
 * points is accurate to about n float epsilons of the frame's largest bin
 * (11 x 6e-8 for the STFT); the log compression turns a relative error of
 * an amplitude into an absolute error no larger, so the compressed bands
 * differ by about 1e-6 of their largest value, and the flux, a sum of 64
 * differences of them, alike. Both GPUs here (RADV, ANV) stay under 5e-7,
 * on real tracks too. The comparison is not per element: where a value is
 * near zero, its relative error means nothing. */
static int check_frontend(RXCompute *gpu)
{
    static const int which[] = { 0, 3, 6, 9, 16, 17 };
    int fails = 0;
    double worst_spec = 0.0, worst_env = 0.0;
    for (size_t w = 0; w < RX_ARRAY_ELEMS(which); w++) {
        Track t;
        Truth truth;
        if (synth(&cases[which[w]], &t, &truth) < 0)
            return fails + 1;
        free(truth.beats);
        RXSpectrum a, b;
        if (rx_spectrum_compute(t.x, t.n, NULL, &a) < 0) {
            free(t.x);
            return fails + 1;
        }
        if (rx_spectrum_compute(t.x, t.n, gpu, &b) < 0) {
            rx_spectrum_free(&a);
            free(t.x);
            return fails + 1;
        }
        const size_t nf = (size_t)a.nb_frames;
        const double d[] = {
            rel_diff(a.prof, b.prof, nf * RX_BEAT_NPROF),
            rel_diff(a.kick, b.kick, nf),
            rel_diff(a.flux_full, b.flux_full, nf),
            rel_diff(a.flux_mid, b.flux_mid, nf),
            rel_diff(a.flux_high, b.flux_high, nf),
        };
        double dmax = 0.0;
        for (size_t i = 0; i < RX_ARRAY_ELEMS(d); i++)
            dmax = fmax(dmax, d[i]);
        rx_spectrum_free(&a);
        rx_spectrum_free(&b);

        /* Every beat of the first section's tempo, from its first downbeat. */
        const double p = 60.0 / truth.bpm[0];
        const int nb = (int)(((double)t.n / RATE - truth.start[0]) / p);
        const size_t len = (size_t)RX_NB_BANDS * RX_ENV_LEN;
        double *tm = malloc((size_t)nb * sizeof(*tm));
        double *ca = malloc((size_t)nb * sizeof(*ca)), *cb = malloc((size_t)nb * sizeof(*cb));
        float *ea = malloc((size_t)nb * len * sizeof(*ea));
        float *eb = malloc((size_t)nb * len * sizeof(*eb));
        RXEnvCtx *c1 = NULL, *c2 = NULL;
        double de = 1.0;
        if (tm && ca && cb && ea && eb && rx_env_alloc(&c1, t.x, t.n, NULL) >= 0 &&
            rx_env_alloc(&c2, t.x, t.n, gpu) >= 0) {
            for (int i = 0; i < nb; i++)
                tm[i] = truth.start[0] + i * p;
            if (rx_env_windows(c1, tm, nb, ea, ca) >= 0 &&
                rx_env_windows(c2, tm, nb, eb, cb) >= 0 &&
                !memcmp(ca, cb, (size_t)nb * sizeof(*ca)))
                de = rel_diff(ea, eb, (size_t)nb * len);
        }
        rx_env_free(&c1);
        rx_env_free(&c2);
        free(tm);
        free(ca);
        free(cb);
        free(ea);
        free(eb);
        free(t.x);

        worst_spec = fmax(worst_spec, dmax);
        worst_env = fmax(worst_env, de);
        if (dmax > 1e-5 || de > 1e-5) {
            printf("FAIL [gpu] front end on %s: prof %.2g kick %.2g flux %.2g %.2g %.2g, "
                   "envelopes %.2g of their largest values\n", cases[which[w]].name, d[0], d[1],
                   d[2], d[3], d[4], de);
            fails++;
        }
    }
    if (!fails)
        printf("ok   [gpu] front end on %d tracks: spectrum within %.1e, envelopes within %.1e "
               "of the CPU's (of the largest values)\n", (int)RX_ARRAY_ELEMS(which), worst_spec,
               worst_env);
    return fails;
}

static int check_misc(RXCompute *gpu)
{
    const char *path = gpu ? "[gpu] " : "";
    RXAnalysisOptions opt;
    rx_analysis_options_default(&opt);
    opt.compute = gpu;
    int fails = 0;
    /* Flag names round-trip. */
    for (unsigned f = 1; f <= RX_GRID_FLAG_DYNAMIC; f <<= 1) {
        const char *n = rx_grid_flag_name((enum RXGridFlag)f);
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
    RXBeatResult r;
    if (rx_analyse_beats(z, RATE * 20, RATE, &opt, &r) < 0 || r.grid.nb_sections != 0) {
        printf("FAIL %ssilence: %d sections\n", path, r.grid.nb_sections);
        fails++;
    }
    rx_beat_result_free(&r);
    if (rx_analyse_beats(z, RATE * 2, RATE, &opt, &r) < 0 || r.grid.nb_sections != 0) {
        printf("FAIL %sshort input\n", path);
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
    RXBeatResult a, b;
    rx_analyse_beats(t.x, t.n, RATE, &opt, &a);
    rx_analyse_beats(t.x, t.n, RATE, &opt, &b);
    if (a.grid.nb_sections != b.grid.nb_sections || !a.grid.nb_sections ||
        a.grid.sections[0].start != b.grid.sections[0].start ||
        a.grid.sections[0].bpm != b.grid.sections[0].bpm || a.confidence != b.confidence) {
        printf("FAIL %sdeterminism\n", path);
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
    RXBeatResult c;
    rx_analyse_beats(x48, n48, 48000, &opt, &c);
    if (!c.grid.nb_sections || fabs(c.grid.sections[0].start - a.grid.sections[0].start) > 0.0005 ||
        fabs(c.grid.sections[0].bpm - a.grid.sections[0].bpm) > 0.01) {
        printf("FAIL %s48 kHz: start %.5f bpm %.4f vs %.5f %.4f\n", path,
               c.grid.nb_sections ? c.grid.sections[0].start : 0.0,
               c.grid.nb_sections ? c.grid.sections[0].bpm : 0.0,
               a.grid.sections[0].start, a.grid.sections[0].bpm);
        fails++;
    } else {
        printf("ok   %sdeterminism and 48 kHz input\n", path);
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
static int check_analyse(RXCompute *gpu)
{
    const char *path = gpu ? "[gpu] " : "";
    RXAnalysisOptions opt;
    rx_analysis_options_default(&opt);
    opt.compute = gpu;
    int fails = 0;
    Track t;
    Truth truth;
    if (synth(&cases[0], &t, &truth) < 0)
        return 1;
    RXBeatResult mono;
    rx_analyse_beats(t.x, t.n, RATE, &opt, &mono);

    int64_t n48 = t.n * 48000 / RATE;
    RXAudio au = {
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
    RXAnalysis a;
    int ret = rx_analyse(&au, &opt, RX_ANALYSE_ALL, &a);
    if (ret < 0 || a.done != RX_ANALYSE_ALL) {
        printf("FAIL %srx_analyse: ret %d done 0x%x\n", path, ret, a.done);
        fails++;
    } else if (!a.beats.grid.nb_sections || !mono.grid.nb_sections ||
               fabs(a.beats.grid.sections[0].start - mono.grid.sections[0].start) > 0.0005 ||
               fabs(a.beats.grid.sections[0].bpm - mono.grid.sections[0].bpm) > 0.01 ||
               a.waves.nb_detail <= 0) {
        printf("FAIL %srx_analyse: grid %.5f@%.3f vs %.5f@%.3f, %d detail columns\n", path,
               a.beats.grid.nb_sections ? a.beats.grid.sections[0].start : 0.0,
               a.beats.grid.nb_sections ? a.beats.grid.sections[0].bpm : 0.0,
               mono.grid.sections[0].start, mono.grid.sections[0].bpm, a.waves.nb_detail);
        fails++;
    } else {
        printf("ok   %srx_analyse (stereo 48 kHz): grid, key \"%s\", %d waveform columns\n",
               path, a.key.key, a.waves.nb_detail);
    }

    /* The cue: the first downbeat at or after the music starts, musical,
     * once; nothing for a track that already has cues. */
    RXTrack *tr = rx_track_new("/test/house124.flac");
    if (tr && a.beats.grid.nb_sections) {
        tr->duration = a.duration;
        double first = a.beats.music_start;
        rx_analysis_auto_cues(tr, &a.beats);
        rx_analysis_auto_cues(tr, &a.beats);
        double want = truth.start[0];
        double bar = 4.0 * 60.0 / truth.bpm[0];
        while (want < first - 0.05)
            want += bar;
        if (tr->nb_cues != 1 || fabs(tr->cues[0].time - want) > 0.003 ||
            !rx_cue_is_musical(&tr->cues[0]) || rx_cue_is_hot(&tr->cues[0])) {
            printf("FAIL %sauto cue: %d cues, at %.4f (want %.4f)\n", path, tr->nb_cues,
                   tr->nb_cues ? tr->cues[0].time : 0.0, want);
            fails++;
        } else {
            printf("ok   %sauto cue at the first downbeat (%.3f s)\n", path, tr->cues[0].time);
        }
        /* A track with a cue of its own gets none. */
        RXTrack *t2 = rx_track_new("/test/has-cue.flac");
        if (t2) {
            t2->duration = a.duration;
            RXCue own;
            rx_cue_init(&own, 30.0);
            rx_track_add_cue(t2, &own);
            rx_analysis_auto_cues(t2, &a.beats);
            if (t2->nb_cues != 1) {
                printf("FAIL %sauto cue added to a track that has cues\n", path);
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
        for (int k = 0; k < truth.nb_beats; k++)
            printf("beat %d: %.5f\n", k, truth.beats[k]);
        free(truth.beats);
        free(t.x);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "-c")) {
        int i = atoi(argv[2]);
        if (i < 0 || i >= (int)RX_ARRAY_ELEMS(cases))
            return 1;
        verbose = true;
        return check(i, NULL, NULL);
    }
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    /* Which paths: see the top of the file. */
    enum RXComputeMode mode = rx_compute_mode();
    RXCompute *gpu = NULL;
    if (mode != RX_COMPUTE_OFF) {
        int ret = rx_compute_create(&gpu, NULL);
        if (ret < 0 && mode == RX_COMPUTE_REQUIRED) {
            printf("FAIL RXBOX_ANALYSIS_GPU=1, but there is no GPU (%s)\n", strerror(-ret));
            return 1;
        }
        if (ret < 0)
            printf("note: no GPU (%s): the GPU half is skipped\n", strerror(-ret));
        else
            printf("note: the GPU half runs on %s\n", rx_compute_device_name(gpu));
    }
    const bool cpu = mode != RX_COMPUTE_REQUIRED, both = cpu && gpu;

    int fails = 0;
    if (cpu) {
        fails += check_misc(NULL);
        fails += check_analyse(NULL);
    }
    if (gpu) {
        fails += check_frontend(gpu);
        fails += check_misc(gpu);
        fails += check_analyse(gpu);
    }
    int same = 0;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(cases); i++) {
        RXBeatResult a = { 0 }, b = { 0 };
        if (cpu)
            fails += check((int)i, NULL, both ? &a : NULL);
        if (gpu)
            fails += check((int)i, gpu, both ? &b : NULL);
        if (both) {
            int f = compare((int)i, &a, &b);
            fails += f;
            same += !f;
        }
        rx_beat_result_free(&a);
        rx_beat_result_free(&b);
    }
    if (both)
        printf("%s cpu/gpu: %d of %d grids the same; largest differences: start %.2g ms, "
               "%.2g BPM, confidence %.2g\n", same == (int)RX_ARRAY_ELEMS(cases) ? "ok  " : "FAIL",
               same, (int)RX_ARRAY_ELEMS(cases), max_dstart * 1e3, max_dbpm, max_dconf);
    if (gpu) {
        int errors = rx_compute_validation_errors(gpu);
        if (errors) {
            printf("FAIL %d Vulkan validation errors\n", errors);
            fails++;
        }
        rx_compute_destroy(&gpu);
    }
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
