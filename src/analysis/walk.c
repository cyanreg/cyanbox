/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The beat-by-beat grid (docs/BEATS.md, "Dynamic grids"): whether a
 * section's line holds its attacks at all, and, where it does not, the
 * walk that turns the attacks into the beats a player would follow.
 *
 * The question a straight line cannot answer is how far the music has
 * moved away from it. The attacks are measured to a third of a
 * millisecond but scatter (a drummer by 5-15 ms, beat to beat), so the
 * residual of one attack says little; the residual of a robust local
 * line through its neighbours says where the music is. Two things are
 * read off that:
 *
 *  - the drift: the largest running median (9 attacks) of the smoothed
 *    residuals. Where it passes 15 ms the straight grid is audibly off
 *    the beat there (the merge tolerance of the sections: below it two
 *    lines are one, above it a DJ hears the flam);
 *  - the bend: how much a quadratic through the attacks rises over the
 *    line between its ends and its middle. A tempo that moves within a
 *    section (a ramp, a drummer speeding up) bends the beat times, and
 *    the change-point search, which can only cut the section into
 *    straight pieces of 32 beats or more, leaves every piece bent. The
 *    bend of white scatter alone is known (sigma * sqrt(180 / n) / 4 for
 *    n attacks: the standard error of the quadratic coefficient over
 *    uniform abscissae, scaled to the rise at the ends), so a short live
 *    section is not called bent by its own scatter.
 *
 * The walk is the same local fit over one bar either side (±4 beats),
 * evaluated at every beat, but a quadratic: under a tempo that moves the
 * beat times curve, and a local line through them sits off the curve by
 * four times its curvature per beat squared (the window's second moment;
 * 17 ms at the steepest point of the rubato case in test_beats), where a
 * quadratic has no bias. It follows a tempo that moves inside a bar or
 * two, as a player following a drummer does, and averages the drummer's
 * scatter rather than reproducing it (a grid that copied every early and
 * late hit would make the deck's beat counter and the player's sync
 * jitter with it). An attack that is not where its neighbours say (a
 * fill, a snare taken for the kick) gets Tukey's weight of nothing and
 * the walk goes through it; a beat the tracker missed, or a breakdown
 * without attacks, is interpolated linearly between the nearest beats
 * that have them, and the ends extrapolated on the last local slope.
 * The tempo keeps its continuity (WALK_TEMPO_STEP): attacks that would
 * make it jump from one beat to the next, as a fill's accents do when
 * the tracker has followed them for a few beats, are no beats either. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The local line's reach (beats either side) for the walk and for the
 * strain; the strain's is longer so that a drummer's scatter averages out
 * before it is read as drift. */
#define WALK_REACH    4
#define STRAIN_REACH  8
#define STRAIN_MEDIAN 9
/* The walk's tempo continuity: the local tempo (60 over the interval to
 * the next beat) changes by at most 4 % from one beat to the next and
 * stays within a factor 1.2 of the stretch's median. A drummer's rubato
 * stays well inside both: the random walk of test_beats (0.4 % a beat)
 * walks at 3.0 % a beat at most, the rubato dip at 2.1 %, the ramp at
 * 0.3 %. The walk that followed Felicidade's syncopated fill went 128,
 * 123, 117, 105, 120, 129, 140 BPM from beat to beat, 12.5 % at once. */
#define WALK_TEMPO_STEP 0.04
#define WALK_TEMPO_BAND 0.2

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static bool supported(const CBBeatObs *b)
{
    return b->weight > 0.0 && !isnan(b->attack);
}

/* A 3x3 system by Cramer's rule; false when singular. */
static bool solve3(const double M[3][3], const double T[3], double sol[3])
{
    double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                 M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                 M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    double scale = fabs(M[0][0] * M[1][1] * M[2][2]);
    if (fabs(det) < 1e-12 * fmax(scale, 1e-300))
        return false;
    for (int col = 0; col < 3; col++) {
        double N[3][3];
        memcpy(N, M, sizeof(N));
        for (int row = 0; row < 3; row++)
            N[row][col] = T[row];
        sol[col] = (N[0][0] * (N[1][1] * N[2][2] - N[1][2] * N[2][1]) -
                    N[0][1] * (N[1][0] * N[2][2] - N[1][2] * N[2][0]) +
                    N[0][2] * (N[1][0] * N[2][1] - N[1][1] * N[2][0])) / det;
    }
    return true;
}

