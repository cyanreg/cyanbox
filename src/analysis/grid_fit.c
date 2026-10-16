/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The grid is fitted, not tracked: a beat tracker finds where the beats
 * roughly are, every beat is then put on the attack of its hit, and
 * straight lines are fitted through the attacks. A line through hundreds of
 * attacks measured to a third of a millisecond fixes the period to well
 * under 0.001 BPM and the phase to a fraction of a millisecond, which is
 * what a grid must do to stay on the beat for six minutes (0.05 BPM of
 * error is a beat and a half of drift by the end).
 *
 * Stages here:
 *  - cb_track_beats: Ellis's dynamic-programming tracker. It finds the
 *    sequence of onsets best explained by beats about one period apart,
 *    over the whole track at once, so it is not thrown by a fill or a
 *    breakdown the way a causal tracker is, and it follows a slowly moving
 *    tempo (a live drummer) without being told how.
 *  - cb_measure_attacks: the nearest strong rise of a fine band envelope,
 *    timed where the band's energy is halfway up the rise. Nearest rather
 *    than strongest (rbxport): a window reaching for the strongest rise
 *    lets a line chase a clap after the kick.
 *  - cb_fit_line: a Theil-Sen start (half the attacks may be wrong without
 *    moving it), then weighted least squares with Tukey's biweight, so a
 *    missed kick or a ghost note pulls the line by nothing at all.
 *  - cb_fold: the average beat (dj's fold) in each band; the start of its
 *    hit is the phase, and the bands say where the hit starts (the click,
 *    not the slower body of the kick).
 *  - cb_segment: lines only where the evidence wants more than one.
 *  - cb_snap_section: integer BPM when it costs no audible drift. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------- tracker */

int cb_track_beats(const CBOnsets *o, const float *period_s, double tightness,
                   CBBeatObs **out, int *nb_out)
{
    *out = NULL;
    *nb_out = 0;
    const int n = o->nb;
    const double fps = o->fps;
    if (n < 4)
        return 0;

    double *score = malloc((size_t)n * sizeof(*score));
    int *back = malloc((size_t)n * sizeof(*back));
    double *pen = NULL;
    int pen_len = 0;
    double pen_period = -1.0;
    if (!score || !back) {
        free(score);
        free(back);
        return CB_ERROR(ENOMEM);
    }

    /* score[t] = onset[t] + max over predecessors tau in [t - 2P, t - P/2]
     * of score[tau] - tightness * log((t - tau) / P)^2. The penalty depends
     * only on (t - tau) / P, so it is tabulated per period. */
    for (int t = 0; t < n; t++) {
        double P = period_s[t] * fps;
        if (P != pen_period) {
            int len = (int)(2.0 * P) + 2;
            if (len > pen_len) {
                double *np = realloc(pen, (size_t)len * sizeof(*np));
                if (!np) {
                    free(score);
                    free(back);
                    free(pen);
                    return CB_ERROR(ENOMEM);
                }
                pen = np;
                pen_len = len;
            }
            for (int d = 1; d < len; d++) {
                double l = log(d / P);
                pen[d] = -tightness * l * l;
            }
            pen_period = P;
        }
        int dmin = CB_MAX((int)lrint(P / 2), 1), dmax = (int)(2.0 * P);
        double best = -INFINITY;
        int arg = -1;
        for (int d = dmin; d <= dmax && t - d >= 0; d++) {
            double v = score[t - d] + pen[d];
            if (v > best) {
                best = v;
                arg = t - d;
            }
        }
        /* The first beat can be anywhere within the first two periods. */
        if (arg < 0)
            best = 0.0;
        score[t] = o->accent[t] + best;
        back[t] = arg;
    }

    /* The last beat: the best score within the last period. */
    double Pend = period_s[n - 1] * fps;
    int last = n - 1;
    for (int t = n - 1; t >= CB_MAX(0, n - 1 - (int)Pend); t--) {
        if (score[t] > score[last])
            last = t;
    }
    int count = 0;
    for (int t = last; t >= 0; t = back[t])
        count++;
    CBBeatObs *b = calloc(CB_MAX(count, 1), sizeof(*b));
    if (!b) {
        free(score);
        free(back);
        free(pen);
        return CB_ERROR(ENOMEM);
    }
    int i = count;
    for (int t = last; t >= 0; t = back[t])
        b[--i].time = t / fps;
    free(score);
    free(back);
    free(pen);

    /* Beat counts: consecutive beats are one apart unless the tracker
     * skipped one (an interval near two periods). */
    for (i = 0; i < count; i++) {
        b[i].attack = NAN;
        b[i].weight = 0.0;
        int f = CB_CLAMP((int)lrint(b[i].time * fps), 0, n - 1);
        double k = 0.0;
        for (int j = CB_MAX(f - 2, 0); j <= CB_MIN(f + 2, n - 1); j++)
            k = fmax(k, o->low[j]);
        b[i].kick = k;
        if (i) {
            double P = period_s[f];
            int step = (int)lrint((b[i].time - b[i - 1].time) / P);
            b[i].index = b[i - 1].index + CB_MAX(step, 1);
        }
    }
    *out = b;
    *nb_out = count;
    return 0;
}

int cb_track_beats_range(const CBOnsets *o, const float *period_s, double tightness,
                         int f0, int f1, CBBeatObs **out, int *nb_out)
{
    *out = NULL;
    *nb_out = 0;
    f0 = CB_CLAMP(f0, 0, o->nb - 1);
    f1 = CB_CLAMP(f1, f0, o->nb - 1);
    /* A view of the onsets from f0: the tracker reads the accent and the
     * kick band by frame. */
    CBOnsets v = *o;
    v.nb = f1 - f0 + 1;
    v.accent = o->accent + f0;
    v.low = o->low + f0;
    int ret = cb_track_beats(&v, period_s + f0, tightness, out, nb_out);
    for (int i = 0; ret >= 0 && i < *nb_out; i++)
        (*out)[i].time += f0 / o->fps;
    return ret;
}

/* --------------------------------------------------------- envelopes */

int cb_beat_envs(CBEnvCtx *ctx, const CBBeatObs *beats, int nb, CBBeatEnvs *out)
{
    memset(out, 0, sizeof(*out));
    out->env = malloc((size_t)CB_MAX(nb, 1) * CB_NB_BANDS * CB_ENV_LEN * sizeof(*out->env));
    out->centre = malloc((size_t)CB_MAX(nb, 1) * sizeof(*out->centre));
    if (!out->env || !out->centre) {
        cb_beat_envs_free(out);
        return CB_ERROR(ENOMEM);
    }
    out->nb = nb;
    /* All at once: on the GPU a window is one workgroup of a batch. */
    double *t = malloc((size_t)CB_MAX(nb, 1) * sizeof(*t));
    if (!t) {
        cb_beat_envs_free(out);
        return CB_ERROR(ENOMEM);
    }
    for (int i = 0; i < nb; i++)
        t[i] = beats[i].time;
    int ret = cb_env_windows(ctx, t, nb, out->env, out->centre);
    free(t);
    if (ret < 0)
        cb_beat_envs_free(out);
    return ret;
}

void cb_beat_envs_free(CBBeatEnvs *e)
{
    free(e->env);
    free(e->centre);
    memset(e, 0, sizeof(*e));
}

double cb_beat_predict(const CBBeatObs *beats, int i, const CBFitSection *s)
{
    if (!s)
        return beats[i].time;
    return s->t0 + (beats[i].index - s->index0) * s->period;
}

/* ------------------------------------------------------------ attacks */

/* The rise of envelope e at sample i, over +-10 ms: how much louder the
 * band is just after than just before. */
static double rise_height(const float *e, int i)
{
    const int K = (int)(0.010 * CB_ENV_RATE);
    double lo = INFINITY, hi = -INFINITY;
    for (int j = CB_MAX(i - K, 0); j <= i; j++)
        lo = fmin(lo, e[j]);
    for (int j = i; j <= CB_MIN(i + K, CB_ENV_LEN - 1); j++)
        hi = fmax(hi, e[j]);
    return fmax(hi - lo, 0.0);
}

/* Where a rise starts, to a fraction of a sample: the instant the band's
 * energy crosses halfway between its level before the rise and the level
 * it reaches (`k` is the steepest point of the rise in the amplitude
 * envelope e[0..n), `w` the samples either side to look for those levels).
 * For a step of energy under symmetric smoothing this is exactly the step;
 * a click's few-ms rise puts it within a millisecond of the first sample
 * of the hit. Returns the position in samples of e. */
static double half_energy(const double *e, int n, int k, int w)
{
    int a = k, b = k;
    for (int j = CB_MAX(k - w, 0); j <= k; j++) {
        if (e[j] < e[a])
            a = j;
    }
    for (int j = k; j <= CB_MIN(k + w, n - 1); j++) {
        if (e[j] > e[b])
            b = j;
    }
    double target = 0.5 * (e[a] * e[a] + e[b] * e[b]);
    for (int j = a; j < b; j++) {
        double e0 = e[j] * e[j], e1 = e[j + 1] * e[j + 1];
        if (e0 <= target && e1 >= target)
            return e1 > e0 ? j + (target - e0) / (e1 - e0) : j;
    }
    return k;
}

void cb_measure_attacks(const CBBeatEnvs *e, CBBeatObs *beats, int first, int last,
                        int band, const CBFitSection *s, double tol)
{
    for (int i = first; i <= last; i++) {
        CBBeatObs *b = &beats[i];
        b->attack = NAN;
        b->weight = 0.0;
        const float *env = e->env + ((size_t)i * CB_NB_BANDS + band) * CB_ENV_LEN;
        double pred = cb_beat_predict(beats, i, s);
        double rel = (pred - e->centre[i]) * CB_ENV_RATE + CB_ENV_HALF;
        int lo = (int)ceil(rel - tol * CB_ENV_RATE), hi = (int)floor(rel + tol * CB_ENV_RATE);
        lo = CB_MAX(lo, 2);
        hi = CB_MIN(hi, CB_ENV_LEN - 3);
        if (hi <= lo)
            continue;

        /* Central differences; candidates are local maxima of the rise at
         * least half as steep as the steepest one in the window. */
        double dmax = 0.0;
        for (int j = lo; j <= hi; j++)
            dmax = fmax(dmax, env[j + 1] - env[j - 1]);
        if (dmax <= 0.0)
            continue;
        int best = -1;
        double bd = INFINITY;
        for (int j = lo; j <= hi; j++) {
            double d = env[j + 1] - env[j - 1];
            double dl = env[j] - env[j - 2], dr = env[j + 2] - env[j];
            if (d < 0.5 * dmax || d < dl || d < dr)
                continue;
            if (fabs(j - rel) < bd) {
                bd = fabs(j - rel);
                best = j;
            }
        }
        if (best < 0)
            continue;
        double ed[CB_ENV_LEN];
        for (int j = 0; j < CB_ENV_LEN; j++)
            ed[j] = env[j];
        double x = half_energy(ed, CB_ENV_LEN, best, (int)(0.010 * CB_ENV_RATE));
        b->attack = e->centre[i] + (x - CB_ENV_HALF) / CB_ENV_RATE;
        b->weight = rise_height(env, best);
    }
}

/* -------------------------------------------------------------- line fit */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static bool supported(const CBBeatObs *b)
{
    return b->weight > 0.0 && !isnan(b->attack);
}

/* Weighted least squares line t = a + b * (k - k0). */
static bool wls(const CBBeatObs *beats, int first, int last, int k0, const double *w,
                double *a, double *b)
{
    double sw = 0, sk = 0, st = 0, skk = 0, skt = 0;
    for (int i = first; i <= last; i++) {
        if (w[i - first] <= 0.0)
            continue;
        double k = beats[i].index - k0, t = beats[i].attack, ww = w[i - first];
        sw += ww;
        sk += ww * k;
        st += ww * t;
        skk += ww * k * k;
        skt += ww * k * t;
    }
    if (sw <= 0.0)
        return false;
    double det = sw * skk - sk * sk;
    if (det <= 1e-12 * sw * sw)
        return false;
    *b = (sw * skt - sk * st) / det;
    *a = (st - *b * sk) / sw;
    return true;
}

/* Theil-Sen: the median slope over pairs of supported beats half the
 * range apart, then the median intercept. Up to half the attacks can be
 * wrong (a stretch tracked on the off-beat) without moving it. 1 when
 * there is a line, 0 when the attacks give none, or an error. */
static int theil_sen(const CBBeatObs *beats, int first, int last, int k0, double *a, double *b)
{
    int m = 0;
    for (int i = first; i <= last; i++)
        m += supported(&beats[i]);
    if (m < 2)
        return 0;
    int *sup = malloc(m * sizeof(*sup));
    double *v = malloc(m * sizeof(*v));
    if (!sup || !v) {
        free(sup);
        free(v);
        return CB_ERROR(ENOMEM);
    }
    m = 0;
    for (int i = first; i <= last; i++) {
        if (supported(&beats[i]))
            sup[m++] = i;
    }
    int h = CB_MAX(m / 2, 1), nv = 0;
    for (int j = 0; j + h < m; j++) {
        const CBBeatObs *p = &beats[sup[j]], *q = &beats[sup[j + h]];
        int dk = q->index - p->index;
        if (dk > 0)
            v[nv++] = (q->attack - p->attack) / dk;
    }
    bool ok = nv > 0;
    if (ok) {
        qsort(v, nv, sizeof(*v), cmp_double);
        *b = v[nv / 2];
        for (int j = 0; j < m; j++)
            v[j] = beats[sup[j]].attack - *b * (beats[sup[j]].index - k0);
        qsort(v, m, sizeof(*v), cmp_double);
        *a = v[m / 2];
    }
    free(sup);
    free(v);
    return ok;
}

void cb_recount(CBBeatObs *beats, int first, int last, const CBFitSection *line, bool from_end)
{
    /* The offset of up to 16 attacks nearest the line's side, in beats of
     * the line, median; then rounded. */
    double d[16];
    int n = 0;
    for (int k = 0; k <= last - first && n < 16; k++) {
        int i = from_end ? last - k : first + k;
        if (!supported(&beats[i]))
            continue;
        double kl = line->index0 + (beats[i].attack - line->t0) / line->period;
        d[n++] = kl - beats[i].index;
    }
    if (!n) {
        /* No attacks: the tracked times will do. */
        int i = from_end ? last : first;
        d[n++] = line->index0 + (beats[i].time - line->t0) / line->period - beats[i].index;
    }
    qsort(d, n, sizeof(*d), cmp_double);
    int shift = (int)lrint(d[n / 2]);
    for (int i = first; i <= last; i++)
        beats[i].index += shift;
}

int cb_fit_line(const CBBeatObs *beats, int first, int last, const CBFitSection *init,
                CBFitSection *s)
{
    int n = last - first + 1;
    if (n <= 0)
        return CB_ERROR(EINVAL);

    double *w = malloc((size_t)n * sizeof(*w));
    double *r = malloc((size_t)n * sizeof(*r));
    if (!w || !r) {
        free(w);
        free(r);
        return CB_ERROR(ENOMEM);
    }
    s->first = first;
    s->last = last;
    s->index0 = beats[first].index;
    s->dynamic = false;

    int sup = 0;
    for (int i = first; i <= last; i++)
        sup += supported(&beats[i]);
    s->supported = sup;

    double a = 0.0, b = 0.0, sigma = 0.0;
    int ok;
    if (init) {
        a = init->t0 + (s->index0 - init->index0) * init->period;
        b = init->period;
        ok = sup >= 2;
    } else {
        ok = theil_sen(beats, first, last, s->index0, &a, &b);
    }
    if (ok <= 0) {
        free(w);
        free(r);
        return ok < 0 ? ok : CB_ERROR(EINVAL);
    }

    /* Tukey biweight IRLS: residuals beyond 4.685 robust sigmas get no
     * weight. Sigma has a floor of 1 ms so that a near-perfect electronic
     * track does not reject attacks for being half a millisecond out.
     * Started from a trusted line, the scale is that line's own and stays
     * fixed (an MM-estimate): when most of a merged stretch disagrees with
     * it, a scale re-estimated from all residuals would grow until nothing
     * is an outlier and the line would leave the beats it was fitted on. */
    double fixed = init ? fmax(init->jitter, 0.001) : 0.0;
    for (int it = 0; it < 8; it++) {
        if (init) {
            sigma = fixed;
        } else {
            int m = 0;
            for (int i = first; i <= last; i++) {
                if (supported(&beats[i]))
                    r[m++] = fabs(beats[i].attack - (a + b * (beats[i].index - s->index0)));
            }
            qsort(r, m, sizeof(*r), cmp_double);
            sigma = fmax(1.4826 * r[m / 2], 0.001);
        }
        double c = 4.685 * sigma;
        for (int i = first; i <= last; i++) {
            if (!supported(&beats[i])) {
                w[i - first] = 0.0;
                continue;
            }
            double u = (beats[i].attack - (a + b * (beats[i].index - s->index0))) / c;
            w[i - first] = fabs(u) < 1.0 ? beats[i].weight * (1 - u * u) * (1 - u * u) : 0.0;
        }
        double na, nb;
        if (!wls(beats, first, last, s->index0, w, &na, &nb))
            break;
        a = na;
        b = nb;
    }
    s->t0 = a;
    s->period = b;

    /* Weighted RMS of the inliers, and the jitter: the robust spread
     * (MAD) of the inliers' residuals, the scale a later fit started from
     * this line keeps. Outliers stay out of it: a merged section that
     * carried its junk attacks' spread into the next merge would accept
     * everything there. */
    double sw = 0.0, s2 = 0.0;
    int m = 0;
    for (int i = first; i <= last; i++) {
        if (!supported(&beats[i]) || w[i - first] <= 0.0)
            continue;
        double res = beats[i].attack - (a + b * (beats[i].index - s->index0));
        r[m++] = fabs(res);
        sw += w[i - first];
        s2 += w[i - first] * res * res;
    }
    qsort(r, m, sizeof(*r), cmp_double);
    s->rms = sw > 0.0 ? sqrt(s2 / sw) : 0.0;
    s->jitter = m ? 1.4826 * r[m / 2] : 0.0;
    free(w);
    free(r);
    return b > 0.0 ? 0 : CB_ERROR(EINVAL);
}

/* ---------------------------------------------------------------- fold */

#define FOLD_BIN   0.00025          /* 0.25 ms */
#define FOLD_HALF  200              /* +-50 ms */
#define FOLD_LEN   (2 * FOLD_HALF + 1)

int cb_fold(const CBBeatEnvs *e, const CBBeatObs *beats, int first, int last,
            const CBFitSection *line, int band, double search,
            double *contrast, double *offset, double *rise)
{
    double fold[CB_NB_BANDS][FOLD_LEN], cnt[CB_NB_BANDS][FOLD_LEN];
    memset(fold, 0, sizeof(fold));
    memset(cnt, 0, sizeof(cnt));

    /* Every envelope sample of every beat of the section, binned by its
     * offset from the line's prediction: the average beat. */
    for (int i = first; i <= last; i++) {
        double pred = cb_beat_predict(beats, i, line);
        for (int b = 0; b < CB_NB_BANDS; b++) {
            const float *env = e->env + ((size_t)i * CB_NB_BANDS + b) * CB_ENV_LEN;
            for (int j = 0; j < CB_ENV_LEN; j++) {
                double t = e->centre[i] + (double)(j - CB_ENV_HALF) / CB_ENV_RATE;
                int k = (int)lrint((t - pred) / FOLD_BIN) + FOLD_HALF;
                if (k < 0 || k >= FOLD_LEN)
                    continue;
                fold[b][k] += env[j];
                cnt[b][k] += 1.0;
            }
        }
    }

    /* Average, then smooth over +-1 ms: the bins are finer than the
     * envelope's samples, so some are empty or single. Per band: the
     * contrast (dj: how much the average beat rises and falls relative to
     * its level), the steepest rise within +-search, and its height: from
     * the lowest level within the search before its steepest point to the
     * highest within the search after it, relative to the level as the
     * contrast is. As far as the search, not half_energy's 10 ms: a
     * drummer's scattered hits (test_beats' 8 ms of jitter) average to a
     * rise some 30 ms long. A decaying tail has no height however far it
     * is read: its lowest level before a ripple and its highest after it
     * are the ripple's own. */
    double sm[CB_NB_BANDS][FOLD_LEN], con[CB_NB_BANDS], pos[CB_NB_BANDS], hgt[CB_NB_BANDS];
    bool rises[CB_NB_BANDS];
    const int S = (int)(search / FOLD_BIN);
    int lo = CB_MAX(FOLD_HALF - S, 1);
    int hi = CB_MIN(FOLD_HALF + S, FOLD_LEN - 2);
    for (int b = 0; b < CB_NB_BANDS; b++) {
        for (int k = 0; k < FOLD_LEN; k++)
            fold[b][k] = cnt[b][k] > 0 ? fold[b][k] / cnt[b][k] : NAN;
        const int K = (int)(0.001 / FOLD_BIN);
        double mn = INFINITY, mx = -INFINITY, mean = 0.0;
        for (int k = 0; k < FOLD_LEN; k++) {
            double acc = 0.0;
            int c = 0;
            for (int j = CB_MAX(k - K, 0); j <= CB_MIN(k + K, FOLD_LEN - 1); j++) {
                if (!isnan(fold[b][j])) {
                    acc += fold[b][j];
                    c++;
                }
            }
            sm[b][k] = c ? acc / c : 0.0;
            mn = fmin(mn, sm[b][k]);
            mx = fmax(mx, sm[b][k]);
            mean += sm[b][k];
        }
        mean /= FOLD_LEN;
        con[b] = (mx - mn) / (mean + 1e-9);
        int arg = FOLD_HALF;
        double bd = -INFINITY;
        for (int k = lo; k <= hi; k++) {
            double d = sm[b][k + 1] - sm[b][k - 1];
            if (d > bd) {
                bd = d;
                arg = k;
            }
        }
        rises[b] = bd > 0.0;
        double x = half_energy(sm[b], FOLD_LEN, arg, (int)(0.010 / FOLD_BIN));
        pos[b] = (x - FOLD_HALF) * FOLD_BIN;
        double r0 = sm[b][arg], r1 = sm[b][arg];
        for (int k = CB_MAX(arg - S, 0); k <= arg; k++)
            r0 = fmin(r0, sm[b][k]);
        for (int k = arg; k <= CB_MIN(arg + S, FOLD_LEN - 1); k++)
            r1 = fmax(r1, sm[b][k]);
        hgt[b] = rises[b] ? (r1 - r0) / (mean + 1e-9) : 0.0;
    }

    /* Which band times the beat. Not simply the one with the most
     * contrast (dj). Below 150 Hz the kick shares the band with the bass,
     * whose notes can start before the kick (a rolling techno bass), and
     * the kick's body needs most of a cycle of its 50 Hz fundamental to
     * build, so a low-band grid sits 5-15 ms after the click that starts
     * the hit (measured with scipy's filters on real tracks). The click
     * bands time the hit; the low band only when neither shows the beat.
     * Between the click bands, the earlier clear rise: a kick's knock
     * around 1 kHz can lead its top end by a millisecond or two. "Clear":
     * contrast at least 0.4 of the best band's and above 0.15. */
    int best = band;
    if (best < 0) {
        int top = 0;
        for (int b = 1; b < CB_NB_BANDS; b++) {
            if (con[b] > con[top])
                top = b;
        }
        best = -1;
        for (int b = CB_BAND_MID; b <= CB_BAND_HIGH; b++) {
            if (!rises[b] || con[b] < 0.15 || con[b] < 0.4 * con[top])
                continue;
            if (best < 0 || pos[b] < pos[best])
                best = b;
        }
        if (best < 0)
            best = top;
    }
    if (contrast)
        *contrast = con[best];
    if (offset)
        *offset = rises[best] ? pos[best] : 0.0;
    if (rise)
        *rise = hgt[best];
    return best;
}

/* -------------------------------------------------------------- snapping */

bool cb_snap_section(const CBBeatObs *beats, CBFitSection *s, double tol)
{
    /* The beats the line was fitted on span [k_lo, k_hi]; re-phased around
     * their weighted centre, a tempo error of d BPM drifts by
     * span/2 * d/bpm at each end. Snap only when that stays within `tol`:
     * producers work at whole (sometimes half) tempos, and the snap
     * removes the last estimation noise without moving any beat audibly
     * (dj's rule, with the drift measured from the centre). */
    double sw = 0.0, sk = 0.0;
    int klo = INT32_MAX, khi = INT32_MIN;
    for (int i = s->first; i <= s->last; i++) {
        if (beats[i].weight <= 0.0 || isnan(beats[i].attack))
            continue;
        sw += beats[i].weight;
        sk += beats[i].weight * (beats[i].index - s->index0);
        klo = CB_MIN(klo, beats[i].index - s->index0);
        khi = CB_MAX(khi, beats[i].index - s->index0);
    }
    if (sw <= 0.0)
        return false;
    double kc = sk / sw, bpm = 60.0 / s->period;
    double half_span = fmax(kc - klo, khi - kc) * s->period;
    static const double steps[] = { 1.0, 0.5 };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(steps); i++) {
        double cand = round(bpm / steps[i]) * steps[i];
        if (half_span * fabs(cand - bpm) / bpm > tol)
            continue;
        /* Keep the line through its weighted centre. */
        double tc = s->t0 + kc * s->period;
        s->period = 60.0 / cand;
        s->t0 = tc - kc * s->period;
        return true;
    }
    return false;
}

