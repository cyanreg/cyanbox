/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The packet store a deck plays from (cyanbox/audio.h): a track demuxed into
 * memory without decoding, every packet with its place on the players'
 * timeline, and a decoder that gives any stretch of it, sample for sample
 * what cb_decode gives for the same frames.
 *
 * Indexing reads the file once, in steps, so a deck can start long before a
 * long file is indexed: cb_packets_open indexes the first packets and the
 * rest follows from cb_packets_index (or on demand, from a decode that
 * needs it). Packet bytes are copied into one arena of the file's size
 * (padding and metadata excluded, so less), allocated at open and filled
 * as indexing goes (pages untouched are not resident), so nothing moves.
 *
 * The timeline is cb_decode's, so the same three routes:
 *
 * - MP3 files: the frames cb_mp3_scan counts, as decode.c feeds them, with
 *   the leading silent frame when rekordbox counts the header frame (no
 *   packet covers it: a decode there gives silence) and the frames before a
 *   restart of the frame map placed before 0. The whole-file scan runs at
 *   open, for the exact length; it walks 5 GB/s of headers (1.6 ms for an
 *   8 MB file here). Indexing then copies the frames.
 * - FLAC files: the frames cb_flac_scan finds (sync code, header CRC-8 and
 *   the expected number), placed at their first sample. libavformat's own
 *   FLAC demuxer runs its parser over every byte: 50 to 110 ms for a 23 MB
 *   file and 0.6 to 1.1 s for a 282 MB one on this machine, against 9 and
 *   128 ms for the walk. The decoder gets STREAMINFO as extradata, as the
 *   demuxer gives it. A file the walk cannot read (no STREAMINFO length)
 *   goes through libavformat.
 * - Everything else: libavformat's packets. A packet's raw start is the
 *   first packet's pts plus the durations before it, and what libavcodec
 *   drops from its output (the stream's initial padding at open, the skip
 *   and discard side data, AV_PKT_FLAG_DISCARD) is followed packet by
 *   packet the way decode.c's discard_samples() does it, so the decoded
 *   length is known without decoding. Durations rather than pts after the
 *   first: they are exact sample counts, and the first pts is what places
 *   Vorbis's first packet (pts -128, duration 128) before 0, where the
 *   decoder gives nothing for it. Checked against cb_decode on Opus, Vorbis,
 *   AAC (LC and HE, with their edit lists), ADTS, WAV and AIFF in
 *   tests/audio/test_packets.c. Matroska keeps milliseconds only and is
 *   refused (-ENOTSUP: decode it whole); so is a stream whose packets carry
 *   no durations.
 *
 * Decoding a stretch that does not follow the last one starts again: a new
 * decoder (libavcodec keeps its pending skip across a flush), opened with
 * no initial padding unless it starts at the first packet, fed from a
 * packet early enough for the codec's state to settle, and every decoded
 * frame is placed by its pts (each packet is sent with its raw start), so
 * whatever the decoder drops at its start lands where it belongs. The
 * pre-roll, measured against cb_decode at 200 random places per file (the
 * largest sample difference after a jump):
 *
 * - none for lossless codecs that code every packet alone (FLAC, PCM,
 *   ALAC): exact.
 * - MP3: two frames and 1.5 kB, for the bit reservoir (a frame's data may
 *   start 511 bytes back, and the IMDCT overlap needs the frame before it
 *   whole): exact, on a 128 kbit/s sine and a real 5-minute track.
 * - Vorbis: two packets (the first after a reset only primes the overlap):
 *   exact.
 * - AAC: three packets. LC is exact from one (a real track and a sine);
 *   HE-AAC's SBR is not, from any number: 2.7e-4 at three, 4e-5 at twenty.
 * - Opus: 640 ms. RFC 7845's 80 ms is not enough for libavcodec's decoder
 *   to come out as a continuous decode does: on a real track 0.117 (a
 *   click) at 80 ms, 3.6e-5 at 320, 1.2e-7 at 640, 0 at 1000. 640 ms is 32
 *   packets, decoded in a millisecond or two.
 *
 * tests/audio/test_packets.c checks the same on files it makes. */

#include "cyanbox/audio.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libavutil/intreadwrite.h>

#include "internal.h"

/* Packets cb_packets_open indexes before it returns. */
#define OPEN_PACKETS 64
/* Packets a decode that runs past the index indexes at a time. */
#define DEMAND_PACKETS 256
/* Opus's pre-roll (see the top). */
#define OPUS_PREROLL_MS 640