/* The weighted least-squares polynomial (a line, or a quadratic with
 * `quadratic`) through the supported attacks within `reach` beats of
 * beat k (by count), with a triangular window on top of the weights w,
 * evaluated at beat k: its value and its slope there. False with too few
 * attacks (three for a line, five for a quadratic) or too few distinct
 * counts. */
static bool local_fit(const CBBeatObs *beats, int first, int last, int k, int reach,
                      const double *w, bool quadratic, double *val, double *slope)
{
    double S[5] = { 0 }, T[3] = { 0 };
    int n = 0, lo_x = INT32_MAX, hi_x = INT32_MIN;
    for (int i = first; i <= last; i++) {
        int dx = beats[i].index - beats[k].index;
        if (dx < -reach || dx > reach || w[i - first] <= 0.0 || !supported(&beats[i]))
            continue;
        double ww = w[i - first] * (1.0 - fabs(dx) / (reach + 1.0));
        double x = dx, y = beats[i].attack, xp = 1.0;
        for (int p = 0; p < 5; p++) {
            S[p] += ww * xp;
            if (p < 3)
                T[p] += ww * xp * y;
            xp *= x;
        }
        n++;
        lo_x = CB_MIN(lo_x, dx);
        hi_x = CB_MAX(hi_x, dx);
    }
    if (n < (quadratic ? 5 : 3) || hi_x - lo_x < (quadratic ? 2 : 1) || S[0] <= 0.0)
        return false;
    if (quadratic) {
        double M[3][3] = { { S[0], S[1], S[2] }, { S[1], S[2], S[3] }, { S[2], S[3], S[4] } };
        double sol[3];
        if (!solve3(M, T, sol))
            return false;
        *val = sol[0];
        *slope = sol[1];
        return true;
    }
    double det = S[0] * S[2] - S[1] * S[1];
    if (det <= 1e-12 * S[0] * S[0])
        return false;
    double b = (S[0] * T[1] - S[1] * T[0]) / det;
    *val = (T[0] - b * S[1]) / S[0];
    *slope = b;
    return true;
}

/* The robust local line at every beat of [first, last]: a first pass
 * with the attacks' own weights, then Tukey's biweight on the residuals
 * from it, with the scale the larger of their robust spread and
 * `sigma`. sm[i] (NAN where the line cannot be had) and, optionally,
 * the slope sl[i]. */
static int smooth(const CBBeatObs *beats, int first, int last, int reach, bool quadratic,
                  double sigma, double *sm, double *sl)
{
    int n = last - first + 1;
    double *w = malloc((size_t)n * sizeof(*w));
    double *r = malloc((size_t)n * sizeof(*r));
    if (!w || !r) {
        free(w);
        free(r);
        return CB_ERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++)
        w[i] = beats[first + i].weight;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = first; i <= last; i++) {
            double v, b;
            if (local_fit(beats, first, last, i, reach, w, quadratic, &v, &b)) {
                sm[i - first] = v;
                if (sl)
                    sl[i - first] = b;
            } else {
                sm[i - first] = NAN;
                if (sl)
                    sl[i - first] = NAN;
            }
        }
        if (pass)
            break;
        int m = 0;
        for (int i = first; i <= last; i++) {
            if (supported(&beats[i]) && !isnan(sm[i - first]))
                r[m++] = fabs(beats[i].attack - sm[i - first]);
        }
        if (m < 3)
            break;
        qsort(r, (size_t)m, sizeof(*r), cmp_double);
        double s = fmax(1.4826 * r[m / 2], fmax(sigma, 0.001));
        double c = 4.685 * s;
        for (int i = first; i <= last; i++) {
            if (!supported(&beats[i]) || isnan(sm[i - first])) {
                w[i - first] = 0.0;
                continue;
            }
            double u = (beats[i].attack - sm[i - first]) / c;
            w[i - first] = fabs(u) < 1.0 ? beats[i].weight * (1 - u * u) * (1 - u * u) : 0.0;
        }
    }
    free(w);
    free(r);
    return 0;
}

