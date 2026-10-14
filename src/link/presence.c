/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Presence on the link (presence.h), as a state machine with no socket and
 * no clock of its own.
 *
 * As a computer it does what rbxport measured rekordbox 7.2.11 doing, on
 * the wire and in its decompiled join (rbl-link src/join.rs, beacon.rs):
 * nothing is sent into an empty network; the first keep-alive from a player
 * or a mixer starts the join; three first-stage claims 100 ms apart; a probe
 * of each of the six numbers rekordbox reserves (17, 18, 41..44) every
 * 100 ms, six rounds through, counter-major, skipping any a device answers
 * for or announces; 17 or 18 then, or six requests to be assigned one.
 * Up, a keep-alive every 2 s, rekordbox's status every 200 ms, an "in use"
 * to whoever probes the number, answers to the questions players ask a
 * computer source, and a greeting to each player once.
 *
 * As the fake player it is a CDJ-3000 with a fixed number, as djl-analysis
 * describes a CDJ starting up (startup.adoc): three announcements, then
 * three of each claim stage, 300 ms apart; then keep-alives (every 1.5 s,
 * rbxport's measurement of a CDJ-3000) and its status every 200 ms to each
 * device heard (vcdj.adoc), and it asks the first computer it hears who it
 * is and what its collection holds, as rbxport saw a CDJ-3000 do.
 *
 * Decisions of its own, not rekordbox's:
 * - A device is on the link from its keep-alive and off it 10 s after its
 *   last one, the silence after which djl-analysis saw a player's peer
 *   count drop and beat-link's DeviceFinder forgets a device (rbxport takes
 *   three keep-alive intervals, 6 s).
 * - A status only updates a device whose number and address its keep-alive
 *   gave, and only devices on the link are answered on the status port: the
 *   database server and NFS lean on the same list (docs/LINK.md,
 *   "Security"). Probes are answered from anyone, as a prober has no
 *   keep-alive yet; the answer is smaller than the probe.
 * - Answers go to the address a datagram came from, never to one a packet
 *   names, so nothing here can be pointed at a third host.
 * - The device table is a fixed array: a flood of keep-alives fills it and
 *   costs nothing more. */

#include "presence.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rxbox/common.h"

#include "packet.h"

#define MS      INT64_C(1000)
#define SECOND  (1000 * MS)

/* rekordbox 7.2.11 (rbxport: the join's timer and counts from the
 * decompilation, the intervals measured on the wire). */
#define JOIN_STEP       (100 * MS)
#define DISCOVERY_SENDS 3
#define PROBE_ROUNDS    6
#define ASSIGN_SENDS    6
#define RB_KEEP_ALIVE   (2 * SECOND)
#define STATUS_EVERY    (200 * MS)

/* djl-analysis startup.adoc ("after about ten seconds"); beat-link
 * DeviceFinder.MAXIMUM_AGE. */
#define FORGET          (10 * SECOND)
/* After a join that found no number, no new one for a while, so a full link
 * is not probed again on every keep-alive [ASSUME: rekordbox gives up]. */
#define RETRY_HOLD      (10 * SECOND)
/* The longest a tick asks to wait with nothing due. */
#define IDLE            SECOND

/* A CDJ: 300 ms between claims (djl-analysis startup.adoc), three
 * announcements and three of each claim stage; keep-alives every 1.5 s
 * (rbxport, a CDJ-3000 measured; djl-analysis saw 2 s from a CDJ-2000nexus).
 * The questions to a computer are asked again each second until answered
 * [ASSUME]. */
#define CDJ_STEP        (300 * MS)
#define CDJ_CLAIMS      12
#define CDJ_KEEP_ALIVE  (1500 * MS)
#define QUERY_RETRY     SECOND

#define MAX_DEVICES     32

/* The numbers rekordbox probes, in its order; it takes only the first two,
 * its wired identities (rbxport, from the decompiled join). */
static const int candidates[6] = { 17, 18, 41, 42, 43, 44 };

/* rekordbox 7.2.11's answer to a device-settings request, as its
 * DEVSETTING.DAT payload (rbxport's capture). */
static const uint8_t default_devsetting[32] = {
    0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00, 0x02,
};

enum Stage {
    ST_WAITING,                 /* computer: no player heard; player: not started */
    ST_DISCOVERY,               /* computer: first-stage claims */
    ST_PROBING,                 /* computer: probing the six numbers */
    ST_ASSIGNING,               /* computer: asking to be assigned one */
    ST_CLAIMING,                /* player: announcements and claims */
    ST_UP,
};

enum SourceStep {
    SRC_NONE,
    SRC_IDENTITY,               /* asking who the computer is */
    SRC_MEDIA,                  /* asking what its collection holds */
    SRC_DONE,
};

typedef struct Device {
    RXLinkPlayer d;
    int  raw_type;
    int  bar_beat;
    bool greeted;
} Device;

struct RXPresence {
    RXPresenceConfig c;
    RXPresenceSend send;
    void *opaque;

    enum Stage stage;
    int  sent;                  /* packets sent in this stage */
    int  round, index;          /* probing: the round from 1, the next candidate */
    bool in_use[6];
    int  number;                /* ours, once up */
    int  first;                 /* player: keep-alive byte 0x25, latched when up */
    int64_t next_join, next_keep_alive, next_status, hold_until;

    Device dev[MAX_DEVICES];
    int  nb_dev;

