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
#include "rxbox/link.h"

/* The interfaces Link serves and the devices on them that the app keeps
 * a copy of (link_state). */
#define RX_APP_LINK_IFS     8
#define RX_APP_LINK_DEVICES 16

/* Per-deck interface state that outlives a frame. */
typedef struct RXDeckView {
    RXDeckCtl  ctl;
    RXTrack   *track;               /* loaded track (library's), or NULL */
    uint64_t   loading_request;     /* rx_jobs_decode in flight, 0 none */
    uint64_t   loading_track_id;
    /* The track queued to follow (the next row of the playlist being
     * viewed; with PLAY MODE CONTINUE of any view), being decoded or on
     * the deck, which plays on into it at the end with no gap; 0 none. */
    uint64_t   next_track_id;
    RXWaveforms waves;              /* from the library; nb_detail == 0 when none */
    RXWaveGpu *wave_preview;        /* PWV6 / PWAV / PWV4 per palette */
    RXWaveGpu *wave_detail;         /* PWV7 / PWV3 / PWV5 */
    uint64_t   artwork;             /* texture or 0 */
    uint64_t   artwork_request;
    double     zoom_bars;           /* 12 */
    bool       grid_edit;           /* grid editing mode shown */
    double     tap_times[8]; int nb_taps;   /* tap tempo */
} RXDeckView;

typedef struct RXApp {
    RXLibrary  lib;
    RXPlayer  *player;
    RXJobs    *jobs;
    RXWindow  *window;              /* NULL in the CLI */
    RXDeckView decks[RX_DECKS];

    /* Browser state */
    RXPlaylist *current_playlist;   /* NULL = collection */
    RXPlaylistFolder *current_folder;
    /* The album shown (rx_app_show_album): the tracks whose album this
     * is, in disc and track order, like a playlist that nothing can be
     * added to and that the export does not know; "" is None, the tracks
     * of no album in the list (rx_app_albums), by title. NULL = not an
     * album view (the collection: None's tracks, then each album of the
     * list in turn). Owned here. */
    char      *current_album;
    /* The album artist shown (rx_app_show_album_artist): the tracks of
     * its albums in the list (rx_app_albums), one album after the other
     * in the list's order, each in disc and track order; "" is None,
     * None's tracks and those of the albums of no artist. NULL = not an
     * album artist view; never set with current_album. Owned here. */
    char      *current_album_artist;
    /* The TAG LIST shown (rx_app_show_tag_list): its tracks in the order
     * they were added, the players' and ours (--pro-link only); never set
     * with a playlist, an album or an album artist. */
    bool       current_tag_list;
    char       search[128];
    int        sort_column;         /* RXColumn index */
    bool       sort_descending;
    uint64_t  *selection; int nb_selection;    /* selected track ids */
    uint64_t   focus_track;         /* the keyboard focus row */
    uint32_t  *rows; int nb_rows;   /* library indices of the current view, sorted and filtered */
    /* Each row's `#`: the track's place in the view's own order (the
     * playlist's; else the collection's, the albums one after the other:
     * rx_app_albums), from 1, whatever the sort and the search. */
    uint32_t  *row_numbers;
    unsigned   rows_generation;     /* lib.generation the rows were built for */
    bool       rows_dirty;

    /* Analysis queue state shown in the status bar */
    int        analysing_total, analysing_done;
    /* The tag batch on `jobs` (rx_app_tag_tracks), 0 before the first; its
     * progress is the jobs' (rx_app_tag_progress). */
    uint64_t   tag_request;
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
        int    format;              /* the stick's audio (rxbox/export.h RXStickFormat);
                                       starts as export.format */
        bool   whole_library;       /* every track, not only those in playlists */
        bool   ignore_unsynced;     /* export although the stick has changes not synced back */
        bool   cancel;              /* set by the interface to stop the export */
        RXPlaylist **playlists;     /* only these and their tracks; NULL/0 = every playlist.
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
        /* The copy's throughput (RXExportStats), copied here each tick. */
        int64_t bytes_done, bytes_total, rate, seconds_left;
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
    RXJobResult *stick_result;

    /* PRO DJ LINK (src/app/link.c, docs/LINK.md): the library served on
     * every Ethernet interface from the start, and plays counted from what
     * the players say they play. Only with --pro-link: src/cli.c sets
     * `enabled` and starts it; without it the interface offers none of it
     * and rx_app_link_start refuses, so nothing is ever bound. The rest is
     * the set's state (rxbox/link.h RXLinkSet), copied here every tick. */
    struct {
        bool     enabled;
        bool     on;                /* the set runs */
        RXLinkServed interfaces[RX_APP_LINK_IFS];   /* served or tried, by name */
        int      nb_interfaces;
        RXLinkPlayer devices[RX_APP_LINK_DEVICES];
        int      device_if[RX_APP_LINK_DEVICES];    /* each one's index in interfaces */
        int      nb_devices;
        /* Per device: the seconds of its load (a track of ours) played so
         * far, distinct audio as the deck counts it, and whether that load
         * was counted as a play. */
        double   played[RX_APP_LINK_DEVICES];
        bool     counted[RX_APP_LINK_DEVICES];
    } link_state;

    /* The waveform palette of the deck and the table's previews: 0 3band,
     * 1 blue, 2 rgb. The players' device.waveform_color, which rx_app_tick
     * reads again every tick; set that, not this. */
    int        wave_palette;
    bool       dirty;               /* the next frame must be drawn */
    bool       quit;

    void      *ui;                  /* src/ui private state */
    struct RXAppPriv *priv;         /* src/app/app.c private state */
    struct RXMpris *mpris;          /* src/app/mpris.c (rxbox/mpris.h); NULL off the bus */
    struct RXAppLink *link;         /* src/app/link.c; NULL before Link is first started */
} RXApp;

