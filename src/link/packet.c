/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The DJ Link packets presence reads and writes (packet.h). Layouts are
 * Deep Symmetry's djl-analysis (startup.adoc, vcdj.adoc, media.adoc) and
 * what rbxport captured rekordbox 7.2.11 sending a CDJ-3000 and the
 * CDJ-3000 sending back (rbl-prolink tests/packets.rs, rbl-link
 * tests/beacon.rs and its fixtures). tests/link/test_packets.c holds those
 * captures and checks every builder against them byte for byte.
 *
 * Where rekordbox and the players fill a byte differently and nobody knows
 * what it means, the byte is a field of the struct, set by presence.c to
 * what the role it plays sends; the fixed bytes are what the captures
 * show. The parsers are lenient where real devices are: the CDJ status
 * length field is not trusted (beat-link's CdjStatus: the XDJ-1000 says one
 * length and sends another), every other length field must not claim more
 * than arrived. */

#include "cyanbox/common.h"

#include "packet.h"

/* ------------------------------------------------------------- the header */

static void get_name(char out[21], const uint8_t *p)
{
    int n = 0;
    while (n < 20 && p[n]) {
        /* Model names are ASCII; anything else would only reach a log line
         * or the Link panel as rubbish. */
        out[n] = p[n] >= 0x20 && p[n] < 0x7f ? (char)p[n] : '?';
        n++;
    }
    out[n] = 0;
}

static void put_name(CBWr *w, const char *name)
{
    uint8_t *d = cb_wr_room(w, 20);
    if (!d)
        return;
    memset(d, 0, 20);
    size_t n = strnlen(name, 20);
    memcpy(d, name, n);
}

int cb_pkt_header(CBPktHeader *h, enum CBLinkPort port, const uint8_t *buf, size_t len)
{
    *h = (CBPktHeader) { 0 };
    CBRd r = cb_rd(buf, len);
    const uint8_t *magic = cb_rd_bytes(&r, sizeof(cb_link_magic));
    if (!magic || memcmp(magic, cb_link_magic, sizeof(cb_link_magic)))
        return CB_ERROR(EINVAL);
    h->kind = cb_rd_u8(&r);

    size_t total;
    const uint8_t *name;
    if (port == CB_LINK_ANNOUNCE) {
        /* The length is the whole packet's (djl-analysis startup.adoc:
         * "len_p ... the length of the entire packet"). */
        h->subtype = cb_rd_u8(&r);
        name = cb_rd_bytes(&r, 20);
        cb_rd_skip(&r, 1);
        h->proto = cb_rd_u8(&r);
        total = cb_rd_u16(&r);
        /* A DJM-900NXS (firmware 1.32) writes it little-endian: its htons
         * swaps the bytes on a big-endian CPU (its emulator, 2026-10-16:
         * 36 00 for a 54-byte keep-alive). A length that does not fit read
         * one way but does read the other is taken that way. */
        size_t swapped = (total >> 8 | total << 8) & 0xffff;
        if (total > len && swapped >= 0x24 && swapped <= len)
            total = swapped;
    } else {
        /* The length is what follows it (djl-analysis vcdj.adoc, len_r),
         * except in rekordbox's own status, subtype 01, where it is the
         * whole packet's again (vcdj.adoc, "Mixer Status Packets"; rbxport's
         * capture of rekordbox 7.2.11 says 0x38 for 0x38 bytes). LINK CUE's
         * packets on 50004 have the same header. */
        name = cb_rd_bytes(&r, 20);
        cb_rd_skip(&r, 1);
        h->subtype = cb_rd_u8(&r);
        h->number = cb_rd_u8(&r);
        size_t declared = cb_rd_u16(&r);
        bool status = port == CB_LINK_STATUS;
        total = status && h->kind == CB_ST_MIXER && h->subtype == 1 ? declared : 0x24 + declared;
        /* Players do not always say their status's length right (beat-link
         * CdjStatus: an XDJ-1000 reports one payload size and sends
         * another), so a status is read within what arrived; an audio
         * packet's length says how much of it is valid, the packet itself
         * is always CB_CUE_AUDIO_SIZE (touch_audio.adoc). */
        if ((status && h->kind == CB_ST_CDJ) || (!status && h->kind == CB_CUE_AUDIO))
            total = len;
    }
    if (r.err || total < 0x24 || total > len)
        return CB_ERROR(EINVAL);
    get_name(h->name, name);
    h->len = total;
    return 0;
}

/* The packet past its header, bounded by its length field, when it is a
 * `kind` on `port`. */
static bool body(CBRd *r, CBPktHeader *h, enum CBLinkPort port, int kind, const uint8_t *buf,
                 size_t len)
{
    if (cb_pkt_header(h, port, buf, len) < 0 || h->kind != kind)
        return false;
    *r = cb_rd(buf, h->len);
    cb_rd_skip(r, 0x24);
    return true;
}

