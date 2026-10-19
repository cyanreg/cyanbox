/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Presence on the link: the DJ Link packets on UDP 50000 (announce, device
 * number claims, keep-alives) and 50002 (status, media queries), and the
 * state machine that joins the link as rekordbox does, keeps the number,
 * answers what players ask of a computer source, and keeps the list of
 * devices heard with what each player's status says.
 *
 * No sockets: the service thread (service.c) hands every datagram that
 * arrived to cb_presence_input and calls cb_presence_tick when its time
 * comes; whatever is to be sent goes out through the send callback. Time
 * is cb_time_us(), passed in, so the tests drive the clock.
 *
 * The same machine plays a player for the fake player (client.c, the
 * tests): it then joins as a CDJ with a fixed number and sends a CDJ's
 * status, saying what it has loaded and whether it plays.
 *
 * The packets themselves, parsed and built, are packet.h's. */

#ifndef CYANBOX_LINK_PRESENCE_H
#define CYANBOX_LINK_PRESENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cyanbox/link.h"

/* Which of the peers' ports a datagram is for or came in on. */
enum CBLinkPort {
    CB_LINK_ANNOUNCE,           /* 50000 */
    CB_LINK_STATUS,             /* 50002 */
    CB_LINK_CUE,                /* 50004: LINK CUE (packet.h), never presence's */
};

#define CB_PRESENCE_BROADCAST 0xFFFFFFFFu   /* `to`: the broadcast address */

/* One datagram to send to `to` (an IPv4 address in host order, or
 * CB_PRESENCE_BROADCAST) on the peers' `port`. Claims and keep-alives are
 * broadcast on the announce port, answers to probes unicast there; the
 * status is broadcast on the status port (rekordbox 7.2.11 sends it from
 * an ephemeral source port, rbxport measured), every answer there is
 * unicast from our own status port, which players answer to. */
typedef void (*CBPresenceSend)(void *opaque, enum CBLinkPort port, uint32_t to,
                               const uint8_t *buf, size_t len);

enum CBPresenceRole {
    CB_PRESENCE_COMPUTER,       /* cyanbox: joins as rekordbox does */
    CB_PRESENCE_PLAYER,         /* the fake player: a CDJ with a fixed number */
};

typedef struct CBPresenceConfig {
    enum CBPresenceRole role;
    char     name[21];          /* "rekordbox"; the fake player a CDJ's model name */
    uint8_t  mac[6];
    uint32_t addr;              /* our IPv4 address, host order */
    int      number;            /* CB_PRESENCE_PLAYER: the number to claim, 1..80 */
    bool     start_up;          /* CB_PRESENCE_COMPUTER: join without waiting for a player */
    /* CB_PRESENCE_COMPUTER: the computer's name, which the identity reply
     * carries (rekordbox sends the host name); empty: `name`. */
    char     host[64];
} CBPresenceConfig;

typedef struct CBPresence CBPresence;

int  cb_presence_new(CBPresence **out, const CBPresenceConfig *c, CBPresenceSend send,
                     void *opaque);
void cb_presence_free(CBPresence **p);

/* A datagram that arrived on our `port` from `from` (host order); answers
 * (an "in use" to a probe, a media response, a greeting) go out through
 * the callback right away. Malformed or foreign datagrams are ignored, and
 * so are our own come back (our address, or our MAC in the packet).
 *
 * An input can make something due at once (the first keep-alive heard
 * starts the join): call cb_presence_tick after a batch of inputs, not only
 * when its last deadline passed. It sends only what is due, so calling it
 * early costs nothing. */
void cb_presence_input(CBPresence *p, enum CBLinkPort port, uint32_t from,
                       const uint8_t *buf, size_t len, int64_t now);
/* Sends what is due at `now` (claims, keep-alives, status) and forgets
 * devices not heard for a while; returns when to call it next (an
 * absolute cb_time_us). */
int64_t cb_presence_tick(CBPresence *p, int64_t now);

enum CBLinkState cb_presence_state(const CBPresence *p, int *number);
/* Whether `addr` is a device on the link (it sent a keep-alive): the
 * database server and NFS answer only these (docs/LINK.md, "Security"). */