    int  nb_tracks, nb_playlists;
    uint8_t devsetting[32];

    int  track_device, track_slot;
    uint32_t track_id;
    bool playing;

    struct {
        enum SourceStep step;
        int number;
        uint32_t addr;
        int64_t next;
        int tracks, playlists;
    } src;
};

/* ---------------------------------------------------------------- helpers */

static void emit(RXPresence *p, enum RXLinkPort port, uint32_t to, const RXWr *w)
{
    if (!w->err && w->len)
        p->send(p->opaque, port, to, w->p, w->len);
}

static int candidate(int number)
{
    for (int i = 0; i < 6; i++) {
        if (candidates[i] == number)
            return i;
    }
    return -1;
}

static bool own_mac(const RXPresence *p, const uint8_t mac[6])
{
    static const uint8_t zero[6];
    return memcmp(p->c.mac, zero, 6) && !memcmp(mac, p->c.mac, 6);
}

static bool joining(const RXPresence *p)
{
    return p->stage == ST_DISCOVERY || p->stage == ST_PROBING || p->stage == ST_ASSIGNING;
}

/* The next time a periodic packet is due: on its cadence, or a whole
 * interval from now when a tick came too late for that. */
static int64_t later(int64_t due, int64_t step, int64_t now)
{
    due += step;
    return due > now ? due : now + step;
}

static Device *find(RXPresence *p, int number)
{
    for (int i = 0; i < p->nb_dev; i++) {
        if (p->dev[i].d.number == number)
            return &p->dev[i];
    }
    return NULL;
}

static void drop(RXPresence *p, int i)
{
    const RXLinkPlayer *d = &p->dev[i].d;
    rx_debug("link: device %d (%s) left", d->number, d->name);
    p->dev[i] = p->dev[--p->nb_dev];
}

static void drop_number(RXPresence *p, int number)
{
    for (int i = 0; i < p->nb_dev; i++) {
        if (p->dev[i].d.number == number) {
            drop(p, i);
            return;
        }
    }
}

/* The fake player's source is a device on the link, or none: it may have
 * left, or moved, by any packet or by silence. */
static void check_source(RXPresence *p)
{
    if (p->src.step != SRC_NONE) {
        const Device *d = find(p, p->src.number);
        if (!d || d->d.addr != p->src.addr)
            p->src.step = SRC_NONE;
    }
}

static void forget(RXPresence *p, int64_t now)
{
    for (int i = p->nb_dev - 1; i >= 0; i--) {
        if (now - p->dev[i].d.last_seen > FORGET)
            drop(p, i);
    }
    check_source(p);
}

/* ----------------------------------------------------------------- sending */

static void send_keep_alive(RXPresence *p)
{
    bool computer = p->c.role == RX_PRESENCE_COMPUTER;
    /* rekordbox 7.2.11 and the CDJ-3000, as rbxport captured them side by
     * side: 01 then 08 at 0x31 and 0x35 for rekordbox, 00 and 64 for the
     * CDJ (djl-analysis: the 64 keeps CDJ-3000s on 5 and 6 on the link).
     * rekordbox counts the other devices, a CDJ itself too (djl-analysis;
     * rbxport's capture: rekordbox 02 and the CDJ 03 with three present). */
    RXPktKeepAlive k = {
        .proto  = 0x03,
        .number = p->number,
        .first  = computer ? 0x01 : p->first,
        .addr   = p->c.addr,
        .peers  = computer ? p->nb_dev : p->nb_dev + 1,
        .flag   = computer ? 0x01 : 0x00,
        .type   = computer ? 0x04 : 0x01,
        .tail   = computer ? 0x08 : 0x64,
    };
    memcpy(k.name, p->c.name, sizeof(k.name));
    memcpy(k.mac, p->c.mac, 6);
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    rx_pkt_keep_alive(&w, &k);
    emit(p, RX_LINK_ANNOUNCE, RX_PRESENCE_BROADCAST, &w);
}

static void send_probe(RXPresence *p, int subtype, int number, int counter)
{
    /* As rekordbox 7.2.11 sends it (rbxport's capture): our address and
     * MAC, the number, the round, device type 04, 01. */
    RXPktClaim c = {
        .kind    = RX_ANN_CLAIM2,
        .subtype = subtype,
        .proto   = 0x03,
        .number  = number,
        .counter = counter,
        .type    = 0x04,
        .flag    = 0x01,
        .addr    = p->c.addr,
    };
    memcpy(c.name, p->c.name, sizeof(c.name));
    memcpy(c.mac, p->c.mac, 6);
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    rx_pkt_claim(&w, &c);
    emit(p, RX_LINK_ANNOUNCE, RX_PRESENCE_BROADCAST, &w);
}

static void send_in_use(RXPresence *p, uint32_t to, int counter)
{
    RXPktNumberReply r = {
        .subtype = RX_CLAIM2_PROBE,
        .number  = p->number,
        .counter = counter,
        .status  = 0x01,
    };
    memcpy(r.name, p->c.name, sizeof(r.name));
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    rx_pkt_number_reply(&w, &r);
    emit(p, RX_LINK_ANNOUNCE, to, &w);
}

/* A CDJ defends its number against a device claiming it (djl-analysis
 * startup.adoc, "Channel Conflicts"; beat-link VirtualCdj sends the same):
 * the number and our address, to the claimer. */
