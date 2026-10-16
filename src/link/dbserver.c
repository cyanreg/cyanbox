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
 * the answer is the track's list as it is then; a rating is kept so too,
 * and the TAG LIST, which is the owner's, kept as it is changed. The
 * other writes from the deck (the grid's offset, history) are refused,
 * each in the shape the player waits for. Requests nobody knows get the generic error,
 * the request's kind in a 4003, as rekordbox answers them (rbxport, from
 * rekordbox's binary). Notices a player sends without waiting (history,
 * on air, analysis uploads) get nothing: an answer would be read as the
 * answer to its next request (rbxport: an XDJ-700's E-8306). */

#include "dbserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyanbox/common.h"

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
    CBCatMenu m;
} Menu;

struct CBDbSession {
    CBCatalog *const *cat;
    int       our_number;
    int       player;
    bool      greeted;
    /* Set up with the second argument rekordbox and a CDJ-3000 send, so
     * answered as rekordbox answers one (sixteen-argument items); else
     * as the djl-analysis describes (twelve). */
    bool      extended;
    bool      latin1;           /* a CDJ-900's: cb_dbmsg_item_latin1 */
    uint8_t  *in;
    size_t    in_len, in_cap;
    Menu      menus[NB_MENUS];
    uint64_t  clock;
};

void cb_dbsession_set_latin1(CBDbSession *s, bool latin1)
{
    s->latin1 = latin1;
}

int cb_dbsession_new(CBDbSession **out, CBCatalog *const *cat, int our_number)
{
    CBDbSession *s = calloc(1, sizeof(*s));
    if (!s)
        return CB_ERROR(ENOMEM);
    s->cat = cat;
    s->our_number = our_number;
    s->extended = true;
    *out = s;
    return 0;
}

void cb_dbsession_free(CBDbSession **sp)
{
    CBDbSession *s = *sp;
    if (!s)
        return;
    for (int i = 0; i < NB_MENUS; i++)
        cb_catalog_menu_free(&s->menus[i].m);
    free(s->in);
    free(s);
    *sp = NULL;
}

/* ------------------------------------------------------------- replies */

static void numbers(CBWr *w, uint32_t txid, uint16_t kind, uint32_t a, uint32_t b)
{
    cb_dbmsg_numbers(w, txid, kind, (const uint32_t[]) { a, b }, 2);
}

static void error_reply(CBWr *w, const CBDbMsg *q)
{
    cb_dbmsg_numbers(w, q->txid, CB_DB_ERROR, (const uint32_t[]) { q->kind }, 1);
}

/* [request kind, status, length, blob] and a trailing number for some:
 * status 0 with the blob, CB_DB_STATUS_NONE with nothing. */
static void blob_reply(CBWr *w, const CBDbMsg *q, uint16_t kind, const uint8_t *blob,
                       size_t len, bool tail, uint32_t tail_value)
{
    bool some = blob && len && len <= UINT32_MAX;
    CBDbMsg m = { .txid = q->txid, .kind = kind, .nb_args = 4 + tail };
    m.args[0] = (CBDbArg) { .type = CB_DB_NUM, .num = q->kind };
    m.args[1] = (CBDbArg) { .type = CB_DB_NUM, .num = some ? 0 : CB_DB_STATUS_NONE };
    m.args[2] = (CBDbArg) { .type = CB_DB_NUM, .num = some ? (uint32_t)len : 0 };
    m.args[3] = (CBDbArg) { .type = CB_DB_BLOB, .p = some ? blob : NULL,
                            .len = some ? (uint32_t)len : 0 };
    m.args[4] = (CBDbArg) { .type = CB_DB_NUM, .num = tail_value };
    cb_dbmsg_write(w, &m);
}

/* The analysis replies: the catalog appends the blob to a buffer of its
 * own, then the message goes out. */
typedef int (*blob_fn)(CBCatalog *c, uint32_t track, CBWr *blob);

