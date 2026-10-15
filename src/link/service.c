/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The service thread of rxbox/link.h: every socket Link uses, on one
 * interface, in one poll loop, handing what arrives to the codecs
 * (presence.h, dbserver.h, nfs.h), which never see a socket.
 *
 * The fences of docs/LINK.md, "Security", are kept here:
 *  - one interface: every socket is tied to it (SO_BINDTODEVICE, which
 *    Linux allows without privileges on an unbound socket since 5.7), and
 *    a datagram whose arrival interface (IP_PKTINFO) is another is dropped
 *    all the same, for kernels that refuse the option; the TCP listeners
 *    and the NFS sockets are bound to the interface's address alone;
 *  - only players: the database server and NFS answer only addresses that
 *    sent a keep-alive (rx_presence_is_peer), or did in the last half
 *    hour (a deck drops off the device list after 10 s without one, and
 *    one busy loading, or behind a switch that rebooted or a cable that
 *    was reseated, comes back mid-load, reading on), unless `any_peer`
 *    says otherwise for a diagnostic client;
 *  - bounded: a fixed number of connections, a cap on what a connection
 *    may have waiting to be sent before it is read no more.
 * Our own broadcasts come back to our presence sockets (Linux delivers a
 * broadcast to local sockets bound to the port); datagrams from our own
 * address are dropped before presence sees them.
 *
 * What is served comes from the library through a connection of the
 * thread's own, read only: a dry export plan over every track whose file
 * is there and every playlist (the paths players are given, the files
 * behind them), the database catalog over it, and the NFS tree over it.
 * A library change builds all three again on a thread of its own, the
 * old ones served meanwhile, and swaps them in between two requests;
 * sessions see the catalog through a pointer to the current one and NFS
 * handles name tracks by id, so nothing a player holds goes stale. */

#include "rxbox/link.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rxbox/common.h"
#include "rxbox/export.h"
#include "rxbox/formats.h"
#include "rxbox/library.h"

#include "dbserver.h"
#include "link_priv.h"
#include "net.h"
#include "nfs.h"
#include "presence.h"
#include "wav.h"

/* What the tracks Link serves as WAVs made as they are read may keep open
 * (their packets, about the size of their files). */
#define WAV_MEMORY ((size_t)1 << 30)

/* Players open a connection or two each (the browser's and a load's); a
 * full network of six players and a mixer stays well inside. */
#define MAX_CONNS       32
/* What one connection may have waiting to be sent before its requests are
 * read no more: a few detail waveforms. */
#define MAX_PENDING     (8 << 20)
/* A port query is a 4-byte length and "RemoteDBServer\0": anything longer
 * than this without being one is not one. */
#define QUERY_MAX       64
/* Datagrams handled per socket per turn of the loop, so a flood on one
 * cannot starve the others. */
#define DRAIN_MAX       64
#define MAX_PLAYERS     64

enum Sock {
    S_ANNOUNCE,
    S_STATUS,
    S_QUERY,
    S_DB,
    S_PORTMAP,
    S_MOUNT,
    S_NFS,
    NB_SOCKS,
};

enum ConnKind {
    CONN_QUERY,
    CONN_DB,
};

typedef struct Conn {
    int fd;
    enum ConnKind kind;
    uint32_t peer;
    uint8_t q[QUERY_MAX];
    size_t qlen;
    RXDbSession *s;
    RXWr out;                   /* replies not sent yet, from `sent` on */
    size_t sent;
    bool closing;               /* close once `out` is sent */
} Conn;

typedef struct Served {
    RXLibrary *lib;
    RXPlan *plan;
    RXCatalog *cat;
    RXVTree *tree;
} Served;

/* Edits players made lately, kept this long to be applied again to the
 * library read again (reload_done): one read may not have them yet. */
#define RECENT_EDITS 64
#define RECENT_US    10000000

struct RXLink {
    /* Set by rx_link_start, read only after. */
    char *root, *name;
    bool any_peer, start_up;
    int test_build_ms;
    RXLinkIf ifc;
    RXLinkPorts ports;          /* as bound */
    RXLinkPorts peer;
    int fds[NB_SOCKS];
    /* The status broadcast leaves from an ephemeral port, as rekordbox's
     * does; replies on 50002 from ours, which players answer to (rbxport's
     * captures of rekordbox 7.2.11). */
    int status_out;
    int wake;                   /* eventfd: stop, reload, the devices changed */
    int pwake;                  /* eventfd: stop the presence thread */
    pthread_t thread, pthread;
    bool running, prunning;
    atomic_bool stop, reload;

    /* Presence, on a thread of its own: keep-alives must go out every 2 s
     * whatever the service thread is doing (a track info request can make
     * a transcode first, seconds). Every call into it holds `plock`. */
    pthread_mutex_t plock;
    RXPresence *pres;
    uint8_t pdgram[2048];

