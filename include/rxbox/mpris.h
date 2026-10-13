/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_MPRIS_H
#define RXBOX_MPRIS_H

/* MPRIS 2 for the deck (src/app/mpris.c): org.mpris.MediaPlayer2 and
 * org.mpris.MediaPlayer2.Player on the D-Bus session bus, as
 * org.mpris.MediaPlayer2.rxbox, so media keys, playerctl and a desktop's
 * media controls see the loaded track and drive play, pause, seek and the
 * next and previous rows of the current view.
 *
 * Only the interface registers (src/cli.c, unless --no-mpris); the
 * subcommands never do. Everything runs on the owner thread: the
 * connection is a private one that rx_mpris_tick() polls without waiting,
 * so the bus never holds up a frame. Built only with libdbus (the meson
 * option mpris); without it these do nothing and create says ENOSYS. */

#include "config.h"
#include "rxbox/app.h"

#if RXBOX_MPRIS
/* Connects to the session bus and takes the name. Without a session bus,
 * or with the name taken (another rxbox), it logs one warning and returns
 * a negative errno: the app runs on without. Sets app->mpris. */
int  rx_mpris_create(RXApp *app);
/* Once per tick of the interface loop, after rx_app_tick(): answers what
 * came in, starts a track a call loaded once it is in, and tells the bus
 * what changed. */
void rx_mpris_tick(RXApp *app);
/* Leaves the bus and removes the cover files it wrote; nothing without
 * create. */
void rx_mpris_destroy(RXApp *app);
#else
static inline int  rx_mpris_create(RXApp *app) { return RX_ERROR(ENOSYS); }
static inline void rx_mpris_tick(RXApp *app) { }
static inline void rx_mpris_destroy(RXApp *app) { }
#endif

#endif /* RXBOX_MPRIS_H */
