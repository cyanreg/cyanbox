/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Decoding a whole file to interleaved float in memory, on the players'
 * timeline (see cyanbox/audio.h): a source (source.c), FFmpeg's path,
 * read straight into the buffer, which starts at the container's length
 * and grows by half when that was short. What a stick, Link or a deck
 * gets is the same source's output, sample for sample (test_pipeline).
 *
 * The rest: buffers made from decoded ones (mono, resampled), and the
 * channel mix and decoder set-up the packet store (packets.c) shares.
 *
 * The mix (cb_mixer_frame): samples are converted to float exactly as
 * libswresample does (s16 / 32768, s32 / 2^31); the mono mix is the mean
 * of the channels, summed then divided, as dj's numpy mean; stereo from
 * mono duplicates it, stereo from more channels uses libswresample's
 * downmix matrix normalised to unity. A float sample that is not finite
 * becomes 0 there, counted. */

#include "cyanbox/audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdckdint.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "internal.h"

/* --------------------------------------------------------------- resampler */

/* libswresample's own engine with its defaults, as a source resamples
 * (source.c), so a buffer resampled here is what a decode at that rate
 * gives. */
static int resampler_open(SwrContext **out, int channels, int in_rate, int out_rate)
{
    *out = NULL;
    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    SwrContext *swr = NULL;
    int ret = swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, out_rate,
                                  &layout, AV_SAMPLE_FMT_FLT, in_rate, 0, NULL);
    av_channel_layout_uninit(&layout);
    if (ret < 0)
        return cb_audio_error(ret);
    ret = swr_init(swr);
    if (ret < 0) {
        swr_free(&swr);
        return cb_audio_error(ret);
    }
    *out = swr;
    return 0;
}

/* ------------------------------------------------------------------- mixer */

static int grow(void **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    void *nb = realloc(*buf, need);
    if (!nb)
        return CB_ERROR(ENOMEM);
    *buf = nb;
    *cap = need;
    return 0;
}

static int mixer_setup(CBMixer *s, const AVChannelLayout *layout)
{
    int in = layout->nb_channels;
    if (in == s->mix_in)
        return 0;
    free(s->matrix);
    s->matrix = NULL;
    s->mix_in = in;

    if (in == s->out_ch) {
        s->mix_kind = CB_MIX_COPY;
        return 0;
    }
    if (s->out_ch == 1) {
        s->mix_kind = CB_MIX_MEAN;
        return 0;
    }

    s->mix_kind = CB_MIX_MATRIX;
    s->matrix = calloc((size_t)s->out_ch * in, sizeof(float));
    if (!s->matrix)
        return CB_ERROR(ENOMEM);

    if (in == 1) {
        for (int o = 0; o < s->out_ch; o++)
            s->matrix[o] = 1.0f;
        return 0;
    }

    if (s->out_ch == 2) {
        AVChannelLayout in_l = { 0 }, out_l = AV_CHANNEL_LAYOUT_STEREO;
        if (layout->order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&in_l, in);
        else if (av_channel_layout_copy(&in_l, layout) < 0)
            return CB_ERROR(ENOMEM);
        /* swr_build_matrix2 scales all 64 x 64 entries at the stride it is
         * given (rematrix.c), whatever the layouts: a matrix of that size */
        double *m = calloc((size_t)64 * 64, sizeof(*m));
        if (!m) {
            av_channel_layout_uninit(&in_l);
            return CB_ERROR(ENOMEM);
        }
        int ret = in <= 64 ? swr_build_matrix2(&in_l, &out_l, M_SQRT1_2, M_SQRT1_2, 0.0, 1.0, 1.0, m,
                                               64, AV_MATRIX_ENCODING_NONE, NULL)
                           : CB_ERROR(ENOTSUP);
        av_channel_layout_uninit(&in_l);
        if (ret >= 0) {
            for (int o = 0; o < 2; o++)
                for (int c = 0; c < in; c++)
                    s->matrix[o * in + c] = (float)m[o * 64 + c];
            free(m);
            return 0;
        }
        free(m);
    }

    /* A channel count that changed mid-stream, or a layout with no downmix:
     * the first channels as they are. */
    for (int o = 0; o < CB_MIN(in, s->out_ch); o++)
        s->matrix[o * in + o] = 1.0f;
    return 0;
}

/* One channel of a frame as float, the way libswresample converts, except
 * that a sample that is not finite becomes 0 (see the top). Returns how
 * many did. */
