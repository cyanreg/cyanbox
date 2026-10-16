/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* ANLZ0000.DAT / .EXT / .2EX analysis files, a port of dj's formats/anlz.py.
 * Every integer is big-endian. Which tags go in which file, their order,
 * the per-tag header lengths and fixed words follow docs/anlz.md (6,
 * writer checklist), as rekordbox writes them:
 *
 *   .DAT  PPTH PVBR PQTZ PWAV PWV2 PCOB(hot A-C) PCOB(memory)
 *   .EXT  PPTH PWV3 PCOB(hot D-H) PCOB(empty) PCO2(hot) PCO2(memory) PQT2 PWV5 PWV4 [PVB2]
 *   .2EX  PPTH PWV7 PWV6 PWVC
 *
 * PSSI (phrases) is not written: phrase analysis cannot be generated from
 * known rules and the CDJ-3000 accepts files without it (docs/anlz.md
 * 4.16). Waveform bytes come from the analysis module; this file only lays
 * them out. The readers are bounds-checked: inspect and pull run them on
 * files a player may have rewritten. */

#include "cyanbox/formats.h"

#include <math.h>
#include <stdio.h>

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"

/* --------------------------------------------------------- directory hash */

/* docs/anlz.md 1.1: the directory is hashed from the stick path's UTF-16
 * code units; players compute it themselves and ignore analyze_path. */
int cb_anlz_dir(const char *usb_path, char *buf, size_t size)
{
    uint16_t *u;
    size_t n;
    int ret = cb_utf8_to_utf16(usb_path, &u, &n);
    if (ret < 0)
        return ret;
    uint32_t h = 0;
    for (size_t i = 0; i < n; i++) {
        h = h * 0x5BC9u + u[i];
        h = h * 0x93B5u + u[i];
    }
    free(u);
    uint32_t r = h % 200003u;
    uint32_t p = (r & 0x01) | ((r >> 1) & 0x02) | ((r >> 4) & 0x04) | ((r >> 4) & 0x08) |
                 ((r >> 5) & 0x10) | ((r >> 8) & 0x20) | ((r >> 10) & 0x40);
    if (snprintf(buf, size, "/PIONEER/USBANLZ/P%03X/%08X", p, r) >= (int)size)
        return CB_ERROR(ENOBUFS);
    return 0;
}

/* --------------------------------------------------------------- writing */

/* A tag: fourcc, len_header, len_tag, then the body (header remainder and
 * payload). The body is built in `body` and appended to `out`. */
static void tag(CBBuf *out, const char *fourcc, uint32_t len_header, CBBuf *body)
{
    cb_buf_put(out, fourcc, 4);
    cb_buf_be32(out, len_header);
    cb_buf_be32(out, (uint32_t)(12 + body->len));
    cb_buf_put(out, body->data, body->len);
    if (body->err && !out->err)
        out->err = body->err;
    cb_buf_free(body);
}

static int file_bytes(CBBuf *body, uint8_t **out, size_t *size)
{
    CBBuf f = { 0 };
    cb_buf_put(&f, "PMAI", 4);
    cb_buf_be32(&f, 28);
    cb_buf_be32(&f, (uint32_t)(28 + body->len));
    /* Header words 0x0C-0x1B as rekordbox 6 and 7 write them [RB]. */
    cb_buf_be32(&f, 1);
    cb_buf_be32(&f, 0x10000);
    cb_buf_be32(&f, 0x10000);
    cb_buf_be32(&f, 0);
    cb_buf_put(&f, body->data, body->len);
    if (body->err && !f.err)
        f.err = body->err;
    cb_buf_free(body);
    return cb_buf_finish(&f, out, size);
}

/* PPTH: UTF-16BE including the terminating NUL; without it the CDJ-3000
 * rejects the file and re-analyses (docs/anlz.md 4.1). */
static void ppth(CBBuf *out, const char *path)
{
    uint16_t *u;
    size_t n;
    if (cb_utf8_to_utf16(path, &u, &n) < 0) {
        out->err = CB_ERROR(ENOMEM);
        return;
    }
    CBBuf b = { 0 };
    cb_buf_be32(&b, (uint32_t)(2 * (n + 1)));
    for (size_t i = 0; i < n; i++)
        cb_buf_be16(&b, u[i]);
    cb_buf_be16(&b, 0);
    free(u);
    tag(out, "PPTH", 0x10, &b);
}

