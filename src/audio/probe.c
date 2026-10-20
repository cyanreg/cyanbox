/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Probing a file with libavformat, as dj's audio.probe did with ffprobe, and
 * the helpers the rest of the module shares (logging, errors, tags;
 * opening and mapping are input.c's).
 *
 * Durations are on the players' timeline: an MP3's comes from the frame scan
 * (exact, the silent header frame included when rekordbox counts it), a
 * FLAC's from STREAMINFO, anything else from the container, made to agree
 * with the decoder (cb_audio_container_samples()). Tags are the stream's then the
 * container's, keys lower-cased, a container tag replacing a stream tag of
 * the same key (dj filled a dict in that order). Values are stored trimmed
 * of white space the way Python's str.strip() trims it, since dj only ever
 * read them through Probe.tag(), which stripped them. */

#include "cyanbox/audio.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>

#include "internal.h"

/* ------------------------------------------------------------------ logging */

/* libav's callback. It may be called from any thread at once; everything
 * here is on the stack and cb_log serialises. A line arriving in pieces
 * (libav continues some without a newline) is logged piece by piece. */
static void av_log_line(void *avcl, int level, const char *fmt, va_list args)
{
    level &= 0xff;                              /* the colour bits */
    if (level > av_log_get_level())
        return;
    enum CBLogLevel rl = level <= AV_LOG_FATAL ? CB_LOG_ERROR :
                         level <= AV_LOG_WARNING ? CB_LOG_WARN : CB_LOG_DEBUG;
    if (rl > cb_log_level())
        return;

    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
        msg[--n] = '\0';
    if (!n)
        return;
    const AVClass *cls = avcl ? *(const AVClass **)avcl : NULL;
    const char *name = cls && cls->item_name ? cls->item_name(avcl) : NULL;
    if (name)
        cb_log(rl, "libav: %s: %s", name, msg);
    else
        cb_log(rl, "libav: %s", msg);
}

static void set_log_callback(void)
{
    av_log_set_callback(av_log_line);
}

void cb_audio_init_logging(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, set_log_callback);
}

/* ------------------------------------------------------------------ helpers */

int cb_audio_error(int averr)
{
    if (averr >= 0)
        return averr;
    if (averr > -4096)
        return averr;
    switch (averr) {
    case AVERROR_EOF:
        return CB_ERROR(ENODATA);
    case AVERROR_INVALIDDATA:
        return CB_ERROR(EINVAL);
    case AVERROR_DECODER_NOT_FOUND:
    case AVERROR_DEMUXER_NOT_FOUND:
    case AVERROR_ENCODER_NOT_FOUND:
    case AVERROR_MUXER_NOT_FOUND:
    case AVERROR_PROTOCOL_NOT_FOUND:
    case AVERROR_PATCHWELCOME:
        return CB_ERROR(ENOTSUP);
    default:
        return CB_ERROR(EIO);
    }
}

size_t cb_audio_skip_id3v2(const uint8_t *data, size_t size)
{
    size_t pos = 0;
    while (size - pos >= 10 && !memcmp(data + pos, "ID3", 3)) {
        size_t len = 0;
        for (int i = 6; i < 10; i++)
            len = (len << 7) | (data[pos + i] & 0x7F);
        size_t next = pos + 10 + len + ((data[pos + 5] & 0x10) ? 10 : 0);
        if (next > size)
            return size;
        pos = next;
    }
    return pos;
}

int cb_audio_first_stream(const AVFormatContext *fmt, enum AVMediaType type)
{
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == type)
            return (int)i;
    return -1;
}

bool cb_audio_is_container(const AVFormatContext *fmt, const char *name)
{
    const char *list = fmt->iformat->name;
    size_t n = strcspn(list, ",");
    return strlen(name) == n && !strncmp(list, name, n);
}

/* --------------------------------------------------------------------- tags */

/* Length of the Unicode white space character UTF-8 coded at s (one that
 * Python's str.isspace() accepts), else 0. */
