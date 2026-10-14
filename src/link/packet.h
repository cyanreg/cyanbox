/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The DJ Link packets presence reads and writes, on UDP 50000 (announce,
 * device number claims, keep-alives) and 50002 (status, media queries and
 * the questions players ask a computer source): pure functions from bytes
 * to a struct and back, for presence.c, its tests and its fuzzer.
 *
 * Two header layouts (djl-analysis, examples/startup_shared.edn and
 * status_shared.edn). On 50000: the ten-byte magic, the kind at 0x0a, a
 * subtype at 0x0b, the twenty-byte name at 0x0c, then 01, a byte that
 * tells the layout apart (rbxport: 03 for rekordbox 7 and the CDJ-3000,
 * 02 for older devices) and the whole packet's length at 0x22. On 50002:
 * the magic, the kind, the name at 0x0b, then 01, a subtype at 0x20, the
 * sender's device number at 0x21 and, at 0x22, the length of what follows
 * 0x24.
 *
 * Every parser reads through RXRd within what arrived and within what the
 * length field says, and refuses a packet whose length field claims more
 * than arrived, or which ends before the fields it needs; nothing is
 * allocated. Every builder writes through RXWr, so a buffer too small
 * leaves err set and the packet unsent. */

#ifndef RXBOX_LINK_PACKET_H
#define RXBOX_LINK_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_priv.h"
#include "presence.h"

/* Room for any packet presence builds: the identity reply, 296 bytes, is
 * the longest. Datagrams up to 64 KiB are parsed. */
#define RX_PKT_MAX 512

/* Kinds on port 50000 (djl-analysis packets.adoc; 03 and 07 from rbxport's
 * reading of rekordbox 7.2.11). */
enum RXAnnounceKind {
    RX_ANN_CLAIM1       = 0x00,     /* first-stage claim: a device starting up */
    RX_ANN_WILL_ASSIGN  = 0x01,     /* a mixer will assign a number */
    RX_ANN_CLAIM2       = 0x02,     /* second-stage claim; rekordbox's number probe */
    RX_ANN_NUMBER_REPLY = 0x03,     /* a number in use, or assigned */
    RX_ANN_CLAIM3       = 0x04,     /* final-stage claim */
    RX_ANN_ASSIGN_DONE  = 0x05,
    RX_ANN_KEEP_ALIVE   = 0x06,
    RX_ANN_DISCONNECT   = 0x07,
    RX_ANN_CONFLICT     = 0x08,
    RX_ANN_HELLO        = 0x0a,     /* the first announcement */
};

/* Kinds on port 50002 (djl-analysis packets.adoc; 10, 11, 16, 30, 31, 46
 * and 47 from rbxport's captures of rekordbox 7.2.11). */
enum RXStatusKind {
    RX_ST_MEDIA_QUERY    = 0x05,
    RX_ST_MEDIA_RESPONSE = 0x06,
    RX_ST_CDJ            = 0x0a,    /* a player's status */
    RX_ST_IDENTITY_QUERY = 0x10,
    RX_ST_IDENTITY       = 0x11,
    RX_ST_GREETING       = 0x16,
    RX_ST_LOAD_ACK       = 0x1a,
    RX_ST_MIXER          = 0x29,    /* a mixer's status, and rekordbox's */
    RX_ST_SETTINGS_QUERY = 0x46,
    RX_ST_SETTINGS       = 0x47,
};

/* The second-stage claim's subtypes (rbxport, from rekordbox 7.2.11). */
enum {
    RX_CLAIM2_PROBE  = 0x00,        /* a probe of one number */
    RX_CLAIM2_ASSIGN = 0x01,        /* a request to be assigned one */
    RX_CLAIM2_BLOCK  = 0x02,        /* a block of numbers, as masks */
};

/* The media slots a status names (djl-analysis vcdj.adoc, "Known S_r
 * values"). */
enum {
    RX_SLOT_CD        = 1,
    RX_SLOT_SD        = 2,
    RX_SLOT_USB       = 3,
    RX_SLOT_REKORDBOX = 4,          /* a computer's collection */
};

