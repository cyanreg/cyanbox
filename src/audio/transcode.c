/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The transcodes the library keeps in its cache (src/export/plan.c): a
 * file of rx_encode's output (encode.c), the one path from a track to the
 * bytes of another format. The stick's compatibility transcode is a 16-bit
 * FLAC at the native rate, or at max_rate when the native one is higher,
 * with the tags and the cover copied: dj's export.transcoded did this with
 * the ffmpeg CLI (-map_metadata 0, -c:a flac) for codecs no player
 * handles. Link's are the same audio as a plain 16-bit stereo WAV, or
 * (link.mp3) a 320 kbps CBR stereo MP3 lined up with the source.
 *
 * The audio is rx_decode's, so it is on the players' timeline: an MP3's
 * silent header frame and encoder delay become plain samples at the start
 * of the FLAC, and every time stored for the source stays right for the
 * transcode. Reducing depth (24-bit or lossy sources, or any resampled
 * signal) dithers by one step; a 16-bit lossless source at its own rate
 * goes through untouched, sample for sample.
 *
 * The file is written next to out_path with a ".part" suffix and renamed
 * into place, so an interrupted export never leaves half a file behind. */

#include "rxbox/audio.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>

#include "internal.h"

typedef struct File {
    int fd;
} File;

static int file_write(void *opaque, const uint8_t *buf, int size)
{
    File *f = opaque;
    for (int done = 0; done < size;) {
        ssize_t w = write(f->fd, buf + done, (size_t)(size - done));
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return AVERROR(errno);
        done += (int)w;
    }
    return size;
}

static int64_t file_seek(void *opaque, int64_t offset, int whence)
{
    File *f = opaque;
    off_t r = lseek(f->fd, (off_t)offset, whence);
    return r < 0 ? AVERROR(errno) : (int64_t)r;
}

/* Whether the source's samples are 16-bit ones that reach the transcode
 * as they are: a lossless codec of 16 bits or fewer, at a rate kept. */
static bool exact16(const char *path, int max_rate)
{
    AVFormatContext *in;
    if (rx_audio_open(path, &in, true) < 0)
        return false;
    bool exact = false;
    int idx = rx_audio_first_stream(in, AVMEDIA_TYPE_AUDIO);
    if (idx >= 0) {
        const AVCodecParameters *p = in->streams[idx]->codecpar;
        const AVCodecDescriptor *d = avcodec_descriptor_get(p->codec_id);
        int depth = p->bits_per_raw_sample ? p->bits_per_raw_sample
                                           : av_get_bits_per_sample(p->codec_id);
        exact = d && (d->props & AV_CODEC_PROP_LOSSLESS) && depth > 0 && depth <= 16 &&
                p->codec_id != AV_CODEC_ID_PCM_F32LE && p->codec_id != AV_CODEC_ID_PCM_F32BE &&
                (max_rate <= 0 || p->sample_rate <= max_rate);
    }
    avformat_close_input(&in);
    return exact;
}

static int to_file(const char *path, const char *out_path, RXEncodeOptions o)
{
    char *part = rx_asprintf("%s.part", out_path);
    if (!part)
        return RX_ERROR(ENOMEM);
    File f = { .fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) };
    if (f.fd < 0) {
        int ret = RX_ERROR(errno);
        free(part);
        return ret;
    }
    o.write = file_write;
    o.seek = file_seek;
    o.opaque = &f;
    int ret = rx_encode(path, &o, NULL);
    if (close(f.fd) < 0 && ret >= 0)
        ret = RX_ERROR(errno);
    if (ret >= 0 && rename(part, out_path) < 0)
        ret = RX_ERROR(errno);
    if (ret < 0)
        unlink(part);
    free(part);
    return ret;
}

int rx_transcode_flac(const char *path, const char *out_path, int max_rate)
{
    return to_file(path, out_path, (RXEncodeOptions) {
        .format = RX_ENCODE_FLAC, .bits = 16, .exact = exact16(path, max_rate),
        .max_rate = max_rate, .tags = true,
    });
}

int rx_transcode_wav(const char *path, const char *out_path, int max_rate)
{
    return to_file(path, out_path, (RXEncodeOptions) {
        .format = RX_ENCODE_WAV, .bits = 16, .exact = exact16(path, max_rate), .channels = 2,
        .max_rate = max_rate,
    });
}

int rx_transcode_mp3(const char *path, const char *out_path, int max_rate)
{
    return to_file(path, out_path, (RXEncodeOptions) {
        .format = RX_ENCODE_MP3, .channels = 2, .max_rate = max_rate,
    });
}
