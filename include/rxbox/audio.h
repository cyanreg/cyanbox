/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_AUDIO_H
#define RXBOX_AUDIO_H

/* Decoding and probing through libavformat/libavcodec, on the players'
 * timeline.
 *
 * The players' timeline: rekordbox and the CDJs count time from the first
 * sample of the first MP3 frame and never trim encoder delay or padding,
 * and they give a leading Xing/Info/VBRI frame a frame of silence unless
 * one of the first four frames differs from the one before it in MPEG
 * version, layer, sample rate or channel mode (then the count starts at
 * the first audio frame). An ANLZ detail waveform has exactly
 * frames * 1152 * 150 / rate columns. ffmpeg trims both by default, so the
 * decoder turns that off (skip_manual) and prepends the silent frame when
 * the rule says so. AAC keeps ffmpeg's handling of the priming samples; what
 * the players do there is unknown. Everything else starts at sample 0.
 *
 * Paths are passed to libavformat with a "file:" prefix so that a name
 * containing ':' ("AD:HOUSE 11/...") is not read as a protocol. */

#include "rxbox/common.h"

typedef struct RxTag { char *key; char *value; } RxTag;   /* key lower-cased */

typedef struct RxMp3Info {
    bool    valid;
    int     version;            /* 1, 2 or 25 (2.5) */
    int     sample_rate;
    int     channels;
    int     frame_samples;      /* 1152 or 576 */
    int64_t nb_frames;          /* audio frames, excluding a header frame */
    bool    header_frame;       /* a Xing/Info/VBRI frame leads the audio */
    bool    header_counts;      /* the rule above: it counts as one frame of silence */
    bool    vbr;                /* Xing (not Info) or VBRI, or bitrates vary */
    int     bitrate_kbps;       /* of the first audio frame, or the average for VBR */
    int64_t first_frame_offset; /* byte offset of the first frame (header frame included) */
    int64_t audio_bytes;        /* from the first frame to the end of the last */
    int     skip_frames;        /* audio frames before the map restarts past the first
                                 * audio frame (rare): not on the timeline */
} RxMp3Info;

typedef struct RxFlacInfo {
    bool    valid;
    int     sample_rate, channels, bit_depth;
    int64_t total_samples;
    int     min_block, max_block;
    int64_t first_frame_offset; /* byte offset of the first audio frame */
    /* The frame table the PVB2 seek tag is built from, as dj's flac.py:
     * (sample index, byte offset relative to first_frame_offset, block size)
     * for every frame. dj's seek_entries picks, for i in 0..399, the last
     * frame whose sample <= i * (total_samples / 400). */
    struct { int64_t sample; int64_t offset; int block; } *frames;
    int     nb_frames;
} RxFlacInfo;

typedef struct RxProbe {
    char     codec[16];         /* libavcodec name */
    char     container[32];     /* libavformat short name (first of the list) */
    double   duration;          /* seconds, players' timeline */
    int64_t  total_samples;     /* players' timeline, at sample_rate */
    int      sample_rate, channels, bit_rate, bit_depth;   /* bit_depth 0 for lossy */
    RxTag   *tags; int nb_tags;
    bool     has_artwork;       /* an attached picture stream */
    RxMp3Info  mp3;             /* valid for MP3 only */
    RxFlacInfo flac;            /* valid for FLAC only */
} RxProbe;

int  rx_probe(const char *path, RxProbe *p);
void rx_probe_free(RxProbe *p);
/* The first non-empty tag among the names given (NULL-terminated list),
 * trimmed, or NULL. Names are matched case-insensitively. */
const char *rx_probe_tag(const RxProbe *p, const char *name, ...);
/* Reads the frame headers directly (no decoding). Both are also called by
 * rx_probe. */
int  rx_mp3_scan(const char *path, RxMp3Info *info);
int  rx_flac_scan(const char *path, RxFlacInfo *info);
void rx_flac_info_free(RxFlacInfo *info);

/* Decoded audio: interleaved float, [-1, 1]. */
typedef struct RxAudio {
    float   *samples;           /* nb_frames * channels */
    int      channels;
    int      rate;
    int64_t  nb_frames;
} RxAudio;

/* Decodes the first audio stream on the players' timeline.
 *   channels: 0 native, 1 mono (mean of the channels), 2 stereo (mono doubled)
 *   rate:     0 native, else resampled with libswresample (soxr when built in)
 * Damaged frames are decoded as silence rather than dropped. */
int  rx_decode(const char *path, int channels, int rate, RxAudio *out);
/* Derived buffers from one already decoded (no file access). */
int  rx_audio_to_mono(const RxAudio *in, RxAudio *out);
int  rx_audio_resample(const RxAudio *in, int rate, RxAudio *out);
void rx_audio_free(RxAudio *a);

/* ----------------------------------------------------------------- artwork */

/* The track's cover: the embedded picture (attached picture stream) when
 * there is one, else an image file in the track's directory, else one in
 * the parent directory (disc subfolders). Among the files there, in this
 * order of preference, any case, .jpg/.jpeg/.png/.webp: front, cover,
 * folder, album, art, then a numbered first one (front1, cover_01, ...),
 * then, if the directory holds exactly one image, that one. back, obi,
 * booklet, spine, scan, cd, inlay, inside, disc and tray are never used
 * (measured on the user's library: 200 of 232 album folders use front.*,
 * the rest cover.* or folder.*, with back/obi/booklet scans beside them).
 * RGBA8, malloc'd. -ENOENT when there is none. */
int  rx_artwork_rgba(const char *path, uint8_t **rgba, int *width, int *height);
/* The cover's original encoded bytes and their codec name ("mjpeg", "png",
 * "webp"): the embedded picture, else the same directory lookup as
 * rx_artwork_rgba (the file's bytes). -ENOENT when none. */
int  rx_artwork_raw(const char *path, uint8_t **data, size_t *size, char codec[16]);
/* The directory lookup alone: the chosen image file's path, or -ENOENT. */
int  rx_artwork_find_file(const char *audio_path, char **image_path);
/* Decodes an image file in memory (JPEG, PNG, WebP ...) to RGBA8. */
int  rx_image_decode(const uint8_t *data, size_t size, uint8_t **rgba, int *width, int *height);
/* Scales (box filter, aspect kept, centred on black) to size x size and
 * encodes a baseline JPEG at `quality` (1..100; dj used 90). */
int  rx_image_jpeg_square(const uint8_t *rgba, int width, int height, int size, int quality,
                          uint8_t **jpeg, size_t *jpeg_size);
/* Scales to at most max_w x max_h keeping aspect (for interface thumbnails). */
int  rx_image_scale(const uint8_t *rgba, int width, int height, int max_w, int max_h,
                    uint8_t **out, int *out_w, int *out_h);

/* ------------------------------------------------------------- transcoding */

/* Writes `path` as 16-bit/44.1 kHz FLAC (or keeps the rate when <= 48 kHz)
 * to `out_path`, copying tags and artwork. For the stick's compatibility
 * mode. */
int  rx_transcode_flac(const char *path, const char *out_path, int max_rate);

#endif /* RXBOX_AUDIO_H */
