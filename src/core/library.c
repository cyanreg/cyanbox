/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The library (rxbox/library.h): every track of the database in memory,
 * written through on each edit.
 *
 * Order of every public edit: the database first, in one transaction, and
 * the arrays only once it has committed. A failed write is logged with
 * SQLite's message and returned, and memory still matches the disk, so
 * nothing is lost silently and nothing claims to be saved that is not.
 * (rx_library_save_track is the exception by its nature: the caller has
 * already edited the track in memory; on failure the edit is still there
 * and the caller can retry.)
 *
 * Tags and overrides: file_meta is what the file says, meta what is shown.
 * A field the user changed is stored in its override column; a field equal
 * to the file's is stored as NULL, so a retagged file shows its new tags
 * unless the user had changed that field. Rating, colour (a hue) and My
 * Tags exist only in the library.
 *
 * Adding a directory probes its files in parallel on a thread pool (the
 * probe is a file open and a header parse, I/O bound, a few ms each) and
 * inserts them in one transaction per directory, in name order, so ids
 * follow the order of the files on disk. Hidden entries and macOS "._"
 * files are skipped and symlinked directories are not followed (no loops),
 * as dj's scan did. */

#include "rxbox/library.h"

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rxbox/audio.h"
#include "rxbox/thread.h"
#include "core.h"

/* The tag fields in the order of the file_* and override columns. */
static const size_t tag_str_offsets[10] = {
    offsetof(RXMeta, title),    offsetof(RXMeta, artist),   offsetof(RXMeta, album),
    offsetof(RXMeta, album_artist), offsetof(RXMeta, genre), offsetof(RXMeta, label),
    offsetof(RXMeta, remixer),  offsetof(RXMeta, composer), offsetof(RXMeta, original_artist),
    offsetof(RXMeta, comment),
};
static const size_t tag_int_offsets[3] = {
    offsetof(RXMeta, year), offsetof(RXMeta, track_number), offsetof(RXMeta, disc_number),
};
#define NB_TAG_STR ((int)RX_ARRAY_ELEMS(tag_str_offsets))
#define NB_TAG_INT ((int)RX_ARRAY_ELEMS(tag_int_offsets))

static char **meta_str(const RXMeta *m, int i)
{
    return (char **)((char *)m + tag_str_offsets[i]);
}

static int *meta_int(const RXMeta *m, int i)
{
    return (int *)((char *)m + tag_int_offsets[i]);
}

static bool str_eq(const char *a, const char *b)
{
    return !strcmp(a ? a : "", b ? b : "");
}

/* ---------------------------------------------------------------- lookups */

