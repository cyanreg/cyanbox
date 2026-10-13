/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* export.pdb and exportExt.pdb, the DeviceSQL "Device Library" the
 * CDJ-3000 and older players read. A port of dj's formats/pdb.py and the
 * table assembly in its export.py (build_pdb, build_pdb_ext). Layout and
 * the hardware-critical rules are in docs/pdb.md: long strings 4-aligned
 * in their row (G1), rekordbox's row allocation slack (2.6, G6), header
 * sequence above every page's (G3), tables 0x11-0x13 populated (G4).
 * Everything is little-endian.
 *
 * Page layout (baken's, verified on CDJ-3000 and NXS2, docs/pdb.md 2.7):
 * the header page, then per table its index page followed by its data
 * pages, then one zero page per table as its empty candidate. The colour,
 * column, menu and sort pages are byte-identical to rekordbox 7.2.18's
 * (tests/formats/test_pdb.c against tests/data/rekordbox-7.2.18-export.pdb).
 *
 * Rows are kept as bytes plus their allocation; the only slot-dependent
 * part of any row is the u16 index_shift at offset 2 (0x20 x slot,
 * docs/pdb.md 2.10), patched in when the page is laid out.
 *
 * The readers (rx_pdb_read and the private ones in formats_priv.h) are
 * bounds-checked throughout: inspect runs them on whatever is on a stick. */

#include "rxbox/formats.h"

#include <math.h>
#include <stdio.h>

#include "rxbox/colors.h"
#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"

#define NO_PAGE 0x03FFFFFFu
#define MAX_ROWS_PER_PAGE 2047

static uint32_t align4(uint32_t n)
{
    return (n + 3) & ~3u;
}

/* ------------------------------------------------------------ strings */

/* DeviceSQL strings the way rekordbox writes them (docs/pdb.md 2.9): short
 * ASCII up to 126 bytes, long ASCII (0x40) beyond, UTF-16LE (0x90) for
 * anything non-ASCII; no terminator. */
static int put_dstr(RXBuf *b, const char *s)
{
    if (!s)
        s = "";
    size_t n = strlen(s);
    bool ascii = true;
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)s[i] >= 0x80)
            ascii = false;
    if (ascii) {
        if (n <= 126) {
            rx_buf_u8(b, (unsigned)((n + 1) << 1 | 1));
            rx_buf_put(b, s, n);
            return b->err;
        }
        if (n + 4 > 0xFFFF)
            return RX_ERROR(E2BIG);
        rx_buf_u8(b, 0x40);
        rx_buf_le16(b, (unsigned)(n + 4));
        rx_buf_u8(b, 0);
        rx_buf_put(b, s, n);
        return b->err;
    }
    uint16_t *u;
    size_t nu;
    int ret = rx_utf8_to_utf16(s, &u, &nu);
    if (ret < 0)
        return ret;
    if (2 * nu + 4 > 0xFFFF) {
        free(u);
        return RX_ERROR(E2BIG);
    }
    rx_buf_u8(b, 0x90);
    rx_buf_le16(b, (unsigned)(2 * nu + 4));
    rx_buf_u8(b, 0);
    for (size_t i = 0; i < nu; i++)
        rx_buf_le16(b, u[i]);
    free(u);
    return b->err;
}

static bool is_long(const uint8_t *enc)
{
    return !(enc[0] & 1);
}

int rx_pdb_string_encode(const char *utf8, uint8_t **out, size_t *size)
{
    RXBuf b = { 0 };
    int ret = put_dstr(&b, utf8);
    if (ret < 0) {
        rx_buf_free(&b);
        return ret;
    }
    return rx_buf_finish(&b, out, size);
}

static char *ascii_copy(const uint8_t *p, size_t n)
{
    /* Each byte above ASCII becomes U+FFFD, three bytes. */
    size_t high = 0;
    for (size_t i = 0; i < n; i++)
        high += p[i] >= 0x80;
    char *s = malloc(n + 2 * high + 1);
    if (!s)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        /* Python's decode("ascii", errors="replace"). */
        if (p[i] >= 0x80)
            o += (size_t)rx_utf8_put(s + o, 0xFFFD);
        else
            s[o++] = (char)p[i];
    }
    s[o] = '\0';
    return s;
}

/* `isrc`: the track's string 0 may be the 0x90/0x03 ISRC form. dj looked
 * for it in every string, which misreads a UTF-16 string starting with a
 * code unit whose low byte is 3 (U+0103 'ă'); only slot 0 can hold one.
 * `size` is where the string must end by: the file's size for a string
 * on its own, the end of its row's room for a row's (rx_pdb_table_rows). */
static int read_dstr(const uint8_t *b, size_t size, size_t pos, bool isrc, char **out)
{
    *out = NULL;
    if (pos >= size)
        return RX_ERROR(EINVAL);
    uint8_t k = b[pos];
    if (k & 1) {
        size_t n = (size_t)(k >> 1) - 1;
        if (k >> 1 == 0 || pos + 1 + n > size)
            return RX_ERROR(EINVAL);
        *out = ascii_copy(b + pos + 1, n);
        return *out ? 0 : RX_ERROR(ENOMEM);
    }
    if (pos + 4 > size)
        return RX_ERROR(EINVAL);
    size_t total = rx_rd_le16(b + pos + 1);
    if (total < 4 || pos + total > size)
        return RX_ERROR(EINVAL);
    const uint8_t *body = b + pos + 4;
    size_t n = total - 4;
    if (k == 0x40) {
        *out = ascii_copy(body, n);
    } else if (k == 0x90) {
        if (isrc && n >= 2 && body[0] == 0x03)
            *out = ascii_copy(body + 1, n - 2);
        else
            *out = rx_utf16le_to_utf8(body, n);
    } else {
        return RX_ERROR(EINVAL);
    }
    return *out ? 0 : RX_ERROR(ENOMEM);
}

int rx_pdb_string_decode(const uint8_t *data, size_t size, char **utf8)
{
    return read_dstr(data, size, 0, false, utf8);
}

int rx_pdb_read_string(const uint8_t *b, size_t size, size_t pos, char **utf8)
{
    return read_dstr(b, size, pos, false, utf8);
}

/* --------------------------------------------------------------- rows */

typedef struct Row {
    uint8_t *data;
    uint32_t len, alloc;
    bool     shift;               /* u16 index_shift at offset 2 */
} Row;

typedef struct Table {
    Row *rows;
    int  nb, cap;
} Table;

static void tables_free(Table *t, int nb)
{
    for (int i = 0; i < nb; i++) {
        for (int r = 0; r < t[i].nb; r++)
            free(t[i].rows[r].data);
        free(t[i].rows);
    }
}

/* Takes the buffer's bytes. alloc 0 means align4(len). */
static int add_row(Table *t, RXBuf *b, uint32_t alloc, bool shift)
{
    if (b->err) {
        int err = b->err;
        rx_buf_free(b);
        return err;
    }
    if (t->nb == t->cap) {
        int cap = t->cap ? 2 * t->cap : 64;
        Row *r = realloc(t->rows, (size_t)cap * sizeof(*r));
        if (!r) {
            rx_buf_free(b);
            return RX_ERROR(ENOMEM);
        }
        t->rows = r;
        t->cap = cap;
    }
    uint32_t len = (uint32_t)b->len;
    t->rows[t->nb++] = (Row) {
        .data  = b->data,
        .len   = len,
        .alloc = alloc ? alloc : align4(len),
        .shift = shift,
    };
    *b = (RXBuf) { 0 };
    return 0;
}

static int add_bytes_row(Table *t, const uint8_t *p, size_t n, uint32_t alloc)
{
    RXBuf b = { 0 };
    rx_buf_put(&b, p, n);
    return add_row(t, &b, alloc, false);
}

/* Strings laid out after a fixed part: long strings start 4-aligned
 * relative to the row (docs/pdb.md 2.9, G1). */
typedef struct Strings {
    RXBuf s[21];
    int   nb;
} Strings;

static void strings_free(Strings *st)
{
    for (int i = 0; i < st->nb; i++)
        rx_buf_free(&st->s[i]);
}

