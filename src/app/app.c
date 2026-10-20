/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The application state the interface reads and writes (cyanbox/app.h):
 * creating and tearing down the library, the jobs, the player and the
 * window; applying job results; loading the deck; the track table's rows
 * and its per-track GPU resources; play history; the export running in
 * the background; and the interface loop.
 *
 * Threads and ownership:
 *  - Everything here runs on the owner (main) thread, which alone touches
 *    the library, the deck and the window. Background work goes through
 *    CBJobs (cyanbox/jobs.h), whose results come back in cb_app_tick(), which
 *    applies and frees them.
 *  - Two job queues: app->jobs (every CPU but one) analyses; a private
 *    two-thread queue decodes for the deck, makes artwork and reads sticks,
 *    so loading a track, showing covers or reading a stick never waits
 *    behind a library-wide analysis.
 *    Request ids are per queue; each kind of request only ever goes to one
 *    of them.
 *  - A decode result's stream is handed to the player: cb_deck_load_stream
 *    takes it over on success (and releases what it replaces only once the
 *    real-time thread and the deck's decoding thread have let go of it); on
 *    failure it stays ours to free. The job only opens the stream, a few
 *    milliseconds, so LOADING shows for about as long as a frame takes.
 *    The track to follow the deck's (the next row of the playlist being
 *    viewed, continue_play) is decoded the same way ahead of the end and
 *    queued on the deck (cb_deck_queue_next), which plays on into it with
 *    no gap; the view follows once the player says it has moved on.
 *  - The deck view's waveforms are its own copy, read from the library;
 *    their GPU uploads, the deck's artwork texture and the browser cache's
 *    textures and previews belong to this file and are released through
 *    the window before it goes.
 *  - The export runs on a thread of its own with a library handle of its
 *    own (a second connection to the same database): cb_export() wants a
 *    mutable library, and sharing the interface's would race with every
 *    edit made while it runs. What the tracks lack is analysed first here,
 *    through the normal jobs (the export waits for its own tracks'
 *    analyses, not for the rest of the queue), and pending saves are
 *    flushed, so the export's handle finds everything in the database. That handle is a
 *    snapshot the export thread opens as the dry run starts (reading the
 *    library and looking at every file take a while, and the interface
 *    goes on), in one read transaction, so an edit committed meanwhile is
 *    in it whole or not at all; the dry run and the real run use the same
 *    one, so what the interface asked about (the files to delete) is what
 *    the real run does, whatever was edited meanwhile. Progress
 *    comes back as malloc'd messages (strings copied) through a mailbox;
 *    the only other shared state is the cancel flag and the finished
 *    flag/result, published with release/acquire.
 *  - Saves asked for by the deck controls are queued and written once per
 *    tick, so a burst of edits costs one transaction.
 *  - Tagging with the model is the user's to ask for (the track menu, the
 *    Analysis menu, `cyanbox tag`), never done unasked: it holds the GPU for
 *    seconds a track and up to 7 GB of its memory. Its results come back
 *    through app->jobs like an analysis's; the batch's progress is the
 *    jobs' own (cb_jobs_tag_progress). */

#include "cyanbox/app.h"

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "cyanbox/analysis.h"
#include "cyanbox/export.h"
#include "cyanbox/formats.h"
#include "cyanbox/mpris.h"
#include "cyanbox/notation.h"

#include "applink.h"

/* Job results applied per tick: an analysis writes the track and its
 * waveforms, so a long queue is spread over several frames. */
#define RESULTS_PER_TICK 16
/* Cover art and previews kept on the GPU for the browser. */
#define GPU_CACHE_MAX    400
/* Thumbnails decoded and previews uploaded per tick (the rest come on the
 * next frames), so scrolling a long table never stalls a frame. */
#define GPU_LOADS_PER_TICK 4
/* rekordbox counts a play once a track has played this long. It is
 * counted in distinct audio, in bins of PLAY_BIN seconds of the track (one
 * bit each, PLAY_BINS of them: 2.3 hours, the rest share the last bin), so
 * a loop, or a hot cue called again and again, counts what it plays once. */
#define PLAY_RECORD_SECONDS 30.0
#define PLAY_BIN            0.25
#define PLAY_BINS           (4096 * 8)
/* A forward step of the playhead longer than this, and longer than the
 * time since the last tick allows, is a seek or a jump, not playing. */
#define PLAY_MAX_STEP       0.5
/* A deck that stops this close to the end played to the end (the player
 * pauses it on the last frame). */
#define END_SLACK           0.05
/* The track to follow the deck's is queued on the deck this long before
 * the end (continue_play): its first seconds decoded well ahead of the
 * end, without a second track's pipelines running for the whole of the
 * first. */
#define NEXT_LEAD_SECONDS   30.0
/* Workers for decoding and artwork. */
#define IO_THREADS       2
/* Artwork jobs scale to this; it is also the library thumbnail's size. */
#define ART_SIZE         240
/* A failed analysis's message stays in the status bar this long before
 * the next tracks' "Analysed ..." may replace it: as long as the bar shows
 * a message as new (src/ui/ui.cpp, status_bar). */
#define STATUS_HOLD_US   8000000

typedef struct CBAppPriv CBAppPriv;

typedef struct Inflight {
    uint64_t request;
    uint64_t track_id;
    unsigned what;
    bool     override_lock;     /* a forced analysis of this one track replaces a locked grid */
} Inflight;

enum GpuState {
    GPU_UNKNOWN = 0,
    GPU_LOADING,
    GPU_DONE,                   /* made, or known to be absent */
};

typedef struct GpuEntry {
    uint64_t   track_id;
    uint64_t   texture;         /* the cover's, shared (artwork_id), or its own */
    int64_t    artwork_id;      /* the library's cover the texture is of; 0 its own */
    CBWaveGpu *preview;
    uint64_t   art_request;     /* io job in flight */
    uint8_t    art_state;
    uint8_t    wave_state;
    uint64_t   used;            /* tick when last asked for */
} GpuEntry;

/* A library cover on the GPU, for every browser entry whose track has it
 * (an album's tracks have one): made once, gone with the last entry. */
typedef struct CoverTexture {
    int64_t  artwork_id;
    uint64_t texture;
    int      refs;
} CoverTexture;

/* What a load of a track's stored waveforms found (waves_stale). */
typedef struct WaveCheck {
    uint64_t track_id;
    bool     stale;
} WaveCheck;

typedef struct ExportMsg {
    char  stage[32];
    char *item;
    int   done, total;
} ExportMsg;

/* Owned by the main thread; the export thread reads the inputs, writes
 * `result` (and `opened`), then sets `finished` last. The library is the
 * export's snapshot (CBAppPriv.export_lib), the thread's alone while it
 * runs; with `root` set the thread opens it first. */
typedef struct ExportJob {
    CBLibrary *lib;
    char    *root;              /* the library to open as the snapshot; NULL: open already */
    bool     opened;            /* the thread opened the snapshot (export_join takes it over) */
    char    *dest;
    int64_t *playlists;         /* ids of the selected playlists */
    int      nb_playlists;
    bool     whole_library, prune, ignore_unsynced, dry_run;
    int      format;
    CBMailbox *mb;
    CBCancel  *cancel;
    CBExportStats stats;        /* written by the export thread, read by the ticks */
    int      result;
    int      finished;          /* atomic */
} ExportJob;

/* An album artist's tracks by their ranks: [lo, hi), and for None also
 * None's, the ranks below nb_none. */
typedef struct ArtistSpan {
    int  lo, hi;
    bool none;
} ArtistSpan;

struct CBAppPriv {
    bool     lib_open;
    bool     ui_ready;
    int      max_frames;
    uint64_t tick;

    CBJobs  *io_jobs;           /* decodes and artwork */
    CBArray  inflight;          /* Inflight: analyses queued and not yet applied */
    int      analyses_failed;   /* since the queue was last empty */
    int64_t  status_held;       /* status_time of a failure's status, kept a while (0 none) */
    CBArray  saves;             /* uint64_t: tracks whose state is due to be written */

    int      uploaded_palette[CB_DECKS];    /* palette of the deck's GPU waveforms, -1 none */
    uint64_t deck_track_id[CB_DECKS];
    uint8_t  play_seen[CB_DECKS][PLAY_BINS / 8];   /* bins of the track played since the load */
    int      play_bins[CB_DECKS];           /* bits set in play_seen */
    double   play_last_pos[CB_DECKS];       /* position at the last tick, < 0 none */
    int64_t  play_last_us[CB_DECKS];
    bool     play_recorded[CB_DECKS];
    bool     was_playing[CB_DECKS];         /* at the last tick (continue_play) */
    uint64_t ended_for[CB_DECKS];           /* stopped at its end waiting for this next (the
                                               candidate then); 0 none */
    uint64_t next_request[CB_DECKS];        /* the decode of the track queued to follow, in
                                               flight on io_jobs; 0 none */
    uint64_t next_failed_id[CB_DECKS];      /* a track whose decode or queueing failed: not
                                               asked for again while it stays the candidate */
    unsigned deck_advanced[CB_DECKS];       /* CBDeckState.advanced as last seen */

    CBArray  gpu;               /* GpuEntry, the browser's cache */
    CBArray  covers;            /* CoverTexture: the entries' covers, one per library cover */
    int      gpu_budget;
    /* The History's plays, oldest first (history_tracks): each one's
     * track, its row's id and its set; each set's start and end, oldest
     * first. For lib.generation history_generation, 0 none. */
    uint64_t *history;
    int64_t  *history_entry;
    int      *history_set;
    int       nb_history;
    unsigned  history_generation;
    int64_t  *history_sets;
    int       nb_history_sets;
    CBArray  albums;            /* CBAlbum, the names copies: the tree's list (cb_app_albums) */
    CBArray  album_artists;     /* CBAlbumArtist, the names copies (cb_app_album_artists) */
    CBArray  artist_spans;      /* ArtistSpan, one per album artist */
    unsigned albums_generation; /* lib.generation it was made for; 0 none */
    /* Per library index, made with the albums (cb_app_albums): the
     * track's place in the collection's own order, None's tracks (the
     * first nb_none) then each album's in the list's order. */
    int     *rank;
    int      nb_rank, nb_none;
    CBArray  wave_checks;       /* WaveCheck: the stored waveforms looked at */

    CBMailbox *export_mb;
    CBCancel   export_cancel;
    int        export_phase;    /* enum ExportPhase */
    int64_t   *export_playlists;    /* ids of the selected playlists, taken at start */
    int        nb_export_playlists;
    CBLibrary  export_lib;      /* the snapshot the dry run and the real run export */
    bool       export_lib_open;
    unsigned   export_generation;   /* lib.generation when the snapshot was taken */
    ExportJob *export_job;      /* the run on the thread, NULL between runs */
    bool       export_started;  /* the thread exists and must be joined */
    pthread_t  export_thread;
    char       export_last_stage[32];
    uint64_t  *export_wait;     /* the export's tracks with an analysis in flight, sorted */
    int        nb_export_wait;
    int        export_wait_total;
    bool       export_idle;     /* the jobs were seen with nothing pending (export_lost) */
    int        export_idle_done;    /* app->analysing_done then */
};

/* ------------------------------------------------------------------ helpers */

/* The players' My Settings that act on the app: what an export would write
 * (cb_settings_get), read when they are needed, so a change in the Player
 * settings dialog or a sync takes effect at once. */
static bool my_setting_is(const CBConfig *cfg, const char *key, const char *value)
{
    const char *v = cb_settings_get(cfg, key);
    return v && !strcmp(v, value);
}

/* The waveform palette is the players' WAVEFORM COLOR (device.waveform_color):
 * what the stick will show is what the deck and the table show. */
static int palette_of(const CBConfig *cfg)
{
    if (my_setting_is(cfg, "device.waveform_color", "blue"))
        return 1;
    if (my_setting_is(cfg, "device.waveform_color", "rgb"))
        return 2;
    return 0;
}

/* QUANTIZE BEAT VALUE: the part of a beat QUANTIZE snaps to. */
static double quantize_beats_of(const CBConfig *cfg)
{
    const char *v = cb_settings_get(cfg, "player.quantize_beat_value");
    if (v && !strcmp(v, "1/2"))
        return 0.5;
    if (v && !strcmp(v, "1/4"))
        return 0.25;
    if (v && !strcmp(v, "1/8"))
        return 0.125;
    return 1.0;
}

/* AUTO CUE and AUTO CUE LEVEL: where a load leaves the deck. */
static CBAutoCue auto_cue_of(const CBConfig *cfg)
{
    if (my_setting_is(cfg, "player.auto_cue", "false"))
        return CB_AUTO_CUE_OFF;
    if (my_setting_is(cfg, "player.auto_cue_level", "memory"))
        return CB_AUTO_CUE_MEMORY;
    return CB_AUTO_CUE_LEVEL;
}

static void analysis_options(const CBConfig *cfg, CBAnalysisOptions *opt)
{
    cb_analysis_options_default(opt);
    if (cfg->bpm_min > 0 && cfg->bpm_max > cfg->bpm_min) {
        opt->bpm_min = cfg->bpm_min;
        opt->bpm_max = cfg->bpm_max;
    }
    opt->snap_bpm = cfg->snap_bpm;
    opt->allow_sections = cfg->allow_sections;
}

static const char *track_name(const CBTrack *t, char *buf, size_t size)
{
    return cb_track_display(t, buf, size);
}

void cb_app_set_status(CBApp *app, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, args);
    va_end(args);
    app->status_time = cb_time_us();
    app->dirty = true;
    cb_debug("status: %s", app->status);
}

/* Decodes the library's thumbnail of a track (owner thread). */
static int artwork_from_library(CBApp *app, uint64_t id, uint8_t **rgba, int *w, int *h)
{
    uint8_t *jpeg = NULL;
    size_t size = 0;
    int tw = 0, th = 0;
    int ret = cb_library_artwork_load(&app->lib, id, &jpeg, &size, &tw, &th);
    if (ret < 0)
        return ret;
    ret = cb_image_decode(jpeg, size, rgba, w, h);
    free(jpeg);
    return ret;
}

/* ---------------------------------------------------------------- waveforms */

static int queue_analysis(CBApp *app, CBTrack *t, unsigned what, bool force, bool override_lock);

static void waves_seen(const CBApp *app, uint64_t id, bool stale)
{
    CBAppPriv *priv = app->priv;
    if (!priv)
        return;
    for (size_t i = 0; i < priv->wave_checks.nb; i++) {
        WaveCheck *c = cb_array_at(&priv->wave_checks, i);
        if (c->track_id == id) {
            c->stale = stale;
            return;
        }
    }
    /* Out of memory: the track is only looked at again. */
    WaveCheck c = { .track_id = id, .stale = stale };
    cb_array_push(&priv->wave_checks, &c);
}

/* Waveforms stored in a format version the library no longer reads
 * (cb_waveforms_unpack's -EPROTO: made before the PWV6/PWV7
 * recalibration) are as good as none, and the track is analysed for them
 * again. A track's are looked at once: the deck and the table's preview
 * record what their loads find, the rest is a look at the stored header
 * here, cheap enough for the whole library at start. */
static bool waves_stale(const CBApp *app, const CBTrack *t)
{
    if (!t->has_waveforms)
        return false;
    CBAppPriv *priv = app->priv;
    for (size_t i = 0; priv && i < priv->wave_checks.nb; i++) {
        const WaveCheck *c = cb_array_at(&priv->wave_checks, i);
        if (c->track_id == t->id)
            return c->stale;
    }
    int ret = cb_library_waveforms_check(&app->lib, t->id);
    if (ret == CB_ERROR(EPROTO))
        cb_debug("track %llu: stored waveforms are an older format; recomputing",
                 (unsigned long long)t->id);
    bool stale = ret == CB_ERROR(EPROTO);
    waves_seen(app, t->id, stale);
    return stale;
}

/* -------------------------------------------------------------------- saves */

void cb_app_save_track(CBApp *app, CBTrack *t)
{
    if (!t)
        return;
    int ret = cb_library_save_track(&app->lib, t);
    if (ret < 0) {
        char name[256];
        app->last_error_count++;
        cb_app_set_status(app, "Cannot save %s: %s", track_name(t, name, sizeof(name)),
                          strerror(-ret));
    }
    app->rows_dirty = true;
}

static void queue_save(CBApp *app, CBTrack *t)
{
    CBAppPriv *priv = app->priv;
    if (!priv) {
        cb_app_save_track(app, t);
        return;
    }
    for (size_t i = 0; i < priv->saves.nb; i++) {
        if (*(uint64_t *)cb_array_at(&priv->saves, i) == t->id)
            return;
    }
    if (!cb_array_push(&priv->saves, &t->id))
        cb_app_save_track(app, t);
}

static bool flush_saves(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    if (!priv || !priv->saves.nb)
        return false;
    for (size_t i = 0; i < priv->saves.nb; i++) {
        CBTrack *t = cb_library_find_id(&app->lib, *(uint64_t *)cb_array_at(&priv->saves, i));
        if (t)
            cb_app_save_track(app, t);
    }
    cb_array_clear(&priv->saves);
    return true;
}

/* The deck controls edited a track's cues or grid (owner thread). */
static void deck_changed(void *opaque, int deck)
{
    CBApp *app = opaque;
    CBTrack *t = app->decks[deck].ctl.track;
    if (t)
        queue_save(app, t);
    app->dirty = true;
}

/* -------------------------------------------------------- browser GPU cache */

/* The texture of a library cover for one more entry: the one made,
 * else one made of `rgba` when given, else of the library's thumbnail. 0
 * when none could be made. */
