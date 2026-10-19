/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_APP_H
#define CYANBOX_APP_H

/* The application: everything the interface reads and writes, owned by
 * the main thread. main.c creates it and runs the loop; src/ui builds the
 * ImGui frame from it; src/app/app.c applies job results, saves tracks to
 * the library, loads decks. */

#include "cyanbox/library.h"
#include "cyanbox/player.h"
#include "cyanbox/jobs.h"
#include "cyanbox/gpu.h"
#include "cyanbox/link.h"

/* The interfaces Link serves and the devices on them that the app keeps
 * a copy of (link_state). */
#define CB_APP_LINK_IFS     8
#define CB_APP_LINK_DEVICES 16

/* Per-deck interface state that outlives a frame. */
typedef struct CBDeckView {
    CBDeckCtl  ctl;
    CBTrack   *track;               /* loaded track (library's), or NULL */
    uint64_t   loading_request;     /* cb_jobs_decode in flight, 0 none */
    uint64_t   loading_track_id;
    /* The track queued to follow (the next row of the playlist being
     * viewed; with PLAY MODE CONTINUE of any view), being decoded or on
     * the deck, which plays on into it at the end with no gap; 0 none. */
    uint64_t   next_track_id;
    CBWaveforms waves;              /* from the library; nb_detail == 0 when none */
    CBWaveGpu *wave_preview;        /* PWV6 / PWAV / PWV4 per palette */
    CBWaveGpu *wave_detail;         /* PWV7 / PWV3 / PWV5 */
    uint64_t   artwork;             /* texture or 0 */
    uint64_t   artwork_request;
    double     zoom_bars;           /* 12 */
    bool       grid_edit;           /* grid editing mode shown */
    double     tap_times[8]; int nb_taps;   /* tap tempo */
} CBDeckView;

