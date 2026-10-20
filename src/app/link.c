/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PRO DJ LINK in the app (cyanbox/app.h, cb_app_link_start): a set of
 * services (cyanbox/link.h CBLinkSet), one on every Ethernet interface,
 * started with the app when it runs with --pro-link and serving from then
 * on; the set's own thread starts and stops them as interfaces come and
 * go, so the interface never waits for that. Each tick copies the set's
 * state into app->link_state, counts plays from the players' status, and
 * tells the services when the library has changed, once it has settled.
 *
 * What the players send back is applied here, on the owner thread: the
 * services never write the library. That is the plays, and the cues a
 * player saves on the deck or deletes and the ratings it gives, which the
 * services have served since and hand over (cb_link_set_edits).
 *
 * The deck's output follows the mixer: while a mixer is heard on the link
 * (its LINK CUE timing) the deck is heard only in its headphone cue, the
 * player on its ring backend, whose audio the services take as the mixer
 * asks (cb_player_ring_take); each tick tells them whether it plays. With
 * no mixer, the deck plays where it would without Link (PipeWire, or what
 * --audio said), as the user asked: "If no mixer is detected, can you make
 * the audio output on --pro-link using pipewire as usual?" */

#include "applink.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cyanbox/common.h"

/* The library is read again this long after its last change, or this
 * long after the first of a run of changes that does not settle (an
 * analysis queue): reading it is a plan over every track. */
#define RELOAD_QUIET_US   2000000
#define RELOAD_LONGEST_US 30000000

/* The deck goes back to its usual output this long after the last mixer
 * was heard (on top of the second the cue's `mixer` lasts), so that a gap
 * in a mixer's timing does not move it; an output that would not open is
 * tried again this long after. */
#define OUTPUT_HOLD_US  2000000
#define OUTPUT_RETRY_US 10000000

/* A player's count, by the interface it is on and its number there
 * (numbers repeat from one network to another). */
typedef struct Counter {
    char interface[16];
    int  number;                /* 0 a free slot */
    bool used;                  /* seen in this tick's devices */
    CBLinkPlays plays;
    CBLinkRun run;
} Counter;

typedef struct CBAppLink {
    CBLinkSet *set;
    CBLinkTagList *tags;        /* the TAG LIST, for as long as the app runs */
    CBLinkCue *cue;             /* LINK CUE, with a player; as long as the app runs */
    bool cue_playing;           /* the deck played at the last tick */
    /* The deck's output with no mixer heard: the player's when Link first
     * started; the ring's while one is. */
    bool usual_known;
    enum CBAudioBackend usual;
    char *usual_sink;           /* NULL the default */
    int64_t mixer_us;           /* cb_time_us a mixer was last heard, 0 never */
    int64_t output_failed_us;   /* when moving the output last failed, 0 never */
    unsigned tags_generation;   /* the one the rows were made for */
    unsigned generation;        /* lib.generation the services read last */
    unsigned seen_generation;   /* the last one seen, since changed_us */
    int64_t changed_us, first_change_us;
    Counter counters[CB_APP_LINK_DEVICES];
} CBAppLink;

/* ---------------------------------------------------------------- plays */

void cb_link_plays_reset(CBLinkPlays *p)
{
    memset(p, 0, sizeof(*p));
    p->last_beat = -1;
}

