/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A track decoded as it is read (internal.h, CBSource): what a pipeline
 * (pipeline.c) runs in its thread. FFmpeg's path, nothing else:
 * libavformat's packets go to libavcodec, its frames to libswresample,
 * which converts them (sample format, channels, rate, dither) and writes
 * them where the caller reads them from, keeping what does not fit for the
 * next read: libswresample is the FIFO.
 *
 * The players' timeline (cyanbox/audio.h): a frame goes where its pts
 * says, and libavcodec moves the pts of a frame it drops samples from (an
 * Opus stream's pre-skip, AAC's priming) by as much. For an MP3 file that
 * is the packets' pts, from 0 (libavformat's mp3 demuxer gives exactly the
 * frames cb_mp3_scan counts), plus one frame of silence when rekordbox
 * counts the header frame, less the frames before a restart of rekordbox's
 * frame map; MP3 keeps its encoder delay and padding (skip_manual). A gap
 * between frames (a packet that did not decode) is silence; whatever lies
 * before the place a read starts at is cut.
 *
 * A seek restarts:
 *
 * - libswresample, at an input frame that is a whole number of periods of
 *   the rate ratio (96 kHz to 44.1: 320 in for 147 out) and
 *   RESTART_MARGIN output frames or more ahead of the target, whose output
 *   up to the target it drops (swr_drop_output). Its output is
 *   time-aligned with its input, and from an aligned start it is a
 *   continuous run's sample for sample once its filter has filled: 15 to
 *   29 output frames in, measured from 22.05 to 96 kHz. Dither noise starts
 *   again, so 16-bit output that is dithered differs by the noise.
 * - libavcodec, flushed (never opened again), fed from `preroll` input
 *   frames before that (as measured for the deck's old packet store: none
 *   for codecs that code every packet alone, MP3's bit reservoir, two
 *   Vorbis packets, three AAC ones, 640 ms of Opus), what it gives before
 *   the restart point cut. A
 *   skip still pending in it after the flush moves its frames' pts too.
 *   The AAC decoder's flush keeps the last frame's window state, so the
 *   first frame after a seek to the very start, where there is no
 *   pre-roll, differs a little from a fresh decoder's (0.0025 on ADTS;
 *   MP4's priming packet, which it drops, hides it).
 * - libavformat: av_seek_frame, whose timestamps after a seek were exact on
 *   every packet tried (MP3 through its generic index, FLAC, MP4 AAC and
 *   ALAC, ADTS, Opus in Ogg; WAV and AIFF seek to the sample). Vorbis in
 *   Ogg is the exception: after a seek libavformat's timestamps were off by
 *   up to a second early in a file, against a decode from the start. Its
 *   packets are only ever read in order from the open: a seek ahead reads
 *   on without decoding, one back opens the file again. So does a seek to
 *   the start, for every format. */

#include "internal.h"

#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

/* Output frames a restart starts ahead of the target, at least (above). */
#define RESTART_MARGIN 128
/* Frames of silence fed at a time. */
#define ZEROS 4096
/* Channels the decoder may have. */
#define MAX_CHANNELS 64
#define OPUS_PREROLL_MS 640

struct CBSource {
    char *path;
    AVFormatContext *fmt;
    int idx;
    AVRational tb;              /* the stream's */
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    SwrContext *swr;
    int64_t drop;               /* output frames the next libswresample drops */

    /* What libswresample is set up for (the decoder's samples) */
    int in_rate, in_ch;
    enum AVSampleFormat in_fmt;
    AVChannelLayout in_layout;
    bool rate_warned;

    int out_rate, out_ch;
    enum AVSampleFormat out_fmt;
    int out_bps;
    bool out_planar;
    int64_t p_in, p_out;        /* the rate ratio, reduced */
    int64_t preroll;

    int64_t shift;              /* timeline frame = pts in frames + shift */
    bool walk;                  /* Vorbis: packets read in order only */
    int64_t length;             /* the container's, input frames; -1 none */

