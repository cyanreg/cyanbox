/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The library database: the SQLite connection, the schema and its
 * migrations, the cached statements, transactions and error mapping.
 *
 * Schema version 8 (PRAGMA user_version = 8, application_id = 'RXBX').
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
 *                             the analyser version (RX_*_VERSION, rxbox/analysis.h)
 *                             each result was made by; 0: before versions were
 *                             kept (schema 1), so older than any
 *                   tags_version ml_club ml_intensity ml_valence ml_vocals
 *                             the model's tagging (rxbox/ml.h), asked for by
 *                             the user: RX_ML_TAGS_VERSION (0 never) and its
 *                             measures, 0 to 1, NULL when never tagged
 *                   ml_colors ml_legend
 *                             the probability the model gave each of the
 *                             eight track colours (8 little-endian float32
 *                             by colour id, NULL when it gave none) and the
 *                             hash of the legend it chose from (0 none)
 *                   added play_count last_played missing
 *   grid_section    track_id, idx (0-based order), start, bpm, beat, measured
 *                   (1: the section is one measured beat of a dynamic grid,
 *                   rxbox/model.h)
 *   cue             track_id, time, grid_beat (NULL: absolute), hot (-1 memory,
 *                   0..7 = A..H), loop_end (NULL: not a loop), loop_beats (NULL:
 *                   absolute end), active, name, color
 *   track_tag       track_id, position, tag (My Tags, in order, unique per track)
 *   waveform        track_id, data (rx_waveforms_pack)
 *   artwork         id, size (of jpeg, the key it is found by), jpeg, width,
 *                   height: a cover's thumbnail, once however many tracks
 *                   have it
 *   track_artwork   track_id, artwork_id: the track's cover. A cover goes
 *                   with its last track (the triggers artwork_unlinked and
 *                   artwork_relinked): the rows that name it are its count
 *   play            track_id, played_at (one row per play)
 *   playlist        id (AUTOINCREMENT), parent_id (NULL: root), name, is_folder,
 *                   position (dense among all children of the parent)
 *   playlist_entry  playlist_id, position (dense), track_id; a track at most
 *                   once per playlist
 *   setting         key, value (text; see config.c for the keys)
 *
 * Migrations: `migrations[v]` takes a database from version v to v + 1, in
 * one transaction with the user_version bump. A database newer than this
 * build is refused rather than written with a schema we do not know, also
 * when a newer rxbox migrates it while this one is (the version is read
 * again under the write lock). A step that adds columns adds only those
 * the table lacks, and after migrating every column of every step must be
 * there; a missing one is added (check_columns): two branches defined a
 * schema 6 (below, the second became 7), and a library migrated by the
 * other is at a number whose columns are not this build's.
 *   1 -> 2  the three analyser version columns of track.
 *
 *   2 -> 3  the tables as they are; a grid's first section stopped
 *           extending back to the start of the track (src/core/grid.c), so
 *           the first section of every stored grid starts at its first beat
 *           at or after time 0 instead, numbered as that beat was: the
 *           grid keeps every beat it had.
 *
 *   3 -> 4  a track's colour became a hue: track.color (one of rekordbox's
 *           eight names) gives way to track.hue, each name its colour's
 *           own hue, so the stick gets the same colour as before. Cue
 *           colours are the players' palette still. Comments imported
 *           from files before are kept; only new imports leave them out.
 *
 *   4 -> 5  the model's tagging: tags_version and the four ml_* measures.
 *
 *   5 -> 6  grid_section.measured, for the beats of a dynamic grid. Every
 *           stored section is straight (0), so every grid is what it was.
 *   6 -> 7  the model picks the colour from a legend: ml_colors and
 *           ml_legend. A track tagged before keeps its hue and measures.
 *           (Its branch numbered this 5 -> 6: a library at that 6 has
 *           these columns and lacks measured, which check_columns adds.)
 *   7 -> 8  a cover is stored once: artwork, one row per track until
 *           then, becomes one row per distinct thumbnail (the same bytes)
 *           and track_artwork links each track to its own. An album's
 *           tracks have the same thumbnail (the same picture, made the
 *           same way), so an album is one row.
 *
 * Journal: WAL, synchronous = NORMAL (a crash loses at most the last
 * transactions, never consistency), and a busy timeout because the export
 * job opens a second connection from its own thread. */