RXTrack *rx_library_find_id(const RXLibrary *lib, uint64_t id)
{
    int lo = 0, hi = lib->nb_tracks - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t m = lib->tracks[mid]->id;
        if (m == id)
            return lib->tracks[mid];
        if (m < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

int rx_library_index_of(const RXLibrary *lib, const RXTrack *t)
{
    if (!t)
        return -1;
    int lo = 0, hi = lib->nb_tracks - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t m = lib->tracks[mid]->id;
        if (m == t->id)
            return lib->tracks[mid] == t ? mid : -1;
        if (m < t->id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

RXTrack *rx_library_find_path(const RXLibrary *lib, const char *abs_path)
{
    if (!abs_path)
        return NULL;
    for (int i = 0; i < lib->nb_tracks; i++) {
        if (lib->tracks[i]->path && !strcmp(lib->tracks[i]->path, abs_path))
            return lib->tracks[i];
    }
    return NULL;
}

/* dj's AUDIO_EXTS; "._" files are macOS resource forks, never audio. */
bool rx_is_audio_path(const char *path)
{
    static const char *const exts[] = {
        ".mp3", ".m4a", ".mp4", ".aac", ".wav", ".aif", ".aiff", ".flac", ".ogg", ".opus",
    };
    if (!path || rx_str_starts(rx_path_base(path), "._"))
        return false;
    char ext[16];
    rx_path_ext(path, ext, sizeof(ext));
    for (size_t i = 0; i < RX_ARRAY_ELEMS(exts); i++) {
        if (!strcmp(ext, exts[i]))
            return true;
    }
    return false;
}

/* The array's capacity is the power of two at or above its size, so it
 * needs no field of its own in RXLibrary. */
static int tracks_append(RXLibrary *lib, RXTrack *t)
{
    int n = lib->nb_tracks;
    if (!(n & (n - 1))) {
        RXTrack **a = realloc(lib->tracks, (size_t)(n ? n * 2 : 64) * sizeof(*a));
        if (!a)
            return RX_ERROR(ENOMEM);
        lib->tracks = a;
    }
    lib->tracks[lib->nb_tracks++] = t;
    return 0;
}

/* ---------------------------------------------------------------- binding */

static double finite_or_zero(double v)
{
    return isfinite(v) ? v : 0.0;
}

static void bind_file(RXDb *db, sqlite3_stmt *st, int p, const RXTrack *t, const char *container)
{
    rx_db_bind_int(db, st, p++, t->mtime);
    rx_db_bind_int(db, st, p++, t->file_size);
    rx_db_bind_text(db, st, p++, t->codec);
    rx_db_bind_text(db, st, p++, container ? container : "");
    rx_db_bind_double(db, st, p++, finite_or_zero(t->duration));
    rx_db_bind_int(db, st, p++, t->total_samples);
    rx_db_bind_int(db, st, p++, t->sample_rate);
    rx_db_bind_int(db, st, p++, t->channels);
    rx_db_bind_int(db, st, p++, t->bit_rate);
    rx_db_bind_int(db, st, p++, t->bit_depth);
    rx_db_bind_int(db, st, p++, t->has_artwork);
    for (int i = 0; i < NB_TAG_STR; i++)
        rx_db_bind_text(db, st, p++, *meta_str(&t->file_meta, i));
    for (int i = 0; i < NB_TAG_INT; i++)
        rx_db_bind_int(db, st, p++, *meta_int(&t->file_meta, i));
}

static void bind_edit(RXDb *db, sqlite3_stmt *st, int p, const RXTrack *t)
{
    for (int i = 0; i < NB_TAG_STR; i++) {
        const char *v = *meta_str(&t->meta, i), *f = *meta_str(&t->file_meta, i);
        /* An override of "" is the user clearing the file's value. */
        rx_db_bind_text(db, st, p++, str_eq(v, f) ? NULL : v ? v : "");
    }
    for (int i = 0; i < NB_TAG_INT; i++) {
        int v = *meta_int(&t->meta, i);
        if (v == *meta_int(&t->file_meta, i))
            rx_db_bind_null(db, st, p++);
        else
            rx_db_bind_int(db, st, p++, v);
    }
    rx_db_bind_int(db, st, p++, RX_CLAMP(t->meta.rating, 0, 5));
    /* NAN binds NULL: no colour. */
    double hue = rx_hue_wrap(t->meta.hue);
    rx_db_bind_double(db, st, p++, rx_hue_valid(hue) ? hue : NAN);
    rx_db_bind_text(db, st, p++, t->key);
    rx_db_bind_double(db, st, p++, finite_or_zero(t->key_confidence));
    rx_db_bind_int(db, st, p++, t->grid.beats_per_bar >= 1 && t->grid.beats_per_bar <= 64 ?
                                t->grid.beats_per_bar : 4);
    rx_db_bind_double(db, st, p++, finite_or_zero(t->grid_confidence));
    rx_db_bind_int(db, st, p++, t->grid_flags);
    rx_db_bind_int(db, st, p++, t->grid_locked);
    rx_db_bind_int(db, st, p++, t->analysed);
    rx_db_bind_int(db, st, p++, t->beats_version);
    rx_db_bind_int(db, st, p++, t->key_version);
    rx_db_bind_int(db, st, p++, t->waves_version);
}

/* Sections, cues and My Tags: replaced as a whole. */
static int write_children(RXLibrary *lib, const RXTrack *t)
{
    RXDb *db = lib->db;
    int64_t id = (int64_t)t->id;

    static const enum RXStmtId clears[] = {
        RX_STMT_SECTION_CLEAR, RX_STMT_CUE_CLEAR, RX_STMT_TAG_CLEAR,
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(clears); i++) {
        sqlite3_stmt *st = rx_db_stmt(db, clears[i]);
        rx_db_bind_int(db, st, 1, id);
        int ret = rx_db_done(db, st, "clearing a track's rows");
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < t->grid.nb_sections; i++) {
        const RXTempoSection *s = &t->grid.sections[i];
        sqlite3_stmt *st = rx_db_stmt(db, RX_STMT_SECTION_INSERT);
        rx_db_bind_int(db, st, 1, id);
        rx_db_bind_int(db, st, 2, i);
        rx_db_bind_double(db, st, 3, s->start);
        rx_db_bind_double(db, st, 4, s->bpm);
        rx_db_bind_int(db, st, 5, s->beat);
        int ret = rx_db_done(db, st, "writing a grid section");
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        sqlite3_stmt *st = rx_db_stmt(db, RX_STMT_CUE_INSERT);
        rx_db_bind_int(db, st, 1, id);
        rx_db_bind_double(db, st, 2, c->time);
        rx_db_bind_double(db, st, 3, c->grid_beat);
        rx_db_bind_int(db, st, 4, c->hot);
        rx_db_bind_double(db, st, 5, rx_cue_is_loop(c) ? c->loop_end : NAN);
        rx_db_bind_double(db, st, 6, rx_cue_is_loop(c) ? c->loop_beats : NAN);
        rx_db_bind_int(db, st, 7, c->active);
        rx_db_bind_text(db, st, 8, c->name);
        rx_db_bind_text(db, st, 9, c->color);
        int ret = rx_db_done(db, st, "writing a cue");
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < t->meta.nb_tags; i++) {
        sqlite3_stmt *st = rx_db_stmt(db, RX_STMT_TAG_INSERT);
        rx_db_bind_int(db, st, 1, id);
        rx_db_bind_int(db, st, 2, i);
        rx_db_bind_text(db, st, 3, t->meta.tags[i]);
        int ret = rx_db_done(db, st, "writing a tag");
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* What the database refuses with a CHECK, refused here with a reason. */
static int check_track(const RXTrack *t)
{
    for (int i = 0; i < t->grid.nb_sections; i++) {
        const RXTempoSection *s = &t->grid.sections[i];
        if (!isfinite(s->start) || !isfinite(s->bpm) || s->bpm <= 0) {
            rx_err("track %llu: grid section %d is not valid (start %g, bpm %g)",
                   (unsigned long long)t->id, i, s->start, s->bpm);
            return RX_ERROR(EINVAL);
        }
    }
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (!isfinite(c->time) || c->hot < -1 || c->hot >= RX_HOT_CUE_SLOTS) {
            rx_err("track %llu: cue %d is not valid (time %g, slot %d)",
                   (unsigned long long)t->id, i, c->time, c->hot);
            return RX_ERROR(EINVAL);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ loading */

/* Columns of the track query: id path added missing play_count last_played,
 * then the file and edit groups. */
#define COL_FILE 6
#define COL_EDIT (COL_FILE + RX_TRACK_FILE_COLS)

static char *track_select_sql(void)
{
    char sql[4096];
    size_t n = (size_t)snprintf(sql, sizeof(sql),
                                "SELECT id, path, added, missing, play_count, last_played");
    for (int i = 0; i < RX_TRACK_FILE_COLS && n < sizeof(sql); i++)
        n += (size_t)snprintf(sql + n, sizeof(sql) - n, ", %s", rx_track_file_cols[i]);
    for (int i = 0; i < RX_TRACK_EDIT_COLS && n < sizeof(sql); i++)
        n += (size_t)snprintf(sql + n, sizeof(sql) - n, ", %s", rx_track_edit_cols[i]);
    if (n < sizeof(sql))
        n += (size_t)snprintf(sql + n, sizeof(sql) - n, " FROM track ORDER BY id");
    return n < sizeof(sql) ? rx_strdup(sql) : NULL;
}

static void column_copy(sqlite3_stmt *st, int col, char *dst, size_t size)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    snprintf(dst, size, "%s", s ? s : "");
}

static int track_from_row(sqlite3_stmt *st, RXTrack **out)
{
    int err = 0;
    RXTrack *t = rx_track_new(NULL);
    if (!t)
        return RX_ERROR(ENOMEM);

    t->id = (uint64_t)sqlite3_column_int64(st, 0);
    t->path = rx_db_column_text(st, 1, &err);
    t->added = sqlite3_column_int64(st, 2);
    t->missing = sqlite3_column_int(st, 3);
    t->play_count = sqlite3_column_int(st, 4);
    t->last_played = sqlite3_column_int64(st, 5);

    int c = COL_FILE;
    t->mtime = sqlite3_column_int64(st, c++);
    t->file_size = sqlite3_column_int64(st, c++);
    column_copy(st, c++, t->codec, sizeof(t->codec));
    /* container: kept in the table for the record, not in RXTrack */
    c++;
    t->duration = sqlite3_column_double(st, c++);
    t->total_samples = sqlite3_column_int64(st, c++);
    t->sample_rate = sqlite3_column_int(st, c++);
    t->channels = sqlite3_column_int(st, c++);
    t->bit_rate = sqlite3_column_int(st, c++);
    t->bit_depth = sqlite3_column_int(st, c++);
    t->has_artwork = sqlite3_column_int(st, c++);
    for (int i = 0; i < NB_TAG_STR; i++)
        *meta_str(&t->file_meta, i) = rx_db_column_text(st, c++, &err);
    for (int i = 0; i < NB_TAG_INT; i++)
        *meta_int(&t->file_meta, i) = sqlite3_column_int(st, c++);

    if (!err)
        err = rx_meta_copy(&t->meta, &t->file_meta);
    c = COL_EDIT;
    for (int i = 0; i < NB_TAG_STR; i++, c++) {
        if (sqlite3_column_type(st, c) != SQLITE_NULL && !err)
            err = rx_meta_set(meta_str(&t->meta, i), (const char *)sqlite3_column_text(st, c));
    }
    for (int i = 0; i < NB_TAG_INT; i++, c++) {
        if (sqlite3_column_type(st, c) != SQLITE_NULL)
            *meta_int(&t->meta, i) = sqlite3_column_int(st, c);
    }
    t->meta.rating = sqlite3_column_int(st, c++);
    t->meta.hue = sqlite3_column_type(st, c) == SQLITE_NULL ? RX_HUE_NONE : sqlite3_column_double(st, c);
    c++;
    column_copy(st, c++, t->key, sizeof(t->key));
    t->key_confidence = sqlite3_column_double(st, c++);
    t->grid.beats_per_bar = sqlite3_column_int(st, c++);
    t->grid_confidence = sqlite3_column_double(st, c++);
    t->grid_flags = (unsigned)sqlite3_column_int64(st, c++);
    t->grid_locked = sqlite3_column_int(st, c++);
    t->analysed = sqlite3_column_int64(st, c++);
    t->beats_version = sqlite3_column_int(st, c++);
    t->key_version = sqlite3_column_int(st, c++);
    t->waves_version = sqlite3_column_int(st, c++);

    if (err || !t->path) {
        rx_track_free(t);
        return err ? err : RX_ERROR(EINVAL);
    }
    *out = t;
    return 0;
}

/* Rows of the child tables arrive sorted by track id, as the array is. */
typedef struct Cursor {
    RXLibrary *lib;
    int i;
} Cursor;

static RXTrack *cursor_find(Cursor *c, uint64_t id)
{
    while (c->i < c->lib->nb_tracks && c->lib->tracks[c->i]->id < id)
        c->i++;
    if (c->i < c->lib->nb_tracks && c->lib->tracks[c->i]->id == id)
        return c->lib->tracks[c->i];
    return NULL;
}

static int load_sections(RXLibrary *lib)
{
    sqlite3_stmt *st = rx_db_prepare(lib->db, "SELECT track_id, start, bpm, beat FROM grid_section "
                                              "ORDER BY track_id, idx");
    if (!st)
        return RX_ERROR(EIO);
    Cursor cur = { .lib = lib };
    int ret;
    while ((ret = rx_db_row(lib->db, st, "loading grids")) > 0) {
        RXTrack *t = cursor_find(&cur, (uint64_t)sqlite3_column_int64(st, 0));
        if (!t)
            continue;
        RXTempoSection *s = realloc(t->grid.sections, (size_t)(t->grid.nb_sections + 1) * sizeof(*s));
        if (!s) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        s[t->grid.nb_sections++] = (RXTempoSection) {
            .start = sqlite3_column_double(st, 1),
            .bpm   = sqlite3_column_double(st, 2),
            .beat  = sqlite3_column_int(st, 3),
        };
        t->grid.sections = s;
    }
    sqlite3_finalize(st);
    return ret;
}

static double column_or_nan(sqlite3_stmt *st, int col)
{
    return sqlite3_column_type(st, col) == SQLITE_NULL ? NAN : sqlite3_column_double(st, col);
}

static int load_cues(RXLibrary *lib)
{
    sqlite3_stmt *st = rx_db_prepare(lib->db, "SELECT track_id, time, grid_beat, hot, loop_end, "
                                              "loop_beats, active, name, color FROM cue "
                                              "ORDER BY track_id, time, id");
    if (!st)
        return RX_ERROR(EIO);
    Cursor cur = { .lib = lib };
    int ret;
    while ((ret = rx_db_row(lib->db, st, "loading cues")) > 0) {
        RXTrack *t = cursor_find(&cur, (uint64_t)sqlite3_column_int64(st, 0));
        if (!t)
            continue;
        RXCue *cues = realloc(t->cues, (size_t)(t->nb_cues + 1) * sizeof(*cues));
        if (!cues) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        t->cues = cues;
        RXCue *c = &cues[t->nb_cues++];
        rx_cue_init(c, sqlite3_column_double(st, 1));
        c->grid_beat = column_or_nan(st, 2);
        c->hot = sqlite3_column_int(st, 3);
        c->loop_end = sqlite3_column_type(st, 4) == SQLITE_NULL ? -1.0 : sqlite3_column_double(st, 4);
        c->loop_beats = column_or_nan(st, 5);
        c->active = sqlite3_column_int(st, 6);
        column_copy(st, 7, c->name, sizeof(c->name));
        column_copy(st, 8, c->color, sizeof(c->color));
    }
    sqlite3_finalize(st);
    return ret;
}

static int load_tags(RXLibrary *lib)
{
    sqlite3_stmt *st = rx_db_prepare(lib->db, "SELECT track_id, tag FROM track_tag "
                                              "ORDER BY track_id, position");
    if (!st)
        return RX_ERROR(EIO);
    Cursor cur = { .lib = lib };
    int ret;
    while ((ret = rx_db_row(lib->db, st, "loading tags")) > 0) {
        RXTrack *t = cursor_find(&cur, (uint64_t)sqlite3_column_int64(st, 0));
        if (t && (ret = rx_meta_add_tag(&t->meta, (const char *)sqlite3_column_text(st, 1))) < 0)
            break;
    }
    sqlite3_finalize(st);
    return ret;
}

static int load_waveform_flags(RXLibrary *lib)
{
    sqlite3_stmt *st = rx_db_prepare(lib->db, "SELECT track_id FROM waveform ORDER BY track_id");
    if (!st)
        return RX_ERROR(EIO);
    Cursor cur = { .lib = lib };
    int ret;
    while ((ret = rx_db_row(lib->db, st, "loading waveform flags")) > 0) {
        RXTrack *t = cursor_find(&cur, (uint64_t)sqlite3_column_int64(st, 0));
        if (t)
            t->has_waveforms = true;
    }
    sqlite3_finalize(st);
    return ret;
}

static int load_tracks(RXLibrary *lib)
{
    char *sql = track_select_sql();
    if (!sql)
        return RX_ERROR(ENOMEM);
    sqlite3_stmt *st = rx_db_prepare(lib->db, sql);
    free(sql);
    if (!st)
        return RX_ERROR(EIO);

    int ret;
    while ((ret = rx_db_row(lib->db, st, "loading tracks")) > 0) {
        RXTrack *t = NULL;
        ret = track_from_row(st, &t);
        if (ret >= 0 && (ret = tracks_append(lib, t)) < 0)
            rx_track_free(t);
        if (ret < 0)
            break;
    }
    sqlite3_finalize(st);

    if (ret >= 0)
        ret = load_sections(lib);
    if (ret >= 0)
        ret = load_cues(lib);
    if (ret >= 0)
        ret = load_tags(lib);
    if (ret >= 0)
        ret = load_waveform_flags(lib);

    for (int i = 0; ret >= 0 && i < lib->nb_tracks; i++) {
        RXTrack *t = lib->tracks[i];
        if (t->grid.nb_sections && !rx_grid_valid(&t->grid))
            rx_warn("track %llu (%s): its grid is not valid (sections out of order or a "
                    "tempo out of range); check the grid", (unsigned long long)t->id, t->path);
    }
    return ret;
}

/* ------------------------------------------------------------ open/close */

int rx_library_open(RXLibrary *lib, const char *root)
{
    *lib = (RXLibrary) { 0 };
    rx_config_defaults(&lib->cfg);

    char *dir = root ? rx_path_expand(root) : rx_library_default_root();
    if (!dir)
        return root ? RX_ERROR(ENOMEM) : RX_ERROR(ENOENT);
    int ret = rx_mkdir_p(dir);
    if (ret < 0) {
        rx_err("cannot create the library directory %s: %s", dir, strerror(-ret));
        free(dir);
        return ret;
    }
    lib->root = rx_path_abs(dir);
    free(dir);
    if (!lib->root)
        return RX_ERROR(ENOMEM);

    char *path = rx_path_join(lib->root, "library.db");
    if (!path) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    ret = rx_db_open(&lib->db, path);
    free(path);
    if (ret < 0)
        goto fail;

    /* Everything is read in one transaction, so from one snapshot: the
     * export job opens the library on its own connection while the
     * interface's may be committing, and tracks and playlists read a moment
     * apart would not agree (an entry whose track is not in the list). The
     * missing flags are written after it, in a transaction of their own. */
    ret = rx_db_begin_read(lib->db);
    if (ret < 0)
        goto fail;
    ret = rx_core_config_load(lib);
    if (ret >= 0)
        ret = load_tracks(lib);
    if (ret >= 0)
        ret = rx_core_playlists_load(lib);
    if (ret >= 0)
        ret = rx_db_commit(lib->db);
    else
        rx_db_rollback(lib->db);
    if (ret >= 0)
        ret = rx_library_rescan_missing(lib);
    if (ret < 0)
        goto fail;
    /* A library that cannot be written keeps its old keys and opens. */
    if (rx_core_config_migrate(lib) < 0)
        rx_warn("could not move the old interface settings to the device settings");

    lib->generation = 1;
    return 0;

fail:
    rx_library_close(lib);
    return ret;
}

void rx_library_close(RXLibrary *lib)
{
    if (!lib)
        return;
    rx_core_playlists_free(lib);
    for (int i = 0; i < lib->nb_tracks; i++)
        rx_track_free(lib->tracks[i]);
    free(lib->tracks);
    rx_config_free(&lib->cfg);
    rx_db_close(lib->db);
    free(lib->root);
    *lib = (RXLibrary) { 0 };
}

int rx_library_check(const RXLibrary *lib)
{
    int problems = 0;
    static const char *const checks[] = { "PRAGMA integrity_check", "PRAGMA foreign_key_check" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(checks); i++) {
        sqlite3_stmt *st = rx_db_prepare(lib->db, checks[i]);
        if (!st)
            return RX_ERROR(EIO);
        int ret;
        while ((ret = rx_db_row(lib->db, st, checks[i])) > 0) {
            const char *first = (const char *)sqlite3_column_text(st, 0);
            if (i == 0 && first && !strcmp(first, "ok"))
                continue;
            problems++;
            if (i == 0)
                rx_err("library database: %s", first ? first : "(null)");
            else
                rx_err("library database: row %lld of %s refers to a missing %s row",
                       (long long)sqlite3_column_int64(st, 1), first ? first : "?",
                       (const char *)sqlite3_column_text(st, 2));
        }
        sqlite3_finalize(st);
        if (ret < 0)
            return ret;
    }
    return problems ? RX_ERROR(EIO) : 0;
}

/* ----------------------------------------------------------------- probing */

static int parse_leading_int(const char *s)
{
    /* dj's _int(): the leading digits, so "2024-05-01" is 2024, "3/12" is 3. */
    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t')
        s++;
    long v = 0;
    while (*s >= '0' && *s <= '9' && v < 100000000)
        v = v * 10 + (*s++ - '0');
    return (int)v;
}

/* The tag names of dj's library.track_from_probe, in its order. */
static int fill_from_probe(RXTrack *t, const RXProbe *p)
{
    RXMeta m = { .hue = RX_HUE_NONE };
    int ret = 0;

    const char *title = rx_probe_tag(p, "title", NULL);
    if (title) {
        ret = rx_meta_set(&m.title, title);
    } else {
        /* dj: the file name without its extension. */
        const char *base = rx_path_base(t->path);
        const char *dot = strrchr(base, '.');
        char *stem = rx_strndup(base, dot && dot != base ? (size_t)(dot - base) : strlen(base));
        ret = stem ? rx_meta_set(&m.title, stem) : RX_ERROR(ENOMEM);
        free(stem);
    }

    const struct {
        char **field;
        const char *names[5];
    } strs[] = {
        { &m.artist,          { "artist" } },
        { &m.album,           { "album" } },
        { &m.album_artist,    { "album_artist", "albumartist", "album artist" } },
        { &m.genre,           { "genre" } },
        { &m.label,           { "label", "publisher", "organization", "tpub" } },
        { &m.remixer,         { "remixer", "mixartist", "tpe4" } },
        { &m.composer,        { "composer" } },
        { &m.original_artist, { "original_artist", "originalartist", "tope" } },
        { &m.comment,         { "comment", "description" } },
    };
    for (size_t i = 0; ret >= 0 && i < RX_ARRAY_ELEMS(strs); i++) {
        const char *const *n = strs[i].names;
        ret = rx_meta_set(strs[i].field, rx_probe_tag(p, n[0], n[1], n[2], n[3], NULL));
    }
    if (ret < 0) {
        rx_meta_free(&m);
        return ret;
    }
    m.year = parse_leading_int(rx_probe_tag(p, "date", "year", "tdrc", "originaldate", NULL));
    m.track_number = parse_leading_int(rx_probe_tag(p, "track", "tracknumber", NULL));
    m.disc_number = parse_leading_int(rx_probe_tag(p, "disc", "discnumber", NULL));

    rx_meta_free(&t->file_meta);
    t->file_meta = m;
    t->duration = p->duration;
    t->total_samples = p->total_samples;
    t->sample_rate = p->sample_rate;
    t->channels = p->channels;
    t->bit_rate = p->bit_rate;
    t->bit_depth = p->bit_depth;
    snprintf(t->codec, sizeof(t->codec), "%s", p->codec);
    t->has_artwork = p->has_artwork;
    t->file_size = rx_file_size(t->path);
    t->mtime = rx_file_mtime(t->path);
    return 0;
}

typedef struct Probed {
    RXTrack *t;
    char     container[32];
    int      err;
} Probed;

/* Runs on a worker: touches only its own Probed and track. */
static void probe_job(void *arg)
{
    Probed *pj = arg;
    RXProbe p;
    pj->err = rx_probe(pj->t->path, &p);
    if (pj->err < 0)
        return;
    snprintf(pj->container, sizeof(pj->container), "%s", p.container);
    pj->err = fill_from_probe(pj->t, &p);
    rx_probe_free(&p);
}

/* `pool` NULL: on this thread. */
static void probe_all(RXThreadPool *pool, Probed *items, int nb)
{
    for (int i = 0; i < nb; i++) {
        if (!pool || nb < 2 || rx_pool_submit(pool, probe_job, &items[i]) < 0)
            probe_job(&items[i]);
    }
    rx_pool_wait(pool);
}

/* --------------------------------------------------------------- adding */

static int insert_track(RXLibrary *lib, RXTrack *t, const char *container)
{
    RXDb *db = lib->db;
    sqlite3_stmt *st = rx_db_stmt(db, RX_STMT_TRACK_INSERT);
    rx_db_bind_text(db, st, 1, t->path);
    rx_db_bind_int(db, st, 2, t->added);
    rx_db_bind_int(db, st, 3, t->missing);
    bind_file(db, st, 4, t, container);
    bind_edit(db, st, 4 + RX_TRACK_FILE_COLS, t);
    int ret = rx_db_done(db, st, t->path);
    if (ret < 0)
        return ret;
    t->id = (uint64_t)sqlite3_last_insert_rowid(db->db);
    return write_children(lib, t);
}

static int set_missing(RXLibrary *lib, RXTrack *t, bool missing)
{
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_TRACK_MISSING);
    rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
    rx_db_bind_int(lib->db, st, 2, missing);
    int ret = rx_db_done(lib->db, st, t->path);
    if (ret >= 0 && t->missing != missing) {
        t->missing = missing;
        lib->generation++;
    }
    return ret;
}

/* A known track whose file is back. */
static RXTrack *known_track(RXLibrary *lib, const char *abs)
{
    RXTrack *t = rx_library_find_path(lib, abs);
    if (t && t->missing && rx_file_exists(abs))
        set_missing(lib, t, false);
    return t;
}

/* Inserts the probed tracks in one transaction, each in a savepoint of its
 * own so one bad row does not cost the others. Reports every one. */
static int insert_batch(RXLibrary *lib, Probed *items, int nb, rx_add_cb cb, void *opaque)
{
    int ret = rx_db_begin(lib->db);
    if (ret < 0)
        return ret;
    int64_t now = rx_time_now();
    for (int i = 0; i < nb; i++) {
        Probed *p = &items[i];
        if (p->err < 0)
            continue;
        p->t->added = now;
        p->err = rx_meta_copy(&p->t->meta, &p->t->file_meta);
        if (p->err < 0)
            continue;
        p->err = rx_db_begin(lib->db);
        if (p->err >= 0) {
            p->err = insert_track(lib, p->t, p->container);
            if (p->err < 0)
                rx_db_rollback(lib->db);
            else
                p->err = rx_db_commit(lib->db);
        }
    }
    ret = rx_db_commit(lib->db);

    int added = 0;
    for (int i = 0; i < nb; i++) {
        Probed *p = &items[i];
        if (ret < 0 && p->err >= 0)
            p->err = ret;
        if (p->err >= 0 && (p->err = tracks_append(lib, p->t)) >= 0) {
            added++;
            if (cb)
                cb(opaque, p->t->path, p->t, 0);
            p->t = NULL;
            continue;
        }
        rx_warn("cannot add %s: %s", p->t->path, strerror(-p->err));
        if (cb)
            cb(opaque, p->t->path, NULL, p->err);
        rx_track_free(p->t);
        p->t = NULL;
    }
    if (added)
        lib->generation++;
    return ret < 0 ? ret : added;
}

RXTrack *rx_library_add_file(RXLibrary *lib, const char *path, int *err)
{
    int dummy;
    if (!err)
        err = &dummy;
    *err = 0;

    char *abs = rx_path_abs(path);
    if (!abs) {
        *err = RX_ERROR(ENOENT);
        return NULL;
    }
    RXTrack *t = known_track(lib, abs);
    if (t) {
        free(abs);
        return t;
    }
    if (!rx_file_exists(abs)) {
        *err = RX_ERROR(ENOENT);
        free(abs);
        return NULL;
    }

    Probed p = { .t = rx_track_new(abs) };
    free(abs);
    if (!p.t) {
        *err = RX_ERROR(ENOMEM);
        return NULL;
    }
    probe_job(&p);
    RXTrack *added = p.err >= 0 ? p.t : NULL;
    int ret = insert_batch(lib, &p, 1, NULL, NULL);
    if (ret <= 0) {
        *err = ret < 0 ? ret : p.err < 0 ? p.err : RX_ERROR(EIO);
        return NULL;
    }
    return added;
}

typedef struct DirList {
    char **files, **dirs;
    int nb_files, nb_dirs;
} DirList;

static void dirlist_free(DirList *l)
{
    for (int i = 0; i < l->nb_files; i++)
        free(l->files[i]);
    for (int i = 0; i < l->nb_dirs; i++)
        free(l->dirs[i]);
    free(l->files);
    free(l->dirs);
}

static int push_path(char ***list, int *nb, char *path)
{
    char **n = realloc(*list, (size_t)(*nb + 1) * sizeof(*n));
    if (!n) {
        free(path);
        return RX_ERROR(ENOMEM);
    }
    n[(*nb)++] = path;
    *list = n;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int list_dir(const char *dir, DirList *l)
{
    *l = (DirList) { 0 };
    DIR *d = opendir(dir);
    if (!d) {
        int err = errno;
        rx_warn("cannot read %s: %s", dir, strerror(err));
        return RX_ERROR(err);
    }
    int ret = 0;
    struct dirent *e;
    while (ret >= 0 && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char *path = rx_path_join(dir, e->d_name);
        if (!path) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        struct stat st;
        if (lstat(path, &st) < 0) {
            free(path);
            continue;
        }
        if (S_ISDIR(st.st_mode))
            ret = push_path(&l->dirs, &l->nb_dirs, path);
        else if (rx_is_audio_path(path) && rx_file_exists(path))
            ret = push_path(&l->files, &l->nb_files, path);
        else
            free(path);
    }
    closedir(d);
    if (ret < 0) {
        dirlist_free(l);
        return ret;
    }
    if (l->nb_files)
        qsort(l->files, (size_t)l->nb_files, sizeof(*l->files), cmp_str);
    if (l->nb_dirs)
        qsort(l->dirs, (size_t)l->nb_dirs, sizeof(*l->dirs), cmp_str);
    return 0;
}

static int add_dir(RXLibrary *lib, RXThreadPool *pool, const char *dir, rx_add_cb cb, void *opaque)
{
    DirList l;
    int ret = list_dir(dir, &l);
    if (ret < 0)
        return ret;

    Probed *items = calloc((size_t)RX_MAX(l.nb_files, 1), sizeof(*items));
    if (!items) {
        dirlist_free(&l);
        return RX_ERROR(ENOMEM);
    }
    int nb = 0;
    for (int i = 0; i < l.nb_files; i++) {
        char *abs = rx_path_abs(l.files[i]);
        if (!abs) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        RXTrack *t = known_track(lib, abs);
        if (t) {
            if (cb)
                cb(opaque, abs, t, 0);
            free(abs);
            continue;
        }
        items[nb].t = rx_track_new(abs);
        free(abs);
        if (!items[nb].t) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        nb++;
    }

    int added = 0;
    if (ret >= 0) {
        probe_all(pool, items, nb);
        ret = insert_batch(lib, items, nb, cb, opaque);
        added = RX_MAX(ret, 0);
    }
    for (int i = 0; i < nb; i++)
        rx_track_free(items[i].t);
    free(items);

    for (int i = 0; ret >= 0 && i < l.nb_dirs; i++) {
        int n = add_dir(lib, pool, l.dirs[i], cb, opaque);
        /* An unreadable subdirectory is reported and skipped. */
        if (n == RX_ERROR(ENOMEM))
            ret = n;
        else if (n > 0)
            added += n;
    }
    dirlist_free(&l);
    return ret < 0 ? ret : added;
}

int rx_library_add_path(RXLibrary *lib, const char *path, rx_add_cb cb, void *opaque)
{
    char *abs = rx_path_abs(path);
    if (!abs)
        return RX_ERROR(ENOENT);

    int ret;
    if (rx_dir_exists(abs)) {
        /* Probing is I/O bound: as many threads as CPUs is plenty. */
        RXThreadPool *pool = NULL;
        if (rx_pool_create(&pool, 0) < 0)
            pool = NULL;
        ret = add_dir(lib, pool, abs, cb, opaque);
        rx_pool_destroy(pool);
    } else if (rx_file_exists(abs)) {
        int before = lib->nb_tracks, err;
        RXTrack *t = rx_library_add_file(lib, abs, &err);
        if (cb)
            cb(opaque, abs, t, t ? 0 : err);
        ret = t ? lib->nb_tracks - before : err;
    } else {
        ret = RX_ERROR(ENOENT);
    }
    free(abs);
    return ret;
}

/* ------------------------------------------------------------------ edits */

static void remove_cache_files(const RXLibrary *lib, uint64_t id)
{
    char *cache = rx_path_join(lib->root, "cache");
    DIR *d = cache ? opendir(cache) : NULL;
    if (!d) {
        free(cache);
        return;
    }
    char prefix[32];
    int n = snprintf(prefix, sizeof(prefix), "%llu.", (unsigned long long)id);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char *kind = rx_path_join(cache, e->d_name);
        DIR *k = kind ? opendir(kind) : NULL;
        struct dirent *f;
        while (k && (f = readdir(k))) {
            if (strncmp(f->d_name, prefix, (size_t)n))
                continue;
            char *file = rx_path_join(kind, f->d_name);
            if (file && unlink(file) < 0)
                rx_warn("cannot delete %s: %s", file, strerror(errno));
            free(file);
        }
        if (k)
            closedir(k);
        free(kind);
    }
    closedir(d);
    free(cache);
}

int rx_library_remove(RXLibrary *lib, RXTrack *t)
{
    int idx = rx_library_index_of(lib, t);
    if (idx < 0)
        return RX_ERROR(ENOENT);

    int ret = rx_db_begin(lib->db);
    if (ret < 0)
        return ret;
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_TRACK_DELETE);
    rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
    ret = rx_db_done(lib->db, st, t->path);
    /* The entries went with the track (ON DELETE CASCADE); the playlists
     * that held it are renumbered so positions stay dense. */
    if (ret >= 0)
        ret = rx_core_playlists_drop_track(lib, t->id);
    if (ret < 0) {
        rx_db_rollback(lib->db);
        return ret;
    }
    ret = rx_db_commit(lib->db);
    if (ret < 0)
        return ret;

    rx_core_playlists_forget_track(lib, t->id);
    remove_cache_files(lib, t->id);
    memmove(&lib->tracks[idx], &lib->tracks[idx + 1],
            (size_t)(lib->nb_tracks - idx - 1) * sizeof(*lib->tracks));
    lib->nb_tracks--;
    rx_track_free(t);
    lib->generation++;
    return 0;
}

int rx_library_save_track(RXLibrary *lib, RXTrack *t)
{
    if (rx_library_index_of(lib, t) < 0)
        return RX_ERROR(ENOENT);
    int ret = check_track(t);
    if (ret < 0)
        return ret;

    ret = rx_db_begin(lib->db);
    if (ret < 0)
        return ret;
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_TRACK_EDIT);
    rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
    bind_edit(lib->db, st, 2, t);
    ret = rx_db_done(lib->db, st, t->path);
    if (ret >= 0)
        ret = write_children(lib, t);
    if (ret < 0) {
        rx_db_rollback(lib->db);
        rx_err("track %llu (%s) was NOT saved", (unsigned long long)t->id, t->path);
        return ret;
    }
    ret = rx_db_commit(lib->db);
    if (ret < 0) {
        rx_err("track %llu (%s) was NOT saved", (unsigned long long)t->id, t->path);
        return ret;
    }
    lib->generation++;
    return 0;
}

int rx_library_refresh_track(RXLibrary *lib, RXTrack *t)
{
    if (rx_library_index_of(lib, t) < 0)
        return RX_ERROR(ENOENT);
    if (!rx_file_exists(t->path)) {
        set_missing(lib, t, true);
        return RX_ERROR(ENOENT);
    }

    Probed p = { .t = rx_track_new(t->path) };
    if (!p.t)
        return RX_ERROR(ENOMEM);
    probe_job(&p);
    int ret = p.err;
    if (ret < 0) {
        rx_warn("cannot read %s: %s", t->path, strerror(-ret));
        goto end;
    }

    ret = rx_db_begin(lib->db);
    if (ret < 0)
        goto end;
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_TRACK_FILE);
    rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
    bind_file(lib->db, st, 2, p.t, p.container);
    ret = rx_db_done(lib->db, st, t->path);
    if (ret < 0) {
        rx_db_rollback(lib->db);
        goto end;
    }
    ret = rx_db_commit(lib->db);
    if (ret < 0)
        goto end;

    /* The new file tags show wherever the user had not overridden them
     * (the same rule the override columns follow). */
    RXTrack *n = p.t;
    for (int i = 0; ret >= 0 && i < NB_TAG_STR; i++) {
        char **v = meta_str(&t->meta, i);
        if (str_eq(*v, *meta_str(&t->file_meta, i)))
            ret = rx_meta_set(v, *meta_str(&n->file_meta, i));
    }
    for (int i = 0; i < NB_TAG_INT; i++) {
        int *v = meta_int(&t->meta, i);
        if (*v == *meta_int(&t->file_meta, i))
            *v = *meta_int(&n->file_meta, i);
    }
    RXMeta old = t->file_meta;
    t->file_meta = n->file_meta;
    n->file_meta = old;
    t->duration = n->duration;
    t->total_samples = n->total_samples;
    t->sample_rate = n->sample_rate;
    t->channels = n->channels;
    t->bit_rate = n->bit_rate;
    t->bit_depth = n->bit_depth;
    memcpy(t->codec, n->codec, sizeof(t->codec));
    t->file_size = n->file_size;
    t->mtime = n->mtime;
    t->has_artwork = n->has_artwork;
    t->missing = false;
    lib->generation++;

end:
    rx_track_free(p.t);
    return ret;
}

int rx_library_record_play(RXLibrary *lib, RXTrack *t)
{
    if (rx_library_index_of(lib, t) < 0)
        return RX_ERROR(ENOENT);
    int64_t now = rx_time_now();
    int ret = rx_db_begin(lib->db);
    if (ret < 0)
        return ret;
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_TRACK_PLAYED);
    rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
    rx_db_bind_int(lib->db, st, 2, t->play_count + 1);
    rx_db_bind_int(lib->db, st, 3, now);
    ret = rx_db_done(lib->db, st, t->path);
    if (ret >= 0) {
        st = rx_db_stmt(lib->db, RX_STMT_PLAY_INSERT);
        rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
        rx_db_bind_int(lib->db, st, 2, now);
        ret = rx_db_done(lib->db, st, t->path);
    }
    if (ret < 0) {
        rx_db_rollback(lib->db);
        return ret;
    }
    ret = rx_db_commit(lib->db);
    if (ret < 0)
        return ret;
    t->play_count++;
    t->last_played = now;
    lib->generation++;
    return 0;
}

int rx_library_relocate(RXLibrary *lib, RXTrack *t, const char *new_abs_path)
{
    if (rx_library_index_of(lib, t) < 0 || !new_abs_path || new_abs_path[0] != '/')
        return RX_ERROR(EINVAL);
    /* Spelled as adding the file would spell it (symlinks and "..", resolved),
     * so the same file is never two tracks: one added under its own path
     * and one moved there by another name. */
    char *path = rx_file_exists(new_abs_path) ? rx_path_abs(new_abs_path)
                                              : rx_strdup(new_abs_path);
    if (!path)
        return RX_ERROR(ENOMEM);
    RXTrack *other = rx_library_find_path(lib, path);
    if (other && other != t) {
        free(path);
        return RX_ERROR(EEXIST);
    }

    bool missing = !rx_file_exists(path);
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_TRACK_PATH);
    rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
    rx_db_bind_text(lib->db, st, 2, path);
    rx_db_bind_int(lib->db, st, 3, missing);
    int ret = rx_db_done(lib->db, st, path);
    if (ret < 0) {
        free(path);
        return ret;
    }
    free(t->path);
    t->path = path;
    t->missing = missing;
    lib->generation++;
    return 0;
}

int rx_library_rescan_missing(RXLibrary *lib)
{
    if (!lib->nb_tracks)
        return 0;
    bool *missing = malloc((size_t)lib->nb_tracks * sizeof(*missing));
    if (!missing)
        return RX_ERROR(ENOMEM);
    int changed = 0;
    for (int i = 0; i < lib->nb_tracks; i++) {
        missing[i] = !rx_file_exists(lib->tracks[i]->path);
        changed += missing[i] != lib->tracks[i]->missing;
    }
    if (!changed) {
        free(missing);
        return 0;
    }

    int ret = rx_db_begin(lib->db);
    for (int i = 0; ret >= 0 && i < lib->nb_tracks; i++) {
        RXTrack *t = lib->tracks[i];
        if (missing[i] == t->missing)
            continue;
        sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_TRACK_MISSING);
        rx_db_bind_int(lib->db, st, 1, (int64_t)t->id);
        rx_db_bind_int(lib->db, st, 2, missing[i]);
        ret = rx_db_done(lib->db, st, t->path);
        if (ret < 0)
            rx_db_rollback(lib->db);
    }
    if (ret >= 0)
        ret = rx_db_commit(lib->db);
    if (ret >= 0) {
        for (int i = 0; i < lib->nb_tracks; i++)
            lib->tracks[i]->missing = missing[i];
        lib->generation++;
    }
    free(missing);
    return ret < 0 ? ret : changed;
}