/* --------------------------------------------------------- segmentation */

/* The tuning of the sections (docs/BEATS.md explains each). */
#define SEG_MIN_SUPPORTED 16        /* attacks a section needs: four bars */
#define SEG_MIN_BEATS     32        /* and the beats it spans: eight bars */
#define SEG_MIN_PHASE_BEATS 64      /* a new phase at the same tempo: sixteen bars */
#define SEG_MIN_HALF_BEATS  128     /* a new phase half a beat away: thirty-two bars */
#define SEG_MAX_DEV       0.015     /* one line within 15 ms everywhere is one line */
#define SEG_SUPPORT_RATIO 1.25      /* own line vs the neighbour's, on the arbiter band */
#define SEG_PHASE_RATIO   1.5       /* the same for a new phase at the same tempo */
#define SEG_SUPPORT_SPAN  64        /* beats either side of a boundary the test reads */

/* The largest running median (9 attacks) of the residuals from the line:
 * how far the line sits from the music anywhere along it. A median, so a
 * single ghost hit cannot hold a boundary in place. */
static int max_deviation(const CBBeatObs *beats, const CBFitSection *s, double *dev)
{
    int n = s->last - s->first + 1, m = 0;
    double *r = malloc((size_t)CB_MAX(n, 1) * sizeof(*r));
    if (!r)
        return CB_ERROR(ENOMEM);
    for (int i = s->first; i <= s->last; i++) {
        if (supported(&beats[i]))
            r[m++] = beats[i].attack - cb_beat_predict(beats, i, s);
    }
    double worst = 0.0, win[9];
    for (int j = 0; j + 9 <= m; j++) {
        memcpy(win, r + j, sizeof(win));
        qsort(win, 9, sizeof(*win), cmp_double);
        worst = fmax(worst, fabs(win[4]));
    }
    free(r);
    *dev = worst;
    return 0;
}

