/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rx_analyse_beats: the beat-grid analyser, stage by stage (docs/BEATS.md
 * explains each decision and the evidence for it):
 *
 *   spectrum.c   STFT, mel bands, kick band               (150 frames/s)
 *   onset.c      SuperFlux and band fluxes
 *   tempo.c      whole-track tempo, octave by evidence, tempo regions
 *   grid_fit.c   beat tracking, attacks, robust lines, sections, snapping
 *   downbeat.c   bar position from structure
 *   here         the order, the half-beat and downbeat decisions, the
 *                grid, the confidence and the flags
 *
 * Also the small public helpers of the beats part of rxbox/analysis.h
 * (options, flag names), kept here rather than in analysis.c so that a
 * program using only rx_analyse_beats does not pull in the waveform and
 * key code. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- helpers */

static const char *const flag_names[] = {
    "tempo-change",
    "octave-uncertain",
    "phase-uncertain",
    "downbeat-weak",
    "no-kicks",
    "low-confidence",
    "unsteady",
    "phase-cut",
};

const char *rx_grid_flag_name(enum RXGridFlag f)
{
    for (size_t i = 0; i < RX_ARRAY_ELEMS(flag_names); i++) {
        if ((unsigned)f == 1u << i)
            return flag_names[i];
    }
    return "";
}

unsigned rx_grid_flag_parse(const char *name)
{
    if (!name)
        return 0;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(flag_names); i++) {
        if (!rx_strcasecmp(name, flag_names[i]))
            return 1u << i;
    }
    return 0;
}

void rx_analysis_options_default(RXAnalysisOptions *o)
{
    *o = (RXAnalysisOptions) {
        .bpm_min               = 0.0,
        .bpm_max               = 0.0,
        .prior_bpm             = 125.0,
        .snap_bpm              = true,
        .allow_sections        = true,
        .deterministic_threads = false,
    };
}

void rx_beat_result_free(RXBeatResult *r)
{
    /* The sections array is plain malloc'd memory (rxbox/model.h). */
    free(r->grid.sections);
    memset(r, 0, sizeof(*r));
}

/* -------------------------------------------------------------- tuning */

/* Ellis's tightness: how hard the tracker holds the period. 100 lets it
 * follow a live drummer's few percent and still walk through fills. */
#define TRACK_TIGHTNESS   100.0
/* Attack search around a tracked beat: the tracker is on 6.7 ms frames
 * and may sit a frame or two off the hit. */
#define ATTACK_TOL_TRACK  0.020
/* Drift allowed by BPM snapping, at each end of a section. */
#define SNAP_TOL          0.005
/* A live drummer's beats scatter by 5-15 ms; a sequencer's by under 1. */
#define UNSTEADY_JITTER   0.006
#define UNSTEADY_SCATTER  0.004
/* Two sections whose lines agree this closely are one (grid_fit.c's
 * SEG_MAX_DEV, the segmentation's test 1). */
#define SECTION_MERGE_DEV 0.015
/* The music's start as a witness (docs/BEATS.md, section 10). A hit or a
 * fade's origin within a tenth of a beat (at most 40 ms) of a beat or a
 * half beat of the line is on it. A hit counts when the kick band rises at
 * it at least as much as on the beats that carry the kicks, and only while
 * the kicks themselves do not mark one half of the beat by a factor e.
 * Each has a log evidence for its half of the beat and a vote for beat 1
 * (in structure scales, as downbeat.c's votes). */
#define START_ON_BEAT     0.10
#define START_ON_BEAT_MAX 0.040
#define HIT_KICK          1.0
#define HIT_KICKS_CLEAR   1.0
#define HIT_PHASE         2.5
#define HIT_DOWNBEAT      2.0
#define FADE_PHASE        1.0
#define FADE_DOWNBEAT     0.25
/* The grid's first beat may lie this much before the music's start (a
 * line a few ms early on the hit; a fade's start is a frame's estimate). */
#define START_SLACK       0.050

typedef struct Ctx {
    const RXAnalysisOptions *opt;
    const float  *x;
    int64_t       n;
    double        duration;
    RXSpectrum    spec;
    RXOnsets      ons;
    RXTempoEstimate tempo;
    float        *period;           /* per onset frame, seconds */
    RXBeatObs    *beats;
    int           nb_beats;
    RXEnvCtx     *envctx;
    RXBeatEnvs    envs;
    RXFitSection *secs;
    int           nb_secs;
    int          *bands;            /* envelope band of each section */
    double        kick_share;
    const float  *arbiter;          /* the kick band, or the full band without kicks */
} Ctx;