/* ------------------------------------------------------- waveforms, art */

int rx_core_waveform_blob_put(RXLibrary *lib, uint64_t id, const uint8_t *data, size_t size)
{
    RXTrack *t = rx_library_find_id(lib, id);
    if (!t)
        return RX_ERROR(ENOENT);
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_WAVE_PUT);
    rx_db_bind_int(lib->db, st, 1, (int64_t)id);
    rx_db_bind_blob(lib->db, st, 2, data, size);
    int ret = rx_db_done(lib->db, st, "saving waveforms");
    if (ret < 0)
        return ret;
    t->has_waveforms = true;
    lib->generation++;
    return 0;
}

int rx_core_waveform_blob_get(const RXLibrary *lib, uint64_t id, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_WAVE_GET);
    rx_db_bind_int(lib->db, st, 1, (int64_t)id);
    int ret = rx_db_row(lib->db, st, "loading waveforms");
    if (ret <= 0)
        return ret < 0 ? ret : RX_ERROR(ENOENT);
    size_t n = (size_t)sqlite3_column_bytes(st, 0);
    const void *blob = sqlite3_column_blob(st, 0);
    uint8_t *copy = malloc(n ? n : 1);
    if (copy && n)
        memcpy(copy, blob, n);
    sqlite3_reset(st);
    if (!copy)
        return RX_ERROR(ENOMEM);
    *data = copy;
    *size = n;
    return 0;
}