static void defend(RXPresence *p, uint32_t to)
{
    RXPktConflict c = {
        .kind   = RX_ANN_CONFLICT,
        .proto  = 0x02,
        .number = p->number,
        .addr   = p->c.addr,
    };
    memcpy(c.name, p->c.name, sizeof(c.name));
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    rx_pkt_conflict(&w, &c);
    emit(p, RX_LINK_ANNOUNCE, to, &w);
}

/* The greeting rekordbox unicasts once to each player's status port, when
 * it is first heard (rbxport: in the capture the player's portmap query
 * follows it by six milliseconds). Once per address: an all-in-one has
 * several numbers at one. */
static void greet(RXPresence *p, Device *d)
{
    bool done = false;
    for (int i = 0; i < p->nb_dev; i++) {
        if (p->dev[i].d.addr == d->d.addr && p->dev[i].greeted)
            done = true;
    }
    if (!done) {
        uint8_t buf[RX_PKT_MAX];
        RXWr w = rx_wr(buf, sizeof(buf));
        rx_pkt_greeting(&w, p->c.name, p->number);
        emit(p, RX_LINK_STATUS, d->d.addr, &w);
    }
    for (int i = 0; i < p->nb_dev; i++) {
        if (p->dev[i].d.addr == d->d.addr)
            p->dev[i].greeted = true;
    }
}

/* rekordbox's status: the tempo master's tempo at its pitch and its beat
 * within the bar, when a player is master with a tempo, otherwise none
 * (rbxport's captures, idle and mirroring a deck at 78.08 BPM; that the
 * beat is the master's is rbxport's reading) [ASSUME for the beat]. */
static void send_status(RXPresence *p)
{
    int bpm = 0, beat = 0;
    for (int i = 0; i < p->nb_dev; i++) {
        const Device *d = &p->dev[i];
        if (d->d.type == RX_LINK_DEVICE_PLAYER && d->d.has_status && d->d.master && d->d.bpm > 0) {
            double t = d->d.bpm * 100.0 * (1.0 + d->d.pitch / 100.0);
            bpm = t > 0 && t < 65535 ? (int)lround(t) : 0;
            beat = bpm ? d->bar_beat : 0;
            break;
        }
    }
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    rx_pkt_mixer_status(&w, p->c.name, p->number, bpm, beat, false);
    emit(p, RX_LINK_STATUS, RX_PRESENCE_BROADCAST, &w);
}

/* The fake player's status, to each address on the link once: players
 * send theirs straight to each device that announces itself (djl-analysis
 * vcdj.adoc, "Creating a Virtual CDJ"). */
static void send_cdj_status(RXPresence *p)
{
    bool loaded = p->track_device != 0;
    RXPktStatus s = {
        .number       = p->number,
        .track_device = p->track_device,
        .track_slot   = loaded ? p->track_slot : 0,
        .track_type   = loaded ? 1 : 0,
        .track_id     = loaded ? p->track_id : 0,
        .playing      = loaded && p->playing,
        .beat         = -1,
    };
    memcpy(s.name, p->c.name, sizeof(s.name));
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    rx_pkt_cdj_status(&w, &s);
    for (int i = 0; i < p->nb_dev; i++) {
        bool seen = false;
        for (int j = 0; j < i; j++)
            seen |= p->dev[j].d.addr == p->dev[i].d.addr;
        if (!seen)
            emit(p, RX_LINK_STATUS, p->dev[i].d.addr, &w);
    }
}

/* ---------------------------------------------------------- the computer */

static void up(RXPresence *p, int number, int64_t now)
{
    p->stage = ST_UP;
    p->number = number;
    p->next_keep_alive = now;
    p->next_status = now;
    if (p->c.role == RX_PRESENCE_COMPUTER)
        rx_info("link: on the link as device %d", number);
    else
        rx_debug("link: player %d on the link", number);
}

static void start_join(RXPresence *p, int64_t now)
{
    p->stage = ST_DISCOVERY;
    p->sent = 0;
    memset(p->in_use, 0, sizeof(p->in_use));
    p->next_join = now;
}

static void join_step(RXPresence *p, int64_t now);

/* After the sixth round: the first of 17 and 18 nobody holds, or a request
 * to be assigned one, sent at once (rbxport's join). */
static void choose(RXPresence *p, int64_t now)
{
    for (int i = 0; i < 2; i++) {
        if (!p->in_use[i]) {
            up(p, candidates[i], now);
            return;
        }
    }
    rx_warn("link: device numbers 17 and 18 are in use; asking to be assigned one");
    p->stage = ST_ASSIGNING;
    p->sent = 0;
    join_step(p, now);
}

