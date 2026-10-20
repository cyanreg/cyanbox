/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A track encoded as it is decoded (cyanbox/audio.h, cb_encode): what a
 * stick gets (src/export). A pipeline (pipeline.c) makes the track's 16-bit
 * samples at 44.1 kHz in chunks of the encoder's frame size, and each chunk
 * goes to libavcodec as the AVFrame's own buffer, unreferenced when the
 * encoder is done with it: nothing copies the samples. libswresample
 * converts, mixes (more than two channels to stereo) and dithers; a 16-bit
 * file at 44.1 kHz comes through as its samples.
 *
 * - FLAC: compression level 8 (the highest that keeps to FLAC's subset,
 *   which every player decodes), the source's tags and cover when asked;
 *   the frame table the ANLZ PVB2 tag is made from, kept from the packets
 *   as they go (the muxer writes the header and then each packet as it is,
 *   so the first frame's offset is what is left when the packets are taken
 *   off the end); the MD5 of the samples from the encoder's last
 *   STREAMINFO.
 * - WAV: the header and the samples, nothing else (players take what they
 *   show from the database); with `frames`, exactly that many (silence
 *   after the decode ends, nothing past it), so its size is known before a
 *   byte is written.
 * - MP3: 320 kbit/s CBR (LAME), no Xing frame (a player plays one as a
 *   frame of silence), ID3v2.3 tags and the cover when asked; the
 *   encoder's delay (initial_padding) taken off the samples fed to it, so
 *   the file as a player decodes it, delay and all, lines up with the
 *   source. Its length is its frames', 1152 samples each.
 *
 * A muxer goes back to its header at the end (FLAC's STREAMINFO, a WAV's
 * sizes) through the caller's seek; without one the header keeps what it
 * had at the start. */

#include "cyanbox/audio.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#include "internal.h"

#define PCM_FRAME 4096          /* frames per PCM packet */
#define AHEAD     2.0           /* seconds the pipeline decodes ahead */

typedef struct Enc {
    const CBEncodeOptions *o;
    const char *path;
    AVFormatContext *in;        /* tags and the cover */
    AVFormatContext *oc;
    AVCodecContext *enc;
    AVStream *st;
    AVPacket *pkt;
    AVFrame *frame;
    int channels;
    int frame_size;
    int64_t sent;               /* frames into the encoder */
    /* The byte sink */
    int64_t pos, size;
    /* FLAC */
    int64_t packet_bytes;
    CBFlacInfo flac;
    int frames_cap;
    uint8_t md5[16];
    bool md5_set;
    /* MP3 */
    int64_t packets;
} Enc;

/* ------------------------------------------------------------- the bytes */

static int io_write(void *opaque, const uint8_t *buf, int n)
{
    Enc *e = opaque;
    int ret = e->o->write(e->o->opaque, buf, n);
    if (ret < 0)
        return ret;
    e->pos += n;
    e->size = CB_MAX(e->size, e->pos);
    return n;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
    Enc *e = opaque;
    if (whence == AVSEEK_SIZE)
        return e->size;
    int64_t to = whence == SEEK_SET ? offset : whence == SEEK_CUR ? e->pos + offset
               : whence == SEEK_END ? e->size + offset : -1;
    if (to < 0)
        return AVERROR(EINVAL);
    int64_t r = e->o->seek(e->o->opaque, to, SEEK_SET);
    if (r < 0)
        return r;
    e->pos = to;
    return to;
}

/* ----------------------------------------------------------- the packets */

static int keep_frame(Enc *e, const AVPacket *p)
{
    if (e->flac.nb_frames == e->frames_cap) {
        int cap = e->frames_cap ? e->frames_cap * 2 : 1024;
        void *f = realloc(e->flac.frames, (size_t)cap * sizeof(*e->flac.frames));
        if (!f)
            return CB_ERROR(ENOMEM);
        e->flac.frames = f;
        e->frames_cap = cap;
    }
    e->flac.frames[e->flac.nb_frames++] = (typeof(*e->flac.frames)) {
        .sample = p->pts, .offset = e->packet_bytes, .block = (int)p->duration,
    };
    e->flac.min_block = e->flac.nb_frames == 1 ? (int)p->duration
                      : CB_MIN(e->flac.min_block, (int)p->duration);
    e->flac.max_block = CB_MAX(e->flac.max_block, (int)p->duration);
    return 0;
}

