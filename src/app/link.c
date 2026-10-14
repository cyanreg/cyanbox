/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PRO DJ LINK in the app (rxbox/app.h, rx_app_link_start): the service of
 * rxbox/link.h started and stopped on a thread of its own, since a start
 * reads the library and builds the export plan over all of it, and a stop
 * waits for a request in progress (a transcode takes seconds): the
 * interface waits for neither. Each tick copies the service's state into
 * app->link_state, counts plays from the players' status, and tells the
 * service when the library has changed, once it has settled.
 *
 * Only with --pro-link (link_state.enabled), and on only while the switch
 * is, which is off at every start (docs/LINK.md, "Security"). What the
 * players send back is applied here, on the owner thread: the service
 * never writes the library. */

#include "applink.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

/* The library is read again this long after its last change, or this
 * long after the first of a run of changes that does not settle (an
 * analysis queue): reading it is a plan over every track. */
#define RELOAD_QUIET_US   2000000
#define RELOAD_LONGEST_US 30000000

typedef struct Counter {
    int number;                 /* the device's; 0 a free slot */
    bool used;                  /* seen in this tick's devices */
    RXLinkPlays plays;
} Counter;

typedef struct RXAppLink {
    RXLink *link;
    /* The start or stop in progress: the thread, what it was asked and
     * the start's result, which it writes before setting `done`. */
    pthread_t thread;
    bool started;
    bool stopping;
    bool stop_after;            /* switched off while starting: stop when done */
    atomic_bool done;
    int result;
    RXLinkOptions opt;
    char *library, *interface, *name, *broadcast;

    unsigned generation;        /* lib.generation the service read last */
    unsigned seen_generation;   /* the last one seen, since changed_us */
    int64_t changed_us, first_change_us;

    Counter counters[RX_APP_LINK_DEVICES];
} RXAppLink;

/* ---------------------------------------------------------------- plays */

void rx_link_plays_reset(RXLinkPlays *p)
{
    memset(p, 0, sizeof(*p));
    p->last_beat = -1;
}

