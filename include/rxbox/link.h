/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_LINK_H
#define RXBOX_LINK_H

/* PRO DJ LINK: the library served to players over the network, as
 * rekordbox's Link Export does (docs/LINK.md). rxbox joins the players'
 * network as "rekordbox", shows up on their SOURCE screen, answers their
 * database requests (menus, metadata, artwork, analysis) and lets them
 * read the audio over NFS version 2 from a virtual tree that holds the
 * library's tracks and nothing of the host.
 *
 * A service (RXLink) serves one Ethernet interface: a thread of its own
 * with its own connection to the library, which it reads and never
 * writes, answering only the players heard on that interface
 * (docs/LINK.md, "Security"). A set (RXLinkSet) runs one on every
 * Ethernet interface that is up with an address, as they come and go. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct RXLink RXLink;

/* The ports, ours and the ones players listen on. Real players use
 * rekordbox's (rx_link_options_default); the tests run a server and a
 * fake player on one host, so each side gets ports of its own, and 0 asks
 * the system for any free one. `database` and `mount` are rekordbox's
 * ephemeral ones: 0 normally (the port query and portmap give them out). */
typedef struct RXLinkPorts {
    uint16_t announce, status;      /* UDP 50000, 50002 */
    uint16_t query, database;       /* TCP 12523, ephemeral */
    uint16_t portmap, mount, nfs;   /* UDP 50111, ephemeral, 2049 */
} RXLinkPorts;

typedef struct RXLinkOptions {
    const char *library;        /* the library's directory, NULL = the default one */
    const char *interface;      /* the Ethernet interface ("eth0"); required by
                                   rx_link_start, NULL for a set: every one */
    const char *name;           /* the name players show, at most 20 bytes; "rekordbox" */
    /* Where presence goes: the interface's broadcast address, unless this
     * names another (the tests' loopback: "127.0.0.1"). */
    const char *broadcast;
    /* Answer the database and NFS from any address, not only players
     * heard on the link: diagnostics only (a client such as rbxport's
     * browse example, which never announces itself). */
    bool any_peer;
    /* Join the link at once (the default): rxbox has its number and is on
     * the players' SOURCE lists before the first of them is switched on,
     * and does not depend on hearing one first. False waits for a player's
     * keep-alive, as rekordbox does (the tests of that wait). */
    bool start_up;
    RXLinkPorts ports;          /* ours */
    RXLinkPorts peer_ports;     /* the players' (announce and status only) */
} RXLinkOptions;

void rx_link_options_default(RXLinkOptions *o);

/* What a device is. Its keep-alive's type byte is not this: CDJs say 1,
 * a DJM-V5 says 3, rekordbox says 4 (rbxport's captures); packet.c maps
 * them. */
enum RXLinkDeviceType {
    RX_LINK_DEVICE_PLAYER   = 1,
    RX_LINK_DEVICE_MIXER    = 2,
    RX_LINK_DEVICE_COMPUTER = 3,    /* rekordbox, and rxbox */
};

/* A device heard on the link and, for a player, what its last status
 * packet said. */
typedef struct RXLinkPlayer {
    int      number;            /* its device number: players 1..6, mixer 33, rekordbox 17.. */
    int      type;              /* enum RXLinkDeviceType */
    char     name[24];
    uint32_t addr;              /* IPv4, host order */
    uint8_t  mac[6];
    int64_t  last_seen;         /* rx_time_us of its last keep-alive */

    bool     has_status;
    int      track_device;      /* where the loaded track is from (a device number); 0 none */
    int      track_slot;        /* 1 CD, 2 SD, 3 USB, 4 a rekordbox collection (djl-analysis
                                   vcdj.adoc; a CDJ-3000 on firmware EP122 says 3 for ours) */
    int      track_type;        /* 1 a rekordbox track ... */
    uint32_t track_id;          /* its rekordbox id: a library id when track_device is ours */
    bool     playing, on_air, master, synced;
    double   bpm;               /* the track's tempo, 0 when unknown */
    double   pitch;             /* the pitch in effect (sync included), +-percent */
    int      beat;              /* beat number in the track, -1 unknown (a pre-nexus
                                   player sends none: a CDJ-900, a CDJ-2000) */
    int64_t  status_us;         /* rx_time_us of its last status, 0 none */
} RXLinkPlayer;

