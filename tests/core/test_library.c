/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The SQLite library (src/core/library.c, db.c, config.c): a library made
 * in a scratch directory from real WAV files, edited, closed and opened
 * again, and checked field by field, then with SQLite's own integrity
 * and foreign key checks. Waveforms are stored through the raw blob layer
 * because the analysis module (which packs them) is not linked here. */

#include "rxbox/library.h"

#include <pthread.h>
#include <unistd.h>

#include <sqlite3.h>

#include "src/core/core.h"
#include "util.h"

static char *music, *root;

static char *path_in(const char *dir, const char *name)
{
    return rx_asprintf("%s/%s", dir, name);
}

typedef struct AddLog {
    int reported, failed;
} AddLog;

static void add_cb(void *opaque, const char *path, RXTrack *t, int err)
{
    AddLog *log = opaque;
    if (err < 0)
        log->failed++;
    else if (t && !strcmp(t->path, path))
        log->reported++;
}

/* Rows of a table for one track, straight from SQLite. */
static int count_rows(const RXLibrary *lib, const char *table, uint64_t id)
{
    char *sql = rx_asprintf("SELECT count(*) FROM %s WHERE track_id = %llu", table, (unsigned long long)id);
    sqlite3_stmt *st;
    int n = -1;
    if (sqlite3_prepare_v2(lib->db->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    free(sql);
    return n;
}

static void make_music(void)
{
    char *p;
    test_write_wav(p = path_in(music, "a.wav"), 1.0, 44100, 2, "Alpha", "Artist A", "2024-05-01");
    free(p);
    test_write_wav(p = path_in(music, "b.wav"), 0.5, 48000, 1, NULL, NULL, NULL);
    free(p);
    test_write_wav(p = path_in(music, "sub/c.wav"), 0.25, 44100, 2, "Gamma", NULL, "1999");
    free(p);
    test_write_wav(p = path_in(music, ".hidden/x.wav"), 0.1, 44100, 2, NULL, NULL, NULL);
    free(p);
    test_write_wav(p = path_in(music, "._d.wav"), 0.1, 44100, 2, NULL, NULL, NULL);
    free(p);
    FILE *f = fopen(p = path_in(music, "notes.txt"), "w");
    fputs("not audio\n", f);
    fclose(f);
    free(p);
}

static void test_create_and_add(void)
{
    RXLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    char *db = path_in(root, "library.db");
    CHECK(rx_file_exists(db));
    free(db);
    CHECK(lib.nb_tracks == 0 && lib.root_folder && !lib.root_folder->nb_playlists);
    CHECK(lib.cfg.snap_bpm && lib.cfg.auto_cues && !strcmp(lib.cfg.key_notation, "classic"));
    CHECK(lib.cfg.export_pdb && lib.cfg.export_onelibrary && lib.cfg.detail_bars == 12);
    CHECK(!strcmp(lib.cfg.wave_palette, "3band"));
    CHECK(!strcmp(lib.cfg.traffic_light, "related3"));

    sqlite3_stmt *st;
    sqlite3_prepare_v2(lib.db->db, "PRAGMA user_version", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(lib.db->db, "PRAGMA journal_mode", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "wal"));
    sqlite3_finalize(st);

    AddLog log = { 0 };
    int n = rx_library_add_path(&lib, music, add_cb, &log);
    CHECK(n == 3 && log.reported == 3 && !log.failed);
    CHECK(lib.nb_tracks == 3);
    if (lib.nb_tracks == 3) {
        RXTrack *a = lib.tracks[0], *b = lib.tracks[1], *c = lib.tracks[2];
        /* Ids in name order, within a directory before its subdirectories. */
        CHECK(a->id == 1 && b->id == 2 && c->id == 3);
        CHECK(rx_str_ends(a->path, "/a.wav") && rx_str_ends(b->path, "/b.wav") && rx_str_ends(c->path, "/sub/c.wav"));
        CHECK_STR(a->file_meta.title, "Alpha");
        CHECK_STR(a->meta.title, "Alpha");
        CHECK_STR(a->meta.artist, "Artist A");
        CHECK(a->file_meta.year == 2024 && a->meta.year == 2024);
        CHECK_STR(b->meta.title, "b");          /* dj: the file name without extension */
        CHECK(c->meta.year == 1999);
        CHECK_NEAR(a->duration, 1.0, 1e-3);
        CHECK(a->sample_rate == 44100 && a->channels == 2 && b->sample_rate == 48000 && b->channels == 1);
        CHECK_STR(a->codec, "pcm_s16le");
        CHECK(a->file_size > 0 && a->mtime > 0 && a->added > 0 && !a->missing);
    }

    /* Again: nothing new, every known file reported with its track. */
    log = (AddLog) { 0 };
    CHECK(rx_library_add_path(&lib, music, add_cb, &log) == 0);
    CHECK(lib.nb_tracks == 3 && log.reported == 3 && !log.failed);

    int err;
    char *p = path_in(music, "nope.wav");
    CHECK(!rx_library_add_file(&lib, p, &err) && err == RX_ERROR(ENOENT));
    free(p);
    p = path_in(music, "notes.txt");
    CHECK(!rx_library_add_file(&lib, p, &err) && err < 0);
    free(p);
    p = path_in(music, "a.wav");
    CHECK(rx_library_add_file(&lib, p, &err) == lib.tracks[0] && err == 0);
    free(p);
    CHECK(lib.nb_tracks == 3);
    CHECK(rx_is_audio_path("/x/y.FLAC") && rx_is_audio_path("a.opus") && !rx_is_audio_path("/x/._y.flac"));
    CHECK(!rx_is_audio_path("/x/y.txt") && !rx_is_audio_path("/x/flac"));
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);
}

static const uint8_t blob[] = { 'R', 'X', 'W', 'V', 0, 1, 2, 3, 255 };
static const uint8_t jpeg[] = { 0xff, 0xd8, 0xff, 0xe0, 'J', 'P', 'E', 'G', 0xff, 0xd9 };

static void test_edit_and_reopen(void)
{
    RXLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    CHECK(lib.nb_tracks == 3);
    RXTrack *a = rx_library_find_id(&lib, 1);
    CHECK(a && rx_library_index_of(&lib, a) == 0);
    CHECK(rx_library_find_path(&lib, a->path) == a);
    if (!a)
        return;

    rx_meta_set(&a->meta.title, "Alpha (Edit)");
    rx_meta_set(&a->meta.comment, "hand edited");
    a->meta.track_number = 7;
    a->meta.rating = 4;
    snprintf(a->meta.color, sizeof(a->meta.color), "pink");
    rx_meta_add_tag(&a->meta, "Situation/Peak Time");
    rx_meta_add_tag(&a->meta, "vocal");
    snprintf(a->key, sizeof(a->key), "Am");
    a->key_confidence = 0.8;
    rx_grid_add_section(&a->grid, 0.3125, 128.0, 1);
    rx_grid_add_section(&a->grid, 120.123456789, 130.5, 0);
    a->grid_confidence = 0.97;
    a->grid_flags = 5;
    a->grid_locked = true;
    a->analysed = 1760000000;

    RXCue c;
    rx_cue_init(&c, 0.3125);
    rx_cue_attach_to_grid(&c, &a->grid);
    rx_track_add_cue(a, &c);
    rx_cue_init(&c, 12.5);
    c.hot = 0;
    snprintf(c.name, sizeof(c.name), "drop \"quoted\" 日本");
    snprintf(c.color, sizeof(c.color), "#ff0000");
    rx_track_add_cue(a, &c);
    rx_cue_init(&c, rx_grid_bar_time(&a->grid, 17, 1));
    c.loop_end = rx_grid_bar_time(&a->grid, 18, 1);
    c.active = true;
    rx_cue_attach_to_grid(&c, &a->grid);
    rx_track_add_cue(a, &c);
    CHECK(rx_library_save_track(&lib, a) == 0);

    CHECK(rx_library_record_play(&lib, a) == 0);
    CHECK(rx_library_record_play(&lib, a) == 0);
    CHECK(a->play_count == 2 && a->last_played > 0);

    lib.cfg.key_notation[0] = '\0';
    snprintf(lib.cfg.key_notation, sizeof(lib.cfg.key_notation), "camelot");
    lib.cfg.bpm_min = 70;
    lib.cfg.bpm_max = 180.5;
    lib.cfg.snap_bpm = false;
    lib.cfg.compat_transcode = true;
    lib.cfg.device_name = rx_strdup("MY STICK");
    lib.cfg.audio_sink = rx_strdup("rxbox-null");
    lib.cfg.detail_bars = 16;
    lib.cfg.split_tree = 0.25f;
    snprintf(lib.cfg.wave_palette, sizeof(lib.cfg.wave_palette), "rgb");
    snprintf(lib.cfg.traffic_light, sizeof(lib.cfg.traffic_light), "related1");
    lib.cfg.player = calloc(2, sizeof(RXKv));
    lib.cfg.player[0] = (RXKv) { rx_strdup("quantize"), rx_strdup("true") };
    lib.cfg.player[1] = (RXKv) { rx_strdup("tempo_range"), rx_strdup("10") };
    lib.cfg.nb_player = 2;
    lib.cfg.device = calloc(1, sizeof(RXKv));
    lib.cfg.device[0] = (RXKv) { rx_strdup("waveform_color"), rx_strdup("3band") };
    lib.cfg.nb_device = 1;
    CHECK(rx_library_config_save(&lib) == 0);
    CHECK(rx_library_setting_set(&lib, "ui.columns", "title,artist,bpm") == 0);
    char *v;
    CHECK(rx_library_setting_get(&lib, "ui.columns", &v) == 0 && v && !strcmp(v, "title,artist,bpm"));
    free(v);
    CHECK(rx_library_setting_get(&lib, "ui.nothing", &v) == RX_ERROR(ENOENT) && !v);

    CHECK(rx_library_artwork_save(&lib, 1, jpeg, sizeof(jpeg), 240, 240) == 0);
    CHECK(rx_library_artwork_save(&lib, 99, jpeg, sizeof(jpeg), 240, 240) == RX_ERROR(ENOENT));
    CHECK(rx_core_waveform_blob_put(&lib, 1, blob, sizeof(blob)) == 0);
    CHECK(rx_library_has_waveforms(&lib, 1) && !rx_library_has_waveforms(&lib, 2));

    unsigned gen = lib.generation;
    RXTrack *b = rx_library_find_id(&lib, 2);
    b->meta.rating = 2;
    CHECK(rx_library_save_track(&lib, b) == 0 && lib.generation != gen);
    rx_library_close(&lib);

    /* Everything back from the disk. */
    CHECK(rx_library_open(&lib, root) == 0);
    a = rx_library_find_id(&lib, 1);
    CHECK(a != NULL);
    if (!a)
        return;
    CHECK_STR(a->meta.title, "Alpha (Edit)");
    CHECK_STR(a->file_meta.title, "Alpha");
    CHECK_STR(a->meta.comment, "hand edited");
    CHECK(!a->file_meta.comment);
    CHECK(a->meta.track_number == 7 && a->file_meta.track_number == 0);
    CHECK(a->meta.rating == 4 && !strcmp(a->meta.color, "pink"));
    CHECK(a->meta.nb_tags == 2 && !strcmp(a->meta.tags[0], "Situation/Peak Time") && !strcmp(a->meta.tags[1], "vocal"));
    CHECK_STR(a->key, "Am");
    CHECK(a->key_confidence == 0.8 && a->grid_confidence == 0.97);
    CHECK(a->grid_flags == 5 && a->grid_locked && a->analysed == 1760000000);
    CHECK(a->grid.nb_sections == 2 && a->grid.beats_per_bar == 4);
    if (a->grid.nb_sections == 2) {
        CHECK(a->grid.sections[0].start == 0.3125 && a->grid.sections[0].bpm == 128.0 && a->grid.sections[0].beat == 1);
        CHECK(a->grid.sections[1].start == 120.123456789 && a->grid.sections[1].bpm == 130.5 && a->grid.sections[1].beat == 0);
    }
    CHECK(a->nb_cues == 3);
    if (a->nb_cues == 3) {
        CHECK(a->cues[0].time == 0.3125 && a->cues[0].grid_beat == 0.0 && a->cues[0].hot == -1);
        CHECK(!rx_cue_is_loop(&a->cues[0]) && isnan(a->cues[0].loop_beats));
        CHECK(a->cues[1].time == 12.5 && isnan(a->cues[1].grid_beat) && a->cues[1].hot == 0);
        CHECK_STR(a->cues[1].name, "drop \"quoted\" 日本");
        CHECK_STR(a->cues[1].color, "#ff0000");
        CHECK(a->cues[2].grid_beat == 64.0 && a->cues[2].loop_beats == 4.0 && a->cues[2].active);
        CHECK(a->cues[2].time == rx_grid_bar_time(&a->grid, 17, 1));
        CHECK(a->cues[2].loop_end == rx_grid_bar_time(&a->grid, 18, 1));
    }
    CHECK(a->play_count == 2 && a->last_played > 0);
    CHECK(count_rows(&lib, "play", 1) == 2);
    CHECK(a->has_waveforms && rx_library_has_waveforms(&lib, 1));
    CHECK(rx_library_find_id(&lib, 2)->meta.rating == 2);

    const RXConfig *cfg = &lib.cfg;
    CHECK_STR(cfg->key_notation, "camelot");
    CHECK(cfg->bpm_min == 70 && cfg->bpm_max == 180.5 && !cfg->snap_bpm && cfg->auto_cues);
    CHECK(cfg->compat_transcode && cfg->export_pdb && cfg->detail_bars == 16 && cfg->split_tree == 0.25f);
    CHECK_STR(cfg->device_name, "MY STICK");
    CHECK_STR(cfg->audio_sink, "rxbox-null");
    CHECK_STR(cfg->wave_palette, "rgb");
    CHECK_STR(cfg->traffic_light, "related1");
    CHECK(cfg->nb_player == 2 && cfg->nb_device == 1 && cfg->nb_mixer == 0);
    if (cfg->nb_player == 2) {
        CHECK_STR(cfg->player[0].key, "quantize");
        CHECK_STR(cfg->player[1].key, "tempo_range");
        CHECK_STR(cfg->player[1].value, "10");
    }
    CHECK(rx_library_setting_get(&lib, "ui.columns", &v) == 0 && v && !strcmp(v, "title,artist,bpm"));
    free(v);

    uint8_t *data;
    size_t size;
    int w, h;
    CHECK(rx_library_artwork_load(&lib, 1, &data, &size, &w, &h) == 0);
    CHECK(size == sizeof(jpeg) && !memcmp(data, jpeg, size) && w == 240 && h == 240);
    free(data);
    CHECK(rx_library_artwork_load(&lib, 2, &data, &size, &w, &h) == RX_ERROR(ENOENT));
    CHECK(rx_core_waveform_blob_get(&lib, 1, &data, &size) == 0);
    CHECK(size == sizeof(blob) && !memcmp(data, blob, size));
    free(data);
    CHECK(rx_core_waveform_blob_get(&lib, 2, &data, &size) == RX_ERROR(ENOENT));

    /* A player-only key removed from the config goes from the database. */
    free(lib.cfg.player[1].key);
    free(lib.cfg.player[1].value);
    lib.cfg.nb_player = 1;
    free(lib.cfg.device_name);
    lib.cfg.device_name = NULL;
    CHECK(rx_library_config_save(&lib) == 0);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, root) == 0);
    CHECK(lib.cfg.nb_player == 1 && !lib.cfg.device_name);
    rx_library_close(&lib);
}