bool rx_link_plays_feed(RXLinkPlays *p, const RXLinkPlayer *pl, int ours, int64_t now_us)
{
    /* A rekordbox track (type 1) of ours: our device number is where the
     * player says it is from, whichever slot it names (a CDJ-3000 on
     * EP122 says USB for a computer's: link.h). */
    bool loaded = ours > 0 && pl->type == RX_LINK_DEVICE_PLAYER && pl->has_status &&
                  pl->track_device == ours && pl->track_type == 1 && pl->track_id;
    if (!loaded) {
        if (p->track_id || p->last_us)
            rx_link_plays_reset(p);
        return false;
    }
    if (pl->track_id != p->track_id) {
        rx_link_plays_reset(p);
        p->track_id = pl->track_id;
    }

    int64_t last_us = p->last_us;
    int last_beat = p->last_beat;
    p->last_us = now_us;
    p->last_beat = pl->beat;
    double since = (double)(now_us - last_us) * 1e-6;
    if (p->recorded || !pl->playing || !last_us || !(since > 0))
        return false;

    double speed = RX_MAX(1.0 + pl->pitch / 100.0, 0.0);
    if (pl->beat >= 1 && pl->bpm > 0) {
        /* Beats played forward since the last look, no more than its
         * tempo allows in the time between (and a beat or two of slack:
         * status packets come every 200 ms, the looks at any time). */
        int step = pl->beat - last_beat;
        double allowed = since * pl->bpm / 60.0 * RX_MAX(speed, 1.0) * 1.25 + 2.0;
        if (last_beat < 1 || step <= 0 || step > allowed)
            return false;
        for (int b = last_beat + 1; b <= pl->beat; b++) {
            int bin = RX_MIN(b, RX_LINK_PLAY_BEATS - 1);
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
        p->seconds += RX_MIN(since, 1.0) * speed;
    }
    if (p->seconds < RX_LINK_PLAY_SECONDS)
        return false;
    p->recorded = true;
    return true;
}

/* -------------------------------------------------------- start and stop */

static void options_free(RXAppLink *l)
{
    free(l->library);
    free(l->interface);
    free(l->name);
    free(l->broadcast);
    l->library = l->interface = l->name = l->broadcast = NULL;
}

static void state_clear(RXApp *app)
{
    RXAppLink *l = app->link;
    app->link_state.on = false;
    app->link_state.state = RX_LINK_WAITING;
    app->link_state.number = 0;
    app->link_state.nb_devices = 0;
    for (int i = 0; i < RX_APP_LINK_DEVICES; i++)
        l->counters[i].number = 0;
}

static void *worker(void *arg)
{
    RXAppLink *l = arg;
    if (l->stopping)
        rx_link_stop(&l->link);
    else
        l->result = rx_link_start(&l->link, &l->opt);
    atomic_store(&l->done, true);
    return NULL;
}

static int run(RXApp *app, bool stopping)
{
    RXAppLink *l = app->link;
    l->stopping = stopping;
    atomic_store(&l->done, false);
    int ret = pthread_create(&l->thread, NULL, worker, l);
    if (ret)
        return RX_ERROR(ret);
    l->started = true;
    app->link_state.busy = true;
    app->dirty = true;
    return 0;
}

int rx_app_link_start(RXApp *app, const RXLinkOptions *o)
{
    if (!app->link_state.enabled)
        return RX_ERROR(EPERM);
    if (!o || !o->interface || !*o->interface)
        return RX_ERROR(EINVAL);
    if (app->link_state.busy)
        return RX_ERROR(EBUSY);
    if (app->link_state.on)
        return RX_ERROR(EALREADY);
    if (!app->link && !(app->link = calloc(1, sizeof(*app->link))))
        return RX_ERROR(ENOMEM);

    RXAppLink *l = app->link;
    options_free(l);
    l->opt = *o;
    const char *library = o->library ? o->library : app->lib.root;
    l->library = library ? rx_strdup(library) : NULL;
    l->interface = rx_strdup(o->interface);
    l->name = o->name ? rx_strdup(o->name) : NULL;
    l->broadcast = o->broadcast ? rx_strdup(o->broadcast) : NULL;
    if ((library && !l->library) || !l->interface || (o->name && !l->name) ||
        (o->broadcast && !l->broadcast)) {
        options_free(l);
        return RX_ERROR(ENOMEM);
    }
    l->opt.library = l->library;
    l->opt.interface = l->interface;
    l->opt.name = l->name;
    l->opt.broadcast = l->broadcast;

    snprintf(app->link_state.interface, sizeof(app->link_state.interface), "%s", o->interface);
    app->link_state.err = 0;
    l->stop_after = false;
    l->generation = l->seen_generation = app->lib.generation;
    l->first_change_us = 0;
    state_clear(app);
    int ret = run(app, false);
    if (ret < 0)
        app->link_state.err = ret;
    return ret;
}

void rx_app_link_stop(RXApp *app)
{
    RXAppLink *l = app->link;
    if (!l)
        return;
    if (app->link_state.busy) {
        if (!l->stopping)
            l->stop_after = true;
        return;
    }
    if (!l->link)
        return;
    if (run(app, true) < 0) {
        /* No thread for it: here, then. */
        rx_link_stop(&l->link);
        state_clear(app);
    }
}

/* ------------------------------------------------------------------ tick */

static Counter *counter_of(RXAppLink *l, int number)
{
    Counter *free_slot = NULL;
    for (int i = 0; i < RX_APP_LINK_DEVICES; i++) {
        Counter *c = &l->counters[i];
        if (c->number == number)
            return c;
        if (!c->number && !free_slot)
            free_slot = c;
    }
    /* Never NULL: there are as many slots as devices kept. */
    free_slot->number = number;
    rx_link_plays_reset(&free_slot->plays);
    return free_slot;
}

static void record(RXApp *app, const RXLinkPlayer *p)
{
    RXTrack *t = rx_library_find_id(&app->lib, p->track_id);
    if (!t)
        return;
    char name[256];
    rx_track_display(t, name, sizeof(name));
    int ret = rx_library_record_play(&app->lib, t);
    if (ret < 0) {
        app->last_error_count++;
        rx_app_set_status(app, "Cannot record the play of %s on player %d: %s", name, p->number,
                          strerror(-ret));
        return;
    }
    rx_app_set_status(app, "Played on player %d: %s", p->number, name);
    app->rows_dirty = true;
}

/* What the panel shows of a device. */
static bool shown_same(const RXLinkPlayer *a, const RXLinkPlayer *b)
{
    return a->number == b->number && a->type == b->type && a->addr == b->addr &&
           !strcmp(a->name, b->name) && a->has_status == b->has_status &&
           a->track_device == b->track_device && a->track_slot == b->track_slot &&
           a->track_id == b->track_id && a->playing == b->playing && a->on_air == b->on_air &&
           a->master == b->master && a->synced == b->synced && a->bpm == b->bpm &&
           a->pitch == b->pitch;
}

/* The service's state into link_state, plays counted on the way; true
 * when what is shown changed. */
static bool follow(RXApp *app, int64_t now)
{
    RXAppLink *l = app->link;
    int number = 0;
    enum RXLinkState state = rx_link_state(l->link, &number);
    RXLinkPlayer dev[RX_APP_LINK_DEVICES];
    int nb = RX_MIN(rx_link_players(l->link, dev, RX_APP_LINK_DEVICES), RX_APP_LINK_DEVICES);
    int ours = state == RX_LINK_UP ? number : 0;

    double played[RX_APP_LINK_DEVICES];
    bool counted[RX_APP_LINK_DEVICES];
    for (int i = 0; i < RX_APP_LINK_DEVICES; i++)
        l->counters[i].used = false;
    for (int i = 0; i < nb; i++) {
        Counter *c = counter_of(l, dev[i].number);
        c->used = true;
        if (rx_link_plays_feed(&c->plays, &dev[i], ours, now))
            record(app, &dev[i]);
        played[i] = c->plays.track_id ? c->plays.seconds : 0.0;
        counted[i] = c->plays.track_id && c->plays.recorded;
    }
    /* A device gone from the link: a load of its own when it is back. */
    for (int i = 0; i < RX_APP_LINK_DEVICES; i++) {
        if (!l->counters[i].used)
            l->counters[i].number = 0;
    }

    typeof(app->link_state) *ls = &app->link_state;
    bool same = state == ls->state && number == ls->number && nb == ls->nb_devices;
    for (int i = 0; i < nb && same; i++)
        same = shown_same(&dev[i], &ls->devices[i]) && counted[i] == ls->counted[i] &&
               (int)played[i] == (int)ls->played[i];
    ls->state = state;
    ls->number = number;
    ls->nb_devices = nb;
    memcpy(ls->devices, dev, (size_t)nb * sizeof(*dev));
    memcpy(ls->played, played, (size_t)nb * sizeof(*played));
    memcpy(ls->counted, counted, (size_t)nb * sizeof(*counted));
    return !same;
}

/* The library read again once its changes have settled: every change
 * bumps lib.generation (an analysis applied, a cue set, a play). */
static void follow_library(RXApp *app, int64_t now)
{
    RXAppLink *l = app->link;
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
    rx_link_library_changed(l->link);
    l->generation = gen;
    l->first_change_us = 0;
}

bool rx_app_link_tick(RXApp *app)
{
    RXAppLink *l = app->link;
    if (!l)
        return false;
    bool changed = false;
    if (l->started) {
        if (!atomic_load(&l->done))
            return false;
        pthread_join(l->thread, NULL);
        l->started = false;
        app->link_state.busy = false;
        changed = true;
        if (l->stopping) {
            state_clear(app);
            rx_app_set_status(app, "PRO DJ LINK off");
        } else if (l->result < 0) {
            state_clear(app);
            app->link_state.err = l->result;
            app->last_error_count++;
            rx_app_set_status(app, "PRO DJ LINK cannot start on %s: %s", l->interface,
                              strerror(-l->result));
        } else {
            app->link_state.on = true;
            rx_app_set_status(app, "PRO DJ LINK on %s, waiting for players", l->interface);
        }
        if (l->stop_after) {
            l->stop_after = false;
            rx_app_link_stop(app);
            return true;
        }
    }
    if (!l->link)
        return changed;

    int64_t now = rx_time_us();
    if (follow(app, now))
        changed = true;
    follow_library(app, now);
    return changed;
}

void rx_app_link_destroy(RXApp *app)
{
    RXAppLink *l = app->link;
    if (!l)
        return;
    if (l->started)
        pthread_join(l->thread, NULL);
    rx_link_stop(&l->link);
    options_free(l);
    free(l);
    app->link = NULL;
    app->link_state.on = app->link_state.busy = false;
    app->link_state.nb_devices = 0;
}
