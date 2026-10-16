/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A deterministic fuzzer for presence, run under AddressSanitizer and
 * UndefinedBehaviorSanitizer (src/link/meson.build): random datagrams and
 * mutations of valid ones (captured bytes and every builder's output, with
 * bits flipped, bytes set, cut short, extended, length fields lying) into
 * every parser and into cb_presence_input of computers and fake players on
 * both ports, with the clock run forward and ticks in between. What the
 * presences send goes to the others too, so the joins, the answers and the
 * fake player's questions are reached and fuzzed in every state.
 *
 * CB_FUZZ_SEED sets the seed, CB_FUZZ_ITERS the number of datagrams. A
 * finding aborts (the sanitizers, or a check here); 0 when clean. */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cyanbox/common.h"

#include "src/link/packet.h"
#include "src/link/presence.h"

#define DEFAULT_SEED  UINT64_C(0x5174707431576d4a)
#define DEFAULT_ITERS 3000000
#define MAX_DGRAM     1500       /* a LINK CUE audio packet's 0x56c fits */
#define NB_NODES      4
#define QUEUE         256

typedef struct Rng {
    uint64_t s;
} Rng;

/* splitmix64 */
static uint64_t rnd(Rng *r)
{
    uint64_t z = (r->s += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static uint32_t below(Rng *r, uint32_t n)
{
    return n ? (uint32_t)(rnd(r) % n) : 0;
}

static void fail(const char *what)
{
    fprintf(stderr, "fuzz_presence: %s\n", what);
    abort();
}

/* ------------------------------------------------------------ the corpus */

/* rbxport's captures (tests/link/test_packets.c has them all and says
 * where each is from). */
static const struct {
    enum CBLinkPort port;
    const char *hex;
} captured[] = {
    { CB_LINK_ANNOUNCE,
      "5173707431576d4a4f4c060072656b6f7264626f780000000000000000000000"
      "01030036110100e04ccf632ec0a8010e020100000408" },
    /* A DJM-900NXS's keep-alive and first claim: little-endian lengths. */
    { CB_LINK_ANNOUNCE,
      "5173707431576d4a4f4c0600444a4d2d3930306e657875730000000000000000"
      "01023600210200e0363a0021a9fe0021010000000301" },
    { CB_LINK_ANNOUNCE,
      "5173707431576d4a4f4c0000444a4d2d3930306e657875730000000000000000"
      "01022c00010300e0363a0021" },
    { CB_LINK_ANNOUNCE,
      "5173707431576d4a4f4c060043444a2d33303030000000000000000000000000"
      "0103003601012497ed0b4043c0a80198030000000164" },
    { CB_LINK_ANNOUNCE,
      "5173707431576d4a4f4c0600444a4d2d56350000000000000000000000000000"
      "010200362102c83dfc1dfea2c0a80142040000000331" },
    { CB_LINK_ANNOUNCE,
      "5173707431576d4a4f4c020072656b6f7264626f780000000000000000000000"
      "01030032c0a8010e00e04ccf632e11010401" },
    { CB_LINK_STATUS,
      "5173707431576d4a4f4c0543444a2d3330303000000000000000000000000001"
      "0001000cc0a801980000001100000003" },
    { CB_LINK_STATUS,
      "5173707431576d4a4f4c4643444a2d3330303000000000000000000000000001"
      "00010004010400e4" },
    { CB_LINK_STATUS,
      "5173707431576d4a4f4c3058444a2d5258330000000000000000000000000001"
      "030b0000" },
    { CB_LINK_STATUS,
      "5173707431576d4a4f4c1a43444a2d3330303000000000000000000000000001"
      "0003000403010000" },
    { CB_LINK_STATUS,
      "5173707431576d4a4f4c0a43444a2d3330303000000000000000000000000001"
      "0501010001000000110301000000431d0000000c000000026d5c28f2ffffffff"
      "000000000000000c000000000000000000000000000000000000000000000000"
      "00000000000000000100040400000004000000040000000000000003332e3230"
      "000000000000000100e4fffa00100000800030fb800030fb00100000000901ff"
      "00000001000001000000000000000000000000000000012c0000000200000000"
      "0010000000100000000000001ff3000012345678000000010101010001010000"
      "0000000000000000000000000000000012345678000000010101010001010000"
      "00000000000000000000000000000000000000050000077a0000077a0b4b0000"
      "00000000" },
};

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
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

/* Device numbers and addresses from small sets, so that packets name the
 * devices and the numbers the presences hold or probe. */
static int some_number(Rng *r)
{
    static const int some[] = { 0, 1, 2, 3, 4, 5, 6, 9, 11, 17, 18, 33, 41, 44, 80, 81, 255 };
    return below(r, 4) ? some[below(r, sizeof(some) / sizeof(some[0]))] : (int)below(r, 256);
}

static uint32_t address(Rng *r)
{
    return below(r, 8) ? 0x0a000001 + below(r, 6) : (uint32_t)rnd(r);
}

static void name(Rng *r, char out[21])
{
    static const char *names[] = { "rekordbox", "CDJ-3000", "CDJ-3000X", "DJM-V5", "", "x" };
    if (below(r, 6)) {
        snprintf(out, 21, "%s", names[below(r, 6)]);
        return;
    }
    for (int i = 0; i < 20; i++)
        out[i] = (char)below(r, 256);
    out[20] = 0;
}

/* A valid packet from one of the builders, with random fields. */
static size_t build(Rng *r, uint8_t *buf, size_t cap, enum CBLinkPort *port)
{
    CBWr w = cb_wr(buf, cap);
    char nm[21];
    name(r, nm);
    int n = some_number(r);
    *port = CB_LINK_ANNOUNCE;
    switch (below(r, 19)) {
    case 0: {
        CBPktKeepAlive k = {
            .proto = (int)below(r, 5), .number = n, .first = (int)below(r, 3),
            .addr = address(r), .peers = (int)below(r, 256), .flag = (int)below(r, 2),
            .type = (int)below(r, 11), .tail = (int)below(r, 256),
        };
        memcpy(k.name, nm, sizeof(k.name));
        for (int i = 0; i < 6; i++)
            k.mac[i] = below(r, 2) ? (uint8_t)i : (uint8_t)rnd(r);
        cb_pkt_keep_alive(&w, &k);
        break;
    }
    case 1: case 2: case 3: case 4: {
        static const int kinds[] = { CB_ANN_CLAIM1, CB_ANN_CLAIM2, CB_ANN_CLAIM3, CB_ANN_HELLO };
        CBPktClaim c = {
            .kind = kinds[below(r, 4)], .subtype = (int)below(r, 3), .proto = (int)below(r, 5),
            .number = n, .counter = (int)below(r, 8), .type = (int)below(r, 9),
            .flag = (int)below(r, 3), .addr = address(r), .wired = (int)below(r, 4),
            .wireless = (int)below(r, 16), .hello = (int)below(r, 2) * 0x40,
        };
        memcpy(c.name, nm, sizeof(c.name));
        for (int i = 0; i < 6; i++)
            c.mac[i] = (uint8_t)below(r, 3);
        cb_pkt_claim(&w, &c);
        break;
    }
    case 5: {
        CBPktNumberReply nr = { .subtype = (int)below(r, 3), .number = n,
                                .counter = (int)below(r, 8), .status = (int)below(r, 3) };
        memcpy(nr.name, nm, sizeof(nr.name));
        cb_pkt_number_reply(&w, &nr);
        break;
    }
    case 6: {
        CBPktConflict c = { .kind = below(r, 2) ? CB_ANN_CONFLICT : CB_ANN_DISCONNECT,
                            .proto = 2, .number = n, .addr = address(r) };
        memcpy(c.name, nm, sizeof(c.name));
        cb_pkt_conflict(&w, &c);
        break;
    }
    case 7: {
        CBPktStatus s = {
            .number = n, .track_device = some_number(r), .track_slot = (int)below(r, 10),
            .track_type = (int)below(r, 7), .track_id = (uint32_t)rnd(r),
            .playing = below(r, 2), .master = below(r, 2), .synced = below(r, 2),
            .on_air = below(r, 2), .play_state = (int)below(r, 20),
            .bpm_x100 = (int)below(r, 70000), .pitch = (double)below(r, 400) - 200.0,
            .beat = (int)below(r, 1000) - 1, .bar_beat = (int)below(r, 5),
        };
        memcpy(s.name, nm, sizeof(s.name));
        cb_pkt_cdj_status(&w, &s);
        *port = CB_LINK_STATUS;
        break;
    }
    case 8:
        cb_pkt_mixer_status(&w, nm, n, (int)below(r, 70000), (int)below(r, 5), below(r, 2));
        *port = CB_LINK_STATUS;
        break;
    case 9: {
        CBPktMediaQuery q = { .number = n, .addr = address(r), .device = some_number(r),
                              .slot = (int)below(r, 6) };
        memcpy(q.name, nm, sizeof(q.name));
        cb_pkt_media_query(&w, &q);
        *port = CB_LINK_STATUS;
        break;
    }
    case 10: {
        CBPktMedia m = { .number = n, .device = some_number(r), .slot = (int)below(r, 6),
                         .tracks = (int)below(r, 70000), .playlists = (int)below(r, 70000) };
        memcpy(m.name, nm, sizeof(m.name));
        cb_pkt_media(&w, &m);
        *port = CB_LINK_STATUS;
        break;
    }
    case 11:
        cb_pkt_greeting(&w, nm, n);
        *port = CB_LINK_STATUS;
        break;
    case 12:
        cb_pkt_identity_query(&w, nm, n);
        *port = CB_LINK_STATUS;
        break;
    case 13:
        cb_pkt_identity(&w, nm, n, below(r, 2) ? nm : "a host \xe2\x9c\x93");
        *port = CB_LINK_STATUS;
        break;
    case 14:
        cb_pkt_settings_query(&w, nm, n);
        *port = CB_LINK_STATUS;
        break;
    case 16: {
        CBPktCueTiming t = { .mixer = n, .counter = (uint32_t)rnd(r), .on = below(r, 2),
                             .source = some_number(r) };
        memcpy(t.name, nm, sizeof(t.name));
        cb_pkt_cue_timing(&w, &t);
        *port = CB_LINK_CUE;
        break;
    }
    case 17:
        cb_pkt_cue_handover(&w, nm, n);
        *port = CB_LINK_CUE;
        break;
    case 18: {
        static CBPktCueAudio a;
        a = (CBPktCueAudio) { .number = n, .mode = (int)below(r, 4), .counter = (uint32_t)rnd(r),
                              .audio = below(r, 2) };
        memcpy(a.name, nm, sizeof(a.name));
        for (int i = 0; i < CB_CUE_FRAMES * 2; i++)
            a.pcm[i] = (int16_t)rnd(r);
        cb_pkt_cue_audio(&w, &a);
        *port = CB_LINK_CUE;
        break;
    }
    default: {
        uint8_t ds[32];
        for (int i = 0; i < 32; i++)
            ds[i] = (uint8_t)rnd(r);
        cb_pkt_settings(&w, nm, n, ds);
        *port = CB_LINK_STATUS;
        break;
    }
    }
    if (w.err)
        fail("a builder ran out of room");
    return w.len;
}

static size_t mutate(Rng *r, uint8_t *buf, size_t len, size_t cap)
{
    int rounds = (int)below(r, 4);
    for (int k = 0; k < rounds; k++) {
        switch (below(r, 7)) {
        case 0:     /* bit flips */
            for (int i = (int)below(r, 8) + 1; i > 0 && len; i--)
                buf[below(r, (uint32_t)len)] ^= (uint8_t)(1 << below(r, 8));
            break;
        case 1:     /* a byte where the header's fields are */
            if (len > 0x24)
                buf[0x0a + below(r, 0x1a)] = (uint8_t)rnd(r);
            break;
        case 2:     /* cut short */
            len = below(r, (uint32_t)len + 1);
            break;
        case 3: {   /* extended */
            size_t more = below(r, 64);
            for (size_t i = 0; i < more && len < cap; i++)
                buf[len++] = (uint8_t)rnd(r);
            break;
        }
        case 4:     /* the length field lies */
            if (len >= 0x24) {
                uint32_t v = below(r, 3) == 0 ? (uint32_t)rnd(r) :
                             (uint32_t)((int)len + (int)below(r, 9) - 4 - (below(r, 2) ? 0x24 : 0));
                buf[0x22] = (uint8_t)(v >> 8);
                buf[0x23] = (uint8_t)v;
            }
            break;
        case 5:     /* another kind */
            if (len > 0x0a)
                buf[0x0a] = (uint8_t)below(r, 0x50);
            break;
        default:    /* a random byte anywhere */
            if (len)
                buf[below(r, (uint32_t)len)] = (uint8_t)rnd(r);
            break;
        }
    }
    return len;
}

static size_t datagram(Rng *r, uint8_t *buf, size_t cap, enum CBLinkPort *port)
{
    size_t len;
    switch (below(r, 8)) {
    case 0:     /* rubbish, with the magic half the time */
        len = below(r, (uint32_t)cap);
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)rnd(r);
        if (below(r, 2) && len >= 10)
            memcpy(buf, cb_link_magic, 10);
        *port = below(r, 2) ? CB_LINK_ANNOUNCE : CB_LINK_STATUS;
        return len;
    case 1: {
        int i = (int)below(r, sizeof(captured) / sizeof(captured[0]));
        len = unhex(buf, cap, captured[i].hex);
        *port = captured[i].port;
        break;
    }
    default:
        len = build(r, buf, cap, port);
        break;
    }
    /* Now and then on the other port. */
    if (!below(r, 16))
        *port = *port == CB_LINK_ANNOUNCE ? CB_LINK_STATUS : CB_LINK_ANNOUNCE;
    return below(r, 3) ? mutate(r, buf, len, cap) : len;
}

/* --------------------------------------------------------- every parser */

static void check_name(const char *n)
{
    size_t l = strnlen(n, 21);
    if (l > 20)
        fail("a name without its NUL");
    for (size_t i = 0; i < l; i++) {
        if (n[i] < 0x20 || n[i] > 0x7e)
            fail("an unprintable name");
    }
}

static void parse_all(const uint8_t *buf, size_t len)
{
    for (int port = CB_LINK_ANNOUNCE; port <= CB_LINK_CUE; port++) {
        CBPktHeader h;
        if (cb_pkt_header(&h, (enum CBLinkPort)port, buf, len) == 0) {
            if (h.len < 0x24 || h.len > len)
                fail("a header longer than its datagram");
            check_name(h.name);
        }
    }
    CBPktKeepAlive k;
    if (cb_pkt_keep_alive_parse(&k, buf, len) == 0) {
        check_name(k.name);
        /* Built again, it reads the same. */
        uint8_t again[CB_PKT_MAX];
        CBWr w = cb_wr(again, sizeof(again));
        cb_pkt_keep_alive(&w, &k);
        CBPktKeepAlive k2;
        if (w.err || cb_pkt_keep_alive_parse(&k2, w.p, w.len) < 0 || k2.number != k.number ||
            k2.addr != k.addr || memcmp(k2.mac, k.mac, 6) || strcmp(k2.name, k.name))
            fail("a keep-alive does not survive a round trip");
    }
    CBPktClaim c;
    if (cb_pkt_claim_parse(&c, buf, len) == 0) {
        uint8_t again[CB_PKT_MAX];
        CBWr w = cb_wr(again, sizeof(again));
        cb_pkt_claim(&w, &c);
        CBPktClaim c2;
        if (w.err || cb_pkt_claim_parse(&c2, w.p, w.len) < 0 || c2.kind != c.kind ||
            c2.number != c.number || c2.counter != c.counter)
            fail("a claim does not survive a round trip");
    }
    CBPktNumberReply n;
    cb_pkt_number_reply_parse(&n, buf, len);
    CBPktConflict cf;
    cb_pkt_conflict_parse(&cf, buf, len);
    CBPktStatus s;
    if (cb_pkt_status_parse(&s, buf, len) == 0) {
        if (!isfinite(s.pitch) || s.beat < -1 || s.bpm_x100 < 0 || s.bpm_x100 > 0xfffe)
            fail("a status out of range");
        uint8_t again[CB_PKT_MAX];
        CBWr w = cb_wr(again, sizeof(again));
        cb_pkt_cdj_status(&w, &s);
        if (w.err)
            fail("a status does not build");
    }
    CBPktMediaQuery q;
    cb_pkt_media_query_parse(&q, buf, len);
    CBPktMedia m;
    cb_pkt_media_parse(&m, buf, len);

    CBPktCueTiming t;
    if (cb_pkt_cue_timing_parse(&t, buf, len) == 0) {
        check_name(t.name);
        uint8_t again[CB_PKT_MAX];
        CBWr w = cb_wr(again, sizeof(again));
        cb_pkt_cue_timing(&w, &t);
        CBPktCueTiming t2;
        if (w.err || cb_pkt_cue_timing_parse(&t2, w.p, w.len) < 0 || t2.mixer != t.mixer ||
            t2.counter != t.counter || t2.on != t.on || t2.source != t.source)
            fail("a LINK CUE timing does not survive a round trip");
    }
    int mixer;
    cb_pkt_cue_handover_parse(&mixer, buf, len);
    static CBPktCueAudio a, a2;
    if (cb_pkt_cue_audio_parse(&a, buf, len) == 0) {
        check_name(a.name);
        static uint8_t again[CB_CUE_AUDIO_SIZE];
        CBWr w = cb_wr(again, sizeof(again));
        cb_pkt_cue_audio(&w, &a);
        if (w.err || w.len != CB_CUE_AUDIO_SIZE || cb_pkt_cue_audio_parse(&a2, w.p, w.len) < 0 ||
            a2.counter != a.counter || a2.mode != a.mode || a2.audio != a.audio ||
            (a.audio && memcmp(a2.pcm, a.pcm, sizeof(a.pcm))))
            fail("a LINK CUE audio packet does not survive a round trip");
    }
}

/* ------------------------------------------------------------ the nodes */

typedef struct Dgram {
    int from;
    enum CBLinkPort port;
    uint32_t to;
    size_t len;
    uint8_t b[CB_PKT_MAX];
} Dgram;

typedef struct Net Net;

typedef struct Node {
    Net *net;
    int idx;
    CBPresence *p;
    uint32_t addr;
} Node;

struct Net {
    Node node[NB_NODES];
    Dgram q[QUEUE];
    int nq;
    uint64_t sent;
};

static void on_send(void *opaque, enum CBLinkPort port, uint32_t to, const uint8_t *buf,
                    size_t len)
{
    Node *n = opaque;
    Net *net = n->net;
    if (port != CB_LINK_ANNOUNCE && port != CB_LINK_STATUS)
        fail("a datagram for no port");
    if (len < 0x24 || len > CB_PKT_MAX || memcmp(buf, cb_link_magic, 10))
        fail("a datagram that is no DJ Link packet");
    /* What goes out reads back as what it is. */
    CBPktHeader h;
    if (cb_pkt_header(&h, port, buf, len) < 0)
        fail("a datagram whose header does not read");
    net->sent++;
    if (net->nq == QUEUE)
        return;
    Dgram *d = &net->q[net->nq++];
    *d = (Dgram) { .from = n->idx, .port = port, .to = to, .len = len };
    memcpy(d->b, buf, len);
}

static void make_nodes(Net *net, Rng *r)
{
    for (int i = 0; i < NB_NODES; i++) {
        Node *n = &net->node[i];
        *n = (Node) { .net = net, .idx = i, .addr = 0x0a000001 + (uint32_t)i };
        CBPresenceConfig c = {
            .role     = i < 2 ? CB_PRESENCE_COMPUTER : CB_PRESENCE_PLAYER,
            .addr     = n->addr,
            .number   = i - 1,
            .start_up = i == 0,
            .mac      = { 2, 0, 0, 0, 0, (uint8_t)i },
        };
        if (i == 1)
            snprintf(c.host, sizeof(c.host), "fuzz-host");
        if (cb_presence_new(&n->p, &c, on_send, n) < 0)
            fail("cb_presence_new");
    }
    (void)r;
}

static void free_nodes(Net *net)
{
    for (int i = 0; i < NB_NODES; i++)
        cb_presence_free(&net->node[i].p);
}

/* What the presences sent, to the others (and back to the sender), now
 * and then mutated on the way. */
static void deliver(Net *net, Rng *r, int64_t now)
{
    for (int i = 0; i < net->nq; i++) {
        Dgram d = net->q[i];
        if (!below(r, 8))
            d.len = mutate(r, d.b, d.len, sizeof(d.b));
        for (int j = 0; j < NB_NODES; j++) {
            Node *n = &net->node[j];
            if (d.to == CB_PRESENCE_BROADCAST || d.to == n->addr)
                cb_presence_input(n->p, d.port, net->node[d.from].addr, d.b, d.len, now);
        }
    }
    net->nq = 0;
}

static uint64_t env_u64(const char *name, uint64_t dflt)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return dflt;
    char *end;
    uint64_t x = strtoull(v, &end, 0);
    return *end ? dflt : x;
}

