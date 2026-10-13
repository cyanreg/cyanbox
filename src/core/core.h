/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private interface of the core module, shared by its files and nothing
 * else: the SQLite layer of db.c (connection, cached statements,
 * transactions, error mapping), the column groups of the track table that
 * library.c binds and selects in the same order the statements were built
 * from, and a few helpers between files. Prefixed rx_db_/rx_core_ because
 * every module ends up in one binary. */

#ifndef RXBOX_CORE_PRIV_H
#define RXBOX_CORE_PRIV_H

#include "rxbox/library.h"

#include <sqlite3.h>

/* ---------------------------------------------------------------- schema */

/* The track table's columns in two groups: the file facts and tags (set
 * by probing) and the editable state (set by rx_library_save_track). The
 * statements below are generated from these lists, and library.c binds
 * and reads them in this order. */
#define RX_TRACK_FILE_COLS 24
#define RX_TRACK_EDIT_COLS 25
extern const char *const rx_track_file_cols[RX_TRACK_FILE_COLS];
extern const char *const rx_track_edit_cols[RX_TRACK_EDIT_COLS];

/* Cached statements. Parameters are listed as ?N. */
enum RXStmtId {
    RX_STMT_TRACK_INSERT,       /* ?1 path ?2 added ?3 missing, file cols, edit cols */
    RX_STMT_TRACK_FILE,         /* ?1 id, file cols; clears missing */
    RX_STMT_TRACK_EDIT,         /* ?1 id, edit cols */
    RX_STMT_TRACK_DELETE,       /* ?1 id */
    RX_STMT_TRACK_MISSING,      /* ?1 id ?2 missing */
    RX_STMT_TRACK_PATH,         /* ?1 id ?2 path ?3 missing */
    RX_STMT_TRACK_PLAYED,       /* ?1 id ?2 play_count ?3 last_played */
    RX_STMT_PLAY_INSERT,        /* ?1 track_id ?2 played_at */
    RX_STMT_SECTION_CLEAR,      /* ?1 track_id */
    RX_STMT_SECTION_INSERT,     /* ?1 track_id ?2 idx ?3 start ?4 bpm ?5 beat */
    RX_STMT_CUE_CLEAR,          /* ?1 track_id */
    RX_STMT_CUE_INSERT,         /* ?1 track_id ?2 time ?3 grid_beat ?4 hot ?5 loop_end
                                 * ?6 loop_beats ?7 active ?8 name ?9 color */
    RX_STMT_TAG_CLEAR,          /* ?1 track_id */
    RX_STMT_TAG_INSERT,         /* ?1 track_id ?2 position ?3 tag */
    RX_STMT_WAVE_PUT,           /* ?1 track_id ?2 data */
    RX_STMT_WAVE_GET,           /* ?1 track_id -> data */
    RX_STMT_ART_PUT,            /* ?1 track_id ?2 jpeg ?3 width ?4 height */
    RX_STMT_ART_GET,            /* ?1 track_id -> jpeg width height */
    RX_STMT_SETTING_GET,        /* ?1 key -> value */
    RX_STMT_SETTING_PUT,        /* ?1 key ?2 value */
    RX_STMT_SETTING_DELETE,     /* ?1 key */
    RX_STMT_SETTING_DELETE_RANGE, /* ?1 <= key < ?2 */
    RX_STMT_PLAYLIST_INSERT,    /* ?1 parent_id (NULL: root) ?2 name ?3 is_folder ?4 position */
    RX_STMT_PLAYLIST_RENAME,    /* ?1 id ?2 name */
    RX_STMT_PLAYLIST_DELETE,    /* ?1 id */
    RX_STMT_PLAYLIST_PLACE,     /* ?1 id ?2 parent_id ?3 position */
    RX_STMT_ENTRY_CLEAR,        /* ?1 playlist_id */
    RX_STMT_ENTRY_INSERT,       /* ?1 playlist_id ?2 position ?3 track_id */
    RX_STMT_NB,
};

struct RXDb {
    sqlite3      *db;
    char         *path;
    int           depth;        /* transaction nesting */
    int           bind_rc;      /* first failed bind since rx_db_stmt() */
    sqlite3_stmt *stmt[RX_STMT_NB];
};

/* Opens or creates the database and brings its schema to the current
 * version. */
