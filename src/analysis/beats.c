/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rx_analyse_beats: the beat-grid analyser, stage by stage (docs/BEATS.md
 * explains each decision and the evidence for it):
 *
 *   spectrum.c   STFT, mel bands, kick band               (150 frames/s)
 *   onset.c      SuperFlux and band fluxes
 *   tempo.c      whole-track tempo, octave by evidence, tempo regions
 *   grid_fit.c   beat tracking, attacks, robust lines, sections, snapping
 *   walk.c       whether a line holds its beats; the beat-by-beat grid
 *   downbeat.c   bar position from structure
 *   here         the order, the dynamic pass (which sections keep their
 *                measured beats), the half-beat and downbeat decisions,
 *                the grid, the confidence and the flags
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
    "dynamic",
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
        .allow_dynamic         = true,
        .deterministic_threads = false,
        .compute               = NULL,
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
/* Around a walked beat of a dynamic stretch (the same frames, and the
 * walk's own slack). */
#define ATTACK_TOL_WALK   0.025
/* Drift allowed by BPM snapping, at each end of a section. */
#define SNAP_TOL          0.005
/* A live drummer's beats scatter by 5-15 ms; a sequencer's by under 1. */
#define UNSTEADY_JITTER   0.006
#define UNSTEADY_SCATTER  0.004
/* Two sections whose lines agree this closely are one (grid_fit.c's
 * SEG_MAX_DEV, the segmentation's test 1). */
#define SECTION_MERGE_DEV 0.015
/* The dynamic pass (walk.c; docs/BEATS.md, "Dynamic grids"). A section
 * keeps its measured beats where the music drifts from its line by more
 * than the merge tolerance (a straight grid is then audibly off there),
 * from where the drift leaves 5 ms to where it returns within it; or
 * everywhere, when the beats bend around the line by more than 10 ms
 * and four times what the section's own scatter would bend them (a tempo
 * that moves inside the section), and the line is indeed 8 ms off
 * somewhere. A straight remnant shorter than a section's minimum (32
 * beats) joins the dynamic stretch beside it. */
#define DYN_DRIFT         0.015
#define DYN_EDGE          0.005
/* Drift is read only where the attacks' spread around the line says live
 * playing (over 3 ms, refine_section's "steady"): sequenced music does
 * not drift, and where its attacks leave the line for a stretch it is
 * another element being timed (a breakdown's claps, an element swelling
 * in), which the line is right to ignore. A DAW's tempo ramp is
 * sequenced and moves: the bend reads it, and its edges come in from the
 * sections beside it. */
#define DYN_LIVE_JITTER   0.003
/* That spread is no proof of live playing: where the tracker followed
 * another element for a while, or a fill's accents, the runs off the
 * line inflate it. AD:HOUSE 11's "Felicidade" is sequenced at 130.000
 * BPM (its segments merge at one tempo within a millisecond), yet its
 * attacks spread 6.9 ms around the line, 7.3 ms beat to beat, and a
 * syncopated fill and breaks tracked on the off-beat read as 375 ms of
 * drift. So the attacks on the line are read on their own
 * (online_scatter): when at least a quarter of their runs of three are
 * spaced as evenly as a sequencer spaces its hits (a scatter of 1.5 ms
 * read from the lower quartile; Felicidade 0.57 ms, the rubato case of
 * test_beats, 3 ms of jitter, 3.0 ms, the other sections of the
 * acceptance list whose spread says live 2.5 to 16 ms), the section is
 * sequenced whatever its spread. Read on 32 such runs at least. */
#define DYN_SEQ_SCATTER   0.0015
#define DYN_SEQ_TRIPLES   32
/* A run of beats off the line whose attacks scatter beat to beat far
 * more than the section's do (three times, and 3 ms) is not the tempo
 * moving but the attacks jumping between elements: a breakbeat's kicks
 * and snares, a breakdown's claps on a sequenced track, where the line
 * is right. A drummer's rubato scatters no more than the drummer does; a
 * DAW's ramp not at all. */
#define DYN_RUN_SCATTER   3.0
#define DYN_RUN_SCATTER_MIN 0.003
#define DYN_BEND          0.010
#define DYN_BEND_DRIFT    0.008
#define DYN_BEND_NOISE    4.0
#define DYN_MIN_STRAIGHT  32
/* A run of beats off the line is another element, not drift, when its
 * deviation sits still: the longest stretch over which the smoothed
 * deviation stays within 3 ms (or the section's scatter) of one level
 * covers at least two fifths of the run, and eight beats. A breakdown
 * tracked on its off-beat sits half a beat from the line for sixty bars
 * on AD:HOUSE 11's "Hot Vacation", its deviation flat at -233 ms the
 * whole way but for the smoothing's eight beats at each end; a drift
 * keeps moving, and a drummer's 29 ms over 58 beats stands still for no
 * more than a quarter of them. */
#define DYN_PLATEAU_TOL   0.003
#define DYN_PLATEAU_SHARE 0.4
#define DYN_PLATEAU_MIN   8
/* A dynamic stretch takes this many beats of the line either side of
 * where the drift was read: the tracker's counts, which the reading
 * rests on, go wrong exactly where the music has moved most (an extra
 * beat on a hat), so the drift's tail can be read a few beats short.
 * Beats that are on the line walk to the line's times, so the margin
 * costs nothing where it is not needed. */
#define DYN_MARGIN        4
/* A walked stretch is judged window by window (judge_stretch), in the
 * confidence's 16-beat windows. A window is lost where the line holds
 * the hits within the merge tolerance and the walked beats lie further
 * from them than the line by as much again and DYN_LOSE: twice as far,
 * and audibly so where the hits are clear. There the line is right
 * (Felicidade's fill: the walk 300 ms off the pulse in one bar, kept
 * because it won by a millisecond over the whole track). A tie is no
 * loss: the fold of 16 beats is noisy in live playing, and blind to a
 * drift that covers less than half of its window (the random-walk case
 * of test_beats ties within 2 ms where its line crosses the music, the
 * rubato case's first window while its line slips 50 ms behind it;
 * given back to the line, those windows cost beats at the joins and 6
 * of the rubato's 48 downbeats). A lost window goes back to the line,
 * and the stretch is split or shortened and walked again; what is left
 * shorter than a window (DYN_MIN_WALK beats) goes back too. A stretch
 * also hands over to a straight piece of its line beside it on the same
 * beat, its end beat within the merge tolerance of the line's (or the
 * window at that end is lost). A stretch that loses nothing is kept when
 * it brings the hits DYN_KEEP_MARGIN closer than the line on average:
 * the line is the simpler grid, and a tie is its. */
#define DYN_KEEP_MARGIN   0.001
#define DYN_LOSE          0.003
#define DYN_MIN_WALK      16
/* The music's start as a witness (docs/BEATS.md, section 10). A hit or a
 * fade's origin within a tenth of a beat (at most 40 ms) of a beat or a
 * half beat of the line is on it. A hit counts when the kick band rises at
 * it at least twice as much as on the beats that carry the kicks (an
 * opening downbeat, not one more kick), and only while the kicks
 * themselves do not mark one half of the beat by a factor e. It is log
 * evidence for its half of the beat and a vote for beat 1 (in structure
 * scales, as downbeat.c's votes); a fade's origin only breaks a tie for
 * beat 1. */
#define START_ON_BEAT     0.10
#define START_ON_BEAT_MAX 0.040
#define HIT_KICK          2.0
#define HIT_KICKS_CLEAR   1.0
#define HIT_PHASE         2.5
#define HIT_DOWNBEAT      2.0
#define FADE_DOWNBEAT     0.5
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
    double        onset_shift;      /* from the onset frames to the attacks (fit_grid) */
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

/* Cuts the envelopes of the beats idx[0 .. nb) again at their times, in
 * one batch: on the GPU each window is a workgroup of one dispatch. */
