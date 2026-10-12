/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* FLAC STREAMINFO and the frame table the ANLZ PVB2 seek tag is built from:
 * a port of dj's formats/flac.py. The frame walk is dj's exactly (sync code,
 * the header's reserved bits, the coded frame or sample number, CRC-8), so
 * the seek entries the formats module picks from it are dj's.
 *
 * One deliberate difference: ID3v2 tags in front of "fLaC" are skipped
 * (some taggers write them, and libavformat accepts the file); dj refused
 * such files. Offsets stay absolute in first_frame_offset and relative to
 * it in the table, as before. */

#include "rxbox/audio.h"

#include <inttypes.h>

#include "internal.h"

/* CRC-8, polynomial x^8 + x^2 + x + 1 (0x07), as in the FLAC frame header. */
static const uint8_t crc8_table[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31,
    0x24, 0x23, 0x2A, 0x2D, 0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65,
    0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D, 0xE0, 0xE7, 0xEE, 0xE9,
    0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1,
    0xB4, 0xB3, 0xBA, 0xBD, 0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2,
    0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA, 0xB7, 0xB0, 0xB9, 0xBE,
    0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16,
    0x03, 0x04, 0x0D, 0x0A, 0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42,
    0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A, 0x89, 0x8E, 0x87, 0x80,
    0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8,
    0xDD, 0xDA, 0xD3, 0xD4, 0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C,
    0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44, 0x19, 0x1E, 0x17, 0x10,
    0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F,
    0x6A, 0x6D, 0x64, 0x63, 0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B,
    0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13, 0xAE, 0xA9, 0xA0, 0xA7,
    0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF,
    0xFA, 0xFD, 0xF4, 0xF3,
};

static uint8_t crc8(const uint8_t *b, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++)
        crc = crc8_table[crc ^ b[i]];
    return crc;
}

typedef struct FrameHeader {
    bool    variable;       /* variable block size: the number is a sample number */
    int64_t number;         /* frame number, or first sample when variable */
    int     block;
} FrameHeader;

/* dj's _header(): the header at pos, read from at most 16 bytes. */
static bool parse_frame_header(const uint8_t *data, size_t size, size_t pos, FrameHeader *f)
{
    const uint8_t *h = data + pos;
    size_t len = RX_MIN(size - pos, (size_t)16);
    if (len < 6 || h[0] != 0xFF || (h[1] & 0xFE) != 0xF8 || (h[3] & 1))
        return false;

    int size_code = h[2] >> 4, rate_code = h[2] & 0xF;
    if (size_code == 0 || rate_code == 15 || (h[3] >> 4) > 10 || ((h[3] >> 1) & 7) == 3)
        return false;

    /* The frame/sample number, UTF-8 coded */
    int lead = 0;
    while (lead < 8 && (h[4] & (0x80 >> lead)))
        lead++;
    if (lead == 1 || lead > 7)
        return false;
    int64_t n = h[4] & (0x7F >> lead);
    size_t at = 5;
    for (int i = 1; i < lead; i++) {
        if (at >= len || (h[at] & 0xC0) != 0x80)
            return false;
        n = (n << 6) | (h[at] & 0x3F);
        at++;
    }

    int block;
    if (size_code == 1) {
        block = 192;
    } else if (size_code >= 2 && size_code <= 5) {
        block = 576 << (size_code - 2);
    } else if (size_code == 6) {
        if (at >= len)
            return false;
        block = h[at] + 1;
        at += 1;
    } else if (size_code == 7) {
        if (at + 1 >= len)
            return false;
        block = (h[at] << 8 | h[at + 1]) + 1;
        at += 2;
    } else {
        block = 256 << (size_code - 8);
    }
    if (rate_code == 12)
        at += 1;
    else if (rate_code == 13 || rate_code == 14)
        at += 2;

    if (at >= len || crc8(h, at) != h[at])
        return false;

    *f = (FrameHeader) {
        .variable = h[1] & 1,
        .number   = n,
        .block    = block,
    };
    return true;
}

static uint64_t rb(const uint8_t *b, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 8) | b[i];
    return v;
}