typedef struct CBApp {
    CBLibrary  lib;
    CBPlayer  *player;
    CBJobs    *jobs;
    CBWindow  *window;              /* NULL in the CLI */
    CBDeckView decks[CB_DECKS];

    /* Browser state */
    CBPlaylist *current_playlist;   /* NULL = collection */
    CBPlaylistFolder *current_folder;
    /* The album shown (cb_app_show_album): the tracks whose album this
     * is, in disc and track order, like a playlist that nothing can be
     * added to and that the export does not know; "" is None, the tracks
     * of no album in the list (cb_app_albums), by title. NULL = not an
     * album view (the collection: None's tracks, then each album of the
     * list in turn). Owned here. */
    char      *current_album;
    /* The album artist shown (cb_app_show_album_artist): the tracks of
     * its albums in the list (cb_app_albums), one album after the other
     * in the list's order, each in disc and track order; "" is None,
     * None's tracks and those of the albums of no artist. NULL = not an
     * album artist view; never set with current_album. Owned here. */
    char      *current_album_artist;
    /* The TAG LIST shown (cb_app_show_tag_list): its tracks in the order
     * they were added, the players' and ours (--pro-link only); never set
     * with a playlist, an album or an album artist. */
    bool       current_tag_list;
    /* The History shown (cb_app_show_history, --pro-link only): each
     * track on it once, at its latest play, numbered from the oldest and
     * shown newest first; never set with another view. */
    bool       current_history;
    char       search[128];
    int        sort_column;         /* CBColumn index */
    bool       sort_descending;
    uint64_t  *selection; int nb_selection;    /* selected track ids */
    uint64_t   focus_track;         /* the keyboard focus row */
    uint32_t  *rows; int nb_rows;   /* library indices of the current view, sorted and filtered */
    /* Each row's `#`: the track's place in the view's own order (the
     * playlist's; else the collection's, the albums one after the other:
     * cb_app_albums), from 1, whatever the sort and the search. */
    uint32_t  *row_numbers;
    unsigned   rows_generation;     /* lib.generation the rows were built for */
    bool       rows_dirty;

    /* Analysis queue state shown in the status bar */
    int        analysing_total, analysing_done;
    /* The tag batch on `jobs` (cb_app_tag_tracks), 0 before the first; its
     * progress is the jobs' (cb_app_tag_progress). */
    uint64_t   tag_request;
    char       status[256];         /* last message */
    int64_t    status_time;         /* when it was set (cb_time_us) */
    unsigned   last_error_count;

    /* Export dialog state. cb_app_start_export() analyses what the
     * selected tracks lack, then dry-runs; when the dry run finds files to
     * prune or player changes not synced back, it stops with `confirm` set:
     * the interface shows prune_list / unsynced, sets `prune` /
     * `ignore_unsynced` from the answer and calls cb_app_start_export()
     * again for the real run (or sets `cancel`). */
    struct {
        bool   open;
        char   dest[512];
        bool   running;             /* from the start until the end, confirmation included */
        bool   prune, compat;
        int    format;              /* the stick's audio (cyanbox/export.h CBStickFormat);
                                       starts as export.format */
        bool   whole_library;       /* every track, not only those in playlists */
        bool   ignore_unsynced;     /* export although the stick has changes not synced back */
        bool   cancel;              /* set by the interface to stop the export */
        CBPlaylist **playlists;     /* only these and their tracks; NULL/0 = every playlist.
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
        /* The copy's throughput (CBExportStats), copied here each tick. */
        int64_t bytes_done, bytes_total, rate, seconds_left;
        char   stage[32];
        char   item[256];
        char   log[4096];
        int    result;
    } export_state;

    /* The stick read or inspection the Sync from USB and Inspect dialogs
     * asked for (cb_app_read_stick, cb_app_inspect_stick), one at a time on
     * a worker. The finished result waits here until the interface takes
     * it (it then frees it with cb_job_result_free). */
    uint64_t   stick_request;       /* in flight, 0 none */
    CBJobResult *stick_result;

    /* PRO DJ LINK (src/app/link.c, docs/LINK.md): the library served on
     * every Ethernet interface from the start, and plays counted from what
     * the players say they play. Only with --pro-link: src/cli.c sets
     * `enabled` and starts it; without it the interface offers none of it
     * and cb_app_link_start refuses, so nothing is ever bound. The rest is
     * the set's state (cyanbox/link.h CBLinkSet), copied here every tick. */
    struct {
        bool     enabled;
        bool     on;                /* the set runs */
        CBLinkServed interfaces[CB_APP_LINK_IFS];   /* served or tried, by name */
        int      nb_interfaces;
        CBLinkPlayer devices[CB_APP_LINK_DEVICES];
        int      device_if[CB_APP_LINK_DEVICES];    /* each one's index in interfaces */
        int      nb_devices;
        /* Per device: the seconds of its load (a track of ours) played so
         * far, distinct audio as the deck counts it, and whether that load
         * was counted as a play. */
        double   played[CB_APP_LINK_DEVICES];
        bool     counted[CB_APP_LINK_DEVICES];
        /* LINK CUE (cyanbox/link.h CBLinkCue): while a mixer is heard on
         * the link the deck is heard only in its headphone cue, the player
         * on its ring backend; with none, as without Link (PipeWire). Where
         * the cue stands, copied every tick. */
        bool     cue_enabled;
        CBLinkCueState cue;
    } link_state;

    /* The waveform palette of the deck and the table's previews: 0 3band,
     * 1 blue, 2 rgb. The players' device.waveform_color, which cb_app_tick
     * reads again every tick; set that, not this. */
    int        wave_palette;
    bool       dirty;               /* the next frame must be drawn */
    bool       quit;

    void      *ui;                  /* src/ui private state */
    struct CBAppPriv *priv;         /* src/app/app.c private state */
    struct CBMpris *mpris;          /* src/app/mpris.c (cyanbox/mpris.h); NULL off the bus */
    struct CBAppLink *link;         /* src/app/link.c; NULL before Link is first started */
} CBApp;

