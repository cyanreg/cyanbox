/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The library database: the SQLite connection, the schema, the cached
 * statements, transactions and error mapping.
 *
 * Schema version 1 (PRAGMA user_version = 1, application_id = 'CBOX').
 * Tables are STRICT so a wrong type is an error instead of a silent
 * conversion, and every row that belongs to a track or a playlist goes
 * with it (ON DELETE CASCADE, foreign keys on for every connection).
 *
 *   track           one row per audio file. `id` is AUTOINCREMENT so an id
 *                   is never handed out twice: it is the track id on every
 *                   stick, and players keep artwork and waveforms by it.
 *                   path      absolute, unique
 *                   mtime file_size codec container duration total_samples
 *                   sample_rate channels bit_rate bit_depth has_artwork
 *                             the file facts from the last probe
 *                   file_*    the tags as read from the file (title .. comment,
 *                             year, track_number, disc_number); file_comment
 *                             stays NULL unless the setting import.comment
 *                             asks for the file's comment
 *                   title .. comment, year, track_number, disc_number
 *                             the user's overrides; NULL = use the file's
 *                   rating hue            the user's (files have none); hue in
 *                             degrees, 0 <= hue < 360, NULL for no colour
 *                   key key_confidence beats_per_bar grid_confidence grid_flags
 *                   grid_locked analysed  analysis results and their state
 *                   beats_version key_version waves_version
 *                             the analyser version (CB_*_VERSION, cyanbox/analysis.h)
 *                             each result was made by; 0 older than any
 *                   tags_version ml_club ml_intensity ml_valence ml_vocals
 *                             the model's tagging (cyanbox/ml.h), asked for by
 *                             the user: CB_ML_TAGS_VERSION (0 never) and its
 *                             measures, 0 to 1, NULL when never tagged
 *                   ml_colors ml_legend
 *                             the probability the model gave each of the
 *                             eight track colours (8 little-endian float32
 *                             by colour id, NULL when it gave none) and the
 *                             hash of the legend it chose from (0 none)
 *                   added play_count last_played missing
 *   grid_section    track_id, idx (0-based order), start, bpm, beat, measured
 *                   (1: the section is one measured beat of a dynamic grid,
 *                   cyanbox/model.h)
 *   cue             track_id, time, grid_beat (NULL: absolute), hot (-1 memory,
 *                   0..7 = A..H), loop_end (NULL: not a loop), loop_beats (NULL:
 *                   absolute end), active, name, color
 *   track_tag       track_id, position, tag (My Tags, in order, unique per track)
 *   waveform_preview
 *                   track_id, data: the packed waveforms' header and previews
 *                   (cb_waveforms_pack, its first CB_WAVE_PACKED_PREVIEW
 *                   bytes), what the browser's rows draw
 *   waveform_detail track_id, data: the rest, the detail waveforms, which only
 *                   the players and the export read (a track has both or
 *                   neither)
 *   artwork         id, size (of jpeg, the key it is found by), jpeg, width,
 *                   height: a cover's thumbnail, once however many tracks
 *                   have it
 *   track_artwork   track_id, artwork_id: the track's cover. A cover goes
 *                   with its last track (the triggers artwork_unlinked and
 *                   artwork_relinked): the rows that name it are its count
 *   play            track_id, played_at (one row per play)
 *   deck_history    id (AUTOINCREMENT), track_id, played_at (epoch seconds:
 *                   when it started playing, or was added), player (the
 *                   deck's number on PRO DJ LINK, 0 added by hand): the
 *                   History, a row per track a deck played straight on for
 *                   1.5 minutes (src/app/link.c) and per one added to it
 *   playlist        id (AUTOINCREMENT), parent_id (NULL: root), name, is_folder,
 *                   position (dense among all children of the parent)
 *   playlist_entry  playlist_id, position (dense), track_id; a track at most
 *                   once per playlist
 *   setting         key, value (text; see config.c for the keys)
 *
 * A new library is made at this schema in one transaction. Nothing older
 * is converted: a database of another application id (rxbox's, 'RXBX',
 * among them) is not a library. One newer than this build is refused
 * rather than written with a schema we do not know, also when a newer
 * cyanbox makes it while this one opens it (the version is read again
 * under the write lock).
 *
 * Space: auto_vacuum is INCREMENTAL, set before a new library's first
 * table; cb_db_trim gives the free pages back to the file system when
 * there are many (at open), so a library shrinks when waveforms, covers
 * or tracks go.
 *
 * Journal: WAL, synchronous = NORMAL (a crash loses at most the last
 * transactions, never consistency), and a busy timeout because the export
 * job opens a second connection from its own thread. */

#include "core.h"

#include <math.h>
#include <stdio.h>

#define CB_DB_APPLICATION_ID 0x43424F58     /* 'CBOX' */

/* -------------------------------------------------------------- columns */

const char *const cb_track_file_cols[CB_TRACK_FILE_COLS] = {
    "mtime", "file_size", "codec", "container", "duration", "total_samples",
    "sample_rate", "channels", "bit_rate", "bit_depth", "has_artwork",
    "file_title", "file_artist", "file_album", "file_album_artist", "file_genre",
    "file_label", "file_remixer", "file_composer", "file_original_artist", "file_comment",
    "file_year", "file_track_number", "file_disc_number",
};

const char *const cb_track_edit_cols[CB_TRACK_EDIT_COLS] = {
    "title", "artist", "album", "album_artist", "genre",
    "label", "remixer", "composer", "original_artist", "comment",
    "year", "track_number", "disc_number",
    "rating", "hue", "key", "key_confidence", "beats_per_bar",
    "grid_confidence", "grid_flags", "grid_locked", "analysed",
    "beats_version", "key_version", "waves_version",
    "tags_version", "ml_club", "ml_intensity", "ml_valence", "ml_vocals",
    "ml_colors", "ml_legend",
};

/* --------------------------------------------------------------- schema */

#define CB_DB_SCHEMA 1

static const char schema[] =
    "CREATE TABLE track ("
    "  id                   INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  path                 TEXT NOT NULL UNIQUE,"
    "  mtime                INTEGER NOT NULL DEFAULT 0,"
    "  file_size            INTEGER NOT NULL DEFAULT 0,"
    "  codec                TEXT NOT NULL DEFAULT '',"
    "  container            TEXT NOT NULL DEFAULT '',"
    "  duration             REAL NOT NULL DEFAULT 0,"
    "  total_samples        INTEGER NOT NULL DEFAULT 0,"
    "  sample_rate          INTEGER NOT NULL DEFAULT 0,"
    "  channels             INTEGER NOT NULL DEFAULT 0,"
    "  bit_rate             INTEGER NOT NULL DEFAULT 0,"
    "  bit_depth            INTEGER NOT NULL DEFAULT 0,"
    "  has_artwork          INTEGER NOT NULL DEFAULT 0,"
    "  file_title           TEXT,"
    "  file_artist          TEXT,"
    "  file_album           TEXT,"
    "  file_album_artist    TEXT,"
    "  file_genre           TEXT,"
    "  file_label           TEXT,"
    "  file_remixer         TEXT,"
    "  file_composer        TEXT,"
    "  file_original_artist TEXT,"
    "  file_comment         TEXT,"
    "  file_year            INTEGER NOT NULL DEFAULT 0,"
    "  file_track_number    INTEGER NOT NULL DEFAULT 0,"
    "  file_disc_number     INTEGER NOT NULL DEFAULT 0,"
    "  title                TEXT,"
    "  artist               TEXT,"
    "  album                TEXT,"
    "  album_artist         TEXT,"
    "  genre                TEXT,"
    "  label                TEXT,"
    "  remixer              TEXT,"
    "  composer             TEXT,"
    "  original_artist      TEXT,"
    "  comment              TEXT,"
    "  year                 INTEGER,"
    "  track_number         INTEGER,"
    "  disc_number          INTEGER,"
    "  rating               INTEGER NOT NULL DEFAULT 0 CHECK (rating BETWEEN 0 AND 5),"
    "  hue                  REAL CHECK (hue >= 0 AND hue < 360),"
    "  key                  TEXT NOT NULL DEFAULT '',"
    "  key_confidence       REAL NOT NULL DEFAULT 0,"
    "  beats_per_bar        INTEGER NOT NULL DEFAULT 4 CHECK (beats_per_bar BETWEEN 1 AND 64),"
    "  grid_confidence      REAL NOT NULL DEFAULT 0,"
    "  grid_flags           INTEGER NOT NULL DEFAULT 0,"
    "  grid_locked          INTEGER NOT NULL DEFAULT 0,"
    "  analysed             INTEGER NOT NULL DEFAULT 0,"
    "  beats_version        INTEGER NOT NULL DEFAULT 0,"
    "  key_version          INTEGER NOT NULL DEFAULT 0,"
    "  waves_version        INTEGER NOT NULL DEFAULT 0,"
    "  tags_version         INTEGER NOT NULL DEFAULT 0,"
    "  ml_club              REAL,"
    "  ml_intensity         REAL,"
    "  ml_valence           REAL,"
    "  ml_vocals            REAL,"
    "  ml_colors            BLOB,"
    "  ml_legend            INTEGER NOT NULL DEFAULT 0,"
    "  added                INTEGER NOT NULL DEFAULT 0,"
    "  play_count           INTEGER NOT NULL DEFAULT 0,"
    "  last_played          INTEGER NOT NULL DEFAULT 0,"
    "  missing              INTEGER NOT NULL DEFAULT 0"
    ") STRICT;"

    "CREATE TABLE grid_section ("
    "  track_id INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
    "  idx      INTEGER NOT NULL,"
    "  start    REAL NOT NULL,"
    "  bpm      REAL NOT NULL CHECK (bpm > 0),"
    "  beat     INTEGER NOT NULL DEFAULT 0,"
    "  measured INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (track_id, idx)"
    ") STRICT, WITHOUT ROWID;"

    "CREATE TABLE cue ("
    "  id         INTEGER PRIMARY KEY,"
    "  track_id   INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
    "  time       REAL NOT NULL,"
    "  grid_beat  REAL,"
    "  hot        INTEGER NOT NULL CHECK (hot BETWEEN -1 AND 7),"
    "  loop_end   REAL,"
    "  loop_beats REAL,"
    "  active     INTEGER NOT NULL DEFAULT 0,"
    "  name       TEXT NOT NULL DEFAULT '',"
    "  color      TEXT NOT NULL DEFAULT ''"
    ") STRICT;"
    "CREATE INDEX cue_track ON cue(track_id);"

    "CREATE TABLE track_tag ("
    "  track_id INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
    "  position INTEGER NOT NULL,"
    "  tag      TEXT NOT NULL,"
    "  PRIMARY KEY (track_id, tag)"
    ") STRICT, WITHOUT ROWID;"

    "CREATE TABLE waveform_preview ("
    "  track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
    "  data     BLOB NOT NULL"
    ") STRICT;"
    "CREATE TABLE waveform_detail ("
    "  track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
    "  data     BLOB NOT NULL"
    ") STRICT;"

    "CREATE TABLE artwork ("
    "  id     INTEGER PRIMARY KEY,"
    "  size   INTEGER NOT NULL,"
    "  jpeg   BLOB NOT NULL,"
    "  width  INTEGER NOT NULL,"
    "  height INTEGER NOT NULL"
    ") STRICT;"
    "CREATE INDEX artwork_size ON artwork(size);"
    "CREATE TABLE track_artwork ("
    "  track_id   INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
    "  artwork_id INTEGER NOT NULL REFERENCES artwork(id)"
    ") STRICT;"
    "CREATE INDEX track_artwork_artwork ON track_artwork(artwork_id);"
    "CREATE TRIGGER artwork_unlinked AFTER DELETE ON track_artwork BEGIN"
    "  DELETE FROM artwork WHERE id = old.artwork_id AND"
    "    NOT EXISTS (SELECT 1 FROM track_artwork WHERE artwork_id = old.artwork_id);"
    "END;"
    "CREATE TRIGGER artwork_relinked AFTER UPDATE OF artwork_id ON track_artwork BEGIN"
    "  DELETE FROM artwork WHERE id = old.artwork_id AND"
    "    NOT EXISTS (SELECT 1 FROM track_artwork WHERE artwork_id = old.artwork_id);"
    "END;"

    "CREATE TABLE play ("
    "  track_id  INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
    "  played_at INTEGER NOT NULL"
    ") STRICT;"
    "CREATE INDEX play_track ON play(track_id);"
    "CREATE TABLE deck_history ("
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  track_id  INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
    "  played_at INTEGER NOT NULL,"
    "  player    INTEGER NOT NULL"
    ") STRICT;"
    "CREATE INDEX deck_history_track ON deck_history(track_id);"

    "CREATE TABLE playlist ("
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  parent_id INTEGER REFERENCES playlist(id) ON DELETE CASCADE,"
    "  name      TEXT NOT NULL,"
    "  is_folder INTEGER NOT NULL,"
    "  position  INTEGER NOT NULL"
    ") STRICT;"
    "CREATE INDEX playlist_parent ON playlist(parent_id, position);"

    "CREATE TABLE playlist_entry ("
    "  playlist_id INTEGER NOT NULL REFERENCES playlist(id) ON DELETE CASCADE,"
    "  position    INTEGER NOT NULL,"
    "  track_id    INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
    "  PRIMARY KEY (playlist_id, position),"
    "  UNIQUE (playlist_id, track_id)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE INDEX playlist_entry_track ON playlist_entry(track_id);"

    "CREATE TABLE setting ("
    "  key   TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ") STRICT, WITHOUT ROWID;";

