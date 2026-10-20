/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_LINK_H
#define CYANBOX_LINK_H

/* PRO DJ LINK: the library served to players over the network, as
 * rekordbox's Link Export does (docs/LINK.md). cyanbox joins the players'
 * network as "rekordbox", shows up on their SOURCE screen, answers their
 * database requests (menus, metadata, artwork, analysis) and lets them
 * read the audio over NFS version 2 from a virtual tree that holds the
 * library's tracks and nothing of the host.
 *
 * A service (CBLink) serves one Ethernet interface: a thread of its own
 * with its own connection to the library, which it reads and never
 * writes, answering only the players heard on that interface
 * (docs/LINK.md, "Security"). A set (CBLinkSet) runs one on every
 * Ethernet interface that is up with an address, as they come and go. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cyanbox/library.h"
#include "cyanbox/model.h"

typedef struct CBLink CBLink;
typedef struct CBLinkTagList CBLinkTagList;
typedef struct CBLinkCue CBLinkCue;

/* The ports, ours and the ones players listen on. Real players use
 * rekordbox's (cb_link_options_default); the tests run a server and a
 * fake player on one host, so each side gets ports of its own, and 0 asks
 * the system for any free one. `database` and `mount` are rekordbox's
 * ephemeral ones: 0 normally (the port query and portmap give them out). */
typedef struct CBLinkPorts {
    uint16_t announce, status;      /* UDP 50000, 50002 */
    uint16_t query, database;       /* TCP 12523, ephemeral */
    uint16_t portmap, mount, nfs;   /* UDP 50111, ephemeral, 2049 */
    uint16_t cue;                   /* UDP 50004, LINK CUE (with CBLinkOptions.cue) */
} CBLinkPorts;

typedef struct CBLinkOptions {
    const char *library;        /* the library's directory, NULL = the default one */
    const char *interface;      /* the Ethernet interface ("eth0"); required by
                                   cb_link_start, NULL for a set: every one */
    const char *name;           /* the name players show, at most 20 bytes; "rekordbox" */
    /* Where presence goes: the interface's broadcast address, unless this
     * names another (the tests' loopback: "127.0.0.1"). */
    const char *broadcast;
    /* Answer the database and NFS from any address, not only players
     * heard on the link: diagnostics only (a client such as rbxport's
     * browse example, which never announces itself). */
    bool any_peer;
    /* Join the link at once (the default): cyanbox has its number and is on
     * the players' SOURCE lists before the first of them is switched on,
     * and does not depend on hearing one first. False waits for a player's
     * keep-alive, as rekordbox does (the tests of that wait). */
    bool start_up;
    CBLinkPorts ports;          /* ours */
    CBLinkPorts peer_ports;     /* the players' (announce and status) and mixers' (cue) */
    /* Tests: each reading of the library after the first takes this much
     * longer, so a test sees players answered while one runs. */
    int test_build_ms;
    /* An all-in-one on USB, a set's only (cb_link_set_start): the ALSA
     * sound card, by its id (XDJRX3, as /proc/asound/cards has it in
     * brackets) or its number, whose MIDI port gets rekordbox's
     * activation every 200 ms while the set runs. An XDJ-RX3 lists the
     * computer on SOURCE only while it comes (src/link/usb.h). NULL: none.
     * `test_usb_root`, tests only: a directory standing in for / to find
     * the card in (NULL the system). */
    const char *usb_card;
    const char *test_usb_root;
    /* The TAG LIST the players see and change (below), the owner's to
     * keep; NULL: a service or a set makes its own. */
    CBLinkTagList *tag_list;
    /* LINK CUE (below): the owner's audio for mixers, the owner's to keep;
     * NULL: none, and its port is not bound. */
    CBLinkCue *cue;
    /* A set's own, for each service it starts: NFS's session on that
     * interface (src/link/nfs.h), kept as long as the set runs, so a deck
     * that reads on after the interface went and came back (a cable, a
     * USB adapter) is answered. NULL: the service's own. */
    struct CBNfsSession *nfs_session;
} CBLinkOptions;

void cb_link_options_default(CBLinkOptions *o);

/* What a device is. Its keep-alive's type byte is not this: CDJs say 1,
 * a DJM-V5 says 3, rekordbox says 4 (rbxport's captures); packet.c maps
 * them. */
