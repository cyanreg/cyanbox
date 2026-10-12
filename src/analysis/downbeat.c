/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Which beat is beat 1, and whether the beats are on the beat at all.
 *
 * The loudest hits cannot answer either question on every track: in tech
 * house the open hat between the kicks has as much flux as the kick, in
 * hardstyle the reverse bass on the off-beat has more low end than the
 * kick (rbxport measured both: no single band picks the beat on every
 * track). Structure can. Dance music changes at bar lines, and much more
 * at phrase lines four or eight bars apart: a drop, a breakdown, a new
 * bass line, a filter opening. Every such change lands on beat 1, which is
 * a kick, never an off-beat.
 *
 * So the music is summarised as one spectral profile per half beat, the
 * novelty at every half beat is the distance between the average profiles
 * before and after it at one, two, four and eight bars, and each of the
 * eight half-beat positions of a bar collects the novelty peaks that land
 * on it. Only peaks vote: summing the raw novelty gives every position
 * nearly the same total, because a slow crescendo raises them all alike
 * (rbxport measured a 3 % margin from sums and 2.8x from peaks). Each
 * scale's peaks are normalised to sum to one so that a long phrase counts
 * as much as a bar.
 *
 * Extra, weaker votes: the beat where the music starts (tracks made for DJs
 * nearly always start on a downbeat, dj's rule) and the beats where the
 * bass note changes (a bass line moves on the bar far more often than
 * inside it). The caller decides what an odd (half-beat) winner means. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GROUP RX_BEAT_GROUP             /* mel bands per profile band: 16 bands */
#define BASS_WEIGHT 0.5                 /* the bass vote against one novelty scale's 1 */
/* The music's start, as much as one scale: in the evaluation sample, 59 of
 * the 66 electronic tracks whose music starts on a beat had beat 1 there by
 * structure alone, the rest mostly on weak votes; with the start worth a
 * scale, all of them do (docs/BEATS.md). */
#define START_WEIGHT 1.0
#define NPROF RX_BEAT_NPROF

/* The bass-note vote: where the bass's pitch class changes between the two
 * beats before and the two beats after, peaks only, normalised to sum 1. */
static int bass_vote(const float *x, int64_t n, double t0, double period, double start,
                     double end, double *score)
{
    int k0 = (int)ceil((start - t0) / period), k1 = (int)floor((end - t0) / period);
    int K = k1 - k0;
    if (K < 16)
        return 0;
    double *times = malloc((size_t)K * sizeof(*times));
    float *c = malloc((size_t)K * 12 * sizeof(*c));
    double *nov = calloc(K, sizeof(*nov));
    int ret = 0;
    if (!times || !c || !nov) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    for (int k = 0; k < K; k++)
        times[k] = t0 + (k0 + k) * period;
    if ((ret = rx_bass_chroma(x, n, times, K, c)) < 0)
        goto end;
    for (int k = 2; k + 2 <= K; k++) {
        double d = 0.0;
        for (int p = 0; p < 12; p++) {
            double before = 0.5 * (c[(k - 2) * 12 + p] + c[(k - 1) * 12 + p]);
            double after = 0.5 * (c[k * 12 + p] + c[(k + 1) * 12 + p]);
            d += fabs(after - before);
        }
        nov[k] = 0.5 * d;
    }
    double tot = 0.0;
    for (int k = 3; k + 3 <= K; k++) {
        if (nov[k] > nov[k - 1] && nov[k] >= nov[k + 1])
            tot += nov[k];
    }
    for (int k = 3; k + 3 <= K && tot > 0.0; k++) {
        if (nov[k] > nov[k - 1] && nov[k] >= nov[k + 1])
            score[((2 * (k0 + k)) % 8 + 8) % 8] += BASS_WEIGHT * nov[k] / tot;
    }

end:
    free(times);
    free(c);
    free(nov);
    return ret;
}

int rx_downbeat_vote(const RxSpectrum *s, const float *x, int64_t n,
                     double t0, double period, double start, double end, double first_onset,
                     RxDownbeat *out)
{
    memset(out, 0, sizeof(*out));
    out->margin = 1.0;
    const double fps = s->fps, hp = period / 2.0;

    /* Half beats j0..j1 relative to t0, so that position = j mod 8. */
    int j0 = (int)ceil((start - t0) / hp), j1 = (int)floor((end - t0) / hp);
    int J = j1 - j0;
    if (J < 32)
        return 0;

    float *prof = calloc((size_t)J * NPROF, sizeof(*prof));
    double *cum = calloc((size_t)(J + 1) * NPROF, sizeof(*cum));
    double *nov = malloc((size_t)J * sizeof(*nov));
    if (!prof || !cum || !nov) {
        free(prof);
        free(cum);
        free(nov);
        return RX_ERROR(ENOMEM);
    }

    /* Profiles: mean log-mel per band group over each half beat. */
    for (int j = 0; j < J; j++) {
        double a = t0 + (j0 + j) * hp, b = a + hp;
        int f0 = RX_MAX((int)ceil(a * fps), 0), f1 = RX_MIN((int)ceil(b * fps), s->nb_frames);
        float *p = prof + (size_t)j * NPROF;
        for (int f = f0; f < f1; f++) {
            const float *m = s->prof + (size_t)f * NPROF;
            for (int g = 0; g < NPROF; g++)
                p[g] += m[g];
        }
        if (f1 > f0) {
            for (int g = 0; g < NPROF; g++)
                p[g] /= (float)((f1 - f0) * GROUP);
        }
    }
    for (int j = 0; j < J; j++) {
        const float *p = prof + (size_t)j * NPROF;
        const double *c0 = cum + (size_t)j * NPROF;
        double *c1 = cum + (size_t)(j + 1) * NPROF;
        for (int g = 0; g < NPROF; g++)
            c1[g] = c0[g] + p[g];
    }

    static const int scales[] = { 8, 16, 32, 64 };     /* one, two, four, eight bars */
    double score[8] = { 0 };
    for (size_t si = 0; si < RX_ARRAY_ELEMS(scales); si++) {
        int sc = scales[si];
        if (J < 2 * sc + 2)
            continue;
        for (int j = 0; j < J; j++)
            nov[j] = 0.0;
        for (int j = sc; j + sc <= J; j++) {
            const double *ca = cum + (size_t)(j - sc) * NPROF, *cj = cum + (size_t)j * NPROF;
            const double *cb = cum + (size_t)(j + sc) * NPROF;
            double d2 = 0.0;
            for (int g = 0; g < NPROF; g++) {
                double before = (cj[g] - ca[g]) / sc, after = (cb[g] - cj[g]) / sc;
                d2 += (after - before) * (after - before);
            }
            nov[j] = sqrt(d2);
        }
        double tot = 0.0;
        for (int j = sc + 1; j + sc < J; j++) {
            if (nov[j] > nov[j - 1] && nov[j] >= nov[j + 1])
                tot += nov[j];
        }
        if (tot <= 0.0)
            continue;
        for (int j = sc + 1; j + sc < J; j++) {
            if (nov[j] > nov[j - 1] && nov[j] >= nov[j + 1]) {
                int pos = ((j0 + j) % 8 + 8) % 8;
                score[pos] += nov[j] / tot;
            }
        }
    }

    /* The bass notes: a weaker vote, half a scale's worth (a bass riff
     * also moves inside the bar). */
    int ret = bass_vote(x, n, t0, period, start, end, score);
    if (ret < 0) {
        free(prof);
        free(cum);
        free(nov);
        return ret;
    }

    /* The beat where the music starts, if a beat is there. */
    double k = (first_onset - t0) / hp;
    int kj = (int)lrint(k);
    if (fabs(k - kj) < 0.2 && first_onset >= start - period && first_onset < end)
        score[((kj % 8) + 8) % 8] += START_WEIGHT;

    memcpy(out->scores, score, sizeof(score));
    int best = 0;
    for (int p = 1; p < 8; p++) {
        if (score[p] > score[best])
            best = p;
    }
    double second = 0.0;
    for (int p = 0; p < 8; p++) {
        if (p != best && (p & 1) == (best & 1))
            second = fmax(second, score[p]);
    }
    out->position = best;
    out->margin = second > 0.0 ? score[best] / second : 10.0;

    free(prof);
    free(cum);
    free(nov);
    return 0;
}