/* ------------------------------------------------------------ statements */

static const char *const fixed_sql[CB_STMT_NB] = {
    [CB_STMT_TRACK_DELETE]    = "DELETE FROM track WHERE id = ?1",
    [CB_STMT_TRACK_MISSING]   = "UPDATE track SET missing = ?2 WHERE id = ?1",
    [CB_STMT_TRACK_PATH]      = "UPDATE track SET path = ?2, missing = ?3 WHERE id = ?1",
    [CB_STMT_TRACK_PLAYED]    = "UPDATE track SET play_count = ?2, last_played = ?3 WHERE id = ?1",
    [CB_STMT_PLAY_INSERT]     = "INSERT INTO play (track_id, played_at) VALUES (?1, ?2)",
    [CB_STMT_HISTORY_INSERT]  = "INSERT INTO deck_history (track_id, played_at, player) "
                                "VALUES (?1, ?2, ?3)",
    [CB_STMT_HISTORY_DELETE]  = "DELETE FROM deck_history WHERE track_id = ?1",
    [CB_STMT_HISTORY_DELETE_ENTRY] = "DELETE FROM deck_history WHERE id = ?1 RETURNING track_id",
    [CB_STMT_SECTION_CLEAR]   = "DELETE FROM grid_section WHERE track_id = ?1",
    [CB_STMT_SECTION_INSERT]  = "INSERT INTO grid_section (track_id, idx, start, bpm, beat, "
                                "measured) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    [CB_STMT_CUE_CLEAR]       = "DELETE FROM cue WHERE track_id = ?1",
    [CB_STMT_CUE_INSERT]      = "INSERT INTO cue (track_id, time, grid_beat, hot, loop_end, "
                                "loop_beats, active, name, color) "
                                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
    [CB_STMT_TAG_CLEAR]       = "DELETE FROM track_tag WHERE track_id = ?1",
    [CB_STMT_TAG_INSERT]      = "INSERT INTO track_tag (track_id, position, tag) VALUES (?1, ?2, ?3)",
    [CB_STMT_PREVIEW_PUT]     = "INSERT INTO waveform_preview (track_id, data) VALUES (?1, ?2) "
                                "ON CONFLICT (track_id) DO UPDATE SET data = excluded.data",
    [CB_STMT_PREVIEW_GET]     = "SELECT data FROM waveform_preview WHERE track_id = ?1",
    [CB_STMT_DETAIL_PUT]      = "INSERT INTO waveform_detail (track_id, data) VALUES (?1, ?2) "
                                "ON CONFLICT (track_id) DO UPDATE SET data = excluded.data",
    [CB_STMT_DETAIL_GET]      = "SELECT data FROM waveform_detail WHERE track_id = ?1",
    [CB_STMT_ART_FIND]        = "SELECT id FROM artwork WHERE size = ?1 AND jpeg = ?2",
    [CB_STMT_ART_PUT]         = "INSERT INTO artwork (size, jpeg, width, height) "
                                "VALUES (?1, ?2, ?3, ?4)",
    [CB_STMT_ART_LINK]        = "INSERT INTO track_artwork (track_id, artwork_id) VALUES (?1, ?2) "
                                "ON CONFLICT (track_id) DO UPDATE SET "
                                "artwork_id = excluded.artwork_id",
    [CB_STMT_ART_ID]          = "SELECT artwork_id FROM track_artwork WHERE track_id = ?1",
    [CB_STMT_ART_GET]         = "SELECT a.jpeg, a.width, a.height FROM track_artwork t "
                                "JOIN artwork a ON a.id = t.artwork_id WHERE t.track_id = ?1",
    [CB_STMT_SETTING_GET]     = "SELECT value FROM setting WHERE key = ?1",
    [CB_STMT_SETTING_PUT]     = "INSERT INTO setting (key, value) VALUES (?1, ?2) "
                                "ON CONFLICT (key) DO UPDATE SET value = excluded.value",
    [CB_STMT_SETTING_DELETE]  = "DELETE FROM setting WHERE key = ?1",
    [CB_STMT_SETTING_DELETE_RANGE] = "DELETE FROM setting WHERE key >= ?1 AND key < ?2",
    [CB_STMT_PLAYLIST_INSERT] = "INSERT INTO playlist (parent_id, name, is_folder, position) "
                                "VALUES (?1, ?2, ?3, ?4)",
    [CB_STMT_PLAYLIST_RENAME] = "UPDATE playlist SET name = ?2 WHERE id = ?1",
    [CB_STMT_PLAYLIST_DELETE] = "DELETE FROM playlist WHERE id = ?1",
    [CB_STMT_PLAYLIST_PLACE]  = "UPDATE playlist SET parent_id = ?2, position = ?3 WHERE id = ?1",
    [CB_STMT_ENTRY_CLEAR]     = "DELETE FROM playlist_entry WHERE playlist_id = ?1",
    [CB_STMT_ENTRY_INSERT]    = "INSERT INTO playlist_entry (playlist_id, position, track_id) "
                                "VALUES (?1, ?2, ?3)",
};

