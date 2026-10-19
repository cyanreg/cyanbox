/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The DJ Link packets presence reads and writes, on UDP 50000 (announce,
 * device number claims, keep-alives) and 50002 (status, media queries and
 * the questions players ask a computer source), and LINK CUE's on 50004
 * (cue.c): pure functions from bytes to a struct and back, for presence.c,
 * cue.c, their tests and the fuzzer.
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
 * Every parser reads through CBRd within what arrived and within what the
 * length field says, and refuses a packet whose length field claims more
 * than arrived, or which ends before the fields it needs; nothing is
 * allocated. Every builder writes through CBWr, so a buffer too small
 * leaves err set and the packet unsent. */

#ifndef CYANBOX_LINK_PACKET_H
#define CYANBOX_LINK_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_priv.h"
#include "presence.h"

/* Room for any packet presence builds: the identity reply, 296 bytes, is
 * the longest. Datagrams up to 64 KiB are parsed. */
#define CB_PKT_MAX 512

/* Kinds on port 50000 (djl-analysis packets.adoc; 03 and 07 from rbxport's
 * reading of rekordbox 7.2.11). */
enum CBAnnounceKind {
    CB_ANN_CLAIM1       = 0x00,     /* first-stage claim: a device starting up */
    CB_ANN_WILL_ASSIGN  = 0x01,     /* a mixer will assign a number */
    CB_ANN_CLAIM2       = 0x02,     /* second-stage claim; rekordbox's number probe */
    CB_ANN_NUMBER_REPLY = 0x03,     /* a number in use, or assigned */
    CB_ANN_CLAIM3       = 0x04,     /* final-stage claim */
    CB_ANN_ASSIGN_DONE  = 0x05,
    CB_ANN_KEEP_ALIVE   = 0x06,
    CB_ANN_DISCONNECT   = 0x07,
    CB_ANN_CONFLICT     = 0x08,
    CB_ANN_HELLO        = 0x0a,     /* the first announcement */
};

/* Kinds on port 50002 (djl-analysis packets.adoc; 10, 11, 16, 30, 31, 46
 * and 47 from rbxport's captures of rekordbox 7.2.11; 35 and 36 from
 * dysentery's S16a capture of two CDJ-2000nexus; 42 and 43 from the cdj
 * session's reading of the DJM-A9's firmware). */
enum CBStatusKind {
    CB_ST_MEDIA_QUERY    = 0x05,
    CB_ST_MEDIA_RESPONSE = 0x06,
    CB_ST_CDJ            = 0x0a,    /* a player's status */
    CB_ST_IDENTITY_QUERY = 0x10,
    CB_ST_IDENTITY       = 0x11,
    CB_ST_GREETING       = 0x16,
    CB_ST_LOAD_TRACK     = 0x19,
    CB_ST_LOAD_ACK       = 0x1a,
    CB_ST_MIXER          = 0x29,    /* a mixer's status, and rekordbox's */
    CB_ST_PROPERTY_QUERY = 0x30,
    CB_ST_PROPERTY       = 0x31,
    CB_ST_MY_SETTINGS_QUERY = 0x35,
    CB_ST_MY_SETTINGS    = 0x36,
    CB_ST_DJM_SETTINGS_QUERY = 0x42,
    CB_ST_DJM_SETTINGS   = 0x43,
    CB_ST_SETTINGS_QUERY = 0x46,
    CB_ST_SETTINGS       = 0x47,
};

/* The second-stage claim's subtypes (rbxport, from rekordbox 7.2.11). */
enum {
    CB_CLAIM2_PROBE  = 0x00,        /* a probe of one number */
    CB_CLAIM2_ASSIGN = 0x01,        /* a request to be assigned one */
    CB_CLAIM2_BLOCK  = 0x02,        /* a block of numbers, as masks */
};

/* The media slots a status names (djl-analysis vcdj.adoc, "Known S_r
 * values"). */
enum {
    CB_SLOT_CD        = 1,
    CB_SLOT_SD        = 2,
    CB_SLOT_USB       = 3,
    CB_SLOT_REKORDBOX = 4,          /* a computer's collection */
};

typedef struct CBPktHeader {
    int    kind;
    int    subtype;                 /* 50000: byte 0x0b; 50002: byte 0x20 */
    int    proto;                   /* 50000: byte 0x21; 50002: 0 */
    int    number;                  /* 50002: the sender's number, byte 0x21; 50000: 0 */
    char   name[21];                /* NUL-terminated, unprintable bytes as '?' */
    size_t len;                     /* the packet's length, as its length field says */
} CBPktHeader;