    /* The service thread's. */
    RXLibrary *lib;
    RXPlan *plan;
    RXCatalog *cat;
    RXVTree *tree;
    RXNfs *nfs;
    RXLinkWav *wav;             /* the WAVs made as they are read (wav.h) */
    bool serving;               /* the NFS tree is offered (the link is up) */
    /* A library change read on a thread of its own (builder) while this
     * one goes on answering, then swapped in (reload_done). */
    pthread_t bthread;
    bool building;              /* bthread runs, or is done and not joined */
    atomic_bool built;          /* its result is in `fresh` */
    Served fresh;
    int fresh_ret;
    int64_t build_start;
    struct { RXLinkEdit e; int64_t at; } recent[RECENT_EDITS];
    int nb_recent;
    RXLinkTagList *tags;        /* the owner's, or ours (own_tags) */
    bool own_tags;
    uint32_t peers[MAX_PLAYERS];    /* the addresses on the link at the last look */
    int nb_peers;
    /* Every address that was on the link, and when last: still answered
     * for KNOWN_FOR after it drops off (the oldest forgotten first). */
    struct { uint32_t addr; int64_t seen; } known[MAX_PLAYERS];
    int nb_known;
    Conn conns[MAX_CONNS];
    int nb_conns;
    uint8_t dgram[65536];
    uint8_t reply[RX_NFS_MAX_DATAGRAM];

    /* What the owner reads, under `lock`. */
    pthread_mutex_t lock;
    enum RXLinkState state;
    int number;
    RXLinkPlayer players[MAX_PLAYERS];
    int nb_players;
    /* Cues players saved, for the owner to apply (rx_link_edits). */
    RXLinkEdit edits[RX_LINK_EDITS_KEPT];
    int nb_edits;
    /* Tracks the owner changed, to be read again (rx_link_tracks_changed);
     * more than fit is the whole library. */
    uint64_t changed[RX_LIBRARY_CHANGES_KEPT];
    int nb_changed;
};

void rx_link_options_default(RXLinkOptions *o)
{
    *o = (RXLinkOptions) {
        .name = "rekordbox",
        .start_up = true,
        .ports = {
            .announce = RX_LINK_PORT_ANNOUNCE,
            .status   = RX_LINK_PORT_STATUS,
            .query    = RX_LINK_PORT_QUERY,
            .portmap  = RX_LINK_PORT_PORTMAP,
            .nfs      = RX_LINK_PORT_NFS,
        },
    };
    o->peer_ports = o->ports;
}

/* --------------------------------------------------------------- sockets */

static int open_sockets(RXLink *l, const RXLinkPorts *want)
{
    static const char *const names[NB_SOCKS] = {
        "announce", "status", "port query", "database", "portmap", "mount", "NFS",
    };
    struct { uint16_t want, *bound; bool tcp, any; } s[NB_SOCKS] = {
        [S_ANNOUNCE] = { want->announce, &l->ports.announce, false, true },
        [S_STATUS]   = { want->status,   &l->ports.status,   false, true },
        [S_QUERY]    = { want->query,    &l->ports.query,    true,  false },
        [S_DB]       = { want->database, &l->ports.database, true,  false },
        [S_PORTMAP]  = { want->portmap,  &l->ports.portmap,  false, false },
        [S_MOUNT]    = { want->mount,    &l->ports.mount,    false, false },
        [S_NFS]      = { want->nfs,      &l->ports.nfs,      false, false },
    };
    /* The presence ports hear broadcasts, so they are bound to any
     * address (and tied to the interface); the rest to its address. */
    for (int i = 0; i < NB_SOCKS; i++) {
        int fd = s[i].tcp ? rx_link_tcp_listen(&l->ifc, s[i].want, s[i].bound) :
                            rx_link_udp(&l->ifc, s[i].any ? INADDR_ANY : l->ifc.addr, s[i].want,
                                        s[i].bound);
        if (fd < 0) {
            rx_err("link: cannot bind the %s port %u on %s: %s%s", names[i], s[i].want, l->ifc.name,
                   strerror(-fd), fd == RX_ERROR(EADDRINUSE) ?
                   " (is rekordbox or another rxbox serving?)" : "");
            return fd;
        }
        l->fds[i] = fd;
    }
    return 0;
}

static void presence_send(void *opaque, enum RXLinkPort port, uint32_t to, const uint8_t *buf,
                          size_t len)
{
    RXLink *l = opaque;
    if (port == RX_LINK_ANNOUNCE)
        rx_link_send(l->fds[S_ANNOUNCE], to == RX_PRESENCE_BROADCAST ? l->ifc.bcast : to,
                   l->peer.announce, buf, len);
    else if (to == RX_PRESENCE_BROADCAST)
        rx_link_send(l->status_out, l->ifc.bcast, l->peer.status, buf, len);
    else
        rx_link_send(l->fds[S_STATUS], to, l->peer.status, buf, len);
}

/* How long an address is still answered once it has dropped off. */
#define KNOWN_FOR (30 * 60 * INT64_C(1000000))

static bool admitted(RXLink *l, uint32_t peer)
{
    if (l->any_peer)
        return true;
    pthread_mutex_lock(&l->plock);
    bool ok = rx_presence_is_peer(l->pres, peer);
    pthread_mutex_unlock(&l->plock);
    int64_t now = rx_time_us();
    for (int i = 0; !ok && i < l->nb_known; i++)
        ok = l->known[i].addr == peer && now - l->known[i].seen < KNOWN_FOR;
    return ok;
}