/* Lifecycle (src/app.c). player_opt NULL: the null audio backend. Creating
 * queues, in the background, the analysis of every stale result
 * (rx_app_analysis_stale; locked grids stay), so an upgrade analyses again
 * what it improved without being asked. */
int  rx_app_create(RXApp *app, const char *library_root, const RXPlayerOptions *player_opt,
                   const RXWindowOptions *window_opt /* NULL: no window */);
void rx_app_destroy(RXApp *app);
/* One iteration of the owner-thread work that is not drawing: job results,
 * deck ticks, pending saves. Returns true when something changed that the
 * interface should show. */
bool rx_app_tick(RXApp *app);
/* The interface loop (returns when the window closes). */
int  rx_app_run(RXApp *app);

/* Actions the interface and the CLI share (src/app.c). */
int  rx_app_load_deck(RXApp *app, int deck, RXTrack *t);
void rx_app_unload_deck(RXApp *app, int deck);
/* One track, named: forced, even its locked grid is analysed again. */
int  rx_app_analyse_track(RXApp *app, RXTrack *t, unsigned what, bool force);
/* A selection: forced, every result is analysed again but a locked grid,
 * unless the selection is the one track (README: a locked grid is replaced
 * only when its track is named). Returns how many were queued. */
int  rx_app_analyse_tracks(RXApp *app, const uint64_t *ids, int n, unsigned what, bool force);
int  rx_app_analyse_all(RXApp *app, bool force);     /* everything not yet analysed, or stale */
/* Which stages (RXAnalyseWhat) of a track are stale: made by an older
 * analyser (rx_analysis_stale: the version is below the current one), or,
 * for waveforms, stored in a format version the library no longer reads
 * (rx_library_waveforms_check's -EPROTO: made before a recalibration;
 * looked at once per track). The one notion of stale every path uses: the
 * add path, a deck load, analyse all, the export's analysis, the startup
 * queue, `rxbox analyze` and `rxbox check`. Says nothing of locks or
 * missing files. Needs only app->lib. */
unsigned rx_app_analysis_stale(const RXApp *app, const RXTrack *t);
/* Which of `what` (RXAnalyseWhat) a track still lacks; everything asked
 * when force. A stale result (rx_app_analysis_stale) is lacking. A track
 * whose waveforms are missing or stale always needs them, whatever was
 * asked; a locked grid never needs beats unless force, stale or not; a
 * missing file needs nothing (it cannot be read). Needs only app->lib. */
