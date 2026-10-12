/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Decoding a whole file to interleaved float in memory, on the players'
 * timeline (see rxbox/audio.h).
 *
 * MP3 (in an MP3 file) does not go through libavformat: the frames
 * rx_mp3_scan counts are handed to libavcodec's decoder one by one, so the
 * decoded length is the scanner's total_samples by construction. In front of
 * them goes one frame of silence when rekordbox counts the Xing/Info/VBRI
 * frame, and frames before a restart of rekordbox's frame map are dropped.
 * Nothing is trimmed: dj ran ffmpeg with -flags2 +skip_manual for the same
 * reason (docs/anlz.md: an ANLZ detail waveform has frames * 1152 * 150 /
 * rate columns, so LAME's delay and padding stay in). tests/audio/test_dj.c
 * checks the result is dj's decode_player output bit for bit, after the
 * silent frame when there is one.
 *
 * Everything else goes through libavformat, from sample 0 of the first
 * audio stream. MP3 in another container still gets skip_manual. AAC keeps
 * ffmpeg's handling of the priming samples (skip_samples side data); what
 * the players do there is unknown. Opus and Vorbis decode natively.
 *
 * A packet that fails to decode is played as silence of its length (one
 * MPEG frame for MP3), not dropped, so everything after it stays in place:
 * rekordbox does so, and rbxport found the one MP3 in 200 whose waveform did
 * not line up was a decoder dropping two damaged packets.
 *
 * Samples are converted to float by hand, exactly as libswresample does
 * (s16 / 32768, s32 / 2^31); the mono mix is the mean of the channels,
 * summed then divided, as dj's numpy mean; stereo from mono duplicates it,
 * stereo from more channels uses libswresample's downmix matrix normalised
 * to unity. Resampling is libswresample with soxr when it was built with it,
 * else its own Kaiser-windowed filter. The output buffer starts at the
 * container's length estimate and grows by half when that was short. */

#include "rxbox/audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdckdint.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "internal.h"

/* Silence is pushed through the sink in chunks of this many frames. */
#define SILENCE_CHUNK 4096
/* Largest Layer III frame: 320 kbit/s at 32 kHz, or 160 kbit/s at 8 kHz,
 * 1441 bytes with padding. */
#define MP3_MAX_FRAME 2048

enum MixKind {
    MIX_COPY,
    MIX_MEAN,
    MIX_MATRIX,
};

/* Where decoded frames go: conversion to float, channel mix, the timeline's
 * drop, resampling, and the output buffer. */
typedef struct Sink {
    const char *path;
    int in_rate;
    int out_ch, out_rate;
    SwrContext *swr;
    bool rate_warned;

    int mix_in;             /* input channels the mixer is set up for, 0 for none */
    enum MixKind mix_kind;
    float *matrix;          /* out_ch x mix_in */

    float *planar;          /* converted input, channel after channel */
    size_t planar_cap;
    float *mixed;           /* mixed, interleaved */
    size_t mixed_cap;
    float *zeros;           /* SILENCE_CHUNK * out_ch */

    int64_t drop;           /* native-rate frames still to drop */
    int64_t last_frames;    /* size of the last decoded frame */

    float *buf;
    int64_t nb, cap;        /* frames */
} Sink;

/* --------------------------------------------------------------- resampler */

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
        return rx_audio_error(ret);

    /* Asking for soxr in a build without it fails at init with an error in
     * the log, so look at the configuration instead. */
    if (strstr(swresample_configuration(), "--enable-libsoxr"))
        av_opt_set_int(swr, "resampler", SWR_ENGINE_SOXR, 0);
    else
        av_opt_set_int(swr, "filter_type", SWR_FILTER_TYPE_KAISER, 0);

    ret = swr_init(swr);
    if (ret < 0) {
        swr_free(&swr);
        return rx_audio_error(ret);
    }
    *out = swr;
    return 0;
}

/* -------------------------------------------------------------------- sink */

static int grow(void **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    void *nb = realloc(*buf, need);
    if (!nb)
        return RX_ERROR(ENOMEM);
    *buf = nb;
    *cap = need;
    return 0;
}

static int reserve(Sink *s, int64_t frames)
{
    int64_t need;
    if (ckd_add(&need, s->nb, frames))
        return RX_ERROR(ENOMEM);
    if (need <= s->cap)
        return 0;
    int64_t cap = RX_MAX(need, s->cap + s->cap / 2);
    size_t bytes;
    if (cap > PTRDIFF_MAX || ckd_mul(&bytes, (size_t)cap, (size_t)s->out_ch * sizeof(float)))
        return RX_ERROR(ENOMEM);
    float *nb = realloc(s->buf, bytes);
    if (!nb)
        return RX_ERROR(ENOMEM);
    s->buf = nb;
    s->cap = cap;
    return 0;
}

