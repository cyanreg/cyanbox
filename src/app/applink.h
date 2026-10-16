/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PRO DJ LINK in the app (link.c): what app.c calls, and the play counter,
 * which the tests feed directly with statuses of their own making. */

#ifndef CYANBOX_APP_APPLINK_H
#define CYANBOX_APP_APPLINK_H

#include "cyanbox/app.h"

/* A load counts as a play once this much of it has played, as a load on
 * the deck does (app.c, PLAY_RECORD_SECONDS). */
#define CB_LINK_PLAY_SECONDS 30.0
/* Beats told apart (a bit each): 45 minutes at 180 BPM; the rest share
 * the last. */
#define CB_LINK_PLAY_BEATS   8192

/* One player's load of a track of ours, counted from its status as the
 * deck counts its own: distinct audio, played forward. With beat numbers
 * (a track with a grid) each beat counts once, 60 / BPM seconds of the
 * track, so a loop or a hot cue called again and again counts what it
 * plays once, and a jump (a seek, a hot cue ahead, a loop going back) adds
 * nothing; without them, the time it plays at its pitch. */
typedef struct CBLinkPlays {
    uint32_t track_id;          /* the load being counted, 0 none */
    int64_t  last_us;           /* the status looked at last, 0 none */
    int      last_beat;         /* its beat number, -1 unknown */
    double   seconds;
    bool     recorded;
    uint8_t  seen[CB_LINK_PLAY_BEATS / 8];
} CBLinkPlays;

void cb_link_plays_reset(CBLinkPlays *p);
/* One look at a player's last status, at now_us; `ours` is our device
 * number (0 before we have one). True once per load: when it has played
 * long enough to count. A player that loads another track, or unloads,
 * starts a new count. */
bool cb_link_plays_feed(CBLinkPlays *p, const CBLinkPlayer *pl, int ours, int64_t now_us);

/* A load goes on the History (cb_library_history_add) once it has played
 * this long straight on: not added up, one stretch with no pause, no jump
 * and no loop. */
#define CB_LINK_HISTORY_SECONDS 90.0

/* One player's load of a track of ours, timed for the History: the
 * stretch it has been playing since it last started, by the clock. With
 * beat numbers a stretch ends at a jump back (a hot cue, a loop going
 * round, a seek) or further ahead than its tempo plays; without them (a
 * pre-nexus player) only at a pause. */
typedef struct CBLinkRun {
    uint32_t track_id;          /* the load being timed, 0 none */
    int64_t  start_us;          /* cb_time_us the stretch began, 0 none */
    int64_t  start_at;          /* epoch seconds then: when it played, for the History */
    int64_t  last_us;           /* the status looked at last */
    int      last_beat;         /* its beat number, -1 unknown */
    bool     recorded;
} CBLinkRun;

void cb_link_run_reset(CBLinkRun *r);
/* One look at a player's last status, at now_us (now_at in epoch
 * seconds); `ours` as for cb_link_plays_feed. True once per load: when a
 * stretch has reached CB_LINK_HISTORY_SECONDS (r->start_at says when it
 * began). */
bool cb_link_run_feed(CBLinkRun *r, const CBLinkPlayer *pl, int ours, int64_t now_us,
                      int64_t now_at);

/* From cb_app_tick, after the saves are written: a start or stop done,
 * the state copied, plays counted, the library read again. True when
 * something the interface shows changed. */
bool cb_app_link_tick(CBApp *app);
/* From cb_app_destroy, before the library closes: stops it and waits. */
void cb_app_link_destroy(CBApp *app);

#endif /* CYANBOX_APP_APPLINK_H */