enum RXLinkState {
    RX_LINK_WAITING,            /* no player heard yet */
    RX_LINK_JOINING,            /* claiming a device number */
    RX_LINK_UP,                 /* on the link with a number */
};

/* An interface Link could serve on: Ethernet (a USB adapter's too; not
 * Wi-Fi, not a tunnel), up, with an IPv4 address. Link runs on Ethernet
 * only: rx_link_start refuses any other interface (-EPROTONOSUPPORT) but
 * the loopback, which reaches no player and carries the tests. */
typedef struct RXLinkInterface {
    char     name[16];
    uint32_t addr;              /* its first IPv4 address, host order */
    int      prefix;            /* the netmask's length */
    bool     running;           /* has a carrier (a cable in, a link up) */
} RXLinkInterface;

/* The interfaces, the ones with a carrier first, then by name: the
 * count, up to `max` filled. */
int  rx_link_interfaces(RXLinkInterface *out, int max);

/* Starts the service thread: binds every port on the interface, opens the
 * library, builds what it serves. Errors are negative errno values
 * (-EADDRINUSE when rekordbox or another rxbox holds the ports, -ENODEV
 * for an unknown interface, -EPROTONOSUPPORT for one that is not
 * Ethernet). */
int  rx_link_start(RXLink **out, const RXLinkOptions *o);
/* Stops and frees; *l is NULL after. Safe on NULL. */
void rx_link_stop(RXLink **l);

/* Where it stands, and the device number it has (0 until up). */
enum RXLinkState rx_link_state(RXLink *l, int *number);
/* The devices heard, players first by number: the count, up to `max`
 * filled. */
int  rx_link_players(RXLink *l, RXLinkPlayer *out, int max);
/* The library changed: what is served is built again from it. */
void rx_link_library_changed(RXLink *l);
/* Tells `player` to load our track `track_id` (a load command, as
 * rekordbox sends for a track dragged onto a deck; docs/LINK.md). 0 once
 * sent: the player's status says when it has loaded it. -EAGAIN before we
 * are on the link, -ENOENT when there is no such player. */
int  rx_link_load(RXLink *l, int player, uint32_t track_id);
/* The ports bound (with any 0 asked for filled in). */
void rx_link_bound_ports(RXLink *l, RXLinkPorts *out);

/* ------------------------------------------------------------- the set */

/* Link on every Ethernet interface (rx_link_interfaces), or on the one
 * the options name: a service each, all with the same options. A thread
 * of the set's own looks at the interfaces every two seconds and starts a
 * service on one that has come up with an IPv4 address, stops the one on
 * an interface gone or without its address, and starts it again when the
 * address changed; a service that would not start (the ports held there
 * by another program) is tried again ten seconds later. Starting and
 * stopping happen on that thread: the owner waits for neither. */
typedef struct RXLinkSet RXLinkSet;

/* One interface the set serves, or tries to. */
typedef struct RXLinkServed {
    char     interface[16];
    uint32_t addr;              /* its IPv4 address, host order */
    bool     running;           /* served; false while starting, or after a failure */
    int      err;               /* why it would not start (tried again), else 0 */
    enum RXLinkState state;     /* when running */
    int      number;            /* our device number there, once up */
} RXLinkServed;

/* Starts the set's thread; o->interface NULL: every Ethernet interface.
 * The options are copied. */