int rx_core_waveform_blob_head(const RXLibrary *lib, uint64_t id, uint8_t *head, size_t size)
{
    /* Incremental blob I/O reads the pages the bytes are on and no more:
     * the rest of a blob of a few hundred kB stays on disk. track_id is
     * the waveform table's rowid. A missing row is SQLITE_ERROR. */
    sqlite3_blob *b = NULL;
    int rc = sqlite3_blob_open(lib->db->db, "main", "waveform", "data", (sqlite3_int64)id, 0, &b);
    if (rc == SQLITE_ERROR) {
        sqlite3_blob_close(b);
        return RX_ERROR(ENOENT);
    }
    if (rc != SQLITE_OK) {
        sqlite3_blob_close(b);
        return rx_db_error(lib->db, rc, "reading a waveform header");
    }
    int ret = 0;
    if (size > INT_MAX || sqlite3_blob_bytes(b) < (int)size)
        ret = RX_ERROR(EINVAL);
    else if ((rc = sqlite3_blob_read(b, head, (int)size, 0)) != SQLITE_OK)
        ret = rx_db_error(lib->db, rc, "reading a waveform header");
    sqlite3_blob_close(b);
    return ret;
}

bool rx_library_has_waveforms(const RXLibrary *lib, uint64_t id)
{
    const RXTrack *t = rx_library_find_id(lib, id);
    return t && t->has_waveforms;
}