static int to_float(const AVFrame *f, int c, float *dst)
{
    int n = f->nb_samples, ch = f->ch_layout.nb_channels;
    bool planar = av_sample_fmt_is_planar(f->format);
    const uint8_t *base = planar ? f->extended_data[c] : f->extended_data[0];
    size_t step = planar ? 1 : (size_t)ch;
    size_t off = planar ? 0 : (size_t)c;
    int bad = 0;

    switch (av_get_packed_sample_fmt(f->format)) {
    case AV_SAMPLE_FMT_U8:
        for (int i = 0; i < n; i++)
            dst[i] = ((int)base[i * step + off] - 0x80) * (1.0f / (1 << 7));
        break;
    case AV_SAMPLE_FMT_S16: {
        const int16_t *x = (const int16_t *)base;
        for (int i = 0; i < n; i++)
            dst[i] = x[i * step + off] * (1.0f / (1 << 15));
        break;
    }
    case AV_SAMPLE_FMT_S32: {
        const int32_t *x = (const int32_t *)base;
        for (int i = 0; i < n; i++)
            dst[i] = x[i * step + off] * (1.0f / (1U << 31));
        break;
    }
    case AV_SAMPLE_FMT_S64: {
        const int64_t *x = (const int64_t *)base;
        for (int i = 0; i < n; i++)
            dst[i] = (float)(x[i * step + off] * (1.0 / (UINT64_C(1) << 63)));
        break;
    }
    case AV_SAMPLE_FMT_FLT: {
        const float *x = (const float *)base;
        for (int i = 0; i < n; i++) {
            float v = x[i * step + off];
            if (!isfinite(v)) {
                v = 0.0f;
                bad++;
            }
            dst[i] = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_DBL: {
        /* Checked as float: a double beyond float's range is infinite there. */
        const double *x = (const double *)base;
        for (int i = 0; i < n; i++) {
            float v = (float)x[i * step + off];
            if (!isfinite(v)) {
                v = 0.0f;
                bad++;
            }
            dst[i] = v;
        }
        break;
    }
    default:
        memset(dst, 0, (size_t)n * sizeof(float));
        break;
    }
    return bad;
}

int cb_mixer_frame(CBMixer *s, const AVFrame *f, const float **out)
{
    int n = f->nb_samples, in = f->ch_layout.nb_channels;
    *out = NULL;
    if (n <= 0 || in <= 0)
        return 0;

    int ret = mixer_setup(s, &f->ch_layout);
    if (ret < 0)
        return ret;
    ret = grow((void **)&s->planar, &s->planar_cap, (size_t)n * in * sizeof(float));
    if (ret >= 0)
        ret = grow((void **)&s->mixed, &s->mixed_cap, (size_t)n * s->out_ch * sizeof(float));
    if (ret < 0)
        return ret;

    for (int c = 0; c < in; c++)
        s->nonfinite += to_float(f, c, s->planar + (size_t)c * n);

    const float *p = s->planar;
    float *m = s->mixed;
    int oc = s->out_ch;
    switch (s->mix_kind) {
    case CB_MIX_COPY:
        for (int i = 0; i < n; i++)
            for (int c = 0; c < in; c++)
                m[(size_t)i * oc + c] = p[(size_t)c * n + i];
        break;
    case CB_MIX_MEAN:
        for (int i = 0; i < n; i++) {
            float sum = 0.0f;
            for (int c = 0; c < in; c++)
                sum += p[(size_t)c * n + i];
            m[i] = sum / (float)in;
        }
        break;
    case CB_MIX_MATRIX:
        for (int i = 0; i < n; i++) {
            for (int o = 0; o < oc; o++) {
                float acc = 0.0f;
                for (int c = 0; c < in; c++)
                    acc += s->matrix[o * in + c] * p[(size_t)c * n + i];
                m[(size_t)i * oc + o] = acc;
            }
        }
        break;
    }
    *out = m;
    return n;
}

void cb_mixer_free(CBMixer *s)
{
    free(s->matrix);
    free(s->planar);
    free(s->mixed);
    *s = (CBMixer) { .out_ch = s->out_ch };
}

int cb_audio_open_decoder(AVCodecContext **out, const AVCodecParameters *par, AVRational tb,
                          bool skip_manual, int delay)
{
    *out = NULL;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        return CB_ERROR(ENOTSUP);
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec)
        return CB_ERROR(ENOMEM);
    int ret = avcodec_parameters_to_context(dec, par);
    if (ret < 0)
        goto fail;
    dec->pkt_timebase = tb;
    if (skip_manual)
        dec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    /* avcodec_parameters_to_context() made it the stream's initial padding,
     * which the decoder drops from its first output */
    if (delay >= 0)
        dec->delay = delay;
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0)
        goto fail;
    *out = dec;
    return 0;

fail:
    avcodec_free_context(&dec);
    return cb_audio_error(ret);
}

/* ------------------------------------------------------------------- decode */