int cb_pkt_device_kind(int raw_type)
{
    switch (raw_type) {
    case 1:
        return CB_LINK_DEVICE_PLAYER;
    case 7:
        /* An all-in-one's session (rbxport, from rekordbox's decompiled
         * linkUpFunc: type 7 brings the link up, numbers 9..12) [ASSUME
         * that it is a player]. */
        return CB_LINK_DEVICE_PLAYER;
    case 2:
    case 3:
        /* 02 in djl-analysis; a DJM-V5 sends 03 (rbxport's capture of
         * 2026-09-13). */
        return CB_LINK_DEVICE_MIXER;
    case 4:
        /* rekordbox 7.2.11's own keep-alive (rbxport's capture). */
        return CB_LINK_DEVICE_COMPUTER;
    case 6:
        /* Holds rekordbox's wireless numbers 41..44 (rbxport's reading of
         * rekordbox's rediscovery) [ASSUME: rekordbox for mobile]. */
        return CB_LINK_DEVICE_COMPUTER;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------- port 50000 */

static size_t ann_header(CBWr *w, int kind, int subtype, const char *name, int proto)
{
    size_t start = w->len;
    cb_wr_bytes(w, cb_link_magic, sizeof(cb_link_magic));
    cb_wr_u8(w, (uint8_t)kind);
    cb_wr_u8(w, (uint8_t)subtype);
    put_name(w, name);
    cb_wr_u8(w, 0x01);
    cb_wr_u8(w, (uint8_t)proto);
    cb_wr_u16(w, 0);
    return start;
}

static void ann_finish(CBWr *w, size_t start)
{
    cb_wr_at_u16(w, start + 0x22, (uint16_t)(w->len - start));
}

int cb_pkt_keep_alive_parse(CBPktKeepAlive *k, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    /* A keep-alive with another subtype is not one (rbxport: rekordbox
     * 7.2.11 reads subtype 00 only). */
    if (!body(&r, &h, CB_LINK_ANNOUNCE, CB_ANN_KEEP_ALIVE, buf, len) || h.subtype)
        return CB_ERROR(EINVAL);
    *k = (CBPktKeepAlive) { .proto = h.proto };
    memcpy(k->name, h.name, sizeof(k->name));
    k->number = cb_rd_u8(&r);
    k->first = cb_rd_u8(&r);
    const uint8_t *mac = cb_rd_bytes(&r, 6);
    k->addr = cb_rd_u32(&r);
    k->peers = cb_rd_u8(&r);
    k->flag = cb_rd_u8(&r);
    cb_rd_skip(&r, 2);
    k->type = cb_rd_u8(&r);
    k->tail = cb_rd_u8(&r);
    if (r.err)
        return CB_ERROR(EINVAL);
    memcpy(k->mac, mac, 6);
    return 0;
}

void cb_pkt_keep_alive(CBWr *w, const CBPktKeepAlive *k)
{
    size_t start = ann_header(w, CB_ANN_KEEP_ALIVE, 0, k->name, k->proto);
    cb_wr_u8(w, (uint8_t)k->number);
    cb_wr_u8(w, (uint8_t)k->first);
    cb_wr_bytes(w, k->mac, 6);
    cb_wr_u32(w, k->addr);
    cb_wr_u8(w, (uint8_t)k->peers);
    cb_wr_u8(w, (uint8_t)k->flag);
    cb_wr_u16(w, 0);
    cb_wr_u8(w, (uint8_t)k->type);
    cb_wr_u8(w, (uint8_t)k->tail);
    ann_finish(w, start);
}

int cb_pkt_claim_parse(CBPktClaim *c, const uint8_t *buf, size_t len)
{
    CBPktHeader h;
    if (cb_pkt_header(&h, CB_LINK_ANNOUNCE, buf, len) < 0)
        return CB_ERROR(EINVAL);
    CBRd r = cb_rd(buf, h.len);
    cb_rd_skip(&r, 0x24);
    *c = (CBPktClaim) { .kind = h.kind, .subtype = h.subtype, .proto = h.proto };
    memcpy(c->name, h.name, sizeof(c->name));
    const uint8_t *mac = NULL;

    switch (h.kind) {
    case CB_ANN_CLAIM1:
        /* The counter, the device type, the MAC (djl-analysis startup.adoc,
         * "First-stage ... claim"; rbxport's capture of rekordbox). */
        c->counter = cb_rd_u8(&r);
        c->type = cb_rd_u8(&r);
        mac = cb_rd_bytes(&r, 6);
        break;
    case CB_ANN_CLAIM2:
        c->addr = cb_rd_u32(&r);
        mac = cb_rd_bytes(&r, 6);
        if (h.subtype == CB_CLAIM2_BLOCK) {
            /* 68 bytes: a mask of 17..18 at 0x30, of 41..44 at 0x33, the
             * counter at 0x43 (rbxport, from rekordbox's readIdBlkRequset)
             * [ASSUME: no capture]. */
            cb_rd_skip(&r, 2);
            c->wired = cb_rd_u8(&r);
            cb_rd_skip(&r, 2);
            c->wireless = cb_rd_u8(&r);
            cb_rd_skip(&r, 0x43 - 0x34);
            c->counter = cb_rd_u8(&r);
        } else {
            /* The number, the counter, the device type, and whether the
             * sender takes any number (djl-analysis, "Second-stage CDJ
             * device number claim": 01 auto, 02 a chosen number). */
            c->number = cb_rd_u8(&r);
            c->counter = cb_rd_u8(&r);
            c->type = cb_rd_u8(&r);
            c->flag = cb_rd_u8(&r);
        }
        break;
    case CB_ANN_CLAIM3:
        c->number = cb_rd_u8(&r);
        c->counter = cb_rd_u8(&r);
        break;
    case CB_ANN_HELLO:
        /* The device type, and on the CDJ-3000's variant one byte more
         * (djl-analysis, "CDJ-3000 compatible initial announcement"). */
        c->type = cb_rd_u8(&r);
        if (cb_rd_left(&r))
            c->hello = cb_rd_u8(&r);
        break;
    default:
        return CB_ERROR(EINVAL);
    }
    if (r.err)
        return CB_ERROR(EINVAL);
    if (mac)
        memcpy(c->mac, mac, 6);
    return 0;
}

void cb_pkt_claim(CBWr *w, const CBPktClaim *c)
{
    size_t start = ann_header(w, c->kind, c->kind == CB_ANN_CLAIM2 ? c->subtype : 0, c->name,
                              c->proto);
    switch (c->kind) {
    case CB_ANN_CLAIM1:
        cb_wr_u8(w, (uint8_t)c->counter);
        cb_wr_u8(w, (uint8_t)c->type);
        cb_wr_bytes(w, c->mac, 6);
        break;
    case CB_ANN_CLAIM2:
        cb_wr_u32(w, c->addr);
        cb_wr_bytes(w, c->mac, 6);
        if (c->subtype == CB_CLAIM2_BLOCK) {
            cb_wr_u16(w, 0);
            cb_wr_u8(w, (uint8_t)c->wired);
            cb_wr_u16(w, 0);
            cb_wr_u8(w, (uint8_t)c->wireless);
            cb_wr_zeros(w, 0x43 - 0x34);
            cb_wr_u8(w, (uint8_t)c->counter);
        } else {
            cb_wr_u8(w, (uint8_t)c->number);
            cb_wr_u8(w, (uint8_t)c->counter);
            cb_wr_u8(w, (uint8_t)c->type);
            cb_wr_u8(w, (uint8_t)c->flag);
        }
        break;
    case CB_ANN_CLAIM3:
        cb_wr_u8(w, (uint8_t)c->number);
        cb_wr_u8(w, (uint8_t)c->counter);
        break;
    case CB_ANN_HELLO:
        cb_wr_u8(w, (uint8_t)c->type);
        if (c->hello)
            cb_wr_u8(w, (uint8_t)c->hello);
        break;
    default:
        w->err = true;
        return;
    }
    ann_finish(w, start);
}

int cb_pkt_number_reply_parse(CBPktNumberReply *n, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_ANNOUNCE, CB_ANN_NUMBER_REPLY, buf, len))
        return CB_ERROR(EINVAL);
    *n = (CBPktNumberReply) { .subtype = h.subtype };
    memcpy(n->name, h.name, sizeof(n->name));
    n->number = cb_rd_u8(&r);
    n->counter = cb_rd_u8(&r);
    n->status = cb_rd_u8(&r);
    return r.err ? CB_ERROR(EINVAL) : 0;
}

