/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Probing a file with libavformat, as dj's audio.probe did with ffprobe, and
 * the helpers the rest of the module shares (opening, mapping, errors).
 *
 * Durations are on the players' timeline: an MP3's comes from the frame scan
 * (exact, the silent header frame included when rekordbox counts it), a
 * FLAC's from STREAMINFO, anything else from the container; the decoder
 * gives the exact count of those later. Tags are the stream's then the
 * container's, keys lower-cased, a container tag replacing a stream tag of
 * the same key (dj filled a dict in that order). Values are stored trimmed
 * of white space the way Python's str.strip() trims it, since dj only ever
 * read them through Probe.tag(), which stripped them. */

#include "rxbox/audio.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>

#include "internal.h"

/* ------------------------------------------------------------------ helpers */

int rx_audio_error(int averr)
{
    if (averr >= 0)
        return averr;
    if (averr > -4096)
        return averr;
    switch (averr) {
    case AVERROR_EOF:
        return RX_ERROR(ENODATA);
    case AVERROR_INVALIDDATA:
        return RX_ERROR(EINVAL);
    case AVERROR_DECODER_NOT_FOUND:
    case AVERROR_DEMUXER_NOT_FOUND:
    case AVERROR_ENCODER_NOT_FOUND:
    case AVERROR_MUXER_NOT_FOUND:
    case AVERROR_PROTOCOL_NOT_FOUND:
    case AVERROR_PATCHWELCOME:
        return RX_ERROR(ENOTSUP);
    default:
        return RX_ERROR(EIO);
    }
}

int rx_audio_map(const char *path, RxMap *m)
{
    *m = (RxMap) { 0 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return RX_ERROR(errno);
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        return RX_ERROR(err);
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return RX_ERROR(EINVAL);
    }
    if (st.st_size > 0) {
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            int err = errno;
            close(fd);
            return RX_ERROR(err);
        }
        madvise(p, (size_t)st.st_size, MADV_SEQUENTIAL);
        m->data = p;
        m->size = (size_t)st.st_size;
    }
    close(fd);
    return 0;
}

void rx_audio_unmap(RxMap *m)
{
    if (m->data)
        munmap((void *)m->data, m->size);
    *m = (RxMap) { 0 };
}

size_t rx_audio_skip_id3v2(const uint8_t *data, size_t size)
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

int rx_audio_open(const char *path, AVFormatContext **out, bool find_info)
{
    *out = NULL;
    char *url = rx_asprintf("file:%s", path);
    if (!url)
        return RX_ERROR(ENOMEM);

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, url, NULL, NULL);
    free(url);
    if (ret < 0) {
        rx_debug("%s: %s", path, av_err2str(ret));
        return rx_audio_error(ret);
    }
    if (find_info) {
        ret = avformat_find_stream_info(fmt, NULL);
        if (ret < 0) {
            rx_debug("%s: no stream info: %s", path, av_err2str(ret));
            avformat_close_input(&fmt);
            return rx_audio_error(ret);
        }
    }
    *out = fmt;
    return 0;
}

int rx_audio_first_stream(const AVFormatContext *fmt, enum AVMediaType type)
{
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == type)
            return (int)i;
    return -1;
}

bool rx_audio_is_container(const AVFormatContext *fmt, const char *name)
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

