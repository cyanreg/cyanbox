/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Onset strength signals: how much new sound starts at each frame, in four
 * bands. Each is a spectral flux (the sum of the rises of the log band
 * amplitudes), timestamped at the centre of its window.
 *
 *  - full: SuperFlux (Böck & Widmer, 2013). The rise at each band is taken
 *    against the maximum of the band and its two neighbours two frames
 *    earlier, so a note that slides or wobbles into the next band (vibrato,
 *    a filter sweep, a pitch bend) does not count as a new onset; a drum
 *    hit, which raises every band at once, still does. The reference is two
 *    frames back (13 ms) because a 46 ms window rises over several hops.
 *  - low: the kick band (30..150 Hz). The kick defines the beat in dance
 *    music, and below 150 Hz there are no hats to confuse it with.
 *  - mid: 150..2500 Hz, where snares and claps have their body.
 *  - high: above 5 kHz, the hats.
 *
 * Timestamps. A difference between frames t-1 and t+1 would naturally be
 * stamped at t, the window's centre (rbxport: stamping at the window's
 * start put every beat 23 ms early). But a *log* flux still peaks early:
 * the log of the energy in a 46 ms window rises fastest while a hit has
 * only just entered it. Measured here as 13-17 ms on the synthetic kicks
 * of tests/analysis/test_beats.c and on real tracks (the average beat of
 * the high band rises 13 ms after the uncorrected frames), so every flux
 * is stamped two frames (13.3 ms) later. The fine envelopes then find the
 * attack to a fraction of a millisecond; this only centres their search. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLUX_DELAY 2

int cb_onset_normalise(float *o, int n, double fps)
{
    if (n <= 0)
        return 0;
    /* Local mean over one second (dj's box_smooth with width fps | 1),
     * removed so that a loud passage does not outweigh a quiet one, then
     * clipped: only rises above the local level are onsets. */
    int w = (int)fps | 1, h = w / 2;
    double *cum = malloc((size_t)(n + 1) * sizeof(*cum));
    if (!cum)
        return CB_ERROR(ENOMEM);
    cum[0] = 0.0;
    for (int i = 0; i < n; i++)
        cum[i + 1] = cum[i] + o[i];
    double sum = 0.0, sum2 = 0.0;
    for (int i = 0; i < n; i++) {
        int lo = CB_MAX(i - h, 0), hi = CB_MIN(i + h + 1, n);
        double m = (cum[hi] - cum[lo]) / (hi - lo);
        double v = fmax(0.0, o[i] - m);
        o[i] = (float)v;
        sum += v;
        sum2 += v * v;
    }
    free(cum);
    double mean = sum / n, sd = sqrt(fmax(sum2 / n - mean * mean, 0.0));
    float g = (float)(1.0 / (sd + 1e-9));
    for (int i = 0; i < n; i++)
        o[i] *= g;
    return 0;
}

void cb_superflux_frame(const float *ref, const float *cur, int mid0, int mid1, int high0,
                        float *full, float *mid, float *high)
{
    const int nm = CB_BEAT_NMELS;
    float d[CB_BEAT_NMELS];
    for (int b = 0; b < nm; b++) {
        float r = ref[b];
        if (b > 0)
            r = fmaxf(r, ref[b - 1]);
        if (b + 1 < nm)
            r = fmaxf(r, ref[b + 1]);
        d[b] = fmaxf(cur[b] - r, 0.0f);
    }
    float a = 0.0f, m = 0.0f, h = 0.0f;
    for (int b = 0; b < nm; b++)
        a += d[b];
    for (int b = mid0; b < mid1; b++)
        m += d[b];
    for (int b = high0; b < nm; b++)
        h += d[b];
    *full = a;
    *mid = m;
    *high = h;
}

/* Shifts v by the lag that best aligns it with ref. Where no lag gives
 * the two anything in common (a silent band, or a silent reference), v
 * stays where it is: the first lag tried, 8 frames back, used to win
 * there, and on fewer than 8 frames its shift wrote outside v. */
static void align_band(const float *ref, float *v, int n)
{
    const int L = CB_MIN(8, n - 1);
    double best = 0.0;
    int lag = 0;
    for (int l = -L; l <= L; l++) {
        double acc = 0.0;
        for (int i = CB_MAX(0, -l); i < n && i + l < n; i++)
            acc += (double)ref[i] * v[i + l];
        if (acc > best) {
            best = acc;
            lag = l;
        }
    }
    /* v[i + lag] lines up with ref[i]: move it back by lag. */
    if (lag > 0) {
        memmove(v, v + lag, (size_t)(n - lag) * sizeof(*v));
        for (int i = n - lag; i < n; i++)
            v[i] = 0.0f;
    } else if (lag < 0) {
        memmove(v - lag, v, (size_t)(n + lag) * sizeof(*v));
        for (int i = 0; i < -lag; i++)
            v[i] = 0.0f;
    }
}