/* The file is retagged: fields the user did not change follow it. */
static void test_refresh(void)
{
    RXLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    RXTrack *a = rx_library_find_id(&lib, 1);
    char *p = path_in(music, "a.wav");
    sleep(1);       /* a new mtime */
    test_write_wav(p, 2.0, 44100, 2, "Alpha 2", "Artist B", "2025");
    free(p);
    CHECK(rx_library_refresh_track(&lib, a) == 0);
    CHECK_STR(a->file_meta.title, "Alpha 2");
    CHECK_STR(a->meta.title, "Alpha (Edit)");
    CHECK_STR(a->meta.artist, "Artist B");
    CHECK(a->meta.year == 2025 && a->meta.track_number == 7);
    CHECK_NEAR(a->duration, 2.0, 1e-3);
    CHECK(a->nb_cues == 3 && a->grid.nb_sections == 2 && a->meta.rating == 4);

    /* Taking an override back makes the field follow the file again. */
    rx_meta_set(&a->meta.title, a->file_meta.title);
    CHECK(rx_library_save_track(&lib, a) == 0);
    rx_library_close(&lib);

    CHECK(rx_library_open(&lib, root) == 0);
    a = rx_library_find_id(&lib, 1);
    CHECK_STR(a->meta.title, "Alpha 2");
    CHECK_STR(a->meta.artist, "Artist B");
    CHECK(count_rows(&lib, "cue", 1) == 3);
    rx_library_close(&lib);
}