static uint64_t cover_ref(CBApp *app, int64_t artwork_id, uint64_t track_id,
                          const uint8_t *rgba, int w, int h)
{
    CBAppPriv *priv = app->priv;
    for (size_t i = 0; i < priv->covers.nb; i++) {
        CoverTexture *c = cb_array_at(&priv->covers, i);
        if (c->artwork_id == artwork_id) {
            c->refs++;
            return c->texture;
        }
    }
    uint64_t texture = 0;
    if (rgba) {
        texture = cb_texture_create(app->window, rgba, w, h);
    } else {
        uint8_t *own = NULL;
        if (artwork_from_library(app, track_id, &own, &w, &h) >= 0)
            texture = cb_texture_create(app->window, own, w, h);
        free(own);
    }
    CoverTexture c = { artwork_id, texture, 1 };
    if (texture && !cb_array_push(&priv->covers, &c)) {
        cb_texture_destroy(app->window, texture);
        texture = 0;
    }
    return texture;
}

static void cover_unref(CBApp *app, int64_t artwork_id)
{
    CBAppPriv *priv = app->priv;
    for (size_t i = 0; i < priv->covers.nb; i++) {
        CoverTexture *c = cb_array_at(&priv->covers, i);
        if (c->artwork_id != artwork_id)
            continue;
        if (--c->refs <= 0) {
            if (app->window)
                cb_texture_destroy(app->window, c->texture);
            cb_array_remove(&priv->covers, i);
        }
        return;
    }
}

static void gpu_entry_release(CBApp *app, GpuEntry *e)
{
    if (e->artwork_id)
        cover_unref(app, e->artwork_id);
    else if (e->texture && app->window)
        cb_texture_destroy(app->window, e->texture);
    if (e->preview && app->window)
        cb_wave_release(app->window, e->preview);
    if (e->art_request && app->priv->io_jobs)
        cb_jobs_cancel(app->priv->io_jobs, e->art_request);
    e->texture = 0;
    e->artwork_id = 0;
    e->preview = NULL;
    e->art_request = 0;
}

static GpuEntry *gpu_find(CBAppPriv *priv, uint64_t id)
{
    for (size_t i = 0; i < priv->gpu.nb; i++) {
        GpuEntry *e = cb_array_at(&priv->gpu, i);
        if (e->track_id == id)
            return e;
    }
    return NULL;
}

/* The entry for a track, made when absent; beyond GPU_CACHE_MAX the one
 * asked for least recently (and not this tick) is recycled. */
static GpuEntry *gpu_entry(CBApp *app, uint64_t id)
{
    CBAppPriv *priv = app->priv;
    GpuEntry *e = gpu_find(priv, id);
    if (e)
        return e;

    if (priv->gpu.nb >= GPU_CACHE_MAX) {
        GpuEntry *old = NULL;
        for (size_t i = 0; i < priv->gpu.nb; i++) {
            GpuEntry *c = cb_array_at(&priv->gpu, i);
            if (c->used != priv->tick && (!old || c->used < old->used))
                old = c;
        }
        if (!old)
            return NULL;
        gpu_entry_release(app, old);
        e = old;
    } else {
        e = cb_array_push(&priv->gpu, NULL);
        if (!e)
            return NULL;
    }
    *e = (GpuEntry) {
        .track_id = id,
        .used     = priv->tick,
    };
    return e;
}

uint64_t cb_app_artwork_texture(CBApp *app, const CBTrack *t)
{
    CBAppPriv *priv = app->priv;
    if (!priv || !app->window || !t)
        return 0;
    GpuEntry *e = gpu_entry(app, t->id);
    if (!e)
        return 0;
    e->used = priv->tick;

    /* The library's thumbnail whatever the track's flag says, on the GPU
     * once for every track that has the cover; else the file's art, when
     * the track has some. Either way once: GPU_DONE without a texture is
     * the answer "none" until the entry is recycled. */
    if (e->art_state == GPU_UNKNOWN && priv->gpu_budget > 0) {
        priv->gpu_budget--;
        int64_t art = cb_library_artwork_id(&app->lib, t->id);
        if (art > 0)
            e->texture = cover_ref(app, art, t->id, NULL, 0, 0);
        if (e->texture) {
            e->artwork_id = art;
            e->art_state = GPU_DONE;
        } else if (t->has_artwork && !t->missing) {
            uint64_t req = cb_jobs_artwork(priv->io_jobs, t->id, t->path, ART_SIZE);
            e->art_request = (int64_t)req > 0 ? req : 0;
            e->art_state = e->art_request ? GPU_LOADING : GPU_DONE;
        } else {
            e->art_state = GPU_DONE;
        }
    }
    return e->texture;
}

bool cb_app_artwork_pending(CBApp *app, const CBTrack *t)
{
    CBAppPriv *priv = app->priv;
    if (!priv || !app->window || !t)
        return false;
    const GpuEntry *e = gpu_find(priv, t->id);
    return !e || e->art_state != GPU_DONE;
}

/* A preview tag on the GPU in a palette: PWV6 (3-band), PWAV (blue) or
 * PWV4 (RGB), the overview the players draw in that WAVEFORM COLOR. */
static CBWaveGpu *upload_preview(CBApp *app, int palette, const CBWaveforms *w)
{
    switch (palette) {
    case 1:
        return cb_wave_upload(app->window, CB_WAVE_BLUE, w->pwav, CB_WAVE_PWAV_COLUMNS);
    case 2:
        return cb_wave_upload(app->window, CB_WAVE_RGB_PREVIEW, w->pwv4, CB_WAVE_PWV4_COLUMNS);
    default:
        return cb_wave_upload(app->window, CB_WAVE_3BAND, w->pwv6, CB_WAVE_PWV6_COLUMNS);
    }
}

CBWaveGpu *cb_app_preview_wave(CBApp *app, const CBTrack *t)
{
    CBAppPriv *priv = app->priv;
    if (!priv || !app->window || !t)
        return NULL;
    GpuEntry *e = gpu_entry(app, t->id);
    if (!e)
        return NULL;
    e->used = priv->tick;

    /* Unknown until the track has waveforms: the analysis drops the entry's
     * state when they land, a change of palette every entry's. */
    if (e->wave_state == GPU_UNKNOWN && t->has_waveforms && priv->gpu_budget > 0) {
        priv->gpu_budget--;
        CBWaveforms w = { 0 };
        int ret = cb_library_waveforms_load_previews(&app->lib, t->id, &w);
        if (ret >= 0)
            e->preview = upload_preview(app, app->wave_palette, &w);
        cb_waveforms_free(&w);
        e->wave_state = GPU_DONE;
        /* Stale ones are made again; the preview follows (gpu_forget_waves). */
        waves_seen(app, t->id, ret == CB_ERROR(EPROTO));
        CBTrack *lt = ret == CB_ERROR(EPROTO) ? cb_library_find_id(&app->lib, t->id) : NULL;
        if (lt)
            queue_analysis(app, lt, CB_ANALYSE_WAVES, false, false);
    }
    return e->preview;
}

/* The waveforms of a track changed: its preview is made again on demand. */
static void gpu_forget_waves(CBApp *app, uint64_t id)
{
    if (!app->priv)
        return;
    GpuEntry *e = gpu_find(app->priv, id);
    if (!e)
        return;
    if (e->preview && app->window)
        cb_wave_release(app->window, e->preview);
    e->preview = NULL;
    e->wave_state = GPU_UNKNOWN;
}

/* The palette changed: every preview is made again, in the new one, as
 * the table asks for it. */
static void gpu_forget_previews(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    for (size_t i = 0; i < priv->gpu.nb; i++) {
        GpuEntry *e = cb_array_at(&priv->gpu, i);
        if (e->preview && app->window)
            cb_wave_release(app->window, e->preview);
        e->preview = NULL;
        e->wave_state = GPU_UNKNOWN;
    }
}

static void gpu_cache_free(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    for (size_t i = 0; i < priv->gpu.nb; i++)
        gpu_entry_release(app, cb_array_at(&priv->gpu, i));
    cb_array_free(&priv->gpu);
    cb_array_free(&priv->covers);
}

/* --------------------------------------------------------------------- deck */

static void release_waves_gpu(CBApp *app, CBDeckView *dv)
{
    if (app->window) {
        if (dv->wave_preview)
            cb_wave_release(app->window, dv->wave_preview);
        if (dv->wave_detail)
            cb_wave_release(app->window, dv->wave_detail);
    }
    dv->wave_preview = NULL;
    dv->wave_detail = NULL;
}

static void release_artwork(CBApp *app, CBDeckView *dv)
{
    if (app->window && dv->artwork)
        cb_texture_destroy(app->window, dv->artwork);
    dv->artwork = 0;
}

/* Uploads the deck's waveforms in the current palette (or releases them
 * when there are none). */
static void deck_upload_waves(CBApp *app, int deck)
{
    CBDeckView *dv = &app->decks[deck];
    release_waves_gpu(app, dv);
    if (app->priv)
        app->priv->uploaded_palette[deck] = app->wave_palette;

    const CBWaveforms *w = &dv->waves;
    if (!app->window || w->nb_detail <= 0)
        return;

    dv->wave_preview = upload_preview(app, app->wave_palette, w);
    switch (app->wave_palette) {
    case 1:
        if (w->pwv3)
            dv->wave_detail = cb_wave_upload(app->window, CB_WAVE_BLUE, w->pwv3, w->nb_detail);
        break;
    case 2:
        if (w->pwv5)
            dv->wave_detail = cb_wave_upload(app->window, CB_WAVE_RGB_DETAIL, w->pwv5,
                                             w->nb_detail);
        break;
    default:
        if (w->pwv7)
            dv->wave_detail = cb_wave_upload(app->window, CB_WAVE_3BAND, w->pwv7, w->nb_detail);
        break;
    }
}

/* Reads the deck track's waveforms from the library into the deck view;
 * the GPU copy follows on the next tick. */
static void deck_load_waves(CBApp *app, int deck)
{
    CBDeckView *dv = &app->decks[deck];
    cb_waveforms_free(&dv->waves);
    memset(&dv->waves, 0, sizeof(dv->waves));
    if (app->priv)
        app->priv->uploaded_palette[deck] = -1;
    if (!dv->track || !dv->track->has_waveforms)
        return;

    /* Stale ones count as none: the analysis queued with the load (or
     * after this one) makes them again. */
    int ret = cb_library_waveforms_load(&app->lib, dv->track->id, &dv->waves);
    waves_seen(app, dv->track->id, ret == CB_ERROR(EPROTO));
    if (ret < 0) {
        if (ret != CB_ERROR(EPROTO))
            cb_warn("cannot read the waveforms of track %llu: %s",
                    (unsigned long long)dv->track->id, strerror(-ret));
        cb_waveforms_free(&dv->waves);
        memset(&dv->waves, 0, sizeof(dv->waves));
    }
}

static void deck_set_artwork(CBApp *app, CBDeckView *dv, const uint8_t *rgba, int w, int h)
{
    release_artwork(app, dv);
    if (app->window && rgba)
        dv->artwork = cb_texture_create(app->window, rgba, w, h);
}

/* Drops what the deck view holds for its track: the artwork request, the
 * waveforms and their uploads, the artwork, the play count. The player is
 * left alone. */
static void deck_view_reset(CBApp *app, int deck)
{
    CBDeckView *dv = &app->decks[deck];
    CBAppPriv *priv = app->priv;

    if (priv && priv->io_jobs && dv->artwork_request)
        cb_jobs_cancel(priv->io_jobs, dv->artwork_request);
    dv->artwork_request = 0;

    release_waves_gpu(app, dv);
    release_artwork(app, dv);
    cb_waveforms_free(&dv->waves);
    memset(&dv->waves, 0, sizeof(dv->waves));
    dv->track = NULL;
    dv->nb_taps = 0;
    if (priv) {
        priv->deck_track_id[deck] = 0;
        priv->uploaded_palette[deck] = -1;
        memset(priv->play_seen[deck], 0, sizeof(priv->play_seen[deck]));
        priv->play_bins[deck] = 0;
        priv->play_last_pos[deck] = -1.0;
        priv->play_last_us[deck] = 0;
        priv->play_recorded[deck] = false;
    }
}

/* Drops the track queued to follow the deck's: its decode in flight, or
 * what the player holds of it. The deck may have moved on into it already
 * (it reached its end before the drop did; with MASTER TEMPO on, up to the
 * shifter's delay before the move is heard): the player says so as long
 * as it may (has_next), and until then the track stays the one to follow,
 * so that the view follows the deck into it (continue_play) and nothing
 * else is queued meanwhile. */
static void cancel_next(CBApp *app, int deck)
{
    CBAppPriv *priv = app->priv;
    CBDeckView *dv = &app->decks[deck];
    if (priv) {
        if (priv->io_jobs && priv->next_request[deck])
            cb_jobs_cancel(priv->io_jobs, priv->next_request[deck]);
        priv->next_request[deck] = 0;
    }
    if (app->player) {
        cb_deck_clear_next(app->player, deck);
        if (cb_deck_has_next(app->player, deck))
            return;
    }
    dv->next_track_id = 0;
}

/* Empties the deck: cancels its loads, unloads the player, drops the
 * waveforms and the artwork. */
static void deck_clear(CBApp *app, int deck)
{
    CBDeckView *dv = &app->decks[deck];
    CBAppPriv *priv = app->priv;

    if (priv && priv->io_jobs && dv->loading_request)
        cb_jobs_cancel(priv->io_jobs, dv->loading_request);
    dv->loading_request = 0;
    dv->loading_track_id = 0;
    cancel_next(app, deck);
    if (priv)
        priv->ended_for[deck] = 0;

    if (app->player) {
        cb_ctl_unload(&dv->ctl);
        cb_deck_unload(app->player, deck);
    }
    deck_view_reset(app, deck);
}

/* The deck's cover: the library's thumbnail, or a job for the file's. */
static void deck_fetch_artwork(CBApp *app, int deck, const CBTrack *t)
{
    CBDeckView *dv = &app->decks[deck];
    if (!app->window)
        return;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (artwork_from_library(app, t->id, &rgba, &w, &h) >= 0) {
        deck_set_artwork(app, dv, rgba, w, h);
        free(rgba);
    } else {
        uint64_t req = cb_jobs_artwork(app->priv->io_jobs, t->id, t->path, ART_SIZE);
        dv->artwork_request = (int64_t)req > 0 ? req : 0;
    }
}

int cb_app_load_deck(CBApp *app, int deck, CBTrack *t)
{
    if (deck < 0 || deck >= CB_DECKS || !t)
        return CB_ERROR(EINVAL);
    if (!app->player || !app->priv || !app->priv->io_jobs)
        return CB_ERROR(ENODEV);

    char name[256];
    track_name(t, name, sizeof(name));
    if (t->missing) {
        cb_app_set_status(app, "Cannot load %s: the file is missing", name);
        return CB_ERROR(ENOENT);
    }

    /* Replacing a playing track is the interface's decision (it pauses
     * the deck, then calls again). */
    CBDeckState st;
    cb_deck_state(app->player, deck, &st);
    if (st.playing)
        return CB_ERROR(EBUSY);

    CBDeckView *dv = &app->decks[deck];
    CBJobs *io = app->priv->io_jobs;
    if (dv->loading_request)
        cb_jobs_cancel(io, dv->loading_request);
    dv->loading_request = 0;
    dv->loading_track_id = 0;
    /* A new start: what was to follow the old track goes. */
    cancel_next(app, deck);
    app->priv->ended_for[deck] = 0;

    uint64_t req = cb_jobs_decode(io, t->id, t->path, deck);
    if ((int64_t)req < 0) {
        cb_app_set_status(app, "Cannot load %s: %s", name, strerror((int)-(int64_t)req));
        return (int)(int64_t)req;
    }

    dv->loading_request = req;
    dv->loading_track_id = t->id;
    cb_app_set_status(app, "Loading %s", name);
    return 0;
}

void cb_app_unload_deck(CBApp *app, int deck)
{
    if (deck < 0 || deck >= CB_DECKS)
        return;
    deck_clear(app, deck);
    app->dirty = true;
}

/* rekordbox counts a play after 30 seconds of playing; so does the
 * history the stick sync compares against. Once per load, and only for
 * audio that really played forward: a seek, a jump, a loop wrapping or a
 * scrub adds nothing, and what a loop or a repeated hot cue plays again
 * counts once (PLAY_BIN). */
static void track_plays(CBApp *app, int deck)
{
    CBAppPriv *priv = app->priv;
    CBDeckView *dv = &app->decks[deck];
    if (!app->player || !dv->track || priv->play_recorded[deck])
        return;

    CBDeckState st;
    cb_deck_state(app->player, deck, &st);
    int64_t now = cb_time_us();
    double last = priv->play_last_pos[deck];
    double since = (now - priv->play_last_us[deck]) * 1e-6;
    priv->play_last_pos[deck] = st.loaded ? st.position : -1.0;
    priv->play_last_us[deck] = now;
    if (!st.loaded || !st.playing || st.scrubbing || last < 0)
        return;

    double step = st.position - last;
    double allowed = CB_MAX(PLAY_MAX_STEP, since * CB_MAX(st.tempo, 1.0) * 1.25 + 0.05);
    if (!(step > 0.0) || step > allowed)
        return;

    int b0 = (int)CB_MIN(floor(last / PLAY_BIN), PLAY_BINS - 1.0);
    int b1 = (int)CB_MIN(floor(st.position / PLAY_BIN), PLAY_BINS - 1.0);
    for (int b = b0; b <= b1; b++) {
        uint8_t bit = (uint8_t)(1u << (b & 7));
        if (priv->play_seen[deck][b >> 3] & bit)
            continue;
        priv->play_seen[deck][b >> 3] |= bit;
        priv->play_bins[deck]++;
    }
    if (priv->play_bins[deck] * PLAY_BIN < PLAY_RECORD_SECONDS)
        return;

    priv->play_recorded[deck] = true;
    int ret = cb_library_record_play(&app->lib, dv->track);
    if (ret < 0) {
        char name[256];
        app->last_error_count++;
        cb_app_set_status(app, "Cannot record the play of %s: %s",
                          track_name(dv->track, name, sizeof(name)), strerror(-ret));
    }
    app->rows_dirty = true;
}