/* The header of a datagram that arrived on `port`: 0, or -EINVAL when it is
 * no DJ Link packet or its length field claims more than arrived. */
int cb_pkt_header(CBPktHeader *h, enum CBLinkPort port, const uint8_t *buf, size_t len);

/* The kind of device a keep-alive's type byte names, as enum
 * CBLinkDeviceType; 0 for a type nobody has named. */
int cb_pkt_device_kind(int raw_type);

/* ------------------------------------------------------------- port 50000 */

/* A keep-alive (06, 0x36 bytes). The bytes rekordbox and players fill
 * differently are fields here, so a captured keep-alive parses and builds
 * back to the same bytes. */
typedef struct CBPktKeepAlive {
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
} CBPktKeepAlive;

int  cb_pkt_keep_alive_parse(CBPktKeepAlive *k, const uint8_t *buf, size_t len);
void cb_pkt_keep_alive(CBWr *w, const CBPktKeepAlive *k);

/* The claims of the join: 00, 02 (a probe, an assign request, or a block of
 * numbers), 04, and the first announcement 0a. One struct for all, the
 * fields each carries filled. */
typedef struct CBPktClaim {
    int      kind;                  /* CB_ANN_CLAIM1, _CLAIM2, _CLAIM3, _HELLO */
    int      subtype;               /* CB_ANN_CLAIM2: CB_CLAIM2_* */
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
} CBPktClaim;

int  cb_pkt_claim_parse(CBPktClaim *c, const uint8_t *buf, size_t len);
void cb_pkt_claim(CBWr *w, const CBPktClaim *c);

/* The answer to a probe of a number in use, or to an assign request (03,
 * 0x27 bytes): the number, the request's counter, the status (01 in use;
 * to an assign request 00 accepts). */
typedef struct CBPktNumberReply {
    int  subtype;                   /* CB_CLAIM2_PROBE or CB_CLAIM2_ASSIGN */
    char name[21];
    int  number, counter, status;
} CBPktNumberReply;

int  cb_pkt_number_reply_parse(CBPktNumberReply *r, const uint8_t *buf, size_t len);
void cb_pkt_number_reply(CBWr *w, const CBPktNumberReply *r);

/* A device leaving or defending its number (07, 08; 0x29 bytes): the
 * number at 0x24, an address after it. */
typedef struct CBPktConflict {
    int      kind;                  /* CB_ANN_DISCONNECT or CB_ANN_CONFLICT */
    char     name[21];
    int      proto;
    int      number;
    uint32_t addr;
} CBPktConflict;

int  cb_pkt_conflict_parse(CBPktConflict *c, const uint8_t *buf, size_t len);
void cb_pkt_conflict(CBWr *w, const CBPktConflict *c);

/* ------------------------------------------------------------- port 50002 */

/* A player's status (0a) or a mixer's (29), what presence needs of it. The
 * builder writes a CDJ-3000's 0x124 bytes (or a mixer's 0x38) from the
 * same fields. */
typedef struct CBPktStatus {
    int      kind;                  /* CB_ST_CDJ or CB_ST_MIXER */
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
} CBPktStatus;

int  cb_pkt_status_parse(CBPktStatus *s, const uint8_t *buf, size_t len);
/* A CDJ-3000's status, as the fake player sends it. */
void cb_pkt_cdj_status(CBWr *w, const CBPktStatus *s);
/* rekordbox's status (29, 0x38 bytes): the master's tempo it mirrors, a
 * beat, whether it is master itself. */
void cb_pkt_mixer_status(CBWr *w, const char *name, int number, int bpm_x100, int beat,
                         bool master);

/* A media query (05, 0x30 bytes): who asks (its number and address), about
 * which device's which slot. */
typedef struct CBPktMediaQuery {
    char     name[21];
    int      number;                /* the asker's */
    uint32_t addr;                  /* the asker's, host order */
    int      device, slot;
} CBPktMediaQuery;

int  cb_pkt_media_query_parse(CBPktMediaQuery *q, const uint8_t *buf, size_t len);
void cb_pkt_media_query(CBWr *w, const CBPktMediaQuery *q);

/* A media response (06, 0xc0 bytes) about a computer's collection. */
typedef struct CBPktMedia {
    char name[21];
    int  number;                    /* the answering device's */
    int  device, slot;              /* the slot asked about, named back */
    int  tracks, playlists;
} CBPktMedia;

