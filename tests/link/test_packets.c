/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Presence without a socket: the DJ Link packets against other people's
 * bytes, the join against a driven clock, and a computer and fake players
 * wired back to back in memory.
 *
 * The fixed vectors are rbxport's captures of rekordbox 7.2.11 talking to a
 * CDJ-3000 (crates/rbl-prolink/tests/packets.rs, crates/rbl-link/tests/
 * beacon.rs and its fixtures cdj-status-*.bin, the CDJ-3000 emulator on
 * firmware 3.20): what rekordbox sends is built here byte for byte, what
 * the players send is parsed. Layouts nobody captured are checked against
 * djl-analysis's figures by offset. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/link/packet.h"
#include "src/link/presence.h"
#include "tests/formats/test.h"

/* ------------------------------------------------------------- the bytes */

/* rbxport, rbl-prolink tests/packets.rs: rekordbox 7.2.11 (MAC
 * 00:e0:4c:cf:63:2e, 192.168.1.14) and a CDJ-3000 (player 1, 192.168.1.152)
 * on one network with a third device, 2026-09-12. */
static const char rb_keep_alive[] =
    "5173707431576d4a4f4c060072656b6f7264626f78000000000000000000000001030036"
    "110100e04ccf632ec0a8010e020100000408";
static const char cdj_keep_alive[] =
    "5173707431576d4a4f4c060043444a2d3330303000000000000000000000000001030036"
    "01012497ed0b4043c0a80198030000000164";
/* A DJM-V5, device 33 at 192.168.1.66, type 03 (2026-09-13). */
static const char djm_keep_alive[] =
    "5173707431576d4a4f4c0600444a4d2d5635000000000000000000000000000001020036"
    "2102c83dfc1dfea2c0a80142040000000331";
/* rekordbox's join (2026-09-12): the first claim, the probes of 17 in round
 * 1 and of 44 in round 6. */
static const char rb_claim1[] =
    "5173707431576d4a4f4c000072656b6f7264626f7800000000000000000000000103002c"
    "010400e04ccf632e";
static const char rb_probe_17_1[] =
    "5173707431576d4a4f4c020072656b6f7264626f78000000000000000000000001030032"
    "c0a8010e00e04ccf632e11010401";
static const char rb_probe_44_6[] =
    "5173707431576d4a4f4c020072656b6f7264626f78000000000000000000000001030032"
    "c0a8010e00e04ccf632e2c060401";
/* rekordbox's status: idle; mirroring a master deck at 78.08 BPM; as master
 * itself at 130.00 (2026-09-14). */
static const char rb_status_idle[] =
    "5173707431576d4a4f4c2972656b6f7264626f7800000000000000000000000101110038"
    "110000c00010000000000000001000000009ff00";
static const char rb_status_mirror[] =
    "5173707431576d4a4f4c2972656b6f7264626f7800000000000000000000000101110038"
    "110000c00010000080001e80001000000009ff01";
static const char rb_status_master[] =
    "5173707431576d4a4f4c2972656b6f7264626f7800000000000000000000000101110038"
    "110000e000100000800032c8001000000109ff01";
static const char rb_greeting[] =
    "5173707431576d4a4f4c1672656b6f7264626f7800000000000000000000000101110000"
    "000000000000000000000000";
/* The CDJ-3000 emulator (EP122) asks about slot 03; a current CDJ-3000
 * (192.168.1.170) about 04; rekordbox answers each naming the slot back. */
static const char cdj_media_query_3[] =
    "5173707431576d4a4f4c0543444a2d33303030000000000000000000000000010001000c"
    "c0a801980000001100000003";
static const char rb_media_3[] =
    "5173707431576d4a4f4c0672656b6f7264626f780000000000000000000000010111009c"
    "000000110000000300720065006b006f007200640062006f007800000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000009719000001010000027300000000"
    "000000000000000000000000";
static const char cdj_media_query_4[] =
    "5173707431576d4a4f4c0543444a2d33303030000000000000000000000000010001000c"
    "c0a801aa0000001100000004";
static const char rb_media_4[] =
    "5173707431576d4a4f4c0672656b6f7264626f780000000000000000000000010111009c"
    "000000110000000400720065006b006f007200640062006f007800000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000036000001010000000200000000"
    "000000000000000000000000";
/* The CDJ-3000's device-settings request (rbl-link tests/beacon.rs) and
 * rekordbox's answer. */
static const char cdj_settings_query[] =
    "5173707431576d4a4f4c4643444a2d333030300000000000000000000000000100010004"
    "010400e4";
static const char rb_settings[] =
    "5173707431576d4a4f4c4772656b6f7264626f7800000000000000000000000101110024"
    "110400001234567800000001010104010101000002000000000000000000000000000000";
/* rekordbox's identity answer, computer "chrisles-MBP" (2026-09-13). */
static const char rb_identity[] =
    "5173707431576d4a4f4c1172656b6f7264626f7800000000000000000000000101110104"
    "1101000000630068007200690073006c00650073002d004d004200500000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000";
/* A CDJ-3000 acknowledging a load command (2026-09-13). */
static const char cdj_load_ack[] =
    "5173707431576d4a4f4c1a43444a2d333030300000000000000000000000000100030004"
    "03010000";
/* rbl-link tests/fixtures: the CDJ-3000 emulator's status with nothing
 * loaded, and playing track 17181 from device 17's USB slot as master at
 * 125.39 BPM. */
static const char cdj_status_empty[] =
    "5173707431576d4a4f4c0a43444a2d3330303000000000000000000000000001"
    "0501010001000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000100040400000004000000040000000000000000332e3230"
    "00000000000000010080fffe001000007fffffff7fffffff00000000000000ff"
    "ffffffff01ff00000000000000000000000000000000012c0000000200000000"
    "0010000000000000000000001ff3000012345678000000010101010001010000"
    "0000000000000000000000000000000012345678000000010101010001010000"
    "000000000000000000000000000000000000000700000c010000001e0c006d00"
    "00000000";
static const char cdj_status_playing[] =
    "5173707431576d4a4f4c0a43444a2d3330303000000000000000000000000001"
    "0501010001000000110301000000431d0000000c000000026d5c28f2ffffffff"
    "000000000000000c000000000000000000000000000000000000000000000000"
    "00000000000000000100040400000004000000040000000000000003332e3230"
    "000000000000000100e4fffa00100000800030fb800030fb00100000000901ff"
    "00000001000001000000000000000000000000000000012c0000000200000000"
    "0010000000100000000000001ff3000012345678000000010101010001010000"
    "0000000000000000000000000000000012345678000000010101010001010000"
    "00000000000000000000000000000000000000050000077a0000077a0b4b0000"
    "00000000";

static const uint8_t rb_mac[6] = { 0x00, 0xe0, 0x4c, 0xcf, 0x63, 0x2e };
#define RB_ADDR  0xc0a8010eu    /* 192.168.1.14 */
#define CDJ_ADDR 0xc0a80198u    /* 192.168.1.152 */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static size_t unhex(uint8_t *out, size_t cap, const char *hex)
{
    size_t n = 0;
    while (hex[0] && hex[1] && n < cap) {
        out[n++] = (uint8_t)(hexval(hex[0]) << 4 | hexval(hex[1]));
        hex += 2;
    }
    return n;
}

static void check_bytes(int line, const char *what, const uint8_t *got, size_t got_len,
                        const uint8_t *want, size_t want_len)
{
    if (got_len == want_len && !memcmp(got, want, want_len))
        return;
    size_t i = 0;
    while (i < got_len && i < want_len && got[i] == want[i])
        i++;
    fprintf(stderr, "%s:%d: %s: %zu bytes, want %zu; first difference at 0x%zx\n", __FILE__, line,
            what, got_len, want_len, i);
    failures++;
}

static void check_hex(int line, const char *what, const uint8_t *got, size_t got_len,
                      const char *hex)
{
    uint8_t want[1024];
    size_t n = unhex(want, sizeof(want), hex);
    check_bytes(line, what, got, got_len, want, n);
}

#define CHECK_HEX(what, p, n, hex) check_hex(__LINE__, what, p, n, hex)
#define CHECK_W(what, w, hex) check_hex(__LINE__, what, (w).p, (w).len, hex)

typedef struct Pkt {
    uint8_t b[1024];
    size_t len;
} Pkt;

static Pkt pkt(const char *hex)
{
    Pkt k;
    k.len = unhex(k.b, sizeof(k.b), hex);
    return k;
}

#define WR(name) uint8_t name##_buf[RX_PKT_MAX]; RXWr name = rx_wr(name##_buf, sizeof(name##_buf))