#include "core.h"

#include <math.h>
#include <stdio.h>

/* For the track colours' RGB macros only: nothing of the formats module
 * is linked. */
#include "rxbox/colors.h"

#define RX_DB_APPLICATION_ID 0x52584258     /* 'RXBX' */

/* -------------------------------------------------------------- columns */

const char *const rx_track_file_cols[RX_TRACK_FILE_COLS] = {
    "mtime", "file_size", "codec", "container", "duration", "total_samples",
    "sample_rate", "channels", "bit_rate", "bit_depth", "has_artwork",
    "file_title", "file_artist", "file_album", "file_album_artist", "file_genre",
    "file_label", "file_remixer", "file_composer", "file_original_artist", "file_comment",
    "file_year", "file_track_number", "file_disc_number",
};

const char *const rx_track_edit_cols[RX_TRACK_EDIT_COLS] = {
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

static const char schema_v1[] =
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
    "  color                TEXT NOT NULL DEFAULT '',"
    "  key                  TEXT NOT NULL DEFAULT '',"
    "  key_confidence       REAL NOT NULL DEFAULT 0,"
    "  beats_per_bar        INTEGER NOT NULL DEFAULT 4 CHECK (beats_per_bar BETWEEN 1 AND 64),"
    "  grid_confidence      REAL NOT NULL DEFAULT 0,"
    "  grid_flags           INTEGER NOT NULL DEFAULT 0,"
    "  grid_locked          INTEGER NOT NULL DEFAULT 0,"
    "  added                INTEGER NOT NULL DEFAULT 0,"
    "  analysed             INTEGER NOT NULL DEFAULT 0,"
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

    "CREATE TABLE waveform ("
    "  track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
    "  data     BLOB NOT NULL"
    ") STRICT;"

    "CREATE TABLE artwork ("
    "  track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
    "  jpeg     BLOB NOT NULL,"
    "  width    INTEGER NOT NULL,"
    "  height   INTEGER NOT NULL"
    ") STRICT;"

    "CREATE TABLE play ("
    "  track_id  INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
    "  played_at INTEGER NOT NULL"
    ") STRICT;"
    "CREATE INDEX play_track ON play(track_id);"

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

/* A column a migration adds: only when its table lacks it (migrate), so a
 * library that has it already from another branch's migration goes on. */
typedef struct Column {
    const char *table, *name, *decl;
} Column;

/* The analyser versions. Rows from before them get 0, which counts as
 * older than any analyser that keeps a version: everything analysed by
 * schema 1's rxbox is analysed again for what it has. */
static const Column columns_v2[] = {
    { "track", "beats_version", "INTEGER NOT NULL DEFAULT 0" },
    { "track", "key_version",   "INTEGER NOT NULL DEFAULT 0" },
    { "track", "waves_version", "INTEGER NOT NULL DEFAULT 0" },
};

/* The first section's start goes back by k = floor(start / period) whole
 * periods (start - k * period, as grid.c computes a beat), and its beat
 * number (0 or out of range is read as 1) back by k. SQLite's % keeps the
 * dividend's sign, hence the + bpb. */
static const char schema_v3[] =
    "UPDATE grid_section AS g SET"
    "  start = g.start - m.k * (60.0 / g.bpm),"
    "  beat = (((CASE WHEN g.beat BETWEEN 1 AND m.bpb THEN g.beat ELSE 1 END) - 1 - m.k)"
    "          % m.bpb + m.bpb) % m.bpb + 1 "
    "FROM (SELECT s.track_id AS id, CAST(s.start / (60.0 / s.bpm) AS INTEGER) AS k,"
    "             t.beats_per_bar AS bpb"
    "      FROM grid_section AS s JOIN track AS t ON t.id = s.track_id"
    "      WHERE s.idx = 0 AND s.start > 0) AS m "
    "WHERE g.track_id = m.id AND g.idx = 0 AND m.k > 0;";

/* Each of rekordbox's eight colour names becomes its colour's hue: the HSV
 * hue of the RGB the interface drew it in (rxbox/colors.h), by the
 * standard formula, in the same order of operations as rx_rgb_hue in
 * src/formats/colors.c, which core does not link; tests/core checks the
 * two agree. A name the old column should never have held is no colour.
 * The old column goes (and this runs while it is there). */
#define SQL_INT_(x) #x
#define SQL_INT(x) SQL_INT_(x)
static const Column columns_v4[] = {
    { "track", "hue", "REAL CHECK (hue >= 0 AND hue < 360)" },
};
static const char schema_v4[] =
    "WITH palette(name, rgb) AS (VALUES"
    "  ('pink', "   SQL_INT(RX_TRACK_RGB_PINK)   "), ('red', "    SQL_INT(RX_TRACK_RGB_RED)    "),"
    "  ('orange', " SQL_INT(RX_TRACK_RGB_ORANGE) "), ('yellow', " SQL_INT(RX_TRACK_RGB_YELLOW) "),"
    "  ('green', "  SQL_INT(RX_TRACK_RGB_GREEN)  "), ('aqua', "   SQL_INT(RX_TRACK_RGB_AQUA)   "),"
    "  ('blue', "   SQL_INT(RX_TRACK_RGB_BLUE)   "), ('purple', " SQL_INT(RX_TRACK_RGB_PURPLE) ")),"
    "rgb(name, r, g, b) AS (SELECT name, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255 FROM palette),"
    "hsv(name, r, g, b, mx, d) AS (SELECT name, r, g, b, max(r, g, b), max(r, g, b) - min(r, g, b)"
    "                              FROM rgb),"
    "angle(name, h) AS (SELECT name, CASE"
    "                     WHEN d = 0 THEN NULL"
    "                     WHEN mx = r THEN 60.0 * ((g - b) * 1.0 / d)"
    "                     WHEN mx = g THEN 60.0 * ((b - r) * 1.0 / d + 2.0)"
    "                     ELSE 60.0 * ((r - g) * 1.0 / d + 4.0) END FROM hsv) "
    "UPDATE track SET hue = (SELECT CASE WHEN h < 0 THEN h + 360.0 ELSE h END FROM angle"
    "                        WHERE angle.name = lower(trim(track.color, ' ' || char(9)))) "
    "WHERE color != '';"
    "ALTER TABLE track DROP COLUMN color;";

/* The model's tagging (rxbox/ml.h): which version of it tagged the track,
 * 0 for never, and its four measures, NULL until it has. */
static const Column columns_v5[] = {
    { "track", "tags_version", "INTEGER NOT NULL DEFAULT 0" },
    { "track", "ml_club",      "REAL" },
    { "track", "ml_intensity", "REAL" },
    { "track", "ml_valence",   "REAL" },
    { "track", "ml_vocals",    "REAL" },
};

/* A dynamic grid's beats (rxbox/model.h): a section that is one measured
 * beat. The sections there were are all straight. */
static const Column columns_v6[] = {
    { "grid_section", "measured", "INTEGER NOT NULL DEFAULT 0" },
};

/* The colour the model picks from the legend (rxbox/ml.h): the
 * probability of each of the eight colours, NULL until a tagging gave
 * them, and the hash of the legend asked, 0 for none. */
static const Column columns_v7[] = {
    { "track", "ml_colors", "BLOB" },
    { "track", "ml_legend", "INTEGER NOT NULL DEFAULT 0" },
};

/* A cover once (artwork), each track linked to its own (track_artwork);
 * a cover no track has any more goes. Runs while artwork is still one row
 * per track. */
static const char schema_v8[] =
    "ALTER TABLE artwork RENAME TO artwork_v7;"
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
    "INSERT INTO artwork (size, jpeg, width, height)"
    "  SELECT length(jpeg), jpeg, width, height FROM artwork_v7"
    "  GROUP BY jpeg ORDER BY min(track_id);"
    "INSERT INTO track_artwork (track_id, artwork_id)"
    "  SELECT o.track_id, a.id FROM artwork_v7 o"
    "  JOIN artwork a ON a.size = length(o.jpeg) AND a.jpeg = o.jpeg;"
    "DROP TABLE artwork_v7;";

/* One step: its columns first, then its SQL, which runs only while
 * `table` has the column `while_column` when that is set. */
typedef struct Migration {
    const Column *add;
    int           nb_add;
    const char   *sql;
    const char   *table, *while_column;
} Migration;

#define COLUMNS(c) .add = (c), .nb_add = (int)RX_ARRAY_ELEMS(c)

static const Migration migrations[] = {
    { .sql = schema_v1 },                                       /* 0 -> 1 */
    { COLUMNS(columns_v2) },                                    /* 1 -> 2 */
    { .sql = schema_v3 },                                       /* 2 -> 3 */
    { COLUMNS(columns_v4), .sql = schema_v4,                    /* 3 -> 4 */
      .table = "track", .while_column = "color" },
    { COLUMNS(columns_v5) },                                    /* 4 -> 5 */
    { COLUMNS(columns_v6) },                                    /* 5 -> 6 */
    { COLUMNS(columns_v7) },                                    /* 6 -> 7 */
    { .sql = schema_v8, .table = "artwork", .while_column = "track_id" },  /* 7 -> 8 */
};

/* ------------------------------------------------------------ statements */

static const char *const fixed_sql[RX_STMT_NB] = {
    [RX_STMT_TRACK_DELETE]    = "DELETE FROM track WHERE id = ?1",
    [RX_STMT_TRACK_MISSING]   = "UPDATE track SET missing = ?2 WHERE id = ?1",
    [RX_STMT_TRACK_PATH]      = "UPDATE track SET path = ?2, missing = ?3 WHERE id = ?1",
    [RX_STMT_TRACK_PLAYED]    = "UPDATE track SET play_count = ?2, last_played = ?3 WHERE id = ?1",
    [RX_STMT_PLAY_INSERT]     = "INSERT INTO play (track_id, played_at) VALUES (?1, ?2)",
    [RX_STMT_SECTION_CLEAR]   = "DELETE FROM grid_section WHERE track_id = ?1",
    [RX_STMT_SECTION_INSERT]  = "INSERT INTO grid_section (track_id, idx, start, bpm, beat, "
                                "measured) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    [RX_STMT_CUE_CLEAR]       = "DELETE FROM cue WHERE track_id = ?1",
    [RX_STMT_CUE_INSERT]      = "INSERT INTO cue (track_id, time, grid_beat, hot, loop_end, "
                                "loop_beats, active, name, color) "
                                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
    [RX_STMT_TAG_CLEAR]       = "DELETE FROM track_tag WHERE track_id = ?1",
    [RX_STMT_TAG_INSERT]      = "INSERT INTO track_tag (track_id, position, tag) VALUES (?1, ?2, ?3)",
    [RX_STMT_WAVE_PUT]        = "INSERT INTO waveform (track_id, data) VALUES (?1, ?2) "
                                "ON CONFLICT (track_id) DO UPDATE SET data = excluded.data",
    [RX_STMT_WAVE_GET]        = "SELECT data FROM waveform WHERE track_id = ?1",
    [RX_STMT_ART_FIND]        = "SELECT id FROM artwork WHERE size = ?1 AND jpeg = ?2",
    [RX_STMT_ART_PUT]         = "INSERT INTO artwork (size, jpeg, width, height) "
                                "VALUES (?1, ?2, ?3, ?4)",
    [RX_STMT_ART_LINK]        = "INSERT INTO track_artwork (track_id, artwork_id) VALUES (?1, ?2) "
                                "ON CONFLICT (track_id) DO UPDATE SET "
                                "artwork_id = excluded.artwork_id",
    [RX_STMT_ART_ID]          = "SELECT artwork_id FROM track_artwork WHERE track_id = ?1",
    [RX_STMT_ART_GET]         = "SELECT a.jpeg, a.width, a.height FROM track_artwork t "
                                "JOIN artwork a ON a.id = t.artwork_id WHERE t.track_id = ?1",
    [RX_STMT_SETTING_GET]     = "SELECT value FROM setting WHERE key = ?1",
    [RX_STMT_SETTING_PUT]     = "INSERT INTO setting (key, value) VALUES (?1, ?2) "
                                "ON CONFLICT (key) DO UPDATE SET value = excluded.value",
    [RX_STMT_SETTING_DELETE]  = "DELETE FROM setting WHERE key = ?1",
    [RX_STMT_SETTING_DELETE_RANGE] = "DELETE FROM setting WHERE key >= ?1 AND key < ?2",
    [RX_STMT_PLAYLIST_INSERT] = "INSERT INTO playlist (parent_id, name, is_folder, position) "
                                "VALUES (?1, ?2, ?3, ?4)",
    [RX_STMT_PLAYLIST_RENAME] = "UPDATE playlist SET name = ?2 WHERE id = ?1",
    [RX_STMT_PLAYLIST_DELETE] = "DELETE FROM playlist WHERE id = ?1",
    [RX_STMT_PLAYLIST_PLACE]  = "UPDATE playlist SET parent_id = ?2, position = ?3 WHERE id = ?1",
    [RX_STMT_ENTRY_CLEAR]     = "DELETE FROM playlist_entry WHERE playlist_id = ?1",
    [RX_STMT_ENTRY_INSERT]    = "INSERT INTO playlist_entry (playlist_id, position, track_id) "
                                "VALUES (?1, ?2, ?3)",
};

/* The track statements are generated from the column lists so the order
 * library.c binds in can never drift from the SQL. */
static char *track_sql(enum RXStmtId id)
{
    char sql[4096];
    size_t n = 0;
#define PUT(...) n += (size_t)snprintf(sql + n, n < sizeof(sql) ? sizeof(sql) - n : 0, __VA_ARGS__)

    if (id == RX_STMT_TRACK_INSERT) {
        PUT("INSERT INTO track (path, added, missing");
        for (int i = 0; i < RX_TRACK_FILE_COLS; i++)
            PUT(", %s", rx_track_file_cols[i]);
        for (int i = 0; i < RX_TRACK_EDIT_COLS; i++)
            PUT(", %s", rx_track_edit_cols[i]);
        PUT(") VALUES (?1, ?2, ?3");
        for (int i = 4; i <= 3 + RX_TRACK_FILE_COLS + RX_TRACK_EDIT_COLS; i++)
            PUT(", ?%d", i);
        PUT(")");
    } else {
        bool file = id == RX_STMT_TRACK_FILE;
        const char *const *cols = file ? rx_track_file_cols : rx_track_edit_cols;
        int nb = file ? RX_TRACK_FILE_COLS : RX_TRACK_EDIT_COLS;
        PUT("UPDATE track SET %s", file ? "missing = 0, " : "");
        for (int i = 0; i < nb; i++)
            PUT("%s%s = ?%d", i ? ", " : "", cols[i], i + 2);
        PUT(" WHERE id = ?1");
    }
#undef PUT
    return n < sizeof(sql) ? rx_strdup(sql) : NULL;
}

/* ---------------------------------------------------------------- errors */

int rx_db_error(RXDb *db, int rc, const char *what)
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
    rx_err("library database: %s: %s (%d)", what,
           db && db->db ? sqlite3_errmsg(db->db) : sqlite3_errstr(rc), rc);
    return RX_ERROR(err);
}

