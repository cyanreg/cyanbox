/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_MPRIS_H
#define CYANBOX_MPRIS_H

/* MPRIS 2 for the deck (src/app/mpris.c): org.mpris.MediaPlayer2 and
 * org.mpris.MediaPlayer2.Player on the D-Bus session bus, as
 * org.mpris.MediaPlayer2.cyanbox, so media keys, playerctl and a desktop's
 * media controls see the loaded track and drive play, pause, seek and the
 * next and previous rows of the current view.
 *
 * Only the interface registers (src/cli.c, unless --no-mpris); the
 * subcommands never do. Everything runs on the owner thread: the
 * connection is one of its own that cb_mpris_tick() polls without
 * waiting, so the bus never holds up a frame. Built only with sd-bus
 * (libsystemd, libelogind or basu; the meson option mpris); without it
 * these do nothing and create says ENOSYS. */

#include "config.h"
#include "cyanbox/app.h"

#if CYANBOX_MPRIS
/* Connects to the session bus and takes the name. Without a session bus,
 * or with the name taken (another cyanbox), it logs one warning and returns
 * a negative errno: the app runs on without. Sets app->mpris. */
int  cb_mpris_create(CBApp *app);
/* Once per tick of the interface loop, after cb_app_tick(): answers what
 * came in, starts a track a call loaded once it is in, and tells the bus
 * what changed. */
void cb_mpris_tick(CBApp *app);
/* Leaves the bus and removes the cover files it wrote; nothing without
 * create. */
void cb_mpris_destroy(CBApp *app);
#else
static inline int  cb_mpris_create(CBApp *app) { return CB_ERROR(ENOSYS); }
static inline void cb_mpris_tick(CBApp *app) { }
static inline void cb_mpris_destroy(CBApp *app) { }
#endif

#endif /* CYANBOX_MPRIS_H */