static void ctx_free(Ctx *c)
{
    rx_spectrum_free(&c->spec);
    rx_onsets_free(&c->ons);
    free(c->period);
    free(c->beats);
    rx_env_free(&c->envctx);
    rx_beat_envs_free(&c->envs);
    free(c->secs);
    free(c->bands);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Robust spread (MAD) of the attacks around the section's line. */
static double section_jitter(const RXBeatObs *beats, const RXFitSection *s)
{
    int n = 0;
    double *r = malloc((size_t)(s->last - s->first + 1) * sizeof(*r));
    if (!r)
        return 0.0;
    for (int i = s->first; i <= s->last; i++) {
        if (beats[i].weight > 0.0 && !isnan(beats[i].attack))
            r[n++] = fabs(beats[i].attack - rx_beat_predict(beats, i, s));
    }
    double j = 0.0;
    if (n >= 8) {
        qsort(r, n, sizeof(*r), cmp_dbl);
        j = 1.4826 * r[n / 2];
    }
    free(r);
    return j;
}

/* How far line d strays from section x's beats: the distance of x's
 * first and last beats, on x's line, from d's nearest beats. The lines
 * are straight, so the ends are the worst. */
static double line_distance(const RXBeatObs *beats, const RXFitSection *x, const RXFitSection *d)
{
    double dev = 0.0;
    const int ends[] = { x->first, x->last };
    for (size_t e = 0; e < RX_ARRAY_ELEMS(ends); e++) {
        double k = (rx_beat_predict(beats, ends[e], x) - d->t0) / d->period;
        dev = fmax(dev, fabs(k - round(k)) * d->period);
    }
    return dev;
}

/* Beat-to-beat scatter of the attacks: the robust spread of the second
 * differences of consecutive attacks, divided by sqrt(6) (white jitter of
 * sigma gives sigma). Unlike the spread around the line, a step or a slow
 * drift hardly moves it: it is what a live drummer shows. -1 when there
 * are too few runs of three attacks to tell. */
static double section_scatter(const RXBeatObs *beats, const RXFitSection *s)
{
    int n = 0;
    double *r = malloc((size_t)(s->last - s->first + 1) * sizeof(*r));
    if (!r)
        return 0.0;
    for (int i = s->first + 1; i < s->last; i++) {
        const RXBeatObs *a = &beats[i - 1], *b = &beats[i], *c = &beats[i + 1];
        if (a->weight <= 0.0 || b->weight <= 0.0 || c->weight <= 0.0 || isnan(a->attack) ||
            isnan(b->attack) || isnan(c->attack))
            continue;
        if (b->index - a->index != 1 || c->index - b->index != 1)
            continue;
        r[n++] = fabs(c->attack - 2.0 * b->attack + a->attack);
    }
    double sc = -1.0;
    if (n >= 8) {
        qsort(r, n, sizeof(*r), cmp_dbl);
        sc = 1.4826 * r[n / 2] / sqrt(6.0);
    }
    free(r);
    return sc;
}

/* Puts a section on its attacks. The fold of the whole section chooses
 * the band and moves the line onto the start of the average beat's hit;
 * then the attacks are measured close to the line and the line refitted
 * through them; then the fold again. The search shrinks at every step
 * (30, then 10 to 30 by the scatter, 5, 3 ms) so that the line cannot walk
 * from the kick onto a hat or a clap a little later, which a re-centred
 * search of constant width would let it do one step at a time. Returns
 * the band used. */
static int refine_section(Ctx *c, RXFitSection *s, double *contrast)
{
    double off = 0.0;
    int band = rx_fold(&c->envs, c->beats, s->first, s->last, s, -1, 0.030, contrast, &off);
    s->t0 += off;
    /* The attack search scales with the section's scatter: a tight window
     * around a line that a drummer's spread has pulled early would cut the
     * late hits off and keep it early. */
    double tol = RX_CLAMP(3.0 * s->jitter, 0.010, 0.030);
    for (int it = 0; it < 2; it++) {
        rx_measure_attacks(&c->envs, c->beats, s->first, s->last, band, s, tol);
        RXFitSection t, init = *s;
        if (rx_fit_line(c->beats, s->first, s->last, &init, &t) == 0 && t.supported >= 8)
            *s = t;
    }
    /* The fold measures a steady section best (it averages before it
     * detects, so a quiet kick still counts), but it smears a drummer's
     * scattered hits and its rise then comes early; there the line through
     * the attacks, their mean, is the phase. */
    bool steady = s->jitter < 0.003;
    if (steady) {
        rx_fold(&c->envs, c->beats, s->first, s->last, s, band, 0.005, contrast, &off);
        s->t0 += off;
    }
    if (c->opt->snap_bpm && rx_snap_section(c->beats, s, SNAP_TOL) && steady) {
        rx_fold(&c->envs, c->beats, s->first, s->last, s, band, 0.003, contrast, &off);
        s->t0 += off;
    }
    /* The jitter: the spread of the attacks around the final line, looked
     * for within 35 ms (a narrower search would cut a live drummer's
     * scatter short and call it steady). */
    rx_measure_attacks(&c->envs, c->beats, s->first, s->last, band, s, 0.035);
    s->jitter = section_jitter(c->beats, s);
    s->scatter = section_scatter(c->beats, s);
    return band;
}

/* The confidence, as dj's: the share of 16-beat windows (every 8 beats)
 * whose average beat starts its rise within 15 ms of the grid, weighted
 * by how clearly it rises. */
static double grid_confidence(const Ctx *c, const int *bands)
{
    double num = 0.0, den = 0.0;
    double *contrast = NULL, *offs = NULL;
    int nw = 0, cap = 0;
    for (int si = 0; si < c->nb_secs; si++) {
        const RXFitSection *s = &c->secs[si];
        double search = fmin(0.125 * s->period, 0.050);
        for (int a = s->first; a <= s->last; a += 8) {
            int b = RX_MIN(a + 15, s->last);
            if (b - a < 7)
                break;
            if (nw == cap) {
                cap = cap ? 2 * cap : 64;
                double *nc = realloc(contrast, cap * sizeof(*nc));
                double *no = realloc(offs, cap * sizeof(*no));
                if (!nc || !no) {
                    free(nc ? nc : contrast);
                    free(no ? no : offs);
                    return 0.0;
                }
                contrast = nc;
                offs = no;
            }
            rx_fold(&c->envs, c->beats, a, b, s, bands[si], search, &contrast[nw], &offs[nw]);
            nw++;
        }
    }
    if (!nw) {
        free(contrast);
        free(offs);
        return 0.0;
    }
    double *tmp = malloc(nw * sizeof(*tmp));
    if (!tmp) {
        free(contrast);
        free(offs);
        return 0.0;
    }
    /* The median over the windows that have an average beat at all. */
    int nt = 0;
    for (int i = 0; i < nw; i++) {
        if (contrast[i] > 0.0)
            tmp[nt++] = contrast[i];
    }
    if (!nt) {
        free(tmp);
        free(contrast);
        free(offs);
        return 0.0;
    }
    for (int i = 1; i < nt; i++) {
        double v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    double med = tmp[nt / 2];
    free(tmp);
    /* dj weighted a window by its contrast less half the median. Capped
     * here at twice the median: in a breakdown the average beat's level is
     * tiny, its contrast explodes, and a few such windows (whose loudest
     * rise is a riser or a crash) outweighed thirty bars of kicks. */
    for (int i = 0; i < nw; i++) {
        double w = contrast[i] < 0.5 * med ? 0.0 : fmin(contrast[i], 2.0 * med) - 0.5 * med;
        num += w * (fabs(offs[i]) <= 0.015);
        den += w;
    }
    free(contrast);
    free(offs);
    return den > 0.0 ? num / den : 0.0;
}

/* --------------------------------------------------- phase and downbeat */

typedef struct SecInfo {
    double     start, end;          /* the section's span of the timeline */
    double     kick_ratio;          /* arbiter band on the beats / on the half beats */
    double     phase_evidence;      /* log-evidence for the chosen half of the beat */
    bool       has_vote;            /* long enough for a structure vote */
    RXDownbeat db;
    bool       shifted;             /* moved by half a beat */
    bool       has_down;            /* beat 1 decided (a structure vote, or the start's) */
    int        pos;                 /* even: the downbeat's half-beat position from t0 */
    double     down_margin;
} SecInfo;

/* Where t falls on line s: the nearest half beat from t0 (even: a beat)
 * and how far t is from it. */
static long half_beat(const RXFitSection *s, double t, double *off)
{
    double hp = s->period / 2;
    long j = lrint((t - s->t0) / hp);
    *off = t - (s->t0 + j * hp);
    return j;
}

static int position(long j)
{
    return (int)((j % 8 + 8) % 8);
}

/* Mean of the arbiter onset over [t - 30 ms, t + 10 ms] at the line's beats
 * in [from, to) moved by `shift`: a kick's low band rises up to 20 ms
 * before its click, so the window reaches back further than forward. */
static double arbiter_support(const Ctx *c, const float *v, const RXFitSection *s,
                              double from, double to, double shift)
{
    double acc = 0.0;
    int n = 0;
    long j0 = (long)ceil((from - s->t0 - shift) / s->period);
    long j1 = (long)floor((to - s->t0 - shift) / s->period);
    for (long j = j0; j <= j1; j++) {
        double t = s->t0 + shift + j * s->period;
        int a = (int)lrint((t - 0.030) * c->ons.fps), b = (int)lrint((t + 0.010) * c->ons.fps);
        double m = 0.0;
        for (int i = RX_MAX(a, 0); i <= RX_MIN(b, c->ons.nb - 1); i++)
            m = fmax(m, v[i]);
        acc += m;
        n++;
    }
    return n ? acc / n : 0.0;
}

/* What the music's start says about section s (docs/BEATS.md). It counts
 * only where it is placed: a hit carrying a kick or a fade's origin, in
 * this section, on a beat or a half beat of the line. Otherwise (the music
 * starts at once, a swell, a hit without a kick, a start between the
 * line's half beats) the structure takes dj's first loud onset as its start
 * vote, as before. */
typedef struct StartWitness {
    bool   counts;
    bool   onset_too;               /* dj's first loud onset still votes in the structure */
    double at;                      /* the hit, or the fade's origin */
    double phase;                   /* log evidence for the half of the beat it is on */
    double down;                    /* its vote for beat 1 when on a beat */
    double kick;                    /* a hit's kick band against the kick beats' (debug) */
} StartWitness;

static StartWitness start_witness(const Ctx *c, const RXFitSection *s, const SecInfo *si,
                                  const RXMusicStart *ms)
{
    StartWitness w = { .at = ms->start };
    if ((ms->kind != RX_START_HIT && ms->kind != RX_START_FADE) || w.at < si->start ||
        w.at >= si->end)
        return w;
    const double P = s->period;
    double off;
    half_beat(s, w.at, &off);
    if (fabs(off) > fmin(START_ON_BEAT * P, START_ON_BEAT_MAX))
        return w;
    /* A fade begins on a beat, not between two; whether on beat 1 the first
     * loud onset knows better (where the drums come in after the fade),
     * so it keeps its vote and the origin only breaks a tie. */
    if (ms->kind == RX_START_FADE) {
        w.counts = true;
        w.onset_too = true;
        w.phase = FADE_PHASE;
        w.down = FADE_DOWNBEAT;
        return w;
    }
    /* The kick band from 15 ms before the hit to 60 ms after it (a kick's
     * body builds after its click), against its level on the beats or the
     * half beats, whichever carry the kicks. A stab or a vocal on the "and"
     * before bar 1 has no kick; the hit that opens a track on beat 1
     * usually has one. */
    int a = (int)lrint((w.at - 0.015) * c->ons.fps), b = (int)lrint((w.at + 0.060) * c->ons.fps);
    double m = 0.0;
    for (int f = RX_MAX(a, 0); f <= RX_MIN(b, c->ons.nb - 1); f++)
        m = fmax(m, c->ons.low[f]);
    double beats = fmax(arbiter_support(c, c->ons.low, s, si->start, si->end, 0.0),
                        arbiter_support(c, c->ons.low, s, si->start, si->end, P / 2));
    w.kick = beats > 0.0 ? m / beats : 0.0;
    /* Where the kick band clearly marks one half of the beat, a hit with
     * a kick half a beat from it is a pickup (a kick and a crash on the
     * "and" before bar 1); where it does not (a two-step kick on the "and"s
     * as often as on the beats), the opening hit is the better witness. */
    if (w.kick >= HIT_KICK && fabs(log(si->kick_ratio)) < HIT_KICKS_CLEAR) {
        w.counts = true;
        w.phase = HIT_PHASE;
        w.down = HIT_DOWNBEAT;
    }
    return w;
}

/* Is the section on the beat or on the off-beat, and where is beat 1?
 *
 * Two witnesses. The arbiter band (the kicks, when the track has them): a
 * grid on the beat has the kicks on its beats, not between them. And the
 * structure (downbeat.c): the music changes on beat 1, which is on the
 * beat. Neither is right on every track: an off-beat bass fills the kick
 * band between the kicks (hardstyle), and a pickup or a riser can put
 * structural change half a beat early. So both are weighed as log
 * evidence, structure at 1.5 times the kicks (rbxport measured structure
 * right on 153 of 155 reference grids, the kick alone on 150), and the
 * grid moves by half a beat only when the sum says so by a margin.
 *
 * The music's start is a third (start.c, start_witness above). A hit
 * after silence that carries a kick, where the kicks leave the half of the
 * beat open, is on the beat (tracks made for DJs start on a bar line) and
 * is beat 1 unless the structure clearly says otherwise. A fade's origin
 * is on the beat too, and breaks a tie for beat 1. Everywhere else (the
 * music starts at once, a swell, any other hit, a start between the
 * line's half beats), and for beat 1 after a fade, dj's first loud onset
 * votes in the structure as one scale, as it always did. */
static int decide_phase(Ctx *c, int i, SecInfo *si, const float *arbiter, const RXMusicStart *ms,
                        double first_onset, int *band, RXBeatDebug *dbg)
{
    RXFitSection *s = &c->secs[i];
    const double P = s->period;
    const double eps = 0.1;
    double on = arbiter_support(c, arbiter, s, si->start, si->end, 0.0);
    double off = arbiter_support(c, arbiter, s, si->start, si->end, P / 2);
    si->kick_ratio = (on + eps) / (off + eps);

    StartWitness w = start_witness(c, s, si, ms);
    si->has_vote = (si->end - si->start) / P >= 32.0;
    double sv = 0.0;
    if (si->has_vote) {
        int ret = rx_downbeat_vote(&c->spec, c->x, c->n, s->t0, P, si->start, si->end,
                                   w.counts && !w.onset_too ? NAN : first_onset, &si->db);
        if (ret < 0)
            return ret;
        double be = 0.0, bo = 0.0;
        for (int p = 0; p < 8; p++) {
            if (p & 1)
                bo = fmax(bo, si->db.scores[p]);
            else
                be = fmax(be, si->db.scores[p]);
        }
        sv = log((be + 0.05) / (bo + 0.05));
    }

    /* The start's half of the beat. */
    double hoff = 0.0;
    long hj = half_beat(s, w.at, &hoff);
    double hv = w.counts ? (hj & 1 ? -w.phase : w.phase) : 0.0;
    bool here = (ms->kind == RX_START_HIT || ms->kind == RX_START_FADE) && w.at >= si->start &&
                w.at < si->end;

    double ev = log(si->kick_ratio) + 1.5 * sv + hv;
    si->phase_evidence = ev;
    rx_debug("beats: section %d phase: arbiter %.3f on, %.3f off (log %.3f); structure %.3f "
             "(scores %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f); start %+.2f (half beat %ld "
             "%+.1f ms); evidence %.3f\n", i, on, off, log(si->kick_ratio), sv,
             si->db.scores[0], si->db.scores[1], si->db.scores[2], si->db.scores[3],
             si->db.scores[4], si->db.scores[5], si->db.scores[6], si->db.scores[7], hv, hj,
             hoff * 1e3, ev);
    if (dbg && here) {
        dbg->start_section = i;
        dbg->start_kick = log(si->kick_ratio);
        dbg->start_struct = sv;
        dbg->start_hit = hv;
        dbg->start_half_beat = hj;
        dbg->start_off = hoff;
        dbg->start_t0 = s->t0;
        dbg->start_period = P;
        memcpy(dbg->start_scores, si->db.scores, sizeof(dbg->start_scores));
        dbg->start_kick_ratio = w.kick;
    }
    if (ev < -log(1.25)) {
        /* Move the line half a beat, re-cut the envelopes where its beats
         * now are, and put it on its attacks again. */
        s->t0 += P / 2;
        for (int k = s->first; k <= s->last; k++) {
            c->beats[k].time = rx_beat_predict(c->beats, k, s);
            int ret = rx_env_window(c->envctx, c->beats[k].time,
                                    c->envs.env + (size_t)k * RX_NB_BANDS * RX_ENV_LEN,
                                    &c->envs.centre[k]);
            if (ret < 0)
                return ret;
        }
        double contrast;
        *band = refine_section(c, s, &contrast);
        si->shifted = true;
        si->phase_evidence = -ev;
        si->kick_ratio = 1.0 / si->kick_ratio;
        if (si->has_vote) {
            double rot[8];
            for (int p = 0; p < 8; p++)
                rot[p] = si->db.scores[(p + 1) % 8];
            memcpy(si->db.scores, rot, sizeof(rot));
        }
    }

    /* The start's vote for beat 1, when it is on a beat of the line as it
     * now is. */
    bool voted = false;
    if (w.counts) {
        long j = half_beat(s, w.at, &hoff);
        if (!(j & 1)) {
            si->db.scores[position(j)] += w.down;
            voted = true;
        }
    }

    /* Beat 1: the best of the four beats (never a half beat). */
    si->has_down = si->has_vote || voted;
    if (si->has_down) {
        int best = 0;
        for (int p = 2; p < 8; p += 2) {
            if (si->db.scores[p] > si->db.scores[best])
                best = p;
        }
        double second = 0.0;
        for (int p = 0; p < 8; p += 2) {
            if (p != best)
                second = fmax(second, si->db.scores[p]);
        }
        si->pos = best;
        si->down_margin = second > 0.0 ? si->db.scores[best] / second : 10.0;
    }
    return 0;
}

/* ------------------------------------------------------------ the grid */

static int longest_section(const Ctx *c)
{
    int m = 0;
    for (int i = 1; i < c->nb_secs; i++) {
        if (c->secs[i].last - c->secs[i].first > c->secs[m].last - c->secs[m].first)
            m = i;
    }
    return m;
}

/* How well a grid's beats land on hits: the mean of the arbiter onset
 * around every beat of every section. */
static double grid_support(const Ctx *c)
{
    double acc = 0.0;
    int n = 0;
    for (int i = 0; i < c->nb_secs; i++) {
        const RXFitSection *s = &c->secs[i];
        double a = rx_beat_predict(c->beats, s->first, s);
        double b = rx_beat_predict(c->beats, s->last, s);
        for (double t = a; t <= b + 1e-6; t += s->period) {
            int f0 = (int)lrint((t - 0.030) * c->ons.fps);
            int f1 = (int)lrint((t + 0.010) * c->ons.fps);
            double m = 0.0;
            for (int f = RX_MAX(f0, 0); f <= RX_MIN(f1, c->ons.nb - 1); f++)
                m = fmax(m, c->arbiter[f]);
            acc += m;
            n++;
        }
    }
    return n ? acc / n : 0.0;
}

/* A fitted grid, moved out of the context while another is tried. */
typedef struct Fit {
    RXBeatObs    *beats;
    int           nb_beats;
    RXFitSection *secs;
    int           nb_secs;
    int          *bands;
    RXBeatEnvs    envs;
    float        *period;
    double        kick_share;
    const float  *arbiter;
} Fit;

static Fit fit_take(Ctx *c)
{
    Fit f = {
        .beats      = c->beats,
        .nb_beats   = c->nb_beats,
        .secs       = c->secs,
        .nb_secs    = c->nb_secs,
        .bands      = c->bands,
        .envs       = c->envs,
        .period     = c->period,
        .kick_share = c->kick_share,
        .arbiter    = c->arbiter,
    };
    c->beats = NULL;
    c->secs = NULL;
    c->bands = NULL;
    c->envs = (RXBeatEnvs) { 0 };
    c->period = NULL;
    c->nb_beats = c->nb_secs = 0;
    return f;
}

static void fit_free(Fit *f)
{
    free(f->beats);
    free(f->secs);
    free(f->bands);
    rx_beat_envs_free(&f->envs);
    free(f->period);
    memset(f, 0, sizeof(*f));
}

/* Puts a fit back, dropping the one in the context. */
static void fit_put(Ctx *c, Fit *f)
{
    Fit old = fit_take(c);
    fit_free(&old);
    c->beats = f->beats;
    c->nb_beats = f->nb_beats;
    c->secs = f->secs;
    c->nb_secs = f->nb_secs;
    c->bands = f->bands;
    c->envs = f->envs;
    c->period = f->period;
    c->kick_share = f->kick_share;
    c->ons.kick_share = f->kick_share;
    c->arbiter = f->arbiter;
    memset(f, 0, sizeof(*f));
}

/* Whether the kick band carries the beat, for the flag: the strength of
 * its periodicity against the full band's at the beat rate or twice it,
 * whichever shows the kick best; or at half the beat rate, counted half.
 * Half: a two-step kick (on 1 and on the "and" of 3, drum & bass) lands
 * half a period apart at the beat rate and cancels there, but not at half
 * of it (the drum & bass tracks of the sample read 1.7-4.9 there, against
 * 0.1-0.5 at the beat rate). Counted half, because a bass or a pianist's
 * left hand moving twice a bar also reads there (0.8 on a piano track). */
static double kick_presence(const Ctx *c, double bpm)
{
    static const struct { double mult, weight; } rates[] = {
        { 0.5, 0.5 }, { 1.0, 1.0 }, { 2.0, 1.0 },
    };
    double best = 0.0;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(rates); i++) {
        double s_low, s_full;
        rx_tempo_phase(c->ons.low, c->ons.nb, c->ons.fps, bpm * rates[i].mult, &s_low);
        rx_tempo_phase(c->ons.full, c->ons.nb, c->ons.fps, bpm * rates[i].mult, &s_full);
        if (s_full > 0.0)
            best = fmax(best, rates[i].weight * s_low / s_full);
    }
    return best;
}

/* Cuts every beat's envelopes again at the centres they were cut at
 * (rx_env_window rounds the same time to the same sample). */
static int recut_envs(Ctx *c)
{
    RXBeatEnvs *e = &c->envs;
    e->env = malloc((size_t)RX_MAX(e->nb, 1) * RX_NB_BANDS * RX_ENV_LEN * sizeof(*e->env));
    if (!e->env)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < e->nb; i++) {
        double centre;
        int ret = rx_env_window(c->envctx, e->centre[i],
                                e->env + (size_t)i * RX_NB_BANDS * RX_ENV_LEN, &centre);
        if (ret < 0)
            return ret;
        if (centre != e->centre[i])
            return RX_ERROR(EINVAL);
    }
    return 0;
}