static void test_missing_relocate_remove(void)
{
    RXLibrary lib;
    char *b_path = path_in(music, "b.wav");
    CHECK(unlink(b_path) == 0);
    CHECK(rx_library_open(&lib, root) == 0);
    RXTrack *b = rx_library_find_id(&lib, 2);
    CHECK(b && b->missing && b->meta.rating == 2);
    CHECK(rx_library_rescan_missing(&lib) == 0);
    rx_library_close(&lib);

    /* Still flagged after a reopen, back when the file is. */
    CHECK(rx_library_open(&lib, root) == 0);
    b = rx_library_find_id(&lib, 2);
    CHECK(b && b->missing);
    test_write_wav(b_path, 0.5, 48000, 1, NULL, NULL, NULL);
    CHECK(rx_library_rescan_missing(&lib) == 1);
    CHECK(!b->missing);
    /* Adding a missing track's file brings it back too. */
    unlink(b_path);
    CHECK(rx_library_rescan_missing(&lib) == 1 && b->missing);
    test_write_wav(b_path, 0.5, 48000, 1, NULL, NULL, NULL);
    int err;
    CHECK(rx_library_add_file(&lib, b_path, &err) == b && !b->missing);

    /* The user moved a file. */
    RXTrack *c = rx_library_find_id(&lib, 3);
    char *c_old = rx_strdup(c->path), *c_new = path_in(music, "sub/c moved.wav");
    CHECK(rename(c_old, c_new) == 0);
    CHECK(rx_library_rescan_missing(&lib) == 1 && c->missing);
    CHECK(rx_library_relocate(&lib, c, c_new) == 0);
    CHECK(!c->missing && !strcmp(c->path, c_new));
    CHECK(rx_library_relocate(&lib, c, rx_library_find_id(&lib, 1)->path) == RX_ERROR(EEXIST));
    /* The same file under another spelling is still that track's file:
     * two tracks must never share one. */
    char *a_other = path_in(music, "sub/../a.wav");
    CHECK(rx_library_relocate(&lib, c, a_other) == RX_ERROR(EEXIST));
    CHECK(!strcmp(c->path, c_new));
    char *a_link = path_in(music, "sub/link to a.wav");
    CHECK(symlink(rx_library_find_id(&lib, 1)->path, a_link) == 0);
    CHECK(rx_library_relocate(&lib, c, a_link) == RX_ERROR(EEXIST));
    unlink(a_link);
    free(a_link);
    free(a_other);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, root) == 0);
    c = rx_library_find_id(&lib, 3);
    CHECK(c && !strcmp(c->path, c_new) && !c->missing);

    /* Removing takes everything that refers to the track. */
    RXTrack *a = rx_library_find_id(&lib, 1);
    RXPlaylist *pl = rx_library_playlist_create(&lib, NULL, "Set");
    CHECK(pl && rx_library_playlist_add(&lib, pl, a, -1) == 0);
    CHECK(rx_library_playlist_add(&lib, pl, c, -1) == 0);
    char *cache = rx_library_cache_path(&lib, "transcode", 1, "flac");
    CHECK(cache && rx_str_ends(cache, "/cache/transcode/1.flac"));
    FILE *f = fopen(cache, "w");
    fclose(f);
    CHECK(rx_library_remove(&lib, a) == 0);
    CHECK(!rx_library_find_id(&lib, 1) && lib.nb_tracks == 2);
    CHECK(!rx_file_exists(cache));
    CHECK(pl->nb == 1 && pl->track_ids[0] == 3);
    static const char *const tables[] = { "cue", "grid_section", "track_tag", "waveform", "artwork", "play", "playlist_entry" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tables); i++) {
        if (count_rows(&lib, tables[i], 1) != 0) {
            fprintf(stderr, "%s still has rows of a removed track\n", tables[i]);
            test_failures++;
        }
    }
    RXTrack *stranger = rx_track_new("/not/in/the/library.wav");
    stranger->id = 1;
    CHECK(rx_library_remove(&lib, stranger) == RX_ERROR(ENOENT));
    CHECK(rx_library_save_track(&lib, stranger) == RX_ERROR(ENOENT));
    rx_track_free(stranger);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);

    CHECK(rx_library_open(&lib, root) == 0);
    pl = rx_library_playlist_find(&lib, "Set");
    CHECK(pl && pl->nb == 1 && pl->track_ids[0] == 3);

    /* Ids are never handed out twice: remove the newest, add another. */
    c = rx_library_find_id(&lib, 3);
    CHECK(rx_library_remove(&lib, c) == 0);
    char *d = path_in(music, "d.wav");
    test_write_wav(d, 0.25, 44100, 2, NULL, NULL, NULL);
    RXTrack *t = rx_library_add_file(&lib, d, &err);
    CHECK(t && t->id == 4);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);
    free(d);
    free(cache);
    free(c_old);
    free(c_new);
    free(b_path);
}