bool cb_link_plays_feed(CBLinkPlays *p, const CBLinkPlayer *pl, int ours, int64_t now_us)
{
    /* A rekordbox track (type 1) of ours: our device number is where the
     * player says it is from, whichever slot it names (a CDJ-3000 on
     * EP122 says USB for a computer's: link.h). */
    bool loaded = ours > 0 && pl->type == CB_LINK_DEVICE_PLAYER && pl->has_status &&
                  pl->track_device == ours && pl->track_type == 1 && pl->track_id;
    if (!loaded) {
        if (p->track_id || p->last_us)
            cb_link_plays_reset(p);
        return false;
    }
    if (pl->track_id != p->track_id) {
        cb_link_plays_reset(p);
        p->track_id = pl->track_id;
    }

    int64_t last_us = p->last_us;
    int last_beat = p->last_beat;
    p->last_us = now_us;
    p->last_beat = pl->beat;
    double since = (double)(now_us - last_us) * 1e-6;
    if (p->recorded || !pl->playing || !last_us || !(since > 0))
        return false;

    double speed = CB_MAX(1.0 + pl->pitch / 100.0, 0.0);
    if (pl->beat >= 1 && pl->bpm > 0) {
        /* Beats played forward since the last look, no more than its
         * tempo allows in the time between (and a beat or two of slack:
         * status packets come every 200 ms, the looks at any time). */
        int step = pl->beat - last_beat;
        double allowed = since * pl->bpm / 60.0 * CB_MAX(speed, 1.0) * 1.25 + 2.0;
        if (last_beat < 1 || step <= 0 || step > allowed)
            return false;
        for (int b = last_beat + 1; b <= pl->beat; b++) {
            int bin = CB_MIN(b, CB_LINK_PLAY_BEATS - 1);
            uint8_t bit = (uint8_t)(1u << (bin & 7));
            if (p->seen[bin >> 3] & bit)
                continue;
            p->seen[bin >> 3] |= bit;
            p->seconds += 60.0 / pl->bpm;
        }
    } else {
        /* No beat numbers (no grid): the time it played, at its pitch; a
         * look long after the last (the interface held up) counts a
         * second at most. */
        p->seconds += CB_MIN(since, 1.0) * speed;
    }
    if (p->seconds < CB_LINK_PLAY_SECONDS)
        return false;
    p->recorded = true;
    return true;
}
/* -------------------------------------------------------------- history */

void cb_link_run_reset(CBLinkRun *r)
{
    *r = (CBLinkRun) { .last_beat = -1 };
}

bool cb_link_run_feed(CBLinkRun *r, const CBLinkPlayer *pl, int ours, int64_t now_us,
                      int64_t now_at)
{
    bool loaded = ours > 0 && pl->type == CB_LINK_DEVICE_PLAYER && pl->has_status &&
                  pl->track_device == ours && pl->track_type == 1 && pl->track_id;
    if (!loaded) {
        if (r->track_id)
            cb_link_run_reset(r);
        return false;
    }
    if (pl->track_id != r->track_id) {
        cb_link_run_reset(r);
        r->track_id = pl->track_id;
    }
    int64_t last_us = r->last_us;
    int last_beat = r->last_beat;
    r->last_us = now_us;
    r->last_beat = pl->beat;
    if (r->recorded)
        return false;
    if (!pl->playing) {
        r->start_us = 0;
        return false;
    }
    /* Straight on since the last look: its beats moved forward no more
     * than its tempo plays in the time between (and a beat or two of
     * slack, as cb_link_plays_feed allows). */
    bool straight = r->start_us != 0;
    if (straight && pl->beat >= 1 && last_beat >= 1 && pl->bpm > 0) {
        double since = (double)(now_us - last_us) * 1e-6;
        double speed = CB_MAX(1.0 + pl->pitch / 100.0, 1.0);
        int step = pl->beat - last_beat;
        straight = step >= 0 && step <= since * pl->bpm / 60.0 * speed * 1.25 + 2.0;
    }
    if (!straight) {
        r->start_us = now_us;
        r->start_at = now_at;
        return false;
    }
    if ((double)(now_us - r->start_us) * 1e-6 < CB_LINK_HISTORY_SECONDS)
        return false;
    r->recorded = true;
    return true;
}

/* -------------------------------------------------------- start and stop */

static void state_clear(CBApp *app)
{
    app->link_state.on = false;
    app->link_state.nb_interfaces = 0;
    app->link_state.nb_devices = 0;
    app->link_state.cue_enabled = false;
    app->link_state.cue = (CBLinkCueState) { 0 };
}

static int ring_take(void *opaque, int16_t *out, int frames)
{
    return cb_player_ring_take(opaque, out, frames);
}

