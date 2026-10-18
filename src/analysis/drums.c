/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Where the drums come in (docs/BEATS.md, "Where the drums come in"): the
 * downbeat the automatic cue goes on.
 *
 * A DJ loads a track to start it where it plays: the user's rule, asked
 * for track by track (2026-10-18), is the first downbeat where the drums
 * are in at full level, past a fade-in and past an intro without them
 * alike (Rouge's eight bars of synths after its fade: on to the kick).
 * The kick is the low band (under 150 Hz) in the first 80 ms of a beat,
 * its body: a bass alone holds the band all through the beat, the kick
 * fills its start. A downbeat is in when its kick is within 8 dB of the
 * track's loud beats (the 90th percentile of every beat's from the start
 * of the music), so that a kick under a bass that joins later is in;
 * when it is not still rising (6 dB under the loudest of the next four
 * downbeats: a sweep into the drop, a fade); and when two of it and the
 * next three are in, so that an opening hit over a quiet intro is not.
 * Worked out on 39 tracks whose drums were placed by eye or by the user
 * (38 right; the other's bar was wrong), with a wide range of thresholds
 * as good. A song whose drums come in at full level only after a third of
 * it (a chorus) has no intro to skip: the start of the music stands. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "beats_internal.h"

#define LOW_HZ        150.0     /* the low band: a kick's body and the bass */
#define ATTACK_S      0.080     /* a kick's body after its beat */
#define ATTACK_SHARE  0.4       /* of a beat at most, so that windows never meet */
#define LOUD_PCT      0.90      /* the track's loud beats */
#define IN_DB         8.0       /* a downbeat's kick within this of them is in */
#define RISE_DB       6.0       /* not this far under the loudest of the next downbeats */
#define AHEAD         4         /* the next downbeats looked at */
#define HELD          2         /* of a downbeat and the next three, this many in */
#define MAX_SHARE     (1.0 / 3.0)   /* no further into the track than this */

/* A second-order low-pass (the bilinear transform, prewarped: two of them
 * at the Q of a fourth-order Butterworth's pole pairs make one), in the
 * transposed direct form II. */
typedef struct Biquad {
    double b0, b1, b2, a1, a2;
    double z1, z2;
} Biquad;

static void lowpass(Biquad *q, double fc, double Q, double rate)
{
    const double w = 2.0 * M_PI * fc / rate;
    const double al = sin(w) / (2.0 * Q), c = cos(w), a0 = 1.0 + al;
    *q = (Biquad) {
        .b0 = (1.0 - c) / 2.0 / a0, .b1 = (1.0 - c) / a0, .b2 = (1.0 - c) / 2.0 / a0,
        .a1 = -2.0 * c / a0, .a2 = (1.0 - al) / a0,
    };
}

static inline double biquad(Biquad *q, double x)
{
    const double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The p quantile of v[0..n), between the two values around it (numpy's
 * default). */
static double quantile(const double *v, int n, double p, double *tmp)
{
    memcpy(tmp, v, (size_t)n * sizeof(*tmp));
    qsort(tmp, (size_t)n, sizeof(*tmp), cmp_double);
    const double at = p * (n - 1);
    const int i = (int)floor(at);
    return i + 1 < n ? tmp[i] + (at - i) * (tmp[i + 1] - tmp[i]) : tmp[n - 1];
}

int cb_beats_drums(const float *x, int64_t n, int rate, CBBeatResult *res)
{
    res->drums_start = res->music_start;
    if (!x || n <= 0 || rate <= 0 || !cb_grid_valid(&res->grid))
        return 0;

    const double duration = (double)n / rate;
    CBBeat *b = NULL;
    int nb = 0, ret = cb_grid_beats(&res->grid, duration, &b, &nb);
    if (ret < 0)
        return ret;
    int first = 0;
    while (first < nb && b[first].time < res->music_start - CB_BEAT_START_SLACK)
        first++;
    /* Every beat with one after it: its window is a share of the beat. */
    const int m = nb - first - 1;
    if (m < 2) {
        free(b);
        return 0;
    }
    const CBBeat *bt = b + first;

    double *kick = malloc((size_t)m * sizeof(*kick));
    double *tmp = malloc((size_t)m * sizeof(*tmp));
    int64_t *from = malloc((size_t)m * sizeof(*from));
    int64_t *to = malloc((size_t)m * sizeof(*to));
    int *down = malloc((size_t)m * sizeof(*down));
    if (!kick || !tmp || !from || !to || !down) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    for (int j = 0; j < m; j++) {
        const double len = fmin(ATTACK_S, ATTACK_SHARE * (bt[j + 1].time - bt[j].time));
        from[j] = CB_CLAMP((int64_t)floor(bt[j].time * rate), (int64_t)0, n);
        to[j] = CB_CLAMP((int64_t)floor((bt[j].time + len) * rate), from[j], n);
    }

    /* One pass of the low-pass over the track; each window's mean power. */
    Biquad q[2];
    lowpass(&q[0], LOW_HZ, 1.0 / (2.0 * cos(M_PI / 8.0)), rate);
    lowpass(&q[1], LOW_HZ, 1.0 / (2.0 * cos(3.0 * M_PI / 8.0)), rate);
    int j = 0;
    double acc = 0.0;
    const int64_t last = to[m - 1];
    for (int64_t i = 0; i < last && j < m; i++) {
        const double y = biquad(&q[1], biquad(&q[0], x[i]));
        if (i < from[j])
            continue;
        acc += y * y;
        while (j < m && i + 1 >= to[j]) {
            kick[j] = 10.0 * log10(acc / (double)CB_MAX(to[j] - from[j], (int64_t)1) + 1e-20);
            acc = 0.0;
            j++;
        }
    }
    for (; j < m; j++)
        kick[j] = -200.0;

    const double loud = quantile(kick, m, LOUD_PCT, tmp);
    int nd = 0;
    for (int k = 0; k < m; k++) {
        if (bt[k].number == 1)
            down[nd++] = k;
    }
    for (int k = 0; k < nd; k++) {
        const double in = kick[down[k]] - loud;
        if (in < -IN_DB)
            continue;
        double ahead = -INFINITY;
        for (int i = k + 1; i <= k + AHEAD && i < nd; i++)
            ahead = fmax(ahead, kick[down[i]] - loud);
        if (in < ahead - RISE_DB)
            continue;
        int held = 0, span = 0;
        for (int i = k; i < k + AHEAD && i < nd; i++, span++)
            held += kick[down[i]] - loud >= -IN_DB;
        if (held < CB_MIN(HELD, span))
            continue;
        if (bt[down[k]].time <= duration * MAX_SHARE)
            res->drums_start = bt[down[k]].time;
        break;
    }
    ret = 0;
end:
    free(kick);
    free(tmp);
    free(from);
    free(to);
    free(down);
    free(b);
    return ret;
}