static int track_strings(const RXPlan *plan, const RXPlanTrack *t, Strings *st)
{
    char title[1024], date_added[16];
    rx_plan_title(t, title, sizeof(title));
    rx_plan_date_added(t, date_added);
    char *anlz = rx_plan_anlz_path(t, ".DAT");
    if (!anlz)
        return RX_ERROR(ENOMEM);
    const char *path = t->usb_path ? t->usb_path : "";
    const char *comment = t->track && t->track->meta.comment ? t->track->meta.comment : "";

    /* Slots as dj writes them (docs/pdb.md 2.11 string table): 2..4 are
     * small counters ("2", "3", "1"), 6 KUVO "ON", 7 hot cue auto load. */
    const char *vals[21] = {
        "", "", "2", "3", "1", "", "ON", plan->hot_cue_auto_load ? "ON" : "", "", "",
        date_added, "", "", "", anlz, plan->date, comment, title, "", rx_path_base(path), path,
    };
    int ret = 0;
    /* All cleared first: a string that fails (longer than a DeviceSQL
     * length holds) leaves the later ones unwritten but freeable. */
    *st = (Strings) { .nb = 21 };
    for (int i = 0; i < 21; i++)
        if ((ret = put_dstr(&st->s[i], vals[i])) < 0)
            break;
    free(anlz);
    if (ret < 0)
        strings_free(st);
    return ret;
}

/* The fixed part, the 21 offsets and the strings (docs/pdb.md 2.11). */
static int track_row(const RXPlan *plan, const RXPlanTrack *t, RXBuf *row, uint32_t *alloc)
{
    Strings st;
    int ret = track_strings(plan, t, &st);
    if (ret < 0)
        return ret;

    const RXTrack *tr = t->track;
    const RXMeta *m = tr ? &tr->meta : NULL;
    /* The first section's tempo; on a dynamic grid the median of its
     * beats' (rxbox/model.h): the players show one tempo per track. */
    double bpm = rx_grid_bpm_median(&t->grid, t->duration);
    /* Python's round(): half to even, which nearbyint does by default. */
    uint32_t tempo = (uint32_t)nearbyint(bpm * 100.0);
    int64_t dur = (int64_t)t->duration;

    rx_buf_le16(row, 0x0024);                     /* subtype */
    rx_buf_le16(row, 0);                          /* index_shift, patched per slot */
    rx_buf_le32(row, 0x000C0700);                 /* "bitmask" [RB] */
    rx_buf_le32(row, (uint32_t)t->sample_rate);
    rx_buf_le32(row, t->composer_id);
    rx_buf_le32(row, (uint32_t)t->file_size);
    rx_buf_le32(row, rx_plan_master_id(t));       /* 0x14: unique per track */
    rx_buf_le32(row, rx_plan_library_id(plan));   /* 0x18: one per database */
    rx_buf_le32(row, t->artwork_id);
    rx_buf_le32(row, t->key_id);
    rx_buf_le32(row, t->original_artist_id);
    rx_buf_le32(row, t->label_id);
    rx_buf_le32(row, t->remixer_id);
    rx_buf_le32(row, (uint32_t)t->bit_rate);
    rx_buf_le32(row, m ? (uint32_t)m->track_number : 0);
    rx_buf_le32(row, tempo);
    rx_buf_le32(row, t->genre_id);
    rx_buf_le32(row, t->album_id);
    rx_buf_le32(row, t->artist_id);
    rx_buf_le32(row, t->id);
    rx_buf_le16(row, m ? (unsigned)m->disc_number : 0);
    rx_buf_le16(row, 0);                          /* play count */
    rx_buf_le16(row, m ? (unsigned)m->year : 0);
    rx_buf_le16(row, (unsigned)t->bit_depth);
    rx_buf_le16(row, (unsigned)RX_CLAMP(dur, 0, 0xFFFF));
    rx_buf_le16(row, 0x0029);                     /* [RB] always 41 */
    rx_buf_u8(row, t->color_id);
    rx_buf_u8(row, m ? (unsigned)RX_CLAMP(m->rating, 0, 5) : 0);
    rx_buf_le16(row, (unsigned)t->file_type);
    rx_buf_le16(row, 0x0003);                     /* offset-array magic */

    uint32_t offs[21];
    uint32_t pos = 0x88, sum = 0;
    for (int i = 0; i < 21; i++) {
        if (is_long(st.s[i].data) && pos % 4)
            pos = align4(pos);
        offs[i] = pos;
        pos += (uint32_t)st.s[i].len;
        sum += align4((uint32_t)st.s[i].len);
    }
    if (pos > 0xFFFF) {
        strings_free(&st);
        return RX_ERROR(E2BIG);
    }
    for (int i = 0; i < 21; i++)
        rx_buf_le16(row, offs[i]);
    for (int i = 0; i < 21; i++) {
        if (is_long(st.s[i].data))
            rx_buf_zeros(row, align4((uint32_t)row->len) - row->len);
        rx_buf_put(row, st.s[i].data, st.s[i].len);
    }
    strings_free(&st);
    /* rekordbox's slack: 0x88 + the 4-aligned strings + 4, never below 224
     * (rows shorter than 221 bytes crashed a CDJ-350, docs/pdb.md G6). */
    *alloc = RX_MAX(224u, 0x88 + sum + 4);
    return row->err;
}

int rx_pdb_track_row_size(const RXPlanTrack *t, const RXPlan *plan, size_t *len, size_t *alloc)
{
    RXBuf row = { 0 };
    uint32_t a;
    int ret = track_row(plan, t, &row, &a);
    if (ret >= 0) {
        *len = row.len;
        *alloc = a;
    }
    rx_buf_free(&row);
    return ret;
}

static uint32_t dir_bytes(uint32_t n)
{
    return 2 * n + 4 * ((n + 15) / 16);
}

int rx_pdb_track_row_overflow(const RXPlanTrack *t, const RXPlan *plan)
{
    size_t len, alloc;
    int ret = rx_pdb_track_row_size(t, plan, &len, &alloc);
    if (ret == RX_ERROR(E2BIG))
        return 0x10000;
    if (ret < 0)
        return 0;
    /* A row alone on a page: alloc + one slot of row index <= the heap. */
    size_t room = RX_PDB_PAGE - RX_PDB_HEAP - dir_bytes(1);
    return alloc > room ? (int)(alloc - room) : 0;
}

/* genres, labels, artwork: u32 id, name at +4. */
static int id_name_row(Table *t, uint32_t id, const char *name)
{
    RXBuf b = { 0 };
    rx_buf_le32(&b, id);
    int ret = put_dstr(&b, name);
    if (ret < 0) {
        rx_buf_free(&b);
        return ret;
    }
    return add_row(t, &b, 0, false);
}

static int key_row(Table *t, uint32_t id, const char *name)
{
    RXBuf b = { 0 };
    rx_buf_le32(&b, id);
    rx_buf_le32(&b, id);
    int ret = put_dstr(&b, name);
    if (ret < 0) {
        rx_buf_free(&b);
        return ret;
    }
    return add_row(t, &b, 0, false);
}

/* Near (u8) offsets always, name right after the header: 0x0A short,
 * 0x0C long with two pad bytes (docs/pdb.md artists, G22). */
static int artist_row(Table *t, uint32_t id, const char *name)
{
    RXBuf s = { 0 };
    int ret = put_dstr(&s, name);
    if (ret < 0) {
        rx_buf_free(&s);
        return ret;
    }
    bool lng = is_long(s.data);
    RXBuf b = { 0 };
    rx_buf_le16(&b, 0x0060);
    rx_buf_le16(&b, 0);
    rx_buf_le32(&b, id);
    rx_buf_u8(&b, 0x03);
    rx_buf_u8(&b, lng ? 0x0C : 0x0A);
    if (lng)
        rx_buf_zeros(&b, 2);
    rx_buf_put(&b, s.data, s.len);
    uint32_t alloc = 12 + align4((uint32_t)s.len) + 4;
    rx_buf_free(&s);
    return add_row(t, &b, alloc, true);
}

static int album_row(Table *t, uint32_t id, const char *name, uint32_t artist_id)
{
    RXBuf s = { 0 };
    int ret = put_dstr(&s, name);
    if (ret < 0) {
        rx_buf_free(&s);
        return ret;
    }
    bool lng = is_long(s.data);
    RXBuf b = { 0 };
    rx_buf_le16(&b, 0x0080);
    rx_buf_le16(&b, 0);
    rx_buf_le32(&b, 0);
    rx_buf_le32(&b, artist_id);
    rx_buf_le32(&b, id);
    rx_buf_le32(&b, 0);
    rx_buf_u8(&b, 0x03);
    rx_buf_u8(&b, lng ? 0x18 : 0x16);
    if (lng)
        rx_buf_zeros(&b, 2);
    rx_buf_put(&b, s.data, s.len);
    uint32_t alloc = 24 + align4((uint32_t)s.len) + 4;
    rx_buf_free(&s);
    return add_row(t, &b, alloc, true);
}