bool cb_presence_is_peer(const CBPresence *p, uint32_t addr);
/* The devices heard, players first by number. */
int  cb_presence_players(const CBPresence *p, CBLinkPlayer *out, int max);

/* CB_PRESENCE_PLAYER: what the fake player's status says from now on
 * (and no beats: see cb_presence_play); what it says it has loaded, as
 * set or as a load command left it. */
void cb_presence_set_loaded(CBPresence *p, int track_device, int slot, uint32_t track_id,
                            bool playing);
void cb_presence_loaded(const CBPresence *p, int *device, int *slot, uint32_t *track_id);
/* CB_PRESENCE_PLAYER: the fake player's loaded track from its start at
 * `now`, whose beats these are (the beat grid's times in ms and tempos
 * x100), playing or paused as it was: a load does not start a player (an
 * emulated CDJ-900 kept a paused deck paused and a playing one playing).
 * Its status says the beat number reached and the tempo there, as a
 * nexus player's does; playing, it stops two seconds past the last. */
int  cb_presence_play(CBPresence *p, const uint32_t *beat_ms, const uint16_t *bpm_x100, int nb,
                      int64_t now);

/* CB_PRESENCE_COMPUTER: tells `player` to load our track `track_id` (a
 * load command, 19, as from rekordbox: our collection's slot, type 1).
 * -EAGAIN before we are up, -ENOENT when there is no such player. */
int  cb_presence_load(CBPresence *p, int player, uint32_t track_id);

/* CB_PRESENCE_COMPUTER: the counts the media response gives a player (the
 * library's tracks and playlists; 0 until set, at most 65535 each). */
void cb_presence_set_media(CBPresence *p, int nb_tracks, int nb_playlists);
/* CB_PRESENCE_COMPUTER: the display settings a player's device-settings
 * request is answered with, as the 32-byte DEVSETTING.DAT payload
 * (docs/settings.md section 6). Until set, rekordbox 7.2.11's own answer:
 * half overview, 3-band colour, classic keys, centred position. */
void cb_presence_set_devsetting(CBPresence *p, const uint8_t payload[32]);
/* CB_PRESENCE_COMPUTER: the player settings a player that LOADs My
 * Settings from us gets, as the 40-byte MYSETTING.DAT and MYSETTING2.DAT
 * payloads (docs/settings.md sections 3 and 4). Until set, such a LOAD is
 * not answered. */
void cb_presence_set_mysetting(CBPresence *p, const uint8_t mysetting[40],
                               const uint8_t mysetting2[40]);
/* CB_PRESENCE_COMPUTER: the mixer settings a mixer that LOADs its My
 * Settings from us gets, as the 52-byte DJMMYSETTING.DAT payload
 * (docs/settings.md section 5). Until set, such a LOAD is not answered. */
void cb_presence_set_djmmysetting(CBPresence *p, const uint8_t payload[52]);
/* CB_PRESENCE_PLAYER, for the tests: LOADs the mixer's My Settings from
 * the computer source found, as a DJM-A9 does (42); the payload it
 * answered once it did as the A9 takes it. -EAGAIN before a source is
 * found. */
int  cb_presence_load_djm_settings(CBPresence *p);
bool cb_presence_loaded_djm_settings(const CBPresence *p, uint8_t payload[52]);
/* CB_PRESENCE_PLAYER: LOADs My Settings from the computer source found,
 * as a player's UTILITY does (35, about its collection's slot);
 * cb_presence_loaded_settings then gives the MYSETTING.DAT and
 * MYSETTING2.DAT payloads it answered (the second zeros in a short
 * answer), false until it did as a CDJ-3000X takes it. -EAGAIN before a
 * source is found. */
int  cb_presence_load_settings(CBPresence *p);
bool cb_presence_loaded_settings(const CBPresence *p, uint8_t mysetting[40],
                                 uint8_t mysetting2[40]);
/* CB_PRESENCE_PLAYER: the computer source the fake player found and what
 * its media response said; false until one answered. Any pointer may be
 * NULL. */
bool cb_presence_source(const CBPresence *p, int *number, uint32_t *addr, int *nb_tracks,
                        int *nb_playlists);

#endif /* CYANBOX_LINK_PRESENCE_H */