int  cb_pkt_media_parse(CBPktMedia *m, const uint8_t *buf, size_t len);
void cb_pkt_media(CBWr *w, const CBPktMedia *m);

/* The greeting (16, 0x30 bytes) rekordbox sends a player once. */
void cb_pkt_greeting(CBWr *w, const char *name, int number);
/* A player's identity query (10, 0x24 bytes: the header alone). */
void cb_pkt_identity_query(CBWr *w, const char *name, int number);
/* rekordbox's identity (11, 296 bytes): its number and the computer's name. */
void cb_pkt_identity(CBWr *w, const char *name, int number, const char *computer);
/* A player's request for the source's display settings (46, 0x28 bytes)
 * and the answer (47, 0x48 bytes): the 32-byte DEVSETTING.DAT payload
 * (docs/settings.md section 6), its integers big-endian here. */
void cb_pkt_settings_query(CBWr *w, const char *name, int number);
void cb_pkt_settings(CBWr *w, const char *name, int number, const uint8_t devsetting[32]);
/* A player's LOAD of My Settings from another device (35, 0x28 bytes: its
 * number, the slot, two zeros; 04 for a computer's collection) and the
 * answer (36): the answerer's number, that slot, 0 for done, a zero, then
 * the 40-byte MYSETTING.DAT payload (docs/settings.md section 3), its two
 * words big-endian, and with MYSETTING2.DAT's 40 bytes (subtype 01, 0x78
 * bytes) or without (subtype 00, 0x50 bytes). A CDJ-3000X (1.41, read
 * from its firmware by the cdj session) takes MYSETTING2 only from a long
 * answer whose version word says 3, and checks the slot and the 0; a
 * CDJ-2000nexus answered short (dysentery's S16a), with the asker's
 * number first. Byte 0x1f is 01 in both, and the deck refuses either
 * without it. */
typedef struct CBPktMySettings {
    char     name[21];
    int      number;                /* the sender's, in the header */
    int      device, slot, result;  /* the four bytes before the payloads */
    uint8_t  mysetting[40];
    uint8_t  mysetting2[40];
    bool     has2;                  /* zeros without */
} CBPktMySettings;

int  cb_pkt_my_settings_query_parse(int *number, int *slot, const uint8_t *buf, size_t len);
void cb_pkt_my_settings_query(CBWr *w, const char *name, int number, int slot);
int  cb_pkt_my_settings_parse(CBPktMySettings *m, const uint8_t *buf, size_t len);
void cb_pkt_my_settings(CBWr *w, const CBPktMySettings *m);

/* A version question (30, the header alone; MixerVersionInfo in the
 * CDJ-3000X's firmware): a DJM-900NXS answers its firmware's version
 * (31, subtype 02, "1.32" in ASCII). A DJM-A9 asks a computer that joined
 * (subtype 03), about once a second up to ten times; any answer from the
 * computer's number has it offer the computer as a source of its My
 * Settings. rekordbox's answer (31, 0x2c bytes, subtype 03: 06 and seven
 * zeros) as rbxport captured it, asked by an XDJ-RX3. */
void cb_pkt_property_query(CBWr *w, const char *name, int number);
void cb_pkt_property(CBWr *w, const char *name, int number);

/* A mixer's LOAD of its My Settings from a computer (42, 0x28 bytes: its
 * number, the slot (04), the computer's number, a zero) and the answer
 * (43): the computer's number, that slot, 0 for done, a zero; the chunk's
 * index and the count of chunks (u16 each, 0 of 1 here), its length and
 * the total (u32 each, both the blob's); the blob: its length after this
 * word, then the 52-byte DJMMYSETTING.DAT payload (docs/settings.md
 * section 5) with its six words big-endian. An A9 reads one chunk of
 * 0x58b bytes at most and blobs to 0x800, extended entries after the 32
 * setting bytes; the parser reads a single chunk of 32 setting bytes and
 * leaves out the entries. */
typedef struct CBPktDjmSettings {
    char     name[21];
    int      number;                /* the sender's, in the header */
    int      device, slot, result;
    uint8_t  djmmysetting[52];
} CBPktDjmSettings;

int  cb_pkt_djm_settings_query_parse(int *asker, int *slot, int *target, const uint8_t *buf,
                                     size_t len);
