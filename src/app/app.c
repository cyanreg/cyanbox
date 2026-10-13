/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The application state the interface reads and writes (rxbox/app.h):
 * creating and tearing down the library, the jobs, the player and the
 * window; applying job results; loading the deck; the track table's rows
 * and its per-track GPU resources; play history; the export running in
 * the background; and the interface loop.
 *
 * Threads and ownership:
 *  - Everything here runs on the owner (main) thread, which alone touches
 *    the library, the deck and the window. Background work goes through
 *    RXJobs (rxbox/jobs.h), whose results come back in rx_app_tick(), which
 *    applies and frees them.
 *  - Two job queues: app->jobs (every CPU but one) analyses; a private
 *    two-thread queue decodes for the deck, makes artwork and reads sticks,
 *    so loading a track, showing covers or reading a stick never waits
 *    behind a library-wide analysis.
 *    Request ids are per queue; each kind of request only ever goes to one
 *    of them.
 *  - A decode result's packet store (or, for a file the store refuses, its
 *    RXAudio decoded whole) is handed to the player: rx_deck_load_packets
 *    takes it over on success (and releases what it replaces only once the
 *    real-time thread and the deck's decoding thread have let go of it); on
 *    failure it stays ours to free. The job only opens the store, a few
 *    milliseconds, so LOADING shows for about as long as a frame takes.
 *  - The deck view's waveforms are its own copy, read from the library;
 *    their GPU uploads, the deck's artwork texture and the browser cache's
 *    textures and previews belong to this file and are released through
 *    the window before it goes.
 *  - The export runs on a thread of its own with a library handle of its
 *    own (a second connection to the same database): rx_export() wants a
 *    mutable library, and sharing the interface's would race with every
 *    edit made while it runs. What the tracks lack is analysed first here,
 *    through the normal jobs, and pending saves are flushed, so the
 *    export's handle finds everything in the database. That handle is a
 *    snapshot opened here, on the thread that makes every edit, so no edit
 *    lands half way through its reading; the dry run and the real run use
 *    the same one, so what the interface asked about (the files to delete)
 *    is what the real run does, whatever was edited meanwhile. Progress
 *    comes back as malloc'd messages (strings copied) through a mailbox;
 *    the only other shared state is the cancel flag and the finished
 *    flag/result, published with release/acquire.
 *  - Saves asked for by the deck controls are queued and written once per
 *    tick, so a burst of edits costs one transaction. */

#include "rxbox/app.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "rxbox/analysis.h"
#include "rxbox/colors.h"
#include "rxbox/export.h"
#include "rxbox/formats.h"
#include "rxbox/notation.h"

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
/* Workers for decoding and artwork. */
#define IO_THREADS       2
/* Artwork jobs scale to this; it is also the library thumbnail's size. */
#define ART_SIZE         240
/* A failed analysis's message stays in the status bar this long before
 * the next tracks' "Analysed ..." may replace it: as long as the bar shows
 * a message as new (src/ui/ui.cpp, status_bar). */
#define STATUS_HOLD_US   8000000

typedef struct RXAppPriv RXAppPriv;

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
    uint64_t   texture;
    RXWaveGpu *preview;
    uint64_t   art_request;     /* io job in flight */
    uint8_t    art_state;
    uint8_t    wave_state;
    uint64_t   used;            /* tick when last asked for */
} GpuEntry;

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
 * `result`, then sets `finished` last. The library is the export's
 * snapshot (RXAppPriv.export_lib), the thread's alone while it runs. */
typedef struct ExportJob {
    RXLibrary *lib;
    char    *dest;
    int64_t *playlists;         /* ids of the selected playlists */
    int      nb_playlists;
    bool     whole_library, prune, ignore_unsynced, compat, dry_run;
    RXMailbox *mb;
    RXCancel  *cancel;
    int      result;
    int      finished;          /* atomic */
} ExportJob;

struct RXAppPriv {
    bool     lib_open;
    bool     ui_ready;
    int      max_frames;
    uint64_t tick;

    RXJobs  *io_jobs;           /* decodes and artwork */
    RXArray  inflight;          /* Inflight: analyses queued and not yet applied */
    int      analyses_failed;   /* since the queue was last empty */
    int64_t  status_held;       /* status_time of a failure's status, kept a while (0 none) */
    RXArray  saves;             /* uint64_t: tracks whose state is due to be written */

    int      uploaded_palette[RX_DECKS];    /* palette of the deck's GPU waveforms, -1 none */
    uint64_t deck_track_id[RX_DECKS];
    uint8_t  play_seen[RX_DECKS][PLAY_BINS / 8];   /* bins of the track played since the load */
    int      play_bins[RX_DECKS];           /* bits set in play_seen */
    double   play_last_pos[RX_DECKS];       /* position at the last tick, < 0 none */
    int64_t  play_last_us[RX_DECKS];
    bool     play_recorded[RX_DECKS];

    RXArray  gpu;               /* GpuEntry, the browser's cache */
    int      gpu_budget;
    RXArray  wave_checks;       /* WaveCheck: the stored waveforms looked at */

    RXMailbox *export_mb;
    RXCancel   export_cancel;
    int        export_phase;    /* enum ExportPhase */
    int64_t   *export_playlists;    /* ids of the selected playlists, taken at start */
    int        nb_export_playlists;
    RXLibrary  export_lib;      /* the snapshot the dry run and the real run export */
    bool       export_lib_open;
    unsigned   export_generation;   /* lib.generation when the snapshot was taken */
    ExportJob *export_job;      /* the run on the thread, NULL between runs */
    bool       export_started;  /* the thread exists and must be joined */
    bool       export_probe;    /* the dry run again, past the unsynced check
                                   (export_after_dry_run) */
    pthread_t  export_thread;
    char       export_last_stage[32];
};

/* ------------------------------------------------------------------ helpers */

/* The waveform palette is the players' WAVEFORM COLOR (device.waveform_color,
 * rxbox/formats.h rx_settings_get): what the stick will show is what the
 * deck and the table show. */
static int palette_of(const RXConfig *cfg)
{
    const char *name = rx_settings_get(cfg, "device.waveform_color");
    if (name && !strcmp(name, "blue"))
        return 1;
    if (name && !strcmp(name, "rgb"))
        return 2;
    return 0;
}

static void analysis_options(const RXConfig *cfg, RXAnalysisOptions *opt)
{
    rx_analysis_options_default(opt);
    if (cfg->bpm_min > 0 && cfg->bpm_max > cfg->bpm_min) {
        opt->bpm_min = cfg->bpm_min;
        opt->bpm_max = cfg->bpm_max;
    }
    opt->snap_bpm = cfg->snap_bpm;
    opt->allow_sections = cfg->allow_sections;
}

static const char *track_name(const RXTrack *t, char *buf, size_t size)
{
    return rx_track_display(t, buf, size);
}

void rx_app_set_status(RXApp *app, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, args);
    va_end(args);
    app->status_time = rx_time_us();
    app->dirty = true;
    rx_debug("status: %s", app->status);
}

/* Decodes the library's thumbnail of a track (owner thread). */
static int artwork_from_library(RXApp *app, uint64_t id, uint8_t **rgba, int *w, int *h)
{
    uint8_t *jpeg = NULL;
    size_t size = 0;
    int tw = 0, th = 0;
    int ret = rx_library_artwork_load(&app->lib, id, &jpeg, &size, &tw, &th);
    if (ret < 0)
        return ret;
    ret = rx_image_decode(jpeg, size, rgba, w, h);
    free(jpeg);
    return ret;
}

/* ---------------------------------------------------------------- waveforms */

static int queue_analysis(RXApp *app, RXTrack *t, unsigned what, bool force, bool override_lock);

static void waves_seen(const RXApp *app, uint64_t id, bool stale)
{
    RXAppPriv *priv = app->priv;
    if (!priv)
        return;
    for (size_t i = 0; i < priv->wave_checks.nb; i++) {
        WaveCheck *c = rx_array_at(&priv->wave_checks, i);
        if (c->track_id == id) {
            c->stale = stale;
            return;
        }
    }
    /* Out of memory: the track is only looked at again. */
    WaveCheck c = { .track_id = id, .stale = stale };
    rx_array_push(&priv->wave_checks, &c);
}

/* Waveforms stored in a format version the library no longer reads
 * (rx_waveforms_unpack's -EPROTO: made before the PWV6/PWV7
 * recalibration) are as good as none, and the track is analysed for them
 * again. A track's are looked at once: the deck and the table's preview
 * record what their loads find, the rest is a look at the stored header
 * here, cheap enough for the whole library at start. */
static bool waves_stale(const RXApp *app, const RXTrack *t)
{
    if (!t->has_waveforms)
        return false;
    RXAppPriv *priv = app->priv;
    for (size_t i = 0; priv && i < priv->wave_checks.nb; i++) {
        const WaveCheck *c = rx_array_at(&priv->wave_checks, i);
        if (c->track_id == t->id)
            return c->stale;
    }
    int ret = rx_library_waveforms_check(&app->lib, t->id);
    if (ret == RX_ERROR(EPROTO))
        rx_debug("track %llu: stored waveforms are an older format; recomputing",
                 (unsigned long long)t->id);
    bool stale = ret == RX_ERROR(EPROTO);
    waves_seen(app, t->id, stale);
    return stale;
}

/* -------------------------------------------------------------------- saves */

