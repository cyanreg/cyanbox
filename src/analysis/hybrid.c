/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The grid with Beat This! (docs/BEATS.md, "With Beat This!"): Beat This!'s
 * frame activations (cyanbox/beatthis.h, 50 a second) made into a DJ's grid,
 * with cyanbox's own analysis of the same audio as the hints and the
 * arbiter.
 *
 * Beat This! hears the beat where cyanbox's onsets mislead it (an off-beat
 * bass taken for the beat, a half tempo, the bar), but its own
 * postprocessing picks peaks, so its tempo wanders and changes level inside
 * a track; cyanbox's grid, where it is right, is a few times steadier. So:
 *
 *  1. the level: the beat period whose grid best explains Beat This!'s
 *     detections, per 20 s window, summed; an octave that scores nearly as
 *     well is settled by the bar its downbeats keep, then by cyanbox's
 *     tempo; folded into 80-201 BPM, and a reading of 88.5-100 that cyanbox
 *     hears at the double doubled (J-core, hardcore, drum and bass);
 *  2. the beats: Ellis' dynamic programming on the activation with a local
 *     period per window, numbered by elapsed periods, split into constant
 *     tempo sections by a penalised least-squares segmentation, sections
 *     either side of a breakdown bridged, one line if one fits;
 *  3. the arbiters, on cyanbox's grid at that level: cyanbox's grid where
 *     the two agree beat for beat; cyanbox's grid moved to Beat This!'s
 *     phase where that is one place all through and cyanbox was unsure
 *     (or off by more than a quarter beat); cyanbox's grid where it explains
 *     Beat This!'s detections as well;
 *  4. the tempo family: cyanbox's tempo where it is no octave of this one
 *     but explains the detections as well, and the beats tracked again
 *     there;
 *  5. the bar: per section, the place where the downbeat activation adds
 *     up most.
 *
 * This is a port of the research script (build-beats/eval/osu/hybrid.py,
 * frozen as "fix16") that was measured on 239 osu! beatmaps, 414 GiantSteps
 * tracks and 623 held-out beatmaps, and checked by ear; it keeps the
 * script's arithmetic, float32 where numpy's was, so that the two give the
 * same grids. Thresholds are the script's, each with the reason it has. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FPS     50.0
#define NB_BPM  720                 /* the tempo axis: 60 .. 239.75 BPM by 0.25 */
#define WIN     1000                /* level()'s window, 20 s, every 10 s */
#define FOLD_LO 80.0                /* the octave convention: 80 <= BPM < 201 */
#define FOLD_HI 201.0
#define CB2X    88.5
#define LAG     0.009               /* Beat This!'s beats are this much later than cyanbox's */
#define AGREE   0.75

static inline double bpm_of(int j)
{
    return 60.0 + 0.25 * j;
}

static inline double period_of(int j)
{
    return 60.0 * FPS / bpm_of(j);
}

/* -------------------------------------------------------------- numerics */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* numpy's median: the middle of the sorted values, or the mean of the two
 * middle ones; NaN for none. Sorts v. */