static int analysis_reply(CBWr *w, const CBDbMsg *q, uint16_t kind, CBCatalog *c, blob_fn fn,
                          uint32_t track, bool tail, uint32_t tail_value)
{
    CBWr blob = cb_wr_dyn();
    int ret = fn(c, track, &blob);
    if (blob.err) {
        cb_wr_free(&blob);
        return CB_ERROR(ENOMEM);
    }
    blob_reply(w, q, kind, ret < 0 ? NULL : blob.p, ret < 0 ? 0 : blob.len, tail, tail_value);
    cb_wr_free(&blob);
    return 0;
}

static int tag_reply(CBWr *w, const CBDbMsg *q, CBCatalog *c)
{
    CBWr blob = cb_wr_dyn();
    int ret = cb_catalog_tag(c, cb_dbmsg_num(q, 1), cb_dbmsg_num(q, 2), cb_dbmsg_num(q, 3), &blob);
    if (blob.err) {
        cb_wr_free(&blob);
        return CB_ERROR(ENOMEM);
    }
    /* A trailing 1, as rekordbox 7.2.11 sent (rbxport's capture; the
     * djl-analysis saw 0 from players). */
    blob_reply(w, q, CB_DB_TAG_REPLY, ret < 0 ? NULL : blob.p, ret < 0 ? 0 : blob.len, true, 1);
    cb_wr_free(&blob);
    return 0;
}

static int artwork_reply(CBDbSession *s, CBWr *w, const CBDbMsg *q, CBCatalog *c)
{
    /* 2003 names an artwork by the id the older item layout carries, or
     * the item's own id in rekordbox's (dbmsg.h, CBDbItem.c), with a third
     * argument 1 for the 240 px one, 2 for a track rekordbox did not
     * analyse (track_metadata.adoc "Album Art"). */
    const uint8_t *jpeg = NULL;
    size_t size = 0;
    uint32_t id = cb_dbmsg_num(q, 1), size_arg = cb_dbmsg_num(q, 2);
    if (size_arg != 2)
        cb_catalog_artwork(c, id, s->extended, size_arg == 1, &jpeg, &size);
    blob_reply(w, q, CB_DB_ARTWORK_REPLY, jpeg, size, false, 0);
    return 0;
}

/* The extended cue list (4e02): the entries and their count last. A track
 * without cues gets status 1 and no entries, which a player reads as
 * success (rbxport, from rekordbox 7.2.19's GetUsbCueExtSharedContent); a
 * track there is not, the 0x32 refusal. */
static int cues_ext_reply(CBWr *w, const CBDbMsg *q, CBCatalog *c)
{
    CBWr blob = cb_wr_dyn();
    uint32_t count = 0;
    int ret = cb_catalog_cues_ext(c, cb_dbmsg_num(q, 1), &blob, &count);
    if (blob.err || ret == CB_ERROR(ENOMEM)) {
        cb_wr_free(&blob);
        return CB_ERROR(ENOMEM);
    }
    if (ret >= 0 && !count) {
        const uint32_t v[3] = { q->kind, 1, 0 };
        CBDbMsg m = { .txid = q->txid, .kind = CB_DB_CUES_EXT_REPLY, .nb_args = 5 };
        for (int i = 0; i < 3; i++)
            m.args[i] = (CBDbArg) { .type = CB_DB_NUM, .num = v[i] };
        m.args[3] = (CBDbArg) { .type = CB_DB_BLOB };
        m.args[4] = (CBDbArg) { .type = CB_DB_NUM, .num = 0 };
        cb_dbmsg_write(w, &m);
    } else {
        blob_reply(w, q, CB_DB_CUES_EXT_REPLY, ret < 0 ? NULL : blob.p, ret < 0 ? 0 : blob.len,
                   true, count);
    }
    cb_wr_free(&blob);
    return 0;
}

/* The older cue list (4702), nine arguments: the request's kind, a
 * status, the records' length and records, their size 0x24, how many hot
 * and memory cues, the times' length and times (track_metadata.adoc
 * "Requesting Cue Points and Loops"). */
