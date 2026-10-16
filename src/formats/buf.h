/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A growable byte buffer for the format writers. Every put records the
 * first allocation failure in `err` and turns later puts into no-ops, so a
 * writer appends a whole structure and checks once at the end. export.pdb
 * and the settings files are little-endian, the ANLZ files big-endian, so
 * both flavours are here. Private to src/formats and src/export. */

#ifndef CYANBOX_FORMATS_BUF_H
#define CYANBOX_FORMATS_BUF_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cyanbox/common.h"

typedef struct CBBuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    int      err;
} CBBuf;

static inline int cb_buf_reserve(CBBuf *b, size_t extra)
{
    if (b->err)
        return b->err;
    if (b->len + extra <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *d = realloc(b->data, cap);
    if (!d) {
        b->err = CB_ERROR(ENOMEM);
        return b->err;
    }
    b->data = d;
    b->cap = cap;
    return 0;
}

static inline void cb_buf_put(CBBuf *b, const void *p, size_t n)
{
    if (!n || cb_buf_reserve(b, n) < 0)
        return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static inline void cb_buf_zeros(CBBuf *b, size_t n)
{
    if (!n || cb_buf_reserve(b, n) < 0)
        return;
    memset(b->data + b->len, 0, n);
    b->len += n;
}

static inline void cb_buf_u8(CBBuf *b, unsigned v)
{
    uint8_t c = (uint8_t)v;
    cb_buf_put(b, &c, 1);
}

static inline void cb_buf_le16(CBBuf *b, unsigned v)
{
    uint8_t c[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    cb_buf_put(b, c, 2);
}

static inline void cb_buf_le32(CBBuf *b, uint32_t v)
{
    uint8_t c[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    cb_buf_put(b, c, 4);
}

static inline void cb_buf_be16(CBBuf *b, unsigned v)
{
    uint8_t c[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    cb_buf_put(b, c, 2);
}

static inline void cb_buf_be32(CBBuf *b, uint32_t v)
{
    uint8_t c[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    cb_buf_put(b, c, 4);
}

static inline void cb_buf_be64(CBBuf *b, uint64_t v)
{
    cb_buf_be32(b, (uint32_t)(v >> 32));
    cb_buf_be32(b, (uint32_t)v);
}

static inline void cb_buf_free(CBBuf *b)
{
    free(b->data);
    *b = (CBBuf) { 0 };
}

/* Hands the bytes over (NULL when empty or after an error). */
static inline int cb_buf_finish(CBBuf *b, uint8_t **out, size_t *size)
{
    if (b->err) {
        int err = b->err;
        cb_buf_free(b);
        return err;
    }
    *out = b->data;
    *size = b->len;
    *b = (CBBuf) { 0 };
    return 0;
}

/* Reading. Callers check bounds first. */
static inline uint16_t cb_rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t cb_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint16_t cb_rd_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t cb_rd_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static inline uint64_t cb_rd_be64(const uint8_t *p)
{
    return (uint64_t)cb_rd_be32(p) << 32 | cb_rd_be32(p + 4);
}

static inline void cb_wr_le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void cb_wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline void cb_wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#endif /* CYANBOX_FORMATS_BUF_H */
