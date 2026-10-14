/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The writer's growing buffer and the UTF-16 text every codec in src/link
 * writes: big-endian in the database's strings (djl-analysis,
 * track_metadata.adoc, "string fields"), little-endian in the names NFS
 * carries (rbxport's captures of rekordbox, docs/LINK.md). */

#include <stdlib.h>

#include "rxbox/common.h"

#include "link_priv.h"

const uint8_t rx_link_magic[10] = { 0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c };

void rx_wr_free(RXWr *w)
{
    if (w->grow)
        free(w->p);
    *w = rx_wr_dyn();
}

uint8_t *rx_wr_room(RXWr *w, size_t n)
{
    if (w->err)
        return NULL;
    if (n > w->cap - w->len) {
        if (!w->grow || n > SIZE_MAX / 2 - w->len) {
            w->err = true;
            return NULL;
        }
        size_t cap = w->cap ? w->cap : 256;
        while (cap - w->len < n)
            cap *= 2;
        uint8_t *p = realloc(w->p, cap);
        if (!p) {
            w->err = true;
            return NULL;
        }
        w->p = p;
        w->cap = cap;
    }
    uint8_t *d = w->p + w->len;
    w->len += n;
    return d;
}

/* One code point from UTF-8; an invalid or overlong sequence is U+FFFD
 * and one byte. */
static uint32_t utf8_next(const uint8_t **sp)
{
    const uint8_t *s = *sp;
    uint32_t c = s[0];
    int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 :
            (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0) {
        *sp = s + 1;
        return 0xFFFD;
    }
    c &= 0x7F >> n;
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *sp = s + 1;
            return 0xFFFD;
        }
        c = c << 6 | (s[i] & 0x3F);
    }
    static const uint32_t min[4] = { 0, 0x80, 0x800, 0x10000 };
    if (c < min[n] || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) {
        *sp = s + 1;
        return 0xFFFD;
    }
    *sp = s + n + 1;
    return c;
}

size_t rx_link_utf16_units(const char *utf8)
{
    const uint8_t *s = (const uint8_t *)utf8;
    size_t n = 0;
    while (*s)
        n += utf8_next(&s) >= 0x10000 ? 2 : 1;
    return n;
}

static void put_unit(RXWr *w, uint16_t u, bool be)
{
    if (be)
        rx_wr_u16(w, u);
    else
        rx_wr_u16le(w, u);
}

void rx_wr_utf16(RXWr *w, const char *utf8, bool be, bool nul)
{
    const uint8_t *s = (const uint8_t *)utf8;
    while (*s) {
        uint32_t c = utf8_next(&s);
        if (c >= 0x10000) {
            c -= 0x10000;
            put_unit(w, (uint16_t)(0xD800 | c >> 10), be);
            put_unit(w, (uint16_t)(0xDC00 | (c & 0x3FF)), be);
        } else {
            put_unit(w, (uint16_t)c, be);
        }
    }
    if (nul)
        put_unit(w, 0, be);
}

char *rx_link_utf16_to_utf8(const uint8_t *p, size_t bytes, bool be)
{
    size_t n = bytes / 2;
    char *out = malloc(3 * n + 1);     /* a pair is 4 bytes for 2 units */
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = be ? (uint32_t)(p[2 * i] << 8 | p[2 * i + 1]) :
                          (uint32_t)(p[2 * i + 1] << 8 | p[2 * i]);
        if (!c)
            break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t d = be ? (uint32_t)(p[2 * i + 2] << 8 | p[2 * i + 3]) :
                              (uint32_t)(p[2 * i + 3] << 8 | p[2 * i + 2]);
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | c >> 18);
            out[o++] = (char)(0x80 | (c >> 12 & 0x3F));
            out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return out;
}