/* Lifecycle (src/app.c). player_opt NULL: the null audio backend. Creating
 * queues, in the background, the analysis of every stale result
 * (cb_app_analysis_stale; locked grids stay), so an upgrade analyses again
 * what it improved without being asked. */
int  cb_app_create(CBApp *app, const char *library_root, const CBPlayerOptions *player_opt,
                   const CBWindowOptions *window_opt /* NULL: no window */);
void cb_app_destroy(CBApp *app);
/* One iteration of the owner-thread work that is not drawing: job results,
 * deck ticks, pending saves. Returns true when something changed that the
 * interface should show. */
bool cb_app_tick(CBApp *app);
/* The interface loop (returns when the window closes). */
int  cb_app_run(CBApp *app);

/* Actions the interface and the CLI share (src/app.c). */
int  cb_app_load_deck(CBApp *app, int deck, CBTrack *t);
void cb_app_unload_deck(CBApp *app, int deck);
/* One track, named: forced, even its locked grid is analysed again. */
int  cb_app_analyse_track(CBApp *app, CBTrack *t, unsigned what, bool force);
/* A selection: forced, every result is analysed again but a locked grid,
 * unless the selection is the one track (README: a locked grid is replaced
 * only when its track is named). Returns how many were queued. */
int  cb_app_analyse_tracks(CBApp *app, const uint64_t *ids, int n, unsigned what, bool force);
int  cb_app_analyse_all(CBApp *app, bool force);     /* everything not yet analysed, or stale */
/* Which stages (CBAnalyseWhat) of a track are stale: made by an older
 * analyser (cb_analysis_stale: the version is below the current one), or,
 * for waveforms, stored in a format version the library no longer reads
 * (cb_library_waveforms_check's -EPROTO: made before a recalibration;
 * looked at once per track). The one notion of stale every path uses: the
 * add path, a deck load, analyse all, the export's analysis, the startup
 * queue, `cyanbox analyze` and `cyanbox check`. Says nothing of locks or
 * missing files. Needs only app->lib. */
unsigned cb_app_analysis_stale(const CBApp *app, const CBTrack *t);
/* Which of `what` (CBAnalyseWhat) a track still lacks; everything asked
 * when force. A stale result (cb_app_analysis_stale) is lacking. A track
 * whose waveforms are missing or stale always needs them, whatever was
 * asked; a locked grid never needs beats unless force, stale or not; a
 * missing file needs nothing (it cannot be read). Needs only app->lib. */
unsigned cb_app_analysis_needed(const CBApp *app, const CBTrack *t, unsigned what, bool force);
int  cb_app_add_path(CBApp *app, const char *path);  /* add + queue analysis */
void cb_app_save_track(CBApp *app, CBTrack *t);      /* to the library now */
void cb_app_set_status(CBApp *app, const char *fmt, ...) CB_PRINTF(2, 3);
void cb_app_rebuild_rows(CBApp *app);
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
typedef struct CBAlbum {
    const char *name;
    int         nb;
    const char *artist;         /* NULL none */
} CBAlbum;
int  cb_app_albums(CBApp *app, const CBAlbum **albums);
/* The album artists for the tree: the artists of the albums list
 * (CBAlbum.artist; names that differ only in case are one, named as the
 * first of its albums has it), in the list's order,
 * each with the count of its albums' tracks; after None (the name "",
 * first, when it has tracks): None's tracks and those of the albums of
 * no artist. Made with the albums, valid as long. Returns the count, None
 * included. */
typedef struct CBAlbumArtist {
    const char *name;
    int         nb;
} CBAlbumArtist;
int  cb_app_album_artists(CBApp *app, const CBAlbumArtist **artists);
/* Shows the album `name` (copied; "": None; NULL: the album, album artist
 * and TAG LIST views are left, the playlist or collection view stays): the
 * playlist, album artist and TAG LIST views are cleared and the rows
 * rebuilt. */
void cb_app_show_album(CBApp *app, const char *name);
/* Shows the album artist `name` (copied; "": None; NULL: the view is
 * left): the playlist, album and TAG LIST views are cleared and the rows
 * rebuilt. */