/* Mean of the support onset at the beats of line s inside [t0, t1]. */
static double line_support(const CBOnsets *o, const float *v, const CBFitSection *s,
                           double t0, double t1)
{
    double acc = 0.0;
    int n = 0;
    long j0 = (long)ceil((t0 - s->t0) / s->period), j1 = (long)floor((t1 - s->t0) / s->period);
    for (long j = j0; j <= j1; j++) {
        double t = s->t0 + j * s->period;
        int c = (int)lrint(t * o->fps);
        double m = 0.0;
        for (int i = c - 2; i <= c + 2; i++) {
            if (i >= 0 && i < o->nb && v[i] > m)
                m = v[i];
        }
        acc += m;
        n++;
    }
    return n ? acc / n : 0.0;
}

typedef struct Seg {
    CBFitSection *s;
    int           nb;
    CBBeatObs    *beats;
} Seg;

/* Recounts sections [i, j] onto section dom's line: each other section's
 * counts shift by the whole number of beats that matches them to the
 * line where they meet (the sections between included, in order outwards
 * from dom). */
static void seg_recount(Seg *g, int i, int j, int dom)
{
    const CBFitSection *line = &g->s[dom];
    for (int k = dom - 1; k >= i; k--)
        cb_recount(g->beats, g->s[k].first, g->s[k].last, line, true);
    for (int k = dom + 1; k <= j; k++)
        cb_recount(g->beats, g->s[k].first, g->s[k].last, line, false);
}

