/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private interface of the core module, shared by its files and nothing
 * else: the SQLite layer of db.c (connection, cached statements,
 * transactions, error mapping), the column groups of the track table that
 * library.c binds and selects in the same order the statements were built
 * from, and a few helpers between files. Prefixed cb_db_/cb_core_ because
 * every module ends up in one binary. */

#ifndef CYANBOX_CORE_PRIV_H
#define CYANBOX_CORE_PRIV_H

#include "cyanbox/library.h"

#include <sqlite3.h>

/* ---------------------------------------------------------------- schema */

/* The track table's columns in two groups: the file facts and tags (set
 * by probing) and the editable state (set by cb_library_save_track). The
 * statements below are generated from these lists, and library.c binds
 * and reads them in this order. */
#define CB_TRACK_FILE_COLS 24
#define CB_TRACK_EDIT_COLS 32
extern const char *const cb_track_file_cols[CB_TRACK_FILE_COLS];
extern const char *const cb_track_edit_cols[CB_TRACK_EDIT_COLS];

/* Cached statements. Parameters are listed as ?N. */
enum CBStmtId {
    CB_STMT_TRACK_INSERT,       /* ?1 path ?2 added ?3 missing, file cols, edit cols */
    CB_STMT_TRACK_FILE,         /* ?1 id, file cols; clears missing */
    CB_STMT_TRACK_EDIT,         /* ?1 id, edit cols */
    CB_STMT_TRACK_DELETE,       /* ?1 id */
    CB_STMT_TRACK_MISSING,      /* ?1 id ?2 missing */
    CB_STMT_TRACK_PATH,         /* ?1 id ?2 path ?3 missing */
    CB_STMT_TRACK_PLAYED,       /* ?1 id ?2 play_count ?3 last_played */
    CB_STMT_PLAY_INSERT,        /* ?1 track_id ?2 played_at */
    CB_STMT_HISTORY_INSERT,     /* ?1 track_id ?2 played_at ?3 player */
    CB_STMT_HISTORY_DELETE,     /* ?1 track_id */
    CB_STMT_HISTORY_DELETE_ENTRY, /* ?1 id; returns its track_id */
    CB_STMT_SECTION_CLEAR,      /* ?1 track_id */
    CB_STMT_SECTION_INSERT,     /* ?1 track_id ?2 idx ?3 start ?4 bpm ?5 beat */
    CB_STMT_CUE_CLEAR,          /* ?1 track_id */
    CB_STMT_CUE_INSERT,         /* ?1 track_id ?2 time ?3 grid_beat ?4 hot ?5 loop_end
                                 * ?6 loop_beats ?7 active ?8 name ?9 color */
    CB_STMT_TAG_CLEAR,          /* ?1 track_id */
    CB_STMT_TAG_INSERT,         /* ?1 track_id ?2 position ?3 tag */
    CB_STMT_PREVIEW_PUT,        /* ?1 track_id ?2 data */
    CB_STMT_PREVIEW_GET,        /* ?1 track_id -> data */
    CB_STMT_DETAIL_PUT,         /* ?1 track_id ?2 data */
    CB_STMT_DETAIL_GET,         /* ?1 track_id -> data */
    CB_STMT_ART_FIND,           /* ?1 size ?2 jpeg -> id */
    CB_STMT_ART_PUT,            /* ?1 size ?2 jpeg ?3 width ?4 height */
    CB_STMT_ART_LINK,           /* ?1 track_id ?2 artwork_id */
    CB_STMT_ART_ID,             /* ?1 track_id -> artwork_id */
    CB_STMT_ART_GET,            /* ?1 track_id -> jpeg width height */
    CB_STMT_SETTING_GET,        /* ?1 key -> value */
    CB_STMT_SETTING_PUT,        /* ?1 key ?2 value */
    CB_STMT_SETTING_DELETE,     /* ?1 key */
    CB_STMT_SETTING_DELETE_RANGE, /* ?1 <= key < ?2 */
    CB_STMT_PLAYLIST_INSERT,    /* ?1 parent_id (NULL: root) ?2 name ?3 is_folder ?4 position */
    CB_STMT_PLAYLIST_RENAME,    /* ?1 id ?2 name */
    CB_STMT_PLAYLIST_DELETE,    /* ?1 id */
    CB_STMT_PLAYLIST_PLACE,     /* ?1 id ?2 parent_id ?3 position */
    CB_STMT_ENTRY_CLEAR,        /* ?1 playlist_id */
    CB_STMT_ENTRY_INSERT,       /* ?1 playlist_id ?2 position ?3 track_id */
    CB_STMT_NB,
};

struct CBDb {
    sqlite3      *db;
    char         *path;
    int           depth;        /* transaction nesting */
    int           bind_rc;      /* first failed bind since cb_db_stmt() */
    sqlite3_stmt *stmt[CB_STMT_NB];
};

/* Opens or creates the database and brings its schema to the current
 * version. */
int  cb_db_open(CBDb **db, const char *path);
void cb_db_close(CBDb *db);
/* Gives the free pages back to the file system when there are many
 * (incremental auto_vacuum); nothing when the library has not that mode. */