enum CBLinkDeviceType {
    CB_LINK_DEVICE_PLAYER   = 1,
    CB_LINK_DEVICE_MIXER    = 2,
    CB_LINK_DEVICE_COMPUTER = 3,    /* rekordbox, and cyanbox */
};

/* A device heard on the link and, for a player, what its last status
 * packet said. */
typedef struct CBLinkPlayer {
    int      number;            /* its device number: players 1..6, mixer 33, rekordbox 17.. */
    int      type;              /* enum CBLinkDeviceType */
    char     name[24];
    uint32_t addr;              /* IPv4, host order */
    uint8_t  mac[6];
    int64_t  last_seen;         /* cb_time_us of its last keep-alive */

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
    int64_t  status_us;         /* cb_time_us of its last status, 0 none */
} CBLinkPlayer;

enum CBLinkState {
    CB_LINK_WAITING,            /* no player heard yet */
    CB_LINK_JOINING,            /* claiming a device number */
    CB_LINK_UP,                 /* on the link with a number */
};

/* An interface Link could serve on: Ethernet (a USB adapter's too; not
 * Wi-Fi, not a tunnel), up, with an IPv4 address. Link runs on Ethernet
 * only: cb_link_start refuses any other interface (-EPROTONOSUPPORT) but
 * the loopback, which reaches no player and carries the tests. */
typedef struct CBLinkInterface {
    char     name[16];
    uint32_t addr;              /* its first IPv4 address, host order */
    int      prefix;            /* the netmask's length */
    bool     running;           /* has a carrier (a cable in, a link up) */
} CBLinkInterface;

/* The interfaces, the ones with a carrier first, then by name: the
 * count, up to `max` filled. */
int  cb_link_interfaces(CBLinkInterface *out, int max);

/* Starts the service thread: binds every port on the interface, opens the
 * library, builds what it serves. Errors are negative errno values
 * (-EADDRINUSE when rekordbox or another cyanbox holds the ports, -ENODEV
 * for an unknown interface, -EPROTONOSUPPORT for one that is not
 * Ethernet). */
int  cb_link_start(CBLink **out, const CBLinkOptions *o);
/* Stops and frees; *l is NULL after. Safe on NULL. */
void cb_link_stop(CBLink **l);

/* Where it stands, and the device number it has (0 until up). */
enum CBLinkState cb_link_state(CBLink *l, int *number);
/* The devices heard, players first by number: the count, up to `max`
 * filled. */
int  cb_link_players(CBLink *l, CBLinkPlayer *out, int max);
/* The library changed: what is served is built again from it (on a
 * thread of its own, the old served meanwhile). */
void cb_link_library_changed(CBLink *l);
/* Only these tracks' own state changed (cb_library_take_changes): each is
 * read again and put in place, unless what changed shapes what is served
 * (a name, the key, the file), which builds it all again. */
void cb_link_tracks_changed(CBLink *l, const uint64_t *ids, int nb);
/* Tells `player` to load our track `track_id` (a load command, as
 * rekordbox sends for a track dragged onto a deck; docs/LINK.md). 0 once
 * sent: the player's status says when it has loaded it. -EAGAIN before we
 * are on the link, -ENOENT when there is no such player. */
int  cb_link_load(CBLink *l, int player, uint32_t track_id);
/* Pushes the My Settings of `cfg` to every player and mixer on the link,
 * unasked, as rekordbox's "Remote Settings using Pro DJ Link" does
 * (docs/LINK.md, "My Settings over the link"): the player settings to
 * the players, the mixer settings to the mixers, cyanbox's defaults where
 * `cfg` sets none. 0 and how many of each it was sent to; -EAGAIN before
 * we are on the link, -EINVAL when a setting is not valid. */
int  cb_link_push_settings(CBLink *l, const CBConfig *cfg, int *players, int *mixers);
/* The ports bound (with any 0 asked for filled in). */
void cb_link_bound_ports(CBLink *l, CBLinkPorts *out);

/* What a player changed on the deck on a track of ours (docs/LINK.md,
 * "Edits on the deck"): a cue saved or deleted, or the rating. The service
 * has applied it to what it serves, so its answer to the player shows it,
 * and hands it on: the library's owner applies it (cb_link_edit_apply)
 * and saves the track. The services never write the library. */