int cb_app_link_start(CBApp *app, const CBLinkOptions *o)
{
    if (!app->link_state.enabled)
        return CB_ERROR(EPERM);
    if (app->link_state.on)
        return CB_ERROR(EALREADY);
    if (!app->link && !(app->link = calloc(1, sizeof(*app->link))))
        return CB_ERROR(ENOMEM);
    CBAppLink *l = app->link;
    CBLinkOptions opt;
    if (o)
        opt = *o;
    else
        cb_link_options_default(&opt);
    if (!opt.library)
        opt.library = app->lib.root;
    if (!opt.tag_list && !(opt.tag_list = cb_app_tag_list(app)))
        return CB_ERROR(ENOMEM);
    /* The deck heard in a mixer's LINK CUE while one is on the link, and
     * where it is usually heard otherwise. */
    if (app->player && !l->usual_known) {
        const char *sink = cb_player_sink(app->player);
        if (sink && !(l->usual_sink = cb_strdup(sink)))
            return CB_ERROR(ENOMEM);
        l->usual = cb_player_backend(app->player);
        l->usual_known = true;
    }
    if (!opt.cue && app->player) {
        if (!l->cue && cb_link_cue_new(&l->cue, ring_take, app->player) < 0)
            return CB_ERROR(ENOMEM);
        opt.cue = l->cue;
    }
    /* The library's handle tag: a deck reads on after cyanbox served again. */
    int ret;
    if (!opt.nfs_tag && (ret = cb_library_link_tag(&app->lib, &opt.nfs_tag)) < 0)
        cb_warn("link: no handle tag in the library (%s): a deck's handles go stale with a "
                "restart", strerror(-ret));
    ret = cb_link_set_start(&l->set, &opt);
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "PRO DJ LINK cannot start: %s", strerror(-ret));
        return ret;
    }
    *l = (CBAppLink) {
        .set = l->set,
        .tags = l->tags,
        .cue = l->cue,
        .usual_known = l->usual_known,
        .usual = l->usual,
        .usual_sink = l->usual_sink,
        .tags_generation = l->tags_generation,
        .generation = app->lib.generation,
        .seen_generation = app->lib.generation,
    };
    /* What changed before is in the library the services read first. */
    uint64_t *ids = NULL;
    int nb = 0;
    cb_library_take_changes(&app->lib, &ids, &nb);
    free(ids);
    state_clear(app);
    app->link_state.on = true;
    app->link_state.cue_enabled = l->cue && opt.cue == l->cue;
    app->dirty = true;
    return 0;
}

int cb_app_link_load(CBApp *app, int index, uint64_t track_id)
{
    CBAppLink *l = app->link;
    if (!l || !l->set || !app->link_state.on || index < 0 || index >= app->link_state.nb_devices ||
        track_id > UINT32_MAX)
        return CB_ERROR(ENOENT);
    const CBLinkPlayer *p = &app->link_state.devices[index];
    return cb_link_set_load(l->set, app->link_state.device_if[index], p->number, (uint32_t)track_id);
}

int cb_app_link_push_settings(CBApp *app, int *players, int *mixers)
{
    *players = *mixers = 0;
    CBAppLink *l = app->link;
    int ret = l && l->set && app->link_state.on
            ? cb_link_set_push_settings(l->set, &app->lib.cfg, players, mixers)
            : CB_ERROR(ENOENT);
    if (ret == CB_ERROR(EAGAIN) || ret == CB_ERROR(ENOENT)) {
        cb_app_set_status(app, "Push settings: not on the link yet");
        return ret;
    }
    if (ret < 0) {
        cb_app_set_status(app, "Push settings: the My Settings are not valid (`cyanbox settings`)");
        return ret;
    }
    /* A player that plays drops them unanswered (the emulated CDJ-3000X
     * and NXS2, 2026-10-19): only its status says so. */
    int playing = 0;
    for (int i = 0; i < app->link_state.nb_devices; i++)
        playing += app->link_state.devices[i].type == CB_LINK_DEVICE_PLAYER &&
                   app->link_state.devices[i].playing;
    char also[64] = "";
    if (playing)
        snprintf(also, sizeof(also), "; %d playing ignore%s them", CB_MIN(playing, *players),
                 CB_MIN(playing, *players) == 1 ? "s" : "");
    cb_app_set_status(app, "Settings pushed to %d player%s and %d mixer%s%s", *players,
                      *players == 1 ? "" : "s", *mixers, *mixers == 1 ? "" : "s", also);
    return 0;
}

static void take_edits(CBApp *app);

/* Moves the deck to `want` (cyanbox/player.h cb_player_set_backend), with
 * a word in the status bar. True when it moved or tried to. */
