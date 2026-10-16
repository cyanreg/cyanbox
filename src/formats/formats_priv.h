/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What src/formats shares with src/export (inspect, pull, the plan) and
 * with the tests, beyond cyanbox/formats.h: text helpers (UTF-8/UTF-16, NFC,
 * zlib's CRC-32), the values both databases derive from the plan the same
 * way, and the lower-level readers inspect needs to check what the public
 * readers smooth over (raw track rows, cue entries as stored, the DeviceSQL
 * structural invariants). Nothing here is part of the module interface. */

#ifndef CYANBOX_FORMATS_PRIV_H
#define CYANBOX_FORMATS_PRIV_H

#include <sqlite3.h>

#include "cyanbox/formats.h"

/* ------------------------------------------------------------------ text */

/* UTF-8 itself and NFC: cyanbox/common.h (cb_utf8_next, cb_utf8_put,
 * cb_utf8_length, cb_utf8_nfc). */
/* UTF-16 code units of a UTF-8 string (surrogate pairs above the BMP). */
int      cb_utf8_to_utf16(const char *s, uint16_t **units, size_t *nb);
/* Allocating conversions from UTF-16 bytes; lone surrogates become U+FFFD. */
char    *cb_utf16le_to_utf8(const uint8_t *p, size_t bytes);
char    *cb_utf16be_to_utf8(const uint8_t *p, size_t bytes);
/* Invalid bytes replaced by U+FFFD, then Unicode NFC: dj's _clean(). */
static inline char *cb_utf8_clean_nfc(const char *s) { return cb_utf8_nfc(s); }
/* zlib's crc32(). */
uint32_t cb_crc32(uint32_t crc, const void *data, size_t size);

/* ----------------------------------------------------------- plan values */

/* One 32-bit constant per library (track row 0x18, OneLibrary masterDbId);
 * the plan sets it to dj's value, zlib.crc32 of the library root. */
uint32_t cb_plan_library_id(const CBPlan *plan);
/* Track row 0x14 / OneLibrary masterContentId: crc32 of the stick path, or
 * 1 when that is 0 (dj). */
uint32_t cb_plan_master_id(const CBPlanTrack *t);
/* "YYYY-MM-DD" of when the track was added to the library (CBTrack.added;
 * dj took the audio file's modification time, used here only for a track
 * without one). The export's own date is CBPlan.date, today. */
void     cb_plan_date_added(const CBPlanTrack *t, char buf[16]);
/* "/PIONEER/USBANLZ/Pxxx/xxxxxxxx/ANLZnnnn.DAT" (or the .EXT/.2EX name). */
char    *cb_plan_anlz_path(const CBPlanTrack *t, const char *ext);
/* "/PIONEER/Artwork/00001": folder id / 20 + 1 (docs/pdb.md 1.4). */
char    *cb_plan_artwork_dir(uint32_t art_id);
/* The track title as it goes in both databases: the tag, else the file
 * name without its extension (dj's track_from_probe). */
const char *cb_plan_title(const CBPlanTrack *t, char *buf, size_t size);
/* The device name as the export.pdb property row can hold it: printable
 * ASCII only, other characters replaced by '_', at most 32. */
void     cb_plan_ascii_label(const char *name, char *buf, size_t size);

/* The My Tag columns (dj's MY_TAG_COLUMNS): ids 1..4. */
extern const char *const cb_my_tag_columns[4];

/* ------------------------------------------------------------------- pdb */

#define CB_PDB_PAGE 4096
#define CB_PDB_HEAP 0x28

enum CBPdbTable {
    CB_PDB_TRACKS = 0, CB_PDB_GENRES, CB_PDB_ARTISTS, CB_PDB_ALBUMS, CB_PDB_LABELS,
    CB_PDB_KEYS, CB_PDB_COLORS, CB_PDB_PLAYLIST_TREE, CB_PDB_PLAYLIST_ENTRIES,
    CB_PDB_HISTORY_PLAYLISTS = 0x0B, CB_PDB_HISTORY_ENTRIES = 0x0C, CB_PDB_ARTWORK = 0x0D,
    CB_PDB_COLUMNS = 0x10, CB_PDB_MENU = 0x11, CB_PDB_SORT = 0x12, CB_PDB_HISTORY = 0x13,
    CB_PDB_NUM_TABLES = 20,
};
enum CBPdbExtTable {
    CB_PDB_EXT_TAGS = 3, CB_PDB_EXT_TAG_TRACKS = 4, CB_PDB_EXT_T7 = 7, CB_PDB_EXT_NUM_TABLES = 9,
};

typedef struct CBPdbTableDir { uint32_t type, empty_candidate, first_page, last_page; } CBPdbTableDir;
typedef struct CBPdbHeader {
    uint32_t len_page, num_tables, next_unused, unknown, sequence;
    CBPdbTableDir tables[64];
} CBPdbHeader;

int  cb_pdb_read_header(const uint8_t *b, size_t size, CBPdbHeader *h);
/* Every present row of a table, in page and slot order, each once (slots
 * of a page listing the same offset are one row): its file offset and the
 * end of its room, the next row of its page in the heap or, for the last,
 * the page's row index. What a row holds (its strings) lies in its room;
 * the readers read nothing of a row beyond it. */
typedef struct CBPdbRow { uint32_t pos, end; } CBPdbRow;
int  cb_pdb_table_rows(const uint8_t *b, size_t size, const CBPdbTableDir *t,
                       CBPdbRow **rows, int *nb);
/* Reads a DeviceSQL string at `pos` (bounds-checked against `size`). */
int  cb_pdb_read_string(const uint8_t *b, size_t size, size_t pos, char **utf8);

typedef void (*cb_problem_cb)(void *opaque, const char *problem);
/* dj's pdb.check(): the structural invariants players depend on. Reports
 * each problem and returns how many there were. */
int  cb_pdb_check(const uint8_t *b, size_t size, cb_problem_cb cb, void *opaque);

/* A track row as stored, for inspect's fixed-value checks. */
typedef struct CBPdbRawTrack {
    uint32_t pos;                /* file offset of the row */
    uint16_t subtype, unk56, magic, file_type, duration, sample_depth;
    uint32_t bitmask, id, tempo, sample_rate, bitrate, artwork_id, artist_id, key_id;
    uint32_t file_size;
    uint8_t  color_id, rating;
    uint16_t string_offsets[21];
    char    *strings[21];        /* isrc ... file_path, see pdb.c */
} CBPdbRawTrack;
enum {
    CB_PDB_STR_AUTOLOAD_HOTCUES = 7, CB_PDB_STR_DATE_ADDED = 10, CB_PDB_STR_ANALYZE_PATH = 14,
    CB_PDB_STR_ANALYZE_DATE = 15, CB_PDB_STR_COMMENT = 16, CB_PDB_STR_TITLE = 17,
    CB_PDB_STR_FILENAME = 19, CB_PDB_STR_FILE_PATH = 20,
};
int  cb_pdb_raw_tracks(const uint8_t *b, size_t size, CBPdbRawTrack **out, int *nb);
void cb_pdb_raw_tracks_free(CBPdbRawTrack *t, int nb);
/* The artwork table: id and path, for checking the files exist. */
int  cb_pdb_artwork(const uint8_t *b, size_t size, uint32_t **ids, char ***paths, int *nb);
/* The history (0x13) row: track count and date. */
int  cb_pdb_history(const uint8_t *b, size_t size, uint32_t *num_tracks, char **date);
/* The play history a player appended: history playlists (0x0B, u32 id
 * and a name such as "HISTORY 001") and their entries (0x0C, u32 track id,
 * u32 playlist id, u32 index; crate-digger rekordbox_pdb.ksy, docs/pdb.md).
 * Lists by id, entries in index order. */
typedef struct CBPdbHistoryList {
    uint32_t  id;
    char     *name;
    uint32_t *track_ids;
    int       nb;
} CBPdbHistoryList;
int  cb_pdb_history_lists(const uint8_t *b, size_t size, CBPdbHistoryList **out, int *nb);
void cb_pdb_history_lists_free(CBPdbHistoryList *l, int nb);
/* Bytes of a track row's allocation and content (the CDJ-350 crash rule,
 * docs/pdb.md G6), for the tests. */
int  cb_pdb_track_row_size(const CBPlanTrack *t, const CBPlan *plan, size_t *len, size_t *alloc);

/* ------------------------------------------------------------------ anlz */

/* A PCPT or PCP2 entry as stored (dj's read_cues dictionaries). */
typedef struct CBAnlzCueEntry {
    uint32_t hot, status;        /* status: PCPT only */
    uint16_t prev, next;         /* PCPT only */
    uint8_t  type;               /* 1 cue, 2 loop */
    uint32_t time, end;          /* ms; end 0xFFFFFFFF when not a loop */
    uint8_t  color_id;           /* PCP2: memory cue colour */
    uint16_t num, den;           /* PCP2: quantised loop length */
    char    *comment;            /* PCP2, UTF-8, NULL when none */
    bool     has_color;          /* PCP2 entry long enough for the colour bytes */
    uint8_t  color[4];           /* code, r, g, b */
} CBAnlzCueEntry;
/* Entries of one PCOB or PCO2 tag; `declared` receives the count the
 * header claims. */
int  cb_anlz_tag_cues(const CBAnlzTag *t, CBAnlzCueEntry **e, int *nb, int *declared);
void cb_anlz_cue_entries_free(CBAnlzCueEntry *e, int nb);
/* dj's ms(): floor(t * 1000 + 0.5), and its loop quantum (numerator,
 * denominator for loops a power of two beats long); shared with the
 * OneLibrary cue table so both libraries agree. */
uint32_t cb_anlz_ms(double t);
void cb_anlz_loop_quantum(const CBPlanCue *c, const CBGrid *grid, uint16_t *num, uint16_t *den);
/* PCOB list type and the memory_count word (docs/anlz.md 4.5). */
int  cb_anlz_pcob_header(const CBAnlzTag *t, uint32_t *list_type, uint32_t *count_word);

/* ------------------------------------------------------------ onelibrary */

/* Opens decrypted database bytes in memory (WAL flag cleared, as memory
 * databases cannot use WAL; dj's open_plain). */
int  cb_onelibrary_open_plain(const uint8_t *plain, size_t size, sqlite3 **db);
/* rekordbox 7.2.18's DDL statements, in creation order. */
extern const char *const cb_onelibrary_schema[];
extern const int cb_onelibrary_schema_count;
/* The fixed browse-menu rows (docs/onelibrary.md 3.4). */
typedef struct CBOneLibMenuItem { int id, kind; const char *name; } CBOneLibMenuItem;
extern const CBOneLibMenuItem cb_onelibrary_menu_items[27];
extern const int cb_onelibrary_category[22][4];
extern const int cb_onelibrary_sort[17][5];

/* ------------------------------------------------------------- sqlcipher */

#define CB_SQLCIPHER_PAGE    4096
#define CB_SQLCIPHER_RESERVE 80
/* The SQLite version rekordbox 7.2.18 stamps (docs/onelibrary.md 1.1). */
#define CB_SQLCIPHER_SQLITE_VERSION 3045003

#endif /* CYANBOX_FORMATS_PRIV_H */