int rx_db_exec(RXDb *db, const char *sql)
{
    char *msg = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &msg);
    sqlite3_free(msg);
    return rc == SQLITE_OK ? 0 : rx_db_error(db, rc, sql);
}

/* -------------------------------------------------------------- binding */

sqlite3_stmt *rx_db_stmt(RXDb *db, enum RXStmtId id)
{
    sqlite3_stmt *st = db->stmt[id];
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    db->bind_rc = SQLITE_OK;
    return st;
}

sqlite3_stmt *rx_db_prepare(RXDb *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        rx_db_error(db, rc, sql);
        return NULL;
    }
    db->bind_rc = SQLITE_OK;
    return st;
}

static void bind_check(RXDb *db, int rc)
{
    if (rc != SQLITE_OK && db->bind_rc == SQLITE_OK)
        db->bind_rc = rc;
}

void rx_db_bind_int(RXDb *db, sqlite3_stmt *st, int i, int64_t v)
{
    bind_check(db, sqlite3_bind_int64(st, i, v));
}

void rx_db_bind_double(RXDb *db, sqlite3_stmt *st, int i, double v)
{
    bind_check(db, isnan(v) ? sqlite3_bind_null(st, i) : sqlite3_bind_double(st, i, v));
}

void rx_db_bind_text(RXDb *db, sqlite3_stmt *st, int i, const char *s)
{
    /* SQLITE_STATIC: every string outlives the step that reads it. */
    bind_check(db, s ? sqlite3_bind_text(st, i, s, -1, SQLITE_STATIC) : sqlite3_bind_null(st, i));
}

