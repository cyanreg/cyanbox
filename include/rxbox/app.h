/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_APP_H
#define RXBOX_APP_H

/* The application: everything the interface reads and writes, owned by
 * the main thread. main.c creates it and runs the loop; src/ui builds the
 * ImGui frame from it; src/app/app.c applies job results, saves tracks to
 * the library, loads decks. */

#include "rxbox/library.h"
#include "rxbox/player.h"
#include "rxbox/jobs.h"
#include "rxbox/gpu.h"

/* Per-deck interface state that outlives a frame. */
typedef struct RxDeckView {
    RxDeckCtl  ctl;
    RxTrack   *track;               /* loaded track (library's), or NULL */
    uint64_t   loading_request;     /* rx_jobs_decode in flight, 0 none */
    uint64_t   loading_track_id;
    RxWaveforms waves;              /* from the library; nb_detail == 0 when none */
    RxWaveGpu *wave_preview;        /* PWV6 / PWAV / PWV4 per palette */
    RxWaveGpu *wave_detail;         /* PWV7 / PWV3 / PWV5 */
    uint64_t   artwork;             /* texture or 0 */
    uint64_t   artwork_request;
    double     zoom_bars;           /* 12 */
    int        pad_mode;            /* 0 hot cues, 1 beat loops, 2 beat jump */
    int        panel;               /* 0 memory list, 1 hot cue list, 2 grid tools */
    bool       grid_edit;           /* grid editing mode shown */
    double     tap_times[8]; int nb_taps;   /* tap tempo */
} RxDeckView;

typedef struct RxApp {
    RxLibrary  lib;
    RxPlayer  *player;
    RxJobs    *jobs;
    RxWindow  *window;              /* NULL in the CLI */
    RxDeckView decks[RX_DECKS];

    /* Browser state */
    RxPlaylist *current_playlist;   /* NULL = collection */
    RxPlaylistFolder *current_folder;
    char       search[128];
    int        sort_column;         /* RxColumn index */
    bool       sort_descending;
    uint64_t  *selection; int nb_selection;    /* selected track ids */
    uint64_t   focus_track;         /* the keyboard focus row */
    uint32_t  *rows; int nb_rows;   /* library indices of the current view, sorted and filtered */
    unsigned   rows_generation;     /* lib.generation the rows were built for */
    bool       rows_dirty;

    /* Analysis queue state shown in the status bar */
    int        analysing_total, analysing_done;
    char       status[256];         /* last message */
    int64_t    status_time;         /* when it was set (rx_time_us) */
    unsigned   last_error_count;

    /* Export dialog state. rx_app_start_export() analyses what the
     * selected tracks lack, then dry-runs; when the dry run finds files to
     * prune or player changes not synced back, it stops with `confirm` set:
     * the interface shows prune_list / unsynced, sets `prune` /
     * `ignore_unsynced` from the answer and calls rx_app_start_export()
     * again for the real run (or sets `cancel`). */
    struct {
        bool   open;
        char   dest[512];
        bool   running;             /* from the start until the end, confirmation included */
        bool   prune, compat;
        bool   whole_library;       /* every track, not only those in playlists */
        bool   ignore_unsynced;     /* export although the stick has changes not synced back */
        bool   cancel;              /* set by the interface to stop the export */
        RxPlaylist **playlists;     /* only these and their tracks; NULL/0 = every playlist.
                                       The interface owns the array; read at start */
        int    nb_playlists;
        bool   confirm;             /* waiting for the interface's answer (see above) */
        char **prune_list; int nb_prune;   /* stick files the export would delete (owned here) */
        int    unsynced;            /* "check" items about changes not synced back */
        int    done, total;
        /* What the real run did, counted from its stages: audio files
         * copied and skipped (already on the stick), files deleted, and
         * the steps that rewrite the rest (a track's ANLZ files, an
         * artwork, a settings file or database). */
        int    copied, skipped, pruned, rewritten;
        char   stage[32];
        char   item[256];
        char   log[4096];
        int    result;
    } export_state;

    /* The stick read or inspection the Sync from USB and Inspect dialogs
     * asked for (rx_app_read_stick, rx_app_inspect_stick), one at a time on
     * a worker. The finished result waits here until the interface takes
     * it (it then frees it with rx_job_result_free). */
    uint64_t   stick_request;       /* in flight, 0 none */
    RxJobResult *stick_result;

    /* Interface preferences (persisted in the library's ui.* settings) */
    int        wave_palette;        /* 0 3band, 1 blue, 2 rgb */
    bool       show_demo;           /* ImGui demo window (debug) */
    bool       dirty;               /* the next frame must be drawn */
    bool       quit;

    void      *ui;                  /* src/ui private state */
    struct RxAppPriv *priv;         /* src/app/app.c private state */
} RxApp;

