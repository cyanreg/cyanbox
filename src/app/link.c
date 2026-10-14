/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PRO DJ LINK in the app (rxbox/app.h, rx_app_link_start): a set of
 * services (rxbox/link.h RXLinkSet), one on every Ethernet interface,
 * started with the app when it runs with --pro-link and serving from then
 * on; the set's own thread starts and stops them as interfaces come and
 * go, so the interface never waits for that. Each tick copies the set's
 * state into app->link_state, counts plays from the players' status, and
 * tells the services when the library has changed, once it has settled.
 *
 * What the players send back is applied here, on the owner thread: the
 * services never write the library. */

#include "applink.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

/* The library is read again this long after its last change, or this
 * long after the first of a run of changes that does not settle (an
 * analysis queue): reading it is a plan over every track. */
#define RELOAD_QUIET_US   2000000
#define RELOAD_LONGEST_US 30000000

/* A player's count, by the interface it is on and its number there
 * (numbers repeat from one network to another). */
typedef struct Counter {
    char interface[16];
    int  number;                /* 0 a free slot */
    bool used;                  /* seen in this tick's devices */
    RXLinkPlays plays;
} Counter;

typedef struct RXAppLink {
    RXLinkSet *set;
    unsigned generation;        /* lib.generation the services read last */
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

static void state_clear(RXApp *app)
{
    app->link_state.on = false;
    app->link_state.nb_interfaces = 0;
    app->link_state.nb_devices = 0;
}

int rx_app_link_start(RXApp *app, const RXLinkOptions *o)
{
    if (!app->link_state.enabled)
        return RX_ERROR(EPERM);
    if (app->link_state.on)
        return RX_ERROR(EALREADY);
    if (!app->link && !(app->link = calloc(1, sizeof(*app->link))))
        return RX_ERROR(ENOMEM);
    RXAppLink *l = app->link;
    RXLinkOptions opt;
    if (o)
        opt = *o;
    else
        rx_link_options_default(&opt);
    if (!opt.library)
        opt.library = app->lib.root;
    int ret = rx_link_set_start(&l->set, &opt);
    if (ret < 0) {
        app->last_error_count++;
        rx_app_set_status(app, "PRO DJ LINK cannot start: %s", strerror(-ret));
        return ret;
    }
    *l = (RXAppLink) {
        .set = l->set,
        .generation = app->lib.generation,
        .seen_generation = app->lib.generation,
    };
    state_clear(app);
    app->link_state.on = true;
    app->dirty = true;
    return 0;
}

void rx_app_link_stop(RXApp *app)
{
    RXAppLink *l = app->link;
    if (!l || !l->set)
        return;
    rx_link_set_stop(&l->set);
    for (int i = 0; i < RX_APP_LINK_DEVICES; i++)
        l->counters[i].number = 0;
    state_clear(app);
    app->dirty = true;
}

/* ------------------------------------------------------------------ tick */

static Counter *counter_of(RXAppLink *l, const char *interface, int number)
{
    Counter *free_slot = NULL;
    for (int i = 0; i < RX_APP_LINK_DEVICES; i++) {
        Counter *c = &l->counters[i];
        if (c->number == number && !strcmp(c->interface, interface))
            return c;
        if (!c->number && !free_slot)
            free_slot = c;
    }
    /* Never NULL: there are as many slots as devices kept. */
    free_slot->number = number;
    snprintf(free_slot->interface, sizeof(free_slot->interface), "%s", interface);
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

/* What the window shows of a device. */
static bool shown_same(const RXLinkPlayer *a, const RXLinkPlayer *b)
{
    return a->number == b->number && a->type == b->type && a->addr == b->addr &&
           !strcmp(a->name, b->name) && a->has_status == b->has_status &&
           a->track_device == b->track_device && a->track_slot == b->track_slot &&
           a->track_id == b->track_id && a->playing == b->playing && a->on_air == b->on_air &&
           a->master == b->master && a->synced == b->synced && a->bpm == b->bpm &&
           a->pitch == b->pitch;
}

static bool served_same(const RXLinkServed *a, const RXLinkServed *b)
{
    return !strcmp(a->interface, b->interface) && a->addr == b->addr &&
           a->running == b->running && a->err == b->err && a->state == b->state &&
           a->number == b->number;
}

/* The set's state into link_state, plays counted on the way; true when
 * what is shown changed. */
static bool follow(RXApp *app, int64_t now)
{
    RXAppLink *l = app->link;
    RXLinkServed ifs[RX_APP_LINK_IFS];
    int nb_ifs = RX_MIN(rx_link_set_interfaces(l->set, ifs, RX_APP_LINK_IFS), RX_APP_LINK_IFS);
    RXLinkPlayer dev[RX_APP_LINK_DEVICES];
    int where[RX_APP_LINK_DEVICES];
    int nb = RX_MIN(rx_link_set_players(l->set, dev, where, RX_APP_LINK_DEVICES),
                    RX_APP_LINK_DEVICES);

    double played[RX_APP_LINK_DEVICES];
    bool counted[RX_APP_LINK_DEVICES];
    for (int i = 0; i < RX_APP_LINK_DEVICES; i++)
        l->counters[i].used = false;
    for (int i = 0; i < nb; i++) {
        /* Our number on the device's own network: its loads of a track
         * of ours name that number. */
        const RXLinkServed *s = where[i] < nb_ifs ? &ifs[where[i]] : NULL;
        int ours = s && s->running && s->state == RX_LINK_UP ? s->number : 0;
        Counter *c = counter_of(l, s ? s->interface : "", dev[i].number);
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
    rx_link_set_library_changed(l->set);
    l->generation = gen;
    l->first_change_us = 0;
}

bool rx_app_link_tick(RXApp *app)
{
    RXAppLink *l = app->link;
    if (!l || !l->set)
        return false;
    int64_t now = rx_time_us();
    bool changed = follow(app, now);
    follow_library(app, now);
    return changed;
}

void rx_app_link_destroy(RXApp *app)
{
    rx_app_link_stop(app);
    free(app->link);
    app->link = NULL;
}
