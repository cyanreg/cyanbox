/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_AUDIO_H
#define CYANBOX_AUDIO_H

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

#include "cyanbox/common.h"

/* Routes libav's own log lines through cb_log, once per process (thread
 * safe, every call after the first does nothing): warnings and errors at
 * CB_LOG_WARN, notes at CB_LOG_DEBUG, as "libav: <what said it>: <line>";
 * av_log_set_level() still filters first. cb_probe, cb_decode and the
 * other entry points here call it; call it directly before using libav
 * elsewhere. */
void cb_audio_init_logging(void);

typedef struct CBTag { char *key; char *value; } CBTag;   /* key lower-cased */

typedef struct CBMp3Info {
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
} CBMp3Info;

typedef struct CBFlacInfo {
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
} CBFlacInfo;

typedef struct CBProbe {
    char     codec[16];         /* libavcodec name */
    char     container[32];     /* libavformat short name (first of the list) */
    double   duration;          /* seconds, players' timeline */
    int64_t  total_samples;     /* players' timeline, at sample_rate */
    int      sample_rate, channels, bit_rate, bit_depth;   /* bit_depth 0 for lossy */
    CBTag   *tags; int nb_tags;
    bool     has_artwork;       /* an attached picture stream, or else a cover file beside it
                                 * (cb_artwork_find_file): a cover cb_artwork_rgba finds unless
                                 * the picture does not decode; looked for at probe time */
    CBMp3Info  mp3;             /* valid for MP3 only */
    CBFlacInfo flac;            /* valid for FLAC only */
} CBProbe;

int  cb_probe(const char *path, CBProbe *p);
void cb_probe_free(CBProbe *p);
/* The first non-empty tag among the names given (NULL-terminated list),
 * trimmed, or NULL. Names are matched case-insensitively. */
const char *cb_probe_tag(const CBProbe *p, const char *name, ...);
/* Reads the frame headers directly (no decoding). Both are also called by
 * cb_probe. */
int  cb_mp3_scan(const char *path, CBMp3Info *info);
int  cb_flac_scan(const char *path, CBFlacInfo *info);
void cb_flac_info_free(CBFlacInfo *info);

/* Decoded audio: interleaved float, [-1, 1]. */
typedef struct CBAudio {
    float   *samples;           /* nb_frames * channels */
    int      channels;
    int      rate;
    int64_t  nb_frames;
} CBAudio;

/* Decodes the first audio stream on the players' timeline.
 *   channels: 0 native, 1 mono (mean of the channels), 2 stereo (mono doubled)
 *   rate:     0 native, else resampled with libswresample (soxr when built in)
 * Damaged frames are decoded as silence rather than dropped. */
int  cb_decode(const char *path, int channels, int rate, CBAudio *out);
/* cb_decode's frames handed over as they are decoded instead of kept: the
 * same samples, in pieces of any length (valid during the call), in
 * memory that does not grow with the file. `start` (may be NULL) is told
 * the stream's channels (after `channels`, as cb_decode) and the
 * decoder's rate before any frame, and returns the rate wanted (0: that
 * one; another resamples) or a negative errno to stop; `frames` gets them,
 * returning < 0 to stop. Returns 0, the first such errno, or cb_decode's. */
typedef struct CBDecodeTo {
    int  (*start)(void *opaque, int channels, int rate);
    int  (*frames)(void *opaque, const float *frames, int64_t n);
    void  *opaque;
} CBDecodeTo;
int  cb_decode_stream(const char *path, int channels, const CBDecodeTo *to);
/* Derived buffers from one already decoded (no file access). */
int  cb_audio_to_mono(const CBAudio *in, CBAudio *out);
int  cb_audio_resample(const CBAudio *in, int rate, CBAudio *out);
void cb_audio_free(CBAudio *a);

/* ------------------------------------------------------------ packet store */

/* A track demuxed into memory, not decoded, from which any stretch decodes
 * on demand: what a deck plays from (cyanbox/player.h), so that loading costs
 * a read of the file, not a decode of all of it, and memory the size of
 * its audio packets, not of the decoded floats. Frames are on the players'
 * timeline and are cb_decode(path, 0, 0)'s: exactly for MP3 and lossless
 * codecs, and as close as a decoder starting cold gets for Opus, Vorbis and
 * AAC after a jump (src/audio/packets.c says how close).
 *
 * Threads: the store is handed from thread to thread, never shared, except
 * that cb_packets_length may be called from any thread at any time, the
 * real-time one included (two atomic loads). */
typedef struct CBPackets CBPackets;

/* Opens path and indexes its first packets; cb_packets_index or a decode
 * does the rest. -ENOTSUP when the packets cannot place their samples
 * exactly (Matroska keeps milliseconds): cb_decode such a file whole. */
int  cb_packets_open(const char *path, CBPackets **pk);
void cb_packets_free(CBPackets *pk);
/* Indexes up to max_packets more: 1 once the whole file is indexed (the
 * length is then exact), 0 while more remains, < 0 on a read error, where
 * the index then ends. */
int  cb_packets_index(CBPackets *pk, int max_packets);
/* The track's length in frames: what the container states until the index
 * is complete, then the count (*exact true). */
int64_t cb_packets_length(const CBPackets *pk, bool *exact);
int  cb_packets_rate(const CBPackets *pk);
int  cb_packets_channels(const CBPackets *pk);
/* Bytes the packets and their index take. */
size_t cb_packets_memory(const CBPackets *pk);