enum CBLinkEditWhat {
    CB_LINK_EDIT_CUE = 0,
    CB_LINK_EDIT_RATING,
};

typedef struct CBLinkEdit {
    int      what;              /* enum CBLinkEditWhat */
    uint32_t track_id;
    int      player;            /* the device number of the player that sent it */
    /* A cue: deleted, else saved. A hot cue names its slot, a memory cue
     * its time. On the track's grid when it sits on a beat, as a cue read
     * back from a stick (cb_stick_cue_to_track). */
    bool     remove;
    CBCue    cue;
    int      rating;            /* the rating: 0 to 5 stars */
} CBLinkEdit;

/* A service keeps this many edits not taken yet; the oldest go first. */
#define CB_LINK_EDITS_KEPT 256
/* The edits players made since the last call, oldest first: the count,
 * up to `max` taken (the rest wait for the next call). */
int  cb_link_edits(CBLink *l, CBLinkEdit *out, int max);
/* Applies an edit to `t` as rekordbox applies one to its library: a hot
 * cue saved takes its slot, replacing what was there; a memory cue saved
 * replaces the one at its time (within 20 ms) or is added; a deletion
 * takes off the hot cue in the slot, or the memory cue at the time; a
 * rating replaces the track's. 1 when `t` changed, 0 when there was
 * nothing to do, or a negative errno. */
int  cb_link_edit_apply(CBTrack *t, const CBLinkEdit *e);

/* ------------------------------------------------------------ tag list */

/* The TAG LIST (docs/LINK.md, "Edits on the deck"): the tracks a DJ
 * collects while browsing, with TAG TRACK on a deck or in the interface,
 * in the order they were added, which every deck browsing cyanbox shows and
 * changes. Temporary: in memory only, shared by the services and their
 * owner; every call takes its lock. */
#define CB_LINK_TAG_LIST_MAX 1000

int  cb_link_tag_list_new(CBLinkTagList **out);
void cb_link_tag_list_free(CBLinkTagList **tl);
/* At the end: 1, or 0 when it is there already; -ENOSPC when full. */
int  cb_link_tag_list_add(CBLinkTagList *tl, uint32_t id);
/* 1, or 0 when it was not there. */
int  cb_link_tag_list_remove(CBLinkTagList *tl, uint32_t id);
void cb_link_tag_list_clear(CBLinkTagList *tl);
bool cb_link_tag_list_has(CBLinkTagList *tl, uint32_t id);
/* The ids in order: how many there are, up to `max` copied. */
int  cb_link_tag_list_get(CBLinkTagList *tl, uint32_t *ids, int max);
/* Bumped by every change, for an owner's view to follow. */
unsigned cb_link_tag_list_generation(CBLinkTagList *tl);

/* ------------------------------------------------------------ LINK CUE */

/* LINK CUE (docs/LINK.md, "LINK CUE"): a mixer's headphone cue fed with
 * the owner's audio, as rekordbox's Link Monitor feeds it. A DJM takes
 * audio from the computers on the link (device 17 and 18) as from its
 * players: it sends each a timing packet every 7 ms, and the source it
 * elected answers each, while the mixer's LINK CUE is on, with the next
 * 7 ms of audio, 336 frames of 16-bit stereo at 48 kHz, which `take` gives
 * (cb_player_ring_take): the mixer's clock paces the decks.
 *
 * A source asks to be elected; cyanbox asks when its decks start playing (a
 * deck started, or a mixer appearing while one plays) and then streams
 * for as long as they play and the mixer keeps it. A player taking the
 * cue over (a CDJ-3000's touch cue) has it until cyanbox's decks start
 * again. Nothing is sent while they do not play. One mixer at a time:
 * the first heard, until it is silent for a second.
 *
 * Shared by the services of a set; every call takes its lock, and `take`
 * is called with it held, on a service's thread. */
typedef int (*cb_link_cue_take_fn)(void *opaque, int16_t *out, int frames);

int  cb_link_cue_new(CBLinkCue **out, cb_link_cue_take_fn take, void *opaque);
void cb_link_cue_free(CBLinkCue **c);
/* The owner's decks: whether one plays, and whether one started since the
 * last call (it then asks the mixer for the cue). */