/* PVBR: zero entries plus the sample count for CBR MP3, all zero for
 * everything else; rekordbox's VBR index is not understood (4.2). */
static void pvbr(CBBuf *out, uint32_t total_samples)
{
    CBBuf b = { 0 };
    cb_buf_be32(&b, 0);
    cb_buf_zeros(&b, 1600);
    cb_buf_be32(&b, total_samples);
    tag(out, "PVBR", 0x10, &b);
}

/* dj's ms(): floor(t * 1000 + 0.5). */
uint32_t cb_anlz_ms(double t)
{
    double v = floor(t * 1000.0 + 0.5);
    return v <= 0 ? 0 : v >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)v;
}

/* PQTZ: one entry per beat, beat number 1..4, BPM x 100 of its section and
 * time in ms; len_header 0x18 and the word 0x00080000 (4.3). */
static int pqtz(CBBuf *out, const CBGrid *grid, double duration)
{
    CBBeat *beats = NULL;
    int nb = 0;
    if (grid && grid->nb_sections > 0) {
        int ret = cb_grid_beats(grid, duration, &beats, &nb);
        if (ret < 0)
            return ret;
    }
    CBBuf b = { 0 };
    cb_buf_be32(&b, 0);
    cb_buf_be32(&b, 0x00080000);
    cb_buf_be32(&b, (uint32_t)nb);
    for (int i = 0; i < nb; i++) {
        cb_buf_be16(&b, (unsigned)beats[i].number);
        cb_buf_be16(&b, (unsigned)nearbyint(beats[i].bpm * 100.0));
        cb_buf_be32(&b, cb_anlz_ms(beats[i].time));
    }
    free(beats);
    tag(out, "PQTZ", 0x18, &b);
    return 0;
}

/* The empty PQT2 rekordbox writes without an extended grid (4.4). */
static void pqt2_empty(CBBuf *out)
{
    CBBuf b = { 0 };
    cb_buf_be32(&b, 0);
    cb_buf_be32(&b, 0x01000002);
    cb_buf_zeros(&b, 0x38 - 0x14);
    tag(out, "PQT2", 0x38, &b);
}

static void preview(CBBuf *out, const char *fourcc, const uint8_t *data, size_t n)
{
    CBBuf b = { 0 };
    cb_buf_be32(&b, (uint32_t)n);
    cb_buf_be32(&b, 0x00010000);
    cb_buf_put(&b, data, n);
    tag(out, fourcc, 0x14, &b);
}

static void detail(CBBuf *out, const char *fourcc, uint32_t entry_bytes, uint32_t word,
                   const uint8_t *data, uint32_t n)
{
    CBBuf b = { 0 };
    cb_buf_be32(&b, entry_bytes);
    cb_buf_be32(&b, n);
    cb_buf_be32(&b, word);
    cb_buf_put(&b, data, (size_t)n * entry_bytes);
    tag(out, fourcc, 0x18, &b);
}

/* (numerator, denominator) for loops a power of two beats long, within
 * 2 ms (dj's _loop_quantum; docs/anlz.md 4.6). dj took the first section's
 * tempo; the tempo at the cue is the same for a one-section grid and right
 * for the others. */
void cb_anlz_loop_quantum(const CBPlanCue *c, const CBGrid *grid, uint16_t *num, uint16_t *den)
{
    *num = *den = 0;
    double bpm = grid && grid->nb_sections ? cb_grid_bpm_at(grid, c->time) : 0.0;
    if (c->loop_end < 0 || bpm <= 0)
        return;
    double beats = (c->loop_end - c->time) * bpm / 60.0;
    for (int k = -5; k <= 8; k++) {
        double q = ldexp(1.0, k);
        if (fabs(beats - q) * 60.0 / bpm < 0.002) {
            *num = k >= 0 ? (uint16_t)q : 1;
            *den = k >= 0 ? 1 : (uint16_t)ldexp(1.0, -k);
            return;
        }
    }
}

