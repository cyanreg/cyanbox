/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Musical key from a tuning-corrected, percussion-suppressed chroma: a port
 * of dj's analysis/key.py, with dj's tuning estimate fixed (below).
 *
 * The chain, as in dj: the mono mix is resampled to 44.1 kHz and then to
 * 11025 Hz with scipy's resample_poly (dj's export path resamples with
 * resample_poly, not ffmpeg, before analyse_key resamples again; both steps
 * are ported here so the samples match); a centred, periodic-Hann STFT of
 * 8192 points every 2048 samples (av_tx); the 60..2200 Hz bins; a 9-frame
 * median along time per bin, which keeps sustained partials and drops drum
 * hits; a global tuning offset from the spectral peaks; triangular weights
 * from each bin onto the semitones C2..B6, log-compressed and folded into
 * 12 pitch classes, summed over the track; and the best of the 24 rotations
 * of Temperley's (2005) Kostka-Payne key profiles by correlation. The
 * confidence is the margin between the best and second-best correlation.
 *
 * Instead of keeping the whole spectrogram, frames are produced on demand
 * into a ring of nine (the median's span) and reduced at once: the tuning
 * needs only the mean of each bin and the chroma only the per-bin sum of the
 * log-compressed magnitudes (the semitone weights are linear), so memory
 * stays small for any track length.
 *
 * Deliberate differences from dj:
 *
 * - dj's _tuning interpolates each peak's frequency from its index in the
 *   band-limited spectrum, which starts at the first bin above 60 Hz
 *   (bin 45), as if it started at bin 0, so every peak reads 45 bins
 *   (60.6 Hz) low. A perfectly tuned A440 tone then measures +44 cents, real
 *   tracks typically -45..-50 cents, and the chroma is shifted by that before
 *   folding: in dj a pure A440 lands 56 % on A and 44 % on Ab, and a track
 *   detuned by +30 cents can come out in an unrelated key. rxbox measures
 *   from the true bin, so tuning_cents is a real estimate (within ~2 cents
 *   on the test signals) and the key is read on the right semitones. Keys
 *   therefore differ from dj's sidecars on some tracks (on the synthetic
 *   test signals dj names 5 of 9 correctly, rxbox 7 of 9; see
 *   tests/analysis/test_key.c).
 * - Silence (no tonal energy at all) gives "" rather than dj's arbitrary
 *   "Bm" with confidence 0. */

#include "rxbox/analysis.h"

#include <math.h>
#include <stdio.h>

#include <libavutil/mem.h>
#include <libavutil/tx.h>

#include "rxbox/common.h"

#define ANALYSIS_RATE 44100
#define KEY_RATE 11025
#define N_FFT 8192
#define HOP 2048
#define NB_BINS (N_FFT / 2 + 1)
/* Median span in frames (scipy median_filter size (9, 1), mode "nearest"). */
#define MEDIAN 9
/* C2 .. B6 */
#define MIDI_LO 36
#define MIDI_HI 96

static const char *const MAJOR_NAMES[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
};
/* Spelled as rekordbox displays them */
static const char *const MINOR_NAMES[12] = {
    "Cm", "Dbm", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm", "Am", "Bbm", "Bm",
};

/* Temperley (2005), Kostka-Payne corpus; dj's default profiles. */
static const double TP_MAJOR[12] = {
    0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400,
};
static const double TP_MINOR[12] = {
    0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330,
};

/* ------------------------------------------------------------- resampling */

