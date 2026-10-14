/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Tempo: which beat rate the track has, from the whole track, and where (if
 * anywhere) a different tempo takes over.
 *
 * The whole-track estimate works in three steps, each answering one kind of
 * mistake:
 *
 * 1. Candidates come from the autocorrelation of the onset signal, summed
 *    over one, two and four beats (a comb). The autocorrelation is good at
 *    finding repetition but cannot tell the beat from a pattern that also
 *    repeats: kick, hat, kick, hat makes a lag of one and a half beats
 *    strong. So every peak brings its relatives (x2, x1/2, x3/2, x2/3).
 * 2. Each candidate is rescored by the onset energy on its own harmonic
 *    series: an onset train with period P has DFT lines at 1/P, 2/P, 3/P,
 *    4/P. A 3/2 or 2/3 impostor hits far fewer of them (dj's idea, kept).
 *    The candidates then fall into octave families (tempos related by a
 *    power of two), and the strongest family wins. Harmonic strength cannot
 *    choose within a family: a kick on every beat with a hat between them
 *    has as many lines at twice the tempo as at the tempo.
 * 3. The octave is not chosen here. The working octave is the family's
 *    member nearest the prior (125 BPM); the grid is fitted there, and on
 *    the fitted grid, whose beats sit on the hits, rx_tempo_octave_decide
 *    weighs half, same and double by the rhythm: where the snares and
 *    claps fall, whether a four-on-the-floor kick marks the level, and the
 *    prior where the rhythm leaves it open. If another octave wins, the
 *    grid is fitted again there. Rhythm evidence needs beats placed on
 *    the hits, which a whole-track estimate does not have.
 *
 * The tempogram (rx_tempo_regions) asks the same question locally, over
 * 12-second windows, to find stretches that settle on a tempo outside the
 * main tempo's octave family (a DJ edit from 128 to 174, say), and to catch
 * a whole-track estimate that chose the wrong family (most windows agree
 * on another tempo). Small drifts are not its business: the beat tracker
 * follows them and the segmentation decides whether they matter. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/mem.h>
#include <libavutil/tx.h>

#define DEFAULT_PRIOR 125.0
/* The prior's width in octaves (rx_tempo_octave_decide explains it). */
#define OCTAVE_PRIOR_WIDTH 0.4
/* The least each rhythm likelihood can be: how far the rhythm can
 * overrule the prior. */
#define OCTAVE_FLOOR 0.25
/* The automatic range's comfortable octave (rx_tempo_octave_decide). */
#define OCTAVE_AUTO_LO 60.0
#define OCTAVE_AUTO_HI 200.0

/* ------------------------------------------------------------ utilities */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int next_pow2(int64_t n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

/* av_tx has fast codelets up to 2^17 complex points (2^18 real); above
 * that it falls back to a naive O(n^2) DFT. Every transform here stays
 * within that. */
#define TX_MAX_REAL (1 << 18)

/* Autocorrelation of o[0..n) for lags [0, maxlag), normalised to ac[0] = 1.
 * Computed in blocks: each block's cross-correlation with itself extended
 * by maxlag samples, by FFT, summed. Any track length, small transforms. */
static int autocorr(const float *o, int n, float *ac, int maxlag)
{
    const int M = RX_MIN(next_pow2((int64_t)n + maxlag), 1 << 16), B = M - maxlag;
    AVTXContext *fwd = NULL, *inv = NULL;
    av_tx_fn ffn, ifn;
    float *buf = av_malloc((M + 2) * sizeof(*buf));
    float *acc = calloc(maxlag, sizeof(*acc));
    AVComplexFloat *sa = av_malloc((M / 2 + 1) * sizeof(*sa));
    AVComplexFloat *sb = av_malloc((M / 2 + 1) * sizeof(*sb));
    float scale = 1.0f;
    int ret = 0;
    if (B <= 0) {
        ret = RX_ERROR(EINVAL);
        goto end;
    }
    if (!buf || !acc || !sa || !sb) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_tx_init(&fwd, &ffn, AV_TX_FLOAT_RDFT, 0, M, &scale, 0)) < 0 ||
        (ret = av_tx_init(&inv, &ifn, AV_TX_FLOAT_RDFT, 1, M, &scale, 0)) < 0)
        goto end;
    for (int b0 = 0; b0 < n; b0 += B) {
        int lb = RX_MIN(B, n - b0), le = RX_MIN(B + maxlag, n - b0);
        /* a = the block, b = the block and the maxlag samples after it:
         * sum_t a[t] b[t + l] = IDFT(conj(A) B)[l]. */
        memset(buf, 0, M * sizeof(*buf));
        memcpy(buf, o + b0, lb * sizeof(*buf));
        ffn(fwd, sa, buf, sizeof(float));
        memset(buf, 0, M * sizeof(*buf));
        memcpy(buf, o + b0, le * sizeof(*buf));
        ffn(fwd, sb, buf, sizeof(float));
        for (int k = 0; k <= M / 2; k++) {
            float re = sa[k].re * sb[k].re + sa[k].im * sb[k].im;
            float im = sa[k].re * sb[k].im - sa[k].im * sb[k].re;
            sa[k].re = re;
            sa[k].im = im;
        }
        ifn(inv, buf, sa, sizeof(AVComplexFloat));
        for (int l = 0; l < maxlag; l++)
            acc[l] += buf[l];
    }
    float a0 = acc[0] > 0.0f ? acc[0] : 1.0f;
    for (int l = 0; l < maxlag; l++)
        ac[l] = acc[l] / a0;

end:
    av_tx_uninit(&fwd);
    av_tx_uninit(&inv);
    av_free(buf);
    free(acc);
    av_free(sa);
    av_free(sb);
    return ret;
}