void cb_pkt_number_reply(CBWr *w, const CBPktNumberReply *n)
{
    /* 0x27 bytes; the counter echoes the request's (rbxport: rekordbox's
     * readIdUseRequest answers both a probe and a block so). */
    size_t start = ann_header(w, CB_ANN_NUMBER_REPLY, n->subtype, n->name, 0x03);
    cb_wr_u8(w, (uint8_t)n->number);
    cb_wr_u8(w, (uint8_t)n->counter);
    cb_wr_u8(w, (uint8_t)n->status);
    ann_finish(w, start);
}

int cb_pkt_conflict_parse(CBPktConflict *c, const uint8_t *buf, size_t len)
{
    CBPktHeader h;
    if (cb_pkt_header(&h, CB_LINK_ANNOUNCE, buf, len) < 0 ||
        (h.kind != CB_ANN_CONFLICT && h.kind != CB_ANN_DISCONNECT))
        return CB_ERROR(EINVAL);
    CBRd r = cb_rd(buf, h.len);
    cb_rd_skip(&r, 0x24);
    *c = (CBPktConflict) { .kind = h.kind, .proto = h.proto };
    memcpy(c->name, h.name, sizeof(c->name));
    c->number = cb_rd_u8(&r);
    if (r.err)
        return CB_ERROR(EINVAL);
    /* The address after the number, in the conflict djl-analysis shows and
     * in rekordbox's own (rbxport); a disconnect is read for its number. */
    if (cb_rd_left(&r) >= 4)
        c->addr = cb_rd_u32(&r);
    return 0;
}

void cb_pkt_conflict(CBWr *w, const CBPktConflict *c)
{
    size_t start = ann_header(w, c->kind, 0, c->name, c->proto);
    cb_wr_u8(w, (uint8_t)c->number);
    cb_wr_u32(w, c->addr);
    ann_finish(w, start);
}

/* ------------------------------------------------------------- port 50002 */

static size_t st_header(CBWr *w, int kind, int subtype, const char *name, int number)
{
    size_t start = w->len;
    cb_wr_bytes(w, cb_link_magic, sizeof(cb_link_magic));
    cb_wr_u8(w, (uint8_t)kind);
    put_name(w, name);
    cb_wr_u8(w, 0x01);
    cb_wr_u8(w, (uint8_t)subtype);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u16(w, 0);
    return start;
}

/* The length of what follows the header. */
static void st_finish(CBWr *w, size_t start)
{
    cb_wr_at_u16(w, start + 0x22, (uint16_t)(w->len - start - 0x24));
}

/* Pitch as the packets carry it: 0x100000 is +0%, 0 is -100%, 0x200000
 * +100% (djl-analysis vcdj.adoc, Pitch_1). */
static double pitch_percent(uint32_t v)
{
    return ((double)v - 0x100000) * 100.0 / 0x100000;
}

static uint32_t pitch_value(double percent)
{
    double v = 0x100000 + percent * 0x100000 / 100.0;
    if (!(v > 0))
        return 0;
    return v >= 0x200000 ? 0x200000 : (uint32_t)(v + 0.5);
}