static void pcpt(CBBuf *b, const CBPlanCue *c, unsigned prev, unsigned next, bool active)
{
    cb_buf_put(b, "PCPT", 4);
    cb_buf_be32(b, 0x1C);
    cb_buf_be32(b, 0x38);
    cb_buf_be32(b, c->hot < 0 ? 0 : (uint32_t)c->hot + 1);
    cb_buf_be32(b, active ? 4 : 0);
    cb_buf_be32(b, 0x00010000);
    cb_buf_be16(b, prev);
    cb_buf_be16(b, next);
    cb_buf_u8(b, c->loop_end >= 0 ? 2 : 1);
    cb_buf_u8(b, 0);
    cb_buf_be16(b, 1000);
    cb_buf_be32(b, cb_anlz_ms(c->time));
    cb_buf_be32(b, c->loop_end >= 0 ? cb_anlz_ms(c->loop_end) : 0xFFFFFFFFu);
    cb_buf_zeros(b, 16);
}

/* PCOB (4.5): hot lists in descending slot order, memory_count 0xFFFFFFFF;
 * memory lists doubly linked by index, memory_count the last index (an
 * XDJ-RX2 treats a non-empty list with 0xFFFFFFFF as empty and hangs). */
static void pcob(CBBuf *out, const CBPlanCue *const *cues, int n, bool hot, const CBPlanCue *active)
{
    CBBuf b = { 0 };
    cb_buf_be32(&b, hot ? 1 : 0);
    cb_buf_be16(&b, 0);
    cb_buf_be16(&b, (unsigned)n);
    cb_buf_be32(&b, hot || !n ? 0xFFFFFFFFu : (uint32_t)n - 1);
    for (int i = 0; i < n; i++) {
        if (hot)
            pcpt(&b, cues[i], 0xFFFF, 0xFFFF, false);
        else
            pcpt(&b, cues[i], i == 0 ? 0xFFFF : (unsigned)i - 1, i == n - 1 ? 0xFFFF : (unsigned)i + 1,
                 cues[i] == active);
    }
    tag(out, "PCOB", 0x18, &b);
}

/* PCP2 (4.6): hot cues carry the colour code and the pad RGB after the
 * comment, memory cues the track colour id at 0x1C; dj's entry length
 * with the 40-byte trailer, which hardware accepted. */
static void pcp2(CBBuf *out, const CBPlanCue *c, const CBGrid *grid)
{
    uint16_t num, den;
    cb_anlz_loop_quantum(c, grid, &num, &den);
    uint16_t *u = NULL;
    size_t nu = 0;
    if (c->name[0] && cb_utf8_to_utf16(c->name, &u, &nu) < 0) {
        out->err = CB_ERROR(ENOMEM);
        return;
    }
    uint32_t comment_len = c->name[0] ? (uint32_t)(2 * (nu + 1)) : 0;
    bool hot = c->hot >= 0;

    CBBuf b = { 0 };
    cb_buf_be32(&b, hot ? (uint32_t)c->hot + 1 : 0);
    cb_buf_u8(&b, c->loop_end >= 0 ? 2 : 1);
    cb_buf_u8(&b, 0);
    cb_buf_be16(&b, 1000);
    cb_buf_be32(&b, cb_anlz_ms(c->time));
    cb_buf_be32(&b, c->loop_end >= 0 ? cb_anlz_ms(c->loop_end) : 0xFFFFFFFFu);
    cb_buf_u8(&b, hot ? 0 : (unsigned)c->color_code);
    static const uint8_t unknown[7] = { 1, 0, 0, 0, 0, 0, 0 };
    cb_buf_put(&b, unknown, 7);
    cb_buf_be16(&b, num);
    cb_buf_be16(&b, den);
    cb_buf_be32(&b, comment_len);
    for (size_t i = 0; comment_len && i < nu; i++)
        cb_buf_be16(&b, u[i]);
    if (comment_len)
        cb_buf_be16(&b, 0);
    free(u);
    uint32_t rgb = hot ? c->rgb : 0;
    cb_buf_u8(&b, hot ? (unsigned)c->color_code : 0);
    cb_buf_u8(&b, rgb >> 16 & 0xFF);
    cb_buf_u8(&b, rgb >> 8 & 0xFF);
    cb_buf_u8(&b, rgb & 0xFF);
    cb_buf_zeros(&b, 40);

    cb_buf_put(out, "PCP2", 4);
    cb_buf_be32(out, 0x10);
    cb_buf_be32(out, (uint32_t)(12 + b.len));
    cb_buf_put(out, b.data, b.len);
    if (b.err && !out->err)
        out->err = b.err;
    cb_buf_free(&b);
}