/* The track statements are generated from the column lists so the order
 * library.c binds in can never drift from the SQL. */
static char *track_sql(enum CBStmtId id)
{
    char sql[4096];
    size_t n = 0;
#define PUT(...) n += (size_t)snprintf(sql + n, n < sizeof(sql) ? sizeof(sql) - n : 0, __VA_ARGS__)

    if (id == CB_STMT_TRACK_INSERT) {
        PUT("INSERT INTO track (path, added, missing");
        for (int i = 0; i < CB_TRACK_FILE_COLS; i++)
            PUT(", %s", cb_track_file_cols[i]);
        for (int i = 0; i < CB_TRACK_EDIT_COLS; i++)
            PUT(", %s", cb_track_edit_cols[i]);
        PUT(") VALUES (?1, ?2, ?3");
        for (int i = 4; i <= 3 + CB_TRACK_FILE_COLS + CB_TRACK_EDIT_COLS; i++)
            PUT(", ?%d", i);
        PUT(")");
    } else {
        bool file = id == CB_STMT_TRACK_FILE;
        const char *const *cols = file ? cb_track_file_cols : cb_track_edit_cols;
        int nb = file ? CB_TRACK_FILE_COLS : CB_TRACK_EDIT_COLS;
        PUT("UPDATE track SET %s", file ? "missing = 0, " : "");
        for (int i = 0; i < nb; i++)
            PUT("%s%s = ?%d", i ? ", " : "", cols[i], i + 2);
        PUT(" WHERE id = ?1");
    }
#undef PUT
    return n < sizeof(sql) ? cb_strdup(sql) : NULL;
}

