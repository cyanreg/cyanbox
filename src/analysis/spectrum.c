/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The spectral front end of the beat analyser: a centred Hann STFT with
 * mel bands, a dedicated kick band, the resampler, and fine band envelopes
 * around chosen instants. Every FFT is libavutil's av_tx. Everything else
 * in the analyser reads only what this file produces, so a GPU version can
 * replace it behind the same interface.
 *
 * Choices, and why:
 *  - 2048-point frames every 294 samples at 44.1 kHz (150 frames a second,
 *    as dj, and the rate of the waveform columns). 46 ms frames give 21.5 Hz
 *    bins, so the kick band (30..150 Hz) is six bins rather than the one and
 *    a half a 1024-point frame would give; rbxport measured that a low band
 *    over four bins cannot place an onset.
 *  - Mel bands are taken on the power spectrum and square-rooted back to an
 *    amplitude (the RMS amplitude of the band). This is the band's energy,
 *    which is what a drum hit raises, and it avoids a square root per bin.
 *  - Compression is dj's log1p(1000 * amplitude / (N/4)): a full-scale sine
 *    reads about log(1000), quiet passages stay near linear so that noise
 *    does not produce flux.
 *  - The fine envelopes are not computed for the whole track. Attack timing
 *    is only needed near beats, so a 186 ms window around each beat is
 *    transformed once and three bands are cut from it in the frequency
 *    domain (zero phase, no group delay to correct). dj ran three 4th-order
 *    zero-phase Butterworth filters over every sample, which alone costs
 *    more than this whole analyser. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/tx.h>
#include <libswresample/swresample.h>

/* --------------------------------------------------------------- resample */

int rx_resample_mono(const float *in, int64_t n, int in_rate, int out_rate,
                     float **out, int64_t *out_n)
{
    *out = NULL;
    *out_n = 0;
    if (in_rate <= 0 || out_rate <= 0 || n < 0)
        return RX_ERROR(EINVAL);

    int64_t cap = av_rescale_rnd(n, out_rate, in_rate, AV_ROUND_UP) + 256;
    float *buf = malloc((size_t)RX_MAX(cap, 1) * sizeof(*buf));
    if (!buf)
        return RX_ERROR(ENOMEM);
    if (in_rate == out_rate) {
        memcpy(buf, in, (size_t)n * sizeof(*buf));
        *out = buf;
        *out_n = n;
        return 0;
    }

    SwrContext *swr = NULL;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    int ret = swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_FLT, out_rate,
                                  &mono, AV_SAMPLE_FMT_FLT, in_rate, 0, NULL);
    if (ret < 0)
        goto fail;
    /* A longer filter than the default: the kick band and the click band
     * both survive any filter, but there is no reason to alias. */
    av_opt_set_int(swr, "filter_size", 32, 0);
    av_opt_set_int(swr, "phase_shift", 10, 0);
    ret = swr_init(swr);
    if (ret < 0)
        goto fail;

    /* libswresample's output is time-aligned with its input (it accounts
     * for its filter's delay itself): an impulse at 1 s from 22.05, 32, 48
     * or 96 kHz comes out at 1 s to a hundredth of a sample. Everything is
     * fed, then flushed, then cut to the expected length. */
    int64_t got = 0;
    const int64_t chunk = 1 << 20;
    for (int64_t pos = 0; pos < n; pos += chunk) {
        int len = (int)RX_MIN(chunk, n - pos);
        const uint8_t *src[1] = { (const uint8_t *)(in + pos) };
        uint8_t *dst[1] = { (uint8_t *)(buf + got) };
        ret = swr_convert(swr, dst, (int)(cap - got), src, len);
        if (ret < 0)
            goto fail;
        got += ret;
    }
    while (1) {
        uint8_t *dst[1] = { (uint8_t *)(buf + got) };
        ret = swr_convert(swr, dst, (int)(cap - got), NULL, 0);
        if (ret < 0)
            goto fail;
        if (ret == 0)
            break;
        got += ret;
    }
    swr_free(&swr);

    int64_t want = av_rescale_rnd(n, out_rate, in_rate, AV_ROUND_NEAR_INF);
    for (int64_t i = RX_MIN(got, want); i < want; i++)
        buf[i] = 0.0f;
    *out = buf;
    *out_n = want;
    return 0;