/* -------------------------------------------------- a presence on a wire */

typedef struct Out {
    int64_t at;
    enum RXLinkPort port;
    uint32_t to;
    size_t len;
    uint8_t b[RX_PKT_MAX];
} Out;

/* What one presence sent, at what time; the clock the test drives. */
typedef struct Cap {
    Out *out;
    int nb, size;
    int64_t now;
} Cap;

static void cap_send(void *opaque, enum RXLinkPort port, uint32_t to, const uint8_t *buf,
                     size_t len)
{
    Cap *c = opaque;
    CHECK(len <= RX_PKT_MAX && len >= 0x24 && !memcmp(buf, rx_link_magic, 10));
    if (len > RX_PKT_MAX)
        return;
    if (c->nb == c->size) {
        c->size = c->size ? 2 * c->size : 256;
        c->out = realloc(c->out, (size_t)c->size * sizeof(*c->out));
        if (!c->out)
            abort();
    }
    Out *o = &c->out[c->nb++];
    *o = (Out) { .at = c->now, .port = port, .to = to, .len = len };
    memcpy(o->b, buf, len);
}

static RXPresence *computer(Cap *c, bool start_up, const char *host)
{
    RXPresenceConfig cfg = {
        .role     = RX_PRESENCE_COMPUTER,
        .name     = "rekordbox",
        .addr     = RB_ADDR,
        .start_up = start_up,
    };
    memcpy(cfg.mac, rb_mac, 6);
    if (host)
        snprintf(cfg.host, sizeof(cfg.host), "%s", host);
    RXPresence *p = NULL;
    CHECK_EQ(rx_presence_new(&p, &cfg, cap_send, c), 0);
    if (!p)
        abort();
    return p;
}

/* Ticks at every deadline up to `until`. */
static void run_until(RXPresence *p, Cap *c, int64_t until)
{
    int64_t next = rx_presence_tick(p, c->now);
    while (next <= until) {
        c->now = next;
        next = rx_presence_tick(p, c->now);
    }
    c->now = until;
}

/* A datagram in, then a tick, as the service does it. */
static void feed(RXPresence *p, Cap *c, enum RXLinkPort port, uint32_t from, const char *hex)
{
    Pkt k = pkt(hex);
    rx_presence_input(p, port, from, k.b, k.len, c->now);
    rx_presence_tick(p, c->now);
}

static void feed_w(RXPresence *p, Cap *c, enum RXLinkPort port, uint32_t from, const RXWr *w)
{
    rx_presence_input(p, port, from, w->p, w->len, c->now);
    rx_presence_tick(p, c->now);
}

static int count_kind(const Cap *c, int from, enum RXLinkPort port, int kind)
{
    int n = 0;
    for (int i = from; i < c->nb; i++)
        n += c->out[i].port == port && c->out[i].b[0x0a] == kind;
    return n;
}

static const Out *find_kind(const Cap *c, int from, enum RXLinkPort port, int kind)
{
    for (int i = from; i < c->nb; i++) {
        if (c->out[i].port == port && c->out[i].b[0x0a] == kind)
            return &c->out[i];
    }
    return NULL;
}

#define T0 INT64_C(1000000000)
#define MS INT64_C(1000)

/* Field by field: the structs have padding. */
static bool same_keep_alive(const RXPktKeepAlive *a, const RXPktKeepAlive *b)
{
    return !strcmp(a->name, b->name) && a->proto == b->proto && a->number == b->number &&
           a->first == b->first && !memcmp(a->mac, b->mac, 6) && a->addr == b->addr &&
           a->peers == b->peers && a->flag == b->flag && a->type == b->type && a->tail == b->tail;
}

static bool same_claim(const RXPktClaim *a, const RXPktClaim *b)
{
    return a->kind == b->kind && a->subtype == b->subtype && !strcmp(a->name, b->name) &&
           a->proto == b->proto && a->number == b->number && a->counter == b->counter &&
           a->type == b->type && a->flag == b->flag && !memcmp(a->mac, b->mac, 6) &&
           a->addr == b->addr && a->wired == b->wired && a->wireless == b->wireless &&
           a->hello == b->hello;
}

static bool same_status(const RXPktStatus *a, const RXPktStatus *b)
{
    return a->kind == b->kind && a->subtype == b->subtype && !strcmp(a->name, b->name) &&
           a->number == b->number && a->track_device == b->track_device &&
           a->track_slot == b->track_slot && a->track_type == b->track_type &&
           a->track_id == b->track_id && a->playing == b->playing && a->master == b->master &&
           a->synced == b->synced && a->on_air == b->on_air && a->play_state == b->play_state &&
           a->bpm_x100 == b->bpm_x100 && fabs(a->pitch - b->pitch) < 1e-9 &&
           a->beat == b->beat && a->bar_beat == b->bar_beat;
}

/* ------------------------------------------------------------ the codecs */

static void keep_alives(void)
{
    /* rekordbox's own, built from its fields, and read back. */
    RXPktKeepAlive k = {
        .name   = "rekordbox",
        .proto  = 0x03,
        .number = 17,
        .first  = 0x01,
        .addr   = RB_ADDR,
        .peers  = 2,
        .flag   = 0x01,
        .type   = 0x04,
        .tail   = 0x08,
    };
    memcpy(k.mac, rb_mac, 6);
    WR(w);
    rx_pkt_keep_alive(&w, &k);
    CHECK_W("rekordbox keep-alive", w, rb_keep_alive);
    RXPktKeepAlive r;
    CHECK_EQ(rx_pkt_keep_alive_parse(&r, w.p, w.len), 0);
    CHECK(same_keep_alive(&r, &k));

    /* A CDJ-3000's reads as a player and builds back to its bytes. */
    Pkt c = pkt(cdj_keep_alive);
    CHECK_EQ(rx_pkt_keep_alive_parse(&r, c.b, c.len), 0);
    CHECK_STR(r.name, "CDJ-3000");
    CHECK_EQ(r.number, 1);
    CHECK_EQ(r.addr, CDJ_ADDR);
    CHECK_EQ(r.peers, 3);
    CHECK_EQ(r.proto, 3);
    CHECK_EQ(rx_pkt_device_kind(r.type), RX_LINK_DEVICE_PLAYER);
    WR(w2);
    rx_pkt_keep_alive(&w2, &r);
    CHECK_W("CDJ-3000 keep-alive", w2, cdj_keep_alive);

    /* A DJM-V5 says 03 and is a mixer. */
    Pkt d = pkt(djm_keep_alive);
    CHECK_EQ(rx_pkt_keep_alive_parse(&r, d.b, d.len), 0);
    CHECK_STR(r.name, "DJM-V5");
    CHECK_EQ(r.number, 33);
    CHECK_EQ(rx_pkt_device_kind(r.type), RX_LINK_DEVICE_MIXER);
    CHECK_EQ(rx_pkt_device_kind(4), RX_LINK_DEVICE_COMPUTER);
    CHECK_EQ(rx_pkt_device_kind(0x42), 0);

    /* A keep-alive with another subtype is not one. */
    c.b[0x0b] = 1;
    CHECK(rx_pkt_keep_alive_parse(&r, c.b, c.len) < 0);
}