enum Source {
    SRC_MP3,
    SRC_FLAC,
    SRC_LAVF,
};

typedef struct Packet {
    int64_t raw;            /* timeline frame of its first decoded sample, before any skip */
    int64_t offset;         /* into the arena */
    int32_t size;
    int32_t dur;            /* samples it decodes to */
    int32_t skip;           /* of those, what libavcodec drops from the start */
    int32_t out;            /* and what is left after both ends */
    int32_t side_skip, side_disc;   /* AV_PKT_DATA_SKIP_SAMPLES it came with */
    bool    discard;        /* AV_PKT_FLAG_DISCARD */
} Packet;

struct CBPackets {
    char   *path;
    enum Source src;
    AVCodecParameters *par;
    int     rate, channels;
    bool    skip_manual;    /* MP3: libavcodec keeps encoder delay and padding */
    int     delay;          /* what a decoder opened as cb_decode's drops first */
    int     pre_packets;    /* pre-roll (see the top) */
    int64_t pre_samples, pre_bytes;

    Packet  *pk;
    int64_t  nb, cap;
    uint8_t *arena;
    size_t   used, arena_cap;
    bool     done;
    bool     anchored;
    int64_t  next_raw;      /* raw start of the next packet */
    int64_t  skip_left;     /* libavcodec's pending skip at the next packet */
    int64_t  end;           /* where the output decoded so far ends */
    int32_t  last_dur;
    bool     dur_warned;

    _Atomic int64_t length;
    atomic_bool     exact;

    CBMap      map;         /* MP3, FLAC: while indexing */
    size_t     released;    /* its pages before this are out of the process */
    CBMp3Info  mp3;
    size_t     mp3_pos;
    int64_t    mp3_left;    /* frames still to come, for the step's count */
    CBFlacWalk flac;
    size_t     flac_at;     /* the frame found last, waiting for the next one's offset */
    int64_t    flac_sample;
    int        flac_block;
    bool       flac_pending;
    AVFormatContext *fmt;   /* the rest: while indexing */
    int        idx;
    AVPacket  *rd;
};

/* ------------------------------------------------------------------- index */

static int append(CBPackets *pk, const uint8_t *data, size_t size, int64_t raw, int dur,
                  int side_skip, int side_disc, bool discard)
{
    if (size > INT32_MAX || pk->arena_cap - pk->used < size) {
        cb_warn("%s: more packet data than the file holds; the track ends at %.1f s", pk->path,
                (double)pk->end / pk->rate);
        return CB_ERROR(EOVERFLOW);
    }
    if (pk->nb == pk->cap) {
        int64_t cap = pk->cap ? pk->cap * 2 : 1024;
        Packet *n = realloc(pk->pk, (size_t)cap * sizeof(*n));
        if (!n)
            return CB_ERROR(ENOMEM);
        pk->pk = n;
        pk->cap = cap;
    }

    /* What libavcodec drops (decode.c, discard_samples): a skip in the side
     * data replaces the pending one; a discarded packet's whole output goes
     * and counts against the skip; then the skip, then the discard padding
     * if it fits what is left. */
    if (side_skip > 0)
        pk->skip_left = side_skip;
    int64_t skip, out;
    if (discard) {
        skip = dur;
        out = 0;
        pk->skip_left = CB_MAX(pk->skip_left - dur, (int64_t)0);
    } else {
        skip = CB_MIN((int64_t)dur, pk->skip_left);
        pk->skip_left -= skip;
        out = dur - skip;
        if (side_disc > 0 && side_disc <= out)
            out -= side_disc;
    }

    memcpy(pk->arena + pk->used, data, size);
    pk->pk[pk->nb++] = (Packet) {
        .raw       = raw,
        .offset    = (int64_t)pk->used,
        .size      = (int32_t)size,
        .dur       = dur,
        .skip      = (int32_t)skip,
        .out       = (int32_t)out,
        .side_skip = side_skip,
        .side_disc = side_disc,
        .discard   = discard,
    };
    pk->used += size;
    pk->next_raw = raw + dur;
    pk->end = CB_MAX(pk->end, raw + skip + out);
    return 0;
}

