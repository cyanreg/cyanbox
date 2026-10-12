/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The application without a window, on the null audio backend: a fresh
 * library in a scratch directory, two copies of a 128 BPM WAV added and
 * analysed through the jobs, the analysis applied (grid, waveforms, the
 * automatic cue), what still needs analysing, the rows (search, sort, a
 * playlist), loading the deck, rescanning, an export to a directory
 * running through its phases to the end, and reading and inspecting that
 * stick on a worker. Then the edges: an export answers exactly what its
 * dry run asked about although the library changed meanwhile, and asks
 * about a player's unsynced change and the files to delete at once; a
 * stick read given up on is dropped; a dry run cancelled as it ends
 * exports nothing; an analysis of a file gone since it was added fails
 * with the track named in the status; a load the player cannot take
 * keeps the track the deck has, the 30 s play record (not for a seek loop,
 * not twice), a track removed while loaded and analysed, waveforms of an
 * older format version made again, and tearing down with jobs in flight
 * (LeakSanitizer checks the rest).
 *
 * The player is the null backend in manual mode: the ticks advance it.
 *
 * Usage: test_app FILE.wav SCRATCH_DIR LONG.wav (LONG: 40 s) */

#include "rxbox/app.h"

#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include <sqlite3.h>

#include "rxbox/formats.h"

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);   \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    remove(path);
    return 0;
}

static void remove_tree(const char *dir)
{
    nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

static int copy_file(const char *from, const char *to)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int ret = rx_file_read(from, &data, &size);
    if (ret < 0)
        return ret;
    ret = rx_file_write_atomic(to, data, size);
    free(data);
    return ret;
}

/* Ticks (and plays 10 ms) until `done` says so, up to `seconds`. */
static bool tick_until(RxApp *app, bool (*done)(RxApp *app), int seconds)
{
    int64_t t0 = rx_time_us();
    while (rx_time_us() - t0 < seconds * 1000000LL) {
        rx_player_null_advance(app->player, 480);
        rx_app_tick(app);
        if (done(app))
            return true;
        usleep(2000);
    }
    return false;
}

/* Plays `seconds` of the deck in steps of `step`, ticking after each,
 * with `pace_us` of wall time per step; `back_from`/`back_to` (when > 0)
 * seek back whenever the playhead passes `back_from`. */
static void play_for(RxApp *app, double seconds, double step, int pace_us, double back_from,
                     double back_to)
{
    int frames = (int)lround(step * 48000);
    for (double done = 0; done < seconds; done += step) {
        rx_player_null_advance(app->player, frames);
        rx_app_tick(app);
        RxDeckState st;
        rx_deck_state(app->player, 0, &st);
        if (back_from > 0 && st.position >= back_from)
            rx_ctl_seek(&app->decks[0].ctl, back_to);
        if (pace_us)
            usleep(pace_us);
    }
}

/* Makes a track's stored waveforms format version 1 (bytes 4..7 of the
 * blob), as a library analysed before the PWV6/PWV7 recalibration holds
 * them. */