int  rx_link_set_start(RXLinkSet **out, const RXLinkOptions *o);
/* Stops every service and the thread; *s is NULL after. Safe on NULL. */
void rx_link_set_stop(RXLinkSet **s);
/* The interfaces served or tried, by name: the count, up to `max` filled. */
int  rx_link_set_interfaces(RXLinkSet *s, RXLinkServed *out, int max);
/* The devices heard on all of them, each interface's in rx_link_players'
 * order, in the order of rx_link_set_interfaces; `where` (may be NULL)
 * gets each one's index in that list. The count, up to `max` filled. */
int  rx_link_set_players(RXLinkSet *s, RXLinkPlayer *out, int *where, int max);
/* rx_link_library_changed for every service. */
void rx_link_set_library_changed(RXLinkSet *s);
/* rx_link_load on the interface `where` (rx_link_set_players' index):
 * -ENOENT when it is not served any more. */
int  rx_link_set_load(RXLinkSet *s, int where, int player, uint32_t track_id);

/* ------------------------------------------------------------ fake player */

/* A player of our own, for the tests and for checking a server by hand
 * (`rxbox link browse`): rxbox's, rbxport's, or rekordbox's. It joins the
 * link as a CDJ-3000 with a number of its own (its presence on a thread
 * of its own), finds the computer source, browses its database and reads
 * its files over NFS the way a player does. It never writes. */
typedef struct RXLinkClient RXLinkClient;

typedef struct RXLinkClientOptions {
    const char *interface;      /* required */
    /* Our address on it, NULL the interface's: the tests' loopback, where
     * the server is 127.0.0.1, uses 127.0.0.2. */
    const char *address;
    const char *broadcast;      /* as RXLinkOptions */
    int         number;         /* the player number: 3 */
    const char *name;           /* "CDJ-3000" */
    /* Do not announce: browse a server that answers anyone (rxbox with
     * any_peer). `server` is then required. */
    bool        quiet;
    const char *server;         /* the server's address; NULL: the computer source heard */
    RXLinkPorts ports;          /* ours: announce and status, 0 any */
    RXLinkPorts server_ports;   /* the server's: announce, status, query and portmap */
} RXLinkClientOptions;

void rx_link_client_options_default(RXLinkClientOptions *o);
int  rx_link_client_start(RXLinkClient **out, const RXLinkClientOptions *o);
void rx_link_client_stop(RXLinkClient **c);
void rx_link_client_bound_ports(RXLinkClient *c, RXLinkPorts *out);
/* Waits up to timeout_ms for a computer source (or the named server):
 * its address and device number; -ETIMEDOUT when none answered. */
int  rx_link_client_wait_source(RXLinkClient *c, int timeout_ms, uint32_t *addr, int *number);
/* What our status says from now on. */
void rx_link_client_set_loaded(RXLinkClient *c, int device, int slot, uint32_t track_id,
                               bool playing);
/* A whole file from the source over NFS, read as a player reads it. */
int  rx_link_client_fetch(RXLinkClient *c, const char *path, uint8_t **data, size_t *size);
/* What our status says is loaded (as set, or as a load command from the
 * source left it): its device, slot and track id, 0 for none. */
void rx_link_client_loaded(RXLinkClient *c, int *device, int *slot, uint32_t *track_id);
/* Plays the loaded track, `track`, from its start: its beat grid asked of
 * the source, our status then says the beat number and tempo as it plays,
 * as a nexus player's does, and stops at its end. The number of beats, or
 * a negative errno (the source has no grid for it, or is gone). */
int  rx_link_client_play(RXLinkClient *c, uint32_t track);

/* What a player does with a source, reported a line at a time: the root
 * and sort menus, all tracks, the playlists; then for `track` (0: the first one) its metadata, track
 * info, artwork, beat grid, waveforms and cues, the requests a CDJ-3000
 * makes after a load (user and delivery info), and its file over NFS,
 * whose size must be the one the track info gave; our status then says
 * it is loaded and playing. 0, or the first error. */
typedef void (*rx_link_line_cb)(void *opaque, const char *line);
int  rx_link_client_browse(RXLinkClient *c, uint32_t track, rx_link_line_cb line, void *opaque);

#endif /* RXBOX_LINK_H */