static void claims(void)
{
    RXPktClaim c = { .kind = RX_ANN_CLAIM1, .name = "rekordbox", .proto = 3, .counter = 1,
                     .type = 4 };
    memcpy(c.mac, rb_mac, 6);
    WR(w);
    rx_pkt_claim(&w, &c);
    CHECK_W("first-stage claim", w, rb_claim1);

    RXPktClaim p = {
        .kind = RX_ANN_CLAIM2, .name = "rekordbox", .proto = 3, .number = 17, .counter = 1,
        .type = 4, .flag = 1, .addr = RB_ADDR,
    };
    memcpy(p.mac, rb_mac, 6);
    WR(w2);
    rx_pkt_claim(&w2, &p);
    CHECK_W("probe of 17", w2, rb_probe_17_1);
    p.number = 44;
    p.counter = 6;
    WR(w3);
    rx_pkt_claim(&w3, &p);
    CHECK_W("probe of 44", w3, rb_probe_44_6);

    RXPktClaim r;
    CHECK_EQ(rx_pkt_claim_parse(&r, w3.p, w3.len), 0);
    CHECK(same_claim(&r, &p));
    CHECK_EQ(rx_pkt_claim_parse(&r, w.p, w.len), 0);
    CHECK(same_claim(&r, &c));

    /* The final stage and the CDJ-3000's announcement, by djl-analysis's
     * figures: D and N after the header, 0x26 bytes; type and 40. */
    RXPktClaim f = { .kind = RX_ANN_CLAIM3, .name = "CDJ-3000", .proto = 3, .number = 2,
                     .counter = 3 };
    WR(w4);
    rx_pkt_claim(&w4, &f);
    CHECK_EQ(w4.len, 0x26);
    CHECK_EQ(w4.p[0x0a], 0x04);
    CHECK_EQ(w4.p[0x23], 0x26);
    CHECK_EQ(w4.p[0x24], 2);
    CHECK_EQ(w4.p[0x25], 3);
    RXPktClaim h = { .kind = RX_ANN_HELLO, .name = "CDJ-3000", .proto = 4, .type = 1,
                     .hello = 0x40 };
    WR(w5);
    rx_pkt_claim(&w5, &h);
    CHECK_EQ(w5.len, 0x26);
    CHECK_EQ(w5.p[0x21], 0x04);
    CHECK_EQ(w5.p[0x24], 0x01);
    CHECK_EQ(w5.p[0x25], 0x40);
    CHECK_EQ(rx_pkt_claim_parse(&r, w5.p, w5.len), 0);
    CHECK(same_claim(&r, &h));

    /* A block of numbers (68 bytes, masks at 0x30 and 0x33, counter 0x43). */
    RXPktClaim b = {
        .kind = RX_ANN_CLAIM2, .subtype = RX_CLAIM2_BLOCK, .name = "x", .proto = 3,
        .addr = 0x0a000005, .wired = 2, .wireless = 5, .counter = 255,
    };
    WR(w6);
    rx_pkt_claim(&w6, &b);
    CHECK_EQ(w6.len, 68);
    CHECK_EQ(w6.p[0x30], 2);
    CHECK_EQ(w6.p[0x33], 5);
    CHECK_EQ(w6.p[0x43], 255);
    CHECK_EQ(rx_pkt_claim_parse(&r, w6.p, w6.len), 0);
    CHECK(same_claim(&r, &b));

    /* The number in use (rbxport's reading of rekordbox: number, counter,
     * 01) and a conflict as beat-link sends it. */
    RXPktNumberReply n = { .name = "rekordbox", .number = 17, .counter = 3, .status = 1 };
    WR(w7);
    rx_pkt_number_reply(&w7, &n);
    CHECK_W("in use", w7, "5173707431576d4a4f4c030072656b6f7264626f78000000000000000000000001"
                         "030027110301");
    RXPktNumberReply nr;
    CHECK_EQ(rx_pkt_number_reply_parse(&nr, w7.p, w7.len), 0);
    CHECK(nr.subtype == n.subtype && !strcmp(nr.name, n.name) && nr.number == n.number &&
          nr.counter == n.counter && nr.status == n.status);
    /* rekordbox's own 41-byte 08 (rbxport's reading of its readReject):
     * number 18, 192.168.50.2. */
    RXPktConflict k = { .kind = RX_ANN_CONFLICT, .name = "rekordbox", .proto = 3, .number = 18,
                        .addr = 0xc0a83202 };
    WR(w8);
    rx_pkt_conflict(&w8, &k);
    CHECK_W("conflict", w8, "5173707431576d4a4f4c080072656b6f7264626f780000000000000000000000"
                            "0103002912c0a83202");
    RXPktConflict kr;
    CHECK_EQ(rx_pkt_conflict_parse(&kr, w8.p, w8.len), 0);
    CHECK(kr.kind == k.kind && !strcmp(kr.name, k.name) && kr.proto == k.proto &&
          kr.number == k.number && kr.addr == k.addr);
}

static void status_packets(void)
{
    WR(w);
    rx_pkt_mixer_status(&w, "rekordbox", 17, 0, 0, false);
    CHECK_W("idle status", w, rb_status_idle);
    WR(w2);
    rx_pkt_mixer_status(&w2, "rekordbox", 17, 7808, 1, false);
    CHECK_W("mirroring status", w2, rb_status_mirror);
    WR(w3);
    rx_pkt_mixer_status(&w3, "rekordbox", 17, 13000, 1, true);
    CHECK_W("master status", w3, rb_status_master);

    /* Read as a mixer's: 0x29 with subtype 01 counts the whole packet. */
    RXPktStatus s;
    CHECK_EQ(rx_pkt_status_parse(&s, w3.p, w3.len), 0);
    CHECK_EQ(s.kind, RX_ST_MIXER);
    CHECK_EQ(s.number, 17);
    CHECK(s.master);
    CHECK_EQ(s.bpm_x100, 13000);
    CHECK_EQ(s.bar_beat, 1);
    CHECK(!s.playing);

    /* The CDJ-3000 playing one of device 17's tracks. */
    Pkt p = pkt(cdj_status_playing);
    CHECK_EQ(p.len, 0x124);
    CHECK_EQ(rx_pkt_status_parse(&s, p.b, p.len), 0);
    CHECK_EQ(s.kind, RX_ST_CDJ);
    CHECK_STR(s.name, "CDJ-3000");
    CHECK_EQ(s.number, 1);
    CHECK_EQ(s.track_device, 17);
    CHECK_EQ(s.track_slot, RX_SLOT_USB);
    CHECK_EQ(s.track_type, 1);
    CHECK_EQ(s.track_id, 17181);
    CHECK(s.playing);
    CHECK(s.master);
    CHECK(!s.synced);
    CHECK(!s.on_air);
    CHECK_EQ(s.bpm_x100, 12539);
    CHECK(fabs(s.pitch) < 1e-9);
    CHECK_EQ(s.beat, 1);
    CHECK_EQ(s.bar_beat, 1);
    CHECK_EQ(s.play_state, 3);

    /* Built from what was read, it reads back the same. */
    WR(w4);
    rx_pkt_cdj_status(&w4, &s);
    CHECK_EQ(w4.len, 0x124);
    RXPktStatus s2;
    CHECK_EQ(rx_pkt_status_parse(&s2, w4.p, w4.len), 0);
    CHECK(same_status(&s, &s2));

    /* Nothing loaded: what the fake player sends is the CDJ-3000's own. */
    Pkt e = pkt(cdj_status_empty);
    CHECK_EQ(rx_pkt_status_parse(&s, e.b, e.len), 0);
    CHECK_EQ(s.track_device, 0);
    CHECK_EQ(s.bpm_x100, 0);
    CHECK_EQ(s.beat, -1);
    CHECK(!s.playing && !s.master);
    RXPktStatus fake = { .name = "CDJ-3000", .number = 1, .beat = -1 };
    WR(w5);
    rx_pkt_cdj_status(&w5, &fake);
    CHECK_W("fake player's empty status", w5, cdj_status_empty);

    /* A pre-nexus player's 0xd0 bytes have no flags: P_1 says it plays. */
    uint8_t old[0xd0] = { 0 };
    memcpy(old, e.b, sizeof(old));
    old[0x22] = 0;
    old[0x23] = 0xd0 - 0x24;
    old[0x89] = 0;
    old[0x7b] = 0x03;
    CHECK_EQ(rx_pkt_status_parse(&s, old, sizeof(old)), 0);
    CHECK(s.playing);
    old[0x7b] = 0x05;
    CHECK_EQ(rx_pkt_status_parse(&s, old, sizeof(old)), 0);
    CHECK(!s.playing);
    /* A status whose length field disagrees is still read (an XDJ-1000
     * does that, beat-link); one shorter than 0xcc is not. */
    old[0x23] = 0x10;
    CHECK_EQ(rx_pkt_status_parse(&s, old, sizeof(old)), 0);
    CHECK(rx_pkt_status_parse(&s, old, 0xcb) < 0);
}

/* The load command, byte for byte as beat-link sends it (the bytes an
 * emulated CDJ-900 and CDJ-2000NXS2 loaded from): rxbox (17) tells player
 * 3 to load track 2158 of its own collection; and the answer. */