int main(void)
{
    cb_log_set_level(CB_LOG_ERROR);
    uint64_t seed = env_u64("CB_FUZZ_SEED", DEFAULT_SEED);
    uint64_t iters = env_u64("CB_FUZZ_ITERS", DEFAULT_ITERS);
    fprintf(stderr, "fuzz_presence: seed 0x%016" PRIx64 ", %" PRIu64 " datagrams\n", seed, iters);

    Rng r = { .s = seed };
    Net *net = calloc(1, sizeof(*net));
    if (!net)
        fail("out of memory");
    make_nodes(net, &r);
    int64_t now = INT64_C(1000000000);
    uint64_t restarts = 0;

    uint8_t buf[MAX_DGRAM];
    for (size_t i = 0; i < CB_ARRAY_ELEMS(captured); i++) {
        size_t n = unhex(buf, sizeof(buf), captured[i].hex);
        CBPktHeader h;
        if (cb_pkt_header(&h, captured[i].port, buf, n) < 0 || h.len != n)
            fail("a captured packet does not read");
    }
    for (uint64_t it = 0; it < iters; it++) {
        enum CBLinkPort port;
        size_t len = datagram(&r, buf, sizeof(buf), &port);
        parse_all(buf, len);

        /* To one presence or to all, from a device's address or our own. */
        uint32_t from = address(&r);
        int to = (int)below(&r, NB_NODES + 1);
        for (int j = 0; j < NB_NODES; j++) {
            if (to == NB_NODES || to == j)
                cb_presence_input(net->node[j].p, port, from, buf, len, now);
        }

        /* The clock: mostly a little, sometimes past every timeout. */
        if (!below(&r, 4)) {
            uint32_t step = below(&r, 16) ? below(&r, 400000) : below(&r, 15000000);
            now += step;
            for (int j = 0; j < NB_NODES; j++) {
                int64_t next = cb_presence_tick(net->node[j].p, now);
                if (next <= now || next > now + 1100000)
                    fail("a deadline in the past or too far ahead");
            }
        }
        deliver(net, &r, now);

        /* The rest of the interface, now and then. */
        if (!below(&r, 64)) {
            Node *n = &net->node[below(&r, NB_NODES)];
            CBLinkPlayer pl[40];
            int max = (int)below(&r, 41);
            int nb = cb_presence_players(n->p, pl, max);
            if (nb < 0 || nb > 32)
                fail("a device count out of range");
            for (int i = 0; i < nb && i < max; i++) {
                if (strnlen(pl[i].name, sizeof(pl[i].name)) >= sizeof(pl[i].name))
                    fail("a device name without its NUL");
                if (!cb_presence_is_peer(n->p, pl[i].addr))
                    fail("a device listed that is no peer");
            }
            int number;
            enum CBLinkState st = cb_presence_state(n->p, &number);
            if (st == CB_LINK_UP ? number < 1 || number > 80 : number != 0)
                fail("a number that does not go with the state");
            int src, tracks, playlists;
            uint32_t addr;
            if (cb_presence_source(n->p, &src, &addr, &tracks, &playlists) &&
                (!cb_presence_is_peer(n->p, addr) || tracks < 0 || tracks > 0xffff))
                fail("a source that is no peer");
            uint8_t ds[32];
            for (int i = 0; i < 32; i++)
                ds[i] = (uint8_t)rnd(&r);
            cb_presence_set_devsetting(n->p, ds);
            cb_presence_set_loaded(n->p, some_number(&r), (int)below(&r, 6), (uint32_t)rnd(&r),
                                   below(&r, 2));
            cb_presence_set_media(n->p, (int)below(&r, 100000) - 10, (int)below(&r, 100000));
        }
        /* And start over now and then, so the joins are run again. */
        if (!below(&r, 50000)) {
            free_nodes(net);
            make_nodes(net, &r);
            restarts++;
        }
    }
    free_nodes(net);
    fprintf(stderr, "fuzz_presence: clean; %" PRIu64 " datagrams sent by the presences, %" PRIu64
            " restarts\n", net->sent, restarts);
    free(net);
    return 0;
}