static int playlist_tree_row(Table *t, const RXPlanPlaylist *p)
{
    RXBuf b = { 0 };
    rx_buf_le32(&b, p->parent_id);
    rx_buf_le32(&b, 0);
    rx_buf_le32(&b, (uint32_t)p->sort_order);
    rx_buf_le32(&b, p->id);
    rx_buf_le32(&b, p->is_folder ? 1 : 0);
    int ret = put_dstr(&b, p->name);
    if (ret < 0) {
        rx_buf_free(&b);
        return ret;
    }
    return add_row(t, &b, 0, false);
}

static int playlist_entry_row(Table *t, uint32_t index, uint32_t track_id, uint32_t playlist_id)
{
    RXBuf b = { 0 };
    rx_buf_le32(&b, index);
    rx_buf_le32(&b, track_id);
    rx_buf_le32(&b, playlist_id);
    return add_row(t, &b, 12, false);
}

/* The 0x13 "history"/property row (docs/pdb.md 2.11): track count (the
 * CDJ-3000 shows it as "Songs", G2), the date, 19 1E, "1000", the device
 * label. Byte 9 is the "Background Color : Device Library" setting, 0
 * default, 1..8 pink..purple (rbxport rbl-pdb rows.rs, observed in
 * rekordbox 7.2.11); dj wrote it 0. */
static int history_row(Table *t, uint32_t num_tracks, const char *date, const char *label,
                       int background)
{
    RXBuf lab = { 0 };
    int ret = put_dstr(&lab, label);
    if (ret < 0) {
        rx_buf_free(&lab);
        return ret;
    }
    RXBuf b = { 0 };
    rx_buf_le16(&b, 0x0280);
    rx_buf_le16(&b, 0);
    rx_buf_le32(&b, num_tracks);
    rx_buf_u8(&b, 0);
    rx_buf_u8(&b, (unsigned)RX_CLAMP(background, 0, 8));
    rx_buf_le16(&b, 0);
    put_dstr(&b, date);
    rx_buf_u8(&b, 0x19);
    rx_buf_u8(&b, 0x1E);
    put_dstr(&b, "1000");
    rx_buf_put(&b, lab.data, lab.len);
    uint32_t alloc = 36 + align4((uint32_t)lab.len);
    rx_buf_free(&lab);
    return add_row(t, &b, alloc, true);
}

/* ---------------------------------------------------------- fixed rows */

static const char *const color_names[8] = {
    "Pink", "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple",
};

static const struct { uint16_t id, code; const char *name; } columns[27] = {
    { 1, 0x80, "GENRE" }, { 2, 0x81, "ARTIST" }, { 3, 0x82, "ALBUM" }, { 4, 0x83, "TRACK" },
    { 5, 0x85, "BPM" }, { 6, 0x86, "RATING" }, { 7, 0x87, "YEAR" }, { 8, 0x88, "REMIXER" },
    { 9, 0x89, "LABEL" }, { 10, 0x8A, "ORIGINAL ARTIST" }, { 11, 0x8B, "KEY" }, { 12, 0x8D, "CUE" },
    { 13, 0x8E, "COLOR" }, { 14, 0x92, "TIME" }, { 15, 0x93, "BITRATE" }, { 16, 0x94, "FILE NAME" },
    { 17, 0x84, "PLAYLIST" }, { 18, 0x98, "HOT CUE BANK" }, { 19, 0x95, "HISTORY" },
    { 20, 0x91, "SEARCH" }, { 21, 0x96, "COMMENTS" }, { 22, 0x8C, "DATE ADDED" },
    { 23, 0x97, "DJ PLAY COUNT" }, { 24, 0x90, "FOLDER" }, { 25, 0xA1, "DEFAULT" },
    { 26, 0xA2, "ALPHABET" }, { 27, 0xAA, "MATCHING" },
};

/* rekordbox 7.2.18's menu (0x11) and sort (0x12) rows, 8 bytes each
 * (docs/pdb.md; the last menu row is 16001b0063050a00 in 7.2.18). */
static const char menu_hex[] =
    "0100010063010000050006000501000006000700630100000700080063010000"
    "080009006301000009000a00630100000a000b00630100000d000f0063010000"
    "0e001300040100000f0014000601000010001500630100001200170063010000"
    "0200020002000100030003000300020004000400010003000b000c0063000400"
    "1100050063000500130016006300060014001200630007001b001a0063020800"
    "180011006300090016001b0063050a00";
static const char sort_hex[] =
    "01000600010000001500070001000000"
    "0e000800010000000800090001000000"
    "09000a00010000000a000b0001000000"
    "0f000d00010000000d000f0001000000"
    "17001000010000001600110001000000"
    "19000000000100001a00010000020000"
    "02000200000300000300030000040000"
    "05000400000500000600050000060000"
    "0b000c0000070000";

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