static void pco2(CBBuf *out, const CBPlanCue *const *cues, int n, bool hot, const CBGrid *grid)
{
    CBBuf b = { 0 };
    cb_buf_be32(&b, hot ? 1 : 0);
    cb_buf_be16(&b, (unsigned)n);
    cb_buf_be16(&b, 0);
    for (int i = 0; i < n; i++)
        pcp2(&b, cues[i], grid);
    tag(out, "PCO2", 0x14, &b);
}

/* PVB2 (4.15): for FLAC, entry i is the frame containing sample
 * i * (total / 400): its first sample, its byte offset from the first
 * frame and its block size (baken's rule, byte-exact on 508 files; dj's
 * flac.seek_entries). */
static void pvb2(CBBuf *out, const CBFlacInfo *fi)
{
    const int count = 400;
    int64_t step = fi->total_samples / count;
    CBBuf b = { 0 };
    cb_buf_be32(&b, 0);
    cb_buf_be64(&b, (uint64_t)fi->total_samples);
    cb_buf_be32(&b, (uint32_t)count);
    cb_buf_be32(&b, 20);
    int f = 0;
    for (int i = 0; i < count; i++) {
        int64_t want = (int64_t)i * step;
        while (f + 1 < fi->nb_frames && fi->frames[f + 1].sample <= want)
            f++;
        int64_t first = fi->nb_frames ? fi->frames[f].sample : 0;
        int64_t offset = fi->nb_frames ? fi->frames[f].offset : 0;
        int64_t bs = fi->nb_frames ? fi->frames[f].block : 0;
        cb_buf_be64(&b, (uint64_t)first);
        cb_buf_be64(&b, (uint64_t)offset);
        cb_buf_be32(&b, (uint32_t)bs);
    }
    tag(out, "PVB2", 0x20, &b);
}

static int cmp_time(const void *a, const void *b)
{
    const CBPlanCue *x = *(const CBPlanCue *const *)a, *y = *(const CBPlanCue *const *)b;
    return x->time < y->time ? -1 : x->time > y->time;
}

static int cmp_hot_desc(const void *a, const void *b)
{
    const CBPlanCue *x = *(const CBPlanCue *const *)a, *y = *(const CBPlanCue *const *)b;
    return y->hot - x->hot;
}

void cb_anlz_files_free(CBAnlzFiles *f)
{
    free(f->dat);
    free(f->ext);
    free(f->ex2);
    *f = (CBAnlzFiles) { 0 };
}

