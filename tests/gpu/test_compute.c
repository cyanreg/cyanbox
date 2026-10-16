/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The compute kernels (cyanbox/compute.h) against a double-precision
 * reference (av_tx's double RDFT), at every transform size they take, 64 to
 * 8192 points: the analysis uses two of them (2048 and 8192, both radix 4
 * all the way), and this is what covers the radix-2 stage of the others.
 * Signals are white noise from a fixed LCG, so every bin matters.
 *
 *  - STFT bands: frames every n / 4 samples, running off both ends of the
 *    signal (zero padding), a Hann window, bands of consecutive bins with
 *    uneven weights, raw sums (scale 0) and compressed ones.
 *  - Envelopes: windows partly outside the signal, a Tukey taper, three
 *    bands with raised-cosine gains, the RMS over 2h + 1 samples at points
 *    `step` apart.
 *
 * Every array must agree within 2e-5 of its largest value (fp32
 * transforms of up to 2^13 points are good to about 1e-6 of it). Needs no
 * window; skipped (77) without a device or with CYANBOX_ANALYSIS_GPU=0. */

#include "cyanbox/compute.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/mem.h>
#include <libavutil/tx.h>

static uint64_t lcg = 12345;

static double noise(void)
{
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((lcg >> 11) + 0.5) / 9007199254740992.0 * 2.0 - 1.0;
}

/* The real DFT of n doubles: n / 2 + 1 bins. */
static void rdft(int n, const double *in, AVComplexDouble *out)
{
    AVTXContext *tx = NULL;
    av_tx_fn fn;
    double scale = 1.0;
    double *buf = av_malloc((n + 2) * sizeof(*buf));
    if (!buf || av_tx_init(&tx, &fn, AV_TX_DOUBLE_RDFT, 0, n, &scale, 0) < 0)
        abort();
    memcpy(buf, in, n * sizeof(*buf));
    fn(tx, out, buf, sizeof(double));
    av_tx_uninit(&tx);
    av_free(buf);
}

/* Its unnormalised inverse. */
static void irdft(int n, AVComplexDouble *in, double *out)
{
    AVTXContext *tx = NULL;
    av_tx_fn fn;
    double scale = 1.0;
    if (av_tx_init(&tx, &fn, AV_TX_DOUBLE_RDFT, 1, n, &scale, 0) < 0)
        abort();
    fn(tx, out, in, sizeof(AVComplexDouble));
    av_tx_uninit(&tx);
}

static double rel_diff(const float *a, const double *ref, size_t n)
{
    double top = 0.0, d = 0.0;
    for (size_t i = 0; i < n; i++) {
        top = fmax(top, fabs(ref[i]));
        d = fmax(d, fabs(a[i] - ref[i]));
    }
    return top > 0.0 ? d / top : d;
}

typedef struct Frames {
    float *out;
    int nb_bands;
} Frames;

static int take(void *opaque, int64_t f0, int nb, const float *bands)
{
    Frames *fr = opaque;
    memcpy(fr->out + f0 * fr->nb_bands, bands, (size_t)nb * fr->nb_bands * sizeof(*bands));
    return 0;
}

static int check_stft(CBCompute *c, int n, float scale)
{
    const int hop = n / 4, nbins = n / 2 + 1, nb_frames = 23;
    const int64_t len = (int64_t)(nb_frames - 3) * hop + n / 3;
    const int width = (nbins + 63) / 64, nb_bands = (nbins + width - 1) / width;
    float *x = malloc(len * sizeof(*x)), *win = malloc(n * sizeof(*win));
    float *w = malloc(nbins * sizeof(*w)), *got = calloc(nb_frames * nb_bands, sizeof(*got));
    int *k0 = malloc(nb_bands * sizeof(*k0)), *k1 = malloc(nb_bands * sizeof(*k1));
    double *ref = malloc(nb_frames * nb_bands * sizeof(*ref)), *frame = malloc(n * sizeof(*frame));
    AVComplexDouble *spec = malloc(nbins * sizeof(*spec));
    if (!x || !win || !w || !got || !k0 || !k1 || !ref || !frame || !spec)
        abort();
    for (int64_t i = 0; i < len; i++)
        x[i] = (float)noise();
    for (int i = 0; i < n; i++)
        win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / n));
    for (int k = 0; k < nbins; k++)
        w[k] = (float)(0.25 + 0.75 * (k % 7) / 6.0);
    for (int b = 0; b < nb_bands; b++) {
        k0[b] = b * width;
        k1[b] = CB_MIN((b + 1) * width, nbins);
    }

    CBComputeSTFT p = {
        .nfft = n, .hop = hop, .window = win, .nb_bands = nb_bands,
        .k0 = k0, .k1 = k1, .weights = w, .scale = scale,
    };
    CBComputeSTFTPlan *plan = NULL;
    Frames fr = { .out = got, .nb_bands = nb_bands };
    int ret = cb_compute_stft_plan(c, &p, &plan);
    if (ret >= 0)
        ret = cb_compute_stft_run(plan, x, len, nb_frames, take, &fr);
    cb_compute_stft_free(&plan);

    for (int f = 0; f < nb_frames; f++) {
        for (int i = 0; i < n; i++) {
            int64_t j = (int64_t)f * hop - n / 2 + i;
            frame[i] = j >= 0 && j < len ? (double)x[j] * win[i] : 0.0;
        }
        rdft(n, frame, spec);
        for (int b = 0; b < nb_bands; b++) {
            double s = 0.0;
            for (int k = k0[b]; k < k1[b]; k++)
                s += w[k] * (spec[k].re * spec[k].re + spec[k].im * spec[k].im);
            ref[f * nb_bands + b] = scale > 0.0f ? log(1.0 + scale * sqrt(s)) : s;
        }
    }
    double d = ret < 0 ? 1.0 : rel_diff(got, ref, (size_t)nb_frames * nb_bands);
    int fail = ret < 0 || d > 2e-5;
    printf("%s stft %5d points, %s: %.1e%s\n", fail ? "FAIL" : "ok  ", n,
           scale > 0.0f ? "compressed" : "raw sums  ", d, ret < 0 ? " (the run failed)" : "");
    free(x);
    free(win);
    free(w);
    free(got);
    free(k0);
    free(k1);
    free(ref);
    free(frame);
    free(spec);
    return fail;
}

