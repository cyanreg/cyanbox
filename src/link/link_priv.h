/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What every part of src/link shares: the ports, the device numbers, and
 * the bounded reader and writer every codec reads and writes the wire with.
 *
 * The reader is the one place a peer's bytes are looked at: each read
 * checks the length against what arrived before it reads, and a read past
 * the end sets `err` and returns zeros, so a parser reads a whole layout
 * and checks `err` once at the end. Nothing is ever allocated from a
 * length a peer sent: a length is checked against what is left first
 * (cb_rd_bytes), and the bytes are used where they are.
 *
 * The writer writes into a fixed buffer (a datagram's) or a growing one
 * (a database reply), and like the reader remembers a failure instead of
 * returning one per call: past a fixed buffer's end, or when growing
 * fails, `err` is set and nothing more is written. Multi-byte numbers are
 * big-endian unless the name says le: the DJ Link packets, the database
 * messages and XDR are all big-endian. */

#ifndef CYANBOX_LINK_PRIV_H
#define CYANBOX_LINK_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cyanbox/link.h"

/* ------------------------------------------------------------------ ports */

/* rekordbox's own (docs/LINK.md): presence on 50000, beats on 50001,
 * status on 50002, LINK CUE on 50004; the database server's port query on
 * TCP 12523; portmap on UDP 50111, not 111; NFS on 2049. The database
 * server and mountd take ephemeral ports, which the port query and portmap
 * give out. All above 1024: nothing needs root. */
#define CB_LINK_PORT_ANNOUNCE   50000
#define CB_LINK_PORT_BEAT       50001
#define CB_LINK_PORT_STATUS     50002
#define CB_LINK_PORT_CUE        50004
#define CB_LINK_PORT_QUERY      12523
#define CB_LINK_PORT_PORTMAP    50111
#define CB_LINK_PORT_NFS        2049

/* Every DJ Link datagram starts with these ten bytes, "Qspt1WmJOL"
 * (djl-analysis, packets.adoc). */
extern const uint8_t cb_link_magic[10];

/* ----------------------------------------------------------------- reader */

typedef struct CBRd {
    const uint8_t *p;
    size_t len, pos;
    bool err;                   /* a read went past the end */
} CBRd;

static inline CBRd cb_rd(const void *p, size_t len)
{
    return (CBRd) { .p = p, .len = len };
}

static inline size_t cb_rd_left(const CBRd *r)
{
    return r->err ? 0 : r->len - r->pos;
}

/* The next n bytes, or NULL (and err set) when fewer are left. */
static inline const uint8_t *cb_rd_bytes(CBRd *r, size_t n)
{
    if (r->err || n > r->len - r->pos) {
        r->err = true;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

static inline void cb_rd_skip(CBRd *r, size_t n)
{
    cb_rd_bytes(r, n);
}

static inline uint8_t cb_rd_u8(CBRd *r)
{
    const uint8_t *p = cb_rd_bytes(r, 1);
    return p ? p[0] : 0;
}

static inline uint16_t cb_rd_u16(CBRd *r)
{
    const uint8_t *p = cb_rd_bytes(r, 2);
    return p ? (uint16_t)(p[0] << 8 | p[1]) : 0;
}

static inline uint32_t cb_rd_u32(CBRd *r)
{
    const uint8_t *p = cb_rd_bytes(r, 4);
    return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : 0;
}

static inline uint64_t cb_rd_u64(CBRd *r)
{
    uint64_t hi = cb_rd_u32(r);
    return hi << 32 | cb_rd_u32(r);
}

static inline uint16_t cb_rd_u16le(CBRd *r)
{
    const uint8_t *p = cb_rd_bytes(r, 2);
    return p ? (uint16_t)(p[1] << 8 | p[0]) : 0;
}

static inline uint32_t cb_rd_u32le(CBRd *r)
{
    const uint8_t *p = cb_rd_bytes(r, 4);
    return p ? (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0] : 0;
}

/* Reads at an absolute offset without moving: fixed packet layouts. */
static inline uint8_t cb_rd_at_u8(const CBRd *r, size_t off)
{
    return off < r->len ? r->p[off] : 0;
}

static inline uint16_t cb_rd_at_u16(const CBRd *r, size_t off)
{
    return off + 2 <= r->len ? (uint16_t)(r->p[off] << 8 | r->p[off + 1]) : 0;
}

static inline uint32_t cb_rd_at_u32(const CBRd *r, size_t off)
{
    return off + 4 <= r->len ? (uint32_t)r->p[off] << 24 | (uint32_t)r->p[off + 1] << 16 |
                               (uint32_t)r->p[off + 2] << 8 | r->p[off + 3] : 0;
}

static inline uint16_t cb_rd_at_u16le(const CBRd *r, size_t off)
{
    return off + 2 <= r->len ? (uint16_t)(r->p[off + 1] << 8 | r->p[off]) : 0;
}

static inline uint32_t cb_rd_at_u32le(const CBRd *r, size_t off)
{
    return off + 4 <= r->len ? (uint32_t)r->p[off + 3] << 24 | (uint32_t)r->p[off + 2] << 16 |
                               (uint32_t)r->p[off + 1] << 8 | r->p[off] : 0;
}

/* ----------------------------------------------------------------- writer */

typedef struct CBWr {
    uint8_t *p;
    size_t len, cap;
    bool grow;                  /* p is malloc'd and may be reallocated */
    bool err;                   /* out of room, or out of memory */
} CBWr;

/* A writer into a fixed buffer. */
static inline CBWr cb_wr(void *buf, size_t cap)
{
    return (CBWr) { .p = buf, .cap = cap };
}

/* A growing writer, empty; cb_wr_free releases it. */
static inline CBWr cb_wr_dyn(void)
{
    return (CBWr) { .grow = true };
}

void cb_wr_free(CBWr *w);
/* Room for n more bytes: a pointer to them (the caller fills them and the
 * length is already counted), or NULL with err set. */
uint8_t *cb_wr_room(CBWr *w, size_t n);

static inline void cb_wr_bytes(CBWr *w, const void *p, size_t n)
{
    uint8_t *d = cb_wr_room(w, n);
    if (d && n)
        memcpy(d, p, n);
}

static inline void cb_wr_zeros(CBWr *w, size_t n)
{
    uint8_t *d = cb_wr_room(w, n);
    if (d && n)
        memset(d, 0, n);
}

static inline void cb_wr_u8(CBWr *w, uint8_t v)
{
    uint8_t *d = cb_wr_room(w, 1);
    if (d)
        d[0] = v;
}

static inline void cb_wr_u16(CBWr *w, uint16_t v)
{
    uint8_t *d = cb_wr_room(w, 2);
    if (d) {
        d[0] = (uint8_t)(v >> 8);
        d[1] = (uint8_t)v;
    }
}

static inline void cb_wr_u32(CBWr *w, uint32_t v)
{
    uint8_t *d = cb_wr_room(w, 4);
    if (d) {
        d[0] = (uint8_t)(v >> 24);
        d[1] = (uint8_t)(v >> 16);
        d[2] = (uint8_t)(v >> 8);
        d[3] = (uint8_t)v;
    }
}

static inline void cb_wr_u64(CBWr *w, uint64_t v)
{
    cb_wr_u32(w, (uint32_t)(v >> 32));
    cb_wr_u32(w, (uint32_t)v);
}

static inline void cb_wr_u16le(CBWr *w, uint16_t v)
{
    uint8_t *d = cb_wr_room(w, 2);
    if (d) {
        d[0] = (uint8_t)v;
        d[1] = (uint8_t)(v >> 8);
    }
}

static inline void cb_wr_u32le(CBWr *w, uint32_t v)
{
    uint8_t *d = cb_wr_room(w, 4);
    if (d) {
        d[0] = (uint8_t)v;
        d[1] = (uint8_t)(v >> 8);
        d[2] = (uint8_t)(v >> 16);
        d[3] = (uint8_t)(v >> 24);
    }
}

/* Patches what was written already (a length known only at the end). */
static inline void cb_wr_at_u16(CBWr *w, size_t off, uint16_t v)
{
    if (!w->err && off + 2 <= w->len) {
        w->p[off] = (uint8_t)(v >> 8);
        w->p[off + 1] = (uint8_t)v;
    }
}

static inline void cb_wr_at_u32(CBWr *w, size_t off, uint32_t v)
{
    if (!w->err && off + 4 <= w->len) {
        w->p[off] = (uint8_t)(v >> 24);
        w->p[off + 1] = (uint8_t)(v >> 16);
        w->p[off + 2] = (uint8_t)(v >> 8);
        w->p[off + 3] = (uint8_t)v;
    }
}

/* ------------------------------------------------------------------- text */

/* The UTF-16 code units of a UTF-8 string (surrogate pairs above the BMP;
 * an invalid byte as U+FFFD), without a terminator. */
size_t cb_link_utf16_units(const char *utf8);
/* Writes them big-endian (the database's strings) or little-endian (NFS
 * names), with a NUL unit after them when `nul`. */
void   cb_wr_utf16(CBWr *w, const char *utf8, bool be, bool nul);
/* UTF-16 bytes to an allocated UTF-8 string, stopping at a NUL unit; an
 * odd last byte is dropped, a lone surrogate is U+FFFD. NULL when out of
 * memory. */
char  *cb_link_utf16_to_utf8(const uint8_t *p, size_t bytes, bool be);

#endif /* CYANBOX_LINK_PRIV_H */