static void finish(CBPackets *pk)
{
    pk->done = true;
    int64_t was = atomic_load_explicit(&pk->length, memory_order_relaxed);
    if (was != pk->end)
        cb_debug("%s: %" PRId64 " frames indexed, the container said %" PRId64, pk->path,
                 pk->end, was);
    atomic_store_explicit(&pk->length, pk->end, memory_order_relaxed);
    atomic_store_explicit(&pk->exact, true, memory_order_release);
    cb_audio_unmap(&pk->map);
    av_packet_free(&pk->rd);
    avformat_close_input(&pk->fmt);
}

/* MP3: frames go in as the walk finds them. */
static int mp3_frame(void *opaque, const uint8_t *data, size_t size, int samples, int kbps)
{
    CBPackets *pk = opaque;
    int ret = append(pk, data, size, pk->next_raw, samples, 0, 0, false);
    if (ret < 0)
        return ret;
    return --pk->mp3_left > 0 ? 0 : 1;
}

/* The mapped file's pages before `upto` have been copied: they leave the
 * process (MADV_DONTNEED on a read-only file mapping drops them from the
 * page tables, the page cache keeps them), or a long file would count
 * twice while it is indexed, mapped and copied. */
static void release_map(CBPackets *pk, size_t upto)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t end = upto / page * page;
    if (end > pk->released && end <= pk->map.size) {
        madvise((void *)(pk->map.data + pk->released), end - pk->released, MADV_DONTNEED);
        pk->released = end;
    }
}

static int index_mp3(CBPackets *pk, int max)
{
    pk->mp3_left = max;
    int ret = cb_mp3_frames_step(pk->path, &pk->map, &pk->mp3, &pk->mp3_pos, mp3_frame, pk);
    release_map(pk, pk->mp3_pos);
    return ret < 0 ? ret : ret == 1;
}

/* FLAC: a frame's bytes end where the next frame starts, so each waits for
 * the next; the last ends with the file, less a trailing ID3v1 or APEv2 tag
 * (the decoder would try them as a frame and say so). */
static size_t flac_end(const CBPackets *pk)
{
    const uint8_t *d = pk->map.data;
    size_t size = pk->map.size;
    if (size >= 128 && !memcmp(d + size - 128, "TAG", 3))
        size -= 128;
    if (size >= 32 && !memcmp(d + size - 32, "APETAGEX", 8)) {
        uint32_t len = AV_RL32(d + size - 20);
        bool header = AV_RL32(d + size - 12) & (1U << 31);
        size_t tag = (size_t)len + (header ? 32 : 0);
        if (tag <= size)
            size -= tag;
    }
    return size;
}

static int index_flac(CBPackets *pk, int max)
{
    for (int n = 0; n < max; n++) {
        size_t at;
        int64_t sample;
        int block;
        bool complete = false;
        int got = cb_flac_walk_next(&pk->flac, &at, &sample, &block, &complete);
        if (pk->flac_pending) {
            size_t end = got > 0 ? at : flac_end(pk);
            if (end > pk->flac_at) {
                int ret = append(pk, pk->map.data + pk->flac_at, end - pk->flac_at,
                                 pk->flac_sample, pk->flac_block, 0, 0, false);
                if (ret < 0)
                    return ret;
            }
            pk->flac_pending = false;
        }
        if (got <= 0) {
            if (!complete)
                cb_warn("%s: the FLAC frames end at %.1f s, before STREAMINFO's length",
                        pk->path, (double)pk->flac.sample / pk->rate);
            return 1;
        }
        pk->flac_at = at;
        pk->flac_sample = sample;
        pk->flac_block = block;
        pk->flac_pending = true;
    }
    release_map(pk, pk->flac_at);
    return 0;
}