void rx_app_save_track(RXApp *app, RXTrack *t)
{
    if (!t)
        return;
    int ret = rx_library_save_track(&app->lib, t);
    if (ret < 0) {
        char name[256];
        app->last_error_count++;
        rx_app_set_status(app, "Cannot save %s: %s", track_name(t, name, sizeof(name)),
                          strerror(-ret));
    }
    app->rows_dirty = true;
}

static void queue_save(RXApp *app, RXTrack *t)
{
    RXAppPriv *priv = app->priv;
    if (!priv) {
        rx_app_save_track(app, t);
        return;
    }
    for (size_t i = 0; i < priv->saves.nb; i++) {
        if (*(uint64_t *)rx_array_at(&priv->saves, i) == t->id)
            return;
    }
    if (!rx_array_push(&priv->saves, &t->id))
        rx_app_save_track(app, t);
}

static bool flush_saves(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    if (!priv || !priv->saves.nb)
        return false;
    for (size_t i = 0; i < priv->saves.nb; i++) {
        RXTrack *t = rx_library_find_id(&app->lib, *(uint64_t *)rx_array_at(&priv->saves, i));
        if (t)
            rx_app_save_track(app, t);
    }
    rx_array_clear(&priv->saves);
    return true;
}

/* The deck controls edited a track's cues or grid (owner thread). */
static void deck_changed(void *opaque, int deck)
{
    RXApp *app = opaque;
    RXTrack *t = app->decks[deck].ctl.track;
    if (t)
        queue_save(app, t);
    app->dirty = true;
}

/* -------------------------------------------------------- browser GPU cache */

static void gpu_entry_release(RXApp *app, GpuEntry *e)
{
    if (app->window) {
        if (e->texture)
            rx_texture_destroy(app->window, e->texture);
        if (e->preview)
            rx_wave_release(app->window, e->preview);
    }
    if (e->art_request && app->priv->io_jobs)
        rx_jobs_cancel(app->priv->io_jobs, e->art_request);
    e->texture = 0;
    e->preview = NULL;
    e->art_request = 0;
}

static GpuEntry *gpu_find(RXAppPriv *priv, uint64_t id)
{
    for (size_t i = 0; i < priv->gpu.nb; i++) {
        GpuEntry *e = rx_array_at(&priv->gpu, i);
        if (e->track_id == id)
            return e;
    }
    return NULL;
}

/* The entry for a track, made when absent; beyond GPU_CACHE_MAX the one
 * asked for least recently (and not this tick) is recycled. */
static GpuEntry *gpu_entry(RXApp *app, uint64_t id)
{
    RXAppPriv *priv = app->priv;
    GpuEntry *e = gpu_find(priv, id);
    if (e)
        return e;

    if (priv->gpu.nb >= GPU_CACHE_MAX) {
        GpuEntry *old = NULL;
        for (size_t i = 0; i < priv->gpu.nb; i++) {
            GpuEntry *c = rx_array_at(&priv->gpu, i);
            if (c->used != priv->tick && (!old || c->used < old->used))
                old = c;
        }
        if (!old)
            return NULL;
        gpu_entry_release(app, old);
        e = old;
    } else {
        e = rx_array_push(&priv->gpu, NULL);
        if (!e)
            return NULL;
    }
    *e = (GpuEntry) {
        .track_id = id,
        .used     = priv->tick,
    };
    return e;
}

uint64_t rx_app_artwork_texture(RXApp *app, const RXTrack *t)
{
    RXAppPriv *priv = app->priv;
    if (!priv || !app->window || !t)
        return 0;
    GpuEntry *e = gpu_entry(app, t->id);
    if (!e)
        return 0;
    e->used = priv->tick;

    /* The library's thumbnail whatever the track says (an older library
     * may hold one for a track whose flag is off); else the file's art,
     * when the track has some. Either way once: GPU_DONE without a
     * texture is the answer "none" until the entry is recycled. */
    if (e->art_state == GPU_UNKNOWN && priv->gpu_budget > 0) {
        priv->gpu_budget--;
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        if (artwork_from_library(app, t->id, &rgba, &w, &h) >= 0) {
            e->texture = rx_texture_create(app->window, rgba, w, h);
            e->art_state = GPU_DONE;
            free(rgba);
        } else if (t->has_artwork && !t->missing) {
            uint64_t req = rx_jobs_artwork(priv->io_jobs, t->id, t->path, ART_SIZE);
            e->art_request = (int64_t)req > 0 ? req : 0;
            e->art_state = e->art_request ? GPU_LOADING : GPU_DONE;
        } else {
            e->art_state = GPU_DONE;
        }
    }
    return e->texture;
}

bool rx_app_artwork_pending(RXApp *app, const RXTrack *t)
{
    RXAppPriv *priv = app->priv;
    if (!priv || !app->window || !t)
        return false;
    const GpuEntry *e = gpu_find(priv, t->id);
    return !e || e->art_state != GPU_DONE;
}

/* A preview tag on the GPU in a palette: PWV6 (3-band), PWAV (blue) or
 * PWV4 (RGB), the overview the players draw in that WAVEFORM COLOR. */
static RXWaveGpu *upload_preview(RXApp *app, int palette, const RXWaveforms *w)
{
    switch (palette) {
    case 1:
        return rx_wave_upload(app->window, RX_WAVE_BLUE, w->pwav, RX_WAVE_PWAV_COLUMNS);
    case 2:
        return rx_wave_upload(app->window, RX_WAVE_RGB_PREVIEW, w->pwv4, RX_WAVE_PWV4_COLUMNS);
    default:
        return rx_wave_upload(app->window, RX_WAVE_3BAND, w->pwv6, RX_WAVE_PWV6_COLUMNS);
    }
}

RXWaveGpu *rx_app_preview_wave(RXApp *app, const RXTrack *t)
{
    RXAppPriv *priv = app->priv;
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
        RXWaveforms w = { 0 };
        int ret = rx_library_waveforms_load(&app->lib, t->id, &w);
        if (ret >= 0)
            e->preview = upload_preview(app, app->wave_palette, &w);
        rx_waveforms_free(&w);
        e->wave_state = GPU_DONE;
        /* Stale ones are made again; the preview follows (gpu_forget_waves). */
        waves_seen(app, t->id, ret == RX_ERROR(EPROTO));
        RXTrack *lt = ret == RX_ERROR(EPROTO) ? rx_library_find_id(&app->lib, t->id) : NULL;
        if (lt)
            queue_analysis(app, lt, RX_ANALYSE_WAVES, false, false);
    }
    return e->preview;
}

/* The waveforms of a track changed: its preview is made again on demand. */
static void gpu_forget_waves(RXApp *app, uint64_t id)
{
    if (!app->priv)
        return;
    GpuEntry *e = gpu_find(app->priv, id);
    if (!e)
        return;
    if (e->preview && app->window)
        rx_wave_release(app->window, e->preview);
    e->preview = NULL;
    e->wave_state = GPU_UNKNOWN;
}

/* The palette changed: every preview is made again, in the new one, as
 * the table asks for it. */
static void gpu_forget_previews(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    for (size_t i = 0; i < priv->gpu.nb; i++) {
        GpuEntry *e = rx_array_at(&priv->gpu, i);
        if (e->preview && app->window)
            rx_wave_release(app->window, e->preview);
        e->preview = NULL;
        e->wave_state = GPU_UNKNOWN;
    }
}

static void gpu_cache_free(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    for (size_t i = 0; i < priv->gpu.nb; i++)
        gpu_entry_release(app, rx_array_at(&priv->gpu, i));
    rx_array_free(&priv->gpu);
}

/* --------------------------------------------------------------------- deck */

static void release_waves_gpu(RXApp *app, RXDeckView *dv)
{
    if (app->window) {
        if (dv->wave_preview)
            rx_wave_release(app->window, dv->wave_preview);
        if (dv->wave_detail)
            rx_wave_release(app->window, dv->wave_detail);
    }
    dv->wave_preview = NULL;
    dv->wave_detail = NULL;
}

static void release_artwork(RXApp *app, RXDeckView *dv)
{
    if (app->window && dv->artwork)
        rx_texture_destroy(app->window, dv->artwork);
    dv->artwork = 0;
}

/* Uploads the deck's waveforms in the current palette (or releases them
 * when there are none). */
static void deck_upload_waves(RXApp *app, int deck)
{
    RXDeckView *dv = &app->decks[deck];
    release_waves_gpu(app, dv);
    if (app->priv)
        app->priv->uploaded_palette[deck] = app->wave_palette;

    const RXWaveforms *w = &dv->waves;
    if (!app->window || w->nb_detail <= 0)
        return;

    dv->wave_preview = upload_preview(app, app->wave_palette, w);
    switch (app->wave_palette) {
    case 1:
        if (w->pwv3)
            dv->wave_detail = rx_wave_upload(app->window, RX_WAVE_BLUE, w->pwv3, w->nb_detail);
        break;
    case 2:
        if (w->pwv5)
            dv->wave_detail = rx_wave_upload(app->window, RX_WAVE_RGB_DETAIL, w->pwv5,
                                             w->nb_detail);
        break;
    default:
        if (w->pwv7)
            dv->wave_detail = rx_wave_upload(app->window, RX_WAVE_3BAND, w->pwv7, w->nb_detail);
        break;
    }
}

/* Reads the deck track's waveforms from the library into the deck view;
 * the GPU copy follows on the next tick. */