static void known_seen(RXLink *l, uint32_t addr, int64_t now)
{
    int at = -1, old = 0;
    for (int i = 0; i < l->nb_known && at < 0; i++)
        if (l->known[i].addr == addr)
            at = i;
    if (at < 0 && l->nb_known < MAX_PLAYERS) {
        at = l->nb_known++;
    } else if (at < 0) {
        for (int i = 1; i < l->nb_known; i++)
            if (l->known[i].seen < l->known[old].seen)
                old = i;
        at = old;
    }
    l->known[at].addr = addr;
    l->known[at].seen = now;
}

/* ------------------------------------------------------------ the library */

static void served_free(Served *s)
{
    rx_catalog_free(&s->cat);
    rx_vtree_free(&s->tree);
    if (s->plan) {
        rx_plan_free(s->plan);
        free(s->plan);
    }
    if (s->lib) {
        rx_library_close(s->lib);
        free(s->lib);
    }
    *s = (Served) { 0 };
}

/* A build stops as soon as the service does: a library of thousands of
 * tracks takes a moment even without reading audio. */
static bool still_wanted(void *opaque, const char *stage, int done, int total, const char *item)
{
    RXLink *l = opaque;
    return !atomic_load(&l->stop);
}

/* The WAV of a track served so, for the catalog and NFS, both on the
 * service thread (and the catalog's first on the starting thread, before
 * the service thread is). */
static int wav_check(void *opaque, const RXPlanTrack *pt)
{
    RXLink *l = opaque;
    return rx_link_wav_check(l->wav, pt);
}

static ssize_t wav_read(void *opaque, const RXPlanTrack *pt, uint64_t offset, uint8_t *dst,
                        size_t n)
{
    RXLink *l = opaque;
    return rx_link_wav_read(l->wav, pt, offset, dst, n);
}

/* A cue a player saved, applied to what is served already (catalog.c):
 * kept for the owner, who writes the library (rx_link_edits). */
static void edited(void *opaque, const RXLinkEdit *e)
{
    RXLink *l = opaque;
    if (l->nb_recent == RECENT_EDITS) {
        memmove(l->recent, l->recent + 1, (RECENT_EDITS - 1) * sizeof(*l->recent));
        l->nb_recent--;
    }
    l->recent[l->nb_recent].e = *e;
    l->recent[l->nb_recent++].at = rx_time_us();
    pthread_mutex_lock(&l->lock);
    if (l->nb_edits == RX_LINK_EDITS_KEPT) {
        rx_warn("link: a cue player %d saved was not taken in time and is dropped",
                l->edits[0].player);
        memmove(l->edits, l->edits + 1, (RX_LINK_EDITS_KEPT - 1) * sizeof(*l->edits));
        l->nb_edits--;
    }
    l->edits[l->nb_edits++] = *e;
    pthread_mutex_unlock(&l->lock);
}

/* The library opened again, the plan over all of it, the catalog and the
 * tree over the plan. Tracks whose file is gone are left out (a player
 * could not read them). The plan reads no audio (RX_PLAN_STORED: the
 * library's probes and thumbnails, a stat per file) and makes no
 * transcodes: the catalog makes one when a player asks for the track
 * (docs/LINK.md, section 4). */
static int served_build(RXLink *l, Served *s)
{
    *s = (Served) { 0 };
    s->lib = calloc(1, sizeof(*s->lib));
    s->plan = calloc(1, sizeof(*s->plan));
    if (!s->lib || !s->plan) {
        free(s->lib);
        free(s->plan);
        *s = (Served) { 0 };
        return RX_ERROR(ENOMEM);
    }
    int ret = rx_library_open(s->lib, l->root);
    if (ret < 0) {
        free(s->lib);
        free(s->plan);
        *s = (Served) { 0 };
        return ret;
    }
    RXLibrary *lib = s->lib;
    int nb_pls = rx_library_playlists_flat(lib, NULL, 0);
    RXTrack **tracks = malloc((size_t)RX_MAX(lib->nb_tracks, 1) * sizeof(*tracks));
    RXPlaylist **pls = calloc((size_t)RX_MAX(nb_pls, 1), sizeof(*pls));
    if (!tracks || !pls) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    nb_pls = rx_library_playlists_flat(lib, pls, RX_MAX(nb_pls, 0));
    int nb = 0;
    for (int i = 0; i < lib->nb_tracks; i++) {
        if (!lib->tracks[i]->missing && lib->tracks[i]->id <= UINT32_MAX)
            tracks[nb++] = lib->tracks[i];
    }
    /* Every track as all the players play it: as a 16-bit WAV decoded as
     * a player reads it (wav.h), what cannot be so as a WAV made when a
     * player loads the track; or with link.mp3 anything but an MP3 as an
     * MP3 made then (RX_TRANSCODE_LINK). */
    ret = rx_plan_build_cb(lib, tracks, nb, (const RXPlaylist *const *)pls, nb_pls, l->name,
                           lib->cfg.link_mp3 ? RX_TRANSCODE_LINK_MP3 : RX_TRANSCODE_LINK,
                           RX_PLAN_STORED, s->plan, still_wanted, l);
    if (ret < 0) {
        free(s->plan);
        s->plan = NULL;
        goto fail;
    }
    if ((ret = rx_catalog_new(&s->cat, lib, s->plan)) < 0 ||
        (ret = rx_vtree_new(&s->tree, s->plan)) < 0)
        goto fail;
    rx_catalog_set_check(s->cat, wav_check, l);
    rx_catalog_set_edit(s->cat, edited, l);
    rx_catalog_set_tag_list(s->cat, l->tags);
    free(tracks);
    free(pls);
    return 0;
fail:
    free(tracks);
    free(pls);
    served_free(s);
    return ret;
}