void cb_link_cue_decks(CBLinkCue *c, bool playing, bool started);

typedef struct CBLinkCueState {
    bool     mixer;             /* a mixer's timing heard in the last second */
    char     name[21];          /* the mixer's, its number below */
    int      number;
    bool     on;                /* its LINK CUE is on */
    int      source;            /* the source it elected last, 0 none yet */
    bool     ours;              /* that is us */
    bool     sending;           /* our audio went to it in the last 100 ms */
} CBLinkCueState;

void cb_link_cue_state(CBLinkCue *c, CBLinkCueState *out);

/* ------------------------------------------------------------- the set */

/* Link on every Ethernet interface (cb_link_interfaces), or on the one
 * the options name: a service each, all with the same options. A thread
 * of the set's own looks at the interfaces every two seconds and starts a
 * service on one that has come up with an IPv4 address, stops the one on
 * an interface gone or without its address, and starts it again when the
 * address changed; a service that would not start (the ports held there
 * by another program) is tried again ten seconds later. Starting and
 * stopping happen on that thread: the owner waits for neither. */
typedef struct CBLinkSet CBLinkSet;

/* One interface the set serves, or tries to. */
typedef struct CBLinkServed {
    char     interface[16];
    uint32_t addr;              /* its IPv4 address, host order */
    bool     running;           /* served; false while starting, or after a failure */
    int      err;               /* why it would not start (tried again), else 0 */
    enum CBLinkState state;     /* when running */
    int      number;            /* our device number there, once up */
} CBLinkServed;

/* Starts the set's thread; o->interface NULL: every Ethernet interface.
 * The options are copied. */
int  cb_link_set_start(CBLinkSet **out, const CBLinkOptions *o);
/* Stops every service and the thread; *s is NULL after. Safe on NULL. */
void cb_link_set_stop(CBLinkSet **s);
/* The interfaces served or tried, by name: the count, up to `max` filled. */
int  cb_link_set_interfaces(CBLinkSet *s, CBLinkServed *out, int max);
/* The devices heard on all of them, each interface's in cb_link_players'
 * order, in the order of cb_link_set_interfaces; `where` (may be NULL)
 * gets each one's index in that list. The count, up to `max` filled. */
int  cb_link_set_players(CBLinkSet *s, CBLinkPlayer *out, int *where, int max);
/* cb_link_library_changed and cb_link_tracks_changed for every service. */
void cb_link_set_library_changed(CBLinkSet *s);
void cb_link_set_tracks_changed(CBLinkSet *s, const uint64_t *ids, int nb);
/* cb_link_load on the interface `where` (cb_link_set_players' index):
 * -ENOENT when it is not served any more. */
int  cb_link_set_load(CBLinkSet *s, int where, int player, uint32_t track_id);
/* cb_link_push_settings on every interface, the counts added up: 0 when
 * one of them is on its link, -EAGAIN when none is. */
int  cb_link_set_push_settings(CBLinkSet *s, const CBConfig *cfg, int *players, int *mixers);
/* cb_link_edits of every service, one after the other. */
int  cb_link_set_edits(CBLinkSet *s, CBLinkEdit *out, int max);

/* ------------------------------------------------------------ fake player */

/* A player of our own, for the tests and for checking a server by hand
 * (`cyanbox link browse`): cyanbox's, rbxport's, or rekordbox's. It joins the
 * link as a CDJ-3000 with a number of its own (its presence on a thread
 * of its own), finds the computer source, browses its database and reads
 * its files over NFS the way a player does. It never writes. */
typedef struct CBLinkClient CBLinkClient;

typedef struct CBLinkClientOptions {
    const char *interface;      /* required */
    /* Our address on it, NULL the interface's: the tests' loopback, where
     * the server is 127.0.0.1, uses 127.0.0.2. */
    const char *address;
    const char *broadcast;      /* as CBLinkOptions */
    int         number;         /* the player number: 3 */
    const char *name;           /* "CDJ-3000" */
    /* Do not announce: browse a server that answers anyone (cyanbox with
     * any_peer). `server` is then required. */
    bool        quiet;
    const char *server;         /* the server's address; NULL: the computer source heard */
    CBLinkPorts ports;          /* ours: announce and status, 0 any */
    CBLinkPorts server_ports;   /* the server's: announce, status, query and portmap */
} CBLinkClientOptions;