int cb_line_strain(const CBBeatObs *beats, const CBFitSection *s, double *dev,
                   double *drift, double *bend, double *bend_noise)
{
    int n = s->last - s->first + 1;
    *drift = 0.0;
    *bend = 0.0;
    *bend_noise = INFINITY;
    double *sm = malloc((size_t)n * sizeof(*sm));
    double *r = malloc((size_t)n * sizeof(*r));
    if (!sm || !r) {
        free(sm);
        free(r);
        return CB_ERROR(ENOMEM);
    }
    /* The scale of the scatter: beat to beat where it could be measured
     * (the spread around the line, the jitter, holds the drift itself). */
    double sigma = s->scatter >= 0.0 ? s->scatter : s->jitter;
    int ret = smooth(beats, s->first, s->last, STRAIN_REACH, false, sigma, sm, NULL);
    if (ret < 0) {
        free(sm);
        free(r);
        return ret;
    }

    /* The smoothed residuals, and their running median: how far the
     * music sits from the line along it. */
    int m = 0;
    for (int i = s->first; i <= s->last; i++) {
        dev[i - s->first] = NAN;
        if (supported(&beats[i]) && !isnan(sm[i - s->first]))
            r[m++] = sm[i - s->first] - cb_beat_predict(beats, i, s);
    }
    if (m >= STRAIN_MEDIAN) {
        double win[STRAIN_MEDIAN];
        int j = 0;
        for (int i = s->first; i <= s->last; i++) {
            if (!supported(&beats[i]) || isnan(sm[i - s->first]))
                continue;
            int a = CB_CLAMP(j - STRAIN_MEDIAN / 2, 0, m - STRAIN_MEDIAN);
            memcpy(win, r + a, sizeof(win));
            qsort(win, STRAIN_MEDIAN, sizeof(*win), cmp_double);
            dev[i - s->first] = win[STRAIN_MEDIAN / 2];
            *drift = fmax(*drift, fabs(dev[i - s->first]));
            j++;
        }
    }

    /* The bend: a quadratic through the residuals from the line, with
     * Tukey's weights on the line's own scale, read as its rise between
     * the middle of the section and its ends. */
    if (m >= 12) {
        double k0 = 0.5 * (beats[s->first].index + beats[s->last].index);
        double K = beats[s->last].index - beats[s->first].index;
        double c = 0.0;
        double *w = malloc((size_t)n * sizeof(*w));
        if (!w) {
            free(sm);
            free(r);
            return CB_ERROR(ENOMEM);
        }
        /* Tukey's weights start from the line's own residuals: an
         * unweighted first fit through a stretch half a beat off the line
         * (an intro tracked on its hats) would bend to it, and then have
         * every attack as an outlier. */
        double scale = 4.685 * fmax(s->jitter, 0.001);
        for (int i = s->first; i <= s->last; i++) {
            if (!supported(&beats[i])) {
                w[i - s->first] = 0.0;
                continue;
            }
            double u = (beats[i].attack - cb_beat_predict(beats, i, s)) / scale;
            w[i - s->first] = fabs(u) < 1.0 ? beats[i].weight * (1 - u * u) * (1 - u * u) : 0.0;
        }
        double a = 0.0, b = 0.0;
        for (int it = 0; it < 4; it++) {
            /* Normal equations of a + b x + c x^2 over x = index - k0. */
            double S[5] = { 0 }, T[3] = { 0 };
            for (int i = s->first; i <= s->last; i++) {
                double ww = w[i - s->first];
                if (ww <= 0.0)
                    continue;
                double x = beats[i].index - k0;
                double y = beats[i].attack - cb_beat_predict(beats, i, s);
                double xp = 1.0;
                for (int p = 0; p < 5; p++) {
                    S[p] += ww * xp;
                    if (p < 3)
                        T[p] += ww * xp * y;
                    xp *= x;
                }
            }
            double M[3][3] = { { S[0], S[1], S[2] }, { S[1], S[2], S[3] }, { S[2], S[3], S[4] } };
            double sol[3];
            if (!solve3(M, T, sol))
                break;
            a = sol[0];
            b = sol[1];
            c = sol[2];
            for (int i = s->first; i <= s->last; i++) {
                if (!supported(&beats[i])) {
                    w[i - s->first] = 0.0;
                    continue;
                }
                double x = beats[i].index - k0;
                double y = beats[i].attack - cb_beat_predict(beats, i, s);
                double u = (y - (a + b * x + c * x * x)) / scale;
                w[i - s->first] = fabs(u) < 1.0 ? beats[i].weight * (1 - u * u) * (1 - u * u) : 0.0;
            }
        }
        free(w);
        *bend = fabs(c) * (K / 2) * (K / 2);
        *bend_noise = fmax(sigma, 0.001) * sqrt(180.0 / m) / 4.0;
    }
    free(sm);
    free(r);
    return 0;
}