typedef struct CBPacketDecoder CBPacketDecoder;
/* A decoder over pk, used on the thread that holds pk. */
int  cb_packets_decoder(CBPackets *pk, CBPacketDecoder **d);
void cb_packets_decoder_free(CBPacketDecoder *d);
/* Frames [start, start + n) into dst, interleaved, the first `channels`
 * channels (0: all), silence outside the track and where a packet does not
 * decode. A call starting where the last one ended carries on; any other
 * starts a decoder again from enough packets before it. Indexes as far as
 * it needs. Returns the frames inside the track (fewer than n at its end)
 * or a negative errno. */
int64_t cb_packets_decode(CBPacketDecoder *d, int64_t start, int64_t n, float *dst,
                          int channels);

/* ----------------------------------------------------------------- artwork */

/* The track's cover: the embedded picture (attached picture stream) when
 * there is one, else an image file in the track's directory, else, when
 * that directory is a disc folder (CD1, Disc 2 ...), one in its parent.
 * Among the files there, in this order of preference, any case,
 * .jpg/.jpeg/.png/.webp: front, cover, folder, album, art, then a
 * numbered first one (front1, cover_01, ...), then, if the directory
 * holds exactly one image, that one. back, obi, booklet, spine, scan, cd,
 * inlay, inside, disc and tray are never used
 * (measured on the user's library: 200 of 232 album folders use front.*,
 * the rest cover.* or folder.*, with back/obi/booklet scans beside them).
 * RGBA8, malloc'd. -ENOENT when there is none. */
int  cb_artwork_rgba(const char *path, uint8_t **rgba, int *width, int *height);
/* The cover's original encoded bytes and their codec name ("mjpeg", "png",
 * "webp"): the embedded picture, else the same directory lookup as
 * cb_artwork_rgba (the file's bytes). -ENOENT when none. */
int  cb_artwork_raw(const char *path, uint8_t **data, size_t *size, char codec[16]);
/* The directory lookup alone: the chosen image file's path, or -ENOENT. */
int  cb_artwork_find_file(const char *audio_path, char **image_path);
/* Decodes an image file in memory (JPEG, PNG, WebP ...) to RGBA8. */
int  cb_image_decode(const uint8_t *data, size_t size, uint8_t **rgba, int *width, int *height);
/* As dj's _jpeg: the centred square of side min(width, height), scaled
 * (Lanczos) to size x size, encoded as a baseline 4:2:0 JPEG at `quality`
 * (1..100, libjpeg's tables; dj used 90). */
int  cb_image_jpeg_square(const uint8_t *rgba, int width, int height, int size, int quality,
                          uint8_t **jpeg, size_t *jpeg_size);
/* Scales to at most max_w x max_h keeping aspect (for interface thumbnails). */
int  cb_image_scale(const uint8_t *rgba, int width, int height, int max_w, int max_h,
                    uint8_t **out, int *out_w, int *out_h);

/* ---------------------------------------------------------------- encoding */

/* A track encoded as it is decoded (src/audio/encode.c): cb_decode_stream's
 * frames, the analysis's samples, to FLAC, WAV, AIFF or MP3, into the
 * caller's write and seek, in memory that does not grow with the file. */
enum CBEncodeFormat {
    CB_ENCODE_FLAC,
    CB_ENCODE_WAV,
    CB_ENCODE_AIFF,
    CB_ENCODE_MP3,             /* 320 kbit/s CBR (160 below 32 kHz) */
};

typedef struct CBEncodeOptions {
    enum CBEncodeFormat format;
    int      bits;              /* FLAC, WAV, AIFF: 16 or 24 */
    bool     exact;             /* the source's samples fit `bits` exactly: no dither */
    int      channels;          /* 0 the source's, else 1 or 2 (cb_decode) */
    int      max_rate;          /* > 0: resampled to it when the source's is higher */
    int64_t  frames;            /* > 0: exactly this many (silence to it, nothing past) */
    bool     tags;              /* FLAC: the source's tags and cover */
    int      compression;       /* FLAC: 1..12, 0 the encoder's default */
    /* The bytes: as AVIO's write_packet and seek (SEEK_SET only; NULL:
     * the muxer cannot go back to its header). */
    int      (*write)(void *opaque, const uint8_t *buf, int size);
    int64_t  (*seek)(void *opaque, int64_t offset, int whence);
    void     *opaque;
    bool     (*cancelled)(void *opaque);    /* asked between pieces; may be NULL */
} CBEncodeOptions;

typedef struct CBEncoded {
    int        rate, channels, bits;
    int64_t    frames;          /* written */
    int64_t    size;            /* bytes */
    CBFlacInfo flac;            /* FLAC: its frame table, for the ANLZ (cb_flac_info_free) */
    uint8_t    md5[16];         /* FLAC: STREAMINFO's MD5 of the samples */
} CBEncoded;

int  cb_encode(const char *path, const CBEncodeOptions *o, CBEncoded *out);

/* ------------------------------------------------------------- transcoding */

/* Writes `path` as a 16-bit FLAC to `out_path`, at the native rate or at
 * max_rate when that is lower (max_rate <= 0: native), copying tags and
 * artwork. The audio is cb_decode's, on the players' timeline. For the
 * stick's compatibility mode. */
int  cb_transcode_flac(const char *path, const char *out_path, int max_rate);
/* For Link: the same audio as a 16-bit PCM WAV, or as a 320 kbps CBR MP3
 * (without a Xing frame, the encoder's delay cut from the start so that it
 * decodes onto the source's timeline), neither with tags or artwork: what
 * every PRO DJ LINK player plays. */
int  cb_transcode_wav(const char *path, const char *out_path, int max_rate);
int  cb_transcode_mp3(const char *path, const char *out_path, int max_rate);

#endif /* CYANBOX_AUDIO_H */