int cb_anlz_build(const CBPlanTrack *t, const CBWaveforms *waves, CBAnlzFiles *out)
{
    *out = (CBAnlzFiles) { 0 };
    int n = t->nb_cues;
    const CBPlanCue **hot = calloc((size_t)CB_MAX(n, 1), sizeof(*hot));
    const CBPlanCue **mem = calloc((size_t)CB_MAX(n, 1), sizeof(*mem));
    const CBPlanCue **sel = calloc((size_t)CB_MAX(n, 1), sizeof(*sel));
    if (!hot || !mem || !sel) {
        free(hot);
        free(mem);
        free(sel);
        return CB_ERROR(ENOMEM);
    }
    int nh = 0, nm = 0;
    for (int i = 0; i < n; i++) {
        if (t->cues[i].hot >= 0)
            hot[nh++] = &t->cues[i];
        else
            mem[nm++] = &t->cues[i];
    }
    qsort(mem, (size_t)nm, sizeof(*mem), cmp_time);
    /* Descending slot, the order rekordbox mostly uses (4.5). */
    qsort(hot, (size_t)nh, sizeof(*hot), cmp_hot_desc);
    /* The active loop: the first memory loop flagged active (status 4, in
     * the .DAT memory list only, at most one per track). */
    const CBPlanCue *active = NULL;
    for (int i = 0; i < nm && !active; i++)
        if (mem[i]->active && mem[i]->loop_end >= 0)
            active = mem[i];

    const char *path = t->usb_path ? t->usb_path : "";
    bool is_mp3 = !strcmp(t->probe.codec, "mp3");
    uint32_t mp3_samples = 0;
    if (is_mp3 && t->probe.mp3.valid && !t->probe.mp3.vbr)
        mp3_samples = (uint32_t)(t->probe.mp3.nb_frames * t->probe.mp3.frame_samples);
    uint32_t nd = waves ? (uint32_t)waves->nb_detail : 0;

    int ret = 0;
    CBBuf dat = { 0 }, ext = { 0 }, ex2 = { 0 };

    /* .DAT */
    ppth(&dat, path);
    pvbr(&dat, mp3_samples);
    if ((ret = pqtz(&dat, &t->grid, t->duration)) < 0)
        goto fail;
    if (waves) {
        preview(&dat, "PWAV", waves->pwav, CB_WAVE_PWAV_COLUMNS);
        preview(&dat, "PWV2", waves->pwv2, CB_WAVE_PWV2_COLUMNS);
    }
    /* Hot cues A-C only in the .DAT, D-H only in the .EXT; writing all of
     * them into both gives old players pads they should not have (4.5). */
    int ns = 0;
    for (int i = 0; i < nh; i++)
        if (hot[i]->hot < 3)
            sel[ns++] = hot[i];
    pcob(&dat, sel, ns, true, NULL);
    pcob(&dat, mem, nm, false, active);

    /* .EXT */
    ppth(&ext, path);
    if (waves && nd)
        detail(&ext, "PWV3", 1, 0x00960000, waves->pwv3, nd);
    ns = 0;
    for (int i = 0; i < nh; i++)
        if (hot[i]->hot >= 3)
            sel[ns++] = hot[i];
    pcob(&ext, sel, ns, true, NULL);
    pcob(&ext, NULL, 0, false, NULL);
    /* PCO2 hot list in ascending slot order (dj). */
    for (int i = 0; i < nh; i++)
        sel[i] = hot[nh - 1 - i];
    pco2(&ext, sel, nh, true, &t->grid);
    pco2(&ext, mem, nm, false, &t->grid);
    pqt2_empty(&ext);
    if (waves) {
        if (nd)
            detail(&ext, "PWV5", 2, 0x00960305, waves->pwv5, nd);
        CBBuf b = { 0 };
        cb_buf_be32(&b, 6);
        cb_buf_be32(&b, CB_WAVE_PWV4_COLUMNS);
        cb_buf_be32(&b, 0);
        cb_buf_put(&b, waves->pwv4, sizeof(waves->pwv4));
        tag(&ext, "PWV4", 0x18, &b);
    }
    if (!strcmp(t->probe.codec, "flac") && t->probe.flac.valid && t->probe.flac.total_samples > 0)
        pvb2(&ext, &t->probe.flac);

    /* .2EX */
    ppth(&ex2, path);
    if (waves) {
        if (nd)
            detail(&ex2, "PWV7", 3, 0x00960000, waves->pwv7, nd);
        CBBuf b = { 0 };
        cb_buf_be32(&b, 3);
        cb_buf_be32(&b, CB_WAVE_PWV6_COLUMNS);
        cb_buf_put(&b, waves->pwv6, sizeof(waves->pwv6));
        tag(&ex2, "PWV6", 0x14, &b);
        /* PWVC: the low, mid and high gains in rekordbox's range (4.14):
         * dj's values, kept since PWV7 moved to rbxport's levels, which
         * rbxport itself writes without a PWVC; without them (all zero) the
         * neutral values the document gives for that case. */
        bool have = waves->gains[0] || waves->gains[1] || waves->gains[2];
        b = (CBBuf) { 0 };
        cb_buf_be16(&b, 0);
        cb_buf_be16(&b, have ? waves->gains[0] : 80);
        cb_buf_be16(&b, have ? waves->gains[1] : 100);
        cb_buf_be16(&b, have ? waves->gains[2] : 100);
        tag(&ex2, "PWVC", 0x0E, &b);
    }

    if ((ret = file_bytes(&dat, &out->dat, &out->dat_size)) < 0 ||
        (ret = file_bytes(&ext, &out->ext, &out->ext_size)) < 0 ||
        (ret = file_bytes(&ex2, &out->ex2, &out->ex2_size)) < 0)
        goto fail;
    free(hot);
    free(mem);
    free(sel);
    return 0;

fail:
    cb_buf_free(&dat);
    cb_buf_free(&ext);
    cb_buf_free(&ex2);
    cb_anlz_files_free(out);
    free(hot);
    free(mem);
    free(sel);
    return ret;
}

/* --------------------------------------------------------------- reading */