/* What presence tells players of the library: the counts in the media
 * response, and the display settings (DEVSETTING.DAT's payload, docs/
 * settings.md section 6) when the library has device settings; without
 * them presence answers what rekordbox 7.2.11 did. */
static void presence_library(RXLink *l)
{
    int nb_lists = 0;
    for (int i = 0; i < l->plan->nb_playlists; i++)
        nb_lists += !l->plan->playlists[i].is_folder;
    RXSettingsFile files[4];
    int nb = 0;
    if (rx_settings_from_config(&l->lib->cfg, files, &nb) < 0)
        nb = 0;
    pthread_mutex_lock(&l->plock);
    rx_presence_set_media(l->pres, l->plan->nb_tracks, nb_lists);
    for (int i = 0; i < nb; i++) {
        if (!strcmp(files[i].name, "DEVSETTING.DAT") && files[i].size >= 0x88)
            rx_presence_set_devsetting(l->pres, files[i].data + 0x68);
    }
    pthread_mutex_unlock(&l->plock);
    for (int i = 0; i < nb; i++)
        free(files[i].data);
}

static void served_take(RXLink *l, Served *s)
{
    l->lib = s->lib;
    l->plan = s->plan;
    l->cat = s->cat;
    l->tree = s->tree;
}

/* A library change: everything built again on a thread of its own, the
 * builder, while the service thread goes on answering players (a library
 * of 1,400 tracks takes 0.5 to 1.3 s: every database answer and NFS read
 * waited that long when it was built here), then swapped in between two
 * requests (reload_done). On failure the old stays served. */
static void wake(int fd);

static void *builder(void *arg)
{
    RXLink *l = arg;
    if (l->test_build_ms > 0)
        usleep((useconds_t)l->test_build_ms * 1000);
    l->fresh_ret = served_build(l, &l->fresh);
    atomic_store(&l->built, true);
    wake(l->wake);
    return NULL;
}

static void reload_start(RXLink *l)
{
    l->fresh = (Served) { 0 };
    l->build_start = rx_time_us();
    if (pthread_create(&l->bthread, NULL, builder, l) != 0) {
        rx_warn("link: the library changed but cannot be read again: no thread");
        return;
    }
    l->building = true;
}

static void reload_done(RXLink *l)
{
    pthread_join(l->bthread, NULL);
    l->building = false;
    Served s = l->fresh;
    l->fresh = (Served) { 0 };
    if (l->fresh_ret == RX_ERROR(ECANCELED))
        return;
    if (l->fresh_ret < 0) {
        rx_warn("link: the library changed but cannot be read again: %s",
                strerror(-l->fresh_ret));
        return;
    }
    /* What players changed lately: the owner may not have saved it before
     * the library was read (rx_catalog_apply_edit changes nothing twice). */
    for (int i = 0; i < l->nb_recent; i++) {
        if (l->recent[i].at >= l->build_start - RECENT_US &&
            rx_catalog_apply_edit(s.cat, &l->recent[i].e) == RX_ERROR(ENOMEM))
            rx_warn("link: out of memory applying an edit of player %d again",
                    l->recent[i].e.player);
    }
    Served old = { .lib = l->lib, .plan = l->plan, .cat = l->cat, .tree = l->tree };
    served_take(l, &s);
    if (l->serving)
        rx_nfs_set_tree(l->nfs, l->tree);
    presence_library(l);
    served_free(&old);
    rx_info("link: serving %d tracks again (read in %.0f ms)", l->plan->nb_tracks,
            (double)(rx_time_us() - l->build_start) / 1000.0);
}

/* Tracks the owner changed: each read again from the library and put in
 * place (rx_catalog_update_track), in a few ms, unless the plan itself
 * would change (a new name, a key, a file), which the whole library read
 * again on the builder does; so does a change while one is read, which
 * may have been read before it. */