static double interp(const float *v, int n, double x)
{
    if (x < 0.0 || x > n - 1)
        return 0.0;
    int i = (int)x;
    if (i >= n - 1)
        return v[n - 1];
    double f = x - i;
    return v[i] * (1.0 - f) + v[i + 1] * f;
}

/* The autocorrelation summed over the beat, two-beat and bar lags (dj). */
static double comb(const float *ac, int n, double fps, double bpm)
{
    double lag = 60.0 * fps / bpm;
    return interp(ac, n, lag) + 0.5 * interp(ac, n, 2 * lag) + 0.25 * interp(ac, n, 4 * lag);
}

/* --------------------------------------------------- harmonic strength */

double rx_tempo_phase(const float *o, int n, double fps, double bpm, double *strength)
{
    double w = 2.0 * M_PI * bpm / 60.0 / fps;
    double cr = cos(w), ci = -sin(w), zr = 1.0, zi = 0.0, ar = 0.0, ai = 0.0;
    for (int i = 0; i < n; i++) {
        ar += o[i] * zr;
        ai += o[i] * zi;
        double t = zr * cr - zi * ci;
        zi = zr * ci + zi * cr;
        zr = t;
        if ((i & 1023) == 1023) {
            double m = 1.0 / hypot(zr, zi);
            zr *= m;
            zi *= m;
        }
    }
    if (strength)
        *strength = hypot(ar, ai) / RX_MAX(n, 1);
    /* sum o[i] e^{-iwi} peaks in phase when the onsets sit at i = phase. */
    double period = 60.0 / bpm;
    double ph = -atan2(ai, ar) / (2.0 * M_PI * bpm / 60.0);
    ph = fmod(ph, period);
    if (ph < 0.0)
        ph += period;
    return ph;
}

/* The magnitude spectrum of the whole onset signal, zero-padded so that
 * one bin is under 0.01 BPM at the fundamental; read with interpolation.
 * One FFT serves every candidate and harmonic. */
typedef struct BigSpec {
    float *mag;
    int    M;
    int    nb;
    double fps;
    double norm;
} BigSpec;

static int bigspec_init(BigSpec *b, const float *o, int n, double fps)
{
    memset(b, 0, sizeof(*b));
    /* Zero-padded four times or more for tracks up to 7 minutes; a longer
     * one is read from its middle 2^18 frames (29 minutes). */
    int M = next_pow2(RX_MAX((int64_t)n * 4, 1 << 16));
    M = RX_MIN(M, TX_MAX_REAL);
    AVTXContext *tx = NULL;
    av_tx_fn fn;
    float scale = 1.0f;
    float *buf = av_malloc((M + 2) * sizeof(*buf));
    AVComplexFloat *spec = av_malloc((M / 2 + 1) * sizeof(*spec));
    b->mag = malloc((M / 2 + 1) * sizeof(*b->mag));
    int ret = 0;
    if (!buf || !spec || !b->mag) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_tx_init(&tx, &fn, AV_TX_FLOAT_RDFT, 0, M, &scale, 0)) < 0)
        goto end;
    int len = RX_MIN(n, M);
    memcpy(buf, o + (n - len) / 2, len * sizeof(*buf));
    memset(buf + len, 0, (M - len) * sizeof(*buf));
    fn(tx, spec, buf, sizeof(float));
    for (int k = 0; k <= M / 2; k++)
        b->mag[k] = hypotf(spec[k].re, spec[k].im);
    b->M = M;
    b->nb = M / 2 + 1;
    b->fps = fps;
    b->norm = 1.0 / RX_MAX(len, 1);

end:
    av_tx_uninit(&tx);
    av_free(buf);
    av_free(spec);
    if (ret < 0) {
        free(b->mag);
        b->mag = NULL;
    }
    return ret;
}

static double bigspec_harm(const BigSpec *b, double bpm)
{
    double s = 0.0;
    for (int h = 1; h <= 4; h++) {
        double k = h * bpm / 60.0 / b->fps * b->M;
        s += interp(b->mag, b->nb, k) / h;
    }
    return s * b->norm;
}