    int64_t pos;                /* output frame the next read gives */
    int64_t next_in;            /* timeline frame of the next input frame given */
    int64_t discard_to;         /* packets ending before it are not decoded */
    int64_t demux_at;           /* timeline frame after the last packet read */
    int64_t expect;             /* where a frame without pts goes */
    bool held;                  /* `frame` holds samples not given yet */
    int64_t held_at;
    bool demux_eof, sent_eof, flushed;
    uint8_t *zeros;             /* ZEROS frames of silence in the decoder's format */
    int zeros_fmt;
};

/* ------------------------------------------------------------------ helpers */

static enum AVSampleFormat av_format(enum CBSampleFormat f)
{
    switch (f) {
    case CB_SAMPLE_FLTP: return AV_SAMPLE_FMT_FLTP;
    case CB_SAMPLE_S16:  return AV_SAMPLE_FMT_S16;
    case CB_SAMPLE_S16P: return AV_SAMPLE_FMT_S16P;
    default:             return AV_SAMPLE_FMT_FLT;
    }
}

static int64_t preroll_of(const AVCodecParameters *par, int rate)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(par->codec_id);
    if (d && (d->props & AV_CODEC_PROP_INTRA_ONLY) && (d->props & AV_CODEC_PROP_LOSSLESS))
        return 0;
    switch (par->codec_id) {
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_MP2:
        /* the reservoir reaches 511 bytes back: 15 frames at 32 kbit/s */
        return 16 * 1152;
    case AV_CODEC_ID_OPUS:
        return (int64_t)rate * OPUS_PREROLL_MS / 1000;
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_AAC:
        /* two Vorbis packets of the longest blocks, three HE-AAC ones */
        return 8192;
    default:
        return rate / 10 + 8192;
    }
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

/* Timeline frames of a pts (stream time base). */
static int64_t at_of(const CBSource *s, int64_t pts)
{
    return av_rescale_q(pts, s->tb, (AVRational) { 1, s->in_rate }) + s->shift;
}

/* ------------------------------------------------------------- the output */

/* The channel mix (decode.c's): mono from stereo as the mean, stereo from
 * mono doubled, more channels through libswresample's stereo downmix
 * (centre and surround at -3 dB, no LFE, normalised to unity; the first
 * two as they are for a layout it has none for), and mono from those as
 * the mean of that mix. */
static int set_matrix(CBSource *s)
{
    int in = s->in_ch, out = s->out_ch;
    double m[2 * MAX_CHANNELS] = { 0 };
    if (out == 1 && in == 2) {
        m[0] = m[1] = 0.5;
    } else if (out == 2 && in == 1) {
        m[0] = m[1] = 1.0;
    } else {
        /* swr_build_matrix2 scales all 64 x 64 entries at the stride it
         * is given (rematrix.c), whatever the layouts */
        double *st = calloc((size_t)MAX_CHANNELS * MAX_CHANNELS, sizeof(*st));
        if (!st)
            return CB_ERROR(ENOMEM);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_build_matrix2(&s->in_layout, &stereo, M_SQRT1_2, M_SQRT1_2, 0.0, 1.0, 1.0, st,
                              MAX_CHANNELS, AV_MATRIX_ENCODING_NONE, NULL) < 0) {
            memset(st, 0, (size_t)MAX_CHANNELS * MAX_CHANNELS * sizeof(*st));
            st[0] = 1.0;
            st[MAX_CHANNELS + 1] = 1.0;
        }
        for (int c = 0; c < in; c++) {
            if (out == 2) {
                m[c] = st[c];
                m[in + c] = st[MAX_CHANNELS + c];
            } else {
                m[c] = 0.5 * (st[c] + st[MAX_CHANNELS + c]);
            }
        }
        free(st);
    }
    return swr_set_matrix(s->swr, m, in);
}

/* libswresample for the decoder's frame f, dropping s->drop output frames
 * first. Its internal format is float when it mixes or resamples (it
 * would work in 16 bits for 16 bits in and out, and not dither); 16-bit
 * output is dithered (triangular), which it does only where precision is
 * lost, so 16 bits in come out as they were. */