static int sink_init(Sink *s, const char *path, int in_ch, int in_rate, int channels, int rate,
                     double est_seconds)
{
    *s = (Sink) { 0 };
    if (in_ch <= 0 || in_rate <= 0 || channels < 0 || channels > 2 || rate < 0)
        return RX_ERROR(EINVAL);

    *s = (Sink) {
        .path     = path,
        .in_rate  = in_rate,
        .out_ch   = channels ? channels : in_ch,
        .out_rate = rate ? rate : in_rate,
    };
    s->zeros = calloc((size_t)SILENCE_CHUNK * s->out_ch, sizeof(float));
    if (!s->zeros)
        return RX_ERROR(ENOMEM);
    if (s->out_rate != in_rate) {
        int ret = resampler_open(&s->swr, s->out_ch, in_rate, s->out_rate);
        if (ret < 0)
            return ret;
    }

    /* The container's estimate plus a little, at most an hour up front */
    double est = RX_CLAMP(est_seconds, 0.0, 3600.0) * 1.01 * s->out_rate;
    return reserve(s, (int64_t)est + 65536);
}

static void sink_free(Sink *s)
{
    swr_free(&s->swr);
    free(s->matrix);
    free(s->planar);
    free(s->mixed);
    free(s->zeros);
    free(s->buf);
    *s = (Sink) { 0 };
}

/* Takes n interleaved frames at out_ch channels and the native rate. */
static int sink_push(Sink *s, const float *x, int64_t n)
{
    if (s->drop > 0) {
        int64_t d = RX_MIN(s->drop, n);
        s->drop -= d;
        x += d * s->out_ch;
        n -= d;
    }
    if (n <= 0)
        return 0;

    if (!s->swr) {
        int ret = reserve(s, n);
        if (ret < 0)
            return ret;
        memcpy(s->buf + s->nb * s->out_ch, x, (size_t)n * s->out_ch * sizeof(float));
        s->nb += n;
        return 0;
    }

    while (n > 0) {
        int chunk = (int)RX_MIN(n, (int64_t)(1 << 20));
        int max = swr_get_out_samples(s->swr, chunk);
        if (max < 0)
            return rx_audio_error(max);
        int ret = reserve(s, max);
        if (ret < 0)
            return ret;
        uint8_t *dst[1] = { (uint8_t *)(s->buf + s->nb * s->out_ch) };
        const uint8_t *src[1] = { (const uint8_t *)x };
        int got = swr_convert(s->swr, dst, max, src, chunk);
        if (got < 0)
            return rx_audio_error(got);
        s->nb += got;
        x += (int64_t)chunk * s->out_ch;
        n -= chunk;
    }
    return 0;
}

static int sink_silence(Sink *s, int64_t n)
{
    while (n > 0) {
        int64_t chunk = RX_MIN(n, (int64_t)SILENCE_CHUNK);
        int ret = sink_push(s, s->zeros, chunk);
        if (ret < 0)
            return ret;
        n -= chunk;
    }
    return 0;
}

static int mixer_setup(Sink *s, const AVChannelLayout *layout)
{
    int in = layout->nb_channels;
    if (in == s->mix_in)
        return 0;
    free(s->matrix);
    s->matrix = NULL;
    s->mix_in = in;

    if (in == s->out_ch) {
        s->mix_kind = MIX_COPY;
        return 0;
    }
    if (s->out_ch == 1) {
        s->mix_kind = MIX_MEAN;
        return 0;
    }

    s->mix_kind = MIX_MATRIX;
    s->matrix = calloc((size_t)s->out_ch * in, sizeof(float));
    if (!s->matrix)
        return RX_ERROR(ENOMEM);

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
            return RX_ERROR(ENOMEM);
        double *m = calloc((size_t)2 * in, sizeof(*m));
        if (!m) {
            av_channel_layout_uninit(&in_l);
            return RX_ERROR(ENOMEM);
        }
        int ret = swr_build_matrix2(&in_l, &out_l, M_SQRT1_2, M_SQRT1_2, 0.0, 1.0, 1.0, m, in,
                                    AV_MATRIX_ENCODING_NONE, NULL);
        av_channel_layout_uninit(&in_l);
        if (ret >= 0) {
            for (int i = 0; i < 2 * in; i++)
                s->matrix[i] = (float)m[i];
            free(m);
            return 0;
        }
        free(m);
    }

    /* A channel count that changed mid-stream, or a layout with no downmix:
     * the first channels as they are. */
    for (int o = 0; o < RX_MIN(in, s->out_ch); o++)
        s->matrix[o * in + o] = 1.0f;
    return 0;
}

