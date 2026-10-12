/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The SQLite library (src/core/library.c, db.c, config.c): a library made
 * in a scratch directory from real WAV files, edited, closed and opened
 * again, and checked field by field, then with SQLite's own integrity
 * and foreign key checks. Waveforms are stored through the raw blob layer
 * because the analysis module (which packs them) is not linked here. */

#include "rxbox/library.h"

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

static void add_cb(void *opaque, const char *path, RxTrack *t, int err)
{
    AddLog *log = opaque;
    if (err < 0)
        log->failed++;
    else if (t && !strcmp(t->path, path))
        log->reported++;
}

/* Rows of a table for one track, straight from SQLite. */
static int count_rows(const RxLibrary *lib, const char *table, uint64_t id)
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
    RxLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    char *db = path_in(root, "library.db");
    CHECK(rx_file_exists(db));
    free(db);
    CHECK(lib.nb_tracks == 0 && lib.root_folder && !lib.root_folder->nb_playlists);
    CHECK(lib.cfg.snap_bpm && lib.cfg.auto_cues && !strcmp(lib.cfg.key_notation, "classic"));
    CHECK(lib.cfg.export_pdb && lib.cfg.export_onelibrary && lib.cfg.detail_bars == 12);
    CHECK(!strcmp(lib.cfg.wave_palette, "3band"));

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
        RxTrack *a = lib.tracks[0], *b = lib.tracks[1], *c = lib.tracks[2];
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
    RxLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    CHECK(lib.nb_tracks == 3);
    RxTrack *a = rx_library_find_id(&lib, 1);
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

    RxCue c;
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
    lib.cfg.player = calloc(2, sizeof(RxKv));
    lib.cfg.player[0] = (RxKv) { rx_strdup("quantize"), rx_strdup("true") };
    lib.cfg.player[1] = (RxKv) { rx_strdup("tempo_range"), rx_strdup("10") };
    lib.cfg.nb_player = 2;
    lib.cfg.device = calloc(1, sizeof(RxKv));
    lib.cfg.device[0] = (RxKv) { rx_strdup("waveform_color"), rx_strdup("3band") };
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
    RxTrack *b = rx_library_find_id(&lib, 2);
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

    const RxConfig *cfg = &lib.cfg;
    CHECK_STR(cfg->key_notation, "camelot");
    CHECK(cfg->bpm_min == 70 && cfg->bpm_max == 180.5 && !cfg->snap_bpm && cfg->auto_cues);
    CHECK(cfg->compat_transcode && cfg->export_pdb && cfg->detail_bars == 16 && cfg->split_tree == 0.25f);
    CHECK_STR(cfg->device_name, "MY STICK");
    CHECK_STR(cfg->audio_sink, "rxbox-null");
    CHECK_STR(cfg->wave_palette, "rgb");
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
    RxLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    RxTrack *a = rx_library_find_id(&lib, 1);
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
    RxLibrary lib;
    char *b_path = path_in(music, "b.wav");
    CHECK(unlink(b_path) == 0);
    CHECK(rx_library_open(&lib, root) == 0);
    RxTrack *b = rx_library_find_id(&lib, 2);
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
    RxTrack *c = rx_library_find_id(&lib, 3);
    char *c_old = rx_strdup(c->path), *c_new = path_in(music, "sub/c moved.wav");
    CHECK(rename(c_old, c_new) == 0);
    CHECK(rx_library_rescan_missing(&lib) == 1 && c->missing);
    CHECK(rx_library_relocate(&lib, c, c_new) == 0);
    CHECK(!c->missing && !strcmp(c->path, c_new));
    CHECK(rx_library_relocate(&lib, c, rx_library_find_id(&lib, 1)->path) == RX_ERROR(EEXIST));
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, root) == 0);
    c = rx_library_find_id(&lib, 3);
    CHECK(c && !strcmp(c->path, c_new) && !c->missing);

    /* Removing takes everything that refers to the track. */
    RxTrack *a = rx_library_find_id(&lib, 1);
    RxPlaylist *pl = rx_library_playlist_create(&lib, NULL, "Set");
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
    RxTrack *stranger = rx_track_new("/not/in/the/library.wav");
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
    RxTrack *t = rx_library_add_file(&lib, d, &err);
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
    RxLibrary lib;
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

    if (!test_failures)
        test_rm_rf(dir);
    free(music);
    free(root);
    free(dir);
    return test_done("core_library");
}
