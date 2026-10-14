/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Presence on the link: the DJ Link packets on UDP 50000 (announce, device
 * number claims, keep-alives) and 50002 (status, media queries), and the
 * state machine that joins the link as rekordbox does, keeps the number,
 * answers what players ask of a computer source, and keeps the list of
 * devices heard with what each player's status says.
 *
 * No sockets: the service thread (service.c) hands every datagram that
 * arrived to rx_presence_input and calls rx_presence_tick when its time
 * comes; whatever is to be sent goes out through the send callback. Time
 * is rx_time_us(), passed in, so the tests drive the clock.
 *
 * The same machine plays a player for the fake player (client.c, the
 * tests): it then joins as a CDJ with a fixed number and sends a CDJ's
 * status, saying what it has loaded and whether it plays.
 *
 * The packets themselves, parsed and built, are packet.h's. */

#ifndef RXBOX_LINK_PRESENCE_H
#define RXBOX_LINK_PRESENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rxbox/link.h"

/* Which of the peers' ports a datagram is for or came in on. */
enum RXLinkPort {
    RX_LINK_ANNOUNCE,           /* 50000 */
    RX_LINK_STATUS,             /* 50002 */
};

#define RX_PRESENCE_BROADCAST 0xFFFFFFFFu   /* `to`: the broadcast address */

/* One datagram to send to `to` (an IPv4 address in host order, or
 * RX_PRESENCE_BROADCAST) on the peers' `port`. Claims and keep-alives are
 * broadcast on the announce port, answers to probes unicast there; the
 * status is broadcast on the status port (rekordbox 7.2.11 sends it from
 * an ephemeral source port, rbxport measured), every answer there is
 * unicast from our own status port, which players answer to. */
typedef void (*RXPresenceSend)(void *opaque, enum RXLinkPort port, uint32_t to,
                               const uint8_t *buf, size_t len);

enum RXPresenceRole {
    RX_PRESENCE_COMPUTER,       /* rxbox: joins as rekordbox does */
    RX_PRESENCE_PLAYER,         /* the fake player: a CDJ with a fixed number */
};

typedef struct RXPresenceConfig {
    enum RXPresenceRole role;
    char     name[21];          /* "rekordbox"; the fake player a CDJ's model name */
    uint8_t  mac[6];
    uint32_t addr;              /* our IPv4 address, host order */
    int      number;            /* RX_PRESENCE_PLAYER: the number to claim, 1..80 */
    bool     start_up;          /* RX_PRESENCE_COMPUTER: join without waiting for a player */
    /* RX_PRESENCE_COMPUTER: the computer's name, which the identity reply
     * carries (rekordbox sends the host name); empty: `name`. */
    char     host[64];
} RXPresenceConfig;

typedef struct RXPresence RXPresence;

int  rx_presence_new(RXPresence **out, const RXPresenceConfig *c, RXPresenceSend send,
                     void *opaque);
void rx_presence_free(RXPresence **p);

/* A datagram that arrived on our `port` from `from` (host order); answers
 * (an "in use" to a probe, a media response, a greeting) go out through
 * the callback right away. Malformed or foreign datagrams are ignored, and
 * so are our own come back (our address, or our MAC in the packet).
 *
 * An input can make something due at once (the first keep-alive heard
 * starts the join): call rx_presence_tick after a batch of inputs, not only
 * when its last deadline passed. It sends only what is due, so calling it
 * early costs nothing. */
void rx_presence_input(RXPresence *p, enum RXLinkPort port, uint32_t from,
                       const uint8_t *buf, size_t len, int64_t now);
/* Sends what is due at `now` (claims, keep-alives, status) and forgets
 * devices not heard for a while; returns when to call it next (an
 * absolute rx_time_us). */
int64_t rx_presence_tick(RXPresence *p, int64_t now);

enum RXLinkState rx_presence_state(const RXPresence *p, int *number);
/* Whether `addr` is a device on the link (it sent a keep-alive): the
 * database server and NFS answer only these (docs/LINK.md, "Security"). */
bool rx_presence_is_peer(const RXPresence *p, uint32_t addr);
/* The devices heard, players first by number. */
int  rx_presence_players(const RXPresence *p, RXLinkPlayer *out, int max);

/* RX_PRESENCE_PLAYER: what the fake player's status says from now on. */
void rx_presence_set_loaded(RXPresence *p, int track_device, int slot, uint32_t track_id,
                            bool playing);

/* RX_PRESENCE_COMPUTER: tells `player` to load our track `track_id` (a
 * load command, 19, as from rekordbox: our collection's slot, type 1).
 * -EAGAIN before we are up, -ENOENT when there is no such player. */
int  rx_presence_load(RXPresence *p, int player, uint32_t track_id);

/* RX_PRESENCE_COMPUTER: the counts the media response gives a player (the
 * library's tracks and playlists; 0 until set, at most 65535 each). */
void rx_presence_set_media(RXPresence *p, int nb_tracks, int nb_playlists);
/* RX_PRESENCE_COMPUTER: the display settings a player's device-settings
 * request is answered with, as the 32-byte DEVSETTING.DAT payload
 * (docs/settings.md section 6). Until set, rekordbox 7.2.11's own answer:
 * half overview, 3-band colour, classic keys, centred position. */
void rx_presence_set_devsetting(RXPresence *p, const uint8_t payload[32]);
/* RX_PRESENCE_PLAYER: the computer source the fake player found and what
 * its media response said; false until one answered. Any pointer may be
 * NULL. */
bool rx_presence_source(const RXPresence *p, int *number, uint32_t *addr, int *nb_tracks,
                        int *nb_playlists);

#endif /* RXBOX_LINK_PRESENCE_H */