/* One channel of a frame as float, the way libswresample converts. */
static void to_float(const AVFrame *f, int c, float *dst)
{
    int n = f->nb_samples, ch = f->ch_layout.nb_channels;
    bool planar = av_sample_fmt_is_planar(f->format);
    const uint8_t *base = planar ? f->extended_data[c] : f->extended_data[0];
    size_t step = planar ? 1 : (size_t)ch;
    size_t off = planar ? 0 : (size_t)c;

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
        for (int i = 0; i < n; i++)
            dst[i] = x[i * step + off];
        break;
    }
    case AV_SAMPLE_FMT_DBL: {
        const double *x = (const double *)base;
        for (int i = 0; i < n; i++)
            dst[i] = (float)x[i * step + off];
        break;
    }
    default:
        memset(dst, 0, (size_t)n * sizeof(float));
        break;
    }
}

static int sink_frame(Sink *s, const AVFrame *f)
{
    int n = f->nb_samples, in = f->ch_layout.nb_channels;
    if (n <= 0 || in <= 0)
        return 0;
    if (f->sample_rate != s->in_rate && !s->rate_warned) {
        rx_warn("%s: sample rate changes from %d to %d Hz; kept as %d Hz", s->path,
                s->in_rate, f->sample_rate, s->in_rate);
        s->rate_warned = true;
    }
    s->last_frames = n;

    int ret = mixer_setup(s, &f->ch_layout);
    if (ret < 0)
        return ret;
    ret = grow((void **)&s->planar, &s->planar_cap, (size_t)n * in * sizeof(float));
    if (ret >= 0)
        ret = grow((void **)&s->mixed, &s->mixed_cap, (size_t)n * s->out_ch * sizeof(float));
    if (ret < 0)
        return ret;

    for (int c = 0; c < in; c++)
        to_float(f, c, s->planar + (size_t)c * n);

    const float *p = s->planar;
    float *m = s->mixed;
    int oc = s->out_ch;
    switch (s->mix_kind) {
    case MIX_COPY:
        for (int i = 0; i < n; i++)
            for (int c = 0; c < in; c++)
                m[(size_t)i * oc + c] = p[(size_t)c * n + i];
        break;
    case MIX_MEAN:
        for (int i = 0; i < n; i++) {
            float sum = 0.0f;
            for (int c = 0; c < in; c++)
                sum += p[(size_t)c * n + i];
            m[i] = sum / (float)in;
        }
        break;
    case MIX_MATRIX:
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
    return sink_push(s, m, n);
}

static int sink_finish(Sink *s, RXAudio *out)
{
    if (s->swr) {
        while (1) {
            int max = swr_get_out_samples(s->swr, 0);
            if (max <= 0)
                break;
            int ret = reserve(s, max);
            if (ret < 0)
                return ret;
            uint8_t *dst[1] = { (uint8_t *)(s->buf + s->nb * s->out_ch) };
            int got = swr_convert(s->swr, dst, max, NULL, 0);
            if (got < 0)
                return rx_audio_error(got);
            if (!got)
                break;
            s->nb += got;
        }
    }
    if (!s->nb) {
        rx_debug("%s: no audio decoded", s->path);
        return RX_ERROR(ENODATA);
    }

    float *shrunk = realloc(s->buf, (size_t)s->nb * s->out_ch * sizeof(float));
    if (shrunk)
        s->buf = shrunk;
    *out = (RXAudio) {
        .samples   = s->buf,
        .channels  = s->out_ch,
        .rate      = s->out_rate,
        .nb_frames = s->nb,
    };
    s->buf = NULL;
    return 0;
}

/* ------------------------------------------------------------------ decoder */

static int open_decoder(AVCodecContext **out, const AVCodecParameters *par, AVRational tb,
                        bool skip_manual)
{
    *out = NULL;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        return RX_ERROR(ENOTSUP);
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec)
        return RX_ERROR(ENOMEM);
    int ret = avcodec_parameters_to_context(dec, par);
    if (ret < 0)
        goto fail;
    dec->pkt_timebase = tb;
    if (skip_manual)
        dec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0)
        goto fail;
    *out = dec;
    return 0;

fail:
    avcodec_free_context(&dec);
    return rx_audio_error(ret);
}