void rx_db_bind_blob(RXDb *db, sqlite3_stmt *st, int i, const void *data, size_t size)
{
    bind_check(db, sqlite3_bind_blob64(st, i, data ? data : "", size, SQLITE_STATIC));
}

void rx_db_bind_null(RXDb *db, sqlite3_stmt *st, int i)
{
    bind_check(db, sqlite3_bind_null(st, i));
}

int rx_db_done(RXDb *db, sqlite3_stmt *st, const char *what)
{
    int rc = db->bind_rc;
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(st);
        while (rc == SQLITE_ROW)
            rc = sqlite3_step(st);
    }
    sqlite3_reset(st);
    return rc == SQLITE_DONE ? 0 : rx_db_error(db, rc, what);
}

int rx_db_row(RXDb *db, sqlite3_stmt *st, const char *what)
{
    int rc = db->bind_rc;
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        return 1;
    sqlite3_reset(st);
    return rc == SQLITE_DONE ? 0 : rx_db_error(db, rc, what);
}

char *rx_db_column_text(sqlite3_stmt *st, int col, int *err)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    if (!s)
        return NULL;
    char *copy = rx_strdup((const char *)s);
    if (!copy)
        *err = RX_ERROR(ENOMEM);
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
static bool transaction_lost(const RXDb *db)
{
    return db->depth > 0 && sqlite3_get_autocommit(db->db);
}