/* Modified Bessel function of the first kind, order 0 (power series). */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4;
    for (int k = 1; k < 500; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/* scipy.signal.resample_poly(x, up, down) with its defaults: a low-pass of
 * 2 * 10 * max(up, down) + 1 taps from firwin with a Kaiser window
 * (beta 5), cutoff 1 / max(up, down) of Nyquist, unit DC gain, times up;
 * zero padding; ceil(n * up / down) outputs, output k centred on input
 * k * down / up. The taps are rounded to float as scipy does for float32
 * input; the sums are kept in double. */
static int resample_poly(const float *x, int64_t n, int up, int down, float **out, int64_t *nb_out)
{
    int ret = 0;
    int max_rate = RX_MAX(up, down);
    int half = 10 * max_rate, taps = 2 * half + 1;
    int64_t nout = (n * up + down - 1) / down;
    double *h = malloc(sizeof(*h) * taps);
    float *hf = malloc(sizeof(*hf) * taps);
    float *y = malloc(sizeof(*y) * RX_MAX(nout, 1));
    if (!h || !hf || !y) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }

    double fc = 1.0 / max_rate, sum = 0.0, i0b = bessel_i0(5.0);
    for (int i = 0; i < taps; i++) {
        double m = i - half;
        double s = m == 0 ? 1.0 : sin(M_PI * fc * m) / (M_PI * fc * m);
        double r = m / half;
        double win = bessel_i0(5.0 * sqrt(1 - r * r)) / i0b;
        h[i] = fc * s * win;
        sum += h[i];
    }
    for (int i = 0; i < taps; i++) {
        hf[i] = (float)(h[i] / sum);
        hf[i] *= (float)up;
    }

    for (int64_t k = 0; k < nout; k++) {
        /* Inputs i with 0 <= c - up * i <= 2 * half */
        int64_t c = k * down + half;
        int64_t lo = c > 2 * half ? (c - 2 * half + up - 1) / up : 0;
        int64_t hi = RX_MIN(c / up, n - 1);
        double acc = 0.0;
        for (int64_t i = lo; i <= hi; i++)
            acc += (double)hf[c - up * i] * x[i];
        y[k] = (float)acc;
    }

    *out = y;
    *nb_out = nout;
    y = NULL;
fail:
    free(h);
    free(hf);
    free(y);
    return ret;
}

static int64_t gcd64(int64_t a, int64_t b)
{
    while (b) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* ------------------------------------------------------------------- STFT */

typedef struct Stft {
    const float *x;
    int64_t n;
    int64_t nb_frames;
    int bin_lo, nb;                 /* the band kept: bins [bin_lo, bin_lo + nb) */
    AVTXContext *tx;
    av_tx_fn fn;
    float *win, *frame;
    AVComplexFloat *spec;
    float *ring;                    /* MEDIAN frames of nb magnitudes */
    int64_t done;                   /* frames computed so far */
} Stft;

/* dj's stft_mag: frame f is centred on sample f * HOP, zero padded, times a
 * periodic Hann window (np.hanning(N + 1)[:-1] as float32); magnitudes of
 * the bins in the band go into the ring. */
static void stft_next(Stft *s)
{
    int64_t f = s->done++;
    int64_t start = f * HOP - N_FFT / 2;
    for (int i = 0; i < N_FFT; i++) {
        int64_t j = start + i;
        s->frame[i] = j >= 0 && j < s->n ? s->x[j] * s->win[i] : 0.0f;
    }
    s->fn(s->tx, s->spec, s->frame, sizeof(float));
    float *row = &s->ring[(f % MEDIAN) * s->nb];
    for (int b = 0; b < s->nb; b++) {
        AVComplexFloat v = s->spec[s->bin_lo + b];
        row[b] = hypotf(v.re, v.im);
    }
}

static void stft_free(Stft *s)
{
    av_tx_uninit(&s->tx);
    av_freep(&s->win);
    av_freep(&s->frame);
    av_freep(&s->spec);
    free(s->ring);
}

static int stft_init(Stft *s, const float *x, int64_t n, int bin_lo, int nb)
{
    *s = (Stft) {
        .x         = x,
        .n         = n,
        .nb_frames = n / HOP + 1,
        .bin_lo    = bin_lo,
        .nb        = nb,
    };
    float scale = 1.0f;
    int ret = av_tx_init(&s->tx, &s->fn, AV_TX_FLOAT_RDFT, 0, N_FFT, &scale, 0);
    /* AVERROR(e) is -e on POSIX systems, the same convention as rxbox's */
    if (ret < 0)
        return ret;
    s->win = av_malloc(sizeof(*s->win) * N_FFT);
    s->frame = av_malloc(sizeof(*s->frame) * (N_FFT + 2));
    s->spec = av_malloc(sizeof(*s->spec) * (NB_BINS + 1));
    s->ring = malloc(sizeof(*s->ring) * MEDIAN * nb);
    if (!s->win || !s->frame || !s->spec || !s->ring)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < N_FFT; i++)
        s->win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / N_FFT));
    return 0;
}