/* Sends one packet (NULL to flush) and takes every frame it gives. A packet
 * that does not decode becomes `silence` frames of silence, less what it
 * did give: a decoder can return the good part of a packet and then fail
 * on the rest (a PCM packet cut mid-frame at the end of a truncated WAV),
 * and only what is missing is silence. */
static int decode_packet(AVCodecContext *dec, Sink *s, AVFrame *frame, const AVPacket *pkt,
                         int64_t silence)
{
    int ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && ret != AVERROR_EOF) {
        if (ret == AVERROR(ENOMEM))
            return RX_ERROR(ENOMEM);
        rx_debug("%s: damaged packet (%s), %" PRId64 " samples of silence", s->path,
                 av_err2str(ret), silence);
        return sink_silence(s, silence);
    }

    int64_t got = 0;
    while (1) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            if (ret == AVERROR(ENOMEM))
                return RX_ERROR(ENOMEM);
            int64_t missing = RX_MAX(silence - got, (int64_t)0);
            rx_debug("%s: damaged frame (%s), %" PRId64 " samples of silence", s->path,
                     av_err2str(ret), missing);
            return pkt ? sink_silence(s, missing) : 0;
        }
        got += frame->nb_samples;
        ret = sink_frame(s, frame);
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }
}

/* --------------------------------------------------------------------- MP3 */

typedef struct Mp3Ctx {
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    Sink *sink;
    uint8_t buf[MP3_MAX_FRAME + AV_INPUT_BUFFER_PADDING_SIZE];
} Mp3Ctx;

static int mp3_frame(void *opaque, const uint8_t *data, size_t size, int samples, int kbps)
{
    Mp3Ctx *c = opaque;
    size = RX_MIN(size, (size_t)MP3_MAX_FRAME);
    memcpy(c->buf, data, size);
    memset(c->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    c->pkt->data = c->buf;
    c->pkt->size = (int)size;
    return decode_packet(c->dec, c->sink, c->frame, c->pkt, samples);
}

/* 1 when this is not an MP3 file the scanner reads (the caller then uses
 * libavformat), 0 when decoded, < 0 on error. */
static int decode_mp3(const char *path, int channels, int rate, RXAudio *out)
{
    RXMap map;
    if (rx_audio_map(path, &map) < 0)
        return 1;
    RXMp3Info info;
    if (rx_mp3_scan_mapped(path, &map, &info) < 0 || !info.nb_frames) {
        rx_audio_unmap(&map);
        return 1;
    }

    Sink sink = { 0 };
    Mp3Ctx *c = calloc(1, sizeof(*c));
    AVCodecParameters *par = avcodec_parameters_alloc();
    int ret = RX_ERROR(ENOMEM);
    if (!c || !par)
        goto end;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->codec_id = AV_CODEC_ID_MP3;
    par->sample_rate = info.sample_rate;
    av_channel_layout_default(&par->ch_layout, info.channels);
    ret = open_decoder(&c->dec, par, (AVRational) { 1, info.sample_rate }, true);
    if (ret < 0)
        goto end;
    c->pkt = av_packet_alloc();
    c->frame = av_frame_alloc();
    if (!c->pkt || !c->frame) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }

    int64_t frames = info.nb_frames + info.header_counts;
    ret = sink_init(&sink, path, info.channels, info.sample_rate, channels, rate,
                    (double)frames * info.frame_samples / info.sample_rate);
    if (ret < 0)
        goto end;
    c->sink = &sink;
    sink.drop = (int64_t)info.skip_frames * info.frame_samples;
    if (info.header_counts) {
        ret = sink_silence(&sink, info.frame_samples);
        if (ret < 0)
            goto end;
    }

    ret = rx_mp3_frames(path, &map, &info, mp3_frame, c);
    if (ret >= 0)
        ret = decode_packet(c->dec, &sink, c->frame, NULL, 0);
    if (ret >= 0)
        ret = sink_finish(&sink, out);

end:
    if (c) {
        avcodec_free_context(&c->dec);
        av_packet_free(&c->pkt);
        av_frame_free(&c->frame);
        free(c);
    }
    avcodec_parameters_free(&par);
    sink_free(&sink);
    rx_audio_unmap(&map);
    return ret;
}

/* ---------------------------------------------------------- everything else */

/* The silence that stands for a packet that failed to decode. */
static int64_t packet_samples(const AVCodecContext *dec, const AVStream *st, const Sink *s,
                              const AVPacket *pkt)
{
    if (pkt->duration > 0)
        return av_rescale_q(pkt->duration, st->time_base, (AVRational) { 1, s->in_rate });
    if (dec->frame_size > 0)
        return dec->frame_size;
    return s->last_frames;
}