static void deck_load_waves(RXApp *app, int deck)
{
    RXDeckView *dv = &app->decks[deck];
    rx_waveforms_free(&dv->waves);
    memset(&dv->waves, 0, sizeof(dv->waves));
    if (app->priv)
        app->priv->uploaded_palette[deck] = -1;
    if (!dv->track || !dv->track->has_waveforms)
        return;

    /* Stale ones count as none: the analysis queued with the load (or
     * after this one) makes them again. */
    int ret = rx_library_waveforms_load(&app->lib, dv->track->id, &dv->waves);
    waves_seen(app, dv->track->id, ret == RX_ERROR(EPROTO));
    if (ret < 0) {
        if (ret != RX_ERROR(EPROTO))
            rx_warn("cannot read the waveforms of track %llu: %s",
                    (unsigned long long)dv->track->id, strerror(-ret));
        rx_waveforms_free(&dv->waves);
        memset(&dv->waves, 0, sizeof(dv->waves));
    }
}

static void deck_set_artwork(RXApp *app, RXDeckView *dv, const uint8_t *rgba, int w, int h)
{
    release_artwork(app, dv);
    if (app->window && rgba)
        dv->artwork = rx_texture_create(app->window, rgba, w, h);
}

/* Drops what the deck view holds for its track: the artwork request, the
 * waveforms and their uploads, the artwork, the play count. The player is
 * left alone. */
static void deck_view_reset(RXApp *app, int deck)
{
    RXDeckView *dv = &app->decks[deck];
    RXAppPriv *priv = app->priv;

    if (priv && priv->io_jobs && dv->artwork_request)
        rx_jobs_cancel(priv->io_jobs, dv->artwork_request);
    dv->artwork_request = 0;

    release_waves_gpu(app, dv);
    release_artwork(app, dv);
    rx_waveforms_free(&dv->waves);
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

/* Empties the deck: cancels its loads, unloads the player, drops the
 * waveforms and the artwork. */
static void deck_clear(RXApp *app, int deck)
{
    RXDeckView *dv = &app->decks[deck];
    RXAppPriv *priv = app->priv;

    if (priv && priv->io_jobs && dv->loading_request)
        rx_jobs_cancel(priv->io_jobs, dv->loading_request);
    dv->loading_request = 0;
    dv->loading_track_id = 0;

    if (app->player) {
        rx_ctl_unload(&dv->ctl);
        rx_deck_unload(app->player, deck);
    }
    deck_view_reset(app, deck);
}

int rx_app_load_deck(RXApp *app, int deck, RXTrack *t)
{
    if (deck < 0 || deck >= RX_DECKS || !t)
        return RX_ERROR(EINVAL);
    if (!app->player || !app->priv || !app->priv->io_jobs)
        return RX_ERROR(ENODEV);

    char name[256];
    track_name(t, name, sizeof(name));
    if (t->missing) {
        rx_app_set_status(app, "Cannot load %s: the file is missing", name);
        return RX_ERROR(ENOENT);
    }

    /* Replacing a playing track is the interface's decision (it pauses
     * the deck, then calls again). */
    RXDeckState st;
    rx_deck_state(app->player, deck, &st);
    if (st.playing)
        return RX_ERROR(EBUSY);

    RXDeckView *dv = &app->decks[deck];
    RXJobs *io = app->priv->io_jobs;
    if (dv->loading_request)
        rx_jobs_cancel(io, dv->loading_request);
    dv->loading_request = 0;
    dv->loading_track_id = 0;

    uint64_t req = rx_jobs_decode(io, t->id, t->path, deck);
    if ((int64_t)req < 0) {
        rx_app_set_status(app, "Cannot load %s: %s", name, strerror((int)-(int64_t)req));
        return (int)(int64_t)req;
    }

    dv->loading_request = req;
    dv->loading_track_id = t->id;
    rx_app_set_status(app, "Loading %s", name);
    return 0;
}

void rx_app_unload_deck(RXApp *app, int deck)
{
    if (deck < 0 || deck >= RX_DECKS)
        return;
    deck_clear(app, deck);
    app->dirty = true;
}

/* rekordbox counts a play after 30 seconds of playing; so does the
 * history the stick sync compares against. Once per load, and only for
 * audio that really played forward: a seek, a jump, a loop wrapping or a
 * scrub adds nothing, and what a loop or a repeated hot cue plays again
 * counts once (PLAY_BIN). */
static void track_plays(RXApp *app, int deck)
{
    RXAppPriv *priv = app->priv;
    RXDeckView *dv = &app->decks[deck];
    if (!app->player || !dv->track || priv->play_recorded[deck])
        return;

    RXDeckState st;
    rx_deck_state(app->player, deck, &st);
    int64_t now = rx_time_us();
    double last = priv->play_last_pos[deck];
    double since = (now - priv->play_last_us[deck]) * 1e-6;
    priv->play_last_pos[deck] = st.loaded ? st.position : -1.0;
    priv->play_last_us[deck] = now;
    if (!st.loaded || !st.playing || st.scrubbing || last < 0)
        return;

    double step = st.position - last;
    double allowed = RX_MAX(PLAY_MAX_STEP, since * RX_MAX(st.tempo, 1.0) * 1.25 + 0.05);
    if (!(step > 0.0) || step > allowed)
        return;

    int b0 = (int)RX_MIN(floor(last / PLAY_BIN), PLAY_BINS - 1.0);
    int b1 = (int)RX_MIN(floor(st.position / PLAY_BIN), PLAY_BINS - 1.0);
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
    int ret = rx_library_record_play(&app->lib, dv->track);
    if (ret < 0) {
        char name[256];
        app->last_error_count++;
        rx_app_set_status(app, "Cannot record the play of %s: %s",
                          track_name(dv->track, name, sizeof(name)), strerror(-ret));
    }
    app->rows_dirty = true;
}

/* ----------------------------------------------------------------- analysis */

unsigned rx_app_analysis_stale(const RXApp *app, const RXTrack *t)
{
    /* The version first: it costs nothing, the format is a read. */
    unsigned stale = rx_analysis_stale(t);
    if (!(stale & RX_ANALYSE_WAVES) && waves_stale(app, t))
        stale |= RX_ANALYSE_WAVES;
    return stale;
}

unsigned rx_app_analysis_needed(const RXApp *app, const RXTrack *t, unsigned what, bool force)
{
    if (!t || t->missing)
        return 0;

    /* A stale result is as good as none: the upgrade that bumped its
     * version is there to replace it. A locked grid is kept all the
     * same. */
    unsigned stale = rx_app_analysis_stale(app, t);
    unsigned need = what & RX_ANALYSE_ALL;
    if (!force) {
        if (rx_track_analysed(t) && !(stale & RX_ANALYSE_BEATS))
            need &= ~RX_ANALYSE_BEATS;
        if (t->key[0] && !(stale & RX_ANALYSE_KEY))
            need &= ~RX_ANALYSE_KEY;
    }
    if (t->grid_locked && !force)
        need &= ~RX_ANALYSE_BEATS;

    /* The deck, the table preview and the stick all need the waveforms,
     * so missing or stale ones are made whatever was asked. */
    if (!t->has_waveforms || (stale & RX_ANALYSE_WAVES))
        need |= RX_ANALYSE_WAVES;
    else if (!force)
        need &= ~RX_ANALYSE_WAVES;

    return need;
}

/* Queues what a track lacks. Returns 1 when queued, 0 when there was
 * nothing to do (or the same work is already queued), or an error. */
static int queue_analysis(RXApp *app, RXTrack *t, unsigned what, bool force, bool override_lock)
{
    if (!app->jobs || !t)
        return RX_ERROR(EINVAL);
    if (t->missing)
        return RX_ERROR(ENOENT);

    unsigned need = rx_app_analysis_needed(app, t, what, force);
    if (t->grid_locked && !override_lock)
        need &= ~RX_ANALYSE_BEATS;
    if (!need)
        return 0;

    RXAppPriv *priv = app->priv;
    if (priv) {
        for (size_t i = 0; i < priv->inflight.nb; i++) {
            const Inflight *f = rx_array_at(&priv->inflight, i);
            if (f->track_id == t->id && !(need & ~f->what))
                return 0;
        }
    }

    RXAnalysisOptions opt;
    analysis_options(&app->lib.cfg, &opt);
    /* With other work queued the pool is the parallelism; a single
     * analysis on its own may use threads inside. */
    opt.deterministic_threads = rx_jobs_pending(app->jobs) > 0;

    uint64_t req = rx_jobs_analyse(app->jobs, t->id, t->path, &opt, need);
    if ((int64_t)req < 0)
        return (int)(int64_t)req;

    if (priv) {
        Inflight f = {
            .request       = req,
            .track_id      = t->id,
            .what          = need,
            .override_lock = override_lock && t->grid_locked,
        };
        if (!rx_array_push(&priv->inflight, &f))
            rx_warn("out of memory tracking analysis requests");
    }
    app->analysing_total++;
    app->dirty = true;
    return 1;
}

int rx_app_analyse_track(RXApp *app, RXTrack *t, unsigned what, bool force)
{
    if (!t)
        return RX_ERROR(EINVAL);
    if (t->missing) {
        char name[256];
        rx_app_set_status(app, "Cannot analyse %s: the file is missing",
                          track_name(t, name, sizeof(name)));
        return RX_ERROR(ENOENT);
    }
    /* One track, asked for explicitly: forcing it replaces even a locked
     * grid (the deck's RE-ANALYSE, the table's "Analyse (force)"). */
    return queue_analysis(app, t, what, force, force);
}

int rx_app_analyse_all(RXApp *app, bool force)
{
    /* Library-wide, locked grids are kept even when forced: a bulk
     * re-analysis must not undo grids fixed by hand. */
    int queued = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        RXTrack *t = app->lib.tracks[i];
        if (!t->missing && queue_analysis(app, t, RX_ANALYSE_ALL, force, false) > 0)
            queued++;
    }
    if (queued)
        rx_app_set_status(app, "Analysing %d track%s", queued, queued == 1 ? "" : "s");
    else
        rx_app_set_status(app, "Every track is analysed");
    return queued;
}