/* ---------------------------------------------------------------- errors */

int cb_db_error(CBDb *db, int rc, const char *what)
{
    int err;
    switch (rc & 0xff) {
    case SQLITE_NOMEM:      err = ENOMEM; break;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:     err = EBUSY;  break;
    case SQLITE_FULL:       err = ENOSPC; break;
    case SQLITE_READONLY:   err = EROFS;  break;
    case SQLITE_PERM:
    case SQLITE_AUTH:
    case SQLITE_CANTOPEN:   err = EACCES; break;
    case SQLITE_CONSTRAINT:
        err = rc == SQLITE_CONSTRAINT_UNIQUE || rc == SQLITE_CONSTRAINT_PRIMARYKEY ? EEXIST : EINVAL;
        break;
    case SQLITE_TOOBIG:
    case SQLITE_RANGE:
    case SQLITE_MISMATCH:   err = EINVAL; break;
    default:                err = EIO;    break;
    }
    cb_err("library database: %s: %s (%d)", what,
           db && db->db ? sqlite3_errmsg(db->db) : sqlite3_errstr(rc), rc);
    return CB_ERROR(err);
}

int cb_db_exec(CBDb *db, const char *sql)
{
    char *msg = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &msg);
    sqlite3_free(msg);
    return rc == SQLITE_OK ? 0 : cb_db_error(db, rc, sql);
}