/* The track that follows `t` when the deck plays it to the end. The row
 * after it in the playlist being viewed, when it is in it: auditioning a
 * playlist plays it through, and stops after its last track (ours). With
 * PLAY MODE CONTINUE (player.play_mode; manual p.75) also in every other
 * view: the next row of the current view in the table's order (the first
 * when `t` is not in the view), wrapping at the end. Past missing files.
 * NULL when there is none. */
static CBTrack *next_candidate(CBApp *app, const CBTrack *t)
{
    if (app->rows_dirty || app->rows_generation != app->lib.generation || !app->rows)
        cb_app_rebuild_rows(app);
    bool wrap = my_setting_is(&app->lib.cfg, "player.play_mode", "continue");
    if (!wrap && !app->current_playlist && !app->current_album && !app->current_album_artist &&
        !app->current_tag_list && !app->current_history)
        return NULL;
    int n = app->nb_rows;
    int idx = cb_library_index_of(&app->lib, t);
    int at = -1;
    for (int i = 0; i < n && at < 0; i++) {
        if ((int)app->rows[i] == idx)
            at = i;
    }
    if (at < 0 && !wrap)
        return NULL;
    for (int k = 1; k <= n; k++) {
        int i = at + k;
        if (i >= n) {
            if (!wrap)
                return NULL;
            i %= n;
        }
        CBTrack *next = app->lib.tracks[app->rows[i]];
        if (!next->missing)
            return next;
    }
    return NULL;
}

/* The deck has moved on to the track queued after its own
 * (CBDeckState.advanced): the view follows; the playhead plays on. */
static void take_over(CBApp *app, int deck, CBTrack *t)
{
    CBDeckView *dv = &app->decks[deck];
    deck_view_reset(app, deck);
    if (!t) {
        /* Gone from the library since it was queued: the deck plays what
         * cannot be shown. */
        cb_app_set_status(app, "The next track left the library meanwhile");
        deck_clear(app, deck);
        return;
    }
    char name[256];
    track_name(t, name, sizeof(name));
    dv->track = t;
    app->priv->deck_track_id[deck] = t->id;
    dv->ctl.auto_cue = auto_cue_of(&app->lib.cfg);
    cb_ctl_continue(&dv->ctl, t);
    deck_load_waves(app, deck);
    queue_analysis(app, t, CB_ANALYSE_ALL, false, false);
    deck_fetch_artwork(app, deck, t);
    cb_app_set_status(app, "Playing %s", name);
}

/* The track to follow: decoded on the io queue as a load is, handed to
 * the player when it comes (handle_next). */
static void queue_next_track(CBApp *app, int deck, const CBTrack *t)
{
    CBAppPriv *priv = app->priv;
    uint64_t req = cb_jobs_decode(priv->io_jobs, t->id, t->path, deck);
    if ((int64_t)req < 0) {
        cb_warn("cannot queue the next track: %s", strerror((int)-(int64_t)req));
        return;
    }
    priv->next_request[deck] = req;
    app->decks[deck].next_track_id = t->id;
}

/* What follows the deck's track (next_candidate) is kept queued on the
 * deck (cb_deck_queue_next) from NEXT_LEAD_SECONDS before the end, so the
 * deck plays on into it with no gap, and the view follows the deck's move
 * (take_over). The candidate goes with the view: a view that changes
 * changes what is queued. A deck that reaches its end before the next is
 * in (a seek into the last second, the decode in flight) stops there and
 * plays on when it comes. With nothing to follow, the deck stays paused
 * on the end, where the player leaves it (PLAY MODE SINGLE). The end is a
 * deck that was playing at the last tick and now stands on its last
 * frames, stopped. */
static bool continue_play(CBApp *app, int deck)
{
    CBAppPriv *priv = app->priv;
    CBDeckView *dv = &app->decks[deck];
    bool changed = false;

    CBDeckState st;
    cb_deck_state(app->player, deck, &st);
    if (st.advanced != priv->deck_advanced[deck]) {
        priv->deck_advanced[deck] = st.advanced;
        if (dv->next_track_id) {
            CBTrack *t = cb_library_find_id(&app->lib, dv->next_track_id);
            dv->next_track_id = 0;
            priv->ended_for[deck] = 0;
            take_over(app, deck, t);
            cb_deck_state(app->player, deck, &st);
            changed = true;
        }
    }
    /* The player let go of the track to follow without moving on into it
     * (a drop it took in time, a load): nothing follows any more. */
    if (dv->next_track_id && !priv->next_request[deck] && !st.has_next) {
        dv->next_track_id = 0;
        changed = true;
    }

    bool was = priv->was_playing[deck];
    priv->was_playing[deck] = st.loaded && st.playing;
    bool loaded = st.loaded && dv->track && !dv->loading_request;
    bool at_end = loaded && st.duration > 0 && st.position >= st.duration - END_SLACK;

    CBTrack *want = loaded ? next_candidate(app, dv->track) : NULL;
    /* One that failed is not asked for again until the candidate changes. */
    if (want && want->id == priv->next_failed_id[deck])
        want = NULL;
    else
        priv->next_failed_id[deck] = 0;
    uint64_t want_id = want ? want->id : 0;

    /* The end reached before the next was in (a seek into the last
     * seconds, its decode in flight): the deck plays on when that next
     * comes, and only that one. A deck that stopped with nothing to follow
     * stays stopped whatever the view does after, and one whose candidate
     * changes meanwhile (another view) stays stopped too. */
    if (was && !st.playing && at_end && !st.scrubbing)
        priv->ended_for[deck] = want_id;
    else if (!loaded || st.playing || !at_end || want_id != priv->ended_for[deck])
        priv->ended_for[deck] = 0;

    if (dv->next_track_id && dv->next_track_id != want_id) {
        uint64_t had = dv->next_track_id;
        cancel_next(app, deck);
        changed |= dv->next_track_id != had;
    }
    bool near = loaded && st.duration > 0 && st.position >= st.duration - NEXT_LEAD_SECONDS;
    if (want && !dv->next_track_id && (near || priv->ended_for[deck])) {
        queue_next_track(app, deck, want);
        changed = true;
    }
    if (priv->ended_for[deck] && st.has_next && dv->next_track_id == priv->ended_for[deck]) {
        /* PLAY on a deck stopped at its end takes the next. */
        cb_deck_play(app->player, deck);
        priv->ended_for[deck] = 0;
        changed = true;
    }
    return changed;
}

/* ----------------------------------------------------------------- analysis */

unsigned cb_app_analysis_stale(const CBApp *app, const CBTrack *t)
{
    /* The version first: it costs nothing, the format is a read. */
    unsigned stale = cb_analysis_stale(t);
    if (!(stale & CB_ANALYSE_WAVES) && waves_stale(app, t))
        stale |= CB_ANALYSE_WAVES;
    return stale;
}

unsigned cb_app_analysis_needed(const CBApp *app, const CBTrack *t, unsigned what, bool force)
{
    if (!t || t->missing)
        return 0;

    /* A stale result is as good as none: the upgrade that bumped its
     * version is there to replace it. A locked grid is kept all the
     * same. */
    unsigned stale = cb_app_analysis_stale(app, t);
    unsigned need = what & CB_ANALYSE_ALL;
    if (!force) {
        /* A stage the current analyser ran is done, whether it found a
         * grid or a key or not (apply_analysis sets the version either
         * way): run again, it would find nothing again, on every add,
         * analyse and export. */
        if ((cb_track_analysed(t) || t->beats_version >= CB_BEATS_VERSION) &&
            !(stale & CB_ANALYSE_BEATS))
            need &= ~CB_ANALYSE_BEATS;
        if ((t->key[0] || t->key_version >= CB_KEY_VERSION) && !(stale & CB_ANALYSE_KEY))
            need &= ~CB_ANALYSE_KEY;
    }
    if (t->grid_locked && !force)
        need &= ~CB_ANALYSE_BEATS;

    /* The deck, the table preview and the stick all need the waveforms,
     * so missing or stale ones are made whatever was asked. */
    if (!t->has_waveforms || (stale & CB_ANALYSE_WAVES))
        need |= CB_ANALYSE_WAVES;
    else if (!force)
        need &= ~CB_ANALYSE_WAVES;

    return need;
}

/* Queues what a track lacks. Returns 1 when queued, 0 when there was
 * nothing to do (or the same work is already queued), or an error. */
static int queue_analysis(CBApp *app, CBTrack *t, unsigned what, bool force, bool override_lock)
{
    if (!app->jobs || !t)
        return CB_ERROR(EINVAL);
    if (t->missing)
        return CB_ERROR(ENOENT);

    unsigned need = cb_app_analysis_needed(app, t, what, force);
    if (t->grid_locked && !override_lock)
        need &= ~CB_ANALYSE_BEATS;
    if (!need)
        return 0;

    CBAppPriv *priv = app->priv;
    if (priv) {
        for (size_t i = 0; i < priv->inflight.nb; i++) {
            const Inflight *f = cb_array_at(&priv->inflight, i);
            if (f->track_id == t->id && !(need & ~f->what))
                return 0;
        }
    }

    CBAnalysisOptions opt;
    analysis_options(&app->lib.cfg, &opt);
    /* With other work queued the pool is the parallelism; a single
     * analysis on its own may use threads inside. */
    opt.deterministic_threads = cb_jobs_pending(app->jobs) > 0;

    uint64_t req = cb_jobs_analyse(app->jobs, t->id, t->path, &opt, need);
    if ((int64_t)req < 0)
        return (int)(int64_t)req;

    if (priv) {
        Inflight f = {
            .request       = req,
            .track_id      = t->id,
            .what          = need,
            .override_lock = override_lock && t->grid_locked,
        };
        if (!cb_array_push(&priv->inflight, &f))
            cb_warn("out of memory tracking analysis requests");
    }
    app->analysing_total++;
    app->dirty = true;
    return 1;
}

int cb_app_analyse_track(CBApp *app, CBTrack *t, unsigned what, bool force)
{
    if (!t)
        return CB_ERROR(EINVAL);
    if (t->missing) {
        char name[256];
        cb_app_set_status(app, "Cannot analyse %s: the file is missing",
                          track_name(t, name, sizeof(name)));
        return CB_ERROR(ENOENT);
    }
    /* One track, asked for explicitly: forcing it replaces even a locked
     * grid (the deck's RE-ANALYSE). */
    return queue_analysis(app, t, what, force, force);
}

int cb_app_analyse_tracks(CBApp *app, const uint64_t *ids, int n, unsigned what, bool force)
{
    /* A locked grid is replaced only when its track is named: one track
     * selected. A selection of many (Ctrl+A among them) keeps every locked
     * grid, as Analyse all does. */
    int queued = 0;
    for (int i = 0; i < n; i++) {
        CBTrack *t = cb_library_find_id(&app->lib, ids[i]);
        if (t && !t->missing && queue_analysis(app, t, what, force, force && n == 1) > 0)
            queued++;
    }
    return queued;
}

int cb_app_analyse_all(CBApp *app, bool force)
{
    /* Library-wide, locked grids are kept even when forced: a bulk
     * re-analysis must not undo grids fixed by hand. */
    int queued = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        CBTrack *t = app->lib.tracks[i];
        if (!t->missing && queue_analysis(app, t, CB_ANALYSE_ALL, force, false) > 0)
            queued++;
    }
    if (queued)
        cb_app_set_status(app, "Analysing %d track%s", queued, queued == 1 ? "" : "s");
    else
        cb_app_set_status(app, "Every track is analysed");
    return queued;
}

/* What an older analyser made, after an upgrade that bumped a stage's
 * version or the waveform format (cb_app_analysis_stale): those stages of
 * every track are queued, and only those (and waveforms the track lacks,
 * which always count). Locked grids stay, as in any analysis that was not
 * asked for by name. Returns how many tracks were queued. */
static int queue_stale(CBApp *app)
{
    int queued = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        CBTrack *t = app->lib.tracks[i];
        if (t->missing)
            continue;
        unsigned stale = cb_app_analysis_stale(app, t);
        if (stale && queue_analysis(app, t, stale, false, false) > 0)
            queued++;
    }
    return queued;
}

/* override_lock: a forced analysis of one track replaces its locked grid
 * (the lock stays). */
static int apply_analysis(CBApp *app, CBTrack *t, CBAnalysis *a, bool override_lock)
{
    if (!t || !a)
        return CB_ERROR(EINVAL);

    CBLibrary *lib = &app->lib;
    int err = 0;

    /* The cue an earlier analysis placed goes, on the grid it was placed
     * on, so that this one places it again: it waits for the drums now,
     * and a cue left on the old place would keep every track's there. */
    bool renew = false;
    if ((a->done & CB_ANALYSE_BEATS) && lib->cfg.auto_cues) {
        int ret = cb_analysis_take_auto_cue(t, &a->beats);
        if (ret < 0)
            cb_warn("cannot move the automatic cue: %s", strerror(-ret));
        renew = ret > 0;
    }

    bool grid_changed = false;
    if ((a->done & CB_ANALYSE_BEATS) && (!t->grid_locked || override_lock) &&
        cb_grid_valid(&a->beats.grid)) {
        CBGrid g = { 0 };
        int ret = cb_grid_copy(&g, &a->beats.grid);
        if (ret < 0)
            return ret;
        cb_grid_free(&t->grid);
        t->grid = g;
        t->grid_confidence = a->beats.confidence;
        t->grid_flags = a->beats.flags;
        grid_changed = true;
    }
    /* The versions say which analyser made each result; they are set here
     * and nowhere else, so a grid edited by hand keeps the version of the
     * one it came from. A stage that ran and found nothing (no grid in a
     * track too short, an undecidable key) keeps the result it had, at the
     * current version all the same: the analysis is deterministic, so
     * running it again would find nothing again, on every start. A locked
     * grid the analysis did not replace keeps its version. */
    if ((a->done & CB_ANALYSE_BEATS) && (!t->grid_locked || override_lock))
        t->beats_version = CB_BEATS_VERSION;

    if ((a->done & CB_ANALYSE_KEY) && a->key.key[0]) {
        snprintf(t->key, sizeof(t->key), "%s", a->key.key);
        t->key_confidence = a->key.confidence;
    }
    if (a->done & CB_ANALYSE_KEY)
        t->key_version = CB_KEY_VERSION;

    /* The decode is the players' timeline: trust it over the probe. */
    if (a->duration > 0) {
        t->duration = a->duration;
        if (t->sample_rate > 0)
            t->total_samples = llround(a->duration * t->sample_rate);
    }

    /* Cues set in bars follow a new grid. */
    if (grid_changed)
        cb_track_resolve_cues(t);

    /* dj's _auto_cues: a memory cue on the first downbeat once the drums
     * are in, for a track with no cues, and again where an earlier
     * analysis had placed one. On a locked grid it is placed on that grid. */
    if ((a->done & CB_ANALYSE_BEATS) && lib->cfg.auto_cues && (!t->nb_cues || renew) &&
        cb_grid_valid(&t->grid)) {
        CBBeatResult b = a->beats;
        b.grid = t->grid;
        int ret = cb_analysis_add_auto_cue(t, &b);
        if (ret < 0)
            cb_warn("cannot add the automatic cue: %s", strerror(-ret));
    }

    if (a->done & CB_ANALYSE_WAVES) {
        int ret = cb_library_waveforms_save(lib, t->id, &a->waves);
        if (ret < 0) {
            cb_warn("cannot store the waveforms of track %llu: %s", (unsigned long long)t->id,
                    strerror(-ret));
            err = ret;
        } else {
            t->has_waveforms = true;
            t->waves_version = CB_WAVES_VERSION;
            waves_seen(app, t->id, false);
        }
        gpu_forget_waves(app, t->id);
    }

    if (a->done)
        t->analysed = cb_time_now();

    int ret = cb_library_save_track(lib, t);
    if (ret < 0 && !err)
        err = ret;

    for (int d = 0; d < CB_DECKS; d++) {
        CBDeckView *dv = &app->decks[d];
        if (dv->track != t)
            continue;
        if (grid_changed && app->player)
            cb_ctl_grid_changed(&dv->ctl);
        if (a->done & CB_ANALYSE_WAVES)
            deck_load_waves(app, d);
    }

    app->rows_dirty = true;
    app->dirty = true;
    return err;
}

int cb_app_apply_analysis(CBApp *app, CBTrack *t, CBAnalysis *a)
{
    return apply_analysis(app, t, a, false);
}

int cb_app_rescan(CBApp *app)
{
    int ret = cb_library_rescan_missing(&app->lib);
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot rescan the library: %s", strerror(-ret));
        return ret;
    }
    int missing = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++)
        missing += app->lib.tracks[i]->missing;
    cb_app_set_status(app, "%d track%s changed state; %d missing", ret, ret == 1 ? "" : "s",
                      missing);
    app->rows_dirty = true;
    return ret;
}

/* ------------------------------------------------------------------ tagging */