int rx_db_begin(RXDb *db)
{
    if (transaction_lost(db))
        return RX_ERROR(EIO);
    int ret = rx_db_exec(db, db->depth ? "SAVEPOINT rx" : "BEGIN IMMEDIATE");
    if (ret >= 0)
        db->depth++;
    return ret;
}

int rx_db_begin_read(RXDb *db)
{
    if (db->depth)
        return rx_db_begin(db);
    int ret = rx_db_exec(db, "BEGIN");
    if (ret >= 0)
        db->depth++;
    return ret;
}

void rx_db_rollback(RXDb *db)
{
    if (db->depth <= 0)
        return;
    if (!transaction_lost(db)) {
        if (db->depth > 1) {
            rx_db_exec(db, "ROLLBACK TO rx");
            rx_db_exec(db, "RELEASE rx");
        } else {
            rx_db_exec(db, "ROLLBACK");
        }
    }
    db->depth--;
}

int rx_db_commit(RXDb *db)
{
    if (db->depth <= 0)
        return RX_ERROR(EINVAL);
    if (transaction_lost(db)) {
        if (db->depth == 1)
            rx_err("library database: SQLite rolled the transaction back after an error; "
                   "nothing of it was saved");
        db->depth--;
        return RX_ERROR(EIO);
    }
    int ret = rx_db_exec(db, db->depth > 1 ? "RELEASE rx" : "COMMIT");
    if (ret < 0) {
        rx_db_rollback(db);
        return ret;
    }
    db->depth--;
    return 0;
}