/* Stage timings for the debug log. */
static void stage(int64_t *tm, const char *name)
{
    int64_t now = rx_time_us();
    rx_debug("beats: %-10s %6.1f ms\n", name, (now - *tm) / 1000.0);
    *tm = now;
}

/* Tracks, measures and fits the grid at `bpm`: beats, their envelopes and
 * attacks, the sections, each put on its attacks. Returns 0, 1 when there
 * is no grid to be had (too few beats or attacks), or a negative error. */
static int fit_grid(Ctx *c, double bpm, double lo, double hi)
{
    free(c->beats);
    free(c->secs);
    free(c->bands);
    rx_beat_envs_free(&c->envs);
    c->beats = NULL;
    c->secs = NULL;
    c->bands = NULL;
    c->nb_beats = c->nb_secs = 0;

    /* How much the kick band carries the beat: the strength of its
     * fundamental at this tempo against the full band's (both onset
     * signals have unit variance). The tracker follows the full band plus
     * the kick band in that proportion, and the kick band, when it carries
     * the beat, is the arbiter of the section tests and of the phase. */
    c->kick_share = 0.0;
    for (int m = 1; m <= 2; m++) {
        /* At the beat rate or twice it: a grid held at half the tempo
         * (a range asked for it) has kicks on every half beat. */
        double s_low, s_full;
        rx_tempo_phase(c->ons.low, c->ons.nb, c->ons.fps, bpm * m, &s_low);
        rx_tempo_phase(c->ons.full, c->ons.nb, c->ons.fps, bpm * m, &s_full);
        if (s_full > 0.0)
            c->kick_share = fmax(c->kick_share, s_low / s_full);
    }
    c->ons.kick_share = c->kick_share;
    c->arbiter = c->kick_share >= 0.5 ? c->ons.low : c->ons.full;
    float wl = (float)RX_CLAMP(c->kick_share, 0.0, 1.0);
    for (int i = 0; i < c->ons.nb; i++)
        c->ons.accent[i] = c->ons.full[i] + wl * c->ons.low[i];

    /* The local tempo the tracker follows. */
    RXTempoRegion *regions = NULL;
    int nb_regions = 0;
    int ret = rx_tempo_regions(&c->ons, bpm, lo, hi, &regions, &nb_regions);
    if (ret < 0)
        return ret;
    if (!c->period)
        c->period = malloc((size_t)c->ons.nb * sizeof(*c->period));
    if (!c->period) {
        free(regions);
        return RX_ERROR(ENOMEM);
    }
    for (int i = 0, r = 0; i < c->ons.nb; i++) {
        double t = i / c->ons.fps;
        while (r + 1 < nb_regions && t >= regions[r + 1].start)
            r++;
        c->period[i] = (float)(60.0 / regions[r].bpm);
    }

    /* Beats, their envelopes, and their attacks. */
    if ((ret = rx_track_beats(&c->ons, c->period, TRACK_TIGHTNESS, &c->beats, &c->nb_beats)) < 0)
        goto end;
    if (c->nb_beats < 16) {
        ret = 1;
        goto end;
    }
    if ((ret = rx_beat_envs(c->envctx, c->beats, c->nb_beats, &c->envs)) < 0)
        goto end;
    /* The average beat around the tracked beats says which band shows the
     * hits best and how far the onset frames sit from their attacks; the
     * tracked times move by that much before each beat's attack is
     * measured. */
    double contrast, shift;
    int band = rx_fold(&c->envs, c->beats, 0, c->nb_beats - 1, NULL, -1, 0.025, &contrast, &shift);
    for (int i = 0; i < c->nb_beats; i++)
        c->beats[i].time += shift;
    rx_measure_attacks(&c->envs, c->beats, 0, c->nb_beats - 1, band, NULL, ATTACK_TOL_TRACK);
    rx_debug("beats: %.3f BPM: %d tracked, band %d, onset delay %.1f ms\n", bpm, c->nb_beats,
             band, shift * 1e3);

    /* Sections, each on its own attacks. */
    ret = rx_segment(c->beats, c->nb_beats, &c->ons, c->arbiter, regions, nb_regions,
                     c->opt->allow_sections, &c->secs, &c->nb_secs);
    if (ret < 0) {
        ret = ret == RX_ERROR(ENOMEM) ? ret : 1;
        goto end;
    }
    c->bands = calloc(c->nb_secs, sizeof(*c->bands));
    if (!c->bands) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < c->nb_secs; i++) {
        c->bands[i] = refine_section(c, &c->secs[i], &contrast);
        rx_debug("beats: section %d: beats %d-%d, %.4f BPM, t0 %.4f, band %d, contrast %.2f, "
                 "jitter %.1f ms\n", i, c->secs[i].first, c->secs[i].last,
                 60.0 / c->secs[i].period, c->secs[i].t0, c->bands[i], contrast,
                 c->secs[i].jitter * 1e3);
    }