int cb_pkt_status_parse(CBPktStatus *s, const uint8_t *buf, size_t len)
{
    CBPktHeader h;
    if (cb_pkt_header(&h, CB_LINK_STATUS, buf, len) < 0)
        return CB_ERROR(EINVAL);
    CBRd r = cb_rd(buf, h.len);
    *s = (CBPktStatus) {
        .kind     = h.kind,
        .subtype  = h.subtype,
        .number   = h.number,
        .beat     = -1,
    };
    memcpy(s->name, h.name, sizeof(s->name));

    if (h.kind == CB_ST_MIXER) {
        /* 0x38 bytes (djl-analysis, "Mixer status packet"): the flags at
         * 0x27, the pitch at 0x28, the tempo at 0x2e, the beat within the
         * bar at 0x37. A mixer always says it plays and is synced (vcdj.adoc:
         * f0 or d0), so only its master flag is taken. */
        if (h.len < 0x38)
            return CB_ERROR(EINVAL);
        uint8_t f = cb_rd_at_u8(&r, 0x27);
        s->master = f & 0x20;
        s->pitch = pitch_percent(cb_rd_at_u32(&r, 0x28));
        int bpm = cb_rd_at_u16(&r, 0x2e);
        s->bpm_x100 = bpm == 0xffff ? 0 : bpm;
        s->bar_beat = cb_rd_at_u8(&r, 0x37);
        return 0;
    }
    if (h.kind != CB_ST_CDJ)
        return CB_ERROR(EINVAL);

    /* Players send 0xd0 (pre-nexus), 0xd4 (nexus), 0x11b (XDJ-1000), 0x11c
     * and 0x124 (nexus 2, newer firmware; the CDJ-3000 emulator on 3.20,
     * rbxport's fixtures) or 0x200 bytes (CDJ-3000, vcdj.adoc); beat-link's
     * CdjStatus needs 0xcc, and so does this. */
    if (h.len < 0xcc)
        return CB_ERROR(EINVAL);
    s->track_device = cb_rd_at_u8(&r, 0x28);
    s->track_slot = cb_rd_at_u8(&r, 0x29);
    s->track_type = cb_rd_at_u8(&r, 0x2a);
    s->track_id = cb_rd_at_u32(&r, 0x2c);
    s->play_state = cb_rd_at_u8(&r, 0x7b);
    uint8_t f = cb_rd_at_u8(&r, 0x89);
    uint8_t p2 = cb_rd_at_u8(&r, 0x8b);
    /* Pitch_1, the pitch in effect, the one the BPM display shows
     * (vcdj.adoc). */
    s->pitch = pitch_percent(cb_rd_at_u32(&r, 0x8c));
    int bpm = cb_rd_at_u16(&r, 0x92);
    s->bpm_x100 = bpm == 0xffff ? 0 : bpm;
    uint32_t beat = cb_rd_at_u32(&r, 0xa0);
    s->beat = beat == 0xffffffffu || beat > INT32_MAX ? -1 : (int)beat;
    s->bar_beat = cb_rd_at_u8(&r, 0xa6);
    s->master = f & 0x20;
    s->synced = f & 0x10;
    s->on_air = f & 0x08;
    if (h.len >= 0xd4) {
        s->playing = f & 0x40;
    } else {
        /* Pre-nexus players send no flags: playing is inferred from P_1 and
         * P_2 (beat-link CdjStatus.isPlaying). */
        bool moving = p2 == 0x6a || p2 == 0x7a || p2 == 0xfa;
        s->playing = s->play_state == 0x03 || s->play_state == 0x04 ||
                     (s->play_state == 0x09 && moving);
    }
    return 0;
}

/* The bytes of a CDJ-3000's status nothing here sets, as the CDJ-3000
 * emulator (firmware 3.20, EP122) sent them with nothing loaded: rbxport's
 * fixture cdj-status-empty.bin. Their meanings, where known, are
 * vcdj.adoc's: activity and media-state bytes at 0x68..0x73, the firmware
 * at 0x7c, Sync_n at 0x84, M_h at 0x9f, the cue countdown at 0xa4 ("no cue
 * ahead"), nx and the Touch Audio bits at 0xcc, the two settings blocks at
 * 0xd0 and 0xf0; the rest are not known. */
static const struct {
    uint16_t off;
    uint8_t n;
    uint8_t b[14];
} cdj_fixed[] = {
    { 0x68,  4, { 0x01, 0x00, 0x04, 0x04 } },
    { 0x6f,  1, { 0x04 } },
    { 0x73,  1, { 0x04 } },
    { 0x7c,  4, { '3', '.', '2', '0' } },
    { 0x87,  1, { 0x01 } },
    { 0x8a,  1, { 0xff } },
    { 0x9f,  1, { 0xff } },
    { 0xa4,  2, { 0x01, 0xff } },
    { 0xb6,  2, { 0x01, 0x2c } },
    { 0xbb,  1, { 0x02 } },
    { 0xcc,  2, { 0x1f, 0xf3 } },
    { 0xd0, 14, { 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01 } },
    { 0xf0, 14, { 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01 } },
    { 0x113, 1, { 0x07 } },
    { 0x116, 2, { 0x0c, 0x01 } },
    { 0x11b, 1, { 0x1e } },
    { 0x11c, 4, { 0x0c, 0x00, 0x6d, 0x00 } },
};

#define CDJ_STATUS_LEN 0x124