/* libavformat: one av_read_frame per packet. */
static int index_lavf(CBPackets *pk, int max)
{
    AVStream *st = pk->fmt->streams[pk->idx];
    AVRational samples = { 1, pk->rate };
    for (int n = 0; n < max;) {
        int ret = av_read_frame(pk->fmt, pk->rd);
        if (ret == AVERROR_EOF)
            return 1;
        if (ret < 0) {
            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EIO)) {
                cb_err("%s: %s", pk->path, av_err2str(ret));
                return cb_audio_error(ret);
            }
            cb_warn("%s: demuxing stopped: %s", pk->path, av_err2str(ret));
            return 1;
        }
        AVPacket *p = pk->rd;
        if (p->stream_index != pk->idx) {
            av_packet_unref(p);
            continue;
        }

        int64_t dur = p->duration > 0 ? av_rescale_q(p->duration, st->time_base, samples) : 0;
        if (dur <= 0 || dur > INT32_MAX) {
            /* cb_decode's silence for a packet of unknown length */
            dur = pk->par->frame_size > 0 ? pk->par->frame_size : pk->last_dur;
            if (!pk->dur_warned)
                cb_debug("%s: a packet without a duration, taken as %" PRId64 " samples",
                         pk->path, dur);
            pk->dur_warned = true;
        }
        if (!pk->anchored) {
            pk->next_raw = p->pts != AV_NOPTS_VALUE ? av_rescale_q(p->pts, st->time_base, samples)
                                                    : 0;
            pk->anchored = true;
        }
        size_t sd_size;
        const uint8_t *sd = av_packet_get_side_data(p, AV_PKT_DATA_SKIP_SAMPLES, &sd_size);
        int side_skip = sd && sd_size >= 10 ? (int)AV_RL32(sd) : 0;
        int side_disc = sd && sd_size >= 10 ? (int)AV_RL32(sd + 4) : 0;
        ret = append(pk, p->data, (size_t)p->size, pk->next_raw, (int)dur, side_skip, side_disc,
                     p->flags & AV_PKT_FLAG_DISCARD);
        av_packet_unref(p);
        if (ret < 0)
            return ret;
        pk->last_dur = (int32_t)dur;
        n++;
    }
    return 0;
}

int cb_packets_index(CBPackets *pk, int max_packets)
{
    if (pk->done)
        return 1;
    int ret;
    switch (pk->src) {
    case SRC_MP3:
        ret = index_mp3(pk, CB_MAX(max_packets, 1));
        break;
    case SRC_FLAC:
        ret = index_flac(pk, CB_MAX(max_packets, 1));
        break;
    default:
        ret = index_lavf(pk, CB_MAX(max_packets, 1));
        break;
    }
    /* An error ends the index where it is: what came before still plays. */
    if (ret != 0)
        finish(pk);
    return ret;
}

/* -------------------------------------------------------------------- open */

static int arena_alloc(CBPackets *pk, size_t size)
{
    pk->arena_cap = size;
    pk->arena = malloc(CB_MAX(size, (size_t)1));
    return pk->arena ? 0 : CB_ERROR(ENOMEM);
}

/* 1 when this is not an MP3 the scanner reads (libavformat then). */
static int open_mp3(CBPackets *pk)
{
    if (cb_audio_map(pk->path, &pk->map) < 0)
        return 1;
    if (cb_mp3_scan_mapped(pk->path, &pk->map, &pk->mp3) < 0 || !pk->mp3.nb_frames) {
        cb_audio_unmap(&pk->map);
        return 1;
    }
    /* The scan touched every page; indexing touches them again in order
     * (release_map) */
    if (pk->map.size)
        madvise((void *)pk->map.data, pk->map.size, MADV_DONTNEED);
    const CBMp3Info *mi = &pk->mp3;
    pk->src = SRC_MP3;
    pk->par->codec_type = AVMEDIA_TYPE_AUDIO;
    pk->par->codec_id = AV_CODEC_ID_MP3;
    pk->par->sample_rate = mi->sample_rate;
    av_channel_layout_default(&pk->par->ch_layout, mi->channels);
    pk->skip_manual = true;
    /* decode.c: a silent frame first when rekordbox counts the header
     * frame, and the frames before the map's restart dropped */
    pk->next_raw = (int64_t)(mi->header_counts - mi->skip_frames) * mi->frame_samples;
    pk->cap = mi->nb_frames;
    pk->pk = malloc((size_t)CB_MAX(pk->cap, (int64_t)1) * sizeof(*pk->pk));
    if (!pk->pk)
        return CB_ERROR(ENOMEM);
    int64_t frames = mi->nb_frames + mi->header_counts - mi->skip_frames;
    atomic_store(&pk->length, CB_MAX(frames, (int64_t)0) * mi->frame_samples);
    return arena_alloc(pk, (size_t)(mi->audio_bytes > 0 ? mi->audio_bytes : 0));
}