typedef struct RXPktHeader {
    int    kind;
    int    subtype;                 /* 50000: byte 0x0b; 50002: byte 0x20 */
    int    proto;                   /* 50000: byte 0x21; 50002: 0 */
    int    number;                  /* 50002: the sender's number, byte 0x21; 50000: 0 */
    char   name[21];                /* NUL-terminated, unprintable bytes as '?' */
    size_t len;                     /* the packet's length, as its length field says */
} RXPktHeader;

/* The header of a datagram that arrived on `port`: 0, or -EINVAL when it is
 * no DJ Link packet or its length field claims more than arrived. */
int rx_pkt_header(RXPktHeader *h, enum RXLinkPort port, const uint8_t *buf, size_t len);

/* The kind of device a keep-alive's type byte names, as enum
 * RXLinkDeviceType; 0 for a type nobody has named. */
int rx_pkt_device_kind(int raw_type);

/* ------------------------------------------------------------- port 50000 */

/* A keep-alive (06, 0x36 bytes). The bytes rekordbox and players fill
 * differently are fields here, so a captured keep-alive parses and builds
 * back to the same bytes. */
typedef struct RXPktKeepAlive {
    char     name[21];
    int      proto;                 /* 0x21: 03 rekordbox 7 and CDJ-3000, 02 older */
    int      number;                /* 0x24 */
    int      first;                 /* 0x25: rekordbox 01; a CDJ 02 when it came up alone */
    uint8_t  mac[6];                /* 0x26 */
    uint32_t addr;                  /* 0x2c, host order */
    int      peers;                 /* 0x30 */
    int      flag;                  /* 0x31: rekordbox 01, CDJ-3000 00 */
    int      type;                  /* 0x34, the raw device type */
    int      tail;                  /* 0x35: rekordbox 08, CDJ-3000 64 */
} RXPktKeepAlive;

int  rx_pkt_keep_alive_parse(RXPktKeepAlive *k, const uint8_t *buf, size_t len);
void rx_pkt_keep_alive(RXWr *w, const RXPktKeepAlive *k);

/* The claims of the join: 00, 02 (a probe, an assign request, or a block of
 * numbers), 04, and the first announcement 0a. One struct for all, the
 * fields each carries filled. */
typedef struct RXPktClaim {
    int      kind;                  /* RX_ANN_CLAIM1, _CLAIM2, _CLAIM3, _HELLO */
    int      subtype;               /* RX_ANN_CLAIM2: RX_CLAIM2_* */
    char     name[21];
    int      proto;                 /* 0x21 */
    int      number;                /* 02 at 0x2e, 04 at 0x24 */
    int      counter;               /* 00 at 0x24, 02 at 0x2f, 04 at 0x25, a block's at 0x43 */
    int      type;                  /* the raw device type: 00 at 0x25, 02 at 0x30, 0a at 0x24 */
    int      flag;                  /* 02 at 0x31: rekordbox 01; a CDJ 01 any, 02 a chosen one */
    uint8_t  mac[6];                /* 00 and 02 */
    uint32_t addr;                  /* 02, host order */
    int      wired, wireless;       /* a block's masks of 17..18 and 41..44, 0x30 and 0x33 */
    int      hello;                 /* 0a's byte at 0x25 */
} RXPktClaim;

int  rx_pkt_claim_parse(RXPktClaim *c, const uint8_t *buf, size_t len);
void rx_pkt_claim(RXWr *w, const RXPktClaim *c);

/* The answer to a probe of a number in use, or to an assign request (03,
 * 0x27 bytes): the number, the request's counter, the status (01 in use;
 * to an assign request 00 accepts). */
typedef struct RXPktNumberReply {
    int  subtype;                   /* RX_CLAIM2_PROBE or RX_CLAIM2_ASSIGN */
    char name[21];
    int  number, counter, status;
} RXPktNumberReply;