fail:
    swr_free(&swr);
    free(buf);
    return ret < 0 ? ret : RX_ERROR(EINVAL);
}

/* ------------------------------------------------------------------ STFT */

static double hz_to_mel(double f)
{
    return 2595.0 * log10(1.0 + f / 700.0);
}

static double mel_to_hz(double m)
{
    return 700.0 * (pow(10.0, m / 2595.0) - 1.0);
}

typedef struct MelBand {
    int    k0, k1;                  /* bins [k0, k1) */
    float *w;                       /* k1 - k0 weights */
} MelBand;

int rx_spectrum_compute(const float *x, int64_t n, RxSpectrum *s)
{
    memset(s, 0, sizeof(*s));
    const int N = RX_BEAT_NFFT, hop = RX_BEAT_HOP, nbins = N / 2 + 1;
    int ret = 0;

    int64_t nf = n / hop + 1;
    if (nf > INT32_MAX / RX_BEAT_NMELS)
        return RX_ERROR(EINVAL);
    s->nb_frames = (int)nf;
    s->fps = (double)RX_BEAT_RATE / hop;
    s->nb_mels = RX_BEAT_NMELS;
    s->mel = malloc((size_t)nf * RX_BEAT_NMELS * sizeof(*s->mel));
    s->kick = malloc((size_t)nf * sizeof(*s->kick));

    AVTXContext *tx = NULL;
    av_tx_fn fn = NULL;
    float *win = av_malloc(N * sizeof(*win));
    float *frame = av_malloc(N * sizeof(*frame));
    AVComplexFloat *spec = av_malloc(nbins * sizeof(*spec));
    float *pow2 = av_malloc(nbins * sizeof(*pow2));
    float *weights = malloc(2 * nbins * sizeof(*weights));
    MelBand bands[RX_BEAT_NMELS];
    if (!s->mel || !s->kick || !win || !frame || !spec || !pow2 || !weights) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    float scale = 1.0f;
    ret = av_tx_init(&tx, &fn, AV_TX_FLOAT_RDFT, 0, N, &scale, 0);
    if (ret < 0)
        goto fail;

    /* Periodic Hann, as np.hanning(N + 1)[:-1] in dj. */
    for (int i = 0; i < N; i++)
        win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / N));

    /* Triangular mel filters exactly as dj's mel_filterbank (HTK mel,
     * 64 bands 30 Hz .. 16 kHz, triangles in Hz over the bin frequencies);
     * stored sparsely since each bin feeds at most two bands. */
    double pts[RX_BEAT_NMELS + 2];
    double m0 = hz_to_mel(30.0), m1 = hz_to_mel(16000.0);
    for (int i = 0; i < RX_BEAT_NMELS + 2; i++)
        pts[i] = mel_to_hz(m0 + (m1 - m0) * i / (RX_BEAT_NMELS + 1));
    float *wp = weights;
    for (int b = 0; b < RX_BEAT_NMELS; b++) {
        double lo = pts[b], c = pts[b + 1], hi = pts[b + 2];
        s->mel_hz[b] = c;
        int k0 = nbins, k1 = 0;
        for (int k = 0; k < nbins; k++) {
            double f = (double)k * RX_BEAT_RATE / N;
            if (fmin((f - lo) / (c - lo), (hi - f) / (hi - c)) > 0.0) {
                k0 = RX_MIN(k0, k);
                k1 = k + 1;
            }
        }
        k1 = RX_MAX(k1, k0);
        bands[b] = (MelBand) { .k0 = k0, .k1 = k1, .w = wp };
        for (int k = k0; k < k1; k++) {
            double f = (double)k * RX_BEAT_RATE / N;
            *wp++ = (float)fmax(0.0, fmin((f - lo) / (c - lo), (hi - f) / (hi - c)));
        }
    }

    /* The kick band: 30..150 Hz from the bins themselves (bins 1 and 7 at
     * half weight), finer than the lowest mel triangles, which straddle
     * kick and bass. */
    static const struct { int k; float w; } kick_bins[] = {
        { 1, 0.5f }, { 2, 1.0f }, { 3, 1.0f }, { 4, 1.0f }, { 5, 1.0f }, { 6, 1.0f }, { 7, 0.5f },
    };

    const float comp = 1000.0f / (N / 4);
    for (int f = 0; f < s->nb_frames; f++) {
        int64_t c = (int64_t)f * hop, a = c - N / 2;
        if (a >= 0 && a + N <= n) {
            const float *src = x + a;
            for (int i = 0; i < N; i++)
                frame[i] = src[i] * win[i];
        } else {
            for (int i = 0; i < N; i++) {
                int64_t j = a + i;
                frame[i] = (j >= 0 && j < n) ? x[j] * win[i] : 0.0f;
            }
        }
        fn(tx, spec, frame, sizeof(float));
        const float *sp = (const float *)spec;
        for (int k = 0; k < nbins; k++)
            pow2[k] = sp[2 * k] * sp[2 * k] + sp[2 * k + 1] * sp[2 * k + 1];

        /* Four accumulators: a single one makes every band a chain of
         * dependent multiply-adds, which was most of this function's time. */
        float *out = s->mel + (size_t)f * RX_BEAT_NMELS;
        for (int b = 0; b < RX_BEAT_NMELS; b++) {
            const MelBand *mb = &bands[b];
            const float *w = mb->w - mb->k0;
            float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            int k = mb->k0;
            for (; k + 4 <= mb->k1; k += 4) {
                a0 += w[k] * pow2[k];
                a1 += w[k + 1] * pow2[k + 1];
                a2 += w[k + 2] * pow2[k + 2];
                a3 += w[k + 3] * pow2[k + 3];
            }
            for (; k < mb->k1; k++)
                a0 += w[k] * pow2[k];
            out[b] = logf(1.0f + comp * sqrtf((a0 + a1) + (a2 + a3)));
        }
        float kick = 0.0f;
        for (size_t i = 0; i < RX_ARRAY_ELEMS(kick_bins); i++)
            kick += kick_bins[i].w * pow2[kick_bins[i].k];
        s->kick[f] = logf(1.0f + comp * sqrtf(kick));
    }