/* The median of MEDIAN values (an insertion sort: nine values, millions
 * of times). */
static float median9(float *v)
{
    for (int i = 1; i < MEDIAN; i++) {
        float t = v[i];
        int j = i;
        for (; j > 0 && v[j - 1] > t; j--)
            v[j] = v[j - 1];
        v[j] = t;
    }
    return v[MEDIAN / 2];
}

/* ------------------------------------------------------------------ chroma */

/* dj's _tuning, corrected: the magnitude-weighted circular mean of the
 * peaks' offsets from the equal-tempered semitones, in semitones. `s` is
 * the mean percussion-suppressed spectrum of the band, which starts at bin
 * `bin_lo`, and `freq` its bin frequencies. */
static double estimate_tuning(const double *s, const double *freq, int nb, int bin_lo)
{
    double df = (double)KEY_RATE / N_FFT, re = 0.0, im = 0.0;
    int nb_peaks = 0;
    for (int i = 1; i < nb - 1; i++) {
        if (!(s[i] > s[i - 1] && s[i] >= s[i + 1]) || !(freq[i] > 80 && freq[i] < 2000))
            continue;
        double a = log(s[i - 1] + 1e-9), b = log(s[i] + 1e-9), c = log(s[i + 1] + 1e-9);
        double off = RX_CLAMP(0.5 * (a - c) / (a - 2 * b + c + 1e-12), -0.5, 0.5);
        /* From the true bin number; dj used i alone (see the top of the file) */
        double f = (bin_lo + i + off) * df;
        double dev = 12 * log2(f / 440.0);
        /* np.round: half to even, as rint does in the default rounding mode */
        dev -= rint(dev);
        re += s[i] * cos(2 * M_PI * dev);
        im += s[i] * sin(2 * M_PI * dev);
        nb_peaks++;
    }
    if (!nb_peaks)
        return 0.0;
    return atan2(im, re) / (2 * M_PI);
}

typedef struct Score {
    double r;
    int tonic;
    bool minor;
} Score;

/* Python's descending sort of (score, tonic, minor) tuples. */
static int cmp_score(const void *pa, const void *pb)
{
    const Score *a = pa, *b = pb;
    if (a->r != b->r)
        return a->r < b->r ? 1 : -1;
    if (a->tonic != b->tonic)
        return a->tonic < b->tonic ? 1 : -1;
    return (int)b->minor - (int)a->minor;
}

/* dj's estimate_key: Pearson correlation of the chroma with each rotation of
 * the major and minor profiles. */
static void estimate_key(const double chroma[12], RxKeyResult *out)
{
    double cm = 0.0, c[12], cn = 0.0;
    for (int i = 0; i < 12; i++)
        cm += chroma[i];
    cm /= 12;
    for (int i = 0; i < 12; i++) {
        c[i] = chroma[i] - cm;
        cn += c[i] * c[i];
    }
    cn = sqrt(cn);

    Score scores[24];
    for (int mode = 0; mode < 2; mode++) {
        const double *prof = mode ? TP_MINOR : TP_MAJOR;
        double pm = 0.0, p[12], pn = 0.0;
        for (int i = 0; i < 12; i++)
            pm += prof[i];
        pm /= 12;
        for (int i = 0; i < 12; i++) {
            p[i] = prof[i] - pm;
            pn += p[i] * p[i];
        }
        pn = sqrt(pn);
        for (int k = 0; k < 12; k++) {
            double dot = 0.0;
            for (int i = 0; i < 12; i++)
                dot += c[i] * p[(i - k + 12) % 12];
            scores[mode * 12 + k] = (Score) {
                .r     = dot / (cn * pn + 1e-12),
                .tonic = k,
                .minor = mode,
            };
        }
    }
    qsort(scores, 24, sizeof(*scores), cmp_score);
    const char *name = scores[0].minor ? MINOR_NAMES[scores[0].tonic] : MAJOR_NAMES[scores[0].tonic];
    snprintf(out->key, sizeof(out->key), "%s", name);
    out->confidence = RX_CLAMP(scores[0].r - scores[1].r, 0.0, 1.0);
}