int  rx_db_open(RXDb **db, const char *path);
void rx_db_close(RXDb *db);

/* Maps an sqlite3 result code to a negative errno and logs it with
 * sqlite3_errmsg() and `what`. */
int  rx_db_error(RXDb *db, int rc, const char *what);
int  rx_db_exec(RXDb *db, const char *sql);

/* A cached statement, reset and with its bindings cleared. */
sqlite3_stmt *rx_db_stmt(RXDb *db, enum RXStmtId id);
/* A one-off statement (loading); finalize it when done. NULL on error. */
sqlite3_stmt *rx_db_prepare(RXDb *db, const char *sql);
/* Binders: NULL text and NAN doubles bind NULL. A failure is kept in
 * db->bind_rc and reported by the next rx_db_done()/rx_db_row(). */
void rx_db_bind_int(RXDb *db, sqlite3_stmt *st, int i, int64_t v);
void rx_db_bind_double(RXDb *db, sqlite3_stmt *st, int i, double v);
void rx_db_bind_text(RXDb *db, sqlite3_stmt *st, int i, const char *s);
void rx_db_bind_blob(RXDb *db, sqlite3_stmt *st, int i, const void *data, size_t size);
void rx_db_bind_null(RXDb *db, sqlite3_stmt *st, int i);
/* Runs a statement that returns no rows, then resets it. */
int  rx_db_done(RXDb *db, sqlite3_stmt *st, const char *what);
/* Steps a query: 1 with a row, 0 at the end (reset), or an error (reset). */
int  rx_db_row(RXDb *db, sqlite3_stmt *st, const char *what);
/* A column as a malloc'd string, NULL for SQL NULL; *err set on ENOMEM. */
char *rx_db_column_text(sqlite3_stmt *st, int col, int *err);

/* Transactions. The outermost is BEGIN IMMEDIATE ... COMMIT; inner ones
 * are savepoints, so a failing step inside a batch undoes only itself.
 * rx_db_commit rolls back on failure. rx_db_begin_read starts the
 * outermost one as a plain BEGIN: one snapshot for several queries
 * without holding the write lock (writes inside it may then fail with
 * -EBUSY when another connection committed since it began). */
int  rx_db_begin(RXDb *db);
int  rx_db_begin_read(RXDb *db);
int  rx_db_commit(RXDb *db);
void rx_db_rollback(RXDb *db);

/* -------------------------------------------------------------- library */

/* Settings <-> RXConfig (config.c), called by open. Migrate runs after the
 * load's read transaction, in one of its own. */
int  rx_core_config_load(RXLibrary *lib);
int  rx_core_config_migrate(RXLibrary *lib);
/* The playlist tree (playlist.c): load at open, free at close. When a
 * track is removed, drop_track renumbers the entries of the playlists that
 * held it, inside the caller's transaction, and forget_track removes it
 * from memory once that transaction has committed. */
int  rx_core_playlists_load(RXLibrary *lib);
void rx_core_playlists_free(RXLibrary *lib);
int  rx_core_playlists_drop_track(RXLibrary *lib, uint64_t id);
void rx_core_playlists_forget_track(RXLibrary *lib, uint64_t id);

/* The waveform table as raw bytes (library.c); wavestore.c packs. _head
 * reads only the first `size` bytes, without loading the rest: -ENOENT
 * when the track has none, -EINVAL when the blob is shorter. */
int  rx_core_waveform_blob_put(RXLibrary *lib, uint64_t id, const uint8_t *data, size_t size);
int  rx_core_waveform_blob_get(const RXLibrary *lib, uint64_t id, uint8_t **data, size_t *size);
int  rx_core_waveform_blob_head(const RXLibrary *lib, uint64_t id, uint8_t *head, size_t size);

/* Grid positions for notation.c: the beat index (rx_grid_time_to_beat
 * units) of "bar N.B", and the bar and number of an integer beat index.
 * The grid must be valid. */
double rx_core_grid_bar_index(const RXGrid *g, int64_t bar, int64_t beat);
void   rx_core_grid_describe(const RXGrid *g, int64_t grid_beat, int64_t *bar, int *beat);

/* The shortest "%.Ng" that strtod() reads back as exactly v. */
const char *rx_core_fmt_double(double v, char *buf, size_t size);

#endif /* RXBOX_CORE_PRIV_H */