static int eight_byte_rows(Table *t, const char *hex)
{
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i += 8) {
        uint8_t row[8];
        for (int k = 0; k < 8; k++)
            row[k] = (uint8_t)(hexval(hex[2 * (i + k)]) << 4 | hexval(hex[2 * (i + k) + 1]));
        int ret = add_bytes_row(t, row, 8, 8);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int fixed_tables(Table *tables)
{
    int ret;
    for (int i = 0; i < 8; i++) {
        RXBuf b = { 0 };
        rx_buf_le32(&b, 0);
        rx_buf_u8(&b, (unsigned)(i + 1));
        rx_buf_le16(&b, (unsigned)(i + 1));
        rx_buf_u8(&b, 0);
        put_dstr(&b, color_names[i]);
        if ((ret = add_row(&tables[RX_PDB_COLORS], &b, 0, false)) < 0)
            return ret;
    }
    /* Column names are UTF-16 wrapped in U+FFFA ... U+FFFB (Appendix A). */
    for (int i = 0; i < 27; i++) {
        RXBuf b = { 0 };
        size_t n = strlen(columns[i].name);
        rx_buf_le16(&b, columns[i].id);
        rx_buf_le16(&b, columns[i].code);
        rx_buf_u8(&b, 0x90);
        rx_buf_le16(&b, (unsigned)(2 * (n + 2) + 4));
        rx_buf_u8(&b, 0);
        rx_buf_le16(&b, 0xFFFA);
        for (size_t k = 0; k < n; k++)
            rx_buf_le16(&b, (uint8_t)columns[i].name[k]);
        rx_buf_le16(&b, 0xFFFB);
        if ((ret = add_row(&tables[RX_PDB_COLUMNS], &b, 0, false)) < 0)
            return ret;
    }
    if ((ret = eight_byte_rows(&tables[RX_PDB_MENU], menu_hex)) < 0)
        return ret;
    return eight_byte_rows(&tables[RX_PDB_SORT], sort_hex);
}

/* ------------------------------------------------------------- assembly */

typedef struct Page {
    int      first, nb;           /* rows of the table */
    uint32_t used;
} Page;

static int paginate(const Table *t, Page **pages, int *nb_pages)
{
    *pages = NULL;
    *nb_pages = 0;
    if (!t->nb)
        return 0;
    int cap = 4, n = 0;
    Page *p = malloc((size_t)cap * sizeof(*p));
    if (!p)
        return RX_ERROR(ENOMEM);
    p[0] = (Page) { 0 };
    n = 1;
    for (int r = 0; r < t->nb; r++) {
        uint32_t alloc = t->rows[r].alloc;
        if (alloc + dir_bytes(1) > RX_PDB_PAGE - RX_PDB_HEAP) {
            free(p);
            return RX_ERROR(EOVERFLOW);
        }
        Page *cur = &p[n - 1];
        uint32_t rows = (uint32_t)cur->nb + 1;
        bool fits = cur->used + alloc + dir_bytes(rows) <= RX_PDB_PAGE - RX_PDB_HEAP &&
                    rows <= MAX_ROWS_PER_PAGE;
        if (!fits) {
            if (n == cap) {
                cap *= 2;
                Page *np = realloc(p, (size_t)cap * sizeof(*p));
                if (!np) {
                    free(p);
                    return RX_ERROR(ENOMEM);
                }
                p = np;
            }
            p[n++] = (Page) { .first = r };
            cur = &p[n - 1];
        }
        cur->nb++;
        cur->used += alloc;
    }
    *pages = p;
    *nb_pages = n;
    return 0;
}

/* Sequences of the tables rekordbox writes in one transaction (docs/pdb.md
 * 2.8): colours 2, columns 3, menu 4, sort 5. */
static uint32_t fixed_sequence(int type)
{
    switch (type) {
    case RX_PDB_COLORS:  return 2;
    case RX_PDB_COLUMNS: return 3;
    case RX_PDB_MENU:    return 4;
    case RX_PDB_SORT:    return 5;
    default:             return 0;
    }
}

static void put_counts(uint8_t *p, uint32_t n, uint32_t valid)
{
    p[0] = (uint8_t)(n & 0xFF);
    p[1] = (uint8_t)(((n >> 8) & 0x1F) | ((valid & 0x7) << 5));
    p[2] = (uint8_t)((valid >> 3) & 0xFF);
}

static void index_page(uint8_t *b, uint32_t index, uint32_t type, uint32_t next, uint32_t first_data)
{
    rx_wr_le32(b + 0x04, index);
    rx_wr_le32(b + 0x08, type);
    rx_wr_le32(b + 0x0C, next);
    rx_wr_le32(b + 0x10, 1);
    b[0x1B] = 0x64;
    rx_wr_le16(b + 0x20, 0x1FFF);
    rx_wr_le16(b + 0x22, 0x1FFF);
    rx_wr_le16(b + 0x24, 0x03EC);
    rx_wr_le32(b + 0x28, index);
    rx_wr_le32(b + 0x2C, first_data);
    rx_wr_le32(b + 0x30, NO_PAGE);
    rx_wr_le16(b + 0x3A, 0x1FFF);
    for (int i = 0; i < 1004; i++)
        rx_wr_le32(b + 0x3C + 4 * i, 0x1FFFFFF8);
}

static void data_page(uint8_t *b, uint32_t index, uint32_t type, uint32_t next, uint32_t seq,
                      const Table *t, const Page *pg, bool single_tx)
{
    uint32_t n = (uint32_t)pg->nb;
    rx_wr_le32(b + 0x04, index);
    rx_wr_le32(b + 0x08, type);
    rx_wr_le32(b + 0x0C, next);
    rx_wr_le32(b + 0x10, seq);
    put_counts(b + 0x18, n, n);
    b[0x1B] = 0x24;
    rx_wr_le16(b + 0x1C, RX_PDB_PAGE - RX_PDB_HEAP - pg->used - dir_bytes(n));
    rx_wr_le16(b + 0x1E, pg->used);
    /* Transaction fields (docs/pdb.md 2.5): (n, 0) for tables written in
     * one transaction, (1, n - 1) for rows added one at a time. */
    rx_wr_le16(b + 0x20, single_tx ? n : 1);
    rx_wr_le16(b + 0x22, single_tx ? 0 : n - 1);

    uint32_t pos = 0;
    for (uint32_t s = 0; s < n; s++) {
        const Row *r = &t->rows[pg->first + (int)s];
        memcpy(b + RX_PDB_HEAP + pos, r->data, r->len);
        if (r->shift)
            rx_wr_le16(b + RX_PDB_HEAP + pos + 2, 0x20 * s);
        uint32_t base = RX_PDB_PAGE - 0x24 * (s / 16);
        rx_wr_le16(b + base - 6 - 2 * (s % 16), pos);
        pos += r->alloc;
    }
    for (uint32_t g = 0; g < (n + 15) / 16; g++) {
        uint32_t base = RX_PDB_PAGE - 0x24 * g;
        uint32_t used = RX_MIN(16u, n - 16 * g);
        uint32_t present = (1u << used) - 1;
        uint32_t txf;
        if (single_tx)
            txf = present;
        else
            txf = g == (n - 1) / 16 ? 1u << (used - 1) : 0;
        rx_wr_le16(b + base - 4, present);
        rx_wr_le16(b + base - 2, txf);
    }
}

static int assemble(const Table *tables, int nb_tables, uint32_t single_mask,
                    uint32_t header_unknown, uint8_t **out, size_t *size)
{
    int ret = 0;
    Page *pages[RX_PDB_NUM_TABLES] = { 0 };
    int nb_pages[RX_PDB_NUM_TABLES] = { 0 };
    uint32_t first_index[RX_PDB_NUM_TABLES], candidate[RX_PDB_NUM_TABLES];
    uint8_t *b = NULL;

    for (int t = 0; t < nb_tables; t++)
        if ((ret = paginate(&tables[t], &pages[t], &nb_pages[t])) < 0)
            goto fail;

    uint32_t nxt = 1;
    for (int t = 0; t < nb_tables; t++) {
        first_index[t] = nxt;
        nxt += 1 + (uint32_t)nb_pages[t];
    }
    for (int t = 0; t < nb_tables; t++)
        candidate[t] = nxt + (uint32_t)t;
    uint32_t total = nxt + (uint32_t)nb_tables;

    b = calloc(total, RX_PDB_PAGE);
    if (!b) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }

    uint32_t seq = 6, max_seq = 5;
    for (int t = 0; t < nb_tables; t++) {
        uint32_t fi = first_index[t];
        bool has = nb_pages[t] > 0;
        index_page(b + (size_t)fi * RX_PDB_PAGE, fi, (uint32_t)t, has ? fi + 1 : candidate[t],
                   has ? fi + 1 : NO_PAGE);
        bool single = single_mask >> t & 1;
        for (int i = 0; i < nb_pages[t]; i++) {
            uint32_t pi = fi + 1 + (uint32_t)i;
            uint32_t s;
            if (single && fixed_sequence(t))
                s = fixed_sequence(t);
            else
                s = seq++;
            max_seq = RX_MAX(max_seq, s);
            uint32_t next = i + 1 < nb_pages[t] ? pi + 1 : candidate[t];
            data_page(b + (size_t)pi * RX_PDB_PAGE, pi, (uint32_t)t, next, s, &tables[t],
                      &pages[t][i], single);
        }
    }

    /* Header (docs/pdb.md 2.2); the candidate pages stay zero. */
    rx_wr_le32(b + 0x04, RX_PDB_PAGE);
    rx_wr_le32(b + 0x08, (uint32_t)nb_tables);
    rx_wr_le32(b + 0x0C, total);
    rx_wr_le32(b + 0x10, header_unknown);
    rx_wr_le32(b + 0x14, max_seq + 1);
    for (int t = 0; t < nb_tables; t++) {
        uint8_t *e = b + 0x1C + 16 * t;
        rx_wr_le32(e, (uint32_t)t);
        rx_wr_le32(e + 4, candidate[t]);
        rx_wr_le32(e + 8, first_index[t]);
        rx_wr_le32(e + 12, nb_pages[t] ? first_index[t] + (uint32_t)nb_pages[t] : first_index[t]);
    }
    *out = b;
    *size = (size_t)total * RX_PDB_PAGE;
    b = NULL;

fail:
    for (int t = 0; t < nb_tables; t++)
        free(pages[t]);
    free(b);
    return ret;
}

/* ------------------------------------------------------------ the files */

int rx_pdb_build(const RXPlan *plan, uint8_t **out, size_t *size)
{
    Table tables[RX_PDB_NUM_TABLES] = { 0 };
    int ret = 0;

    for (int i = 0; i < plan->nb_tracks; i++) {
        RXBuf row = { 0 };
        uint32_t alloc;
        if ((ret = track_row(plan, &plan->tracks[i], &row, &alloc)) < 0) {
            rx_buf_free(&row);
            goto fail;
        }
        if ((ret = add_row(&tables[RX_PDB_TRACKS], &row, alloc, true)) < 0)
            goto fail;
    }
    for (int i = 0; i < plan->nb_genres; i++)
        if ((ret = id_name_row(&tables[RX_PDB_GENRES], plan->genres[i].id, plan->genres[i].name)) < 0)
            goto fail;
    for (int i = 0; i < plan->nb_artists; i++)
        if ((ret = artist_row(&tables[RX_PDB_ARTISTS], plan->artists[i].id, plan->artists[i].name)) < 0)
            goto fail;
    for (int i = 0; i < plan->nb_albums; i++)
        if ((ret = album_row(&tables[RX_PDB_ALBUMS], plan->albums[i].id, plan->albums[i].name,
                             plan->albums[i].artist_id)) < 0)
            goto fail;
    for (int i = 0; i < plan->nb_labels; i++)
        if ((ret = id_name_row(&tables[RX_PDB_LABELS], plan->labels[i].id, plan->labels[i].name)) < 0)
            goto fail;
    for (int i = 0; i < plan->nb_keys; i++)
        if ((ret = key_row(&tables[RX_PDB_KEYS], plan->keys[i].id, plan->keys[i].name)) < 0)
            goto fail;
    for (int i = 0; i < plan->nb_playlists; i++)
        if ((ret = playlist_tree_row(&tables[RX_PDB_PLAYLIST_TREE], &plan->playlists[i])) < 0)
            goto fail;
    for (int i = 0; i < plan->nb_playlists; i++) {
        const RXPlanPlaylist *p = &plan->playlists[i];
        for (int k = 0; k < p->nb; k++)
            if ((ret = playlist_entry_row(&tables[RX_PDB_PLAYLIST_ENTRIES], (uint32_t)k + 1,
                                          p->track_ids[k], p->id)) < 0)
                goto fail;
    }
    for (int i = 0; i < plan->nb_artworks; i++) {
        const RXPlanArtwork *a = &plan->artworks[i];
        char *path = a->path80 ? rx_strdup(a->path80) : NULL;
        if (!path) {
            char *dir = rx_plan_artwork_dir(a->id);
            path = dir ? rx_asprintf("%s/a%u.jpg", dir, a->id) : NULL;
            free(dir);
        }
        if (!path) {
            ret = RX_ERROR(ENOMEM);
            goto fail;
        }
        ret = id_name_row(&tables[RX_PDB_ARTWORK], a->id, path);
        free(path);
        if (ret < 0)
            goto fail;
    }
    if ((ret = fixed_tables(tables)) < 0)
        goto fail;
    char label[64];
    rx_plan_ascii_label(plan->device_name, label, sizeof(label));
    if ((ret = history_row(&tables[RX_PDB_HISTORY], (uint32_t)plan->nb_tracks, plan->date, label,
                           plan->background_color)) < 0)
        goto fail;

    uint32_t single = 1u << RX_PDB_COLORS | 1u << RX_PDB_COLUMNS | 1u << RX_PDB_MENU | 1u << RX_PDB_SORT;
    ret = assemble(tables, RX_PDB_NUM_TABLES, single, 1, out, size);

fail:
    tables_free(tables, RX_PDB_NUM_TABLES);
    return ret;
}