/* What an older analyser made, after an upgrade that bumped a stage's
 * version or the waveform format (rx_app_analysis_stale): those stages of
 * every track are queued, and only those (and waveforms the track lacks,
 * which always count). Locked grids stay, as in any analysis that was not
 * asked for by name. Returns how many tracks were queued. */
static int queue_stale(RXApp *app)
{
    int queued = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        RXTrack *t = app->lib.tracks[i];
        if (t->missing)
            continue;
        unsigned stale = rx_app_analysis_stale(app, t);
        if (stale && queue_analysis(app, t, stale, false, false) > 0)
            queued++;
    }
    return queued;
}

/* override_lock: a forced analysis of one track replaces its locked grid
 * (the lock stays). */
static int apply_analysis(RXApp *app, RXTrack *t, RXAnalysis *a, bool override_lock)
{
    if (!t || !a)
        return RX_ERROR(EINVAL);

    RXLibrary *lib = &app->lib;
    int err = 0;

    bool grid_changed = false;
    if ((a->done & RX_ANALYSE_BEATS) && (!t->grid_locked || override_lock) &&
        rx_grid_valid(&a->beats.grid)) {
        RXGrid g = { 0 };
        int ret = rx_grid_copy(&g, &a->beats.grid);
        if (ret < 0)
            return ret;
        rx_grid_free(&t->grid);
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
    if ((a->done & RX_ANALYSE_BEATS) && (!t->grid_locked || override_lock))
        t->beats_version = RX_BEATS_VERSION;

    if ((a->done & RX_ANALYSE_KEY) && a->key.key[0]) {
        snprintf(t->key, sizeof(t->key), "%s", a->key.key);
        t->key_confidence = a->key.confidence;
    }
    if (a->done & RX_ANALYSE_KEY)
        t->key_version = RX_KEY_VERSION;

    /* The decode is the players' timeline: trust it over the probe. */
    if (a->duration > 0) {
        t->duration = a->duration;
        if (t->sample_rate > 0)
            t->total_samples = llround(a->duration * t->sample_rate);
    }

    /* Cues set in bars follow a new grid. */
    if (grid_changed)
        rx_track_resolve_cues(t);

    /* dj's _auto_cues: a memory cue on the first downbeat once the music
     * starts, for a track with no cues. On a locked grid it is placed on
     * that grid. */
    if ((a->done & RX_ANALYSE_BEATS) && lib->cfg.auto_cues && !t->nb_cues &&
        rx_grid_valid(&t->grid)) {
        RXBeatResult b = a->beats;
        b.grid = t->grid;
        int ret = rx_analysis_auto_cues(t, &b);
        if (ret < 0)
            rx_warn("cannot add the automatic cue: %s", strerror(-ret));
    }

    if (a->done & RX_ANALYSE_WAVES) {
        int ret = rx_library_waveforms_save(lib, t->id, &a->waves);
        if (ret < 0) {
            rx_warn("cannot store the waveforms of track %llu: %s", (unsigned long long)t->id,
                    strerror(-ret));
            err = ret;
        } else {
            t->has_waveforms = true;
            t->waves_version = RX_WAVES_VERSION;
            waves_seen(app, t->id, false);
        }
        gpu_forget_waves(app, t->id);
    }

    if (a->done)
        t->analysed = rx_time_now();

    int ret = rx_library_save_track(lib, t);
    if (ret < 0 && !err)
        err = ret;

    for (int d = 0; d < RX_DECKS; d++) {
        RXDeckView *dv = &app->decks[d];
        if (dv->track != t)
            continue;
        if (grid_changed && app->player)
            rx_ctl_grid_changed(&dv->ctl);
        if (a->done & RX_ANALYSE_WAVES)
            deck_load_waves(app, d);
    }

    app->rows_dirty = true;
    app->dirty = true;
    return err;
}

int rx_app_apply_analysis(RXApp *app, RXTrack *t, RXAnalysis *a)
{
    return apply_analysis(app, t, a, false);
}

int rx_app_rescan(RXApp *app)
{
    int ret = rx_library_rescan_missing(&app->lib);
    if (ret < 0) {
        app->last_error_count++;
        rx_app_set_status(app, "Cannot rescan the library: %s", strerror(-ret));
        return ret;
    }
    int missing = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++)
        missing += app->lib.tracks[i]->missing;
    rx_app_set_status(app, "%d track%s changed state; %d missing", ret, ret == 1 ? "" : "s",
                      missing);
    app->rows_dirty = true;
    return ret;
}

/* -------------------------------------------------------------- job results */

static bool take_inflight(RXAppPriv *priv, uint64_t request, Inflight *out)
{
    for (size_t i = 0; i < priv->inflight.nb; i++) {
        Inflight *f = rx_array_at(&priv->inflight, i);
        if (f->request == request) {
            *out = *f;
            rx_array_remove(&priv->inflight, i);
            return true;
        }
    }
    return false;
}

/* Why an analysis failed, for the status bar. The decoder's errors come
 * back as errno values (rx_audio_error: damaged data is EINVAL, a file
 * without audio ENODATA), whose own wording means little there; the log
 * has strerror's. */
static const char *analysis_error(int err)
{
    switch (err) {
    case RX_ERROR(ENOENT):  return "the file is missing";
    case RX_ERROR(EACCES):
    case RX_ERROR(EPERM):   return "the file cannot be read (permission denied)";
    case RX_ERROR(EINVAL):
    case RX_ERROR(ENODATA): return "the file does not decode (damaged, or no audio)";
    case RX_ERROR(ENOTSUP): return "the file's format is not supported";
    case RX_ERROR(ENOMEM):  return "out of memory";
    default:                return strerror(-err);
    }
}

/* A failure is said with the track's name and kept in the status bar for a
 * while: in a batch, the next results (up to RESULTS_PER_TICK in the same
 * tick) would otherwise replace it before it is drawn. Any other message
 * set meanwhile ends that. The row's spinner follows the worker
 * (rx_jobs_active_tracks), which is idle again by now, failed or not. */
static void handle_analysis(RXApp *app, RXJobResult *r)
{
    RXAppPriv *priv = app->priv;
    Inflight f = { 0 };
    take_inflight(priv, r->request, &f);
    app->analysing_done++;

    RXTrack *t = rx_library_find_id(&app->lib, r->track_id);
    char name[256];
    bool held = priv->status_held && priv->status_held == app->status_time &&
                rx_time_us() - app->status_time < STATUS_HOLD_US;
    if (!t) {
        /* Removed from the library while it was analysed. */
    } else if (r->err < 0 || !r->analysis) {
        int err = r->err < 0 ? r->err : RX_ERROR(EINVAL);
        app->last_error_count++;
        priv->analyses_failed++;
        rx_warn("cannot analyse %s: %s", t->path, strerror(-err));
        track_name(t, name, sizeof(name));
        if (priv->analyses_failed > 1)
            rx_app_set_status(app, "Analysis failed for %s: %s (%d failed so far)", name,
                              analysis_error(err), priv->analyses_failed);
        else
            rx_app_set_status(app, "Analysis failed for %s: %s", name, analysis_error(err));
        priv->status_held = app->status_time;
    } else {
        int ret = apply_analysis(app, t, r->analysis, f.override_lock);

        track_name(t, name, sizeof(name));
        if (ret < 0) {
            app->last_error_count++;
            rx_app_set_status(app, "Analysed %s, but saving failed: %s", name, strerror(-ret));
            priv->status_held = app->status_time;
        } else if (held) {
            /* The failure before it stays. */
        } else if (rx_grid_valid(&t->grid)) {
            rx_app_set_status(app, "Analysed %s: %.2f BPM%s%s", name, rx_grid_bpm(&t->grid),
                              t->key[0] ? ", " : "", t->key);
        } else {
            rx_app_set_status(app, "Analysed %s", name);
        }
    }

    if (!priv->inflight.nb) {
        app->analysing_total = 0;
        app->analysing_done = 0;
        priv->analyses_failed = 0;
    }
}