static void tracks_changed(RXLink *l)
{
    uint64_t ids[RX_LIBRARY_CHANGES_KEPT];
    pthread_mutex_lock(&l->lock);
    int nb = l->nb_changed;
    memcpy(ids, l->changed, (size_t)nb * sizeof(*ids));
    l->nb_changed = 0;
    pthread_mutex_unlock(&l->lock);
    if (!nb)
        return;
    if (l->building || !l->cat) {
        atomic_store(&l->reload, true);
        return;
    }
    int64_t t0 = rx_time_us();
    int done = 0;
    for (int i = 0; i < nb; i++) {
        int err = 0;
        RXTrack *fresh = rx_library_read_track(l->lib, ids[i], &err);
        int ret = fresh ? rx_catalog_update_track(l->cat, fresh) : err;
        rx_track_free(fresh);
        if (ret != 0) {
            if (ret < 0 && ret != RX_ERROR(ENOENT))
                rx_warn("link: track %llu cannot be read again: %s", (unsigned long long)ids[i],
                        strerror(-ret));
            atomic_store(&l->reload, true);
            return;
        }
        done++;
    }
    rx_info("link: %d track%s read again (%.1f ms)", done, done == 1 ? "" : "s",
            (double)(rx_time_us() - t0) / 1000.0);
}

/* ----------------------------------------------------------- connections */

static void conn_close(RXLink *l, int i)
{
    Conn *c = &l->conns[i];
    close(c->fd);
    rx_dbsession_free(&c->s);
    rx_wr_free(&c->out);
    l->conns[i] = l->conns[--l->nb_conns];
}

static void conn_accept(RXLink *l, enum ConnKind kind)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    int fd = accept4(l->fds[kind == CONN_QUERY ? S_QUERY : S_DB], (struct sockaddr *)&sa, &len,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    uint32_t peer = ntohl(sa.sin_addr.s_addr);
    char a[16];
    rx_link_addr_text(peer, a);
    if (!admitted(l, peer) || l->nb_conns == MAX_CONNS) {
        rx_debug("link: %s connection from %s refused (%s)", kind == CONN_QUERY ? "query" : "database",
                 a, l->nb_conns == MAX_CONNS ? "too many" : "not a player on the link");
        close(fd);
        return;
    }
    Conn *c = &l->conns[l->nb_conns];
    *c = (Conn) { .fd = fd, .kind = kind, .peer = peer, .out = rx_wr_dyn() };
    if (kind == CONN_DB) {
        int number = 0;
        rx_link_state(l, &number);
        if (rx_dbsession_new(&c->s, &l->cat, number) < 0) {
            close(fd);
            return;
        }
        /* A CDJ-900 shows only Latin-1: its texts as Latin-1, romaji and
         * all, by the name its keep-alives give. */
        pthread_mutex_lock(&l->lock);
        for (int i = 0; i < l->nb_players; i++)
            if (l->players[i].addr == peer && !strcmp(l->players[i].name, "CDJ-900"))
                rx_dbsession_set_latin1(c->s, true);
        pthread_mutex_unlock(&l->lock);
    }
    l->nb_conns++;
    rx_debug("link: %s connection from %s", kind == CONN_QUERY ? "query" : "database", a);
}