static void join_step(RXPresence *p, int64_t now)
{
    switch (p->stage) {
    case ST_DISCOVERY: {
        RXPktClaim c = {
            .kind    = RX_ANN_CLAIM1,
            .proto   = 0x03,
            .counter = ++p->sent,
            .type    = 0x04,
        };
        memcpy(c.name, p->c.name, sizeof(c.name));
        memcpy(c.mac, p->c.mac, 6);
        uint8_t buf[RX_PKT_MAX];
        RXWr w = rx_wr(buf, sizeof(buf));
        rx_pkt_claim(&w, &c);
        emit(p, RX_LINK_ANNOUNCE, RX_PRESENCE_BROADCAST, &w);
        if (p->sent == DISCOVERY_SENDS) {
            p->stage = ST_PROBING;
            p->round = 1;
            p->index = 0;
        }
        p->next_join = now + JOIN_STEP;
        return;
    }
    case ST_PROBING:
        /* All six at round 1, then all six at round 2 ...: counter-major,
         * as rekordbox sends it on a cold start (rbxport, 2026-09-13). */
        while (1) {
            if (p->round > PROBE_ROUNDS) {
                choose(p, now);
                return;
            }
            if (p->index >= 6) {
                p->round++;
                p->index = 0;
                continue;
            }
            if (p->in_use[p->index]) {
                p->index++;
                continue;
            }
            break;
        }
        send_probe(p, RX_CLAIM2_PROBE, candidates[p->index], p->round);
        p->index++;
        p->next_join = now + JOIN_STEP;
        return;
    case ST_ASSIGNING:
        if (p->sent >= ASSIGN_SENDS) {
            rx_warn("link: no device number free and none assigned; waiting");
            p->stage = ST_WAITING;
            p->hold_until = now + RETRY_HOLD;
            return;
        }
        send_probe(p, RX_CLAIM2_ASSIGN, candidates[0], ++p->sent);
        p->next_join = now + JOIN_STEP;
        return;
    default:
        return;
    }
}

static void tick_computer(RXPresence *p, int64_t now)
{
    if (p->stage == ST_WAITING && p->c.start_up && now >= p->hold_until)
        start_join(p, now);
    if (joining(p) && now >= p->next_join)
        join_step(p, now);
    if (p->stage != ST_UP)
        return;
    if (now >= p->next_keep_alive) {
        send_keep_alive(p);
        p->next_keep_alive = later(p->next_keep_alive, RB_KEEP_ALIVE, now);
    }
    if (now >= p->next_status) {
        send_status(p);
        p->next_status = later(p->next_status, STATUS_EVERY, now);
    }
    /* Players heard before we came up. */
    for (int i = 0; i < p->nb_dev; i++) {
        if (p->dev[i].d.type == RX_LINK_DEVICE_PLAYER && !p->dev[i].greeted)
            greet(p, &p->dev[i]);
    }
}

/* ------------------------------------------------------------ the player */

static void send_cdj_claim(RXPresence *p, int i)
{
    static const int kinds[4] = { RX_ANN_HELLO, RX_ANN_CLAIM1, RX_ANN_CLAIM2, RX_ANN_CLAIM3 };
    /* The CDJ-3000's variants (djl-analysis, "Startup with CDJ-3000s"):
     * layout byte 04 and a byte more in the announcement, 03 in the claims;
     * type 01; in the second stage the number, and 02: a chosen one. */
    RXPktClaim c = {
        .kind    = kinds[i / 3],
        .subtype = RX_CLAIM2_PROBE,
        .proto   = i < 3 ? 0x04 : 0x03,
        .number  = p->c.number,
        .counter = i % 3 + 1,
        .type    = 0x01,
        .flag    = 0x02,
        .addr    = p->c.addr,
        .hello   = 0x40,
    };
    memcpy(c.name, p->c.name, sizeof(c.name));
    memcpy(c.mac, p->c.mac, 6);
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    rx_pkt_claim(&w, &c);
    emit(p, RX_LINK_ANNOUNCE, RX_PRESENCE_BROADCAST, &w);
}

static void ask_source(RXPresence *p, int64_t now)
{
    uint8_t buf[RX_PKT_MAX];
    RXWr w = rx_wr(buf, sizeof(buf));
    if (p->src.step == SRC_IDENTITY) {
        rx_pkt_identity_query(&w, p->c.name, p->number);
    } else {
        /* About the computer's collection, its slot 04, as a current
         * CDJ-3000 asks (rbxport's capture). */
        RXPktMediaQuery q = {
            .number = p->number,
            .addr   = p->c.addr,
            .device = p->src.number,
            .slot   = RX_SLOT_REKORDBOX,
        };
        memcpy(q.name, p->c.name, sizeof(q.name));
        rx_pkt_media_query(&w, &q);
    }
    emit(p, RX_LINK_STATUS, p->src.addr, &w);
    p->src.next = now + QUERY_RETRY;
}

static void tick_player(RXPresence *p, int64_t now)
{
    if (p->stage == ST_WAITING) {
        p->stage = ST_CLAIMING;
        p->sent = 0;
        p->next_join = now;
    }
    if (p->stage == ST_CLAIMING && now >= p->next_join) {
        if (p->sent == CDJ_CLAIMS) {
            /* 02 when it came up alone, 01 when another device was there
             * (djl-analysis, CDJ keep-alive byte 0x25). */
            p->first = p->nb_dev ? 0x01 : 0x02;
            up(p, p->c.number, now);
        } else {
            send_cdj_claim(p, p->sent++);
            p->next_join = now + CDJ_STEP;
        }
    }
    if (p->stage != ST_UP)
        return;
    if (now >= p->next_keep_alive) {
        send_keep_alive(p);
        p->next_keep_alive = later(p->next_keep_alive, CDJ_KEEP_ALIVE, now);
    }
    if (now >= p->next_status) {
        send_cdj_status(p);
        p->next_status = later(p->next_status, STATUS_EVERY, now);
    }
    if (p->src.step == SRC_NONE) {
        for (int i = 0; i < p->nb_dev; i++) {
            const Device *d = &p->dev[i];
            if (d->d.type == RX_LINK_DEVICE_COMPUTER) {
                p->src.step = SRC_IDENTITY;
                p->src.number = d->d.number;
                p->src.addr = d->d.addr;
                p->src.next = now;
                break;
            }
        }
    }
    if ((p->src.step == SRC_IDENTITY || p->src.step == SRC_MEDIA) && now >= p->src.next)
        ask_source(p, now);
}