static int cut_envs(Ctx *c, const int *idx, int nb)
{
    if (nb <= 0)
        return 0;
    const size_t len = (size_t)RX_NB_BANDS * RX_ENV_LEN;
    double *t = malloc((size_t)nb * sizeof(*t));
    double *centre = malloc((size_t)nb * sizeof(*centre));
    float *env = malloc((size_t)nb * len * sizeof(*env));
    int ret = RX_ERROR(ENOMEM);
    if (t && centre && env) {
        for (int i = 0; i < nb; i++)
            t[i] = c->beats[idx[i]].time;
        ret = rx_env_windows(c->envctx, t, nb, env, centre);
    }
    if (ret >= 0) {
        for (int i = 0; i < nb; i++) {
            memcpy(c->envs.env + idx[i] * len, env + i * len, len * sizeof(*env));
            c->envs.centre[idx[i]] = centre[i];
        }
    }
    free(t);
    free(centre);
    free(env);
    return ret;
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Robust spread (MAD) of the attacks around the section's line; r has
 * room for the section's beats. */
static double section_jitter(const RXBeatObs *beats, const RXFitSection *s, double *r)
{
    int n = 0;
    for (int i = s->first; i <= s->last; i++) {
        if (beats[i].weight > 0.0 && !isnan(beats[i].attack))
            r[n++] = fabs(beats[i].attack - rx_beat_predict(beats, i, s->dynamic ? NULL : s));
    }
    double j = 0.0;
    if (n >= 8) {
        qsort(r, n, sizeof(*r), cmp_dbl);
        j = 1.4826 * r[n / 2];
    }
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
 * are too few runs of three attacks to tell. r as for section_jitter. */
static double section_scatter(const RXBeatObs *beats, const RXFitSection *s, double *r)
{
    int n = 0;
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
    return sc;
}

/* The section's jitter and scatter, or -ENOMEM: no value stands in for
 * them (a spread of 0 calls a section steady and sequenced). */
static int section_spread(const RXBeatObs *beats, RXFitSection *s)
{
    double *r = malloc((size_t)(s->last - s->first + 1) * sizeof(*r));
    if (!r)
        return RX_ERROR(ENOMEM);
    s->jitter = section_jitter(beats, s, r);
    s->scatter = section_scatter(beats, s, r);
    free(r);
    return 0;
}

/* Puts a section on its attacks. The fold of the whole section chooses
 * the band and moves the line onto the start of the average beat's hit;
 * then the attacks are measured close to the line and the line refitted
 * through them; then the fold again. The search shrinks at every step
 * (30, then 10 to 30 by the scatter, 5, 3 ms) so that the line cannot walk
 * from the kick onto a hat or a clap a little later, which a re-centred
 * search of constant width would let it do one step at a time. Returns
 * the band used, or a negative error (an allocation failed). */
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
        RXFitSection t = { 0 }, init = *s;
        int ret = rx_fit_line(c->beats, s->first, s->last, &init, &t);
        if (ret == RX_ERROR(ENOMEM))
            return ret;
        if (ret == 0 && t.supported >= 8)
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
    int ret = section_spread(c->beats, s);
    return ret < 0 ? ret : band;
}

/* ------------------------------------------------------- dynamic pass */

/* The least-squares line through the times of beats [first, last]: a
 * dynamic section's straight approximation, for the arithmetic that
 * wants a period and a count (the flags, the octave's reading). */
static void line_through_times(const RXBeatObs *beats, RXFitSection *s)
{
    double sk = 0, st = 0, skk = 0, skt = 0;
    int n = 0;
    s->index0 = beats[s->first].index;
    for (int i = s->first; i <= s->last; i++) {
        double k = beats[i].index - s->index0, t = beats[i].time;
        sk += k;
        st += t;
        skk += k * k;
        skt += k * t;
        n++;
    }
    double det = n * skk - sk * sk;
    if (n < 2 || det <= 0.0)
        return;
    s->period = (n * skt - sk * st) / det;
    s->t0 = (st - s->period * sk) / n;
}

/* Replaces the observations of section si by `nb` new beats (times, and
 * consecutive counts from `index0`; no attacks yet), or by `saved` ones
 * put back as they were with their envelopes (`saved_env`,
 * `saved_centre`): the beat array, the envelopes (cut at the beats'
 * times when not given) and the sections after si, whose observations
 * and counts move so that the beat after the stretch keeps the count
 * after its last, and their lines and the numbering across the boundary
 * stay as they were. */
static int replace_beats(Ctx *c, int si, const double *t, const RXBeatObs *saved,
                         const float *saved_env, const double *saved_centre, int nb, int index0)
{
    RXFitSection *s = &c->secs[si];
    int old = s->last - s->first + 1, delta = nb - old;
    if (saved)
        index0 = saved[0].index;
    /* By the last count going in, not the first and the count: the
     * tracker's beats can skip a count (an interval near two periods),
     * and a stretch put back as it was must move the sections after it
     * back by what its re-tracked, gapless beats moved them. */
    int new_last = saved ? saved[nb - 1].index : index0 + nb - 1;
    int shift = new_last - c->beats[s->last].index;
    const size_t len = (size_t)RX_NB_BANDS * RX_ENV_LEN;
    int total = c->nb_beats + delta;
    RXBeatObs *b = malloc((size_t)RX_MAX(total, 1) * sizeof(*b));
    float *env = malloc((size_t)RX_MAX(total, 1) * len * sizeof(*env));
    double *centre = malloc((size_t)RX_MAX(total, 1) * sizeof(*centre));
    int *idx = malloc((size_t)RX_MAX(nb, 1) * sizeof(*idx));
    if (!b || !env || !centre || !idx) {
        free(b);
        free(env);
        free(centre);
        free(idx);
        return RX_ERROR(ENOMEM);
    }
    int o = 0;
    for (int i = 0; i < s->first; i++, o++) {
        b[o] = c->beats[i];
        memcpy(env + o * len, c->envs.env + i * len, len * sizeof(*env));
        centre[o] = c->envs.centre[i];
    }
    for (int k = 0; k < nb; k++, o++) {
        if (saved) {
            b[o] = saved[k];
        } else {
            b[o] = (RXBeatObs) {
                .time   = t[k],
                .attack = NAN,
                .weight = 0.0,
                .kick   = 0.0,
                .index  = index0 + k,
            };
        }
        centre[o] = b[o].time;
        if (saved_env) {
            memcpy(env + o * len, saved_env + (size_t)k * len, len * sizeof(*env));
            centre[o] = saved_centre[k];
        }
        idx[k] = o;
    }
    for (int i = s->last + 1; i < c->nb_beats; i++, o++) {
        b[o] = c->beats[i];
        b[o].index += shift;
        memcpy(env + o * len, c->envs.env + i * len, len * sizeof(*env));
        centre[o] = c->envs.centre[i];
    }
    free(c->beats);
    free(c->envs.env);
    free(c->envs.centre);
    c->beats = b;
    c->nb_beats = total;
    c->envs.env = env;
    c->envs.centre = centre;
    c->envs.nb = total;
    s->last = s->first + nb - 1;
    for (int j = si + 1; j < c->nb_secs; j++) {
        c->secs[j].first += delta;
        c->secs[j].last += delta;
        c->secs[j].index0 += shift;
    }
    int ret = saved_env ? 0 : cut_envs(c, idx, nb);
    free(idx);
    return ret;
}

/* The beats of a dynamic stretch, tracked again at a period that follows
 * the music. Ellis's tracker held to one period puts extra beats on the
 * hats when the music slows by a fifth (the rubato case of test_beats:
 * two inserted in eight bars, the count wrong from there on), so the
 * stretch's tempo is read first: at every tracked beat the median of
 * the intervals between consecutive attacks within eight beats of it,
 * those within 40 % of the line's period (an inserted beat makes a half
 * interval, a skipped one a double, and neither counts), and the tracker
 * runs over the stretch's frames with that period, from half a period
 * before its first beat to half a period before the beat after it. The
 * first beat found takes the count its time has on the old beats, so the
 * numbering across the stretch holds. The times are the tracker's, moved
 * by the fold's delay as the first ones were; the attacks refine them. */
static int retrack_stretch(const Ctx *c, const RXFitSection *s, double next,
                           double **out, int *nb, int *index0)
{
    const RXOnsets *o = &c->ons;
    const double fps = o->fps, P = s->period;
    int n_old = s->last - s->first + 1;
    const RXBeatObs *b = c->beats + s->first;

    /* The local period at each old beat. */
    double *iv = malloc((size_t)n_old * sizeof(*iv));
    double *lp = malloc((size_t)n_old * sizeof(*lp));
    double *win = malloc((size_t)n_old * sizeof(*win));
    if (!iv || !lp || !win) {
        free(iv);
        free(lp);
        free(win);
        return RX_ERROR(ENOMEM);
    }
    int prev = -1;
    for (int i = 0; i < n_old; i++) {
        iv[i] = NAN;
        if (!(b[i].weight > 0.0) || isnan(b[i].attack))
            continue;
        if (prev >= 0) {
            double d = (b[i].attack - b[prev].attack) / (b[i].index - b[prev].index);
            if (d > 0.6 * P && d < 1.4 * P)
                iv[i] = d;
        }
        prev = i;
    }
    for (int i = 0; i < n_old; i++) {
        int m = 0;
        for (int j = RX_MAX(i - 8, 0); j <= RX_MIN(i + 8, n_old - 1); j++) {
            if (!isnan(iv[j]))
                win[m++] = iv[j];
        }
        if (m >= 3) {
            qsort(win, (size_t)m, sizeof(*win), cmp_dbl);
            lp[i] = win[m / 2];
        } else {
            lp[i] = NAN;
        }
    }
    /* Gaps in the local period are filled from the neighbours, the ends
     * from the line. */
    for (int i = 0; i < n_old; i++) {
        if (!isnan(lp[i]))
            continue;
        int a = i - 1, z = i + 1;
        while (a >= 0 && isnan(lp[a]))
            a--;
        while (z < n_old && isnan(lp[z]))
            z++;
        if (a >= 0 && z < n_old)
            lp[i] = lp[a] + (lp[z] - lp[a]) * (i - a) / (double)(z - a);
        else if (a >= 0)
            lp[i] = lp[a];
        else if (z < n_old)
            lp[i] = lp[z];
        else
            lp[i] = P;
    }

    /* The period per frame over the stretch, and the tracker. */
    /* From a quarter period before the first beat (the tracker's first
     * beat may be anywhere within its first two periods: the second beat
     * then, which the count below absorbs) to a quarter period past where
     * the last beat belongs, a period before the beat after the stretch:
     * the tracker ends on the best score within its last period, and a
     * range reaching further lets a hat there end it half a beat short. */
    double t_first = b[0].time, t_end = isnan(next) ? b[n_old - 1].time + lp[n_old - 1] : next;
    int f0 = (int)lrint((t_first - 0.25 * lp[0] - c->onset_shift) * fps);
    int f1 = (int)lrint((t_end - 0.75 * lp[n_old - 1] - c->onset_shift) * fps);
    f0 = RX_CLAMP(f0, 0, o->nb - 1);
    f1 = RX_CLAMP(f1, f0, o->nb - 1);
    float *period = malloc((size_t)o->nb * sizeof(*period));
    if (!period) {
        free(iv);
        free(lp);
        free(win);
        return RX_ERROR(ENOMEM);
    }
    for (int f = f0, i = 0; f <= f1; f++) {
        double t = f / fps + c->onset_shift;
        while (i + 1 < n_old && b[i + 1].time <= t)
            i++;
        double p = lp[i];
        if (i + 1 < n_old && b[i + 1].time > b[i].time) {
            double u = RX_CLAMP((t - b[i].time) / (b[i + 1].time - b[i].time), 0.0, 1.0);
            p = lp[i] + (lp[i + 1] - lp[i]) * u;
        }
        period[f] = (float)p;
    }
    RXBeatObs *nb_obs = NULL;
    int n = 0;
    int ret = rx_track_beats_range(o, period, TRACK_TIGHTNESS, f0, f1, &nb_obs, &n);
    free(period);
    free(iv);
    free(win);
    if (ret < 0 || n < 2) {
        free(nb_obs);
        free(lp);
        return ret < 0 ? ret : RX_ERROR(EINVAL);
    }
    double *tm = malloc((size_t)n * sizeof(*tm));
    if (!tm) {
        free(nb_obs);
        free(lp);
        return RX_ERROR(ENOMEM);
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        double t = nb_obs[i].time + c->onset_shift;
        /* The first beat kept sits on the old count (within three tenths
         * of a period of a beat of it): a hat between is no anchor. Two
         * beats closer than half a period are one hit: the first. And the
         * stretch ends a beat before the section after it, not on a hat
         * halfway there. */
        if (m == 0) {
            double k = (t - t_first) / lp[0];
            if (fabs(k - round(k)) > 0.3)
                continue;
        }
        if (m > 0 && t - tm[m - 1] < 0.5 * lp[RX_MIN(i, n_old - 1)])
            continue;
        if (!isnan(next) && t > next - 0.6 * lp[n_old - 1])
            break;
        tm[m++] = t;
    }
    n = m;
    free(nb_obs);
    if (n < 2) {
        free(tm);
        free(lp);
        return RX_ERROR(EINVAL);
    }
    /* The first beat's count: where its time falls on the old beats. */
    *index0 = b[0].index + (int)lrint((tm[0] - t_first) / lp[0]);
    free(lp);
    *out = tm;
    *nb = n;
    return 0;
}

/* How far the hits of section s lie from the beats given (its walked
 * beats, or its line's beats), window by window: the distance of the
 * average beat's rise from them in each of grid_confidence's windows (16
 * beats, every 8: window w is beats [8w, 8w + 15]), on envelopes cut at
 * those very times, as the whole-track confidence reads the grid as
 * written (judged on envelopes cut around the tracked beats, a line far
 * from the music has no data where it is wrong and scores on the
 * windows where it is right). The confidence's own yes-or-no at 15 ms is
 * too coarse to compare a line a few beats off the music with the beats
 * themselves. dist holds STRETCH_WINDOWS(n) values; returns how many
 * windows there are, 0 without one, or a negative error. */
#define STRETCH_WINDOWS(n) ((n) / 8 + 2)

static int stretch_windows(Ctx *c, const RXFitSection *s, int band, const double *times,
                           double *med_ref, double *dist)
{
    int n = s->last - s->first + 1;
    int ret = RX_ERROR(ENOMEM);
    RXBeatObs *obs = malloc((size_t)n * sizeof(*obs));
    RXBeatEnvs e = { .nb = n };
    e.env = malloc((size_t)n * RX_NB_BANDS * RX_ENV_LEN * sizeof(*e.env));
    e.centre = malloc((size_t)n * sizeof(*e.centre));
    int cap = STRETCH_WINDOWS(n), nw = 0;
    double *contrast = malloc((size_t)cap * sizeof(*contrast));
    double *offs = malloc((size_t)cap * sizeof(*offs));
    double *tmp = malloc((size_t)cap * sizeof(*tmp));
    if (!obs || !e.env || !e.centre || !contrast || !offs || !tmp)
        goto end;
    for (int i = 0; i < n; i++) {
        obs[i] = c->beats[s->first + i];
        obs[i].time = times[i];
    }
    if ((ret = rx_env_windows(c->envctx, times, n, e.env, e.centre)) < 0)
        goto end;
    ret = 0;
    double search = fmin(0.125 * s->period, 0.050);
    for (int a = 0; a < n && nw < cap; a += 8) {
        int b = RX_MIN(a + 15, n - 1);
        if (b - a < 7)
            break;
        rx_fold(&e, obs, a, b, NULL, band, search, &contrast[nw], &offs[nw]);
        nw++;
    }
    /* The reference contrast: the median over these windows when none is
     * given (the walked beats'), which the line is then read against. A
     * window whose average beat rises by less than half of it has no hit
     * where the beats are (rx_fold reports no rise as an offset of 0): the
     * hits are as far as the search goes. */
    if (*med_ref < 0.0) {
        int nt = 0;
        for (int i = 0; i < nw; i++) {
            if (contrast[i] > 0.0)
                tmp[nt++] = contrast[i];
        }
        if (!nt)
            goto end;
        qsort(tmp, (size_t)nt, sizeof(*tmp), cmp_dbl);
        *med_ref = tmp[nt / 2];
    }
    for (int i = 0; i < nw; i++)
        dist[i] = contrast[i] < 0.5 * *med_ref ? search : fabs(offs[i]);
    ret = nw;
end:
    free(obs);
    free(e.env);
    free(e.centre);
    free(contrast);
    free(offs);
    free(tmp);
    return ret;
}

/* Puts a dynamic section on its attacks again, where its beats now are:
 * the attacks within 20 ms of the beats' times (a tracker's frame or
 * two), then the walk through them, and the line and the spread for the
 * arithmetic and the flags. */
static int refine_dynamic(Ctx *c, RXFitSection *s, int band)
{
    rx_measure_attacks(&c->envs, c->beats, s->first, s->last, band, NULL, ATTACK_TOL_WALK);
    for (int i = s->first; i <= s->last; i++) {
        if (c->beats[i].weight < s->wmin)
            c->beats[i].weight = 0.0;
    }
    int ret = rx_walk_beats(c->beats, s->first, s->last, s->scatter);
    if (ret < 0)
        return ret;
    line_through_times(c->beats, s);
    s->dynamic = true;
    if ((ret = section_spread(c->beats, s)) < 0)
        return ret;
    s->supported = 0;
    for (int i = s->first; i <= s->last; i++)
        s->supported += c->beats[i].weight > 0.0 && !isnan(c->beats[i].attack);
    return 0;
}

/* The judgment of the walked stretch qi against the line it replaces
 * (`line`, over the same beats), window by window, in the confidence's
 * 16-beat windows (stretch_windows; DYN_KEEP_MARGIN). A window is lost
 * when the line holds its hits within the merge tolerance and the walked
 * beats lie twice as far from them and DYN_LOSE more. And a stretch hands
 * over to a straight piece beside it (`prev`, `next`: the parent's
 * remnants, NULL where there is none) on the same beat: its end beat
 * within the merge tolerance of that piece's line's beat of the same
 * count, or the window at that end is lost too (a walk that left the
 * line's phase or its count would put two beats a flam apart there, or
 * skip one).
 *
 * A stretch that loses nothing and brings the hits closer on average by
 * DYN_KEEP_MARGIN is kept (*keep); the line is the simpler grid, and a
 * tie is its. One that loses some of its windows gives them back to the
 * line: `dyn` (the classification of the stretch's `n_old` tracked beats
 * `saved`) loses every beat within their span, and what is left shorter
 * than a window, and *again asks for the section to be split anew, the
 * shorter stretches walked and judged again. A walk that wins on
 * average while it loses a bar by 300 ms (Felicidade's fill, before the
 * sequencer's rule) is kept nowhere near that bar. */
static int judge_stretch(Ctx *c, int qi, const RXFitSection *line, const RXFitSection *prev,
                         const RXFitSection *next, const RXBeatObs *saved, int n_old, bool *dyn,
                         bool *keep, bool *again)
{
    const RXFitSection *q = &c->secs[qi];
    int nq = q->last - q->first + 1, cap = STRETCH_WINDOWS(nq), band = c->bands[qi];
    double *tl = malloc((size_t)nq * sizeof(*tl)), *tb = malloc((size_t)nq * sizeof(*tb));
    double *dl = malloc((size_t)cap * sizeof(*dl)), *db = malloc((size_t)cap * sizeof(*db));
    bool *lost = malloc((size_t)cap * sizeof(*lost));
    int ret = RX_ERROR(ENOMEM);
    *keep = false;
    if (!tl || !tb || !dl || !db || !lost)
        goto end;
    for (int j = 0; j < nq; j++) {
        tl[j] = rx_beat_predict(c->beats, q->first + j, line);
        tb[j] = c->beats[q->first + j].time;
    }
    /* The walked beats' median contrast is the reference for both (a
     * window without a rise where the beats are counts as far as the
     * search reaches). Without windows, or should an envelope fail to
     * be cut, the line stays. */
    double med = -1.0;
    int nw = stretch_windows(c, q, band, tb, &med, db);
    int nl = nw > 0 ? stretch_windows(c, line, band, tl, &med, dl) : 0;
    if (nw == RX_ERROR(ENOMEM) || nl == RX_ERROR(ENOMEM))
        goto end;
    ret = 0;
    if (nw <= 0 || nl != nw)
        nw = 0;
    int nb_lost = 0;
    double sum_b = 0.0, sum_l = 0.0;
    for (int w = 0; w < nw; w++) {
        lost[w] = dl[w] <= DYN_DRIFT && db[w] > dl[w] + fmax(DYN_LOSE, dl[w]);
        sum_b += db[w];
        sum_l += dl[w];
    }
    /* The joins. */
    double off_lo = 0.0, off_hi = 0.0;
    if (nw && prev) {
        const RXBeatObs *b = &c->beats[q->first];
        off_lo = b->time - (prev->t0 + (b->index - prev->index0) * prev->period);
        lost[0] = lost[0] || fabs(off_lo) > DYN_DRIFT;
    }
    if (nw && next) {
        const RXBeatObs *b = &c->beats[q->last];
        off_hi = b->time - (next->t0 + (b->index - next->index0) * next->period);
        lost[nw - 1] = lost[nw - 1] || fabs(off_hi) > DYN_DRIFT;
    }
    for (int w = 0; w < nw; w++)
        nb_lost += lost[w];
    *keep = nw > 0 && !nb_lost && sum_b / nw + DYN_KEEP_MARGIN < sum_l / nw;
    if (nb_lost && nb_lost < nw) {
        /* The lost windows' beats, walked or on the line, and half a
         * period around: the tracked beats there are straight. */
        int cleared = 0;
        for (int w = 0; w < nw; w++) {
            if (!lost[w])
                continue;
            int a = 8 * w, b = RX_MIN(8 * w + 15, nq - 1);
            double t0 = fmin(tb[a], tl[a]) - 0.5 * line->period;
            double t1 = fmax(tb[b], tl[b]) + 0.5 * line->period;
            for (int j = 0; j < n_old; j++) {
                if (dyn[j] && saved[j].time >= t0 && saved[j].time <= t1) {
                    dyn[j] = false;
                    cleared++;
                }
            }
        }
        /* And what is left of the stretch shorter than a window. */
        for (int i = 0; i < n_old;) {
            int j = i;
            while (j < n_old && dyn[j] == dyn[i])
                j++;
            if (dyn[i] && saved[j - 1].index - saved[i].index + 1 < DYN_MIN_WALK) {
                for (int k = i; k < j; k++)
                    dyn[k] = false;
                cleared += j - i;
            }
            i = j;
        }
        *again = cleared > 0;
    }
    rx_debug("beats: section %d dynamic: beats %d-%d, %.2f BPM on average, jitter %.1f ms, "
             "scatter %.1f ms; hits %.1f ms from its beats, %.1f ms from the line; joins "
             "%+.1f, %+.1f ms; %d of %d windows lost%s\n", qi, q->first, q->last,
             60.0 / q->period, q->jitter * 1e3, q->scatter * 1e3, nw ? sum_b / nw * 1e3 : -1.0,
             nw ? sum_l / nw * 1e3 : -1.0, off_lo * 1e3, off_hi * 1e3, nb_lost, nw,
             *keep ? "" : nb_lost && nb_lost < nw ? ": they go back to the line"
                                                  : ": the line stays");
end:
    free(tl);
    free(tb);
    free(dl);
    free(db);
    free(lost);
    return ret;
}

/* What the strain of a section's line says, kept until every section's
 * is known: a drift run at a section's edge counts when the neighbour
 * is dynamic there, whatever the section's own kind. */
typedef struct Strain {
    RXBeatObs *tr;                  /* the attacks around the tracked beats */
    double    *dev;                 /* rx_line_strain's smoothed deviation */
    bool      *dyn;                 /* the beats that keep their measured times */
    double     drift, bend, bend_noise;
    double     on_line;             /* online_scatter: the attacks on the line, -1: too few */
    bool       bent;
    bool       live;                /* the attacks' spread says live playing */
    bool       done;                /* strain read (enough attacks) */
} Strain;

static void strain_free(Strain *st, int nb)
{
    for (int i = 0; i < nb; i++) {
        free(st[i].tr);
        free(st[i].dev);
        free(st[i].dyn);
    }
    free(st);
}

/* The beat-to-beat scatter of the attacks that are on the line, read
 * from its lower quartile (docs/BEATS.md, "Dynamic grids"). Over every
 * three consecutive beats whose attacks lie within the merge tolerance
 * of one point of the line's lattice of beats and half beats (the half
 * of the beat is decided later, section 9: Felicidade's line stood on
 * its off-beat hats here, the pulse half a beat from it), the second
 * differences of the attacks: a sequencer spaces its hits evenly, a
 * drummer by his scatter. Their lower quartile over 0.3186 sqrt(6)
 * reads white scatter sigma as sigma, as the median over 0.6745 sqrt(6)
 * would; but where a fill's accents or a conga's slaps land near the
 * beats among a sequencer's hits, the median reads the mixture and the
 * lower quartile the sequencer. The line's own small error of tempo
 * does not enter a second difference. *out is -1 with fewer than
 * DYN_SEQ_TRIPLES such triples. */
static int online_scatter(const RXBeatObs *tr, const RXFitSection *s, double *out)
{
    int n = s->last - s->first + 1, m = 0;
    double *r = malloc((size_t)RX_MAX(n, 1) * sizeof(*r));
    *out = -1.0;
    if (!r)
        return RX_ERROR(ENOMEM);
    const double half = 0.5 * s->period;
    for (int i = s->first + 1; i < s->last; i++) {
        if (tr[i].index - tr[i - 1].index != 1 || tr[i + 1].index - tr[i].index != 1)
            continue;
        bool on = true;
        double q0 = 0.0;
        for (int k = i - 1; k <= i + 1 && on; k++) {
            if (!(tr[k].weight > 0.0) || isnan(tr[k].attack)) {
                on = false;
                break;
            }
            double res = tr[k].attack - rx_beat_predict(tr, k, s), q = round(res / half);
            if (k == i - 1)
                q0 = q;
            on = q == q0 && fabs(q) <= 1.0 && fabs(res - q * half) <= SECTION_MERGE_DEV;
        }
        if (on)
            r[m++] = fabs(tr[i + 1].attack - 2.0 * tr[i].attack + tr[i - 1].attack);
    }
    if (m >= DYN_SEQ_TRIPLES) {
        qsort(r, (size_t)m, sizeof(*r), cmp_dbl);
        *out = r[m / 4] / (0.3186 * sqrt(6.0));
    }
    free(r);
    return 0;
}

/* The strain of section si: the attacks measured again around its
 * tracked beats (the line's own attack search, 35 ms wide, loses the
 * beats a drift has carried furthest from it), lighter than a quarter of
 * their median dropped as the segmentation drops them (a breakdown's
 * pads have rises too), and rx_line_strain on them. */
static int strain_read(Ctx *c, int si, Strain *st)
{
    RXFitSection *s = &c->secs[si];
    int n = s->last - s->first + 1;
    memset(st, 0, sizeof(*st));
    if (s->supported < 16 || n < 8)
        return 0;
    st->tr = malloc((size_t)n * sizeof(*st->tr));
    st->dev = malloc((size_t)n * sizeof(*st->dev));
    st->dyn = calloc((size_t)n, sizeof(*st->dyn));
    if (!st->tr || !st->dev || !st->dyn)
        return RX_ERROR(ENOMEM);
    memcpy(st->tr, c->beats + s->first, (size_t)n * sizeof(*st->tr));
    RXBeatEnvs e = {
        .nb     = n,
        .env    = c->envs.env + (size_t)s->first * RX_NB_BANDS * RX_ENV_LEN,
        .centre = c->envs.centre + s->first,
    };
    rx_measure_attacks(&e, st->tr, 0, n - 1, c->bands[si], NULL, ATTACK_TOL_TRACK);
    int nw = 0;
    for (int i = 0; i < n; i++) {
        if (st->tr[i].weight > 0.0 && !isnan(st->tr[i].attack))
            st->dev[nw++] = st->tr[i].weight;
    }
    double wmin = 0.0;
    if (nw) {
        qsort(st->dev, (size_t)nw, sizeof(*st->dev), cmp_dbl);
        wmin = 0.25 * st->dev[nw / 2];
    }
    for (int i = 0; i < n; i++) {
        if (st->tr[i].weight < wmin)
            st->tr[i].weight = 0.0;
    }
    s->wmin = wmin;
    RXFitSection ts = *s;
    ts.first = 0;
    ts.last = n - 1;
    int ret = rx_line_strain(st->tr, &ts, st->dev, &st->drift, &st->bend, &st->bend_noise);
    if (ret < 0)
        return ret;
    st->bent = st->bend > fmax(DYN_BEND, DYN_BEND_NOISE * st->bend_noise) &&
               st->drift > DYN_BEND_DRIFT;
    if ((ret = online_scatter(st->tr, &ts, &st->on_line)) < 0)
        return ret;
    /* Live by its spread around the line, unless the attacks on the
     * line say a sequencer (a run off the line is then another element
     * being timed: Felicidade's fill, its breaks on the off-beat). */
    st->live = s->jitter > DYN_LIVE_JITTER &&
               !(st->on_line >= 0.0 && st->on_line <= DYN_SEQ_SCATTER);
    st->done = true;
    rx_debug("beats: section %d: drift %.1f ms, bend %.1f ms (scatter would bend %.1f), "
             "jitter %.1f ms, on the line %.2f ms%s%s\n", si, st->drift * 1e3, st->bend * 1e3,
             st->bend_noise * 1e3, s->jitter * 1e3, st->on_line * 1e3,
             st->bent ? ", bent" : st->drift > DYN_DRIFT ? ", drifts" : "",
             st->live ? "" : " (sequenced)");
    return 0;
}

/* Which beats of section si keep their measured times, from its strain
 * and its neighbours': everything when bent; otherwise each run of beats
 * off the line by more than DYN_EDGE that reaches DYN_DRIFT somewhere,
 * when the section's attacks say live playing or the run meets a dynamic
 * neighbour (a ramp's edge in a sequenced track), and the run is neither
 * a plateau nor ragged. Then the straight remnants too short to stand
 * join. Returns how many beats are dynamic, or -ENOMEM. */
static int strain_classify(const Ctx *c, int si, Strain *all)
{
    const RXFitSection *s = &c->secs[si];
    Strain *st = &all[si];
    int n = s->last - s->first + 1;
    if (!st->done)
        return 0;
    const RXBeatObs *tr = st->tr;
    const double *dev = st->dev;
    bool *dyn = st->dyn;
    double *tmp = malloc((size_t)n * sizeof(*tmp));
    if (!tmp)
        return RX_ERROR(ENOMEM);
    memset(dyn, 0, (size_t)n * sizeof(*dyn));
    bool left = si > 0 && all[si - 1].done && all[si - 1].dyn[c->secs[si - 1].last -
                                                              c->secs[si - 1].first];
    bool right = si + 1 < c->nb_secs && all[si + 1].done &&
                 (all[si + 1].bent || all[si + 1].dyn[0]);
    if (st->bent) {
        for (int i = 0; i < n; i++)
            dyn[i] = true;
    } else if (st->drift > DYN_DRIFT) {
        int i = 0;
        while (i < n) {
            if (isnan(dev[i]) || fabs(dev[i]) <= DYN_EDGE) {
                i++;
                continue;
            }
            int j = i;
            bool far = false;
            int last_off = i;
            while (j < n && (isnan(dev[j]) || fabs(dev[j]) > DYN_EDGE)) {
                if (!isnan(dev[j])) {
                    far = far || fabs(dev[j]) > DYN_DRIFT;
                    last_off = j;
                }
                j++;
            }
            /* A sequenced section's run counts only at a dynamic
             * neighbour's edge: a ramp the change-point search cut
             * where it was still within a line's reach. */
            bool edge = (i == 0 && left) || (j == n && right);
            if (!far || (!st->live && !edge)) {
                i = j;
                continue;
            }
            /* Drift, or another element? The run's deviation: its
             * longest stretch at one level. */
            int m = 0;
            for (int k = i; k <= last_off; k++) {
                if (!isnan(dev[k]))
                    tmp[m++] = dev[k];
            }
            double tol = fmax(DYN_PLATEAU_TOL, s->scatter);
            int longest = 0;
            for (int a = 0; a < m; a++) {
                double lo = tmp[a], hi = tmp[a];
                int z = a;
                while (z + 1 < m) {
                    double nlo = fmin(lo, tmp[z + 1]), nhi = fmax(hi, tmp[z + 1]);
                    if (nhi - nlo > tol)
                        break;
                    lo = nlo;
                    hi = nhi;
                    z++;
                }
                longest = RX_MAX(longest, z - a + 1);
            }
            bool plateau = longest >= RX_MAX(DYN_PLATEAU_MIN, (int)(DYN_PLATEAU_SHARE * m));
            /* And its attacks' own beat-to-beat scatter. */
            bool ragged = false;
            m = 0;
            for (int k = i + 1; k < last_off; k++) {
                const RXBeatObs *a = &tr[k - 1], *b = &tr[k], *z = &tr[k + 1];
                if (!(a->weight > 0.0) || !(b->weight > 0.0) || !(z->weight > 0.0) ||
                    isnan(a->attack) || isnan(b->attack) || isnan(z->attack))
                    continue;
                if (b->index - a->index != 1 || z->index - b->index != 1)
                    continue;
                tmp[m++] = fabs(z->attack - 2.0 * b->attack + a->attack);
            }
            double sc = 0.0;
            if (m >= 4) {
                qsort(tmp, (size_t)m, sizeof(*tmp), cmp_dbl);
                sc = 1.4826 * tmp[m / 2] / sqrt(6.0);
                double base = s->scatter >= 0.0 ? s->scatter : s->jitter;
                ragged = sc > fmax(DYN_RUN_SCATTER_MIN, DYN_RUN_SCATTER * base);
            }
            rx_debug("beats:   section %d run %d-%d: scatter %.1f ms%s%s%s\n", si, i, last_off,
                     sc * 1e3, plateau ? ", a plateau" : "", ragged ? ", ragged" : "",
                     edge ? ", at a dynamic neighbour" : "");
            if (!plateau && !ragged) {
                for (int k = RX_MAX(i - DYN_MARGIN, 0); k <= RX_MIN(last_off + DYN_MARGIN, n - 1);
                     k++)
                    dyn[k] = true;
            }
            i = j;
        }
    }
    free(tmp);

    /* Straight remnants too short for a section of their own. */
    int i = 0;
    while (i < n) {
        if (dyn[i]) {
            i++;
            continue;
        }
        int j = i;
        while (j < n && !dyn[j])
            j++;
        int beats_span = tr[j - 1].index - tr[i].index + 1;
        bool between = i > 0 && j < n;
        if (beats_span < (between ? 2 * DYN_MIN_STRAIGHT : DYN_MIN_STRAIGHT) && (i > 0 || j < n)) {
            for (int k = i; k < j; k++)
                dyn[k] = true;
        }
        i = j;
    }
    int nb_dyn = 0;
    for (i = 0; i < n; i++)
        nb_dyn += dyn[i];
    return nb_dyn;
}

/* Splits section si into its straight and dynamic pieces (the strain's
 * classification), re-tracks each dynamic piece at a period that follows
 * the music, puts it on its attacks and walks it. A straight remnant
 * keeps the parent's line, which held it within 5 ms (and is snapped and
 * on the fold). Returns the number of pieces; the sections after move.
 * *again when a stretch lost some of its windows: the strain's
 * classification has lost their beats, and the caller splits the section
 * anew from where it was. */
static int strain_apply(Ctx *c, int si, Strain *st, bool *again)
{
    RXFitSection *s = &c->secs[si];
    int n = s->last - s->first + 1;
    const bool *dyn = st->dyn;
    int nb_pieces = 1;
    for (int i = 1; i < n; i++)
        nb_pieces += dyn[i] != dyn[i - 1];
    RXFitSection *secs = realloc(c->secs, (size_t)(c->nb_secs + nb_pieces - 1) * sizeof(*secs));
    if (!secs)
        return RX_ERROR(ENOMEM);
    c->secs = secs;
    s = &c->secs[si];
    int *bands = realloc(c->bands, (size_t)(c->nb_secs + nb_pieces - 1) * sizeof(*bands));
    if (!bands)
        return RX_ERROR(ENOMEM);
    c->bands = bands;
    memmove(c->secs + si + nb_pieces, c->secs + si + 1,
            (size_t)(c->nb_secs - si - 1) * sizeof(*c->secs));
    memmove(c->bands + si + nb_pieces, c->bands + si + 1,
            (size_t)(c->nb_secs - si - 1) * sizeof(*c->bands));
    RXFitSection parent = *s;
    int band = c->bands[si];
    c->nb_secs += nb_pieces - 1;
    /* The parent's observations and envelopes as they were, before any
     * piece is touched: a stretch the line wins goes back to them, bit
     * for bit (the attacks its line was put on, the envelopes not cut
     * again). Each piece remembers its offsets in them. */
    const size_t len = (size_t)RX_NB_BANDS * RX_ENV_LEN;
    RXBeatObs *orig = malloc((size_t)n * sizeof(*orig));
    float *orig_env = malloc((size_t)n * len * sizeof(*orig_env));
    double *orig_centre = malloc((size_t)n * sizeof(*orig_centre));
    int *piece_off = malloc((size_t)nb_pieces * sizeof(*piece_off));
    if (!orig || !orig_env || !orig_centre || !piece_off) {
        free(orig);
        free(orig_env);
        free(orig_centre);
        free(piece_off);
        return RX_ERROR(ENOMEM);
    }
    memcpy(orig, c->beats + parent.first, (size_t)n * sizeof(*orig));
    memcpy(orig_env, c->envs.env + (size_t)parent.first * len, (size_t)n * len * sizeof(*orig_env));
    memcpy(orig_centre, c->envs.centre + parent.first, (size_t)n * sizeof(*orig_centre));
    int pi = 0, i = 0;
    while (i < n) {
        int j = i;
        while (j < n && dyn[j] == dyn[i])
            j++;
        RXFitSection *q = &c->secs[si + pi];
        *q = parent;
        q->first = parent.first + i;
        q->last = parent.first + j - 1;
        q->dynamic = dyn[i];
        q->joined = 0;
        piece_off[pi] = i;
        c->bands[si + pi] = band;
        q->supported = 0;
        for (int k = i; k < j; k++) {
            if (dyn[i]) {
                c->beats[parent.first + k].attack = st->tr[k].attack;
                c->beats[parent.first + k].weight = st->tr[k].weight;
            }
            q->supported += c->beats[parent.first + k].weight > 0.0 &&
                            !isnan(c->beats[parent.first + k].attack);
        }
        pi++;
        i = j;
    }

    /* The dynamic pieces: re-tracked at a period that follows the music,
     * then the attacks, the walk, the line and the spread. A count found
     * different from the tracker's moves the sections after. Then the
     * judgment, window by window (judge_stretch): where the walked beats
     * do not follow the hits better than the line, the tracker's beats
     * come back and the line stays. */
    int ret = 0;
    for (int k = 0; k < nb_pieces; k++) {
        RXFitSection *q = &c->secs[si + k];
        if (!q->dynamic)
            continue;
        int n_old = q->last - q->first + 1;
        const RXBeatObs *saved = orig + piece_off[k];
        const float *saved_env = orig_env + (size_t)piece_off[k] * len;
        const double *saved_centre = orig_centre + piece_off[k];
        double *tm = NULL;
        int nt = 0, idx0 = 0;
        double next = NAN;
        if (si + k + 1 < c->nb_secs) {
            const RXFitSection *z = &c->secs[si + k + 1];
            next = z->dynamic ? c->beats[z->first].time : rx_beat_predict(c->beats, z->first, z);
        }
        ret = retrack_stretch(c, q, next, &tm, &nt, &idx0);
        if (ret == RX_ERROR(EINVAL)) {
            q->dynamic = false;
            ret = 0;
            continue;
        }
        if (ret < 0)
            goto end;
        rx_debug("beats: section %d re-tracked: %d beats for %d (count %d for %d)\n", si + k, nt,
                 n_old, idx0, saved[0].index);
        ret = replace_beats(c, si + k, tm, NULL, NULL, NULL, nt, idx0);
        free(tm);
        if (ret < 0)
            goto end;
        q = &c->secs[si + k];
        RXFitSection line = parent;
        line.first = q->first;
        line.last = q->last;
        bool keep = false;
        ret = refine_dynamic(c, q, c->bands[si + k]);
        if (ret == RX_ERROR(ENOMEM))
            goto end;
        if (ret >= 0) {
            /* The straight remnants beside it (the pieces alternate). */
            const RXFitSection *lo = k > 0 ? &c->secs[si + k - 1] : NULL;
            const RXFitSection *hi = k + 1 < nb_pieces ? &c->secs[si + k + 1] : NULL;
            ret = judge_stretch(c, si + k, &line, lo, hi, saved, n_old,
                                st->dyn + piece_off[k], &keep, again);
            if (ret < 0)
                goto end;
        }
        if (!keep) {
            /* The tracker's beats, their envelopes and the line as they
             * were. */
            ret = replace_beats(c, si + k, NULL, saved, saved_env, saved_centre, n_old, 0);
            if (ret < 0)
                goto end;
            q = &c->secs[si + k];
            *q = line;
            q->last = q->first + n_old - 1;
            q->dynamic = false;
            q->supported = 0;
            for (int j = q->first; j <= q->last; j++)
                q->supported += c->beats[j].weight > 0.0 && !isnan(c->beats[j].attack);
            continue;
        }
        q->joined = (k > 0 ? 1u : 0u) | (k + 1 < nb_pieces ? 2u : 0u);
    }
    ret = 0;

    /* Straight pieces side by side (a stretch judged for the line between
     * two remnants, or at one) share the parent's line and are one
     * section again, as they were: split, they would vote on their
     * downbeat and their half of the beat apart. */
    for (int k = 1; k < nb_pieces;) {
        RXFitSection *a = &c->secs[si + k - 1], *b = &c->secs[si + k];
        if (a->dynamic || b->dynamic) {
            k++;
            continue;
        }
        a->last = b->last;
        a->supported += b->supported;
        memmove(c->secs + si + k, c->secs + si + k + 1,
                (size_t)(c->nb_secs - si - k - 1) * sizeof(*c->secs));
        memmove(c->bands + si + k, c->bands + si + k + 1,
                (size_t)(c->nb_secs - si - k - 1) * sizeof(*c->bands));
        c->nb_secs--;
        nb_pieces--;
    }
end:
    free(orig);
    free(orig_env);
    free(orig_centre);
    free(piece_off);
    return ret < 0 ? ret : nb_pieces;
}

/* What the dynamic pass changes (the beats, their envelopes, the
 * sections), kept to split a section anew from. */
typedef struct Snapshot {
    RXBeatObs    *beats;
    float        *env;
    double       *centre;
    RXFitSection *secs;
    int          *bands;
    int           nb_beats, nb_secs;
} Snapshot;

static void snapshot_free(Snapshot *s)
{
    free(s->beats);
    free(s->env);
    free(s->centre);
    free(s->secs);
    free(s->bands);
    memset(s, 0, sizeof(*s));
}

static int snapshot_copy(Snapshot *d, const Snapshot *s)
{
    const size_t len = (size_t)RX_NB_BANDS * RX_ENV_LEN;
    const size_t nb = (size_t)RX_MAX(s->nb_beats, 1), ns = (size_t)RX_MAX(s->nb_secs, 1);
    *d = (Snapshot) {
        .beats    = malloc(nb * sizeof(*d->beats)),
        .env      = malloc(nb * len * sizeof(*d->env)),
        .centre   = malloc(nb * sizeof(*d->centre)),
        .secs     = malloc(ns * sizeof(*d->secs)),
        .bands    = malloc(ns * sizeof(*d->bands)),
        .nb_beats = s->nb_beats,
        .nb_secs  = s->nb_secs,
    };
    if (!d->beats || !d->env || !d->centre || !d->secs || !d->bands) {
        snapshot_free(d);
        return RX_ERROR(ENOMEM);
    }
    memcpy(d->beats, s->beats, (size_t)s->nb_beats * sizeof(*d->beats));
    memcpy(d->env, s->env, (size_t)s->nb_beats * len * sizeof(*d->env));
    memcpy(d->centre, s->centre, (size_t)s->nb_beats * sizeof(*d->centre));
    memcpy(d->secs, s->secs, (size_t)s->nb_secs * sizeof(*d->secs));
    memcpy(d->bands, s->bands, (size_t)s->nb_secs * sizeof(*d->bands));
    return 0;
}

static int snapshot_take(const Ctx *c, Snapshot *s)
{
    const Snapshot now = {
        .beats    = c->beats,
        .env      = c->envs.env,
        .centre   = c->envs.centre,
        .secs     = c->secs,
        .bands    = c->bands,
        .nb_beats = c->nb_beats,
        .nb_secs  = c->nb_secs,
    };
    return snapshot_copy(s, &now);
}

/* Puts a copy of the snapshot's state back in c. */
static int snapshot_put(Ctx *c, const Snapshot *s)
{
    Snapshot t;
    int ret = snapshot_copy(&t, s);
    if (ret < 0)
        return ret;
    free(c->beats);
    free(c->envs.env);
    free(c->envs.centre);
    free(c->secs);
    free(c->bands);
    c->beats = t.beats;
    c->nb_beats = t.nb_beats;
    c->envs.env = t.env;
    c->envs.centre = t.centre;
    c->envs.nb = t.nb_beats;
    c->secs = t.secs;
    c->bands = t.bands;
    c->nb_secs = t.nb_secs;
    return 0;
}

/* Which sections keep their measured beats (docs/BEATS.md, "Dynamic
 * grids"): the strain of every section's line first, then each section
 * classified with its neighbours known (a ramp's edge continues into the
 * sequenced section beside it), the edges settled in a second sweep, and
 * each section split, its dynamic stretches re-tracked and walked. */
static int dynamic_pass(Ctx *c)
{
    if (!c->opt->allow_dynamic)
        return 0;
    int nb = c->nb_secs;
    Strain *st = calloc((size_t)RX_MAX(nb, 1), sizeof(*st));
    if (!st)
        return RX_ERROR(ENOMEM);
    int ret = 0;
    for (int si = 0; si < nb && ret >= 0; si++)
        ret = strain_read(c, si, &st[si]);
    if (ret < 0) {
        strain_free(st, nb);
        return ret;
    }
    for (int sweep = 0; sweep < 2 && ret >= 0; sweep++) {
        for (int si = 0; si < nb && ret >= 0; si++)
            ret = strain_classify(c, si, st);
    }
    if (ret < 0) {
        strain_free(st, nb);
        return ret;
    }
    /* Splitting moves the sections after: from the last backwards, so
     * that every index still names the section its strain was read on. */
    for (int si = nb - 1; si >= 0 && ret >= 0; si--) {
        int nb_dyn = 0;
        int n = c->secs[si].last - c->secs[si].first + 1;
        for (int i = 0; st[si].done && i < n; i++)
            nb_dyn += st[si].dyn[i];
        if (!nb_dyn)
            continue;
        /* A stretch that lost some of its windows has given them back
         * to the line: the section is split again from the state
         * before, until no stretch loses a window (each round takes
         * beats from the classification, so it ends). */
        Snapshot snap = { 0 };
        if ((ret = snapshot_take(c, &snap)) < 0)
            break;
        while (1) {
            bool again = false;
            ret = strain_apply(c, si, &st[si], &again);
            if (ret < 0 || !again)
                break;
            if ((ret = snapshot_put(c, &snap)) < 0)
                break;
            nb_dyn = 0;
            for (int i = 0; i < n; i++)
                nb_dyn += st[si].dyn[i];
            if (!nb_dyn)
                break;
        }
        snapshot_free(&snap);
    }
    strain_free(st, nb);
    return ret < 0 ? ret : 0;
}

/* The confidence, as dj's: the share of 16-beat windows (every 8 beats)
 * whose average beat starts its rise within 15 ms of the grid, weighted
 * by how clearly it rises. Into *out; 0 or -ENOMEM (a confidence of 0
 * would flag the grid low-confidence). */
static int grid_confidence(const Ctx *c, const int *bands, double *out)
{
    double num = 0.0, den = 0.0;
    double *contrast = NULL, *offs = NULL;
    int nw = 0, cap = 0;
    *out = 0.0;
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
                    return RX_ERROR(ENOMEM);
                }
                contrast = nc;
                offs = no;
            }
            rx_fold(&c->envs, c->beats, a, b, s->dynamic ? NULL : s, bands[si], search,
                    &contrast[nw], &offs[nw]);
            nw++;
        }
    }
    if (!nw) {
        free(contrast);
        free(offs);
        return 0;
    }
    double *tmp = malloc(nw * sizeof(*tmp));
    if (!tmp) {
        free(contrast);
        free(offs);
        return RX_ERROR(ENOMEM);
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
        return 0;
    }
    qsort(tmp, (size_t)nt, sizeof(*tmp), cmp_dbl);
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
    *out = den > 0.0 ? num / den : 0.0;
    return 0;
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
                                  const RXMusicStart *ms, double off, double kick_beats)
{
    StartWitness w = { .at = ms->start };
    if ((ms->kind != RX_START_HIT && ms->kind != RX_START_FADE) || w.at < si->start ||
        w.at >= si->end)
        return w;
    /* A fade: the grid's first beat, the first at or after it, is beat 1
     * unless the structure says otherwise by more than half a scale. The
     * first loud onset keeps its vote (where the drums come in after the
     * fade), and the fade says nothing about the half of the beat. */
    if (ms->kind == RX_START_FADE) {
        w.counts = true;
        w.onset_too = true;
        w.down = FADE_DOWNBEAT;
        return w;
    }
    const double P = s->period;
    if (fabs(off) > fmin(START_ON_BEAT * P, START_ON_BEAT_MAX))
        return w;
    /* The kick band from 15 ms before the hit to 60 ms after it (a kick's
     * body builds after its click), against its level on the beats or the
     * half beats, whichever carry the kicks. A stab or a vocal on the "and"
     * before bar 1 has no kick; the hit that opens a track on beat 1
     * usually has one. */
    int a = (int)lrint((w.at - 0.015) * c->ons.fps), b = (int)lrint((w.at + 0.060) * c->ons.fps);
    double m = 0.0;
    for (int f = RX_MAX(a, 0); f <= RX_MIN(b, c->ons.nb - 1); f++)
        m = fmax(m, c->ons.low[f]);
    w.kick = kick_beats > 0.0 ? m / kick_beats : 0.0;
    /* Where the kick band clearly marks one half of the beat, a hit with
     * a kick half a beat from it is a pickup (a kick and a crash on the
     * "and" before bar 1); where it does not (a two-step kick on the "and"s
     * as often as on the beats), the opening hit is the better witness. */
    /* After a fade (a pad, a riser) the hit is the first loud thing at
     * all, the drop, and that is where a DJ expects the track to start
     * whatever it is made of (pushover's "PASSIVE": a loud part, not a
     * kick); it counts without a kick. After silence a kick tells the
     * opening hit from a pickup. */
    bool after_fade = ms->intro < ms->start;
    if ((after_fade || w.kick >= HIT_KICK) && fabs(log(si->kick_ratio)) < HIT_KICKS_CLEAR) {
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
 * breaks a tie for beat 1. Everywhere else (the music starts at once, a
 * swell, any other hit, a start between the line's half beats), and after
 * a fade, dj's first loud onset votes in the structure as one scale, as it
 * always did. */
static int decide_phase(Ctx *c, int i, SecInfo *si, const float *arbiter, const RXMusicStart *ms,
                        double first_onset, int *band, RXBeatDebug *dbg)
{
    RXFitSection *s = &c->secs[i];
    const double P = s->period;
    const double eps = 0.1;
    double on = arbiter_support(c, arbiter, s, si->start, si->end, 0.0);
    double off = arbiter_support(c, arbiter, s, si->start, si->end, P / 2);
    si->kick_ratio = (on + eps) / (off + eps);

    double woff = 0.0;
    half_beat(s, ms->start, &woff);
    double kick_beats = fmax(arbiter_support(c, c->ons.low, s, si->start, si->end, 0.0),
                             arbiter_support(c, c->ons.low, s, si->start, si->end, P / 2));
    StartWitness w = start_witness(c, s, si, ms, woff, kick_beats);
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
        int *idx = malloc((size_t)(s->last - s->first + 1) * sizeof(*idx));
        if (!idx)
            return RX_ERROR(ENOMEM);
        for (int k = s->first; k <= s->last; k++) {
            c->beats[k].time = rx_beat_predict(c->beats, k, s);
            idx[k - s->first] = k;
        }
        int ret = cut_envs(c, idx, s->last - s->first + 1);
        free(idx);
        if (ret < 0)
            return ret;
        double contrast;
        if ((ret = refine_section(c, s, &contrast)) < 0)
            return ret;
        *band = ret;
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
    if (w.counts && ms->kind == RX_START_FADE) {
        long k = (long)ceil((w.at - START_SLACK - s->t0) / P - 1e-9);
        si->db.scores[position(2 * k)] += w.down;
        voted = true;
    } else if (w.counts) {
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

/* The same for a dynamic section, on its beats' own times. The half
 * beats are the beats and the midpoints after them (the last one towards
 * the next section's first beat, or a beat's length on); position 2j is
 * beat j of the bar, counted from the section's index0 as on a line, so
 * the downbeat is read back the same way. A shift by half a beat puts
 * every beat on the midpoint after it, cuts the envelopes there, measures
 * the attacks and walks again. */

/* Mean of the arbiter onset over [t - 30 ms, t + 10 ms] at the times t. */
static double support_at(const Ctx *c, const float *v, const double *t, int n)
{
    double acc = 0.0;
    for (int j = 0; j < n; j++) {
        int a = (int)lrint((t[j] - 0.030) * c->ons.fps), b = (int)lrint((t[j] + 0.010) * c->ons.fps);
        double m = 0.0;
        for (int i = RX_MAX(a, 0); i <= RX_MIN(b, c->ons.nb - 1); i++)
            m = fmax(m, v[i]);
        acc += m;
    }
    return n ? acc / n : 0.0;
}

/* The 2n + 1 half-beat edges of dynamic section i: beat, midpoint, beat,
 * ... up to the beat after its last. */
static int section_edges(const Ctx *c, int i, double **out)
{
    const RXFitSection *s = &c->secs[i];
    int n = s->last - s->first + 1;
    double *e = malloc((size_t)(2 * n + 1) * sizeof(*e));
    if (!e)
        return RX_ERROR(ENOMEM);
    double next;
    if (i + 1 < c->nb_secs) {
        const RXFitSection *q = &c->secs[i + 1];
        next = q->dynamic ? c->beats[q->first].time : rx_beat_predict(c->beats, q->first, q);
    } else {
        next = c->beats[s->last].time +
               (n > 1 ? c->beats[s->last].time - c->beats[s->last - 1].time : s->period);
    }
    for (int k = 0; k < n; k++) {
        double t = c->beats[s->first + k].time;
        double t1 = k + 1 < n ? c->beats[s->first + k + 1].time : next;
        e[2 * k] = t;
        e[2 * k + 1] = t + 0.5 * (t1 - t);
    }
    e[2 * n] = next;
    *out = e;
    return 2 * n + 1;
}

/* The edge nearest to t (index into edges), and t's distance from it. */
static int nearest_edge(const double *e, int ne, double t, double *off)
{
    int lo = 0, hi = ne - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (e[mid] < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    int best = lo;
    if (lo > 0 && fabs(e[lo - 1] - t) < fabs(e[lo] - t))
        best = lo - 1;
    *off = t - e[best];
    return best;
}

/* decide_phase for a dynamic section, on its beats' own times. With
 * `follow` 0 or 1 the half of the beat is not its own to decide: it
 * stays on its beats or moves to the midpoints after them, as the
 * straight pieces of its line beside it did (the evidence is still read,
 * for the flags and the downbeat). */
static int decide_phase_dynamic(Ctx *c, int i, SecInfo *si, const float *arbiter,
                                const RXMusicStart *ms, double first_onset, int follow, int *band,
                                RXBeatDebug *dbg)
{
    RXFitSection *s = &c->secs[i];
    const double P = s->period;
    const double eps = 0.1;
    int n = s->last - s->first + 1;
    double *e = NULL;
    int ne = section_edges(c, i, &e);
    if (ne < 0)
        return ne;
    int pos0 = (int)(((2L * (c->beats[s->first].index - s->index0)) % 8 + 8) % 8);

    double *on_t = malloc((size_t)n * sizeof(*on_t)), *off_t = malloc((size_t)n * sizeof(*off_t));
    if (!on_t || !off_t) {
        free(e);
        free(on_t);
        free(off_t);
        return RX_ERROR(ENOMEM);
    }
    for (int k = 0; k < n; k++) {
        on_t[k] = e[2 * k];
        off_t[k] = e[2 * k + 1];
    }
    double on = support_at(c, arbiter, on_t, n);
    double off = support_at(c, arbiter, off_t, n);
    si->kick_ratio = (on + eps) / (off + eps);

    double woff = 0.0;
    int wedge = nearest_edge(e, ne, ms->start, &woff);
    double kick_beats = fmax(support_at(c, c->ons.low, on_t, n), support_at(c, c->ons.low, off_t, n));
    StartWitness w = start_witness(c, s, si, ms, woff, kick_beats);
    si->has_vote = n >= 32;
    double sv = 0.0;
    int ret = 0;
    if (si->has_vote) {
        double onset = w.counts && !w.onset_too ? NAN : first_onset;
        int start_pos = -1;
        if (!isnan(onset)) {
            double o;
            int k = nearest_edge(e, ne, onset, &o);
            double hp = k + 1 < ne ? e[k + 1] - e[k] : e[k] - e[k - 1];
            if (fabs(o) < 0.2 * hp && onset >= si->start - P && onset < si->end)
                start_pos = (pos0 + k) % 8;
        }
        ret = rx_downbeat_vote_times(&c->spec, c->x, c->n, e, e + 1, ne - 1, pos0, on_t, n, pos0,
                                     start_pos, &si->db);
        if (ret < 0)
            goto end;
        double be = 0.0, bo = 0.0;
        for (int p = 0; p < 8; p++) {
            if (p & 1)
                bo = fmax(bo, si->db.scores[p]);
            else
                be = fmax(be, si->db.scores[p]);
        }
        sv = log((be + 0.05) / (bo + 0.05));
    }

    double hoff = woff;
    long hj = wedge;
    double hv = w.counts ? (hj & 1 ? -w.phase : w.phase) : 0.0;
    bool here = (ms->kind == RX_START_HIT || ms->kind == RX_START_FADE) && w.at >= si->start &&
                w.at < si->end;
    double ev = log(si->kick_ratio) + 1.5 * sv + hv;
    si->phase_evidence = ev;
    rx_debug("beats: section %d (dynamic) phase: arbiter %.3f on, %.3f off (log %.3f); structure "
             "%.3f; start %+.2f (edge %ld %+.1f ms); evidence %.3f%s\n", i, on, off,
             log(si->kick_ratio), sv, hv, hj, hoff * 1e3, ev,
             follow < 0 ? "" : follow ? "; moves with its line" : "; stays with its line");
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
    if (follow >= 0 ? follow > 0 : ev < -log(1.25)) {
        int *idx = malloc((size_t)n * sizeof(*idx));
        if (!idx) {
            ret = RX_ERROR(ENOMEM);
            goto end;
        }
        for (int k = 0; k < n; k++) {
            c->beats[s->first + k].time = off_t[k];
            idx[k] = s->first + k;
        }
        ret = cut_envs(c, idx, n);
        free(idx);
        if (ret < 0)
            goto end;
        s->t0 += P / 2;
        if ((ret = refine_dynamic(c, s, *band)) < 0) {
            if (ret == RX_ERROR(ENOMEM))
                goto end;
            /* Nothing to walk on the other half: back where it was. */
            for (int k = 0; k < n; k++)
                c->beats[s->first + k].time = on_t[k];
            s->t0 -= P / 2;
            ret = 0;
        } else {
            si->shifted = true;
            si->phase_evidence = -ev;
            si->kick_ratio = 1.0 / si->kick_ratio;
            if (si->has_vote) {
                double rot[8];
                for (int p = 0; p < 8; p++)
                    rot[p] = si->db.scores[(p + 1) % 8];
                memcpy(si->db.scores, rot, sizeof(rot));
            }
            free(e);
            ne = section_edges(c, i, &e);
            if (ne < 0) {
                ret = ne;
                e = NULL;
                goto end;
            }
        }
    }

    /* The start's vote for beat 1, on the beats as they now are. */
    bool voted = false;
    if (w.counts && ms->kind == RX_START_FADE) {
        int k = 0;
        while (k + 1 < n && c->beats[s->first + k].time < w.at - START_SLACK)
            k++;
        si->db.scores[(pos0 + 2 * k) % 8] += w.down;
        voted = true;
    } else if (w.counts) {
        double o;
        int k = nearest_edge(e, ne, w.at, &o);
        if (!(k & 1)) {
            si->db.scores[(pos0 + k) % 8] += w.down;
            voted = true;
        }
    }
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

end:
    free(e);
    free(on_t);
    free(off_t);
    return ret;
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

/* How well a grid's beats land on hits: the mean of the onset v (the
 * kick band, or the full band) around every beat of every section. */
static double grid_support(const Ctx *c, const float *v)
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
                m = fmax(m, v[f]);
            acc += m;
            n++;
        }
    }
    return n ? acc / n : 0.0;
}

/* The accent the tracker follows (fit_grid): the full band plus the kick
 * band in proportion to how much of the beat's periodicity it carries. */
static void set_accent(Ctx *c)
{
    float wl = (float)RX_CLAMP(c->kick_share, 0.0, 1.0);
    for (int i = 0; i < c->ons.nb; i++)
        c->ons.accent[i] = c->ons.full[i] + wl * c->ons.low[i];
}

/* A fitted grid, moved out of the context while another is tried: all
 * that fit_grid leaves behind for the stages after it. The dynamic pass
 * tracks stretches again on the accent and moves the tracker's frames
 * by the fold's delay, so a grid put back takes its own (a runner-up
 * family's, up to 25 ms another way, once followed it). */
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
    double        onset_shift;
} Fit;

static Fit fit_take(Ctx *c)
{
    Fit f = {
        .beats       = c->beats,
        .nb_beats    = c->nb_beats,
        .secs        = c->secs,
        .nb_secs     = c->nb_secs,
        .bands       = c->bands,
        .envs        = c->envs,
        .period      = c->period,
        .kick_share  = c->kick_share,
        .arbiter     = c->arbiter,
        .onset_shift = c->onset_shift,
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
    c->onset_shift = f->onset_shift;
    set_accent(c);
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
    double *centre = malloc((size_t)RX_MAX(e->nb, 1) * sizeof(*centre));
    int ret = RX_ERROR(ENOMEM);
    if (e->env && centre)
        ret = rx_env_windows(c->envctx, e->centre, e->nb, e->env, centre);
    for (int i = 0; ret >= 0 && i < e->nb; i++) {
        if (centre[i] != e->centre[i])
            ret = RX_ERROR(EINVAL);
    }
    free(centre);
    return ret < 0 ? ret : 0;
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
    set_accent(c);

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
    c->onset_shift = shift;
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
        if ((ret = refine_section(c, &c->secs[i], &contrast)) < 0)
            goto end;
        c->bands[i] = ret;
        ret = 0;
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

/* The whole analysis; `pre`, when given, is the spectrum of the 44.1 kHz
 * input, taken over. */
static int analyse(const float *mono, int64_t nb_samples, int rate, RXSpectrum *pre,
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
    if (!mono || nb_samples < 0 || rate <= 0 || (pre && rate != RX_BEAT_RATE)) {
        if (pre)
            rx_spectrum_free(pre);
        return RX_ERROR(EINVAL);
    }

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
        if (pre)
            rx_spectrum_free(pre);
        return 0;
    }

    int64_t tm = rx_time_us(), tm0 = tm;
    if (pre) {
        c.spec = *pre;
        memset(pre, 0, sizeof(*pre));
    } else if ((ret = rx_spectrum_compute(c.x, c.n, c.opt->compute, &c.spec)) < 0) {
        goto fail;
    }
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
    if (ret == RX_ERROR(ENOMEM))
        goto fail;
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

    if ((ret = rx_env_alloc(&c.envctx, c.x, c.n, c.opt->compute)) < 0)
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
     * nothing happens. It must beat the first by 10 %, both read on one
     * band: the kick band when either grid's tempo has kicks carrying it
     * (its arbiter), else the full band. Each fit's own arbiter compared
     * unlike quantities where the kick share crosses a half between the
     * two tempos; the kicks mark the beat where there are any, and the
     * full band, hats on every subdivision, favours neither grid. */
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
        double s1_low = grid_support(&c, c.ons.low), s1_full = grid_support(&c, c.ons.full);
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
        bool kicks = first.arbiter == c.ons.low || (r2 == 0 && c.arbiter == c.ons.low);
        const float *band = kicks ? c.ons.low : c.ons.full;
        double s1 = kicks ? s1_low : s1_full;
        double s2 = r2 == 0 ? grid_support(&c, band) : 0.0;
        rx_debug("beats: family %.3f support %.3f vs %.3f support %.3f (the %s band; kick "
                 "shares %.2f, %.2f)\n", bpm0, s1, alt, s2, kicks ? "kick" : "full",
                 first.kick_share, r2 == 0 ? c.kick_share : 0.0);
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
     * tracker then follows the right beats). Within the requested range.
     * A refit with no grid to be had (a loop of a few seconds has 16 beats
     * at 174 but not at 87) keeps the grid it had, its octave uncertain:
     * the other level's evidence stands against it. */
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
            Fit working = fit_take(&c);
            /* Its envelopes go while the other is fitted, as above. */
            free(working.envs.env);
            working.envs.env = NULL;
            ret = fit_grid(&c, bpm0 * od.factor, lo, hi);
            stage(&tm, "octave");
            if (ret > 0) {
                rx_debug("beats: no grid at %.3f BPM: the grid stays at %.3f\n",
                         bpm0 * od.factor, bpm0);
                fit_put(&c, &working);
                if ((ret = recut_envs(&c)) < 0)
                    goto fail;
                /* The grid's octave against the one that beat it: under
                 * 1, and so flagged octave-uncertain. */
                out->octave_margin = 1.0 / od.margin;
            } else {
                fit_free(&working);
                if (ret < 0)
                    goto fail;
            }
        }
    }
    /* Which sections keep their measured beats. */
    if ((ret = dynamic_pass(&c)) < 0)
        goto fail;
    stage(&tm, "dynamic");
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
        const RXFitSection *s = &c.secs[i];
        sinfo[i].start = !i ? 0.0 : s->dynamic ? c.beats[s->first].time
                                               : rx_beat_predict(c.beats, s->first, s);
        sinfo[i].end = c.duration;
        if (i)
            sinfo[i - 1].end = sinfo[i].start;
    }
    if (dbg)
        dbg->start = ms;
    /* The straight sections first: a dynamic stretch cut from a line and
     * handing over to its straight pieces on the same beat takes their
     * half of the beat (the one with the stronger evidence, should the
     * two disagree), not one of its own, which would put the joins half
     * a beat apart (two beats a flam apart there, or a gap). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < c.nb_secs; i++) {
            const RXFitSection *s = &c.secs[i];
            if (s->dynamic != (pass == 1))
                continue;
            if (!pass) {
                ret = decide_phase(&c, i, &sinfo[i], arbiter, &ms, first_onset, &bands[i], dbg);
            } else {
                int with = -1;
                if (s->joined & 1)
                    with = i - 1;
                if ((s->joined & 2) && (with < 0 || fabs(sinfo[i + 1].phase_evidence) >
                                                    fabs(sinfo[with].phase_evidence)))
                    with = i + 1;
                int follow = with < 0 ? -1 : sinfo[with].shifted;
                ret = decide_phase_dynamic(&c, i, &sinfo[i], arbiter, &ms, first_onset, follow,
                                           &bands[i], dbg);
            }
            if (ret < 0)
                goto fail;
        }
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
        if (a->dynamic || b->dynamic) {
            i++;
            continue;
        }
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
     * (beat 0) unless its own music says clearly otherwise. A dynamic
     * section is one measured beat per beat, the first carrying the
     * section's number, each with the tempo of its own interval (the
     * last's that of the interval to the next section's first beat, or
     * the one before it when nothing follows); a dynamic first section
     * reaches back to time 0 at its first interval, as a straight one's
     * line does, before the music's start moves the grid on. */
    out->grid.beats_per_bar = 4;
    int cap = 0;
    for (int i = 0; i < c.nb_secs; i++) {
        const RXFitSection *s = &c.secs[i];
        int n = s->last - s->first + 1;
        cap += s->dynamic ? n : 1;
        if (i == 0 && s->dynamic && n > 1) {
            /* The beats reached back to time 0 at the first interval, as
             * the build below counts them. */
            double dt = c.beats[s->first + 1].time - c.beats[s->first].time;
            cap += (dt > 0.0 ? (int)floor(c.beats[s->first].time / dt) : 0) + 2;
        }
    }
    out->grid.sections = calloc((size_t)cap, sizeof(*out->grid.sections));
    if (!out->grid.sections) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    int ng = 0;
    double prev_down = 0.0, prev_period = 0.0;
    int prev_last_number = 0;
    bool prev_dynamic = false;
    for (int i = 0; i < c.nb_secs; i++) {
        RXFitSection *s = &c.secs[i];
        SecInfo *si = &sinfo[i];
        const double P = s->period;
        if (s->dynamic) {
            int n = s->last - s->first + 1;
            const RXBeatObs *b = c.beats + s->first;
            /* The first beat's number by its own vote: the downbeat is at
             * position pos, i.e. index0 + pos / 2 and every four from it. */
            int mine = si->has_down
                     ? (int)(((b[0].index - s->index0 - si->pos / 2) % 4 + 4) % 4) + 1 : 0;
            int number, beat_field;
            if (i == 0) {
                number = mine ? mine : 1;
                beat_field = number;
            } else {
                bool continuous;
                int cont;
                if (prev_dynamic) {
                    continuous = true;
                    cont = prev_last_number % 4 + 1;
                } else {
                    double ka = (b[0].time - prev_down) / prev_period;
                    continuous = fabs(ka - round(ka)) * prev_period <= 0.015;
                    cont = (int)((lrint(ka) % 4 + 4) % 4) + 1;
                }
                if (continuous && (!mine || mine == cont || si->down_margin < 1.5)) {
                    beat_field = 0;
                    number = cont;
                } else {
                    number = mine ? mine : cont;
                    beat_field = number;
                }
            }
            double next = i + 1 < c.nb_secs
                        ? (c.secs[i + 1].dynamic ? c.beats[c.secs[i + 1].first].time
                                                 : rx_beat_predict(c.beats, c.secs[i + 1].first,
                                                                   &c.secs[i + 1]))
                        : NAN;
            if (i == 0 && n > 1) {
                /* Back to time 0 at the first interval. */
                double dt = b[1].time - b[0].time;
                int back = dt > 0.0 ? (int)floor(b[0].time / dt) : 0;
                for (int k = back; k >= 1; k--) {
                    out->grid.sections[ng++] = (RXTempoSection) {
                        .start    = b[0].time - k * dt,
                        .bpm      = 60.0 / dt,
                        .beat     = ((number - 1 - k) % 4 + 4) % 4 + 1,
                        .measured = true,
                    };
                }
                if (back > 0)
                    beat_field = 0;
            }
            for (int k = 0; k < n; k++) {
                double t1 = k + 1 < n ? b[k + 1].time : next;
                double dt = !isnan(t1) ? t1 - b[k].time
                          : n > 1 ? b[k].time - b[k - 1].time : P;
                /* A beat not after the one before it (nothing should
                 * make one; a grid must stay sorted) is dropped. */
                if (ng > 0 && !(b[k].time > out->grid.sections[ng - 1].start)) {
                    rx_warn("beats: a measured beat at %.4f s not after %.4f s, dropped", b[k].time,
                            out->grid.sections[ng - 1].start);
                    continue;
                }
                out->grid.sections[ng++] = (RXTempoSection) {
                    .start    = b[k].time,
                    .bpm      = RX_CLAMP(60.0 / fmax(dt, 1e-3), 1.0, 1000.0),
                    .beat     = k ? 0 : beat_field,
                    .measured = true,
                };
            }
            prev_dynamic = true;
            prev_last_number = (number - 1 + n - 1) % 4 + 1;
            prev_period = P;
            prev_down = b[0].time - (number - 1) * P;
            continue;
        }
        RXTempoSection *g = &out->grid.sections[ng++];
        g->bpm = 60.0 / P;
        double own = si->has_down ? s->t0 + si->pos / 2 * P : NAN;
        g->measured = false;
        if (i == 0) {
            /* From its first beat at or after 0 (the music's start moves it
             * on below), numbered from the downbeat. Not before 0, not even
             * by a few ms where the music starts at once: the players'
             * timeline has no beat there (rx_grid_beats lists none, and the
             * stick's beat times are unsigned ms), and the whole line would
             * have to move to put it at 0. */
            double d = isnan(own) ? s->t0 : own;
            long k = (long)floor(d / P);
            g->start = d - k * P;
            g->beat = (int)((-k % 4 + 4) % 4) + 1;
            prev_down = d;
            prev_period = P;
            continue;
        }
        g->start = rx_beat_predict(c.beats, s->first, s);
        /* The previous section's count at this time. */
        bool continuous;
        int cont;
        if (prev_dynamic) {
            continuous = true;
            cont = prev_last_number % 4 + 1;
        } else {
            double ka = (g->start - prev_down) / prev_period;
            continuous = fabs(ka - round(ka)) * prev_period <= 0.015;
            cont = (int)((lrint(ka) % 4 + 4) % 4) + 1;
        }
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
        prev_period = P;
        prev_dynamic = false;
        /* A straight section's start must follow the beat before it too
         * (a walked beat can end up past the line's first beat). */
        if (ng > 1 && !(g->start > out->grid.sections[ng - 2].start)) {
            rx_warn("beats: a section at %.4f s not after the beat at %.4f s, dropped", g->start,
                    out->grid.sections[ng - 2].start);
            ng--;
        }
    }
    out->grid.nb_sections = ng;
    /* No grid in the silence before the music: the first beat is the first
     * at or after its start, every later beat as it was. */
    if (ms.kind != RX_START_AT_ONCE &&
        (ret = rx_grid_start_at(&out->grid, fmax(ms.start - START_SLACK, 0.0))) < 0)
        goto fail;

    /* Confidence and flags. The confidence reads the grid as written:
     * where a tracked beat strayed from it (a stretch followed on the
     * off-beat and outvoted), the envelopes are cut again at the grid. */
    stage(&tm, "phase+down");
    {
        size_t total = 1;
        for (int i = 0; i < c.nb_secs; i++)
            total += (size_t)(c.secs[i].last - c.secs[i].first + 1);
        int *strays = malloc(total * sizeof(*strays));
        if (!strays) {
            ret = RX_ERROR(ENOMEM);
            goto fail;
        }
        int nb_strays = 0;
        for (int i = 0; i < c.nb_secs; i++) {
            const RXFitSection *s = &c.secs[i];
            for (int k = s->first; k <= s->last; k++) {
                if (s->dynamic) {
                    if (fabs(c.envs.centre[k] - c.beats[k].time) > 0.025)
                        strays[nb_strays++] = k;
                    continue;
                }
                double p = rx_beat_predict(c.beats, k, s);
                if (fabs(c.beats[k].time - p) <= 0.025)
                    continue;
                c.beats[k].time = p;
                strays[nb_strays++] = k;
            }
        }
        ret = cut_envs(&c, strays, nb_strays);
        free(strays);
        if (ret < 0)
            goto fail;
    }
    if ((ret = grid_confidence(&c, bands, &out->confidence)) < 0)
        goto fail;
    stage(&tm, "confidence");
    rx_debug("beats: total      %6.1f ms\n", (rx_time_us() - tm0) / 1000.0);
    out->bpm_second_best = c.tempo.second_bpm;
    /* A boundary between two sections at one tempo is a phase cut (the
     * music re-enters shifted), any other a tempo change. */
    unsigned flags = 0;
    for (int i = 1; i < c.nb_secs; i++) {
        if (c.secs[i].dynamic || c.secs[i - 1].dynamic)
            continue;
        if (fabs(c.secs[i].period / c.secs[i - 1].period - 1.0) < 0.005)
            flags |= RX_GRID_FLAG_PHASE_CUT;
        else
            flags |= RX_GRID_FLAG_TEMPO_CHANGE;
    }
    for (int i = 0; i < c.nb_secs; i++) {
        if (c.secs[i].dynamic)
            flags |= RX_GRID_FLAG_DYNAMIC;
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

int rx_analyse_beats_dbg(const float *mono, int64_t nb_samples, int rate,
                         const RXAnalysisOptions *opt, RXBeatResult *out, RXBeatDebug *dbg)
{
    return analyse(mono, nb_samples, rate, NULL, opt, out, dbg);
}

int rx_analyse_beats_spec(const float *x, int64_t n, RXSpectrum *spec,
                          const RXAnalysisOptions *opt, RXBeatResult *out, RXBeatDebug *dbg)
{
    return analyse(x, n, RX_BEAT_RATE, spec, opt, out, dbg);
}