int cb_anlz_parse(const uint8_t *data, size_t size, CBAnlzTag **tags, int *nb)
{
    *tags = NULL;
    *nb = 0;
    if (size < 12 || memcmp(data, "PMAI", 4))
        return CB_ERROR(EINVAL);
    /* len_header is 28 in every file seen (docs/anlz.md 3); one that does
     * not fit the file is not an empty file but a broken one. */
    size_t pos = cb_rd_be32(data + 4);
    if (pos < 12 || pos > size)
        return CB_ERROR(EINVAL);
    CBArray a;
    cb_array_init(&a, sizeof(CBAnlzTag));
    while (pos + 12 <= size) {
        uint32_t len = cb_rd_be32(data + pos + 8);
        if (len < 12 || pos + len > size) {
            cb_array_free(&a);
            return CB_ERROR(EINVAL);
        }
        CBAnlzTag t = {
            .body     = data + pos + 12,
            .size     = len - 12,
            .raw      = data + pos,
            .raw_size = len,
        };
        memcpy(t.fourcc, data + pos, 4);
        if (!cb_array_push(&a, &t)) {
            cb_array_free(&a);
            return CB_ERROR(ENOMEM);
        }
        pos += len;
    }
    if (pos != size && pos < size) {
        cb_array_free(&a);
        return CB_ERROR(EINVAL);
    }
    *tags = a.data;
    *nb = (int)a.nb;
    return 0;
}

static const CBAnlzTag *find_tag(const CBAnlzTag *tags, int nb, const char *fourcc)
{
    for (int i = 0; i < nb; i++)
        if (!memcmp(tags[i].fourcc, fourcc, 4))
            return &tags[i];
    return NULL;
}

int cb_anlz_read_path(const uint8_t *data, size_t size, char **path)
{
    *path = NULL;
    CBAnlzTag *tags;
    int nb;
    int ret = cb_anlz_parse(data, size, &tags, &nb);
    if (ret < 0)
        return ret;
    const CBAnlzTag *t = find_tag(tags, nb, "PPTH");
    ret = CB_ERROR(ENOENT);
    if (t && t->raw_size >= 0x10) {
        uint32_t len = cb_rd_be32(t->raw + 0x0C);
        if (0x10 + (size_t)len <= t->raw_size) {
            /* Drop the terminating NUL. */
            size_t n = len;
            while (n >= 2 && !t->raw[0x10 + n - 2] && !t->raw[0x10 + n - 1])
                n -= 2;
            *path = cb_utf16be_to_utf8(t->raw + 0x10, n);
            ret = *path ? 0 : CB_ERROR(ENOMEM);
        } else {
            ret = CB_ERROR(EINVAL);
        }
    }
    free(tags);
    return ret;
}

int cb_anlz_read_beats(const uint8_t *data, size_t size, CBBeat **beats, int *nb)
{
    *beats = NULL;
    *nb = 0;
    CBAnlzTag *tags;
    int nt;
    int ret = cb_anlz_parse(data, size, &tags, &nt);
    if (ret < 0)
        return ret;
    const CBAnlzTag *t = find_tag(tags, nt, "PQTZ");
    if (!t || t->raw_size < 0x18) {
        free(tags);
        return CB_ERROR(ENOENT);
    }
    uint32_t n = cb_rd_be32(t->raw + 0x14);
    if (0x18 + (size_t)n * 8 > t->raw_size) {
        free(tags);
        return CB_ERROR(EINVAL);
    }
    CBBeat *b = malloc((size_t)CB_MAX(n, 1) * sizeof(*b));
    if (!b) {
        free(tags);
        return CB_ERROR(ENOMEM);
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = t->raw + 0x18 + 8 * i;
        b[i] = (CBBeat) {
            .number = cb_rd_be16(e),
            .bpm    = cb_rd_be16(e + 2) / 100.0,
            .time   = cb_rd_be32(e + 4) / 1000.0,
        };
    }
    free(tags);
    *beats = b;
    *nb = (int)n;
    return 0;
}

void cb_anlz_cue_entries_free(CBAnlzCueEntry *e, int nb)
{
    for (int i = 0; e && i < nb; i++)
        free(e[i].comment);
    free(e);
}

int cb_anlz_pcob_header(const CBAnlzTag *t, uint32_t *list_type, uint32_t *count_word)
{
    if (memcmp(t->fourcc, "PCOB", 4) || t->raw_size < 0x18)
        return CB_ERROR(EINVAL);
    *list_type = cb_rd_be32(t->raw + 0x0C);
    *count_word = cb_rd_be32(t->raw + 0x14);
    return 0;
}