int  cb_db_trim(CBDb *db);

/* Maps an sqlite3 result code to a negative errno and logs it with
 * sqlite3_errmsg() and `what`. */
int  cb_db_error(CBDb *db, int rc, const char *what);
int  cb_db_exec(CBDb *db, const char *sql);

/* A cached statement, reset and with its bindings cleared. */
sqlite3_stmt *cb_db_stmt(CBDb *db, enum CBStmtId id);
/* A one-off statement (loading); finalize it when done. NULL on error. */
sqlite3_stmt *cb_db_prepare(CBDb *db, const char *sql);
/* Binders: NULL text and NAN doubles bind NULL. A failure is kept in
 * db->bind_rc and reported by the next cb_db_done()/cb_db_row(). */
void cb_db_bind_int(CBDb *db, sqlite3_stmt *st, int i, int64_t v);
void cb_db_bind_double(CBDb *db, sqlite3_stmt *st, int i, double v);
void cb_db_bind_text(CBDb *db, sqlite3_stmt *st, int i, const char *s);
void cb_db_bind_blob(CBDb *db, sqlite3_stmt *st, int i, const void *data, size_t size);
void cb_db_bind_null(CBDb *db, sqlite3_stmt *st, int i);
/* Runs a statement that returns no rows, then resets it. */
int  cb_db_done(CBDb *db, sqlite3_stmt *st, const char *what);
/* Steps a query: 1 with a row, 0 at the end (reset), or an error (reset). */
int  cb_db_row(CBDb *db, sqlite3_stmt *st, const char *what);
/* A column as a malloc'd string, NULL for SQL NULL; *err set on ENOMEM. */
char *cb_db_column_text(sqlite3_stmt *st, int col, int *err);

/* Transactions. The outermost is BEGIN IMMEDIATE ... COMMIT; inner ones
 * are savepoints, so a failing step inside a batch undoes only itself.
 * cb_db_commit rolls back on failure. cb_db_begin_read starts the
 * outermost one as a plain BEGIN: one snapshot for several queries
 * without holding the write lock (writes inside it may then fail with
 * -EBUSY when another connection committed since it began). */
int  cb_db_begin(CBDb *db);
int  cb_db_begin_read(CBDb *db);
int  cb_db_commit(CBDb *db);
void cb_db_rollback(CBDb *db);

/* -------------------------------------------------------------- library */

/* Settings <-> CBConfig (config.c), called by open. */
/* A change: the generation bumped and the record of changes told which
 * track's own state changed, or with id 0 that anything else did
 * (cb_library_take_changes). */
void cb_core_changed(CBLibrary *lib, uint64_t id);
int  cb_core_config_load(CBLibrary *lib);
/* Removes setting rows whose value their key does not take and keys
 * cyanbox does not know (not CBConfig's, not My Settings, not the
 * interface's ui.* or the sync's sync.*), counting them in *removed. */
int  cb_core_config_clean(CBLibrary *lib, int *removed);
/* What the library removes when it opens (clean.c): entries that are not
 * valid or not known. Never fails the open. */
int  cb_core_clean(CBLibrary *lib);
/* The playlist tree (playlist.c): load at open, free at close. When a
 * track is removed, drop_track renumbers the entries of the playlists that
 * held it, inside the caller's transaction, and forget_track removes it
 * from memory once that transaction has committed. */
int  cb_core_playlists_load(CBLibrary *lib);
void cb_core_playlists_free(CBLibrary *lib);
int  cb_core_playlists_drop_track(CBLibrary *lib, uint64_t id);
void cb_core_playlists_forget_track(CBLibrary *lib, uint64_t id);

/* The waveforms as raw bytes (library.c); wavestore.c packs. Put stores
 * the first `split` bytes as the preview and the rest as the detail, in
 * one transaction; get gives the preview, followed by the detail when
 * asked (-ENOENT when either is missing). _head reads only the preview's
 * first `size` bytes, without loading the rest: -ENOENT when the track has
 * none, -EINVAL when it is shorter. */
int  cb_core_waveform_blob_put(CBLibrary *lib, uint64_t id, const uint8_t *data, size_t size,
                               size_t split);
int  cb_core_waveform_blob_get(const CBLibrary *lib, uint64_t id, bool detail, uint8_t **data,
                               size_t *size);
int  cb_core_waveform_blob_head(const CBLibrary *lib, uint64_t id, uint8_t *head, size_t size);

/* Grid positions for notation.c: the beat index (cb_grid_time_to_beat
 * units) of "bar N.B", and the bar and number of an integer beat index.
 * The grid must be valid. */
double cb_core_grid_bar_index(const CBGrid *g, int64_t bar, int64_t beat);
void   cb_core_grid_describe(const CBGrid *g, int64_t grid_beat, int64_t *bar, int *beat);

/* The shortest "%.Ng" that strtod() reads back as exactly v. */
const char *cb_core_fmt_double(double v, char *buf, size_t size);

#endif /* CYANBOX_CORE_PRIV_H */