static int stale_waveforms(const char *root, uint64_t id)
{
    char *path = rx_path_join(root, "library.db");
    sqlite3 *db = NULL;
    int rc = path ? sqlite3_open(path, &db) : SQLITE_NOMEM;
    free(path);
    sqlite3_stmt *get = NULL, *put = NULL;
    uint8_t *data = NULL;
    int size = 0;
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(db, "SELECT data FROM waveform WHERE track_id = ?", -1, &get, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(get, 1, (sqlite3_int64)id);
        rc = sqlite3_step(get) == SQLITE_ROW ? SQLITE_OK : SQLITE_ERROR;
    }
    if (rc == SQLITE_OK) {
        size = sqlite3_column_bytes(get, 0);
        data = size > 8 ? malloc(size) : NULL;
        rc = data ? SQLITE_OK : SQLITE_ERROR;
    }
    if (rc == SQLITE_OK) {
        memcpy(data, sqlite3_column_blob(get, 0), size);
        data[4] = 1;
        data[5] = data[6] = data[7] = 0;
        rc = sqlite3_prepare_v2(db, "UPDATE waveform SET data = ? WHERE track_id = ?", -1, &put,
                                NULL);
    }
    if (rc == SQLITE_OK) {
        sqlite3_bind_blob(put, 1, data, size, SQLITE_TRANSIENT);
        sqlite3_bind_int64(put, 2, (sqlite3_int64)id);
        rc = sqlite3_step(put) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    }
    sqlite3_finalize(get);
    sqlite3_finalize(put);
    sqlite3_close(db);
    free(data);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Plays a player that changed a My Settings value on the stick since the
 * export: `key` in PIONEER/MYSETTING.DAT set to `value`. */
static int stick_setting(const char *stick, const char *key, const char *value)
{
    char *path = rx_path_join(stick, "PIONEER/MYSETTING.DAT");
    uint8_t *data = NULL, *out = NULL;
    size_t size = 0, out_size = 0;
    RxKv *kv = NULL;
    int nb = 0;
    int ret = path ? rx_file_read(path, &data, &size) : RX_ERROR(ENOMEM);
    if (ret >= 0)
        ret = rx_settings_decode("MYSETTING.DAT", data, size, &kv, &nb);
    if (ret >= 0) {
        ret = RX_ERROR(ENOENT);
        for (int i = 0; i < nb; i++) {
            if (strcmp(kv[i].key, key))
                continue;
            free(kv[i].value);
            kv[i].value = rx_strdup(value);
            ret = kv[i].value ? 0 : RX_ERROR(ENOMEM);
        }
    }
    if (ret >= 0)
        ret = rx_settings_encode("MYSETTING.DAT", kv, nb, &out, &out_size);
    if (ret >= 0)
        ret = rx_file_write_atomic(path, out, out_size);
    rx_kv_free(kv, nb);
    free(out);
    free(data);
    free(path);
    return ret;
}

static bool deck_idle(RxApp *app)
{
    return !app->decks[0].loading_request;
}

/* The stick copy of an audio file: the file under `dir` named `base`. */
static char *found_path;
static const char *found_base;

static int find_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    if (flag == FTW_F && !strcmp(path + ftw->base, found_base)) {
        found_path = rx_strdup(path);
        return 1;
    }
    return 0;
}

static char *find_file(const char *dir, const char *base)
{
    found_path = NULL;
    found_base = base;
    nftw(dir, find_one, 16, FTW_PHYS);
    return found_path;
}

static bool analysis_done(RxApp *app)
{
    return !app->analysing_total && !rx_jobs_pending(app->jobs);
}

static bool deck_loaded(RxApp *app)
{
    return app->decks[0].track && !app->decks[0].loading_request;
}

static bool export_done(RxApp *app)
{
    return !app->export_state.running || app->export_state.confirm;
}

static bool stick_done(RxApp *app)
{
    return !app->stick_request;
}

static bool export_cancel_done(RxApp *app)
{
    return !app->export_state.running;
}