/* A database from a newer rxbox is left alone; a foreign one too. */
static void test_refusals(void)
{
    char *dir = test_dir("library-newer");
    char *db = path_in(dir, "library.db");
    sqlite3 *s;
    sqlite3_open(db, &s);
    char sql[96];
    snprintf(sql, sizeof(sql), "PRAGMA application_id = %d; PRAGMA user_version = 99", 0x52584258);
    sqlite3_exec(s, sql, NULL, NULL, NULL);
    sqlite3_close(s);
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == RX_ERROR(EPROTO));
    free(db);
    test_rm_rf(dir);
    free(dir);

    dir = test_dir("library-foreign");
    db = path_in(dir, "library.db");
    sqlite3_open(db, &s);
    sqlite3_exec(s, "PRAGMA application_id = 7; CREATE TABLE x (y)", NULL, NULL, NULL);
    sqlite3_close(s);
    CHECK(rx_library_open(&lib, dir) == RX_ERROR(EINVAL));
    sqlite3_open(db, &s);
    sqlite3_exec(s, "PRAGMA application_id = 0", NULL, NULL, NULL);
    sqlite3_close(s);
    CHECK(rx_library_open(&lib, dir) == RX_ERROR(EINVAL));
    /* Refused before anything is written: still in its own journal mode. */
    sqlite3_open(db, &s);
    sqlite3_stmt *st;
    sqlite3_prepare_v2(s, "PRAGMA journal_mode", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "delete"));
    sqlite3_finalize(st);
    sqlite3_close(s);
    free(db);
    test_rm_rf(dir);
    free(dir);

    /* $RXBOX_LIBRARY, else ~/.local/share/rxbox. */
    setenv("RXBOX_LIBRARY", "/some/where", 1);
    char *r = rx_library_default_root();
    CHECK_STR(r, "/some/where");
    free(r);
    unsetenv("RXBOX_LIBRARY");
    setenv("HOME", "/home/someone", 1);
    r = rx_library_default_root();
    CHECK_STR(r, "/home/someone/.local/share/rxbox");
    free(r);
}