static int drain(Enc *e)
{
    while (1) {
        int ret = avcodec_receive_packet(e->enc, e->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        /* The last FLAC packet carries the final STREAMINFO, MD5 and all. */
        size_t sd_size = 0;
        const uint8_t *sd = av_packet_get_side_data(e->pkt, AV_PKT_DATA_NEW_EXTRADATA, &sd_size);
        if (sd && sd_size >= 34 && e->o->format == CB_ENCODE_FLAC) {
            memcpy(e->md5, sd + 18, 16);
            e->md5_set = true;
        }
        if (e->o->format == CB_ENCODE_FLAC && e->pkt->size > 0) {
            if ((ret = keep_frame(e, e->pkt)) < 0)
                return ret;
            e->packet_bytes += e->pkt->size;
        }
        if (e->o->format == CB_ENCODE_MP3 && e->pkt->size > 0)
            e->packets++;
        e->pkt->stream_index = e->st->index;
        av_packet_rescale_ts(e->pkt, e->enc->time_base, e->st->time_base);
        ret = av_write_frame(e->oc, e->pkt);
        av_packet_unref(e->pkt);
        if (ret < 0)
            return ret;
    }
}

static int send_frame(Enc *e, AVFrame *f)
{
    f->pts = e->sent;
    e->sent += f->nb_samples;
    int ret = avcodec_send_frame(e->enc, f);
    av_frame_unref(f);
    return ret >= 0 ? drain(e) : ret;
}

/* The encoder's frame over a chunk's own samples: unreferenced when the
 * encoder lets the frame go. */
static void chunk_gone(void *opaque, uint8_t *data)
{
    CBChunk *c = opaque;
    cb_chunk_unref(&c);
}

static int frame_of(Enc *e, CBChunk *c, int n)
{
    AVFrame *f = e->frame;
    enum AVSampleFormat fmt = e->enc->sample_fmt;
    bool planar = av_sample_fmt_is_planar(fmt);
    int bps = av_get_bytes_per_sample(fmt);
    /* both planes of a planar chunk lie in its one allocation */
    size_t size = planar && e->channels > 1 ? (size_t)(c->data[1] - c->data[0]) * 2
                                            : (size_t)n * bps * e->channels;
    f->buf[0] = av_buffer_create(c->data[0], size, chunk_gone, c, 0);
    if (!f->buf[0]) {
        cb_chunk_unref(&c);
        return CB_ERROR(ENOMEM);
    }
    f->data[0] = c->data[0];
    if (planar && e->channels > 1)
        f->data[1] = c->data[1];
    f->extended_data = f->data;
    f->linesize[0] = (int)(planar ? (size_t)n * bps : (size_t)n * bps * e->channels);
    f->nb_samples = n;
    f->format = fmt;
    f->sample_rate = CB_ENCODE_RATE;
    return av_channel_layout_copy(&f->ch_layout, &e->enc->ch_layout);
}

/* n frames of silence. */
static int silence(Enc *e, int n)
{
    AVFrame *f = e->frame;
    f->format = e->enc->sample_fmt;
    f->sample_rate = CB_ENCODE_RATE;
    f->nb_samples = n;
    int ret = av_channel_layout_copy(&f->ch_layout, &e->enc->ch_layout);
    if (ret >= 0)
        ret = av_frame_get_buffer(f, 0);
    if (ret >= 0)
        ret = av_samples_set_silence(f->extended_data, 0, n, e->channels, f->format);
    return ret < 0 ? ret : send_frame(e, f);
}

/* ----------------------------------------------------------- set-up */

static int copy_tags(AVDictionary **dst, const AVFormatContext *in)
{
    static const char *const drop[] = { "encoder", "encoded_by", "itunsmpb", "itunnorm" };
    int idx = cb_audio_first_stream((AVFormatContext *)in, AVMEDIA_TYPE_AUDIO);
    const AVDictionary *src[] = { idx >= 0 ? in->streams[idx]->metadata : NULL, in->metadata };
    for (int s = 0; s < 2; s++) {
        const AVDictionaryEntry *t = NULL;
        while ((t = av_dict_iterate(src[s], t))) {
            bool skip = false;
            for (size_t i = 0; i < CB_ARRAY_ELEMS(drop); i++)
                skip |= !cb_strcasecmp(t->key, drop[i]);
            int ret = skip ? 0 : av_dict_set(dst, t->key, t->value, 0);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

static int open_output(Enc *e)
{
    const CBEncodeOptions *o = e->o;
    static const enum AVCodecID codecs[] = {
        [CB_ENCODE_FLAC] = AV_CODEC_ID_FLAC, [CB_ENCODE_WAV] = AV_CODEC_ID_PCM_S16LE,
        [CB_ENCODE_MP3] = AV_CODEC_ID_MP3,
    };
    static const char *const muxers[] = {
        [CB_ENCODE_FLAC] = "flac", [CB_ENCODE_WAV] = "wav", [CB_ENCODE_MP3] = "mp3",
    };
    const AVCodec *codec = avcodec_find_encoder(codecs[o->format]);
    if (!codec)
        return CB_ERROR(ENOTSUP);
    int ret = avformat_alloc_output_context2(&e->oc, NULL, muxers[o->format], NULL);
    if (ret < 0)
        return ret;
    /* The same bytes from the same samples, every time. */
    e->oc->flags |= AVFMT_FLAG_BITEXACT;

    e->enc = avcodec_alloc_context3(codec);
    e->pkt = av_packet_alloc();
    e->frame = av_frame_alloc();
    if (!e->enc || !e->pkt || !e->frame)
        return CB_ERROR(ENOMEM);
    e->enc->sample_rate = CB_ENCODE_RATE;
    e->enc->time_base = (AVRational) { 1, CB_ENCODE_RATE };
    av_channel_layout_default(&e->enc->ch_layout, e->channels);
    e->enc->sample_fmt = o->format == CB_ENCODE_MP3 ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
    e->enc->bits_per_raw_sample = 16;
    if (o->format == CB_ENCODE_FLAC)
        e->enc->compression_level = 8;
    if (o->format == CB_ENCODE_MP3)
        e->enc->bit_rate = 320000;
    if (e->oc->oformat->flags & AVFMT_GLOBALHEADER)
        e->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((ret = avcodec_open2(e->enc, codec, NULL)) < 0)
        return ret;
    e->frame_size = e->enc->frame_size > 0 ? e->enc->frame_size : PCM_FRAME;

    e->st = avformat_new_stream(e->oc, NULL);
    if (!e->st)
        return CB_ERROR(ENOMEM);
    e->st->time_base = e->enc->time_base;
    if ((ret = avcodec_parameters_from_context(e->st->codecpar, e->enc)) < 0)
        return ret;

    /* The source's tags and cover: the picture as a stream of its own, its
     * one packet after the header (as the FLAC and MP3 muxers want them
     * before the audio). */
    const AVStream *pic_in = NULL;
    AVStream *pic_out = NULL;
    if (o->tags && o->format != CB_ENCODE_WAV && cb_audio_open(e->path, &e->in, true) >= 0) {
        if ((ret = copy_tags(&e->oc->metadata, e->in)) < 0)
            return ret;
        for (unsigned i = 0; i < e->in->nb_streams && !pic_in; i++) {
            const AVStream *s = e->in->streams[i];
            if ((s->disposition & AV_DISPOSITION_ATTACHED_PIC) && s->attached_pic.size > 0)
                pic_in = s;
        }
        if (pic_in) {
            if (!(pic_out = avformat_new_stream(e->oc, NULL)))
                return CB_ERROR(ENOMEM);
            if ((ret = avcodec_parameters_copy(pic_out->codecpar, pic_in->codecpar)) < 0 ||
                (ret = av_dict_copy(&pic_out->metadata, pic_in->metadata, 0)) < 0)
                return ret;
            pic_out->codecpar->codec_tag = 0;
            pic_out->disposition = AV_DISPOSITION_ATTACHED_PIC;
            pic_out->time_base = (AVRational) { 1, 90000 };
        }
    }

    uint8_t *buf = av_malloc(1 << 16);
    if (!buf)
        return CB_ERROR(ENOMEM);
    e->oc->pb = avio_alloc_context(buf, 1 << 16, 1, e, NULL, io_write, o->seek ? io_seek : NULL);
    if (!e->oc->pb) {
        av_free(buf);
        return CB_ERROR(ENOMEM);
    }
    e->oc->pb->seekable = o->seek ? AVIO_SEEKABLE_NORMAL : 0;

    AVDictionary *mux = NULL;
    if (o->format == CB_ENCODE_MP3) {
        av_dict_set(&mux, "write_xing", "0", 0);
        av_dict_set(&mux, "id3v2_version", "3", 0);
    }
    ret = avformat_write_header(e->oc, &mux);
    av_dict_free(&mux);
    if (ret < 0)
        return ret;
    if (pic_out) {
        AVPacket *pp = av_packet_clone(&pic_in->attached_pic);
        if (!pp)
            return CB_ERROR(ENOMEM);
        pp->stream_index = pic_out->index;
        pp->pts = pp->dts = 0;
        pp->flags |= AV_PKT_FLAG_KEY;
        ret = av_write_frame(e->oc, pp);
        av_packet_free(&pp);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* The next chunk, waited for. */
static CBChunk *next_chunk(CBPipeline *p)
{
    while (1) {
        CBChunk *c = cb_pipeline_pop(p);
        if (c)
            return c;
        struct pollfd pfd = { .fd = cb_pipeline_fd(p), .events = POLLIN };
        if (poll(&pfd, 1, 10000) <= 0)
            return NULL;
        uint64_t v;
        if (read(pfd.fd, &v, sizeof(v)) < 0)
            continue;
    }
}

static int encode(Enc *e)
{
    const CBEncodeOptions *o = e->o;
    CBPipelineOptions po = {
        .format = { .sample = o->format == CB_ENCODE_MP3 ? CB_SAMPLE_S16P : CB_SAMPLE_S16,
                    .rate = CB_ENCODE_RATE, .channels = e->channels },
        .chunk = e->frame_size, .ahead = AHEAD,
        /* LAME's delay off the front: the file decodes in line */
        .offset = o->format == CB_ENCODE_MP3 ? CB_MAX(e->enc->initial_padding, 0) : 0,
        .name = "encode",
    };
    CBPipeline *p;
    int ret = cb_pipeline_open(&p, e->path, &po);
    if (ret < 0)
        return ret;
    int64_t limit = o->frames > 0 ? o->frames : -1;
    while (ret >= 0) {
        if (o->cancelled && o->cancelled(o->opaque)) {
            ret = CB_ERROR(ECANCELED);
            break;
        }
        CBChunk *c = next_chunk(p);
        if (!c) {
            ret = CB_ERROR(ETIMEDOUT);
            break;
        }
        if (c->err < 0) {
            ret = c->err;
            cb_chunk_unref(&c);
            break;
        }
        bool last = c->last;
        int n = c->nb;
        if (limit >= 0)
            n = (int)CB_MIN((int64_t)n, limit - e->sent);
        if (n > 0) {
            if ((ret = frame_of(e, c, n)) >= 0)
                ret = send_frame(e, e->frame);
        } else {
            cb_chunk_unref(&c);
        }
        if (last || (limit >= 0 && e->sent >= limit))
            break;
    }
    cb_pipeline_free(&p);
    /* Silence to the length asked for when the decode ended first. */
    while (ret >= 0 && limit > 0 && e->sent < limit)
        ret = silence(e, (int)CB_MIN((int64_t)e->frame_size, limit - e->sent));
    if (ret >= 0 && !e->sent)
        ret = CB_ERROR(ENODATA);
    return ret;
}

int cb_encode(const char *path, const CBEncodeOptions *o, CBEncoded *out)
{
    if (out)
        *out = (CBEncoded) { 0 };
    if (!path || !o || !o->write || (unsigned)o->format > CB_ENCODE_MP3 || o->channels < 0 ||
        o->channels > 2)
        return CB_ERROR(EINVAL);
    cb_audio_init_logging();
    Enc e = { .o = o, .path = path, .channels = o->channels };
    int ret = 0;
    if (!e.channels) {
        /* the source's, as the pipeline will make them */
        CBAudioFormat f = { .sample = CB_SAMPLE_S16, .rate = CB_ENCODE_RATE };
        CBSource *s;
        if ((ret = cb_source_open(&s, path, &f)) >= 0) {
            e.channels = cb_source_channels(s);
            cb_source_free(&s);
        }
    }
    if (ret >= 0)
        ret = open_output(&e);
    if (ret >= 0)
        ret = encode(&e);
    if (ret >= 0 && (ret = avcodec_send_frame(e.enc, NULL)) >= 0)
        ret = drain(&e);
    if (ret >= 0)
        ret = av_write_trailer(e.oc);
    if (ret >= 0) {
        avio_flush(e.oc->pb);
        ret = e.oc->pb->error;
    }
    if (ret < 0) {
        if (ret != CB_ERROR(ECANCELED) && ret != AVERROR(ECANCELED))
            cb_err("%s: encoding failed: %s", path, av_err2str(ret));
    } else if (out) {
        *out = (CBEncoded) {
            .rate = CB_ENCODE_RATE, .channels = e.channels, .frames = e.sent, .size = e.size,
        };
        if (o->format == CB_ENCODE_FLAC) {
            e.flac.valid = true;
            e.flac.sample_rate = CB_ENCODE_RATE;
            e.flac.channels = e.channels;
            e.flac.bit_depth = 16;
            e.flac.total_samples = e.sent;
            e.flac.first_frame_offset = e.size - e.packet_bytes;
            out->flac = e.flac;
            e.flac = (CBFlacInfo) { 0 };
            if (e.md5_set)
                memcpy(out->md5, e.md5, 16);
        } else if (o->format == CB_ENCODE_MP3) {
            out->mp3_frames = e.packets;
            out->frames = e.packets * 1152;
        }
    }
    if (e.oc && e.oc->pb) {
        av_freep(&e.oc->pb->buffer);
        avio_context_free(&e.oc->pb);
    }
    avformat_free_context(e.oc);
    cb_audio_close(&e.in);
    avcodec_free_context(&e.enc);
    av_packet_free(&e.pkt);
    av_frame_free(&e.frame);
    cb_flac_info_free(&e.flac);
    return ret < 0 ? cb_audio_error(ret) : 0;
}
