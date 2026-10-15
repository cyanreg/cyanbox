/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A player's session with the database server (dbserver.h): the greeting,
 * setup and teardown, and every request answered from the catalog.
 *
 * A menu request is answered with how many rows it has (4000, the
 * request's kind and the count); the rows come when the player renders
 * them (3000, an offset and a count), as a header (4001), the items (4101)
 * and a footer (4201) (djl-analysis, menus.adoc; rbxport's capture of
 * rekordbox 7.2.11 and a CDJ-3000, rbl-dbserver tests/corpus.rs). The
 * session keeps the last menu asked for at each menu location, the second
 * byte of the request's first argument: a CDJ-3000 keeps a list at 1, the
 * next level at 2 and a track's metadata at 2 while loading at 8, and
 * renders each later. Data requests (artwork, waveforms, grid, cues, ANLZ
 * tags) are answered with one message, the request's kind first, a status,
 * the blob's length and the blob; with nothing to send the status is 0x32
 * and the blob is left out, as rekordbox's "no artwork" reply (rbxport's
 * capture), never silence: a player waits for each answer.
 *
 * A cue saved on the deck, or deleted, is kept (save_cue): the catalog
 * applies it to what it serves and hands it to the library's owner, and
 * the answer is the track's list as it is then; a rating is kept so too.
 * The other writes from the deck (the grid's offset, the tag list,
 * history) are refused, each in the shape the player waits for. Requests nobody knows get the generic error,
 * the request's kind in a 4003, as rekordbox answers them (rbxport, from
 * rekordbox's binary). Notices a player sends without waiting (history,
 * on air, analysis uploads) get nothing: an answer would be read as the
 * answer to its next request (rbxport: an XDJ-700's E-8306). */

#include "dbserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

#include "catalog.h"
#include "dbmsg.h"

/* The longest request taken, which bounds what a session holds of a
 * partial one. A player's are a few dozen bytes; its largest are uploads
 * of analysis it made (1604 bytes from an XDJ-700, rbxport's corpus),
 * refused anyway. Assumed: no player sends one this long. */
#define REQUEST_MAX (256 * 1024)
/* Menu locations a session keeps a menu for (players use 1, 2, 3, 5 and 8,
 * rbxport's capture); the oldest is dropped for a ninth. */
#define NB_MENUS    8
/* The most rows one render draws. Players ask for a screenful (25 at most
 * in rbxport's capture, 64 in beat-link's); this bounds a reply. */
#define RENDER_MAX  1024
/* rekordbox's KUVO user info: 160 bytes, of which a CDJ-3000 copies the
 * first 32 into what it reports (rbxport's capture). Ours is zeros, a user
 * with nothing to say. */
#define USER_INFO_LEN 160

typedef struct Menu {
    bool      used;
    int       location;
    uint64_t  age;
    RXCatMenu m;
} Menu;

struct RXDbSession {
    RXCatalog *const *cat;
    int       our_number;
    int       player;
    bool      greeted;
    /* Set up with the second argument rekordbox and a CDJ-3000 send, so
     * answered as rekordbox answers one (sixteen-argument items); else
     * as the djl-analysis describes (twelve). */
    bool      extended;
    bool      latin1;           /* a CDJ-900's: rx_dbmsg_item_latin1 */
    uint8_t  *in;
    size_t    in_len, in_cap;
    Menu      menus[NB_MENUS];
    uint64_t  clock;
};

void rx_dbsession_set_latin1(RXDbSession *s, bool latin1)
{
    s->latin1 = latin1;
}

int rx_dbsession_new(RXDbSession **out, RXCatalog *const *cat, int our_number)
{
    RXDbSession *s = calloc(1, sizeof(*s));
    if (!s)
        return RX_ERROR(ENOMEM);
    s->cat = cat;
    s->our_number = our_number;
    s->extended = true;
    *out = s;
    return 0;
}

void rx_dbsession_free(RXDbSession **sp)
{
    RXDbSession *s = *sp;
    if (!s)
        return;
    for (int i = 0; i < NB_MENUS; i++)
        rx_catalog_menu_free(&s->menus[i].m);
    free(s->in);
    free(s);
    *sp = NULL;
}

/* ------------------------------------------------------------- replies */

static void numbers(RXWr *w, uint32_t txid, uint16_t kind, uint32_t a, uint32_t b)
{
    rx_dbmsg_numbers(w, txid, kind, (const uint32_t[]) { a, b }, 2);
}

static void error_reply(RXWr *w, const RXDbMsg *q)
{
    rx_dbmsg_numbers(w, q->txid, RX_DB_ERROR, (const uint32_t[]) { q->kind }, 1);
}

/* [request kind, status, length, blob] and a trailing number for some:
 * status 0 with the blob, RX_DB_STATUS_NONE with nothing. */
static void blob_reply(RXWr *w, const RXDbMsg *q, uint16_t kind, const uint8_t *blob,
                       size_t len, bool tail, uint32_t tail_value)
{
    bool some = blob && len && len <= UINT32_MAX;
    RXDbMsg m = { .txid = q->txid, .kind = kind, .nb_args = 4 + tail };
    m.args[0] = (RXDbArg) { .type = RX_DB_NUM, .num = q->kind };
    m.args[1] = (RXDbArg) { .type = RX_DB_NUM, .num = some ? 0 : RX_DB_STATUS_NONE };
    m.args[2] = (RXDbArg) { .type = RX_DB_NUM, .num = some ? (uint32_t)len : 0 };
    m.args[3] = (RXDbArg) { .type = RX_DB_BLOB, .p = some ? blob : NULL,
                            .len = some ? (uint32_t)len : 0 };
    m.args[4] = (RXDbArg) { .type = RX_DB_NUM, .num = tail_value };
    rx_dbmsg_write(w, &m);
}

/* The analysis replies: the catalog appends the blob to a buffer of its
 * own, then the message goes out. */
typedef int (*blob_fn)(RXCatalog *c, uint32_t track, RXWr *blob);

static int analysis_reply(RXWr *w, const RXDbMsg *q, uint16_t kind, RXCatalog *c, blob_fn fn,
                          uint32_t track, bool tail, uint32_t tail_value)
{
    RXWr blob = rx_wr_dyn();
    int ret = fn(c, track, &blob);
    if (blob.err) {
        rx_wr_free(&blob);
        return RX_ERROR(ENOMEM);
    }
    blob_reply(w, q, kind, ret < 0 ? NULL : blob.p, ret < 0 ? 0 : blob.len, tail, tail_value);
    rx_wr_free(&blob);
    return 0;
}

static int tag_reply(RXWr *w, const RXDbMsg *q, RXCatalog *c)
{
    RXWr blob = rx_wr_dyn();
    int ret = rx_catalog_tag(c, rx_dbmsg_num(q, 1), rx_dbmsg_num(q, 2), rx_dbmsg_num(q, 3), &blob);
    if (blob.err) {
        rx_wr_free(&blob);
        return RX_ERROR(ENOMEM);
    }
    /* A trailing 1, as rekordbox 7.2.11 sent (rbxport's capture; the
     * djl-analysis saw 0 from players). */
    blob_reply(w, q, RX_DB_TAG_REPLY, ret < 0 ? NULL : blob.p, ret < 0 ? 0 : blob.len, true, 1);
    rx_wr_free(&blob);
    return 0;
}

static int artwork_reply(RXDbSession *s, RXWr *w, const RXDbMsg *q, RXCatalog *c)
{
    /* 2003 names an artwork by the id the older item layout carries, or
     * the item's own id in rekordbox's (dbmsg.h, RXDbItem.c), with a third
     * argument 1 for the 240 px one, 2 for a track rekordbox did not
     * analyse (track_metadata.adoc "Album Art"). */
    const uint8_t *jpeg = NULL;
    size_t size = 0;
    uint32_t id = rx_dbmsg_num(q, 1), size_arg = rx_dbmsg_num(q, 2);
    if (size_arg != 2)
        rx_catalog_artwork(c, id, s->extended, size_arg == 1, &jpeg, &size);
    blob_reply(w, q, RX_DB_ARTWORK_REPLY, jpeg, size, false, 0);
    return 0;
}

/* The extended cue list (4e02): the entries and their count last. A track
 * without cues gets status 1 and no entries, which a player reads as
 * success (rbxport, from rekordbox 7.2.19's GetUsbCueExtSharedContent); a
 * track there is not, the 0x32 refusal. */
static int cues_ext_reply(RXWr *w, const RXDbMsg *q, RXCatalog *c)
{
    RXWr blob = rx_wr_dyn();
    uint32_t count = 0;
    int ret = rx_catalog_cues_ext(c, rx_dbmsg_num(q, 1), &blob, &count);
    if (blob.err || ret == RX_ERROR(ENOMEM)) {
        rx_wr_free(&blob);
        return RX_ERROR(ENOMEM);
    }
    if (ret >= 0 && !count) {
        const uint32_t v[3] = { q->kind, 1, 0 };
        RXDbMsg m = { .txid = q->txid, .kind = RX_DB_CUES_EXT_REPLY, .nb_args = 5 };
        for (int i = 0; i < 3; i++)
            m.args[i] = (RXDbArg) { .type = RX_DB_NUM, .num = v[i] };
        m.args[3] = (RXDbArg) { .type = RX_DB_BLOB };
        m.args[4] = (RXDbArg) { .type = RX_DB_NUM, .num = 0 };
        rx_dbmsg_write(w, &m);
    } else {
        blob_reply(w, q, RX_DB_CUES_EXT_REPLY, ret < 0 ? NULL : blob.p, ret < 0 ? 0 : blob.len,
                   true, count);
    }
    rx_wr_free(&blob);
    return 0;
}

/* The older cue list (4702), nine arguments: the request's kind, a
 * status, the records' length and records, their size 0x24, how many hot
 * and memory cues, the times' length and times (track_metadata.adoc
 * "Requesting Cue Points and Loops"). */
static void cues_message(RXWr *w, const RXDbMsg *q, bool ok, const RXWr *records,
                         const RXWr *times, uint32_t hot, uint32_t memory)
{
    RXDbMsg m = { .txid = q->txid, .kind = RX_DB_CUES_REPLY, .nb_args = 9 };
    uint32_t nr = ok ? (uint32_t)records->len : 0, nt = ok ? (uint32_t)times->len : 0;
    const uint32_t v[] = { q->kind, ok ? 0 : RX_DB_STATUS_NONE, nr };
    for (int i = 0; i < 3; i++)
        m.args[i] = (RXDbArg) { .type = RX_DB_NUM, .num = v[i] };
    m.args[3] = (RXDbArg) { .type = RX_DB_BLOB, .p = nr ? records->p : NULL, .len = nr };
    m.args[4] = (RXDbArg) { .type = RX_DB_NUM, .num = 0x24 };
    m.args[5] = (RXDbArg) { .type = RX_DB_NUM, .num = ok ? hot : 0 };
    m.args[6] = (RXDbArg) { .type = RX_DB_NUM, .num = ok ? memory : 0 };
    m.args[7] = (RXDbArg) { .type = RX_DB_NUM, .num = nt };
    m.args[8] = (RXDbArg) { .type = RX_DB_BLOB, .p = nt ? times->p : NULL, .len = nt };
    rx_dbmsg_write(w, &m);
}

/* The track's older cue list, or with `refuse` its failure. */
static int cues_reply(RXWr *w, const RXDbMsg *q, RXCatalog *c, bool refuse)
{
    RXWr records = rx_wr_dyn(), times = rx_wr_dyn();
    uint32_t hot = 0, memory = 0;
    int ret = !refuse && c ?
              rx_catalog_cues(c, rx_dbmsg_num(q, 1), &records, &times, &hot, &memory) :
              RX_ERROR(EROFS);
    if (records.err || times.err || ret == RX_ERROR(ENOMEM)) {
        rx_wr_free(&records);
        rx_wr_free(&times);
        return RX_ERROR(ENOMEM);
    }
    cues_message(w, q, ret >= 0, &records, &times, hot, memory);
    rx_wr_free(&records);
    rx_wr_free(&times);
    return 0;
}

/* ------------------------------------------------------- cues saved */

/* 1/150 s as ms, rounded up, as rekordbox converts a player's frames
 * (rbxport, from rekordbox 7.2.19's SavUsbCue). */
static uint32_t frames_ms(uint64_t frames)
{
    return (uint32_t)RX_MIN((frames * 1000 + 149) / 150, 0xfffffffeu);
}

/* An entry of the extended cue list (2705; the 4e02 list's layout) read
 * as rekordbox 7.2.19 reads one (rbxport, from its SavUsbCueExt),
 * little-endian: the hot cue's slot at 4 (1..8, 0 a memory cue), 1 a cue
 * or 2 a loop at 6, the time base at 0xa (1000 for ms, or 150 or 75
 * frames a second), in and out at 0xc and 0x10, an active memory loop's
 * bit 4 at 0x18; then, when the length at 0x34 says that much follows: a
 * memory cue's colour id at 0x3a, the comment's length in bytes (its NUL
 * included) at 0x48 and the comment in UTF-16 after it, and after that a
 * length and a hot cue's colour code and RGB. -EINVAL for one rekordbox
 * does not take: it changes nothing and answers with the list. */
static int cue_ext_parse(const uint8_t *p, uint32_t len, RXStickCue *c)
{
    RXRd r = rx_rd(p, len);
    unsigned slot = rx_rd_at_u16le(&r, 4), shape = rx_rd_at_u8(&r, 6);
    unsigned base = rx_rd_at_u16le(&r, 0xa);
    if (slot > RX_HOT_CUE_SLOTS || (shape != 1 && shape != 2) ||
        (base != 1000 && base != 150 && base != 75))
        return RX_ERROR(EINVAL);
    bool loop = shape == 2;
    uint32_t in = rx_rd_at_u32le(&r, 0xc), out = rx_rd_at_u32le(&r, 0x10);
    if (base != 1000) {
        uint64_t k = base == 75 ? 2 : 1;
        in = frames_ms(in * k);
        out = frames_ms(out * k + 1);
    }
    *c = (RXStickCue) {
        .hot    = slot ? (int)slot - 1 : -1,
        .in     = in,
        .out    = loop ? out : 0xffffffffu,
        .active = !slot && loop && (rx_rd_at_u16le(&r, 0x18) & 4),
    };
    uint32_t more = rx_rd_at_u32le(&r, 0x34);
    if (!more || len < 0x38 + (uint64_t)more)
        return 0;
    if (!slot && rx_rd_at_u8(&r, 0x3a) <= 8)
        c->code = rx_rd_at_u8(&r, 0x3a);
    uint32_t name_len = rx_rd_at_u16le(&r, 0x48);
    if (name_len && 0x4a + name_len <= len) {
        char *name = rx_link_utf16_to_utf8(p + 0x4a, name_len, false);
        if (!name)
            return RX_ERROR(ENOMEM);
        snprintf(c->name, sizeof(c->name), "%s", name);
        free(name);
    }
    uint32_t tail = 0x4a + name_len;
    if (slot && tail + 8 <= len) {
        uint32_t n = RX_MIN(rx_rd_at_u32le(&r, tail), 4u);
        uint8_t v[4] = { 0 };
        for (uint32_t i = 0; i < n; i++)
            v[i] = rx_rd_at_u8(&r, tail + 4 + i);
        c->code = v[0] < 64 ? v[0] : 0;
        c->rgb = (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
    }
    return 0;
}

/* A record of the older cue list (2105, 36 bytes) and the milliseconds
 * sent after it, read as rekordbox 7.2.19 reads them (rbxport, from its
 * SavUsbCue): flags at 0 (the hot cue's letter 1..3 << 16, bit 0 a loop,
 * 0x200 an active one), in and out in 1/150 s at 0xc and 0x10, or the
 * milliseconds' in and out when their in is not 0. -EINVAL for a hot
 * cue past C, which rekordbox refuses. */
static int cue_old_parse(const uint8_t *p, uint32_t len, const uint8_t *ms, uint32_t ms_len,
                         RXStickCue *c)
{
    RXRd r = rx_rd(p, len), m = rx_rd(ms, ms_len);
    uint32_t flags = rx_rd_at_u32le(&r, 0);
    if (flags >= 0x40000)
        return RX_ERROR(EINVAL);
    bool loop = flags & 1;
    unsigned slot = flags >> 16;
    uint32_t in = rx_rd_at_u32le(&m, 0), out = rx_rd_at_u32le(&m, 4);
    if (!in) {
        in = frames_ms(rx_rd_at_u32le(&r, 0xc));
        out = frames_ms((uint64_t)rx_rd_at_u32le(&r, 0x10) + 1);
    }
    *c = (RXStickCue) {
        .hot    = slot ? (int)slot - 1 : -1,
        .in     = in,
        .out    = loop ? out : 0xffffffffu,
        .active = !slot && loop && (flags & 0x200),
    };
    return 0;
}

/* A cue saved on the deck, or deleted (its third argument 0): [r:m:s:t,
 * track, save, the record's length, the record] and for the older form
 * [8, the milliseconds] after it. Applied to what is served and handed
 * to the library's owner (rx_catalog_cue_edit), then answered with the
 * track's list as it is now, in the request's form (4e02 for 2705, 4702
 * for 2105), as rekordbox answers (rbxport, from rekordbox 7.2.19). A
 * record too short to be one is refused (0x32), and so is an older one
 * past C; an extended one rekordbox does not take changes nothing and
 * gets the list. Never the generic error: a CDJ-3000 that gets it for a
 * saved cue sends the cue again forever (rbxport, from CDJ-3000 firmware
 * 3.20). */
static int save_cue(RXDbSession *s, RXWr *w, const RXDbMsg *q, RXCatalog *c)
{
    bool ext = q->kind == RX_DB_SAVE_CUE_EXT;
    uint32_t len = 0, ms_len = 0;
    const uint8_t *rec = rx_dbmsg_blob(q, 4, &len);
    const uint8_t *ms = ext ? NULL : rx_dbmsg_blob(q, 6, &ms_len);
    len = RX_MIN(len, rx_dbmsg_num(q, 3));
    RXStickCue cue;
    int ret = RX_ERROR(EPROTO);
    if (c && rec && len >= (ext ? 0x38u : 0x24u))
        ret = ext ? cue_ext_parse(rec, len, &cue) : cue_old_parse(rec, len, ms, ms_len, &cue);
    if (ret == RX_ERROR(ENOMEM))
        return ret;
    bool refuse = ret < 0 && (ret != RX_ERROR(EINVAL) || !ext);
    if (ret >= 0) {
        /* The player by the request's r:m:s:t, else as it set up. */
        int player = (int)(rx_dbmsg_num(q, 0) >> 24);
        ret = rx_catalog_cue_edit(c, rx_dbmsg_num(q, 1), player ? player : s->player,
                                  !rx_dbmsg_num(q, 2), &cue);
        if (ret == RX_ERROR(ENOMEM))
            return ret;
        refuse = ret < 0;
    }
    if (!ext)
        return cues_reply(w, q, c, refuse);
    if (refuse) {
        blob_reply(w, q, RX_DB_CUES_EXT_REPLY, NULL, 0, true, 0);
        return 0;
    }
    return cues_ext_reply(w, q, c);
}

/* --------------------------------------------------------------- menus */

static Menu *menu_at(RXDbSession *s, int location, bool make)
{
    Menu *old = &s->menus[0];
    for (int i = 0; i < NB_MENUS; i++) {
        if (s->menus[i].used && s->menus[i].location == location)
            return &s->menus[i];
        if (!s->menus[i].used || (old->used && s->menus[i].age < old->age))
            old = &s->menus[i];
    }
    if (!make)
        return NULL;
    rx_catalog_menu_free(&old->m);
    *old = (Menu) { .used = true, .location = location };
    return old;
}

static int menu_request(RXDbSession *s, RXWr *w, const RXDbMsg *q, RXCatalog *c)
{
    RXCatMenu m;
    int ret = c ? rx_catalog_menu(c, q, &m) : RX_ERROR(ENOENT);
    if (ret == RX_ERROR(ENOENT)) {
        /* Not a menu: rekordbox's generic error, and the menus kept as
         * they were (rbxport, from rekordbox's binary). */
        error_reply(w, q);
        return 0;
    }
    if (ret < 0)
        return ret;
    Menu *slot = menu_at(s, rx_db_location(rx_dbmsg_num(q, 0)), true);
    rx_catalog_menu_free(&slot->m);
    slot->m = m;
    slot->age = ++s->clock;
    numbers(w, q->txid, RX_DB_AVAILABLE, q->kind, m.nb);
    return 0;
}

static void render(RXDbSession *s, RXWr *w, const RXDbMsg *q, RXCatalog *c)
{
    Menu *slot = menu_at(s, rx_db_location(rx_dbmsg_num(q, 0)), false);
    uint32_t total = slot ? slot->m.nb : 0;
    uint32_t offset = RX_MIN(rx_dbmsg_num(q, 1), total);
    uint32_t count = RX_MIN(RX_MIN(rx_dbmsg_num(q, 2), total - offset), (uint32_t)RENDER_MAX);
    /* The column beside a title: one a CDJ-3000's render names (its
     * seventh and eighth arguments, both non-zero); a six-argument render,
     * the djl-analysis', shows the column of the sort the list was asked
     * with (track_metadata.adoc, "Alternate Track List Sort Orders"); else
     * the default. */
    int column = RX_CAT_COLUMN_DEFAULT;
    if (q->nb_args >= 8 && rx_dbmsg_num(q, 6) && rx_dbmsg_num(q, 7))
        column = (int)RX_MIN(rx_dbmsg_num(q, 7), 0xffu);
    else if (q->nb_args == 6 && slot)
        column = slot->m.sort;
    numbers(w, q->txid, RX_DB_MENU_HEADER, 1, offset);
    if (slot && c && count)
        rx_catalog_render(c, &slot->m, offset, count, column, s->extended, s->latin1, q->txid, w);
    rx_dbmsg_numbers(w, q->txid, RX_DB_MENU_FOOTER, NULL, 0);
}

/* ------------------------------------------------------------ dispatch */

static int handle(RXDbSession *s, const RXDbMsg *q, RXWr *w)
{
    RXCatalog *c = s->cat ? *s->cat : NULL;
    uint32_t a1 = rx_dbmsg_num(q, 1);
    switch (q->kind) {
    case RX_DB_SETUP:
        /* rekordbox answers a CDJ-3000's setup [device, 0x14] with its own
         * kind and [its number, 0x14] (rbxport's capture); the older setup
         * of one argument with 4000 [0, its number] (track_metadata.adoc
         * "Query context setup response"). */
        s->player = (int)rx_dbmsg_num(q, 0);
        s->extended = q->nb_args > 1;
        if (s->extended)
            numbers(w, q->txid, RX_DB_SETUP, (uint32_t)s->our_number, RX_DB_SETUP_MAGIC);
        else
            numbers(w, q->txid, RX_DB_AVAILABLE, 0, (uint32_t)s->our_number);
        return 0;
    case RX_DB_TEARDOWN:
        /* The server closes the connection; nothing is sent (beat-link,
         * Client.close). */
        return RX_ERROR(ESHUTDOWN);
    case RX_DB_CANCEL:
        /* A player gives up a request, a preview of the row it scrolled
         * past, by sending 0001 with that request's txid and no arguments;
         * a CDJ-2000nexus answers nothing to it (dysentery's S05, S06, S15a
         * and S20 captures: the request's own answer and no other). Each
         * request here is answered before the next is read, so there is
         * nothing left to give up. beat-link names it INVALID_DATA. A
         * CDJ-900 sends it after a preview too (2026-10-14), and was
         * answered with the unknown request's error until then. */
        return 0;
    case RX_DB_RENDER:
        render(s, w, q, c);
        return 0;
    case RX_DB_ITEM_POSITION: {
        /* Where an item is in the menu at the location: 4000 [3100, its
         * index] (rbxport's capture, 0 for the first). */
        Menu *slot = menu_at(s, rx_db_location(rx_dbmsg_num(q, 0)), false);
        numbers(w, q->txid, RX_DB_AVAILABLE, q->kind,
                slot ? rx_catalog_menu_find(&slot->m, a1) : RX_DB_ALL);
        return 0;
    }

    case RX_DB_ARTWORK:
        return artwork_reply(s, w, q, c);
    case RX_DB_PREVIEW:
        /* The track in the third argument (track_metadata.adoc; rbxport's
         * capture). */
        return analysis_reply(w, q, RX_DB_PREVIEW_REPLY, c, rx_catalog_preview,
                              rx_dbmsg_num(q, 2), false, 0);
    case RX_DB_DETAIL:
        return analysis_reply(w, q, RX_DB_DETAIL_REPLY, c, rx_catalog_detail, a1, false, 0);
    case RX_DB_GRID:
        /* A trailing 0, the grid's offset (rbxport's capture). */
        return analysis_reply(w, q, RX_DB_GRID_REPLY, c, rx_catalog_grid, a1, true, 0);
    case RX_DB_VBR:
        return analysis_reply(w, q, RX_DB_VBR_REPLY, c, rx_catalog_vbr, a1, false, 0);
    case RX_DB_TAG:
    case RX_DB_TAG_2EX:
        return tag_reply(w, q, c);
    case RX_DB_CUES_EXT:
        return cues_ext_reply(w, q, c);
    case RX_DB_CUES:
        return cues_reply(w, q, c, false);
    case RX_DB_USER_INFO: {
        /* [3006, 0, 160, the blob]: a CDJ-3000 waits for this after every
         * load from a rekordbox source before it asks for anything else
         * (rbxport, CDJ-3000 3.20). */
        static const uint8_t user[USER_INFO_LEN];
        blob_reply(w, q, RX_DB_USER_INFO_REPLY, user, sizeof(user), false, 0);
        return 0;
    }
    case RX_DB_FILTER:
        /* The CDJ-3000's first request: 4000 [3007, 0] (rbxport's capture). */
        numbers(w, q->txid, RX_DB_AVAILABLE, q->kind, 0);
        return 0;

    /* Cues saved on the deck are kept, and ratings (below); the other
     * writes refused (docs/LINK.md). */
    case RX_DB_SAVE_CUE_EXT:
    case RX_DB_SAVE_CUE:
        return save_cue(s, w, q, c);
    case RX_DB_SAVE_RATING: {
        /* [r:m:s:t, track, 0 to 5 stars], kept as a saved cue is:
         * 4000 [kind, 0] when it was, 1 when not (rbxport, from rekordbox
         * 7.2.19). */
        uint32_t stars = rx_dbmsg_num(q, 2);
        int player = (int)(rx_dbmsg_num(q, 0) >> 24);
        int ret = !c ? RX_ERROR(ENOENT) : stars > 5 ? RX_ERROR(EINVAL) :
                  rx_catalog_rating_edit(c, a1, player ? player : s->player, (int)stars);
        if (ret == RX_ERROR(ENOMEM))
            return ret;
        numbers(w, q->txid, RX_DB_AVAILABLE, q->kind, ret < 0);
        return 0;
    }
    case RX_DB_SAVE_GRID_OFFSET:
    case RX_DB_SAVE_TAG:
    case RX_DB_CLEAR_TAGS:
        /* 4000 [kind, 1]: 0 says it was saved (rbxport). */
        numbers(w, q->txid, RX_DB_AVAILABLE, q->kind, 1);
        return 0;
    case RX_DB_SAVE_ATOM:
    case RX_DB_UPDATE_ATOM:
        numbers(w, q->txid, RX_DB_AVAILABLE, q->kind, RX_DB_STATUS_NONE);
        return 0;
    case RX_DB_HISTORY_REMOVE:
        numbers(w, q->txid, RX_DB_AVAILABLE, q->kind, RX_DB_ALL);
        return 0;
    case RX_DB_HISTORY_ADD:
    case RX_DB_HISTORY_DELETE:
    case RX_DB_ON_AIR:
    case RX_DB_SAVE_WAVEFORM:
    case RX_DB_SAVE_VBR:
    case 0x3203:
    case 0x3503:
        return 0;
    }
    return menu_request(s, w, q, c);
}

static int keep(RXDbSession *s, const uint8_t *p, size_t n)
{
    if (n > s->in_cap) {
        uint8_t *in = realloc(s->in, n);
        if (!in)
            return RX_ERROR(ENOMEM);
        s->in = in;
        s->in_cap = n;
    }
    if (n && p != s->in)
        memmove(s->in, p, n);
    s->in_len = n;
    return 0;
}

int rx_dbsession_input(RXDbSession *s, const uint8_t *buf, size_t len, RXWr *out)
{
    if (!len)
        return 0;
    const uint8_t *p = buf;
    size_t n = len;
    /* What is left of a message from the last call goes first. */
    if (s->in_len) {
        if (len > SIZE_MAX - s->in_len)
            return RX_ERROR(E2BIG);
        size_t have = s->in_len;
        if (have + len > s->in_cap) {
            uint8_t *in = realloc(s->in, have + len);
            if (!in)
                return RX_ERROR(ENOMEM);
            s->in = in;
            s->in_cap = have + len;
        }
        if (len)
            memcpy(s->in + have, buf, len);
        s->in_len = have + len;
        p = s->in;
        n = s->in_len;
    }

    size_t pos = 0;
    int ret = 0;
    while (pos < n && ret >= 0) {
        if (!s->greeted) {
            size_t have = RX_MIN(n - pos, (size_t)RX_DB_GREETING_LEN);
            if (memcmp(p + pos, rx_db_greeting, have)) {
                ret = RX_ERROR(EPROTO);
                break;
            }
            if (have < RX_DB_GREETING_LEN)
                break;
            rx_wr_bytes(out, rx_db_greeting, RX_DB_GREETING_LEN);
            s->greeted = true;
            pos += RX_DB_GREETING_LEN;
            continue;
        }
        RXDbMsg q;
        int r = rx_dbmsg_parse(p + pos, n - pos, REQUEST_MAX, &q);
        if (r == 0)
            break;
        if (r < 0) {
            rx_debug("link: player %d: a request that is not one (%s)", s->player, strerror(-r));
            ret = r;
            break;
        }
        pos += (size_t)r;
        if (rx_log_level() >= RX_LOG_DEBUG) {
            char args[160] = "";
            for (int i = 0, at = 0; i < q.nb_args && at < (int)sizeof(args) - 16; i++)
                at += snprintf(args + at, sizeof(args) - at, q.args[i].type == RX_DB_NUM ? " %x" :
                               q.args[i].type == RX_DB_STR ? " str" : " blob", q.args[i].num);
            rx_debug("link: player %d asks %04x:%s", s->player, q.kind, args);
        }
        size_t before = out->len;
        ret = handle(s, &q, out);
        rx_debug("link: answered %04x with %zu bytes%s", q.kind, out->len - before,
                 ret < 0 ? " and closes" : "");
    }
    if (ret < 0) {
        s->in_len = 0;
        return ret;
    }
    if (out->err)
        return RX_ERROR(ENOMEM);
    return keep(s, p + pos, n - pos);
}

/* -------------------------------------------------------------- the port */

/* The query on 12523: a four-byte length, 15, and "RemoteDBServer" with
 * its NUL; the answer is the port, two bytes big-endian
 * (track_metadata.adoc "Connecting to the Database"; rbxport's capture). */
static const uint8_t query[] = {
    0x00, 0x00, 0x00, 0x0f, 'R', 'e', 'm', 'o', 't', 'e', 'D', 'B', 'S', 'e', 'r', 'v', 'e', 'r', 0x00,
};

int rx_dbquery_parse(const uint8_t *buf, size_t len)
{
    size_t have = RX_MIN(len, sizeof(query));
    if (memcmp(buf, query, have))
        return RX_ERROR(EPROTO);
    return have < sizeof(query) ? 0 : (int)sizeof(query);
}

void rx_dbquery_reply(uint16_t port, uint8_t reply[2])
{
    reply[0] = (uint8_t)(port >> 8);
    reply[1] = (uint8_t)port;
}