void cb_link_client_options_default(CBLinkClientOptions *o);
int  cb_link_client_start(CBLinkClient **out, const CBLinkClientOptions *o);
void cb_link_client_stop(CBLinkClient **c);
void cb_link_client_bound_ports(CBLinkClient *c, CBLinkPorts *out);
/* Waits up to timeout_ms for a computer source (or the named server):
 * its address and device number; -ETIMEDOUT when none answered. */
int  cb_link_client_wait_source(CBLinkClient *c, int timeout_ms, uint32_t *addr, int *number);
/* What our status says from now on. */
void cb_link_client_set_loaded(CBLinkClient *c, int device, int slot, uint32_t track_id,
                               bool playing);
/* A whole file from the source over NFS, read as a player reads it. */
int  cb_link_client_fetch(CBLinkClient *c, const char *path, uint8_t **data, size_t *size);
/* The last file fetched, again, through the mount the first fetch made: as
 * a deck reads on with the handles it has, never mounting again. -ENOENT
 * when nothing was fetched. */
int  cb_link_client_fetch_again(CBLinkClient *c, uint8_t **data, size_t *size);
/* What our status says is loaded (as set, or as a load command from the
 * source left it): its device, slot and track id, 0 for none. */
void cb_link_client_loaded(CBLinkClient *c, int *device, int *slot, uint32_t *track_id);
/* The loaded track, `track`, from its start: its beat grid asked of the
 * source, our status then says the beat number and tempo as it plays, as a
 * nexus player's does, and it stops at its end; playing or paused as we
 * were (a load does not start a player). The number of beats, or a
 * negative errno (the source has no grid for it, or is gone). */
int  cb_link_client_play(CBLinkClient *c, uint32_t track);

/* LOADs My Settings from the source, as a player's UTILITY does: the
 * 40-byte MYSETTING.DAT and MYSETTING2.DAT payloads it answered within
 * timeout_ms (docs/settings.md sections 3 and 4); -EAGAIN before a source
 * is found, -ETIMEDOUT when it did not answer. */
int  cb_link_client_load_settings(CBLinkClient *c, int timeout_ms, uint8_t mysetting[40],
                                  uint8_t mysetting2[40]);
/* Waits up to timeout_ms for the source to push us its My Settings (as
 * cb_link_push_settings does): the payloads, as cb_link_client_load_settings
 * gives them; -ETIMEDOUT when none came. */
int  cb_link_client_wait_pushed_settings(CBLinkClient *c, int timeout_ms, uint8_t mysetting[40],
                                         uint8_t mysetting2[40]);
/* LOADs the mixer's My Settings from the source, as a DJM-A9 does: the
 * 52-byte DJMMYSETTING.DAT payload it answered within timeout_ms
 * (docs/settings.md section 5); errors as cb_link_client_load_settings. */
int  cb_link_client_load_djm_settings(CBLinkClient *c, int timeout_ms, uint8_t djmmysetting[52]);
/* Saves a cue on `track` as a player does on the deck (as a CDJ-3000X
 * sends one): hot cue `hot` (0..7 for A..H, in its slot's pad colour) or
 * with -1 a memory cue, at `time` seconds. 0 and how many cues the
 * source's answer lists; -EPROTO when it refused the cue. */
int  cb_link_client_save_cue(CBLinkClient *c, uint32_t track, int hot, double time, int *nb_cues);

/* What a player does with a source, reported a line at a time: the root
 * and sort menus, all tracks, the playlists; then for `track` (0: the first one) its metadata, track
 * info, artwork, beat grid, waveforms and cues, the requests a CDJ-3000
 * makes after a load (user and delivery info), and its file over NFS,
 * whose size must be the one the track info gave; our status then says
 * it is loaded and playing. 0, or the first error. */
typedef void (*cb_link_line_cb)(void *opaque, const char *line);
int  cb_link_client_browse(CBLinkClient *c, uint32_t track, cb_link_line_cb line, void *opaque);

#endif /* CYANBOX_LINK_H */