/* SQLite rolls a whole transaction back by itself on some errors (a write
 * interrupted before its statement journal opened, SQLITE_FULL or
 * SQLITE_IOERR without one). A batch that loses its transaction that way
 * must not go on writing in autocommit mode: every file of it is reported
 * failed, so none of them may be in the database afterwards either. The
 * statement after the second track row is interrupted as it starts. */
typedef struct Interrupt {
    int inserts;
    bool armed;
    int fired;
} Interrupt;

static void count_inserts(void *opaque, int op, const char *db, const char *table, sqlite3_int64 rowid)
{
    Interrupt *in = opaque;
    if (op == SQLITE_INSERT && !strcmp(table, "track"))
        in->inserts++;
}

static int arm_at_statement(unsigned type, void *opaque, void *stmt, void *sql)
{
    Interrupt *in = opaque;
    if (in->inserts == 2 && !in->fired)
        in->armed = true;
    return 0;
}

static int interrupt_once(void *opaque)
{
    Interrupt *in = opaque;
    if (!in->armed)
        return 0;
    in->armed = false;
    in->fired++;
    return 1;
}

static int track_rows(const RXLibrary *lib)
{
    sqlite3_stmt *st;
    int n = -1;
    if (sqlite3_prepare_v2(lib->db->db, "SELECT count(*) FROM track", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

static void test_lost_transaction(void)
{
    char *dir = test_dir("library-lost");
    char *files = path_in(dir, "music"), *lroot = path_in(dir, "lib");
    static const char *const names[] = { "a.wav", "b.wav", "c.wav", "d.wav" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(names); i++) {
        char *p = path_in(files, names[i]);
        test_write_wav(p, 0.1, 44100, 1, NULL, NULL, NULL);
        free(p);
    }

    RXLibrary lib;
    CHECK(rx_library_open(&lib, lroot) == 0);
    Interrupt in = { 0 };
    sqlite3_update_hook(lib.db->db, count_inserts, &in);
    sqlite3_trace_v2(lib.db->db, SQLITE_TRACE_STMT, arm_at_statement, &in);
    sqlite3_progress_handler(lib.db->db, 1, interrupt_once, &in);
    AddLog log = { 0 };
    int n = rx_library_add_path(&lib, files, add_cb, &log);
    sqlite3_progress_handler(lib.db->db, 0, NULL, NULL);
    sqlite3_trace_v2(lib.db->db, 0, NULL, NULL);
    sqlite3_update_hook(lib.db->db, NULL, NULL);
    CHECK(in.fired == 1);
    CHECK(n < 0 && lib.nb_tracks == 0 && log.failed == 4 && !log.reported);
    CHECK(track_rows(&lib) == 0);
    CHECK(lib.db->depth == 0);

    /* And the library works on: the same files go in normally. */
    log = (AddLog) { 0 };
    CHECK(rx_library_add_path(&lib, files, add_cb, &log) == 4 && lib.nb_tracks == 4);
    CHECK(track_rows(&lib) == 4 && log.reported == 4);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, lroot) == 0 && lib.nb_tracks == 4);
    rx_library_close(&lib);

    test_rm_rf(dir);
    free(files);
    free(lroot);
    free(dir);
}

/* A Linux path is bytes, not necessarily UTF-8: such a file goes in, comes
 * back from the database byte for byte, and is found by its path. */
static void test_byte_paths(void)
{
    char *dir = test_dir("library-bytes");
    char *files = path_in(dir, "m\xe4sic"), *lroot = path_in(dir, "lib");
    char *p = path_in(files, "\xff\xfe caf\xe9 \x80.wav");
    test_write_wav(p, 0.1, 44100, 1, NULL, NULL, NULL);

    RXLibrary lib;
    CHECK(rx_library_open(&lib, lroot) == 0);
    int err;
    RXTrack *t = rx_library_add_file(&lib, p, &err);
    CHECK(t && !strcmp(t->path, p) && !strcmp(t->meta.title, "\xff\xfe caf\xe9 \x80"));
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, lroot) == 0 && lib.nb_tracks == 1);
    t = rx_library_find_path(&lib, p);
    CHECK(t && !t->missing && !strcmp(t->meta.title, "\xff\xfe caf\xe9 \x80"));
    CHECK(rx_library_add_file(&lib, p, &err) == t && lib.nb_tracks == 1);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);

    test_rm_rf(dir);
    free(p);
    free(files);
    free(lroot);
    free(dir);
}