static void cues_message(CBWr *w, const CBDbMsg *q, bool ok, const CBWr *records,
                         const CBWr *times, uint32_t hot, uint32_t memory)
{
    CBDbMsg m = { .txid = q->txid, .kind = CB_DB_CUES_REPLY, .nb_args = 9 };
    uint32_t nr = ok ? (uint32_t)records->len : 0, nt = ok ? (uint32_t)times->len : 0;
    const uint32_t v[] = { q->kind, ok ? 0 : CB_DB_STATUS_NONE, nr };
    for (int i = 0; i < 3; i++)
        m.args[i] = (CBDbArg) { .type = CB_DB_NUM, .num = v[i] };
    m.args[3] = (CBDbArg) { .type = CB_DB_BLOB, .p = nr ? records->p : NULL, .len = nr };
    m.args[4] = (CBDbArg) { .type = CB_DB_NUM, .num = 0x24 };
    m.args[5] = (CBDbArg) { .type = CB_DB_NUM, .num = ok ? hot : 0 };
    m.args[6] = (CBDbArg) { .type = CB_DB_NUM, .num = ok ? memory : 0 };
    m.args[7] = (CBDbArg) { .type = CB_DB_NUM, .num = nt };
    m.args[8] = (CBDbArg) { .type = CB_DB_BLOB, .p = nt ? times->p : NULL, .len = nt };
    cb_dbmsg_write(w, &m);
}

/* The track's older cue list, or with `refuse` its failure. */
static int cues_reply(CBWr *w, const CBDbMsg *q, CBCatalog *c, bool refuse)
{
    CBWr records = cb_wr_dyn(), times = cb_wr_dyn();
    uint32_t hot = 0, memory = 0;
    int ret = !refuse && c ?
              cb_catalog_cues(c, cb_dbmsg_num(q, 1), &records, &times, &hot, &memory) :
              CB_ERROR(EROFS);
    if (records.err || times.err || ret == CB_ERROR(ENOMEM)) {
        cb_wr_free(&records);
        cb_wr_free(&times);
        return CB_ERROR(ENOMEM);
    }
    cues_message(w, q, ret >= 0, &records, &times, hot, memory);
    cb_wr_free(&records);
    cb_wr_free(&times);
    return 0;
}

/* ------------------------------------------------------- cues saved */

/* 1/150 s as ms, rounded up, as rekordbox converts a player's frames
 * (rbxport, from rekordbox 7.2.19's SavUsbCue). */
