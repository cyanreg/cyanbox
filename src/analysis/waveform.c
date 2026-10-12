/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The ANLZ waveform tags (PWAV, PWV2, PWV3, PWV4, PWV5, PWV6, PWV7 and the
 * PWVC gains) from the mono mix at the file's native rate, and their packed
 * form (the library's blob and the .rxw file) that keeps them between runs.
 *
 * This is a port of dj's analysis/waveform.py that reproduces its output
 * byte for byte (tests/analysis/test_waveform.c checks every tag against
 * dj's output on synthetic signals). The rules and constants come from
 * there; dj took them from the baken project (MIT licence,
 * https://github.com/M-Igashi/baken, crates/baken-export/src/anlz/generate/
 * waveform.rs), which fitted them on 1070 rekordbox-analysed tracks. The
 * monochrome height and whiteness rules (PWV3) reproduce rekordbox byte for
 * byte on 99.5 % of columns; the colour and three-band rules are fitted
 * approximations. docs/anlz.md has the tag layouts.
 *
 * Byte agreement with numpy depends on doing every operation in the same
 * order in double precision, so the expressions below keep dj's order
 * ((24 * r) / max, not 24 * (r / max)), round with dj's floor(v + 0.5), and
 * the filters are scipy's lfilter (direct form II transposed) step by step.
 * The build is ISO C23, where GCC does not contract a * b + c into a fused
 * multiply-add, which would change the last bit of some products.
 *
 * Unlike dj, which runs each filter over the whole track in float64 arrays
 * (several hundred MB for a long hi-res file), the samples are streamed
 * once through all five filters and reduced to per-column values on the way:
 * the arithmetic per sample is the same, so the results are too.
 *
 * Deliberate differences from dj:
 *
 * - Two firmware fixes measured by rbxport (docs/OVERVIEW.md section 3,
 *   item 2) and confirmed on ~2100 rekordbox files: PWAV heights are never
 *   below 2 and PWV2 heights never below 1. rekordbox never writes lower
 *   values, and Nexus firmware rejects a preview that has a zero column.
 * - The three-band levels of PWV7 and PWV6 are recalibrated per band
 *   against rekordbox's own bytes (see "PWV7" and "PWV6" below): dj's mid
 *   and high bands are several times too hot, so loud masters drew an
 *   all-cream overview. PWAV, PWV2, PWV3, PWV4, PWV5 and PWVC stay
 *   byte-identical to dj (PWV4 is still derived from dj's PWV6 levels,
 *   which baken fitted it against). */

#include "rxbox/analysis.h"

#include <math.h>
#include <stdckdint.h>

#include "rxbox/common.h"

/* Every detail column is 1/150 s (docs/anlz.md, "half-frame"). */
#define COLUMNS_PER_SECOND 150

/* Filter corners in Hz (baken's fit). Whiteness is how much of a column's
 * peak survives a 150 Hz low-pass; the three bands of PWV5/PWV7 are a
 * 300 Hz low-pass, a 250..1500 Hz band (high-pass then low-pass) and a
 * 2500 Hz high-pass. */
#define WHITENESS_LOWPASS_HZ 150.0
#define LOW_HZ      300.0
#define MID_LOW_HZ  250.0
#define MID_HIGH_HZ 1500.0
#define HIGH_HZ     2500.0

/* Per-column release of the band peak envelopes, low/mid/high. */
static const double RELEASE[3]     = { 0.97, 0.93, 0.90 };
/* PWV7 gain: the level the loudest column of each band is scaled to, and
 * the floors and caps of the gain, which is also what PWVC stores (rekordbox
 * files show low ~80, mid 80..200, high up to 500: docs/anlz.md 4.14). */
static const double GAIN_TARGET[3] = { 78.0, 105.0, 115.0 };
static const double GAIN_FLOOR[3]  = { 80, 80, 95 };
static const double GAIN_CAP[3]    = { 267, 300, 500 };
static const double BAND_SCALE[3]  = { 1.25, 1.15, 0.6 };
/* dj's PWV6 = range mean of dj's PWV7 times these (baken's fit of
 * rekordbox's PWV6 against rekordbox's PWV7), and PWV4 bytes 3..5 = that
 * PWV6 times these. rxbox writes its own PWV6 and keeps dj's only for PWV4. */
static const double PWV6_SCALE[3]  = { 0.56, 0.71, 2.0 };
static const double PWV4_SCALE[3]  = { 2.4, 1.6, 1.1 };

/* The firmware minimums (see the top of the file). */
#define PWAV_MIN_HEIGHT 2
#define PWV2_MIN_HEIGHT 1

/* dj's _round: half away from zero for the non-negative values used here. */
static inline double round_half(double v)
{
    return floor(v + 0.5);
}

/* An RBJ cookbook biquad with Q = 1/sqrt(2), normalised by a0 as dj does
 * before handing it to lfilter. */
typedef struct Biquad {
    double b0, b1, b2, a1, a2;
    double z0, z1;
} Biquad;

static void biquad_init(Biquad *f, bool highpass, double fc, int rate)
{
    double w = 2 * M_PI * fc / rate;
    double s = sin(w), c = cos(w);
    double alpha = s / (2 * sqrt(0.5));
    double b[3], a[3] = { 1 + alpha, -2 * c, 1 - alpha };
    if (highpass) {
        b[0] = (1 + c) / 2;
        b[1] = -(1 + c);
        b[2] = (1 + c) / 2;
    } else {
        b[0] = (1 - c) / 2;
        b[1] = 1 - c;
        b[2] = (1 - c) / 2;
    }
    *f = (Biquad) {
        .b0 = b[0] / a[0],
        .b1 = b[1] / a[0],
        .b2 = b[2] / a[0],
        .a1 = a[1] / a[0],
        .a2 = a[2] / a[0],
    };
}

/* One step of scipy's lfilter: direct form II transposed, the same
 * operations in the same order as scipy's DOUBLE_filt. */
static inline double biquad_step(Biquad *f, double x)
{
    double y = f->z0 + f->b0 * x;
    f->z0 = f->z1 + x * f->b1 - y * f->a1;
    f->z1 = x * f->b2 - y * f->a2;
    return y;
}

/* dj's _split: `parts` consecutive ranges [a, b) of n items, each at least
 * one item long. */
static void split_range(int64_t n, int parts, int k, int64_t *a, int64_t *b)
{
    *a = RX_MIN((int64_t)k * n / parts, n - 1);
    *b = RX_CLAMP((int64_t)(k + 1) * n / parts, *a + 1, n);
}

/* Per-column reductions of the samples. */
typedef struct Columns {
    int64_t n;
    double *peak, *white, *band;     /* band: n * 3, low/mid/high */
    double *sumsq;
    int64_t *count;
} Columns;

static void columns_free(Columns *c)
{
    free(c->peak);
    free(c->white);
    free(c->band);
    free(c->sumsq);
    free(c->count);
}

static int columns_compute(const float *x, int64_t ns, int rate, Columns *c)
{
    /* n = max(1, ceil(samples * 150 / rate)); column k starts at
     * round(k * rate / 150) (docs/anlz.md, "Waveform-detail column grid").
     * For MP3, `ns` already counts every MPEG frame from sample 0, so this
     * is rekordbox's frames * 1152 * 150 / rate. */
    int64_t n = (int64_t)ceil((double)(ns * COLUMNS_PER_SECOND) / rate);
    n = RX_MAX(n, 1);
    if (n > INT32_MAX / 6)
        return RX_ERROR(EINVAL);
    *c = (Columns) { .n = n };
    c->peak  = calloc(n, sizeof(*c->peak));
    c->white = calloc(n, sizeof(*c->white));
    c->band  = calloc(n * 3, sizeof(*c->band));
    c->sumsq = calloc(n, sizeof(*c->sumsq));
    c->count = calloc(n, sizeof(*c->count));
    if (!c->peak || !c->white || !c->band || !c->sumsq || !c->count) {
        columns_free(c);
        return RX_ERROR(ENOMEM);
    }

    Biquad white, low, mid_hp, mid_lp, high;
    biquad_init(&white,  false, WHITENESS_LOWPASS_HZ, rate);
    biquad_init(&low,    false, LOW_HZ, rate);
    biquad_init(&mid_hp, true,  MID_LOW_HZ, rate);
    biquad_init(&mid_lp, false, MID_HIGH_HZ, rate);
    biquad_init(&high,   true,  HIGH_HZ, rate);

    /* The sum of squares per column is a difference of the running sum,
     * as dj's _colsum takes it from np.cumsum. */
    double cum = 0.0;
    int64_t start = 0;
    for (int64_t k = 0; k < n; k++) {
        int64_t end = (int64_t)round_half((double)((k + 1) * rate) / COLUMNS_PER_SECOND);
        end = RX_MIN(end, ns);
        double cum0 = cum;
        double pk = 0.0, wh = 0.0, lo = 0.0, md = 0.0, hi = 0.0;
        for (int64_t i = start; i < end; i++) {
            double v = x[i];
            pk = fmax(pk, fabs(v));
            wh = fmax(wh, fabs(biquad_step(&white, v)));
            lo = fmax(lo, fabs(biquad_step(&low, v)));
            md = fmax(md, fabs(biquad_step(&mid_lp, biquad_step(&mid_hp, v))));
            hi = fmax(hi, fabs(biquad_step(&high, v)));
            cum += v * v;
        }
        c->peak[k] = pk;
        c->white[k] = wh;
        c->band[k * 3 + 0] = lo;
        c->band[k * 3 + 1] = md;
        c->band[k * 3 + 2] = hi;
        c->sumsq[k] = cum - cum0;
        c->count[k] = end - start;
        start = end;
    }
    return 0;
}

/* dj's rms_over: RMS of the samples over `parts` consecutive column groups,
 * from running sums of the column sums of squares and sample counts. */
static void rms_over(const Columns *c, const double *cs, const int64_t *cc,
                     int parts, double *out, double *max)
{
    *max = 0.0;
    for (int k = 0; k < parts; k++) {
        int64_t a, b;
        split_range(c->n, parts, k, &a, &b);
        double s = cs[b] - cs[a];
        int64_t cnt = cc[b] - cc[a];
        out[k] = cnt > 0 ? sqrt(s / (double)cnt) : 0.0;
        *max = fmax(*max, out[k]);
    }
}

/* dj's PWV6 rule: range means of PWV7 over a 1200-way split, rescaled per
 * band. `acc` holds (n + 1) * 3 running sums. rxbox uses it only for dj's
 * PWV6 levels that PWV4 is built from. */
static void pwv6_dj(const uint8_t *pwv7, int64_t n, double *acc, uint8_t *pwv6)
{
    for (int j = 0; j < 3; j++)
        acc[j] = 0.0;
    for (int64_t i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            acc[(i + 1) * 3 + j] = acc[i * 3 + j] + pwv7[i * 3 + j];
    for (int k = 0; k < RX_WAVE_PWV6_COLUMNS; k++) {
        int64_t a, b;
        split_range(n, RX_WAVE_PWV6_COLUMNS, k, &a, &b);
        for (int j = 0; j < 3; j++) {
            double mean = (acc[b * 3 + j] - acc[a * 3 + j]) / (double)(b - a);
            pwv6[k * 3 + j] = (uint8_t)fmin(127, round_half(PWV6_SCALE[j] * mean));
        }
    }
}

/* rxbox's PWV6 (see its use): range means of rxbox's PWV7 over a 1200-way
 * split, 0.5 * mean for low and mid, 0.5 * sqrt(127 * mean) for high. */
static void pwv6_rxbox(const uint8_t *pwv7, int64_t n, double *acc, uint8_t *pwv6)
{
    for (int j = 0; j < 3; j++)
        acc[j] = 0.0;
    for (int64_t i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            acc[(i + 1) * 3 + j] = acc[i * 3 + j] + pwv7[i * 3 + j];
    for (int k = 0; k < RX_WAVE_PWV6_COLUMNS; k++) {
        int64_t a, b;
        split_range(n, RX_WAVE_PWV6_COLUMNS, k, &a, &b);
        for (int j = 0; j < 3; j++) {
            double mean = (acc[b * 3 + j] - acc[a * 3 + j]) / (double)(b - a);
            double v = j < 2 ? 0.5 * mean : 0.5 * sqrt(127 * mean);
            pwv6[k * 3 + j] = (uint8_t)fmin(127, round_half(v));
        }
    }
}

static int alloc_detail(RXWaveforms *w, int n)
{
    w->nb_detail = n;
    w->pwv3 = malloc((size_t)n);
    w->pwv5 = malloc((size_t)n * 2);
    w->pwv7 = malloc((size_t)n * 3);
    if (!w->pwv3 || !w->pwv5 || !w->pwv7) {
        rx_waveforms_free(w);
        return RX_ERROR(ENOMEM);
    }
    return 0;
}

int rx_waveforms_compute(const float *mono, int64_t nb_samples, int rate, RXWaveforms *out)
{
    *out = (RXWaveforms) { 0 };
    if (rate <= 0 || nb_samples < 0 || (nb_samples && !mono))
        return RX_ERROR(EINVAL);
    int64_t scaled;
    if (ckd_mul(&scaled, nb_samples, (int64_t)COLUMNS_PER_SECOND))
        return RX_ERROR(EINVAL);

    Columns c;
    int ret = columns_compute(mono, nb_samples, rate, &c);
    if (ret < 0)
        return ret;
    int64_t n = c.n;

    double *env = malloc(n * 3 * sizeof(*env));
    double *whiteness = malloc(n * sizeof(*whiteness));
    uint8_t *height = malloc(n);
    double *cs = malloc((n + 1) * sizeof(*cs));
    int64_t *cc = malloc((n + 1) * sizeof(*cc));
    double *acc = malloc((n + 1) * 3 * sizeof(*acc));
    uint8_t *dj7 = malloc(n * 3);
    if (!env || !whiteness || !height || !cs || !cc || !acc || !dj7) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    ret = alloc_detail(out, (int)n);
    if (ret < 0)
        goto fail;
    out->rate = rate;

    /* PWV3: height is the squared peak relative to the track peak, whiteness
     * how little of the peak survives a 150 Hz low-pass. Silence is white. */
    double track_peak = 0.0;
    for (int64_t i = 0; i < n; i++)
        track_peak = fmax(track_peak, c.peak[i]);
    for (int64_t i = 0; i < n; i++) {
        height[i] = 0;
        if (track_peak > 0) {
            double r = c.peak[i] / track_peak;
            height[i] = (uint8_t)fmin(31, floor(31.5 * (r * r)));
        }
        if (c.peak[i] > 0) {
            double ratio = RX_CLAMP(c.white[i] / c.peak[i], 0.0, 1.0);
            whiteness[i] = 7 - fmin(7, floor(8 * ratio));
        } else {
            whiteness[i] = 7;
        }
        out->pwv3[i] = (uint8_t)((uint8_t)whiteness[i] << 5 | height[i]);
    }

    /* PWV7: band peak envelopes with a per-column release (peak hold with
     * exponential release), scaled so the loudest column of each band hits
     * a target level. The gains are the PWVC tag. */
    for (int j = 0; j < 3; j++) {
        double r = RELEASE[j], prev = 0.0, emax = 0.0;
        for (int64_t i = 0; i < n; i++) {
            double v = c.band[i * 3 + j];
            prev = v > prev * r ? v : prev * r;
            env[i * 3 + j] = prev;
            emax = fmax(emax, prev);
        }
        double g = GAIN_FLOOR[j];
        if (emax > 0)
            g = RX_CLAMP(round_half(GAIN_TARGET[j] / emax), GAIN_FLOOR[j], GAIN_CAP[j]);
        out->gains[j] = (uint16_t)g;
    }
    /* dj writes u = min(127, BAND_SCALE * gain * envelope) as the level.
     * The gain brings each band's loudest column to GAIN_TARGET * BAND_SCALE
     * (97.5, 120.75, 69) unless it hits its floor or cap. rxbox takes
     * x = min(1, u / that), so every band's loudest column is 1 as in
     * rekordbox (whose PWV7 reaches 126..127 in all three bands), and writes
     * 127 * x^1.5 for the low and mid bands and 127 * x^2.5 for the high band
     * (x * sqrt(x) and x * x * sqrt(x): exact operations, so Python mirrors
     * them byte for byte). The envelopes and gains are dj's, so PWVC is too.
     *
     * Fitted per band (exponents from 1, 1.25, 1.5, 2, 2.5, 3) to rekordbox's
     * own PWV7 bytes. Mean, p50, p95, max per band, low | mid | high:
     *
     *   rekordbox, its two 6.6.1 demo tracks:   48.8 53 94 126 | 24.8 19 71 126 |  6.2  1 34 127
     *   dj's rule, 10 tracks:                    47.4 49 78 100 | 41.5 41 73 121 | 17.2 15 40  69
     *   this rule, the same tracks:              46.3 46 91 127 | 27.6 25 60 127 |  7.5  3 33 127
     *
     * The rekordbox bytes are the only rekordbox-written .2EX with waveform
     * bytes at hand (rekordcrate's data/complete_export, pyrekordbox's
     * .testdata; the ~2100 seabass .2EX carry only PWVC, whose gains, median
     * 80/108/109, agree with ours). The 10 tracks are lines 1, 5, 17, 20,
     * 23, 27, 33, 42, 51 and 55 of build-check/acceptance/list.txt (house,
     * drum & bass, J-pop, jazz; FLAC, Opus, MP3; 302k columns), mixed to
     * mono as the app does (the mean of the channels). dj's high band sat at
     * a fifth of its peak almost everywhere (median 15) where rekordbox's is
     * near zero except on transients (median 1), and its mid was half as
     * hot again as rekordbox's. With two reference tracks this is a
     * calibration, not rekordbox's algorithm. */
    for (int j = 0; j < 3; j++) {
        double scale = BAND_SCALE[j] * out->gains[j];
        double full = GAIN_TARGET[j] * BAND_SCALE[j];
        for (int64_t i = 0; i < n; i++) {
            double u = fmin(127, scale * env[i * 3 + j]);
            dj7[i * 3 + j] = (uint8_t)round_half(u);
            double x = fmin(1.0, u / full), sx = sqrt(x);
            double v = j < 2 ? x * sx : x * x * sx;
            out->pwv7[i * 3 + j] = (uint8_t)round_half(127 * v);
        }
    }

    /* PWV5: each band's share of the column's strongest band (square root so
     * faint bands still tint), with the PWV3 height; big-endian
     * rrrgggbbbhhhhh00. */
    for (int64_t i = 0; i < n; i++) {
        const double *b = &c.band[i * 3];
        double bmax = fmax(fmax(b[0], b[1]), b[2]);
        unsigned level[3] = { 0 };
        if (bmax > 0)
            for (int j = 0; j < 3; j++)
                level[j] = (unsigned)fmin(7, round_half(7 * sqrt(b[j] / bmax)));
        unsigned v = level[0] << 13 | level[1] << 10 | level[2] << 7 | (unsigned)height[i] << 2;
        out->pwv5[i * 2 + 0] = (uint8_t)(v >> 8);
        out->pwv5[i * 2 + 1] = (uint8_t)v;
    }

    /* Previews: RMS over column groups. */
    cs[0] = 0.0;
    cc[0] = 0;
    for (int64_t i = 0; i < n; i++) {
        cs[i + 1] = cs[i] + c.sumsq[i];
        cc[i + 1] = cc[i] + c.count[i];
    }

    /* PWAV: height from a 400-way RMS normalised to 24, whiteness the mean
     * whiteness over a 200-way split (each covering two columns). */
    double r400[RX_WAVE_PWAV_COLUMNS], r400max;
    rms_over(&c, cs, cc, RX_WAVE_PWAV_COLUMNS, r400, &r400max);
    acc[0] = 0.0;
    for (int64_t i = 0; i < n; i++)
        acc[i + 1] = acc[i] + whiteness[i];
    for (int k = 0; k < RX_WAVE_PWAV_COLUMNS; k++) {
        int64_t a, b;
        split_range(n, RX_WAVE_PWAV_COLUMNS / 2, k / 2, &a, &b);
        double w200 = round_half((acc[b] - acc[a]) / (double)(b - a));
        double h = r400max > 0 ? fmin(31, round_half(24 * r400[k] / r400max)) : 0;
        /* Firmware fix: never below 2 (see the top of the file). */
        h = fmax(h, PWAV_MIN_HEIGHT);
        out->pwav[k] = (uint8_t)((uint8_t)fmin(7, w200) << 5 | (uint8_t)h);
    }

    /* PWV2: 100-way RMS, square-root compressed to 0..15. */
    double r100[RX_WAVE_PWV2_COLUMNS], r100max;
    rms_over(&c, cs, cc, RX_WAVE_PWV2_COLUMNS, r100, &r100max);
    for (int k = 0; k < RX_WAVE_PWV2_COLUMNS; k++) {
        double h = r100max > 0 ? fmin(15, round_half(15 * sqrt(r100[k] / r100max))) : 0;
        /* Firmware fix: never below 1 (see the top of the file). */
        out->pwv2[k] = (uint8_t)fmax(h, PWV2_MIN_HEIGHT);
    }

    /* PWV6 from rxbox's PWV7: the range means over a 1200-way split, halved
     * for the low and mid bands, and 0.5 * sqrt(127 * mean) for the high
     * band. The square root keeps a bright track's highs from reaching past
     * its bass: the overview draws each band's reach over the last (blue,
     * amber, then cream), so the fraction of columns where high >= low is
     * the fraction drawn all cream, and where mid >= low all amber.
     *
     * Fitted (multipliers 0.3..3, exponents 0.5, 0.75, 1 for mid and high)
     * to rekordbox's two demo tracks, where high >= low in 28 % and 32 % of
     * columns and mid >= low in 32 % and 25 %, with the band means below.
     * On the 10 tracks above, high >= low / mid >= low:
     *
     *   track (list line)     1    5    17   20   23   27   33   42   51   55
     *   dj's rule           .44  .68  .69  .97  .76  .78  .12  .86  .79  .60
     *                       .40  .57  .43  .81  .85  .79  .55  .49  .62  .75
     *   this rule           .04  .21  .10  .33  .11  .20  .10  .30  .29  .19
     *                       .06  .23  .11  .17  .36  .39  .28  .13  .28  .37
     *
     * Line 20 is a loud drum & bass master: with dj's levels its overview
     * was cream in 97 % of the columns, now a blue body with amber and the
     * cream at its peaks. Band means, p50, p95, max:
     *
     *   rekordbox, the two demo tracks:   22.1 26 37 43 | 15.3 15 29 39 | 13.6 13 28 38
     *   dj's rule, the 10 tracks:         25.3 27 39 52 | 28.3 30 46 71 | 31.7 33 60 82
     *   this rule, the same tracks:       21.8 23 39 59 | 13.2 13 25 48 | 12.3 13 25 34 */
    uint8_t dj6[RX_WAVE_PWV6_COLUMNS * 3];
    pwv6_rxbox(out->pwv7, n, acc, out->pwv6);
    pwv6_dj(dj7, n, acc, dj6);

    /* PWV4: bytes 3..5 are the PWV6 bands rescaled; byte 0 follows the
     * strongest of them, byte 1 falls as byte 0 rises, byte 2 is their
     * magnitude. All fitted by baken against dj's PWV6 levels, so it is
     * built from those (rbxport measured rekordbox's PWV4 colour bytes
     * about three times hotter than its PWV6, reaching 127 on every track,
     * which this keeps). Columns with no signal are zero. */
    for (int k = 0; k < RX_WAVE_PWV4_COLUMNS; k++) {
        uint8_t *p = &out->pwv4[k * 6];
        double b345[3], mx = 0.0, sq = 0.0;
        for (int j = 0; j < 3; j++) {
            b345[j] = fmin(127, round_half(PWV4_SCALE[j] * dj6[k * 3 + j]));
            mx = fmax(mx, b345[j]);
            sq += b345[j] * b345[j];
            p[3 + j] = (uint8_t)b345[j];
        }
        p[0] = p[1] = p[2] = 0;
        if (mx > 0) {
            double b0 = fmin(127, round_half(0.88 * mx + 38));
            p[0] = (uint8_t)b0;
            p[1] = (uint8_t)RX_CLAMP(round_half(203 - 0.49 * b0), 0.0, 255.0);
            p[2] = (uint8_t)fmin(127, round_half(0.91 * sqrt(sq)));
        }
    }
    ret = 0;

fail:
    if (ret < 0)
        rx_waveforms_free(out);
    free(env);
    free(whiteness);
    free(height);
    free(cs);
    free(cc);
    free(acc);
    free(dj7);
    columns_free(&c);
    return ret;
}

void rx_waveforms_free(RXWaveforms *w)
{
    if (!w)
        return;
    free(w->pwv3);
    free(w->pwv5);
    free(w->pwv7);
    *w = (RXWaveforms) { 0 };
}

/* ---------------------------------------------------------------- packing */

/* The packed form, one blob per track in the library database and the
 * content of a .rxw file; all integers little-endian:
 *
 *   0   4  "RXWV"
 *   4   4  version (3)
 *   8   4  nb_detail
 *  12   4  rate
 *  16   6  gains, 3 x u16
 *  22   2  zero
 *  24      PWAV (400), PWV2 (100), PWV4 (1200 x 6), PWV6 (1200 x 3),
 *          PWV3 (n), PWV5 (2n), PWV7 (3n): the tag payloads as they go on
 *          the stick.
 *
 * It is a cache of what analysis computes, so a version change just makes
 * unpack fail with -EPROTO and the caller recompute; damaged data fails
 * with -EINVAL. Unpack checks the size exactly against the header before
 * reading anything.
 *
 * Version 1 held dj's PWV6/PWV7 levels, version 2 a first recalibration
 * (dj's levels squared), version 3 the per-band one (see "PWV7" and
 * "PWV6" above); older blobs must not be used. */
#define RXW_MAGIC "RXWV"
#define RXW_VERSION 3
#define RXW_HEADER 24
/* 24 hours of columns: anything above is a damaged file, not a track. */
#define RXW_MAX_DETAIL (COLUMNS_PER_SECOND * 3600 * 24)

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t rxw_size(int n)
{
    return RXW_HEADER + RX_WAVE_PWAV_COLUMNS + RX_WAVE_PWV2_COLUMNS +
           RX_WAVE_PWV4_COLUMNS * 6 + RX_WAVE_PWV6_COLUMNS * 3 + (size_t)n * 6;
}

int rx_waveforms_pack(const RXWaveforms *w, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    if (!w || w->nb_detail <= 0 || w->nb_detail > RXW_MAX_DETAIL ||
        !w->pwv3 || !w->pwv5 || !w->pwv7)
        return RX_ERROR(EINVAL);
    size_t n = (size_t)w->nb_detail, len = rxw_size(w->nb_detail);
    uint8_t *buf = malloc(len);
    if (!buf)
        return RX_ERROR(ENOMEM);

    memcpy(buf, RXW_MAGIC, 4);
    put_le32(buf + 4, RXW_VERSION);
    put_le32(buf + 8, (uint32_t)w->nb_detail);
    put_le32(buf + 12, (uint32_t)RX_MAX(w->rate, 0));
    for (int j = 0; j < 3; j++) {
        buf[16 + 2 * j] = (uint8_t)w->gains[j];
        buf[17 + 2 * j] = (uint8_t)(w->gains[j] >> 8);
    }
    buf[22] = buf[23] = 0;
    uint8_t *p = buf + RXW_HEADER;
    memcpy(p, w->pwav, sizeof(w->pwav));
    p += sizeof(w->pwav);
    memcpy(p, w->pwv2, sizeof(w->pwv2));
    p += sizeof(w->pwv2);
    memcpy(p, w->pwv4, sizeof(w->pwv4));
    p += sizeof(w->pwv4);
    memcpy(p, w->pwv6, sizeof(w->pwv6));
    p += sizeof(w->pwv6);
    memcpy(p, w->pwv3, n);
    p += n;
    memcpy(p, w->pwv5, n * 2);
    p += n * 2;
    memcpy(p, w->pwv7, n * 3);

    *data = buf;
    *size = len;
    return 0;
}

int rx_waveforms_unpack(const uint8_t *data, size_t size, RXWaveforms *w)
{
    *w = (RXWaveforms) { 0 };
    if (!data || size < RXW_HEADER || memcmp(data, RXW_MAGIC, 4))
        return RX_ERROR(EINVAL);
    /* Another format version (1, 2: older three-band levels): not damaged,
     * but stale, so the caller recomputes */
    if (get_le32(data + 4) != RXW_VERSION)
        return RX_ERROR(EPROTO);
    uint32_t n = get_le32(data + 8), rate = get_le32(data + 12);
    if (n == 0 || n > RXW_MAX_DETAIL || rate > INT32_MAX || size != rxw_size((int)n))
        return RX_ERROR(EINVAL);
    int ret = alloc_detail(w, (int)n);
    if (ret < 0)
        return ret;

    w->rate = (int)rate;
    for (int j = 0; j < 3; j++)
        w->gains[j] = (uint16_t)(data[16 + 2 * j] | data[17 + 2 * j] << 8);
    const uint8_t *p = data + RXW_HEADER;
    memcpy(w->pwav, p, sizeof(w->pwav));
    p += sizeof(w->pwav);
    memcpy(w->pwv2, p, sizeof(w->pwv2));
    p += sizeof(w->pwv2);
    memcpy(w->pwv4, p, sizeof(w->pwv4));
    p += sizeof(w->pwv4);
    memcpy(w->pwv6, p, sizeof(w->pwv6));
    p += sizeof(w->pwv6);
    memcpy(w->pwv3, p, n);
    p += n;
    memcpy(w->pwv5, p, (size_t)n * 2);
    p += (size_t)n * 2;
    memcpy(w->pwv7, p, (size_t)n * 3);
    return 0;
}

int rx_waveforms_save(const RXWaveforms *w, const char *path)
{
    if (!path)
        return RX_ERROR(EINVAL);
    uint8_t *buf;
    size_t size;
    int ret = rx_waveforms_pack(w, &buf, &size);
    if (ret < 0)
        return ret;
    ret = rx_file_write_atomic(path, buf, size);
    free(buf);
    return ret;
}

int rx_waveforms_load(const char *path, RXWaveforms *w)
{
    *w = (RXWaveforms) { 0 };
    uint8_t *buf;
    size_t size;
    int ret = rx_file_read(path, &buf, &size);
    if (ret < 0)
        return ret;
    ret = rx_waveforms_unpack(buf, size, w);
    free(buf);
    return ret;
}