/* exportExt.pdb tag rows (docs/pdb.md 3): categories (My Tag columns) and
 * the tags inside them. The name follows the u8 offsets, then an empty
 * second string. */
static int tag_row(Table *t, uint32_t id, const char *name, uint32_t category, uint32_t position,
                   bool is_category)
{
    RXBuf s = { 0 };
    int ret = put_dstr(&s, name);
    if (ret < 0) {
        rx_buf_free(&s);
        return ret;
    }
    bool lng = is_long(s.data);
    unsigned name_at = lng ? 0x20 : 0x1F;
    RXBuf b = { 0 };
    rx_buf_le16(&b, 0x0680);
    rx_buf_le16(&b, 0);
    rx_buf_le32(&b, 0);
    rx_buf_le32(&b, 0);
    rx_buf_le32(&b, category);
    rx_buf_le32(&b, position);
    rx_buf_le32(&b, id);
    rx_buf_le32(&b, is_category ? 0x01000000 : 0);
    rx_buf_u8(&b, 0x03);
    rx_buf_u8(&b, name_at);
    rx_buf_u8(&b, name_at + (unsigned)s.len);
    rx_buf_zeros(&b, name_at - 0x1F);
    rx_buf_put(&b, s.data, s.len);
    put_dstr(&b, "");
    uint32_t alloc = 32 + align4((uint32_t)s.len) + 4 + 4;
    rx_buf_free(&s);
    return add_row(t, &b, alloc, true);
}

int rx_pdb_ext_build(const RXPlan *plan, uint8_t **out, size_t *size)
{
    Table tables[RX_PDB_EXT_NUM_TABLES] = { 0 };
    int ret = 0;

    /* Each category row followed by its tags, in plan order (dj's
     * build_pdb_ext). */
    for (int c = 0; c < plan->nb_tags; c++) {
        const RXPlanTag *cat = &plan->tags[c];
        if (!cat->is_category)
            continue;
        if ((ret = tag_row(&tables[RX_PDB_EXT_TAGS], cat->id, cat->name, 0,
                           (uint32_t)cat->position, true)) < 0)
            goto fail;
        for (int i = 0; i < plan->nb_tags; i++) {
            const RXPlanTag *tg = &plan->tags[i];
            if (tg->is_category || (uint32_t)tg->category != cat->id)
                continue;
            if ((ret = tag_row(&tables[RX_PDB_EXT_TAGS], tg->id, tg->name, (uint32_t)tg->category,
                               (uint32_t)tg->position, false)) < 0)
                goto fail;
        }
    }
    for (int i = 0; i < plan->nb_tag_tracks; i++) {
        RXBuf b = { 0 };
        rx_buf_le32(&b, 0);
        rx_buf_le32(&b, plan->tag_tracks[i].track_id);
        rx_buf_le32(&b, plan->tag_tracks[i].tag_id);
        rx_buf_le32(&b, 3);
        if ((ret = add_row(&tables[RX_PDB_EXT_TAG_TRACKS], &b, 16, false)) < 0)
            goto fail;
    }
    /* Table 7's one row (docs/pdb.md 3): the u32 at 0x18 was the track-row
     * library constant + 1 in rekordbox's files. */
    RXBuf b = { 0 };
    static const uint8_t t7_head[4] = { 0x00, 0x07, 0x00, 0x00 };
    static const uint8_t t7_tail[11] = { 0x03, 0x22, 0x23, 0x24, 0x25, 0x26, 0x03, 0x03, 0x03, 0x03, 0x03 };
    rx_buf_put(&b, t7_head, 4);
    rx_buf_zeros(&b, 20);
    rx_buf_le32(&b, rx_plan_library_id(plan) + 1);
    rx_buf_put(&b, t7_tail, sizeof(t7_tail));
    if ((ret = add_row(&tables[RX_PDB_EXT_T7], &b, 60, false)) < 0)
        goto fail;

    ret = assemble(tables, RX_PDB_EXT_NUM_TABLES, 1u << RX_PDB_EXT_TAGS, 5, out, size);

fail:
    tables_free(tables, RX_PDB_EXT_NUM_TABLES);
    return ret;
}

/* ------------------------------------------------------------- reading */

int rx_pdb_read_header(const uint8_t *b, size_t size, RXPdbHeader *h)
{
    *h = (RXPdbHeader) { 0 };
    if (size < RX_PDB_PAGE)
        return RX_ERROR(EINVAL);
    h->len_page = rx_rd_le32(b + 0x04);
    h->num_tables = rx_rd_le32(b + 0x08);
    h->next_unused = rx_rd_le32(b + 0x0C);
    h->unknown = rx_rd_le32(b + 0x10);
    h->sequence = rx_rd_le32(b + 0x14);
    if (h->len_page != RX_PDB_PAGE || h->num_tables > 64 || 0x1C + 16 * h->num_tables > RX_PDB_PAGE)
        return RX_ERROR(EINVAL);
    for (uint32_t i = 0; i < h->num_tables; i++) {
        const uint8_t *e = b + 0x1C + 16 * i;
        h->tables[i] = (RXPdbTableDir) {
            .type            = rx_rd_le32(e),
            .empty_candidate = rx_rd_le32(e + 4),
            .first_page      = rx_rd_le32(e + 8),
            .last_page       = rx_rd_le32(e + 12),
        };
    }
    return 0;
}