/* How far the beat at a[j] (an index into t) lies from where its
 * neighbours among the anchors a[0 .. na) put it: between them, or on
 * the line through the next two at an end. */
static double anchor_offset(const double *t, const int *a, int na, int j)
{
    if (na < 3)
        return 0.0;
    int p, q;
    if (j == 0) {
        p = a[1];
        q = a[2];
    } else if (j == na - 1) {
        p = a[na - 3];
        q = a[na - 2];
    } else {
        p = a[j - 1];
        q = a[j + 1];
    }
    double v = (t[q] - t[p]) / (q - p);
    return fabs(t[a[j]] - (t[p] + (a[j] - p) * v));
}

/* The tempo continuity of the walk (WALK_TEMPO_STEP, WALK_TEMPO_BAND).
 * Between consecutive beats that have their own times t (NAN where they
 * are to be interpolated), each interval per beat is held against the
 * one before it and against the stretch's median interval `med`. While
 * some interval breaks the rules, of the beats that bound it (and the
 * one before, for a step) the one furthest from where its neighbours
 * put it is no beat, and goes to NAN: an attack that demands a jump in
 * tempo, a fill's accent the robust fit could not outvote, is walked
 * through by interpolation. Stops at three beats with times. */
static int tempo_continuity(double *t, int n, double med)
{
    int *a = malloc((size_t)CB_MAX(n, 1) * sizeof(*a));
    if (!a)
        return CB_ERROR(ENOMEM);
    const double step = log(1.0 + WALK_TEMPO_STEP), band = log(1.0 + WALK_TEMPO_BAND);
    while (1) {
        int na = 0;
        for (int i = 0; i < n; i++) {
            if (!isnan(t[i]))
                a[na++] = i;
        }
        if (na < 4)
            break;
        int drop = -1;
        double worst = -1.0;
        for (int j = 0; j + 1 < na; j++) {
            double v = (t[a[j + 1]] - t[a[j]]) / (a[j + 1] - a[j]);
            bool bad = !(v > 0.0) || fabs(log(v / med)) > band;
            int lo = j;
            if (!bad && j > 0) {
                double u = (t[a[j]] - t[a[j - 1]]) / (a[j] - a[j - 1]);
                bad = u > 0.0 && fabs(log(v / u)) > step;
                lo = j - 1;
            }
            if (!bad)
                continue;
            for (int k = lo; k <= j + 1; k++) {
                double d = anchor_offset(t, a, na, k);
                if (d > worst) {
                    worst = d;
                    drop = k;
                }
            }
        }
        if (drop < 0)
            break;
        t[a[drop]] = NAN;
    }
    free(a);
    return 0;
}

