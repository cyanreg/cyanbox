/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MPEG audio frame scanning: where the players' timeline of an MP3 starts,
 * how many frames it has, CBR or VBR. A port of dj's formats/mp3.py plus
 * rekordbox's frame-map rule.
 *
 * The frame map (facts measured by rbxport, crates/rbl-core/src/mpeg.rs; not
 * its code): rekordbox 7 builds a map of the file's frames before decoding
 * and gives every frame in it a slot of samples, a leading Xing/Info/VBRI
 * frame included, which then plays as one frame of silence. The map is
 * thrown away and started again at a frame when one of the first four frames
 * differs from the frame before it in MPEG version, layer, sample rate or
 * channel mode. A LAME file's Info frame says joint stereo like its audio, so
 * it counts; an ffmpeg file's Info frame says stereo over joint-stereo audio,
 * so the map restarts at the first audio frame. rbxport checked this on 200
 * MP3s of a rekordbox stick against the PWV3 waveform rekordbox wrote: 86
 * were one frame (1152 samples) later than a decoder that drops the tag
 * frame, 113 agreed, split exactly by this rule.
 *
 * The whole file is mapped and walked header to header: MP3s are small and
 * every header is four bytes. */

#include "rxbox/audio.h"

#include <math.h>

#include "internal.h"

/* rekordbox keeps a change of frame parameters once its map holds this many
 * frames, and starts the map again before that (rbxport, MAP_SETTLED). */
#define MAP_SETTLED 4
/* Past this many frames nothing can move the start of the map. */
#define MAP_FRAMES (MAP_SETTLED + 2)

/* kbps by bitrate index, Layer III: MPEG-1, then MPEG-2 and 2.5. Index 0 is
 * free format (no frame length in the header, rejected), 15 is invalid. */
static const uint16_t bitrates[2][16] = {
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
    { 0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160, 0 },
};

/* Sample rates by the two version bits (3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5). */
static const int rates[4][3] = {
    [0] = { 11025, 12000,  8000 },
    [2] = { 22050, 24000, 16000 },
    [3] = { 44100, 48000, 32000 },
};

typedef struct Header {
    int version;        /* the two version bits */
    int layer;          /* the two layer bits, 1 = Layer III */
    int rate_index;
    int mode;           /* 0 stereo, 1 joint stereo, 2 dual channel, 3 mono */
    int kbps;
    int rate;
    int samples;        /* per frame */
    int length;         /* bytes, padding included */
} Header;

static bool parse_header(const uint8_t *data, size_t size, size_t pos, Header *h)
{
    if (pos > size || size - pos < 4)
        return false;
    const uint8_t *b = data + pos;
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0)
        return false;

    int version = (b[1] >> 3) & 3;
    int layer = (b[1] >> 1) & 3;
    int br_index = b[2] >> 4;
    int rate_index = (b[2] >> 2) & 3;
    if (version == 1 || layer != 1 || br_index == 0 || br_index == 15 || rate_index == 3)
        return false;

    bool mpeg1 = version == 3;
    *h = (Header) {
        .version    = version,
        .layer      = layer,
        .rate_index = rate_index,
        .mode       = b[3] >> 6,
        .kbps       = bitrates[!mpeg1][br_index],
        .rate       = rates[version][rate_index],
        .samples    = mpeg1 ? 1152 : 576,
    };
    h->length = (h->samples / 8) * h->kbps * 1000 / h->rate + ((b[2] >> 1) & 1);
    return true;
}

/* What rekordbox compares between neighbouring frames. */
static bool same_shape(const Header *a, const Header *b)
{
    return a->version == b->version && a->layer == b->layer &&
           a->rate_index == b->rate_index && a->mode == b->mode;
}

/* Bytes of side information after the four header bytes. */
static int side_info_size(const Header *h)
{
    bool mono = h->mode == 3;
    if (h->version == 3)
        return mono ? 17 : 32;
    return mono ? 9 : 17;
}

static bool tag_at(const uint8_t *data, size_t size, size_t pos, const char *tag)
{
    return pos <= size && size - pos >= 4 && !memcmp(data + pos, tag, 4);
}

/* The first frame that the next frame confirms (same version, layer and
 * rate, as rbxport and ffmpeg's mp3 demuxer require; dj only asked for any
 * valid header), or -1. */
static int64_t find_first_frame(const uint8_t *data, size_t size, size_t pos, Header *h)
{
    for (; pos + 4 <= size; pos++) {
        Header next;
        if (!parse_header(data, size, pos, h))
            continue;
        if (parse_header(data, size, pos + h->length, &next) &&
            next.version == h->version && next.layer == h->layer &&
            next.rate_index == h->rate_index)
            return (int64_t)pos;
    }
    return -1;
}

/* Calls fn for every audio frame from pos on. Junk between frames is
 * skipped when two chained frames follow it. ffmpeg's parser would make the
 * junk and the frame after it one packet that fails to decode, a frame of
 * silence in rxbox: the same count either way, and rx_decode, which feeds
 * these frames itself, plays the frame. dj stopped counting at the first
 * junk instead. A truncated last frame is passed short; dj counted it as a
 * frame and decoders play it as one. fn returning 1 stops the walk after
 * that frame (returns 0, *end is where to go on); 1 is returned at the end
 * of the file. */