/* Replaces sections [i, j] by one fitted on their union, counted on
 * section dom's line. */
static int seg_merge(Seg *g, int i, int j, int dom)
{
    CBFitSection init = g->s[dom], out = { 0 };
    seg_recount(g, i, j, dom);
    int ret = cb_fit_line(g->beats, g->s[i].first, g->s[j].last, &init, &out);
    if (ret < 0)
        return ret;
    cb_debug("segment: merged %d-%d on %d: %.3f BPM, jitter %.1f ms\n", i, j, dom,
             60.0 / out.period, out.jitter * 1e3);
    g->s[i] = out;
    memmove(g->s + i + 1, g->s + j + 1, (g->nb - j - 1) * sizeof(*g->s));
    g->nb -= j - i;
    return 0;
}

/* The longer of two sections (by attacks): whose line a merge keeps. */
static int seg_dominant(const Seg *g, int i, int j)
{
    return g->s[i].supported >= g->s[j].supported ? i : j;
}

/* Deviation of the line one would get by merging [i, j] on dom's line
 * (INFINITY when the attacks give no line). The beats' counts are restored
 * afterwards. `hide` (or -1): a section whose attacks do not take part
 * (the breakdown between two sections). */
static int merged_deviation(Seg *g, int i, int j, int dom, int hide, double *dev)
{
    int first = g->s[i].first, last = g->s[j].last;
    int nb = last - first + 1;
    int *save = malloc(nb * sizeof(*save));
    double *hw = malloc(nb * sizeof(*hw));
    if (!save || !hw) {
        free(save);
        free(hw);
        return CB_ERROR(ENOMEM);
    }
    for (int k = first; k <= last; k++) {
        save[k - first] = g->beats[k].index;
        hw[k - first] = g->beats[k].weight;
    }
    if (hide >= 0) {
        for (int k = g->s[hide].first; k <= g->s[hide].last; k++)
            g->beats[k].weight = 0.0;
    }
    seg_recount(g, i, j, dom);
    CBFitSection init = g->s[dom], out = { 0 };
    *dev = INFINITY;
    int ret = cb_fit_line(g->beats, first, last, &init, &out);
    if (ret == 0)
        ret = max_deviation(g->beats, &out, dev);
    else if (ret == CB_ERROR(EINVAL))
        ret = 0;
    for (int k = first; k <= last; k++) {
        g->beats[k].index = save[k - first];
        g->beats[k].weight = hw[k - first];
    }
    free(save);
    free(hw);
    return ret;
}