/* The export job opens the library on a connection of its own while the
 * interface's goes on committing edits. What the job loads must be one
 * moment of the database: here the interface adds a track to a playlist
 * just as the job starts reading the playlists, after it read the tracks.
 * (An auto extension hooks the job's connection as it is opened.) */
typedef struct Meddle {
    RXLibrary *ui;
    const char *path;
    bool done;
} Meddle;

static Meddle meddle;

static int meddle_at_playlists(unsigned type, void *opaque, void *stmt, void *sql)
{
    const char *text = sqlite3_sql(stmt);
    if (meddle.done || !text || !rx_str_starts(text, "SELECT id, parent_id"))
        return 0;
    meddle.done = true;
    int err;
    RXTrack *t = rx_library_add_file(meddle.ui, meddle.path, &err);
    RXPlaylist *p = rx_library_playlist_find(meddle.ui, "Set");
    if (t && p)
        rx_library_playlist_add(meddle.ui, p, t, -1);
    return 0;
}

static int hook_connection(sqlite3 *db, const char **err, const struct sqlite3_api_routines *api)
{
    sqlite3_trace_v2(db, SQLITE_TRACE_STMT, meddle_at_playlists, NULL);
    return SQLITE_OK;
}

static void test_open_snapshot(void)
{
    char *dir = test_dir("library-snapshot");
    char *files = path_in(dir, "music"), *lroot = path_in(dir, "lib");
    char *a = path_in(files, "a.wav"), *b = path_in(files, "b.wav");
    test_write_wav(a, 0.1, 44100, 1, NULL, NULL, NULL);
    test_write_wav(b, 0.1, 44100, 1, NULL, NULL, NULL);

    RXLibrary ui, job;
    CHECK(rx_library_open(&ui, lroot) == 0);
    int err;
    RXTrack *t = rx_library_add_file(&ui, a, &err);
    RXPlaylist *p = rx_library_playlist_create(&ui, NULL, "Set");
    CHECK(t && p && rx_library_playlist_add(&ui, p, t, -1) == 0);

    meddle = (Meddle) { .ui = &ui, .path = b };
    sqlite3_auto_extension((void (*)(void))hook_connection);
    CHECK(rx_library_open(&job, lroot) == 0);
    sqlite3_cancel_auto_extension((void (*)(void))hook_connection);
    CHECK(meddle.done && ui.nb_tracks == 2 && p->nb == 2);

    RXPlaylist *jp = rx_library_playlist_find(&job, "Set");
    CHECK(jp != NULL);
    for (int i = 0; jp && i < jp->nb; i++) {
        if (!rx_library_find_id(&job, jp->track_ids[i])) {
            fprintf(stderr, "the job's playlist holds track %llu, its track list does not\n",
                    (unsigned long long)jp->track_ids[i]);
            test_failures++;
        }
    }
    rx_library_close(&job);
    rx_library_close(&ui);

    test_rm_rf(dir);
    free(a);
    free(b);
    free(files);
    free(lroot);
    free(dir);
}