static int push_frame(RXFlacInfo *info, int *cap, int64_t sample, int64_t offset, int block)
{
    if (info->nb_frames == *cap) {
        int ncap = *cap ? *cap * 2 : 1024;
        void *nf = realloc(info->frames, (size_t)ncap * sizeof(*info->frames));
        if (!nf)
            return RX_ERROR(ENOMEM);
        info->frames = nf;
        *cap = ncap;
    }
    info->frames[info->nb_frames].sample = sample;
    info->frames[info->nb_frames].offset = offset;
    info->frames[info->nb_frames].block = block;
    info->nb_frames++;
    return 0;
}

int rx_flac_scan(const char *path, RXFlacInfo *info)
{
    *info = (RXFlacInfo) { 0 };

    RXMap m;
    int ret = rx_audio_map(path, &m);
    if (ret < 0)
        return ret;
    const uint8_t *d = m.data;
    size_t size = m.size;

    size_t pos = rx_audio_skip_id3v2(d, size);
    if (size - pos < 4 || memcmp(d + pos, "fLaC", 4)) {
        rx_debug("%s: not a FLAC file", path);
        ret = RX_ERROR(EINVAL);
        goto end;
    }
    pos += 4;

    /* Metadata blocks up to the one flagged last */
    bool streaminfo = false;
    while (1) {
        if (size - pos < 4) {
            ret = RX_ERROR(EINVAL);
            goto end;
        }
        const uint8_t *hdr = d + pos;
        size_t length = (size_t)rb(hdr + 1, 3);
        if ((hdr[0] & 0x7F) == 0 && length >= 34 && size - pos - 4 >= 34) {
            const uint8_t *s = hdr + 4;
            uint64_t packed = rb(s + 10, 8);
            info->min_block = (int)rb(s, 2);
            info->max_block = (int)rb(s + 2, 2);
            info->sample_rate = (int)(packed >> 44);
            info->channels = (int)((packed >> 41) & 7) + 1;
            info->bit_depth = (int)((packed >> 36) & 0x1F) + 1;
            info->total_samples = (int64_t)(packed & ((UINT64_C(1) << 36) - 1));
            streaminfo = true;
        }
        if (size - pos - 4 < length) {
            ret = RX_ERROR(EINVAL);
            goto end;
        }
        pos += 4 + length;
        if (hdr[0] & 0x80)
            break;
    }
    if (!streaminfo) {
        rx_debug("%s: no STREAMINFO", path);
        ret = RX_ERROR(EINVAL);
        goto end;
    }
    size_t start = pos;
    info->first_frame_offset = (int64_t)start;

    /* Each frame is the first sync candidate after the previous one whose
     * header parses with the expected frame number (fixed block size) or
     * sample number (variable). Candidates only move forward, as in dj. */
    int cap = 0;
    if (info->max_block > 0) {
        int64_t est = info->total_samples / info->max_block + 2;
        cap = (int)RX_MIN(est, (int64_t)(1 << 24));
        info->frames = malloc((size_t)cap * sizeof(*info->frames));
        if (!info->frames) {
            ret = RX_ERROR(ENOMEM);
            goto end;
        }
    }
    int64_t sample = 0, index = 0;
    size_t cand = start;
    while (sample < info->total_samples && cand + 1 < size) {
        const uint8_t *p = memchr(d + cand, 0xFF, size - 1 - cand);
        if (!p)
            break;
        size_t at = (size_t)(p - d);
        cand = at + 1;
        FrameHeader f;
        if ((d[at + 1] & 0xFE) != 0xF8 || !parse_frame_header(d, size, at, &f))
            continue;
        if (f.number != (f.variable ? sample : index))
            continue;
        ret = push_frame(info, &cap, sample, (int64_t)(at - start), f.block);
        if (ret < 0)
            goto end;
        sample += f.block;
        index++;
    }
    if (sample < info->total_samples) {
        rx_debug("%s: frames end at sample %" PRId64 ", before the STREAMINFO count %" PRId64,
                 path, sample, info->total_samples);
        ret = RX_ERROR(EINVAL);
        goto end;
    }
    info->valid = true;

end:
    if (ret < 0) {
        free(info->frames);
        info->frames = NULL;
        info->nb_frames = 0;
    }
    rx_audio_unmap(&m);
    return ret;
}

void rx_flac_info_free(RXFlacInfo *info)
{
    free(info->frames);
    info->frames = NULL;
    info->nb_frames = 0;
    info->valid = false;
}
