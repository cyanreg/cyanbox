/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Helpers shared by the files of the audio module (implemented in probe.c).
 * Nothing here is part of rxbox/audio.h. */

#ifndef RXBOX_AUDIO_INTERNAL_H
#define RXBOX_AUDIO_INTERNAL_H

#include "rxbox/audio.h"

#include <libavformat/avformat.h>

/* A whole file mapped read-only. The scanners walk MP3 and FLAC frames over
 * it, which is faster than reading and needs no copy. */
typedef struct RxMap {
    const uint8_t *data;
    size_t size;
} RxMap;

int  rx_audio_map(const char *path, RxMap *m);
void rx_audio_unmap(RxMap *m);

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
int  rx_mp3_scan_mapped(const char *path, const RxMap *m, RxMp3Info *info);
/* Calls fn for every audio frame rx_mp3_scan_mapped counted, in order (the
 * header frame excluded), with its bytes (short for a truncated last frame),
 * samples and bitrate. The decoder feeds these to libavcodec itself, so its
 * timeline is the scanner's by construction. */
typedef int (*RxMp3FrameFn)(void *opaque, const uint8_t *frame, size_t size, int samples, int kbps);
int  rx_mp3_frames(const char *path, const RxMap *m, const RxMp3Info *info,
                   RxMp3FrameFn fn, void *opaque);

#endif /* RXBOX_AUDIO_INTERNAL_H */