/* ------------------------------------------------------------- the input */

static void hear_keep_alive(RXPresence *p, uint32_t from, const RXPktKeepAlive *k, int64_t now)
{
    /* rekordbox's join reads numbers 1..80 and device types 1..9 only
     * (rbxport, the decompiled hear_keep_alive). */
    if (k->number < 1 || k->number > 80 || k->type < 1 || k->type > 9)
        return;
    bool computer = p->c.role == RX_PRESENCE_COMPUTER;
    if (!computer && p->stage == ST_UP && k->number == p->number) {
        /* beat-link's DeviceFinder defends against a keep-alive on its
         * number too. */
        defend(p, from);
        return;
    }

    Device *d = find(p, k->number);
    if (!d && p->nb_dev < MAX_DEVICES) {
        d = &p->dev[p->nb_dev++];
        *d = (Device) { .d = { .number = k->number, .beat = -1 } };
        rx_debug("link: device %d (%s, type %d) at %u.%u.%u.%u", k->number, k->name, k->type,
                 from >> 24, (from >> 16) & 0xff, (from >> 8) & 0xff, from & 0xff);
    }
    if (d) {
        if (d->d.addr != from)
            d->greeted = false;
        d->d.addr = from;
        d->d.type = rx_pkt_device_kind(k->type);
        d->raw_type = k->type;
        memcpy(d->d.name, k->name, sizeof(k->name));
        memcpy(d->d.mac, k->mac, 6);
        d->d.last_seen = now;
    }
    if (!computer)
        return;

    int i = candidate(k->number);
    if (i >= 0 && (p->stage == ST_DISCOVERY || p->stage == ST_PROBING))
        p->in_use[i] = true;
    /* A player (01, an all-in-one 07) or a mixer (02, a DJM 03) brings the
     * link up; another computer does not (rbxport, the decompiled
     * linkUpFunc). rekordbox also refuses an original CDJ-2000 or CDJ-900
     * unless it knows the link is wired; rxbox takes the wired identities,
     * so it does not. */
    bool brings_up = k->type == 1 || k->type == 2 || k->type == 3 || k->type == 7;
    if (p->stage == ST_WAITING && brings_up && now >= p->hold_until) {
        rx_debug("link: %s (device %d) heard; joining", k->name, k->number);
        start_join(p, now);
    }
    if (p->stage == ST_UP && d && d->d.type == RX_LINK_DEVICE_PLAYER && !d->greeted)
        greet(p, d);
}

static void hear_claim(RXPresence *p, uint32_t from, const RXPktClaim *c, int64_t now)
{
    bool computer = p->c.role == RX_PRESENCE_COMPUTER;
    switch (c->kind) {
    case RX_ANN_CLAIM1:
        /* A device starting over: what it was on the link is forgotten
         * (rbxport's reading of rekordbox's rediscovery, which also limits
         * it to the numbers the type holds). */
        for (int i = p->nb_dev - 1; i >= 0; i--) {
            if (!memcmp(p->dev[i].d.mac, c->mac, 6))
                drop(p, i);
        }
        return;
    case RX_ANN_CLAIM2:
        if (p->stage != ST_UP)
            return;
        if (computer && c->subtype == RX_CLAIM2_PROBE && c->number == p->number) {
            send_in_use(p, from, c->counter);
        } else if (computer && c->subtype == RX_CLAIM2_BLOCK) {
            /* A block names our number by its bit: 17..18 in the wired
             * mask, 41..44 in the wireless one (rbxport, from rekordbox's
             * readIdBlkRequset) [ASSUME: no capture]. */
            bool named = p->number >= 17 && p->number <= 18 ?
                         (c->wired >> (p->number - 17) & 1) :
                         p->number >= 41 && p->number <= 44 ?
                         (c->wireless >> (p->number - 41) & 1) : false;
            if (named)
                send_in_use(p, from, c->counter);
        } else if (!computer && c->subtype == RX_CLAIM2_PROBE && c->number == p->number) {
            defend(p, from);
        }
        return;
    case RX_ANN_CLAIM3:
        if (!computer && p->stage == ST_UP && c->number == p->number)
            defend(p, from);
        return;
    default:
        return;
    }
}

static void hear_reply(RXPresence *p, const RXPktNumberReply *r, int64_t now)
{
    if (p->c.role != RX_PRESENCE_COMPUTER)
        return;
    int i = candidate(r->number);
    if ((p->stage == ST_DISCOVERY || p->stage == ST_PROBING) && r->subtype == RX_CLAIM2_PROBE &&
        r->status == 0x01 && i >= 0) {
        if (!p->in_use[i])
            rx_debug("link: device number %d is held by %s", r->number, r->name);
        p->in_use[i] = true;
    } else if (p->stage == ST_ASSIGNING && r->subtype == RX_CLAIM2_ASSIGN && r->status == 0x00 &&
               r->number >= 1 && r->number <= 80) {
        up(p, r->number, now);
    }
}