static int open_flac(CBPackets *pk)
{
    if (cb_audio_map(pk->path, &pk->map) < 0)
        return 1;
    CBFlacInfo fi;
    const uint8_t *si;
    if (cb_flac_walk_init(&pk->flac, pk->path, pk->map.data, pk->map.size, &fi, &si) < 0 ||
        fi.total_samples <= 0 || fi.sample_rate <= 0) {
        cb_audio_unmap(&pk->map);
        return 1;
    }
    pk->src = SRC_FLAC;
    pk->par->codec_type = AVMEDIA_TYPE_AUDIO;
    pk->par->codec_id = AV_CODEC_ID_FLAC;
    pk->par->sample_rate = fi.sample_rate;
    pk->par->bits_per_raw_sample = fi.bit_depth;
    av_channel_layout_default(&pk->par->ch_layout, fi.channels);
    pk->par->extradata = av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!pk->par->extradata)
        return CB_ERROR(ENOMEM);
    memcpy(pk->par->extradata, si, 34);
    pk->par->extradata_size = 34;
    if (fi.max_block > 0) {
        pk->cap = fi.total_samples / CB_MAX(fi.min_block, 16) + 2;
        pk->cap = CB_MIN(pk->cap, (int64_t)1 << 24);
        pk->pk = malloc((size_t)pk->cap * sizeof(*pk->pk));
        if (!pk->pk)
            return CB_ERROR(ENOMEM);
    }
    atomic_store(&pk->length, fi.total_samples);
    return arena_alloc(pk, pk->map.size - (size_t)fi.first_frame_offset);
}

/* The decoded length a container states (probe.c's container_samples, less
 * the walk it does for a length estimated from the bitrate, which the
 * caller indexes whole instead): -1 when it states none. */
static int64_t container_length(AVFormatContext *fmt, int idx, int rate)
{
    const AVStream *st = fmt->streams[idx];
    AVRational samples = { 1, rate };
    int64_t total;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        total = av_rescale_q(st->duration, st->time_base, samples);
    else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        total = av_rescale(fmt->duration, rate, AV_TIME_BASE);
    else
        return -1;
    if (cb_audio_is_container(fmt, "ogg")) {
        if (st->start_time != AV_NOPTS_VALUE && st->start_time < 0)
            total += av_rescale_q(st->start_time, st->time_base, samples);
        total -= st->codecpar->initial_padding;
    }
    return CB_MAX(total, (int64_t)0);
}

static int open_lavf(CBPackets *pk, AVFormatContext *fmt)
{
    pk->src = SRC_LAVF;
    pk->fmt = fmt;
    pk->idx = cb_audio_first_stream(fmt, AVMEDIA_TYPE_AUDIO);
    if (pk->idx < 0) {
        cb_debug("%s: no audio stream", pk->path);
        return CB_ERROR(ENODATA);
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        fmt->streams[i]->discard = (int)i == pk->idx ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    AVStream *st = fmt->streams[pk->idx];
    if (cb_audio_is_container(fmt, "matroska")) {
        cb_debug("%s: Matroska times are in milliseconds, not samples", pk->path);
        return CB_ERROR(ENOTSUP);
    }

    /* Ogg, MP4, WAV and AIFF give the codec's parameters and the length in
     * their headers; raw streams (ADTS) need packets read for them. */
    bool probed = false;
    if (st->codecpar->sample_rate <= 0 || st->codecpar->ch_layout.nb_channels <= 0 ||
        st->codecpar->codec_id == AV_CODEC_ID_NONE) {
        int ret = avformat_find_stream_info(fmt, NULL);
        if (ret < 0) {
            cb_debug("%s: no stream info: %s", pk->path, av_err2str(ret));
            return cb_audio_error(ret);
        }
        probed = true;
    }
    if (st->codecpar->sample_rate <= 0 || st->codecpar->ch_layout.nb_channels <= 0) {
        cb_debug("%s: no decodable audio", pk->path);
        return CB_ERROR(ENODATA);
    }
    int ret = avcodec_parameters_copy(pk->par, st->codecpar);
    if (ret < 0)
        return cb_audio_error(ret);
    pk->skip_manual = st->codecpar->codec_id == AV_CODEC_ID_MP3;
    pk->rd = av_packet_alloc();
    if (!pk->rd)
        return CB_ERROR(ENOMEM);

    int64_t size = avio_size(fmt->pb);
    ret = arena_alloc(pk, size > 0 ? (size_t)size : (size_t)64 << 20);
    if (ret < 0)
        return ret;

    int64_t length = probed && fmt->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE
                     ? -1 : container_length(fmt, pk->idx, st->codecpar->sample_rate);
    atomic_store(&pk->length, CB_MAX(length, (int64_t)0));
    /* A length only estimated: indexed whole now (raw streams, small) */
    return length < 0 ? 2 : 0;
}

static enum AVCodecID codec_of(const CBPackets *pk)
{
    return pk->par->codec_id;
}

/* The pre-roll (see the top). */
static void set_preroll(CBPackets *pk)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codec_of(pk));
    int props = desc ? desc->props : 0;
    if ((props & AV_CODEC_PROP_INTRA_ONLY) && (props & AV_CODEC_PROP_LOSSLESS))
        return;
    switch (codec_of(pk)) {
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_MP2:
        pk->pre_packets = 2;
        pk->pre_bytes = 1536;
        break;
    case AV_CODEC_ID_VORBIS:
        pk->pre_packets = 2;
        break;
    case AV_CODEC_ID_OPUS:
        pk->pre_packets = 1;
        pk->pre_samples = (int64_t)pk->rate * OPUS_PREROLL_MS / 1000;
        break;
    case AV_CODEC_ID_AAC:
        pk->pre_packets = 3;
        break;
    default:
        pk->pre_packets = 2;
        pk->pre_samples = pk->rate / 10;
        break;
    }
}