fail:
    av_tx_uninit(&tx);
    av_free(win);
    av_free(frame);
    av_free(spec);
    av_free(pow2);
    free(weights);
    if (ret < 0)
        rx_spectrum_free(s);
    return ret;
}

void rx_spectrum_free(RxSpectrum *s)
{
    free(s->mel);
    free(s->kick);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------- fine envelopes */

/* The window transformed per beat: 8192 samples (186 ms). The envelope is
 * read only from its middle +-58 ms, so the band filters' impulse
 * responses (the narrowest transition is 40 Hz wide, ~25 ms) never wrap
 * around the window's edges into the part that is read. */
#define ENV_N     8192
#define ENV_BINS  (ENV_N / 2 + 1)

/* Energy smoothing per band, as dj's fine_envelopes: a kick body needs
 * 10 ms to be one bump, a click 2 ms. */
static const double env_smooth[RX_NB_BANDS] = { 0.010, 0.004, 0.002 };

struct RxEnvCtx {
    const float    *x;
    int64_t         n;
    AVTXContext    *fwd, *inv;
    av_tx_fn        fwd_fn, inv_fn;
    float          *buf;            /* ENV_N + 2 */
    AVComplexFloat *spec;           /* ENV_BINS */
    AVComplexFloat *band;           /* ENV_BINS */
    float          *y;              /* ENV_N */
    float          *mask[RX_NB_BANDS];
    double         *cum;            /* ENV_N + 1 prefix sums of y^2 */
};

/* A band-pass mask with raised-cosine edges `tw` Hz wide around lo and
 * hi (lo <= 0: low-pass, hi <= 0: high-pass). Real and even, so zero phase. */
static void make_mask(float *m, double lo, double hi, double tw)
{
    for (int k = 0; k < ENV_BINS; k++) {
        double f = (double)k * RX_BEAT_RATE / ENV_N, g = 1.0;
        if (lo > 0.0) {
            double u = (f - (lo - tw / 2)) / tw;
            g *= u <= 0.0 ? 0.0 : u >= 1.0 ? 1.0 : 0.5 - 0.5 * cos(M_PI * u);
        }
        if (hi > 0.0) {
            double u = (f - (hi - tw / 2)) / tw;
            g *= u <= 0.0 ? 1.0 : u >= 1.0 ? 0.0 : 0.5 + 0.5 * cos(M_PI * u);
        }
        /* The inverse RDFT is unnormalised. */
        m[k] = (float)(g / ENV_N);
    }
}

int rx_env_alloc(RxEnvCtx **out, const float *x, int64_t n)
{
    RxEnvCtx *c = calloc(1, sizeof(*c));
    if (!c)
        return RX_ERROR(ENOMEM);
    c->x = x;
    c->n = n;
    c->buf = av_malloc((ENV_N + 2) * sizeof(*c->buf));
    c->spec = av_malloc(ENV_BINS * sizeof(*c->spec));
    c->band = av_malloc(ENV_BINS * sizeof(*c->band));
    c->y = av_malloc(ENV_N * sizeof(*c->y));
    c->cum = malloc((ENV_N + 1) * sizeof(*c->cum));
    int ret = 0;
    for (int b = 0; b < RX_NB_BANDS; b++) {
        c->mask[b] = malloc(ENV_BINS * sizeof(*c->mask[b]));
        if (!c->mask[b])
            ret = RX_ERROR(ENOMEM);
    }
    if (!c->buf || !c->spec || !c->band || !c->y || !c->cum)
        ret = RX_ERROR(ENOMEM);
    if (ret < 0)
        goto fail;

    float scale = 1.0f;
    ret = av_tx_init(&c->fwd, &c->fwd_fn, AV_TX_FLOAT_RDFT, 0, ENV_N, &scale, 0);
    if (ret < 0)
        goto fail;
    ret = av_tx_init(&c->inv, &c->inv_fn, AV_TX_FLOAT_RDFT, 1, ENV_N, &scale, 0);
    if (ret < 0)
        goto fail;

    /* dj's bands: low-pass 150 Hz, band-pass 150..2500 Hz, high-pass
     * 2500 Hz. The top is cut at 16 kHz so that lossy files' empty top
     * octave adds no noise. */
    make_mask(c->mask[RX_BAND_LOW], 25.0, 150.0, 40.0);
    make_mask(c->mask[RX_BAND_MID], 150.0, 2500.0, 80.0);
    make_mask(c->mask[RX_BAND_HIGH], 2500.0, 16000.0, 400.0);
    *out = c;
    return 0;

fail:
    rx_env_free(&c);
    return ret;
}

void rx_env_free(RxEnvCtx **pc)
{
    RxEnvCtx *c = *pc;
    if (!c)
        return;
    av_tx_uninit(&c->fwd);
    av_tx_uninit(&c->inv);
    av_free(c->buf);
    av_free(c->spec);
    av_free(c->band);
    av_free(c->y);
    free(c->cum);
    for (int b = 0; b < RX_NB_BANDS; b++)
        free(c->mask[b]);
    free(c);
    *pc = NULL;
}

int rx_env_window(RxEnvCtx *c, double t, float *env, double *centre)
{
    /* The centre on the envelope's own sample grid, so that windows taken
     * around different beats share one time base and can be folded. */
    int64_t ci = (int64_t)llround(t * RX_ENV_RATE);
    int64_t cs = ci * RX_ENV_DECIM;
    *centre = (double)ci / RX_ENV_RATE;
    int64_t a = cs - ENV_N / 2;

    /* A gentle taper (Tukey, 10 % each side) against the window's edges
     * ringing through the band filters. */
    for (int i = 0; i < ENV_N; i++) {
        int64_t j = a + i;
        float v = (j >= 0 && j < c->n) ? c->x[j] : 0.0f;
        int e = RX_MIN(i, ENV_N - 1 - i);
        if (e < ENV_N / 10)
            v *= (float)(0.5 - 0.5 * cos(M_PI * e / (ENV_N / 10)));
        c->buf[i] = v;
    }
    c->fwd_fn(c->fwd, c->spec, c->buf, sizeof(float));

    for (int b = 0; b < RX_NB_BANDS; b++) {
        const float *m = c->mask[b];
        for (int k = 0; k < ENV_BINS; k++) {
            c->band[k].re = c->spec[k].re * m[k];
            c->band[k].im = c->spec[k].im * m[k];
        }
        c->inv_fn(c->inv, c->y, c->band, sizeof(AVComplexFloat));

        /* Box-smoothed energy, sampled every RX_ENV_DECIM samples, as an
         * amplitude (its square root). dj compressed further (^0.3 of the
         * energy); that makes the steepest rise sit at the very start of a
         * rise, where the filters' faint pre-ringing is, 4-5 ms early on
         * the synthetic kicks. Attack times are taken on the energy
         * (grid_fit.c), the amplitude only finds the rises. */
        /* Prefix sums only over the samples the envelope reads. */
        int h = (int)(env_smooth[b] * RX_BEAT_RATE) / 2;
        int a0 = ENV_N / 2 - RX_ENV_HALF * RX_ENV_DECIM - h;
        int a1 = ENV_N / 2 + RX_ENV_HALF * RX_ENV_DECIM + h + 1;
        double *cum = c->cum - a0;
        cum[a0] = 0.0;
        for (int i = a0; i < a1; i++)
            cum[i + 1] = cum[i] + (double)c->y[i] * c->y[i];
        float *e = env + b * RX_ENV_LEN;
        for (int i = 0; i < RX_ENV_LEN; i++) {
            int p = ENV_N / 2 + (i - RX_ENV_HALF) * RX_ENV_DECIM;
            double en = (cum[p + h + 1] - cum[p - h]) / (2 * h + 1);
            e[i] = (float)sqrt(fmax(en, 0.0));
        }
    }
    return 0;
}

/* ------------------------------------------------------------ bass chroma */

#define BASS_DECIM 16
#define BASS_N     2048

int rx_bass_chroma(const float *x, int64_t n, const double *times, int nb, float *out)
{
    const double rate = (double)RX_BEAT_RATE / BASS_DECIM;
    AVTXContext *tx = NULL;
    av_tx_fn fn;
    float scale = 1.0f;
    float *buf = av_malloc((BASS_N + 2) * sizeof(*buf));
    AVComplexFloat *spec = av_malloc((BASS_N / 2 + 1) * sizeof(*spec));
    int ret = 0;
    if (!buf || !spec) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_tx_init(&tx, &fn, AV_TX_FLOAT_RDFT, 0, BASS_N, &scale, 0)) < 0)
        goto end;

    float *bass_win = malloc(BASS_N * sizeof(*bass_win));
    if (!bass_win) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < BASS_N; i++)
        bass_win[i] = (float)((0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / BASS_N)) / BASS_DECIM);

    /* The pitch class of every bin from 30 to 250 Hz (A = 440 Hz tuning;
     * a bass a few cents out still lands in its class). */
    int pc[BASS_N / 2 + 1];
    for (int k = 0; k <= BASS_N / 2; k++) {
        double f = k * rate / BASS_N;
        pc[k] = -1;
        if (f >= 30.0 && f <= 250.0)
            pc[k] = ((int)lrint(12.0 * log2(f / 440.0)) % 12 + 12) % 12;
    }

    for (int b = 0; b < nb; b++) {
        float *c = out + (size_t)b * 12;
        memset(c, 0, 12 * sizeof(*c));
        /* Decimate by averaging blocks of 16 samples: flat to 250 Hz
         * within 0.1 dB, and what folds into 30..250 Hz comes from around
         * 2.5-3 kHz, next to the average's own null. */
        int64_t s0 = (int64_t)llround((times[b] + 0.0625) * RX_BEAT_RATE);
        for (int i = 0; i < BASS_N; i++) {
            int64_t c0 = s0 + (int64_t)i * BASS_DECIM;
            float acc = 0.0f;
            if (c0 >= 0 && c0 + BASS_DECIM <= n) {
                for (int j = 0; j < BASS_DECIM; j++)
                    acc += x[c0 + j];
            } else {
                for (int j = 0; j < BASS_DECIM; j++)
                    acc += c0 + j >= 0 && c0 + j < n ? x[c0 + j] : 0.0f;
            }
            buf[i] = acc * bass_win[i];
        }
        fn(tx, spec, buf, sizeof(float));
        double tot = 0.0;
        for (int k = 0; k <= BASS_N / 2; k++) {
            if (pc[k] < 0)
                continue;
            double m = spec[k].re * spec[k].re + spec[k].im * spec[k].im;
            c[pc[k]] += (float)m;
            tot += m;
        }
        if (tot > 0.0) {
            for (int k = 0; k < 12; k++)
                c[k] = (float)(c[k] / tot);
        }
    }

    free(bass_win);

end:
    av_tx_uninit(&tx);
    av_free(buf);
    av_free(spec);
    return ret;
}
