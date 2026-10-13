/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Helpers shared by the files of the audio module (implemented in probe.c).
 * Nothing here is part of rxbox/audio.h. */

#ifndef RXBOX_AUDIO_INTERNAL_H
#define RXBOX_AUDIO_INTERNAL_H

#include "rxbox/audio.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/* A whole file mapped read-only. The scanners walk MP3 and FLAC frames over
 * it, which is faster than reading and needs no copy. */
typedef struct RXMap {
    const uint8_t *data;
    size_t size;
} RXMap;

int  rx_audio_map(const char *path, RXMap *m);
void rx_audio_unmap(RXMap *m);

/* avformat_open_input on "file:" + path, so that a name such as
 * "AD:HOUSE 11/x.flac" is not taken for a protocol (the file protocol does
 * no URL unescaping, so '?' and '%' are safe too). find_info runs
 * avformat_find_stream_info, which the codec parameters of MP3 and Ogg need. */
int  rx_audio_open(const char *path, AVFormatContext **fmt, bool find_info);
/* Index of the first stream of the given type, or -1. */
int  rx_audio_first_stream(const AVFormatContext *fmt, enum AVMediaType type);
/* An AVERROR as a negative errno (AVERROR(e) already is one). */
int  rx_audio_error(int averr);

/* The ID3v2 tags at the head of a file, one after another: the offset of the
 * first byte after them. */
size_t rx_audio_skip_id3v2(const uint8_t *data, size_t size);

/* True for the container short name `name` (the first of the demuxer's name
 * list). */
bool rx_audio_is_container(const AVFormatContext *fmt, const char *name);

/* rx_mp3_scan over a mapped file. */
int  rx_mp3_scan_mapped(const char *path, const RXMap *m, RXMp3Info *info);
/* Calls fn for every audio frame rx_mp3_scan_mapped counted, in order (the
 * header frame excluded), with its bytes (short for a truncated last frame),
 * samples and bitrate. The decoder feeds these to libavcodec itself, so its
 * timeline is the scanner's by construction. */
typedef int (*RXMp3FrameFn)(void *opaque, const uint8_t *frame, size_t size, int samples, int kbps);
int  rx_mp3_frames(const char *path, const RXMap *m, const RXMp3Info *info,
                   RXMp3FrameFn fn, void *opaque);
/* The same walk in steps: from byte *pos (0: the first audio frame) until
 * fn returns 1 (that frame included) or the file ends; *pos is then where
 * the next step starts. Returns 1 at the end of the file, 0 when stopped,
 * < 0 on fn's error. */
int  rx_mp3_frames_step(const char *path, const RXMap *m, const RXMp3Info *info, size_t *pos,
                        RXMp3FrameFn fn, void *opaque);

/* rx_flac_scan's frame walk in steps, over a mapped file. Init reads the
 * metadata into info (frames not filled) and points *streaminfo at the 34
 * bytes of STREAMINFO; next gives the frames in order, as the scan finds
 * them: 1 with a frame, 0 once total_samples are covered or the file ends
 * (*complete then says whether the frames reached total_samples). */
typedef struct RXFlacWalk {
    const uint8_t *data;
    size_t  size, start, cand;
    int64_t sample, index, total;
} RXFlacWalk;
int  rx_flac_walk_init(RXFlacWalk *w, const char *path, const uint8_t *data, size_t size,
                       RXFlacInfo *info, const uint8_t **streaminfo);
int  rx_flac_walk_next(RXFlacWalk *w, size_t *offset, int64_t *sample, int *block, bool *complete);

/* ------------------------------------------------------------ decode.c */

/* Decoded frames to interleaved float at out_ch channels: the conversion
 * and channel mix rx_decode applies (see decode.c), shared so that every
 * path through libavcodec gives the same samples. Set out_ch, zero the
 * rest; the buffers grow as frames need. */
enum RXMixKind {
    RX_MIX_COPY,
    RX_MIX_MEAN,
    RX_MIX_MATRIX,
};
typedef struct RXMixer {
    int    out_ch;
    int    mix_in;              /* input channels the mixer is set up for, 0 for none */
    enum RXMixKind mix_kind;
    float *matrix;              /* out_ch x mix_in */
    float *planar;              /* converted input, channel after channel */
    size_t planar_cap;
    float *mixed;               /* mixed, interleaved */
    size_t mixed_cap;
} RXMixer;
/* The frame's samples, interleaved, in *out (valid until the next call);
 * returns their count or a negative errno. */
int  rx_mixer_frame(RXMixer *m, const AVFrame *f, const float **out);
void rx_mixer_free(RXMixer *m);

/* A decoder for par. tb is the packets' time base; skip_manual keeps
 * libavcodec from dropping encoder delay and padding (MP3); delay >= 0
 * replaces the samples it drops from its first output (the stream's
 * initial padding by default, < 0 keeps that). */
int  rx_audio_open_decoder(AVCodecContext **out, const AVCodecParameters *par, AVRational tb,
                           bool skip_manual, int delay);

#endif /* RXBOX_AUDIO_INTERNAL_H */