/* ------------------------------------------------------------- open/close */

static int pragma_int(RXDb *db, const char *sql, int64_t *out)
{
    sqlite3_stmt *st = rx_db_prepare(db, sql);
    if (!st)
        return RX_ERROR(EIO);
    int ret = rx_db_row(db, st, sql);
    if (ret > 0)
        *out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ret < 0 ? ret : 0;
}

/* Refuses what is not ours to touch before anything is written to it (the
 * journal mode included): a database of something else, even one without
 * an application id, and one from a newer rxbox. One statement, so one
 * snapshot: a library another process is creating right now is either
 * empty or whole here. *version: its schema. */
static int check_identity(RXDb *db, int64_t *version)
{
    static const char sql[] =
        "SELECT (SELECT application_id FROM pragma_application_id), "
        "(SELECT user_version FROM pragma_user_version), (SELECT count(*) FROM sqlite_schema)";
    sqlite3_stmt *st = rx_db_prepare(db, sql);
    if (!st)
        return RX_ERROR(EIO);
    int ret = rx_db_row(db, st, "reading the schema version");
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

    if ((app_id && app_id != RX_DB_APPLICATION_ID) || (!app_id && !*version && tables)) {
        rx_err("%s is not an rxbox library", db->path);
        return RX_ERROR(EINVAL);
    }
    int64_t current = (int64_t)RX_ARRAY_ELEMS(migrations);
    if (*version > current) {
        rx_err("%s was written by a newer rxbox (schema %lld, this build knows %lld); "
               "not touching it", db->path, (long long)*version, (long long)current);
        return RX_ERROR(EPROTO);
    }
    return 0;
}