/* `dir`/models/`name` when it is there. */
static char *model_in(const char *dir, const char *name)
{
    char *models = cb_path_join(dir, "models");
    char *p = models ? cb_path_join(models, name) : NULL;
    free(models);
    if (p && cb_file_exists(p))
        return p;
    free(p);
    return NULL;
}

/* A model's file: `named` when set, else `name` in models/ (below). */
static int find_model(const char *named, const char *name, char **path)
{
    *path = NULL;
    if (named && named[0]) {
        char *p = cb_path_expand(named);
        if (!p)
            return CB_ERROR(ENOMEM);
        *path = p;
        return cb_file_exists(p) ? 0 : CB_ERROR(ENOENT);
    }

    /* Beside the program, so an installed cyanbox and one run from its
     * build directory (whose parent is the source tree) both find the
     * tree's models/; then the current directory; then where `meson
     * install` put them. */
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        for (int up = 0; up < 2 && !*path; up++) {
            char *slash = strrchr(exe, '/');
            if (!slash)
                break;
            *slash = '\0';
            *path = model_in(exe[0] ? exe : "/", name);
        }
    }
    if (!*path)
        *path = model_in(".", name);
    if (!*path)
        *path = model_in(CB_MODELS_DIR, name);
    return *path ? 0 : CB_ERROR(ENOENT);
}

int cb_app_ml_model(const CBApp *app, char **path)
{
    const char *named = app->lib.cfg.ml_model;
    if (!named || !named[0])
        named = getenv("CYANBOX_ML_MODEL");
    return find_model(named, CB_APP_ML_MODEL, path);
}

int cb_app_beat_model(char **path)
{
    return find_model(getenv("CYANBOX_BEATTHIS_MODEL"), CB_APP_BEAT_MODEL, path);
}

int cb_app_set_beat_model(CBApp *app)
{
    if (!app->jobs)
        return CB_ERROR(EINVAL);
    char *model = NULL;
    int ret = cb_app_beat_model(&model);
    cb_jobs_set_beat_model(app->jobs, ret < 0 ? NULL : model);
    if (ret < 0) {
        app->last_error_count++;
        if (model)
            cb_app_set_status(app, "Cannot analyse beats: there is no Beat This! model at %s",
                              model);
        else
            cb_app_set_status(app, "Cannot analyse beats: no Beat This! model in models/ (%s), "
                              "and CYANBOX_BEATTHIS_MODEL names none", CB_APP_BEAT_MODEL);
    }
    free(model);
    return ret;
}

int cb_app_tag_tracks(CBApp *app, const uint64_t *ids, int n)
{
    if (!app->jobs)
        return CB_ERROR(EINVAL);

    char *model = NULL;
    int ret = cb_app_ml_model(app, &model);
    if (ret < 0) {
        app->last_error_count++;
        if (model)
            cb_app_set_status(app, "Cannot tag: there is no model at %s", model);
        else
            cb_app_set_status(app, "Cannot tag: no model in models/ (%s), and neither the "
                              "setting ml.model nor CYANBOX_ML_MODEL names one", CB_APP_ML_MODEL);
        free(model);
        return ret;
    }

    /* The legend as it is now goes with each track (the batch copies it). */
    const char *legend[CB_TRACK_COLORS];
    cb_config_legend(&app->lib.cfg, legend);
    int queued = 0, missing = 0;
    const CBTrack *first = NULL;
    for (int i = 0; i < n; i++) {
        CBTrack *t = cb_library_find_id(&app->lib, ids[i]);
        if (!t)
            continue;
        if (t->missing) {
            missing++;
            continue;
        }
        uint64_t req = cb_jobs_tag(app->jobs, model, legend, t->id, t->path);
        if ((int64_t)req < 0) {
            ret = (int)(int64_t)req;
            break;
        }
        app->tag_request = req;
        if (!queued++)
            first = t;
    }
    free(model);

    char name[256];
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot tag: %s", strerror(-ret));
        return ret;
    }
    if (!queued && missing)
        cb_app_set_status(app, "Nothing to tag: the file%s missing", missing == 1 ? " is" : "s are");
    else if (!queued)
        cb_app_set_status(app, "Nothing to tag");
    else if (queued == 1)
        cb_app_set_status(app, "Tagging %s", track_name(first, name, sizeof(name)));
    else
        cb_app_set_status(app, "Tagging %d tracks", queued);
    return queued;
}

void cb_app_cancel_tags(CBApp *app)
{
    if (!app->jobs || !cb_jobs_tag_progress(app->jobs, NULL, NULL, NULL))
        return;
    cb_jobs_cancel(app->jobs, app->tag_request);
    cb_app_set_status(app, "Tagging stopped");
}

bool cb_app_tag_progress(CBApp *app, int *done, int *total, uint64_t *current)
{
    return cb_jobs_tag_progress(app->jobs, done, total, current);
}

int cb_app_apply_tags(CBApp *app, CBTrack *t, const CBMLTags *tags)
{
    if (!t || !tags)
        return CB_ERROR(EINVAL);

    /* The genre the user sees is the file's unless they changed it; an
     * override that is there and not empty is theirs, and stays. */
    const char *g = t->meta.genre, *fg = t->file_meta.genre;
    bool own = g && g[0] && strcmp(g, fg ? fg : "");
    int ret = 0;
    if (tags->genre && tags->genre[0] && !own)
        ret = cb_meta_set(&t->meta.genre, tags->genre);
    if (ret >= 0)
        ret = cb_meta_set(&t->meta.comment, tags->comment ? tags->comment : "");
    if (ret < 0)
        return ret;
    /* The colour the model picked is stored as that colour's hue, as one
     * picked by hand is: the stick gets the same colour. */
    if (tags->color >= 1 && tags->color <= CB_TRACK_COLORS)
        t->meta.hue = cb_track_color_hue(tags->color);
    static_assert(CB_ARRAY_ELEMS(t->ml_colors) == CB_ARRAY_ELEMS(tags->colors),
                  "a probability per track colour");
    memcpy(t->ml_colors, tags->colors, sizeof(t->ml_colors));
    t->ml_legend = tags->legend;
    t->ml_club = tags->club;
    t->ml_intensity = tags->intensity;
    t->ml_valence = tags->valence;
    t->ml_vocals = tags->vocals;
    t->tags_version = CB_ML_TAGS_VERSION;

    ret = cb_library_save_track(&app->lib, t);
    app->rows_dirty = true;
    app->dirty = true;
    return ret;
}

/* -------------------------------------------------------------- job results */

static bool take_inflight(CBAppPriv *priv, uint64_t request, Inflight *out)
{
    for (size_t i = 0; i < priv->inflight.nb; i++) {
        Inflight *f = cb_array_at(&priv->inflight, i);
        if (f->request == request) {
            *out = *f;
            cb_array_remove(&priv->inflight, i);
            return true;
        }
    }
    return false;
}

/* Why an analysis failed, for the status bar. The decoder's errors come
 * back as errno values (cb_audio_error: damaged data is EINVAL, a file
 * without audio ENODATA), whose own wording means little there; the log
 * has strerror's. */
static const char *analysis_error(int err)
{
    switch (err) {
    case CB_ERROR(ENOENT):  return "the file is missing";
    case CB_ERROR(EACCES):
    case CB_ERROR(EPERM):   return "the file cannot be read (permission denied)";
    case CB_ERROR(EINVAL):
    case CB_ERROR(ENODATA): return "the file does not decode (damaged, or no audio)";
    case CB_ERROR(ENOTSUP): return "the file's format is not supported";
    case CB_ERROR(ENOMEM):  return "out of memory";
    case CB_ERROR(ENODEV):  return "Beat This!, which the grid is made with, cannot run "
                                   "(no model, or no GPU for it: see the log)";
    default:                return strerror(-err);
    }
}

/* A failure is said with the track's name and kept in the status bar for a
 * while: in a batch, the next results (up to RESULTS_PER_TICK in the same
 * tick) would otherwise replace it before it is drawn. Any other message
 * set meanwhile ends that. The row's spinner follows the worker
 * (cb_jobs_active_tracks), which is idle again by now, failed or not. */
static void handle_analysis(CBApp *app, CBJobResult *r)
{
    CBAppPriv *priv = app->priv;
    Inflight f = { 0 };
    take_inflight(priv, r->request, &f);
    app->analysing_done++;

    CBTrack *t = cb_library_find_id(&app->lib, r->track_id);
    char name[256];
    bool held = priv->status_held && priv->status_held == app->status_time &&
                cb_time_us() - app->status_time < STATUS_HOLD_US;
    if (!t) {
        /* Removed from the library while it was analysed. */
    } else if (r->err < 0 || !r->analysis) {
        int err = r->err < 0 ? r->err : CB_ERROR(EINVAL);
        app->last_error_count++;
        priv->analyses_failed++;
        cb_warn("cannot analyse %s: %s", t->path, strerror(-err));
        track_name(t, name, sizeof(name));
        if (priv->analyses_failed > 1)
            cb_app_set_status(app, "Analysis failed for %s: %s (%d failed so far)", name,
                              analysis_error(err), priv->analyses_failed);
        else
            cb_app_set_status(app, "Analysis failed for %s: %s", name, analysis_error(err));
        priv->status_held = app->status_time;
    } else {
        int ret = apply_analysis(app, t, r->analysis, f.override_lock);

        track_name(t, name, sizeof(name));
        if (ret < 0) {
            app->last_error_count++;
            cb_app_set_status(app, "Analysed %s, but saving failed: %s", name, strerror(-ret));
            priv->status_held = app->status_time;
        } else if (held) {
            /* The failure before it stays. */
        } else if (cb_grid_valid(&t->grid)) {
            cb_app_set_status(app, "Analysed %s: %.2f BPM%s%s", name, cb_grid_bpm(&t->grid),
                              t->key[0] ? ", " : "", t->key);
        } else {
            cb_app_set_status(app, "Analysed %s", name);
        }
    }

    if (!priv->inflight.nb) {
        app->analysing_total = 0;
        app->analysing_done = 0;
        priv->analyses_failed = 0;
    }
}

/* Why tagging failed, for the status bar: the model's own errors, else
 * the decoder's (analysis_error). */
static const char *tag_error(int err)
{
    switch (err) {
    case CB_ERROR(ENODEV): return "no GPU here can run the model (Vulkan 1.3 with 16- and 8-bit "
                                  "storage)";
    case CB_ERROR(EIO):    return "the GPU failed (see the log)";
    case CB_ERROR(E2BIG):  return "the track is too long for the model";
    default:               return analysis_error(err);
    }
}

/* A failure is held in the status bar as an analysis's is. */
static void handle_tags(CBApp *app, CBJobResult *r)
{
    CBAppPriv *priv = app->priv;
    char name[256];
    if (!r->track_id) {
        /* The model did not load; the batch is over. */
        app->last_error_count++;
        cb_app_set_status(app, "Cannot load the model to tag: %s",
                          r->err == CB_ERROR(EINVAL) ? "the file is not MOSS-Audio" : tag_error(r->err));
        priv->status_held = app->status_time;
        return;
    }

    CBTrack *t = cb_library_find_id(&app->lib, r->track_id);
    if (!t)
        return;
    track_name(t, name, sizeof(name));
    bool held = priv->status_held && priv->status_held == app->status_time &&
                cb_time_us() - app->status_time < STATUS_HOLD_US;
    if (r->err < 0 || !r->tags) {
        int err = r->err < 0 ? r->err : CB_ERROR(EINVAL);
        app->last_error_count++;
        cb_warn("cannot tag %s: %s", t->path, strerror(-err));
        cb_app_set_status(app, "Tagging failed for %s: %s", name, tag_error(err));
        priv->status_held = app->status_time;
        return;
    }

    const CBMLTags *tags = r->tags;
    int ret = cb_app_apply_tags(app, t, tags);
    /* "red (intense, peak time)", in the words the model was asked with:
     * the legend's now, unless it was edited since. */
    const char *legend[CB_TRACK_COLORS];
    cb_config_legend(&app->lib.cfg, legend);
    const char *cname = cb_track_color_name(tags->color);
    const char *vibe = tags->legend == cb_ml_legend_hash(legend) ?
                       cb_config_vibe(&app->lib.cfg, tags->color) : NULL;
    char colour[160];
    if (!cname)
        snprintf(colour, sizeof(colour), "no colour");
    else if (vibe)
        snprintf(colour, sizeof(colour), "%s (%s)", cname, vibe);
    else
        snprintf(colour, sizeof(colour), "%s", cname);
    cb_info("tagged %s in %.1f s: %s %.2f, %s, %s", t->path, r->seconds, colour,
            cname ? tags->colors[tags->color - 1] : 0.0f, tags->genre ? tags->genre : "no genre",
            tags->comment ? tags->comment : "");
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Tagged %s, but saving failed: %s", name, strerror(-ret));
        priv->status_held = app->status_time;
    } else if (held) {
        /* The failure before it stays. */
    } else if (tags->genre && t->meta.genre && strcmp(t->meta.genre, tags->genre)) {
        cb_app_set_status(app, "Tagged %s in %.1f s: %s; the genre stays %s (the model "
                          "hears %s)", name, r->seconds, colour, t->meta.genre, tags->genre);
    } else {
        cb_app_set_status(app, "Tagged %s in %.1f s: %s, %s", name, r->seconds,
                          tags->genre ? tags->genre : "no genre", colour);
    }
}

/* The decode of the track queued to follow the deck's: handed to the
 * player, which plays on into it at the end; a deck already stopped at
 * its end plays on now. */
static void handle_next(CBApp *app, CBJobResult *r, int deck)
{
    CBAppPriv *priv = app->priv;
    CBDeckView *dv = &app->decks[deck];
    priv->next_request[deck] = 0;

    CBTrack *t = cb_library_find_id(&app->lib, dv->next_track_id);
    if (!t || !app->player || !dv->track || dv->loading_request) {
        dv->next_track_id = 0;
        return;
    }
    char name[256];
    track_name(t, name, sizeof(name));
    int ret = r->err < 0 ? r->err : (r->stream ? 0 : CB_ERROR(EINVAL));
    if (ret == 0) {
        const CBGrid *grid = cb_grid_valid(&t->grid) ? &t->grid : NULL;
        CBStream *s = r->stream;
        r->stream = NULL;
        ret = cb_deck_queue_next_stream(app->player, deck, s, grid);
        if (ret < 0)
            cb_stream_free(s);
    }
    if (ret < 0) {
        /* Not asked for again and again: the deck will stop at its end. */
        priv->next_failed_id[deck] = dv->next_track_id;
        dv->next_track_id = 0;
        app->last_error_count++;
        cb_app_set_status(app, "Cannot queue %s to follow: %s", name, strerror(-ret));
        return;
    }
    if (priv->ended_for[deck] && priv->ended_for[deck] == dv->next_track_id) {
        cb_deck_play(app->player, deck);
        priv->ended_for[deck] = 0;
    }
}

static void handle_decode(CBApp *app, CBJobResult *r)
{
    int deck = r->deck;
    if (deck < 0 || deck >= CB_DECKS)
        return;
    CBDeckView *dv = &app->decks[deck];

    if (r->request && r->request == app->priv->next_request[deck]) {
        handle_next(app, r, deck);
        return;
    }
    /* A result for a request the deck has moved on from is dropped. */
    if (!r->request || r->request != dv->loading_request)
        return;
    uint64_t id = dv->loading_track_id;
    dv->loading_request = 0;
    dv->loading_track_id = 0;

    CBTrack *t = cb_library_find_id(&app->lib, id);
    if (!t) {
        cb_app_set_status(app, "Not loaded: the track left the library meanwhile");
        return;
    }
    if (!app->player)
        return;

    char name[256];
    track_name(t, name, sizeof(name));
    if (r->err < 0 || !r->stream) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot load %s: %s", name,
                          strerror(r->err < 0 ? -r->err : EINVAL));
        return;
    }

    /* The player owns the stream from here; on failure it stays ours. Loading replaces what the deck plays, so the old track goes
     * only once the new one is in: a player that cannot take it (its
     * command queue full while the output stalls) keeps the track it has. */
    const CBGrid *grid = cb_grid_valid(&t->grid) ? &t->grid : NULL;
    CBStream *s = r->stream;
    r->stream = NULL;
    int ret = cb_deck_load_stream(app->player, deck, s, grid);
    if (ret < 0)
        cb_stream_free(s);
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot load %s: %s", name, strerror(-ret));
        return;
    }
    cb_ctl_unload(&dv->ctl);
    deck_view_reset(app, deck);

    dv->track = t;
    app->priv->deck_track_id[deck] = t->id;
    dv->ctl.auto_cue = auto_cue_of(&app->lib.cfg);
    cb_ctl_load(&dv->ctl, t);
    deck_load_waves(app, deck);

    /* rekordbox analyses a track on load when it has not been: here only
     * what is missing, the waveforms among them. */
    queue_analysis(app, t, CB_ANALYSE_ALL, false, false);
    deck_fetch_artwork(app, deck, t);

    cb_app_set_status(app, "Loaded %s", name);
}

