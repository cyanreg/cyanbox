/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A track encoded as it is decoded (cyanbox/audio.h, cb_encode): the frames
 * cb_decode_stream hands over, converted to the encoder's samples,
 * encoded and muxed into the caller's write and seek, in memory that does
 * not grow with the file. What a stick gets (src/export), what the
 * transcodes in the library's cache are (transcode.c), what Link serves
 * when it serves a file: one path from the file to the bytes, the samples
 * the analysis decoded.
 *
 * - Samples: 16 bits as the decoder's float * 32768, 24 bits * 2^23,
 *   rounded. A source whose samples are exactly that (the caller says so:
 *   16-bit lossless into 16 bits, 24-bit or less into 24) goes through
 *   bit for bit; anything else gets one step of triangular dither drawn
 *   from the sample's place. Clipped to the range.
 * - Length: what the decoder gives, or with `frames` exactly that many
 *   (silence after the decode ends, nothing past it), so a WAV's or an
 *   AIFF's size is known before a byte is written.
 * - FLAC: the source's tags and cover when asked; the frame table the
 *   ANLZ PVB2 tag is made from, kept from the packets as they go (the
 *   muxer writes the header and then each packet as it is, so the first
 *   frame's offset is what is left when the packets are taken off the
 *   end); the MD5 of the samples from the encoder's last STREAMINFO.
 * - WAV and AIFF: the header and the samples, nothing else (no LIST, no
 *   ID3: players take what they show from the database).
 * - MP3: 320 kbit/s CBR, no tags, no Xing frame (a player plays it as a
 *   frame of silence), and the encoder's delay (initial_padding) dropped
 *   from the samples fed to it, so the file as a player decodes it, delay
 *   and all, lines up with the source.
 *
 * A muxer goes back to its header at the end (FLAC's STREAMINFO, the
 * sizes in a WAV or an AIFF) through the caller's seek; without one the
 * header keeps what it had at the start. */

#include "cyanbox/audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#include "internal.h"

#define PCM_FRAME 4096          /* frames per PCM packet */

typedef struct Enc {
    const CBEncodeOptions *o;
    const char *path;
    AVFormatContext *in;        /* tags and the cover (FLAC) */
    AVFormatContext *oc;
    AVCodecContext *enc;
    AVStream *st;
    AVPacket *pkt;
    AVFrame *frame;
    int channels, rate, bits;
    bool exact;
    int frame_size;
    int64_t skip;               /* frames still to drop (MP3's delay) */
    int64_t limit;              /* frames to write, < 0 no limit */
    int64_t sent;               /* frames into the encoder */
    int filled;                 /* frames in `frame` */
    /* The byte sink */
    int64_t pos, size;
    /* FLAC */
    int64_t packet_bytes;
    CBFlacInfo flac;
    int frames_cap;
    uint8_t md5[16];
    bool md5_set;
    bool header_written;
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
        e->pkt->stream_index = e->st->index;
        av_packet_rescale_ts(e->pkt, e->enc->time_base, e->st->time_base);
        ret = av_write_frame(e->oc, e->pkt);
        av_packet_unref(e->pkt);
        if (ret < 0)
            return ret;
    }
}

static int send_frame(Enc *e)
{
    if (!e->filled)
        return 0;
    e->frame->nb_samples = e->filled;
    e->frame->pts = e->sent;
    e->sent += e->filled;
    e->filled = 0;
    int ret = avcodec_send_frame(e->enc, e->frame);
    if (ret >= 0)
        ret = drain(e);
    if (ret >= 0)
        ret = av_frame_make_writable(e->frame);
    return ret;
}

/* ----------------------------------------------------------- the samples */

/* Triangular noise of one step, in (-1, 1), for sample i (splitmix64). */
static float tpdf(uint64_t i)
{
    uint64_t z = i + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (float)(z >> 40) * 0x1p-24f - (float)(z & 0xFFFFFF) * 0x1p-24f;
}