static void load_command(void)
{
    WR(w);
    RXPktLoad l = { .name = "rxbox", .number = 17, .target = 3, .device = 17, .slot = 4,
                    .type = 1, .id = 2158 };
    rx_pkt_load_track(&w, &l);
    /* beat-link's payload after the name: 01 00 17 00 34 17, then the
     * source, slot and type at 9, the id at 0x0d, 32 at 0x14, the target
     * less one at 0x21. */
    uint8_t want[0x58 - 0x1f] = { 0x01, 0x00, 17, 0x00, 0x34, 17 };
    want[0x09] = 17;
    want[0x0a] = 4;
    want[0x0b] = 1;
    want[0x0f] = 0x08;
    want[0x10] = 0x6e;
    want[0x14] = 0x32;
    want[0x21] = 2;
    CHECK_EQ(w.len, 0x58);
    CHECK_EQ(w_buf[0x0a], RX_ST_LOAD_TRACK);
    CHECK(!memcmp(w_buf + 0x1f, want, sizeof(want)));
    RXPktLoad back;
    CHECK_EQ(rx_pkt_load_track_parse(&back, w_buf, w.len), 0);
    CHECK(back.number == 17 && back.target == 3 && back.device == 17 && back.slot == 4 &&
          back.type == 1 && back.id == 2158 && !strcmp(back.name, "rxbox"));
    CHECK(rx_pkt_load_track_parse(&back, w_buf, 0x50) < 0);

    WR(a);
    rx_pkt_load_ack(&a, "CDJ-2000NXS2", 3);
    static const uint8_t nxs2[] = { 0x01, 0x00, 0x03, 0x00, 0x04, 0x03, 0x01, 0x00, 0x00 };
    CHECK(a.len == 0x28 && a_buf[0x0a] == RX_ST_LOAD_ACK && !memcmp(a_buf + 0x1f, nxs2, sizeof(nxs2)));
}

static void questions(void)
{
    /* Media queries from both firmwares, and rekordbox's answers. */
    Pkt q = pkt(cdj_media_query_3);
    RXPktMediaQuery mq;
    CHECK_EQ(rx_pkt_media_query_parse(&mq, q.b, q.len), 0);
    CHECK_STR(mq.name, "CDJ-3000");
    CHECK_EQ(mq.number, 1);
    CHECK_EQ(mq.addr, CDJ_ADDR);
    CHECK_EQ(mq.device, 17);
    CHECK_EQ(mq.slot, 3);
    WR(w);
    rx_pkt_media_query(&w, &mq);
    CHECK_W("media query", w, cdj_media_query_3);

    RXPktMedia m = { .name = "rekordbox", .number = 17, .device = 17, .slot = 3,
                     .tracks = 38681, .playlists = 627 };
    WR(w2);
    rx_pkt_media(&w2, &m);
    CHECK_W("media response, slot 03", w2, rb_media_3);
    RXPktMedia mr;
    CHECK_EQ(rx_pkt_media_parse(&mr, w2.p, w2.len), 0);
    CHECK(!strcmp(mr.name, m.name) && mr.number == m.number && mr.device == m.device &&
          mr.slot == m.slot && mr.tracks == m.tracks && mr.playlists == m.playlists);
    m.slot = 4;
    m.tracks = 54;
    m.playlists = 2;
    WR(w3);
    rx_pkt_media(&w3, &m);
    CHECK_W("media response, slot 04", w3, rb_media_4);

    WR(w4);
    rx_pkt_greeting(&w4, "rekordbox", 17);
    CHECK_W("greeting", w4, rb_greeting);

    WR(w5);
    rx_pkt_identity(&w5, "rekordbox", 17, "chrisles-MBP");
    CHECK_W("identity", w5, rb_identity);

    WR(w6);
    rx_pkt_settings_query(&w6, "CDJ-3000", 1);
    CHECK_W("settings request", w6, cdj_settings_query);
    static const uint8_t devsetting[32] = {
        0x78, 0x56, 0x34, 0x12, 0x01, 0, 0, 0, 0x01, 0x01, 0x04, 0x01, 0x01, 0x01, 0, 0, 0x02,
    };
    WR(w7);
    rx_pkt_settings(&w7, "rekordbox", 17, devsetting);
    CHECK_W("settings", w7, rb_settings);
}

/* Every captured packet cut short, with its length field lying, or with
 * its magic broken is refused by every parser, and a computer on the link
 * answers none of them. */
static void malformed(void)
{
    static const struct { const char *hex; enum RXLinkPort port; } all[] = {
        { rb_keep_alive, RX_LINK_ANNOUNCE }, { cdj_keep_alive, RX_LINK_ANNOUNCE },
        { djm_keep_alive, RX_LINK_ANNOUNCE }, { rb_claim1, RX_LINK_ANNOUNCE },
        { rb_probe_17_1, RX_LINK_ANNOUNCE }, { rb_probe_44_6, RX_LINK_ANNOUNCE },
        { rb_status_idle, RX_LINK_STATUS }, { rb_status_master, RX_LINK_STATUS },
        { cdj_media_query_3, RX_LINK_STATUS }, { cdj_media_query_4, RX_LINK_STATUS },
        { rb_media_3, RX_LINK_STATUS }, { cdj_settings_query, RX_LINK_STATUS },
        { rb_settings, RX_LINK_STATUS }, { cdj_load_ack, RX_LINK_STATUS },
        { cdj_status_empty, RX_LINK_STATUS }, { cdj_status_playing, RX_LINK_STATUS },
    };
    Cap c = { .now = T0 };
    RXPresence *p = computer(&c, false, NULL);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    run_until(p, &c, T0 + 4000 * MS);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_UP);
    int sent = c.nb;

    for (size_t i = 0; i < RX_ARRAY_ELEMS(all); i++) {
        Pkt k = pkt(all[i].hex);
        bool cdj = all[i].port == RX_LINK_STATUS && k.b[0x0a] == RX_ST_CDJ;
        size_t need = cdj ? 0xcc : k.len;
        for (size_t cut = 0; cut < need; cut++) {
            RXPktKeepAlive ka;
            RXPktClaim cl;
            RXPktStatus st;
            RXPktMediaQuery mq;
            RXPktMedia md;
            bool ok = rx_pkt_keep_alive_parse(&ka, k.b, cut) == 0 ||
                      rx_pkt_claim_parse(&cl, k.b, cut) == 0 ||
                      rx_pkt_status_parse(&st, k.b, cut) == 0 ||
                      rx_pkt_media_query_parse(&mq, k.b, cut) == 0 ||
                      rx_pkt_media_parse(&md, k.b, cut) == 0;
            if (ok)
                fprintf(stderr, "packet %zu read when cut to %zu bytes\n", i, cut);
            CHECK(!ok);
            /* The CDJ's own media query, cut, from the CDJ: no answer. */
            rx_presence_input(p, all[i].port, CDJ_ADDR, k.b, cut, c.now);
        }
        if (!cdj) {
            /* The length field one more than arrived. */
            k.b[0x23]++;
            RXPktHeader h;
            CHECK(rx_pkt_header(&h, all[i].port, k.b, k.len) < 0);
            rx_presence_input(p, all[i].port, CDJ_ADDR, k.b, k.len, c.now);
            k.b[0x23]--;
        }
        k.b[3] ^= 0x20;
        RXPktHeader h;
        CHECK(rx_pkt_header(&h, all[i].port, k.b, k.len) < 0);
        rx_presence_input(p, all[i].port, CDJ_ADDR, k.b, k.len, c.now);
    }
    /* A keep-alive saying it is shorter than its fields. */
    Pkt k = pkt(cdj_keep_alive);
    k.b[0x23] = 0x30;
    RXPktKeepAlive ka;
    CHECK(rx_pkt_keep_alive_parse(&ka, k.b, k.len) < 0);
    CHECK_EQ(c.nb, sent);
    rx_presence_free(&p);
    CHECK(!p);
    free(c.out);
}

/* ----------------------------------------------------------------- joins */

/* The join, packet by packet against the clock: silence until a player is
 * heard, three claims and thirty-six probes 100 ms apart, then the number,
 * a keep-alive, the status and the greeting at once. */