static void handle_artwork(CBApp *app, CBJobResult *r)
{
    CBAppPriv *priv = app->priv;
    CBTrack *t = cb_library_find_id(&app->lib, r->track_id);

    /* The thumbnail goes into the library whoever asked. */
    if (t && r->err >= 0 && r->jpeg) {
        int ret = cb_library_artwork_save(&app->lib, t->id, r->jpeg, r->jpeg_size, ART_SIZE,
                                          ART_SIZE);
        if (ret < 0)
            cb_warn("cannot store the artwork of track %llu: %s", (unsigned long long)t->id,
                    strerror(-ret));
    }
    bool ok = r->err >= 0 && r->rgba;

    for (int d = 0; d < CB_DECKS; d++) {
        CBDeckView *dv = &app->decks[d];
        if (!dv->artwork_request || dv->artwork_request != r->request)
            continue;
        dv->artwork_request = 0;
        if (ok && dv->track && dv->track->id == r->track_id)
            deck_set_artwork(app, dv, r->rgba, r->width, r->height);
    }

    /* The browser's entry takes the cover's texture, shared with the
     * tracks that have it, when the library stored it. */
    int64_t art = t && ok ? cb_library_artwork_id(&app->lib, t->id) : 0;
    for (size_t i = 0; i < priv->gpu.nb; i++) {
        GpuEntry *e = cb_array_at(&priv->gpu, i);
        if (!e->art_request || e->art_request != r->request)
            continue;
        e->art_request = 0;
        e->art_state = GPU_DONE;
        if (!ok || !app->window)
            continue;
        if (art > 0) {
            e->texture = cover_ref(app, art, t->id, r->rgba, r->width, r->height);
            e->artwork_id = e->texture ? art : 0;
        } else {
            e->texture = cb_texture_create(app->window, r->rgba, r->width, r->height);
        }
    }
}

/* A stick read or inspection is kept whole until the interface takes it.
 * drain_jobs frees the result it hands in, so the payload moves to a copy. */
static void handle_stick(CBApp *app, CBJobResult *r)
{
    if (r->request != app->stick_request)
        return;
    app->stick_request = 0;
    CBJobResult *keep = malloc(sizeof(*keep));
    if (!keep) {
        cb_app_set_status(app, "Out of memory reading the stick");
        return;
    }
    *keep = *r;
    r->stick = NULL;
    r->sync_items = NULL;
    r->nb_sync_items = 0;
    r->report = NULL;
    cb_job_result_free(app->stick_result);
    app->stick_result = keep;
}

static void handle_result(CBApp *app, CBJobResult *r)
{
    switch (r->kind) {
    case CB_JOB_ANALYSE:
        handle_analysis(app, r);
        break;
    case CB_JOB_DECODE:
        handle_decode(app, r);
        break;
    case CB_JOB_ARTWORK:
        handle_artwork(app, r);
        break;
    case CB_JOB_STICK_READ:
    case CB_JOB_INSPECT:
        handle_stick(app, r);
        break;
    case CB_JOB_TAG:
        handle_tags(app, r);
        break;
    }
}

/* On the io queue: a read is mostly waiting on the stick, and must not
 * queue behind a library-wide analysis. */
static int stick_job(CBApp *app, const char *mount, bool inspect)
{
    CBAppPriv *priv = app->priv;
    if (!priv || !priv->io_jobs || !mount || !mount[0])
        return CB_ERROR(EINVAL);
    if (app->stick_request)
        return CB_ERROR(EBUSY);

    /* The worker compares with the library as the database holds it. */
    if (!inspect)
        flush_saves(app);
    uint64_t req = inspect ? cb_jobs_inspect(priv->io_jobs, mount)
                           : cb_jobs_stick_read(priv->io_jobs, app->lib.root, mount);
    if ((int64_t)req < 0)
        return (int)(int64_t)req;

    cb_job_result_free(app->stick_result);
    app->stick_result = NULL;
    app->stick_request = req;
    return 0;
}

int cb_app_read_stick(CBApp *app, const char *mount)
{
    return stick_job(app, mount, false);
}

int cb_app_inspect_stick(CBApp *app, const char *mount)
{
    return stick_job(app, mount, true);
}

/* A running read cannot be stopped; with stick_request moved on,
 * handle_stick drops its result when it comes. */
void cb_app_cancel_stick(CBApp *app, uint64_t request)
{
    if (!request)
        return;
    if (app->stick_request == request) {
        if (app->priv && app->priv->io_jobs)
            cb_jobs_cancel(app->priv->io_jobs, request);
        app->stick_request = 0;
    }
    if (app->stick_result && app->stick_result->request == request) {
        cb_job_result_free(app->stick_result);
        app->stick_result = NULL;
    }
}

/* ----------------------------------------------------------------- add path */

typedef struct AddCtx {
    CBArray ids;
    int     failed;
} AddCtx;

static void add_cb(void *opaque, const char *path, CBTrack *added, int err)
{
    AddCtx *ctx = opaque;
    if (err < 0) {
        ctx->failed++;
        cb_warn("cannot add %s: %s", path, strerror(-err));
        return;
    }
    if (added && !cb_array_push(&ctx->ids, &added->id))
        ctx->failed++;
}

int cb_app_add_path(CBApp *app, const char *path)
{
    char *abs = cb_path_abs(path);
    if (!abs) {
        cb_app_set_status(app, "Cannot add %s: no such file or directory", path);
        return CB_ERROR(ENOENT);
    }

    AddCtx ctx = { 0 };
    cb_array_init(&ctx.ids, sizeof(uint64_t));
    int before = app->lib.nb_tracks;
    int ret = cb_library_add_path(&app->lib, abs, add_cb, &ctx);
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot add %s: %s", abs, strerror(-ret));
        goto end;
    }

    /* Everything reported is queued: already known tracks too, when they
     * still lack something. */
    int queued = 0;
    for (size_t i = 0; i < ctx.ids.nb; i++) {
        CBTrack *t = cb_library_find_id(&app->lib, *(uint64_t *)cb_array_at(&ctx.ids, i));
        if (t && queue_analysis(app, t, CB_ANALYSE_ALL, false, false) > 0)
            queued++;
    }

    int added = app->lib.nb_tracks - before;
    if (ctx.failed)
        cb_app_set_status(app, "Added %d track%s from %s (%d failed), analysing %d", added,
                          added == 1 ? "" : "s", cb_path_base(abs), ctx.failed, queued);
    else
        cb_app_set_status(app, "Added %d track%s from %s, analysing %d", added,
                          added == 1 ? "" : "s", cb_path_base(abs), queued);
    app->rows_dirty = true;

end:
    cb_array_free(&ctx.ids);
    free(abs);
    return ret;
}

/* ------------------------------------------------------------------- export */

/* The export runs in phases: what the selected tracks lack is analysed
 * through the jobs; a dry run on the export thread reports what a real run
 * would copy, skip and prune and whether the stick has player changes not
 * synced back (a dry run that only those stop lists everything all the
 * same, cyanbox/export.h); when there is something to ask, the interface
 * confirms (prune or not, export over unsynced changes or not), both
 * questions at once; then the real run. */
enum ExportPhase {
    EXPORT_IDLE = 0,
    EXPORT_ANALYSING,
    EXPORT_DRY_RUN,
    EXPORT_CONFIRM,
    EXPORT_RUNNING,
};

static void export_job_free(ExportJob *ej)
{
    if (!ej)
        return;
    free(ej->root);
    free(ej->dest);
    free(ej->playlists);
    free(ej);
}

static void export_msg_free(ExportMsg *m)
{
    if (!m)
        return;
    free(m->item);
    free(m);
}

static void export_drain(CBAppPriv *priv)
{
    if (!priv->export_mb)
        return;
    ExportMsg *m;
    while ((m = cb_mailbox_take(priv->export_mb)))
        export_msg_free(m);
}

static void export_prune_list_free(CBApp *app)
{
    typeof(app->export_state) *es = &app->export_state;
    for (int i = 0; i < es->nb_prune; i++)
        free(es->prune_list[i]);
    free(es->prune_list);
    es->prune_list = NULL;
    es->nb_prune = 0;
}

/* Appends a line to the dialog's log, dropping the oldest lines when full. */
static void export_log(CBApp *app, const char *fmt, ...) CB_PRINTF(2, 3);
static void export_log(CBApp *app, const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    char *log = app->export_state.log;
    size_t cap = sizeof(app->export_state.log);
    size_t add = strlen(line) + 1;
    size_t len = strlen(log);
    while (len && len + add + 1 > cap) {
        char *nl = strchr(log, '\n');
        size_t drop = nl ? (size_t)(nl + 1 - log) : len;
        memmove(log, log + drop, len - drop + 1);
        len -= drop;
    }
    snprintf(log + len, cap - len, "%s\n", line);
}