int cb_packets_open(const char *path, CBPackets **out)
{
    cb_audio_init_logging();
    *out = NULL;
    CBPackets *pk = calloc(1, sizeof(*pk));
    if (!pk)
        return CB_ERROR(ENOMEM);
    atomic_init(&pk->length, 0);
    atomic_init(&pk->exact, false);
    pk->path = cb_strdup(path);
    pk->par = avcodec_parameters_alloc();
    int ret = CB_ERROR(ENOMEM);
    if (!pk->path || !pk->par)
        goto fail;

    /* cb_decode's choice of route: the scanners only for files libavformat
     * calls MP3 or FLAC */
    AVFormatContext *fmt;
    ret = cb_audio_open(path, &fmt, false);
    if (ret < 0)
        goto fail;
    ret = 1;
    if (cb_audio_is_container(fmt, "mp3") || cb_audio_is_container(fmt, "flac")) {
        bool mp3 = cb_audio_is_container(fmt, "mp3");
        avformat_close_input(&fmt);
        ret = mp3 ? open_mp3(pk) : open_flac(pk);
        if (ret < 0)
            goto fail;
        if (ret > 0) {
            avcodec_parameters_free(&pk->par);
            pk->par = avcodec_parameters_alloc();
            if (!pk->par) {
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
            ret = cb_audio_open(path, &fmt, false);
            if (ret < 0)
                goto fail;
            ret = 1;
        }
    }
    bool whole = false;
    if (ret > 0) {
        ret = open_lavf(pk, fmt);
        if (ret < 0) {
            if (!pk->fmt)
                avformat_close_input(&fmt);
            goto fail;
        }
        whole = ret == 2;
    }

    /* A decoder opened as cb_decode opens it says what it drops first and
     * how many channels come out (and that there is a decoder at all). */
    AVCodecContext *dec;
    ret = cb_audio_open_decoder(&dec, pk->par, (AVRational) { 1, pk->par->sample_rate },
                                pk->skip_manual, -1);
    if (ret < 0)
        goto fail;
    pk->rate = dec->sample_rate > 0 ? dec->sample_rate : pk->par->sample_rate;
    pk->channels = dec->ch_layout.nb_channels > 0 ? dec->ch_layout.nb_channels
                                                  : pk->par->ch_layout.nb_channels;
    pk->delay = dec->delay;
    pk->skip_left = dec->delay;
    avcodec_free_context(&dec);
    set_preroll(pk);

    do {
        ret = cb_packets_index(pk, whole ? 4096 : OPEN_PACKETS);
        if (ret < 0 && !pk->nb)
            goto fail;
    } while (whole && !pk->done);
    if (!pk->nb && pk->done) {
        cb_debug("%s: no audio packets", path);
        ret = CB_ERROR(ENODATA);
        goto fail;
    }
    *out = pk;
    return 0;

fail:
    cb_packets_free(pk);
    return ret;
}

void cb_packets_free(CBPackets *pk)
{
    if (!pk)
        return;
    cb_audio_unmap(&pk->map);
    av_packet_free(&pk->rd);
    avformat_close_input(&pk->fmt);
    avcodec_parameters_free(&pk->par);
    free(pk->pk);
    free(pk->arena);
    free(pk->path);
    free(pk);
}

int64_t cb_packets_length(const CBPackets *pk, bool *exact)
{
    CBPackets *p = (CBPackets *)pk;
    bool e = atomic_load_explicit(&p->exact, memory_order_acquire);
    if (exact)
        *exact = e;
    return atomic_load_explicit(&p->length, memory_order_relaxed);
}

int cb_packets_rate(const CBPackets *pk)
{
    return pk->rate;
}

int cb_packets_channels(const CBPackets *pk)
{
    return pk->channels;
}

size_t cb_packets_memory(const CBPackets *pk)
{
    return pk->used + (size_t)pk->nb * sizeof(*pk->pk);
}

/* ----------------------------------------------------------------- decoder */

struct CBPacketDecoder {
    CBPackets *pk;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame  *frame;
    CBMixer   mixer;
    uint8_t  *buf;          /* a packet's bytes and zeroed padding */
    size_t    buf_cap;
    int64_t   k;            /* the next packet to send */
    int64_t   next;         /* where a call that carries on starts; -1 none */
    bool      drained;
    /* The frame decoded last, converted, for a call that carries on */
    float    *held;
    size_t    held_cap;
    int64_t   held_pos;
    int       held_n;
    int64_t   cursor;       /* after the last output, for a frame without pts */
    bool      warned;       /* about samples that were not finite */
};

int cb_packets_decoder(CBPackets *pk, CBPacketDecoder **out)
{
    *out = NULL;
    CBPacketDecoder *d = calloc(1, sizeof(*d));
    if (!d)
        return CB_ERROR(ENOMEM);
    d->pk = pk;
    d->next = -1;
    d->mixer.out_ch = pk->channels;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) {
        cb_packets_decoder_free(d);
        return CB_ERROR(ENOMEM);
    }
    *out = d;
    return 0;
}