static uint32_t frames_ms(uint64_t frames)
{
    return (uint32_t)CB_MIN((frames * 1000 + 149) / 150, 0xfffffffeu);
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
static int cue_ext_parse(const uint8_t *p, uint32_t len, CBStickCue *c)
{
    CBRd r = cb_rd(p, len);
    unsigned slot = cb_rd_at_u16le(&r, 4), shape = cb_rd_at_u8(&r, 6);
    unsigned base = cb_rd_at_u16le(&r, 0xa);
    if (slot > CB_HOT_CUE_SLOTS || (shape != 1 && shape != 2) ||
        (base != 1000 && base != 150 && base != 75))
        return CB_ERROR(EINVAL);
    bool loop = shape == 2;
    uint32_t in = cb_rd_at_u32le(&r, 0xc), out = cb_rd_at_u32le(&r, 0x10);
    if (base != 1000) {
        uint64_t k = base == 75 ? 2 : 1;
        in = frames_ms(in * k);
        out = frames_ms(out * k + 1);
    }
    *c = (CBStickCue) {
        .hot    = slot ? (int)slot - 1 : -1,
        .in     = in,
        .out    = loop ? out : 0xffffffffu,
        .active = !slot && loop && (cb_rd_at_u16le(&r, 0x18) & 4),
    };
    uint32_t more = cb_rd_at_u32le(&r, 0x34);
    if (!more || len < 0x38 + (uint64_t)more)
        return 0;
    if (!slot && cb_rd_at_u8(&r, 0x3a) <= 8)
        c->code = cb_rd_at_u8(&r, 0x3a);
    uint32_t name_len = cb_rd_at_u16le(&r, 0x48);
    if (name_len && 0x4a + name_len <= len) {
        char *name = cb_link_utf16_to_utf8(p + 0x4a, name_len, false);
        if (!name)
            return CB_ERROR(ENOMEM);
        snprintf(c->name, sizeof(c->name), "%s", name);
        free(name);
    }
    uint32_t tail = 0x4a + name_len;
    if (slot && tail + 8 <= len) {
        uint32_t n = CB_MIN(cb_rd_at_u32le(&r, tail), 4u);
        uint8_t v[4] = { 0 };
        for (uint32_t i = 0; i < n; i++)
            v[i] = cb_rd_at_u8(&r, tail + 4 + i);
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
                         CBStickCue *c)
{
    CBRd r = cb_rd(p, len), m = cb_rd(ms, ms_len);
    uint32_t flags = cb_rd_at_u32le(&r, 0);
    if (flags >= 0x40000)
        return CB_ERROR(EINVAL);
    bool loop = flags & 1;
    unsigned slot = flags >> 16;
    uint32_t in = cb_rd_at_u32le(&m, 0), out = cb_rd_at_u32le(&m, 4);
    if (!in) {
        in = frames_ms(cb_rd_at_u32le(&r, 0xc));
        out = frames_ms((uint64_t)cb_rd_at_u32le(&r, 0x10) + 1);
    }
    *c = (CBStickCue) {
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
 * to the library's owner (cb_catalog_cue_edit), then answered with the
 * track's list as it is now, in the request's form (4e02 for 2705, 4702
 * for 2105), as rekordbox answers (rbxport, from rekordbox 7.2.19). A
 * record too short to be one is refused (0x32), and so is an older one
 * past C; an extended one rekordbox does not take changes nothing and
 * gets the list. Never the generic error: a CDJ-3000 that gets it for a
 * saved cue sends the cue again forever (rbxport, from CDJ-3000 firmware
 * 3.20). */
static int save_cue(CBDbSession *s, CBWr *w, const CBDbMsg *q, CBCatalog *c)
{
    bool ext = q->kind == CB_DB_SAVE_CUE_EXT;
    uint32_t len = 0, ms_len = 0;
    const uint8_t *rec = cb_dbmsg_blob(q, 4, &len);
    const uint8_t *ms = ext ? NULL : cb_dbmsg_blob(q, 6, &ms_len);
    len = CB_MIN(len, cb_dbmsg_num(q, 3));
    CBStickCue cue;
    int ret = CB_ERROR(EPROTO);
    if (c && rec && len >= (ext ? 0x38u : 0x24u))
        ret = ext ? cue_ext_parse(rec, len, &cue) : cue_old_parse(rec, len, ms, ms_len, &cue);
    if (ret == CB_ERROR(ENOMEM))
        return ret;
    bool refuse = ret < 0 && (ret != CB_ERROR(EINVAL) || !ext);
    if (ret >= 0) {
        /* The player by the request's r:m:s:t, else as it set up. */
        int player = (int)(cb_dbmsg_num(q, 0) >> 24);
        ret = cb_catalog_cue_edit(c, cb_dbmsg_num(q, 1), player ? player : s->player,
                                  !cb_dbmsg_num(q, 2), &cue);
        if (ret == CB_ERROR(ENOMEM))
            return ret;
        refuse = ret < 0;
    }
    if (!ext)
        return cues_reply(w, q, c, refuse);
    if (refuse) {
        blob_reply(w, q, CB_DB_CUES_EXT_REPLY, NULL, 0, true, 0);
        return 0;
    }
    return cues_ext_reply(w, q, c);
}

/* --------------------------------------------------------------- menus */

static Menu *menu_at(CBDbSession *s, int location, bool make)
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
    cb_catalog_menu_free(&old->m);
    *old = (Menu) { .used = true, .location = location };
    return old;
}

static int menu_request(CBDbSession *s, CBWr *w, const CBDbMsg *q, CBCatalog *c)
{
    CBCatMenu m;
    int ret = c ? cb_catalog_menu(c, q, &m) : CB_ERROR(ENOENT);
    if (ret == CB_ERROR(ENOENT)) {
        /* Not a menu: rekordbox's generic error, and the menus kept as
         * they were (rbxport, from rekordbox's binary). */
        error_reply(w, q);
        return 0;
    }
    if (ret < 0)
        return ret;
    Menu *slot = menu_at(s, cb_db_location(cb_dbmsg_num(q, 0)), true);
    cb_catalog_menu_free(&slot->m);
    slot->m = m;
    slot->age = ++s->clock;
    numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, m.nb);
    return 0;
}

static void render(CBDbSession *s, CBWr *w, const CBDbMsg *q, CBCatalog *c)
{
    Menu *slot = menu_at(s, cb_db_location(cb_dbmsg_num(q, 0)), false);
    uint32_t total = slot ? slot->m.nb : 0;
    uint32_t offset = CB_MIN(cb_dbmsg_num(q, 1), total);
    uint32_t count = CB_MIN(CB_MIN(cb_dbmsg_num(q, 2), total - offset), (uint32_t)RENDER_MAX);
    /* The column beside a title: one a CDJ-3000's render names (its
     * seventh and eighth arguments, both non-zero); a six-argument render,
     * the djl-analysis', shows the column of the sort the list was asked
     * with (track_metadata.adoc, "Alternate Track List Sort Orders"); else
     * the default. */
    int column = CB_CAT_COLUMN_DEFAULT;
    if (q->nb_args >= 8 && cb_dbmsg_num(q, 6) && cb_dbmsg_num(q, 7))
        column = (int)CB_MIN(cb_dbmsg_num(q, 7), 0xffu);
    else if (q->nb_args == 6 && slot)
        column = slot->m.sort;
    numbers(w, q->txid, CB_DB_MENU_HEADER, 1, offset);
    if (slot && c && count)
        cb_catalog_render(c, &slot->m, offset, count, column, s->extended, s->latin1, q->txid, w);
    cb_dbmsg_numbers(w, q->txid, CB_DB_MENU_FOOTER, NULL, 0);
}

/* ------------------------------------------------------------ dispatch */

static int handle(CBDbSession *s, const CBDbMsg *q, CBWr *w)
{
    CBCatalog *c = s->cat ? *s->cat : NULL;
    uint32_t a1 = cb_dbmsg_num(q, 1);
    switch (q->kind) {
    case CB_DB_SETUP:
        /* rekordbox answers a CDJ-3000's setup [device, 0x14] with its own
         * kind and [its number, 0x14] (rbxport's capture); the older setup
         * of one argument with 4000 [0, its number] (track_metadata.adoc
         * "Query context setup response"). */
        s->player = (int)cb_dbmsg_num(q, 0);
        s->extended = q->nb_args > 1;
        if (s->extended)
            numbers(w, q->txid, CB_DB_SETUP, (uint32_t)s->our_number, CB_DB_SETUP_MAGIC);
        else
            numbers(w, q->txid, CB_DB_AVAILABLE, 0, (uint32_t)s->our_number);
        return 0;
    case CB_DB_TEARDOWN:
        /* The server closes the connection; nothing is sent (beat-link,
         * Client.close). */
        return CB_ERROR(ESHUTDOWN);
    case CB_DB_CANCEL:
        /* A player gives up a request, a preview of the row it scrolled
         * past, by sending 0001 with that request's txid and no arguments;
         * a CDJ-2000nexus answers nothing to it (dysentery's S05, S06, S15a
         * and S20 captures: the request's own answer and no other). Each
         * request here is answered before the next is read, so there is
         * nothing left to give up. beat-link names it INVALID_DATA. A
         * CDJ-900 sends it after a preview too (2026-10-14), and was
         * answered with the unknown request's error until then. */
        return 0;
    case CB_DB_RENDER:
        render(s, w, q, c);
        return 0;
    case CB_DB_ITEM_POSITION: {
        /* Where an item is in the menu at the location: 4000 [3100, its
         * index] (rbxport's capture, 0 for the first). */
        Menu *slot = menu_at(s, cb_db_location(cb_dbmsg_num(q, 0)), false);
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind,
                slot ? cb_catalog_menu_find(&slot->m, a1) : CB_DB_ALL);
        return 0;
    }

    case CB_DB_ARTWORK:
        return artwork_reply(s, w, q, c);
    case CB_DB_PREVIEW:
        /* The track in the third argument (track_metadata.adoc; rbxport's
         * capture). */
        return analysis_reply(w, q, CB_DB_PREVIEW_REPLY, c, cb_catalog_preview,
                              cb_dbmsg_num(q, 2), false, 0);
    case CB_DB_DETAIL:
        return analysis_reply(w, q, CB_DB_DETAIL_REPLY, c, cb_catalog_detail, a1, false, 0);
    case CB_DB_GRID:
        /* A trailing 0, the grid's offset (rbxport's capture). */
        return analysis_reply(w, q, CB_DB_GRID_REPLY, c, cb_catalog_grid, a1, true, 0);
    case CB_DB_VBR:
        return analysis_reply(w, q, CB_DB_VBR_REPLY, c, cb_catalog_vbr, a1, false, 0);
    case CB_DB_TAG:
    case CB_DB_TAG_2EX:
        return tag_reply(w, q, c);
    case CB_DB_CUES_EXT:
        return cues_ext_reply(w, q, c);
    case CB_DB_CUES:
        return cues_reply(w, q, c, false);
    case CB_DB_USER_INFO: {
        /* [3006, 0, 160, the blob]: a CDJ-3000 waits for this after every
         * load from a rekordbox source before it asks for anything else
         * (rbxport, CDJ-3000 3.20). */
        static const uint8_t user[USER_INFO_LEN];
        blob_reply(w, q, CB_DB_USER_INFO_REPLY, user, sizeof(user), false, 0);
        return 0;
    }
    case CB_DB_FILTER:
        /* The CDJ-3000's first request: 4000 [3007, 0] (rbxport's capture). */
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, 0);
        return 0;

    /* Cues saved on the deck are kept, and ratings (below); the other
     * writes refused (docs/LINK.md). */
    case CB_DB_SAVE_CUE_EXT:
    case CB_DB_SAVE_CUE:
        return save_cue(s, w, q, c);
    case CB_DB_SAVE_RATING: {
        /* [r:m:s:t, track, 0 to 5 stars], kept as a saved cue is:
         * 4000 [kind, 0] when it was, 1 when not (rbxport, from rekordbox
         * 7.2.19). */
        uint32_t stars = cb_dbmsg_num(q, 2);
        int player = (int)(cb_dbmsg_num(q, 0) >> 24);
        int ret = !c ? CB_ERROR(ENOENT) : stars > 5 ? CB_ERROR(EINVAL) :
                  cb_catalog_rating_edit(c, a1, player ? player : s->player, (int)stars);
        if (ret == CB_ERROR(ENOMEM))
            return ret;
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, ret < 0);
        return 0;
    }
    case CB_DB_TAG_LIST_ADD: {
        /* [r:m:s:t, track, 1 to add it to the TAG LIST, 0 to take it off]:
         * 4000 [kind, 0] when done, 1 when not (rbxport, from rekordbox). */
        CBLinkTagList *tl = c ? cb_catalog_tag_list(c) : NULL;
        uint32_t add = cb_dbmsg_num(q, 2);
        int ret = !tl || !cb_catalog_serves(c, a1) || add > 1 ? -1 :
                  add ? cb_link_tag_list_add(tl, a1) : cb_link_tag_list_remove(tl, a1);
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, ret < 0);
        return 0;
    }
    case CB_DB_TAG_LIST_CLEAR: {
        CBLinkTagList *tl = c ? cb_catalog_tag_list(c) : NULL;
        if (tl)
            cb_link_tag_list_clear(tl);
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, !tl);
        return 0;
    }
    case CB_DB_SAVE_GRID_OFFSET:
        /* 4000 [kind, 1]: 0 says it was saved (rbxport). */
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, 1);
        return 0;
    case CB_DB_SAVE_ATOM:
    case CB_DB_UPDATE_ATOM:
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, CB_DB_STATUS_NONE);
        return 0;
    case CB_DB_HISTORY_REMOVE:
        numbers(w, q->txid, CB_DB_AVAILABLE, q->kind, CB_DB_ALL);
        return 0;
    case CB_DB_HISTORY_ADD:
    case CB_DB_HISTORY_DELETE:
    case CB_DB_ON_AIR:
    case CB_DB_SAVE_WAVEFORM:
    case CB_DB_SAVE_VBR:
    case 0x3203:
    case 0x3503:
        return 0;
    }
    return menu_request(s, w, q, c);
}