static void join_timeline(void)
{
    Cap c = { .now = T0 - 5000 * MS };
    RXPresence *p = computer(&c, false, NULL);
    run_until(p, &c, T0);
    CHECK_EQ(c.nb, 0);
    int number = -1;
    CHECK_EQ(rx_presence_state(p, &number), RX_LINK_WAITING);
    CHECK_EQ(number, 0);

    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_JOINING);
    run_until(p, &c, T0 + 3899 * MS);
    CHECK_EQ(c.nb, 39);
    for (int i = 0; i < c.nb && i < 39; i++) {
        const Out *o = &c.out[i];
        CHECK_EQ(o->at, T0 + i * 100 * MS);
        CHECK_EQ(o->port, RX_LINK_ANNOUNCE);
        CHECK_EQ(o->to, RX_PRESENCE_BROADCAST);
        if (i < 3) {
            Pkt want = pkt(rb_claim1);
            want.b[0x24] = (uint8_t)(i + 1);
            check_bytes(__LINE__, "claim", o->b, o->len, want.b, want.len);
        } else {
            static const int numbers[6] = { 17, 18, 41, 42, 43, 44 };
            Pkt want = pkt(rb_probe_17_1);
            want.b[0x2e] = (uint8_t)numbers[(i - 3) % 6];
            want.b[0x2f] = (uint8_t)((i - 3) / 6 + 1);
            check_bytes(__LINE__, "probe", o->b, o->len, want.b, want.len);
        }
    }
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_JOINING);

    run_until(p, &c, T0 + 3900 * MS);
    CHECK_EQ(rx_presence_state(p, &number), RX_LINK_UP);
    CHECK_EQ(number, 17);
    CHECK_EQ(c.nb, 42);
    if (c.nb >= 42) {
        Pkt ka = pkt(rb_keep_alive);
        ka.b[0x30] = 1;         /* one other device: the CDJ */
        CHECK_EQ(c.out[39].at, T0 + 3900 * MS);
        CHECK_EQ(c.out[39].to, RX_PRESENCE_BROADCAST);
        check_bytes(__LINE__, "keep-alive", c.out[39].b, c.out[39].len, ka.b, ka.len);
        CHECK_EQ(c.out[40].port, RX_LINK_STATUS);
        CHECK_EQ(c.out[40].to, RX_PRESENCE_BROADCAST);
        CHECK_HEX("status", c.out[40].b, c.out[40].len, rb_status_idle);
        CHECK_EQ(c.out[41].port, RX_LINK_STATUS);
        CHECK_EQ(c.out[41].to, CDJ_ADDR);
        CHECK_HEX("greeting", c.out[41].b, c.out[41].len, rb_greeting);
    }

    /* Then the status every 200 ms and a keep-alive every 2 s. */
    int from = c.nb;
    run_until(p, &c, T0 + 5900 * MS);
    CHECK_EQ(count_kind(&c, from, RX_LINK_STATUS, RX_ST_MIXER), 10);
    CHECK_EQ(count_kind(&c, from, RX_LINK_ANNOUNCE, RX_ANN_KEEP_ALIVE), 1);
    const Out *k = find_kind(&c, from, RX_LINK_ANNOUNCE, RX_ANN_KEEP_ALIVE);
    CHECK(k && k->at == T0 + 5900 * MS);
    for (int i = from; i < c.nb; i++) {
        if (c.out[i].port == RX_LINK_STATUS)
            CHECK_EQ((c.out[i].at - T0 - 3900 * MS) % (200 * MS), 0);
    }
    CHECK_EQ(count_kind(&c, from, RX_LINK_STATUS, RX_ST_GREETING), 0);
    rx_presence_free(&p);
    free(c.out);
}

static int probes_of(const Cap *c, int from, int number)
{
    int n = 0;
    for (int i = from; i < c->nb; i++) {
        const Out *o = &c->out[i];
        n += o->b[0x0a] == RX_ANN_CLAIM2 && o->b[0x0b] == RX_CLAIM2_PROBE && o->b[0x2e] == number;
    }
    return n;
}

static void in_use(RXPresence *p, Cap *c, int number, int subtype, int status)
{
    RXPktNumberReply r = { .subtype = subtype, .name = "rekordbox", .number = number,
                           .counter = 1, .status = status };
    WR(w);
    rx_pkt_number_reply(&w, &r);
    feed_w(p, c, RX_LINK_ANNOUNCE, 0xc0a80114, &w);
}

static void join_variants(void)
{
    /* Another rekordbox holds 17 and answers the probe: 17 is not probed
     * again, the rest of round one and five rounds of five follow, 18 is
     * taken (rbxport's join test). */
    Cap c = { .now = T0 };
    RXPresence *p = computer(&c, false, NULL);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    run_until(p, &c, T0 + 300 * MS);
    CHECK_EQ(probes_of(&c, 0, 17), 1);
    in_use(p, &c, 17, RX_CLAIM2_PROBE, 1);
    int from = c.nb;
    run_until(p, &c, T0 + 5000 * MS);
    CHECK_EQ(probes_of(&c, from, 17), 0);
    int probes = 0;
    for (int n = 18; n <= 44; n++)
        probes += probes_of(&c, from, n);
    CHECK_EQ(probes, 5 + 5 * 5);
    int number = 0;
    CHECK_EQ(rx_presence_state(p, &number), RX_LINK_UP);
    CHECK_EQ(number, 18);
    const Out *k = find_kind(&c, from, RX_LINK_ANNOUNCE, RX_ANN_KEEP_ALIVE);
    CHECK(k && k->b[0x24] == 18);
    rx_presence_free(&p);
    free(c.out);

    /* 17 and 18 held: four numbers six times, then six requests to be
     * assigned 17, then back to waiting; a player heard within the hold
     * does not start it again, one heard after it does. */
    c = (Cap) { .now = T0 };
    p = computer(&c, false, NULL);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    run_until(p, &c, T0 + 250 * MS);
    in_use(p, &c, 17, RX_CLAIM2_PROBE, 1);
    in_use(p, &c, 18, RX_CLAIM2_PROBE, 1);
    from = c.nb;
    run_until(p, &c, T0 + 6000 * MS);
    probes = 0;
    for (int n = 17; n <= 44; n++)
        probes += probes_of(&c, from, n);
    CHECK_EQ(probes, 4 * 6);
    int assigns = 0;
    for (int i = from; i < c.nb; i++) {
        const Out *o = &c.out[i];
        if (o->b[0x0a] == RX_ANN_CLAIM2 && o->b[0x0b] == RX_CLAIM2_ASSIGN) {
            CHECK_EQ(o->b[0x2e], 17);
            CHECK_EQ(o->b[0x2f], assigns + 1);
            assigns++;
        }
    }
    CHECK_EQ(assigns, 6);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_WAITING);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_WAITING);
    run_until(p, &c, T0 + 16000 * MS);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_JOINING);
    rx_presence_free(&p);
    free(c.out);

    /* The same, with an assigning device accepting the first request. */
    c = (Cap) { .now = T0 };
    p = computer(&c, false, NULL);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    run_until(p, &c, T0 + 250 * MS);
    in_use(p, &c, 17, RX_CLAIM2_PROBE, 1);
    in_use(p, &c, 18, RX_CLAIM2_PROBE, 1);
    run_until(p, &c, T0 + 2700 * MS);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_JOINING);
    in_use(p, &c, 17, RX_CLAIM2_ASSIGN, 2);         /* a retry: nothing */
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_JOINING);
    in_use(p, &c, 17, RX_CLAIM2_ASSIGN, 0);
    CHECK_EQ(rx_presence_state(p, &number), RX_LINK_UP);
    CHECK_EQ(number, 17);
    rx_presence_free(&p);
    free(c.out);

    /* Another rekordbox's keep-alive does not bring the link up; a mixer's
     * does. */
    c = (Cap) { .now = T0 };
    p = computer(&c, false, NULL);
    Pkt rb = pkt(rb_keep_alive);
    rb.b[0x2f] = 0x20;  /* another address */
    rb.b[0x2b] = 0x99;  /* and MAC */
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80120, rb.b, rb.len, c.now);
    run_until(p, &c, T0 + 1000 * MS);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_WAITING);
    CHECK_EQ(c.nb, 0);
    CHECK(rx_presence_is_peer(p, 0xc0a80120));
    feed(p, &c, RX_LINK_ANNOUNCE, 0xc0a80142, djm_keep_alive);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_JOINING);
    /* That rekordbox holds 17: its keep-alive during the probing marks it. */
    run_until(p, &c, T0 + 2000 * MS);
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80120, rb.b, rb.len, c.now);
    run_until(p, &c, T0 + 6000 * MS);
    CHECK_EQ(rx_presence_state(p, &number), RX_LINK_UP);
    CHECK_EQ(number, 18);
    /* Its own packets, come back, change nothing. */
    Pkt own = pkt(rb_probe_17_1);
    own.b[0x2e] = 18;
    from = c.nb;
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80177, own.b, own.len, c.now);
    rx_presence_input(p, RX_LINK_ANNOUNCE, RB_ADDR, own.b, own.len, c.now);
    CHECK_EQ(c.nb, from);
    rx_presence_free(&p);
    free(c.out);

    /* A device defending 17 with a conflict, as a CDJ does: 18. */
    c = (Cap) { .now = T0 };
    p = computer(&c, false, NULL);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    run_until(p, &c, T0 + 350 * MS);
    RXPktConflict cf = { .kind = RX_ANN_CONFLICT, .name = "beat-link", .proto = 2, .number = 17,
                         .addr = 0xc0a8011e };
    WR(w);
    rx_pkt_conflict(&w, &cf);
    feed_w(p, &c, RX_LINK_ANNOUNCE, 0xc0a8011e, &w);
    run_until(p, &c, T0 + 5000 * MS);
    CHECK_EQ(rx_presence_state(p, &number), RX_LINK_UP);
    CHECK_EQ(number, 18);
    rx_presence_free(&p);
    free(c.out);

    /* Told to start up, it joins with nobody there. */
    c = (Cap) { .now = T0 };
    p = computer(&c, true, NULL);
    run_until(p, &c, T0 + 3900 * MS);
    CHECK_EQ(rx_presence_state(p, &number), RX_LINK_UP);
    CHECK_EQ(number, 17);
    CHECK_EQ(c.nb, 3 + 36 + 2);
    rx_presence_free(&p);
    free(c.out);
}

