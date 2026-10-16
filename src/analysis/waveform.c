/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The ANLZ waveform tags (PWAV, PWV2, PWV3, PWV4, PWV5, PWV6, PWV7 and the
 * PWVC gains) from the mono mix at the file's native rate, and their packed
 * form (the library's blob and the .rxw file) that keeps them between runs.
 *
 * PWAV, PWV2, PWV3, PWV4, PWV5 and PWVC are a port of dj's
 * analysis/waveform.py that reproduces its output byte for byte
 * (tests/analysis/test_waveform.c checks them against dj's output on
 * synthetic signals). The rules and constants come from there; dj took them
 * from the baken project (MIT licence, https://github.com/M-Igashi/baken,
 * crates/baken-export/src/anlz/generate/waveform.rs), which fitted them on
 * 1070 rekordbox-analysed tracks. The monochrome height and whiteness rules
 * (PWV3) reproduce rekordbox byte for byte on 99.5 % of columns; the colour
 * rules are fitted approximations. docs/anlz.md has the tag layouts.
 *
 * The three-band levels (PWV7, PWV6) are rbxport's analyser instead
 * (rbxport crates/rbl-analysis/src/waveform.rs, and crates/rbl-anlz/src/
 * encode.rs for the bytes; GPL, so its rules are taken, not its code):
 * one-pole crossovers at 200 Hz and 2 kHz and each band's peak over the
 * column for the detail, and for the overview energy measures normalised
 * per band to the track's loudness. See "PWV7" and "PWV6" below. They
 * replace two rounds of fits to rekordbox's own bytes, whose high band is
 * thin (its mean 0.13 of the low band's in PWV7): drawn, that is a blue
 * waveform with a thin cream core, and the user prefers rbxport's picture,
 * where the blue lows, amber mids and white highs all show on every kind of
 * track.
 *
 * Byte agreement with numpy depends on doing every operation in the same
 * order in double precision, so the expressions below keep dj's order
 * ((24 * r) / max, not 24 * (r / max)), round with dj's floor(v + 0.5), and
 * the filters are scipy's lfilter (direct form II transposed) step by step.
 * The build is ISO C23, where GCC does not contract a * b + c into a fused
 * multiply-add, which would change the last bit of some products.
 *
 * Unlike dj and rbxport, which run each filter over the whole track in
 * float64 arrays (several hundred MB for a long hi-res file) or make a
 * second pass for the overview, the samples are streamed once through all
 * the filters and reduced to per-column and per-overview-column values on
 * the way: the arithmetic per sample is the same, so the results are too.
 *
 * Deliberate differences from dj:
 *
 * - Two firmware fixes measured by rbxport (docs/OVERVIEW.md section 3,
 *   item 2) and confirmed on ~2100 rekordbox files: PWAV heights are never
 *   below 2 and PWV2 heights never below 1. rekordbox never writes lower
 *   values, and Nexus firmware rejects a preview that has a zero column.
 * - PWV7 and PWV6 are rbxport's rules (above). PWAV, PWV2, PWV3, PWV4, PWV5
 *   and PWVC stay byte-identical to dj (PWV4 is still derived from dj's
 *   PWV6 levels, which baken fitted it against).
 *
 * And from rbxport: everything is in double where its detail columns are
 * float, and the detail columns follow rekordbox's grid (column k starts at
 * round(k * rate / 150)) where rbxport cuts fixed floor(rate / 150) sample
 * chunks. The two agree at 44.1, 48, 88.2 and 96 kHz. */

#include "cyanbox/analysis.h"

#include <math.h>
#include <stdckdint.h>

#include "cyanbox/common.h"

/* Every detail column is 1/150 s (docs/anlz.md, "half-frame"). */
#define COLUMNS_PER_SECOND 150

/* Filter corners in Hz (baken's fit). Whiteness is how much of a column's
 * peak survives a 150 Hz low-pass; dj's three bands (PWV5, and the levels
 * PWV4 and PWVC come from) are a 300 Hz low-pass, a 250..1500 Hz band
 * (high-pass then low-pass) and a 2500 Hz high-pass. */
#define WHITENESS_LOWPASS_HZ 150.0
#define LOW_HZ      300.0
#define MID_LOW_HZ  250.0
#define MID_HIGH_HZ 1500.0
#define HIGH_HZ     2500.0

/* dj's per-column release of the band peak envelopes, low/mid/high. */
static const double RELEASE[3]     = { 0.97, 0.93, 0.90 };
/* dj's PWV7 gain: the level the loudest column of each band is scaled to,
 * and the floors and caps of the gain, which is what PWVC stores (rekordbox
 * files show low ~80, mid 80..200, high up to 500: docs/anlz.md 4.14). */
static const double GAIN_TARGET[3] = { 78.0, 105.0, 115.0 };
static const double GAIN_FLOOR[3]  = { 80, 80, 95 };
static const double GAIN_CAP[3]    = { 267, 300, 500 };
static const double BAND_SCALE[3]  = { 1.25, 1.15, 0.6 };
/* dj's PWV6 = range mean of dj's PWV7 times these (baken's fit of
 * rekordbox's PWV6 against rekordbox's PWV7), and PWV4 bytes 3..5 = that
 * PWV6 times these. cyanbox writes rbxport's PWV6 and keeps dj's for PWV4. */
static const double PWV6_SCALE[3]  = { 0.56, 0.71, 2.0 };
static const double PWV4_SCALE[3]  = { 2.4, 1.6, 1.1 };

/* rbxport's three bands (crates/rbl-analysis/src/waveform.rs): crossovers
 * at 200 Hz and 2 kHz. */
#define SPLIT_LOW_HZ  200.0
#define SPLIT_HIGH_HZ 2000.0
/* rbxport's overview: every band's mean over the track is this many times
 * the track's mean RMS per 1/150 s column, and the low band is its mean
 * square to this power ("measured against nine rekordbox references"). */
#define PWV6_LEVEL     92.0
#define PWV6_LOW_POWER 0.45
/* rbxport takes a band whose mean over the track is below this as silent. */
#define PWV6_SILENT    1e-6

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

/* rbxport's one-pole low-pass, y = x * (1 - c) + y * c with
 * c = exp(-2 pi fc / rate). It falls at 6 dB an octave, so its bands
 * overlap widely: a 1 kHz tone shows about half as high in the high band as
 * in the mid (tests/analysis/test_waveform.c works the levels out). */
typedef struct OnePole {
    double c, g, y;
} OnePole;

static void onepole_init(OnePole *f, double fc, int rate)
{
    double c = exp(-2 * M_PI * fc / rate);
    *f = (OnePole) { .c = c, .g = 1 - c };
}

static inline double onepole_step(OnePole *f, double x)
{
    f->y = x * f->g + f->y * f->c;
    return f->y;
}

/* dj's _split: `parts` consecutive ranges [a, b) of n items, each at least
 * one item long. */
static void split_range(int64_t n, int parts, int k, int64_t *a, int64_t *b)
{
    *a = CB_MIN((int64_t)k * n / parts, n - 1);
    *b = CB_CLAMP((int64_t)(k + 1) * n / parts, *a + 1, n);
}

/* Per-column reductions of the samples, and the sums of the overview
 * columns. */
typedef struct Columns {
    int64_t n;
    double *peak, *white, *band;     /* band: n * 3, low/mid/high, dj's filters */
    double *split;                   /* n * 3: rbxport's bands, low/mid/high */
    double *sumsq;
    int64_t *count;
    double *ov;                      /* CB_WAVE_PWV6_COLUMNS * 3: low energy, |mid|, |high| */
    int64_t *ov_count;
} Columns;

static void columns_free(Columns *c)
{
    free(c->peak);
    free(c->white);
    free(c->band);
    free(c->split);
    free(c->sumsq);
    free(c->count);
    free(c->ov);
    free(c->ov_count);
}

/* The sample where overview column b ends: rbxport puts sample i in column
 * ((i + 1) * 1200 - 1) / ns, which is [b * ns / 1200, (b + 1) * ns / 1200)
 * in integers. Shorter than 1200 samples, some columns are empty. */
static inline int64_t ov_end(int b, int64_t ns)
{
    return (int64_t)(b + 1) * ns / CB_WAVE_PWV6_COLUMNS;
}

static int columns_compute(const float *x, int64_t ns, int rate, Columns *c)
{
    /* n = max(1, ceil(samples * 150 / rate)); column k starts at
     * round(k * rate / 150) (docs/anlz.md, "Waveform-detail column grid").
     * For MP3, `ns` already counts every MPEG frame from sample 0, so this
     * is rekordbox's frames * 1152 * 150 / rate. */
    int64_t n = (int64_t)ceil((double)(ns * COLUMNS_PER_SECOND) / rate);
    n = CB_MAX(n, 1);
    if (n > INT32_MAX / 6)
        return CB_ERROR(EINVAL);
    *c = (Columns) { .n = n };
    c->peak     = calloc(n, sizeof(*c->peak));
    c->white    = calloc(n, sizeof(*c->white));
    c->band     = calloc(n * 3, sizeof(*c->band));
    c->split    = calloc(n * 3, sizeof(*c->split));
    c->sumsq    = calloc(n, sizeof(*c->sumsq));
    c->count    = calloc(n, sizeof(*c->count));
    c->ov       = calloc(CB_WAVE_PWV6_COLUMNS * 3, sizeof(*c->ov));
    c->ov_count = calloc(CB_WAVE_PWV6_COLUMNS, sizeof(*c->ov_count));
    if (!c->peak || !c->white || !c->band || !c->split || !c->sumsq || !c->count || !c->ov ||
        !c->ov_count) {
        columns_free(c);
        return CB_ERROR(ENOMEM);
    }

    Biquad white, low, mid_hp, mid_lp, high;
    biquad_init(&white,  false, WHITENESS_LOWPASS_HZ, rate);
    biquad_init(&low,    false, LOW_HZ, rate);
    biquad_init(&mid_hp, true,  MID_LOW_HZ, rate);
    biquad_init(&mid_lp, false, MID_HIGH_HZ, rate);
    biquad_init(&high,   true,  HIGH_HZ, rate);

    /* rbxport's bands: the 200 Hz low-pass is the low band of both the
     * detail and the overview; the detail's mid and high are what lies
     * between it and the 2 kHz low-pass, and above that. The overview's mid
     * and high are second-order Butterworth sections instead (the same RBJ
     * design as dj's, Q = 1/sqrt(2)), "to keep bass out of the upper
     * overview bands", with rbxport's corners kept below 0.45 of the rate. */
    OnePole lp_low, lp_high;
    onepole_init(&lp_low,  SPLIT_LOW_HZ, rate);
    onepole_init(&lp_high, SPLIT_HIGH_HZ, rate);
    Biquad ov_hp, ov_lp, ov_high;
    biquad_init(&ov_hp,   true,  fmin(SPLIT_LOW_HZ, 0.45 * rate), rate);
    biquad_init(&ov_lp,   false, fmin(SPLIT_HIGH_HZ, 0.45 * rate), rate);
    biquad_init(&ov_high, true,  fmin(SPLIT_HIGH_HZ, 0.45 * rate), rate);
    int b = 0;
    int64_t b_end = ov_end(0, ns);

    /* The sum of squares per column is a difference of the running sum,
     * as dj's _colsum takes it from np.cumsum. */
    double cum = 0.0;
    int64_t start = 0;
    for (int64_t k = 0; k < n; k++) {
        int64_t end = (int64_t)round_half((double)((k + 1) * rate) / COLUMNS_PER_SECOND);
        end = CB_MIN(end, ns);
        double cum0 = cum;
        double pk = 0.0, wh = 0.0, lo = 0.0, md = 0.0, hi = 0.0;
        double s_lo = 0.0, s_md = 0.0, s_hi = 0.0;
        for (int64_t i = start; i < end; i++) {
            double v = x[i];
            pk = fmax(pk, fabs(v));
            wh = fmax(wh, fabs(biquad_step(&white, v)));
            lo = fmax(lo, fabs(biquad_step(&low, v)));
            md = fmax(md, fabs(biquad_step(&mid_lp, biquad_step(&mid_hp, v))));
            hi = fmax(hi, fabs(biquad_step(&high, v)));
            cum += v * v;

            double l = onepole_step(&lp_low, v), below = onepole_step(&lp_high, v);
            s_lo = fmax(s_lo, fabs(l));
            s_md = fmax(s_md, fabs(below - l));
            s_hi = fmax(s_hi, fabs(v - below));

            while (i >= b_end)
                b_end = ov_end(++b, ns);
            double om = biquad_step(&ov_lp, biquad_step(&ov_hp, v));
            double oh = biquad_step(&ov_high, v);
            c->ov[b * 3 + 0] += l * l;
            c->ov[b * 3 + 1] += fabs(om);
            c->ov[b * 3 + 2] += fabs(oh);
            c->ov_count[b]++;
        }
        c->peak[k] = pk;
        c->white[k] = wh;
        c->band[k * 3 + 0] = lo;
        c->band[k * 3 + 1] = md;
        c->band[k * 3 + 2] = hi;
        c->split[k * 3 + 0] = s_lo;
        c->split[k * 3 + 1] = s_md;
        c->split[k * 3 + 2] = s_hi;
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
 * band. `acc` holds (n + 1) * 3 running sums. cyanbox uses it only for dj's
 * PWV6 levels that PWV4 is built from. */
static void pwv6_dj(const uint8_t *pwv7, int64_t n, double *acc, uint8_t *pwv6)
{
    for (int j = 0; j < 3; j++)
        acc[j] = 0.0;
    for (int64_t i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            acc[(i + 1) * 3 + j] = acc[i * 3 + j] + pwv7[i * 3 + j];
    for (int k = 0; k < CB_WAVE_PWV6_COLUMNS; k++) {
        int64_t a, b;
        split_range(n, CB_WAVE_PWV6_COLUMNS, k, &a, &b);
        for (int j = 0; j < 3; j++) {
            double mean = (acc[b * 3 + j] - acc[a * 3 + j]) / (double)(b - a);
            pwv6[k * 3 + j] = (uint8_t)fmin(127, round_half(PWV6_SCALE[j] * mean));
        }
    }
}

/* rbxport's PWV6 from the overview column sums (see its use). Overwrites
 * the sums. */
static void pwv6_rbxport(Columns *c, uint8_t *pwv6)
{
    /* The target: PWV6_LEVEL times the mean of the column RMS levels,
     * over the columns that hold samples. */
    double level = 0.0;
    int64_t frames = 0;
    for (int64_t k = 0; k < c->n; k++) {
        if (c->count[k] <= 0)
            continue;
        level += sqrt(c->sumsq[k] / (double)c->count[k]);
        frames++;
    }
    double target = frames ? level / (double)frames * PWV6_LEVEL : 0.0;

    double *v = c->ov, mean[3] = { 0.0, 0.0, 0.0 };
    for (int k = 0; k < CB_WAVE_PWV6_COLUMNS; k++) {
        double cnt = (double)CB_MAX(c->ov_count[k], 1);
        v[k * 3 + 0] = pow(v[k * 3 + 0] / cnt, PWV6_LOW_POWER);
        v[k * 3 + 1] /= cnt;
        v[k * 3 + 2] /= cnt;
        for (int j = 0; j < 3; j++)
            mean[j] += v[k * 3 + j];
    }
    for (int j = 0; j < 3; j++)
        mean[j] /= CB_WAVE_PWV6_COLUMNS;

    /* A two-column trailing average ("matches the reference envelope's
     * release and half-bucket alignment"), the first column with itself. */
    double prev[3] = { v[0], v[1], v[2] };
    for (int k = 0; k < CB_WAVE_PWV6_COLUMNS; k++) {
        for (int j = 0; j < 3; j++) {
            double cur = v[k * 3 + j];
            v[k * 3 + j] = (cur + prev[j]) / 2;
            prev[j] = cur;
        }
    }

    for (int k = 0; k < CB_WAVE_PWV6_COLUMNS; k++) {
        for (int j = 0; j < 3; j++) {
            double u = mean[j] < PWV6_SILENT ? 0.0 : v[k * 3 + j] * target / mean[j];
            pwv6[k * 3 + j] = (uint8_t)CB_CLAMP(round(u), 0.0, 127.0);
        }
    }
}

static int alloc_detail(CBWaveforms *w, int n)
{
    w->nb_detail = n;
    w->pwv3 = malloc((size_t)n);
    w->pwv5 = malloc((size_t)n * 2);
    w->pwv7 = malloc((size_t)n * 3);
    if (!w->pwv3 || !w->pwv5 || !w->pwv7) {
        cb_waveforms_free(w);
        return CB_ERROR(ENOMEM);
    }
    return 0;
}

int cb_waveforms_compute(const float *mono, int64_t nb_samples, int rate, CBWaveforms *out)
{
    *out = (CBWaveforms) { 0 };
    if (rate <= 0 || nb_samples < 0 || (nb_samples && !mono))
        return CB_ERROR(EINVAL);
    int64_t scaled;
    if (ckd_mul(&scaled, nb_samples, (int64_t)COLUMNS_PER_SECOND))
        return CB_ERROR(EINVAL);

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
        ret = CB_ERROR(ENOMEM);
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
            double ratio = CB_CLAMP(c.white[i] / c.peak[i], 0.0, 1.0);
            whiteness[i] = 7 - fmin(7, floor(8 * ratio));
        } else {
            whiteness[i] = 7;
        }
        out->pwv3[i] = (uint8_t)((uint8_t)whiteness[i] << 5 | height[i]);
    }

    /* dj's PWV7, which PWVC and (through dj's PWV6) PWV4 are made from:
     * band peak envelopes with a per-column release (peak hold with
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
            g = CB_CLAMP(round_half(GAIN_TARGET[j] / emax), GAIN_FLOOR[j], GAIN_CAP[j]);
        out->gains[j] = (uint16_t)g;
    }
    /* dj writes min(127, BAND_SCALE * gain * envelope) as its level; cyanbox
     * keeps it only for PWV4 (below).
     *
     * PWVC stays dj's gains although PWV7 is no longer scaled by them, as
     * rbxport's files do: rbxport writes no PWVC of its own (crates/rbl-anlz/
     * src/encode.rs authors PWV6 and PWV7 only), and re-analysing a track
     * rekordbox analysed it keeps rekordbox's PWVC beside its own PWV7. dj's
     * gains are in rekordbox's range (the ~2100 seabass .2EX, median
     * 80/108/109; docs/anlz.md 4.14), which is all a player can be relying
     * on: no reader is known to scale PWV7 by them. */
    for (int j = 0; j < 3; j++) {
        double scale = BAND_SCALE[j] * out->gains[j];
        for (int64_t i = 0; i < n; i++)
            dj7[i * 3 + j] = (uint8_t)round_half(fmin(127, scale * env[i * 3 + j]));
    }

    /* PWV7, rbxport's detail: each band's peak over the column, as a
     * fraction of full scale, to 0..255 truncated (rbxport's column byte),
     * then halved into PWV7's seven bits (encode.rs's pwv7(): "the detail
     * uses the full seven-bit range"). So a full-scale peak is 127 and a
     * track is drawn at its own loudness, where rekordbox's PWV7 reaches
     * 126..127 in every band of rekordbox 6.6.1's two demo tracks
     * (rekordcrate's data/complete_export) and 127 on all 80 tracks rbxport
     * measured (src/canvas/waveform.ts).
     *
     * Band means over the track, PWV7 low | mid | high, format version 3
     * (fitted to rekordbox's bytes) -> this, on lines of
     * build-check/acceptance/list.txt mixed to mono as the app does. The
     * bytes agree with rbxport's own analyser run on the same samples on all
     * but 0..3 in 100k (its float arithmetic), and its PWV6 exactly:
     *
     *   1   house, -9.5 dB RMS        63.0 31.3  6.5 -> 54.2 42.1 19.1
     *   20  drum & bass, -8.2 dB      53.3 36.7 15.7 -> 53.5 59.6 50.1
     *   23  drum & bass, -10.2 dB     45.4 34.7  7.1 -> 44.4 48.3 33.0
     *   36  vocal jazz-pop, -14.8 dB  23.9 16.5  1.8 -> 22.9 27.8 13.0
     *   47  soundtrack, -26.4 dB       7.2 11.0  0.6 ->  3.1  6.7  3.5 */
    for (int64_t i = 0; i < n * 3; i++) {
        double v = fmin(c.split[i], 1.0);
        out->pwv7[i] = (uint8_t)((unsigned)(255 * v) >> 1);
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
    double r400[CB_WAVE_PWAV_COLUMNS], r400max;
    rms_over(&c, cs, cc, CB_WAVE_PWAV_COLUMNS, r400, &r400max);
    acc[0] = 0.0;
    for (int64_t i = 0; i < n; i++)
        acc[i + 1] = acc[i] + whiteness[i];
    for (int k = 0; k < CB_WAVE_PWAV_COLUMNS; k++) {
        int64_t a, b;
        split_range(n, CB_WAVE_PWAV_COLUMNS / 2, k / 2, &a, &b);
        double w200 = round_half((acc[b] - acc[a]) / (double)(b - a));
        double h = r400max > 0 ? fmin(31, round_half(24 * r400[k] / r400max)) : 0;
        /* Firmware fix: never below 2 (see the top of the file). */
        h = fmax(h, PWAV_MIN_HEIGHT);
        out->pwav[k] = (uint8_t)((uint8_t)fmin(7, w200) << 5 | (uint8_t)h);
    }

    /* PWV2: 100-way RMS, square-root compressed to 0..15. */
    double r100[CB_WAVE_PWV2_COLUMNS], r100max;
    rms_over(&c, cs, cc, CB_WAVE_PWV2_COLUMNS, r100, &r100max);
    for (int k = 0; k < CB_WAVE_PWV2_COLUMNS; k++) {
        double h = r100max > 0 ? fmin(15, round_half(15 * sqrt(r100[k] / r100max))) : 0;
        /* Firmware fix: never below 1 (see the top of the file). */
        out->pwv2[k] = (uint8_t)fmax(h, PWV2_MIN_HEIGHT);
    }

    /* PWV6, rbxport's overview, measured on the samples rather than taken
     * from PWV7: over each of the 1200 columns, the low band's mean square
     * to the power 0.45 and the mid and high bands' mean rectified values
     * (energy where the detail has peaks), a two-column trailing average,
     * then each band scaled so that its mean over the track is 92 times the
     * track's mean column RMS, rounded and clamped to 127.
     *
     * So the three bands have the same mean on every track and differ in
     * where they are loud: the overview shows the bass, the body and the
     * highs of a track wherever they are, whatever the mix's balance,
     * instead of a low band that buries the other two on a bass-led master
     * and fills the strip on a quiet one. The level follows the track's
     * loudness, so a quiet track is a small overview (line 47 above: band
     * means 3.4; a loud master's 25..35). */
    uint8_t dj6[CB_WAVE_PWV6_COLUMNS * 3];
    pwv6_rbxport(&c, out->pwv6);
    pwv6_dj(dj7, n, acc, dj6);

    /* PWV4: bytes 3..5 are the PWV6 bands rescaled; byte 0 follows the
     * strongest of them, byte 1 falls as byte 0 rises, byte 2 is their
     * magnitude. All fitted by baken against dj's PWV6 levels, so it is
     * built from those (rbxport measured rekordbox's PWV4 colour bytes
     * about three times hotter than its PWV6, reaching 127 on every track,
     * which this keeps). Columns with no signal are zero. */
    for (int k = 0; k < CB_WAVE_PWV4_COLUMNS; k++) {
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
            p[1] = (uint8_t)CB_CLAMP(round_half(203 - 0.49 * b0), 0.0, 255.0);
            p[2] = (uint8_t)fmin(127, round_half(0.91 * sqrt(sq)));
        }
    }
    ret = 0;

fail:
    if (ret < 0)
        cb_waveforms_free(out);
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

void cb_waveforms_free(CBWaveforms *w)
{
    if (!w)
        return;
    free(w->pwv3);
    free(w->pwv5);
    free(w->pwv7);
    *w = (CBWaveforms) { 0 };
}

/* ---------------------------------------------------------------- packing */

/* The packed form, one blob per track in the library database and the
 * content of a .rxw file; all integers little-endian:
 *
 *   0   4  "RXWV"
 *   4   4  version (4)
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
 * to rekordbox's bytes (dj's levels squared), version 3 a per-band one,
 * version 4 rbxport's rules (see "PWV7" and "PWV6" above); older blobs
 * must not be used. The other tags have not changed since version 1, but
 * a blob is recomputed whole: it is one analysis stage. */
/* From when the project was rxbox; kept, so stored blobs read. */
#define CBW_MAGIC "RXWV"
#define CBW_VERSION 4
#define CBW_HEADER 24
/* 24 hours of columns: anything above is a damaged file, not a track. */
#define CBW_MAX_DETAIL (COLUMNS_PER_SECOND * 3600 * 24)

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
    return CBW_HEADER + CB_WAVE_PWAV_COLUMNS + CB_WAVE_PWV2_COLUMNS +
           CB_WAVE_PWV4_COLUMNS * 6 + CB_WAVE_PWV6_COLUMNS * 3 + (size_t)n * 6;
}

static_assert(CB_WAVE_PACKED_PREVIEW == CBW_HEADER + CB_WAVE_PWAV_COLUMNS + CB_WAVE_PWV2_COLUMNS +
                                        CB_WAVE_PWV4_COLUMNS * 6 + CB_WAVE_PWV6_COLUMNS * 3,
              "the previews end where the detail starts");

int cb_waveforms_pack(const CBWaveforms *w, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    if (!w || w->nb_detail <= 0 || w->nb_detail > CBW_MAX_DETAIL ||
        !w->pwv3 || !w->pwv5 || !w->pwv7)
        return CB_ERROR(EINVAL);
    size_t n = (size_t)w->nb_detail, len = rxw_size(w->nb_detail);
    uint8_t *buf = malloc(len);
    if (!buf)
        return CB_ERROR(ENOMEM);

    memcpy(buf, CBW_MAGIC, 4);
    put_le32(buf + 4, CBW_VERSION);
    put_le32(buf + 8, (uint32_t)w->nb_detail);
    put_le32(buf + 12, (uint32_t)CB_MAX(w->rate, 0));
    for (int j = 0; j < 3; j++) {
        buf[16 + 2 * j] = (uint8_t)w->gains[j];
        buf[17 + 2 * j] = (uint8_t)(w->gains[j] >> 8);
    }
    buf[22] = buf[23] = 0;
    uint8_t *p = buf + CBW_HEADER;
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

/* The header's rate and gains and the previews, from a blob checked. */
static void unpack_previews(const uint8_t *data, CBWaveforms *w)
{
    w->rate = (int)get_le32(data + 12);
    for (int j = 0; j < 3; j++)
        w->gains[j] = (uint16_t)(data[16 + 2 * j] | data[17 + 2 * j] << 8);
    const uint8_t *p = data + CBW_HEADER;
    memcpy(w->pwav, p, sizeof(w->pwav));
    p += sizeof(w->pwav);
    memcpy(w->pwv2, p, sizeof(w->pwv2));
    p += sizeof(w->pwv2);
    memcpy(w->pwv4, p, sizeof(w->pwv4));
    p += sizeof(w->pwv4);
    memcpy(w->pwv6, p, sizeof(w->pwv6));
}

int cb_waveforms_unpack_previews(const uint8_t *data, size_t size, CBWaveforms *w)
{
    *w = (CBWaveforms) { 0 };
    if (!data || size < CBW_HEADER || memcmp(data, CBW_MAGIC, 4))
        return CB_ERROR(EINVAL);
    if (get_le32(data + 4) != CBW_VERSION)
        return CB_ERROR(EPROTO);
    uint32_t n = get_le32(data + 8), rate = get_le32(data + 12);
    if (n == 0 || n > CBW_MAX_DETAIL || rate > INT32_MAX || size != CB_WAVE_PACKED_PREVIEW)
        return CB_ERROR(EINVAL);
    unpack_previews(data, w);
    return 0;
}

int cb_waveforms_unpack(const uint8_t *data, size_t size, CBWaveforms *w)
{
    *w = (CBWaveforms) { 0 };
    if (!data || size < CBW_HEADER || memcmp(data, CBW_MAGIC, 4))
        return CB_ERROR(EINVAL);
    /* Another format version (1 to 3: older three-band levels): not
     * damaged, but stale, so the caller recomputes */
    if (get_le32(data + 4) != CBW_VERSION)
        return CB_ERROR(EPROTO);
    uint32_t n = get_le32(data + 8), rate = get_le32(data + 12);
    if (n == 0 || n > CBW_MAX_DETAIL || rate > INT32_MAX || size != rxw_size((int)n))
        return CB_ERROR(EINVAL);
    int ret = alloc_detail(w, (int)n);
    if (ret < 0)
        return ret;
    unpack_previews(data, w);
    const uint8_t *p = data + CB_WAVE_PACKED_PREVIEW;
    memcpy(w->pwv3, p, n);
    p += n;
    memcpy(w->pwv5, p, (size_t)n * 2);
    p += (size_t)n * 2;
    memcpy(w->pwv7, p, (size_t)n * 3);
    return 0;
}

int cb_waveforms_save(const CBWaveforms *w, const char *path)
{
    if (!path)
        return CB_ERROR(EINVAL);
    uint8_t *buf;
    size_t size;
    int ret = cb_waveforms_pack(w, &buf, &size);
    if (ret < 0)
        return ret;
    ret = cb_file_write_atomic(path, buf, size);
    free(buf);
    return ret;
}

int cb_waveforms_load(const char *path, CBWaveforms *w)
{
    *w = (CBWaveforms) { 0 };
    uint8_t *buf;
    size_t size;
    int ret = cb_file_read(path, &buf, &size);
    if (ret < 0)
        return ret;
    ret = cb_waveforms_unpack(buf, size, w);
    free(buf);
    return ret;
}