int cb_anlz_tag_cues(const CBAnlzTag *t, CBAnlzCueEntry **out, int *nb, int *declared)
{
    *out = NULL;
    *nb = 0;
    *declared = 0;
    const uint8_t *r = t->raw;
    size_t size = t->raw_size;
    bool pcob = !memcmp(t->fourcc, "PCOB", 4);
    if (!pcob && memcmp(t->fourcc, "PCO2", 4))
        return CB_ERROR(EINVAL);
    if (size < (pcob ? 0x18u : 0x14u))
        return CB_ERROR(EINVAL);
    int n = pcob ? cb_rd_be16(r + 0x12) : cb_rd_be16(r + 0x10);
    *declared = n;
    CBAnlzCueEntry *e = calloc((size_t)CB_MAX(n, 1), sizeof(*e));
    if (!e)
        return CB_ERROR(ENOMEM);
    int k = 0;
    if (pcob) {
        for (int i = 0; i < n; i++) {
            size_t at = 0x18 + 0x38 * (size_t)i;
            if (at + 0x38 > size || memcmp(r + at, "PCPT", 4))
                break;
            const uint8_t *p = r + at;
            e[k++] = (CBAnlzCueEntry) {
                .hot    = cb_rd_be32(p + 0x0C),
                .status = cb_rd_be32(p + 0x10),
                .prev   = cb_rd_be16(p + 0x18),
                .next   = cb_rd_be16(p + 0x1A),
                .type   = p[0x1C],
                .time   = cb_rd_be32(p + 0x20),
                .end    = cb_rd_be32(p + 0x24),
            };
        }
    } else {
        size_t pos = 0x14;
        for (int i = 0; i < n; i++) {
            /* Real entries can stop after the comment, or before it
             * (docs/anlz.md 4.6 len_entry variants). */
            if (pos + 0x1D > size || memcmp(r + pos, "PCP2", 4))
                break;
            uint32_t len = cb_rd_be32(r + pos + 8);
            if (len < 0x1D || pos + len > size)
                break;
            const uint8_t *p = r + pos;
            CBAnlzCueEntry *x = &e[k++];
            *x = (CBAnlzCueEntry) {
                .hot      = cb_rd_be32(p + 0x0C),
                .type     = p[0x10],
                .time     = cb_rd_be32(p + 0x14),
                .end      = cb_rd_be32(p + 0x18),
                .color_id = p[0x1C],
            };
            if (len >= 0x2C) {
                x->num = cb_rd_be16(p + 0x24);
                x->den = cb_rd_be16(p + 0x26);
                uint32_t lc = cb_rd_be32(p + 0x28);
                if (lc && 0x2C + (size_t)lc <= len) {
                    size_t nbytes = lc;
                    while (nbytes >= 2 && !p[0x2C + nbytes - 2] && !p[0x2C + nbytes - 1])
                        nbytes -= 2;
                    x->comment = cb_utf16be_to_utf8(p + 0x2C, nbytes);
                    if (!x->comment) {
                        cb_anlz_cue_entries_free(e, k);
                        return CB_ERROR(ENOMEM);
                    }
                }
                if (0x2C + (size_t)lc + 4 <= len) {
                    x->has_color = true;
                    memcpy(x->color, p + 0x2C + lc, 4);
                }
            }
            pos += len;
        }
    }
    *out = e;
    *nb = k;
    return 0;
}

/* Cues of one file: its PCO2 lists when it has them (complete: A-H, names
 * and colours), else its PCOB lists. Active loops come from PCOB status 4,
 * which only the .DAT carries. */