/* The local maximum of the harmonic strength within +-1.5 % of bpm. */
static double bigspec_refine(const BigSpec *b, double bpm, double *strength)
{
    double best = bpm, bs = -1.0;
    for (double x = bpm * 0.985; x <= bpm * 1.015; x += 0.005) {
        double v = bigspec_harm(b, x);
        if (v > bs) {
            bs = v;
            best = x;
        }
    }
    /* Then to 0.0005 BPM around it. */
    double c = best;
    for (double x = c - 0.005; x <= c + 0.005; x += 0.0005) {
        double v = bigspec_harm(b, x);
        if (v > bs) {
            bs = v;
            best = x;
        }
    }
    if (strength)
        *strength = bs;
    return best;
}

/* The largest value of o within +-r frames of x. */
static double peak_near(const float *o, int n, double x, int r)
{
    int c = (int)lrint(x);
    double m = 0.0;
    for (int i = c - r; i <= c + r; i++) {
        if (i >= 0 && i < n && o[i] > m)
            m = o[i];
    }
    return m;
}

/* ------------------------------------------------------ whole-track tempo */

typedef struct Cand {
    double bpm, strength;
    int    family;
} Cand;

static bool octave_related(double a, double b, double tol)
{
    double r = log2(a / b);
    return fabs(r - round(r)) < tol / M_LN2;
}