static int space_len(const uint8_t *s, size_t n)
{
    if ((s[0] >= 0x09 && s[0] <= 0x0D) || (s[0] >= 0x1C && s[0] <= 0x20))
        return 1;
    if (n >= 2 && s[0] == 0xC2 && (s[1] == 0x85 || s[1] == 0xA0))
        return 2;
    if (n < 3)
        return 0;
    uint32_t c = (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
    if ((s[0] & 0xF0) != 0xE0 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
        return 0;
    if (c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
        c == 0x202F || c == 0x205F || c == 0x3000)
        return 3;
    return 0;
}

/* Trims in place; returns the start of the trimmed string inside s. */
static char *trim(char *s)
{
    size_t n = strlen(s);
    uint8_t *b = (uint8_t *)s;
    size_t lo = 0;
    while (lo < n) {
        int l = space_len(b + lo, n - lo);
        if (!l)
            break;
        lo += (size_t)l;
    }
    size_t hi = n;
    while (hi > lo) {
        /* Step back over one UTF-8 character */
        size_t c = hi - 1;
        while (c > lo && (b[c] & 0xC0) == 0x80)
            c--;
        int l = space_len(b + c, hi - c);
        if (!l || c + (size_t)l != hi)
            break;
        hi = c;
    }
    s[hi] = '\0';
    return s + lo;
}

static int add_tags(CBProbe *p, const AVDictionary *dict)
{
    const AVDictionaryEntry *e = NULL;
    while ((e = av_dict_iterate(dict, e))) {
        char *key = cb_strdup(e->key);
        char *raw = cb_strdup(e->value);
        if (!key || !raw) {
            free(key);
            free(raw);
            return CB_ERROR(ENOMEM);
        }
        for (char *c = key; *c; c++)
            *c = (char)av_tolower(*c);
        char *value = cb_strdup(trim(raw));
        free(raw);
        if (!value) {
            free(key);
            return CB_ERROR(ENOMEM);
        }

        int i = 0;
        while (i < p->nb_tags && strcmp(p->tags[i].key, key))
            i++;
        if (i < p->nb_tags) {
            free(key);
            free(p->tags[i].value);
            p->tags[i].value = value;
            continue;
        }
        CBTag *nt = realloc(p->tags, ((size_t)p->nb_tags + 1) * sizeof(*nt));
        if (!nt) {
            free(key);
            free(value);
            return CB_ERROR(ENOMEM);
        }
        p->tags = nt;
        p->tags[p->nb_tags++] = (CBTag) { .key = key, .value = value };
    }
    return 0;
}

const char *cb_probe_tag(const CBProbe *p, const char *name, ...)
{
    va_list args;
    va_start(args, name);
    const char *found = NULL;
    for (const char *n = name; n && !found; n = va_arg(args, const char *)) {
        for (int i = 0; i < p->nb_tags; i++) {
            if (!cb_strcasecmp(p->tags[i].key, n)) {
                if (p->tags[i].value[0])
                    found = p->tags[i].value;
                break;
            }
        }
    }
    va_end(args);
    return found;
}

/* -------------------------------------------------------------------- probe */

static int64_t to_samples(int64_t ts, AVRational tb, int rate)
{
    return av_rescale_q(ts, tb, (AVRational) { 1, rate });
}

/* The decoded length of anything but MP3 and FLAC files, from what the
 * container says, made to agree with cb_decode (checked in test_decode on
 * files ffmpeg makes, and on the user's library):
 *
 * - MP4/M4A (AAC, ALAC, Opus): the stream duration already leaves out the
 *   priming the edit list skips, which is what the decoder drops.
 * - Ogg: the duration counts from granule 0, so Opus's pre-skip (initial
 *   padding) is in it, and Vorbis's first packet starts before 0 (a
 *   negative start time); the decoder drops both.
 * - A duration libavformat only estimated from the bitrate (ADTS AAC and
 *   other raw streams; off by a factor of three on a VBR AAC here): the
 *   packet durations, summed over the whole file. Nothing is trimmed there.
 * - WAV, AIFF, everything else: the stream duration as it is. */
int64_t cb_audio_container_samples(AVFormatContext *fmt, int idx)
{
    const AVStream *st = fmt->streams[idx];
    const AVCodecParameters *par = st->codecpar;
    int rate = par->sample_rate;

    if (fmt->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE) {
        AVPacket *pkt = av_packet_alloc();
        if (pkt) {
            int64_t total = 0;
            while (av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == idx && pkt->duration > 0)
                    total += to_samples(pkt->duration, st->time_base, rate);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
            if (total > 0)
                return total;
        }
    }

    int64_t total = 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        total = to_samples(st->duration, st->time_base, rate);
    else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        total = av_rescale(fmt->duration, rate, AV_TIME_BASE);

    if (cb_audio_is_container(fmt, "ogg")) {
        if (st->start_time != AV_NOPTS_VALUE && st->start_time < 0)
            total += to_samples(st->start_time, st->time_base, rate);
        total -= par->initial_padding;
    }
    return CB_MAX(total, (int64_t)0);
}

int cb_probe(const char *path, CBProbe *p)
{
    cb_audio_init_logging();
    *p = (CBProbe) { 0 };

    AVFormatContext *fmt;
    int ret = cb_audio_open(path, &fmt, true);
    if (ret < 0)
        return ret;

    int idx = cb_audio_first_stream(fmt, AVMEDIA_TYPE_AUDIO);
    if (idx < 0) {
        cb_debug("%s: no audio stream", path);
        ret = CB_ERROR(ENODATA);
        goto fail;
    }
    const AVStream *st = fmt->streams[idx];
    const AVCodecParameters *par = st->codecpar;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
    /* libavformat guesses a format from the extension alone; a stream it
     * could not find a rate or channels for is not audio we can play */
    if (par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0) {
        cb_debug("%s: no decodable audio", path);
        ret = CB_ERROR(ENODATA);
        goto fail;
    }

    snprintf(p->codec, sizeof(p->codec), "%s", desc ? desc->name : "none");
    snprintf(p->container, sizeof(p->container), "%.*s",
             (int)strcspn(fmt->iformat->name, ","), fmt->iformat->name);
    p->sample_rate = par->sample_rate;
    p->channels = par->ch_layout.nb_channels;
    int64_t br = fmt->bit_rate > 0 ? fmt->bit_rate : par->bit_rate;
    p->bit_rate = (int)CB_CLAMP(br, (int64_t)0, (int64_t)INT32_MAX);

    /* ffprobe's bits_per_raw_sample, else its bits_per_sample (which is
     * av_get_bits_per_sample() of the codec); never a depth for lossy codecs */
    bool lossy = desc && (desc->props & AV_CODEC_PROP_LOSSY) &&
                 !(desc->props & AV_CODEC_PROP_LOSSLESS);
    if (!lossy)
        p->bit_depth = par->bits_per_raw_sample ? par->bits_per_raw_sample
                                                : av_get_bits_per_sample(par->codec_id);

    ret = add_tags(p, st->metadata);
    if (ret >= 0)
        ret = add_tags(p, fmt->metadata);
    if (ret < 0)
        goto fail;

    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVStream *s = fmt->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (s->disposition & AV_DISPOSITION_ATTACHED_PIC))
            p->has_artwork = true;
    }
    /* Else the cover beside it that the deck and the stick will use
     * (cb_artwork_find_file), so the interface shows it too and not a
     * placeholder for an Opus or WAV in a folder with a front.jpg: a
     * directory listing and the first bytes of a candidate or two. */
    if (!p->has_artwork) {
        char *image;
        if (cb_artwork_find_file(path, &image) >= 0) {
            p->has_artwork = true;
            free(image);
        }
    }

    /* The players' timeline */
    p->total_samples = -1;
    if (par->codec_id == AV_CODEC_ID_MP3 && cb_audio_is_container(fmt, "mp3")) {
        if (cb_mp3_scan_mapped(path, cb_audio_input_map(fmt), &p->mp3) >= 0 &&
            p->mp3.nb_frames > 0) {
            int64_t frames = p->mp3.nb_frames + p->mp3.header_counts - p->mp3.skip_frames;
            p->total_samples = CB_MAX(frames, (int64_t)0) * p->mp3.frame_samples;
            if (p->mp3.sample_rate != p->sample_rate)
                cb_warn("%s: frame scan says %d Hz, the stream %d Hz", path,
                        p->mp3.sample_rate, p->sample_rate);
        }
    } else if (par->codec_id == AV_CODEC_ID_FLAC && cb_audio_is_container(fmt, "flac")) {
        if (cb_flac_scan_mapped(path, cb_audio_input_map(fmt), &p->flac) < 0)
            cb_warn("%s: FLAC frame scan failed, no seek table", path);
        if (p->flac.total_samples > 0)
            p->total_samples = p->flac.total_samples;
    }
    if (p->total_samples < 0)
        p->total_samples = cb_audio_container_samples(fmt, idx);
    p->duration = p->sample_rate > 0 ? (double)p->total_samples / p->sample_rate : 0.0;

    cb_audio_close(&fmt);
    return 0;

fail:
    cb_audio_close(&fmt);
    cb_probe_free(p);
    return ret;
}

void cb_probe_free(CBProbe *p)
{
    for (int i = 0; i < p->nb_tags; i++) {
        free(p->tags[i].key);
        free(p->tags[i].value);
    }
    free(p->tags);
    p->tags = NULL;
    p->nb_tags = 0;
    cb_flac_info_free(&p->flac);
}