/* Section start and end times (the first and last beat on its line). */
static double seg_t0(const Seg *g, int i)
{
    return cb_beat_predict(g->beats, g->s[i].first, &g->s[i]);
}

static double seg_t1(const Seg *g, int i)
{
    return cb_beat_predict(g->beats, g->s[i].last, &g->s[i]);
}

static double region_bpm(const CBTempoRegion *r, int nb, double t)
{
    for (int i = nb - 1; i >= 0; i--) {
        if (t >= r[i].start)
            return r[i].bpm;
    }
    return nb ? r[0].bpm : 0.0;
}

int cb_segment(CBBeatObs *beats, int nb, const CBOnsets *o, const float *support,
               const CBTempoRegion *regions, int nb_regions, bool allow,
               CBFitSection **secs, int *nb_secs)
{
    *secs = NULL;
    *nb_secs = 0;
    int ret = 0;

    /* Supported beats: attacks at least a quarter of the median strength;
     * weaker ones are noise in a breakdown, not beats. */
    double *ws = malloc((size_t)CB_MAX(nb, 1) * sizeof(*ws));
    int *sup = malloc((size_t)CB_MAX(nb, 1) * sizeof(*sup));
    CBFitSection *s = calloc(CB_MAX(nb, 1), sizeof(*s));
    double *F = NULL;
    int *arg = NULL;
    if (!ws || !sup || !s) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    int m = 0;
    for (int i = 0; i < nb; i++) {
        if (supported(&beats[i]))
            ws[m++] = beats[i].weight;
    }
    if (m < SEG_MIN_SUPPORTED) {
        ret = CB_ERROR(EINVAL);
        goto end;
    }
    qsort(ws, m, sizeof(*ws), cmp_double);
    double wmed = ws[m / 2];
    m = 0;
    for (int i = 0; i < nb; i++) {
        if (supported(&beats[i]) && beats[i].weight < 0.25 * wmed)
            beats[i].weight = 0.0;
        if (supported(&beats[i]))
            sup[m++] = i;
    }

    /* One line through everything first: the reference for the change
     * points, and the answer when sections are not allowed. */
    CBFitSection g = { 0 };
    if ((ret = cb_fit_line(beats, 0, nb - 1, NULL, &g)) < 0)
        goto end;
    if (!allow || m < 2 * SEG_MIN_SUPPORTED) {
        s[0] = g;
        *nb_secs = 1;
        *secs = s;
        s = NULL;
        goto end;
    }

    /* Isolated ghost attacks (a snare taken for the kick on one beat) are
     * dropped before the change-point search, which would otherwise give
     * each its own section: an attack further than max(4 jitters, 8 ms)
     * from the running median of its neighbours' residuals. A real change
     * moves the running median with it and survives. */
    {
        double *res = malloc(m * sizeof(*res));
        if (!res) {
            ret = CB_ERROR(ENOMEM);
            goto end;
        }
        for (int j = 0; j < m; j++) {
            const CBBeatObs *b = &beats[sup[j]];
            res[j] = b->attack - (g.t0 + (b->index - g.index0) * g.period);
        }
        double tol = fmax(4.0 * g.jitter, 0.008), win[9];
        int dropped = 0;
        for (int j = 0; j < m; j++) {
            int a = CB_CLAMP(j - 4, 0, CB_MAX(m - 9, 0)), k = CB_MIN(9, m);
            memcpy(win, res + a, k * sizeof(*win));
            qsort(win, k, sizeof(*win), cmp_double);
            if (fabs(res[j] - win[k / 2]) > tol) {
                beats[sup[j]].weight = 0.0;
                dropped++;
            }
        }
        free(res);
        if (dropped) {
            int k = 0;
            for (int j = 0; j < m; j++) {
                if (supported(&beats[sup[j]]))
                    sup[k++] = sup[j];
            }
            m = k;
        }
    }

    /* The jitter from second differences of consecutive attacks (white
     * jitter sigma gives sqrt(6) sigma), immune to tempo changes and cuts,
     * which touch only a few of them. */
    int nd = 0;
    for (int j = 1; j + 1 < m; j++) {
        const CBBeatObs *a = &beats[sup[j - 1]], *b = &beats[sup[j]], *c = &beats[sup[j + 1]];
        if (b->index - a->index == 1 && c->index - b->index == 1)
            ws[nd++] = fabs(c->attack - 2 * b->attack + a->attack);
    }
    double sigma = 0.002;
    if (nd > 8) {
        qsort(ws, nd, sizeof(*ws), cmp_double);
        sigma = fmax(1.4826 * ws[nd / 2] / sqrt(6.0), 0.002);
    }

    /* Change points: optimal partition of the attacks into lines, each
     * costing its squared residuals in units of sigma^2 plus a penalty of
     * 3 ln(m) (a little over BIC for two parameters). This proposes;
     * the tests below decide. Prefix sums of the residuals from the
     * global line keep every segment's cost O(1). */
    double *P = calloc((size_t)(m + 1) * 6, sizeof(*P));
    F = malloc((size_t)(m + 1) * sizeof(*F));
    arg = malloc((size_t)(m + 1) * sizeof(*arg));
    if (!P || !F || !arg) {
        free(P);
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    for (int j = 0; j < m; j++) {
        const CBBeatObs *b = &beats[sup[j]];
        double k = b->index - g.index0;
        double r = b->attack - (g.t0 + k * g.period);
        double w = fmin(b->weight / wmed, 3.0);
        double *p = P + (size_t)j * 6, *q = P + (size_t)(j + 1) * 6;
        q[0] = p[0] + w;
        q[1] = p[1] + w * k;
        q[2] = p[2] + w * k * k;
        q[3] = p[3] + w * r;
        q[4] = p[4] + w * k * r;
        q[5] = p[5] + w * r * r;
    }
    const double lambda = 3.0 * log((double)m), is2 = 1.0 / (sigma * sigma);
    const int L = SEG_MIN_SUPPORTED / 2;
    F[0] = 0.0;
    for (int q = 1; q <= m; q++) {
        F[q] = INFINITY;
        arg[q] = 0;
        for (int p = 0; p + L <= q; p++) {
            if (p > 0 && p < L)
                continue;
            if (!isfinite(F[p]))
                continue;
            const double *a = P + (size_t)p * 6, *b = P + (size_t)q * 6;
            double sw = b[0] - a[0], sk = b[1] - a[1], skk = b[2] - a[2];
            double sr = b[3] - a[3], skr = b[4] - a[4], srr = b[5] - a[5];
            double vk = skk - sk * sk / sw, ckr = skr - sk * sr / sw;
            double sse = srr - sr * sr / sw - (vk > 1e-9 ? ckr * ckr / vk : 0.0);
            double cost = F[p] + fmax(sse, 0.0) * is2 + lambda;
            if (cost < F[q]) {
                F[q] = cost;
                arg[q] = p;
            }
        }
    }
    free(P);

    /* Back to sections over the tracked beats: a section starts at the
     * first attack of its segment and runs to the next one's. */
    int cuts[4096], nc = 0;
    for (int q = m; q > 0 && nc < 4096; q = arg[q])
        cuts[nc++] = arg[q];
    Seg sg = { .s = s, .nb = 0, .beats = beats };
    for (int c = nc - 1; c >= 0; c--) {
        int first = c == nc - 1 ? 0 : sup[cuts[c]];
        int last = c == 0 ? nb - 1 : sup[cuts[c - 1]] - 1;
        if ((ret = cb_fit_line(beats, first, last, NULL, &s[sg.nb])) < 0) {
            /* A segment too odd to fit on its own joins the previous one. */
            if (sg.nb > 0 && ret == CB_ERROR(EINVAL)) {
                s[sg.nb - 1].last = last;
                ret = 0;
                continue;
            }
            goto end;
        }
        sg.nb++;
    }

    for (int i = 0; i < sg.nb; i++)
        cb_debug("segment: proposed %d: beats %d-%d (index %d), %.3f BPM, %d attacks, "
                 "jitter %.1f ms\n",
                 i, sg.s[i].first, sg.s[i].last, sg.s[i].index0, 60.0 / sg.s[i].period,
                 sg.s[i].supported, sg.s[i].jitter * 1e3);

    /* The evidence tests, repeated until nothing changes. */
    bool changed = true;
    while (changed && sg.nb > 1) {
        changed = false;

        /* 1. A boundary a single line does not need: the merged line stays
         *    within 15 ms of the attacks everywhere. Cheapest first. */
        int bi = -1;
        double bd = SEG_MAX_DEV;
        for (int i = 0; i + 1 < sg.nb; i++) {
            double d;
            if ((ret = merged_deviation(&sg, i, i + 1, seg_dominant(&sg, i, i + 1), -1, &d)) < 0)
                goto end;
            if (d <= bd) {
                bd = d;
                bi = i;
            }
        }
        if (bi >= 0) {
            if ((ret = seg_merge(&sg, bi, bi + 1, seg_dominant(&sg, bi, bi + 1))) < 0)
                goto end;
            changed = true;
            continue;
        }

        /* 2. Hold through a breakdown: when the sections either side of
         *    one are on the same line, whatever happened between them
         *    (an off-beat bass, a half-time bar, a fill) does not get a
         *    grid of its own. */
        for (int i = 1; i + 1 < sg.nb && !changed; i++) {
            /* Only the weaker part is a breakdown: two scraps of junk
             * either side of a good section must not take it over. */
            if (sg.s[i].supported >= CB_MIN(sg.s[i - 1].supported, sg.s[i + 1].supported))
                continue;
            int dom = seg_dominant(&sg, i - 1, i + 1);
            double d;
            if ((ret = merged_deviation(&sg, i - 1, i + 1, dom, i, &d)) < 0)
                goto end;
            if (d <= SEG_MAX_DEV) {
                if ((ret = seg_merge(&sg, i - 1, i + 1, dom)) < 0)
                    goto end;
                changed = true;
            }
        }
        if (changed)
            continue;

        /* 3. Support: each side of a boundary must be explained by its own
         *    line clearly better than by its neighbour's carried across,
         *    on the arbiter band (the kicks, when the track has them). A
         *    hats-only intro tracked on the off-beat has no kick on either
         *    line and joins the music after it. */
        for (int i = 0; i + 1 < sg.nb && !changed; i++) {
            CBFitSection *A = &sg.s[i], *B = &sg.s[i + 1];
            /* Not between two tempos the tempogram found settled (3 % or
             * more apart): test 1 has already said the two lines differ
             * audibly, and on soft drums the kick band may confirm neither;
             * merging would keep a line right for neither (a DJ mix of pop
             * songs collapsed into one). A tempo the tracker merely drifted
             * to (a loose intro) must still pass. */
            if (fabs(A->period / B->period - 1.0) >= 0.03) {
                double ra = region_bpm(regions, nb_regions,
                                       0.5 * (seg_t0(&sg, i) + seg_t1(&sg, i)));
                double rb = region_bpm(regions, nb_regions,
                                       0.5 * (seg_t0(&sg, i + 1) + seg_t1(&sg, i + 1)));
                if (fabs(ra / rb - 1.0) >= 0.03)
                    continue;
            }
            double tb = seg_t0(&sg, i + 1);
            double ta = fmax(seg_t0(&sg, i), tb - SEG_SUPPORT_SPAN * A->period);
            double tc = fmin(seg_t1(&sg, i + 1), tb + SEG_SUPPORT_SPAN * B->period);
            const double eps = 0.2;
            double a_own = line_support(o, support, A, ta, tb - 0.5 * A->period);
            double a_by_b = line_support(o, support, B, ta, tb - 0.5 * A->period);
            double b_own = line_support(o, support, B, tb, tc);
            double b_by_a = line_support(o, support, A, tb, tc);
            /* A new phase at the same tempo needs more (rbxport: half again
             * as much kick on the new line as the old one carried on): an
             * off-beat bass alone can make a half-beat line look nearly as
             * good. */
            double need = fabs(A->period / B->period - 1.0) < 0.005 ? SEG_PHASE_RATIO
                                                                     : SEG_SUPPORT_RATIO;
            bool keep_a = (a_own + eps) >= need * (a_by_b + eps);
            bool keep_b = (b_own + eps) >= need * (b_by_a + eps);
            if (keep_a && keep_b)
                continue;
            int dom = seg_dominant(&sg, i, i + 1);
            if ((ret = seg_merge(&sg, i, i + 1, dom)) < 0)
                goto end;
            changed = true;
        }
        if (changed)
            continue;

        /* 4. Too short to have a tempo or phase of its own (rbxport: an
         *    outro's last bars never do): joins the neighbour whose line
         *    fits it best. */
        for (int i = 0; i < sg.nb && !changed; i++) {
            CBFitSection *X = &sg.s[i];
            int len = X->last - X->first + 1;
            /* At the tempo of a neighbour, a section is a phase cut, and a
             * phase needs 64 beats to be believed (an outro's last bars,
             * a fill: rbxport's rule). */
            /* Half a beat away it needs 128 (32 bars): an off-beat bass and
             * hats carry intros, breakdowns and outros on their own, and
             * read as a line half a beat off. */
            bool same = false, half = false;
            for (int k = i - 1; k <= i + 1; k += 2) {
                if (k < 0 || k >= sg.nb || fabs(sg.s[k].period / X->period - 1.0) >= 0.005)
                    continue;
                same = true;
                double t = seg_t0(&sg, i), q = (t - sg.s[k].t0) / sg.s[k].period;
                if (fabs(fabs(q - floor(q)) - 0.5) < 0.1)
                    half = true;
            }
            int need = !same ? SEG_MIN_BEATS : half ? SEG_MIN_HALF_BEATS : SEG_MIN_PHASE_BEATS;
            if (X->supported >= SEG_MIN_SUPPORTED && len >= need)
                continue;
            double dl = INFINITY, dr = INFINITY;
            if ((i > 0 && (ret = merged_deviation(&sg, i - 1, i, i - 1, -1, &dl)) < 0) ||
                (i + 1 < sg.nb && (ret = merged_deviation(&sg, i, i + 1, i + 1, -1, &dr)) < 0))
                goto end;
            if (dl <= dr && i > 0)
                ret = seg_merge(&sg, i - 1, i, i - 1);
            else if (i + 1 < sg.nb)
                ret = seg_merge(&sg, i, i + 1, i + 1);
            if (ret < 0)
                goto end;
            changed = true;
        }
    }

    /* A boundary sits on the first beat the new line explains: at a
     * change made on the beat (a DJ edit cut on the bar line) the beat at
     * the join lies on both lines, and the search above gave it to the old
     * one. Move it back while the old section's last attacks fit the new
     * line within 3 ms. */
    for (int i = 1; i < sg.nb; i++) {
        CBFitSection *A = &sg.s[i - 1], *B = &sg.s[i];
        while (B->first - 1 > A->first + SEG_MIN_BEATS) {
            const CBBeatObs *b = &beats[B->first - 1];
            if (!supported(b))
                break;
            double pb = B->t0 + lrint((b->attack - B->t0) / B->period) * B->period;
            if (fabs(b->attack - pb) > fmax(0.003, 2.0 * B->jitter))
                break;
            B->first--;
            A->last--;
        }
    }

    /* Each section refitted on its final beats. */
    for (int i = 0; i < sg.nb; i++) {
        CBFitSection init = sg.s[i];
        if ((ret = cb_fit_line(beats, sg.s[i].first, sg.s[i].last, &init, &sg.s[i])) < 0)
            goto end;
    }
    *secs = s;
    *nb_secs = sg.nb;
    s = NULL;

end:
    free(ws);
    free(sup);
    free(s);
    free(F);
    free(arg);
    return ret;
}