void cb_pkt_cdj_status(CBWr *w, const CBPktStatus *s)
{
    /* Subtype 05, as the CDJ-3000 sends (vcdj.adoc has seen 03..06). */
    size_t start = st_header(w, CB_ST_CDJ, 0x05, s->name, s->number);
    if (!cb_wr_room(w, CDJ_STATUS_LEN - 0x24))
        return;
    uint8_t *p = w->p + start;
    memset(p + 0x24, 0, CDJ_STATUS_LEN - 0x24);
    for (size_t i = 0; i < CB_ARRAY_ELEMS(cdj_fixed); i++)
        memcpy(p + cdj_fixed[i].off, cdj_fixed[i].b, cdj_fixed[i].n);

    bool loaded = s->track_device != 0;
    bool playing = loaded && s->playing;
    p[0x24] = (uint8_t)s->number;
    p[0x28] = (uint8_t)s->track_device;
    p[0x29] = (uint8_t)s->track_slot;
    p[0x2a] = (uint8_t)s->track_type;
    p[0x2c] = (uint8_t)(s->track_id >> 24);
    p[0x2d] = (uint8_t)(s->track_id >> 16);
    p[0x2e] = (uint8_t)(s->track_id >> 8);
    p[0x2f] = (uint8_t)s->track_id;
    /* P_1: none loaded, playing, or paused (vcdj.adoc, "Known P_1 values"). */
    p[0x7b] = s->play_state ? (uint8_t)s->play_state : !loaded ? 0x00 : playing ? 0x03 : 0x05;
    /* The flags; bit 7 is always set on the CDJ-3000 (both of rbxport's
     * fixtures). */
    p[0x89] = (uint8_t)(0x80 | (playing ? 0x40 : 0) | (s->master ? 0x20 : 0) |
                        (s->synced ? 0x10 : 0) | (s->on_air ? 0x08 : 0));
    /* P_2 as the CDJ-3000 sends it: fa moving, fe stopped. */
    p[0x8b] = playing ? 0xfa : 0xfe;

    uint32_t pitch = pitch_value(s->pitch);
    /* M_v: 8000 with a rekordbox track, 0000 another, 7fff none; the
     * tempo, ffff unknown; then the same pair for slip (vcdj.adoc, and the
     * playing fixture repeats them). */
    uint16_t mv = !loaded ? 0x7fff : s->track_type == 1 ? 0x8000 : 0x0000;
    uint16_t bpm = s->bpm_x100 > 0 && s->bpm_x100 < 0xffff ? (uint16_t)s->bpm_x100 : 0xffff;
    CBWr f = cb_wr(p + 0x8c, 0xa8 - 0x8c);
    cb_wr_u32(&f, pitch);
    cb_wr_u16(&f, mv);
    cb_wr_u16(&f, bpm);
    cb_wr_u16(&f, mv);
    cb_wr_u16(&f, bpm);
    /* Pitch_2 follows the fader while playing and drops to 0 when stopped. */
    cb_wr_u32(&f, playing ? pitch : 0);
    cb_wr_u8(&f, 0);
    /* P_3: 00 none, 01 paused, 09 playing forward in vinyl mode (as the
     * playing fixture). */
    cb_wr_u8(&f, !loaded ? 0x00 : playing ? 0x09 : 0x01);
    cb_wr_u8(&f, s->master ? 0x01 : 0x00);
    cb_wr_u8(&f, 0xff);
    cb_wr_u32(&f, s->beat < 0 ? 0xffffffffu : (uint32_t)s->beat);
    cb_wr_room(&f, 2);              /* the cue countdown, fixed above */
    cb_wr_u8(&f, (uint8_t)s->bar_beat);
    /* Pitch_3 the pitch in effect; Pitch_4 the fader's, 0 when stopped. */
    CBWr g = cb_wr(p + 0xc0, 8);
    cb_wr_u32(&g, pitch);
    cb_wr_u32(&g, playing ? pitch : 0);
    st_finish(w, start);
}

void cb_pkt_mixer_status(CBWr *w, const char *name, int number, int bpm_x100, int beat,
                         bool master)
{
    /* rekordbox 7.2.11's, 0x38 bytes, byte for byte as rbxport captured it
     * idle, mirroring a master deck and as master itself: the flags c0 (e0
     * as master: the master bit), the pitch fixed at +0%, 80 before the
     * tempo when a tempo is carried, M_m 01 as master, M_h ff, the beat. */
    size_t start = st_header(w, CB_ST_MIXER, 0x01, name, number);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u16(w, 0);
    cb_wr_u8(w, master ? 0xe0 : 0xc0);
    cb_wr_u32(w, 0x100000);
    cb_wr_u8(w, master || bpm_x100 ? 0x80 : 0x00);
    cb_wr_u8(w, 0);
    cb_wr_u16(w, (uint16_t)CB_CLAMP(bpm_x100, 0, 0xffff));
    cb_wr_u32(w, 0x100000);
    cb_wr_u8(w, master ? 0x01 : 0x00);
    cb_wr_u8(w, 0x09);
    cb_wr_u8(w, 0xff);
    cb_wr_u8(w, (uint8_t)beat);
    cb_wr_at_u16(w, start + 0x22, (uint16_t)(w->len - start));
}

int cb_pkt_media_query_parse(CBPktMediaQuery *q, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_STATUS, CB_ST_MEDIA_QUERY, buf, len))
        return CB_ERROR(EINVAL);
    /* The asker's address, then the device and the slot as 32-bit numbers
     * (djl-analysis media.adoc; rbxport's capture of a CDJ-3000). */
    *q = (CBPktMediaQuery) { .number = h.number };
    memcpy(q->name, h.name, sizeof(q->name));
    q->addr = cb_rd_u32(&r);
    uint32_t device = cb_rd_u32(&r);
    uint32_t slot = cb_rd_u32(&r);
    if (r.err || device > 0xff || slot > 0xff)
        return CB_ERROR(EINVAL);
    q->device = (int)device;
    q->slot = (int)slot;
    return 0;
}

void cb_pkt_media_query(CBWr *w, const CBPktMediaQuery *q)
{
    size_t start = st_header(w, CB_ST_MEDIA_QUERY, 0x00, q->name, q->number);
    cb_wr_u32(w, q->addr);
    cb_wr_u32(w, (uint32_t)q->device);
    cb_wr_u32(w, (uint32_t)q->slot);
    st_finish(w, start);
}

int cb_pkt_media_parse(CBPktMedia *m, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_STATUS, CB_ST_MEDIA_RESPONSE, buf, len) || h.len < 0xb0)
        return CB_ERROR(EINVAL);
    *m = (CBPktMedia) { .number = h.number };
    memcpy(m->name, h.name, sizeof(m->name));
    m->device = (int)(cb_rd_at_u32(&r, 0x24) & 0xff);
    m->slot = (int)(cb_rd_at_u32(&r, 0x28) & 0xff);
    m->tracks = cb_rd_at_u16(&r, 0xa6);
    m->playlists = cb_rd_at_u16(&r, 0xae);
    return 0;
}