int cb_decode(const char *path, int channels, int rate, CBAudio *out)
{
    *out = (CBAudio) { 0 };
    if (channels < 0 || channels > 2 || rate < 0)
        return CB_ERROR(EINVAL);
    CBAudioFormat f = { .sample = CB_SAMPLE_FLT, .rate = rate, .channels = channels };
    CBSource *s;
    int ret = cb_source_open(&s, path, &f);
    if (ret < 0)
        return ret;
    int ch = cb_source_channels(s);
    /* The container's length plus a little, an hour at most up front */
    int64_t est = cb_source_length(s);
    int64_t cap = CB_CLAMP(est > 0 ? est + est / 100 : 0, (int64_t)0,
                           (int64_t)3600 * cb_source_rate(s)) + 65536;
    float *buf = NULL;
    int64_t nb = 0;
    while (1) {
        if (nb + 65536 > cap || !buf) {
            int64_t want = buf ? cap + cap / 2 : cap;
            size_t bytes;
            float *g = ckd_mul(&bytes, (size_t)want, (size_t)ch * sizeof(float)) ? NULL
                                                                                : realloc(buf, bytes);
            if (!g) {
                ret = CB_ERROR(ENOMEM);
                break;
            }
            buf = g;
            cap = want;
        }
        uint8_t *dst[1] = { (uint8_t *)(buf + nb * ch) };
        int got = cb_source_read(s, dst, (int)CB_MIN(cap - nb, (int64_t)1 << 20));
        if (got < 0) {
            ret = got;
            break;
        }
        if (!got)
            break;
        nb += got;
    }
    out->rate = cb_source_rate(s);
    cb_source_free(&s);
    if (ret >= 0 && !nb) {
        cb_debug("%s: no audio decoded", path);
        ret = CB_ERROR(ENODATA);
    }
    if (ret < 0) {
        free(buf);
        *out = (CBAudio) { 0 };
        return ret;
    }
    float *shrunk = realloc(buf, (size_t)nb * ch * sizeof(float));
    *out = (CBAudio) { .samples = shrunk ? shrunk : buf, .channels = ch, .rate = out->rate,
                       .nb_frames = nb };
    return 0;
}

/* ---------------------------------------------------------- derived buffers */

int cb_audio_to_mono(const CBAudio *in, CBAudio *out)
{
    *out = (CBAudio) { 0 };
    if (in->channels <= 0 || in->nb_frames < 0)
        return CB_ERROR(EINVAL);
    float *x = malloc((size_t)CB_MAX(in->nb_frames, (int64_t)1) * sizeof(float));
    if (!x)
        return CB_ERROR(ENOMEM);
    int ch = in->channels;
    for (int64_t i = 0; i < in->nb_frames; i++) {
        float sum = 0.0f;
        for (int c = 0; c < ch; c++)
            sum += in->samples[i * ch + c];
        x[i] = ch == 1 ? sum : sum / (float)ch;
    }
    *out = (CBAudio) {
        .samples   = x,
        .channels  = 1,
        .rate      = in->rate,
        .nb_frames = in->nb_frames,
    };
    return 0;
}

int cb_audio_resample(const CBAudio *in, int rate, CBAudio *out)
{
    cb_audio_init_logging();
    *out = (CBAudio) { 0 };
    if (in->channels <= 0 || in->rate <= 0 || rate <= 0 || in->nb_frames < 0)
        return CB_ERROR(EINVAL);
    if (!in->nb_frames) {
        *out = (CBAudio) { .channels = in->channels, .rate = rate };
        return 0;
    }
    SwrContext *swr;
    int ret = resampler_open(&swr, in->channels, in->rate, rate);
    if (ret < 0)
        return ret;
    int64_t cap = av_rescale_rnd(in->nb_frames, rate, in->rate, AV_ROUND_UP) + 4096;
    size_t bytes;
    float *buf = NULL;
    if (cap > INT_MAX || ckd_mul(&bytes, (size_t)cap, (size_t)in->channels * sizeof(float)) ||
        !(buf = malloc(bytes))) {
        swr_free(&swr);
        return CB_ERROR(ENOMEM);
    }
    int64_t got = 0;
    for (int64_t at = 0; at < in->nb_frames && ret >= 0;) {
        int n = (int)CB_MIN(in->nb_frames - at, (int64_t)1 << 20);
        const uint8_t *src[1] = { (const uint8_t *)(in->samples + at * in->channels) };
        uint8_t *dst[1] = { (uint8_t *)(buf + got * in->channels) };
        ret = swr_convert(swr, dst, (int)(cap - got), src, n);
        got += CB_MAX(ret, 0);
        at += n;
    }
    while (ret >= 0) {
        uint8_t *dst[1] = { (uint8_t *)(buf + got * in->channels) };
        if ((ret = swr_convert(swr, dst, (int)(cap - got), NULL, 0)) <= 0)
            break;
        got += ret;
    }
    swr_free(&swr);
    if (ret < 0) {
        free(buf);
        return cb_audio_error(ret);
    }
    *out = (CBAudio) { .samples = buf, .channels = in->channels, .rate = rate, .nb_frames = got };
    return 0;
}

void cb_audio_free(CBAudio *a)
{
    free(a->samples);
    *a = (CBAudio) { 0 };
}