static void handle_decode(RXApp *app, RXJobResult *r)
{
    int deck = r->deck;
    if (deck < 0 || deck >= RX_DECKS)
        return;
    RXDeckView *dv = &app->decks[deck];

    /* A result for a request the deck has moved on from is dropped. */
    if (!r->request || r->request != dv->loading_request)
        return;
    uint64_t id = dv->loading_track_id;
    dv->loading_request = 0;
    dv->loading_track_id = 0;

    RXTrack *t = rx_library_find_id(&app->lib, id);
    if (!t) {
        rx_app_set_status(app, "Not loaded: the track left the library meanwhile");
        return;
    }
    if (!app->player)
        return;

    char name[256];
    track_name(t, name, sizeof(name));
    if (r->err < 0 || (!r->packets && !r->audio)) {
        app->last_error_count++;
        rx_app_set_status(app, "Cannot load %s: %s", name,
                          strerror(r->err < 0 ? -r->err : EINVAL));
        return;
    }

    /* The player owns the packet store, or the buffer decoded whole, from
     * here (the job malloc'd it, as the player's default release wants); on
     * failure it stays ours. Loading replaces what the deck plays, so the
     * old track goes only once the new one is in: a player that cannot
     * take it (its command queue full while the output stalls) keeps the
     * track it has. */
    const RXGrid *grid = rx_grid_valid(&t->grid) ? &t->grid : NULL;
    int ret;
    if (r->packets) {
        RXPackets *pk = r->packets;
        r->packets = NULL;
        ret = rx_deck_load_packets(app->player, deck, pk, grid);
        if (ret < 0)
            rx_packets_free(pk);
    } else {
        RXAudio *audio = r->audio;
        r->audio = NULL;
        ret = rx_deck_load(app->player, deck, audio, grid);
        if (ret < 0) {
            rx_audio_free(audio);
            free(audio);
        }
    }
    if (ret < 0) {
        app->last_error_count++;
        rx_app_set_status(app, "Cannot load %s: %s", name, strerror(-ret));
        return;
    }
    rx_ctl_unload(&dv->ctl);
    deck_view_reset(app, deck);

    dv->track = t;
    app->priv->deck_track_id[deck] = t->id;
    rx_ctl_load(&dv->ctl, t);
    deck_load_waves(app, deck);

    /* rekordbox analyses a track on load when it has not been: here only
     * what is missing, the waveforms among them. */
    queue_analysis(app, t, RX_ANALYSE_ALL, false, false);

    if (app->window) {
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        if (artwork_from_library(app, t->id, &rgba, &w, &h) >= 0) {
            deck_set_artwork(app, dv, rgba, w, h);
            free(rgba);
        } else {
            uint64_t req = rx_jobs_artwork(app->priv->io_jobs, t->id, t->path, ART_SIZE);
            dv->artwork_request = (int64_t)req > 0 ? req : 0;
        }
    }

    rx_app_set_status(app, "Loaded %s", name);
}

static void handle_artwork(RXApp *app, RXJobResult *r)
{
    RXAppPriv *priv = app->priv;
    RXTrack *t = rx_library_find_id(&app->lib, r->track_id);

    /* The thumbnail goes into the library whoever asked. */
    if (t && r->err >= 0 && r->jpeg) {
        int ret = rx_library_artwork_save(&app->lib, t->id, r->jpeg, r->jpeg_size, ART_SIZE,
                                          ART_SIZE);
        if (ret < 0)
            rx_warn("cannot store the artwork of track %llu: %s", (unsigned long long)t->id,
                    strerror(-ret));
    }
    bool ok = r->err >= 0 && r->rgba;

    for (int d = 0; d < RX_DECKS; d++) {
        RXDeckView *dv = &app->decks[d];
        if (!dv->artwork_request || dv->artwork_request != r->request)
            continue;
        dv->artwork_request = 0;
        if (ok && dv->track && dv->track->id == r->track_id)
            deck_set_artwork(app, dv, r->rgba, r->width, r->height);
    }

    for (size_t i = 0; i < priv->gpu.nb; i++) {
        GpuEntry *e = rx_array_at(&priv->gpu, i);
        if (!e->art_request || e->art_request != r->request)
            continue;
        e->art_request = 0;
        e->art_state = GPU_DONE;
        if (ok && app->window)
            e->texture = rx_texture_create(app->window, r->rgba, r->width, r->height);
    }
}

/* A stick read or inspection is kept whole until the interface takes it.
 * drain_jobs frees the result it hands in, so the payload moves to a copy. */
static void handle_stick(RXApp *app, RXJobResult *r)
{
    if (r->request != app->stick_request)
        return;
    app->stick_request = 0;
    RXJobResult *keep = malloc(sizeof(*keep));
    if (!keep) {
        rx_app_set_status(app, "Out of memory reading the stick");
        return;
    }
    *keep = *r;
    r->stick = NULL;
    r->sync_items = NULL;
    r->nb_sync_items = 0;
    r->report = NULL;
    rx_job_result_free(app->stick_result);
    app->stick_result = keep;
}

static void handle_result(RXApp *app, RXJobResult *r)
{
    switch (r->kind) {
    case RX_JOB_ANALYSE:
        handle_analysis(app, r);
        break;
    case RX_JOB_DECODE:
        handle_decode(app, r);
        break;
    case RX_JOB_ARTWORK:
        handle_artwork(app, r);
        break;
    case RX_JOB_STICK_READ:
    case RX_JOB_INSPECT:
        handle_stick(app, r);
        break;
    }
}

/* On the io queue: a read is mostly waiting on the stick, and must not
 * queue behind a library-wide analysis. */
static int stick_job(RXApp *app, const char *mount, bool inspect)
{
    RXAppPriv *priv = app->priv;
    if (!priv || !priv->io_jobs || !mount || !mount[0])
        return RX_ERROR(EINVAL);
    if (app->stick_request)
        return RX_ERROR(EBUSY);

    /* The worker compares with the library as the database holds it. */
    if (!inspect)
        flush_saves(app);
    uint64_t req = inspect ? rx_jobs_inspect(priv->io_jobs, mount)
                           : rx_jobs_stick_read(priv->io_jobs, app->lib.root, mount);
    if ((int64_t)req < 0)
        return (int)(int64_t)req;

    rx_job_result_free(app->stick_result);
    app->stick_result = NULL;
    app->stick_request = req;
    return 0;
}

int rx_app_read_stick(RXApp *app, const char *mount)
{
    return stick_job(app, mount, false);
}

int rx_app_inspect_stick(RXApp *app, const char *mount)
{
    return stick_job(app, mount, true);
}

/* A running read cannot be stopped; with stick_request moved on,
 * handle_stick drops its result when it comes. */
void rx_app_cancel_stick(RXApp *app, uint64_t request)
{
    if (!request)
        return;
    if (app->stick_request == request) {
        if (app->priv && app->priv->io_jobs)
            rx_jobs_cancel(app->priv->io_jobs, request);
        app->stick_request = 0;
    }
    if (app->stick_result && app->stick_result->request == request) {
        rx_job_result_free(app->stick_result);
        app->stick_result = NULL;
    }
}

/* ----------------------------------------------------------------- add path */

typedef struct AddCtx {
    RXArray ids;
    int     failed;
} AddCtx;

static void add_cb(void *opaque, const char *path, RXTrack *added, int err)
{
    AddCtx *ctx = opaque;
    if (err < 0) {
        ctx->failed++;
        rx_warn("cannot add %s: %s", path, strerror(-err));
        return;
    }
    if (added && !rx_array_push(&ctx->ids, &added->id))
        ctx->failed++;
}

int rx_app_add_path(RXApp *app, const char *path)
{
    char *abs = rx_path_abs(path);
    if (!abs) {
        rx_app_set_status(app, "Cannot add %s: no such file or directory", path);
        return RX_ERROR(ENOENT);
    }

    AddCtx ctx = { 0 };
    rx_array_init(&ctx.ids, sizeof(uint64_t));
    int before = app->lib.nb_tracks;
    int ret = rx_library_add_path(&app->lib, abs, add_cb, &ctx);
    if (ret < 0) {
        app->last_error_count++;
        rx_app_set_status(app, "Cannot add %s: %s", abs, strerror(-ret));
        goto end;
    }

    /* Everything reported is queued: already known tracks too, when they
     * still lack something. */
    int queued = 0;
    for (size_t i = 0; i < ctx.ids.nb; i++) {
        RXTrack *t = rx_library_find_id(&app->lib, *(uint64_t *)rx_array_at(&ctx.ids, i));
        if (t && queue_analysis(app, t, RX_ANALYSE_ALL, false, false) > 0)
            queued++;
    }

    int added = app->lib.nb_tracks - before;
    if (ctx.failed)
        rx_app_set_status(app, "Added %d track%s from %s (%d failed), analysing %d", added,
                          added == 1 ? "" : "s", rx_path_base(abs), ctx.failed, queued);
    else
        rx_app_set_status(app, "Added %d track%s from %s, analysing %d", added,
                          added == 1 ? "" : "s", rx_path_base(abs), queued);
    app->rows_dirty = true;

end:
    rx_array_free(&ctx.ids);
    free(abs);
    return ret;
}

/* ------------------------------------------------------------------- export */

/* The export runs in phases: what the selected tracks lack is analysed
 * through the jobs; a dry run on the export thread reports what a real run
 * would copy, skip and prune and whether the stick has player changes not
 * synced back; when there is something to ask, the interface confirms
 * (prune or not, export over unsynced changes or not), both questions at
 * once; then the real run. rx_export's checks stop a dry run at the
 * unsynced changes before it lists what pruning would delete, so a dry run
 * stopped there alone is run again past that check (the probe), and the
 * question waits for both answers. */
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

static void export_drain(RXAppPriv *priv)
{
    if (!priv->export_mb)
        return;
    ExportMsg *m;
    while ((m = rx_mailbox_take(priv->export_mb)))
        export_msg_free(m);
}

static void export_prune_list_free(RXApp *app)
{
    typeof(app->export_state) *es = &app->export_state;
    for (int i = 0; i < es->nb_prune; i++)
        free(es->prune_list[i]);
    free(es->prune_list);
    es->prune_list = NULL;
    es->nb_prune = 0;
}

/* Appends a line to the dialog's log, dropping the oldest lines when full. */
static void export_log(RXApp *app, const char *fmt, ...) RX_PRINTF(2, 3);
static void export_log(RXApp *app, const char *fmt, ...)
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

/* Export thread: progress to the mailbox, strings copied. */
static bool export_progress(void *opaque, const char *stage, int done, int total, const char *item)
{
    ExportJob *ej = opaque;
    ExportMsg *m = calloc(1, sizeof(*m));
    if (m) {
        snprintf(m->stage, sizeof(m->stage), "%s", stage ? stage : "");
        m->item = rx_strdup(item);
        m->done = done;
        m->total = total;
        if (rx_mailbox_post(ej->mb, m) < 0)
            export_msg_free(m);
    }
    return !rx_cancelled(ej->cancel);
}

