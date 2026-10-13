/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Crafted files for the readers' bounds: what a hostile or badly broken
 * stick could hold, built here so that test_pdb's bounds and the fuzzer's
 * seeds are the same bytes. Each is a few pages that would cost a reader
 * that trusted it far more than its size: row indexes listing one row
 * hundreds of times, rows a byte apart that all read the same ids, strings
 * shared by every row, page chains that loop. */

#ifndef RXBOX_TESTS_FORMATS_HOSTILE_H
#define RXBOX_TESTS_FORMATS_HOSTILE_H

#include <stdlib.h>
#include <string.h>

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"

/* The most slots a page's row index can hold: groups of 16 down to the heap. */
#define HOSTILE_SLOTS (((RX_PDB_PAGE - RX_PDB_HEAP - 0x24) / 0x24 + 1) * 16)

/* A data page whose row index lists `slots` rows, all present, the row of
 * slot s at heap offset `stride * s`; the heap is `fill` bytes. */
static void hostile_page(uint8_t *p, uint32_t index, uint32_t type, uint32_t next, int slots,
                         int stride, uint8_t fill)
{
    memset(p, 0, RX_PDB_PAGE);
    memset(p + RX_PDB_HEAP, fill, RX_PDB_PAGE - RX_PDB_HEAP);
    rx_wr_le32(p + 0x04, index);
    rx_wr_le32(p + 0x08, type);
    rx_wr_le32(p + 0x0C, next);
    rx_wr_le32(p + 0x10, 1);
    p[0x18] = (uint8_t)(slots & 0xFF);
    p[0x19] = (uint8_t)((slots >> 8) & 0x1F);
    p[0x1B] = 0x24;
    for (int s = 0; s < slots; s++) {
        uint32_t base = RX_PDB_PAGE - 0x24 * (uint32_t)(s / 16);
        rx_wr_le16(p + base - 6 - 2 * (s % 16), (unsigned)(stride * s));
        rx_wr_le16(p + base - 4, 0xFFFF);
    }
}

/* An export.pdb of `pages` data pages for each of the tables the readers
 * join (tracks, artists, the playlist tree and entries, the history lists
 * and entries, artwork), every page's index listing the row at `stride` *
 * slot, `slots` of them, over a heap of 0x03 bytes: each row reads ids
 * 0x03030303 and empty names (0x03 is the empty short string), so every
 * tree row is the same playlist and every entry belongs to it. stride 0:
 * one row listed again and again; stride 1: rows a byte apart. */
static uint8_t *hostile_pdb_rows(int pages, int slots, int stride, size_t *size)
{
    static const uint32_t types[] = {
        RX_PDB_TRACKS, RX_PDB_ARTISTS, RX_PDB_PLAYLIST_TREE, RX_PDB_PLAYLIST_ENTRIES,
        RX_PDB_HISTORY_PLAYLISTS, RX_PDB_HISTORY_ENTRIES, RX_PDB_ARTWORK,
    };
    int nt = (int)RX_ARRAY_ELEMS(types);
    size_t n = (size_t)(1 + nt * pages);
    uint8_t *b = calloc(n, RX_PDB_PAGE);
    if (!b)
        return NULL;
    rx_wr_le32(b + 0x04, RX_PDB_PAGE);
    rx_wr_le32(b + 0x08, (uint32_t)nt);
    rx_wr_le32(b + 0x0C, (uint32_t)n);
    rx_wr_le32(b + 0x14, 2);
    for (int t = 0; t < nt; t++) {
        uint32_t first = 1 + (uint32_t)(t * pages), last = first + (uint32_t)pages - 1;
        uint8_t *e = b + 0x1C + 16 * t;
        rx_wr_le32(e, types[t]);
        rx_wr_le32(e + 4, (uint32_t)n);
        rx_wr_le32(e + 8, first);
        rx_wr_le32(e + 12, last);
        for (uint32_t pg = first; pg <= last; pg++)
            hostile_page(b + (size_t)pg * RX_PDB_PAGE, pg, types[t], pg < last ? pg + 1 : (uint32_t)n,
                         slots, stride, 0x03);
    }
    *size = n * RX_PDB_PAGE;
    return b;
}

/* Track rows 0x88 bytes apart whose 21 string offsets all point at one
 * long ASCII string at the end of the heap, of bytes above 0x7F (each read
 * as U+FFFD, three bytes): every row would decode the whole string 21
 * times. */
static uint8_t *hostile_pdb_strings(int pages, size_t *size)
{
    size_t n = (size_t)(1 + pages);
    uint8_t *b = calloc(n, RX_PDB_PAGE);
    if (!b)
        return NULL;
    rx_wr_le32(b + 0x04, RX_PDB_PAGE);
    rx_wr_le32(b + 0x08, 1);
    rx_wr_le32(b + 0x0C, (uint32_t)n);
    rx_wr_le32(b + 0x14, 2);
    rx_wr_le32(b + 0x1C, RX_PDB_TRACKS);
    rx_wr_le32(b + 0x1C + 4, (uint32_t)n);
    rx_wr_le32(b + 0x1C + 8, 1);
    rx_wr_le32(b + 0x1C + 12, (uint32_t)pages);
    /* 16 rows (one index group), the string from heap offset 16 * 0x88 to
     * the row index. */
    const int rows = 16, stride = 0x88;
    const uint32_t str = (uint32_t)(rows * stride);
    const uint32_t end = RX_PDB_PAGE - 0x24 - RX_PDB_HEAP;
    for (int pg = 1; pg <= pages; pg++) {
        uint8_t *p = b + (size_t)pg * RX_PDB_PAGE;
        hostile_page(p, (uint32_t)pg, RX_PDB_TRACKS, pg < pages ? (uint32_t)pg + 1 : (uint32_t)n, rows,
                     stride, 0);
        for (int r = 0; r < rows; r++) {
            uint8_t *row = p + RX_PDB_HEAP + r * stride;
            rx_wr_le16(row, 0x24);
            rx_wr_le32(row + 0x48, (uint32_t)r + 1);
            for (int s = 0; s < 21; s++)
                rx_wr_le16(row + 0x5E + 2 * s, str - (uint32_t)(r * stride));
        }
        uint8_t *s = p + RX_PDB_HEAP + str;
        s[0] = 0x40;
        rx_wr_le16(s + 1, end - str);
        s[3] = 0;
        memset(s + 4, 0xFF, end - str - 4);
    }
    *size = n * RX_PDB_PAGE;
    return b;
}

/* One page: a header whose 64 tables all start at page 0 and end at a page
 * the chain never reaches, page 0's own "next" (next_unused_page) being 0:
 * every chain loops on the header page. */
static uint8_t *hostile_pdb_loop(size_t *size)
{
    uint8_t *b = calloc(1, RX_PDB_PAGE);
    if (!b)
        return NULL;
    rx_wr_le32(b + 0x04, RX_PDB_PAGE);
    rx_wr_le32(b + 0x08, 64);
    rx_wr_le32(b + 0x0C, 0);
    for (int t = 0; t < 64; t++) {
        uint8_t *e = b + 0x1C + 16 * t;
        rx_wr_le32(e, (uint32_t)t);
        rx_wr_le32(e + 12, 1);
    }
    *size = RX_PDB_PAGE;
    return b;
}

#endif /* RXBOX_TESTS_FORMATS_HOSTILE_H */