int cb_onsets_compute(CBSpectrum *s, CBOnsets *o)
{
    memset(o, 0, sizeof(*o));
    const int n = s->nb_frames;
    o->nb = n;
    o->fps = s->fps;
    o->full = s->flux_full;
    o->mid = s->flux_mid;
    o->high = s->flux_high;
    s->flux_full = s->flux_mid = s->flux_high = NULL;
    o->low = calloc(CB_MAX(n, 1), sizeof(float));
    o->accent = calloc(CB_MAX(n, 1), sizeof(float));
    if (!o->full || !o->low || !o->mid || !o->high || !o->accent) {
        cb_onsets_free(o);
        return CB_ERROR(ENOMEM);
    }
    for (int t = 1; t + 1 < n; t++)
        o->low[t] = fmaxf(0.0f, s->kick[t + 1] - s->kick[t - 1]);

    /* The two-frame delay of the log flux (above). */
    float *sig[] = { o->full, o->low, o->mid, o->high };
    for (size_t k = 0; k < CB_ARRAY_ELEMS(sig); k++) {
        memmove(sig[k] + FLUX_DELAY, sig[k], (size_t)CB_MAX(n - FLUX_DELAY, 0) * sizeof(float));
        for (int t = 0; t < CB_MIN(FLUX_DELAY, n); t++)
            sig[k][t] = 0.0f;
    }

    for (size_t k = 0; k < CB_ARRAY_ELEMS(sig); k++) {
        int ret = cb_onset_normalise(sig[k], n, o->fps);
        if (ret < 0) {
            cb_onsets_free(o);
            return ret;
        }
    }

    /* Each band's flux has its own delay. The kick band's is the largest:
     * between sidechained kicks the band is nearly empty, so its log rises
     * almost fully while a sliver of the kick has entered the window, and
     * its flux peaks 15-40 ms before the full band's (measured on house and
     * techno tracks). The bands are put on the full band's time base by
     * the lag of their cross-correlation with it, within +-8 frames (53
     * ms, far short of an off-beat). */
    align_band(o->full, o->low, n);
    align_band(o->full, o->mid, n);
    align_band(o->full, o->high, n);
    memcpy(o->accent, o->full, (size_t)n * sizeof(float));
    return 0;
}

void cb_onsets_free(CBOnsets *o)
{
    free(o->full);
    free(o->low);
    free(o->mid);
    free(o->high);
    free(o->accent);
    memset(o, 0, sizeof(*o));
}

/* A float's bits as an unsigned key in the floats' order (negative ones
 * reversed), and back. */
static uint32_t float_key(float x)
{
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    return u & 0x80000000u ? ~u : u | 0x80000000u;
}

static float key_float(uint32_t k)
{
    uint32_t u = k & 0x80000000u ? k & 0x7fffffffu : ~k;
    float x;
    memcpy(&x, &u, sizeof(x));
    return x;
}

/* The k-th smallest of x[0, n) (from 0), the value sorting would put
 * there, without a copy to sort: the smallest key with more than k of
 * them at or below it, found bit by bit (32 passes). */
static float kth_smallest(const float *x, int n, int k)
{
    uint32_t key = 0;
    for (int bit = 31; bit >= 0; bit--) {
        uint32_t below = key | ((1u << bit) - 1);
        int at_or_below = 0;
        for (int i = 0; i < n; i++)
            at_or_below += float_key(x[i]) <= below;
        if (at_or_below <= k)
            key |= 1u << bit;
    }
    return key_float(key);
}

double cb_onsets_first(const CBOnsets *o)
{
    /* dj's rule: the first frame above a quarter of the 99th percentile,
     * i.e. the first onset that is loud for this track. The percentile is
     * selected in place, so nothing can fail here: a sorted copy whose
     * allocation failed gave 0, a loud onset at the very start. */
    if (o->nb <= 0)
        return 0.0;
    double p99 = kth_smallest(o->full, o->nb,
                              (int)CB_MIN((int64_t)(0.99 * (o->nb - 1) + 0.5), o->nb - 1));
    double thr = 0.25 * p99;
    for (int i = 0; i < o->nb; i++) {
        if (o->full[i] > thr)
            return i / o->fps;
    }
    return 0.0;
}