static int export_cmp_id(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void export_wait_free(CBAppPriv *priv)
{
    free(priv->export_wait);
    priv->export_wait = NULL;
    priv->nb_export_wait = priv->export_wait_total = 0;
}

/* What the analysing phase waits for: those of the export's tracks (`ids`)
 * with an analysis in flight, queued by the export or before it, and
 * nothing else (after an analyser upgrade the startup queue holds the
 * whole library). */
static int export_wait_for(CBApp *app, const CBArray *ids)
{
    CBAppPriv *priv = app->priv;
    export_wait_free(priv);
    uint64_t *in = malloc(CB_MAX(priv->inflight.nb, 1) * sizeof(*in));
    uint64_t *w = malloc(CB_MAX(ids->nb, 1) * sizeof(*w));
    if (!in || !w) {
        free(in);
        free(w);
        return CB_ERROR(ENOMEM);
    }
    for (size_t i = 0; i < priv->inflight.nb; i++)
        in[i] = ((const Inflight *)cb_array_at(&priv->inflight, i))->track_id;
    if (priv->inflight.nb)
        qsort(in, priv->inflight.nb, sizeof(*in), export_cmp_id);
    int n = 0;
    for (size_t i = 0; i < ids->nb; i++) {
        const uint64_t *id = cb_array_at(ids, i);
        if (priv->inflight.nb && bsearch(id, in, priv->inflight.nb, sizeof(*in), export_cmp_id))
            w[n++] = *id;
    }
    free(in);
    if (n)
        qsort(w, (size_t)n, sizeof(*w), export_cmp_id);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (!k || w[k - 1] != w[i])
            w[k++] = w[i];
    priv->export_wait = w;
    priv->nb_export_wait = priv->export_wait_total = k;
    priv->export_idle = false;
    return 0;
}

/* How many of them are still in flight; the others leave the list. */
static int export_waiting(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    if (!priv->nb_export_wait)
        return 0;
    uint8_t *still = calloc((size_t)priv->nb_export_wait, 1);
    if (!still)
        return priv->nb_export_wait;
    for (size_t i = 0; i < priv->inflight.nb; i++) {
        const Inflight *f = cb_array_at(&priv->inflight, i);
        const uint64_t *at = bsearch(&f->track_id, priv->export_wait, (size_t)priv->nb_export_wait,
                                     sizeof(*priv->export_wait), export_cmp_id);
        if (at)
            still[at - priv->export_wait] = 1;
    }
    int k = 0;
    for (int i = 0; i < priv->nb_export_wait; i++)
        if (still[i])
            priv->export_wait[k++] = priv->export_wait[i];
    priv->nb_export_wait = k;
    free(still);
    return k;
}

/* Whether the analyses still waited for will never come back. The jobs
 * post a result before they count it done (cyanbox/jobs.h), so with nothing
 * pending every result is in their mailbox, and a tick that then applies
 * none (cb_app_tick drains before this) has seen the last: one whose post
 * failed for memory is lost, and the export goes on without it rather
 * than wait for ever. */
static bool export_lost(CBApp *app, int waiting)
{
    CBAppPriv *priv = app->priv;
    if (cb_jobs_pending(app->jobs) > 0) {
        priv->export_idle = false;
        return false;
    }
    if (!priv->export_idle || priv->export_idle_done != app->analysing_done) {
        priv->export_idle = true;
        priv->export_idle_done = app->analysing_done;
        return false;
    }
    cb_warn("export: %d analys%s never came back", waiting, waiting == 1 ? "is" : "es");
    export_log(app, "%d analys%s never came back; exporting without waiting", waiting,
               waiting == 1 ? "is" : "es");
    return true;
}

/* Export thread: progress to the mailbox, strings copied. */
static bool export_progress(void *opaque, const char *stage, int done, int total, const char *item)
{
    ExportJob *ej = opaque;
    ExportMsg *m = calloc(1, sizeof(*m));
    if (m) {
        snprintf(m->stage, sizeof(m->stage), "%s", stage ? stage : "");
        m->item = cb_strdup(item);
        m->done = done;
        m->total = total;
        if (cb_mailbox_post(ej->mb, m) < 0)
            export_msg_free(m);
    }
    return !cb_cancelled(ej->cancel);
}

/* Export thread: the selected playlists in its own library handle. */
static int resolve_playlists(CBLibrary *lib, const ExportJob *ej, CBPlaylist ***out)
{
    int nb_all = cb_library_playlists_flat(lib, NULL, 0);
    CBPlaylist **all = calloc(CB_MAX(nb_all, 1), sizeof(*all));
    CBPlaylist **sel = calloc(ej->nb_playlists, sizeof(*sel));
    if (!all || !sel) {
        free(all);
        free(sel);
        return CB_ERROR(ENOMEM);
    }
    nb_all = cb_library_playlists_flat(lib, all, nb_all);

    int ret = 0;
    for (int i = 0; i < ej->nb_playlists; i++) {
        for (int k = 0; k < nb_all && !sel[i]; k++) {
            if (all[k]->id == ej->playlists[i])
                sel[i] = all[k];
        }
        if (!sel[i]) {
            cb_err("export: playlist %lld is gone", (long long)ej->playlists[i]);
            ret = CB_ERROR(ENOENT);
        }
    }
    free(all);
    if (ret < 0) {
        free(sel);
        return ret;
    }
    *out = sel;
    return 0;
}

static void *export_main(void *arg)
{
    ExportJob *ej = arg;
    CBPlaylist **playlists = NULL;

    /* The snapshot, opened here: reading the library and looking at every
     * file takes a while, and the interface goes on meanwhile. */
    int ret = 0;
    if (ej->root && (ret = cb_library_open(ej->lib, ej->root)) >= 0)
        ej->opened = true;
    if (ret >= 0 && ej->nb_playlists)
        ret = resolve_playlists(ej->lib, ej, &playlists);
    if (ret >= 0) {
        CBExportOptions opt;
        cb_export_options_default(&opt);
        opt.whole_library = ej->whole_library;
        opt.playlists = (const CBPlaylist *const *)playlists;
        opt.nb_playlists = ej->nb_playlists;
        opt.prune = ej->prune;
        opt.ignore_unsynced = ej->ignore_unsynced;
        opt.format = ej->format;
        opt.dry_run = ej->dry_run;
        opt.stats = &ej->stats;
        ret = cb_export(ej->lib, ej->dest, &opt, export_progress, ej);
    }
    free(playlists);

    ej->result = ret;
    __atomic_store_n(&ej->finished, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* Joins the export thread and drops its job; a snapshot the thread opened
 * is the app's from here on. */
static void export_join(CBAppPriv *priv)
{
    if (priv->export_started) {
        pthread_join(priv->export_thread, NULL);
        priv->export_started = false;
    }
    if (priv->export_job && priv->export_job->opened)
        priv->export_lib_open = true;
    export_drain(priv);
    export_job_free(priv->export_job);
    priv->export_job = NULL;
}

/* The export is over: its snapshot goes (after export_join). */
static void export_snapshot_close(CBAppPriv *priv)
{
    if (priv->export_lib_open)
        cb_library_close(&priv->export_lib);
    priv->export_lib_open = false;
}

static void export_finish(CBApp *app, int result)
{
    CBAppPriv *priv = app->priv;
    export_join(priv);
    export_snapshot_close(priv);
    export_wait_free(priv);
    priv->export_phase = EXPORT_IDLE;

    typeof(app->export_state) *es = &app->export_state;
    es->running = false;
    es->confirm = false;
    es->cancel = false;
    es->result = result;
    es->item[0] = '\0';
    if (result >= 0) {
        snprintf(es->stage, sizeof(es->stage), "done");
        export_log(app, "Finished");
        cb_app_set_status(app, "Export to %s finished", es->dest);
    } else if (result == CB_ERROR(ECANCELED)) {
        snprintf(es->stage, sizeof(es->stage), "cancelled");
        export_log(app, "Cancelled");
        cb_app_set_status(app, "Export to %s cancelled", es->dest);
    } else {
        snprintf(es->stage, sizeof(es->stage), "failed");
        export_log(app, "Failed: %s", strerror(-result));
        app->last_error_count++;
        cb_app_set_status(app, "Export to %s failed: %s", es->dest, strerror(-result));
    }
}

/* Starts a dry or a real run on the export thread, from the dialog state. */
static void export_run(CBApp *app, bool dry_run)
{
    CBAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;

    /* The snapshot both runs export: the dry run's thread opens it once
     * pending saves are written (export_tick), the real run exports the
     * same one. An edit committed while it is read is in it whole or not
     * at all (cb_library_open reads in one transaction). */
    bool open = !priv->export_lib_open;
    if (open)
        priv->export_generation = app->lib.generation;
    else if (!dry_run && priv->export_generation != app->lib.generation)
        export_log(app, "Changes made since the check are not in this export");

    ExportJob *ej = calloc(1, sizeof(*ej));
    if (!ej) {
        export_finish(app, CB_ERROR(ENOMEM));
        return;
    }
    ej->lib = &priv->export_lib;
    ej->root = open ? cb_strdup(app->lib.root) : NULL;
    ej->dest = cb_strdup(es->dest);
    if (priv->nb_export_playlists) {
        ej->playlists = calloc(priv->nb_export_playlists, sizeof(*ej->playlists));
        if (ej->playlists) {
            memcpy(ej->playlists, priv->export_playlists,
                   priv->nb_export_playlists * sizeof(*ej->playlists));
            ej->nb_playlists = priv->nb_export_playlists;
        }
    }
    if (!ej->dest || (open && !ej->root) || (priv->nb_export_playlists && !ej->playlists)) {
        export_job_free(ej);
        export_finish(app, CB_ERROR(ENOMEM));
        return;
    }
    ej->whole_library = es->whole_library;
    /* The dry run lists what pruning would delete, for the question. */
    ej->prune = dry_run || es->prune;
    ej->ignore_unsynced = es->ignore_unsynced;
    ej->format = es->format;
    ej->dry_run = dry_run;
    ej->mb = priv->export_mb;
    ej->cancel = &priv->export_cancel;

    if (dry_run) {
        export_prune_list_free(app);
        es->unsynced = 0;
    }
    es->done = 0;
    es->total = 0;
    es->copied = es->skipped = es->pruned = es->rewritten = 0;
    es->item[0] = '\0';
    snprintf(es->stage, sizeof(es->stage), "prepare");
    priv->export_last_stage[0] = '\0';
    __atomic_store_n(&priv->export_cancel.flag, 0, __ATOMIC_RELAXED);

    priv->export_job = ej;
    priv->export_phase = dry_run ? EXPORT_DRY_RUN : EXPORT_RUNNING;
    int ret = pthread_create(&priv->export_thread, NULL, export_main, ej);
    if (ret) {
        export_finish(app, CB_ERROR(ret));
        return;
    }
    priv->export_started = true;
    export_log(app, "%s to %s", dry_run ? "Checking the export" : "Exporting", es->dest);
    cb_app_set_status(app, "%s to %s", dry_run ? "Checking the export" : "Exporting", es->dest);
}

/* The check that blocks an export over player changes not synced back
 * (cyanbox/export.h) is recognised by its wording. */
static bool unsynced_check(const char *item)
{
    return cb_str_contains_fold(item, "unsynced") || cb_str_contains_fold(item, "not synced") ||
           cb_str_contains_fold(item, "sync first");
}

/* How many changes that check counts: the first number in its wording
 * ("stick has 3 unsynced changes ..."), else one. */
static int unsynced_changes(const char *item)
{
    const char *p = item;
    while (*p && (*p < '0' || *p > '9'))
        p++;
    long n = *p ? strtol(p, NULL, 10) : 1;
    return n > 0 && n < INT32_MAX ? (int)n : 1;
}

static void export_apply_msg(CBApp *app, const ExportMsg *m)
{
    CBAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    bool dry = priv->export_phase == EXPORT_DRY_RUN;

    es->done = m->done;
    es->total = m->total;
    if (strcmp(m->stage, priv->export_last_stage)) {
        snprintf(priv->export_last_stage, sizeof(priv->export_last_stage), "%s", m->stage);
        snprintf(es->stage, sizeof(es->stage), "%s", m->stage);
        es->item[0] = '\0';
        if (!dry)
            export_log(app, "%s", m->stage);
    }
    /* A step without an item (a copy's tick, a file kept as it was) moves
     * the bar and keeps the name shown. */
    if (!m->item)
        return;
    snprintf(es->item, sizeof(es->item), "%s", m->item);

    /* The summary's counts. A step of a stage carries its total; a message
     * about one (export.c's reportf) carries none. */
    if (!dry && m->total > 0) {
        if (!strcmp(m->stage, "copy"))
            es->copied++;
        else if (!strcmp(m->stage, "skip"))
            es->skipped++;
        else if (!strcmp(m->stage, "prune") && es->prune)
            es->pruned++;
        else if (!strcmp(m->stage, "analysis files") || !strcmp(m->stage, "artwork") ||
                 !strcmp(m->stage, "database"))
            es->rewritten++;
    } else if (!dry && !strcmp(m->stage, "prune") && cb_str_starts(m->item, "cannot remove")) {
        es->pruned--;
    }

    if (!strcmp(m->stage, "check")) {
        /* The checks name their problems in `item`. */
        export_log(app, "  %s", m->item);
        if (dry && unsynced_check(m->item))
            es->unsynced += unsynced_changes(m->item);
    } else if (!strcmp(m->stage, "prune")) {
        /* Stick paths are the files beyond the export; anything else is a
         * message about them. */
        bool path = m->item[0] == '/';
        if (dry && path) {
            char **list = realloc(es->prune_list, (es->nb_prune + 1) * sizeof(*list));
            if (list) {
                es->prune_list = list;
                list[es->nb_prune] = cb_strdup(m->item);
                if (list[es->nb_prune])
                    es->nb_prune++;
            }
        } else if (!dry && (!path || es->prune)) {
            export_log(app, "  %s%s", path ? "deleted " : "", m->item);
        }
    } else if (!strcmp(m->stage, "flush")) {
        /* A failed or cancelled run's sync, or the sync failing. */
        export_log(app, "  %s", m->item);
    }
}

/* A dry run ended: ask, run for real, or stop. */
static void export_after_dry_run(CBApp *app, int result)
{
    CBAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    export_join(priv);

    /* Stopped by the unsynced changes alone (EBUSY), the dry run listed
     * everything else, and one question covers both; stopped by anything
     * else, the export cannot run: nothing to ask. */
    bool blocked = es->unsynced > 0 && !es->ignore_unsynced;
    if (result < 0 && !(blocked && result == CB_ERROR(EBUSY))) {
        export_finish(app, result);
        return;
    }

    if (blocked || es->nb_prune > 0) {
        priv->export_phase = EXPORT_CONFIRM;
        es->confirm = true;
        snprintf(es->stage, sizeof(es->stage), "confirm");
        if (blocked)
            export_log(app, "The stick has %d change%s made on a player and not synced back",
                       es->unsynced, es->unsynced == 1 ? "" : "s");
        if (es->nb_prune)
            export_log(app, "%d file%s on the stick %s no longer exported", es->nb_prune,
                       es->nb_prune == 1 ? "" : "s", es->nb_prune == 1 ? "is" : "are");
        cb_app_set_status(app, "Export to %s: waiting for confirmation", es->dest);
        return;
    }
    export_run(app, false);
}

static bool export_tick(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    if (priv->export_phase == EXPORT_IDLE)
        return false;

    if (es->cancel) {
        if (priv->export_phase == EXPORT_ANALYSING || priv->export_phase == EXPORT_CONFIRM) {
            export_finish(app, CB_ERROR(ECANCELED));
            return true;
        }
        cb_cancel(&priv->export_cancel);
    }

    switch (priv->export_phase) {
    case EXPORT_ANALYSING: {
        int waiting = export_waiting(app);
        es->done = priv->export_wait_total - waiting;
        es->total = priv->export_wait_total;
        if (waiting && !export_lost(app, waiting))
            return false;
        export_wait_free(priv);
        flush_saves(app);
        export_run(app, true);
        return true;
    }
    case EXPORT_CONFIRM:
        return false;
    default:
        break;
    }

    /* Read the flag first: every message was posted before it was set. */
    ExportJob *ej = priv->export_job;
    bool finished = ej && __atomic_load_n(&ej->finished, __ATOMIC_ACQUIRE);
    bool changed = false;
    ExportMsg *m;
    while ((m = cb_mailbox_take(priv->export_mb))) {
        export_apply_msg(app, m);
        export_msg_free(m);
        changed = true;
    }
    if (ej) {
        int64_t v[4] = {
            __atomic_load_n(&ej->stats.bytes_done, __ATOMIC_RELAXED),
            __atomic_load_n(&ej->stats.bytes_total, __ATOMIC_RELAXED),
            __atomic_load_n(&ej->stats.rate, __ATOMIC_RELAXED),
            __atomic_load_n(&ej->stats.seconds_left, __ATOMIC_RELAXED),
        };
        if (v[0] != es->bytes_done || v[1] != es->bytes_total || v[2] != es->rate ||
            v[3] != es->seconds_left) {
            es->bytes_done = v[0];
            es->bytes_total = v[1];
            es->rate = v[2];
            es->seconds_left = v[3];
            changed = true;
        }
    }
    if (finished) {
        int result = ej->result;
        /* A real run that got to the end despite a late cancel did export;
         * a dry run that did exported nothing, and the real one never
         * starts. */
        if (cb_cancelled(&priv->export_cancel)) {
            bool exported = result >= 0 && priv->export_phase == EXPORT_RUNNING;
            export_finish(app, exported ? result : CB_ERROR(ECANCELED));
        } else if (priv->export_phase == EXPORT_DRY_RUN) {
            export_after_dry_run(app, result);
        } else {
            export_finish(app, result);
        }
        changed = true;
    }
    return changed;
}

/* Stops a running export for good (cb_app_destroy). */
static void export_stop(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    cb_cancel(&priv->export_cancel);
    export_join(priv);
    export_snapshot_close(priv);
    export_wait_free(priv);
    priv->export_phase = EXPORT_IDLE;
    app->export_state.running = false;
    app->export_state.confirm = false;
    export_prune_list_free(app);
}

int cb_app_start_export(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    if (!priv)
        return CB_ERROR(EINVAL);

    /* The answer to the confirmation: the real run, which the dry run
     * found nothing else in the way of. */
    if (priv->export_phase == EXPORT_CONFIRM) {
        es->confirm = false;
        export_run(app, false);
        app->dirty = true;
        return 0;
    }
    if (priv->export_phase != EXPORT_IDLE) {
        cb_app_set_status(app, "An export is already running");
        return CB_ERROR(EBUSY);
    }
    if (!es->dest[0] || !cb_dir_exists(es->dest)) {
        cb_app_set_status(app, "%s is not a directory (mount the stick first)",
                          es->dest[0] ? es->dest : "The destination");
        return CB_ERROR(ENOTDIR);
    }

    es->running = true;
    es->confirm = false;
    es->cancel = false;
    es->result = 0;
    es->unsynced = 0;
    es->done = 0;
    es->total = 0;
    es->bytes_done = es->bytes_total = es->rate = 0;
    es->seconds_left = -1;
    es->item[0] = '\0';
    es->log[0] = '\0';
    export_prune_list_free(app);

    /* The interface owns its selection array: keep the ids. */
    free(priv->export_playlists);
    priv->export_playlists = NULL;
    priv->nb_export_playlists = 0;
    if (!es->whole_library && es->nb_playlists > 0 && es->playlists) {
        priv->export_playlists = calloc(es->nb_playlists, sizeof(*priv->export_playlists));
        if (!priv->export_playlists) {
            es->running = false;
            return CB_ERROR(ENOMEM);
        }
        for (int i = 0; i < es->nb_playlists; i++) {
            if (es->playlists[i])
                priv->export_playlists[priv->nb_export_playlists++] = es->playlists[i]->id;
        }
    }

    /* What goes on the stick is analysed first, here, so the export's
     * handle finds every grid and waveform in the database: the chosen
     * playlists' tracks, every playlist's by default, or everything. The
     * export waits for those (export_wait_for), not for the rest of the
     * queue. */
    CBArray ids;
    cb_array_init(&ids, sizeof(uint64_t));
    bool oom = false;
    if (es->whole_library) {
        for (int i = 0; i < app->lib.nb_tracks; i++) {
            CBTrack *t = app->lib.tracks[i];
            if (t->missing)
                continue;
            queue_analysis(app, t, CB_ANALYSE_ALL, false, false);
            oom |= !cb_array_push(&ids, &t->id);
        }
    } else {
        int nb = es->nb_playlists;
        CBPlaylist **pls = es->playlists;
        CBPlaylist **all = NULL;
        if (!nb || !pls) {
            nb = cb_library_playlists_flat(&app->lib, NULL, 0);
            all = calloc(CB_MAX(nb, 1), sizeof(*all));
            nb = all ? cb_library_playlists_flat(&app->lib, all, nb) : 0;
            pls = all;
        }
        for (int i = 0; i < nb; i++) {
            const CBPlaylist *p = pls[i];
            for (int k = 0; p && k < p->nb; k++) {
                CBTrack *t = cb_library_find_id(&app->lib, p->track_ids[k]);
                if (!t || t->missing)
                    continue;
                queue_analysis(app, t, CB_ANALYSE_ALL, false, false);
                oom |= !cb_array_push(&ids, &t->id);
            }
        }
        free(all);
    }
    int ret = oom ? CB_ERROR(ENOMEM) : export_wait_for(app, &ids);
    cb_array_free(&ids);
    if (ret < 0) {
        es->running = false;
        return ret;
    }
    flush_saves(app);

    export_log(app, "Export to %s as %s", es->dest, cb_stick_format_name(es->format));
    priv->export_phase = EXPORT_ANALYSING;
    snprintf(es->stage, sizeof(es->stage), "analysis");
    if (priv->nb_export_wait) {
        export_log(app, "Analysing %d track%s first", priv->nb_export_wait,
                   priv->nb_export_wait == 1 ? "" : "s");
        cb_app_set_status(app, "Analysing before the export to %s", es->dest);
    }
    /* With nothing to wait for, the next tick starts the dry run. */
    app->dirty = true;
    return 0;
}

/* --------------------------------------------------------------------- rows */

typedef struct RowKey {
    uint32_t index;             /* in lib->tracks */
    uint32_t number;            /* `#`: the place in the view's own order, from 1 */
} RowKey;

typedef struct SortCtx {
    const CBLibrary *lib;
    int  column;
    bool descending;
} SortCtx;

typedef struct SortVal {
    bool        known;          /* unknown values sort last in both directions */
    double      num;
    const char *str;
} SortVal;

static const char *title_of(const CBTrack *t)
{
    return t->meta.title && t->meta.title[0] ? t->meta.title : cb_path_base(t->path);
}

/* Camelot order: 1A 1B 2A 2B ... 12B, which keeps compatible keys together. */
static int key_rank(const CBTrack *t)
{
    if (!t->key[0])
        return -1;
    char buf[8];
    const char *c = cb_key_to_camelot(t->key, buf, sizeof(buf));
    if (!c || !c[0])
        return -1;
    char *end;
    long n = strtol(c, &end, 10);
    if (n < 1 || n > 12)
        return -1;
    return (int)n * 2 + (*end == 'B' || *end == 'b');
}

static SortVal sort_value(const CBTrack *t, int column)
{
    SortVal v = { 0 };
    switch (column) {
    case CB_COL_TITLE:
        v.str = title_of(t);
        break;
    case CB_COL_ARTIST:
        v.str = t->meta.artist;
        break;
    case CB_COL_ALBUM:
        v.str = t->meta.album;
        break;
    case CB_COL_GENRE:
        v.str = t->meta.genre;
        break;
    case CB_COL_PATH:
        v.str = t->path;
        break;
    case CB_COL_BPM:
        v.num = cb_grid_bpm(&t->grid);
        v.known = v.num > 0;
        return v;
    case CB_COL_KEY:
        v.num = key_rank(t);
        v.known = v.num >= 0;
        return v;
    case CB_COL_TIME:
        v.num = t->duration;
        v.known = v.num > 0;
        return v;
    case CB_COL_RATING:
        v.num = t->meta.rating;
        v.known = true;
        return v;
    case CB_COL_COLOR: {
        /* In the legend's order, pink to purple, no colour last. */
        int id = cb_track_color_of_hue(t->meta.hue);
        v.num = id;
        v.known = id > 0;
        return v;
    }
    case CB_COL_CUES: {
        int hot = 0;
        for (int i = 0; i < t->nb_cues; i++)
            hot += cb_cue_is_hot(&t->cues[i]);
        v.num = hot * 1000.0 + (t->nb_cues - hot);
        v.known = true;
        return v;
    }
    case CB_COL_YEAR:
        v.num = t->meta.year;
        v.known = v.num > 0;
        return v;
    case CB_COL_ADDED:
        v.num = (double)t->added;
        v.known = true;
        return v;
    case CB_COL_PLAYS:
        v.num = t->play_count;
        v.known = true;
        return v;
    case CB_COL_LAST_PLAYED:
        v.num = (double)t->last_played;
        v.known = v.num > 0;
        return v;
    default:
        /* `#`, artwork and preview: the natural order. */
        return v;
    }
    v.known = v.str && v.str[0];
    return v;
}

static int compare_rows(const void *pa, const void *pb, void *opaque)
{
    const RowKey *ra = pa, *rb = pb;
    const SortCtx *ctx = opaque;
    SortVal va = sort_value(ctx->lib->tracks[ra->index], ctx->column);
    SortVal vb = sort_value(ctx->lib->tracks[rb->index], ctx->column);

    if (va.known != vb.known)
        return va.known ? -1 : 1;
    if (va.known) {
        int c;
        if (va.str || vb.str)
            c = cb_strnatcasecmp(va.str ? va.str : "", vb.str ? vb.str : "");
        else
            c = (va.num > vb.num) - (va.num < vb.num);
        if (ctx->descending)
            c = -c;
        if (c)
            return c;
    }
    /* Stable: equal rows keep the view's own order. */
    return (ra->number > rb->number) - (ra->number < rb->number);
}

#define MAX_SEARCH_WORDS 16

/* Splits the search box into words (ASCII and ideographic spaces). */
static int search_words(const char *search, char *buf, size_t size, char **words)
{
    snprintf(buf, size, "%s", search);
    for (char *p = buf; (p = strstr(p, "\xe3\x80\x80")); p += 3)
        memset(p, ' ', 3);

    int nb = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, " \t", &save); w && nb < MAX_SEARCH_WORDS;
         w = strtok_r(NULL, " \t", &save))
        words[nb++] = w;
    return nb;
}

static bool field_matches(const char *field, const char *word)
{
    return field && field[0] && cb_str_contains_fold(field, word);
}

/* Every word must be found in one of the fields the table shows. */
static bool track_matches(const CBTrack *t, char **words, int nb_words)
{
    for (int i = 0; i < nb_words; i++) {
        const char *w = words[i];
        if (!field_matches(t->meta.title, w) && !field_matches(t->meta.artist, w) &&
            !field_matches(t->meta.album, w) && !field_matches(t->meta.genre, w) &&
            !field_matches(t->meta.comment, w) && !field_matches(cb_path_base(t->path), w))
            return false;
    }
    return true;
}

static bool playlist_exists(const CBLibrary *lib, const CBPlaylist *p)
{
    int nb = cb_library_playlists_flat(lib, NULL, 0);
    if (nb <= 0)
        return false;
    CBPlaylist **all = calloc(nb, sizeof(*all));
    if (!all)
        return true;
    nb = cb_library_playlists_flat(lib, all, nb);
    bool found = false;
    for (int i = 0; i < nb && !found; i++)
        found = all[i] == p;
    free(all);
    return found;
}

static bool in_album(const CBTrack *t, const char *album)
{
    return t->meta.album && !cb_strcasecmp(t->meta.album, album);
}

/* The History's plays (cb_app_show_history), oldest first, a track as
 * often as it was played, and its sets, made again when the library has
 * changed. A set is the plays less than CB_APP_HISTORY_GAP apart; it starts
 * with its first and ends as its last track did, its start and its length
 * when a deck played it (it was added by hand otherwise), but not after
 * the next set starts. Plays of tracks no longer in the library are left
 * out of the rows, not of the sets. The count of plays, or a negative
 * errno. */
static int history_tracks(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    CBLibrary *lib = &app->lib;
    if (!priv)
        return CB_ERROR(EINVAL);
    if (priv->history_generation == lib->generation)
        return priv->nb_history;
    CBHistoryEntry *h = NULL;
    int nb = 0;
    int ret = cb_library_history(lib, &h, &nb);
    size_t nr = (size_t)CB_MAX(nb, 1);
    uint64_t *ids = ret >= 0 ? malloc(nr * sizeof(*ids)) : NULL;
    int64_t *entries = ids ? malloc(nr * sizeof(*entries)) : NULL;
    int *set_of = entries ? malloc(nr * sizeof(*set_of)) : NULL;
    int64_t *sets = set_of ? malloc(nr * 2 * sizeof(*sets)) : NULL;
    if (!sets) {
        free(h);
        free(ids);
        free(entries);
        free(set_of);
        return ret < 0 ? ret : CB_ERROR(ENOMEM);
    }
    int nb_sets = 0, n = 0;
    for (int i = 0; i < nb; i++) {
        if (!i || h[i].played_at - h[i - 1].played_at > CB_APP_HISTORY_GAP) {
            sets[2 * nb_sets] = sets[2 * nb_sets + 1] = h[i].played_at;
            nb_sets++;
        }
        const CBTrack *t = cb_library_find_id(lib, h[i].track_id);
        int64_t end = h[i].played_at +
                      (t && h[i].player && t->duration > 0 ? (int64_t)ceil(t->duration) : 0);
        sets[2 * nb_sets - 1] = CB_MAX(sets[2 * nb_sets - 1], end);
        if (!t)
            continue;
        ids[n] = h[i].track_id;
        entries[n] = h[i].id;
        set_of[n++] = nb_sets - 1;
    }
    for (int s = 0; s + 1 < nb_sets; s++)
        sets[2 * s + 1] = CB_MIN(sets[2 * s + 1], sets[2 * s + 2]);
    free(h);
    free(priv->history);
    free(priv->history_entry);
    free(priv->history_set);
    free(priv->history_sets);
    priv->history = ids;
    priv->history_entry = entries;
    priv->history_set = set_of;
    priv->nb_history = n;
    priv->history_sets = sets;
    priv->nb_history_sets = nb_sets;
    priv->history_generation = lib->generation;
    return n;
}

bool cb_app_history_row(CBApp *app, int row, int64_t *entry, int *set)
{
    if (!app->current_history || app->rows_generation != app->lib.generation || row < 0 ||
        row >= app->nb_rows || history_tracks(app) < 0)
        return false;
    /* `#` numbers the rows in the History's own order: the play's place. */
    uint32_t k = app->row_numbers[row] - 1;
    if (k >= (uint32_t)app->priv->nb_history)
        return false;
    if (entry)
        *entry = app->priv->history_entry[k];
    if (set)
        *set = app->priv->history_set[k];
    return true;
}

bool cb_app_history_set_times(CBApp *app, int set, int64_t *started, int64_t *ended)
{
    if (!app->link_state.enabled || history_tracks(app) < 0 || set < 0 ||
        set >= app->priv->nb_history_sets)
        return false;
    *started = app->priv->history_sets[2 * set];
    *ended = app->priv->history_sets[2 * set + 1];
    return true;
}

int cb_app_history_count(CBApp *app)
{
    if (!app->link_state.enabled)
        return 0;
    int n = history_tracks(app);
    return CB_MAX(n, 0);
}

/* Leaving the History: the backwards order it opened with goes. */
static void leave_history(CBApp *app)
{
    if (!app->current_history)
        return;
    app->current_history = false;
    if (app->sort_column < 0 && app->sort_descending)
        app->sort_descending = false;
}

/* None's tracks (cb_app_albums ranks them first): no album tag, or one
 * no other track has. */
static bool in_none(const CBAppPriv *priv, int index)
{
    return priv && index >= 0 && index < priv->nb_rank && priv->rank[index] < priv->nb_none;
}

/* The album artist `name` in the list made with the albums: its index,
 * -1 when it has none. */
static int artist_index(const CBAppPriv *priv, const char *name)
{
    for (size_t i = 0; priv && i < priv->album_artists.nb; i++) {
        const CBAlbumArtist *aa = cb_array_at(&priv->album_artists, i);
        if (!aa->name[0] == !name[0] && !cb_strnatcasecmp(aa->name, name))
            return (int)i;
    }
    return -1;
}

static bool in_artist(const CBAppPriv *priv, int artist, int index)
{
    if (!priv || artist < 0 || index < 0 || index >= priv->nb_rank)
        return false;
    const ArtistSpan *s = cb_array_at(&priv->artist_spans, (size_t)artist);
    int r = priv->rank[index];
    return (r >= s->lo && r < s->hi) || (s->none && r < priv->nb_none);
}

static int compare_ranks(const void *pa, const void *pb, void *opaque)
{
    const RowKey *ra = pa, *rb = pb;
    const CBAppPriv *priv = opaque;
    return (priv->rank[ra->index] > priv->rank[rb->index]) -
           (priv->rank[ra->index] < priv->rank[rb->index]);
}

void cb_app_rebuild_rows(CBApp *app)
{
    CBLibrary *lib = &app->lib;

    /* The interface may have deleted the playlist it was showing. */
    if (app->current_playlist && !playlist_exists(lib, app->current_playlist))
        app->current_playlist = NULL;

    const CBPlaylist *pl = app->current_playlist;
    /* The TAG LIST and the History: their ids in their order, as a
     * playlist's. */
    CBLinkTagList *tags = pl || !app->current_tag_list ? NULL : cb_app_tag_list(app);
    uint64_t *tag_ids = NULL;
    int nb_tags = 0;
    if (tags) {
        uint32_t *v = malloc(CB_LINK_TAG_LIST_MAX * sizeof(*v));
        tag_ids = malloc(CB_LINK_TAG_LIST_MAX * sizeof(*tag_ids));
        nb_tags = v && tag_ids ? CB_MIN(cb_link_tag_list_get(tags, v, CB_LINK_TAG_LIST_MAX),
                                        CB_LINK_TAG_LIST_MAX) : 0;
        for (int i = 0; i < nb_tags; i++)
            tag_ids[i] = v[i];
        free(v);
    } else if (!pl && app->current_history && history_tracks(app) >= 0) {
        const CBAppPriv *hp = app->priv;
        if ((tag_ids = malloc((size_t)CB_MAX(hp->nb_history, 1) * sizeof(*tag_ids)))) {
            nb_tags = hp->nb_history;
            memcpy(tag_ids, hp->history, (size_t)nb_tags * sizeof(*tag_ids));
        }
    }
    bool own = pl || tag_ids;
    const char *album = own ? NULL : app->current_album;
    /* None (the album ""): the tracks no album of the list holds. The
     * list ranks every track, the collection's own order. */
    bool none = album && !album[0];
    const char *artist = own || album ? NULL : app->current_album_artist;
    const CBAppPriv *priv = app->priv;
    if (!pl) {
        const CBAlbum *albums;
        cb_app_albums(app, &albums);
    }
    int artist_at = artist ? artist_index(priv, artist) : -1;
    bool ranked = !own && priv && priv->nb_rank == lib->nb_tracks;
    int cap = pl ? pl->nb : tag_ids ? nb_tags : lib->nb_tracks;
    RowKey *keys = calloc(CB_MAX(cap, 1), sizeof(*keys));
    if (!keys) {
        free(tag_ids);
        cb_err("out of memory building the track table");
        return;
    }

    char buf[sizeof(app->search)];
    char *words[MAX_SEARCH_WORDS];
    int nb_words = search_words(app->search, buf, sizeof(buf), words);

    int nb = 0;
    for (int i = 0; i < cap; i++) {
        const CBTrack *t = pl      ? cb_library_find_id(lib, pl->track_ids[i])
                         : tag_ids ? cb_library_find_id(lib, tag_ids[i])
                                   : lib->tracks[i];
        if (!t)
            continue;
        if (none ? !in_none(app->priv, i) : album && !in_album(t, album))
            continue;
        if (artist && !in_artist(priv, artist_at, i))
            continue;
        keys[nb++] = (RowKey) {
            .index = own ? (uint32_t)cb_library_index_of(lib, t) : (uint32_t)i,
        };
    }
    free(tag_ids);

    /* The view's own order, which `#` numbers before the search and the
     * sort: a playlist's; else the collection's (cb_app_albums), None's
     * tracks by title then the albums in the list's order, each in disc
     * and track order, of which None and an album are runs, and an album
     * artist's albums one after the other. (Without the
     * ranks, out of memory, the library's order.) */
    if (ranked)
        qsort_r(keys, nb, sizeof(*keys), compare_ranks, (void *)priv);
    for (int i = 0; i < nb; i++)
        keys[i].number = (uint32_t)i + 1;

    if (nb_words) {
        int kept = 0;
        for (int i = 0; i < nb; i++) {
            if (track_matches(lib->tracks[keys[i].index], words, nb_words))
                keys[kept++] = keys[i];
        }
        nb = kept;
    }

    SortCtx ctx = {
        .lib        = lib,
        .column     = app->sort_column,
        .descending = app->sort_descending,
    };
    if (ctx.column >= 0 && ctx.column < CB_COL_COUNT) {
        qsort_r(keys, nb, sizeof(*keys), compare_rows, &ctx);
    } else if (ctx.descending) {
        for (int i = 0; i < nb / 2; i++) {
            RowKey tmp = keys[i];
            keys[i] = keys[nb - 1 - i];
            keys[nb - 1 - i] = tmp;
        }
    }

    /* New arrays, so a failure leaves the old rows and numbers whole. */
    uint32_t *rows = malloc(CB_MAX(nb, 1) * sizeof(*rows));
    uint32_t *numbers = malloc(CB_MAX(nb, 1) * sizeof(*numbers));
    if (!rows || !numbers) {
        free(rows);
        free(numbers);
        free(keys);
        cb_err("out of memory building the track table");
        return;
    }
    for (int i = 0; i < nb; i++) {
        rows[i] = keys[i].index;
        numbers[i] = keys[i].number;
    }
    free(keys);

    free(app->rows);
    free(app->row_numbers);
    app->rows = rows;
    app->row_numbers = numbers;
    app->nb_rows = nb;
    app->rows_generation = lib->generation;
    app->rows_dirty = false;
    app->dirty = true;
}

/* Albums whose names differ only in case are one album (a tag typed
 * "Greatest Hits" on one track and "greatest hits" on another), in
 * natural order ("AD:HOUSE 3" before "AD:HOUSE 10"), the exact spellings
 * in a fixed order within, so a run of one album is contiguous and named
 * by its first spelling; then the library's order. */
typedef struct AlbumTrack {
    const char *name;
    int         index;          /* in lib->tracks */
} AlbumTrack;

static int compare_album_names(const char *a, const char *b)
{
    int c = cb_strnatcasecmp(a, b);
    return c ? c : strcmp(a, b);
}

static int compare_album_tracks(const void *pa, const void *pb)
{
    const AlbumTrack *a = pa, *b = pb;
    int c = compare_album_names(a->name, b->name);
    return c ? c : (a->index > b->index) - (a->index < b->index);
}

/* An album of the list being made: its entry, and its tracks, a run of
 * the sorted AlbumTracks. */
typedef struct AlbumRun {
    CBAlbum al;
    int     start;
} AlbumRun;

/* The list's order: by artist, in natural order, each artist's albums
 * together by name; the albums of no artist after them, by name. */
static int compare_albums(const void *pa, const void *pb)
{
    const CBAlbum *a = &((const AlbumRun *)pa)->al, *b = &((const AlbumRun *)pb)->al;
    if (!a->artist != !b->artist)
        return a->artist ? -1 : 1;
    int c = a->artist ? cb_strnatcasecmp(a->artist, b->artist) : 0;
    return c ? c : compare_album_names(a->name, b->name);
}

/* An album's tracks: disc, then track number (tracks without one last),
 * then title; the library's order between equals. */
static int compare_in_album(const void *pa, const void *pb, void *opaque)
{
    const CBLibrary *lib = opaque;
    int ia = ((const AlbumTrack *)pa)->index, ib = ((const AlbumTrack *)pb)->index;
    const CBTrack *a = lib->tracks[ia], *b = lib->tracks[ib];
    int da = a->meta.disc_number > 0 ? a->meta.disc_number : INT_MAX;
    int db = b->meta.disc_number > 0 ? b->meta.disc_number : INT_MAX;
    if (da != db)
        return da < db ? -1 : 1;
    int na = a->meta.track_number > 0 ? a->meta.track_number : INT_MAX;
    int nb = b->meta.track_number > 0 ? b->meta.track_number : INT_MAX;
    if (na != nb)
        return na < nb ? -1 : 1;
    int c = cb_strnatcasecmp(title_of(a), title_of(b));
    return c ? c : (ia > ib) - (ia < ib);
}

/* None's tracks: by title, then artist; the library's order between
 * equals. */
static int compare_in_none(const void *pa, const void *pb, void *opaque)
{
    const CBLibrary *lib = opaque;
    int ia = *(const int *)pa, ib = *(const int *)pb;
    const CBTrack *a = lib->tracks[ia], *b = lib->tracks[ib];
    int c = cb_strnatcasecmp(title_of(a), title_of(b));
    if (!c)
        c = cb_strnatcasecmp(a->meta.artist ? a->meta.artist : "",
                             b->meta.artist ? b->meta.artist : "");
    return c ? c : (ia > ib) - (ia < ib);
}

/* The artist an album sorts under: the album artist of its first track
 * (in the library's order) that has one, else the artist its tracks
 * share; NULL when they share none. */
static const char *album_artist(const CBLibrary *lib, const AlbumTrack *run, int n)
{
    int first = -1;
    for (int i = 0; i < n; i++) {
        const char *a = lib->tracks[run[i].index]->meta.album_artist;
        if (a && a[0] && (first < 0 || run[i].index < first))
            first = run[i].index;
    }
    if (first >= 0)
        return lib->tracks[first]->meta.album_artist;
    const char *artist = lib->tracks[run[0].index]->meta.artist;
    if (!artist || !artist[0])
        return NULL;
    for (int i = 1; i < n; i++) {
        const char *a = lib->tracks[run[i].index]->meta.artist;
        if (!a || cb_strcasecmp(a, artist))
            return NULL;
    }
    return artist;
}

static void albums_free(CBAppPriv *priv)
{
    for (size_t i = 0; i < priv->albums.nb; i++) {
        CBAlbum *al = cb_array_at(&priv->albums, i);
        free((char *)al->name);
        free((char *)al->artist);
    }
    for (size_t i = 0; i < priv->album_artists.nb; i++)
        free((char *)((CBAlbumArtist *)cb_array_at(&priv->album_artists, i))->name);
    priv->albums.nb = 0;
    priv->album_artists.nb = 0;
    priv->artist_spans.nb = 0;
    priv->albums_generation = 0;
    priv->nb_rank = 0;
    priv->nb_none = 0;
}

static bool artist_push(CBAppPriv *priv, const char *name, int nb, ArtistSpan span)
{
    CBAlbumArtist aa = { .name = cb_strdup(name), .nb = nb };
    if (!aa.name || !cb_array_push(&priv->album_artists, &aa)) {
        free((char *)aa.name);
        return false;
    }
    return cb_array_push(&priv->artist_spans, &span);
}

/* The album artists of the list just made, whose ranks run in its order
 * after None's: each artist's albums are a run of it (it is by artist),
 * and the albums of no artist, last, go to None with None's tracks. */
static bool artists_make(CBAppPriv *priv, int nb_tracks, int nb_none)
{
    const CBAlbum *albums = priv->albums.data;
    int nb_albums = (int)priv->albums.nb;
    ArtistSpan none = { .lo = nb_tracks, .hi = nb_tracks, .none = true };
    int nb = nb_none;
    for (int i = nb_albums - 1; i >= 0 && albums[i].name[0] && !albums[i].artist; i--) {
        none.lo -= albums[i].nb;
        nb += albums[i].nb;
    }
    if (nb && !artist_push(priv, "", nb, none))
        return false;
    int rank = nb_none;
    for (int i = 0; i < nb_albums && rank < none.lo; i++) {
        const CBAlbum *al = &albums[i];
        if (!al->name[0])
            continue;
        if (!al->artist)
            break;
        size_t last = priv->album_artists.nb;
        CBAlbumArtist *aa = last ? cb_array_at(&priv->album_artists, last - 1) : NULL;
        if (aa && aa->name[0] && !cb_strnatcasecmp(aa->name, al->artist)) {
            aa->nb += al->nb;
            ((ArtistSpan *)cb_array_at(&priv->artist_spans, last - 1))->hi += al->nb;
        } else if (!artist_push(priv, al->artist, al->nb,
                                (ArtistSpan) { .lo = rank, .hi = rank + al->nb })) {
            return false;
        }
        rank += al->nb;
    }
    return true;
}

/* Makes the list, the album artists and the ranks; false when out of
 * memory (the lists are then empty, the ranks none). */
static bool albums_make(CBAppPriv *priv, const CBLibrary *lib)
{
    int nt = lib->nb_tracks;
    int *rank = realloc(priv->rank, sizeof(*rank) * CB_MAX(nt, 1));
    if (!rank)
        return false;
    priv->rank = rank;
    AlbumTrack *tracks = malloc(sizeof(*tracks) * CB_MAX(nt, 1));
    AlbumRun *runs = malloc(sizeof(*runs) * CB_MAX(nt / 2, 1));
    int *none = malloc(sizeof(*none) * CB_MAX(nt, 1));
    bool ok = tracks && runs && none;

    /* The names with their tracks, sorted; a run of equal ones is one
     * album, of two tracks or more; the rest are None's. */
    int n = 0, nb_runs = 0, nb_none = 0;
    for (int i = 0; ok && i < nt; i++) {
        const char *a = lib->tracks[i]->meta.album;
        if (a && a[0])
            tracks[n++] = (AlbumTrack) { a, i };
        else
            none[nb_none++] = i;
    }
    if (ok)
        qsort(tracks, n, sizeof(*tracks), compare_album_tracks);
    for (int i = 0; ok && i < n;) {
        int j = i + 1;
        while (j < n && !cb_strcasecmp(tracks[j].name, tracks[i].name))
            j++;
        if (j - i == 1) {
            none[nb_none++] = tracks[i].index;
        } else {
            const char *artist = album_artist(lib, tracks + i, j - i);
            AlbumRun *r = &runs[nb_runs++];
            *r = (AlbumRun) {
                .al = {
                    .name   = cb_strdup(tracks[i].name),
                    .nb     = j - i,
                    .artist = artist ? cb_strdup(artist) : NULL,
                },
                .start = i,
            };
            ok = r->al.name && (!artist || r->al.artist);
        }
        i = j;
    }

    /* None first, by title; then the albums in the list's order, each in
     * its own. */
    if (ok) {
        qsort(runs, nb_runs, sizeof(*runs), compare_albums);
        qsort_r(none, nb_none, sizeof(*none), compare_in_none, (void *)lib);
        int k = 0;
        for (int i = 0; i < nb_none; i++)
            rank[none[i]] = k++;
        for (int r = 0; r < nb_runs; r++) {
            AlbumTrack *run = tracks + runs[r].start;
            qsort_r(run, runs[r].al.nb, sizeof(*run), compare_in_album, (void *)lib);
            for (int i = 0; i < runs[r].al.nb; i++)
                rank[run[i].index] = k++;
        }
        CBAlbum al = { .name = cb_strdup(""), .nb = nb_none };
        ok = al.name && (!nb_none || cb_array_push(&priv->albums, &al));
        if (!nb_none || !ok)
            free((char *)al.name);
    }
    for (int r = 0; r < nb_runs; r++) {
        if (ok && cb_array_push(&priv->albums, &runs[r].al))
            continue;
        ok = false;
        free((char *)runs[r].al.name);
        free((char *)runs[r].al.artist);
    }
    free(tracks);
    free(runs);
    free(none);
    if (!ok || !artists_make(priv, nt, nb_none)) {
        albums_free(priv);
        return false;
    }
    priv->nb_rank = nt;
    priv->nb_none = nb_none;
    return true;
}

int cb_app_albums(CBApp *app, const CBAlbum **albums)
{
    CBAppPriv *priv = app->priv;
    CBLibrary *lib = &app->lib;
    *albums = NULL;
    if (!priv)
        return 0;
    if (priv->albums_generation != lib->generation) {
        albums_free(priv);
        if (!albums_make(priv, lib)) {
            cb_err("out of memory making the albums");
            return 0;
        }
        priv->albums_generation = lib->generation;
    }
    *albums = priv->albums.data;
    return (int)priv->albums.nb;
}

int cb_app_album_artists(CBApp *app, const CBAlbumArtist **artists)
{
    const CBAlbum *albums;
    cb_app_albums(app, &albums);
    CBAppPriv *priv = app->priv;
    *artists = priv ? priv->album_artists.data : NULL;
    return priv ? (int)priv->album_artists.nb : 0;
}

void cb_app_show_album(CBApp *app, const char *name)
{
    char *copy = name ? cb_strdup(name) : NULL;
    if (name && !copy)
        return;
    free(app->current_album);
    app->current_album = copy;
    free(app->current_album_artist);
    app->current_album_artist = NULL;
    app->current_tag_list = false;
    leave_history(app);
    if (copy)
        app->current_playlist = NULL;
    cb_app_rebuild_rows(app);
}

void cb_app_show_album_artist(CBApp *app, const char *name)
{
    char *copy = name ? cb_strdup(name) : NULL;
    if (name && !copy)
        return;
    free(app->current_album_artist);
    app->current_album_artist = copy;
    if (copy) {
        free(app->current_album);
        app->current_album = NULL;
        app->current_playlist = NULL;
        app->current_tag_list = false;
        leave_history(app);
    }
    cb_app_rebuild_rows(app);
}

void cb_app_show_tag_list(CBApp *app)
{
    if (!cb_app_tag_list(app))
        return;
    free(app->current_album);
    app->current_album = NULL;
    free(app->current_album_artist);
    app->current_album_artist = NULL;
    app->current_playlist = NULL;
    leave_history(app);
    app->current_tag_list = true;
    cb_app_rebuild_rows(app);
}

void cb_app_show_history(CBApp *app)
{
    if (!app->link_state.enabled)
        return;
    free(app->current_album);
    app->current_album = NULL;
    free(app->current_album_artist);
    app->current_album_artist = NULL;
    app->current_playlist = NULL;
    app->current_tag_list = false;
    if (!app->current_history) {
        app->current_history = true;
        app->sort_column = -1;
        app->sort_descending = true;
    }
    cb_app_rebuild_rows(app);
}

/* ---------------------------------------------------------------- lifecycle */

int cb_app_create(CBApp *app, const char *library_root, const CBPlayerOptions *player_opt,
                  const CBWindowOptions *window_opt)
{
    memset(app, 0, sizeof(*app));
    CBAppPriv *priv = calloc(1, sizeof(*priv));
    if (!priv)
        return CB_ERROR(ENOMEM);
    app->priv = priv;
    cb_array_init(&priv->inflight, sizeof(Inflight));
    cb_array_init(&priv->saves, sizeof(uint64_t));
    cb_array_init(&priv->gpu, sizeof(GpuEntry));
    cb_array_init(&priv->covers, sizeof(CoverTexture));
    cb_array_init(&priv->wave_checks, sizeof(WaveCheck));
    cb_array_init(&priv->albums, sizeof(CBAlbum));
    cb_array_init(&priv->album_artists, sizeof(CBAlbumArtist));
    cb_array_init(&priv->artist_spans, sizeof(ArtistSpan));
    for (int d = 0; d < CB_DECKS; d++) {
        priv->uploaded_palette[d] = -1;
        priv->play_last_pos[d] = -1.0;
    }

    int ret = cb_mailbox_create(&priv->export_mb);
    if (ret < 0)
        goto fail;

    /* Opening also checks which files went missing. */
    ret = cb_library_open(&app->lib, library_root);
    if (ret < 0) {
        cb_err("cannot open the library%s%s: %s", library_root ? " at " : "",
               library_root ? library_root : "", strerror(-ret));
        goto fail;
    }
    priv->lib_open = true;
    /* My Settings that are not valid go, as the rest of what is not
     * (cb_library_open's cleaning). */
    cb_settings_clean(&app->lib);
    const CBConfig *cfg = &app->lib.cfg;

    /* One core stays with the interface and the audio. */
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    ret = cb_jobs_create(&app->jobs, (int)CB_MAX(cpus - 1, 1));
    if (ret < 0)
        goto fail;
    /* The analyses run on the GPU the window was asked for (--device), or
     * on the CPU only (--no-gpu). */
    if (window_opt)
        cb_jobs_set_gpu(app->jobs, !window_opt->cpu_analysis, window_opt->device);
    cb_app_set_beat_model(app);
    ret = cb_jobs_create(&priv->io_jobs, IO_THREADS);
    if (ret < 0)
        goto fail;

    CBPlayerOptions po;
    if (player_opt) {
        po = *player_opt;
    } else {
        cb_player_options_default(&po);
        po.backend = CB_AUDIO_NULL;
    }
    if (!po.sink && po.backend == CB_AUDIO_PIPEWIRE)
        po.sink = cfg->audio_sink;
    if (!po.app_name)
        po.app_name = "cyanbox";
    ret = cb_player_create(&app->player, &po);
    if (ret < 0 && po.backend != CB_AUDIO_NULL) {
        cb_warn("no audio output (%s); the player plays silently", strerror(-ret));
        po.backend = CB_AUDIO_NULL;
        ret = cb_player_create(&app->player, &po);
    }
    if (ret < 0)
        goto fail;

    for (int d = 0; d < CB_DECKS; d++) {
        CBDeckView *dv = &app->decks[d];
        cb_ctl_init(&dv->ctl, app->player, d, deck_changed, app);
        dv->zoom_bars = cfg->detail_bars > 0 ? cfg->detail_bars : 12;
    }
    app->wave_palette = palette_of(cfg);
    app->export_state.format = cfg->export_format;
    app->sort_column = -1;
    app->rows_dirty = true;

    if (window_opt) {
        priv->max_frames = window_opt->max_frames;
        ret = cb_window_create(&app->window, window_opt);
        if (ret < 0)
            goto fail;
        ret = cb_ui_init(app);
        if (ret < 0)
            goto fail;
        priv->ui_ready = true;
    }

    cb_app_rebuild_rows(app);

    /* After an upgrade, what the older analyser made is analysed again in
     * the background, unasked, with the status bar's usual progress. */
    int stale = queue_stale(app);
    char again[96] = "";
    if (stale > 0)
        snprintf(again, sizeof(again), "; %d made by an older analyser, analysing %s again",
                 stale, stale == 1 ? "it" : "them");

    int missing = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++)
        missing += app->lib.tracks[i]->missing;
    if (missing)
        cb_app_set_status(app, "%d tracks in %s, %d missing%s", app->lib.nb_tracks, app->lib.root,
                          missing, again);
    else
        cb_app_set_status(app, "%d track%s in %s%s", app->lib.nb_tracks,
                          app->lib.nb_tracks == 1 ? "" : "s", app->lib.root, again);
    return 0;

fail:
    cb_app_destroy(app);
    return ret;
}