static int add_tags(RxProbe *p, const AVDictionary *dict)
{
    const AVDictionaryEntry *e = NULL;
    while ((e = av_dict_iterate(dict, e))) {
        char *key = rx_strdup(e->key);
        char *raw = rx_strdup(e->value);
        if (!key || !raw) {
            free(key);
            free(raw);
            return RX_ERROR(ENOMEM);
        }
        for (char *c = key; *c; c++)
            *c = (char)av_tolower(*c);
        char *value = rx_strdup(trim(raw));
        free(raw);
        if (!value) {
            free(key);
            return RX_ERROR(ENOMEM);
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
        RxTag *nt = realloc(p->tags, ((size_t)p->nb_tags + 1) * sizeof(*nt));
        if (!nt) {
            free(key);
            free(value);
            return RX_ERROR(ENOMEM);
        }
        p->tags = nt;
        p->tags[p->nb_tags++] = (RxTag) { .key = key, .value = value };
    }
    return 0;
}

const char *rx_probe_tag(const RxProbe *p, const char *name, ...)
{
    va_list args;
    va_start(args, name);
    const char *found = NULL;
    for (const char *n = name; n && !found; n = va_arg(args, const char *)) {
        for (int i = 0; i < p->nb_tags; i++) {
            if (!rx_strcasecmp(p->tags[i].key, n)) {
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

int rx_probe(const char *path, RxProbe *p)
{
    *p = (RxProbe) { 0 };

    AVFormatContext *fmt;
    int ret = rx_audio_open(path, &fmt, true);
    if (ret < 0)
        return ret;

    int idx = rx_audio_first_stream(fmt, AVMEDIA_TYPE_AUDIO);
    if (idx < 0) {
        rx_debug("%s: no audio stream", path);
        ret = RX_ERROR(ENODATA);
        goto fail;
    }
    const AVStream *st = fmt->streams[idx];
    const AVCodecParameters *par = st->codecpar;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
    /* libavformat guesses a format from the extension alone; a stream it
     * could not find a rate or channels for is not audio we can play */
    if (par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0) {
        rx_debug("%s: no decodable audio", path);
        ret = RX_ERROR(ENODATA);
        goto fail;
    }

    snprintf(p->codec, sizeof(p->codec), "%s", desc ? desc->name : "none");
    snprintf(p->container, sizeof(p->container), "%.*s",
             (int)strcspn(fmt->iformat->name, ","), fmt->iformat->name);
    p->sample_rate = par->sample_rate;
    p->channels = par->ch_layout.nb_channels;
    int64_t br = fmt->bit_rate > 0 ? fmt->bit_rate : par->bit_rate;
    p->bit_rate = (int)RX_CLAMP(br, (int64_t)0, (int64_t)INT32_MAX);

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

    /* The players' timeline */
    p->total_samples = -1;
    if (par->codec_id == AV_CODEC_ID_MP3 && rx_audio_is_container(fmt, "mp3")) {
        if (rx_mp3_scan(path, &p->mp3) >= 0 && p->mp3.nb_frames > 0) {
            int64_t frames = p->mp3.nb_frames + p->mp3.header_counts - p->mp3.skip_frames;
            p->total_samples = RX_MAX(frames, (int64_t)0) * p->mp3.frame_samples;
            if (p->mp3.sample_rate != p->sample_rate)
                rx_warn("%s: frame scan says %d Hz, the stream %d Hz", path,
                        p->mp3.sample_rate, p->sample_rate);
        }
    } else if (par->codec_id == AV_CODEC_ID_FLAC && rx_audio_is_container(fmt, "flac")) {
        if (rx_flac_scan(path, &p->flac) < 0)
            rx_warn("%s: FLAC frame scan failed, no seek table", path);
        if (p->flac.total_samples > 0)
            p->total_samples = p->flac.total_samples;
    }
    if (p->total_samples < 0) {
        double d = 0.0;
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
            d = (double)st->duration * av_q2d(st->time_base);
        else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
            d = (double)fmt->duration / AV_TIME_BASE;
        p->total_samples = llround(d * p->sample_rate);
        /* The decoder drops the encoder's priming (Opus pre-skip) that the
         * container's duration still counts; an MP3 elsewhere keeps it */
        if (par->codec_id != AV_CODEC_ID_MP3)
            p->total_samples = RX_MAX(p->total_samples - par->initial_padding, (int64_t)0);
    }
    p->duration = p->sample_rate > 0 ? (double)p->total_samples / p->sample_rate : 0.0;

    avformat_close_input(&fmt);
    return 0;

fail:
    avformat_close_input(&fmt);
    rx_probe_free(p);
    return ret;
}

void rx_probe_free(RxProbe *p)
{
    for (int i = 0; i < p->nb_tags; i++) {
        free(p->tags[i].key);
        free(p->tags[i].value);
    }
    free(p->tags);
    p->tags = NULL;
    p->nb_tags = 0;
    rx_flac_info_free(&p->flac);
}