void cb_app_show_album_artist(CBApp *app, const char *name);
int  cb_app_start_export(CBApp *app);                /* from export_state */
/* CB_JOB_STICK_READ against the library as saved (pending saves are
 * written first), or CB_JOB_INSPECT, of the stick at `mount`; see
 * stick_request. -EBUSY while one is in flight. */
int  cb_app_read_stick(CBApp *app, const char *mount);
int  cb_app_inspect_stick(CBApp *app, const char *mount);
/* Gives up the read or inspection `request` (stick_request when it was
 * asked for), for a dialog closed meanwhile: dropped if it has not
 * started, else its result is discarded when it comes; a result already
 * in stick_result for it is freed. Another read may start at once. */
void cb_app_cancel_stick(CBApp *app, uint64_t request);
/* Applies a finished analysis to a track: grid (unless locked), key,
 * waveforms, auto cues; saves the track. */
int  cb_app_apply_analysis(CBApp *app, CBTrack *t, CBAnalysis *a);

/* Tagging with the model (cyanbox/ml.h, cb_jobs_tag), only ever when the
 * user asks: never on add, at startup, in analyse all or before an
 * export. The tracks join the one batch on app->jobs, which loads the
 * model once, tags them one at a time and unloads it when there is
 * nothing left. Needs only app->lib and app->jobs (the CLI's app too).
 * Returns how many tracks were queued (missing files are not), or a
 * negative errno with the status set: -ENOENT when there is no model
 * file (cb_app_ml_model). */
int  cb_app_tag_tracks(CBApp *app, const uint64_t *ids, int n);
/* Stops the batch: the track being tagged and the queue are dropped, the
 * model is unloaded. */
void cb_app_cancel_tags(CBApp *app);
/* cb_jobs_tag_progress of the batch: false when none runs. */
bool cb_app_tag_progress(CBApp *app, int *done, int *total, uint64_t *current);
/* What a result writes, through the library: the colour the model picked
 * (as that colour's hue, cb_track_color_hue) and the comment always (the
 * comment field is the machine's), the genre into the user's override only
 * when that is empty (a genre the user typed stays; the file's tag is
 * never touched), the eight colours' probabilities and the legend's hash,
 * the four measures and CB_ML_TAGS_VERSION. Saves the track. */
int  cb_app_apply_tags(CBApp *app, CBTrack *t, const CBMLTags *tags);
/* The model's file: the setting ml.model when set, else $CYANBOX_ML_MODEL,
 * else models/CB_APP_ML_MODEL in the executable's directory, its parent
 * (a build directory inside the source tree), the current directory or
 * the installed data directory (share/cyanbox).
 * -ENOENT when it is not there: *path is then the file the setting or the
 * environment named, or NULL when models/ was looked in. Allocating. */
#define CB_APP_ML_MODEL "moss-audio-4b-instruct-q4_k.gguf"
int  cb_app_ml_model(const CBApp *app, char **path);
/* Beat This!'s model (cyanbox/beatthis.h), which the grid is made with:
 * $CYANBOX_BEATTHIS_MODEL, else models/CB_APP_BEAT_MODEL as above (the
 * source tree has it). */
#define CB_APP_BEAT_MODEL "beat-this-f16.gguf"
int  cb_app_beat_model(char **path);
/* Hands it to app->jobs (cb_jobs_set_beat_model); -ENOENT when there is
 * none, with the status set. */
int  cb_app_set_beat_model(CBApp *app);
/* PRO DJ LINK on (src/app/link.c): served on every Ethernet interface, as
 * they come and go (o NULL), or as `o` says (the tests: one interface,
 * ports of their own; its library, when NULL, the app's), by a set
 * (cyanbox/link.h CBLinkSet) whose thread starts the services. While it
 * runs, each tick copies its state into link_state, counts a play of a
 * track of ours that a player has played 30 s of (cb_library_record_play,
 * as the deck's), and, a little after the library last changed, has the
 * services read it again. The deck's output follows the mixer: its LINK
 * CUE while one is heard (and two seconds on), the player's usual output
 * (the one it had when Link first started) otherwise. -EPERM without
 * --pro-link (link_state.enabled false), -EALREADY when on. */