void cb_app_destroy(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    if (!priv)
        return;

    /* Off the bus before anything a call reaches goes. */
    cb_mpris_destroy(app);

    /* The export has a library handle of its own; cancel it and wait. */
    export_stop(app);
    /* So has Link: it stops serving before anything it reads goes. */
    cb_app_link_destroy(app);

    /* The interface goes before anything it draws from. */
    if (priv->ui_ready) {
        cb_ui_shutdown(app);
        priv->ui_ready = false;
    }

    /* Running jobs finish, queued ones are dropped, results freed. */
    cb_jobs_destroy(app->jobs);
    app->jobs = NULL;
    cb_jobs_destroy(priv->io_jobs);
    priv->io_jobs = NULL;
    cb_job_result_free(app->stick_result);
    app->stick_result = NULL;
    app->stick_request = 0;

    for (int d = 0; d < CB_DECKS; d++)
        deck_clear(app, d);
    gpu_cache_free(app);

    if (priv->lib_open)
        flush_saves(app);

    /* The player before the library: the deck controls point at tracks. */
    if (app->player)
        cb_player_destroy(app->player);
    app->player = NULL;

    /* The window after every texture and waveform made through it. */
    if (app->window)
        cb_window_destroy(app->window);
    app->window = NULL;

    free(app->rows);
    app->rows = NULL;
    free(app->row_numbers);
    app->row_numbers = NULL;
    app->nb_rows = 0;
    free(app->current_album);
    app->current_album = NULL;
    free(app->current_album_artist);
    app->current_album_artist = NULL;
    albums_free(priv);
    free(priv->rank);
    free(priv->history);
    free(priv->history_entry);
    free(priv->history_set);
    free(priv->history_sets);
    cb_array_free(&priv->albums);
    cb_array_free(&priv->album_artists);
    cb_array_free(&priv->artist_spans);
    free(app->selection);
    app->selection = NULL;
    app->nb_selection = 0;
    app->current_playlist = NULL;
    app->current_folder = NULL;

    if (priv->lib_open)
        cb_library_close(&app->lib);

    cb_array_free(&priv->inflight);
    cb_array_free(&priv->saves);
    cb_array_free(&priv->wave_checks);
    free(priv->export_playlists);
    if (priv->export_mb)
        cb_mailbox_destroy(priv->export_mb);
    free(priv);
    app->priv = NULL;
}