/* Two processes starting on a new library at once (the CLI and the
 * interface): whichever creates the schema, the other opens it. */
typedef struct Opener {
    const char *root;
    pthread_barrier_t *go;
    int ret;
} Opener;

static void *open_thread(void *arg)
{
    Opener *o = arg;
    RXLibrary lib;
    pthread_barrier_wait(o->go);
    o->ret = rx_library_open(&lib, o->root);
    if (o->ret >= 0)
        rx_library_close(&lib);
    return NULL;
}

static void test_concurrent_create(void)
{
    char *dir = test_dir("library-race");
    for (int round = 0; round < 8; round++) {
        char *lroot = rx_asprintf("%s/lib%d", dir, round);
        enum { N = 6 };
        pthread_barrier_t go;
        pthread_barrier_init(&go, NULL, N);
        pthread_t th[N];
        Opener o[N];
        for (int i = 0; i < N; i++) {
            o[i] = (Opener) { .root = lroot, .go = &go };
            pthread_create(&th[i], NULL, open_thread, &o[i]);
        }
        int failed = 0;
        for (int i = 0; i < N; i++) {
            pthread_join(th[i], NULL);
            failed += o[i].ret < 0;
        }
        pthread_barrier_destroy(&go);
        CHECK(!failed);
        free(lroot);
    }
    test_rm_rf(dir);
    free(dir);
}

int main(void)
{
    char *dir = test_dir("library");
    if (!dir)
        return 1;
    music = path_in(dir, "music");
    root = path_in(dir, "lib");
    make_music();

    test_create_and_add();
    test_edit_and_reopen();
    test_refresh();
    test_missing_relocate_remove();
    test_refusals();
    test_lost_transaction();
    test_byte_paths();
    test_open_snapshot();
    test_concurrent_create();

    if (!test_failures)
        test_rm_rf(dir);
    free(music);
    free(root);
    free(dir);
    return test_done("core_library");
}