int rx_tempo_estimate(const RXOnsets *o, double lo, double hi, double prior,
                      RXTempoEstimate *out)
{
    memset(out, 0, sizeof(*out));
    const int n = o->nb;
    const double fps = o->fps;
    if (n < (int)(4 * fps))
        return RX_ERROR(EINVAL);
    if (prior <= 0.0)
        prior = DEFAULT_PRIOR;
    bool ranged = lo > 0.0 && hi > lo;
    if (!ranged) {
        lo = RX_TEMPO_MIN;
        hi = RX_TEMPO_MAX;
    }

    /* Lags up to four beats at the slowest tempo, plus room to interpolate. */
    const int nlag = (int)(4 * 60.0 * fps / RX_TEMPO_MIN) + 4;
    float *ac = malloc((size_t)nlag * sizeof(*ac));
    if (!ac)
        return RX_ERROR(ENOMEM);
    int ret = autocorr(o->full, n, ac, nlag);
    if (ret < 0) {
        free(ac);
        return ret;
    }

    /* Comb over 50..220 BPM in 0.05 steps; the six strongest peaks. */
    const int nc = (int)((RX_TEMPO_MAX - RX_TEMPO_MIN) / 0.05);
    double peaks[6] = { 0 }, pv[6] = { 0 };
    double prev2 = 0.0, prev = 0.0;
    for (int i = 0; i <= nc; i++) {
        double v = comb(ac, nlag, fps, RX_TEMPO_MIN + i * 0.05);
        if (i >= 2 && prev > prev2 && prev >= v) {
            double b = RX_TEMPO_MIN + (i - 1) * 0.05;
            for (int j = 0; j < 6; j++) {
                if (prev > pv[j]) {
                    memmove(peaks + j + 1, peaks + j, (5 - j) * sizeof(*peaks));
                    memmove(pv + j + 1, pv + j, (5 - j) * sizeof(*pv));
                    peaks[j] = b;
                    pv[j] = prev;
                    break;
                }
            }
        }
        prev2 = prev;
        prev = v;
    }
    free(ac);

    BigSpec bs;
    if ((ret = bigspec_init(&bs, o->full, n, fps)) < 0)
        return ret;

    /* Candidates: each peak and its simple relatives, rescored by their
     * harmonic series. */
    static const double ratios[] = { 1.0, 2.0, 0.5, 1.5, 2.0 / 3.0 };
    Cand cands[6 * RX_ARRAY_ELEMS(ratios)];
    int nb = 0;
    for (int p = 0; p < 6; p++) {
        if (peaks[p] <= 0.0)
            continue;
        for (size_t r = 0; r < RX_ARRAY_ELEMS(ratios); r++) {
            double c = peaks[p] * ratios[r];
            if (c < RX_TEMPO_MIN || c > RX_TEMPO_MAX)
                continue;
            double st;
            c = bigspec_refine(&bs, c, &st);
            bool dup = false;
            for (int j = 0; j < nb; j++) {
                if (fabs(cands[j].bpm / c - 1.0) < 0.004) {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                cands[nb++] = (Cand) { .bpm = c, .strength = st, .family = -1 };
        }
    }
    if (!nb) {
        free(bs.mag);
        return RX_ERROR(EINVAL);
    }

    /* Octave families; a family's strength is its strongest member's. */
    int nfam = 0;
    double fam_strength[RX_ARRAY_ELEMS(cands)];
    double fam_bpm[RX_ARRAY_ELEMS(cands)];
    for (int i = 0; i < nb; i++) {
        for (int f = 0; f < nfam && cands[i].family < 0; f++) {
            if (octave_related(cands[i].bpm, fam_bpm[f], 0.01))
                cands[i].family = f;
        }
        if (cands[i].family < 0) {
            cands[i].family = nfam;
            fam_bpm[nfam] = cands[i].bpm;
            fam_strength[nfam++] = 0.0;
        }
        int f = cands[i].family;
        if (cands[i].strength > fam_strength[f]) {
            fam_strength[f] = cands[i].strength;
            fam_bpm[f] = cands[i].bpm;
        }
    }
    /* The strongest family; with a requested range, the strongest that
     * has a member inside it (asked for 180-210, a track read at 130 is
     * the 3:2 family's 195, not 130 folded to 260). */
    int best_fam = -1;
    for (int f = 0; f < nfam; f++) {
        if (ranged) {
            double c = fam_bpm[f];
            while (c > hi)
                c /= 2.0;
            while (c < lo)
                c *= 2.0;
            if (c > hi)
                continue;
        }
        if (best_fam < 0 || fam_strength[f] > fam_strength[best_fam])
            best_fam = f;
    }
    if (best_fam < 0) {
        best_fam = 0;
        for (int f = 1; f < nfam; f++) {
            if (fam_strength[f] > fam_strength[best_fam])
                best_fam = f;
        }
    }
    int second_fam = -1;
    for (int f = 0; f < nfam; f++) {
        if (f != best_fam && (second_fam < 0 || fam_strength[f] > fam_strength[second_fam]))
            second_fam = f;
    }

    /* Every octave of the winning family inside the allowed range, each
     * refined on its own harmonic series. The one nearest the prior is
     * the working octave: the rhythm evidence needs beats placed on the
     * hits, so the octave is decided again on the fitted grid
     * (rx_tempo_octave_decide). */
    double base = fam_bpm[best_fam];
    while (base / 2.0 >= lo * 0.999)
        base /= 2.0;
    while (base < lo * 0.999)
        base *= 2.0;
    double obpm[4], oscore[4];
    int no = 0;
    for (double c = base; c <= hi * 1.001 && no < 4; c *= 2.0) {
        double st;
        double r = bigspec_refine(&bs, c, &st);
        obpm[no] = r;
        oscore[no] = exp(-0.5 * pow(log2(r / prior) / OCTAVE_PRIOR_WIDTH, 2.0));
        no++;
    }
    if (!no) {
        /* The range is narrower than an octave and misses the family:
         * fold the family's best into it anyway. */
        double c = fam_bpm[best_fam];
        while (c > hi)
            c /= 2.0;
        while (c < lo)
            c *= 2.0;
        obpm[0] = c;
        oscore[0] = 1.0;
        no = 1;
    }
    int bo = 0;
    for (int i = 1; i < no; i++) {
        if (oscore[i] > oscore[bo])
            bo = i;
    }
    out->bpm = obpm[bo];
    out->strength = bigspec_harm(&bs, out->bpm);
    double other = 0.0;
    for (int i = 0; i < no; i++) {
        if (i != bo && oscore[i] > other) {
            other = oscore[i];
            out->octave_bpm = obpm[i];
        }
    }
    out->octave_margin = other > 0.0 ? oscore[bo] / other : 10.0;
    if (second_fam >= 0) {
        double c = fam_bpm[second_fam];
        while (c > out->bpm * M_SQRT2)
            c /= 2.0;
        while (c < out->bpm / M_SQRT2)
            c *= 2.0;
        out->second_bpm = c;
        out->second_ratio = fam_strength[second_fam] / fmax(fam_strength[best_fam], 1e-12);
    }
    free(bs.mag);
    return 0;
}

/* ------------------------------------------------------- octave on a grid */

/* Mid-band (snare, clap) balance and subdivision of the beats
 * t0 + k * P in [from, to), the kick band's the same, and both by the
 * position in a bar of four beats (beats and half beats: eight places).
 *
 * The bar positions are there because parity averages hide a beat in
 * four: on "Love me, myself" (STYLEY NiGHT ii, 100 BPM) the 200 level's
 * kick averages 1.55 on its even beats and 2.03 on its odd ones (0.77,
 * "alike"), but by its four beats it is 0.68, 1.73, 2.42, 2.31: a pop
 * kick on 1, the "and" of 3 and late in 4 at 100, no four-on-the-floor
 * at 200.
 *
 * The snare of a position is the mid band above its mean where the low
 * band is not above its own (mid / mean - low / mean): a kick's body
 * reaches into the mid band, and on drum & bass ("Desolate") the
 * strongest mid-band beat is the kick on 1, not a snare. */
static void level_evidence(const RXOnsets *o, double t0, double P, double from, double to,
                           RXOctaveEvidence *ev)
{
    const double fps = o->fps;
    const int n = o->nb;
    double a[2] = { 0 }, half = 0.0, k[2] = { 0 }, khalf = 0.0;
    double bm[8] = { 0 }, bk[8] = { 0 };
    int na[2] = { 0 }, nh = 0, nb[4] = { 0 };
    long j0 = (long)ceil((from - t0) / P), j1 = (long)floor((to - t0 - P / 2) / P);
    for (long j = j0; j <= j1; j++) {
        double t = (t0 + j * P) * fps;
        if (t < 2 || t + P / 2 * fps >= n - 2)
            continue;
        double m = peak_near(o->mid, n, t, 2), kb = peak_near(o->low, n, t, 2);
        double mh = peak_near(o->mid, n, t + P / 2 * fps, 2);
        double kh = peak_near(o->low, n, t + P / 2 * fps, 2);
        a[j & 1] += m;
        k[j & 1] += kb;
        na[j & 1]++;
        half += mh;
        khalf += kh;
        nh++;
        int q = (int)(((j % 4) + 4) % 4);
        bm[2 * q] += m;
        bk[2 * q] += kb;
        bm[2 * q + 1] += mh;
        bk[2 * q + 1] += kh;
        nb[q]++;
    }
    memset(ev, 0, sizeof(*ev));
    if (!na[0] || !na[1] || !nh)
        return;
    double e0 = a[0] / na[0], e1 = a[1] / na[1], eh = half / nh;
    double k0 = k[0] / na[0], k1 = k[1] / na[1];
    ev->balance = fmax(e0, e1) > 0.0 ? fmin(e0, e1) / fmax(e0, e1) : 0.0;
    ev->subdiv = e0 + e1 > 0.0 ? eh / (0.5 * (e0 + e1)) : 1.0;
    ev->kick_alt = fmax(k0, k1) > 0.0 ? fmin(k0, k1) / fmax(k0, k1) : 1.0;
    ev->kick_sub = k0 + k1 > 0.0 ? khalf / nh / (0.5 * (k0 + k1)) : 1.0;

    ev->kick_bar = 1.0;
    if (!nb[0] || !nb[1] || !nb[2] || !nb[3])
        return;
    double mm = 0.0, mk = 0.0;
    for (int i = 0; i < 8; i++) {
        bm[i] /= nb[i / 2];
        bk[i] /= nb[i / 2];
        mm += bm[i] / 8.0;
        mk += bk[i] / 8.0;
    }
    double kmin = bk[0], kmax = bk[0];
    for (int q = 1; q < 4; q++) {
        kmin = fmin(kmin, bk[2 * q]);
        kmax = fmax(kmax, bk[2 * q]);
    }
    ev->kick_bar = kmax > 0.0 ? kmin / kmax : 1.0;
    if (mm <= 0.0 || mk <= 0.0)
        return;
    double sn[4];
    int best = 0;
    for (int q = 0; q < 4; q++) {
        sn[q] = bm[2 * q] / mm - bk[2 * q] / mk;
        if (sn[q] > sn[best])
            best = q;
    }
    if (sn[best] > 0.0) {
        ev->snare = sn[best];
        ev->snare_lone = 1.0 - fmax(sn[(best + 2) % 4], 0.0) / sn[best];
    }
}

/* Of the beats t0 + k * P and those moved by `alt`, the set whose beats
 * carry more of a (+ b) in [from, to). */
static double level_phase(const RXOnsets *o, const float *a, const float *b, double t0, double P,
                          double alt, double from, double to)
{
    double sum[2] = { 0 };
    for (int s = 0; s < 2; s++) {
        double ts = t0 + s * alt;
        long j0 = (long)ceil((from - ts) / P), j1 = (long)floor((to - ts) / P);
        for (long j = j0; j <= j1; j++) {
            double t = (ts + j * P) * o->fps;
            sum[s] += peak_near(a, o->nb, t, 2) + (b ? peak_near(b, o->nb, t, 2) : 0.0);
        }
    }
    return sum[1] > sum[0] ? t0 + alt : t0;
}

/* A kick on every beat of the level alike: as many on its even beats as on
 * its odd ones (dj's alternation), and on each of the bar's four beats,
 * the weakest at least 0.4 of the strongest. The parity alone takes a
 * pop kick at twice its tempo for one: "Love me, myself" at 200 (0.77
 * by parity, 0.28 by the bar; the instrumental 0.75 and 0.30), Clean
 * Tears' "Skyscraper" at 196 (0.77, 0.27), whose low band has eighths
 * in its louder sections, three beats in four. On the acceptance list one
 * other level the rules read changes: AD:HOUSE 7's "琥珀" at 121 (0.76,
 * 0.29: by the bar 5.1, 4.1, 2.1, 1.5, a kick on 1 and 2, no
 * four-on-the-floor), whose octave stands (margin 68, now 27); the rest
 * have 0.45 or more. */
static bool kick_on_every_beat(const RXOctaveEvidence *ev)
{
    return ev->kick_alt >= 0.7 && ev->kick_bar >= 0.4;
}

/* The octave on a fitted grid. The witnesses are the snares and claps
 * (the mid band, 150-2500 Hz, aligned to the full band), because they are
 * what places the beat in nearly all of this music: on 2 and 4.
 *  - Too slow: at half the true tempo the snares fall on the half beats,
 *    as strong as the beats (drum & bass at 87; house at 64, whose claps
 *    on 2 and 4 become the "and"s). Subdivision near 1. Not where the
 *    level above has a lone snare, one beat in four: that level is the
 *    half-time reading of this one (pop at 100 whose eighths fill the
 *    half beats, read at 200).
 *  - Too fast: at twice the true tempo every other beat is a 16th without
 *    snare or clap. Balance well below 1.
 * The kick band was tried as a third witness and made things worse: an
 * off-beat bass fills it between the kicks. The prior, log-normal around
 * 125 BPM and 0.4 octave wide, settles what the rhythm leaves open; it
 * weighs 64 against 128 heavily but 87 against 174 hardly at all.
 * Measured on the 81 tracks of the evaluation sample whose octave is
 * certain (house, techno, garage at 100-160; drum & bass at 150-200): the
 * prior alone is right on 78, this rule on all 81 (docs/BEATS.md). */
void rx_tempo_octave_decide(const RXOnsets *o, double t0, double period, double from, double to,
                            double lo, double hi, double prior, RXOctaveDecision *out)
{
    memset(out, 0, sizeof(*out));
    out->factor = 1.0;
    out->margin = 10.0;
    if (prior <= 0.0)
        prior = DEFAULT_PRIOR;
    bool automatic = !(lo > 0.0 && hi > lo);
    if (automatic) {
        lo = RX_TEMPO_MIN;
        hi = RX_TEMPO_MAX;
    }
    /* The levels' beats, chosen from the finest down. The line may sit on
     * any eighth of the double level (a tracker held at half the tempo can
     * lock onto the "and"s: the off-beat is decided later, with the
     * structure). Double: the line's beats and half beats, or those a
     * quarter beat later, whichever carry more kick and snare. Same: every
     * other one of those, again by kick and snare (by the kick alone, a
     * drum & bass track whose kicks fall on the "and"s put the snares on
     * its half beats and lost to half tempo). Half: every other one of
     * those, the parity with more kick (at half tempo the beats would be
     * the kicks; with the snares counted, drum & bass's snares would win
     * and hide the test). */
    double t2 = level_phase(o, o->low, o->mid, t0, period / 2.0, period / 4.0, from, to);
    double t1 = level_phase(o, o->low, o->mid, t2, period, period / 2.0, from, to);
    double th = level_phase(o, o->low, NULL, t1, 2.0 * period, period, from, to);
    const double lt0[3] = { th, t1, t2 };
    const double lp[3] = { 2.0 * period, period, period / 2.0 };
    double score[3];
    out->from = from;
    out->to = to;
    for (int l = 0; l < 3; l++) {
        out->bpm[l] = 60.0 / lp[l];
        out->t0[l] = lt0[l];
        level_evidence(o, lt0[l], lp[l], from, to, &out->ev[l]);
    }
    for (int l = 0; l < 3; l++) {
        double slow = RX_CLAMP((1.0 - out->ev[l].subdiv) / 0.5, OCTAVE_FLOOR, 1.0);
        double fast = RX_CLAMP(out->ev[l].balance / 0.8, OCTAVE_FLOOR, 1.0);
        double pr = exp(-0.5 * pow(log2(out->bpm[l] / prior) / OCTAVE_PRIOR_WIDTH, 2.0));
        /* The snares on the half beats make a level too slow because
         * they are the next level's backbeat, its 2 and 4. Where the
         * next level up has a lone snare instead, one beat in four with
         * nothing like it two beats on, it is the half-time reading of
         * this level, and what fills this level's half beats is
         * eighth-note syncopation: pop at 100 with a kick on the "and"s
         * and eighths in the mid band. "Love me, myself" (STYLEY NiGHT
         * ii, 100 BPM by ear) has its snare on 2 and 4 at 100 (by the
         * snare measure of level_evidence +1.13 and +0.90 on beats two
         * apart, -1.24 and +0.24 on the others) and alone at 200 (+1.85,
         * -0.17 two beats on); Clean Tears' "Skyscraper" (98 by ear) is
         * alone at 196 (+0.64, +0.10). Drum & bass is not: on the eight
         * acceptance tracks at 172-180 its snares on 2 and 4 pair (lone
         * 0.08-0.59), or are under 0.5, which is no evidence either way. */
        if (l < 2 && out->ev[l + 1].snare >= 0.5 && out->ev[l + 1].snare_lone >= 0.8)
            slow = 1.0;
        /* A four-on-the-floor kick (on every beat alike, hardly between)
         * is the beat whatever the mid band does: a stab on the "and"
         * must not halve a house track. Only from 90 BPM: a kick on 1 and
         * 3 of a 125 track looks like that at 62.5. It only ever adds:
         * as a veto the kick band fails, an off-beat bass fills it. */
        if (out->bpm[l] >= 90.0 && kick_on_every_beat(&out->ev[l]) && out->ev[l].kick_sub <= 0.4)
            pr *= 3.0;
        /* Kicks on the half beats make a level too slow when they belong
         * to a four-on-the-floor one level up (every beat of it has a kick
         * alike: UK hardcore at 175 read at 87.5), not when they are a
         * syncopation (drum & bass's kick on the "and" of 3, whose next
         * level up alternates; a pop kick on 1, the "and" of 3 and of 4,
         * one beat in four without a kick at the next level up). Nor
         * when that level is beyond the range (above 200 BPM without
         * one): a busy drum & bass kick, on the beats and the "and"s, is
         * no four-on-the-floor at 344. */
        double up_hi = automatic ? OCTAVE_AUTO_HI : hi;
        if (l < 2 && kick_on_every_beat(&out->ev[l + 1]) && out->bpm[l + 1] <= up_hi * 1.001)
            slow = fmin(slow, RX_CLAMP((1.0 - out->ev[l].kick_sub) / 0.5, OCTAVE_FLOOR, 1.0));
        /* Without a requested range, a level above 200 or below 60 BPM
         * must win by a factor of four: a pop song at 105 with sixteenths
         * on the hats reads as a four-on-the-floor 210. (Drum & bass at
         * 170-180 is inside.) */
        if (automatic && (out->bpm[l] > OCTAVE_AUTO_HI || out->bpm[l] < OCTAVE_AUTO_LO))
            pr *= 0.25;
        out->ev[l].score = slow * fast;
        bool in = out->bpm[l] >= lo * 0.999 && out->bpm[l] <= hi * 1.001;
        score[l] = in ? slow * fast * pr : -1.0;
    }
    /* The working octave stays unless another level is in the range and
     * scores better. */
    int best = 1;
    for (int l = 0; l < 3; l++) {
        if (score[l] >= 0.0 && score[l] > score[best])
            best = l;
    }
    double second = 0.0;
    for (int l = 0; l < 3; l++) {
        if (l != best && score[l] > second)
            second = score[l];
    }
    out->factor = best == 0 ? 0.5 : best == 2 ? 2.0 : 1.0;
    out->margin = second > 0.0 ? score[best] / second : 10.0;
}

/* ------------------------------------------------------------ tempogram */

#define TG_WIN   12.0      /* seconds: three bars at 60 BPM, eight at 160 */
#define TG_HOP   3.0
#define TG_RUN   4         /* windows a different tempo must hold: 21 s */
#define TG_STEP  0.005     /* log2 step of the tempo states: 0.35 % */

int rx_tempo_regions(const RXOnsets *o, double bpm, double lo, double hi,
                     RXTempoRegion **regions, int *nb)
{
    *regions = NULL;
    *nb = 0;
    const double fps = o->fps, dur = o->nb / fps;
    const int W = (int)(TG_WIN * fps), H = (int)(TG_HOP * fps);
    const int nlag = (int)(4 * 60.0 * fps / (bpm / M_SQRT2)) + 4;
    int nw = o->nb > W ? (o->nb - W) / H + 1 : 0;
    /* The states: an octave around the main tempo, inside the requested
     * range when there is one. */
    double e_lo = -0.5, e_hi = 0.5;
    if (lo > 0.0 && hi > lo) {
        e_lo = fmax(e_lo, log2(lo / bpm));
        e_hi = fmin(e_hi, log2(hi / bpm));
    }

    double *best = calloc(RX_MAX(nw, 1), sizeof(*best));
    double *ratio = calloc(RX_MAX(nw, 1), sizeof(*ratio));
    double *power = calloc(RX_MAX(nw, 1), sizeof(*power));
    float *seg = malloc((size_t)W * sizeof(*seg));
    float *ac = malloc((size_t)RX_MAX(nlag, 1) * sizeof(*ac));
    int ret = 0;
    if (!best || !ratio || !power || !seg || !ac) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    if (nlag >= W / 2)
        nw = 0;

    /* Per window: the strongest tempo within the octave around the main
     * one (the comb sums the beat, two-beat and bar lags, so an octave
     * relative of the main tempo, a half-time stretch, still reads as the
     * main tempo), how strongly the window is rhythmic at all, and how
     * well the main tempo itself does there. */
    for (int w = 0; w < nw; w++) {
        const float *src = o->full + (size_t)w * H;
        double mean = 0.0;
        for (int i = 0; i < W; i++)
            mean += src[i];
        mean /= W;
        for (int i = 0; i < W; i++)
            seg[i] = (float)((src[i] - mean) * (0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / W)));
        if ((ret = autocorr(seg, W, ac, nlag)) < 0)
            goto end;
        double bb = bpm, bv = -INFINITY;
        for (double e = e_lo; e < e_hi; e += TG_STEP) {
            double t = bpm * exp2(e), v = comb(ac, nlag, fps, t);
            if (v > bv) {
                bv = v;
                bb = t;
            }
        }
        best[w] = bb;
        power[w] = bv;
        double vm = comb(ac, nlag, fps, bpm);
        ratio[w] = bv > 0.0 ? vm / bv : 1.0;
    }

    /* If most rhythmic windows agree on one tempo that is not the main
     * one, the whole-track estimate chose the wrong family (a 4:3 or 3:2
     * reading of a syncopated track): that tempo is the main one. */
    {
        int nr_w = 0;
        for (int k = 0; k < nw; k++)
            nr_w += power[k] > 0.3;
        double cand = 0.0;
        int votes = 0;
        for (int k = 0; k < nw; k++) {
            if (power[k] <= 0.3)
                continue;
            int v = 0;
            for (int m = 0; m < nw; m++)
                v += power[m] > 0.3 && fabs(log2(best[m] / best[k])) < log2(1.015);
            if (v > votes) {
                votes = v;
                cand = best[k];
            }
        }
        int own = 0;
        for (int k = 0; k < nw; k++)
            own += power[k] > 0.3 && fabs(log2(best[k] / bpm)) < log2(1.015);
        /* Not at 3:2, 2:3, 4:3 or 3:4 of it: between those the grids of
         * both families are fitted and compared (beats.c), which the
         * windows' autocorrelation cannot do as well. */
        bool rel = false;
        static const double fam[] = { 1.5, 2.0 / 3.0, 4.0 / 3.0, 0.75 };
        for (size_t q = 0; q < RX_ARRAY_ELEMS(fam); q++) {
            if (fabs(log2(cand / bpm / fam[q])) < log2(1.01))
                rel = true;
        }
        if (nr_w >= 2 * TG_RUN && votes > nr_w / 2 && own < nr_w / 4 && !rel &&
            fabs(log2(cand / bpm)) > log2(1.03)) {
            rx_debug("tempo: windows settle on %.2f, not %.2f\n", cand, bpm);
            bpm = cand;
            for (int k = 0; k < nw; k++) {
                /* The main tempo's comb relative to each window's best is
                 * not kept per window; a window agreeing with the new main
                 * tempo is simply not "other". */
                if (fabs(log2(best[k] / bpm)) < log2(1.015))
                    ratio[k] = 1.0;
            }
        }
    }

    /* Runs of windows that settle on one other tempo: at least TG_RUN in a
     * row, each within 1.5 % of the run's first window, whose tempo is more
     * than 3 % from the main one; rhythmic (comb above 0.3), with the main
     * tempo doing clearly worse there (below 0.7 of the best). The region
     * takes the run's median tempo. Anything shorter is a fill, a riser or
     * a triplet passage, and the tracker's own slack covers small drifts. */
    RXTempoRegion *r = malloc(sizeof(*r) * (size_t)(nw + 1));
    if (!r) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    int nr = 0;
    int w = 0;
    r[nr++] = (RXTempoRegion) { .start = 0.0, .end = dur, .bpm = bpm };
    while (w < nw) {
        /* Not a tempo a rhythm makes on its own at the main tempo: a
         * triplet or dotted passage reads at 3:2, 2:3, 4:3 or 3:4 of it
         * (rbxport's rule, within 1 %: 128 to 174 is 1.9 % from 4:3). */
        static const double rhythmic[] = { 1.5, 2.0 / 3.0, 4.0 / 3.0, 0.75 };
        bool ratio_of_beat = false;
        for (size_t q = 0; q < RX_ARRAY_ELEMS(rhythmic); q++) {
            if (fabs(log2(best[w] / bpm / rhythmic[q])) < log2(1.01))
                ratio_of_beat = true;
        }
        bool other = power[w] > 0.3 && ratio[w] < 0.7 && fabs(log2(best[w] / bpm)) > log2(1.03) &&
                     !ratio_of_beat;
        if (!other) {
            w++;
            continue;
        }
        int e = w + 1;
        while (e < nw && fabs(log2(best[e] / best[w])) < log2(1.015) && power[e] > 0.3 &&
               ratio[e] < 0.7)
            e++;
        if (e - w >= TG_RUN) {
            /* The median tempo of the run. */
            double tmp[256];
            int m = RX_MIN(e - w, 256);
            for (int k = 0; k < m; k++)
                tmp[k] = best[w + k];
            qsort(tmp, (size_t)m, sizeof(*tmp), cmp_double);
            double t0 = (w * H + W / 2.0) / fps - TG_HOP / 2.0;
            double t1 = ((e - 1) * H + W / 2.0) / fps + TG_HOP / 2.0;
            r[nr - 1].end = t0;
            r[nr++] = (RXTempoRegion) { .start = t0, .end = t1, .bpm = tmp[m / 2] };
            r[nr++] = (RXTempoRegion) { .start = t1, .end = dur, .bpm = bpm };
            rx_debug("tempo: region %.1f-%.1f s at %.2f BPM\n", t0, t1, tmp[m / 2]);
        }
        w = e;
    }
    *regions = r;
    *nb = nr;

end:
    free(best);
    free(ratio);
    free(power);
    free(seg);
    free(ac);
    return ret;
}
