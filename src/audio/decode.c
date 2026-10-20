/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Decoding a whole file to interleaved float in memory, on the players'
 * timeline (see cyanbox/audio.h): a source (source.c), FFmpeg's path,
 * read straight into the buffer, which starts at the container's length
 * and grows by half when that was short. What a stick, Link or a deck
 * gets is the same source's output, sample for sample (test_pipeline).
 *
 * The rest: buffers made from decoded ones (mono, resampled), and the
 * decoder set-up sources use. */

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

/* ------------------------------------------------------------------ decoder */

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