int rx_analyse_key(const float *mono, int64_t nb_samples, int rate, RxKeyResult *out)
{
    *out = (RxKeyResult) { 0 };
    if (rate <= 0 || nb_samples < 0 || (nb_samples && !mono))
        return RX_ERROR(EINVAL);
    if (!nb_samples)
        return 0;

    int ret = 0;
    float *x44 = NULL, *y = NULL;
    double *mean = NULL, *logsum = NULL, *freq = NULL;
    float *col = NULL;
    Stft s = { 0 };

    /* dj's export path: native rate -> 44.1 kHz, then analyse_key's
     * 44.1 kHz -> 11025 Hz, both with resample_poly. */
    const float *x = mono;
    int64_t n = nb_samples;
    if (rate != ANALYSIS_RATE) {
        int64_t g = gcd64(ANALYSIS_RATE, rate);
        ret = resample_poly(mono, nb_samples, (int)(ANALYSIS_RATE / g), (int)(rate / g), &x44, &n);
        if (ret < 0)
            goto fail;
        x = x44;
    }
    int64_t ny;
    ret = resample_poly(x, n, 1, ANALYSIS_RATE / KEY_RATE, &y, &ny);
    free(x44);
    x44 = NULL;
    if (ret < 0)
        goto fail;

    /* The 60..2200 Hz bins, with frequencies computed as numpy does. */
    int bin_lo = -1, bin_hi = -1;
    for (int k = 0; k < NB_BINS; k++) {
        double f = (double)(k * KEY_RATE) / N_FFT;
        if (f > 60 && f < 2200) {
            if (bin_lo < 0)
                bin_lo = k;
            bin_hi = k;
        }
    }
    int nb = bin_hi - bin_lo + 1;

    mean = calloc(nb, sizeof(*mean));
    logsum = calloc(nb, sizeof(*logsum));
    freq = malloc(sizeof(*freq) * nb);
    col = malloc(sizeof(*col) * MEDIAN);
    if (!mean || !logsum || !freq || !col) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    for (int b = 0; b < nb; b++)
        freq[b] = (double)((bin_lo + b) * KEY_RATE) / N_FFT;

    ret = stft_init(&s, y, ny, bin_lo, nb);
    if (ret < 0)
        goto fail;

    /* Median over frames f-4 .. f+4 (clamped to the ends) per bin. The ring
     * holds the last MEDIAN frames, enough for that span. */
    int64_t nf = s.nb_frames;
    for (int64_t f = 0; f < nf; f++) {
        while (s.done <= RX_MIN(f + MEDIAN / 2, nf - 1))
            stft_next(&s);
        for (int b = 0; b < nb; b++) {
            for (int d = 0; d < MEDIAN; d++) {
                int64_t j = RX_CLAMP(f + d - MEDIAN / 2, 0, nf - 1);
                col[d] = s.ring[(j % MEDIAN) * nb + b];
            }
            double harm = median9(col);
            mean[b] += harm;
            /* Full-scale sine -> ~1.0 before compression, as in dj */
            logsum[b] += log1p(100.0 * harm / (N_FFT / 4));
        }
    }
    for (int b = 0; b < nb; b++)
        mean[b] /= nf;

    double tuning = estimate_tuning(mean, freq, nb, bin_lo);

    /* Triangular weights from each bin onto the semitone centres, folded
     * into pitch classes. */
    double chroma[12] = { 0 };
    for (int b = 0; b < nb; b++) {
        double midi = 69 + 12 * log2(freq[b] / 440.0) - tuning;
        for (int m = MIDI_LO; m < MIDI_HI; m++) {
            double w = 1.0 - fabs(midi - m);
            if (w > 0)
                chroma[m % 12] += logsum[b] * w;
        }
    }

    out->tuning_cents = 100 * tuning;
    double cmax = 0.0;
    for (int i = 0; i < 12; i++)
        cmax = fmax(cmax, chroma[i]);
    if (cmax > 0)
        estimate_key(chroma, out);

fail:
    stft_free(&s);
    free(x44);
    free(y);
    free(mean);
    free(logsum);
    free(freq);
    free(col);
    return ret;
}