static void hear_conflict(RXPresence *p, const RXPktConflict *c, int64_t now)
{
    if (c->kind == RX_ANN_CONFLICT) {
        /* A device defending a number (djl-analysis "Channel Conflicts",
         * beat-link): a number we were about to take is held. */
        int i = candidate(c->number);
        if (p->c.role == RX_PRESENCE_COMPUTER && i >= 0 &&
            (p->stage == ST_DISCOVERY || p->stage == ST_PROBING)) {
            p->in_use[i] = true;
            return;
        }
        if (p->c.role == RX_PRESENCE_PLAYER && p->stage == ST_CLAIMING &&
            c->number == p->c.number) {
            /* A CDJ gives the number up; the fake player has only the one,
             * so it starts over [ASSUME]. */
            rx_debug("link: player number %d defended by %s; starting over", c->number, c->name);
            p->sent = 0;
            p->next_join = now + CDJ_STEP;
            return;
        }
    }
    /* rekordbox reads a disconnect, and a conflict naming another device,
     * as that device leaving (rbxport, the decompiled readers). */
    if (c->number != p->number)
        drop_number(p, c->number);
}

/* A packet in the debug log, its first 0x60 bytes in hex: what a player
 * sends that is not understood, or not answered. */
static void dump(const char *what, uint32_t from, const uint8_t *buf, size_t len)
{
    if (rx_log_level() < RX_LOG_DEBUG)
        return;
    char hex[0x60 * 3 + 1] = "";
    for (size_t i = 0; i < len && i < 0x60; i++)
        snprintf(hex + i * 3, 4, "%02x ", buf[i]);
    rx_debug("link: %s from %u.%u.%u.%u, %zu bytes: %s", what, from >> 24, from >> 16 & 0xFF,
             from >> 8 & 0xFF, from & 0xFF, len, hex);
}

static void announce_in(RXPresence *p, uint32_t from, const uint8_t *buf, size_t len, int64_t now)
{
    RXPktHeader h;
    if (rx_pkt_header(&h, RX_LINK_ANNOUNCE, buf, len) < 0) {
        dump("announce port: not a packet", from, buf, len);
        return;
    }
    switch (h.kind) {
    case RX_ANN_KEEP_ALIVE: {
        RXPktKeepAlive k;
        if (rx_pkt_keep_alive_parse(&k, buf, len) < 0)
            dump("keep-alive not understood", from, buf, len);
        else if (!own_mac(p, k.mac))
            hear_keep_alive(p, from, &k, now);
        return;
    }
    case RX_ANN_CLAIM1:
    case RX_ANN_CLAIM2:
    case RX_ANN_CLAIM3: {
        RXPktClaim c;
        if (rx_pkt_claim_parse(&c, buf, len) == 0 &&
            (c.kind == RX_ANN_CLAIM3 || !own_mac(p, c.mac)))
            hear_claim(p, from, &c, now);
        return;
    }
    case RX_ANN_NUMBER_REPLY: {
        RXPktNumberReply r;
        if (rx_pkt_number_reply_parse(&r, buf, len) == 0)
            hear_reply(p, &r, now);
        return;
    }
    case RX_ANN_DISCONNECT:
    case RX_ANN_CONFLICT: {
        RXPktConflict c;
        if (rx_pkt_conflict_parse(&c, buf, len) == 0)
            hear_conflict(p, &c, now);
        return;
    }
    default:
        dump("announce port", from, buf, len);
        return;
    }
}

static void hear_status(RXPresence *p, uint32_t from, const uint8_t *buf, size_t len, int64_t now)
{
    RXPktStatus s;
    if (rx_pkt_status_parse(&s, buf, len) < 0)
        return;
    Device *d = find(p, s.number);
    if (!d || d->d.addr != from)
        return;
    RXLinkPlayer *l = &d->d;
    l->has_status = true;
    bool cdj = s.kind == RX_ST_CDJ;
    l->track_device = cdj ? s.track_device : 0;
    l->track_slot = cdj ? s.track_slot : 0;
    l->track_type = cdj ? s.track_type : 0;
    l->track_id = cdj ? s.track_id : 0;
    l->playing = cdj && s.playing;
    l->on_air = cdj && s.on_air;
    l->synced = cdj && s.synced;
    l->master = s.master;
    l->bpm = s.bpm_x100 / 100.0;
    l->pitch = s.pitch;
    l->beat = s.beat;
    l->status_us = now;
    d->bar_beat = s.bar_beat;
    /* rekordbox greets a player on its first status too, if its keep-alive
     * did not get there first (rbxport's beacon) [ASSUME]. */
    if (p->c.role == RX_PRESENCE_COMPUTER && p->stage == ST_UP &&
        l->type == RX_LINK_DEVICE_PLAYER && !d->greeted)
        greet(p, d);
}

static void answer_media(RXPresence *p, uint32_t from, const uint8_t *buf, size_t len)
{
    RXPktMediaQuery q;
    if (rx_pkt_media_query_parse(&q, buf, len) < 0) {
        dump("media query not understood", from, buf, len);
        return;
    }
    rx_debug("link: media query from %s (%d) about device %d slot %d%s", q.name, q.number,
             q.device, q.slot,
             q.device != p->number || (q.slot != RX_SLOT_REKORDBOX && q.slot != RX_SLOT_USB)
             ? ": not ours" : "");
    /* About our collection: slot 04, or 03 from older CDJ-3000 firmware
     * (EP122); the answer names back the slot asked about, or the player
     * ignores it (rbxport's captures). Any other is not ours to answer. */
    if (q.device != p->number || (q.slot != RX_SLOT_REKORDBOX && q.slot != RX_SLOT_USB))
        return;
    RXPktMedia m = {
        .number    = p->number,
        .device    = p->number,
        .slot      = q.slot,
        .tracks    = p->nb_tracks,
        .playlists = p->nb_playlists,
    };
    memcpy(m.name, p->c.name, sizeof(m.name));
    uint8_t out[RX_PKT_MAX];
    RXWr w = rx_wr(out, sizeof(out));
    rx_pkt_media(&w, &m);
    emit(p, RX_LINK_STATUS, from, &w);
}