/* -------------------------------------------------------------- binding */

sqlite3_stmt *cb_db_stmt(CBDb *db, enum CBStmtId id)
{
    sqlite3_stmt *st = db->stmt[id];
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    db->bind_rc = SQLITE_OK;
    return st;
}

sqlite3_stmt *cb_db_prepare(CBDb *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        cb_db_error(db, rc, sql);
        return NULL;
    }
    db->bind_rc = SQLITE_OK;
    return st;
}

static void bind_check(CBDb *db, int rc)
{
    if (rc != SQLITE_OK && db->bind_rc == SQLITE_OK)
        db->bind_rc = rc;
}

void cb_db_bind_int(CBDb *db, sqlite3_stmt *st, int i, int64_t v)
{
    bind_check(db, sqlite3_bind_int64(st, i, v));
}

void cb_db_bind_double(CBDb *db, sqlite3_stmt *st, int i, double v)
{
    bind_check(db, isnan(v) ? sqlite3_bind_null(st, i) : sqlite3_bind_double(st, i, v));
}

void cb_db_bind_text(CBDb *db, sqlite3_stmt *st, int i, const char *s)
{
    /* SQLITE_STATIC: every string outlives the step that reads it. */
    bind_check(db, s ? sqlite3_bind_text(st, i, s, -1, SQLITE_STATIC) : sqlite3_bind_null(st, i));
}