void cb_pkt_djm_settings_query(CBWr *w, const char *name, int number, int slot, int target);
int  cb_pkt_djm_settings_parse(CBPktDjmSettings *m, const uint8_t *buf, size_t len);
void cb_pkt_djm_settings(CBWr *w, const CBPktDjmSettings *m);

/* A load command (19, 0x58 bytes), as beat-link's VirtualCdj sends it
 * (sendLoadTrackCommand), to the target's status port: the player
 * `target` loads track `id` of type `type` from slot `slot` of device
 * `device`, as for a track dragged onto it in rekordbox. An emulated
 * CDJ-900 (4.32) and CDJ-2000NXS2 (1.87) loaded and played what it named
 * (2026-10-15); the NXS2 answers with a 1a. */
typedef struct CBPktLoad {
    char     name[21];
    int      number;                /* the sender's */
    int      target;
    int      device, slot, type;
    uint32_t id;
} CBPktLoad;
int  cb_pkt_load_track_parse(CBPktLoad *l, const uint8_t *buf, size_t len);
void cb_pkt_load_track(CBWr *w, const CBPktLoad *l);
/* The answer (1a, 0x28 bytes): the player's number, then 01 (an NXS2's,
 * [ASSUME] that it says the load was taken). */
void cb_pkt_load_ack(CBWr *w, const char *name, int number);

/* ------------------------------------------------------------- port 50004 */

/* LINK CUE (docs/LINK.md, "LINK CUE"): a mixer's headphone cue fed with a
 * source's audio. A DJM sends each source it takes audio from a timing
 * packet every 7 ms; the source it has elected answers each with the next
 * 7 ms. Layouts from djl-analysis touch_audio.adoc (a CDJ-3000 and a
 * DJM-900NXS2 captured) and what the DJM-A9's firmware 1.22 reads of them
 * (docs/LINK.md, "Sources"); the header is the status port's. */
enum CBCueKind {
    CB_CUE_AUDIO    = 0x1e,         /* a source's 7 ms of audio, to the mixer */
    CB_CUE_HANDOVER = 0x1f,         /* the mixer to a source: elected */
    CB_CUE_TIMING   = 0x20,         /* the mixer's clock, every 7 ms */
};

/* What an audio packet asks of the mixer (byte 0x24, the DJM-A9's
 * reading): go on; engage (from the source elected and LINK CUE on: stop);
 * take over (from the source elected: go on). */
enum {
    CB_CUE_GO_ON  = 0,
    CB_CUE_ENGAGE = 1,
    CB_CUE_TAKE   = 2,
};

#define CB_CUE_FRAMES     336       /* an audio packet's: 7 ms of 48 kHz stereo */
#define CB_CUE_AUDIO_SIZE 0x56c     /* every audio packet's size, with audio or not */

/* A timing packet (20, 0x2a bytes). */
typedef struct CBPktCueTiming {
    char     name[21];
    int      mixer;                 /* its device number, 0x21 */
    uint32_t counter;               /* 0x24, one more every packet */
    bool     on;                    /* 0x28: the mixer's LINK CUE is on */
    int      source;                /* 0x29: the source elected last, 0 none yet */
} CBPktCueTiming;

int  cb_pkt_cue_timing_parse(CBPktCueTiming *t, const uint8_t *buf, size_t len);
void cb_pkt_cue_timing(CBWr *w, const CBPktCueTiming *t);
/* A handover (1f, 0x24 bytes: the header alone): the mixer's number. */
int  cb_pkt_cue_handover_parse(int *mixer, const uint8_t *buf, size_t len);
void cb_pkt_cue_handover(CBWr *w, const char *name, int mixer);

/* An audio packet (1e, always CB_CUE_AUDIO_SIZE bytes): the source's
 * number, what it asks, the counter of the timing it answers, and 336
 * frames of 16-bit stereo (little-endian, interleaved), or none: the
 * length field then says 0x2c, the header alone, and the rest is zeros. */
typedef struct CBPktCueAudio {
    char     name[21];
    int      number;
    int      mode;                  /* CB_CUE_GO_ON, _ENGAGE or _TAKE */
    uint32_t counter;
    bool     audio;
    int16_t  pcm[CB_CUE_FRAMES * 2];
} CBPktCueAudio;

int  cb_pkt_cue_audio_parse(CBPktCueAudio *a, const uint8_t *buf, size_t len);
void cb_pkt_cue_audio(CBWr *w, const CBPktCueAudio *a);

#endif /* CYANBOX_LINK_PACKET_H */