static void status_in(RXPresence *p, uint32_t from, const uint8_t *buf, size_t len, int64_t now)
{
    RXPktHeader h;
    if (rx_pkt_header(&h, RX_LINK_STATUS, buf, len) < 0) {
        dump("status port: not a packet", from, buf, len);
        return;
    }
    if (h.kind == RX_ST_CDJ || h.kind == RX_ST_MIXER) {
        hear_status(p, from, buf, len, now);
        return;
    }
    /* A load command for the fake player: it loads what it names, as a
     * player does, and answers as an NXS2 does. Whoever sends it, as long
     * as it is on the link. */
    if (h.kind == RX_ST_LOAD_TRACK && p->c.role == RX_PRESENCE_PLAYER) {
        RXPktLoad l;
        if (rx_pkt_load_track_parse(&l, buf, len) < 0 || l.target != p->number ||
            !rx_presence_is_peer(p, from))
            return;
        rx_presence_set_loaded(p, l.device, l.slot, l.id, p->playing);
        uint8_t ack[RX_PKT_MAX];
        RXWr aw = rx_wr(ack, sizeof(ack));
        rx_pkt_load_ack(&aw, p->c.name, p->number);
        emit(p, RX_LINK_STATUS, from, &aw);
        return;
    }
    if (h.kind == RX_ST_LOAD_ACK && p->c.role == RX_PRESENCE_COMPUTER) {
        rx_debug("link: %s (%d) took the load", h.name, h.number);
        return;
    }
    if (p->c.role == RX_PRESENCE_COMPUTER)
        dump(p->stage != ST_UP ? "status port, not up yet" :
             !rx_presence_is_peer(p, from) ? "status port, not a peer" : "status port", from,
             buf, len);

    uint8_t out[RX_PKT_MAX];
    RXWr w = rx_wr(out, sizeof(out));
    if (p->c.role == RX_PRESENCE_PLAYER) {
        if (p->src.step == SRC_NONE || from != p->src.addr)
            return;
        if (h.kind == RX_ST_IDENTITY && p->src.step == SRC_IDENTITY) {
            /* Only then does a player go on to the media query (rbxport);
             * it asks for the display settings too (the CDJ-3000 did). */
            p->src.step = SRC_MEDIA;
            rx_pkt_settings_query(&w, p->c.name, p->number);
            emit(p, RX_LINK_STATUS, from, &w);
            ask_source(p, now);
        } else if (h.kind == RX_ST_MEDIA_RESPONSE && p->src.step == SRC_MEDIA) {
            RXPktMedia m;
            if (rx_pkt_media_parse(&m, buf, len) < 0 || m.device != p->src.number)
                return;
            p->src.step = SRC_DONE;
            p->src.tracks = m.tracks;
            p->src.playlists = m.playlists;
        }
        return;
    }

    /* Nothing is answered before we are up (rbxport: rekordbox's receive
     * path is gated until then), nor to a device not on the link. */
    if (p->stage != ST_UP || !rx_presence_is_peer(p, from))
        return;
    const char *host = p->c.host[0] ? p->c.host : p->c.name;
    switch (h.kind) {
    case RX_ST_MEDIA_QUERY:
        answer_media(p, from, buf, len);
        return;
    case RX_ST_IDENTITY_QUERY:
        rx_pkt_identity(&w, p->c.name, p->number, host);
        break;
    case RX_ST_SETTINGS_QUERY:
        rx_pkt_settings(&w, p->c.name, p->number, p->devsetting);
        break;
    default:
        return;
    }
    emit(p, RX_LINK_STATUS, from, &w);
}

/* ------------------------------------------------------------- public API */

int rx_presence_new(RXPresence **out, const RXPresenceConfig *c, RXPresenceSend send,
                    void *opaque)
{
    if (!out || !c || !send)
        return RX_ERROR(EINVAL);
    *out = NULL;
    if (c->role != RX_PRESENCE_COMPUTER && c->role != RX_PRESENCE_PLAYER)
        return RX_ERROR(EINVAL);
    if (c->role == RX_PRESENCE_PLAYER && (c->number < 1 || c->number > 80))
        return RX_ERROR(EINVAL);
    RXPresence *p = calloc(1, sizeof(*p));
    if (!p)
        return RX_ERROR(ENOMEM);
    p->c = *c;
    p->c.name[sizeof(p->c.name) - 1] = 0;
    p->c.host[sizeof(p->c.host) - 1] = 0;
    if (!p->c.name[0])
        snprintf(p->c.name, sizeof(p->c.name), "%s",
                 c->role == RX_PRESENCE_COMPUTER ? "rekordbox" : "CDJ-3000");
    p->send = send;
    p->opaque = opaque;
    p->stage = ST_WAITING;
    p->hold_until = INT64_MIN;
    memcpy(p->devsetting, default_devsetting, sizeof(p->devsetting));
    *out = p;
    return 0;
}