int cb_walk_beats(CBBeatObs *beats, int first, int last, double scatter)
{
    int n = last - first + 1;
    if (n < 2)
        return CB_ERROR(EINVAL);
    for (int i = first + 1; i <= last; i++) {
        if (beats[i].index != beats[i - 1].index + 1)
            return CB_ERROR(EINVAL);
    }
    int m = 0;
    for (int i = first; i <= last; i++)
        m += supported(&beats[i]);
    if (m < 4)
        return CB_ERROR(EINVAL);

    double *sm = malloc((size_t)n * sizeof(*sm));
    double *sl = malloc((size_t)n * sizeof(*sl));
    if (!sm || !sl) {
        free(sm);
        free(sl);
        return CB_ERROR(ENOMEM);
    }
    int ret = smooth(beats, first, last, WALK_REACH, true, scatter, sm, sl);
    if (ret < 0) {
        free(sm);
        free(sl);
        return ret;
    }

    /* Only beats with an attack of their own take their smoothed time;
     * the others are interpolated between them, so that a stretch the
     * tracker followed without attacks (a breakdown) gets evenly spaced
     * beats between its edges rather than the tail of a local line. */
    for (int i = first; i <= last; i++) {
        if (!supported(&beats[i]))
            sm[i - first] = NAN;
    }
    /* The intervals between consecutive smoothed beats, against the
     * median interval: a beat whose intervals either side are both odd
     * (an attack cluster the biweight could not outvote) is no beat. */
    double *iv = malloc((size_t)n * sizeof(*iv));
    if (!iv) {
        free(sm);
        free(sl);
        return CB_ERROR(ENOMEM);
    }
    int ni = 0;
    for (int i = 1; i < n; i++) {
        if (!isnan(sm[i]) && !isnan(sm[i - 1]))
            iv[ni++] = sm[i] - sm[i - 1];
    }
    if (ni >= 4) {
        qsort(iv, (size_t)ni, sizeof(*iv), cmp_double);
        double med = iv[ni / 2];
        for (int i = 0; i < n; i++) {
            if (isnan(sm[i]))
                continue;
            bool odd_l = i > 0 && !isnan(sm[i - 1]) &&
                         (sm[i] - sm[i - 1] < 0.5 * med || sm[i] - sm[i - 1] > 1.5 * med);
            bool odd_r = i + 1 < n && !isnan(sm[i + 1]) &&
                         (sm[i + 1] - sm[i] < 0.5 * med || sm[i + 1] - sm[i] > 1.5 * med);
            if (odd_l && odd_r)
                sm[i] = NAN;
        }
        /* And no two beats closer than a third of the median interval:
         * the later one goes. */
        for (int i = 1; i < n; i++) {
            int a = i - 1;
            while (a >= 0 && isnan(sm[a]))
                a--;
            if (a >= 0 && !isnan(sm[i]) && sm[i] - sm[a] < med / 3.0)
                sm[i] = NAN;
        }
        /* And the tempo keeps its continuity. */
        if (tempo_continuity(sm, n, med) < 0) {
            free(iv);
            free(sm);
            free(sl);
            return CB_ERROR(ENOMEM);
        }
    }
    free(iv);

    /* Gaps: linear between the nearest beats either side; the ends on
     * the last local line's slope. */
    int lo = -1, hi = -1;
    for (int i = 0; i < n; i++) {
        if (!isnan(sm[i])) {
            if (lo < 0)
                lo = i;
            hi = i;
        }
    }
    if (lo < 0 || hi == lo) {
        free(sm);
        free(sl);
        return CB_ERROR(EINVAL);
    }
    for (int i = lo + 1; i < hi; i++) {
        if (!isnan(sm[i]))
            continue;
        int j = i + 1;
        while (isnan(sm[j]))
            j++;
        for (int k = i; k < j; k++)
            sm[k] = sm[i - 1] + (sm[j] - sm[i - 1]) * (k - (i - 1)) / (double)(j - (i - 1));
        i = j;
    }
    /* The ends' tempo continues the first and the last interval within
     * WALK_TEMPO_STEP, as every other does its neighbour's. */
    double v_lo = sm[lo + 1] - sm[lo], v_hi = sm[hi] - sm[hi - 1];
    double p_lo = !isnan(sl[lo]) && sl[lo] > 0.0 ? sl[lo] : v_lo;
    double p_hi = !isnan(sl[hi]) && sl[hi] > 0.0 ? sl[hi] : v_hi;
    p_lo = CB_CLAMP(p_lo, v_lo / (1.0 + WALK_TEMPO_STEP), v_lo * (1.0 + WALK_TEMPO_STEP));
    p_hi = CB_CLAMP(p_hi, v_hi / (1.0 + WALK_TEMPO_STEP), v_hi * (1.0 + WALK_TEMPO_STEP));
    for (int i = lo - 1; i >= 0; i--)
        sm[i] = sm[lo] - (lo - i) * p_lo;
    for (int i = hi + 1; i < n; i++)
        sm[i] = sm[hi] + (i - hi) * p_hi;

    for (int i = first; i <= last; i++)
        beats[i].time = sm[i - first];
    free(sm);
    free(sl);
    return 0;
}