end:
    free(regions);
    return ret;
}

/* ---------------------------------------------------------------- main */

void rx_beat_debug_free(RXBeatDebug *dbg)
{
    rx_onsets_free(&dbg->ons);
    free(dbg->beats);
    free(dbg->secs);
    memset(dbg, 0, sizeof(*dbg));
}

int rx_analyse_beats(const float *mono, int64_t nb_samples, int rate,
                     const RXAnalysisOptions *opt, RXBeatResult *out)
{
    return rx_analyse_beats_dbg(mono, nb_samples, rate, opt, out, NULL);
}

int rx_analyse_beats_dbg(const float *mono, int64_t nb_samples, int rate,
                         const RXAnalysisOptions *opt_in, RXBeatResult *out, RXBeatDebug *dbg)
{
    memset(out, 0, sizeof(*out));
    if (dbg) {
        memset(dbg, 0, sizeof(*dbg));
        dbg->start_section = -1;
    }
    RXAnalysisOptions defopt;
    if (!opt_in) {
        rx_analysis_options_default(&defopt);
        opt_in = &defopt;
    }
    if (!mono || nb_samples < 0 || rate <= 0)
        return RX_ERROR(EINVAL);

    Ctx c = { .opt = opt_in };
    float *resampled = NULL;
    SecInfo *sinfo = NULL;
    int ret = 0;
    if (rate != RX_BEAT_RATE) {
        int64_t rn;
        ret = rx_resample_mono(mono, nb_samples, rate, RX_BEAT_RATE, &resampled, &rn);
        if (ret < 0)
            return ret;
        c.x = resampled;
        c.n = rn;
    } else {
        c.x = mono;
        c.n = nb_samples;
    }
    c.duration = (double)c.n / RX_BEAT_RATE;

    /* Too short for a grid: an empty grid, honestly flagged. */
    if (c.duration < 8.0) {
        out->flags = RX_GRID_FLAG_LOW_CONFIDENCE;
        free(resampled);
        return 0;
    }

    int64_t tm = rx_time_us(), tm0 = tm;
    if ((ret = rx_spectrum_compute(c.x, c.n, &c.spec)) < 0)
        goto fail;
    stage(&tm, "spectrum");
    if ((ret = rx_onsets_compute(&c.spec, &c.ons)) < 0)
        goto fail;
    stage(&tm, "onsets");
    /* Where the music starts, from the level; and dj's first loud onset,
     * which votes for beat 1 when the music starts at once. */
    RXMusicStart ms;
    if ((ret = rx_music_start(c.x, c.n, &ms)) < 0)
        goto fail;
    out->music_start = ms.start;
    out->start_kind = ms.kind;
    const double first_onset = rx_onsets_first(&c.ons);
    rx_debug("beats: music starts at %.4f s (%s; over the threshold at %.4f, floor %.1f dB, "
             "loud %.1f dB, jump %.1f dB, rise %.3f s); first loud onset %.3f s\n", ms.start,
             ms.kind == RX_START_HIT ? "hit" : ms.kind == RX_START_FADE ? "fade" :
             ms.kind == RX_START_SWELL ? "swell" : "at once",
             ms.audible, ms.floor_db, ms.loud_db, ms.jump_db, ms.rise, first_onset);

    /* Tempo. */
    double lo = c.opt->bpm_min, hi = c.opt->bpm_max;
    if (!(lo > 0.0 && hi > lo))
        lo = hi = 0.0;
    ret = rx_tempo_estimate(&c.ons, lo, hi, c.opt->prior_bpm, &c.tempo);
    stage(&tm, "tempo");
    if (ret < 0) {
        /* No periodicity at all (silence, a spoken word track). */
        out->flags = RX_GRID_FLAG_LOW_CONFIDENCE;
        ret = 0;
        goto fail;
    }
    double bpm0 = c.tempo.bpm;
    RXOctaveDecision od = { .factor = 1.0, .margin = 10.0 };
    rx_debug("beats: tempo %.3f (octave %.3f margin %.2f, other family %.3f at %.2f)\n",
             bpm0, c.tempo.octave_bpm, c.tempo.octave_margin, c.tempo.second_bpm,
             c.tempo.second_ratio);

    if ((ret = rx_env_alloc(&c.envctx, c.x, c.n)) < 0)
        goto fail;
    ret = fit_grid(&c, bpm0, lo, hi);
    stage(&tm, "grid");
    if (ret > 0) {
        out->flags = RX_GRID_FLAG_LOW_CONFIDENCE;
        ret = 0;
        goto fail;
    }
    if (ret < 0)
        goto fail;

    /* A close runner-up family (3:2, 4:3: harmonic strength within 25 %)
     * is fitted too, and the grid whose beats land on hits wins: an
     * impostor at three halves of the beat puts every third beat where
     * nothing happens. It must beat the first by 10 %. */
    double alt = c.tempo.second_bpm;
    const double rlo = lo > 0.0 ? lo : RX_TEMPO_MIN, rhi = hi > 0.0 ? hi : RX_TEMPO_MAX;
    if (alt > 0.0) {
        /* Its octave nearest the prior, inside the range. */
        double prior = c.opt->prior_bpm > 0.0 ? c.opt->prior_bpm : 125.0;
        while (alt > rhi)
            alt /= 2.0;
        while (alt < rlo)
            alt *= 2.0;
        while (alt / 2.0 >= rlo && fabs(log2(alt / 2.0 / prior)) < fabs(log2(alt / prior)))
            alt /= 2.0;
        while (alt * 2.0 <= rhi && fabs(log2(alt * 2.0 / prior)) < fabs(log2(alt / prior)))
            alt *= 2.0;
    }
    if (alt > 0.0 && alt <= rhi && c.tempo.second_ratio >= 0.75) {
        double s1 = grid_support(&c);
        Fit first = fit_take(&c);
        /* Its envelopes go while the other is tried (an hour of them is
         * 30 MB); their centres are enough to cut them again. */
        free(first.envs.env);
        first.envs.env = NULL;
        int r2 = fit_grid(&c, alt, lo, hi);
        if (r2 < 0) {
            fit_free(&first);
            ret = r2;
            goto fail;
        }
        double s2 = r2 == 0 ? grid_support(&c) : 0.0;
        rx_debug("beats: family %.3f support %.3f vs %.3f support %.3f\n", bpm0, s1, alt, s2);
        /* And it must be as clean a grid: no more sections, no more
         * scatter (a syncopated two-step at 3/4 of its tempo can collect
         * hits, but only with a ragged grid). */
        bool clean = false;
        if (r2 == 0 && c.nb_secs <= first.nb_secs) {
            double j1 = first.secs[0].jitter, j2 = c.secs[0].jitter;
            for (int i = 1; i < first.nb_secs; i++)
                j1 = fmax(j1, first.secs[i].jitter);
            for (int i = 1; i < c.nb_secs; i++)
                j2 = fmax(j2, c.secs[i].jitter);
            clean = j2 <= 1.2 * j1 + 0.0005;
        }
        if (clean && s2 > 1.1 * s1) {
            fit_free(&first);
            bpm0 = alt;
        } else {
            fit_put(&c, &first);
            if ((ret = recut_envs(&c)) < 0)
                goto fail;
        }
    }

    /* The octave, judged on the fitted grid, whose beats are on the hits:
     * if another octave is the beat, the grid is fitted again there (the
     * tracker then follows the right beats). Within the requested range. */
    {
        int m = longest_section(&c);
        const RXFitSection *s = &c.secs[m];
        double from = m ? rx_beat_predict(c.beats, s->first, s) : 0.0;
        double to = c.duration;
        if (m + 1 < c.nb_secs)
            to = rx_beat_predict(c.beats, c.secs[m + 1].first, &c.secs[m + 1]);
        rx_tempo_octave_decide(&c.ons, s->t0, s->period, from, to, lo, hi, c.opt->prior_bpm, &od);
        out->octave_margin = od.margin;
        rx_debug("beats: octave x%g (margin %.2f; %.2f: %.2f, %.2f: %.2f, %.2f: %.2f)\n",
                 od.factor, od.margin, od.bpm[0], od.ev[0].score, od.bpm[1], od.ev[1].score,
                 od.bpm[2], od.ev[2].score);
        if (od.factor != 1.0) {
            ret = fit_grid(&c, bpm0 * od.factor, lo, hi);
            stage(&tm, "octave");
            if (ret > 0) {
                out->flags = RX_GRID_FLAG_LOW_CONFIDENCE;
                ret = 0;
                goto fail;
            }
            if (ret < 0)
                goto fail;
        }
    }
    const double kick_share = c.kick_share;
    const float *arbiter = c.arbiter;
    int *bands = c.bands;
    stage(&tm, "refine");
    /* Which half of the beat, and which beat of the bar, section by
     * section. */
    sinfo = calloc(c.nb_secs, sizeof(*sinfo));
    if (!sinfo) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    for (int i = 0; i < c.nb_secs; i++) {
        sinfo[i].start = i ? rx_beat_predict(c.beats, c.secs[i].first, &c.secs[i]) : 0.0;
        sinfo[i].end = c.duration;
        if (i)
            sinfo[i - 1].end = sinfo[i].start;
    }
    if (dbg)
        dbg->start = ms;
    for (int i = 0; i < c.nb_secs; i++) {
        if ((ret = decide_phase(&c, i, &sinfo[i], arbiter, &ms, first_onset, &bands[i], dbg)) < 0)
            goto fail;
    }

    /* Sections the half-beat decisions have put back on (nearly) one line
     * are one section again: one line holds the other's beats within the
     * merge tolerance (15 ms, the segmentation's test 1). Such a "cut" is
     * no cut a DJ could hear, and on a stick it would make two beats a few
     * ms apart. The line kept is the one with more attacks if it holds
     * both, expressed in the earlier one's count (its t0 stays on the same
     * beat, which the downbeat vote counts from). */
    for (int i = 1; i < c.nb_secs;) {
        RXFitSection *a = &c.secs[i - 1], *b = &c.secs[i];
        const RXFitSection *keep = b->supported > a->supported ? b : a;
        const RXFitSection *other = keep == a ? b : a;
        if (line_distance(c.beats, other, keep) > SECTION_MERGE_DEV) {
            const RXFitSection *t = keep;
            keep = other;
            other = t;
            if (line_distance(c.beats, other, keep) > SECTION_MERGE_DEV) {
                i++;
                continue;
            }
        }
        double t0 = keep->t0 + round((a->t0 - keep->t0) / keep->period) * keep->period;
        double period = keep->period;
        for (int j = b->first; j <= b->last; j++) {
            double t = rx_beat_predict(c.beats, j, b);
            c.beats[j].index = a->index0 + (int)lrint((t - t0) / period);
        }
        a->t0 = t0;
        a->period = period;
        a->last = b->last;
        a->supported += b->supported;
        a->jitter = fmax(a->jitter, b->jitter);
        a->scatter = fmax(a->scatter, b->scatter);
        sinfo[i - 1].end = sinfo[i].end;
        memmove(c.secs + i, c.secs + i + 1, (c.nb_secs - i - 1) * sizeof(*c.secs));
        memmove(sinfo + i, sinfo + i + 1, (c.nb_secs - i - 1) * sizeof(*sinfo));
        memmove(bands + i, bands + i + 1, (c.nb_secs - i - 1) * sizeof(*bands));
        c.nb_secs--;
        rx_debug("beats: sections %d and %d are one line again\n", i - 1, i);
    }

    /* The section that speaks for the track in the flags: the longest. */
    int main_sec = 0;
    for (int i = 1; i < c.nb_secs; i++) {
        if (c.secs[i].last - c.secs[i].first > c.secs[main_sec].last - c.secs[main_sec].first)
            main_sec = i;
    }
    out->phase_margin = exp(fabs(sinfo[main_sec].phase_evidence));

    /* The grid. Each section's downbeat comes from its own vote; a later
     * section that starts on the previous one's grid continues its count
     * (beat 0) unless its own music says clearly otherwise. */
    out->grid.beats_per_bar = 4;
    out->grid.sections = calloc(c.nb_secs, sizeof(*out->grid.sections));
    if (!out->grid.sections) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    double prev_down = 0.0;
    for (int i = 0; i < c.nb_secs; i++) {
        RXFitSection *s = &c.secs[i];
        SecInfo *si = &sinfo[i];
        RXTempoSection *g = &out->grid.sections[i];
        const double P = s->period;
        g->bpm = 60.0 / P;
        double own = si->has_down ? s->t0 + si->pos / 2 * P : NAN;
        if (i == 0) {
            /* From its first beat at or after 0 (the music's start moves it
             * on below), numbered from the downbeat. */
            double d = isnan(own) ? s->t0 : own;
            long k = (long)floor(d / P);
            g->start = d - k * P;
            g->beat = (int)((-k % 4 + 4) % 4) + 1;
            prev_down = d;
            continue;
        }
        const RXFitSection *a = &c.secs[i - 1];
        g->start = rx_beat_predict(c.beats, s->first, s);
        /* The previous section's count at this time. */
        double ka = (g->start - prev_down) / a->period;
        bool continuous = fabs(ka - round(ka)) * a->period <= 0.015;
        int cont = (int)((lrint(ka) % 4 + 4) % 4) + 1;
        int mine = 0;
        if (!isnan(own)) {
            long kb = lrint((g->start - own) / P);
            mine = (int)((kb % 4 + 4) % 4) + 1;
        }
        if (continuous && (!mine || mine == cont || si->down_margin < 1.5)) {
            g->beat = 0;
            mine = cont;
        } else {
            g->beat = mine ? mine : cont;
            mine = g->beat;
        }
        prev_down = g->start - (mine - 1) * P;
    }
    out->grid.nb_sections = c.nb_secs;
    /* No grid in the silence before the music: the first beat is the first
     * at or after its start, every later beat as it was. */
    if (ms.kind != RX_START_AT_ONCE &&
        (ret = rx_grid_start_at(&out->grid, fmax(ms.start - START_SLACK, 0.0))) < 0)
        goto fail;

    /* Confidence and flags. The confidence reads the grid as written:
     * where a tracked beat strayed from it (a stretch followed on the
     * off-beat and outvoted), the envelopes are cut again at the grid. */
    stage(&tm, "phase+down");
    for (int i = 0; i < c.nb_secs; i++) {
        const RXFitSection *s = &c.secs[i];
        for (int k = s->first; k <= s->last; k++) {
            double p = rx_beat_predict(c.beats, k, s);
            if (fabs(c.beats[k].time - p) <= 0.025)
                continue;
            c.beats[k].time = p;
            ret = rx_env_window(c.envctx, p, c.envs.env + (size_t)k * RX_NB_BANDS * RX_ENV_LEN,
                                &c.envs.centre[k]);
            if (ret < 0)
                goto fail;
        }
    }
    out->confidence = grid_confidence(&c, bands);
    stage(&tm, "confidence");
    rx_debug("beats: total      %6.1f ms\n", (rx_time_us() - tm0) / 1000.0);
    out->bpm_second_best = c.tempo.second_bpm;
    /* A boundary between two sections at one tempo is a phase cut (the
     * music re-enters shifted), any other a tempo change. */
    unsigned flags = 0;
    for (int i = 1; i < c.nb_secs; i++) {
        if (fabs(c.secs[i].period / c.secs[i - 1].period - 1.0) < 0.005)
            flags |= RX_GRID_FLAG_PHASE_CUT;
        else
            flags |= RX_GRID_FLAG_TEMPO_CHANGE;
    }
    if (out->octave_margin < 1.5)
        flags |= RX_GRID_FLAG_OCTAVE_UNCERTAIN;
    if (out->phase_margin < 1.5)
        flags |= RX_GRID_FLAG_PHASE_UNCERTAIN;
    if (!sinfo[main_sec].has_vote || sinfo[main_sec].down_margin < 1.2)
        flags |= RX_GRID_FLAG_DOWNBEAT_WEAK;
    if (kick_presence(&c, 60.0 / c.secs[longest_section(&c)].period) < 0.5)
        flags |= RX_GRID_FLAG_NO_KICKS;
    if (out->confidence < 0.5)
        flags |= RX_GRID_FLAG_LOW_CONFIDENCE;
    double jit = 0.0, scat = 0.0;
    for (int i = 0; i < c.nb_secs; i++) {
        jit = fmax(jit, c.secs[i].jitter);
        scat = fmax(scat, c.secs[i].scatter);
    }
    /* Live playing spreads the attacks around the line and scatters them
     * from beat to beat. A small step merged into one line (a re-entry
     * within 15 ms) or a slow drift only spreads them; an alternation
     * (the attack taken on another hit every other beat) mostly
     * scatters them. Both are needed (the spread alone where too few
     * consecutive attacks show the scatter). */
    for (int i = 0; i < c.nb_secs; i++) {
        const RXFitSection *s = &c.secs[i];
        if (s->jitter > UNSTEADY_JITTER && (s->scatter < 0.0 || s->scatter > UNSTEADY_SCATTER))
            flags |= RX_GRID_FLAG_UNSTEADY;
    }
    out->flags = flags;

    if (dbg) {
        dbg->ons = c.ons;
        memset(&c.ons, 0, sizeof(c.ons));
        dbg->beats = c.beats;
        dbg->nb_beats = c.nb_beats;
        c.beats = NULL;
        dbg->secs = c.secs;
        dbg->nb_secs = c.nb_secs;
        c.secs = NULL;
        dbg->tempo = c.tempo;
        dbg->octave = od;
        dbg->downbeat = sinfo[main_sec].db;
        dbg->half_shift = sinfo[main_sec].shifted;
        dbg->kick_share = kick_share;
        dbg->jitter = jit;
        dbg->scatter = scat;
        dbg->band = bands[0];
    }

fail:
    free(sinfo);
    ctx_free(&c);
    free(resampled);
    if (ret < 0)
        rx_beat_result_free(out);
    return ret;
}