/* Sends what is waiting; false when the connection is to go. */
static bool conn_flush(Conn *c)
{
    while (c->sent < c->out.len) {
        ssize_t n = send(c->fd, c->out.p + c->sent, c->out.len - c->sent,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        c->sent += (size_t)n;
    }
    c->out.len = c->sent = 0;
    return !c->closing;
}

/* What arrived on a connection; false when it is to go. */
static bool conn_read(RXLink *l, Conn *c)
{
    uint8_t buf[16384];
    ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0)
        return false;
    if (n < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    if (c->kind == CONN_QUERY) {
        if ((size_t)n > sizeof(c->q) - c->qlen)
            return false;
        memcpy(c->q + c->qlen, buf, (size_t)n);
        c->qlen += (size_t)n;
        int ret = rx_dbquery_parse(c->q, c->qlen);
        if (ret < 0)
            return false;
        if (ret > 0) {
            /* A player asks once and goes: answered, the connection closes
             * when the answer is out (djl-analysis track_metadata.adoc,
             * "connecting to the database server"). */
            uint8_t reply[2];
            rx_dbquery_reply(l->ports.database, reply);
            rx_wr_bytes(&c->out, reply, sizeof(reply));
            c->closing = true;
        }
    } else {
        if (rx_dbsession_input(c->s, buf, (size_t)n, &c->out) < 0)
            c->closing = true;
    }
    if (c->out.err)
        return false;
    return conn_flush(c);
}

/* ------------------------------------------------------------- datagrams */

static void drain_presence(RXLink *l, enum Sock s)
{
    for (int k = 0; k < DRAIN_MAX; k++) {
        uint32_t from;
        uint16_t port;
        ssize_t n = rx_link_recv(&l->ifc, l->fds[s], l->pdgram, sizeof(l->pdgram), &from, &port);
        if (n < 0)
            break;
        if (!n || from == l->ifc.addr)
            continue;
        pthread_mutex_lock(&l->plock);
        rx_presence_input(l->pres, s == S_ANNOUNCE ? RX_LINK_ANNOUNCE : RX_LINK_STATUS, from,
                          l->pdgram, (size_t)n, rx_time_us());
        pthread_mutex_unlock(&l->plock);
    }
}

static void drain_nfs(RXLink *l, enum Sock s)
{
    static const enum RXNfsService svc[NB_SOCKS] = {
        [S_PORTMAP] = RX_NFS_PORTMAP, [S_MOUNT] = RX_NFS_MOUNT, [S_NFS] = RX_NFS_NFS,
    };
    for (int k = 0; k < DRAIN_MAX; k++) {
        uint32_t from;
        uint16_t port;
        ssize_t n = rx_link_recv(&l->ifc, l->fds[s], l->dgram, sizeof(l->dgram), &from, &port);
        if (n < 0)
            break;
        if (!n || !admitted(l, from))
            continue;
        ssize_t r = rx_nfs_handle(l->nfs, svc[s], from, l->dgram, (size_t)n, l->reply,
                                  sizeof(l->reply));
        if (r > 0)
            rx_link_send(l->fds[s], from, port, l->reply, (size_t)r);
    }
}

/* ------------------------------------------------------------ the threads */

static void wake(int fd)
{
    uint64_t v = 1;
    if (write(fd, &v, sizeof(v)) < 0)
        rx_debug("link: wake: %s", strerror(errno));
}

/* The state and the devices for the owner and the service thread; the
 * service thread is woken when they change. */
static void publish(RXLink *l)
{
    RXLinkPlayer players[MAX_PLAYERS];
    int number;
    pthread_mutex_lock(&l->plock);
    enum RXLinkState state = rx_presence_state(l->pres, &number);
    int nb = RX_MIN(rx_presence_players(l->pres, players, MAX_PLAYERS), MAX_PLAYERS);
    pthread_mutex_unlock(&l->plock);

    pthread_mutex_lock(&l->lock);
    bool changed = state != l->state || nb != l->nb_players;
    for (int i = 0; i < nb && !changed; i++)
        changed = players[i].addr != l->players[i].addr;
    l->state = state;
    l->number = number;
    l->nb_players = nb;
    memcpy(l->players, players, (size_t)nb * sizeof(*players));
    pthread_mutex_unlock(&l->lock);
    if (changed)
        wake(l->wake);
}

static void *presence_thread(void *arg)
{
    RXLink *l = arg;
    while (!atomic_load(&l->stop)) {
        /* Every turn, not only when due: a keep-alive heard makes the
         * join due at once, and a tick only sends what is due. */
        pthread_mutex_lock(&l->plock);
        int64_t now = rx_time_us(), next = rx_presence_tick(l->pres, now);
        pthread_mutex_unlock(&l->plock);
        publish(l);

        struct pollfd p[3] = {
            { .fd = l->fds[S_ANNOUNCE], .events = POLLIN },
            { .fd = l->fds[S_STATUS], .events = POLLIN },
            { .fd = l->pwake, .events = POLLIN },
        };
        int timeout = next > now ? (int)RX_MIN((next - now + 999) / 1000, 1000) : 0;
        if (poll(p, 3, timeout) < 0 && errno != EINTR) {
            rx_err("link: poll: %s", strerror(errno));
            break;
        }
        if (p[0].revents & POLLIN)
            drain_presence(l, S_ANNOUNCE);
        if (p[1].revents & POLLIN)
            drain_presence(l, S_STATUS);
    }
    return NULL;
}

/* What follows here from the devices on the link: the tree is offered
 * once the link is up (before that rekordbox lists no export and refuses
 * mounts: rbxport's capture), and the devices are kept as known. A
 * device gone from the link keeps its mount: a deck back from a lapse of
 * its keep-alives reads on and never mounts again, and a refusal hangs
 * its load for good (an emulated CDJ-2000NXS2, 2026-10-15). */
static void follow(RXLink *l)
{
    uint32_t now[MAX_PLAYERS];
    pthread_mutex_lock(&l->lock);
    bool up = l->state == RX_LINK_UP;
    int nb = l->nb_players;
    for (int i = 0; i < nb; i++)
        now[i] = l->players[i].addr;
    pthread_mutex_unlock(&l->lock);

    if (up && !l->serving) {
        rx_nfs_set_tree(l->nfs, l->tree);
        l->serving = true;
    }
    int64_t t = rx_time_us();
    for (int i = 0; i < nb; i++)
        known_seen(l, now[i], t);
    memcpy(l->peers, now, (size_t)nb * sizeof(*now));
    l->nb_peers = nb;
}

/* The sockets after the presence ones, which the presence thread has. */
#define S_FIRST S_QUERY
#define NB_SERVED (NB_SOCKS - S_FIRST)

static void *service(void *arg)
{
    RXLink *l = arg;
    while (!atomic_load(&l->stop)) {
        follow(l);
        struct pollfd pfd[NB_SERVED + 1 + MAX_CONNS];
        int nb = 0;
        for (int i = S_FIRST; i < NB_SOCKS; i++)
            pfd[nb++] = (struct pollfd) { .fd = l->fds[i], .events = POLLIN };
        pfd[nb++] = (struct pollfd) { .fd = l->wake, .events = POLLIN };
        for (int i = 0; i < l->nb_conns; i++) {
            Conn *c = &l->conns[i];
            short ev = 0;
            if (!c->closing && c->out.len - c->sent < MAX_PENDING)
                ev |= POLLIN;
            if (c->sent < c->out.len)
                ev |= POLLOUT;
            pfd[nb++] = (struct pollfd) { .fd = c->fd, .events = ev };
        }

        if (poll(pfd, (nfds_t)nb, 1000) < 0 && errno != EINTR) {
            rx_err("link: poll: %s", strerror(errno));
            break;
        }

        if (pfd[NB_SERVED].revents & POLLIN) {
            uint64_t v;
            if (read(l->wake, &v, sizeof(v)) < 0 && errno != EAGAIN)
                rx_debug("link: wake: %s", strerror(errno));
            if (atomic_exchange(&l->built, false))
                reload_done(l);
            tracks_changed(l);
            /* A change while one is read: read again after it. */
            if (!l->building && atomic_exchange(&l->reload, false))
                reload_start(l);
        }
        if (pfd[S_QUERY - S_FIRST].revents & POLLIN)
            conn_accept(l, CONN_QUERY);
        if (pfd[S_DB - S_FIRST].revents & POLLIN)
            conn_accept(l, CONN_DB);
        for (enum Sock s = S_PORTMAP; s <= S_NFS; s++) {
            if (pfd[s - S_FIRST].revents & POLLIN)
                drain_nfs(l, s);
        }
        /* The connections polled are the first ones; any accepted above
         * wait for the next turn. Backwards, as closing moves the last
         * one into the closed one's place. */
        int polled = nb - NB_SERVED - 1;
        for (int i = RX_MIN(polled, l->nb_conns) - 1; i >= 0; i--) {
            Conn *c = &l->conns[i];
            short re = pfd[NB_SERVED + 1 + i].revents;
            bool keep = true;
            if (re & (POLLERR | POLLNVAL))
                keep = false;
            if (keep && (re & POLLIN))
                keep = conn_read(l, c);
            else if (keep && (re & POLLHUP))
                keep = false;
            if (keep && (re & POLLOUT))
                keep = conn_flush(c);
            if (!keep)
                conn_close(l, i);
        }
    }
    while (l->nb_conns)
        conn_close(l, l->nb_conns - 1);
    return NULL;
}

/* ------------------------------------------------------------ public API */

int rx_link_start(RXLink **out, const RXLinkOptions *o)
{
    *out = NULL;
    if (!o->interface || !*o->interface)
        return RX_ERROR(EINVAL);
    RXLink *l = calloc(1, sizeof(*l));
    if (!l)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < NB_SOCKS; i++)
        l->fds[i] = -1;
    l->status_out = l->wake = l->pwake = -1;
    pthread_mutex_init(&l->lock, NULL);
    pthread_mutex_init(&l->plock, NULL);
    l->root = o->library ? rx_strdup(o->library) : NULL;
    l->name = rx_strndup(o->name && *o->name ? o->name : "rekordbox", 20);
    l->any_peer = o->any_peer;
    l->start_up = o->start_up;
    l->test_build_ms = o->test_build_ms;
    l->tags = o->tag_list;
    l->peer = o->peer_ports;
    int ret = RX_ERROR(ENOMEM);
    if (!l->tags && rx_link_tag_list_new(&l->tags) >= 0)
        l->own_tags = true;
    if ((o->library && !l->root) || !l->name || !l->tags)
        goto fail;
    if ((ret = rx_link_if_find(&l->ifc, o->interface, NULL, o->broadcast)) < 0 ||
        (ret = open_sockets(l, &o->ports)) < 0)
        goto fail;
    uint16_t eph;
    if ((l->status_out = rx_link_udp(&l->ifc, INADDR_ANY, 0, &eph)) < 0) {
        ret = l->status_out;
        goto fail;
    }
    if ((l->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0 ||
        (l->pwake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
        ret = RX_ERROR(errno);
        goto fail;
    }

    if ((ret = rx_link_wav_new(&l->wav, WAV_MEMORY)) < 0)
        goto fail;
    int64_t t0 = rx_time_us();
    Served s;
    if ((ret = served_build(l, &s)) < 0) {
        rx_err("link: cannot read the library: %s", strerror(-ret));
        goto fail;
    }
    served_take(l, &s);
    /* The export list offers the interface's subnet, as rekordbox's does:
     * a CDJ-3000 mounts nothing whose list names no group (nfs.h). */
    RXNfsConfig nc = {
        .mount_port   = l->ports.mount,
        .nfs_port     = l->ports.nfs,
        .portmap_port = l->ports.portmap,
        .addr         = l->ifc.addr,
        .netmask      = l->ifc.mask,
        .read         = wav_read,
        .read_opaque  = l,
    };
    if ((ret = rx_nfs_new(&l->nfs, &nc)) < 0)
        goto fail;

    RXPresenceConfig pc = {
        .role = RX_PRESENCE_COMPUTER,
        .addr = l->ifc.addr,
        .start_up = l->start_up,
    };
    memcpy(pc.mac, l->ifc.mac, sizeof(pc.mac));
    snprintf(pc.name, sizeof(pc.name), "%s", l->name);
    /* The identity reply names the computer, as rekordbox's does. */
    if (gethostname(pc.host, sizeof(pc.host) - 1) < 0)
        pc.host[0] = 0;
    if ((ret = rx_presence_new(&l->pres, &pc, presence_send, l)) < 0)
        goto fail;
    presence_library(l);

    if ((ret = pthread_create(&l->thread, NULL, service, l))) {
        ret = RX_ERROR(ret);
        goto fail;
    }
    l->running = true;
    if ((ret = pthread_create(&l->pthread, NULL, presence_thread, l))) {
        ret = RX_ERROR(ret);
        goto fail;
    }
    l->prunning = true;

    char a[16], b[16];
    rx_link_addr_text(l->ifc.addr, a);
    rx_link_addr_text(l->ifc.bcast, b);
    rx_info("link: serving %d tracks on %s (%s, broadcast %s) as \"%s\" (%.0f ms); "
            "database %u, portmap %u, mount %u, NFS %u", l->plan->nb_tracks, l->ifc.name, a, b,
            l->name, (double)(rx_time_us() - t0) / 1000.0, l->ports.database, l->ports.portmap,
            l->ports.mount, l->ports.nfs);
    *out = l;
    return 0;
fail:
    rx_link_stop(&l);
    return ret;
}

void rx_link_stop(RXLink **lp)
{
    RXLink *l = *lp;
    if (!l)
        return;
    atomic_store(&l->stop, true);
    if (l->prunning) {
        wake(l->pwake);
        pthread_join(l->pthread, NULL);
    }
    if (l->running) {
        wake(l->wake);
        pthread_join(l->thread, NULL);
    }
    /* A reading of the library in progress stops at once (still_wanted). */
    if (l->building) {
        pthread_join(l->bthread, NULL);
        served_free(&l->fresh);
    }
    rx_presence_free(&l->pres);
    rx_nfs_free(&l->nfs);
    if (l->own_tags)
        rx_link_tag_list_free(&l->tags);
    Served s = { .lib = l->lib, .plan = l->plan, .cat = l->cat, .tree = l->tree };
    served_free(&s);
    rx_link_wav_free(&l->wav);
    for (int i = 0; i < NB_SOCKS; i++) {
        if (l->fds[i] >= 0)
            close(l->fds[i]);
    }
    if (l->status_out >= 0)
        close(l->status_out);
    if (l->wake >= 0)
        close(l->wake);
    if (l->pwake >= 0)
        close(l->pwake);
    pthread_mutex_destroy(&l->lock);
    pthread_mutex_destroy(&l->plock);
    free(l->root);
    free(l->name);
    free(l);
    *lp = NULL;
}

enum RXLinkState rx_link_state(RXLink *l, int *number)
{
    pthread_mutex_lock(&l->lock);
    enum RXLinkState s = l->state;
    if (number)
        *number = l->number;
    pthread_mutex_unlock(&l->lock);
    return s;
}

int rx_link_players(RXLink *l, RXLinkPlayer *out, int max)
{
    pthread_mutex_lock(&l->lock);
    int n = l->nb_players;
    if (out)
        memcpy(out, l->players, (size_t)RX_MIN(RX_MAX(max, 0), n) * sizeof(*out));
    pthread_mutex_unlock(&l->lock);
    return n;
}

int rx_link_load(RXLink *l, int player, uint32_t track_id)
{
    pthread_mutex_lock(&l->plock);
    int ret = rx_presence_load(l->pres, player, track_id);
    pthread_mutex_unlock(&l->plock);
    return ret;
}

void rx_link_library_changed(RXLink *l)
{
    atomic_store(&l->reload, true);
    wake(l->wake);
}

void rx_link_tracks_changed(RXLink *l, const uint64_t *ids, int nb)
{
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < nb; i++) {
        int k = 0;
        while (k < l->nb_changed && l->changed[k] != ids[i])
            k++;
        if (k < l->nb_changed)
            continue;
        if (l->nb_changed == RX_LIBRARY_CHANGES_KEPT) {
            l->nb_changed = 0;
            atomic_store(&l->reload, true);
            break;
        }
        l->changed[l->nb_changed++] = ids[i];
    }
    pthread_mutex_unlock(&l->lock);
    wake(l->wake);
}

int rx_link_edits(RXLink *l, RXLinkEdit *out, int max)
{
    pthread_mutex_lock(&l->lock);
    int n = RX_MIN(l->nb_edits, RX_MAX(max, 0));
    memcpy(out, l->edits, (size_t)n * sizeof(*out));
    memmove(l->edits, l->edits + n, (size_t)(l->nb_edits - n) * sizeof(*l->edits));
    l->nb_edits -= n;
    pthread_mutex_unlock(&l->lock);
    return n;
}

void rx_link_bound_ports(RXLink *l, RXLinkPorts *out)
{
    *out = l->ports;
}