/* --------------------------------------------------------- up and serving */

static void serving(void)
{
    Cap c = { .now = T0 };
    RXPresence *p = computer(&c, false, "chrisles-MBP");
    rx_presence_set_media(p, 38681, 627);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    run_until(p, &c, T0 + 3950 * MS);
    CHECK_EQ(rx_presence_state(p, NULL), RX_LINK_UP);
    CHECK(rx_presence_is_peer(p, CDJ_ADDR));
    CHECK(!rx_presence_is_peer(p, 0xc0a80199));

    /* A probe of 17 is answered to the prober with its counter; a probe of
     * another number, an assign request and our own probe are not. */
    RXPktClaim probe = { .kind = RX_ANN_CLAIM2, .name = "rekordbox", .proto = 3, .number = 17,
                         .counter = 3, .type = 4, .flag = 1, .addr = 0xc0a80114,
                         .mac = { 1, 2, 3, 4, 5, 6 } };
    WR(w);
    rx_pkt_claim(&w, &probe);
    int from = c.nb;
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80114, w.p, w.len, c.now);
    CHECK_EQ(c.nb, from + 1);
    if (c.nb == from + 1) {
        CHECK_EQ(c.out[from].to, 0xc0a80114);
        CHECK_EQ(c.out[from].port, RX_LINK_ANNOUNCE);
        CHECK_HEX("in use", c.out[from].b, c.out[from].len,
                  "5173707431576d4a4f4c030072656b6f7264626f78000000000000000000000001"
                  "030027110301");
    }
    probe.number = 18;
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_claim(&w, &probe);
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80114, w.p, w.len, c.now);
    probe.number = 17;
    probe.subtype = RX_CLAIM2_ASSIGN;
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_claim(&w, &probe);
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80114, w.p, w.len, c.now);
    Pkt own = pkt(rb_probe_17_1);
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80114, own.b, own.len, c.now);
    CHECK_EQ(c.nb, from + 1);

    /* A block naming 17 in its wired mask is answered with its counter. */
    RXPktClaim block = { .kind = RX_ANN_CLAIM2, .subtype = RX_CLAIM2_BLOCK, .name = "rekordbox",
                         .proto = 3, .addr = 0xc0a80114, .wired = 1, .counter = 255,
                         .mac = { 1, 2, 3, 4, 5, 6 } };
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_claim(&w, &block);
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80114, w.p, w.len, c.now);
    CHECK_EQ(c.nb, from + 2);
    if (c.nb == from + 2)
        CHECK_EQ(c.out[from + 1].b[0x25], 255);
    block.wired = 2;
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_claim(&w, &block);
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80114, w.p, w.len, c.now);
    CHECK_EQ(c.nb, from + 2);

    /* The media query, from the player, about us: rekordbox's answers. */
    from = c.nb;
    feed(p, &c, RX_LINK_STATUS, CDJ_ADDR, cdj_media_query_3);
    const Out *o = find_kind(&c, from, RX_LINK_STATUS, RX_ST_MEDIA_RESPONSE);
    CHECK(o && o->to == CDJ_ADDR);
    if (o)
        CHECK_HEX("media response", o->b, o->len, rb_media_3);
    rx_presence_set_media(p, 54, 2);
    from = c.nb;
    feed(p, &c, RX_LINK_STATUS, CDJ_ADDR, cdj_media_query_4);
    o = find_kind(&c, from, RX_LINK_STATUS, RX_ST_MEDIA_RESPONSE);
    if (o)
        CHECK_HEX("media response 4", o->b, o->len, rb_media_4);
    CHECK(o != NULL);
    /* Not from a stranger, not about device 18, not about slot 02. */
    from = c.nb;
    feed(p, &c, RX_LINK_STATUS, 0xc0a80199, cdj_media_query_4);
    Pkt mq = pkt(cdj_media_query_4);
    mq.b[0x2b] = 18;
    rx_presence_input(p, RX_LINK_STATUS, CDJ_ADDR, mq.b, mq.len, c.now);
    mq.b[0x2b] = 17;
    mq.b[0x2f] = 2;
    rx_presence_input(p, RX_LINK_STATUS, CDJ_ADDR, mq.b, mq.len, c.now);
    CHECK_EQ(count_kind(&c, from, RX_LINK_STATUS, RX_ST_MEDIA_RESPONSE), 0);

    /* Who we are and the display settings. */
    WR(iq);
    rx_pkt_identity_query(&iq, "CDJ-3000", 1);
    from = c.nb;
    feed_w(p, &c, RX_LINK_STATUS, CDJ_ADDR, &iq);
    o = find_kind(&c, from, RX_LINK_STATUS, RX_ST_IDENTITY);
    CHECK(o && o->to == CDJ_ADDR);
    if (o)
        CHECK_HEX("identity", o->b, o->len, rb_identity);
    from = c.nb;
    feed(p, &c, RX_LINK_STATUS, CDJ_ADDR, cdj_settings_query);
    o = find_kind(&c, from, RX_LINK_STATUS, RX_ST_SETTINGS);
    CHECK(o != NULL);
    if (o)
        CHECK_HEX("settings", o->b, o->len, rb_settings);
    /* A load acknowledgement is only read. */
    from = c.nb;
    Pkt ack = pkt(cdj_load_ack);
    rx_presence_input(p, RX_LINK_STATUS, CDJ_ADDR, ack.b, ack.len, c.now);
    CHECK_EQ(c.nb, from);

    /* The player's status: what it has loaded, from where, that it plays
     * and is master; our status then carries its tempo and its beat. */
    from = c.nb;
    feed(p, &c, RX_LINK_STATUS, CDJ_ADDR, cdj_status_playing);
    RXLinkPlayer pl[4];
    CHECK_EQ(rx_presence_players(p, pl, 4), 1);
    CHECK_EQ(pl[0].number, 1);
    CHECK_STR(pl[0].name, "CDJ-3000");
    CHECK_EQ(pl[0].type, RX_LINK_DEVICE_PLAYER);
    CHECK_EQ(pl[0].addr, CDJ_ADDR);
    CHECK(pl[0].has_status);
    CHECK_EQ(pl[0].track_device, 17);
    CHECK_EQ(pl[0].track_slot, 3);
    CHECK_EQ(pl[0].track_type, 1);
    CHECK_EQ(pl[0].track_id, 17181);
    CHECK(pl[0].playing && pl[0].master && !pl[0].synced && !pl[0].on_air);
    CHECK(fabs(pl[0].bpm - 125.39) < 1e-9);
    CHECK_EQ(pl[0].beat, 1);
    run_until(p, &c, c.now + 200 * MS);
    o = find_kind(&c, from, RX_LINK_STATUS, RX_ST_MIXER);
    CHECK(o != NULL);
    if (o) {
        CHECK_EQ(o->b[0x2c], 0x80);
        CHECK_EQ(o->b[0x2e] << 8 | o->b[0x2f], 12539);
        CHECK_EQ(o->b[0x37], 1);
    }
    /* A status from another address, or for a number not heard, is not
     * taken. */
    Pkt e = pkt(cdj_status_empty);
    rx_presence_input(p, RX_LINK_STATUS, 0xc0a80199, e.b, e.len, c.now);
    e.b[0x21] = 2;
    rx_presence_input(p, RX_LINK_STATUS, CDJ_ADDR, e.b, e.len, c.now);
    rx_presence_players(p, pl, 4);
    CHECK_EQ(pl[0].track_id, 17181);
    feed(p, &c, RX_LINK_STATUS, CDJ_ADDR, cdj_status_empty);
    rx_presence_players(p, pl, 4);
    CHECK(pl[0].track_device == 0 && !pl[0].playing);

    /* The player was greeted once, however often it spoke since. */
    CHECK_EQ(count_kind(&c, 0, RX_LINK_STATUS, RX_ST_GREETING), 1);

    /* Silent, it is forgotten 10 s after its last keep-alive. */
    int64_t last = c.now;
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    run_until(p, &c, last + 10000 * MS);
    CHECK(rx_presence_is_peer(p, CDJ_ADDR));
    run_until(p, &c, last + 10001 * MS);
    CHECK(!rx_presence_is_peer(p, CDJ_ADDR));
    CHECK_EQ(rx_presence_players(p, pl, 4), 0);
    /* Its media query now goes unanswered. */
    from = c.nb;
    feed(p, &c, RX_LINK_STATUS, CDJ_ADDR, cdj_media_query_3);
    CHECK_EQ(count_kind(&c, from, RX_LINK_STATUS, RX_ST_MEDIA_RESPONSE), 0);
    /* Back, it is greeted again. */
    from = c.nb;
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    CHECK_EQ(count_kind(&c, from, RX_LINK_STATUS, RX_ST_GREETING), 1);
    /* Leaving with a disconnect, it is gone at once. */
    RXPktConflict bye = { .kind = RX_ANN_DISCONNECT, .name = "CDJ-3000", .proto = 3, .number = 1 };
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_conflict(&w, &bye);
    feed_w(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, &w);
    CHECK(!rx_presence_is_peer(p, CDJ_ADDR));

    rx_presence_free(&p);
    free(c.out);
}