static bool set_output(CBApp *app, enum CBAudioBackend want, const char *mixer, int64_t now)
{
    CBAppLink *l = app->link;
    if (cb_player_backend(app->player) == want ||
        (l->output_failed_us && now - l->output_failed_us < OUTPUT_RETRY_US))
        return false;
    int ret = cb_player_set_backend(app->player, want,
                                    want == CB_AUDIO_PIPEWIRE ? l->usual_sink : NULL);
    l->output_failed_us = ret < 0 ? now : 0;
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "The deck's audio cannot move: %s", strerror(-ret));
    } else if (want == CB_AUDIO_RING) {
        cb_app_set_status(app, "%s on the link: the deck is heard in its LINK CUE", mixer);
    } else {
        cb_app_set_status(app, "No mixer on the link: the deck plays %s",
                          want == CB_AUDIO_PIPEWIRE ? "through PipeWire" : "silently");
    }
    app->dirty = true;
    return true;
}

static void link_stop(CBApp *app, bool restore)
{
    CBAppLink *l = app->link;
    if (!l || !l->set)
        return;
    take_edits(app);
    cb_link_set_stop(&l->set);
    for (int i = 0; i < CB_APP_LINK_DEVICES; i++)
        l->counters[i].number = 0;
    state_clear(app);
    /* No cue any more: the deck where it is usually heard. */
    l->mixer_us = 0;
    if (restore && app->player && l->usual_known)
        set_output(app, l->usual, NULL, cb_time_us());
    app->dirty = true;
}

void cb_app_link_stop(CBApp *app)
{
    link_stop(app, true);
}

/* ------------------------------------------------------------------ tick */

static Counter *counter_of(CBAppLink *l, const char *interface, int number)
{
    Counter *free_slot = NULL;
    for (int i = 0; i < CB_APP_LINK_DEVICES; i++) {
        Counter *c = &l->counters[i];
        if (c->number == number && !strcmp(c->interface, interface))
            return c;
        if (!c->number && !free_slot)
            free_slot = c;
    }
    /* Never NULL: there are as many slots as devices kept. */
    free_slot->number = number;
    snprintf(free_slot->interface, sizeof(free_slot->interface), "%s", interface);
    cb_link_plays_reset(&free_slot->plays);
    cb_link_run_reset(&free_slot->run);
    return free_slot;
}

static void record(CBApp *app, const CBLinkPlayer *p)
{
    CBTrack *t = cb_library_find_id(&app->lib, p->track_id);
    if (!t)
        return;
    char name[256];
    cb_track_display(t, name, sizeof(name));
    int ret = cb_library_record_play(&app->lib, t);
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot record the play of %s on player %d: %s", name, p->number,
                          strerror(-ret));
        return;
    }
    cb_app_set_status(app, "Played on player %d: %s", p->number, name);
    app->rows_dirty = true;
}

/* A cue a player saved on the deck or deleted, or a rating it gave,
 * into the library. */
static void apply_edit(CBApp *app, const CBLinkEdit *e)
{
    CBTrack *t = cb_library_find_id(&app->lib, e->track_id);
    if (!t)
        return;
    char name[256], what[16];
    cb_track_display(t, name, sizeof(name));
    if (e->what == CB_LINK_EDIT_RATING) {
        int ret = cb_link_edit_apply(t, e);
        if (ret > 0)
            ret = cb_library_save_track(&app->lib, t);
        if (ret < 0) {
            app->last_error_count++;
            cb_app_set_status(app, "Cannot keep the rating player %d gave %s: %s", e->player, name,
                              strerror(-ret));
            return;
        }
        cb_app_set_status(app, "Player %d rated %s %d star%s", e->player, name, e->rating,
                          e->rating == 1 ? "" : "s");
        app->rows_dirty = true;
        app->dirty = true;
        return;
    }
    if (e->cue.hot >= 0)
        snprintf(what, sizeof(what), "hot cue %c", cb_cue_letter(&e->cue));
    else
        snprintf(what, sizeof(what), "a memory %s", cb_cue_is_loop(&e->cue) ? "loop" : "cue");
    int ret = cb_link_edit_apply(t, e);
    if (ret > 0)
        ret = cb_library_save_track(&app->lib, t);
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot keep %s player %d %s on %s: %s", what, e->player,
                          e->remove ? "deleted" : "saved", name, strerror(-ret));
        return;
    }
    cb_app_set_status(app, "Player %d %s %s on %s", e->player, e->remove ? "deleted" : "saved",
                      what, name);
    app->rows_dirty = true;
    app->dirty = true;
}

static void take_edits(CBApp *app)
{
    CBLinkEdit e[16];
    int n;
    do {
        n = cb_link_set_edits(app->link->set, e, 16);
        for (int i = 0; i < n; i++)
            apply_edit(app, &e[i]);
    } while (n == 16);
}

