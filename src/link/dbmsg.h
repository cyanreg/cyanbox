/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The database server's messages: fields, messages and menu items, as pure
 * functions over bytes, for the server's sessions and for the client.
 *
 * A field is a tag byte and a value: 0f, 10, 11 a big-endian number of 1,
 * 2 or 4 bytes; 14 a blob (a 4-byte length, the bytes); 26 a string (a
 * 4-byte count of UTF-16 units including a NUL, the units big-endian)
 * (djl-analysis, track_metadata.adoc "Field Types"). A message is fields:
 * the magic 872349ae, a transaction id, the kind (2 bytes), the argument
 * count (1 byte), the argument tags as a blob (02 string, 03 blob, 06
 * number), then the arguments (track_metadata.adoc "Messages"). The tag
 * list is exactly `count` long in what rekordbox 7.2.11 and a CDJ-3000
 * send (rbxport's capture, rbl-dbserver tests/corpus.rs); clients written
 * from the djl-analysis send twelve, zero-padded, and players answer them
 * (rbxport, src/lib.rs), so the parser takes either and the writer writes
 * `count`.
 *
 * One rule makes the stream hard to frame: a blob is always preceded by a
 * number holding its length, and when that number is 0 the blob field is
 * declared but not sent at all (rekordbox's "no artwork" reply declares
 * four arguments and carries three; a player's waveform request declares
 * five and carries four: track_metadata.adoc "Waveform Previews", both
 * seen in rbxport's capture). The parser and the writer apply it the same
 * way beat-link's Message.read does.
 *
 * Nothing here allocates from a length a peer sent: a parsed message's
 * blobs and strings point into the bytes it was parsed from, and every
 * length is checked against what arrived and against a cap before it is
 * used. */

#ifndef CYANBOX_LINK_DBMSG_H
#define CYANBOX_LINK_DBMSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_priv.h"

#define CB_DB_MAGIC       0x872349aeu
/* The transaction id of setup and teardown (track_metadata.adoc). */
#define CB_DB_SETUP_TXID  0xfffffffeu
/* The second argument of rekordbox's and a CDJ-3000's setup, both ways;
 * meaning unknown (rbxport's capture). A setup without it is the older
 * form the djl-analysis describes (beat-link's), answered the older way. */
#define CB_DB_SETUP_MAGIC 0x14u
/* The most arguments a message declares. A menu item carries sixteen
 * (rbxport's capture); rbxport accepts tag lists up to 32. */
#define CB_DB_MAX_ARGS    32
/* Each side sends a number field holding 1 first on the database
 * connection, and the server answers with the same five bytes
 * (track_metadata.adoc "Connecting"; rbxport's capture). */
#define CB_DB_GREETING_LEN 5
extern const uint8_t cb_db_greeting[CB_DB_GREETING_LEN];
/* "ALL" and "nothing" in a request's id arguments (menus.adoc). */
#define CB_DB_ALL         0xffffffffu

/* The tag bytes in front of each field. */
enum CBDbField {
    CB_DB_FIELD_NUM1   = 0x0f,
    CB_DB_FIELD_NUM2   = 0x10,
    CB_DB_FIELD_NUM4   = 0x11,
    CB_DB_FIELD_BLOB   = 0x14,
    CB_DB_FIELD_STRING = 0x26,
};

/* The tags in a message's argument list. */
enum CBDbArgType {
    CB_DB_STR  = 0x02,
    CB_DB_BLOB = 0x03,
    CB_DB_NUM  = 0x06,
};

/* Message kinds. Requests from the djl-analysis (track_metadata.adoc,
 * menus.adoc) unless marked; the rest from rbxport (rbl-dbserver
 * src/lib.rs), measured against rekordbox 7.2.11 and CDJ-3000 firmware
 * 3.20, or read from rekordbox's binary. */
enum CBDbKind {
    CB_DB_SETUP              = 0x0000,
    CB_DB_CANCEL             = 0x0001,  /* a request given up, by its txid; no answer */
    CB_DB_TEARDOWN           = 0x0100,

    /* Menus: answered with CB_DB_AVAILABLE and a count, drawn by a render. */
    CB_DB_ROOT               = 0x1000,
    CB_DB_GENRES             = 0x1001,
    CB_DB_ARTISTS            = 0x1002,
    CB_DB_ALBUMS             = 0x1003,
    CB_DB_TRACKS             = 0x1004,
    CB_DB_BPMS               = 0x1006,
    CB_DB_RATINGS            = 0x1007,
    CB_DB_DECADES            = 0x1008,
    CB_DB_LABELS             = 0x100a,
    CB_DB_KEYS_2000          = 0x100b,  /* KEY from a CDJ-2000 (fw 4.33): its own table by
                                           the row's type, 0x8b (an emulated one, 2026-10-15) */
    CB_DB_COLORS             = 0x100d,
    CB_DB_TIMES              = 0x1010,
    CB_DB_BITRATES           = 0x1011,
    CB_DB_HISTORIES          = 0x1012,
    CB_DB_FILE_NAMES         = 0x1013,
    CB_DB_TAG_LIST           = 0x100f,  /* CMD_PREPARE_ROOT: the TAG LIST's tracks (rbxport) */
    CB_DB_KEYS               = 0x1014,
    CB_DB_GENRE_ARTISTS      = 0x1101,
    CB_DB_ARTIST_ALBUMS      = 0x1102,
    CB_DB_ALBUM_TRACKS       = 0x1103,
    CB_DB_PLAYLIST           = 0x1105,
    CB_DB_BPM_RANGES         = 0x1106,  /* rbxport */
    CB_DB_RATING_TRACKS      = 0x1107,
    CB_DB_DECADE_YEARS       = 0x1108,
    CB_DB_LABEL_ARTISTS      = 0x110a,
    CB_DB_COLOR_TRACKS       = 0x110d,
    CB_DB_TIME_TRACKS        = 0x1110,
    CB_DB_BITRATE_TRACKS     = 0x1111,  /* rbxport */
    CB_DB_HISTORY_TRACKS     = 0x1112,
    CB_DB_RELATED_KEYS       = 0x1114,
    CB_DB_GENRE_ARTIST_ALBUMS = 0x1201,
    CB_DB_ARTIST_ALBUM_TRACKS = 0x1202,
    CB_DB_BPM_TRACKS         = 0x1206,
    CB_DB_YEAR_TRACKS        = 0x1208,
    CB_DB_LABEL_ARTIST_ALBUMS = 0x120a,
    CB_DB_KEY_TRACKS         = 0x1214,
    CB_DB_KEY_TRACKS_2000    = 0x110b,  /* a CDJ-2000's: the key's tracks, no distances (it
                                           takes the related keys' rows for a leaf) */
    CB_DB_SEARCH             = 0x1300,
    CB_DB_GENRE_TRACKS       = 0x1301,
    CB_DB_ORIGINALS          = 0x1302,
    CB_DB_LABEL_TRACKS       = 0x130a,
    CB_DB_SORTS              = 0x1400,  /* rbxport */
    CB_DB_ORIGINAL_ALBUMS    = 0x1402,
    CB_DB_SEARCH_LIVE        = 0x1500,  /* the CDJ-3000's (rbxport) */
    CB_DB_ORIGINAL_TRACKS    = 0x1502,
    CB_DB_REMIXERS           = 0x1602,
    CB_DB_REMIXER_ALBUMS     = 0x1702,
    CB_DB_ADDED_YEARS        = 0x1708,  /* rbxport */
    CB_DB_REMIXER_TRACKS     = 0x1802,
    CB_DB_ADDED_MONTHS       = 0x1808,  /* rbxport */
    CB_DB_ADDED_DAYS         = 0x1908,  /* rbxport */
    CB_DB_ADDED_TRACKS       = 0x1a08,  /* rbxport, assumed by the pattern */
    CB_DB_METADATA           = 0x2002,
    CB_DB_TRACK_INFO         = 0x2102,  /* rbxport */
    CB_DB_DELIVERY           = 0x2602,  /* rbxport */

    /* One reply each. */
    CB_DB_ARTWORK            = 0x2003,
    CB_DB_PREVIEW            = 0x2004,
    CB_DB_CUES               = 0x2104,
    CB_DB_GRID               = 0x2204,
    CB_DB_VBR                = 0x2504,  /* rbxport */
    CB_DB_DETAIL             = 0x2904,
    CB_DB_CUES_EXT           = 0x2b04,
    CB_DB_TAG                = 0x2c04,
    CB_DB_TAG_2EX            = 0x2d04,  /* rbxport */
    CB_DB_RENDER             = 0x3000,
    CB_DB_USER_INFO          = 0x3006,  /* rbxport */
    CB_DB_FILTER             = 0x3007,  /* rbxport */
    CB_DB_ITEM_POSITION      = 0x3100,  /* rbxport */

    /* Writes from the deck (rbxport). */
    CB_DB_SAVE_WAVEFORM      = 0x2005,
    CB_DB_SAVE_CUE           = 0x2105,
    CB_DB_SAVE_RATING        = 0x2107,
    CB_DB_SAVE_VBR           = 0x2205,
    CB_DB_SAVE_GRID_OFFSET   = 0x2605,
    CB_DB_SAVE_CUE_EXT       = 0x2705,
    CB_DB_SAVE_ATOM          = 0x2805,
    CB_DB_UPDATE_ATOM        = 0x2905,
    CB_DB_HISTORY_ADD        = 0x3001,
    CB_DB_TAG_LIST_ADD       = 0x3002,  /* CMD_ADD_PREPARE (firmware): 1 add, 0 remove */
    CB_DB_HISTORY_DELETE     = 0x3101,
    CB_DB_ON_AIR             = 0x3201,
    CB_DB_TAG_LIST_CLEAR     = 0x3202,  /* CMD_DEL_TAGLIST_ALL */
    CB_DB_HISTORY_REMOVE     = 0x3401,

    /* Replies. */
    CB_DB_AVAILABLE          = 0x4000,
    CB_DB_MENU_HEADER        = 0x4001,
    CB_DB_ARTWORK_REPLY      = 0x4002,
    CB_DB_ERROR              = 0x4003,
    CB_DB_MENU_ITEM          = 0x4101,
    CB_DB_MENU_FOOTER        = 0x4201,
    CB_DB_PREVIEW_REPLY      = 0x4402,
    CB_DB_VBR_REPLY          = 0x4502,
    CB_DB_GRID_REPLY         = 0x4602,
    CB_DB_CUES_REPLY         = 0x4702,
    CB_DB_DETAIL_REPLY       = 0x4a02,
    CB_DB_USER_INFO_REPLY    = 0x4d02,
    CB_DB_CUES_EXT_REPLY     = 0x4e02,
    CB_DB_TAG_REPLY          = 0x4f02,
};

/* The status a blob reply carries in its second argument when there is
 * nothing to send: rekordbox's "no artwork" reply (rbxport's capture,
 * CAPTURED_NO_ARTWORK in rbl-dbserver tests/messages.rs). */
#define CB_DB_STATUS_NONE 0x32u

/* Menu item types, the seventh argument (track_metadata.adoc "Menu Item
 * Types"; the ones it lacks from rbxport's capture). A track row's type
 * is CB_DB_ITEM_TITLE with the type of the column shown beside the title
 * in its second byte: 0x0704 title and artist. */
enum CBDbItemType {
    CB_DB_ITEM_PATH          = 0x00,    /* track info's path row (rbxport) */
    CB_DB_ITEM_FOLDER        = 0x01,
    CB_DB_ITEM_ALBUM         = 0x02,
    CB_DB_ITEM_TITLE         = 0x04,
    CB_DB_ITEM_GENRE         = 0x06,
    CB_DB_ITEM_ARTIST        = 0x07,
    CB_DB_ITEM_PLAYLIST      = 0x08,
    CB_DB_ITEM_RATING        = 0x0a,
    CB_DB_ITEM_DURATION      = 0x0b,
    CB_DB_ITEM_TEMPO         = 0x0d,
    CB_DB_ITEM_LABEL         = 0x0e,
    CB_DB_ITEM_KEY           = 0x0f,
    CB_DB_ITEM_BIT_RATE      = 0x10,
    CB_DB_ITEM_YEAR          = 0x11,
    CB_DB_ITEM_FILE_TYPE     = 0x12,    /* delivery info (rbxport) */
    CB_DB_ITEM_COLOR_NONE    = 0x13,    /* 0x14 pink .. 0x1b purple */
    CB_DB_ITEM_COMMENT       = 0x23,
    CB_DB_ITEM_HISTORY       = 0x24,
    CB_DB_ITEM_ORIGINAL      = 0x28,
    CB_DB_ITEM_REMIXER       = 0x29,
    CB_DB_ITEM_PLAY_COUNT    = 0x2a,
    CB_DB_ITEM_DATE_ADDED    = 0x2e,
    CB_DB_ITEM_INFO_2F       = 0x2f,    /* track info, 1 for every file, meaning unknown */
    CB_DB_ITEM_DELIVERY_36   = 0x36,    /* delivery info rows rekordbox sends empty */
    CB_DB_ITEM_DELIVERY_37   = 0x37,
    CB_DB_ITEM_DELIVERY_ID   = 0x4f,
    CB_DB_ITEM_ALL           = 0xa0,
    CB_DB_ITEM_MENU_DEFAULT  = 0xa1,
    CB_DB_ITEM_MENU_ALPHABET = 0xa2,
};

/* Track row flags (rbxport's capture): a track a player loaded this
 * session; a row of an artist's, an album's or a playlist's tracks. */
#define CB_DB_TRACK_PLAYED 0x100u
#define CB_DB_TRACK_LISTED 0x1000000u

/* One argument. Parsed: a number's value, or a blob's bytes or a
 * string's UTF-16BE units with their NUL as they are on the wire, in the
 * buffer parsed. To write, a string is given as UTF-8 in `str`. */
typedef struct CBDbArg {
    uint8_t        type;            /* enum CBDbArgType */
    uint32_t       num;
    const uint8_t *p;
    uint32_t       len;             /* bytes */
    const char    *str;
} CBDbArg;

typedef struct CBDbMsg {
    uint32_t txid;
    uint16_t kind;
    int      nb_args;
    CBDbArg  args[CB_DB_MAX_ARGS];
} CBDbMsg;

/* One message from the start of `buf`: its length (consumed) when whole,
 * 0 when more bytes are needed, -EPROTO when the bytes cannot be one (a
 * wrong fixed byte, an unknown tag, a field whose tag is not the one
 * declared), -E2BIG when it would be longer than `max`. Bytes that cannot
 * start a message are refused as soon as they arrive, not after waiting
 * for more. */
int  cb_dbmsg_parse(const uint8_t *buf, size_t len, size_t max, CBDbMsg *m);
/* Appends the message; numbers as 4-byte fields, as rekordbox sends
 * them. A blob after a number 0 is left out (declared, not sent). */
void cb_dbmsg_write(CBWr *w, const CBDbMsg *m);
/* A message of numbers only. */
void cb_dbmsg_numbers(CBWr *w, uint32_t txid, uint16_t kind, const uint32_t *v, int nb);

/* Argument `i` as a number: 0 when absent or not a number. */
uint32_t cb_dbmsg_num(const CBDbMsg *m, int i);
/* Argument `i` as an allocated UTF-8 string; NULL when absent, not a
 * string, or out of memory. */
char    *cb_dbmsg_str(const CBDbMsg *m, int i);
/* Argument `i` as a blob: its bytes, NULL with *len 0 when absent. */
const uint8_t *cb_dbmsg_blob(const CBDbMsg *m, int i, uint32_t *len);

/* The first argument of most requests: the asking device, the menu
 * location (1 the main list, 2 the second, 3 a preview, 8 data), the slot
 * and the track type, a byte each (track_metadata.adoc, "DMST";
 * menus.adoc, "r:m:s:t"). A CDJ-3000 browsing a Link Export source names
 * slot 3, the USB slot, and track type 1 (rbxport's capture). */
static inline uint32_t cb_db_rmst(int device, int location, int slot, int type)
{
    return (uint32_t)(device & 0xff) << 24 | (uint32_t)(location & 0xff) << 16 |
           (uint32_t)(slot & 0xff) << 8 | (uint32_t)(type & 0xff);
}
static inline int cb_db_location(uint32_t rmst)
{
    return (int)(rmst >> 16 & 0xff);
}

/* --------------------------------------------------------------- items */

/* A menu item (4101), the sixteen arguments rekordbox 7.2.11 sends a
 * CDJ-3000 (rbxport's capture, rbl-dbserver src/item.rs); a session set
 * up the older way gets the first twelve, the layout the djl-analysis
 * documents (track_metadata.adoc "Menu item arguments"). Each text goes
 * after its length in bytes as UTF-16 with its NUL. NULL texts are "". */
typedef struct CBDbItem {
    uint32_t    a;                  /* a parent id, or the column's value in a track row */
    uint32_t    id;
    const char *text, *text2;
    uint32_t    type;               /* enum CBDbItemType, or a track row's */
    uint32_t    flags;
    uint32_t    c;                  /* a track row's id (artwork id in the older layout) */
    uint32_t    d;                  /* a position in a playlist or album */
    uint32_t    e;                  /* 0x100 in every track row, meaning unknown */
    uint32_t    key;                /* a track row's key, 1..24 round the wheel */
    uint32_t    art;                /* a track row's key id */
    const char *text3;              /* a track row's key name */
    uint32_t    f;                  /* a track row's tempo x100 */
} CBDbItem;

void cb_dbmsg_item(CBWr *w, uint32_t txid, const CBDbItem *it, bool extended);
/* The same with `latin1`: the texts as cb_utf8_latin1 makes them (romaji,
 * Latin-1 punctuation), for a CDJ-900, whose display turns what is not
 * Latin-1 into '?' (an emulated one, 2026-10-15: "Don’t" as "Don?t", kana
 * and kanji all '?'). */
void cb_dbmsg_item_latin1(CBWr *w, uint32_t txid, const CBDbItem *it, bool extended, bool latin1);

/* An item read back by a client: the same fields with its texts in
 * UTF-8, allocated. */
typedef struct CBDbRow {
    uint32_t a, id, type, flags, c, d, e, key, art, f;
    char    *text, *text2, *text3;
} CBDbRow;

/* A 4101 message into a row (twelve or sixteen arguments): 0, -EPROTO
 * when it is not one, -ENOMEM. */
int  cb_dbrow_parse(const CBDbMsg *m, CBDbRow *row);
void cb_dbrow_free(CBDbRow *row);
void cb_dbrows_free(CBDbRow **rows, int nb);

#endif /* CYANBOX_LINK_DBMSG_H */