static int drain_jobs(CBApp *app, CBJobs *jobs, int max)
{
    int n = 0;
    CBJobResult *r;
    while (n < max && (r = cb_jobs_poll(jobs))) {
        handle_result(app, r);
        cb_job_result_free(r);
        n++;
    }
    return n;
}

bool cb_app_tick(CBApp *app)
{
    CBAppPriv *priv = app->priv;
    if (!priv)
        return false;

    priv->tick++;
    priv->gpu_budget = GPU_LOADS_PER_TICK;
    bool changed = false;

    /* A track removed from the library while loaded: the deck lets go of
     * it before anything (a job result among them) looks at it. */
    for (int d = 0; d < CB_DECKS; d++) {
        CBDeckView *dv = &app->decks[d];
        if (dv->track && cb_library_find_id(&app->lib, priv->deck_track_id[d]) != dv->track) {
            dv->track = NULL;
            deck_clear(app, d);
            changed = true;
        }
    }

    /* The deck's decodes first: they are what someone is waiting for. */
    if (drain_jobs(app, priv->io_jobs, RESULTS_PER_TICK))
        changed = true;
    if (drain_jobs(app, app->jobs, RESULTS_PER_TICK))
        changed = true;

    if (export_tick(app))
        changed = true;

    /* The palette follows the setting, which the Player settings dialog,
     * the View menu and Sync from USB change: the deck's waveforms are
     * uploaded again below, the table's previews as it asks for them. */
    int palette = palette_of(&app->lib.cfg);
    if (palette != app->wave_palette) {
        app->wave_palette = palette;
        gpu_forget_previews(app);
        changed = true;
    }

    double quantize_beats = quantize_beats_of(&app->lib.cfg);
    for (int d = 0; d < CB_DECKS; d++) {
        CBDeckView *dv = &app->decks[d];
        dv->ctl.quantize_beats = quantize_beats;
        if (app->player) {
            /* The deck's move to the next track first: the controls then
             * tick on the track the deck plays. */
            if (continue_play(app, d))
                changed = true;
            cb_ctl_tick(&dv->ctl);
        }
        track_plays(app, d);

        if (dv->track && priv->uploaded_palette[d] != app->wave_palette) {
            deck_upload_waves(app, d);
            changed = true;
        }
    }

    if (flush_saves(app))
        changed = true;

    /* After the saves: the library Link reads again is the one saved. */
    if (cb_app_link_tick(app))
        changed = true;

    if (app->rows_dirty || app->rows_generation != app->lib.generation) {
        cb_app_rebuild_rows(app);
        changed = true;
    }

    if (changed)
        app->dirty = true;
    return changed;
}

static bool deck_moving(const CBApp *app)
{
    if (!app->player)
        return false;
    for (int d = 0; d < CB_DECKS; d++) {
        CBDeckState st;
        cb_deck_state(app->player, d, &st);
        if (st.playing || st.scrubbing)
            return true;
    }
    return false;
}

int cb_app_run(CBApp *app)
{
    if (!app->window || !app->priv)
        return CB_ERROR(EINVAL);

    CBAppPriv *priv = app->priv;
    bool ui_wants = true;
    app->dirty = true;

    while (!app->quit) {
        cb_app_tick(app);
        cb_mpris_tick(app);

        /* An export waiting for the answer to its question is not busy. */
        bool busy = cb_jobs_pending(app->jobs) > 0 || cb_jobs_pending(priv->io_jobs) > 0 ||
                    (app->export_state.running && !app->export_state.confirm);
        bool want = app->dirty || ui_wants || deck_moving(app) || busy;
        /* --frames: draw continuously so the count is reached. */
        if (priv->max_frames > 0)
            want = true;

        if (cb_window_poll(app->window, want, want ? 4 : 50)) {
            app->dirty = false;
            cb_window_begin_frame(app->window);
            ui_wants = cb_ui_frame(app);
            cb_window_end_frame(app->window);
        }

        if (cb_window_should_close(app->window))
            app->quit = true;
        if (priv->max_frames > 0 && cb_window_frames_drawn(app->window) >= priv->max_frames)
            app->quit = true;
    }

    if (app->export_state.running) {
        app->export_state.cancel = true;
        export_tick(app);
    }
    flush_saves(app);
    return 0;
}