/* A load a player has played straight on for 1.5 minutes, on the History
 * from when it started. */
static void to_history(CBApp *app, const CBLinkPlayer *p, int64_t at)
{
    CBTrack *t = cb_library_find_id(&app->lib, p->track_id);
    if (!t)
        return;
    char name[256];
    cb_track_display(t, name, sizeof(name));
    int ret = cb_library_history_add(&app->lib, t, at, p->number);
    if (ret < 0) {
        app->last_error_count++;
        cb_app_set_status(app, "Cannot put %s on the History: %s", name, strerror(-ret));
        return;
    }
    cb_app_set_status(app, "On the History from player %d: %s", p->number, name);
    app->rows_dirty = true;
}

/* What the window shows of a device. */
static bool shown_same(const CBLinkPlayer *a, const CBLinkPlayer *b)
{
    return a->number == b->number && a->type == b->type && a->addr == b->addr &&
           !strcmp(a->name, b->name) && a->has_status == b->has_status &&
           a->track_device == b->track_device && a->track_slot == b->track_slot &&
           a->track_id == b->track_id && a->playing == b->playing && a->on_air == b->on_air &&
           a->master == b->master && a->synced == b->synced && a->bpm == b->bpm &&
           a->pitch == b->pitch;
}

static bool served_same(const CBLinkServed *a, const CBLinkServed *b)
{
    return !strcmp(a->interface, b->interface) && a->addr == b->addr &&
           a->running == b->running && a->err == b->err && a->state == b->state &&
           a->number == b->number;
}

/* The set's state into link_state, plays counted on the way; true when
 * what is shown changed. */
static bool follow(CBApp *app, int64_t now)
{
    CBAppLink *l = app->link;
    CBLinkServed ifs[CB_APP_LINK_IFS];
    int nb_ifs = CB_MIN(cb_link_set_interfaces(l->set, ifs, CB_APP_LINK_IFS), CB_APP_LINK_IFS);
    CBLinkPlayer dev[CB_APP_LINK_DEVICES];
    int where[CB_APP_LINK_DEVICES];
    int nb = CB_MIN(cb_link_set_players(l->set, dev, where, CB_APP_LINK_DEVICES),
                    CB_APP_LINK_DEVICES);

    double played[CB_APP_LINK_DEVICES];
    bool counted[CB_APP_LINK_DEVICES];
    for (int i = 0; i < CB_APP_LINK_DEVICES; i++)
        l->counters[i].used = false;
    for (int i = 0; i < nb; i++) {
        /* Our number on the device's own network: its loads of a track
         * of ours name that number. */
        const CBLinkServed *s = where[i] < nb_ifs ? &ifs[where[i]] : NULL;
        int ours = s && s->running && s->state == CB_LINK_UP ? s->number : 0;
        Counter *c = counter_of(l, s ? s->interface : "", dev[i].number);
        c->used = true;
        if (cb_link_plays_feed(&c->plays, &dev[i], ours, now))
            record(app, &dev[i]);
        if (cb_link_run_feed(&c->run, &dev[i], ours, now, (int64_t)time(NULL)))
            to_history(app, &dev[i], c->run.start_at);
        played[i] = c->plays.track_id ? c->plays.seconds : 0.0;
        counted[i] = c->plays.track_id && c->plays.recorded;
    }
    /* A device gone from the link: a load of its own when it is back. */
    for (int i = 0; i < CB_APP_LINK_DEVICES; i++) {
        if (!l->counters[i].used)
            l->counters[i].number = 0;
    }

    typeof(app->link_state) *ls = &app->link_state;
    bool same = nb_ifs == ls->nb_interfaces && nb == ls->nb_devices;
    for (int i = 0; i < nb_ifs && same; i++)
        same = served_same(&ifs[i], &ls->interfaces[i]);
    for (int i = 0; i < nb && same; i++)
        same = shown_same(&dev[i], &ls->devices[i]) && where[i] == ls->device_if[i] &&
               counted[i] == ls->counted[i] && (int)played[i] == (int)ls->played[i];
    ls->nb_interfaces = nb_ifs;
    memcpy(ls->interfaces, ifs, (size_t)nb_ifs * sizeof(*ifs));
    ls->nb_devices = nb;
    memcpy(ls->devices, dev, (size_t)nb * sizeof(*dev));
    memcpy(ls->device_if, where, (size_t)nb * sizeof(*where));
    memcpy(ls->played, played, (size_t)nb * sizeof(*played));
    memcpy(ls->counted, counted, (size_t)nb * sizeof(*counted));
    return !same;
}