static void listing(void)
{
    Cap c = { .now = T0 };
    RXPresence *p = computer(&c, false, NULL);
    RXPktKeepAlive k;
    Pkt cdj = pkt(cdj_keep_alive);
    rx_pkt_keep_alive_parse(&k, cdj.b, cdj.len);
    /* A mixer, players 2 and 1, another rekordbox on 18. */
    feed(p, &c, RX_LINK_ANNOUNCE, 0xc0a80142, djm_keep_alive);
    k.number = 2;
    k.addr = 0xc0a80199;
    k.mac[5] = 0x99;
    WR(w);
    rx_pkt_keep_alive(&w, &k);
    feed_w(p, &c, RX_LINK_ANNOUNCE, 0xc0a80199, &w);
    feed(p, &c, RX_LINK_ANNOUNCE, CDJ_ADDR, cdj_keep_alive);
    Pkt rb = pkt(rb_keep_alive);
    rb.b[0x24] = 18;
    rb.b[0x2b] = 0x77;
    rx_presence_input(p, RX_LINK_ANNOUNCE, 0xc0a80177, rb.b, rb.len, c.now);
    RXLinkPlayer pl[8];
    CHECK_EQ(rx_presence_players(p, pl, 8), 4);
    CHECK_EQ(pl[0].number, 1);
    CHECK_EQ(pl[1].number, 2);
    CHECK_EQ(pl[2].number, 18);
    CHECK_EQ(pl[2].type, RX_LINK_DEVICE_COMPUTER);
    CHECK_EQ(pl[3].number, 33);
    CHECK_EQ(pl[3].type, RX_LINK_DEVICE_MIXER);
    CHECK_EQ(pl[3].last_seen, T0);
    CHECK(!memcmp(pl[0].mac, cdj.b + 0x26, 6));
    RXLinkPlayer two[2] = { 0 };
    CHECK_EQ(rx_presence_players(p, two, 2), 4);
    CHECK_EQ(two[1].number, 2);
    CHECK_EQ(rx_presence_players(p, NULL, 0), 4);
    /* A device starting over (its first-stage claim) is forgotten. */
    RXPktClaim again = { .kind = RX_ANN_CLAIM1, .name = "CDJ-3000", .proto = 3, .counter = 1,
                         .type = 1 };
    memcpy(again.mac, k.mac, 6);
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_claim(&w, &again);
    feed_w(p, &c, RX_LINK_ANNOUNCE, 0xc0a80199, &w);
    CHECK(!rx_presence_is_peer(p, 0xc0a80199));
    CHECK_EQ(rx_presence_players(p, NULL, 0), 3);
    /* Numbers and types out of rekordbox's range are not devices. */
    k.number = 0;
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_keep_alive(&w, &k);
    feed_w(p, &c, RX_LINK_ANNOUNCE, 0xc0a80199, &w);
    k.number = 3;
    k.type = 10;
    w = rx_wr(w_buf, sizeof(w_buf));
    rx_pkt_keep_alive(&w, &k);
    feed_w(p, &c, RX_LINK_ANNOUNCE, 0xc0a80199, &w);
    CHECK_EQ(rx_presence_players(p, NULL, 0), 3);
    rx_presence_free(&p);
    free(c.out);
}

/* ---------------------------------------------------------- back to back */

#define MAX_NODES 4

typedef struct Net Net;

typedef struct Node {
    Net *net;
    int idx;
    RXPresence *p;
    uint32_t addr;
    int64_t next;
    bool plugged;
    int sent[2][256];           /* by port and kind */
    int got[2][256];
} Node;

typedef struct Dgram {
    int from;
    enum RXLinkPort port;
    uint32_t to;
    size_t len;
    uint8_t b[RX_PKT_MAX];
} Dgram;

struct Net {
    Node node[MAX_NODES];
    int nb;
    Dgram *q;
    int nq, size;
    int64_t now;
};

static void net_send(void *opaque, enum RXLinkPort port, uint32_t to, const uint8_t *buf,
                     size_t len)
{
    Node *n = opaque;
    Net *net = n->net;
    if (!n->plugged || len > RX_PKT_MAX)
        return;
    n->sent[port][buf[0x0a]]++;
    if (net->nq == net->size) {
        net->size = net->size ? 2 * net->size : 64;
        net->q = realloc(net->q, (size_t)net->size * sizeof(*net->q));
        if (!net->q)
            abort();
    }
    Dgram *d = &net->q[net->nq++];
    *d = (Dgram) { .from = n->idx, .port = port, .to = to, .len = len };
    memcpy(d->b, buf, len);
}

static Node *net_add(Net *net, const RXPresenceConfig *cfg)
{
    Node *n = &net->node[net->nb];
    *n = (Node) { .net = net, .idx = net->nb, .addr = cfg->addr, .plugged = true };
    net->nb++;
    CHECK_EQ(rx_presence_new(&n->p, cfg, net_send, n), 0);
    if (!n->p)
        abort();
    n->next = net->now;
    return n;
}

/* Delivers what is queued, broadcasts to every node, the sender too (its
 * own come back, as on a real interface), each followed by a tick. */
static void net_pump(Net *net)
{
    for (int i = 0; i < net->nq; i++) {
        Dgram d = net->q[i];
        uint32_t from = net->node[d.from].addr;
        for (int j = 0; j < net->nb; j++) {
            Node *n = &net->node[j];
            if (!n->plugged || (d.to != RX_PRESENCE_BROADCAST && d.to != n->addr))
                continue;
            n->got[d.port][d.b[0x0a]]++;
            rx_presence_input(n->p, d.port, from, d.b, d.len, net->now);
            n->next = rx_presence_tick(n->p, net->now);
        }
    }
    net->nq = 0;
}

static void net_run(Net *net, int64_t until)
{
    net_pump(net);
    while (1) {
        int64_t t = INT64_MAX;
        for (int i = 0; i < net->nb; i++) {
            if (net->node[i].plugged)
                t = RX_MIN(t, net->node[i].next);
        }
        if (t > until)
            break;
        net->now = RX_MAX(t, net->now);
        for (int i = 0; i < net->nb; i++) {
            Node *n = &net->node[i];
            if (n->plugged && n->next <= net->now)
                n->next = rx_presence_tick(n->p, net->now);
        }
        net_pump(net);
    }
    net->now = until;
}

static void net_free(Net *net)
{
    for (int i = 0; i < net->nb; i++)
        rx_presence_free(&net->node[i].p);
    free(net->q);
}

static RXPresenceConfig node_cfg(enum RXPresenceRole role, uint32_t addr, int number)
{
    RXPresenceConfig cfg = {
        .role   = role,
        .addr   = addr,
        .number = number,
        .mac    = { 0x02, 0, 0, 0, 0, (uint8_t)addr },
    };
    snprintf(cfg.name, sizeof(cfg.name), "%s",
             role == RX_PRESENCE_COMPUTER ? "rekordbox" : "CDJ-3000");
    return cfg;
}

static int state(const Node *n, int *number)
{
    return (int)rx_presence_state(n->p, number);
}