void cb_pkt_media(CBWr *w, const CBPktMedia *m)
{
    /* rekordbox 7.2.11's answer about its collection (rbxport's captures,
     * slots 03 and 04): subtype 01, the device and slot asked about, the
     * name again in UTF-16BE in 0x40 bytes, no creation date, the track
     * count at 0xa6, colour none, rekordbox tracks (01), My Settings
     * present (01), the playlist count at 0xae, no sizes (media.adoc). */
    size_t start = st_header(w, CB_ST_MEDIA_RESPONSE, 0x01, m->name, m->number);
    cb_wr_u32(w, (uint32_t)m->device);
    cb_wr_u32(w, (uint32_t)m->slot);
    uint8_t *name = cb_wr_room(w, 0x40);
    if (name) {
        memset(name, 0, 0x40);
        CBWr n = cb_wr(name, 0x3e);
        cb_wr_utf16(&n, m->name, true, false);
    }
    cb_wr_zeros(w, 0xa6 - 0x6c);
    cb_wr_u16(w, (uint16_t)CB_CLAMP(m->tracks, 0, 0xffff));
    cb_wr_u8(w, 0x00);
    cb_wr_u8(w, 0x00);
    cb_wr_u8(w, 0x01);
    cb_wr_u8(w, 0x01);
    cb_wr_u16(w, 0);
    cb_wr_u16(w, (uint16_t)CB_CLAMP(m->playlists, 0, 0xffff));
    cb_wr_zeros(w, 16);
    st_finish(w, start);
}

void cb_pkt_greeting(CBWr *w, const char *name, int number)
{
    /* 0x30 bytes with a length field of 0, as rbxport captured it; what it
     * means is not known. */
    st_header(w, CB_ST_GREETING, 0x01, name, number);
    cb_wr_zeros(w, 0x30 - 0x24);
}

void cb_pkt_identity_query(CBWr *w, const char *name, int number)
{
    /* The header alone, 0x24 bytes (rbxport: "magic, the byte, the
     * player's name"); the subtype is the media query's [ASSUME]. */
    st_header(w, CB_ST_IDENTITY_QUERY, 0x00, name, number);
}

void cb_pkt_identity(CBWr *w, const char *name, int number, const char *computer)
{
    /* 296 bytes: our number twice around 01, then the computer's name in
     * UTF-16BE, zero-padded (rbxport's capture of 2026-09-13; without it a
     * player keeps asking and never lists the source). */
    size_t start = st_header(w, CB_ST_IDENTITY, 0x01, name, number);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u8(w, 0x01);
    cb_wr_u16(w, 0);
    uint8_t *text = cb_wr_room(w, 296 - 0x28);
    if (text) {
        memset(text, 0, 296 - 0x28);
        CBWr t = cb_wr(text, 296 - 0x28 - 2);
        cb_wr_utf16(&t, computer, true, false);
    }
    st_finish(w, start);
}

void cb_pkt_settings_query(CBWr *w, const char *name, int number)
{
    /* As the CDJ-3000 sent it (rbxport's beacon test): our number, 04 (the
     * rekordbox slot, it seems), 00 e4 [UNKNOWN]. */
    size_t start = st_header(w, CB_ST_SETTINGS_QUERY, 0x00, name, number);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u8(w, 0x04);
    cb_wr_u8(w, 0x00);
    cb_wr_u8(w, 0xe4);
    st_finish(w, start);
}

int cb_pkt_load_track_parse(CBPktLoad *l, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_STATUS, CB_ST_LOAD_TRACK, buf, len) || h.len < 0x58)
        return CB_ERROR(EINVAL);
    *l = (CBPktLoad) {
        .number = h.number,
        .device = cb_rd_at_u8(&r, 0x28),
        .slot   = cb_rd_at_u8(&r, 0x29),
        .type   = cb_rd_at_u8(&r, 0x2a),
        .id     = cb_rd_at_u32(&r, 0x2c),
        .target = cb_rd_at_u8(&r, 0x40) + 1,
    };
    memcpy(l->name, h.name, sizeof(l->name));
    return 0;
}

void cb_pkt_load_track(CBWr *w, const CBPktLoad *l)
{
    /* The sender's number at 0x24, the track's device, slot and type at
     * 0x28, its id (big-endian) at 0x2c, 32 at 0x33 [UNKNOWN] and the
     * target's number less one at 0x40, the rest zeros (beat-link). */
    size_t start = st_header(w, CB_ST_LOAD_TRACK, 0x00, l->name, l->number);
    uint8_t *b = cb_wr_room(w, 0x58 - 0x24);
    if (b) {
        memset(b, 0, 0x58 - 0x24);
        b[0x24 - 0x24] = (uint8_t)l->number;
        b[0x28 - 0x24] = (uint8_t)l->device;
        b[0x29 - 0x24] = (uint8_t)l->slot;
        b[0x2a - 0x24] = (uint8_t)l->type;
        b[0x2c - 0x24] = (uint8_t)(l->id >> 24);
        b[0x2d - 0x24] = (uint8_t)(l->id >> 16);
        b[0x2e - 0x24] = (uint8_t)(l->id >> 8);
        b[0x2f - 0x24] = (uint8_t)l->id;
        b[0x33 - 0x24] = 0x32;
        b[0x40 - 0x24] = (uint8_t)(l->target - 1);
    }
    st_finish(w, start);
}

void cb_pkt_load_ack(CBWr *w, const char *name, int number)
{
    size_t start = st_header(w, CB_ST_LOAD_ACK, 0x00, name, number);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u8(w, 0x01);
    cb_wr_u16(w, 0);
    st_finish(w, start);
}