static int walk_frames(const char *path, const uint8_t *d, size_t size, size_t pos,
                       RXMp3FrameFn fn, void *opaque, size_t *end)
{
    *end = pos;
    while (pos + 4 <= size) {
        Header h;
        if (!parse_header(d, size, pos, &h)) {
            Header next;
            int64_t resync = find_first_frame(d, size, pos + 1, &next);
            if (resync < 0)
                break;
            rx_debug("%s: %zu bytes of junk at %zu", path, (size_t)resync - pos, pos);
            pos = (size_t)resync;
            continue;
        }
        int ret = fn(opaque, d + pos, RX_MIN((size_t)h.length, size - pos), h.samples, h.kbps);
        if (ret < 0)
            return ret;
        pos += h.length;
        *end = RX_MIN(pos, size);
        if (ret > 0)
            return 0;
    }
    return 1;
}

typedef struct Stats {
    int64_t frames, total_kbps;
    int min_kbps, max_kbps;
} Stats;

static int count_frame(void *opaque, const uint8_t *frame, size_t size, int samples, int kbps)
{
    Stats *s = opaque;
    s->frames++;
    s->total_kbps += kbps;
    s->min_kbps = RX_MIN(s->min_kbps, kbps);
    s->max_kbps = RX_MAX(s->max_kbps, kbps);
    return 0;
}

int rx_mp3_scan_mapped(const char *path, const RXMap *m, RXMp3Info *info)
{
    *info = (RXMp3Info) { 0 };
    const uint8_t *d = m->data;
    size_t size = m->size;

    Header first;
    int64_t start = find_first_frame(d, size, rx_audio_skip_id3v2(d, size), &first);
    if (start < 0) {
        rx_debug("%s: no MPEG Layer III frames", path);
        return RX_ERROR(EINVAL);
    }

    /* dj's test: "Xing"/"Info" straight after the side information, or
     * "VBRI" 32 bytes after the header. It is also what ffmpeg's mp3 demuxer
     * looks for when it drops the frame. */
    size_t tag_pos = (size_t)start + 4 + side_info_size(&first);
    bool xing = tag_at(d, size, tag_pos, "Xing");
    bool vbri = tag_at(d, size, (size_t)start + 36, "VBRI");
    bool header_frame = xing || vbri || tag_at(d, size, tag_pos, "Info");

    /* rekordbox's frame map over the first frames, header frame included:
     * restarted at frame i + 1 whenever frame i + 1 differs from frame i
     * while fewer than MAP_SETTLED frames are held. */
    Header map[MAP_FRAMES];
    int nb_map = 0;
    for (size_t pos = (size_t)start; nb_map < MAP_FRAMES; nb_map++) {
        if (!parse_header(d, size, pos, &map[nb_map]))
            break;
        pos += map[nb_map].length;
    }
    int map_start = 0;
    for (int i = 0; i + 1 < nb_map; i++) {
        int held = i + 1 - map_start;
        if (!same_shape(&map[i], &map[i + 1]) && held < MAP_SETTLED)
            map_start = i + 1;
    }

    Stats st = { .min_kbps = INT32_MAX };
    size_t end;
    size_t pos = (size_t)start + (header_frame ? (size_t)first.length : 0);
    walk_frames(path, d, size, pos, count_frame, &st, &end);

    int audio_start = header_frame ? 1 : 0;
    *info = (RXMp3Info) {
        .valid              = true,
        .version            = first.version == 3 ? 1 : first.version == 2 ? 2 : 25,
        .sample_rate        = first.rate,
        .channels           = first.mode == 3 ? 1 : 2,
        .frame_samples      = first.samples,
        .nb_frames          = st.frames,
        .header_frame       = header_frame,
        .header_counts      = header_frame && map_start == 0,
        .skip_frames        = RX_MAX(map_start - audio_start, 0),
        .vbr                = xing || vbri || (st.frames && st.min_kbps != st.max_kbps),
        /* dj: round(total / frames); Python rounds half to even, as rint does */
        .bitrate_kbps       = st.frames ? (int)rint((double)st.total_kbps / (double)st.frames) : 0,
        .first_frame_offset = start,
        .audio_bytes        = (int64_t)(end - (size_t)start),
    };
    if (info->skip_frames)
        rx_debug("%s: frame map restarts at frame %d, %d audio frames off the timeline",
                 path, map_start, info->skip_frames);
    return 0;
}

int rx_mp3_frames(const char *path, const RXMap *m, const RXMp3Info *info,
                  RXMp3FrameFn fn, void *opaque)
{
    size_t pos = (size_t)info->first_frame_offset;
    Header first;
    if (!info->valid || !parse_header(m->data, m->size, pos, &first))
        return RX_ERROR(EINVAL);
    if (info->header_frame)
        pos += (size_t)first.length;
    size_t end;
    return walk_frames(path, m->data, m->size, pos, fn, opaque, &end);
}

int rx_mp3_frames_step(const char *path, const RXMap *m, const RXMp3Info *info, size_t *pos,
                       RXMp3FrameFn fn, void *opaque)
{
    if (!*pos) {
        Header first;
        *pos = (size_t)info->first_frame_offset;
        if (!info->valid || !parse_header(m->data, m->size, *pos, &first))
            return RX_ERROR(EINVAL);
        if (info->header_frame)
            *pos += (size_t)first.length;
    }
    return walk_frames(path, m->data, m->size, *pos, fn, opaque, pos);
}

int rx_mp3_scan(const char *path, RXMp3Info *info)
{
    *info = (RXMp3Info) { 0 };
    RXMap m;
    int ret = rx_audio_map(path, &m);
    if (ret < 0)
        return ret;
    ret = rx_mp3_scan_mapped(path, &m, info);
    rx_audio_unmap(&m);
    return ret;
}