static double median(double *v, int n)
{
    if (n <= 0)
        return NAN;
    qsort(v, n, sizeof(*v), cmp_double);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static double median_of(const double *v, int n)
{
    double *t = malloc((size_t)CB_MAX(n, 1) * sizeof(*t));
    if (!t)
        return NAN;
    memcpy(t, v, (size_t)n * sizeof(*t));
    double m = median(t, n);
    free(t);
    return m;
}

/* The median of diff(v). */
static double median_diff(const double *v, int n)
{
    if (n < 2)
        return NAN;
    double *t = malloc((size_t)(n - 1) * sizeof(*t));
    if (!t)
        return NAN;
    for (int i = 0; i + 1 < n; i++)
        t[i] = v[i + 1] - v[i];
    double m = median(t, n - 1);
    free(t);
    return m;
}

/* numpy's quantile, its default ("linear") method. Sorts v. */
static double quantile(double *v, int n, double q)
{
    if (n <= 0)
        return NAN;
    qsort(v, n, sizeof(*v), cmp_double);
    double vi = n * q + (1.0 + q * (1.0 - 1.0 - 1.0)) - 1.0;
    double lo = floor(vi);
    double g = vi - lo;
    int a = (int)CB_CLAMP(lo, 0.0, (double)(n - 1));
    int b = (int)CB_CLAMP(lo + 1.0, 0.0, (double)(n - 1));
    double d = v[b] - v[a];
    return g >= 0.5 ? v[b] - d * (1.0 - g) : v[a] + d * g;
}

/* Python's x % 1.0 (the sign of the divisor). */
static double mod1(double x)
{
    double m = fmod(x, 1.0);
    if (m != 0.0 && m < 0.0)
        m += 1.0;
    return m;
}

/* The first index of the first value >= x (numpy's searchsorted). */
static int search(const double *v, int n, double x)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (v[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The weighted least-squares line t = t0 + p i. */
static void fit_line(const double *i, const double *t, const double *w, int n, double *t0,
                     double *p)
{
    double W = 0.0, wi = 0.0, wt = 0.0;
    for (int k = 0; k < n; k++) {
        W += w[k];
        wi += w[k] * i[k];
        wt += w[k] * t[k];
    }
    double mi = wi / W, mt = wt / W;
    double var = 0.0, cov = 0.0;
    for (int k = 0; k < n; k++) {
        var += w[k] * ((i[k] - mi) * (i[k] - mi));
        cov += w[k] * (i[k] - mi) * (t[k] - mt);
    }
    *p = var > 0.0 ? cov / var : 0.0;
    *t0 = mt - *p * mi;
}

/* a at fractional frames (0 outside). */
static double at(const float *a, int n, double f)
{
    double i = floor(f);
    double w = f - i;
    if (!(i >= 0.0 && i + 1.0 < n))
        return 0.0;
    int k = (int)i;
    return (double)a[k] * (1.0 - w) + (double)a[k + 1] * w;
}

/* Beat This!'s own detections: local maxima of the activation over 0.5. */
static void peak_train(const float *a, int n, uint8_t *pk)
{
    memset(pk, 0, (size_t)n);
    for (int i = 1; i + 1 < n; i++)
        pk[i] = a[i] > a[i - 1] && a[i] >= a[i + 1] && a[i] > 0.5f;
}

/* The share of x with a y within tol. */
static double agree(const double *x, int nx, const double *y, int ny, double tol)
{
    if (!nx || !ny)
        return 0.0;
    int hit = 0;
    for (int i = 0; i < nx; i++) {
        int j = CB_MIN(CB_MAX(search(y, ny, x[i]), 1), ny - 1);
        double d = fmin(fabs(y[CB_MAX(j - 1, 0)] - x[i]), fabs(y[j] - x[i]));
        hit += d < tol;
    }
    return (double)hit / nx;
}

/* ------------------------------------------------------------- the level */

typedef struct Level {
    double score[NB_BPM];           /* summed over the windows */
    int     nb_win;
    double *centre;                 /* [nb_win]: the window's centre, frames */
    double *rows;                   /* [nb_win][NB_BPM]: its own score */
} Level;

static void level_free(Level *L)
{
    free(L->centre);
    free(L->rows);
    memset(L, 0, sizeof(*L));
}

/* The beat period in frames: in each window, the grid that best explains
 * Beat This!'s detections (the F-measure of grid points with a detection
 * within a frame against detections hit), summed over the windows so that
 * drift between them does not matter. */
static int level(const float *a, int n, Level *L, double *period)
{
    memset(L, 0, sizeof(*L));
    uint8_t *pk = malloc((size_t)n), *near = malloc((size_t)n);
    int stop = CB_MAX(1, n - WIN / 2);
    L->nb_win = (stop + WIN / 2 - 1) / (WIN / 2);
    L->centre = calloc(L->nb_win, sizeof(*L->centre));
    L->rows = calloc((size_t)L->nb_win * NB_BPM, sizeof(*L->rows));
    if (!pk || !near || !L->centre || !L->rows) {
        free(pk);
        free(near);
        level_free(L);
        return CB_ERROR(ENOMEM);
    }
    peak_train(a, n, pk);
    for (int i = 0; i < n; i++)
        near[i] = pk[i] | (i > 0 && pk[i - 1]) | (i + 1 < n && pk[i + 1]);

    for (int w = 0; w < L->nb_win; w++) {
        int s = w * (WIN / 2);
        int len = CB_MIN(WIN, n - s);
        L->centre[w] = s + len / 2.0;
        double *row = L->rows + (size_t)w * NB_BPM;
        int npk = 0;
        for (int i = s; i < s + len; i++)
            npk += pk[i];
        if (npk < 4)
            continue;
        const uint8_t *seg = near + s;
        for (int j = 0; j < NB_BPM; j++) {
            double p = period_of(j);
            int nph = (int)ceil(p / 0.5), nk = (int)ceil((len - 1) / p);
            double best = -INFINITY;
            for (int q = 0; q < nph; q++) {
                double ph = q * 0.5;
                int hit = 0;
                for (int k = 0; k < nk; k++) {
                    long g = lrint(ph + k * p);
                    hit += seg[CB_MIN(g, (long)len - 1)];
                }
                double prec = (double)hit / nk, rec = fmin((double)hit / npk, 1.0);
                best = fmax(best, 2 * prec * rec / (prec + rec + 1e-9));
            }
            row[j] = best;
        }
        for (int j = 0; j < NB_BPM; j++)
            L->score[j] += row[j];
    }
    free(pk);
    free(near);

    int best = 0;
    for (int j = 1; j < NB_BPM; j++) {
        if (L->score[j] > L->score[best])
            best = j;
    }
    *period = period_of(best);
    return 0;
}

static double score_max(const double *sc)
{
    double m = sc[0];
    for (int j = 1; j < NB_BPM; j++)
        m = fmax(m, sc[j]);
    return m;
}

/* The share of gaps between Beat This!'s downbeat detections that are a bar
 * of 4 (or 3) beats of `period` (frames). */
static double bar_share(const uint8_t *dpk, int n, double period)
{
    int nb = 0, prev = -1, four = 0, three = 0;
    for (int i = 0; i < n; i++) {
        if (!dpk[i])
            continue;
        if (prev >= 0) {
            double g = (i - prev) / period;
            four += fabs(g / 4 - 1) < 0.1;
            three += fabs(g / 3 - 1) < 0.1;
        }
        prev = i;
        nb++;
    }
    if (nb < 8)
        return 0.0;
    return fmax((double)four / (nb - 1), (double)three / (nb - 1));
}

/* The level found, unless an octave of it scores nearly as well (within
 * 15 %): Beat This!'s activations alternate between the two there, and the
 * level whose bar its downbeats keep decides; when they keep none, the one
 * cyanbox found (hint, BPM). */
static double choose(double p, const double *score, const uint8_t *dpk, int n, double hint)
{
    double best = score_max(score);
    double cands[3] = { p };
    int nc = 1;
    static const double fs[2] = { 0.5, 2.0 };
    for (int f = 0; f < 2; f++) {
        double b = 60 * FPS / p * fs[f];
        int arg = -1;
        for (int j = 0; j < NB_BPM; j++) {
            if (fabs(bpm_of(j) - b) <= 0.5 && (arg < 0 || score[j] > score[arg]))
                arg = j;
        }
        if (arg >= 0 && score[arg] >= 0.85 * best)
            cands[nc++] = 60 * FPS / bpm_of(arg);
    }
    if (nc == 1)
        return p;
    double share[3];
    int arg = 0;
    for (int c = 0; c < nc; c++) {
        share[c] = bar_share(dpk, n, cands[c]);
        if (share[c] > share[arg])
            arg = c;
    }
    if (share[arg] >= 0.2)
        return cands[arg];
    if (hint) {
        for (int c = 0; c < nc; c++) {
            if (fabs(60 * FPS / cands[c] / hint - 1) < 0.04)
                return cands[c];
        }
    }
    return p;
}

/* A period folded into the convention's range by octaves. */
static double fold(double c)
{
    while (60 * FPS / c < FOLD_LO && 120 * FPS / c < FOLD_HI)
        c /= 2;
    while (60 * FPS / c >= FOLD_HI)
        c *= 2;
    return c;
}

/* The level: choose()'s, folded; a reading of 88.5 to 100 that cyanbox hears
 * at the double is doubled: J-core, hardcore and drum and bass, which DJs
 * count at the double (below 88.5 the slow ones, chill-out, deep house,
 * nu-disco, are as many). */
static double policy(double p, const double *score, const uint8_t *dpk, int n, double hint)
{
    double c = fold(choose(p, score, dpk, n, hint));
    double b = 60 * FPS / c;
    if (hint && fabs(hint / (2 * b) - 1) < 0.04 && b >= CB2X && 2 * b < FOLD_HI)
        c /= 2;
    return c;
}

/* How well a tempo (its octaves in range) explains Beat This!'s detections,
 * against the best. */
static double level_score(const double *sc, double bpm)
{
    static const double fs[3] = { 0.5, 1.0, 2.0 };
    double m = score_max(sc), v = -INFINITY;
    bool any = false;
    for (int f = 0; f < 3; f++) {
        double b = bpm * fs[f];
        if (!(60.5 <= b && b < 239.5))
            continue;
        for (int j = 0; j < NB_BPM; j++) {
            if (fabs(bpm_of(j) - b) <= 0.5) {
                v = fmax(v, sc[j]);
                any = true;
            }
        }
    }
    return any && m > 0 ? v / m : 0.0;
}

static bool octave_of(double x, double y)
{
    static const double fs[5] = { 1, 2, 0.5, 4, 0.25 };
    for (int f = 0; f < 5; f++) {
        if (fabs(x / (y * fs[f]) - 1) < 0.04)
            return true;
    }
    return false;
}

/* The period in force at each frame: every window's best within rng of p
 * (the same level), a median of three windows, held where a window has
 * nothing. */
static int local_periods(const Level *L, double p, int n, double rng, double *pl)
{
    double *cs = malloc((size_t)CB_MAX(L->nb_win, 1) * sizeof(*cs));
    double *ps = malloc((size_t)CB_MAX(L->nb_win, 1) * sizeof(*ps));
    double *sm = malloc((size_t)CB_MAX(L->nb_win, 1) * sizeof(*sm));
    if (!cs || !ps || !sm) {
        free(cs);
        free(ps);
        free(sm);
        return CB_ERROR(ENOMEM);
    }
    int m = 0;
    for (int w = 0; w < L->nb_win; w++) {
        const double *row = L->rows + (size_t)w * NB_BPM;
        int arg = -1;
        for (int j = 0; j < NB_BPM; j++) {
            double q = period_of(j);
            if (q > p / rng && q < p * rng && (arg < 0 || row[j] > row[arg]))
                arg = j;
        }
        if (arg >= 0 && row[arg] > 0) {
            cs[m] = L->centre[w];
            ps[m++] = period_of(arg);
        }
    }
    if (!m) {
        for (int i = 0; i < n; i++)
            pl[i] = p;
    } else {
        for (int i = 0; i < m; i++) {
            int a = CB_MAX(0, i - 1), b = CB_MIN(m, i + 2);
            sm[i] = median_of(ps + a, b - a);
        }
        /* numpy's interp */
        int j = 0;
        for (int i = 0; i < n; i++) {
            double x = i;
            if (x <= cs[0]) {
                pl[i] = sm[0];
                continue;
            }
            if (x >= cs[m - 1]) {
                pl[i] = sm[m - 1];
                continue;
            }
            while (j + 1 < m && cs[j + 1] <= x)
                j++;
            pl[i] = x == cs[j] ? sm[j]
                  : (sm[j + 1] - sm[j]) / (cs[j + 1] - cs[j]) * (x - cs[j]) + sm[j];
        }
    }
    free(cs);
    free(ps);
    free(sm);
    return 0;
}

/* ------------------------------------------------------------- the beats */

/* Ellis' dynamic programming beat tracker on the activation, with a period
 * (frames) per frame. Its score is float, as numpy's was. */
static int track(const float *a, int n, const double *pl, int **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    float *score = malloc((size_t)n * sizeof(*score));
    int *back = malloc((size_t)n * sizeof(*back));
    if (!score || !back) {
        free(score);
        free(back);
        return CB_ERROR(ENOMEM);
    }
    memcpy(score, a, (size_t)n * sizeof(*score));
    double lo_p = pl[0], hi_p = pl[0];
    for (int i = 1; i < n; i++) {
        lo_p = fmin(lo_p, pl[i]);
        hi_p = fmax(hi_p, pl[i]);
    }
    int d0 = CB_MAX((int)(lo_p * 0.75), 1), d1 = (int)(hi_p * 1.33) + 1;
    for (int t = 0; t < n; t++)
        back[t] = -1;
    for (int t = d0; t < n; t++) {
        double best = -INFINITY;
        int arg = -1;
        for (int d = d0; d < d1 && t - d >= 0; d++) {
            double l = log(d / pl[t]);
            double c = (double)score[t - d] - 200.0 * (l * l);
            if (c > best) {
                best = c;
                arg = t - d;
            }
        }
        if (arg >= 0 && best > 0) {
            score[t] = (float)((double)score[t] + best);
            back[t] = arg;
        }
    }
    int m = (int)pl[n - 1] + 1;
    int from = CB_MAX(0, n - m), t = from;
    for (int i = from + 1; i < n; i++) {
        if (score[i] > score[t])
            t = i;
    }
    int cnt = 0;
    for (int k = t; k >= 0; k = back[k])
        cnt++;
    int *b = malloc((size_t)CB_MAX(cnt, 1) * sizeof(*b));
    if (!b) {
        free(score);
        free(back);
        return CB_ERROR(ENOMEM);
    }
    for (int k = t, i = cnt - 1; k >= 0; k = back[k], i--)
        b[i] = k;
    free(score);
    free(back);
    *out = b;
    *nb = cnt;
    return 0;
}

/* Each beat to the activation's peak within two frames, parabolic between
 * frames, in float as numpy's was; *f32 says whether all of them were (the
 * times are then divided in float too). */
static void refine(const float *a, int n, const int *beat, int nb, double *f, bool *f32)
{
    *f32 = true;
    for (int b = 0; b < nb; b++) {
        int i = beat[b];
        int lo = CB_MAX(1, i - 2), hi = CB_MIN(n - 2, i + 2);
        if (hi < lo) {
            f[b] = i;
            *f32 = false;
            continue;
        }
        int j = lo;
        for (int k = lo + 1; k <= hi; k++) {
            if (a[k] > a[j])
                j = k;
        }
        float y0 = a[j - 1], y1 = a[j], y2 = a[j + 1];
        float den = y0 - 2.0f * y1 + y2;
        if (den < 0) {
            float r = 0.5f * (y0 - y2) / den;
            f[b] = (float)j + r;
        } else {
            f[b] = j;
            *f32 = false;
        }
    }
}

/* bt[i] - bt[j], in float where the times are. */
static inline double dt(const double *bt, int i, int j, bool f32)
{
    return f32 ? (double)((float)bt[i] - (float)bt[j]) : bt[i] - bt[j];
}

/* The tracker's beats numbered by the periods between them (per, s, at
 * each), not by their place: a beat it skipped leaves its number out, so a
 * line through beats either side of it still fits. Of two closer than three
 * quarters of a period the weaker goes. Returns the count kept. */
static int numbered(double *bt, double *w, double *per, int n, bool f32, double *kk)
{
    int m = 1;
    for (int i = 1; i < n; i++) {
        int j = m - 1;
        if (dt(bt, i, j, f32) < 0.75 * per[j]) {
            if (w[i] > w[j]) {
                bt[j] = bt[i];
                w[j] = w[i];
                per[j] = per[i];
            }
            continue;
        }
        bt[m] = bt[i];
        w[m] = w[i];
        per[m] = per[i];
        m++;
    }
    kk[0] = 0.0;
    for (int i = 1; i < m; i++)
        kk[i] = kk[i - 1] + fmax(1.0, rint(dt(bt, i, i - 1, f32) / per[i - 1]));
    return m;
}

/* Beats a line through their neighbours (8 each side, itself left out)
 * misses by 25 ms. */
static void outliers(const double *bt, const double *w, const double *kk, int n, bool *bad)
{
    double ii[16], tt[16], ones[16];
    for (int k = 0; k < 16; k++)
        ones[k] = 1.0;
    for (int i = 0; i < n; i++) {
        bad[i] = false;
        int m = 0;
        for (int j = CB_MAX(0, i - 8); j < CB_MIN(n, i + 9); j++) {
            if (j != i && w[j] >= 0.3) {
                ii[m] = kk[j];
                tt[m++] = bt[j];
            }
        }
        if (m < 4)
            continue;
        double t0, p;
        fit_line(ii, tt, ones, m, &t0, &p);
        bad[i] = fabs(bt[i] - (t0 + p * kk[i])) > 0.025;
    }
}

/* The beats' timing noise: a constant tempo's second differences have
 * sqrt(6) times it, robustly (the median absolute one) over the strong
 * beats; in float where the times are. */
static double jitter(const double *bt, const double *w, int n, bool f32)
{
    const double lo = 0.006, hi = 0.02;
    if (n < 3)
        return lo;
    double *d = malloc((size_t)n * sizeof(*d));
    float *df = malloc((size_t)n * sizeof(*df));
    if (!d || !df) {
        free(d);
        free(df);
        return lo;
    }
    int m = 0;
    for (int i = 2; i < n; i++) {
        if (!(w[i] >= 0.5 && w[i - 1] >= 0.5 && w[i - 2] >= 0.5))
            continue;
        if (f32)
            df[m++] = fabsf((float)bt[i] - 2.0f * (float)bt[i - 1] + (float)bt[i - 2]);
        else
            d[m++] = fabs(bt[i] - 2 * bt[i - 1] + bt[i - 2]);
    }
    double j = lo;
    if (m >= 16) {
        if (f32) {
            qsort(df, m, sizeof(*df), cmp_float);
            float med = m & 1 ? df[m / 2] : (df[m / 2 - 1] + df[m / 2]) / 2.0f;
            j = (double)(1.4826f * med) / sqrt(6.0);
        } else {
            j = 1.4826 * median(d, m) / sqrt(6.0);
        }
        j = CB_CLAMP(j, lo, hi);
    }
    free(d);
    free(df);
    return j;
}

typedef struct Sec {
    int s, e;                       /* beats [s, e) */
    double t0, p;
} Sec;

/* Piecewise constant tempo through the beat times, weighted by strength:
 * the split into runs of consecutive beats (8 at least) that minimises the
 * squared error (in the beats' jitter) plus 60 a run. Every run's weighted
 * line fit comes from prefix sums in O(1). */
static int sections(const double *bt, const double *w, const double *kk, int n, bool f32,
                    Sec **out, int *nb)
{
    const double penalty = 60.0;
    const int min_len = 8;
    *out = NULL;
    *nb = 0;
    double sigma = jitter(bt, w, n, f32);
    size_t sz = (size_t)(n + 1) * sizeof(double);
    double *ww = malloc(sz), *S = malloc(sz), *Si = malloc(sz), *St = malloc(sz);
    double *Sii = malloc(sz), *Sit = malloc(sz), *Stt = malloc(sz), *best = malloc(sz);
    int *arg = malloc((size_t)(n + 1) * sizeof(*arg));
    bool *bad = malloc((size_t)CB_MAX(n, 1) * sizeof(*bad));
    Sec *secs = NULL;
    int ret = CB_ERROR(ENOMEM);
    if (!ww || !S || !Si || !St || !Sii || !Sit || !Stt || !best || !arg || !bad)
        goto end;
    outliers(bt, w, kk, n, bad);
    for (int i = 0; i < n; i++)
        ww[i] = bad[i] ? 0.0 : CB_CLAMP(w[i], 0.05, 1.0);
    S[0] = Si[0] = St[0] = Sii[0] = Sit[0] = Stt[0] = 0.0;
    for (int i = 0; i < n; i++) {
        S[i + 1] = S[i] + ww[i];
        Si[i + 1] = Si[i] + ww[i] * kk[i];
        St[i + 1] = St[i] + ww[i] * bt[i];
        Sii[i + 1] = Sii[i] + ww[i] * kk[i] * kk[i];
        Sit[i + 1] = Sit[i] + ww[i] * kk[i] * bt[i];
        Stt[i + 1] = Stt[i] + ww[i] * bt[i] * bt[i];
    }
    for (int i = 0; i <= n; i++) {
        best[i] = INFINITY;
        arg[i] = 0;
    }
    best[0] = 0.0;
    for (int e = min_len; e <= n; e++) {
        double bc = 0.0;
        int bs = -1;
        for (int s = 0; s <= e - min_len; s++) {
            if (!isfinite(best[s]))
                continue;
            double W = S[e] - S[s];
            double Wi = Si[e] - Si[s], Wt = St[e] - St[s];
            double Wii = Sii[e] - Sii[s], Wit = Sit[e] - Sit[s], Wtt = Stt[e] - Stt[s];
            double vi = Wii - Wi * Wi / W;
            double cov = Wit - Wi * Wt / W;
            double vt = Wtt - Wt * Wt / W;
            double sse = vi > 0 ? vt - cov * cov / vi : vt;
            /* numpy's maximum and argmin: a NaN is kept, and the first wins */
            double c = best[s] + (isnan(sse) ? sse : fmax(sse, 0.0)) / (sigma * sigma) + penalty;
            if (isnan(c)) {
                bc = c;
                bs = s;
                break;
            }
            if (bs < 0 || c < bc) {
                bc = c;
                bs = s;
            }
        }
        if (bs >= 0) {
            best[e] = bc;
            arg[e] = bs;
        }
    }
    double *w9 = malloc(sz);
    if (!w9)
        goto end;
    for (int i = 0; i < n; i++)
        w9[i] = ww[i] + 1e-9;
    if (!isfinite(best[n])) {
        secs = malloc(sizeof(*secs));
        if (secs) {
            secs[0] = (Sec) { 0, n };
            fit_line(kk, bt, w9, n, &secs[0].t0, &secs[0].p);
            *nb = 1;
        }
    } else {
        int cnt = 0;
        for (int e = n; e > 0; e = arg[e])
            cnt++;
        secs = malloc((size_t)cnt * sizeof(*secs));
        if (secs) {
            int k = cnt - 1;
            for (int e = n; e > 0; e = arg[e], k--)
                secs[k] = (Sec) { arg[e], e };
            for (k = 0; k < cnt; k++) {
                Sec *c = &secs[k];
                fit_line(kk + c->s, bt + c->s, w9 + c->s, c->e - c->s, &c->t0, &c->p);
            }
            *nb = cnt;
        }
    }
    free(w9);
    if (secs) {
        *out = secs;
        ret = 0;
    }
end:
    free(ww);
    free(S);
    free(Si);
    free(St);
    free(Sii);
    free(Sit);
    free(Stt);
    free(best);
    free(arg);
    free(bad);
    return ret;
}

typedef struct Group {
    double start, t0, p;
    double *k, *t, *w;
    int n;
} Group;

/* A steady line through the whole track when nearly all its strong beats
 * lie on it (a robust line: refitted twice without the beats it misses by
 * 20 ms). */
static bool single(const double *bt, const double *w, const double *kk, int n, Group *g)
{
    const double tol = 0.02;
    int strong = 0;
    for (int i = 0; i < n; i++)
        strong += w[i] >= 0.5;
    if (strong < 16)
        return false;
    double *ii = malloc((size_t)n * sizeof(*ii)), *tt = malloc((size_t)n * sizeof(*tt));
    double *ws = malloc((size_t)n * sizeof(*ws));
    bool *keep = malloc((size_t)n * sizeof(*keep));
    if (!ii || !tt || !ws || !keep) {
        free(ii);
        free(tt);
        free(ws);
        free(keep);
        return false;
    }
    for (int i = 0; i < n; i++)
        keep[i] = w[i] >= 0.5;
    double t0 = 0.0, p = 0.0;
    for (int it = 0; it < 3; it++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (keep[i]) {
                ii[m] = kk[i];
                tt[m] = bt[i];
                ws[m++] = w[i];
            }
        }
        fit_line(ii, tt, ws, m, &t0, &p);
        for (int i = 0; i < n; i++)
            keep[i] = w[i] >= 0.5 && fabs(bt[i] - (t0 + p * kk[i])) < tol;
    }
    int on = 0;
    for (int i = 0; i < n; i++)
        on += keep[i];
    free(ii);
    free(tt);
    free(ws);
    free(keep);
    if ((double)on / strong < 0.9)
        return false;
    g->start = 0.0;
    g->t0 = t0;
    g->p = p;
    return true;
}

static int group_add(Group *g, const double *k, const double *t, const double *w, int n)
{
    double *nk = realloc(g->k, (size_t)(g->n + n) * sizeof(*nk));
    if (nk)
        g->k = nk;
    double *nt = realloc(g->t, (size_t)(g->n + n) * sizeof(*nt));
    if (nt)
        g->t = nt;
    double *nw = realloc(g->w, (size_t)(g->n + n) * sizeof(*nw));
    if (nw)
        g->w = nw;
    if (!nk || !nt || !nw)
        return CB_ERROR(ENOMEM);
    for (int i = 0; i < n; i++) {
        g->k[g->n + i] = k[i];
        g->t[g->n + i] = t[i];
        g->w[g->n + i] = CB_CLAMP(w[i], 0.05, 1.0);
    }
    g->n += n;
    fit_line(g->k, g->t, g->w, g->n, &g->t0, &g->p);
    return 0;
}

/* Whether section c's beats fall on the grid carried over in g (not its own
 * fitted tempo: a short run's is a few tenths of a percent off): most
 * within 15 ms, nearly all within twice that, the tempo within 1 %. k gets
 * their numbers on it. */
static bool fits(const Group *g, const Sec *c, const double *bt, const double *w, double *k,
                 double *fr)
{
    int n = c->e - c->s, strong = 0;
    for (int i = 0; i < n; i++) {
        double x = (bt[c->s + i] - g->t0) / g->p;
        k[i] = rint(x);
        strong += w[c->s + i] >= 0.5;
    }
    if (!(fabs(c->p / g->p - 1) < 0.01))
        return false;
    int m = 0;
    for (int i = 0; i < n; i++) {
        double x = (bt[c->s + i] - g->t0) / g->p;
        if (strong < 4 || w[c->s + i] >= 0.5)
            fr[m++] = fabs(x - rint(x)) * g->p;
    }
    if (!(median(fr, m) < 0.015))
        return false;
    return quantile(fr, m, 0.9) < 0.03;
}

static void groups_free(Group *g, int n)
{
    for (int i = 0; i < n; i++) {
        free(g[i].k);
        free(g[i].t);
        free(g[i].w);
    }
    free(g);
}

/* Steady stretches either side of a breakdown are one grid when they agree.
 * The sections in order: one joins the group before it when its tempo
 * matches and its beats fall on the grid carried over; a weak one (short or
 * faint) in between that does not is taken over by the group when the next
 * strong one does join; any other starts a group of its own. */
static int bridge(const Sec *secs, int ns, const double *bt, const double *w, const double *kk,
                  int n, Group **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    Group *gs = calloc(ns, sizeof(*gs));
    bool *strong = calloc(ns, sizeof(*strong));
    double *k = malloc((size_t)CB_MAX(n, 1) * sizeof(*k));
    double *fr = malloc((size_t)CB_MAX(n, 1) * sizeof(*fr));
    double *k0 = malloc((size_t)CB_MAX(n, 1) * sizeof(*k0));
    int ng = 0, ret = CB_ERROR(ENOMEM);
    if (!gs || !strong || !k || !fr || !k0)
        goto fail;
    for (int i = 0; i < ns; i++) {
        double sum = 0.0;
        for (int b = secs[i].s; b < secs[i].e; b++)
            sum += w[b];
        strong[i] = secs[i].e - secs[i].s >= 16 && sum / (secs[i].e - secs[i].s) >= 0.5;
    }
#define NEW_GROUP(c)                                                                  \
    do {                                                                              \
        const Sec *c_ = (c);                                                          \
        Group *g_ = &gs[ng++];                                                        \
        g_->start = bt[c_->s];                                                        \
        for (int b_ = c_->s; b_ < c_->e; b_++)                                        \
            k0[b_ - c_->s] = kk[b_] - kk[c_->s];                                      \
        if ((ret = group_add(g_, k0, bt + c_->s, w + c_->s, c_->e - c_->s)) < 0)      \
            goto fail;                                                                \
    } while (0)
    NEW_GROUP(&secs[0]);
    for (int i = 1; i < ns;) {
        Group *g = &gs[ng - 1];
        if (fits(g, &secs[i], bt, w, k, fr)) {
            if ((ret = group_add(g, k, bt + secs[i].s, w + secs[i].s, secs[i].e - secs[i].s)) < 0)
                goto fail;
            i++;
            continue;
        }
        if (!strong[i]) {
            /* a weak run: bridged when the next strong section carries on the grid */
            int m = i + 1;
            while (m < ns && !strong[m])
                m++;
            if (m < ns && fits(g, &secs[m], bt, w, k, fr)) {
                if ((ret = group_add(g, k, bt + secs[m].s, w + secs[m].s,
                                     secs[m].e - secs[m].s)) < 0)
                    goto fail;
                i = m + 1;
                continue;
            }
        }
        NEW_GROUP(&secs[i]);
        i++;
    }
#undef NEW_GROUP
    if (ng > 1) {
        Group one = { 0 };
        if (single(bt, w, kk, n, &one)) {
            groups_free(gs, ng);
            gs = calloc(1, sizeof(*gs));
            if (!gs) {
                ng = 0;
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
            gs[0] = one;
            ng = 1;
        }
    }
    gs[0].start = 0.0;
    free(strong);
    free(k);
    free(fr);
    free(k0);
    *out = gs;
    *nb = ng;
    return 0;
fail:
    groups_free(gs, ng);
    free(strong);
    free(k);
    free(fr);
    free(k0);
    return ret;
}

/* Each group's grid through cyanbox's beats where most of its beats have
 * one within 25 ms: cyanbox's are a few times steadier. Elsewhere only the
 * constant latency between the two comes off. */
static void refine_cb(Group *gs, int ng, double dur, const double *cb, int ncb)
{
    for (int n = 0; n < ng; n++) {
        Group *g = &gs[n];
        double stop = n + 1 < ng ? gs[n + 1].start : dur;
        double a = ceil((fmax(g->start, 0.0) - g->t0) / g->p);
        double b = floor((stop - g->t0) / g->p) + 1;
        int m = (int)fmax(b - a, 0.0), ok = 0;
        double *kk = malloc((size_t)CB_MAX(m, 1) * sizeof(*kk));
        double *tt = malloc((size_t)CB_MAX(m, 1) * sizeof(*tt));
        double *ones = malloc((size_t)CB_MAX(m, 1) * sizeof(*ones));
        if (kk && tt && ones && ncb) {
            for (int i = 0; i < m; i++) {
                double k = a + i;
                double t = g->t0 + k * g->p - LAG;
                int j = search(cb, ncb, t);
                int jl = CB_CLAMP(j - 1, 0, ncb - 1), jr = CB_CLAMP(j, 0, ncb - 1);
                double nr = fabs(cb[jl] - t) < fabs(cb[jr] - t) ? cb[jl] : cb[jr];
                if (fabs(nr - t) < 0.025) {
                    kk[ok] = k;
                    tt[ok] = nr;
                    ones[ok++] = 1.0;
                }
            }
        }
        g->start -= LAG;
        if (kk && tt && ones && m >= 8 && (double)ok / m >= 0.6)
            fit_line(kk, tt, ones, ok, &g->t0, &g->p);
        else
            g->t0 -= LAG;
        free(kk);
        free(tt);
        free(ones);
    }
}

typedef struct Beat {
    double t, bpm;
    int sec;
} Beat;

/* The beats of the groups: each one's grid from its start to the next
 * one's. */
static int grid_of(const Group *gs, int ng, double dur, Beat **out, int *nb)
{
    int cap = 64, m = 0;
    Beat *b = malloc((size_t)cap * sizeof(*b));
    if (!b)
        return CB_ERROR(ENOMEM);
    for (int n = 0; n < ng; n++) {
        const Group *g = &gs[n];
        if (!(g->p > 0.0))
            continue;
        double stop = n + 1 < ng ? gs[n + 1].start - 0.35 * g->p : dur;
        double k = n ? ceil((g->start - g->t0) / g->p - 1e-9) : ceil(-g->t0 / g->p - 1e-9);
        while (1) {
            double t = g->t0 + k * g->p;
            if (t >= stop)
                break;
            if (t >= 0) {
                if (m == cap) {
                    Beat *nb2 = realloc(b, (size_t)(cap *= 2) * sizeof(*b));
                    if (!nb2) {
                        free(b);
                        return CB_ERROR(ENOMEM);
                    }
                    b = nb2;
                }
                b[m++] = (Beat) { t, 60 / g->p, n };
            }
            k += 1;
        }
    }
    *out = b;
    *nb = m;
    return 0;
}

/* -------------------------------------------------------------- arbiters */

typedef struct Vec {
    double *v;
    int n;
} Vec;

/* cyanbox's beats at the level of ref's (its octave above or below);
 * *ok false when neither. */
static int at_level(const double *cb, int ncb, const double *ref, int nref, Vec *out, bool *ok)
{
    *ok = false;
    out->v = NULL;
    out->n = 0;
    if (ncb < 8 || nref < 8)
        return 0;
    double r = median_diff(cb, ncb) / median_diff(ref, nref);
    if (fabs(r - 1) < 0.04) {
        out->v = malloc((size_t)ncb * sizeof(double));
        if (!out->v)
            return CB_ERROR(ENOMEM);
        memcpy(out->v, cb, (size_t)ncb * sizeof(double));
        out->n = ncb;
    } else if (fabs(r - 2) < 0.08) {
        out->v = malloc((size_t)(2 * ncb - 1) * sizeof(double));
        if (!out->v)
            return CB_ERROR(ENOMEM);
        for (int i = 0; i < ncb; i++) {
            out->v[2 * i] = cb[i];
            if (i + 1 < ncb)
                out->v[2 * i + 1] = (cb[i] + cb[i + 1]) / 2;
        }
        out->n = 2 * ncb - 1;
        qsort(out->v, out->n, sizeof(double), cmp_double);
    } else if (fabs(r - 0.5) < 0.02) {
        int na = (ncb + 1) / 2, nb = ncb / 2;
        double *a = malloc((size_t)na * sizeof(*a)), *b = malloc((size_t)CB_MAX(nb, 1) * sizeof(*b));
        if (!a || !b) {
            free(a);
            free(b);
            return CB_ERROR(ENOMEM);
        }
        for (int i = 0; i < ncb; i++) {
            if (i & 1)
                b[i / 2] = cb[i];
            else
                a[i / 2] = cb[i];
        }
        if (agree(a, na, ref, nref, 0.035) >= agree(b, nb, ref, nref, 0.035)) {
            out->v = a;
            out->n = na;
            free(b);
        } else {
            out->v = b;
            out->n = nb;
            free(a);
        }
    } else {
        return 0;
    }
    *ok = true;
    return 0;
}

typedef struct Shift {
    double small_ms;                /* within this of c's own phase: c as it is */
    bool sure;                      /* cyanbox sure of its phase */
} Shift;

/* cyanbox's grid c at the phase ht's beats keep on it (each one's phase in
 * c's beat, their circular mean, concentrated): within small_ms of c's own,
 * c as it is (Beat This!'s beats stray that far from a right grid: 95 % of
 * the osu! maps' tracks within 20 ms); further, c moved there, when most
 * 30 s windows (75 %) agree with that, to 0.15 of a beat. Only ht's beats at
 * c's tempo count (a stretch at another tempo says nothing of c's phase).
 * 0: moved into `out` (or kept, *same), 1: ht keeps no one place on c. */
static int shift_to(const double *c, int nc, const double *ht, int nh, const Shift *o,
                    double *out, bool *same)
{
    const double win = 30.0, conc = 0.8, keep = 0.4, loose = 0.15, majority = 0.75;
    *same = false;
    if (nc < 2)
        return 1;
    double *t = malloc((size_t)CB_MAX(nh, 1) * sizeof(*t));
    double *ph = malloc((size_t)CB_MAX(nh, 1) * sizeof(*ph));
    double *per = malloc((size_t)nc * sizeof(*per));
    if (!t || !ph || !per) {
        free(t);
        free(ph);
        free(per);
        return CB_ERROR(ENOMEM);
    }
    double cp = median_diff(c, nc);
    int m = 0;
    for (int i = 0; i < nh; i++) {
        int j = search(c, nc, ht[i]) - 1;
        if (!(j >= 0 && j < nc - 1))
            continue;
        if (nh > 2) {
            double ib = i == 0 ? ht[1] - ht[0]
                      : i == nh - 1 ? ht[nh - 1] - ht[nh - 2]
                      : fmin(ht[i] - ht[i - 1], ht[i + 1] - ht[i]);
            if (!(fabs(ib / cp - 1) < 0.03))
                continue;
        }
        t[m] = ht[i];
        ph[m++] = (ht[i] - c[j]) / (c[j + 1] - c[j]);
    }
    int ret = 1;
    if (m < 32)
        goto end;
    double re = 0.0, im = 0.0;
    for (int i = 0; i < m; i++) {
        re += cos(2 * M_PI * ph[i]);
        im += sin(2 * M_PI * ph[i]);
    }
    re /= m;
    im /= m;
    double R = hypot(re, im);
    double mean = mod1(atan2(im, re) / (2 * M_PI) + 0.5) - 0.5;
    for (int i = 0; i + 1 < nc; i++)
        per[i] = c[i + 1] - c[i];
    per[nc - 1] = per[nc - 2];
    double mper = median_of(per, nc - 1);
    /* on c on the whole: c as it is; when cyanbox is sure of it, even if
     * they scatter (Beat This!'s timing wobbles where cyanbox's grid is
     * right) */
    if (R >= (o->sure ? keep : conc) && fabs(mean) * mper * 1000 <= o->small_ms) {
        memcpy(out, c, (size_t)nc * sizeof(*out));
        *same = true;
        ret = 0;
        goto end;
    }
    /* moving c takes beats that keep one place (less firmly when cyanbox
     * was unsure) */
    if (R < (o->sure ? conc : 0.6))
        goto end;
    int good = 0, nw = 0;
    int cnt = (int)ceil((t[m - 1] - t[0]) / win);
    for (int k = 0; k < cnt; k++) {
        double a = t[0] + k * win;
        double sr = 0.0, si = 0.0;
        int sel = 0;
        for (int i = 0; i < m; i++) {
            if (t[i] >= a && t[i] < a + win) {
                sr += cos(2 * M_PI * ph[i]);
                si += sin(2 * M_PI * ph[i]);
                sel++;
            }
        }
        if (sel >= 8) {
            nw++;
            double d = mod1(atan2(si / sel, sr / sel) / (2 * M_PI) - mean + 0.5) - 0.5;
            good += fabs(d) <= loose;
        }
    }
    if (!nw || good < majority * nw)
        goto end;
    for (int i = 0; i < nc; i++)
        out[i] = c[i] + mean * per[i];
    ret = 0;
end:
    free(t);
    free(ph);
    free(per);
    return ret;
}

/* cyanbox's grid in its sections: runs of beats whose spacing keeps to the
 * run's (a phase cut or a change of tempo starts the next). */
static int runs_of(const double *c, int n, int (*runs)[2], int max)
{
    if (n < 3) {
        runs[0][0] = 0;
        runs[0][1] = n;
        return 1;
    }
    int *cuts = malloc((size_t)n * sizeof(*cuts));
    double *tmp = malloc(8 * sizeof(*tmp));
    if (!cuts || !tmp) {
        free(cuts);
        free(tmp);
        return CB_ERROR(ENOMEM);
    }
    int nc = 0;
    cuts[nc++] = 0;
    for (int i = 1; i < n - 1; i++) {
        int a = CB_MAX(cuts[nc - 1], i - 8), m = 0;
        for (int k = a; k < i; k++)
            tmp[m++] = c[k + 1] - c[k];
        double ref = median(tmp, m);
        double x = fabs((c[i + 1] - c[i]) / ref - 1);
        if (x > 0.03)
            cuts[nc++] = x > 0.5 ? i + 1 : i;
    }
    int nr = 0;
    for (int k = 0; k < nc; k++) {
        int a = cuts[k], b = k + 1 < nc ? cuts[k + 1] : n;
        if (b - a >= 2) {
            if (nr < max) {
                runs[nr][0] = a;
                runs[nr][1] = b;
            }
            nr++;
        }
    }
    free(cuts);
    free(tmp);
    return nr;
}

/* shift_to section by section of cyanbox's grid: a section of its own phase
 * (after a phase cut) is right or wrong on its own. A few steady sections
 * are decided each on its own; one, or a grid measured beat by beat (many
 * short runs), as a whole. A section without the beats to decide stays as
 * it is; 1 when the longest decides nothing. */
static int shift_sections(const double *c, int nc, const double *ht, int nh, const Shift *o,
                          double *out, bool *same)
{
    int runs[5][2];
    int nr = runs_of(c, nc, runs, 5);
    if (nr < 0)
        return nr;
    if (nr == 1 || nr > 4)
        return shift_to(c, nc, ht, nh, o, out, same);
    if (nr == 0)
        return 1;
    memcpy(out, c, (size_t)nc * sizeof(*out));
    int longest = 0;
    for (int r = 1; r < nr; r++) {
        if (runs[r][1] - runs[r][0] > runs[longest][1] - runs[longest][0])
            longest = r;
    }
    bool all_same = true;
    for (int r = 0; r < nr; r++) {
        int a = runs[r][0], b = runs[r][1];
        if (b - a < 16)
            continue;
        int h0 = 0;
        while (h0 < nh && !(ht[h0] >= c[a] - 0.05))
            h0++;
        int h1 = h0;
        while (h1 < nh && ht[h1] <= c[b - 1] + 0.05)
            h1++;
        bool s;
        int ret = shift_to(c + a, b - a, ht + h0, h1 - h0, o, out + a, &s);
        if (ret < 0)
            return ret;
        if (ret) {
            if (r == longest)
                return 1;
            continue;
        }
        all_same &= s;
    }
    *same = all_same;
    return 0;
}

/* How well a grid explains Beat This!'s detections (pk, s) over the span
 * they share: the F-measure of its beats with a detection within 50 ms and
 * detections with a beat. */
static double evidence(const double *g, int ng, const double *pk, int npk)
{
    const double tol = 0.05;
    if (ng < 8 || npk < 8)
        return 0.0;
    double lo = fmax(g[0], pk[0]) - tol, hi = fmin(g[ng - 1], pk[npk - 1]) + tol;
    int g0 = 0, g1 = ng, p0 = 0, p1 = npk;
    while (g0 < ng && !(g[g0] >= lo))
        g0++;
    while (g1 > g0 && !(g[g1 - 1] <= hi))
        g1--;
    while (p0 < npk && !(pk[p0] >= lo))
        p0++;
    while (p1 > p0 && !(pk[p1 - 1] <= hi))
        p1--;
    double p = agree(g + g0, g1 - g0, pk + p0, p1 - p0, tol);
    double r = agree(pk + p0, p1 - p0, g + g0, g1 - g0, tol);
    return p + r ? 2 * p * r / (p + r) : 0.0;
}

/* The grid as cyanbox's beats c, each with the tempo of its interval. */
static int take(const double *c, int nc, Beat **g, int *ng)
{
    Beat *b = malloc((size_t)CB_MAX(nc, 1) * sizeof(*b));
    if (!b)
        return CB_ERROR(ENOMEM);
    for (int i = 0; i < nc; i++) {
        double d = i + 1 < nc ? c[i + 1] - c[i] : nc > 1 ? c[nc - 1] - c[nc - 2] : 0.5;
        b[i] = (Beat) { c[i], 60 / fmax(d, 1e-3), 0 };
    }
    free(*g);
    *g = b;
    *ng = nc;
    return 0;
}

/* -------------------------------------------------------------- the bar */

/* The bar per section: the beat place where the downbeat activation adds up
 * most. */
static void downbeats(const Beat *g, int ng, const float *d, int n, int *num)
{
    for (int i = 0; i < ng;) {
        int e = i;
        while (e < ng && g[e].sec == g[i].sec)
            e++;
        /* (a section's beats are consecutive: grid_of makes them so) */
        double sums[4] = { 0 };
        for (int k = i; k < e; k++)
            sums[(k - i) % 4] += at(d, n, (g[k].t + LAG) * FPS);
        int r = 0;
        for (int q = 1; q < 4; q++) {
            if (sums[q] > sums[r])
                r = q;
        }
        for (int k = i; k < e; k++)
            num[k] = (((k - i) - r) % 4 + 4) % 4 + 1;
        i = e;
    }
}

/* ---------------------------------------------------------------- a run */

typedef struct Run {
    const float *a, *d;             /* the activations */
    int n;
    const uint8_t *dpk;             /* the downbeat's detections */
    const CBHybridHints *h;
    double last_score[NB_BPM];
} Run;

static int run1(Run *R, double force, double hold, CBBeat **out, int *nb)
{
    const float *a = R->a;
    const int n = R->n;
    const CBHybridHints *h = R->h;
    const double dur = n / FPS;
    *out = NULL;
    *nb = 0;

    Level L;
    double p;
    int ret = level(a, n, &L, &p);
    if (ret < 0)
        return ret;
    memcpy(R->last_score, L.score, sizeof(L.score));
    /* No window with four detections: Beat This! hears no beat, and the
     * tracker would only follow its noise. */
    if (!(score_max(L.score) > 0)) {
        level_free(&L);
        return 0;
    }
    if (force)
        p = fold(60 * FPS / force);
    else
        p = policy(p, L.score, R->dpk, n, h->bpm);

    double *pl = malloc((size_t)n * sizeof(*pl));
    int *trk = NULL, ntrk = 0;
    double *f = NULL, *bt = NULL, *w = NULL, *per = NULL, *kk = NULL;
    Sec *secs = NULL;
    Group *gs = NULL;
    int ns = 0, ngr = 0;
    Beat *g = NULL;
    int ng = 0;
    Vec c = { 0 };
    double *ht = NULL, *c2 = NULL, *pk = NULL;
    int *num = NULL;
    if (!pl) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    /* a level given is held to (its local tempo within 4 %), not left to
     * roam to Beat This!'s */
    if ((ret = local_periods(&L, p, n, force ? hold : 1.25, pl)) < 0 ||
        (ret = track(a, n, pl, &trk, &ntrk)) < 0)
        goto end;
    if (ntrk < 8)
        goto end;
    f = malloc((size_t)ntrk * sizeof(*f));
    bt = malloc((size_t)ntrk * sizeof(*bt));
    w = malloc((size_t)ntrk * sizeof(*w));
    per = malloc((size_t)ntrk * sizeof(*per));
    kk = malloc((size_t)ntrk * sizeof(*kk));
    if (!f || !bt || !w || !per || !kk) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    bool f32;
    refine(a, n, trk, ntrk, f, &f32);
    for (int i = 0; i < ntrk; i++) {
        bt[i] = f32 ? (double)((float)f[i] / (float)FPS) : f[i] / FPS;
        w[i] = at(a, n, f[i]);
        per[i] = pl[CB_CLAMP((int)f[i], 0, n - 1)] / FPS;
    }
    int nbt = numbered(bt, w, per, ntrk, f32, kk);
    if (nbt < 8)
        goto end;
    if ((ret = sections(bt, w, kk, nbt, f32, &secs, &ns)) < 0 ||
        (ret = bridge(secs, ns, bt, w, kk, nbt, &gs, &ngr)) < 0)
        goto end;
    refine_cb(gs, ngr, dur, h->beats, h->nb_beats);
    if ((ret = grid_of(gs, ngr, dur, &g, &ng)) < 0)
        goto end;

    const int ncb = h->nb_beats;
    bool took = false, ok = false;
    ht = malloc((size_t)CB_MAX(ng, 1) * sizeof(*ht));
    if (!ht) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < ng; i++)
        ht[i] = g[i].t;
    if (ncb >= 8) {
        /* cyanbox's grid where it hears what Beat This! hears, beat for
         * beat: its timing is steadier than Beat This!'s on the tracks where
         * the latter's wobbles (at the level chosen: cyanbox's octave above
         * or below, made this one's) */
        if ((ret = at_level(h->beats, ncb, ht, ng, &c, &ok)) < 0)
            goto end;
        if (ok && agree(ht, ng, c.v, c.n, 0.035) >= AGREE && agree(c.v, c.n, ht, ng, 0.035) >= AGREE) {
            if ((ret = take(c.v, c.n, &g, &ng)) < 0)
                goto end;
            took = true;
        }
    }
    /* cyanbox's tempo at the phase Beat This! hears, when that is one place
     * all through: cyanbox's grid off the beat (an off-beat bass taken for
     * the beat), but its tempo right. Not a grid with a passage cyanbox
     * measured beat by beat: its least sure, and a wobble this would keep. */
    if (ncb >= 8 && !took && !h->measured && ok) {
        /* cyanbox unsure of its phase (a low margin, a flag): Beat This!
         * moves it from 25 ms off; sure, only from a quarter of a beat (the
         * errors it makes, off-beats, are bigger; Beat This!'s own beats
         * have been 49 ms late on a track cyanbox had right) */
        Shift o = { .sure = !h->unsure };
        o.small_ms = h->unsure ? 25.0
                   : 0.25 * 60000 / (c.n > 1 ? 60 / median_diff(c.v, c.n) : 120);
        c2 = malloc((size_t)c.n * sizeof(*c2));
        if (!c2) {
            ret = CB_ERROR(ENOMEM);
            goto end;
        }
        bool same;
        int r = shift_sections(c.v, c.n, ht, ng, &o, c2, &same);
        if (r < 0) {
            ret = r;
            goto end;
        }
        if (!r && !same) {
            /* moved: unless in fact no beat moved */
            same = !memcmp(c2, c.v, (size_t)c.n * sizeof(*c2));
        }
        if (!r && (same || agree(ht, ng, c2, c.n, 0.035) >= AGREE - 0.1)) {
            if ((ret = take(c2, c.n, &g, &ng)) < 0)
                goto end;
            took = true;
        }
    }
    if (ncb >= 8 && !took && ok) {
        /* cyanbox's grid, at the level chosen, where it explains Beat This!'s
         * detections as well as this one does (within 0.02): its timing is
         * steadier than Beat This!'s */
        uint8_t *apk = malloc((size_t)n);
        pk = malloc((size_t)n * sizeof(*pk));
        if (!apk || !pk) {
            free(apk);
            ret = CB_ERROR(ENOMEM);
            goto end;
        }
        peak_train(a, n, apk);
        int npk = 0;
        for (int i = 0; i < n; i++) {
            if (apk[i])
                pk[npk++] = i / FPS - LAG;
        }
        free(apk);
        if (evidence(c.v, c.n, pk, npk) >= evidence(ht, ng, pk, npk) - 0.02) {
            if ((ret = take(c.v, c.n, &g, &ng)) < 0)
                goto end;
        }
    }

    num = malloc((size_t)CB_MAX(ng, 1) * sizeof(*num));
    CBBeat *res = malloc((size_t)CB_MAX(ng, 1) * sizeof(*res));
    if (!num || !res) {
        free(res);
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    /* the downbeat activation is on Beat This!'s timeline */
    downbeats(g, ng, R->d, n, num);
    for (int i = 0; i < ng; i++)
        res[i] = (CBBeat) { .time = g[i].t, .number = num[i], .bpm = g[i].bpm };
    *out = res;
    *nb = ng;
    ret = 0;

end:
    level_free(&L);
    free(pl);
    free(trk);
    free(f);
    free(bt);
    free(w);
    free(per);
    free(kk);
    free(secs);
    groups_free(gs, ngr);
    free(g);
    free(c.v);
    free(ht);
    free(c2);
    free(pk);
    free(num);
    return ret;
}

int cb_hybrid_beats(const float *beat, const float *downbeat, int nb_frames,
                    const CBHybridHints *h, CBBeat **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    if (nb_frames < 3)
        return 0;
    const int n = nb_frames;
    float *a = malloc((size_t)n * sizeof(*a)), *d = malloc((size_t)n * sizeof(*d));
    uint8_t *dpk = malloc((size_t)n);
    if (!a || !d || !dpk) {
        free(a);
        free(d);
        free(dpk);
        return CB_ERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++) {
        a[i] = 1.0f / (1.0f + expf(-beat[i]));
        d[i] = 1.0f / (1.0f + expf(-downbeat[i]));
    }
    peak_train(d, n, dpk);
    Run R = { .a = a, .d = d, .n = n, .dpk = dpk, .h = h };
    int ret = run1(&R, 0.0, 1.25, out, nb);
    /* cyanbox's tempo where it is no octave of this one but explains Beat
     * This!'s detections as well (to 0.05): the fitting can settle on a
     * tempo that is not Beat This!'s best. Held to cyanbox's tempo where its
     * grid is one steady tempo; where it has sections (a song that changes
     * tempo) from there, free to follow them. */
    if (ret >= 0 && h->bpm && *nb > 0) {
        /* the most common tempo, to 0.01 BPM, the first of equals */
        const int m = *nb;
        double *v = malloc((size_t)m * sizeof(*v));
        if (!v) {
            ret = CB_ERROR(ENOMEM);
        } else {
            for (int i = 0; i < m; i++)
                v[i] = rint((*out)[i].bpm * 100.0) / 100.0;
            double main = v[0];
            int best = 0;
            for (int i = 0; i < m; i++) {
                bool seen = false;
                for (int k = 0; k < i && !seen; k++)
                    seen = v[k] == v[i];
                if (seen)
                    continue;
                int cnt = 0;
                for (int k = i; k < m; k++)
                    cnt += v[k] == v[i];
                if (cnt > best) {
                    best = cnt;
                    main = v[i];
                }
            }
            free(v);
            if (!octave_of(h->bpm, main) &&
                level_score(R.last_score, h->bpm) >= level_score(R.last_score, main) - 0.05) {
                free(*out);
                *out = NULL;
                *nb = 0;
                ret = run1(&R, h->bpm, h->steady ? 1.04 : 1.25, out, nb);
            }
        }
    }
    if (ret < 0) {
        free(*out);
        *out = NULL;
        *nb = 0;
    }
    free(a);
    free(d);
    free(dpk);
    return ret;
}

/* -------------------------------------------------------------- the grid */

/* The beats as a grid: each run of eight or more on one line, their count
 * going on, is a straight section from its first; any other beat is a
 * measured one with the tempo of its interval. A beat not after the one
 * before it (runs of cyanbox's grid moved apart can overlap) goes. */
static int to_grid(const CBBeat *b, int n, CBGrid *g)
{
    memset(g, 0, sizeof(*g));
    g->beats_per_bar = 4;
    if (!n)
        return 0;
    g->sections = calloc(n, sizeof(*g->sections));
    if (!g->sections)
        return CB_ERROR(ENOMEM);
    int ns = 0;
    double last = -INFINITY;
    for (int i = 0; i < n;) {
        if (!(b[i].time > last)) {
            i++;
            continue;
        }
        int e = i + 1;
        if (e < n && b[e].number == b[i].number % 4 + 1) {
            double p = b[e].time - b[i].time;
            e++;
            while (e < n && b[e].number == b[e - 1].number % 4 + 1 &&
                   fabs(b[e].time - (b[i].time + (e - i) * p)) < 1e-6)
                e++;
        }
        CBTempoSection *s = &g->sections[ns++];
        if (e - i >= 8) {
            double p = (b[e - 1].time - b[i].time) / (e - 1 - i);
            *s = (CBTempoSection) {
                .start = b[i].time, .bpm = CB_CLAMP(60.0 / p, 1.0, 1000.0), .beat = b[i].number,
            };
            last = b[e - 1].time;
            i = e;
            continue;
        }
        double d = i + 1 < n ? b[i + 1].time - b[i].time : i > 0 ? b[i].time - b[i - 1].time : 0.5;
        *s = (CBTempoSection) {
            .start    = b[i].time,
            .bpm      = CB_CLAMP(60.0 / fmax(d, 1e-3), 1.0, 1000.0),
            .beat     = b[i].number,
            .measured = true,
        };
        last = b[i].time;
        i++;
    }
    g->nb_sections = ns;
    return 0;
}

int cb_beats_hybrid(const float *beat, const float *downbeat, int nb_frames, double duration,
                    CBBeatResult *res)
{
    /* As cyanbox's own: no grid for under 8 s. */
    const unsigned own = res->flags;
    if (duration < 8.0 || nb_frames < 3)
        return 0;

    /* The hints: cyanbox's tempo, its beats, whether it was unsure of its
     * phase (a flag, a margin under 10), one straight section, a measured
     * passage. */
    CBHybridHints h = { .unsure = own != 0 || res->phase_margin < 10 };
    CBBeat *cb = NULL;
    int ncb = 0, ret;
    double *times = NULL;
    if (cb_grid_valid(&res->grid)) {
        if ((ret = cb_grid_beats(&res->grid, duration, &cb, &ncb)) < 0)
            return ret;
        h.bpm = cb_grid_bpm_median(&res->grid, duration);
        h.steady = res->grid.nb_sections == 1 && !res->grid.sections[0].measured;
        h.measured = cb_grid_dynamic(&res->grid);
    }
    times = malloc((size_t)CB_MAX(ncb, 1) * sizeof(*times));
    if (!times) {
        free(cb);
        return CB_ERROR(ENOMEM);
    }
    for (int i = 0; i < ncb; i++)
        times[i] = cb[i].time;
    free(cb);
    h.beats = times;
    h.nb_beats = ncb;

    CBBeat *b = NULL;
    int nb = 0;
    ret = cb_hybrid_beats(beat, downbeat, nb_frames, &h, &b, &nb);
    free(times);
    if (ret < 0)
        return ret;
    /* Beat This! heard too little to make a grid of: cyanbox's stands. */
    if (!nb) {
        free(b);
        return 0;
    }
    CBGrid g;
    if ((ret = to_grid(b, nb, &g)) < 0) {
        free(b);
        return ret;
    }
    free(b);
    /* No grid in the silence before the music, as cyanbox's. */
    if (g.nb_sections && res->start_kind != CB_START_AT_ONCE &&
        (ret = cb_grid_start_at(&g, fmax(res->music_start - CB_BEAT_START_SLACK, 0.0))) < 0) {
        cb_grid_free(&g);
        return ret;
    }

    /* The confidence: how well the grid explains Beat This!'s detections,
     * the F-measure of beats and detections within 50 ms of each other. */
    double conf = 0.0;
    CBBeat *gb = NULL;
    int ngb = 0;
    if (g.nb_sections && (ret = cb_grid_beats(&g, duration, &gb, &ngb)) < 0) {
        cb_grid_free(&g);
        return ret;
    }
    double *gt = malloc((size_t)CB_MAX(ngb, 1) * sizeof(*gt));
    double *pk = malloc((size_t)nb_frames * sizeof(*pk));
    if (!gt || !pk) {
        free(gb);
        free(gt);
        free(pk);
        cb_grid_free(&g);
        return CB_ERROR(ENOMEM);
    }
    for (int i = 0; i < ngb; i++)
        gt[i] = gb[i].time;
    free(gb);
    int npk = 0;
    for (int i = 1; i + 1 < nb_frames; i++) {
        if (beat[i] > beat[i - 1] && beat[i] >= beat[i + 1] && beat[i] > 0.0f)
            pk[npk++] = i / FPS - LAG;
    }
    conf = evidence(gt, ngb, pk, npk);
    free(gt);
    free(pk);

    /* The flags: the grid's own shape, and of cyanbox's those that say
     * something of the music (its others were of its own choices). */
    unsigned flags = own & CB_GRID_FLAG_UNSTEADY;
    for (int i = 0; i < g.nb_sections; i++) {
        const CBTempoSection *s = &g.sections[i];
        if (s->measured) {
            flags |= CB_GRID_FLAG_DYNAMIC;
            continue;
        }
        if (i && !g.sections[i - 1].measured)
            flags |= fabs(s->bpm / g.sections[i - 1].bpm - 1) < 0.005 ? CB_GRID_FLAG_PHASE_CUT
                                                                     : CB_GRID_FLAG_TEMPO_CHANGE;
    }
    if (!g.nb_sections || conf < 0.5)
        flags |= CB_GRID_FLAG_LOW_CONFIDENCE;

    cb_grid_free(&res->grid);
    res->grid = g;
    res->confidence = conf;
    res->flags = flags;
    return 0;
}