static int keep(CBDbSession *s, const uint8_t *p, size_t n)
{
    if (n > s->in_cap) {
        uint8_t *in = realloc(s->in, n);
        if (!in)
            return CB_ERROR(ENOMEM);
        s->in = in;
        s->in_cap = n;
    }
    if (n && p != s->in)
        memmove(s->in, p, n);
    s->in_len = n;
    return 0;
}

int cb_dbsession_input(CBDbSession *s, const uint8_t *buf, size_t len, CBWr *out)
{
    if (!len)
        return 0;
    const uint8_t *p = buf;
    size_t n = len;
    /* What is left of a message from the last call goes first. */
    if (s->in_len) {
        if (len > SIZE_MAX - s->in_len)
            return CB_ERROR(E2BIG);
        size_t have = s->in_len;
        if (have + len > s->in_cap) {
            uint8_t *in = realloc(s->in, have + len);
            if (!in)
                return CB_ERROR(ENOMEM);
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
            size_t have = CB_MIN(n - pos, (size_t)CB_DB_GREETING_LEN);
            if (memcmp(p + pos, cb_db_greeting, have)) {
                ret = CB_ERROR(EPROTO);
                break;
            }
            if (have < CB_DB_GREETING_LEN)
                break;
            cb_wr_bytes(out, cb_db_greeting, CB_DB_GREETING_LEN);
            s->greeted = true;
            pos += CB_DB_GREETING_LEN;
            continue;
        }
        CBDbMsg q;
        int r = cb_dbmsg_parse(p + pos, n - pos, REQUEST_MAX, &q);
        if (r == 0)
            break;
        if (r < 0) {
            cb_debug("link: player %d: a request that is not one (%s)", s->player, strerror(-r));
            ret = r;
            break;
        }
        pos += (size_t)r;
        if (cb_log_level() >= CB_LOG_DEBUG) {
            char args[160] = "";
            for (int i = 0, at = 0; i < q.nb_args && at < (int)sizeof(args) - 16; i++)
                at += snprintf(args + at, sizeof(args) - at, q.args[i].type == CB_DB_NUM ? " %x" :
                               q.args[i].type == CB_DB_STR ? " str" : " blob", q.args[i].num);
            cb_debug("link: player %d asks %04x:%s", s->player, q.kind, args);
        }
        size_t before = out->len;
        ret = handle(s, &q, out);
        cb_debug("link: answered %04x with %zu bytes%s", q.kind, out->len - before,
                 ret < 0 ? " and closes" : "");
    }
    if (ret < 0) {
        s->in_len = 0;
        return ret;
    }
    if (out->err)
        return CB_ERROR(ENOMEM);
    return keep(s, p + pos, n - pos);
}

/* -------------------------------------------------------------- the port */

/* The query on 12523: a four-byte length, 15, and "RemoteDBServer" with
 * its NUL; the answer is the port, two bytes big-endian
 * (track_metadata.adoc "Connecting to the Database"; rbxport's capture). */
static const uint8_t query[] = {
    0x00, 0x00, 0x00, 0x0f, 'R', 'e', 'm', 'o', 't', 'e', 'D', 'B', 'S', 'e', 'r', 'v', 'e', 'r', 0x00,
};

int cb_dbquery_parse(const uint8_t *buf, size_t len)
{
    size_t have = CB_MIN(len, sizeof(query));
    if (memcmp(buf, query, have))
        return CB_ERROR(EPROTO);
    return have < sizeof(query) ? 0 : (int)sizeof(query);
}

void cb_dbquery_reply(uint16_t port, uint8_t reply[2])
{
    reply[0] = (uint8_t)(port >> 8);
    reply[1] = (uint8_t)port;
}