static void back_to_back(void)
{
    Net net = { .now = T0 };
    RXPresenceConfig cc = node_cfg(RX_PRESENCE_COMPUTER, 0x0a000001, 0);
    snprintf(cc.host, sizeof(cc.host), "rxbox-test");
    Node *pc = net_add(&net, &cc);
    RXPresenceConfig pcfg = node_cfg(RX_PRESENCE_PLAYER, 0x0a000002, 2);
    Node *cdj = net_add(&net, &pcfg);
    rx_presence_set_media(pc->p, 1234, 56);

    /* The player claims its number; the computer hears the claims and says
     * nothing: only a keep-alive brings it up. */
    net_run(&net, T0 + 3500 * MS);
    int number = -1;
    CHECK_EQ(state(cdj, &number), RX_LINK_JOINING);
    CHECK_EQ(cdj->sent[RX_LINK_ANNOUNCE][RX_ANN_HELLO], 3);
    CHECK_EQ(cdj->sent[RX_LINK_ANNOUNCE][RX_ANN_CLAIM1], 3);
    CHECK_EQ(cdj->sent[RX_LINK_ANNOUNCE][RX_ANN_CLAIM2], 3);
    CHECK_EQ(cdj->sent[RX_LINK_ANNOUNCE][RX_ANN_CLAIM3], 3);
    CHECK_EQ(state(pc, NULL), RX_LINK_WAITING);
    int any = 0;
    for (int k = 0; k < 256; k++)
        any += pc->sent[0][k] + pc->sent[1][k];
    CHECK_EQ(any, 0);
    CHECK(!rx_presence_is_peer(pc->p, cdj->addr));

    /* Up at 3.6 s; its keep-alive starts the computer's join, which takes
     * 17 3.9 s later. */
    net_run(&net, T0 + 3600 * MS);
    CHECK_EQ(state(cdj, &number), RX_LINK_UP);
    CHECK_EQ(number, 2);
    CHECK_EQ(state(pc, NULL), RX_LINK_JOINING);
    CHECK(rx_presence_is_peer(pc->p, cdj->addr));
    net_run(&net, T0 + 7499 * MS);
    CHECK_EQ(state(pc, NULL), RX_LINK_JOINING);
    net_run(&net, T0 + 7500 * MS);
    CHECK_EQ(state(pc, &number), RX_LINK_UP);
    CHECK_EQ(number, 17);

    /* The player hears the computer, asks who it is and what it holds, is
     * greeted once. */
    net_run(&net, T0 + 9000 * MS);
    int src = 0, tracks = 0, playlists = 0;
    uint32_t src_addr = 0;
    CHECK(rx_presence_source(cdj->p, &src, &src_addr, &tracks, &playlists));
    CHECK_EQ(src, 17);
    CHECK_EQ(src_addr, pc->addr);
    CHECK_EQ(tracks, 1234);
    CHECK_EQ(playlists, 56);
    CHECK_EQ(cdj->got[RX_LINK_STATUS][RX_ST_GREETING], 1);
    CHECK_EQ(cdj->got[RX_LINK_STATUS][RX_ST_IDENTITY], 1);
    CHECK_EQ(cdj->got[RX_LINK_STATUS][RX_ST_SETTINGS], 1);
    CHECK_EQ(cdj->got[RX_LINK_STATUS][RX_ST_MEDIA_RESPONSE], 1);
    RXLinkPlayer pl[4];
    CHECK_EQ(rx_presence_players(cdj->p, pl, 4), 1);
    CHECK_EQ(pl[0].number, 17);
    CHECK_EQ(pl[0].type, RX_LINK_DEVICE_COMPUTER);
    CHECK(pl[0].has_status);

    /* The computer lists the player, nothing loaded. */
    CHECK_EQ(rx_presence_players(pc->p, pl, 4), 1);
    CHECK_EQ(pl[0].number, 2);
    CHECK_EQ(pl[0].type, RX_LINK_DEVICE_PLAYER);
    CHECK_STR(pl[0].name, "CDJ-3000");
    CHECK_EQ(pl[0].addr, cdj->addr);
    CHECK(pl[0].has_status);
    CHECK_EQ(pl[0].track_device, 0);
    CHECK(rx_presence_is_peer(pc->p, cdj->addr));
    CHECK(!rx_presence_is_peer(pc->p, 0x0a000009));

    /* It loads one of ours and plays it; then pauses. */
    rx_presence_set_loaded(cdj->p, 17, RX_SLOT_REKORDBOX, 4242, true);
    net_run(&net, T0 + 9300 * MS);
    rx_presence_players(pc->p, pl, 4);
    CHECK_EQ(pl[0].track_device, 17);
    CHECK_EQ(pl[0].track_slot, RX_SLOT_REKORDBOX);
    CHECK_EQ(pl[0].track_type, 1);
    CHECK_EQ(pl[0].track_id, 4242);
    CHECK(pl[0].playing);
    rx_presence_set_loaded(cdj->p, 17, RX_SLOT_REKORDBOX, 4242, false);
    net_run(&net, T0 + 9600 * MS);
    rx_presence_players(pc->p, pl, 4);
    CHECK(!pl[0].playing);
    CHECK_EQ(pl[0].track_id, 4242);

    /* A second player claiming number 2 is told it is held, and never gets
     * it. */
    RXPresenceConfig qcfg = node_cfg(RX_PRESENCE_PLAYER, 0x0a000004, 2);
    Node *rival = net_add(&net, &qcfg);
    net_run(&net, T0 + 20000 * MS);
    CHECK(cdj->sent[RX_LINK_ANNOUNCE][RX_ANN_CONFLICT] > 0);
    CHECK_EQ(state(rival, NULL), RX_LINK_JOINING);
    CHECK_EQ(state(cdj, &number), RX_LINK_UP);
    CHECK_EQ(number, 2);
    rival->plugged = false;

    /* Unplugged, the player is forgotten 10 s after its last keep-alive, and
     * the computer stays up. */
    cdj->plugged = false;
    int64_t gone = net.now;
    net_run(&net, gone + 8500 * MS);
    CHECK(rx_presence_is_peer(pc->p, cdj->addr));
    net_run(&net, gone + 11000 * MS);
    CHECK(!rx_presence_is_peer(pc->p, cdj->addr));
    CHECK_EQ(rx_presence_players(pc->p, pl, 4), 0);
    CHECK_EQ(state(pc, &number), RX_LINK_UP);
    CHECK_EQ(number, 17);
    net_free(&net);
}

/* Two computers: one already on 17 answers the other's probes of it, and
 * the newcomer takes 18. */
static void conflict_on_17(void)
{
    Net net = { .now = T0 };
    RXPresenceConfig bc = node_cfg(RX_PRESENCE_COMPUTER, 0x0a000003, 0);
    bc.start_up = true;
    Node *b = net_add(&net, &bc);
    net_run(&net, T0 + 4000 * MS);
    int number = 0;
    CHECK_EQ(state(b, &number), RX_LINK_UP);
    CHECK_EQ(number, 17);

    RXPresenceConfig ac = node_cfg(RX_PRESENCE_COMPUTER, 0x0a000001, 0);
    Node *a = net_add(&net, &ac);
    RXPresenceConfig pcfg = node_cfg(RX_PRESENCE_PLAYER, 0x0a000002, 1);
    Node *cdj = net_add(&net, &pcfg);
    net_run(&net, T0 + 12000 * MS);
    CHECK_EQ(state(cdj, NULL), RX_LINK_UP);
    CHECK_EQ(state(a, &number), RX_LINK_UP);
    CHECK_EQ(number, 18);
    CHECK_EQ(state(b, &number), RX_LINK_UP);
    CHECK_EQ(number, 17);
    CHECK(b->sent[RX_LINK_ANNOUNCE][RX_ANN_NUMBER_REPLY] > 0);
    /* Each lists the other and the player; the player sees both. */
    RXLinkPlayer pl[4];
    CHECK_EQ(rx_presence_players(a->p, pl, 4), 2);
    CHECK_EQ(pl[0].number, 1);
    CHECK_EQ(pl[1].number, 17);
    CHECK_EQ(rx_presence_players(cdj->p, pl, 4), 2);
    /* The player talks to the first computer it heard. */
    int src = 0;
    CHECK(rx_presence_source(cdj->p, &src, NULL, NULL, NULL));
    CHECK_EQ(src, 17);
    net_free(&net);
}

int main(void)
{
    rx_log_set_level(RX_LOG_WARN);
    keep_alives();
    claims();
    status_packets();
    load_command();
    questions();
    malformed();
    join_timeline();
    join_variants();
    serving();
    listing();
    back_to_back();
    conflict_on_17();
    return test_result("link packets");
}
