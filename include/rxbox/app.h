/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_APP_H
#define RXBOX_APP_H

/* The application: everything the interface reads and writes, owned by
 * the main thread. main.c creates it and runs the loop; src/ui builds the
 * ImGui frame from it; src/app.c applies job results, saves sidecars and
 * caches, loads decks. */

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
    RxWaveforms waves;              /* loaded from the cache; nb_detail == 0 when none */
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
    int64_t    status_time;
    unsigned   last_error_count;

    /* Export dialog state */
    struct {
        bool   open;
        char   dest[512];
        bool   running;
        bool   prune, compat;
        bool   cancel;              /* set by the interface to stop a running export */
        RxPlaylist **playlists;     /* only these and their tracks; NULL/0 = the whole library.
                                       The interface owns the array; read at start */
        int    nb_playlists;
        int    done, total;
        char   stage[32];
        char   item[256];
        char   log[4096];
        int    result;
    } export_state;

    /* Interface preferences (persisted in rxbox.toml [ui]) */
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
 * when force. A track without a waveform cache file always needs waves; a
 * locked grid never needs beats unless force. Needs only app->lib. */
unsigned rx_app_analysis_needed(const RxApp *app, const RxTrack *t, unsigned what, bool force);
int  rx_app_add_path(RxApp *app, const char *path);  /* add + queue analysis */
void rx_app_save_track(RxApp *app, RxTrack *t);      /* sidecar now */
void rx_app_set_status(RxApp *app, const char *fmt, ...) RX_PRINTF(2, 3);
void rx_app_rebuild_rows(RxApp *app);
int  rx_app_start_export(RxApp *app);                /* from export_state */
/* Applies a finished analysis to a track: grid (unless locked), key,
 * waveform cache, auto cues; saves the sidecar. */
int  rx_app_apply_analysis(RxApp *app, RxTrack *t, RxAnalysis *a);

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
    RX_COL_YEAR, RX_COL_ADDED, RX_COL_PATH, RX_COL_COUNT,
};

#endif /* RXBOX_APP_H */