void cb_db_bind_blob(CBDb *db, sqlite3_stmt *st, int i, const void *data, size_t size)
{
    bind_check(db, sqlite3_bind_blob64(st, i, data ? data : "", size, SQLITE_STATIC));
}

void cb_db_bind_null(CBDb *db, sqlite3_stmt *st, int i)
{
    bind_check(db, sqlite3_bind_null(st, i));
}

int cb_db_done(CBDb *db, sqlite3_stmt *st, const char *what)
{
    int rc = db->bind_rc;
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(st);
        while (rc == SQLITE_ROW)
            rc = sqlite3_step(st);
    }
    sqlite3_reset(st);
    return rc == SQLITE_DONE ? 0 : cb_db_error(db, rc, what);
}

int cb_db_row(CBDb *db, sqlite3_stmt *st, const char *what)
{
    int rc = db->bind_rc;
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        return 1;
    sqlite3_reset(st);
    return rc == SQLITE_DONE ? 0 : cb_db_error(db, rc, what);
}

char *cb_db_column_text(sqlite3_stmt *st, int col, int *err)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    if (!s)
        return NULL;
    char *copy = cb_strdup((const char *)s);
    if (!copy)
        *err = CB_ERROR(ENOMEM);
    return copy;
}

/* --------------------------------------------------------- transactions */

/* SQLite rolls the whole transaction back by itself after some errors
 * (SQLITE_FULL or SQLITE_IOERR outside a statement journal, a write
 * interrupted as it starts; sqlite.org/lang_transaction.html), savepoints
 * and all. Nothing is open then although depth says otherwise, and a
 * SAVEPOINT would start a transaction of its own that its RELEASE commits:
 * the rest of a batch would be written behind the caller, who is about to
 * report the whole batch as failed. So nothing nests in a lost
 * transaction, and its commit fails (and says why, once). */
static bool transaction_lost(const CBDb *db)
{
    return db->depth > 0 && sqlite3_get_autocommit(db->db);
}

int cb_db_begin(CBDb *db)
{
    if (transaction_lost(db))
        return CB_ERROR(EIO);
    int ret = cb_db_exec(db, db->depth ? "SAVEPOINT cb" : "BEGIN IMMEDIATE");
    if (ret >= 0)
        db->depth++;
    return ret;
}