/* Export thread: the selected playlists in its own library handle. */
static int resolve_playlists(RXLibrary *lib, const ExportJob *ej, RXPlaylist ***out)
{
    int nb_all = rx_library_playlists_flat(lib, NULL, 0);
    RXPlaylist **all = calloc(RX_MAX(nb_all, 1), sizeof(*all));
    RXPlaylist **sel = calloc(ej->nb_playlists, sizeof(*sel));
    if (!all || !sel) {
        free(all);
        free(sel);
        return RX_ERROR(ENOMEM);
    }
    nb_all = rx_library_playlists_flat(lib, all, nb_all);

    int ret = 0;
    for (int i = 0; i < ej->nb_playlists; i++) {
        for (int k = 0; k < nb_all && !sel[i]; k++) {
            if (all[k]->id == ej->playlists[i])
                sel[i] = all[k];
        }
        if (!sel[i]) {
            rx_err("export: playlist %lld is gone", (long long)ej->playlists[i]);
            ret = RX_ERROR(ENOENT);
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
    RXPlaylist **playlists = NULL;

    int ret = 0;
    if (ej->nb_playlists)
        ret = resolve_playlists(ej->lib, ej, &playlists);
    if (ret >= 0) {
        RXExportOptions opt;
        rx_export_options_default(&opt);
        opt.whole_library = ej->whole_library;
        opt.playlists = (const RXPlaylist *const *)playlists;
        opt.nb_playlists = ej->nb_playlists;
        opt.prune = ej->prune;
        opt.ignore_unsynced = ej->ignore_unsynced;
        opt.compat_transcode = ej->compat;
        opt.dry_run = ej->dry_run;
        ret = rx_export(ej->lib, ej->dest, &opt, export_progress, ej);
    }
    free(playlists);

    ej->result = ret;
    __atomic_store_n(&ej->finished, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* Joins the export thread and drops its job. */
static void export_join(RXAppPriv *priv)
{
    if (priv->export_started) {
        pthread_join(priv->export_thread, NULL);
        priv->export_started = false;
    }
    export_drain(priv);
    export_job_free(priv->export_job);
    priv->export_job = NULL;
}

/* The export is over: its snapshot goes (after export_join). */
static void export_snapshot_close(RXAppPriv *priv)
{
    if (priv->export_lib_open)
        rx_library_close(&priv->export_lib);
    priv->export_lib_open = false;
}

static void export_finish(RXApp *app, int result)
{
    RXAppPriv *priv = app->priv;
    export_join(priv);
    export_snapshot_close(priv);
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
        rx_app_set_status(app, "Export to %s finished", es->dest);
    } else if (result == RX_ERROR(ECANCELED)) {
        snprintf(es->stage, sizeof(es->stage), "cancelled");
        export_log(app, "Cancelled");
        rx_app_set_status(app, "Export to %s cancelled", es->dest);
    } else {
        snprintf(es->stage, sizeof(es->stage), "failed");
        export_log(app, "Failed: %s", strerror(-result));
        app->last_error_count++;
        rx_app_set_status(app, "Export to %s failed: %s", es->dest, strerror(-result));
    }
}

/* Starts a dry or a real run on the export thread, from the dialog state. */
static void export_run(RXApp *app, bool dry_run)
{
    RXAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;

    /* The snapshot both runs export, taken once pending saves are written
     * (export_tick) and before anything can be edited again. */
    if (!priv->export_lib_open) {
        int ret = rx_library_open(&priv->export_lib, app->lib.root);
        if (ret < 0) {
            export_finish(app, ret);
            return;
        }
        priv->export_lib_open = true;
        priv->export_generation = app->lib.generation;
    } else if (!dry_run && priv->export_generation != app->lib.generation) {
        export_log(app, "Changes made since the check are not in this export");
    }

    ExportJob *ej = calloc(1, sizeof(*ej));
    if (!ej) {
        export_finish(app, RX_ERROR(ENOMEM));
        return;
    }
    ej->lib = &priv->export_lib;
    ej->dest = rx_strdup(es->dest);
    if (priv->nb_export_playlists) {
        ej->playlists = calloc(priv->nb_export_playlists, sizeof(*ej->playlists));
        if (ej->playlists) {
            memcpy(ej->playlists, priv->export_playlists,
                   priv->nb_export_playlists * sizeof(*ej->playlists));
            ej->nb_playlists = priv->nb_export_playlists;
        }
    }
    if (!ej->dest || (priv->nb_export_playlists && !ej->playlists)) {
        export_job_free(ej);
        export_finish(app, RX_ERROR(ENOMEM));
        return;
    }
    ej->whole_library = es->whole_library;
    /* The dry run lists what pruning would delete, for the question. */
    ej->prune = dry_run || es->prune;
    ej->ignore_unsynced = es->ignore_unsynced || (dry_run && priv->export_probe);
    ej->compat = es->compat;
    ej->dry_run = dry_run;
    ej->mb = priv->export_mb;
    ej->cancel = &priv->export_cancel;

    /* The probe keeps what the first dry run found unsynced. */
    if (dry_run) {
        export_prune_list_free(app);
        if (!priv->export_probe)
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
        export_finish(app, RX_ERROR(ret));
        return;
    }
    priv->export_started = true;
    if (!priv->export_probe)
        export_log(app, "%s to %s", dry_run ? "Checking the export" : "Exporting", es->dest);
    rx_app_set_status(app, "%s to %s", dry_run ? "Checking the export" : "Exporting", es->dest);
}

/* The check that blocks an export over player changes not synced back
 * (rxbox/export.h) is recognised by its wording. */
static bool unsynced_check(const char *item)
{
    return rx_str_contains_fold(item, "unsynced") || rx_str_contains_fold(item, "not synced") ||
           rx_str_contains_fold(item, "sync first");
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

static void export_apply_msg(RXApp *app, const ExportMsg *m)
{
    RXAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    bool dry = priv->export_phase == EXPORT_DRY_RUN;

    es->done = m->done;
    es->total = m->total;
    if (strcmp(m->stage, priv->export_last_stage)) {
        snprintf(priv->export_last_stage, sizeof(priv->export_last_stage), "%s", m->stage);
        snprintf(es->stage, sizeof(es->stage), "%s", m->stage);
        if (!dry)
            export_log(app, "%s", m->stage);
    }
    snprintf(es->item, sizeof(es->item), "%s", m->item ? m->item : "");
    if (!m->item)
        return;

    /* The summary's counts. A step of a stage carries its total; a message
     * about one (export.c's reportf) carries none. */
    if (!dry && m->total > 0) {
        if (!strcmp(m->stage, "copy"))
            es->copied++;
        else if (!strcmp(m->stage, "skip"))
            es->skipped++;
        else if (!strcmp(m->stage, "prune") && es->prune)
            es->pruned++;
        else if (!strcmp(m->stage, "analysis") || !strcmp(m->stage, "artwork") ||
                 !strcmp(m->stage, "database"))
            es->rewritten++;
    } else if (!dry && !strcmp(m->stage, "prune") && rx_str_starts(m->item, "cannot remove")) {
        es->pruned--;
    }

    if (!strcmp(m->stage, "check")) {
        /* The checks name their problems in `item`. The probe's are the
         * first dry run's again, but for the unsynced changes, which it
         * passes ("overwriting ...") before anyone said so. */
        if (priv->export_probe)
            return;
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
                list[es->nb_prune] = rx_strdup(m->item);
                if (list[es->nb_prune])
                    es->nb_prune++;
            }
        } else if (!dry && (!path || es->prune)) {
            export_log(app, "  %s%s", path ? "deleted " : "", m->item);
        }
    }
}

/* A dry run ended: ask, run for real, or stop. */
static void export_after_dry_run(RXApp *app, int result)
{
    RXAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    export_join(priv);

    bool blocked = es->unsynced > 0 && !es->ignore_unsynced;
    bool probed = priv->export_probe;
    priv->export_probe = false;
    if (blocked && !probed) {
        /* Stopped at the checks, the unsynced one among them, before the
         * prune list: again past that one, so the question covers both
         * (and is not asked at all when another check fails too). */
        priv->export_probe = true;
        export_run(app, true);
        return;
    }
    /* Past the unsynced check, the export still cannot run: nothing to
     * ask. */
    if (result < 0) {
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
        rx_app_set_status(app, "Export to %s: waiting for confirmation", es->dest);
        return;
    }
    export_run(app, false);
}

static bool export_tick(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    if (priv->export_phase == EXPORT_IDLE)
        return false;

    if (es->cancel) {
        if (priv->export_phase == EXPORT_ANALYSING || priv->export_phase == EXPORT_CONFIRM) {
            export_finish(app, RX_ERROR(ECANCELED));
            return true;
        }
        rx_cancel(&priv->export_cancel);
    }

    switch (priv->export_phase) {
    case EXPORT_ANALYSING:
        es->done = app->analysing_done;
        es->total = app->analysing_total;
        if (priv->inflight.nb)
            return false;
        flush_saves(app);
        export_run(app, true);
        return true;
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
    while ((m = rx_mailbox_take(priv->export_mb))) {
        export_apply_msg(app, m);
        export_msg_free(m);
        changed = true;
    }
    if (finished) {
        int result = ej->result;
        /* A real run that got to the end despite a late cancel did export;
         * a dry run that did exported nothing, and the real one never
         * starts. */
        if (rx_cancelled(&priv->export_cancel)) {
            bool exported = result >= 0 && priv->export_phase == EXPORT_RUNNING;
            export_finish(app, exported ? result : RX_ERROR(ECANCELED));
        } else if (priv->export_phase == EXPORT_DRY_RUN) {
            export_after_dry_run(app, result);
        } else {
            export_finish(app, result);
        }
        changed = true;
    }
    return changed;
}

/* Stops a running export for good (rx_app_destroy). */
static void export_stop(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    rx_cancel(&priv->export_cancel);
    export_join(priv);
    export_snapshot_close(priv);
    priv->export_phase = EXPORT_IDLE;
    app->export_state.running = false;
    app->export_state.confirm = false;
    export_prune_list_free(app);
}

int rx_app_start_export(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    typeof(app->export_state) *es = &app->export_state;
    if (!priv)
        return RX_ERROR(EINVAL);

    /* The answer to the confirmation: the real run, which the dry run
     * (and the probe) found nothing else in the way of. */
    if (priv->export_phase == EXPORT_CONFIRM) {
        es->confirm = false;
        export_run(app, false);
        app->dirty = true;
        return 0;
    }
    if (priv->export_phase != EXPORT_IDLE) {
        rx_app_set_status(app, "An export is already running");
        return RX_ERROR(EBUSY);
    }
    if (!es->dest[0] || !rx_dir_exists(es->dest)) {
        rx_app_set_status(app, "%s is not a directory (mount the stick first)",
                          es->dest[0] ? es->dest : "The destination");
        return RX_ERROR(ENOTDIR);
    }

    es->running = true;
    es->confirm = false;
    es->cancel = false;
    es->result = 0;
    es->unsynced = 0;
    es->done = 0;
    es->total = 0;
    es->item[0] = '\0';
    es->log[0] = '\0';
    export_prune_list_free(app);
    priv->export_probe = false;

    /* The interface owns its selection array: keep the ids. */
    free(priv->export_playlists);
    priv->export_playlists = NULL;
    priv->nb_export_playlists = 0;
    if (!es->whole_library && es->nb_playlists > 0 && es->playlists) {
        priv->export_playlists = calloc(es->nb_playlists, sizeof(*priv->export_playlists));
        if (!priv->export_playlists) {
            es->running = false;
            return RX_ERROR(ENOMEM);
        }
        for (int i = 0; i < es->nb_playlists; i++) {
            if (es->playlists[i])
                priv->export_playlists[priv->nb_export_playlists++] = es->playlists[i]->id;
        }
    }

    /* What goes on the stick is analysed first, here, so the export's
     * handle finds every grid and waveform in the database: the chosen
     * playlists' tracks, every playlist's by default, or everything. */
    int queued = 0;
    if (es->whole_library) {
        for (int i = 0; i < app->lib.nb_tracks; i++) {
            RXTrack *t = app->lib.tracks[i];
            if (!t->missing && queue_analysis(app, t, RX_ANALYSE_ALL, false, false) > 0)
                queued++;
        }
    } else {
        int nb = es->nb_playlists;
        RXPlaylist **pls = es->playlists;
        RXPlaylist **all = NULL;
        if (!nb || !pls) {
            nb = rx_library_playlists_flat(&app->lib, NULL, 0);
            all = calloc(RX_MAX(nb, 1), sizeof(*all));
            nb = all ? rx_library_playlists_flat(&app->lib, all, nb) : 0;
            pls = all;
        }
        for (int i = 0; i < nb; i++) {
            const RXPlaylist *p = pls[i];
            for (int k = 0; p && k < p->nb; k++) {
                RXTrack *t = rx_library_find_id(&app->lib, p->track_ids[k]);
                if (t && !t->missing && queue_analysis(app, t, RX_ANALYSE_ALL, false, false) > 0)
                    queued++;
            }
        }
        free(all);
    }
    flush_saves(app);

    export_log(app, "Export to %s%s", es->dest,
               es->compat ? " with the compatibility transcode" : "");
    priv->export_phase = EXPORT_ANALYSING;
    snprintf(es->stage, sizeof(es->stage), "analysis");
    if (queued) {
        export_log(app, "Analysing %d track%s first", queued, queued == 1 ? "" : "s");
        rx_app_set_status(app, "Analysing before the export to %s", es->dest);
    }
    /* With nothing to wait for, the next tick starts the dry run. */
    app->dirty = true;
    return 0;
}

/* --------------------------------------------------------------------- rows */

typedef struct RowKey {
    uint32_t index;             /* in lib->tracks */
    int      pos;               /* natural order: playlist position or library order */
} RowKey;

typedef struct SortCtx {
    const RXLibrary *lib;
    int  column;
    bool descending;
} SortCtx;

typedef struct SortVal {
    bool        known;          /* unknown values sort last in both directions */
    double      num;
    const char *str;
} SortVal;

static const char *title_of(const RXTrack *t)
{
    return t->meta.title && t->meta.title[0] ? t->meta.title : rx_path_base(t->path);
}

/* Camelot order: 1A 1B 2A 2B ... 12B, which keeps compatible keys together. */
static int key_rank(const RXTrack *t)
{
    if (!t->key[0])
        return -1;
    char buf[8];
    const char *c = rx_key_to_camelot(t->key, buf, sizeof(buf));
    if (!c || !c[0])
        return -1;
    char *end;
    long n = strtol(c, &end, 10);
    if (n < 1 || n > 12)
        return -1;
    return (int)n * 2 + (*end == 'B' || *end == 'b');
}

static SortVal sort_value(const RXTrack *t, int column)
{
    SortVal v = { 0 };
    switch (column) {
    case RX_COL_TITLE:
        v.str = title_of(t);
        break;
    case RX_COL_ARTIST:
        v.str = t->meta.artist;
        break;
    case RX_COL_ALBUM:
        v.str = t->meta.album;
        break;
    case RX_COL_GENRE:
        v.str = t->meta.genre;
        break;
    case RX_COL_PATH:
        v.str = t->path;
        break;
    case RX_COL_BPM:
        v.num = rx_grid_bpm(&t->grid);
        v.known = v.num > 0;
        return v;
    case RX_COL_KEY:
        v.num = key_rank(t);
        v.known = v.num >= 0;
        return v;
    case RX_COL_TIME:
        v.num = t->duration;
        v.known = v.num > 0;
        return v;
    case RX_COL_RATING:
        v.num = t->meta.rating;
        v.known = true;
        return v;
    case RX_COL_COLOR:
        v.num = t->meta.color[0] ? rx_track_color_id(t->meta.color) : 0;
        v.known = v.num > 0;
        return v;
    case RX_COL_CUES: {
        int hot = 0;
        for (int i = 0; i < t->nb_cues; i++)
            hot += rx_cue_is_hot(&t->cues[i]);
        v.num = hot * 1000.0 + (t->nb_cues - hot);
        v.known = true;
        return v;
    }
    case RX_COL_YEAR:
        v.num = t->meta.year;
        v.known = v.num > 0;
        return v;
    case RX_COL_ADDED:
        v.num = (double)t->added;
        v.known = true;
        return v;
    case RX_COL_PLAYS:
        v.num = t->play_count;
        v.known = true;
        return v;
    case RX_COL_LAST_PLAYED:
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
            c = rx_strcasecmp(va.str ? va.str : "", vb.str ? vb.str : "");
        else
            c = (va.num > vb.num) - (va.num < vb.num);
        if (ctx->descending)
            c = -c;
        if (c)
            return c;
    }
    /* Stable: equal rows keep their natural order. */
    return (ra->pos > rb->pos) - (ra->pos < rb->pos);
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
    return field && field[0] && rx_str_contains_fold(field, word);
}

/* Every word must be found in one of the fields the table shows. */
static bool track_matches(const RXTrack *t, char **words, int nb_words)
{
    for (int i = 0; i < nb_words; i++) {
        const char *w = words[i];
        if (!field_matches(t->meta.title, w) && !field_matches(t->meta.artist, w) &&
            !field_matches(t->meta.album, w) && !field_matches(t->meta.genre, w) &&
            !field_matches(t->meta.comment, w) && !field_matches(rx_path_base(t->path), w))
            return false;
    }
    return true;
}

static bool playlist_exists(const RXLibrary *lib, const RXPlaylist *p)
{
    int nb = rx_library_playlists_flat(lib, NULL, 0);
    if (nb <= 0)
        return false;
    RXPlaylist **all = calloc(nb, sizeof(*all));
    if (!all)
        return true;
    nb = rx_library_playlists_flat(lib, all, nb);
    bool found = false;
    for (int i = 0; i < nb && !found; i++)
        found = all[i] == p;
    free(all);
    return found;
}

void rx_app_rebuild_rows(RXApp *app)
{
    RXLibrary *lib = &app->lib;

    /* The interface may have deleted the playlist it was showing. */
    if (app->current_playlist && !playlist_exists(lib, app->current_playlist))
        app->current_playlist = NULL;

    const RXPlaylist *pl = app->current_playlist;
    int cap = pl ? pl->nb : lib->nb_tracks;
    RowKey *keys = calloc(RX_MAX(cap, 1), sizeof(*keys));
    if (!keys) {
        rx_err("out of memory building the track table");
        return;
    }

    char buf[sizeof(app->search)];
    char *words[MAX_SEARCH_WORDS];
    int nb_words = search_words(app->search, buf, sizeof(buf), words);

    int nb = 0;
    for (int i = 0; i < cap; i++) {
        const RXTrack *t = pl ? rx_library_find_id(lib, pl->track_ids[i]) : lib->tracks[i];
        if (!t)
            continue;
        if (nb_words && !track_matches(t, words, nb_words))
            continue;
        keys[nb++] = (RowKey) {
            .index = pl ? (uint32_t)rx_library_index_of(lib, t) : (uint32_t)i,
            .pos   = i,
        };
    }

    SortCtx ctx = {
        .lib        = lib,
        .column     = app->sort_column,
        .descending = app->sort_descending,
    };
    if (ctx.column >= 0 && ctx.column < RX_COL_COUNT) {
        qsort_r(keys, nb, sizeof(*keys), compare_rows, &ctx);
    } else if (ctx.descending) {
        for (int i = 0; i < nb / 2; i++) {
            RowKey tmp = keys[i];
            keys[i] = keys[nb - 1 - i];
            keys[nb - 1 - i] = tmp;
        }
    }

    uint32_t *rows = realloc(app->rows, RX_MAX(nb, 1) * sizeof(*rows));
    if (!rows) {
        free(keys);
        rx_err("out of memory building the track table");
        return;
    }
    for (int i = 0; i < nb; i++)
        rows[i] = keys[i].index;
    free(keys);

    app->rows = rows;
    app->nb_rows = nb;
    app->rows_generation = lib->generation;
    app->rows_dirty = false;
    app->dirty = true;
}

/* ---------------------------------------------------------------- lifecycle */

int rx_app_create(RXApp *app, const char *library_root, const RXPlayerOptions *player_opt,
                  const RXWindowOptions *window_opt)
{
    memset(app, 0, sizeof(*app));
    RXAppPriv *priv = calloc(1, sizeof(*priv));
    if (!priv)
        return RX_ERROR(ENOMEM);
    app->priv = priv;
    rx_array_init(&priv->inflight, sizeof(Inflight));
    rx_array_init(&priv->saves, sizeof(uint64_t));
    rx_array_init(&priv->gpu, sizeof(GpuEntry));
    rx_array_init(&priv->wave_checks, sizeof(WaveCheck));
    for (int d = 0; d < RX_DECKS; d++) {
        priv->uploaded_palette[d] = -1;
        priv->play_last_pos[d] = -1.0;
    }

    int ret = rx_mailbox_create(&priv->export_mb);
    if (ret < 0)
        goto fail;

    /* Opening also checks which files went missing. */
    ret = rx_library_open(&app->lib, library_root);
    if (ret < 0) {
        rx_err("cannot open the library%s%s: %s", library_root ? " at " : "",
               library_root ? library_root : "", strerror(-ret));
        goto fail;
    }
    priv->lib_open = true;
    const RXConfig *cfg = &app->lib.cfg;

    /* One core stays with the interface and the audio. */
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    ret = rx_jobs_create(&app->jobs, (int)RX_MAX(cpus - 1, 1));
    if (ret < 0)
        goto fail;
    ret = rx_jobs_create(&priv->io_jobs, IO_THREADS);
    if (ret < 0)
        goto fail;

    RXPlayerOptions po;
    if (player_opt) {
        po = *player_opt;
    } else {
        rx_player_options_default(&po);
        po.backend = RX_AUDIO_NULL;
    }
    if (!po.sink && po.backend == RX_AUDIO_PIPEWIRE)
        po.sink = cfg->audio_sink;
    if (!po.app_name)
        po.app_name = "rxbox";
    ret = rx_player_create(&app->player, &po);
    if (ret < 0 && po.backend != RX_AUDIO_NULL) {
        rx_warn("no audio output (%s); the player plays silently", strerror(-ret));
        po.backend = RX_AUDIO_NULL;
        ret = rx_player_create(&app->player, &po);
    }
    if (ret < 0)
        goto fail;

    for (int d = 0; d < RX_DECKS; d++) {
        RXDeckView *dv = &app->decks[d];
        rx_ctl_init(&dv->ctl, app->player, d, deck_changed, app);
        dv->zoom_bars = cfg->detail_bars > 0 ? cfg->detail_bars : 12;
    }
    app->wave_palette = palette_of(cfg);
    app->export_state.compat = cfg->compat_transcode;
    app->sort_column = -1;
    app->rows_dirty = true;

    if (window_opt) {
        priv->max_frames = window_opt->max_frames;
        ret = rx_window_create(&app->window, window_opt);
        if (ret < 0)
            goto fail;
        ret = rx_ui_init(app);
        if (ret < 0)
            goto fail;
        priv->ui_ready = true;
    }

    rx_app_rebuild_rows(app);

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
        rx_app_set_status(app, "%d tracks in %s, %d missing%s", app->lib.nb_tracks, app->lib.root,
                          missing, again);
    else
        rx_app_set_status(app, "%d track%s in %s%s", app->lib.nb_tracks,
                          app->lib.nb_tracks == 1 ? "" : "s", app->lib.root, again);
    return 0;

fail:
    rx_app_destroy(app);
    return ret;
}

void rx_app_destroy(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    if (!priv)
        return;

    /* The export has a library handle of its own; cancel it and wait. */
    export_stop(app);

    /* The interface goes before anything it draws from. */
    if (priv->ui_ready) {
        rx_ui_shutdown(app);
        priv->ui_ready = false;
    }

    /* Running jobs finish, queued ones are dropped, results freed. */
    rx_jobs_destroy(app->jobs);
    app->jobs = NULL;
    rx_jobs_destroy(priv->io_jobs);
    priv->io_jobs = NULL;
    rx_job_result_free(app->stick_result);
    app->stick_result = NULL;
    app->stick_request = 0;

    for (int d = 0; d < RX_DECKS; d++)
        deck_clear(app, d);
    gpu_cache_free(app);

    if (priv->lib_open)
        flush_saves(app);

    /* The player before the library: the deck controls point at tracks. */
    if (app->player)
        rx_player_destroy(app->player);
    app->player = NULL;

    /* The window after every texture and waveform made through it. */
    if (app->window)
        rx_window_destroy(app->window);
    app->window = NULL;

    free(app->rows);
    app->rows = NULL;
    app->nb_rows = 0;
    free(app->selection);
    app->selection = NULL;
    app->nb_selection = 0;
    app->current_playlist = NULL;
    app->current_folder = NULL;

    if (priv->lib_open)
        rx_library_close(&app->lib);

    rx_array_free(&priv->inflight);
    rx_array_free(&priv->saves);
    rx_array_free(&priv->wave_checks);
    free(priv->export_playlists);
    if (priv->export_mb)
        rx_mailbox_destroy(priv->export_mb);
    free(priv);
    app->priv = NULL;
}

static int drain_jobs(RXApp *app, RXJobs *jobs, int max)
{
    int n = 0;
    RXJobResult *r;
    while (n < max && (r = rx_jobs_poll(jobs))) {
        handle_result(app, r);
        rx_job_result_free(r);
        n++;
    }
    return n;
}

bool rx_app_tick(RXApp *app)
{
    RXAppPriv *priv = app->priv;
    if (!priv)
        return false;

    priv->tick++;
    priv->gpu_budget = GPU_LOADS_PER_TICK;
    bool changed = false;

    /* A track removed from the library while loaded: the deck lets go of
     * it before anything (a job result among them) looks at it. */
    for (int d = 0; d < RX_DECKS; d++) {
        RXDeckView *dv = &app->decks[d];
        if (dv->track && rx_library_find_id(&app->lib, priv->deck_track_id[d]) != dv->track) {
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

    for (int d = 0; d < RX_DECKS; d++) {
        RXDeckView *dv = &app->decks[d];
        if (app->player)
            rx_ctl_tick(&dv->ctl);
        track_plays(app, d);

        if (dv->track && priv->uploaded_palette[d] != app->wave_palette) {
            deck_upload_waves(app, d);
            changed = true;
        }
    }

    if (flush_saves(app))
        changed = true;

    if (app->rows_dirty || app->rows_generation != app->lib.generation) {
        rx_app_rebuild_rows(app);
        changed = true;
    }

    if (changed)
        app->dirty = true;
    return changed;
}

static bool deck_moving(const RXApp *app)
{
    if (!app->player)
        return false;
    for (int d = 0; d < RX_DECKS; d++) {
        RXDeckState st;
        rx_deck_state(app->player, d, &st);
        if (st.playing || st.scrubbing)
            return true;
    }
    return false;
}

int rx_app_run(RXApp *app)
{
    if (!app->window || !app->priv)
        return RX_ERROR(EINVAL);

    RXAppPriv *priv = app->priv;
    bool ui_wants = true;
    app->dirty = true;

    while (!app->quit) {
        rx_app_tick(app);

        /* An export waiting for the answer to its question is not busy. */
        bool busy = rx_jobs_pending(app->jobs) > 0 || rx_jobs_pending(priv->io_jobs) > 0 ||
                    (app->export_state.running && !app->export_state.confirm);
        bool want = app->dirty || ui_wants || deck_moving(app) || busy;
        /* --frames: draw continuously so the count is reached. */
        if (priv->max_frames > 0)
            want = true;

        if (rx_window_poll(app->window, want, want ? 4 : 50)) {
            app->dirty = false;
            rx_window_begin_frame(app->window);
            ui_wants = rx_ui_frame(app);
            rx_window_end_frame(app->window);
        }

        if (rx_window_should_close(app->window))
            app->quit = true;
        if (priv->max_frames > 0 && rx_window_frames_drawn(app->window) >= priv->max_frames)
            app->quit = true;
    }

    if (app->export_state.running) {
        app->export_state.cancel = true;
        export_tick(app);
    }
    flush_saves(app);
    return 0;
}