static int open_swr(CBSource *s, const AVFrame *f)
{
    swr_free(&s->swr);
    av_channel_layout_uninit(&s->in_layout);
    int ret;
    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels <= 0)
        av_channel_layout_default(&s->in_layout, f->ch_layout.nb_channels);
    else if ((ret = av_channel_layout_copy(&s->in_layout, &f->ch_layout)) < 0)
        return cb_audio_error(ret);
    s->in_fmt = f->format;
    if (s->in_layout.nb_channels != s->in_ch) {
        cb_warn("%s: %d channels from here, not %d", s->path, s->in_layout.nb_channels, s->in_ch);
        s->in_ch = s->in_layout.nb_channels;
    }
    if (s->in_ch <= 0 || s->in_ch > MAX_CHANNELS)
        return CB_ERROR(ENOTSUP);

    AVChannelLayout out;
    av_channel_layout_default(&out, s->out_ch);
    ret = swr_alloc_set_opts2(&s->swr, &out, s->out_fmt, s->out_rate, &s->in_layout, s->in_fmt,
                              s->in_rate, 0, NULL);
    av_channel_layout_uninit(&out);
    if (ret < 0)
        return cb_audio_error(ret);
    bool mix = s->in_ch != s->out_ch, resample = s->in_rate != s->out_rate;
    if (av_get_packed_sample_fmt(s->out_fmt) == AV_SAMPLE_FMT_S16)
        av_opt_set_int(s->swr, "dither_method", SWR_DITHER_TRIANGULAR, 0);
    if (mix || resample)
        av_opt_set_sample_fmt(s->swr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
    if (mix && (ret = set_matrix(s)) < 0)
        return cb_audio_error(ret);
    if ((ret = swr_init(s->swr)) < 0)
        return cb_audio_error(ret);
    if (s->drop > 0 && (ret = swr_drop_output(s->swr, (int)s->drop)) < 0)
        return cb_audio_error(ret);
    s->drop = 0;

    /* Silence in the decoder's format: one buffer serves every plane */
    if (!s->zeros || s->zeros_fmt != s->in_fmt) {
        free(s->zeros);
        int bps = av_get_bytes_per_sample(s->in_fmt);
        size_t size = (size_t)ZEROS * bps * (av_sample_fmt_is_planar(s->in_fmt) ? 1 : s->in_ch);
        if (!(s->zeros = malloc(size)))
            return CB_ERROR(ENOMEM);
        memset(s->zeros, av_get_packed_sample_fmt(s->in_fmt) == AV_SAMPLE_FMT_U8 ? 0x80 : 0, size);
        s->zeros_fmt = s->in_fmt;
    }
    return 0;
}

/* The output's planes, `got` frames in. */
static void out_planes(const CBSource *s, uint8_t *const *dst, int got, uint8_t **o)
{
    for (int c = 0; c < (s->out_planar ? s->out_ch : 1); c++)
        o[c] = dst[c] + (size_t)got * s->out_bps * (s->out_planar ? 1 : s->out_ch);
}

/* Converts `count` input frames from `in` (NULL: none, what libswresample
 * holds; flushed: its tail) into dst from `got` on. */
static int convert(CBSource *s, uint8_t *const *dst, int got, int n, const uint8_t **in, int count)
{
    if (!s->swr || got >= n)
        return 0;
    uint8_t *o[2];
    out_planes(s, dst, got, o);
    const uint8_t *none[1] = { s->zeros };
    int ret = swr_convert(s->swr, o, n - got, in ? in : s->flushed ? NULL : none, in ? count : 0);
    return ret < 0 ? cb_audio_error(ret) : ret;
}

/* --------------------------------------------------------------- the input */