int cb_db_begin_read(CBDb *db)
{
    if (db->depth)
        return cb_db_begin(db);
    int ret = cb_db_exec(db, "BEGIN");
    if (ret >= 0)
        db->depth++;
    return ret;
}

void cb_db_rollback(CBDb *db)
{
    if (db->depth <= 0)
        return;
    if (!transaction_lost(db)) {
        if (db->depth > 1) {
            cb_db_exec(db, "ROLLBACK TO cb");
            cb_db_exec(db, "RELEASE cb");
        } else {
            cb_db_exec(db, "ROLLBACK");
        }
    }
    db->depth--;
}

int cb_db_commit(CBDb *db)
{
    if (db->depth <= 0)
        return CB_ERROR(EINVAL);
    if (transaction_lost(db)) {
        if (db->depth == 1)
            cb_err("library database: SQLite rolled the transaction back after an error; "
                   "nothing of it was saved");
        db->depth--;
        return CB_ERROR(EIO);
    }
    int ret = cb_db_exec(db, db->depth > 1 ? "RELEASE cb" : "COMMIT");
    if (ret < 0) {
        cb_db_rollback(db);
        return ret;
    }
    db->depth--;
    return 0;
}

/* ------------------------------------------------------------- open/close */

static int pragma_int(CBDb *db, const char *sql, int64_t *out)
{
    sqlite3_stmt *st = cb_db_prepare(db, sql);
    if (!st)
        return CB_ERROR(EIO);
    int ret = cb_db_row(db, st, sql);
    if (ret > 0)
        *out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ret < 0 ? ret : 0;
}

/* Refuses what is not ours to touch before anything is written to it (the
 * journal mode included): a database of something else, even one without
 * an application id, and one from a newer cyanbox. One statement, so one
 * snapshot: a library another process is creating right now is either
 * empty or whole here. *version: its schema. */
