/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The application without a window, on the null audio backend: a fresh
 * library in a scratch directory, two copies of a 128 BPM WAV added and
 * analysed through the jobs, the analysis applied (grid, waveforms, the
 * automatic cue), what still needs analysing, the rows (search, sort, a
 * playlist), loading the deck, rescanning, an export to a directory
 * running through its phases to the end, and reading and inspecting that
 * stick on a worker.
 *
 * Usage: test_app FILE.wav SCRATCH_DIR */

#include "rxbox/app.h"

#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

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

/* Ticks until `done` says so, up to `seconds`. */
static bool tick_until(RxApp *app, bool (*done)(RxApp *app), int seconds)
{
    int64_t t0 = rx_time_us();
    while (rx_time_us() - t0 < seconds * 1000000LL) {
        rx_app_tick(app);
        if (done(app))
            return true;
        usleep(2000);
    }
    return false;
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
    if (argc < 3) {
        fprintf(stderr, "usage: test_app FILE.wav SCRATCH_DIR\n");
        return 2;
    }
    const char *wav = argv[1];
    const char *dir = argv[2];

    remove_tree(dir);
    char *music = rx_path_join(dir, "music");
    char *root = rx_path_join(dir, "lib");
    char *stick = rx_path_join(dir, "stick");
    char *a_wav = rx_path_join(music, "a.wav");
    char *b_wav = rx_path_join(music, "b.wav");
    if (!music || !root || !stick || !a_wav || !b_wav || rx_mkdir_p(music) < 0 ||
        rx_mkdir_p(stick) < 0 || copy_file(wav, a_wav) < 0 || copy_file(wav, b_wav) < 0) {
        fprintf(stderr, "cannot set up %s\n", dir);
        return 2;
    }

    RxPlayerOptions po;
    rx_player_options_default(&po);
    po.backend = RX_AUDIO_NULL;

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

    /* Exporting only another playlist: the dry run finds b's files on the
     * stick, asks, and pruning on the answer deletes them. */
    RxPlaylist *other = rx_library_playlist_create(&app.lib, app.lib.root_folder, "Other");
    if (app.export_state.result >= 0 && other && rx_library_playlist_add(&app.lib, other, a, -1) >= 0) {
        char *pdb = rx_path_join(stick, "PIONEER/rekordbox/export.pdb");
        CHECK(pdb && rx_file_exists(pdb), "no export.pdb on the stick");
        free(pdb);

        RxPlaylist *only[1] = { other };
        app.export_state.playlists = only;
        app.export_state.nb_playlists = 1;
        app.export_state.prune = false;
        CHECK(rx_app_start_export(&app) == 0, "second export");
        CHECK(tick_until(&app, export_done, 300), "the second export did not finish");
        CHECK(app.export_state.confirm && app.export_state.nb_prune > 0,
              "no question about pruning (confirm %d, %d to prune)", app.export_state.confirm,
              app.export_state.nb_prune);
        bool b_listed = false;
        for (int i = 0; i < app.export_state.nb_prune; i++)
            b_listed |= rx_str_ends(app.export_state.prune_list[i], "/b.wav");
        CHECK(b_listed, "b.wav is not in the prune list");
        if (app.export_state.confirm) {
            app.export_state.prune = true;
            CHECK(rx_app_start_export(&app) == 0, "confirm");
            CHECK(tick_until(&app, export_done, 300), "the pruning export did not finish");
            CHECK(!app.export_state.confirm && app.export_state.result >= 0, "pruning export: %s",
                  strerror(-app.export_state.result));
            CHECK(app.export_state.pruned == app.export_state.nb_prune, "%d pruned of %d",
                  app.export_state.pruned, app.export_state.nb_prune);
            for (int i = 0; i < app.export_state.nb_prune; i++) {
                char *f = rx_path_join(stick, app.export_state.prune_list[i] + 1);
                CHECK(f && !rx_file_exists(f), "%s was not pruned", f);
                free(f);
            }
        }
        app.export_state.playlists = NULL;
        app.export_state.nb_playlists = 0;
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

end:
    rx_app_destroy(&app);
    CHECK(!app.priv, "destroy left state");
    remove_tree(dir);
    free(music);
    free(root);
    free(stick);
    free(a_wav);
    free(b_wav);

    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("app: OK\n");
    return 0;
}