static int open_input(CBSource *s)
{
    /* A 16-bit PCM WAV opened again (a seek to the start). libavformat
     * holds back such a file's first 32 packets to look for DTS or AC-3 in
     * them (wavdec.c, handle_stream_probing; every format's probe at each
     * power of two: 18 ms of a 48 kHz stereo file, which every seek to 0
     * paid). The first open looked at these bytes and found PCM, its first
     * look at the first packet alone, so probing ends there now with the
     * same answer, and the parameters the header holds are not looked for
     * again. Anything else opens as it first did: an ADTS stream's
     * timestamps depend on what avformat_find_stream_info finds. */
    bool wav16 = s->fmt && s->dec && s->dec->codec_id == AV_CODEC_ID_PCM_S16LE &&
                 cb_audio_is_container(s->fmt, "wav");
    cb_audio_close(&s->fmt);
    int ret = cb_audio_open(s->path, &s->fmt, !wav16);
    if (ret < 0)
        return ret;
    if (wav16)
        s->fmt->probesize = 32;
    s->idx = cb_audio_first_stream(s->fmt, AVMEDIA_TYPE_AUDIO);
    if (s->idx < 0) {
        cb_debug("%s: no audio stream", s->path);
        return CB_ERROR(ENODATA);
    }
    for (unsigned i = 0; i < s->fmt->nb_streams; i++)
        s->fmt->streams[i]->discard = (int)i == s->idx ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    s->tb = s->fmt->streams[s->idx]->time_base;
    s->demux_at = INT64_MIN;
    return 0;
}

/* Sends the next packet to the decoder, or marks the end. */
static int read_packet(CBSource *s)
{
    while (1) {
        int ret = av_read_frame(s->fmt, s->pkt);
        if (ret == AVERROR_EOF) {
            s->demux_eof = true;
            return 0;
        }
        if (ret < 0) {
            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EIO)) {
                cb_err("%s: %s", s->path, av_err2str(ret));
                return cb_audio_error(ret);
            }
            cb_warn("%s: demuxing stopped: %s", s->path, av_err2str(ret));
            s->demux_eof = true;
            return 0;
        }
        AVPacket *p = s->pkt;
        if (p->stream_index != s->idx) {
            av_packet_unref(p);
            continue;
        }
        if (p->pts != AV_NOPTS_VALUE) {
            int64_t at = at_of(s, p->pts);
            int64_t end = at + (p->duration > 0 ? av_rescale_q(p->duration, s->tb,
                                                               (AVRational) { 1, s->in_rate }) : 0);
            s->demux_at = end;
            if (p->duration > 0 && end <= s->discard_to) {
                av_packet_unref(p);
                continue;
            }
        }
        ret = avcodec_send_packet(s->dec, p);
        av_packet_unref(p);
        /* A packet that does not decode leaves a gap, which is silence */
        return ret == AVERROR(ENOMEM) ? CB_ERROR(ENOMEM) : 0;
    }
}

/* 1 with a frame held, 0 at the end, < 0 on an error. */
static int next_frame(CBSource *s)
{
    while (1) {
        int ret = avcodec_receive_frame(s->dec, s->frame);
        if (ret >= 0) {
            AVFrame *f = s->frame;
            if (f->nb_samples <= 0) {
                av_frame_unref(f);
                continue;
            }
            if (f->sample_rate != s->in_rate && !s->rate_warned) {
                cb_warn("%s: sample rate changes from %d to %d Hz; kept as %d Hz", s->path,
                        s->in_rate, f->sample_rate, s->in_rate);
                s->rate_warned = true;
            }
            if (!s->swr || f->format != s->in_fmt ||
                av_channel_layout_compare(&f->ch_layout, &s->in_layout)) {
                if (s->swr && f->ch_layout.nb_channels == s->in_layout.nb_channels &&
                    f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && f->format == s->in_fmt) {
                    /* the same layout, unnamed */
                } else if ((ret = open_swr(s, f)) < 0) {
                    av_frame_unref(f);
                    return ret;
                }
            }
            s->held_at = f->pts != AV_NOPTS_VALUE ? at_of(s, f->pts) : s->expect;
            s->expect = s->held_at + f->nb_samples;
            s->held = true;
            return 1;
        }
        if (ret == AVERROR_EOF)
            return 0;
        if (ret != AVERROR(EAGAIN)) {
            if (ret == AVERROR(ENOMEM))
                return CB_ERROR(ENOMEM);
            continue;           /* a damaged frame: its gap is silence */
        }
        if (s->demux_eof) {
            if (s->sent_eof)
                return 0;
            avcodec_send_packet(s->dec, NULL);
            s->sent_eof = true;
            continue;
        }
        if ((ret = read_packet(s)) < 0)
            return ret;
    }
}