/* Lifecycle (src/app.c). player_opt NULL: the null audio backend. */
int  rx_app_create(RxApp *app, const char *library_root, const RxPlayerOptions *player_opt,
                   const RxWindowOptions *window_opt /* NULL: no window */);
void rx_app_destroy(RxApp *app);
/* One iteration of the owner-thread work that is not drawing: job results,
 * deck ticks, pending saves. Returns true when something changed that the
 * interface should show. */
bool rx_app_tick(RxApp *app);
/* The interface loop (returns when the window closes). */
int  rx_app_run(RxApp *app);

/* Actions the interface and the CLI share (src/app.c). */
int  rx_app_load_deck(RxApp *app, int deck, RxTrack *t);
void rx_app_unload_deck(RxApp *app, int deck);
int  rx_app_analyse_track(RxApp *app, RxTrack *t, unsigned what, bool force);
int  rx_app_analyse_all(RxApp *app, bool force);     /* everything not yet analysed */
/* Which of `what` (RxAnalyseWhat) a track still lacks; everything asked
 * when force. A track without waveforms in the library always needs them,
 * and so does one whose stored waveforms are of a format version the
 * library no longer reads (-EPROTO: made before a recalibration; they are
 * loaded once to see); a locked grid never needs beats unless force; a
 * missing file needs nothing (it cannot be read). Needs only app->lib. */
unsigned rx_app_analysis_needed(const RxApp *app, const RxTrack *t, unsigned what, bool force);
int  rx_app_add_path(RxApp *app, const char *path);  /* add + queue analysis */
void rx_app_save_track(RxApp *app, RxTrack *t);      /* to the library now */
void rx_app_set_status(RxApp *app, const char *fmt, ...) RX_PRINTF(2, 3);
void rx_app_rebuild_rows(RxApp *app);
int  rx_app_start_export(RxApp *app);                /* from export_state */
/* RX_JOB_STICK_READ against the library as saved (pending saves are
 * written first), or RX_JOB_INSPECT, of the stick at `mount`; see
 * stick_request. -EBUSY while one is in flight. */
int  rx_app_read_stick(RxApp *app, const char *mount);
int  rx_app_inspect_stick(RxApp *app, const char *mount);
/* Gives up the read or inspection `request` (stick_request when it was
 * asked for), for a dialog closed meanwhile: dropped if it has not
 * started, else its result is discarded when it comes; a result already
 * in stick_result for it is freed. Another read may start at once. */
void rx_app_cancel_stick(RxApp *app, uint64_t request);
/* Applies a finished analysis to a track: grid (unless locked), key,
 * waveforms, auto cues; saves the track. */
int  rx_app_apply_analysis(RxApp *app, RxTrack *t, RxAnalysis *a);
/* Re-checks which audio files are missing; returns how many changed. */
int  rx_app_rescan(RxApp *app);
/* For the browser: a track's cover thumbnail texture and its PWV6 preview
 * (3-band, 1200 columns) on the GPU, made on demand from the library (an
 * artwork job when the library has no thumbnail yet; it is stored there)
 * and cached, the least recently asked for released beyond a few hundred.
 * 0 / NULL while being made or when there is none. Window only; valid
 * until the next rx_app_tick. */
uint64_t   rx_app_artwork_texture(RxApp *app, const RxTrack *t);
RxWaveGpu *rx_app_preview_wave(RxApp *app, const RxTrack *t);
/* The thumbnail is still being looked for (the library's is tried for
 * every track, the file's art only when has_artwork): the 0 that
 * rx_app_artwork_texture() returns may change. */
bool       rx_app_artwork_pending(RxApp *app, const RxTrack *t);

/* The interface (src/ui, C++). */
int  rx_ui_init(RxApp *app);
void rx_ui_shutdown(RxApp *app);
/* Builds one ImGui frame. Returns true when the interface wants another
 * frame soon (animations, a deck playing, a drag). */
bool rx_ui_frame(RxApp *app);

/* Track table columns, in default order. */
enum RxColumn {
    RX_COL_ART = 0, RX_COL_TITLE, RX_COL_ARTIST, RX_COL_ALBUM, RX_COL_BPM, RX_COL_KEY,
    RX_COL_TIME, RX_COL_RATING, RX_COL_COLOR, RX_COL_CUES, RX_COL_PREVIEW, RX_COL_GENRE,
    RX_COL_YEAR, RX_COL_ADDED, RX_COL_PATH, RX_COL_PLAYS, RX_COL_LAST_PLAYED, RX_COL_COUNT,
};

#endif /* RXBOX_APP_H */