unsigned rx_app_analysis_needed(const RXApp *app, const RXTrack *t, unsigned what, bool force);
int  rx_app_add_path(RXApp *app, const char *path);  /* add + queue analysis */
void rx_app_save_track(RXApp *app, RXTrack *t);      /* to the library now */
void rx_app_set_status(RXApp *app, const char *fmt, ...) RX_PRINTF(2, 3);
void rx_app_rebuild_rows(RXApp *app);
/* The albums for the tree: every album name that two tracks or more
 * share, each with its track count and its artist (the album artist of
 * its first track that has one, else the artist all its tracks share,
 * else none), by artist in the order the table sorts names, each
 * artist's albums together by name, the albums of no artist last; after
 * None (the name "", first, when it has tracks): the tracks without an
 * album and those whose album no other track has. The collection's own
 * order is this list's tracks one after the other (None's by title, an
 * album's in disc and track order), whatever order they were added in.
 * Made when the library has changed since the last call; valid until the
 * next one. Returns the count, None included. */
typedef struct RXAlbum {
    const char *name;
    int         nb;
    const char *artist;         /* NULL none */
} RXAlbum;
int  rx_app_albums(RXApp *app, const RXAlbum **albums);
/* The album artists for the tree: the artists of the albums list
 * (RXAlbum.artist; names that differ only in case are one, named as the
 * first of its albums has it), in the list's order,
 * each with the count of its albums' tracks; after None (the name "",
 * first, when it has tracks): None's tracks and those of the albums of
 * no artist. Made with the albums, valid as long. Returns the count, None
 * included. */
typedef struct RXAlbumArtist {
    const char *name;
    int         nb;
} RXAlbumArtist;
int  rx_app_album_artists(RXApp *app, const RXAlbumArtist **artists);
/* Shows the album `name` (copied; "": None; NULL: the album, album artist
 * and TAG LIST views are left, the playlist or collection view stays): the
 * playlist, album artist and TAG LIST views are cleared and the rows
 * rebuilt. */
void rx_app_show_album(RXApp *app, const char *name);
/* Shows the album artist `name` (copied; "": None; NULL: the view is
 * left): the playlist, album and TAG LIST views are cleared and the rows
 * rebuilt. */
void rx_app_show_album_artist(RXApp *app, const char *name);
int  rx_app_start_export(RXApp *app);                /* from export_state */
/* RX_JOB_STICK_READ against the library as saved (pending saves are
 * written first), or RX_JOB_INSPECT, of the stick at `mount`; see
 * stick_request. -EBUSY while one is in flight. */
int  rx_app_read_stick(RXApp *app, const char *mount);
int  rx_app_inspect_stick(RXApp *app, const char *mount);
/* Gives up the read or inspection `request` (stick_request when it was
 * asked for), for a dialog closed meanwhile: dropped if it has not
 * started, else its result is discarded when it comes; a result already
 * in stick_result for it is freed. Another read may start at once. */
void rx_app_cancel_stick(RXApp *app, uint64_t request);
/* Applies a finished analysis to a track: grid (unless locked), key,
 * waveforms, auto cues; saves the track. */
int  rx_app_apply_analysis(RXApp *app, RXTrack *t, RXAnalysis *a);

/* Tagging with the model (rxbox/ml.h, rx_jobs_tag), only ever when the
 * user asks: never on add, at startup, in analyse all or before an
 * export. The tracks join the one batch on app->jobs, which loads the
 * model once, tags them one at a time and unloads it when there is
 * nothing left. Needs only app->lib and app->jobs (the CLI's app too).
 * Returns how many tracks were queued (missing files are not), or a
 * negative errno with the status set: -ENOENT when there is no model
 * file (rx_app_ml_model). */
int  rx_app_tag_tracks(RXApp *app, const uint64_t *ids, int n);
/* Stops the batch: the track being tagged and the queue are dropped, the
 * model is unloaded. */
void rx_app_cancel_tags(RXApp *app);
/* rx_jobs_tag_progress of the batch: false when none runs. */
bool rx_app_tag_progress(RXApp *app, int *done, int *total, uint64_t *current);
/* What a result writes, through the library: the colour the model picked
 * (as that colour's hue, rx_track_color_hue) and the comment always (the
 * comment field is the machine's), the genre into the user's override only
 * when that is empty (a genre the user typed stays; the file's tag is
 * never touched), the eight colours' probabilities and the legend's hash,
 * the four measures and RX_ML_TAGS_VERSION. Saves the track. */