void rx_presence_free(RXPresence **p)
{
    if (!p)
        return;
    free(*p);
    *p = NULL;
}

void rx_presence_input(RXPresence *p, enum RXLinkPort port, uint32_t from, const uint8_t *buf,
                       size_t len, int64_t now)
{
    /* Our own broadcasts come back to our own sockets (the service drops
     * them too). */
    if (!buf || (p->c.addr && from == p->c.addr))
        return;
    if (port == RX_LINK_ANNOUNCE)
        announce_in(p, from, buf, len, now);
    else if (port == RX_LINK_STATUS)
        status_in(p, from, buf, len, now);
    check_source(p);
}

int64_t rx_presence_tick(RXPresence *p, int64_t now)
{
    forget(p, now);
    if (p->c.role == RX_PRESENCE_COMPUTER)
        tick_computer(p, now);
    else
        tick_player(p, now);

    int64_t next = now + IDLE;
    if (joining(p) || p->stage == ST_CLAIMING)
        next = RX_MIN(next, p->next_join);
    if (p->stage == ST_WAITING && p->c.role == RX_PRESENCE_COMPUTER && p->c.start_up &&
        p->hold_until > now)
        next = RX_MIN(next, p->hold_until);
    if (p->stage == ST_UP) {
        next = RX_MIN(next, p->next_keep_alive);
        next = RX_MIN(next, p->next_status);
        if (p->src.step == SRC_IDENTITY || p->src.step == SRC_MEDIA)
            next = RX_MIN(next, p->src.next);
    }
    for (int i = 0; i < p->nb_dev; i++)
        next = RX_MIN(next, p->dev[i].d.last_seen + FORGET + 1);
    return next > now ? next : now + 1;
}

enum RXLinkState rx_presence_state(const RXPresence *p, int *number)
{
    if (number)
        *number = p->stage == ST_UP ? p->number : 0;
    switch (p->stage) {
    case ST_UP:
        return RX_LINK_UP;
    case ST_WAITING:
        return RX_LINK_WAITING;
    default:
        return RX_LINK_JOINING;
    }
}

bool rx_presence_is_peer(const RXPresence *p, uint32_t addr)
{
    for (int i = 0; i < p->nb_dev; i++) {
        if (p->dev[i].d.addr == addr)
            return true;
    }
    return false;
}

static bool before(const RXLinkPlayer *a, const RXLinkPlayer *b)
{
    bool pa = a->type == RX_LINK_DEVICE_PLAYER, pb = b->type == RX_LINK_DEVICE_PLAYER;
    return pa != pb ? pa : a->number < b->number;
}

int rx_presence_players(const RXPresence *p, RXLinkPlayer *out, int max)
{
    int order[MAX_DEVICES];
    for (int i = 0; i < p->nb_dev; i++) {
        int j = i;
        while (j > 0 && before(&p->dev[i].d, &p->dev[order[j - 1]].d)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for (int i = 0; out && i < p->nb_dev && i < max; i++)
        out[i] = p->dev[order[i]].d;
    return p->nb_dev;
}

void rx_presence_set_loaded(RXPresence *p, int track_device, int slot, uint32_t track_id,
                            bool playing)
{
    p->track_device = RX_CLAMP(track_device, 0, 0xff);
    p->track_slot = RX_CLAMP(slot, 0, 0xff);
    p->track_id = track_id;
    p->playing = playing;
}

int rx_presence_load(RXPresence *p, int player, uint32_t track_id)
{
    if (p->c.role != RX_PRESENCE_COMPUTER || p->stage != ST_UP)
        return RX_ERROR(EAGAIN);
    Device *d = find(p, player);
    if (!d || d->d.type != RX_LINK_DEVICE_PLAYER)
        return RX_ERROR(ENOENT);
    RXPktLoad l = {
        .number = p->number, .target = player,
        .device = p->number, .slot = RX_SLOT_REKORDBOX, .type = 1, .id = track_id,
    };
    snprintf(l.name, sizeof(l.name), "%s", p->c.name);
    uint8_t out[RX_PKT_MAX];
    RXWr w = rx_wr(out, sizeof(out));
    rx_pkt_load_track(&w, &l);
    emit(p, RX_LINK_STATUS, d->d.addr, &w);
    rx_debug("link: asked %s (%d) to load track %u", d->d.name, player, track_id);
    return 0;
}

void rx_presence_set_media(RXPresence *p, int nb_tracks, int nb_playlists)
{
    p->nb_tracks = RX_CLAMP(nb_tracks, 0, 0xffff);
    p->nb_playlists = RX_CLAMP(nb_playlists, 0, 0xffff);
}

void rx_presence_set_devsetting(RXPresence *p, const uint8_t payload[32])
{
    memcpy(p->devsetting, payload, sizeof(p->devsetting));
}

bool rx_presence_source(const RXPresence *p, int *number, uint32_t *addr, int *nb_tracks,
                        int *nb_playlists)
{
    bool done = p->src.step == SRC_DONE;
    if (number)
        *number = done ? p->src.number : 0;
    if (addr)
        *addr = done ? p->src.addr : 0;
    if (nb_tracks)
        *nb_tracks = done ? p->src.tracks : 0;
    if (nb_playlists)
        *nb_playlists = done ? p->src.playlists : 0;
    return done;
}