/* The slots a page's row index can hold: groups of 16 down to the heap. */
#define MAX_SLOTS (((RX_PDB_PAGE - RX_PDB_HEAP - 0x24) / 0x24 + 1) * 16)

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* The rows of the data page at file offset `at`, in slot order, each once:
 * slots listing the same offset are one row (a slot reused, or a crafted
 * index naming one row hundreds of times). A row's room runs to the next
 * row of the page in the heap, the last one's to the row index. A row is
 * written whole into its allocation, its strings with it (docs/pdb.md 2.6,
 * rekordbox and this writer alike: the rekordbox fixture and the 25
 * exports under dj's .ref read the same bounded so as unbounded), so the
 * readers keep what a row holds inside its room. Rows a byte apart then
 * hold nothing, and no string is read by two rows. */
static int page_rows(const uint8_t *p, size_t at, RXArray *rows)
{
    uint16_t off[MAX_SLOTS], sorted[MAX_SLOTS];
    bool done[MAX_SLOTS] = { 0 };
    int k = 0;
    uint32_t v = p[0x18] | p[0x19] << 8 | p[0x1A] << 16;
    uint32_t n = v & 0x1FFF, slots = 0;
    for (uint32_t s = 0; s < n; s++) {
        uint32_t base = RX_PDB_PAGE - 0x24 * (s / 16);
        if (base < RX_PDB_HEAP + 0x24)
            break;
        slots = s + 1;
        uint16_t present = rx_rd_le16(p + base - 4);
        if (!(present >> (s % 16) & 1))
            continue;
        uint32_t o = rx_rd_le16(p + base - 6 - 2 * (s % 16));
        if (RX_PDB_HEAP + o < RX_PDB_PAGE)
            off[k++] = (uint16_t)o;
    }
    memcpy(sorted, off, (size_t)k * sizeof(*off));
    qsort(sorted, (size_t)k, sizeof(*sorted), cmp_u16);
    int nu = 0;
    for (int i = 0; i < k; i++) {
        if (!nu || sorted[i] != sorted[nu - 1])
            sorted[nu++] = sorted[i];
    }
    uint32_t heap_end = RX_PDB_PAGE - dir_bytes(slots);
    for (int i = 0; i < k; i++) {
        const uint16_t *f = bsearch(&off[i], sorted, (size_t)nu, sizeof(*sorted), cmp_u16);
        int u = (int)(f - sorted);
        if (done[u])
            continue;
        done[u] = true;
        uint32_t start = RX_PDB_HEAP + off[i];
        uint32_t end = u + 1 < nu ? RX_PDB_HEAP + sorted[u + 1] : heap_end;
        if (start >= end)
            continue;
        RXPdbRow r = { .pos = (uint32_t)(at + start), .end = (uint32_t)(at + end) };
        if (!rx_array_push(rows, &r))
            return RX_ERROR(ENOMEM);
    }
    return 0;
}

int rx_pdb_table_rows(const uint8_t *b, size_t size, const RXPdbTableDir *t, RXPdbRow **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    /* Row offsets are u32: a file past 4 GB is no export.pdb, and is read
     * up to there. */
    size_t nb_pages = RX_MIN(size, (size_t)UINT32_MAX) / RX_PDB_PAGE;
    uint8_t *seen = calloc(nb_pages + 1, 1);
    if (!seen)
        return RX_ERROR(ENOMEM);
    RXArray rows;
    rx_array_init(&rows, sizeof(RXPdbRow));
    int ret = 0;
    uint32_t page = t->first_page;
    while (page < nb_pages && !seen[page]) {
        seen[page] = 1;
        const uint8_t *p = b + (size_t)page * RX_PDB_PAGE;
        if (!(p[0x1B] & 0x40) && (ret = page_rows(p, (size_t)page * RX_PDB_PAGE, &rows)) < 0)
            break;
        if (page == t->last_page)
            break;
        page = rx_rd_le32(p + 0x0C);
    }
    if (ret >= 0) {
        *out = rows.data;
        *nb = (int)rows.nb;
        rows.data = NULL;
    }
    rx_array_free(&rows);
    free(seen);
    return ret;
}

static const RXPdbTableDir *table_of(const RXPdbHeader *h, uint32_t type)
{
    for (uint32_t i = 0; i < h->num_tables; i++)
        if (h->tables[i].type == type)
            return &h->tables[i];
    return NULL;
}

void rx_pdb_raw_tracks_free(RXPdbRawTrack *t, int nb)
{
    if (!t)
        return;
    for (int i = 0; i < nb; i++)
        for (int k = 0; k < 21; k++)
            free(t[i].strings[k]);
    free(t);
}

int rx_pdb_raw_tracks(const uint8_t *b, size_t size, RXPdbRawTrack **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    RXPdbHeader h;
    int ret = rx_pdb_read_header(b, size, &h);
    if (ret < 0)
        return ret;
    const RXPdbTableDir *td = table_of(&h, RX_PDB_TRACKS);
    if (!td)
        return 0;
    RXPdbRow *rows;
    int n;
    if ((ret = rx_pdb_table_rows(b, size, td, &rows, &n)) < 0)
        return ret;
    RXPdbRawTrack *t = calloc((size_t)RX_MAX(n, 1), sizeof(*t));
    if (!t) {
        free(rows);
        return RX_ERROR(ENOMEM);
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        size_t p = rows[i].pos, end = rows[i].end;
        if (p + 0x88 > end)
            continue;
        const uint8_t *r = b + p;
        RXPdbRawTrack *x = &t[k++];
        *x = (RXPdbRawTrack) {
            .pos          = (uint32_t)p,
            .subtype      = rx_rd_le16(r),
            .bitmask      = rx_rd_le32(r + 0x04),
            .sample_rate  = rx_rd_le32(r + 0x08),
            .artwork_id   = rx_rd_le32(r + 0x1C),
            .key_id       = rx_rd_le32(r + 0x20),
            .bitrate      = rx_rd_le32(r + 0x30),
            .tempo        = rx_rd_le32(r + 0x38),
            .artist_id    = rx_rd_le32(r + 0x44),
            .id           = rx_rd_le32(r + 0x48),
            .sample_depth = rx_rd_le16(r + 0x52),
            .duration     = rx_rd_le16(r + 0x54),
            .unk56        = rx_rd_le16(r + 0x56),
            .color_id     = r[0x58],
            .rating       = r[0x59],
            .file_type    = rx_rd_le16(r + 0x5A),
            .magic        = rx_rd_le16(r + 0x5C),
        };
        for (int s = 0; s < 21; s++) {
            x->string_offsets[s] = rx_rd_le16(r + 0x5E + 2 * s);
            if (read_dstr(b, end, p + x->string_offsets[s], s == 0, &x->strings[s]) == RX_ERROR(ENOMEM) ||
                (!x->strings[s] && !(x->strings[s] = rx_strdup("")))) {
                free(rows);
                rx_pdb_raw_tracks_free(t, k);
                return RX_ERROR(ENOMEM);
            }
        }
    }
    free(rows);
    *out = t;
    *nb = k;
    return 0;
}

typedef struct IdName {
    uint32_t id, pos;
    char    *name;
} IdName;

/* By id, and a repeated id by where its row is. */
static int cmp_idname(const void *a, const void *b)
{
    const IdName *x = a, *y = b;
    if (x->id != y->id)
        return x->id < y->id ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

static void names_free(IdName *v, int nb)
{
    for (int i = 0; v && i < nb; i++)
        free(v[i].name);
    free(v);
}

/* The id and name of every row of a table, by id. An id is a row's once:
 * a second row with it (a damaged or crafted file) is dropped, the first
 * in the file kept. */
static int read_names(const uint8_t *b, size_t size, const RXPdbHeader *h, uint32_t type,
                      IdName **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    const RXPdbTableDir *td = table_of(h, type);
    if (!td)
        return 0;
    RXPdbRow *rows;
    int n;
    int ret = rx_pdb_table_rows(b, size, td, &rows, &n);
    if (ret < 0)
        return ret;
    IdName *v = calloc((size_t)RX_MAX(n, 1), sizeof(*v));
    if (!v) {
        free(rows);
        return RX_ERROR(ENOMEM);
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        size_t p = rows[i].pos, end = rows[i].end;
        uint32_t id;
        size_t at;
        if (type == RX_PDB_ARTISTS) {
            if (p + 10 > end)
                continue;
            id = rx_rd_le32(b + p + 4);
            at = p + b[p + 9];
        } else if (type == RX_PDB_ALBUMS) {
            if (p + 0x16 > end)
                continue;
            id = rx_rd_le32(b + p + 0x0C);
            at = p + b[p + 0x15];
        } else if (type == RX_PDB_KEYS) {
            if (p + 8 > end)
                continue;
            id = rx_rd_le32(b + p);
            at = p + 8;
        } else {
            if (p + 4 > end)
                continue;
            id = rx_rd_le32(b + p);
            at = p + 4;
        }
        char *name;
        if ((ret = read_dstr(b, end, at, false, &name)) < 0) {
            if (ret == RX_ERROR(ENOMEM))
                break;
            ret = 0;
            continue;
        }
        v[k++] = (IdName) { .id = id, .pos = (uint32_t)p, .name = name };
    }
    free(rows);
    qsort(v, (size_t)k, sizeof(*v), cmp_idname);
    int u = 0;
    for (int i = 0; i < k; i++) {
        if (u && v[u - 1].id == v[i].id)
            free(v[i].name);
        else
            v[u++] = v[i];
    }
    if (ret < 0) {
        names_free(v, u);
        return ret;
    }
    *out = v;
    *nb = u;
    return 0;
}

static const char *name_of(const IdName *v, int nb, uint32_t id)
{
    /* A file without the table: no array to search. */
    if (!nb)
        return NULL;
    for (int lo = 0, hi = nb; lo < hi;) {
        int mid = lo + (hi - lo) / 2;
        if (v[mid].id == id)
            return v[mid].name;
        if (v[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

int rx_pdb_artwork(const uint8_t *b, size_t size, uint32_t **ids, char ***paths, int *nb)
{
    *ids = NULL;
    *paths = NULL;
    *nb = 0;
    RXPdbHeader h;
    int ret = rx_pdb_read_header(b, size, &h);
    if (ret < 0)
        return ret;
    IdName *v;
    int n;
    if ((ret = read_names(b, size, &h, RX_PDB_ARTWORK, &v, &n)) < 0)
        return ret;
    *ids = malloc((size_t)RX_MAX(n, 1) * sizeof(**ids));
    *paths = malloc((size_t)RX_MAX(n, 1) * sizeof(**paths));
    if (!*ids || !*paths) {
        free(*ids);
        free(*paths);
        *ids = NULL;
        *paths = NULL;
        names_free(v, n);
        return RX_ERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++) {
        (*ids)[i] = v[i].id;
        (*paths)[i] = v[i].name;
    }
    free(v);
    *nb = n;
    return 0;
}

int rx_pdb_history(const uint8_t *b, size_t size, uint32_t *num_tracks, char **date)
{
    *num_tracks = 0;
    *date = NULL;
    RXPdbHeader h;
    int ret = rx_pdb_read_header(b, size, &h);
    if (ret < 0)
        return ret;
    const RXPdbTableDir *td = table_of(&h, RX_PDB_HISTORY);
    if (!td)
        return RX_ERROR(ENOENT);
    RXPdbRow *rows;
    int n;
    if ((ret = rx_pdb_table_rows(b, size, td, &rows, &n)) < 0)
        return ret;
    ret = RX_ERROR(ENOENT);
    if (n > 0 && rows[0].pos + 0x0D <= rows[0].end) {
        *num_tracks = rx_rd_le32(b + rows[0].pos + 4);
        ret = read_dstr(b, rows[0].end, rows[0].pos + 0x0C, false, date);
    }
    free(rows);
    return ret;
}

void rx_pdb_history_lists_free(RXPdbHistoryList *l, int nb)
{
    for (int i = 0; l && i < nb; i++) {
        free(l[i].name);
        free(l[i].track_ids);
    }
    free(l);
}

typedef struct Entry {
    uint32_t index, track, playlist;
} Entry;

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->playlist != y->playlist)
        return x->playlist < y->playlist ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

/* The tracks of `playlist`, in index order: its run of the entries sorted
 * by cmp_entry, found by binary search. Every playlist is one search and
 * one run, so the entries are read once whatever the tree says. */
static int playlist_tracks(const Entry *ent, int ne, uint32_t playlist, uint32_t **ids, int *nb)
{
    *ids = NULL;
    *nb = 0;
    int lo = 0, hi = ne;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ent[mid].playlist < playlist)
            lo = mid + 1;
        else
            hi = mid;
    }
    int end = lo;
    while (end < ne && ent[end].playlist == playlist)
        end++;
    if (end == lo)
        return 0;
    if (!(*ids = malloc((size_t)(end - lo) * sizeof(**ids))))
        return RX_ERROR(ENOMEM);
    for (int k = lo; k < end; k++)
        (*ids)[(*nb)++] = ent[k].track;
    return 0;
}

int rx_pdb_history_lists(const uint8_t *b, size_t size, RXPdbHistoryList **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    RXPdbHeader h;
    int ret = rx_pdb_read_header(b, size, &h);
    if (ret < 0)
        return ret;
    IdName *lists = NULL;
    int nb_lists = 0;
    RXPdbRow *rows = NULL;
    int nb_rows = 0;
    Entry *ent = NULL;
    RXPdbHistoryList *l = NULL;

    if ((ret = read_names(b, size, &h, RX_PDB_HISTORY_PLAYLISTS, &lists, &nb_lists)) < 0)
        goto fail;
    const RXPdbTableDir *te = table_of(&h, RX_PDB_HISTORY_ENTRIES);
    if (te && (ret = rx_pdb_table_rows(b, size, te, &rows, &nb_rows)) < 0)
        goto fail;
    ent = malloc((size_t)RX_MAX(nb_rows, 1) * sizeof(*ent));
    l = calloc((size_t)RX_MAX(nb_lists, 1), sizeof(*l));
    if (!ent || !l) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    /* History entries put the track first and the index last, unlike
     * playlist entries. */
    int ne = 0;
    for (int i = 0; i < nb_rows; i++) {
        if (rows[i].pos + 12 > rows[i].end)
            continue;
        const uint8_t *p = b + rows[i].pos;
        ent[ne++] = (Entry) {
            .track    = rx_rd_le32(p),
            .playlist = rx_rd_le32(p + 4),
            .index    = rx_rd_le32(p + 8),
        };
    }
    qsort(ent, (size_t)ne, sizeof(*ent), cmp_entry);
    /* The lists' ids are unique (read_names), so no entry is handed out
     * twice. */
    for (int i = 0; i < nb_lists; i++) {
        l[i].id = lists[i].id;
        l[i].name = lists[i].name;
        lists[i].name = NULL;
        if ((ret = playlist_tracks(ent, ne, l[i].id, &l[i].track_ids, &l[i].nb)) < 0)
            goto fail;
    }
    *out = l;
    *nb = nb_lists;
    l = NULL;

fail:
    rx_pdb_history_lists_free(l, nb_lists);
    names_free(lists, nb_lists);
    free(rows);
    free(ent);
    return ret;
}

void rx_pdb_free(RXPdb *p)
{
    for (int i = 0; i < p->nb_tracks; i++) {
        free(p->tracks[i].path);
        free(p->tracks[i].anlz_path);
        free(p->tracks[i].title);
        free(p->tracks[i].artist);
    }
    free(p->tracks);
    for (int i = 0; i < p->nb_playlists; i++) {
        free(p->playlists[i].name);
        free(p->playlists[i].track_ids);
    }
    free(p->playlists);
    *p = (RXPdb) { 0 };
}

/* A playlist id is one playlist's: a second tree row with it (a damaged
 * or crafted file) is dropped, the first in the file kept, so that no
 * playlist's entries are handed out twice. */
static int unique_playlists(RXPdb *out)
{
    int n = out->nb_playlists;
    if (n < 2)
        return 0;
    IdName *v = malloc((size_t)n * sizeof(*v));
    bool *drop = calloc((size_t)n, sizeof(*drop));
    if (!v || !drop) {
        free(v);
        free(drop);
        return RX_ERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++)
        v[i] = (IdName) { .id = out->playlists[i].id, .pos = (uint32_t)i };
    qsort(v, (size_t)n, sizeof(*v), cmp_idname);
    for (int i = 1; i < n; i++)
        drop[v[i].pos] = v[i].id == v[i - 1].id;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (drop[i])
            free(out->playlists[i].name);
        else
            out->playlists[k++] = out->playlists[i];
    }
    out->nb_playlists = k;
    free(v);
    free(drop);
    return 0;
}

int rx_pdb_read(const uint8_t *data, size_t size, RXPdb *out)
{
    *out = (RXPdb) { 0 };
    RXPdbHeader h;
    int ret = rx_pdb_read_header(data, size, &h);
    if (ret < 0)
        return ret;
    out->nb_tables = (int)h.num_tables;
    out->next_unused_page = h.next_unused;
    out->sequence = h.sequence;

    RXPdbRawTrack *raw = NULL;
    int nb_raw = 0;
    IdName *artists = NULL;
    int nb_artists = 0;
    RXPdbRow *tree = NULL, *entries = NULL;
    int nb_tree = 0, nb_entries = 0;
    Entry *ent = NULL;

    if ((ret = rx_pdb_raw_tracks(data, size, &raw, &nb_raw)) < 0 ||
        (ret = read_names(data, size, &h, RX_PDB_ARTISTS, &artists, &nb_artists)) < 0)
        goto fail;

    out->tracks = calloc((size_t)RX_MAX(nb_raw, 1), sizeof(*out->tracks));
    if (!out->tracks) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    for (int i = 0; i < nb_raw; i++) {
        RXPdbRawTrack *r = &raw[i];
        const char *artist = name_of(artists, nb_artists, r->artist_id);
        out->tracks[i] = (RXPdbTrack) {
            .id         = r->id,
            .path       = r->strings[RX_PDB_STR_FILE_PATH],
            .anlz_path  = r->strings[RX_PDB_STR_ANALYZE_PATH],
            .title      = r->strings[RX_PDB_STR_TITLE],
            .artist     = rx_strdup(artist ? artist : ""),
            .bpm_x100   = (int)r->tempo,
            .duration   = r->duration,
            .artwork_id = r->artwork_id,
            .key_id     = r->key_id,
            .color_id   = r->color_id,
        };
        r->strings[RX_PDB_STR_FILE_PATH] = NULL;
        r->strings[RX_PDB_STR_ANALYZE_PATH] = NULL;
        r->strings[RX_PDB_STR_TITLE] = NULL;
        out->nb_tracks++;
        if (!out->tracks[i].artist) {
            ret = RX_ERROR(ENOMEM);
            goto fail;
        }
    }

    const RXPdbTableDir *tt = table_of(&h, RX_PDB_PLAYLIST_TREE);
    const RXPdbTableDir *te = table_of(&h, RX_PDB_PLAYLIST_ENTRIES);
    if (tt && (ret = rx_pdb_table_rows(data, size, tt, &tree, &nb_tree)) < 0)
        goto fail;
    if (te && (ret = rx_pdb_table_rows(data, size, te, &entries, &nb_entries)) < 0)
        goto fail;
    ent = malloc((size_t)RX_MAX(nb_entries, 1) * sizeof(*ent));
    out->playlists = calloc((size_t)RX_MAX(nb_tree, 1), sizeof(*out->playlists));
    if (!ent || !out->playlists) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    int ne = 0;
    for (int i = 0; i < nb_entries; i++) {
        if (entries[i].pos + 12 > entries[i].end)
            continue;
        const uint8_t *p = data + entries[i].pos;
        ent[ne++] = (Entry) { rx_rd_le32(p), rx_rd_le32(p + 4), rx_rd_le32(p + 8) };
    }
    qsort(ent, (size_t)ne, sizeof(*ent), cmp_entry);
    for (int i = 0; i < nb_tree; i++) {
        if (tree[i].pos + 21 > tree[i].end)
            continue;
        const uint8_t *p = data + tree[i].pos;
        char *name;
        if ((ret = read_dstr(data, tree[i].end, tree[i].pos + 20, false, &name)) < 0) {
            if (ret == RX_ERROR(ENOMEM))
                goto fail;
            continue;
        }
        out->playlists[out->nb_playlists++] = (RXPdbPlaylist) {
            .parent_id = rx_rd_le32(p),
            .id        = rx_rd_le32(p + 12),
            .is_folder = rx_rd_le32(p + 16) != 0,
            .name      = name,
        };
    }
    if ((ret = unique_playlists(out)) < 0)
        goto fail;
    for (int i = 0; i < out->nb_playlists; i++) {
        RXPdbPlaylist *pl = &out->playlists[i];
        if ((ret = playlist_tracks(ent, ne, pl->id, &pl->track_ids, &pl->nb)) < 0)
            goto fail;
    }
    ret = 0;

fail:
    rx_pdb_raw_tracks_free(raw, nb_raw);
    names_free(artists, nb_artists);
    free(tree);
    free(entries);
    free(ent);
    if (ret < 0)
        rx_pdb_free(out);
    return ret;
}

/* --------------------------------------------------------------- check */

/* A file can be damaged on every page: the first MAX_PROBLEMS problems
 * are reported, the rest counted and said in one line at the end, so that
 * a crafted stick cannot flood inspect's output or the interface's memory
 * (the Inspect job keeps every line). */
#define MAX_PROBLEMS 200

typedef struct Problems {
    int           count;
    rx_problem_cb cb;
    void         *opaque;
} Problems;

static void problem(Problems *pr, const char *fmt, ...) RX_PRINTF(2, 3);
static void problem(Problems *pr, const char *fmt, ...)
{
    if (++pr->count > MAX_PROBLEMS || !pr->cb)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pr->cb(pr->opaque, buf);
}

static int problems_done(const Problems *pr)
{
    if (pr->count > MAX_PROBLEMS && pr->cb) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d more problems not listed", pr->count - MAX_PROBLEMS);
        pr->cb(pr->opaque, buf);
    }
    return pr->count;
}

int rx_pdb_check(const uint8_t *b, size_t size, rx_problem_cb cb, void *opaque)
{
    Problems pr = { .cb = cb, .opaque = opaque };
    if (size % RX_PDB_PAGE)
        problem(&pr, "file size is not a multiple of 4096");
    RXPdbHeader h;
    if (rx_pdb_read_header(b, size, &h) < 0) {
        problem(&pr, "unreadable file header");
        return problems_done(&pr);
    }
    size_t nb_pages = size / RX_PDB_PAGE;
    /* The table whose chain reached each page last (its index + 1): a
     * chain that comes back to a page of its own loops, said once, and is
     * followed no further. Each chain visits a page at most once. */
    uint8_t *owner = calloc(nb_pages + 1, 1);
    if (!owner) {
        problem(&pr, "out of memory");
        return problems_done(&pr);
    }
    uint32_t max_seq = 0;
    for (uint32_t i = 0; i < h.num_tables; i++) {
        const RXPdbTableDir *t = &h.tables[i];
        uint32_t page = t->first_page;
        while (1) {
            if (page >= nb_pages) {
                problem(&pr, "table %u: page %u beyond the end of the file", t->type, page);
                break;
            }
            if (owner[page] == i + 1) {
                problem(&pr, "table %u: page chain loops back to page %u", t->type, page);
                break;
            }
            const uint8_t *p = b + (size_t)page * RX_PDB_PAGE;
            max_seq = RX_MAX(max_seq, rx_rd_le32(p + 0x10));
            if (rx_rd_le32(p + 0x04) != page)
                problem(&pr, "table %u: page %u has wrong page_index", t->type, page);
            if (owner[page])
                problem(&pr, "page %u shared between tables", page);
            owner[page] = (uint8_t)(i + 1);
            if (!(p[0x1B] & 0x40)) {
                uint32_t v = p[0x18] | p[0x19] << 8 | p[0x1A] << 16;
                uint32_t n = v & 0x1FFF, valid = v >> 13;
                uint32_t free_size = rx_rd_le16(p + 0x1C), used = rx_rd_le16(p + 0x1E);
                if (RX_PDB_HEAP + used + dir_bytes(n) > RX_PDB_PAGE ||
                    free_size != RX_PDB_PAGE - RX_PDB_HEAP - used - dir_bytes(n))
                    problem(&pr, "page %u: free_size %u inconsistent", page, free_size);
                uint32_t present = 0;
                for (uint32_t g = 0; g < (n + 15) / 16 && 0x24 * g + 4 < RX_PDB_PAGE - RX_PDB_HEAP; g++)
                    present += (uint32_t)__builtin_popcount(rx_rd_le16(p + RX_PDB_PAGE - 0x24 * g - 4));
                if (present != valid)
                    problem(&pr, "page %u: %u present rows but num_rows %u", page, present, valid);
            }
            if (page == t->last_page)
                break;
            page = rx_rd_le32(p + 0x0C);
        }
        if ((size_t)t->last_page < nb_pages) {
            uint32_t last_next = rx_rd_le32(b + (size_t)t->last_page * RX_PDB_PAGE + 0x0C);
            if (last_next != t->empty_candidate)
                problem(&pr, "table %u: last page next %u != empty_candidate %u",
                        t->type, last_next, t->empty_candidate);
        }
        if ((size_t)t->empty_candidate < nb_pages) {
            const uint8_t *p = b + (size_t)t->empty_candidate * RX_PDB_PAGE;
            for (int k = 0; k < RX_PDB_PAGE; k++) {
                if (p[k]) {
                    problem(&pr, "table %u: empty_candidate page is not empty", t->type);
                    break;
                }
            }
        }
        if (t->empty_candidate >= h.next_unused)
            problem(&pr, "table %u: empty_candidate beyond next_unused_page", t->type);
    }
    free(owner);
    if (h.sequence <= max_seq)
        problem(&pr, "header sequence %u not above page sequence %u", h.sequence, max_seq);

    /* Long strings must be 4-aligned within their rows (docs/pdb.md G1). */
    for (uint32_t i = 0; i < h.num_tables; i++) {
        const RXPdbTableDir *t = &h.tables[i];
        /* Table types mean other things in exportExt.pdb (9 tables), where
         * 3 is the tags table with its name offset at 0x1D. */
        bool ext = h.num_tables == RX_PDB_EXT_NUM_TABLES;
        if (ext ? t->type != RX_PDB_EXT_TAGS
                : t->type != RX_PDB_TRACKS && t->type != RX_PDB_ARTISTS && t->type != RX_PDB_ALBUMS)
            continue;
        RXPdbRow *rows;
        int n;
        if (rx_pdb_table_rows(b, size, t, &rows, &n) < 0)
            continue;
        for (int r = 0; r < n; r++) {
            size_t p = rows[r].pos, end = rows[r].end;
            if (!ext && t->type == RX_PDB_TRACKS) {
                if (p + 0x88 > end)
                    continue;
                for (int s = 0; s < 21; s++) {
                    uint32_t o = rx_rd_le16(b + p + 0x5E + 2 * s);
                    if (p + o < end && !(b[p + o] & 1) && o % 4)
                        problem(&pr, "track row at 0x%zx: long string at unaligned offset 0x%x", p, o);
                }
            } else {
                size_t at = p + (ext ? 0x1D : t->type == RX_PDB_ARTISTS ? 9 : 0x15);
                if (at >= end)
                    continue;
                uint32_t o = b[at];
                if (p + o < end && !(b[p + o] & 1) && o % 4)
                    problem(&pr, "table %u row at 0x%zx: unaligned long name", t->type, p);
            }
        }
        free(rows);
    }
    return problems_done(&pr);
}