static int has_column(RXDb *db, const char *table, const char *column, bool *has)
{
    static const char sql[] = "SELECT count(*) FROM pragma_table_info(?1) WHERE name = ?2";
    sqlite3_stmt *st = rx_db_prepare(db, sql);
    if (!st)
        return RX_ERROR(EIO);
    rx_db_bind_text(db, st, 1, table);
    rx_db_bind_text(db, st, 2, column);
    int ret = rx_db_row(db, st, sql);
    *has = ret > 0 && sqlite3_column_int64(st, 0) > 0;
    sqlite3_finalize(st);
    return ret < 0 ? ret : 0;
}

/* Adds the columns the table lacks; *added counts them. */
static int add_columns(RXDb *db, const Column *c, int nb, int *added)
{
    for (int i = 0; i < nb; i++) {
        bool has;
        int ret = has_column(db, c[i].table, c[i].name, &has);
        if (ret < 0)
            return ret;
        if (has)
            continue;
        char sql[256];
        snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s", c[i].table, c[i].name, c[i].decl);
        if ((ret = rx_db_exec(db, sql)) < 0)
            return ret;
        if (added)
            (*added)++;
    }
    return 0;
}

static int run_migration(RXDb *db, const Migration *m)
{
    int ret = add_columns(db, m->add, m->nb_add, NULL);
    if (ret < 0 || !m->sql)
        return ret;
    bool run = true;
    if (m->while_column && (ret = has_column(db, m->table, m->while_column, &run)) < 0)
        return ret;
    return run ? rx_db_exec(db, m->sql) : 0;
}

/* The columns every migration adds, after migrating. Two branches each
 * defined a schema 6 (the measured beats, the colour legend; the second
 * became 7 at the merge): a library migrated by the other one is at a
 * version whose columns are not this build's, and a step it skipped
 * left its columns out. They are added here (each is plain data with a
 * default), and then must all be there. */
static int check_columns(RXDb *db)
{
    int missing = 0;
    for (size_t m = 0; m < RX_ARRAY_ELEMS(migrations); m++) {
        for (int i = 0; i < migrations[m].nb_add; i++) {
            bool has;
            int ret = has_column(db, migrations[m].add[i].table, migrations[m].add[i].name, &has);
            if (ret < 0)
                return ret;
            missing += !has;
        }
    }
    if (!missing)
        return 0;

    int added = 0;
    int ret = rx_db_begin(db);
    for (size_t m = 0; ret >= 0 && m < RX_ARRAY_ELEMS(migrations); m++)
        ret = add_columns(db, migrations[m].add, migrations[m].nb_add, &added);
    if (ret >= 0)
        ret = rx_db_commit(db);
    else
        rx_db_rollback(db);
    if (ret < 0) {
        rx_err("%s: the library lacks columns of its schema and they could not be added", db->path);
        return ret;
    }
    if (added)
        rx_info("%s: added %d column%s its schema lacked", db->path, added, added == 1 ? "" : "s");
    for (size_t m = 0; m < RX_ARRAY_ELEMS(migrations); m++) {
        for (int i = 0; i < migrations[m].nb_add; i++) {
            bool has;
            ret = has_column(db, migrations[m].add[i].table, migrations[m].add[i].name, &has);
            if (ret < 0)
                return ret;
            if (!has) {
                rx_err("%s: column %s.%s missing", db->path, migrations[m].add[i].table,
                       migrations[m].add[i].name);
                return RX_ERROR(EPROTO);
            }
        }
    }
    return 0;
}