/* ------------------------------------------------------------------- reads */

int cb_source_read(CBSource *s, uint8_t *const *dst, int n)
{
    int got = 0;
    while (got < n) {
        int ret = convert(s, dst, got, n, NULL, 0);
        if (ret < 0)
            return ret;
        got += ret;
        if (got >= n)
            break;
        if (s->flushed) {
            if (!ret)
                break;
            continue;
        }
        if (!s->held) {
            ret = next_frame(s);
            if (ret < 0)
                return ret;
            if (!ret) {
                s->flushed = true;
                continue;
            }
        }

        AVFrame *f = s->frame;
        if (s->held_at > s->next_in) {
            /* a gap before the frame: silence */
            int k = (int)CB_MIN(s->held_at - s->next_in, (int64_t)ZEROS);
            const uint8_t *z[MAX_CHANNELS];
            for (int c = 0; c < (av_sample_fmt_is_planar(s->in_fmt) ? s->in_ch : 1); c++)
                z[c] = s->zeros;
            if ((ret = convert(s, dst, got, n, z, k)) < 0)
                return ret;
            got += ret;
            s->next_in += k;
            continue;
        }
        int64_t cut = s->next_in - s->held_at;
        if (cut < f->nb_samples) {
            const uint8_t *in[MAX_CHANNELS];
            bool planar = av_sample_fmt_is_planar(s->in_fmt);
            size_t step = (size_t)av_get_bytes_per_sample(s->in_fmt) * (planar ? 1 : s->in_ch);
            for (int c = 0; c < (planar ? s->in_ch : 1); c++)
                in[c] = f->extended_data[c] + (size_t)cut * step;
            int k = f->nb_samples - (int)cut;
            if ((ret = convert(s, dst, got, n, in, k)) < 0)
                return ret;
            got += ret;
            s->next_in += k;
        }
        av_frame_unref(f);
        s->held = false;
    }
    s->pos += got;
    return got;
}

/* ------------------------------------------------------------------- seeks */

/* Puts the demuxer at or before timeline frame `from`. */
static int position(CBSource *s, int64_t from)
{
    /* from the start: every packet, those before 0 too (they prime the
     * decoder, and what they give is cut) */
    s->discard_to = from > 0 ? from : INT64_MIN;
    if (s->walk ? from >= s->demux_at && !s->demux_eof : false)
        return 0;
    if (!s->walk && from > 0) {
        int64_t ts = av_rescale_q(from - s->shift, (AVRational) { 1, s->in_rate }, s->tb);
        if (av_seek_frame(s->fmt, s->idx, ts, AVSEEK_FLAG_BACKWARD) >= 0)
            return 0;
    }
    return open_input(s);
}

int cb_source_seek(CBSource *s, int64_t at)
{
    at = CB_MAX(at, (int64_t)0);
    if (at == s->pos)
        return 0;
    int64_t start_out = at, start_in = at;
    if (s->in_rate != s->out_rate) {
        start_out = CB_MAX(at - RESTART_MARGIN, (int64_t)0) / s->p_out * s->p_out;
        start_in = start_out / s->p_out * s->p_in;
    }
    int64_t from = start_in - s->preroll;
    int ret = position(s, from);
    if (ret < 0)
        return ret;
    avcodec_flush_buffers(s->dec);
    av_frame_unref(s->frame);
    swr_free(&s->swr);
    s->drop = at - start_out;
    s->next_in = start_in;
    s->expect = CB_MAX(from, (int64_t)0);
    s->held = s->demux_eof = s->sent_eof = s->flushed = false;
    s->pos = at;
    return 0;
}

/* -------------------------------------------------------------------- open */

/* The players' timeline of an MP3 file, from its frames, over the mapping
 * libavformat reads. */