int rx_library_artwork_save(RXLibrary *lib, uint64_t id, const uint8_t *jpeg, size_t size,
                            int width, int height)
{
    if (!rx_library_find_id(lib, id))
        return RX_ERROR(ENOENT);
    if (!jpeg || !size || width <= 0 || height <= 0)
        return RX_ERROR(EINVAL);
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_ART_PUT);
    rx_db_bind_int(lib->db, st, 1, (int64_t)id);
    rx_db_bind_blob(lib->db, st, 2, jpeg, size);
    rx_db_bind_int(lib->db, st, 3, width);
    rx_db_bind_int(lib->db, st, 4, height);
    int ret = rx_db_done(lib->db, st, "saving artwork");
    if (ret >= 0)
        lib->generation++;
    return ret;
}

int rx_library_artwork_load(const RXLibrary *lib, uint64_t id, uint8_t **jpeg, size_t *size,
                            int *width, int *height)
{
    *jpeg = NULL;
    *size = 0;
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_ART_GET);
    rx_db_bind_int(lib->db, st, 1, (int64_t)id);
    int ret = rx_db_row(lib->db, st, "loading artwork");
    if (ret <= 0)
        return ret < 0 ? ret : RX_ERROR(ENOENT);
    size_t n = (size_t)sqlite3_column_bytes(st, 0);
    uint8_t *copy = malloc(n ? n : 1);
    if (copy && n)
        memcpy(copy, sqlite3_column_blob(st, 0), n);
    if (width)
        *width = sqlite3_column_int(st, 1);
    if (height)
        *height = sqlite3_column_int(st, 2);
    sqlite3_reset(st);
    if (!copy)
        return RX_ERROR(ENOMEM);
    *jpeg = copy;
    *size = n;
    return 0;
}

char *rx_library_cache_path(const RXLibrary *lib, const char *kind, uint64_t id, const char *ext)
{
    char *dir = rx_asprintf("%s/cache/%s", lib->root, kind);
    if (!dir)
        return NULL;
    int ret = rx_mkdir_p(dir);
    if (ret < 0) {
        rx_err("cannot create %s: %s", dir, strerror(-ret));
        free(dir);
        return NULL;
    }
    if (ext && ext[0] == '.')
        ext++;
    char *path = rx_asprintf("%s/%llu%s%s", dir, (unsigned long long)id,
                             ext && *ext ? "." : "", ext ? ext : "");
    free(dir);
    return path;
}