void cb_packets_decoder_free(CBPacketDecoder *d)
{
    if (!d)
        return;
    avcodec_free_context(&d->dec);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    cb_mixer_free(&d->mixer);
    free(d->buf);
    free(d->held);
    free(d);
}

/* The last packet whose raw start is at or before `frame` (0 when none is). */
static int64_t find_packet(const CBPackets *pk, int64_t frame)
{
    int64_t lo = 0, hi = pk->nb;
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        if (pk->pk[mid].raw <= frame)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

/* Indexes until the packet that holds `frame` and the one after it are in,
 * or the file ends. */
static int index_to(CBPackets *pk, int64_t frame)
{
    while (!pk->done && (pk->nb < 2 || pk->pk[pk->nb - 2].raw <= frame)) {
        int ret = cb_packets_index(pk, DEMAND_PACKETS);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int seek(CBPacketDecoder *d, int64_t start)
{
    CBPackets *pk = d->pk;
    int ret = index_to(pk, start);
    if (ret < 0 && !pk->nb)
        return ret;

    int64_t t = find_packet(pk, start), j = t, bytes = 0;
    while (j > 0 && (t - j < pk->pre_packets || start - pk->pk[j].raw < pk->pre_samples ||
                     bytes < pk->pre_bytes)) {
        j--;
        bytes += pk->pk[j].size;
    }

    /* A new decoder: one only flushed would keep a skip pending from
     * before. Its initial padding is dropped only from the first packet,
     * as cb_decode drops it. */
    avcodec_free_context(&d->dec);
    ret = cb_audio_open_decoder(&d->dec, pk->par, (AVRational) { 1, pk->rate }, pk->skip_manual,
                                j == 0 ? -1 : 0);
    if (ret < 0)
        return ret;
    d->k = j;
    d->held_n = 0;
    d->drained = false;
    d->cursor = pk->nb ? pk->pk[j].raw : 0;
    return 0;
}

/* Copies what of [pos, pos + n) (interleaved, pk->channels) lies in
 * [start, end) to dst (ch channels). */
static void place(const CBPacketDecoder *d, const float *x, int64_t pos, int n, int64_t start,
                  int64_t end, float *dst, int ch)
{
    int64_t a = CB_MAX(pos, start), b = CB_MIN(pos + n, end);
    int nc = d->pk->channels;
    for (int64_t i = a; i < b; i++) {
        const float *s = x + (i - pos) * nc;
        float *o = dst + (i - start) * ch;
        for (int c = 0; c < ch; c++)
            o[c] = s[c];
    }
}

static int receive(CBPacketDecoder *d, int64_t start, int64_t end, float *dst, int ch)
{
    while (1) {
        int ret = avcodec_receive_frame(d->dec, d->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            /* A damaged packet: silence where it was, as in cb_decode */
            if (ret == AVERROR(ENOMEM))
                return CB_ERROR(ENOMEM);
            return 0;
        }
        const float *x;
        int n = cb_mixer_frame(&d->mixer, d->frame, &x);
        int64_t pos = d->frame->pts != AV_NOPTS_VALUE ? d->frame->pts : d->cursor;
        av_frame_unref(d->frame);
        if (n < 0)
            return n;
        if (d->mixer.nonfinite && !d->warned) {
            /* Once: a seek decodes the same packets again. */
            cb_warn("%s: samples that are not finite (NaN or infinite) play as silence",
                    d->pk->path);
            d->warned = true;
        }
        if (n == 0)
            continue;
        place(d, x, pos, n, start, end, dst, ch);
        d->cursor = pos + n;

        size_t need = (size_t)n * d->pk->channels * sizeof(float);
        if (need > d->held_cap) {
            float *h = realloc(d->held, need);
            if (!h)
                return CB_ERROR(ENOMEM);
            d->held = h;
            d->held_cap = need;
        }
        memcpy(d->held, x, need);
        d->held_pos = pos;
        d->held_n = n;
    }
}

static int send(CBPacketDecoder *d, const Packet *p)
{
    size_t need = (size_t)p->size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (need > d->buf_cap) {
        uint8_t *b = realloc(d->buf, need);
        if (!b)
            return CB_ERROR(ENOMEM);
        d->buf = b;
        d->buf_cap = need;
    }
    memcpy(d->buf, d->pk->arena + p->offset, (size_t)p->size);
    memset(d->buf + p->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    AVPacket *pkt = d->pkt;
    av_packet_unref(pkt);
    pkt->data = d->buf;
    pkt->size = p->size;
    pkt->pts = pkt->dts = p->raw;
    pkt->duration = p->dur;
    pkt->flags = p->discard ? AV_PKT_FLAG_DISCARD : 0;
    if (p->side_skip || p->side_disc) {
        uint8_t *sd = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
        if (!sd)
            return CB_ERROR(ENOMEM);
        AV_WL32(sd, (uint32_t)p->side_skip);
        AV_WL32(sd + 4, (uint32_t)p->side_disc);
    }
    int ret = avcodec_send_packet(d->dec, pkt);
    /* the side data is ours to free; the bytes stay in d->buf */
    pkt->data = NULL;
    pkt->size = 0;
    av_packet_free_side_data(pkt);
    if (ret == AVERROR(ENOMEM))
        return CB_ERROR(ENOMEM);
    return 0;
}

int64_t cb_packets_decode(CBPacketDecoder *d, int64_t start, int64_t n, float *dst, int channels)
{
    CBPackets *pk = d->pk;
    int ch = channels > 0 ? CB_MIN(channels, pk->channels) : pk->channels;
    if (n <= 0)
        return 0;
    memset(dst, 0, (size_t)n * ch * sizeof(float));
    int64_t end = start + n;

    int ret;
    if (!d->dec || start != d->next || d->drained) {
        ret = seek(d, start);
        if (ret < 0)
            return ret;
    } else if (d->held_n) {
        place(d, d->held, d->held_pos, d->held_n, start, end, dst, ch);
    }
    d->next = -1;

    while (1) {
        if (d->k >= pk->nb) {
            if (!pk->done) {
                ret = cb_packets_index(pk, DEMAND_PACKETS);
                if (ret < 0 && d->k >= pk->nb)
                    return ret;
                continue;
            }
            /* The end of the track: whatever the decoder still holds */
            if (!d->drained) {
                avcodec_send_packet(d->dec, NULL);
                ret = receive(d, start, end, dst, ch);
                if (ret < 0)
                    return ret;
                d->drained = true;
            }
            break;
        }
        const Packet *p = &pk->pk[d->k];
        if (p->raw >= end)
            break;
        ret = send(d, p);
        if (ret >= 0)
            ret = receive(d, start, end, dst, ch);
        if (ret < 0)
            return ret;
        d->k++;
    }

    d->next = end;
    int64_t length = cb_packets_length(pk, NULL);
    return CB_CLAMP(length - start, (int64_t)0, n);
}