/* The library read again once its changes have settled: every change
 * bumps lib.generation (an analysis applied, a cue set, a play). Only the
 * tracks changed are, when the library's record says which
 * (cb_library_take_changes); the whole of it when anything else did. */
static void follow_library(CBApp *app, int64_t now)
{
    CBAppLink *l = app->link;
    unsigned gen = app->lib.generation;
    if (gen == l->generation)
        return;
    if (gen != l->seen_generation) {
        l->seen_generation = gen;
        l->changed_us = now;
        if (!l->first_change_us)
            l->first_change_us = now;
    }
    if (now - l->changed_us < RELOAD_QUIET_US && now - l->first_change_us < RELOAD_LONGEST_US)
        return;
    uint64_t *ids = NULL;
    int nb = 0;
    if (cb_library_take_changes(&app->lib, &ids, &nb) != 0)
        cb_link_set_library_changed(l->set);
    else if (nb)
        cb_link_set_tracks_changed(l->set, ids, nb);
    free(ids);
    l->generation = gen;
    l->first_change_us = 0;
}

CBLinkTagList *cb_app_tag_list(CBApp *app)
{
    if (!app->link_state.enabled)
        return NULL;
    if (!app->link && !(app->link = calloc(1, sizeof(*app->link))))
        return NULL;
    if (!app->link->tags && cb_link_tag_list_new(&app->link->tags) < 0)
        return NULL;
    return app->link->tags;
}

/* A change to the TAG LIST, from a player or here: its view made again. */
static bool follow_tags(CBApp *app)
{
    CBAppLink *l = app->link;
    unsigned gen = l->tags ? cb_link_tag_list_generation(l->tags) : 0;
    if (gen == l->tags_generation)
        return false;
    l->tags_generation = gen;
    if (app->current_tag_list)
        app->rows_dirty = true;
    return true;
}

static bool cue_same(const CBLinkCueState *a, const CBLinkCueState *b)
{
    return a->mixer == b->mixer && a->number == b->number && a->on == b->on &&
           a->source == b->source && a->ours == b->ours && a->sending == b->sending &&
           !strcmp(a->name, b->name);
}

/* LINK CUE: the deck told the services (playing, scrubbing, and a start,
 * which asks the mixer for the cue), and where it stands copied. True when
 * that changed. */
static bool follow_cue(CBApp *app)
{
    CBAppLink *l = app->link;
    if (!app->link_state.cue_enabled)
        return false;
    CBDeckState s;
    cb_deck_state(app->player, 0, &s);
    bool playing = s.loaded && (s.playing || s.scrubbing);
    cb_link_cue_decks(l->cue, playing, playing && !l->cue_playing);
    l->cue_playing = playing;
    CBLinkCueState st;
    cb_link_cue_state(l->cue, &st);
    bool changed = !cue_same(&st, &app->link_state.cue);
    app->link_state.cue = st;
    return changed;
}

/* The deck's output: a mixer's LINK CUE while one is heard, OUTPUT_HOLD_US
 * on, else the usual one. True when it moved. */
static bool follow_output(CBApp *app, int64_t now)
{
    CBAppLink *l = app->link;
    if (!app->link_state.cue_enabled || !app->player || !l->usual_known)
        return false;
    if (app->link_state.cue.mixer)
        l->mixer_us = now;
    bool cue = l->mixer_us && now - l->mixer_us < OUTPUT_HOLD_US;
    return set_output(app, cue ? CB_AUDIO_RING : l->usual, app->link_state.cue.name, now);
}

bool cb_app_link_tick(CBApp *app)
{
    CBAppLink *l = app->link;
    if (l && follow_tags(app))
        app->dirty = true;
    if (!l || !l->set)
        return false;
    int64_t now = cb_time_us();
    bool changed = follow(app, now);
    changed |= follow_cue(app);
    changed |= follow_output(app, now);
    take_edits(app);
    follow_library(app, now);
    return changed;
}

void cb_app_link_destroy(CBApp *app)
{
    link_stop(app, false);
    if (app->link) {
        cb_link_tag_list_free(&app->link->tags);
        cb_link_cue_free(&app->link->cue);
        free(app->link->usual_sink);
    }
    free(app->link);
    app->link = NULL;
}