/* Frames into the encoder's frame, by its sample format. */
static void put_samples(Enc *e, const float *x, int n, int64_t first)
{
    AVFrame *f = e->frame;
    enum AVSampleFormat fmt = e->enc->sample_fmt;
    double scale = e->bits == 24 ? 8388608.0 : 32768.0;
    long lo = e->bits == 24 ? -8388608 : -32768, hi = -lo - 1;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < e->channels; c++) {
            float v = x[(int64_t)i * e->channels + c];
            int at = e->filled + i;
            if (fmt == AV_SAMPLE_FMT_FLTP) {
                ((float *)f->extended_data[c])[at] = v;
                continue;
            }
            uint64_t k = (uint64_t)(first + i) * (uint64_t)e->channels + (uint64_t)c;
            long s = lrint(v * scale + (e->exact ? 0.0 : tpdf(k)));
            s = CB_CLAMP(s, lo, hi);
            if (fmt == AV_SAMPLE_FMT_S16)
                ((int16_t *)f->data[0])[at * e->channels + c] = (int16_t)s;
            else if (fmt == AV_SAMPLE_FMT_S32)
                ((int32_t *)f->data[0])[at * e->channels + c] = (int32_t)((uint32_t)s << 8);
        }
    }
    e->filled += n;
}

static int take(Enc *e, const float *x, int64_t n)
{
    if (e->o->cancelled && e->o->cancelled(e->o->opaque))
        return CB_ERROR(ECANCELED);
    if (e->skip > 0) {
        int64_t d = CB_MIN(e->skip, n);
        e->skip -= d;
        x += d * e->channels;
        n -= d;
    }
    if (e->limit >= 0)
        n = CB_MIN(n, e->limit - (e->sent + e->filled));
    while (n > 0) {
        int k = (int)CB_MIN(n, (int64_t)(e->frame_size - e->filled));
        put_samples(e, x, k, e->sent + e->filled);
        x += (int64_t)k * e->channels;
        n -= k;
        if (e->filled == e->frame_size) {
            int ret = send_frame(e);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- set-up */

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
        [CB_ENCODE_AIFF] = AV_CODEC_ID_PCM_S16BE, [CB_ENCODE_MP3] = AV_CODEC_ID_MP3,
    };
    static const char *const muxers[] = {
        [CB_ENCODE_FLAC] = "flac", [CB_ENCODE_WAV] = "wav", [CB_ENCODE_AIFF] = "aiff",
        [CB_ENCODE_MP3] = "mp3",
    };
    enum AVCodecID id = codecs[o->format];
    if (e->bits == 24 && o->format == CB_ENCODE_WAV)
        id = AV_CODEC_ID_PCM_S24LE;
    else if (e->bits == 24 && o->format == CB_ENCODE_AIFF)
        id = AV_CODEC_ID_PCM_S24BE;
    const AVCodec *codec = avcodec_find_encoder(id);
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
    e->enc->sample_rate = e->rate;
    e->enc->time_base = (AVRational) { 1, e->rate };
    av_channel_layout_default(&e->enc->ch_layout, e->channels);
    if (o->format == CB_ENCODE_MP3) {
        e->enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        e->enc->bit_rate = e->rate >= 32000 ? 320000 : 160000;
    } else {
        e->enc->sample_fmt = e->bits == 24 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
        e->enc->bits_per_raw_sample = e->bits;
    }
    if (o->format == CB_ENCODE_FLAC && o->compression > 0)
        e->enc->compression_level = o->compression;
    if (e->oc->oformat->flags & AVFMT_GLOBALHEADER)
        e->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((ret = avcodec_open2(e->enc, codec, NULL)) < 0)
        return ret;
    e->frame_size = e->enc->frame_size > 0 ? e->enc->frame_size : PCM_FRAME;
    e->skip = o->format == CB_ENCODE_MP3 ? CB_MAX(e->enc->initial_padding, 0) : 0;

    e->st = avformat_new_stream(e->oc, NULL);
    if (!e->st)
        return CB_ERROR(ENOMEM);
    e->st->time_base = e->enc->time_base;
    if ((ret = avcodec_parameters_from_context(e->st->codecpar, e->enc)) < 0)
        return ret;

    /* FLAC with the source's tags and cover: the picture as a stream of
     * its own, its one packet after the header (as the FLAC muxer wants
     * them before the audio). */
    const AVStream *pic_in = NULL;
    AVStream *pic_out = NULL;
    if (o->format == CB_ENCODE_FLAC && o->tags && cb_audio_open(e->path, &e->in, true) >= 0) {
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
        av_dict_set(&mux, "id3v2_version", "0", 0);
    } else if (o->format == CB_ENCODE_AIFF) {
        av_dict_set(&mux, "write_id3v2", "0", 0);
    }
    ret = avformat_write_header(e->oc, &mux);
    av_dict_free(&mux);
    if (ret < 0)
        return ret;
    e->header_written = true;
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

    e->frame->format = e->enc->sample_fmt;
    e->frame->sample_rate = e->rate;
    e->frame->nb_samples = e->frame_size;
    if ((ret = av_channel_layout_copy(&e->frame->ch_layout, &e->enc->ch_layout)) < 0 ||
        (ret = av_frame_get_buffer(e->frame, 0)) < 0)
        return ret;
    return 0;
}

static int on_start(void *opaque, int channels, int rate)
{
    Enc *e = opaque;
    const CBEncodeOptions *o = e->o;
    e->channels = channels;
    e->rate = o->max_rate > 0 && rate > o->max_rate ? o->max_rate : rate;
    int ret = open_output(e);
    if (ret < 0)
        return cb_audio_error(ret);
    return e->rate == rate ? 0 : e->rate;
}

static int on_frames(void *opaque, const float *x, int64_t n)
{
    int ret = take(opaque, x, n);
    return ret < 0 ? cb_audio_error(ret) : 0;
}

int cb_encode(const char *path, const CBEncodeOptions *o, CBEncoded *out)
{
    if (out)
        *out = (CBEncoded) { 0 };
    if (!path || !o || !o->write || (unsigned)o->format > CB_ENCODE_MP3 ||
        (o->format != CB_ENCODE_MP3 && o->bits != 16 && o->bits != 24) ||
        o->channels < 0 || o->channels > 2)
        return CB_ERROR(EINVAL);
    cb_audio_init_logging();
    Enc e = {
        .o = o, .path = path, .bits = o->format == CB_ENCODE_MP3 ? 0 : o->bits,
        .exact = o->exact, .limit = o->frames > 0 ? o->frames : -1,
    };
    CBDecodeTo to = { .start = on_start, .frames = on_frames, .opaque = &e };
    int ret = cb_decode_stream(path, o->channels, &to);
    /* Silence to the length asked for when the decode ended first. */
    if (ret >= 0 && e.limit > 0 && e.enc) {
        static const float zeros[2 * 1024];
        while (ret >= 0 && e.sent + e.filled < e.limit)
            ret = take(&e, zeros, CB_MIN((int64_t)1024, e.limit - e.sent - e.filled));
    }
    if (ret >= 0 && !e.enc)
        ret = CB_ERROR(ENODATA);
    if (ret >= 0)
        ret = send_frame(&e);
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
            .rate = e.rate, .channels = e.channels, .bits = e.bits, .frames = e.sent, .size = e.size,
        };
        if (o->format == CB_ENCODE_FLAC) {
            e.flac.valid = true;
            e.flac.sample_rate = e.rate;
            e.flac.channels = e.channels;
            e.flac.bit_depth = e.bits;
            e.flac.total_samples = e.sent;
            e.flac.first_frame_offset = e.size - e.packet_bytes;
            out->flac = e.flac;
            e.flac = (CBFlacInfo) { 0 };
            if (e.md5_set)
                memcpy(out->md5, e.md5, 16);
        }
    }
    if (e.oc && e.oc->pb) {
        av_freep(&e.oc->pb->buffer);
        avio_context_free(&e.oc->pb);
    }
    avformat_free_context(e.oc);
    avformat_close_input(&e.in);
    avcodec_free_context(&e.enc);
    av_packet_free(&e.pkt);
    av_frame_free(&e.frame);
    cb_flac_info_free(&e.flac);
    return ret < 0 ? cb_audio_error(ret) : 0;
}