int  cb_app_link_start(CBApp *app, const CBLinkOptions *o);
/* Off: every service stopped (it waits for a request in progress), and the
 * deck back on its usual output. */
void cb_app_link_stop(CBApp *app);
/* Re-checks which audio files are missing; returns how many changed. */
int  cb_app_rescan(CBApp *app);
/* For the browser: a track's cover thumbnail texture and its preview in the
 * waveform palette (PWV6, PWAV or PWV4: wave_palette) on the GPU, made on
 * demand from the library (an
 * artwork job when the library has no thumbnail yet; it is stored there)
 * and cached, the least recently asked for released beyond a few hundred.
 * 0 / NULL while being made or when there is none. Window only; valid
 * until the next cb_app_tick. */
uint64_t   cb_app_artwork_texture(CBApp *app, const CBTrack *t);
CBWaveGpu *cb_app_preview_wave(CBApp *app, const CBTrack *t);
/* The TAG LIST shared with the players (cyanbox/link.h CBLinkTagList): the
 * tracks collected with TAG TRACK on a deck or Add to Tag List here, for
 * this session only. NULL without --pro-link; made when first asked for,
 * and the services share it from their start. */
CBLinkTagList *cb_app_tag_list(CBApp *app);
/* Shows the TAG LIST: the playlist, album and album artist views are
 * cleared and the rows rebuilt; nothing without --pro-link. */
void cb_app_show_tag_list(CBApp *app);
/* The History (cb_library_history), with --pro-link only: what the decks
 * played straight on for 1.5 minutes and what was added to it by hand.
 * The view shows each track once, at its latest play, in the table's own
 * order backwards (the latest first, the numbers going down) until
 * another order is picked; leaving it puts that order back. Nothing
 * without --pro-link. The count is of its tracks, 0 without --pro-link. */
void cb_app_show_history(CBApp *app);
int  cb_app_history_count(CBApp *app);
/* Tells the Link device link_state.devices[index] (a player) to load
 * `track_id` of the library: -ENOENT when it is gone, -EAGAIN before we
 * are on its link (cyanbox/link.h cb_link_load). */
int        cb_app_link_load(CBApp *app, int index, uint64_t track_id);
/* Pushes the library's My Settings, as they are now, to every player and
 * mixer on the link (cyanbox/link.h cb_link_set_push_settings): how many
 * of each; -ENOENT without Link, -EAGAIN before we are on it. The status
 * says it, and how many of the players play, which drop them. */
int        cb_app_link_push_settings(CBApp *app, int *players, int *mixers);
/* The thumbnail is still being looked for (the library's is tried for
 * every track, the file's art only when has_artwork): the 0 that
 * cb_app_artwork_texture() returns may change. */
bool       cb_app_artwork_pending(CBApp *app, const CBTrack *t);

/* The interface (src/ui, C++). */
int  cb_ui_init(CBApp *app);
void cb_ui_shutdown(CBApp *app);
/* Builds one ImGui frame. Returns true when the interface wants another
 * frame soon (animations, a deck playing, a drag). */
bool cb_ui_frame(CBApp *app);

/* Track table columns, in default order. */
enum CBColumn {
    CB_COL_ART = 0, CB_COL_TITLE, CB_COL_ARTIST, CB_COL_ALBUM, CB_COL_BPM, CB_COL_KEY,
    CB_COL_TIME, CB_COL_RATING, CB_COL_COLOR, CB_COL_CUES, CB_COL_PREVIEW, CB_COL_GENRE,
    CB_COL_YEAR, CB_COL_ADDED, CB_COL_PATH, CB_COL_PLAYS, CB_COL_LAST_PLAYED, CB_COL_COUNT,
};

#endif /* CYANBOX_APP_H */