int  rx_app_apply_tags(RXApp *app, RXTrack *t, const RXMLTags *tags);
/* The model's file: the setting ml.model when set, else $RXBOX_ML_MODEL,
 * else models/RX_APP_ML_MODEL in the executable's directory, its parent
 * (a build directory inside the source tree) or the current directory.
 * -ENOENT when it is not there: *path is then the file the setting or the
 * environment named, or NULL when models/ was looked in. Allocating. */
#define RX_APP_ML_MODEL "moss-audio-4b-instruct-q4_k.gguf"
int  rx_app_ml_model(const RXApp *app, char **path);
/* PRO DJ LINK on (src/app/link.c): served on every Ethernet interface, as
 * they come and go (o NULL), or as `o` says (the tests: one interface,
 * ports of their own; its library, when NULL, the app's), by a set
 * (rxbox/link.h RXLinkSet) whose thread starts the services. While it
 * runs, each tick copies its state into link_state, counts a play of a
 * track of ours that a player has played 30 s of (rx_library_record_play,
 * as the deck's), and, a little after the library last changed, has the
 * services read it again. -EPERM without --pro-link (link_state.enabled
 * false), -EALREADY when on. */
int  rx_app_link_start(RXApp *app, const RXLinkOptions *o);
/* Off: every service stopped (it waits for a request in progress). */
void rx_app_link_stop(RXApp *app);
/* Re-checks which audio files are missing; returns how many changed. */
int  rx_app_rescan(RXApp *app);
/* For the browser: a track's cover thumbnail texture and its preview in the
 * waveform palette (PWV6, PWAV or PWV4: wave_palette) on the GPU, made on
 * demand from the library (an
 * artwork job when the library has no thumbnail yet; it is stored there)
 * and cached, the least recently asked for released beyond a few hundred.
 * 0 / NULL while being made or when there is none. Window only; valid
 * until the next rx_app_tick. */
uint64_t   rx_app_artwork_texture(RXApp *app, const RXTrack *t);
RXWaveGpu *rx_app_preview_wave(RXApp *app, const RXTrack *t);
/* The TAG LIST shared with the players (rxbox/link.h RXLinkTagList): the
 * tracks collected with TAG TRACK on a deck or Add to Tag List here, for
 * this session only. NULL without --pro-link; made when first asked for,
 * and the services share it from their start. */
RXLinkTagList *rx_app_tag_list(RXApp *app);
/* Shows the TAG LIST: the playlist, album and album artist views are
 * cleared and the rows rebuilt; nothing without --pro-link. */
void rx_app_show_tag_list(RXApp *app);
/* Tells the Link device link_state.devices[index] (a player) to load
 * `track_id` of the library: -ENOENT when it is gone, -EAGAIN before we
 * are on its link (rxbox/link.h rx_link_load). */
int        rx_app_link_load(RXApp *app, int index, uint64_t track_id);
/* The thumbnail is still being looked for (the library's is tried for
 * every track, the file's art only when has_artwork): the 0 that
 * rx_app_artwork_texture() returns may change. */
bool       rx_app_artwork_pending(RXApp *app, const RXTrack *t);

/* The interface (src/ui, C++). */
int  rx_ui_init(RXApp *app);
void rx_ui_shutdown(RXApp *app);
/* Builds one ImGui frame. Returns true when the interface wants another
 * frame soon (animations, a deck playing, a drag). */
bool rx_ui_frame(RXApp *app);

/* Track table columns, in default order. */
enum RXColumn {
    RX_COL_ART = 0, RX_COL_TITLE, RX_COL_ARTIST, RX_COL_ALBUM, RX_COL_BPM, RX_COL_KEY,
    RX_COL_TIME, RX_COL_RATING, RX_COL_COLOR, RX_COL_CUES, RX_COL_PREVIEW, RX_COL_GENRE,
    RX_COL_YEAR, RX_COL_ADDED, RX_COL_PATH, RX_COL_PLAYS, RX_COL_LAST_PLAYED, RX_COL_COUNT,
};

#endif /* RXBOX_APP_H */
