/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The stick's compatibility transcode: any file to a 16-bit FLAC at the
 * native rate, or at max_rate when the native one is higher, with the tags
 * and the cover copied. dj's export.transcoded did this with the ffmpeg CLI
 * (-map_metadata 0, -c:a flac) for codecs no player handles.
 *
 * The audio is rx_decode's, so it is on the players' timeline: an MP3's
 * silent header frame and encoder delay become plain samples at the start
 * of the FLAC, and every time stored for the source stays right for the
 * transcode. Reducing depth (24-bit or lossy sources, or any resampled
 * signal) uses libswresample's triangular (TPDF) dither; a 16-bit source at
 * its own rate goes through untouched, sample for sample.
 *
 * The file is written next to out_path with a ".part" suffix and renamed
 * into place, so an interrupted export never leaves half a FLAC behind. */

#include "rxbox/audio.h"

#include <stdio.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "internal.h"

typedef struct Out {
    AVFormatContext *oc;
    AVCodecContext *enc;
    AVStream *st;
    AVPacket *pkt;
} Out;

static int write_packets(Out *o)
{
    while (1) {
        int ret = avcodec_receive_packet(o->enc, o->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        o->pkt->stream_index = o->st->index;
        av_packet_rescale_ts(o->pkt, o->enc->time_base, o->st->time_base);
        ret = av_interleaved_write_frame(o->oc, o->pkt);
        if (ret < 0)
            return ret;
    }
}

/* Tags as rx_probe reads them: the stream's, then the container's over
 * them. What described the old encoding is left out. */
static int copy_tags(AVDictionary **dst, const AVFormatContext *in, const AVStream *st)
{
    static const char *const drop[] = { "encoder", "encoded_by", "itunsmpb", "itunnorm" };
    const AVDictionary *src[] = { st->metadata, in->metadata };
    for (int s = 0; s < 2; s++) {
        const AVDictionaryEntry *e = NULL;
        while ((e = av_dict_iterate(src[s], e))) {
            bool skip = false;
            for (size_t i = 0; i < RX_ARRAY_ELEMS(drop); i++)
                skip |= !rx_strcasecmp(e->key, drop[i]);
            if (skip)
                continue;
            int ret = av_dict_set(dst, e->key, e->value, 0);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

int rx_transcode_flac(const char *path, const char *out_path, int max_rate)
{
    rx_audio_init_logging();
    AVFormatContext *in;
    int ret = rx_audio_open(path, &in, true);
    if (ret < 0)
        return ret;

    RXAudio a = { 0 };
    Out o = { 0 };
    SwrContext *swr = NULL;
    AVFrame *frame = NULL;
    char *part = NULL, *url = NULL;
    bool opened = false;

    int idx = rx_audio_first_stream(in, AVMEDIA_TYPE_AUDIO);
    if (idx < 0) {
        ret = RX_ERROR(ENODATA);
        goto end;
    }
    const AVStream *ist = in->streams[idx];
    const AVCodecParameters *ipar = ist->codecpar;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(ipar->codec_id);
    bool lossy = desc && (desc->props & AV_CODEC_PROP_LOSSY) &&
                 !(desc->props & AV_CODEC_PROP_LOSSLESS);
    int depth = ipar->bits_per_raw_sample ? ipar->bits_per_raw_sample
                                          : av_get_bits_per_sample(ipar->codec_id);

    /* The decoder's rate, which for Opus is not the container's */
    ret = rx_decode(path, 0, 0, &a);
    if (ret < 0)
        goto end;
    int rate = max_rate > 0 && a.rate > max_rate ? max_rate : a.rate;
    if (rate != a.rate) {
        RXAudio r;
        ret = rx_audio_resample(&a, rate, &r);
        rx_audio_free(&a);
        if (ret < 0)
            goto end;
        a = r;
    }
    bool dither = lossy || depth == 0 || depth > 16 || rate != ipar->sample_rate;

    part = rx_asprintf("%s.part", out_path);
    url = rx_asprintf("file:%s", part);
    if (!part || !url) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }

    /* Encoder */
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec) {
        ret = RX_ERROR(ENOTSUP);
        goto end;
    }
    ret = avformat_alloc_output_context2(&o.oc, NULL, "flac", NULL);
    if (ret < 0)
        goto end;
    o.enc = avcodec_alloc_context3(codec);
    o.pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!o.enc || !o.pkt || !frame) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    o.enc->sample_fmt = AV_SAMPLE_FMT_S16;
    o.enc->sample_rate = rate;
    o.enc->bits_per_raw_sample = 16;
    o.enc->time_base = (AVRational) { 1, rate };
    if (ipar->ch_layout.nb_channels == a.channels &&
        ipar->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC)
        ret = av_channel_layout_copy(&o.enc->ch_layout, &ipar->ch_layout);
    else
        av_channel_layout_default(&o.enc->ch_layout, a.channels);
    if (ret < 0)
        goto end;
    if (o.oc->oformat->flags & AVFMT_GLOBALHEADER)
        o.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ret = avcodec_open2(o.enc, codec, NULL);
    if (ret < 0)
        goto end;

    /* Streams: the audio, then the cover */
    o.st = avformat_new_stream(o.oc, NULL);
    if (!o.st) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    o.st->time_base = o.enc->time_base;
    ret = avcodec_parameters_from_context(o.st->codecpar, o.enc);
    if (ret < 0)
        goto end;

    const AVStream *pic_in = NULL;
    AVStream *pic_out = NULL;
    for (unsigned i = 0; i < in->nb_streams && !pic_in; i++) {
        const AVStream *s = in->streams[i];
        if ((s->disposition & AV_DISPOSITION_ATTACHED_PIC) && s->attached_pic.size > 0)
            pic_in = s;
    }
    if (pic_in) {
        pic_out = avformat_new_stream(o.oc, NULL);
        if (!pic_out) {
            ret = RX_ERROR(ENOMEM);
            goto end;
        }
        ret = avcodec_parameters_copy(pic_out->codecpar, pic_in->codecpar);
        if (ret >= 0)
            ret = av_dict_copy(&pic_out->metadata, pic_in->metadata, 0);
        if (ret < 0)
            goto end;
        pic_out->codecpar->codec_tag = 0;
        pic_out->disposition = AV_DISPOSITION_ATTACHED_PIC;
        pic_out->time_base = (AVRational) { 1, 90000 };
    }

    ret = copy_tags(&o.oc->metadata, in, ist);
    if (ret < 0)
        goto end;

    ret = avio_open(&o.oc->pb, url, AVIO_FLAG_WRITE);
    if (ret < 0)
        goto end;
    opened = true;
    ret = avformat_write_header(o.oc, NULL);
    if (ret < 0)
        goto end;
    if (pic_out) {
        AVPacket *pp = av_packet_clone(&pic_in->attached_pic);
        if (!pp) {
            ret = RX_ERROR(ENOMEM);
            goto end;
        }
        pp->stream_index = pic_out->index;
        pp->pts = pp->dts = 0;
        pp->flags |= AV_PKT_FLAG_KEY;
        ret = av_interleaved_write_frame(o.oc, pp);
        av_packet_free(&pp);
        if (ret < 0)
            goto end;
    }

    /* float -> s16, dithered when that loses depth */
    ret = swr_alloc_set_opts2(&swr, &o.enc->ch_layout, AV_SAMPLE_FMT_S16, rate,
                              &o.enc->ch_layout, AV_SAMPLE_FMT_FLT, rate, 0, NULL);
    if (ret < 0)
        goto end;
    if (dither)
        av_opt_set_int(swr, "dither_method", SWR_DITHER_TRIANGULAR, 0);
    ret = swr_init(swr);
    if (ret < 0)
        goto end;

    int frame_size = o.enc->frame_size > 0 ? o.enc->frame_size : 4096;
    for (int64_t pos = 0; pos < a.nb_frames; pos += frame_size) {
        int n = (int)RX_MIN((int64_t)frame_size, a.nb_frames - pos);
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = rate;
        frame->nb_samples = n;
        ret = av_channel_layout_copy(&frame->ch_layout, &o.enc->ch_layout);
        if (ret >= 0)
            ret = av_frame_get_buffer(frame, 0);
        if (ret < 0)
            goto end;
        const uint8_t *src[1] = { (const uint8_t *)(a.samples + pos * a.channels) };
        ret = swr_convert(swr, frame->extended_data, n, src, n);
        if (ret >= 0 && ret != n)
            ret = AVERROR_BUG;
        if (ret < 0)
            goto end;
        frame->pts = pos;
        ret = avcodec_send_frame(o.enc, frame);
        av_frame_unref(frame);
        if (ret >= 0)
            ret = write_packets(&o);
        if (ret < 0)
            goto end;
    }
    ret = avcodec_send_frame(o.enc, NULL);
    if (ret >= 0)
        ret = write_packets(&o);
    if (ret >= 0)
        ret = av_write_trailer(o.oc);
    if (ret < 0)
        goto end;
    ret = avio_closep(&o.oc->pb);
    opened = false;
    if (ret < 0)
        goto end;
    if (rename(part, out_path) < 0)
        ret = RX_ERROR(errno);

end:
    if (ret < 0)
        rx_err("%s: transcode to FLAC failed: %s", path, av_err2str(ret));
    if (opened)
        avio_closep(&o.oc->pb);
    if (ret < 0 && part)
        unlink(part);
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&o.pkt);
    avcodec_free_context(&o.enc);
    avformat_free_context(o.oc);
    avformat_close_input(&in);
    rx_audio_free(&a);
    free(part);
    free(url);
    return ret < 0 ? rx_audio_error(ret) : 0;
}