static int migrate(RXDb *db, int64_t version)
{
    int64_t current = (int64_t)RX_ARRAY_ELEMS(migrations);
    /* A new library goes through every migration; only an existing one is
     * said to be upgraded. */
    bool existing = version > 0;
    while (version < current) {
        int ret = rx_db_begin(db);
        if (ret < 0)
            return ret;
        /* Again under the write lock: another process opening the same new
         * library at the same moment may have created it meanwhile, or a
         * newer rxbox taken it past this build's schema. */
        int64_t now = version;
        ret = pragma_int(db, "PRAGMA user_version", &now);
        if (ret >= 0 && now > current) {
            rx_db_rollback(db);
            rx_err("%s was migrated by a newer rxbox meanwhile (schema %lld, this build knows %lld); "
                   "not touching it", db->path, (long long)now, (long long)current);
            return RX_ERROR(EPROTO);
        }
        if (ret >= 0 && now == version) {
            char bump[96];
            snprintf(bump, sizeof(bump), "PRAGMA user_version = %lld; PRAGMA application_id = %d",
                     (long long)(version + 1), RX_DB_APPLICATION_ID);
            ret = run_migration(db, &migrations[version]);
            if (ret >= 0)
                ret = rx_db_exec(db, bump);
        }
        if (ret < 0) {
            rx_db_rollback(db);
            rx_err("%s: migration to schema %lld failed", db->path, (long long)(version + 1));
            return ret;
        }
        ret = rx_db_commit(db);
        if (ret < 0)
            return ret;
        if (now != version) {
            version = now;
            continue;
        }
        if (existing)
            rx_info("%s: library upgraded to schema %lld", db->path, (long long)(version + 1));
        version++;
    }
    return check_columns(db);
}

/* WAL is a property of the file. Switching needs the database to itself:
 * two connections switching a new library at once deadlock on their read
 * locks and SQLite answers one of them SQLITE_BUSY at once, without
 * waiting. The other one switches it, and SQLite moves every connection
 * to WAL when it sees the file is, so that one goes on as it is. */
static int set_wal(RXDb *db)
{
    sqlite3_stmt *st = rx_db_prepare(db, "PRAGMA journal_mode = WAL");
    if (!st)
        return RX_ERROR(EIO);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW && sqlite3_stricmp((const char *)sqlite3_column_text(st, 0), "wal"))
        rx_warn("%s: WAL journal not available here (%s); using it as it is", db->path,
                (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE)
        return 0;
    if ((rc & 0xff) == SQLITE_BUSY) {
        rx_debug("%s: busy, not switched to WAL by this connection", db->path);
        return 0;
    }
    return rx_db_error(db, rc, "journal_mode");
}

int rx_db_open(RXDb **out, const char *path)
{
    *out = NULL;
    RXDb *db = calloc(1, sizeof(*db));
    if (!db)
        return RX_ERROR(ENOMEM);
    db->path = rx_strdup(path);
    if (!db->path) {
        free(db);
        return RX_ERROR(ENOMEM);
    }

    int ret;
    int rc = sqlite3_open_v2(path, &db->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        ret = rx_db_error(db, rc, path);
        goto fail;
    }
    sqlite3_extended_result_codes(db->db, 1);
    sqlite3_busy_timeout(db->db, 5000);

    int64_t version;
    ret = rx_db_exec(db, "PRAGMA foreign_keys = ON");
    if (ret >= 0)
        ret = check_identity(db, &version);
    if (ret >= 0)
        ret = set_wal(db);
    if (ret >= 0)
        ret = rx_db_exec(db, "PRAGMA synchronous = NORMAL");
    if (ret >= 0)
        ret = migrate(db, version);
    if (ret < 0)
        goto fail;

    for (int i = 0; i < RX_STMT_NB; i++) {
        char *gen = NULL;
        const char *sql = fixed_sql[i];
        if (!sql) {
            gen = track_sql((enum RXStmtId)i);
            if (!gen) {
                ret = RX_ERROR(ENOMEM);
                goto fail;
            }
            sql = gen;
        }
        rc = sqlite3_prepare_v3(db->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &db->stmt[i], NULL);
        if (rc != SQLITE_OK)
            ret = rx_db_error(db, rc, sql);
        free(gen);
        if (ret < 0)
            goto fail;
    }

    *out = db;
    return 0;

fail:
    rx_db_close(db);
    return ret;
}

void rx_db_close(RXDb *db)
{
    if (!db)
        return;
    while (db->depth > 0)
        rx_db_rollback(db);
    for (int i = 0; i < RX_STMT_NB; i++)
        sqlite3_finalize(db->stmt[i]);
    if (db->db) {
        /* Folds the WAL back in so the library is one file at rest. */
        if (sqlite3_close(db->db) != SQLITE_OK) {
            rx_err("library database: %s", sqlite3_errmsg(db->db));
            sqlite3_close_v2(db->db);
        }
    }
    free(db->path);
    free(db);
}