int cb_anlz_read_cues(const uint8_t *data, size_t size, CBPlanCue **cues, int *nb)
{
    *cues = NULL;
    *nb = 0;
    CBAnlzTag *tags;
    int nt;
    int ret = cb_anlz_parse(data, size, &tags, &nt);
    if (ret < 0)
        return ret;
    bool has_pco2 = find_tag(tags, nt, "PCO2") != NULL;
    CBArray a;
    cb_array_init(&a, sizeof(CBPlanCue));
    for (int i = 0; i < nt; i++) {
        if (memcmp(tags[i].fourcc, has_pco2 ? "PCO2" : "PCOB", 4))
            continue;
        CBAnlzCueEntry *e;
        int n, declared;
        if ((ret = cb_anlz_tag_cues(&tags[i], &e, &n, &declared)) < 0)
            goto fail;
        for (int k = 0; k < n; k++) {
            /* Slots are 1..8 (A..H, docs/anlz.md 4.5); anything else is
             * not a cue the library can hold, and not a memory cue. */
            if (e[k].hot > CB_HOT_CUE_SLOTS)
                continue;
            CBPlanCue c = {
                .time     = e[k].time / 1000.0,
                .loop_end = e[k].type == 2 && e[k].end != 0xFFFFFFFFu ? e[k].end / 1000.0 : -1.0,
                .hot      = e[k].hot ? (int)e[k].hot - 1 : -1,
                .active   = e[k].status == 4,
            };
            if (has_pco2) {
                if (c.hot >= 0 && e[k].has_color) {
                    c.color_code = e[k].color[0];
                    c.rgb = (uint32_t)e[k].color[1] << 16 | (uint32_t)e[k].color[2] << 8 | e[k].color[3];
                } else if (c.hot < 0) {
                    c.color_code = e[k].color_id;
                }
                if (e[k].comment)
                    snprintf(c.name, sizeof(c.name), "%s", e[k].comment);
            }
            if (!cb_array_push(&a, &c)) {
                cb_anlz_cue_entries_free(e, n);
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
        }
        cb_anlz_cue_entries_free(e, n);
    }
    free(tags);
    *cues = a.data;
    *nb = (int)a.nb;
    return 0;

fail:
    free(tags);
    cb_array_free(&a);
    return ret;
}

int cb_anlz_read_waveforms(const uint8_t *data, size_t size, CBWaveforms *w)
{
    CBAnlzTag *tags;
    int nt;
    int ret = cb_anlz_parse(data, size, &tags, &nt);
    if (ret < 0)
        return ret;
    for (int i = 0; i < nt; i++) {
        const CBAnlzTag *t = &tags[i];
        const uint8_t *r = t->raw;
        if ((!memcmp(t->fourcc, "PWAV", 4) || !memcmp(t->fourcc, "PWV2", 4)) && t->raw_size >= 0x14) {
            bool pwav = t->fourcc[3] == 'V';
            size_t want = pwav ? CB_WAVE_PWAV_COLUMNS : CB_WAVE_PWV2_COLUMNS;
            if (cb_rd_be32(r + 0x0C) == want && 0x14 + want <= t->raw_size)
                memcpy(pwav ? w->pwav : w->pwv2, r + 0x14, want);
        } else if (!memcmp(t->fourcc, "PWV4", 4) && t->raw_size >= 0x18 + sizeof(w->pwv4)) {
            memcpy(w->pwv4, r + 0x18, sizeof(w->pwv4));
        } else if (!memcmp(t->fourcc, "PWV6", 4) && t->raw_size >= 0x14 + sizeof(w->pwv6)) {
            memcpy(w->pwv6, r + 0x14, sizeof(w->pwv6));
        } else if ((!memcmp(t->fourcc, "PWV3", 4) || !memcmp(t->fourcc, "PWV5", 4) ||
                    !memcmp(t->fourcc, "PWV7", 4)) && t->raw_size >= 0x18) {
            uint32_t eb = cb_rd_be32(r + 0x0C), n = cb_rd_be32(r + 0x10);
            uint8_t **dst = t->fourcc[3] == '3' ? &w->pwv3 : t->fourcc[3] == '5' ? &w->pwv5 : &w->pwv7;
            uint32_t want_eb = t->fourcc[3] == '3' ? 1 : t->fourcc[3] == '5' ? 2 : 3;
            if (eb != want_eb || 0x18 + (size_t)n * eb > t->raw_size)
                continue;
            if (w->nb_detail && (uint32_t)w->nb_detail != n) {
                ret = CB_ERROR(EINVAL);
                break;
            }
            uint8_t *d = malloc(CB_MAX((size_t)n * eb, 1));
            if (!d) {
                ret = CB_ERROR(ENOMEM);
                break;
            }
            memcpy(d, r + 0x18, (size_t)n * eb);
            free(*dst);
            *dst = d;
            w->nb_detail = (int)n;
        }
    }
    free(tags);
    return ret;
}