static void mp3_timeline(CBSource *s)
{
    CBMp3Info mi;
    if (cb_mp3_scan_mapped(s->path, cb_audio_input_map(s->fmt), &mi) >= 0 && mi.nb_frames > 0) {
        s->shift = (int64_t)(mi.header_counts - mi.skip_frames) * mi.frame_samples;
        int64_t frames = mi.nb_frames + mi.header_counts - mi.skip_frames;
        s->length = CB_MAX(frames, (int64_t)0) * mi.frame_samples;
    }
}

int cb_source_open(CBSource **out, const char *path, const CBAudioFormat *f)
{
    cb_audio_init_logging();
    *out = NULL;
    if (!f || f->channels < 0 || f->channels > 2 || f->rate < 0)
        return CB_ERROR(EINVAL);
    CBSource *s = calloc(1, sizeof(*s));
    if (!s)
        return CB_ERROR(ENOMEM);
    s->path = cb_strdup(path);
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    s->length = -1;
    s->discard_to = INT64_MIN;
    int ret = CB_ERROR(ENOMEM);
    if (!s->path || !s->pkt || !s->frame)
        goto fail;
    if ((ret = open_input(s)) < 0)
        goto fail;

    AVStream *st = s->fmt->streams[s->idx];
    const AVCodecParameters *par = st->codecpar;
    if (par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0) {
        cb_debug("%s: no decodable audio", path);
        ret = CB_ERROR(ENODATA);
        goto fail;
    }
    bool mp3 = par->codec_id == AV_CODEC_ID_MP3;
    if ((ret = cb_audio_open_decoder(&s->dec, par, st->time_base, mp3, -1)) < 0)
        goto fail;
    s->in_rate = s->dec->sample_rate > 0 ? s->dec->sample_rate : par->sample_rate;
    s->in_ch = s->dec->ch_layout.nb_channels > 0 ? s->dec->ch_layout.nb_channels
                                                 : par->ch_layout.nb_channels;
    s->in_fmt = AV_SAMPLE_FMT_NONE;
    s->walk = par->codec_id == AV_CODEC_ID_VORBIS;
    s->preroll = preroll_of(par, s->in_rate);

    if (mp3 && cb_audio_is_container(s->fmt, "mp3"))
        mp3_timeline(s);
    if (s->length < 0) {
        bool estimated = s->fmt->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE;
        s->length = cb_audio_container_samples(s->fmt, s->idx);
        /* the count read the file through */
        if (estimated && (ret = open_input(s)) < 0)
            goto fail;
    }

    s->out_rate = f->rate ? f->rate : s->in_rate;
    s->out_ch = f->channels ? f->channels : CB_MIN(s->in_ch, 2);
    s->out_fmt = av_format(f->sample);
    s->out_bps = av_get_bytes_per_sample(s->out_fmt);
    s->out_planar = av_sample_fmt_is_planar(s->out_fmt);
    int64_t g = gcd64(s->in_rate, s->out_rate);
    s->p_in = s->in_rate / g;
    s->p_out = s->out_rate / g;
    s->demux_at = INT64_MIN;
    *out = s;
    return 0;

fail:
    cb_source_free(&s);
    return ret;
}

void cb_source_free(CBSource **ps)
{
    CBSource *s = *ps;
    if (!s)
        return;
    swr_free(&s->swr);
    avcodec_free_context(&s->dec);
    cb_audio_close(&s->fmt);
    av_packet_free(&s->pkt);
    av_frame_free(&s->frame);
    av_channel_layout_uninit(&s->in_layout);
    free(s->zeros);
    free(s->path);
    free(s);
    *ps = NULL;
}

int cb_source_rate(const CBSource *s)
{
    return s->out_rate;
}

const char *cb_source_codec(const CBSource *s)
{
    return s->dec->codec->name;
}

int cb_source_channels(const CBSource *s)
{
    return s->out_ch;
}

enum AVSampleFormat cb_source_format(const CBSource *s)
{
    return s->out_fmt;
}

int64_t cb_source_length(const CBSource *s)
{
    if (s->length < 0)
        return -1;
    return av_rescale_rnd(s->length, s->out_rate, s->in_rate, AV_ROUND_UP);
}