void cb_pkt_settings(CBWr *w, const char *name, int number, const uint8_t devsetting[32])
{
    /* 0x48 bytes (rbxport's capture): our number, 04, two zeros, then the
     * DEVSETTING.DAT payload with its integers turned big-endian: the
     * magic and version words, sixteen bytes of settings, two 16-bit and
     * one 32-bit field (rbxport reads the last three as integers; they are
     * zero in every file docs/settings.md lists) [ASSUME for those]. */
    size_t start = st_header(w, CB_ST_SETTINGS, 0x01, name, number);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u8(w, 0x04);
    cb_wr_u16(w, 0);
    CBRd d = cb_rd(devsetting, 32);
    cb_wr_u32(w, cb_rd_u32le(&d));
    cb_wr_u32(w, cb_rd_u32le(&d));
    cb_wr_bytes(w, cb_rd_bytes(&d, 16), 16);
    cb_wr_u16(w, cb_rd_u16le(&d));
    cb_wr_u16(w, cb_rd_u16le(&d));
    cb_wr_u32(w, cb_rd_u32le(&d));
    st_finish(w, start);
}

int cb_pkt_my_settings_query_parse(int *number, int *slot, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_STATUS, CB_ST_MY_SETTINGS_QUERY, buf, len) || buf[0x1f] != 0x01)
        return CB_ERROR(EINVAL);
    *number = cb_rd_u8(&r);
    *slot = cb_rd_u8(&r);
    cb_rd_skip(&r, 2);
    return r.err ? CB_ERROR(EINVAL) : 0;
}

void cb_pkt_my_settings_query(CBWr *w, const char *name, int number, int slot)
{
    /* As deck 2 asked deck 1 for its USB's (S16a): 02 03 00 00. */
    size_t start = st_header(w, CB_ST_MY_SETTINGS_QUERY, 0x00, name, number);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u8(w, (uint8_t)slot);
    cb_wr_u16(w, 0);
    st_finish(w, start);
}

int cb_pkt_my_settings_parse(CBPktMySettings *m, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_STATUS, CB_ST_MY_SETTINGS, buf, len) || buf[0x1f] != 0x01)
        return CB_ERROR(EINVAL);
    *m = (CBPktMySettings) { .number = h.number };
    memcpy(m->name, h.name, sizeof(m->name));
    m->device = cb_rd_u8(&r);
    m->slot = cb_rd_u8(&r);
    m->result = cb_rd_u8(&r);
    cb_rd_skip(&r, 1);
    CBWr d = cb_wr(m->mysetting, 40);
    cb_wr_u32le(&d, cb_rd_u32(&r));
    cb_wr_u32le(&d, cb_rd_u32(&r));
    const uint8_t *set = cb_rd_bytes(&r, 32);
    if (!set)
        return CB_ERROR(EINVAL);
    cb_wr_bytes(&d, set, 32);
    /* The second payload as the 3000X reads it: there, and said so. */
    const uint8_t *set2 = h.subtype ? cb_rd_bytes(&r, 40) : NULL;
    if (set2) {
        memcpy(m->mysetting2, set2, 40);
        m->has2 = true;
    }
    return 0;
}

void cb_pkt_my_settings(CBWr *w, const CBPktMySettings *m)
{
    /* The payload as cb_pkt_settings gives DEVSETTING's: its magic and
     * version words big-endian (12345678 00000002 in S16a), the 32 setting
     * bytes as they are; MYSETTING2's 40 as they are. */
    size_t start = st_header(w, CB_ST_MY_SETTINGS, m->has2, m->name, m->number);
    cb_wr_u8(w, (uint8_t)m->device);
    cb_wr_u8(w, (uint8_t)m->slot);
    cb_wr_u8(w, (uint8_t)m->result);
    cb_wr_u8(w, 0);
    CBRd d = cb_rd(m->mysetting, 40);
    cb_wr_u32(w, cb_rd_u32le(&d));
    cb_wr_u32(w, cb_rd_u32le(&d));
    cb_wr_bytes(w, cb_rd_bytes(&d, 32), 32);
    if (m->has2)
        cb_wr_bytes(w, m->mysetting2, 40);
    st_finish(w, start);
}

void cb_pkt_property_query(CBWr *w, const char *name, int number)
{
    st_header(w, CB_ST_PROPERTY_QUERY, 0x03, name, number);
}

void cb_pkt_property(CBWr *w, const char *name, int number)
{
    size_t start = st_header(w, CB_ST_PROPERTY, 0x03, name, number);
    cb_wr_u8(w, 0x06);
    cb_wr_zeros(w, 7);
    st_finish(w, start);
}

int cb_pkt_djm_settings_query_parse(int *asker, int *slot, int *target, const uint8_t *buf,
                                    size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_STATUS, CB_ST_DJM_SETTINGS_QUERY, buf, len) || buf[0x1f] != 0x01)
        return CB_ERROR(EINVAL);
    *asker = cb_rd_u8(&r);
    *slot = cb_rd_u8(&r);
    *target = cb_rd_u8(&r);
    cb_rd_skip(&r, 1);
    return r.err ? CB_ERROR(EINVAL) : 0;
}

void cb_pkt_djm_settings_query(CBWr *w, const char *name, int number, int slot, int target)
{
    /* As an A9 (33) asked computer 17 (the cdj session's capture): 21 04
     * 11 00. */
    size_t start = st_header(w, CB_ST_DJM_SETTINGS_QUERY, 0x00, name, number);
    cb_wr_u8(w, (uint8_t)number);
    cb_wr_u8(w, (uint8_t)slot);
    cb_wr_u8(w, (uint8_t)target);
    cb_wr_u8(w, 0);
    st_finish(w, start);
}

#define DJM_BLOB (4 + 52)