int  rx_pkt_number_reply_parse(RXPktNumberReply *r, const uint8_t *buf, size_t len);
void rx_pkt_number_reply(RXWr *w, const RXPktNumberReply *r);

/* A device leaving or defending its number (07, 08; 0x29 bytes): the
 * number at 0x24, an address after it. */
typedef struct RXPktConflict {
    int      kind;                  /* RX_ANN_DISCONNECT or RX_ANN_CONFLICT */
    char     name[21];
    int      proto;
    int      number;
    uint32_t addr;
} RXPktConflict;

int  rx_pkt_conflict_parse(RXPktConflict *c, const uint8_t *buf, size_t len);
void rx_pkt_conflict(RXWr *w, const RXPktConflict *c);

/* ------------------------------------------------------------- port 50002 */

/* A player's status (0a) or a mixer's (29), what presence needs of it. The
 * builder writes a CDJ-3000's 0x124 bytes (or a mixer's 0x38) from the
 * same fields. */
typedef struct RXPktStatus {
    int      kind;                  /* RX_ST_CDJ or RX_ST_MIXER */
    int      subtype;
    char     name[21];
    int      number;
    int      track_device, track_slot, track_type;
    uint32_t track_id;
    bool     playing, master, synced, on_air;
    int      play_state;            /* P1 (0x7b): 03 playing, 05 paused ...; 0 none */
    int      bpm_x100;              /* the track's tempo x 100; 0 unknown */
    double   pitch;                 /* +-percent */
    int      beat;                  /* beat number in the track, -1 unknown */
    int      bar_beat;              /* 1..4, 0 unknown */
} RXPktStatus;

int  rx_pkt_status_parse(RXPktStatus *s, const uint8_t *buf, size_t len);
/* A CDJ-3000's status, as the fake player sends it. */
void rx_pkt_cdj_status(RXWr *w, const RXPktStatus *s);
/* rekordbox's status (29, 0x38 bytes): the master's tempo it mirrors, a
 * beat, whether it is master itself. */
void rx_pkt_mixer_status(RXWr *w, const char *name, int number, int bpm_x100, int beat,
                         bool master);

/* A media query (05, 0x30 bytes): who asks (its number and address), about
 * which device's which slot. */
typedef struct RXPktMediaQuery {
    char     name[21];
    int      number;                /* the asker's */
    uint32_t addr;                  /* the asker's, host order */
    int      device, slot;
} RXPktMediaQuery;

int  rx_pkt_media_query_parse(RXPktMediaQuery *q, const uint8_t *buf, size_t len);
void rx_pkt_media_query(RXWr *w, const RXPktMediaQuery *q);

/* A media response (06, 0xc0 bytes) about a computer's collection. */
typedef struct RXPktMedia {
    char name[21];
    int  number;                    /* the answering device's */
    int  device, slot;              /* the slot asked about, named back */
    int  tracks, playlists;
} RXPktMedia;

int  rx_pkt_media_parse(RXPktMedia *m, const uint8_t *buf, size_t len);
void rx_pkt_media(RXWr *w, const RXPktMedia *m);

/* The greeting (16, 0x30 bytes) rekordbox sends a player once. */
void rx_pkt_greeting(RXWr *w, const char *name, int number);
/* A player's identity query (10, 0x24 bytes: the header alone). */
void rx_pkt_identity_query(RXWr *w, const char *name, int number);
/* rekordbox's identity (11, 296 bytes): its number and the computer's name. */
void rx_pkt_identity(RXWr *w, const char *name, int number, const char *computer);
/* A player's request for the source's display settings (46, 0x28 bytes)
 * and the answer (47, 0x48 bytes): the 32-byte DEVSETTING.DAT payload
 * (docs/settings.md section 6), its integers big-endian here. */
void rx_pkt_settings_query(RXWr *w, const char *name, int number);
void rx_pkt_settings(RXWr *w, const char *name, int number, const uint8_t devsetting[32]);

#endif /* RXBOX_LINK_PACKET_H */
