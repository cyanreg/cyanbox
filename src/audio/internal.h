/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Helpers shared by the files of the audio module (implemented in probe.c).
 * Nothing here is part of cyanbox/audio.h. */

#ifndef CYANBOX_AUDIO_INTERNAL_H
#define CYANBOX_AUDIO_INTERNAL_H

#include "cyanbox/audio.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/* A whole file mapped read-only. The scanners walk MP3 and FLAC frames over
 * it, which is faster than reading and needs no copy. */
typedef struct CBMap {
    const uint8_t *data;
    size_t size;
} CBMap;

int  cb_audio_map(const char *path, CBMap *m);
void cb_audio_unmap(CBMap *m);

/* avformat_open_input on "file:" + path, so that a name such as
 * "AD:HOUSE 11/x.flac" is not taken for a protocol (the file protocol does
 * no URL unescaping, so '?' and '%' are safe too). find_info runs
 * avformat_find_stream_info, which the codec parameters of MP3 and Ogg need.
 * -ENOTSUP for Matroska (and WebM), whatever the file's name: its times are
 * milliseconds, so a seek cannot say which sample it landed on, and every
 * path that reads a track (the probe, the decks, Link, the stick) needs
 * that. */
int  cb_audio_open(const char *path, AVFormatContext **fmt, bool find_info);
/* Index of the first stream of the given type, or -1. */
int  cb_audio_first_stream(const AVFormatContext *fmt, enum AVMediaType type);
/* An AVERROR as a negative errno (AVERROR(e) already is one). */
int  cb_audio_error(int averr);

/* The decoded length the container states for stream idx, made to agree
 * with the decoder (probe.c says how, format by format). A length
 * libavformat only estimated from the bitrate is counted from every
 * packet's duration instead, which reads the file to its end. */
int64_t cb_audio_container_samples(AVFormatContext *fmt, int idx);

/* The ID3v2 tags at the head of a file, one after another: the offset of the
 * first byte after them. */
size_t cb_audio_skip_id3v2(const uint8_t *data, size_t size);

/* True for the container short name `name` (the first of the demuxer's name
 * list). */
bool cb_audio_is_container(const AVFormatContext *fmt, const char *name);

/* cb_mp3_scan over a mapped file. */
int  cb_mp3_scan_mapped(const char *path, const CBMap *m, CBMp3Info *info);

/* ------------------------------------------------------------ decode.c */

/* A decoder for par. tb is the packets' time base; skip_manual keeps
 * libavcodec from dropping encoder delay and padding (MP3); delay >= 0
 * replaces the samples it drops from its first output (the stream's
 * initial padding by default, < 0 keeps that). */
int  cb_audio_open_decoder(AVCodecContext **out, const AVCodecParameters *par, AVRational tb,
                           bool skip_manual, int delay);

/* ------------------------------------------------------------ source.c */

/* A track decoded on the players' timeline and converted by
 * libswresample to the format asked for, read in order from wherever the
 * last seek put it (source.c says how). One thread at a time. */
typedef struct CBSource CBSource;

int  cb_source_open(CBSource **s, const char *path, const CBAudioFormat *f);
void cb_source_free(CBSource **s);
/* The output's rate, channels and libavutil sample format. */
int  cb_source_rate(const CBSource *s);
int  cb_source_channels(const CBSource *s);
enum AVSampleFormat cb_source_format(const CBSource *s);
/* The output's length as the container states it, in frames: an estimate
 * (the last read ends the track), -1 when it states none. */
int64_t cb_source_length(const CBSource *s);
/* The next read starts at output frame `at`. */
int  cb_source_seek(CBSource *s, int64_t at);
/* Up to n frames into dst (one plane, or one per channel), written by
 * libswresample: n, fewer only where the track ends (0 past it), or a
 * negative errno. */
int  cb_source_read(CBSource *s, uint8_t *const *dst, int n);

#endif /* CYANBOX_AUDIO_INTERNAL_H */