static int check_env(CBCompute *c, int n)
{
    enum { BANDS = 3, NB = 7 };
    const int nbins = n / 2 + 1, step = CB_MAX(n / 512, 1), half = n / (8 * step);
    const int smooth[BANDS] = { n / 40, n / 100, n / 200 };
    const int len = 2 * half + 1;
    const int64_t sig = 3 * n;
    float *x = malloc(sig * sizeof(*x)), *taper = malloc(n * sizeof(*taper));
    float *gains = malloc(BANDS * nbins * sizeof(*gains));
    float *got = malloc(NB * BANDS * len * sizeof(*got));
    double *ref = malloc(NB * BANDS * len * sizeof(*ref)), *buf = malloc((n + 2) * sizeof(*buf));
    double *y = malloc((n + 2) * sizeof(*y));
    AVComplexDouble *spec = malloc(nbins * sizeof(*spec)), *band = malloc(nbins * sizeof(*band));
    if (!x || !taper || !gains || !got || !ref || !buf || !y || !spec || !band)
        abort();
    for (int64_t i = 0; i < sig; i++)
        x[i] = (float)noise();
    for (int i = 0; i < n; i++) {
        int e = CB_MIN(i, n - 1 - i);
        taper[i] = e < n / 10 ? (float)(0.5 - 0.5 * cos(M_PI * e / (n / 10))) : 1.0f;
    }
    /* Low-pass, band-pass, high-pass, with raised-cosine edges. */
    const double edges[BANDS][2] = { { 0.0, 0.1 }, { 0.1, 0.4 }, { 0.4, 1.0 } };
    for (int b = 0; b < BANDS; b++) {
        for (int k = 0; k < nbins; k++) {
            double f = (double)k / (nbins - 1), g = 1.0;
            for (int s = 0; s < 2; s++) {
                double u = (f - edges[b][s]) / 0.05 + 0.5;
                if (s == 0 && edges[b][0] > 0.0)
                    g *= u <= 0.0 ? 0.0 : u >= 1.0 ? 1.0 : 0.5 - 0.5 * cos(M_PI * u);
                if (s == 1 && edges[b][1] < 1.0)
                    g *= u <= 0.0 ? 1.0 : u >= 1.0 ? 0.0 : 0.5 + 0.5 * cos(M_PI * u);
            }
            gains[b * nbins + k] = (float)(g / n);
        }
    }
    int64_t start[NB];
    for (int i = 0; i < NB; i++)
        start[i] = -n / 2 + (int64_t)i * (sig + n / 2) / NB;

    CBComputeEnv p = {
        .nfft = n, .taper = taper, .nb_bands = BANDS, .gains = gains, .smooth = smooth,
        .step = step, .half = half,
    };
    CBComputeEnvPlan *plan = NULL;
    int ret = cb_compute_env_plan(c, &p, &plan);
    if (ret >= 0)
        ret = cb_compute_env_run(plan, x, sig, start, NB, got);
    cb_compute_env_free(&plan);

    for (int w = 0; w < NB; w++) {
        for (int i = 0; i < n; i++) {
            int64_t j = start[w] + i;
            buf[i] = j >= 0 && j < sig ? (double)x[j] * taper[i] : 0.0;
        }
        rdft(n, buf, spec);
        for (int b = 0; b < BANDS; b++) {
            for (int k = 0; k < nbins; k++) {
                band[k].re = spec[k].re * gains[b * nbins + k];
                band[k].im = spec[k].im * gains[b * nbins + k];
            }
            irdft(n, band, y);
            int h = smooth[b];
            for (int i = 0; i < len; i++) {
                int q = n / 2 + (i - half) * step;
                double s = 0.0;
                for (int j = q - h; j <= q + h; j++)
                    s += y[j] * y[j];
                ref[(w * BANDS + b) * len + i] = sqrt(s / (2 * h + 1));
            }
        }
    }
    double d = ret < 0 ? 1.0 : rel_diff(got, ref, (size_t)NB * BANDS * len);
    int fail = ret < 0 || d > 2e-5;
    printf("%s envelopes %5d points: %.1e%s\n", fail ? "FAIL" : "ok  ", n, d,
           ret < 0 ? " (the run failed)" : "");
    free(x);
    free(taper);
    free(gains);
    free(got);
    free(ref);
    free(buf);
    free(y);
    free(spec);
    free(band);
    return fail;
}

int main(void)
{
    CBCompute *c = NULL;
    int ret = cb_compute_create(&c, NULL);
    if (ret < 0) {
        printf("no GPU (%s): skipped\n", strerror(-ret));
        return cb_compute_mode() == CB_COMPUTE_REQUIRED ? 1 : 77;
    }
    printf("on %s\n", cb_compute_device_name(c));

    int fails = 0;
    for (int n = 64; n <= 8192; n *= 2) {
        fails += check_stft(c, n, 0.0f);
        fails += check_stft(c, n, 2.0f);
    }
    for (int n = 256; n <= 8192; n *= 2)
        fails += check_env(c, n);

    int errors = cb_compute_validation_errors(c);
    if (errors) {
        printf("FAIL %d Vulkan validation errors\n", errors);
        fails++;
    }
    cb_compute_destroy(&c);
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