static int decode_av(const char *path, int channels, int rate, RXAudio *out)
{
    AVFormatContext *fmt;
    int ret = rx_audio_open(path, &fmt, true);
    if (ret < 0)
        return ret;

    AVCodecContext *dec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    Sink sink = { 0 };
    if (!pkt || !frame) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }

    int idx = rx_audio_first_stream(fmt, AVMEDIA_TYPE_AUDIO);
    if (idx < 0) {
        rx_debug("%s: no audio stream", path);
        ret = RX_ERROR(ENODATA);
        goto end;
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        fmt->streams[i]->discard = (int)i == idx ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    AVStream *st = fmt->streams[idx];

    bool mp3 = st->codecpar->codec_id == AV_CODEC_ID_MP3;
    ret = open_decoder(&dec, st->codecpar, st->time_base, mp3);
    if (ret < 0)
        goto end;

    double est = 0.0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        est = (double)st->duration * av_q2d(st->time_base);
    else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        est = (double)fmt->duration / AV_TIME_BASE;
    ret = sink_init(&sink, path, dec->ch_layout.nb_channels, dec->sample_rate, channels, rate, est);
    if (ret < 0)
        goto end;

    while (1) {
        ret = av_read_frame(fmt, pkt);
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EIO)) {
                rx_err("%s: %s", path, av_err2str(ret));
                ret = rx_audio_error(ret);
                goto end;
            }
            rx_warn("%s: demuxing stopped: %s", path, av_err2str(ret));
            break;
        }
        if (pkt->stream_index != idx) {
            av_packet_unref(pkt);
            continue;
        }
        ret = decode_packet(dec, &sink, frame, pkt, packet_samples(dec, st, &sink, pkt));
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    ret = decode_packet(dec, &sink, frame, NULL, 0);
    if (ret >= 0)
        ret = sink_finish(&sink, out);

end:
    sink_free(&sink);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return ret;
}

int rx_decode(const char *path, int channels, int rate, RXAudio *out)
{
    rx_audio_init_logging();
    *out = (RXAudio) { 0 };
    if (channels < 0 || channels > 2 || rate < 0)
        return RX_ERROR(EINVAL);

    /* Only a file libavformat calls MP3 gets the frame walk: an MP3 stream
     * in WAV or MP4 has no frame map of its own. */
    AVFormatContext *fmt;
    int ret = rx_audio_open(path, &fmt, false);
    if (ret < 0)
        return ret;
    bool mp3 = rx_audio_is_container(fmt, "mp3");
    avformat_close_input(&fmt);

    if (mp3) {
        ret = decode_mp3(path, channels, rate, out);
        if (ret <= 0)
            return ret;
    }
    return decode_av(path, channels, rate, out);
}

/* ---------------------------------------------------------- derived buffers */

int rx_audio_to_mono(const RXAudio *in, RXAudio *out)
{
    *out = (RXAudio) { 0 };
    if (in->channels <= 0 || in->nb_frames < 0)
        return RX_ERROR(EINVAL);
    float *x = malloc((size_t)RX_MAX(in->nb_frames, (int64_t)1) * sizeof(float));
    if (!x)
        return RX_ERROR(ENOMEM);
    int ch = in->channels;
    for (int64_t i = 0; i < in->nb_frames; i++) {
        float sum = 0.0f;
        for (int c = 0; c < ch; c++)
            sum += in->samples[i * ch + c];
        x[i] = ch == 1 ? sum : sum / (float)ch;
    }
    *out = (RXAudio) {
        .samples   = x,
        .channels  = 1,
        .rate      = in->rate,
        .nb_frames = in->nb_frames,
    };
    return 0;
}

int rx_audio_resample(const RXAudio *in, int rate, RXAudio *out)
{
    rx_audio_init_logging();
    *out = (RXAudio) { 0 };
    if (in->channels <= 0 || in->rate <= 0 || rate <= 0 || in->nb_frames < 0)
        return RX_ERROR(EINVAL);
    if (!in->nb_frames) {
        *out = (RXAudio) { .channels = in->channels, .rate = rate };
        return 0;
    }

    Sink s = { 0 };
    int ret = sink_init(&s, "(buffer)", in->channels, in->rate, 0, rate,
                        (double)in->nb_frames / in->rate);
    if (ret >= 0)
        ret = sink_push(&s, in->samples, in->nb_frames);
    if (ret >= 0)
        ret = sink_finish(&s, out);
    sink_free(&s);
    return ret;
}

void rx_audio_free(RXAudio *a)
{
    free(a->samples);
    *a = (RXAudio) { 0 };
}