static RxTrack *track_named(RxApp *app, const char *base)
{
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        if (!strcmp(rx_path_base(app->lib.tracks[i]->path), base))
            return app->lib.tracks[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: test_app FILE.wav SCRATCH_DIR LONG.wav\n");
        return 2;
    }
    const char *wav = argv[1];
    const char *dir = argv[2];
    const char *long_wav = argv[3];

    remove_tree(dir);
    char *music = rx_path_join(dir, "music");
    char *more = rx_path_join(dir, "more");
    char *root = rx_path_join(dir, "lib");
    char *stick = rx_path_join(dir, "stick");
    char *a_wav = rx_path_join(music, "a.wav");
    char *b_wav = rx_path_join(music, "b.wav");
    char *c_wav = rx_path_join(more, "long.wav");
    if (!music || !more || !root || !stick || !a_wav || !b_wav || !c_wav ||
        rx_mkdir_p(music) < 0 || rx_mkdir_p(more) < 0 || rx_mkdir_p(stick) < 0 ||
        copy_file(wav, a_wav) < 0 || copy_file(wav, b_wav) < 0 || copy_file(long_wav, c_wav) < 0) {
        fprintf(stderr, "cannot set up %s\n", dir);
        return 2;
    }

    /* Manual: nothing plays unless the test advances it. */
    RxPlayerOptions po;
    rx_player_options_default(&po);
    po.backend = RX_AUDIO_NULL;
    po.period = -480;

    char *a_on_stick = NULL;
    RxApp app;
    int ret = rx_app_create(&app, root, &po, NULL);
    CHECK(ret == 0, "rx_app_create: %s", strerror(-ret));
    if (ret < 0)
        return 1;
    CHECK(!app.window && app.player && app.jobs, "window %p player %p", (void *)app.window,
          (void *)app.player);
    CHECK(app.lib.nb_tracks == 0 && app.nb_rows == 0, "%d tracks in a new library",
          app.lib.nb_tracks);

    /* Adding queues the analysis; the ticks apply it. */
    ret = rx_app_add_path(&app, music);
    CHECK(ret == 2, "added %d", ret);
    CHECK(app.analysing_total == 2, "analysing %d", app.analysing_total);
    CHECK(tick_until(&app, analysis_done, 300), "the analysis did not finish");
    printf("status: %s\n", app.status);

    RxTrack *a = track_named(&app, "a.wav");
    RxTrack *b = track_named(&app, "b.wav");
    CHECK(a && b, "tracks a %p b %p", (void *)a, (void *)b);
    if (!a || !b)
        goto end;
    for (int i = 0; i < 2; i++) {
        RxTrack *t = i ? b : a;
        CHECK(t->has_waveforms, "%s has no waveforms", t->path);
        CHECK(t->analysed > 0, "%s not marked analysed", t->path);
        CHECK(fabs(t->duration - 12.0) < 1e-6, "%s lasts %f s", t->path, t->duration);
        CHECK(t->total_samples == 12 * 48000, "%s has %lld samples", t->path,
              (long long)t->total_samples);
        if (rx_grid_valid(&t->grid)) {
            printf("%s: %.3f BPM, key \"%s\", %d cue%s\n", rx_path_base(t->path),
                   rx_grid_bpm(&t->grid), t->key, t->nb_cues, t->nb_cues == 1 ? "" : "s");
            CHECK(!app.lib.cfg.auto_cues || t->nb_cues == 1, "%d cues after the analysis",
                  t->nb_cues);
        } else {
            fprintf(stderr, "warning: %s got no grid (the analyser?)\n", t->path);
        }
    }

    /* Nothing left to do unless forced; waveforms always count. */
    CHECK(rx_app_analysis_needed(&app, a, RX_ANALYSE_ALL, false) ==
          (rx_track_analysed(a) ? 0u : (unsigned)RX_ANALYSE_BEATS) +
          (a->key[0] ? 0u : (unsigned)RX_ANALYSE_KEY),
          "needed %#x", rx_app_analysis_needed(&app, a, RX_ANALYSE_ALL, false));
    CHECK(rx_app_analysis_needed(&app, a, RX_ANALYSE_ALL, true) == RX_ANALYSE_ALL, "forced");
    bool locked = a->grid_locked;
    a->grid_locked = true;
    CHECK(!(rx_app_analysis_needed(&app, a, RX_ANALYSE_BEATS, false) & RX_ANALYSE_BEATS),
          "a locked grid needs beats");
    a->grid_locked = locked;
    a->has_waveforms = false;
    CHECK(rx_app_analysis_needed(&app, a, RX_ANALYSE_KEY, false) & RX_ANALYSE_WAVES,
          "no waveforms and none needed");
    a->has_waveforms = true;
    a->missing = true;
    CHECK(rx_app_analysis_needed(&app, a, RX_ANALYSE_ALL, true) == 0, "a missing file needs work");
    CHECK(rx_app_analyse_track(&app, a, RX_ANALYSE_ALL, true) == RX_ERROR(ENOENT),
          "a missing file analysed");
    CHECK(rx_app_load_deck(&app, 0, a) == RX_ERROR(ENOENT), "a missing file loaded");
    a->missing = false;

    /* Rows: everything, a search on the file name, a sort. */
    rx_app_tick(&app);
    CHECK(app.nb_rows == 2, "%d rows", app.nb_rows);
    snprintf(app.search, sizeof(app.search), "B.WAV");
    rx_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 1 && app.lib.tracks[app.rows[0]] == b, "search: %d rows", app.nb_rows);
    snprintf(app.search, sizeof(app.search), "a.wav nothing");
    rx_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 0, "two words must both match: %d rows", app.nb_rows);
    app.search[0] = '\0';
    app.sort_column = RX_COL_PATH;
    app.sort_descending = true;
    rx_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == b, "sort by path descending");
    app.sort_descending = false;
    rx_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == a, "sort by path");

    /* A playlist shows its own tracks in its order. */
    RxPlaylist *p = rx_library_playlist_create(&app.lib, app.lib.root_folder, "Test");
    CHECK(p, "playlist");
    if (p) {
        CHECK(rx_library_playlist_add(&app.lib, p, b, -1) >= 0, "playlist add");
        app.current_playlist = p;
        app.sort_column = -1;
        rx_app_tick(&app);
        CHECK(app.nb_rows == 1 && app.lib.tracks[app.rows[0]] == b, "playlist rows %d",
              app.nb_rows);
        app.current_playlist = NULL;
        app.rows_dirty = true;
        rx_app_tick(&app);
    }

    /* The deck: decoded on the io queue, waveforms from the library. */
    ret = rx_app_load_deck(&app, 0, b);
    CHECK(ret == 0, "load: %s", strerror(-ret));
    CHECK(tick_until(&app, deck_loaded, 60), "the deck did not load");
    CHECK(app.decks[0].track == b, "deck holds %p", (void *)app.decks[0].track);
    CHECK(rx_deck_loaded(app.player, 0), "the player has no track");
    CHECK(app.decks[0].waves.nb_detail > 0, "the deck has no waveforms");
    rx_app_unload_deck(&app, 0);
    CHECK(!app.decks[0].track && !rx_deck_loaded(app.player, 0), "unload");

    CHECK(rx_app_rescan(&app) == 0, "rescan changed something");

    /* The export runs its phases to the end or to a question. */
    snprintf(app.export_state.dest, sizeof(app.export_state.dest), "%s", stick);
    ret = rx_app_start_export(&app);
    CHECK(ret == 0, "start export: %s", strerror(-ret));
    CHECK(tick_until(&app, export_done, 300), "the export did not finish");
    if (app.export_state.confirm) {
        printf("export asks: %d to prune, %d unsynced\n", app.export_state.nb_prune,
               app.export_state.unsynced);
        app.export_state.prune = false;
        app.export_state.ignore_unsynced = true;
        rx_app_start_export(&app);
        CHECK(tick_until(&app, export_done, 300), "the export did not finish");
    }
    printf("export: %s, result %d (%s)\n%s", app.export_state.stage, app.export_state.result,
           strerror(-app.export_state.result), app.export_state.log);
    CHECK(app.export_state.result >= 0, "the export failed");
    /* The "Test" playlist holds b alone. */
    CHECK(app.export_state.copied == 1 && app.export_state.skipped == 0 &&
          app.export_state.rewritten > 0, "counted %d copied, %d skipped, %d rewritten",
          app.export_state.copied, app.export_state.skipped, app.export_state.rewritten);

    /* The whole library: a joins b on the stick, and with a player setting
     * in the library, My Settings files too. */
    ret = rx_library_setting_set(&app.lib, "player.quantize", "true");
    CHECK(ret >= 0, "set player.quantize: %s", strerror(-ret));
    if (app.export_state.result >= 0) {
        app.export_state.whole_library = true;
        CHECK(rx_app_start_export(&app) == 0, "whole library export");
        CHECK(tick_until(&app, export_done, 300), "the whole library export did not finish");
        CHECK(!app.export_state.confirm && app.export_state.result >= 0 &&
              app.export_state.copied == 1 && app.export_state.skipped == 1,
              "whole library: %s, %d copied, %d skipped", strerror(-app.export_state.result),
              app.export_state.copied, app.export_state.skipped);
        app.export_state.whole_library = false;
    }
    a_on_stick = find_file(stick, "a.wav");
    CHECK(a_on_stick, "a.wav is not on the stick");

    /* Exporting only another playlist: the dry run finds b's files on the
     * stick, asks, and pruning on the answer deletes them, and only them:
     * a taken out of the playlist while the question is open is still
     * exported as the dry run had it (the snapshot), not pruned unasked.
     * A player changed a setting on the stick meanwhile: the one question
     * covers that change and the files to delete, and one answer to both
     * runs the export. */
    RxPlaylist *other = rx_library_playlist_create(&app.lib, app.lib.root_folder, "Other");
    if (app.export_state.result >= 0 && other && rx_library_playlist_add(&app.lib, other, a, -1) >= 0) {
        char *pdb = rx_path_join(stick, "PIONEER/rekordbox/export.pdb");
        CHECK(pdb && rx_file_exists(pdb), "no export.pdb on the stick");
        free(pdb);

        ret = stick_setting(stick, "quantize", "false");
        CHECK(ret >= 0, "change the stick's quantize setting: %s", strerror(-ret));

        RxPlaylist *only[1] = { other };
        app.export_state.playlists = only;
        app.export_state.nb_playlists = 1;
        app.export_state.prune = false;
        app.export_state.ignore_unsynced = false;
        CHECK(rx_app_start_export(&app) == 0, "second export");
        CHECK(tick_until(&app, export_done, 300), "the second export did not finish");
        CHECK(app.export_state.confirm && app.export_state.nb_prune > 0,
              "no question about pruning (confirm %d, %d to prune)", app.export_state.confirm,
              app.export_state.nb_prune);
        CHECK(app.export_state.confirm && app.export_state.unsynced == (ret >= 0),
              "the question counts %d unsynced changes", app.export_state.unsynced);
        printf("export asks: %d to prune, %d unsynced\n%s", app.export_state.nb_prune,
               app.export_state.unsynced, app.export_state.log);
        bool b_listed = false;
        for (int i = 0; i < app.export_state.nb_prune; i++)
            b_listed |= rx_str_ends(app.export_state.prune_list[i], "/b.wav");
        CHECK(b_listed, "b.wav is not in the prune list");
        if (app.export_state.confirm) {
            CHECK(rx_library_playlist_remove_at(&app.lib, other, 0) >= 0, "playlist remove");
            app.export_state.prune = true;
            app.export_state.ignore_unsynced = true;
            CHECK(rx_app_start_export(&app) == 0, "confirm");
            CHECK(tick_until(&app, export_done, 300), "the pruning export did not finish");
            CHECK(!app.export_state.confirm && app.export_state.result >= 0,
                  "pruning export (confirm %d, asked again?): %s", app.export_state.confirm,
                  strerror(-app.export_state.result));
            CHECK(app.export_state.pruned == app.export_state.nb_prune, "%d pruned of %d",
                  app.export_state.pruned, app.export_state.nb_prune);
            for (int i = 0; i < app.export_state.nb_prune; i++) {
                char *f = rx_path_join(stick, app.export_state.prune_list[i] + 1);
                CHECK(f && !rx_file_exists(f), "%s was not pruned", f);
                free(f);
            }
            CHECK(a_on_stick && rx_file_exists(a_on_stick),
                  "%s was deleted although the confirmation did not list it", a_on_stick);
        }
        app.export_state.playlists = NULL;
        app.export_state.nb_playlists = 0;
        app.export_state.ignore_unsynced = false;
    }

    /* The stick read on a worker, one at a time: nothing changed on the
     * stick since the export. */
    ret = rx_app_read_stick(&app, stick);
    CHECK(ret == 0, "read the stick: %s", strerror(-ret));
    CHECK(rx_app_read_stick(&app, stick) == RX_ERROR(EBUSY), "a second read while one runs");
    CHECK(tick_until(&app, stick_done, 60), "the stick read did not finish");
    RxJobResult *r = app.stick_result;
    app.stick_result = NULL;
    CHECK(r && r->kind == RX_JOB_STICK_READ && !r->err && r->stick, "stick read: %s",
          r ? strerror(-r->err) : "no result");
    if (r && r->stick)
        CHECK(r->stick->nb_tracks > 0 && r->nb_sync_items == 0, "%d tracks, %d changes",
              r->stick->nb_tracks, r->nb_sync_items);
    rx_job_result_free(r);

    ret = rx_app_inspect_stick(&app, stick);
    CHECK(ret == 0, "inspect the stick: %s", strerror(-ret));
    CHECK(tick_until(&app, stick_done, 60), "the inspection did not finish");
    r = app.stick_result;
    app.stick_result = NULL;
    CHECK(r && r->kind == RX_JOB_INSPECT && !r->err && r->report && !r->problems,
          "inspect: %s, %d problems", r ? strerror(-r->err) : "no result", r ? r->problems : -1);
    if (r && r->report)
        printf("inspect:\n%s", r->report);
    rx_job_result_free(r);

    /* A read given up on (its dialog closed): another may start at once,
     * and the result that comes is the new one's; one given up on after
     * it finished is freed. */
    ret = rx_app_read_stick(&app, stick);
    uint64_t gave_up = app.stick_request;
    CHECK(ret == 0 && gave_up, "read the stick: %s", strerror(-ret));
    rx_app_cancel_stick(&app, gave_up);
    CHECK(!app.stick_request, "the read is still in flight after giving it up");
    ret = rx_app_read_stick(&app, stick);
    uint64_t wanted = app.stick_request;
    CHECK(ret == 0 && wanted != gave_up, "read again: %s", strerror(-ret));
    CHECK(tick_until(&app, stick_done, 60), "the second read did not finish");
    for (int i = 0; i < 50; i++) {
        rx_app_tick(&app);
        usleep(2000);
    }
    CHECK(app.stick_result && app.stick_result->request == wanted,
          "the result is request %llu, not %llu",
          app.stick_result ? (unsigned long long)app.stick_result->request : 0ULL,
          (unsigned long long)wanted);
    rx_app_cancel_stick(&app, wanted);
    CHECK(!app.stick_result, "a finished read given up on kept its result");

    /* A dry run cancelled after it got to its end: nothing was exported
     * and the real run never starts. The dry run is given time to finish
     * before the cancel is seen. */
    ret = rx_app_start_export(&app);
    CHECK(ret == 0, "start export: %s", strerror(-ret));
    rx_app_tick(&app);
    rx_app_tick(&app);
    usleep(3000000);
    app.export_state.cancel = true;
    CHECK(tick_until(&app, export_cancel_done, 60), "the cancelled export did not stop");
    CHECK(app.export_state.result == RX_ERROR(ECANCELED) &&
          !strcmp(app.export_state.stage, "cancelled"),
          "a cancelled dry run ended as \"%s\" (%s)", app.export_state.stage,
          strerror(-app.export_state.result));

    /* A load the player cannot take (its command queue full, as when the
     * output stalls) keeps the track the deck has. */
    ret = rx_app_load_deck(&app, 0, b);
    CHECK(ret == 0 && tick_until(&app, deck_loaded, 60) && app.decks[0].track == b, "load b");
    ret = rx_app_load_deck(&app, 0, a);
    CHECK(ret == 0, "load a: %s", strerror(-ret));
    for (int i = 0; i < 3000; i++)
        rx_deck_seek(app.player, 0, 1.0);
    for (int64_t t0 = rx_time_us(); !deck_idle(&app) && rx_time_us() - t0 < 60000000;) {
        rx_app_tick(&app);
        usleep(2000);
    }
    CHECK(app.decks[0].track == b && app.decks[0].ctl.track == b && rx_deck_loaded(app.player, 0),
          "a failed load emptied the deck (track %p, b %p)", (void *)app.decks[0].track, (void *)b);
    printf("status: %s\n", app.status);
    for (int i = 0; i < 100; i++)
        rx_player_null_advance(app.player, 480);
    rx_app_unload_deck(&app, 0);

    /* The play record. A 40 s track, played in a loop between 10 and 15 s
     * for over 30 s of wall time, is not a play; 31 s straight through
     * then is, once. */
    ret = rx_app_add_path(&app, more);
    CHECK(ret == 1, "added %d", ret);
    CHECK(tick_until(&app, analysis_done, 300), "the analysis of the long track did not finish");
    RxTrack *c = track_named(&app, "long.wav");
    CHECK(c && c->duration > 39, "long.wav: %f s", c ? c->duration : 0.0);
    if (c) {
        ret = rx_app_load_deck(&app, 0, c);
        CHECK(ret == 0 && tick_until(&app, deck_loaded, 60) && app.decks[0].track == c,
              "load the long track");
        rx_ctl_seek(&app.decks[0].ctl, 10.0);
        rx_ctl_play_pause(&app.decks[0].ctl);
        play_for(&app, 31.0, 0.01, 10000, 15.0, 10.0);
        CHECK(c->play_count == 0, "a 5 s loop played for 31 s counted as a play");
        rx_ctl_seek(&app.decks[0].ctl, 0.0);
        play_for(&app, 31.0, 0.1, 0, 0, 0);
        CHECK(c->play_count == 1, "31 s played straight through: %d plays", c->play_count);
        rx_ctl_seek(&app.decks[0].ctl, 0.0);
        play_for(&app, 35.0, 0.1, 0, 0, 0);
        CHECK(c->play_count == 1, "one load, played on: %d plays", c->play_count);
        rx_app_unload_deck(&app, 0);
    }

    /* A file gone since it was added: its analysis fails, the status names
     * the track and why, and no worker shows it as analysed any more. */
    char *gone_dir = rx_path_join(dir, "gone");
    char *gone_wav = gone_dir ? rx_path_join(gone_dir, "gone.wav") : NULL;
    if (gone_wav && rx_mkdir_p(gone_dir) >= 0 && copy_file(wav, gone_wav) >= 0 &&
        rx_app_add_path(&app, gone_dir) == 1 && tick_until(&app, analysis_done, 300)) {
        RxTrack *g = track_named(&app, "gone.wav");
        unsigned errors = app.last_error_count;
        CHECK(g && remove(gone_wav) == 0, "remove gone.wav");
        CHECK(g && rx_app_analyse_track(&app, g, RX_ANALYSE_ALL, true) == 1, "analyse gone.wav");
        CHECK(tick_until(&app, analysis_done, 300), "the analysis of a gone file never ended");
        printf("status: %s\n", app.status);
        CHECK(strstr(app.status, "Analysis failed for") && strstr(app.status, "gone") &&
              strstr(app.status, "the file is missing"), "status \"%s\"", app.status);
        CHECK(app.last_error_count == errors + 1, "%u errors counted",
              app.last_error_count - errors);
        uint64_t ids[8];
        CHECK(rx_jobs_active_tracks(app.jobs, ids, 8) == 0, "a worker still shows a track");
    } else {
        CHECK(0, "cannot set up %s", gone_wav ? gone_wav : "gone.wav");
    }
    free(gone_wav);
    free(gone_dir);

    /* A track removed from the library while it is loaded and analysed:
     * the deck lets go of it and the result is dropped. */
    ret = rx_app_load_deck(&app, 0, a);
    CHECK(ret == 0 && tick_until(&app, deck_loaded, 60) && app.decks[0].track == a, "load a");
    CHECK(rx_app_analyse_track(&app, a, RX_ANALYSE_ALL, true) == 1, "forced analysis of a");
    uint64_t a_id = a->id;
    CHECK(rx_library_remove(&app.lib, a) >= 0, "remove a");
    a = NULL;
    CHECK(tick_until(&app, analysis_done, 300), "the analysis of a removed track never ended");
    CHECK(!app.decks[0].track && !rx_deck_loaded(app.player, 0) &&
          !rx_library_find_id(&app.lib, a_id), "the deck still holds a removed track");

    /* A library analysed before the waveform recalibration: b's stored
     * waveforms are format version 1. They are as good as none, so b is
     * analysed for them again, and the deck that loads it gets new ones. */
    uint64_t b_id = b->id;
    rx_app_destroy(&app);
    CHECK(stale_waveforms(root, b_id) == 0, "cannot make b's waveforms stale");
    ret = rx_app_create(&app, root, &po, NULL);
    CHECK(ret == 0, "rx_app_create again: %s", strerror(-ret));
    if (ret < 0)
        goto end;
    b = rx_library_find_id(&app.lib, b_id);
    CHECK(b && b->has_waveforms, "b lost its waveforms");
    if (!b)
        goto end;
    CHECK(rx_app_analysis_needed(&app, b, RX_ANALYSE_ALL, false) & RX_ANALYSE_WAVES,
          "stale waveforms need no analysis");
    ret = rx_app_load_deck(&app, 0, b);
    CHECK(ret == 0 && tick_until(&app, deck_loaded, 60), "load b with stale waveforms");
    CHECK(tick_until(&app, analysis_done, 300), "the stale waveforms were not made again");
    rx_app_tick(&app);
    CHECK(app.decks[0].track == b && app.decks[0].waves.nb_detail > 0,
          "the deck has no waveforms after the analysis");
    CHECK(!(rx_app_analysis_needed(&app, b, RX_ANALYSE_ALL, false) & RX_ANALYSE_WAVES),
          "the new waveforms still count as stale");
    rx_app_unload_deck(&app, 0);

    /* Torn down with an analysis and a decode in flight, and a decode the
     * deck moved on from. */
    CHECK(rx_app_analyse_track(&app, b, RX_ANALYSE_ALL, true) == 1, "forced analysis of b");
    CHECK(rx_app_load_deck(&app, 0, b) == 0, "load b");
    rx_app_unload_deck(&app, 0);
    CHECK(rx_app_load_deck(&app, 0, b) == 0, "load b again");

end:
    rx_app_destroy(&app);
    CHECK(!app.priv, "destroy left state");
    remove_tree(dir);
    free(a_on_stick);
    free(music);
    free(more);
    free(root);
    free(stick);
    free(a_wav);
    free(b_wav);
    free(c_wav);

    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("app: OK\n");
    return 0;
}