static int check_identity(CBDb *db, int64_t *version)
{
    static const char sql[] =
        "SELECT (SELECT application_id FROM pragma_application_id), "
        "(SELECT user_version FROM pragma_user_version), (SELECT count(*) FROM sqlite_schema)";
    sqlite3_stmt *st = cb_db_prepare(db, sql);
    if (!st)
        return CB_ERROR(EIO);
    int ret = cb_db_row(db, st, "reading the schema version");
    int64_t app_id = 0, tables = 0;
    *version = 0;
    if (ret > 0) {
        app_id = sqlite3_column_int64(st, 0);
        *version = sqlite3_column_int64(st, 1);
        tables = sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    if (ret < 0)
        return ret;

    if ((app_id && app_id != CB_DB_APPLICATION_ID) || (!app_id && !*version && tables)) {
        cb_err("%s is not a cyanbox library", db->path);
        return CB_ERROR(EINVAL);
    }
    if (*version > CB_DB_SCHEMA) {
        cb_err("%s was written by a newer cyanbox (schema %lld, this build knows %d); "
               "not touching it", db->path, (long long)*version, CB_DB_SCHEMA);
        return CB_ERROR(EPROTO);
    }
    return 0;
}

/* Makes a new library (version 0) in one transaction with the version
 * and the application id. The version is read again under the write
 * lock: another process may have made the library meanwhile (it is then
 * at this schema and there is nothing to do), or a newer cyanbox made it
 * at a schema past this build's. */
static int create(CBDb *db, int64_t version)
{
    if (version == CB_DB_SCHEMA)
        return 0;
    int ret = cb_db_begin(db);
    if (ret < 0)
        return ret;
    int64_t now = version;
    ret = pragma_int(db, "PRAGMA user_version", &now);
    if (ret >= 0 && now > CB_DB_SCHEMA) {
        cb_db_rollback(db);
        cb_err("%s was made by a newer cyanbox meanwhile (schema %lld, this build knows %d); "
               "not touching it", db->path, (long long)now, CB_DB_SCHEMA);
        return CB_ERROR(EPROTO);
    }
    if (ret >= 0 && !now) {
        char stamp[96];
        snprintf(stamp, sizeof(stamp), "PRAGMA user_version = %d; PRAGMA application_id = %d",
                 CB_DB_SCHEMA, CB_DB_APPLICATION_ID);
        ret = cb_db_exec(db, schema);
        if (ret >= 0)
            ret = cb_db_exec(db, stamp);
    }
    if (ret < 0) {
        cb_db_rollback(db);
        cb_err("%s: making schema %d failed", db->path, CB_DB_SCHEMA);
        return ret;
    }
    return cb_db_commit(db);
}

int cb_db_trim(CBDb *db)
{
    int64_t free_pages = 0, vacuum = 0;
    int ret = pragma_int(db, "PRAGMA freelist_count", &free_pages);
    if (ret >= 0)
        ret = pragma_int(db, "PRAGMA auto_vacuum", &vacuum);
    if (ret < 0 || vacuum != 2 || free_pages < 256)
        return ret;
    /* sqlite3_exec steps it to the end: each step frees one page. */
    ret = cb_db_exec(db, "PRAGMA incremental_vacuum");
    if (ret >= 0)
        cb_debug("%s: %lld free pages given back", db->path, (long long)free_pages);
    return ret;
}

/* WAL is a property of the file. Switching needs the database to itself:
 * two connections switching a new library at once deadlock on their read
 * locks and SQLite answers one of them SQLITE_BUSY at once, without
 * waiting. The other one switches it, and SQLite moves every connection
 * to WAL when it sees the file is, so that one goes on as it is. */
static int set_wal(CBDb *db)
{
    sqlite3_stmt *st = cb_db_prepare(db, "PRAGMA journal_mode = WAL");
    if (!st)
        return CB_ERROR(EIO);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW && sqlite3_stricmp((const char *)sqlite3_column_text(st, 0), "wal"))
        cb_warn("%s: WAL journal not available here (%s); using it as it is", db->path,
                (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE)
        return 0;
    if ((rc & 0xff) == SQLITE_BUSY) {
        cb_debug("%s: busy, not switched to WAL by this connection", db->path);
        return 0;
    }
    return cb_db_error(db, rc, "journal_mode");
}

int cb_db_open(CBDb **out, const char *path)
{
    *out = NULL;
    CBDb *db = calloc(1, sizeof(*db));
    if (!db)
        return CB_ERROR(ENOMEM);
    db->path = cb_strdup(path);
    if (!db->path) {
        free(db);
        return CB_ERROR(ENOMEM);
    }

    int ret;
    int rc = sqlite3_open_v2(path, &db->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        ret = cb_db_error(db, rc, path);
        goto fail;
    }
    sqlite3_extended_result_codes(db->db, 1);
    sqlite3_busy_timeout(db->db, 5000);

    int64_t version;
    ret = cb_db_exec(db, "PRAGMA foreign_keys = ON");
    if (ret >= 0)
        ret = check_identity(db, &version);
    /* Before the first table, and before WAL, which writes the header:
     * after either it no longer takes. */
    if (ret >= 0 && !version)
        ret = cb_db_exec(db, "PRAGMA auto_vacuum = INCREMENTAL");
    if (ret >= 0)
        ret = set_wal(db);
    if (ret >= 0)
        ret = cb_db_exec(db, "PRAGMA synchronous = NORMAL");
    if (ret >= 0)
        ret = create(db, version);
    if (ret < 0)
        goto fail;

    for (int i = 0; i < CB_STMT_NB; i++) {
        char *gen = NULL;
        const char *sql = fixed_sql[i];
        if (!sql) {
            gen = track_sql((enum CBStmtId)i);
            if (!gen) {
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
            sql = gen;
        }
        rc = sqlite3_prepare_v3(db->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &db->stmt[i], NULL);
        if (rc != SQLITE_OK)
            ret = cb_db_error(db, rc, sql);
        free(gen);
        if (ret < 0)
            goto fail;
    }

    *out = db;
    return 0;

fail:
    cb_db_close(db);
    return ret;
}

void cb_db_close(CBDb *db)
{
    if (!db)
        return;
    while (db->depth > 0)
        cb_db_rollback(db);
    for (int i = 0; i < CB_STMT_NB; i++)
        sqlite3_finalize(db->stmt[i]);
    if (db->db) {
        /* Folds the WAL back in so the library is one file at rest. */
        if (sqlite3_close(db->db) != SQLITE_OK) {
            cb_err("library database: %s", sqlite3_errmsg(db->db));
            sqlite3_close_v2(db->db);
        }
    }
    free(db->path);
    free(db);
}