int cb_pkt_djm_settings_parse(CBPktDjmSettings *m, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_STATUS, CB_ST_DJM_SETTINGS, buf, len))
        return CB_ERROR(EINVAL);
    *m = (CBPktDjmSettings) { .number = h.number };
    memcpy(m->name, h.name, sizeof(m->name));
    m->device = cb_rd_u8(&r);
    m->slot = cb_rd_u8(&r);
    m->result = cb_rd_u8(&r);
    cb_rd_skip(&r, 1);
    unsigned index = cb_rd_u16(&r), count = cb_rd_u16(&r);
    uint32_t chunk = cb_rd_u32(&r), total = cb_rd_u32(&r);
    uint32_t blob = cb_rd_u32(&r), magic = cb_rd_u32(&r), version = cb_rd_u32(&r);
    uint32_t n = cb_rd_u32(&r);
    const uint8_t *set = cb_rd_bytes(&r, 32);
    uint32_t entries = cb_rd_u32(&r), size = cb_rd_u32(&r);
    if (r.err || index != 0 || count != 1 || chunk != total || total < DJM_BLOB ||
        blob != total - 4 || magic != 0x12345678 || n != 32)
        return CB_ERROR(EINVAL);
    CBWr d = cb_wr(m->djmmysetting, 52);
    cb_wr_u32le(&d, magic);
    cb_wr_u32le(&d, version);
    cb_wr_u32le(&d, n);
    cb_wr_bytes(&d, set, 32);
    cb_wr_u32le(&d, entries);
    cb_wr_u32le(&d, size);
    return 0;
}

void cb_pkt_djm_settings(CBWr *w, const CBPktDjmSettings *m)
{
    size_t start = st_header(w, CB_ST_DJM_SETTINGS, 0x00, m->name, m->number);
    cb_wr_u8(w, (uint8_t)m->device);
    cb_wr_u8(w, (uint8_t)m->slot);
    cb_wr_u8(w, (uint8_t)m->result);
    cb_wr_u8(w, 0);
    cb_wr_u16(w, 0);
    cb_wr_u16(w, 1);
    cb_wr_u32(w, DJM_BLOB);
    cb_wr_u32(w, DJM_BLOB);
    cb_wr_u32(w, DJM_BLOB - 4);
    CBRd d = cb_rd(m->djmmysetting, 52);
    for (int i = 0; i < 3; i++)
        cb_wr_u32(w, cb_rd_u32le(&d));
    cb_wr_bytes(w, cb_rd_bytes(&d, 32), 32);
    for (int i = 0; i < 2; i++)
        cb_wr_u32(w, cb_rd_u32le(&d));
    st_finish(w, start);
}

/* ------------------------------------------------------------- port 50004 */

int cb_pkt_cue_timing_parse(CBPktCueTiming *t, const uint8_t *buf, size_t len)
{
    CBRd r;
    CBPktHeader h;
    if (!body(&r, &h, CB_LINK_CUE, CB_CUE_TIMING, buf, len))
        return CB_ERROR(EINVAL);
    *t = (CBPktCueTiming) { .mixer = h.number };
    memcpy(t->name, h.name, sizeof(t->name));
    t->counter = cb_rd_u32(&r);
    t->on = cb_rd_u8(&r) != 0;
    t->source = cb_rd_u8(&r);
    return r.err ? CB_ERROR(EINVAL) : 0;
}

void cb_pkt_cue_timing(CBWr *w, const CBPktCueTiming *t)
{
    size_t start = st_header(w, CB_CUE_TIMING, 0x00, t->name, t->mixer);
    cb_wr_u32(w, t->counter);
    cb_wr_u8(w, t->on);
    cb_wr_u8(w, (uint8_t)t->source);
    st_finish(w, start);
}

int cb_pkt_cue_handover_parse(int *mixer, const uint8_t *buf, size_t len)
{
    CBPktHeader h;
    if (cb_pkt_header(&h, CB_LINK_CUE, buf, len) < 0 || h.kind != CB_CUE_HANDOVER)
        return CB_ERROR(EINVAL);
    *mixer = h.number;
    return 0;
}

void cb_pkt_cue_handover(CBWr *w, const char *name, int mixer)
{
    st_header(w, CB_CUE_HANDOVER, 0x00, name, mixer);
}

int cb_pkt_cue_audio_parse(CBPktCueAudio *a, const uint8_t *buf, size_t len)
{
    CBPktHeader h;
    if (cb_pkt_header(&h, CB_LINK_CUE, buf, len) < 0 || h.kind != CB_CUE_AUDIO ||
        h.len < CB_CUE_AUDIO_SIZE)
        return CB_ERROR(EINVAL);
    CBRd r = cb_rd(buf, CB_CUE_AUDIO_SIZE);
    *a = (CBPktCueAudio) {
        .number  = h.number,
        .mode    = cb_rd_at_u8(&r, 0x24),
        .counter = cb_rd_at_u32(&r, 0x28),
        .audio   = cb_rd_at_u16(&r, 0x22) == CB_CUE_AUDIO_SIZE,
    };
    memcpy(a->name, h.name, sizeof(a->name));
    for (int i = 0; i < CB_CUE_FRAMES * 2; i++)
        a->pcm[i] = (int16_t)cb_rd_at_u16le(&r, 0x2c + (size_t)i * 2);
    return 0;
}

void cb_pkt_cue_audio(CBWr *w, const CBPktCueAudio *a)
{
    /* Byte 0x20 is 06 in a CDJ-3000's (touch_audio.adoc); the DJM-A9 does
     * not read it. Without audio the length says the header alone and the
     * samples are zeros, the packet as long all the same. */
    size_t start = st_header(w, CB_CUE_AUDIO, 0x06, a->name, a->number);
    cb_wr_u8(w, (uint8_t)a->mode);
    cb_wr_zeros(w, 3);
    cb_wr_u32(w, a->counter);
    for (int i = 0; i < CB_CUE_FRAMES * 2; i++)
        cb_wr_u16le(w, a->audio ? (uint16_t)a->pcm[i] : 0);
    cb_wr_at_u16(w, start + 0x22, a->audio ? CB_CUE_AUDIO_SIZE : 0x2c);
}
