/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The database server, tested against other people's bytes where there
 * are some, and against a small real library otherwise:
 *
 *  - the codec: rbxport's captures of rekordbox 7.2.11 serving a CDJ-3000
 *    (rbl-dbserver tests/messages.rs, tests/session.rs, and messages cut
 *    from tests/corpus) parsed and written back byte for byte, and the
 *    items rekordbox sent written from their values byte for byte;
 *    truncation, garbage and lying lengths refused;
 *  - the CDJ-3000's whole request stream from that capture (97 requests)
 *    answered by a session over an in-memory catalog that holds two of its
 *    tracks, each request on its own transaction in the shape rekordbox
 *    answered it; the extended cue list and the grid of a track made like
 *    the captured one compared with rekordbox's bytes;
 *  - a small library made under RX_TEST_DIR (four tracks from the ffmpeg
 *    CLI, a cover, playlists, cues, grids, waveforms, plays) browsed and
 *    loaded with the client over a socketpair and over loopback TCP as a
 *    CDJ-3000 does, and with a client set up the older way;
 *  - byte streams split at every boundary; malformed and oversized
 *    messages closing the session; a library change between a menu and
 *    its render; a 10 000-track catalog made and browsed in time.
 *
 * Needs the ffmpeg CLI with libmp3lame and libopus; the library part skips
 * (77) without, after the rest has run. */

#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rxbox/analysis.h"
#include "rxbox/audio.h"
#include "rxbox/colors.h"
#include "rxbox/export.h"

#include "src/link/catalog.h"
#include "src/link/dbmsg.h"
#include "src/link/dbserver.h"
#include "tests/formats/test.h"

/* ------------------------------------------------------------- helpers */

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
    }
    return n;
}

static bool same(const uint8_t *a, size_t na, const uint8_t *b, size_t nb, const char *what)
{
    if (na == nb && !memcmp(a, b, na))
        return true;
    size_t i = 0;
    while (i < na && i < nb && a[i] == b[i])
        i++;
    fprintf(stderr, "%s: %zu and %zu bytes differ at %zu (%02x, %02x)\n", what, na, nb, i,
            i < na ? a[i] : 0, i < nb ? b[i] : 0);
    return false;
}

#define CHECK_BYTES(a, na, b, nb, what) CHECK(same(a, na, b, nb, what))

static int sh(const char *fmt, ...) RX_PRINTF(1, 2);
static int sh(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *cmd = rx_vasprintf(fmt, ap);
    va_end(ap);
    int r = cmd ? system(cmd) : -1;
    free(cmd);
    return r;
}

/* A whole message from `bytes`, written back. */
static void round_trip(const char *hex, const char *what)
{
    uint8_t b[2048];
    size_t n = unhex(hex, b, sizeof(b));
    RXDbMsg m;
    CHECK_EQ(rx_dbmsg_parse(b, n, 1 << 20, &m), (int)n);
    RXWr w = rx_wr_dyn();
    rx_dbmsg_write(&w, &m);
    CHECK_BYTES(w.p, w.len, b, n, what);
    rx_wr_free(&w);
    /* Every prefix is a message still arriving. */
    for (size_t k = 0; k < n; k++) {
        int r = rx_dbmsg_parse(b, k, 1 << 20, &m);
        if (r != 0) {
            fprintf(stderr, "%s: a prefix of %zu bytes gave %d\n", what, k, r);
            failures++;
            break;
        }
    }
}

/* The messages in a session's output, one by one. */
typedef struct Out {
    const uint8_t *p;
    size_t len, pos;
} Out;

static bool next_msg(Out *o, RXDbMsg *m)
{
    if (o->pos >= o->len)
        return false;
    int r = rx_dbmsg_parse(o->p + o->pos, o->len - o->pos, 64 << 20, m);
    if (r <= 0) {
        fprintf(stderr, "output at %zu does not parse (%d)\n", o->pos, r);
        failures++;
        o->pos = o->len;
        return false;
    }
    o->pos += (size_t)r;
    return true;
}

static void put_msg(RXWr *w, uint32_t txid, uint16_t kind, const uint32_t *v, int nb)
{
    rx_dbmsg_numbers(w, txid, kind, v, nb);
}

/* ------------------------------------------------------- codec vectors */

/* rbxport's captures of rekordbox 7.2.11 and a CDJ-3000 (rbl-dbserver
 * tests/messages.rs and tests/session.rs). */
static const char setup_req[] =
    "11872349ae11fffffffe1000000f021400000002060611000000011100000014";
static const char setup_rep[] =
    "11872349ae11fffffffe1000000f021400000002060611000000111100000014";
static const char root_req[] =
    "11872349ae110000017f1010000f031400000003060606110101030111000000001105cfffff";
static const char no_art[] =
    "11872349ae11000001871040020f04140000000406060603110000200311000000321100000000";
static const char artist_row[] =
    "11872349ae11000001801041010f1014000000100606060206020606060606060606020611000000001100000002"
    "11000000122600000009fffa004100520054004900530054fffb0000110000000226000000010000110000008111"
    "00000000110000000011000000001100000000110000000011000000001100000002260000000100001100000000";
static const char user_info_hdr[] =
    "11872349ae11000000a2104d020f041400000004060606031100003006110000000011000000a014000000a0";
/* The error a session answers a request nobody knows with, and the older
 * setup and its answer (rbxport's tests/session.rs, which hold rekordbox's
 * envelope from its binary). */
static const char error_2fff[] =
    "11872349ae11000081231040030f011400000001061100002fff";
static const char legacy_setup[] =
    "11872349ae11fffffffe1000000f011400000001061100000005";
static const char legacy_setup_rep[] =
    "11872349ae11fffffffe1040000f021400000002060611000000001100000011";

/* Cut from rbxport's tests/corpus/link-export.rekordbox.bin: transaction
 * 0x195's third row (a track with the comment column, "D♯ - 109"), 0x186's
 * ALL row and first album, 0x1b3's date added, 0x1b7's path, 0x184's
 * footer, 0x181's count and 0x1c9's render header. */
static const char track_row[] =
    "11872349ae11000001951041010f101400000010060606020602060606060606060602061100000eea1100000eea"
    "1100000014260000000a0043006f006d00650020004f0076006500720000110000001226000000090044266f0020"
    "002d00200031003000390000110000230411010000001100000eea11000000001100000100110000000911e22311"
    "9a110000000626000000030043006d00001100002a9e";
static const char all_row[] =
    "11872349ae11000001861041010f10140000001006060602060206060606060606060206110000000011ffffffff"
    "110000000c2600000006fffa0041004c004cfffb000011000000022600000001000011000000a011000000001100"
    "00000011000000001100000000110000000011000000001100000002260000000100001100000000";
static const char album_row[] =
    "11872349ae11000001861041010f10140000001006060602060206060606060606060206110000000011dc0d0bca"
    "1100000010260000000800410061006c006900790061006800001100000002260000000100001100000002110000"
    "000011dc0d0bca11000000001100000000110000000011000000001100000002260000000100001100000000";
static const char date_row[] =
    "11872349ae11000001b31041010f101400000010060606020602060606060606060602061100000001110000475f"
    "1100000016260000000b0032003000320033002d00300038002d0030003600001100000002260000000100001100"
    "00002e11000000001100000000110000000011000000001100000000110000000011000000022600000001000011"
    "00000000";
static const char path_row[] =
    "11872349ae11000001b71041010f101400000010060606020602060606060606060602061100b11858110000475f"
    "11000000902600000048002f0056006f006c0075006d00650073002f00530044002f00520042002f00410061006c"
    "0069007900610068002f0055006e006b006e006f0077006e00200041006c00620075006d002f0037003000200061"
    "007400200079006f0075007200200062006500730074002000280079006f007500200061007200650020006c006f"
    "007600650029002e006d007000330000110000000226000000010000110000000011000000001100000000110000"
    "00001100000000110000000011000000001100000002260000000100001100000000";
static const char footer[] =
    "11872349ae11000001841042010f001400000000";
static const char count_1400[] =
    "11872349ae11000001811040000f021400000002060611000014001100000007";
static const char header_0b[] =
    "11872349ae11000001c91040010f02140000000206061100000001110000000b";
/* A CDJ-3000's render: 8 arguments (rbxport's corpus, transaction 0x180). */
static const char render_req[] =
    "11872349ae11000001801030000f0814000000080606060606060606110101030111000000001100000009110000"
    "00001100000009110000000c11000000011100000000";
/* Its waveform preview request: five declared, the blob left out. */
static const char preview_req[] =
    "11872349ae11000001bd1020040f051400000005060606060311010803011100000000110000475f1100000000";

static void item_bytes(const RXDbItem *it, uint32_t txid, const char *hex, const char *what)
{
    uint8_t b[2048];
    size_t n = unhex(hex, b, sizeof(b));
    RXWr w = rx_wr_dyn();
    rx_dbmsg_item(&w, txid, it, true);
    CHECK_BYTES(w.p, w.len, b, n, what);
    rx_wr_free(&w);
}

/* An item's texts as Latin-1 for a CDJ-900 (rx_utf8_latin1: punctuation,
 * kana; what is Latin-1 already as it is); without the flag, all as they
 * are. */
static void test_latin1(void)
{
    RXDbItem it = { .id = 1, .text = "Don’t “stop” ‐ now…", .text2 = "☆ é",
                    .text3 = "ごーいん！", .type = RX_DB_ITEM_TITLE };
    for (int latin1 = 0; latin1 < 2; latin1++) {
        RXWr w = rx_wr_dyn();
        rx_dbmsg_item_latin1(&w, 7, &it, true, latin1);
        RXDbMsg m;
        RXDbRow row;
        CHECK(rx_dbmsg_parse(w.p, w.len, 1 << 16, &m) > 0 && rx_dbrow_parse(&m, &row) == 0);
        CHECK_STR(row.text, latin1 ? "Don't \"stop\" - now..." : it.text);
        CHECK_STR(row.text2, it.text2);
        CHECK_STR(row.text3, latin1 ? "Gooin!" : it.text3);
        rx_dbrow_free(&row);
        rx_wr_free(&w);
    }
}

static void test_codec(void)
{
    const char *const vectors[] = {
        setup_req, setup_rep, root_req, no_art, artist_row, user_info_hdr, error_2fff,
        legacy_setup, legacy_setup_rep, track_row, all_row, album_row, date_row, path_row, footer,
        count_1400, header_0b, render_req, preview_req,
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(vectors); i++) {
        if (vectors[i] == user_info_hdr)
            continue;           /* a header only: its blob is not there */
        char what[32];
        snprintf(what, sizeof(what), "vector %zu", i);
        round_trip(vectors[i], what);
    }

    /* The items, written from their values. */
    item_bytes(&(RXDbItem) { .id = 2, .text = "\xef\xbf\xba" "ARTIST" "\xef\xbf\xbb", .type = 0x81 },
               0x180, artist_row, "artist heading");
    item_bytes(&(RXDbItem) { .a = 0xeea, .id = 0xeea, .text = "Come Over", .text2 = "D\xe2\x99\xaf - 109",
                             .type = 0x2304, .flags = 0x1000000, .c = 0xeea, .e = 0x100, .key = 9,
                             .art = 0xe223119a, .text3 = "Cm", .f = 0x2a9e },
               0x195, track_row, "track row");
    item_bytes(&(RXDbItem) { .id = RX_DB_ALL, .text = "\xef\xbf\xba" "ALL" "\xef\xbf\xbb", .type = 0xa0 },
               0x186, all_row, "ALL row");
    item_bytes(&(RXDbItem) { .id = 0xdc0d0bca, .text = "Aaliyah", .type = 2, .c = 0xdc0d0bca },
               0x186, album_row, "album row");
    item_bytes(&(RXDbItem) { .a = 1, .id = 0x475f, .text = "2023-08-06", .type = 0x2e }, 0x1b3,
               date_row, "date row");
    item_bytes(&(RXDbItem) { .a = 0xb11858, .id = 0x475f, .type = 0,
                             .text = "/Volumes/SD/RB/Aaliyah/Unknown Album/70 at your best (you are love).mp3" },
               0x1b7, path_row, "path row");

    /* The rows read back. */
    uint8_t b[2048];
    size_t n = unhex(track_row, b, sizeof(b));
    RXDbMsg m;
    RXDbRow row;
    CHECK_EQ(rx_dbmsg_parse(b, n, 1 << 20, &m), (int)n);
    CHECK_EQ(rx_dbrow_parse(&m, &row), 0);
    CHECK_STR(row.text, "Come Over");
    CHECK_STR(row.text2, "D\xe2\x99\xaf - 109");
    CHECK_STR(row.text3, "Cm");
    CHECK(row.type == 0x2304 && row.flags == 0x1000000 && row.art == 0xe223119a && row.f == 0x2a9e);
    rx_dbrow_free(&row);

    /* The preview request: five declared, four sent, the blob empty. */
    n = unhex(preview_req, b, sizeof(b));
    CHECK_EQ(rx_dbmsg_parse(b, n, 1 << 20, &m), (int)n);
    CHECK(m.nb_args == 5 && m.args[4].type == RX_DB_BLOB && !m.args[4].p && m.args[2].num == 0x475f);
    /* rekordbox's "no artwork": four declared, three sent. */
    n = unhex(no_art, b, sizeof(b));
    CHECK_EQ(rx_dbmsg_parse(b, n, 1 << 20, &m), (int)n);
    CHECK(m.nb_args == 4 && m.args[1].num == 0x32 && !m.args[3].p);
    /* Two back to back: the second's magic is not eaten. */
    uint8_t two[256];
    memcpy(two, b, n);
    memcpy(two + n, b, n);
    CHECK_EQ(rx_dbmsg_parse(two, 2 * n, 1 << 20, &m), (int)n);

    /* A tag list of twelve, zero-padded (rbxport: alphatheta-connect's). */
    RXWr w = rx_wr_dyn();
    put_msg(&w, 1, RX_DB_ROOT, NULL, 0);
    w.p[14] = 2;
    rx_wr_at_u32(&w, 16, 12);
    static const uint8_t padded[] = { 6, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0x11, 0, 0, 0, 7, 0x0f, 9 };
    rx_wr_bytes(&w, padded, sizeof(padded));
    CHECK_EQ(rx_dbmsg_parse(w.p, w.len, 1 << 20, &m), (int)w.len);
    CHECK(m.nb_args == 2 && m.args[0].num == 7 && m.args[1].num == 9);
    rx_wr_free(&w);

    /* A two-byte number. */
    w = rx_wr_dyn();
    put_msg(&w, 1, RX_DB_ROOT, NULL, 0);
    w.p[14] = 1;
    rx_wr_at_u32(&w, 16, 1);
    rx_wr_bytes(&w, (const uint8_t[]) { 6, 0x10, 0x01, 0x00 }, 4);
    CHECK_EQ(rx_dbmsg_parse(w.p, w.len, 1 << 20, &m), (int)w.len);
    CHECK_EQ(rx_dbmsg_num(&m, 0), 256);
    rx_wr_free(&w);

    /* Lies and garbage. */
    const uint8_t garbage[] = { 0x51, 0x73, 0x70, 0x74 };
    CHECK_EQ(rx_dbmsg_parse(garbage, sizeof(garbage), 1 << 20, &m), -EPROTO);
    n = unhex(setup_req, b, sizeof(b));
    b[2] ^= 1;                  /* the magic */
    CHECK_EQ(rx_dbmsg_parse(b, 3, 1 << 20, &m), -EPROTO);
    for (int lie = 0; lie < 5; lie++) {
        w = rx_wr_dyn();
        put_msg(&w, 1, RX_DB_SEARCH, NULL, 0);
        const uint8_t *tail;
        size_t tn;
        static const uint8_t blob_lie[] = { 6, 3, 0x11, 0, 0, 0, 5, 0x14, 0x7f, 0xff, 0xff, 0xff };
        static const uint8_t str_lie[] = { 2, 0x26, 0x40, 0, 0, 0 };
        static const uint8_t wrong_tag[] = { 6, 0x26, 0, 0, 0, 1, 0, 0 };
        static const uint8_t unknown_tag[] = { 9 };
        static const uint8_t too_many[] = { 0 };
        int want;
        switch (lie) {
        case 0:  tail = blob_lie; tn = sizeof(blob_lie); want = -E2BIG; w.p[14] = 2; break;
        case 1:  tail = str_lie; tn = sizeof(str_lie); want = -E2BIG; w.p[14] = 1; break;
        case 2:  tail = wrong_tag; tn = sizeof(wrong_tag); want = -EPROTO; w.p[14] = 1; break;
        case 3:  tail = unknown_tag; tn = sizeof(unknown_tag); want = -EPROTO; w.p[14] = 1; break;
        default: tail = too_many; tn = sizeof(too_many); want = -EPROTO; w.p[14] = 33; break;
        }
        rx_wr_at_u32(&w, 16, lie == 0 ? 2 : lie == 4 ? 33 : 1);
        rx_wr_bytes(&w, tail, tn);
        int r = rx_dbmsg_parse(w.p, w.len, 1 << 20, &m);
        if (r != want) {
            fprintf(stderr, "lie %d: %d, not %d\n", lie, r, want);
            failures++;
        }
        rx_wr_free(&w);
    }

    /* Text above the BMP survives a round trip. */
    RXDbMsg s = { .txid = 9, .kind = RX_DB_SEARCH, .nb_args = 2 };
    s.args[0] = (RXDbArg) { .type = RX_DB_STR, .str = "Bj\xc3\xb6rk \xf0\x9f\x8e\xa7 \xe3\x81\x82" };
    s.args[1] = (RXDbArg) { .type = RX_DB_BLOB, .p = (const uint8_t *)"abc", .len = 3 };
    w = rx_wr_dyn();
    rx_dbmsg_write(&w, &s);
    CHECK_EQ(rx_dbmsg_parse(w.p, w.len, 1 << 20, &m), (int)w.len);
    char *t = rx_dbmsg_str(&m, 0);
    CHECK_STR(t, "Bj\xc3\xb6rk \xf0\x9f\x8e\xa7 \xe3\x81\x82");
    free(t);
    uint32_t bl;
    const uint8_t *bp = rx_dbmsg_blob(&m, 1, &bl);
    CHECK(bp && bl == 3 && !memcmp(bp, "abc", 3));
    rx_wr_free(&w);

    /* The port query and its answer (track_metadata.adoc). */
    static const uint8_t q[] = { 0, 0, 0, 0x0f, 'R', 'e', 'm', 'o', 't', 'e', 'D', 'B', 'S', 'e',
                                 'r', 'v', 'e', 'r', 0 };
    CHECK_EQ(rx_dbquery_parse(q, sizeof(q)), (int)sizeof(q));
    CHECK_EQ(rx_dbquery_parse(q, 7), 0);
    CHECK_EQ(rx_dbquery_parse((const uint8_t *)"GET / HTTP", 10), -EPROTO);
    uint8_t rep[2];
    rx_dbquery_reply(1051, rep);
    CHECK(rep[0] == 0x04 && rep[1] == 0x1b);
}

/* --------------------------------------------------- in-memory catalogs */

typedef struct Mem {
    RXPlan    plan;
    RXTrack **tracks;
    int       nb;
} Mem;

static void mem_free(Mem *m)
{
    rx_plan_free(&m->plan);
    for (int i = 0; i < m->nb; i++)
        rx_track_free(m->tracks[i]);
    free(m->tracks);
    *m = (Mem) { 0 };
}

static RXPlanName *mem_names(int n, const char *fmt)
{
    RXPlanName *v = calloc((size_t)RX_MAX(n, 1), sizeof(*v));
    for (int i = 0; v && i < n; i++)
        v[i] = (RXPlanName) { .id = (uint32_t)i + 1, .name = rx_asprintf(fmt, i) };
    return v;
}

/* Two of the tracks in rbxport's capture, as far as the replies compared
 * here need them: "At Your Best (You Are Love)" (0x475f), 290 s, a grid at
 * 78.08 BPM whose first beat, a third, is at 467 ms, and its eight cues,
 * each named "CUE(Auto)", the hot ones in colour 0x15 with green LEDs. */
static int capture_catalog(Mem *m)
{
    *m = (Mem) { .nb = 2 };
    m->tracks = calloc(2, sizeof(*m->tracks));
    m->plan.tracks = calloc(2, sizeof(*m->plan.tracks));
    m->plan.artists = mem_names(1, "Aaliyah");
    m->plan.nb_artists = 1;
    if (!m->tracks || !m->plan.tracks || !m->plan.artists)
        return -ENOMEM;
    m->plan.nb_tracks = 2;
    static const struct { uint32_t id; const char *title, *file; double duration; } v[2] = {
        { 0x431d, "The One I Gave My Heart To", "78 the one i gave my heart to.mp3", 271.07 },
        { 0x475f, "At Your Best (You Are Love)", "70 at your best (you are love).mp3", 290.116 },
    };
    for (int i = 0; i < 2; i++) {
        RXTrack *t = m->tracks[i] = rx_track_new(v[i].file);
        if (!t)
            return -ENOMEM;
        rx_meta_set(&t->meta.title, v[i].title);
        snprintf(t->key, sizeof(t->key), "%s", i ? "D" : "G");
        RXPlanTrack *pt = &m->plan.tracks[i];
        *pt = (RXPlanTrack) {
            .id        = v[i].id,
            .track     = t,
            .usb_path  = rx_asprintf("/Contents/Aaliyah/%s", v[i].file),
            .duration  = v[i].duration,
            .artist_id = 1,
            .bit_rate  = 320,
            .file_type = 1,
            .file_size = 11606104,
        };
        snprintf(pt->key_name, sizeof(pt->key_name), "%s", t->key);
        if (rx_grid_init(&pt->grid, 0.467, 78.08) < 0)
            return -ENOMEM;
        pt->grid.sections[0].start = 0.467;
        pt->grid.sections[0].beat = 3;
    }
    RXPlanTrack *pt = &m->plan.tracks[1];
    static const double hot[4] = { 0.467, 0.083, 23.521, 201.803 };
    static const double mem[4] = { 2.004, 0.467, 23.521, 201.803 };
    pt->cues = calloc(8, sizeof(*pt->cues));
    if (!pt->cues)
        return -ENOMEM;
    for (int i = 0; i < 8; i++) {
        RXPlanCue *q = &pt->cues[pt->nb_cues++];
        *q = (RXPlanCue) {
            .time       = i < 4 ? hot[i] : mem[i - 4],
            .loop_end   = -1.0,
            .hot        = i < 4 ? i : -1,
            .color_code = i < 4 ? 0x15 : 0,
            .rgb        = i < 4 ? 0x00ff00 : 0,
        };
        snprintf(q->name, sizeof(q->name), "CUE(Auto)");
    }
    return 0;
}

/* rekordbox's extended cue list for 0x475f (rbxport's corpus, transaction
 * 0x1b8): hot cues A to D, then four memory cues. */
static const char captured_cues[] =
    "90000000010001000000e803d3010000ffffffff000000000000000000000000ffffffff000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "00001500ff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "90000000020001000000e80353000000ffffffff000000000000000000000000ffffffff000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "00001500ff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "90000000030001000000e803e15b0000ffffffff000000000000000000000000ffffffff000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "00001500ff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "90000000040001000000e8034b140300ffffffff000000000000000000000000ffffffff000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "00001500ff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "90000000000001000000e803d4070000ffffffff000000000000000000000000ffff0100000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "90000000000001000000e803d3010000ffffffff00000000000000000000000000000200000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "90000000000001000000e803e15b0000ffffffff00000000000000000000000001000300000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "90000000000001000000e8034b140300ffffffff0000000000000000000000000200ffff000000000000000000000000"
    "000000005600000000000000000000000000000000000000140043005500450028004100750074006f00290000002c00"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
/* The head of its grid reply (0x1b9), 6052 bytes in all. */
static const char captured_grid[] =
    "00000800790100009017000001000000010000000300801ed3010000ffffffffffffffff0400801ed3040000ffffffff"
    "ffffffff0100801ed4070000ffffffffffffffff";

/* The CDJ-3000's side of rbxport's capture, whole
 * (tests/corpus/link-export.player.bin, 4383 bytes: the greeting and 97
 * requests). */
static const char player_stream[] =
    "110000000111872349ae11fffffffe1000000f02140000000206061100000001110000001411872349ae110000017e10"
    "30070f02140000000206061101080301110000000011872349ae110000017f1010000f03140000000306060611010103"
    "0111000000001105cfffff11872349ae11000001801030000f0814000000080606060606060606110101030111000000"
    "00110000000911000000001100000009110000000c1100000001110000000011872349ae11000001811014000f031400"
    "00000306060611010503011100000000110000000011872349ae11000001821030000f08140000000806060606060606"
    "0611010503011100000000110000000711000000001100000007110000000c1100000001110000000011872349ae1100"
    "0001831010020f02140000000206061101010301110000000011872349ae11000001841030000f081400000008060606"
    "060606060611010103011100000000110000001911000000001100003fb6110000000c11000000011100000000118723"
    "49ae11000001851011020f03140000000306060611010203011100000000116d5c28f211872349ae1100000186103000"
    "0f081400000008060606060606060611010203011100000000110000000711000000001100000007110000000c110000"
    "0001110000000011872349ae11000001871020030f031400000003060606110108030111dc0d0bca1100000001118723"
    "49ae11000001881020030f03140000000306060611010803011143895263110000000111872349ae1100000189102003"
    "0f031400000003060606110108030111438953cc110000000111872349ae110000018a1020030f031400000003060606"
    "1101080301114389539f110000000111872349ae110000018b1020030f031400000003060606110108030111efdb88a5"
    "110000000111872349ae110000018c1010020f02140000000206061101010301110000000011872349ae110000018d10"
    "31000f041400000004060606061101010301116d5c28f21100000000110000000111872349ae110000018e1010020f02"
    "140000000206061101010301110000000011872349ae110000018f1030000f0814000000080606060606060606110101"
    "03011100000000110000001911000000001100003fb6110000000c1100000001110000000011872349ae110000019010"
    "11020f03140000000306060611010203011100000000116d5c28f211872349ae11000001911030000f08140000000806"
    "0606060606060611010203011100000000110000000711000000001100000007110000000c1100000001110000000011"
    "872349ae11000001921011020f03140000000306060611010103011100000000116d5c28f211872349ae110000019310"
    "30000f081400000008060606060606060611010103011100000000110000000711000000001100000007110000000c11"
    "00000001110000000011872349ae11000001941012020f0414000000040606060611010203011100000000116d5c28f2"
    "11ffffffff11872349ae11000001951030000f081400000008060606060606060611010203011100000000110000000c"
    "1100000000110000000c110000000c1100000001110000000011872349ae11000001961020030f031400000003060606"
    "1101080301110000475f110000000111872349ae1100000197102c040f04140000000406060606110108030111000047"
    "5f1134565750110054584511872349ae11000001981020030f031400000003060606110108030111000035fe11000000"
    "0111872349ae1100000199102c040f04140000000406060606110108030111000035fe11345657501100545845118723"
    "49ae110000019a1020030f03140000000306060611010803011100000eea110000000111872349ae110000019b102003"
    "0f03140000000306060611010803011100003b34110000000111872349ae110000019c102c040f041400000004060606"
    "0611010803011100000eea1134565750110054584511872349ae110000019d1020030f03140000000306060611010803"
    "0111000019fa110000000111872349ae110000019e102c040f0414000000040606060611010803011100003b34113456"
    "5750110054584511872349ae110000019f1020030f03140000000306060611010803011100000ce61100000001118723"
    "49ae11000001a0102c040f04140000000406060606110108030111000019fa1134565750110054584511872349ae1100"
    "0001a11020030f031400000003060606110108030111000028dc110000000111872349ae11000001a2102c040f041400"
    "0000040606060611010803011100000ce61134565750110054584511872349ae11000001a31020030f03140000000306"
    "060611010803011100006272110000000111872349ae11000001a4102c040f0414000000040606060611010803011100"
    "0028dc1134565750110054584511872349ae11000001a51020030f03140000000306060611010803011100007bb01100"
    "00000111872349ae11000001a6102c040f04140000000406060606110108030111000062721134565750110054584511"
    "872349ae11000001a71020030f03140000000306060611010803011100004842110000000111872349ae11000001a810"
    "2c040f0414000000040606060611010803011100007bb01134565750110054584511872349ae11000001a91020030f03"
    "140000000306060611010803011100001b5c110000000111872349ae11000001aa102c040f0414000000040606060611"
    "0108030111000048421134565750110054584511872349ae11000001ab1020030f031400000003060606110108030111"
    "0000431d110000000111872349ae11000001ac102c040f0414000000040606060611010803011100001b5c1134565750"
    "110054584511872349ae11000001ad102c040f041400000004060606061101080301110000431d113456575011005458"
    "4511872349ae11000001ae1012020f0414000000040606060611010103011100000000116d5c28f211ffffffff118723"
    "49ae11000001af1031000f041400000004060606061101010301110000475f1100000000110000000111872349ae1100"
    "0001b01012020f0414000000040606060611010103011100000000116d5c28f211ffffffff11872349ae11000001b110"
    "30000f081400000008060606060606060611010103011100000000110000000c1100000000110000000c110000000c11"
    "00000001110000000011872349ae11000001b21020020f02140000000206061101020301110000475f11872349ae1100"
    "0001b31030000f0814000000080606060606060606110102030111000000001100000010110000000011000000101100"
    "00000c1100000001110000000011872349ae11000001b41020020f02140000000206061101020301110000475f118723"
    "49ae11000001b51030000f08140000000806060606060606061101020301110000000011000000101100000000110000"
    "0010110000000c1100000001110000000011872349ae11000001b61021020f0214000000020606110108030111000047"
    "5f11872349ae11000001b71030000f081400000008060606060606060611010803011100000000110000000711000000"
    "001100000007110000000c1100000001110000000011872349ae11000001b8102b040f03140000000306060611010803"
    "01110000475f110000000011872349ae11000001b91022040f02140000000206061101080301110000475f11872349ae"
    "11000001ba102c040f041400000004060606061101080301110000475f1132545150110054584511872349ae11000001"
    "bb1025040f02140000000206061101080301110000475f11872349ae11000001bc102d040f0414000000040606060611"
    "01080301110000475f1136565750110058453211872349ae11000001bd1020040f051400000005060606060311010803"
    "011100000000110000475f110000000011872349ae11000001be102d040f041400000004060606061101080301110000"
    "475f1137565750110058453211872349ae11000001bf1020020f02140000000206061101020301110000475f11872349"
    "ae11000001c0102c040f041400000004060606061101080301110000475f1135565750110054584511872349ae110000"
    "01c11030000f081400000008060606060606060611010203011100000000110000001011000000001100000010110000"
    "000c1100000001110000000011872349ae11000001c21029040f0314000000030606061101080301110000475f110000"
    "000011872349ae11000001c3102c040f041400000004060606061101080301110000475f114953535011005458451187"
    "2349ae11000001c41020020f02140000000206061101020301110000475f11872349ae11000001c51030000f08140000"
    "0008060606060606060611010203011100000000110000001011000000001100000010110000000c1100000001110000"
    "000011872349ae11000001c61020020f02140000000206061101020301110000475f11872349ae11000001c71030000f"
    "081400000008060606060606060611010203011100000000110000001011000000001100000010110000000c11000000"
    "01110000000011872349ae11000001c81012020f0414000000040606060611010303011100000000116d5c28f211ffff"
    "ffff11872349ae11000001c91030000f08140000000806060606060606061101030301110000000b1100000001110000"
    "0000110000000c110000000c1100000000110000000011872349ae11000001ca1021020f021400000002060611010803"
    "01110000431d11872349ae11000001cb1030000f08140000000806060606060606061101080301110000000011000000"
    "0711000000001100000007110000000c1100000001110000000011872349ae11000001cc1020020f0214000000020606"
    "1101020301110000431d11872349ae11000001cd102b040f0314000000030606061101080301110000431d1100000000"
    "11872349ae11000001ce1030000f08140000000806060606060606061101020301110000000011000000101100000000"
    "1100000010110000000c1100000001110000000011872349ae11000001cf1022040f0214000000020606110108030111"
    "0000431d11872349ae11000001d0102c040f041400000004060606061101080301110000431d11325451501100545845"
    "11872349ae11000001d11025040f02140000000206061101080301110000431d11872349ae11000001d2102d040f0414"
    "00000004060606061101080301110000431d1136565750110058453211872349ae11000001d31020040f051400000005"
    "060606060311010803011100000000110000431d110000000011872349ae11000001d4102d040f041400000004060606"
    "061101080301110000431d1137565750110058453211872349ae11000001d51020020f02140000000206061101020301"
    "110000431d11872349ae11000001d61030000f0814000000080606060606060606110102030111000000001100000010"
    "11000000001100000010110000000c1100000001110000000011872349ae11000001d7102c040f041400000004060606"
    "061101080301110000431d1135565750110054584511872349ae11000001d81029040f03140000000306060611010803"
    "01110000431d110000000011872349ae11000001d9102c040f041400000004060606061101080301110000431d114953"
    "5350110054584511872349ae11000001da1020020f02140000000206061101020301110000431d11872349ae11000001"
    "db1030000f08140000000806060606060606061101020301110000000011000000101100000000110000001011000000"
    "0c1100000001110000000011872349ae11000001dc1020020f02140000000206061101020301110000431d11872349ae"
    "11000001dd1030000f081400000008060606060606060611010203011100000000110000001011000000001100000010"
    "110000000c11000000011100000000"
    ;

static void test_capture(void)
{
    Mem mem;
    RXCatalog *cat = NULL;
    CHECK_EQ(capture_catalog(&mem), 0);
    CHECK_EQ(rx_catalog_new(&cat, NULL, &mem.plan), 0);
    if (!cat) {
        mem_free(&mem);
        return;
    }

    /* The extended cue list against rekordbox's: the hot cues byte for
     * byte; the memory cues too, but for their times, which rekordbox sent
     * in its table's order and rxbox sends by time. */
    RXWr blob = rx_wr_dyn();
    uint32_t count = 0;
    CHECK_EQ(rx_catalog_cues_ext(cat, 0x475f, &blob, &count), 0);
    CHECK_EQ(count, 8);
    static uint8_t want[2048];
    size_t wn = unhex(captured_cues, want, sizeof(want));
    CHECK_EQ(wn, 1152);
    CHECK_EQ(blob.len, 1152);
    if (blob.len == wn) {
        CHECK_BYTES(blob.p, 4 * 0x90, want, 4 * 0x90, "hot cues");
        for (int i = 4; i < 8; i++) {
            uint8_t a[0x90], b[0x90];
            memcpy(a, blob.p + i * 0x90, 0x90);
            memcpy(b, want + i * 0x90, 0x90);
            memset(a + 0x0c, 0, 4);
            memset(b + 0x0c, 0, 4);
            CHECK_BYTES(a, 0x90, b, 0x90, "memory cue");
        }
    }
    rx_wr_free(&blob);

    /* The grid against rekordbox's. */
    blob = rx_wr_dyn();
    CHECK_EQ(rx_catalog_grid(cat, 0x475f, &blob), 0);
    wn = unhex(captured_grid, want, sizeof(want));
    CHECK_EQ(blob.len, 6052);
    CHECK_BYTES(blob.p, RX_MIN(blob.len, wn), want, wn, "grid head");
    RXBeat *beats;
    int nb;
    CHECK_EQ(rx_db_grid_parse(blob.p, blob.len, &beats, &nb), 0);
    CHECK_EQ(nb, 377);
    if (nb == 377)
        CHECK(beats[0].number == 3 && fabs(beats[2].time - 2.004) < 1e-9 && fabs(beats[0].bpm - 78.08) < 1e-9);
    free(beats);
    rx_wr_free(&blob);

    /* The CDJ-3000's requests, all of them: each answered on its own
     * transaction as rekordbox answered it (rbxport's corpus test). */
    static uint8_t stream[8192];
    size_t sn = unhex(player_stream, stream, sizeof(stream));
    CHECK_EQ(sn, 4383);
    RXDbSession *s;
    CHECK_EQ(rx_dbsession_new(&s, &cat, 0x11), 0);
    RXWr out = rx_wr_dyn();
    CHECK_EQ(rx_dbsession_input(s, stream, sn, &out), 0);
    rx_dbsession_free(&s);
    CHECK(out.len > 5 && !memcmp(out.p, rx_db_greeting, 5));

    Out in = { .p = stream, .len = sn, .pos = 5 }, rep = { .p = out.p, .len = out.len, .pos = 5 };
    RXDbMsg q, r;
    int requests = 0;
    uint8_t vb[256];
    while (next_msg(&in, &q)) {
        requests++;
        if (!next_msg(&rep, &r) || r.txid != q.txid) {
            fprintf(stderr, "request %04x (%x) unanswered\n", q.kind, q.txid);
            failures++;
            break;
        }
        size_t at = rep.pos;
        if (q.kind == RX_DB_SETUP) {
            size_t vn = unhex(setup_rep, vb, sizeof(vb));
            CHECK_BYTES(rep.p + at - vn, vn, vb, vn, "setup reply");
        } else if (q.kind == RX_DB_RENDER) {
            CHECK_EQ(r.kind, RX_DB_MENU_HEADER);
            while (next_msg(&rep, &r) && r.kind == RX_DB_MENU_ITEM)
                CHECK_EQ(r.nb_args, 16);
            CHECK_EQ(r.kind, RX_DB_MENU_FOOTER);
        } else if (q.kind >> 12 == 2 && q.kind != RX_DB_METADATA && q.kind != RX_DB_TRACK_INFO) {
            CHECK_EQ(rx_dbmsg_num(&r, 0), q.kind);
            CHECK_EQ(r.kind >> 12, 4);
            if (q.txid == 0x187) {
                size_t vn = unhex(no_art, vb, sizeof(vb));
                CHECK_BYTES(rep.p + at - vn, vn, vb, vn, "no artwork");
            }
        } else {
            CHECK_EQ(r.kind, RX_DB_AVAILABLE);
            CHECK_EQ(rx_dbmsg_num(&r, 0), q.kind);
            if (q.kind == RX_DB_METADATA)
                CHECK_EQ(rx_dbmsg_num(&r, 1), 16);
            if (q.kind == RX_DB_TRACK_INFO)
                CHECK_EQ(rx_dbmsg_num(&r, 1), 7);
            if (q.kind == RX_DB_ROOT)
                CHECK_EQ(rx_dbmsg_num(&r, 1), 19);
            if (q.kind == RX_DB_SORTS)
                CHECK_EQ(rx_dbmsg_num(&r, 1), 7);
            if (q.kind == RX_DB_FILTER) {
                static const char filter_rep[] =
    "11872349ae110000017e1040000f021400000002060611000030071100000000";
                size_t vn = unhex(filter_rep, vb, sizeof(vb));
                CHECK_BYTES(rep.p + at - vn, vn, vb, vn, "filter reply");
            }
        }
    }
    CHECK_EQ(requests, 97);
    CHECK_EQ(rep.pos, rep.len);
    rx_wr_free(&out);

    /* The root menu's first row as rekordbox sent it (0x180). */
    CHECK_EQ(rx_dbsession_new(&s, &cat, 0x11), 0);
    out = rx_wr_dyn();
    RXWr req = rx_wr_dyn();
    rx_wr_bytes(&req, rx_db_greeting, 5);
    put_msg(&req, 0x17f, RX_DB_ROOT, (const uint32_t[]) { 0x01010301, 0, 0x05cfffff }, 3);
    put_msg(&req, 0x180, RX_DB_RENDER, (const uint32_t[]) { 0x01010301, 0, 9, 0, 9, 12, 1, 0 }, 8);
    put_msg(&req, 0xa2, RX_DB_USER_INFO, (const uint32_t[]) { 0x03090301 }, 1);
    put_msg(&req, 0x8123, 0x2fff, (const uint32_t[]) { 0x01010301, 0 }, 2);
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    rep = (Out) { .p = out.p, .len = out.len, .pos = 5 };
    next_msg(&rep, &r);
    CHECK(r.kind == RX_DB_AVAILABLE && rx_dbmsg_num(&r, 1) == 19);
    next_msg(&rep, &r);
    size_t at = rep.pos;
    next_msg(&rep, &r);
    size_t vn = unhex(artist_row, vb, sizeof(vb));
    CHECK_BYTES(rep.p + at, rep.pos - at, vb, vn, "root's first row");
    while (next_msg(&rep, &r) && r.kind != RX_DB_MENU_FOOTER)
        ;
    at = rep.pos;
    next_msg(&rep, &r);
    vn = unhex(user_info_hdr, vb, sizeof(vb));
    CHECK_EQ(rep.pos - at, vn + 160);
    CHECK_BYTES(rep.p + at, RX_MIN(vn, rep.pos - at), vb, vn, "user info");
    at = rep.pos;
    next_msg(&rep, &r);
    vn = unhex(error_2fff, vb, sizeof(vb));
    CHECK_BYTES(rep.p + at, rep.pos - at, vb, vn, "unknown request");
    rx_wr_free(&out);
    rx_wr_free(&req);
    rx_dbsession_free(&s);

    /* The older setup and its answer. */
    CHECK_EQ(rx_dbsession_new(&s, &cat, 0x11), 0);
    out = rx_wr_dyn();
    req = rx_wr_dyn();
    rx_wr_bytes(&req, rx_db_greeting, 5);
    vn = unhex(legacy_setup, vb, sizeof(vb));
    rx_wr_bytes(&req, vb, vn);
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    vn = unhex(legacy_setup_rep, vb, sizeof(vb));
    CHECK(out.len == 5 + vn && !memcmp(out.p + 5, vb, vn));
    /* A request given up (0001 with its txid, as players send it after a
     * preview they scrolled past): the request's answer and nothing more. */
    size_t had = out.len;
    rx_wr_free(&req);
    req = rx_wr_dyn();
    put_msg(&req, 0x3800584, RX_DB_ARTIST_ALBUMS, (const uint32_t[]) { 0x02020401, 0, 3 }, 3);
    put_msg(&req, 0x3800584, RX_DB_CANCEL, NULL, 0);
    put_msg(&req, 0x3800585, RX_DB_ROOT, (const uint32_t[]) { 0x02010401, 0, 0x00fdffff }, 3);
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    rep = (Out) { .p = out.p, .len = out.len, .pos = had };
    CHECK(next_msg(&rep, &r) && r.txid == 0x3800584 && r.kind == RX_DB_AVAILABLE &&
          rx_dbmsg_num(&r, 0) == RX_DB_ARTIST_ALBUMS);
    CHECK(next_msg(&rep, &r) && r.txid == 0x3800585 && r.kind == RX_DB_AVAILABLE &&
          rx_dbmsg_num(&r, 0) == RX_DB_ROOT);
    CHECK_EQ(rep.pos, rep.len);
    rx_wr_free(&out);
    rx_wr_free(&req);
    rx_dbsession_free(&s);

    rx_catalog_free(&cat);
    mem_free(&mem);
}

/* ------------------------------------------------------ session streams */

/* A conversation: what a CDJ-3000 sends to browse and look at a track. */
static void conversation(RXWr *w, uint32_t track)
{
    rx_wr_bytes(w, rx_db_greeting, 5);
    put_msg(w, RX_DB_SETUP_TXID, RX_DB_SETUP, (const uint32_t[]) { 2, 0x14 }, 2);
    put_msg(w, 1, RX_DB_ROOT, (const uint32_t[]) { 0x02010301, 0, 0x05cfffff }, 3);
    put_msg(w, 2, RX_DB_RENDER, (const uint32_t[]) { 0x02010301, 0, 25, 0, 25, 12, 1, 0 }, 8);
    put_msg(w, 3, RX_DB_TRACKS, (const uint32_t[]) { 0x02010301, 0 }, 2);
    put_msg(w, 4, RX_DB_RENDER, (const uint32_t[]) { 0x02010301, 0, 25, 0, 25, 12, 1, 0 }, 8);
    put_msg(w, 5, RX_DB_METADATA, (const uint32_t[]) { 0x02020301, track }, 2);
    put_msg(w, 6, RX_DB_RENDER, (const uint32_t[]) { 0x02020301, 0, 16, 0, 16, 12, 1, 0 }, 8);
    put_msg(w, 7, RX_DB_ARTWORK, (const uint32_t[]) { 0x02080301, track, 1 }, 3);
    put_msg(w, 8, RX_DB_GRID, (const uint32_t[]) { 0x02080301, track }, 2);
}

static int feed(RXCatalog **cat, const uint8_t *p, const size_t *cuts, int nb_cuts, size_t len,
                RXWr *out)
{
    RXDbSession *s;
    if (rx_dbsession_new(&s, cat, 17) < 0)
        return -ENOMEM;
    int ret = 0;
    size_t from = 0;
    for (int i = 0; i <= nb_cuts && ret >= 0; i++) {
        size_t to = i < nb_cuts ? cuts[i] : len;
        ret = rx_dbsession_input(s, p + from, to - from, out);
        from = to;
    }
    rx_dbsession_free(&s);
    return ret;
}

static void test_streams(RXCatalog **cat, uint32_t track)
{
    RXWr req = rx_wr_dyn(), ref = rx_wr_dyn();
    conversation(&req, track);
    CHECK_EQ(feed(cat, req.p, NULL, 0, req.len, &ref), 0);
    CHECK(ref.len > 1000);
    /* Split once at every byte boundary, then a byte at a time: the same
     * replies. */
    int bad = 0;
    for (size_t k = 0; k <= req.len; k++) {
        RXWr out = rx_wr_dyn();
        int ret = feed(cat, req.p, &k, 1, req.len, &out);
        if (ret || out.len != ref.len || memcmp(out.p, ref.p, ref.len))
            bad++;
        rx_wr_free(&out);
    }
    CHECK_EQ(bad, 0);
    size_t *cuts = malloc(req.len * sizeof(*cuts));
    for (size_t k = 0; cuts && k < req.len; k++)
        cuts[k] = k + 1;
    RXWr out = rx_wr_dyn();
    CHECK_EQ(feed(cat, req.p, cuts, (int)req.len - 1, req.len, &out), 0);
    CHECK(out.len == ref.len && !memcmp(out.p, ref.p, ref.len));
    free(cuts);
    rx_wr_free(&out);

    /* A teardown closes after the answers before it; nothing after it. */
    RXWr more = rx_wr_dyn();
    rx_wr_bytes(&more, req.p, req.len);
    put_msg(&more, RX_DB_SETUP_TXID, RX_DB_TEARDOWN, NULL, 0);
    put_msg(&more, 9, RX_DB_ROOT, (const uint32_t[]) { 0x02010301, 0 }, 2);
    out = rx_wr_dyn();
    CHECK_EQ(feed(cat, more.p, NULL, 0, more.len, &out), -ESHUTDOWN);
    CHECK(out.len == ref.len && !memcmp(out.p, ref.p, ref.len));
    rx_wr_free(&out);
    rx_wr_free(&more);

    /* Garbage closes: a wrong greeting, a foreign packet after it, a
     * message claiming a blob larger than the session takes, too many
     * arguments; a notice gets no answer. */
    static const uint8_t bad_greeting[] = { 0x11, 0, 0, 0, 2 };
    out = rx_wr_dyn();
    CHECK_EQ(feed(cat, bad_greeting, NULL, 0, 5, &out), -EPROTO);
    CHECK_EQ(out.len, 0);
    rx_wr_free(&out);
    RXWr g = rx_wr_dyn();
    rx_wr_bytes(&g, rx_db_greeting, 5);
    rx_wr_bytes(&g, "Qspt1WmJOL", 10);
    out = rx_wr_dyn();
    CHECK_EQ(feed(cat, g.p, NULL, 0, g.len, &out), -EPROTO);
    CHECK_EQ(out.len, 5);
    rx_wr_free(&out);
    rx_wr_free(&g);
    g = rx_wr_dyn();
    rx_wr_bytes(&g, rx_db_greeting, 5);
    put_msg(&g, 1, RX_DB_SAVE_CUE_EXT, NULL, 0);
    g.p[5 + 14] = 2;
    rx_wr_at_u32(&g, 5 + 16, 2);
    rx_wr_bytes(&g, (const uint8_t[]) { 6, 3, 0x11, 0, 0, 0, 1, 0x14, 0x10, 0, 0, 0 }, 12);
    out = rx_wr_dyn();
    CHECK_EQ(feed(cat, g.p, NULL, 0, g.len, &out), -E2BIG);
    rx_wr_free(&out);
    rx_wr_free(&g);
    g = rx_wr_dyn();
    rx_wr_bytes(&g, rx_db_greeting, 5);
    put_msg(&g, 1, RX_DB_HISTORY_ADD, (const uint32_t[]) { 0x02010301, track }, 2);
    put_msg(&g, 2, RX_DB_FILTER, (const uint32_t[]) { 0x02010301, 0 }, 2);
    out = rx_wr_dyn();
    CHECK_EQ(feed(cat, g.p, NULL, 0, g.len, &out), 0);
    RXDbMsg r;
    Out o = { .p = out.p, .len = out.len, .pos = 5 };
    CHECK(next_msg(&o, &r) && r.txid == 2 && r.kind == RX_DB_AVAILABLE);
    CHECK_EQ(o.pos, o.len);
    rx_wr_free(&out);
    rx_wr_free(&g);

    /* A render beyond the menu, or asking for everything: clamped. */
    g = rx_wr_dyn();
    rx_wr_bytes(&g, rx_db_greeting, 5);
    put_msg(&g, 1, RX_DB_TRACKS, (const uint32_t[]) { 0x02010301, 0 }, 2);
    put_msg(&g, 2, RX_DB_RENDER, (const uint32_t[]) { 0x02010301, 0xffffffff, 0xffffffff, 0, 0, 0 }, 6);
    put_msg(&g, 3, RX_DB_RENDER, (const uint32_t[]) { 0x02010301, 0, 0xffffffff, 0, 0, 0 }, 6);
    put_msg(&g, 4, RX_DB_RENDER, (const uint32_t[]) { 0x02070301, 0, 10, 0, 10, 0 }, 6);
    out = rx_wr_dyn();
    CHECK_EQ(feed(cat, g.p, NULL, 0, g.len, &out), 0);
    o = (Out) { .p = out.p, .len = out.len, .pos = 5 };
    next_msg(&o, &r);
    uint32_t total = rx_dbmsg_num(&r, 1);
    next_msg(&o, &r);
    CHECK(r.kind == RX_DB_MENU_HEADER && rx_dbmsg_num(&r, 1) == total);
    next_msg(&o, &r);
    CHECK_EQ(r.kind, RX_DB_MENU_FOOTER);
    uint32_t items = 0;
    next_msg(&o, &r);
    while (next_msg(&o, &r) && r.kind == RX_DB_MENU_ITEM)
        items++;
    CHECK_EQ(items, total);
    /* Nothing kept at location 7: a header and a footer. */
    next_msg(&o, &r);
    CHECK(r.kind == RX_DB_MENU_HEADER && next_msg(&o, &r) && r.kind == RX_DB_MENU_FOOTER);
    rx_wr_free(&out);
    rx_wr_free(&g);

    /* No catalog at all (between two libraries): every request still
     * answered, with nothing: the setup; the generic error for each menu,
     * a header and a footer for each render; the no-data replies. */
    RXCatalog *none = NULL;
    out = rx_wr_dyn();
    CHECK_EQ(feed(&none, req.p, NULL, 0, req.len, &out), 0);
    o = (Out) { .p = out.p, .len = out.len, .pos = 5 };
    int replies = 0;
    while (next_msg(&o, &r))
        replies++;
    CHECK_EQ(replies, 1 + 3 * (1 + 2) + 1 + 1);
    rx_wr_free(&out);

    rx_wr_free(&req);
    rx_wr_free(&ref);
}

/* --------------------------------------------------------- the library */

static char root[512], music[700], libdir[700];

typedef struct Fixture {
    const char *file, *codec, *title, *artist, *album, *genre, *label;
    double      bpm;
    const char *key;
    int         year, rating, color, number;
} Fixture;

static const Fixture fixtures[] = {
    { "alpha.flac", "-c:a flac", "Alpha", "Synth", "Synthetic", "Techno", "RXB", 126, "Am", 2024, 4, 5, 1 },
    { "nocover/bravo.mp3", "-c:a libmp3lame -b:a 320k", "Bravo", "Synth", "Synthetic", "House", NULL,
      128, "F#", 2019, 2, 0, 2 },
    { "charlie.opus", "-c:a libopus", "Charlie", "Other", NULL, "Techno", NULL, 140, "C", 0, 0, 8, 0 },
    { "delta.wav", "-c:a pcm_s16le", NULL, NULL, NULL, NULL, "RXB", 122, "", 1999, 5, 0, 0 },
};

static RXTrack *by_file(RXLibrary *lib, const char *file)
{
    for (int i = 0; i < lib->nb_tracks; i++)
        if (rx_str_ends(lib->tracks[i]->path, rx_path_base(file)))
            return lib->tracks[i];
    return NULL;
}

static const RXPlanTrack *plan_track(const RXPlan *p, uint32_t id)
{
    for (int i = 0; i < p->nb_tracks; i++)
        if (p->tracks[i].id == id)
            return &p->tracks[i];
    return NULL;
}

static int make_library(RXLibrary *lib)
{
    for (size_t i = 0; i < RX_ARRAY_ELEMS(fixtures); i++) {
        const Fixture *f = &fixtures[i];
        double p = 60.0 / f->bpm;
        if (sh("ffmpeg -v error -nostdin -y -f lavfi -i \"aevalsrc='0.7*sin(2*PI*55*t)*exp(-14*mod(t,%.6f))"
               "+0.05*sin(2*PI*7000*t)*exp(-60*mod(t+%.6f,%.6f))':s=44100:d=8\" %s '%s/%s'",
               p, p / 2, p, f->codec, music, f->file))
            return 77;
    }
    if (sh("ffmpeg -v error -nostdin -y -f lavfi -i color=c=orange:s=320x320 -frames:v 1 '%s/front.jpg'", music))
        return 77;

    CHECK_EQ(rx_library_open(lib, libdir), 0);
    int64_t now = rx_time_now();
    for (size_t i = 0; i < RX_ARRAY_ELEMS(fixtures); i++) {
        const Fixture *f = &fixtures[i];
        char *path = rx_asprintf("%s/%s", music, f->file);
        int err = 0;
        RXTrack *t = rx_library_add_file(lib, path, &err);
        free(path);
        CHECK(t);
        if (!t)
            return 1;
        RXMeta *m = &t->meta;
        rx_meta_set(&m->title, f->title);
        rx_meta_set(&m->artist, f->artist);
        rx_meta_set(&m->album, f->album);
        rx_meta_set(&m->genre, f->genre);
        rx_meta_set(&m->label, f->label);
        rx_meta_set(&m->comment, i == 0 ? "warm" : NULL);
        if (i == 1) {
            rx_meta_set(&m->remixer, "Remixer One");
            rx_meta_set(&m->original_artist, "Original One");
        }
        m->year = f->year;
        m->rating = f->rating;
        m->track_number = f->number;
        m->hue = f->color ? rx_track_color_hue(f->color) : RX_HUE_NONE;
        snprintf(t->key, sizeof(t->key), "%s", f->key);
        CHECK_EQ(rx_grid_init(&t->grid, 0.1, f->bpm), 0);
        double beat = 60.0 / f->bpm;
        RXCue c;
        if (i == 0) {
            rx_cue_init(&c, 0.1 + 4 * beat);
            c.hot = 0;
            c.loop_end = c.time + 4 * beat;
            snprintf(c.name, sizeof(c.name), "loop");
            snprintf(c.color, sizeof(c.color), "red");
            rx_track_add_cue(t, &c);
            rx_cue_init(&c, 0.1);
            rx_track_add_cue(t, &c);
            rx_cue_init(&c, 2.0);
            c.loop_end = 3.0;
            snprintf(c.color, sizeof(c.color), "blue");
            rx_track_add_cue(t, &c);
        } else if (i == 1) {
            rx_cue_init(&c, 1.0);
            c.hot = 3;
            rx_track_add_cue(t, &c);
            rx_cue_init(&c, 0.5);
            rx_track_add_cue(t, &c);
        }
        CHECK_EQ(rx_library_save_track(lib, t), 0);
        RXAudio a;
        CHECK_EQ(rx_decode(t->path, 1, 0, &a), 0);
        RXWaveforms w;
        CHECK_EQ(rx_waveforms_compute(a.samples, a.nb_frames, a.rate, &w), 0);
        CHECK_EQ(rx_library_waveforms_save(lib, t->id, &w), 0);
        rx_waveforms_free(&w);
        rx_audio_free(&a);
    }
    /* Played today: charlie, then alpha twice; charlie three days ago. */
    RXTrack *alpha = by_file(lib, "alpha.flac"), *charlie = by_file(lib, "charlie.opus");
    CHECK_EQ(rx_library_record_play_at(lib, charlie, now - 3 * 86400), 0);
    CHECK_EQ(rx_library_record_play(lib, charlie), 0);
    CHECK_EQ(rx_library_record_play(lib, alpha), 0);
    CHECK_EQ(rx_library_record_play(lib, alpha), 0);
    charlie->last_played = now - 20;
    alpha->last_played = now - 10;

    RXPlaylistFolder *sets = rx_library_folder_create(lib, lib->root_folder, "Sets");
    RXPlaylist *peak = sets ? rx_library_playlist_create(lib, sets, "Peak") : NULL;
    RXPlaylist *warm = rx_library_playlist_create(lib, lib->root_folder, "Warmup");
    CHECK(sets && peak && warm);
    if (!peak || !warm)
        return 1;
    rx_library_playlist_add(lib, peak, charlie, -1);
    rx_library_playlist_add(lib, peak, alpha, -1);
    rx_library_playlist_add(lib, warm, by_file(lib, "delta.wav"), -1);
    rx_library_playlist_add(lib, warm, by_file(lib, "bravo.mp3"), -1);
    rx_library_playlist_add(lib, warm, alpha, -1);
    return 0;
}

/* The server's side of a socket: a session fed what arrives. */
typedef struct Server {
    int               fd;
    RXCatalog *const *cat;
    int               result;
    pthread_t         thread;
} Server;

static void *serve(void *opaque)
{
    Server *sv = opaque;
    RXDbSession *s = NULL;
    if (rx_dbsession_new(&s, sv->cat, 17) < 0) {
        close(sv->fd);
        sv->result = -ENOMEM;
        return NULL;
    }
    uint8_t buf[1500];
    while (1) {
        ssize_t n = recv(sv->fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        RXWr out = rx_wr_dyn();
        int ret = rx_dbsession_input(s, buf, (size_t)n, &out);
        for (size_t off = 0; off < out.len;) {
            ssize_t w = send(sv->fd, out.p + off, out.len - off, MSG_NOSIGNAL);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        rx_wr_free(&out);
        if (ret < 0) {
            sv->result = ret;
            break;
        }
    }
    rx_dbsession_free(&s);
    close(sv->fd);
    return NULL;
}

static RXDbClient *open_pair(Server *sv, RXCatalog *const *cat, bool legacy)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0)
        return NULL;
    *sv = (Server) { .fd = fds[0], .cat = cat };
    if (pthread_create(&sv->thread, NULL, serve, sv)) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    RXDbClient *c = NULL;
    CHECK_EQ(rx_dbclient_open(&c, fds[1], 1, legacy, 5000), 0);
    if (c)
        CHECK_EQ(rx_dbclient_server_number(c), 17);
    return c;
}

static void close_pair(Server *sv, RXDbClient **c)
{
    rx_dbclient_close(c);
    pthread_join(sv->thread, NULL);
    CHECK_EQ(sv->result, -ESHUTDOWN);
}

static int find_row(const RXDbRow *rows, int nb, uint32_t id)
{
    for (int i = 0; i < nb; i++)
        if (rows[i].id == id)
            return i;
    return -1;
}

/* A menu's ids, in order, against the expected ones. */
static void menu_ids(RXDbClient *c, uint16_t kind, int loc, const uint32_t *args, int nb,
                     const uint32_t *want, int nb_want, const char *what)
{
    RXDbRow *rows;
    int n = 0;
    int ret = rx_dbclient_menu(c, kind, loc, args, nb, &rows, &n);
    bool ok = ret == 0 && n == nb_want;
    for (int i = 0; ok && i < n; i++)
        ok = rows[i].id == want[i];
    if (!ok) {
        fprintf(stderr, "%s (%04x): %d rows (%d):", what, kind, n, ret);
        for (int i = 0; i < n; i++)
            fprintf(stderr, " %x", rows[i].id);
        fprintf(stderr, ", wanted");
        for (int i = 0; i < nb_want; i++)
            fprintf(stderr, " %x", want[i]);
        fprintf(stderr, "\n");
        failures++;
    }
    rx_dbrows_free(&rows, n);
}

#define IDS(...) (const uint32_t[]) { __VA_ARGS__ }, (int)(sizeof((const uint32_t[]) { __VA_ARGS__ }) / sizeof(uint32_t))

static void test_library(RXLibrary *lib, RXPlan *plan, RXCatalog **cat)
{
    uint32_t alpha = (uint32_t)by_file(lib, "alpha.flac")->id;
    uint32_t bravo = (uint32_t)by_file(lib, "bravo.mp3")->id;
    uint32_t charlie = (uint32_t)by_file(lib, "charlie.opus")->id;
    uint32_t delta = (uint32_t)by_file(lib, "delta.wav")->id;
    const RXPlanTrack *pa = plan_track(plan, alpha), *pb = plan_track(plan, bravo);
    const RXPlanTrack *pc = plan_track(plan, charlie);
    CHECK(pa && pb && pc);
    if (!pa || !pb || !pc)
        return;

    Server sv;
    RXDbClient *c = open_pair(&sv, cat, false);
    if (!c)
        return;
    RXDbRow *rows;
    int n;

    /* The root menu: all 19 with a CDJ-3000's mask; ARTIST alone with
     * its bit; no DATE ADDED with the older 24-bit mask. */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_ROOT, 1, IDS(0, 0x05cfffff), &rows, &n), 0);
    CHECK_EQ(n, 19);
    if (n)
        CHECK(rows[0].id == 2 && rows[0].type == 0x81 && !strcmp(rows[0].text, "\xef\xbf\xba" "ARTIST" "\xef\xbf\xbb"));
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_ROOT, 1, IDS(0, 1u << 1), IDS(2), "root, ARTIST bit");
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_ROOT, 1, IDS(0, 0x00ffffff), &rows, &n), 0);
    CHECK(n == 18 && find_row(rows, n, 27) < 0);
    rx_dbrows_free(&rows, n);

    /* All tracks, by title, the artist beside it. */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_TRACKS, 1, IDS(0), &rows, &n), 0);
    CHECK_EQ(n, 4);
    if (n == 4) {
        CHECK(rows[0].id == alpha && rows[1].id == bravo && rows[2].id == charlie && rows[3].id == delta);
        CHECK_STR(rows[0].text, "Alpha");
        CHECK_STR(rows[0].text2, "Synth");
        CHECK_STR(rows[3].text, "delta");
        CHECK(rows[0].type == 0x0704 && rows[0].c == alpha && rows[0].e == 0x100 && rows[0].f == 12600);
        CHECK(rows[0].key == 15 && rows[0].art == pa->key_id);
        CHECK_STR(rows[0].text3, "Am");
        CHECK(rows[2].key == 16 && rows[1].key == 4);   /* C 8B, F# 2B */
        CHECK(rows[0].flags == 0 && rows[0].d == 0);
    }
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_TRACKS, 1, IDS(4), IDS(delta, alpha, bravo, charlie), "by BPM");
    menu_ids(c, RX_DB_TRACKS, 1, IDS(5), IDS(delta, alpha, bravo, charlie), "by rating");
    menu_ids(c, RX_DB_TRACKS, 1, IDS(12), IDS(bravo, alpha, charlie, delta), "by key");
    menu_ids(c, RX_DB_TRACKS, 1, IDS(2), IDS(charlie, alpha, bravo, delta), "by artist");

    /* Where a row is (3100, rbxport's capture). */
    RXDbMsg r;
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_TRACKS, 1, IDS(0), &rows, &n), 0);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_ITEM_POSITION, 1, (const uint32_t[]) { bravo, 0, 1 }, 3, &r), 0);
    CHECK(r.kind == RX_DB_AVAILABLE && rx_dbmsg_num(&r, 0) == 0x3100 && rx_dbmsg_num(&r, 1) == 1);

    /* Playlists: the root folder, a folder, a list in its order. */
    uint32_t sets = 0, peak = 0, warm = 0;
    for (int i = 0; i < plan->nb_playlists; i++) {
        const RXPlanPlaylist *pl = &plan->playlists[i];
        if (!strcmp(pl->name, "Sets"))
            sets = pl->id;
        else if (!strcmp(pl->name, "Peak"))
            peak = pl->id;
        else if (!strcmp(pl->name, "Warmup"))
            warm = pl->id;
    }
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_PLAYLIST, 1, IDS(0, 0, 1), &rows, &n), 0);
    CHECK_EQ(n, 2);
    if (n == 2)
        CHECK(rows[0].id == sets && rows[0].type == 1 && rows[0].d == 1 && rows[1].id == warm &&
              rows[1].type == 8 && rows[1].d == 2 && !strcmp(rows[1].text, "Warmup"));
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_PLAYLIST, 2, IDS(0, sets, 1), IDS(peak), "folder");
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_PLAYLIST, 2, IDS(0, warm, 0), &rows, &n), 0);
    CHECK_EQ(n, 3);
    if (n == 3)
        CHECK(rows[0].id == delta && rows[1].id == bravo && rows[2].id == alpha &&
              rows[0].d == 1 && rows[2].d == 3 && rows[0].flags == RX_DB_TRACK_LISTED);
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_PLAYLIST, 2, IDS(4, warm, 0), IDS(delta, alpha, bravo), "playlist by BPM");
    menu_ids(c, RX_DB_PLAYLIST, 2, IDS(0, 0x7777777, 0), NULL, 0, "no such playlist");

    /* Metadata: sixteen rows in rekordbox's order (rbxport's capture: the
     * year, 0x11, before the bit rate, 0x10). */
    CHECK_EQ(rx_dbclient_metadata(c, alpha, &rows, &n), 0);
    CHECK_EQ(n, 16);
    if (n == 16) {
        static const uint32_t types[16] = { 0x0704, 7, 2, 0x0b, 0x0d, 0x0f, 0x0a, 0x18, 6, 0x2e,
                                            0x23, 0x11, 0x10, 0x0e, 0x28, 0x29 };
        for (int i = 0; i < 16; i++)
            CHECK_EQ(rows[i].type, types[i]);
        CHECK(rows[0].id == alpha && !strcmp(rows[0].text, "Alpha"));
        CHECK(rows[1].a == 1 && rows[1].id == pa->artist_id && !strcmp(rows[1].text, "Synth"));
        CHECK(rows[2].a == 1 && !strcmp(rows[2].text, "Synthetic"));
        CHECK(rows[3].id == 8 || rows[3].id == 7);
        CHECK_EQ(rows[4].id, 12600);
        CHECK(rows[5].a == 1 && rows[5].id == pa->key_id && !strcmp(rows[5].text, "Am"));
        CHECK_EQ(rows[6].id, 4);
        CHECK(rows[7].id == 5 && !strcmp(rows[7].text, "Green"));
        CHECK_STR(rows[8].text, "Techno");
        char today[16];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(today, sizeof(today), "%Y-%m-%d", &tm);
        CHECK(rows[9].a == 1 && rows[9].id == alpha && !strcmp(rows[9].text, today));
        CHECK(rows[10].id == alpha && !strcmp(rows[10].text, "warm"));
        CHECK_EQ(rows[11].id, 2024);
        CHECK_EQ(rows[12].id, (uint32_t)pa->bit_rate);
        CHECK_STR(rows[13].text, "RXB");
    }
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_metadata(c, 0x7fffffff, &rows, &n), 0);
    CHECK_EQ(n, 0);

    /* Track info: the path the NFS tree serves and the size; the opus
     * track's transcode is made first (the plan was a dry one). */
    char *path;
    uint64_t size;
    CHECK_EQ(rx_dbclient_track_info(c, alpha, &path, &size), 0);
    CHECK_STR(path, pa->usb_path);
    CHECK_EQ(size, (uint64_t)rx_file_size(pa->source));
    free(path);
    CHECK(!rx_file_exists(pc->source));
    CHECK_EQ(rx_dbclient_track_info(c, charlie, &path, &size), 0);
    CHECK(path && rx_str_ends(path, ".flac") && !strcmp(path, pc->usb_path));
    CHECK(rx_file_exists(pc->source) && size == (uint64_t)rx_file_size(pc->source) &&
          (int64_t)size == pc->file_size);
    free(path);
    CHECK_EQ(rx_dbclient_track_info(c, 0x7fffffff, &path, &size), -ENOENT);
    /* The file's type in the first row's id, a FLAC's 5 for alpha (and a
     * player decodes the file as that); 1 in the 2f row, whatever it is. */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_TRACK_INFO, 8, &alpha, 1, &rows, &n), 0);
    CHECK(n == 7 && pa->file_type == 5 && rows[0].type == 0x2304 && rows[0].id == 5 &&
          rows[5].type == RX_DB_ITEM_INFO_2F && rows[5].id == 1);
    rx_dbrows_free(&rows, n);

    /* Artwork: by the track's id in the row, both sizes; none for bravo,
     * in rekordbox's no-art shape; an album's by its row's id. */
    const RXPlanArtwork *art = NULL;
    for (int i = 0; i < plan->nb_artworks; i++)
        if (plan->artworks[i].id == pa->artwork_id)
            art = &plan->artworks[i];
    CHECK(art && pa->artwork_id && !pb->artwork_id);
    uint8_t *jpeg;
    size_t jn;
    CHECK_EQ(rx_dbclient_artwork(c, alpha, true, &jpeg, &jn), 0);
    CHECK(art && jpeg && jn == art->size240 && jpeg[0] == 0xff && jpeg[1] == 0xd8);
    free(jpeg);
    CHECK_EQ(rx_dbclient_artwork(c, alpha, false, &jpeg, &jn), 0);
    CHECK(art && jn == art->size80);
    free(jpeg);
    CHECK_EQ(rx_dbclient_artwork(c, bravo, true, &jpeg, &jn), -ENOENT);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_ARTWORK, 8, (const uint32_t[]) { bravo, 1 }, 2, &r), 0);
    CHECK(r.kind == RX_DB_ARTWORK_REPLY && r.nb_args == 4 && rx_dbmsg_num(&r, 0) == RX_DB_ARTWORK &&
          rx_dbmsg_num(&r, 1) == 0x32 && rx_dbmsg_num(&r, 2) == 0 && !r.args[3].p);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_ALBUMS, 1, IDS(0), &rows, &n), 0);
    CHECK(n == 1 && rows[0].c == rows[0].id && !strcmp(rows[0].text, "Synthetic"));
    if (n == 1) {
        CHECK_EQ(rx_dbclient_artwork(c, rows[0].c, true, &jpeg, &jn), 0);
        CHECK(art && jn == art->size240);
        free(jpeg);
    }
    uint32_t album = n ? rows[0].id : 0;
    rx_dbrows_free(&rows, n);

    /* The analysis, against the ANLZ files the export would write: from
     * the file's whole probe (its FLAC seek points, PVB2, which Link's
     * plan leaves to the catalog to read). */
    RXWaveforms w;
    CHECK_EQ(rx_library_waveforms_load(lib, alpha, &w), 0);
    RXPlanTrack whole = *pa;
    CHECK_EQ(rx_probe(pa->source, &whole.probe), 0);
    CHECK(whole.probe.flac.valid && !pa->probe.flac.valid);
    RXAnlzFiles files;
    CHECK_EQ(rx_anlz_build(&whole, &w, &files), 0);
    rx_probe_free(&whole.probe);
    RXAnlzTag *ext_tags = NULL, *dat_tags = NULL, *ex2_tags = NULL;
    int next = 0, ndat = 0, nex2 = 0;
    CHECK_EQ(rx_anlz_parse(files.ext, files.ext_size, &ext_tags, &next), 0);
    CHECK_EQ(rx_anlz_parse(files.dat, files.dat_size, &dat_tags, &ndat), 0);
    CHECK_EQ(rx_anlz_parse(files.ex2, files.ex2_size, &ex2_tags, &nex2), 0);

    uint8_t *blob;
    size_t bn;
    CHECK_EQ(rx_dbclient_blob(c, RX_DB_GRID, alpha, &blob, &bn), 0);
    RXBeat *got = NULL, *want = NULL;
    int ng = 0, nw = 0;
    CHECK_EQ(rx_db_grid_parse(blob, bn, &got, &ng), 0);
    CHECK_EQ(rx_anlz_read_beats(files.dat, files.dat_size, &want, &nw), 0);
    CHECK(ng == nw && ng > 10);
    for (int i = 0; i < ng && i < nw; i++)
        if (got[i].number != want[i].number || fabs(got[i].time - want[i].time) > 0.0005 ||
            fabs(got[i].bpm - want[i].bpm) > 0.005) {
            fprintf(stderr, "beat %d differs\n", i);
            failures++;
            break;
        }
    free(got);
    free(want);
    free(blob);

    CHECK_EQ(rx_dbclient_blob(c, RX_DB_PREVIEW, alpha, &blob, &bn), 0);
    CHECK_EQ(bn, 904);
    if (bn == 904) {
        CHECK(!memcmp(blob + 900, (const uint8_t[]) { 0x9e, 0xeb, 0x78, 0x10 }, 4));
        bool ok = true;
        for (int i = 0; i < 400; i++)
            ok &= blob[2 * i] == (w.pwav[i] & 0x1f) && blob[2 * i + 1] == w.pwav[i] >> 5;
        for (int i = 0; i < 100; i++)
            ok &= blob[800 + i] >= 1 && blob[800 + i] <= 15;
        CHECK(ok);
    }
    free(blob);

    CHECK_EQ(rx_dbclient_blob(c, RX_DB_DETAIL, alpha, &blob, &bn), 0);
    CHECK_EQ(bn, 20 + (size_t)w.nb_detail);
    if (bn == 20 + (size_t)w.nb_detail) {
        RXRd rd = rx_rd(blob, bn);
        CHECK(rx_rd_u32le(&rd) == (uint32_t)w.nb_detail && rx_rd_u32le(&rd) == 1 &&
              rx_rd_u32le(&rd) == (uint32_t)w.nb_detail && rx_rd_u32le(&rd) == 150 && rx_rd_u32le(&rd) == 1);
        CHECK(!memcmp(blob + 20, w.pwv3, (size_t)w.nb_detail));
    }
    free(blob);

    static const struct { const char *fourcc, *ext; } tags[] = {
        { "PWV4", "EXT" }, { "PWV5", "EXT" }, { "PQT2", "EXT" }, { "PVB2", "EXT" },
        { "PWV6", "2EX" }, { "PWV7", "2EX" }, { "PWVC", "2EX" }, { "PQTZ", "DAT" }, { "PWAV", "DAT" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tags); i++) {
        CHECK_EQ(rx_dbclient_tag(c, alpha, tags[i].fourcc, tags[i].ext, &blob, &bn), 0);
        RXAnlzTag t;
        CHECK_EQ(rx_db_tag_parse(blob, bn, &t), 0);
        CHECK(!memcmp(t.fourcc, tags[i].fourcc, 4) && bn % 4 == 0);
        const RXAnlzTag *v = !strcmp(tags[i].ext, "EXT") ? ext_tags : !strcmp(tags[i].ext, "DAT") ? dat_tags : ex2_tags;
        int nv = !strcmp(tags[i].ext, "EXT") ? next : !strcmp(tags[i].ext, "DAT") ? ndat : nex2;
        bool found = false;
        for (int k = 0; k < nv; k++)
            if (!memcmp(v[k].fourcc, tags[i].fourcc, 4))
                found = t.raw_size == v[k].raw_size && !memcmp(t.raw, v[k].raw, t.raw_size);
        CHECK(found);
        free(blob);
    }
    CHECK_EQ(rx_dbclient_tag(c, alpha, "PSSI", "EXT", &blob, &bn), -ENOENT);
    CHECK_EQ(rx_dbclient_tag(c, alpha, "PWV4", "XYZ", &blob, &bn), -ENOENT);
    /* The 4f02 reply: kind, 0, length, blob, a trailing 1 (rbxport's capture). */
    CHECK_EQ(rx_dbclient_request(c, RX_DB_TAG, 8, (const uint32_t[]) { alpha, 0x34565750, 0x545845 }, 3, &r), 0);
    CHECK(r.kind == RX_DB_TAG_REPLY && r.nb_args == 5 && rx_dbmsg_num(&r, 2) == 0x1c3c &&
          rx_dbmsg_num(&r, 4) == 1 && r.args[3].len == 0x1c3c);
    CHECK(r.args[3].p && !memcmp(r.args[3].p, (const uint8_t[]) { 0x38, 0x1c, 0, 0, 'P', 'W', 'V', '4' }, 8));
    rx_anlz_files_free(&files);
    free(ext_tags);
    free(dat_tags);
    free(ex2_tags);
    rx_waveforms_free(&w);

    /* VBR: the PVBR words little-endian; an MP3's last is its samples,
     * from its frames, which Link's plan does not read: the catalog does
     * when a player asks. */
    CHECK_EQ(rx_dbclient_blob(c, RX_DB_VBR, bravo, &blob, &bn), 0);
    CHECK_EQ(bn, 1604);
    RXMp3Info mp3;
    CHECK(!pb->probe.mp3.valid && rx_mp3_scan(pb->source, &mp3) >= 0 && mp3.valid);
    if (bn == 1604) {
        uint32_t trailer = (uint32_t)blob[1600] | (uint32_t)blob[1601] << 8 |
                           (uint32_t)blob[1602] << 16 | (uint32_t)blob[1603] << 24;
        CHECK(trailer && trailer == (uint32_t)(mp3.nb_frames * mp3.frame_samples));
    }
    free(blob);

    /* Cues: the extended list read back; the older list without hot cue D. */
    CHECK_EQ(rx_dbclient_blob(c, RX_DB_CUES_EXT, alpha, &blob, &bn), 0);
    RXPlanCue *cues = NULL;
    int nc = 0;
    CHECK_EQ(rx_db_cues_ext_parse(blob, bn, &cues, &nc), 0);
    CHECK_EQ(nc, pa->nb_cues);
    CHECK(nc == 3 && cues[0].hot == 0 && cues[0].loop_end > cues[0].time && !strcmp(cues[0].name, "loop"));
    if (nc == 3) {
        CHECK(cues[1].hot < 0 && fabs(cues[1].time - 0.1) < 0.001 && cues[1].loop_end < 0);
        CHECK(cues[2].hot < 0 && fabs(cues[2].loop_end - 3.0) < 0.001 && cues[2].color_code == 7);
        for (int i = 0; i < pa->nb_cues; i++)
            if (pa->cues[i].hot == 0)
                CHECK(cues[0].color_code == pa->cues[i].color_code && cues[0].rgb == pa->cues[i].rgb);
    }
    free(cues);
    free(blob);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_CUES_EXT, 8, (const uint32_t[]) { charlie, 0 }, 2, &r), 0);
    CHECK(r.kind == RX_DB_CUES_EXT_REPLY && rx_dbmsg_num(&r, 1) == 1 && rx_dbmsg_num(&r, 2) == 0 &&
          r.nb_args == 5);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_CUES, 8, (const uint32_t[]) { alpha }, 1, &r), 0);
    CHECK(r.kind == RX_DB_CUES_REPLY && r.nb_args == 9 && rx_dbmsg_num(&r, 2) == 3 * 36 &&
          rx_dbmsg_num(&r, 4) == 0x24 && rx_dbmsg_num(&r, 5) == 1 && rx_dbmsg_num(&r, 6) == 2 &&
          rx_dbmsg_num(&r, 7) == 24);
    if (r.nb_args == 9 && r.args[3].len >= 36) {
        RXRd rd = rx_rd(r.args[3].p, r.args[3].len);
        uint32_t flags = rx_rd_u32le(&rd);
        CHECK_EQ(flags, 0x10101);   /* the hot cue A loop first */
    }
    CHECK_EQ(rx_dbclient_request(c, RX_DB_CUES, 8, (const uint32_t[]) { bravo }, 1, &r), 0);
    CHECK(rx_dbmsg_num(&r, 2) == 36 && rx_dbmsg_num(&r, 5) == 0 && rx_dbmsg_num(&r, 6) == 1);

    /* After a load: the KUVO user info, then the delivery info. */
    CHECK_EQ(rx_dbclient_request(c, RX_DB_USER_INFO, 3, NULL, 0, &r), 0);
    CHECK(r.kind == RX_DB_USER_INFO_REPLY && rx_dbmsg_num(&r, 0) == RX_DB_USER_INFO &&
          rx_dbmsg_num(&r, 2) == 160 && r.args[3].len == 160);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_DELIVERY, 3, IDS(alpha), &rows, &n), 0);
    CHECK_EQ(n, 13);
    if (n == 13) {
        static const uint32_t types[13] = { 0x36, 7, 0x0f, 0x0b, 4, 0x23, 2, 0x0d, 0x37, 0x0e, 0x12, 6, 0x4f };
        for (int i = 0; i < 13; i++)
            CHECK_EQ(rows[i].type, types[i]);
        CHECK(rows[4].id == alpha && !strcmp(rows[4].text, "Alpha") && rows[12].a == alpha && rows[12].c == 1);
        CHECK_EQ(rows[10].id, 5);   /* FLAC */
    }
    rx_dbrows_free(&rows, n);

    /* Search, the older one and the CDJ-3000's live one. */
    CHECK_EQ(rx_dbclient_search(c, RX_DB_SEARCH, "SYNTH", &rows, &n), 0);
    CHECK(n == 2 && rows[0].id == alpha && rows[1].id == bravo);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_search(c, RX_DB_SEARCH_LIVE, "CHAR", &rows, &n), 0);
    CHECK(n == 1 && rows[0].id == charlie);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_search(c, RX_DB_SEARCH_LIVE, "REMIXER", &rows, &n), 0);
    CHECK(n == 1 && rows[0].id == bravo);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_search(c, RX_DB_SEARCH, "ZZZ", &rows, &n), 0);
    CHECK_EQ(n, 0);

    /* Sorts, keys, history. */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_SORTS, 1, IDS(0, 0), &rows, &n), 0);
    CHECK(n == 7 && rows[0].type == 0xa1 && rows[6].id == 12 && rows[6].type == 0x8b);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_KEYS, 1, IDS(0), &rows, &n), 0);
    CHECK(n == 24 && rows[0].id == 1 && !strcmp(rows[0].text, "Abm") && rows[14].id == 15 &&
          !strcmp(rows[14].text, "Am") && rows[23].type == 0x0f);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_RELATED_KEYS, 2, IDS(0, 15), &rows, &n), 0);
    CHECK(n == 3 && !strcmp(rows[0].text, "Am") && !strcmp(rows[1].text, "Am, C") &&
          !strcmp(rows[2].text, "Am, C, Dm, Em") && rows[2].a == 2 && rows[2].id == 15);
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_KEY_TRACKS, 2, IDS(0, 15, 0), IDS(alpha), "key, distance 0");
    menu_ids(c, RX_DB_KEY_TRACKS, 2, IDS(0, 15, 1), IDS(alpha, charlie), "key, distance 1");
    /* A CDJ-2000's key menu: two levels, the keys (0x100b), then a key's
     * tracks (0x110b), no distances. */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_KEYS_2000, 1, IDS(0), &rows, &n), 0);
    CHECK(n == 24 && !strcmp(rows[14].text, "Am"));
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_KEY_TRACKS_2000, 2, IDS(0, 15), IDS(alpha), "a 2000's key");
    /* Every play: two days, today first; today charlie, then alpha twice
     * (as rekordbox's history keeps each play); charlie's other day. */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_HISTORIES, 1, IDS(0), &rows, &n), 0);
    CHECK(n == 2 && rows[0].type == 0x24 && rx_str_starts(rows[0].text, "HISTORY 20") &&
          rows[0].id > rows[1].id);
    uint32_t day = n ? rows[0].id : 0, before = n == 2 ? rows[1].id : 0;
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_HISTORY_TRACKS, 2, IDS(0, day), &rows, &n), 0);
    CHECK(n == 3 && rows[0].id == charlie && rows[1].id == alpha && rows[2].id == alpha &&
          rows[0].flags == RX_DB_TRACK_PLAYED && rows[2].d == 3);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_HISTORY_TRACKS, 2, IDS(0, before), &rows, &n), 0);
    CHECK(n == 1 && rows[0].id == charlie);
    rx_dbrows_free(&rows, n);

    /* The categories. */
    uint32_t synth = pa->artist_id, other = pc->artist_id, techno = pa->genre_id, house = pb->genre_id;
    menu_ids(c, RX_DB_GENRES, 1, IDS(0), IDS(house, techno), "genres");
    menu_ids(c, RX_DB_GENRE_ARTISTS, 2, IDS(0, techno), IDS(RX_DB_ALL, other, synth), "genre's artists");
    menu_ids(c, RX_DB_GENRE_ARTIST_ALBUMS, 2, IDS(0, techno, synth), IDS(album), "genre artist's albums");
    menu_ids(c, RX_DB_GENRE_TRACKS, 2, IDS(0, techno, RX_DB_ALL, RX_DB_ALL), IDS(alpha, charlie), "genre's tracks");
    menu_ids(c, RX_DB_ARTISTS, 1, IDS(0), IDS(other, synth), "artists");
    menu_ids(c, RX_DB_ARTIST_ALBUMS, 2, IDS(0, synth), IDS(album), "artist's albums");
    menu_ids(c, RX_DB_ARTIST_ALBUMS, 2, IDS(0, other), IDS(0), "artist's unknown album");
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_ARTIST_ALBUM_TRACKS, 2, IDS(0, synth, RX_DB_ALL), &rows, &n), 0);
    CHECK(n == 2 && rows[0].id == alpha && rows[0].flags == RX_DB_TRACK_LISTED);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_ALBUM_TRACKS, 2, IDS(0, album), &rows, &n), 0);
    CHECK(n == 2 && rows[0].id == alpha && rows[0].d == 1 && rows[1].d == 2);
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_BPMS, 1, IDS(0), IDS(12200, 12600, 12800, 14000), "BPMs");
    menu_ids(c, RX_DB_BPM_RANGES, 2, IDS(0, 12600), IDS(0, 1, 2, 3, 4, 5, 6), "BPM ranges");
    menu_ids(c, RX_DB_BPM_TRACKS, 2, IDS(0, 12600, 0), IDS(alpha), "BPM 0 %");
    menu_ids(c, RX_DB_BPM_TRACKS, 2, IDS(0, 12600, 2), IDS(alpha, bravo), "BPM 2 %");
    menu_ids(c, RX_DB_RATINGS, 1, IDS(0), IDS(5, 4, 2, 0), "ratings");
    menu_ids(c, RX_DB_RATING_TRACKS, 2, IDS(0, 4), IDS(alpha), "rating's tracks");
    menu_ids(c, RX_DB_DECADES, 1, IDS(0), IDS(2020, 2010, 1990), "decades");
    menu_ids(c, RX_DB_DECADE_YEARS, 2, IDS(0, 2010), IDS(2019), "decade's years");
    menu_ids(c, RX_DB_YEAR_TRACKS, 2, IDS(0, 2020, RX_DB_ALL), IDS(alpha), "decade's tracks");
    menu_ids(c, RX_DB_LABELS, 1, IDS(0), IDS(pa->label_id), "labels");
    menu_ids(c, RX_DB_LABEL_ARTISTS, 2, IDS(0, pa->label_id), IDS(synth), "label's artists");
    menu_ids(c, RX_DB_LABEL_TRACKS, 2, IDS(0, pa->label_id, RX_DB_ALL, RX_DB_ALL), IDS(alpha, delta),
             "label's tracks");
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_COLORS, 1, IDS(0), &rows, &n), 0);
    CHECK(n == 2 && rows[0].id == 5 && rows[0].type == 0x18 && !strcmp(rows[1].text, "Purple"));
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_COLOR_TRACKS, 2, IDS(0, 8), IDS(charlie), "colour's tracks");
    menu_ids(c, RX_DB_TIMES, 1, IDS(0), IDS(0), "times");
    menu_ids(c, RX_DB_TIME_TRACKS, 2, IDS(0, 0), IDS(alpha, bravo, charlie, delta), "time's tracks");
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_BITRATES, 1, IDS(0), &rows, &n), 0);
    CHECK(n >= 2 && rows[0].id > rows[n - 1].id && rows[0].type == 0x10);
    rx_dbrows_free(&rows, n);
    menu_ids(c, RX_DB_BITRATE_TRACKS, 2, IDS(0, (uint32_t)pb->bit_rate), IDS(bravo), "bitrate's tracks");
    menu_ids(c, RX_DB_ORIGINALS, 1, IDS(0), IDS(pb->original_artist_id), "original artists");
    menu_ids(c, RX_DB_ORIGINAL_ALBUMS, 2, IDS(0, pb->original_artist_id), IDS(album), "original's albums");
    menu_ids(c, RX_DB_ORIGINAL_TRACKS, 2, IDS(0, pb->original_artist_id, RX_DB_ALL), IDS(bravo), "original's tracks");
    menu_ids(c, RX_DB_REMIXERS, 1, IDS(0), IDS(pb->remixer_id), "remixers");
    menu_ids(c, RX_DB_REMIXER_TRACKS, 2, IDS(0, pb->remixer_id, album), IDS(bravo), "remixer's tracks");
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t y = (uint32_t)tm.tm_year + 1900, mo = (uint32_t)tm.tm_mon + 1, d = (uint32_t)tm.tm_mday;
    menu_ids(c, RX_DB_ADDED_YEARS, 1, IDS(0), IDS(y), "added years");
    menu_ids(c, RX_DB_ADDED_MONTHS, 2, IDS(0, y), IDS(RX_DB_ALL, mo), "added months");
    menu_ids(c, RX_DB_ADDED_DAYS, 2, IDS(0, y, mo), IDS(RX_DB_ALL, d), "added days");
    menu_ids(c, RX_DB_ADDED_DAYS, 2, IDS(0, y, RX_DB_ALL), IDS(RX_DB_ALL), "added days, all months");
    menu_ids(c, RX_DB_ADDED_TRACKS, 2, IDS(0, y, mo, RX_DB_ALL), IDS(alpha, bravo, charlie, delta), "added tracks");
    /* FILE NAME: by the file's name, whatever the titles say. */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_FILE_NAMES, 1, IDS(0), &rows, &n), 0);
    CHECK(n == 4 && !strcmp(rows[0].text, rx_path_base(pa->usb_path)));
    for (int i = 1; i < n; i++)
        CHECK(rx_strcasecmp(rows[i - 1].text, rows[i].text) <= 0);
    rx_dbrows_free(&rows, n);

    /* Unknown requests, and writes refused. */
    CHECK_EQ(rx_dbclient_menu(c, 0x1017, 1, IDS(0, alpha), &rows, &n), -ENOENT);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_SAVE_GRID_OFFSET, 8, (const uint32_t[]) { alpha, 5 }, 2, &r), 0);
    CHECK(r.kind == RX_DB_AVAILABLE && rx_dbmsg_num(&r, 0) == RX_DB_SAVE_GRID_OFFSET && rx_dbmsg_num(&r, 1) == 1);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_SAVE_CUE_EXT, 8, (const uint32_t[]) { alpha, 1, 0 }, 3, &r), 0);
    CHECK(r.kind == RX_DB_CUES_EXT_REPLY && rx_dbmsg_num(&r, 0) == RX_DB_SAVE_CUE_EXT && rx_dbmsg_num(&r, 1) == 0x32);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_SAVE_CUE, 8, (const uint32_t[]) { alpha, 1, 0 }, 3, &r), 0);
    CHECK(r.kind == RX_DB_CUES_REPLY && r.nb_args == 9 && rx_dbmsg_num(&r, 1) == 0x32);
    CHECK_EQ(rx_dbclient_request(c, RX_DB_SAVE_RATING, 8, (const uint32_t[]) { alpha, 5 }, 2, &r), 0);
    CHECK(r.kind == RX_DB_AVAILABLE && rx_dbmsg_num(&r, 1) == 1);
    close_pair(&sv, &c);

    /* A client set up the older way: twelve-argument rows, the plain
     * title row in the metadata with the artwork id, which 2003 then
     * takes; six-argument renders with the sort's column (track_metadata.adoc,
     * "Alternate Track List Sort Orders": none for BPM, the key's name). */
    c = open_pair(&sv, cat, true);
    if (!c)
        return;
    CHECK_EQ(rx_dbclient_metadata(c, alpha, &rows, &n), 0);
    CHECK_EQ(n, 16);
    if (n == 16) {
        static const uint32_t types[16] = { 4, 7, 2, 0x0b, 0x0d, 0x23, 0x0f, 0x0a, 0x18, 6, 0x2e,
                                            0x10, 0x11, 0x0e, 0x28, 0x29 };
        for (int i = 0; i < 16; i++)
            CHECK_EQ(rows[i].type, types[i]);
        CHECK(rows[0].c == pa->artwork_id && !rows[0].text2[0] && !rows[0].f);
        CHECK_EQ(rx_dbclient_artwork(c, rows[0].c, true, &jpeg, &jn), 0);
        CHECK(art && jn == art->size240);
        free(jpeg);
    }
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_TRACKS, 1, IDS(4), &rows, &n), 0);
    CHECK(n == 4 && rows[1].id == alpha && rows[1].type == 0x0d04 && rows[1].a == 12600 &&
          !rows[1].text2[0] && !rows[0].text2[0]);
    rx_dbrows_free(&rows, n);
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_TRACKS, 1, IDS(12), &rows, &n), 0);
    CHECK(n == 4 && rows[1].id == alpha && rows[1].type == 0x0f04 && !strcmp(rows[1].text2, "Am"));
    rx_dbrows_free(&rows, n);
    /* Track info's first row as a CDJ-2000nexus sends it: a title row, the
     * file type its id and nothing else (dysentery's S13 capture). */
    CHECK_EQ(rx_dbclient_menu(c, RX_DB_TRACK_INFO, 8, &alpha, 1, &rows, &n), 0);
    CHECK(n == 7 && rows[0].type == RX_DB_ITEM_TITLE && rows[0].id == 5 && !rows[0].a &&
          !rows[0].e && !rows[0].key && rows[5].id == 1);
    rx_dbrows_free(&rows, n);
    close_pair(&sv, &c);
}

/* The port query and a session over loopback TCP, on ports the system
 * gives. */
typedef struct Listener {
    int        fd;
    uint16_t   port;
    bool       query;          /* answer the port query, else serve a session */
    RXCatalog *const *cat;
    int        result;
    pthread_t  thread;
} Listener;

static void *listen_once(void *opaque)
{
    Listener *l = opaque;
    int fd = accept(l->fd, NULL, NULL);
    if (fd < 0) {
        l->result = -errno;
        return NULL;
    }
    if (l->query) {
        uint8_t b[64];
        size_t n = 0;
        int r = 0;
        while (!r && n < sizeof(b)) {
            ssize_t got = recv(fd, b + n, sizeof(b) - n, 0);
            if (got <= 0)
                break;
            n += (size_t)got;
            r = rx_dbquery_parse(b, n);
        }
        uint8_t rep[2];
        rx_dbquery_reply(4321, rep);
        if (r > 0)
            send(fd, rep, 2, MSG_NOSIGNAL);
        l->result = r;
        close(fd);
        return NULL;
    }
    Server sv = { .fd = fd, .cat = l->cat };
    serve(&sv);
    l->result = sv.result;
    return NULL;
}

static int listener(Listener *l)
{
    l->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(a);
    if (l->fd < 0 || bind(l->fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(l->fd, 1) < 0 ||
        getsockname(l->fd, (struct sockaddr *)&a, &len) < 0)
        return -errno;
    l->port = ntohs(a.sin_port);
    return pthread_create(&l->thread, NULL, listen_once, l) ? -EAGAIN : 0;
}

static void test_tcp(RXCatalog **cat)
{
    Listener q = { .query = true };
    CHECK_EQ(listener(&q), 0);
    uint16_t port = 0;
    CHECK_EQ(rx_dbclient_query_port("127.0.0.1", q.port, 5000, &port), 0);
    CHECK_EQ(port, 4321);
    pthread_join(q.thread, NULL);
    CHECK_EQ(q.result, 19);
    close(q.fd);

    Listener s = { .cat = cat };
    CHECK_EQ(listener(&s), 0);
    RXDbClient *c = NULL;
    CHECK_EQ(rx_dbclient_connect(&c, "127.0.0.1", s.port, 2, 5000), 0);
    RXDbRow *rows;
    int n;
    if (c) {
        CHECK_EQ(rx_dbclient_menu(c, RX_DB_TRACKS, 1, (const uint32_t[]) { 0 }, 1, &rows, &n), 0);
        CHECK_EQ(n, 4);
        rx_dbrows_free(&rows, n);
    }
    rx_dbclient_close(&c);
    pthread_join(s.thread, NULL);
    CHECK_EQ(s.result, -ESHUTDOWN);
    close(s.fd);
}

/* A library change between a menu and its render: the rows come from the
 * catalog that is current, those gone left out. */
static void test_swap(RXLibrary *lib, RXPlan *plan, RXCatalog **cat)
{
    RXDbSession *s;
    CHECK_EQ(rx_dbsession_new(&s, cat, 17), 0);
    RXWr req = rx_wr_dyn(), out = rx_wr_dyn();
    rx_wr_bytes(&req, rx_db_greeting, 5);
    put_msg(&req, 1, RX_DB_TRACKS, (const uint32_t[]) { 0x02010301, 0 }, 2);
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    RXCatalog *old = *cat, *fresh = NULL;
    RXPlan empty = { 0 };
    CHECK_EQ(rx_catalog_new(&fresh, lib, &empty), 0);
    *cat = fresh;
    rx_catalog_free(&old);
    rx_wr_free(&req);
    req = rx_wr_dyn();
    put_msg(&req, 2, RX_DB_RENDER, (const uint32_t[]) { 0x02010301, 0, 25, 0, 25, 12, 1, 0 }, 8);
    rx_wr_free(&out);
    out = rx_wr_dyn();
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    Out o = { .p = out.p, .len = out.len };
    RXDbMsg r;
    CHECK(next_msg(&o, &r) && r.kind == RX_DB_MENU_HEADER);
    CHECK(next_msg(&o, &r) && r.kind == RX_DB_MENU_FOOTER);
    /* And back: the same menu renders whole again. */
    rx_catalog_free(cat);
    CHECK_EQ(rx_catalog_new(cat, lib, plan), 0);
    rx_wr_free(&out);
    out = rx_wr_dyn();
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    o = (Out) { .p = out.p, .len = out.len };
    int items = 0;
    while (next_msg(&o, &r))
        items += r.kind == RX_DB_MENU_ITEM;
    CHECK_EQ(items, 4);
    rx_wr_free(&req);
    rx_wr_free(&out);
    rx_dbsession_free(&s);
}

/* ------------------------------------------------------------ 10 000 */

static double ms_since(int64_t t0)
{
    return (double)(rx_time_us() - t0) / 1000.0;
}

static void test_large(void)
{
    enum { N = 10000 };
    Mem m = { .nb = N };
    m.tracks = calloc(N, sizeof(*m.tracks));
    m.plan.tracks = calloc(N, sizeof(*m.plan.tracks));
    m.plan.artists = mem_names(1000, "Artist %d");
    m.plan.albums = mem_names(2000, "Album %d");
    m.plan.genres = mem_names(20, "Genre %d");
    m.plan.nb_artists = 1000;
    m.plan.nb_albums = 2000;
    m.plan.nb_genres = 20;
    CHECK(m.tracks && m.plan.tracks && m.plan.artists && m.plan.albums && m.plan.genres);
    if (!m.tracks || !m.plan.tracks) {
        mem_free(&m);
        return;
    }
    static const char *const keys[] = { "Am", "C", "F#m", "Eb", "G", "Bbm" };
    for (int i = 0; i < N; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/music/%05d.flac", i);
        RXTrack *t = m.tracks[i] = rx_track_new(path);
        CHECK(t);
        if (!t)
            break;
        char title[64];
        snprintf(title, sizeof(title), "Track %d of the set", (i * 7919) % N);
        rx_meta_set(&t->meta.title, title);
        t->meta.year = 1990 + i % 35;
        t->meta.rating = i % 6;
        t->added = 1700000000 + (int64_t)i * 3600;
        snprintf(t->key, sizeof(t->key), "%s", keys[i % 6]);
        RXPlanTrack *pt = &m.plan.tracks[i];
        *pt = (RXPlanTrack) {
            .id        = (uint32_t)i + 1,
            .track     = t,
            .usb_path  = rx_asprintf("/Contents/A/B/%05d.flac", i),
            .duration  = 180 + i % 300,
            .artist_id = (uint32_t)(i % 1000) + 1,
            .album_id  = (uint32_t)(i % 2000) + 1,
            .genre_id  = (uint32_t)(i % 20) + 1,
            .bit_rate  = 1411,
        };
        snprintf(pt->key_name, sizeof(pt->key_name), "%s", t->key);
        rx_grid_init(&pt->grid, 0.05, 110 + i % 40);
    }
    m.plan.nb_tracks = N;

    int64_t t0 = rx_time_us();
    RXCatalog *cat = NULL;
    CHECK_EQ(rx_catalog_new(&cat, NULL, &m.plan), 0);
    double build = ms_since(t0);
    fprintf(stderr, "  10000 tracks: catalog in %.1f ms", build);
    CHECK(build < 100.0);

    /* The first all-tracks menu sorts by title; then artist; a search. */
    RXDbSession *s;
    CHECK_EQ(rx_dbsession_new(&s, &cat, 17), 0);
    RXWr req = rx_wr_dyn(), out = rx_wr_dyn();
    rx_wr_bytes(&req, rx_db_greeting, 5);
    put_msg(&req, 1, RX_DB_TRACKS, (const uint32_t[]) { 0x02010301, 0 }, 2);
    put_msg(&req, 2, RX_DB_RENDER, (const uint32_t[]) { 0x02010301, 9990, 64, 0, 64, 12, 1, 0 }, 8);
    put_msg(&req, 3, RX_DB_TRACKS, (const uint32_t[]) { 0x02010301, 2 }, 2);
    put_msg(&req, 4, RX_DB_ARTISTS, (const uint32_t[]) { 0x02010301, 0 }, 2);
    put_msg(&req, 5, RX_DB_KEY_TRACKS, (const uint32_t[]) { 0x02010301, 0, 15, 2 }, 4);
    t0 = rx_time_us();
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    double browse = ms_since(t0);
    fprintf(stderr, ", the first menus in %.1f ms", browse);
    rx_wr_free(&req);
    req = rx_wr_dyn();
    rx_wr_free(&out);
    out = rx_wr_dyn();
    RXDbMsg q = { .txid = 6, .kind = RX_DB_SEARCH_LIVE, .nb_args = 5 };
    q.args[0] = (RXDbArg) { .type = RX_DB_NUM, .num = 0x02010301 };
    q.args[1] = (RXDbArg) { .type = RX_DB_NUM, .num = 0 };
    q.args[2] = (RXDbArg) { .type = RX_DB_NUM, .num = 8 };
    q.args[3] = (RXDbArg) { .type = RX_DB_STR, .str = "SET" };
    q.args[4] = (RXDbArg) { .type = RX_DB_NUM, .num = 0 };
    rx_dbmsg_write(&req, &q);
    put_msg(&req, 7, RX_DB_TRACKS, (const uint32_t[]) { 0x02010301, 0 }, 2);
    t0 = rx_time_us();
    CHECK_EQ(rx_dbsession_input(s, req.p, req.len, &out), 0);
    double again = ms_since(t0);
    fprintf(stderr, ", a search of all and the list again in %.1f ms\n", again);
    CHECK(browse < 1000.0 && again < 500.0);
    Out o = { .p = out.p, .len = out.len };
    RXDbMsg r;
    CHECK(next_msg(&o, &r) && rx_dbmsg_num(&r, 1) == N);
    rx_wr_free(&req);
    rx_wr_free(&out);
    rx_dbsession_free(&s);
    rx_catalog_free(&cat);
    mem_free(&m);
}

/* ------------------------------------------------------- the stored plan */

static void same_track(const RXPlanTrack *a, const RXPlanTrack *b)
{
    CHECK_STR(a->source, b->source);
    CHECK_STR(a->usb_path, b->usb_path);
    CHECK_EQ(a->file_type, b->file_type);
    CHECK_EQ(a->file_size, b->file_size);
    CHECK_EQ(a->sample_rate, b->sample_rate);
    CHECK_EQ(a->bit_rate, b->bit_rate);
    CHECK_EQ(a->bit_depth, b->bit_depth);
    CHECK_EQ(a->total_samples, b->total_samples);
    CHECK(fabs(a->duration - b->duration) < 1e-9);
    CHECK_EQ(!a->artwork_id, !b->artwork_id);
}

static void same_plans(RXLibrary *lib)
{
    RXPlan dry, stored;
    CHECK_EQ(rx_plan_build_cb(lib, lib->tracks, lib->nb_tracks, NULL, 0, NULL, false, RX_PLAN_DRY,
                              &dry, NULL, NULL), 0);
    CHECK_EQ(rx_plan_build_cb(lib, lib->tracks, lib->nb_tracks, NULL, 0, NULL, false,
                              RX_PLAN_STORED, &stored, NULL, NULL), 0);
    CHECK_EQ(dry.nb_tracks, stored.nb_tracks);
    for (int i = 0; i < dry.nb_tracks && i < stored.nb_tracks; i++)
        same_track(&dry.tracks[i], &stored.tracks[i]);
    rx_plan_free(&dry);
    rx_plan_free(&stored);
}

/* The colour at the centre of an artwork's 240 px JPEG: 'r' or 'b'. */
static char cover_colour(const RXPlan *p, uint32_t track_id)
{
    const RXPlanTrack *pt = plan_track(p, track_id);
    const RXPlanArtwork *a = NULL;
    for (int i = 0; pt && i < p->nb_artworks; i++)
        if (p->artworks[i].id == pt->artwork_id)
            a = &p->artworks[i];
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (!a || rx_image_decode(a->jpeg240, a->size240, &rgba, &w, &h) < 0)
        return 0;
    const uint8_t *px = rgba + ((size_t)(h / 2) * w + w / 2) * 4;
    char c = px[0] > px[2] ? 'r' : 'b';
    free(rgba);
    return c;
}

/* Link's plan reads no audio (RX_PLAN_STORED): each track described as
 * the export's dry run, which probes every file, describes it (a
 * transcode not made yet, charlie's Opus, included); a cover from the
 * library's thumbnail when it has one, else from the file (the folder's
 * orange front.jpg); a file changed since the library probed it probed
 * again. */
static void test_stored(RXLibrary *lib)
{
    same_plans(lib);

    RXTrack *alpha = by_file(lib, "alpha.flac"), *delta = by_file(lib, "delta.wav");
    uint8_t blue[16 * 16 * 4];
    for (size_t i = 0; i < sizeof(blue); i += 4) {
        blue[i] = 0x10;
        blue[i + 1] = 0x20;
        blue[i + 2] = 0xF0;
        blue[i + 3] = 0xFF;
    }
    uint8_t *jpeg = NULL;
    size_t size = 0;
    CHECK_EQ(rx_image_jpeg_square(blue, 16, 16, 240, 90, &jpeg, &size), 0);
    CHECK_EQ(rx_library_artwork_save(lib, alpha->id, jpeg, size, 240, 240), 0);
    free(jpeg);
    RXPlan p;
    CHECK_EQ(rx_plan_build_cb(lib, lib->tracks, lib->nb_tracks, NULL, 0, NULL, false,
                              RX_PLAN_STORED, &p, NULL, NULL), 0);
    CHECK_EQ(cover_colour(&p, (uint32_t)alpha->id), 'b');
    CHECK_EQ(cover_colour(&p, (uint32_t)delta->id), 'r');
    rx_plan_free(&p);

    struct timespec later[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = delta->mtime + 60 } };
    CHECK_EQ(utimensat(AT_FDCWD, delta->path, later, 0), 0);
    same_plans(lib);
}

/* Link's plan (RX_TRANSCODE_LINK): every track a 16-bit stereo WAV made
 * as it is read, named .wav, its file the track's own and nothing made.
 * When the check at track info refuses one (here charlie), the WAV is made
 * as a file then and served instead, at its own size; the rest make none. */
static int refuse_charlie(void *opaque, const RXPlanTrack *pt)
{
    return rx_str_ends(pt->source, "charlie.opus") ? RX_ERROR(ENOTSUP) : 0;
}

static void test_link_plan(RXLibrary *lib)
{
    RXPlan p;
    CHECK_EQ(rx_plan_build_cb(lib, lib->tracks, lib->nb_tracks, NULL, 0, NULL, RX_TRANSCODE_LINK,
                              RX_PLAN_STORED, &p, NULL, NULL), 0);
    for (int i = 0; i < p.nb_tracks; i++) {
        const RXPlanTrack *t = &p.tracks[i];
        CHECK(t->pcm && rx_str_ends(t->usb_path, ".wav") && t->file_type == 11 &&
              !strcmp(t->source, t->track->path) && t->total_samples == t->track->total_samples &&
              t->file_size == 44 + 4 * t->total_samples && t->bit_rate == t->sample_rate * 32 / 1000);
    }
    RXCatalog *cat = NULL;
    CHECK_EQ(rx_catalog_new(&cat, lib, &p), 0);
    if (cat) {
        rx_catalog_set_check(cat, refuse_charlie, NULL);
        Server sv;
        RXDbClient *c = open_pair(&sv, &cat, true);
        uint32_t alpha = (uint32_t)by_file(lib, "alpha.flac")->id;
        uint32_t charlie = (uint32_t)by_file(lib, "charlie.opus")->id;
        const RXPlanTrack *pa = plan_track(&p, alpha), *pc = plan_track(&p, charlie);
        char *path = NULL;
        uint64_t size = 0;
        CHECK(c && rx_dbclient_track_info(c, alpha, &path, &size) == 0);
        CHECK(path && !strcmp(path, pa->usb_path) && pa->pcm && (int64_t)size == pa->file_size);
        free(path);
        path = NULL;
        CHECK(c && rx_dbclient_track_info(c, charlie, &path, &size) == 0);
        CHECK(path && !strcmp(path, pc->usb_path) && !pc->pcm &&
              rx_str_ends(pc->source, ".link.wav") && rx_file_exists(pc->source) &&
              (int64_t)size == rx_file_size(pc->source) && pc->file_size == (int64_t)size);
        free(path);
        if (c)
            close_pair(&sv, &c);
        char *made = rx_asprintf("%s/cache/transcode/%u.link.wav", libdir, alpha);
        CHECK(made && !rx_file_exists(made));
        free(made);
    }
    rx_catalog_free(&cat);
    rx_plan_free(&p);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    test_codec();
    test_latin1();
    test_capture();
    test_large();

    const char *base = getenv("RX_TEST_DIR");
    snprintf(root, sizeof(root), "%s", base ? base : "test-link-db");
    snprintf(music, sizeof(music), "%s/music", root);
    snprintf(libdir, sizeof(libdir), "%s/library", root);
    sh("rm -rf '%s'", root);
    char sub[800];
    snprintf(sub, sizeof(sub), "%s/nocover", music);
    rx_mkdir_p(sub);
    rx_mkdir_p(libdir);

    RXLibrary lib = { 0 };
    int ret = make_library(&lib);
    if (ret == 77) {
        fprintf(stderr, "test_db: no ffmpeg CLI with libmp3lame and libopus: the library part skipped\n");
        rx_library_close(&lib);
        sh("rm -rf '%s'", root);
        return failures ? test_result("test_db") : 77;
    }
    test_stored(&lib);
    test_link_plan(&lib);
    RXPlan plan;
    CHECK_EQ(rx_plan_build_cb(&lib, lib.tracks, lib.nb_tracks, NULL, 0, NULL, false,
                              RX_PLAN_STORED, &plan, NULL, NULL), 0);
    RXCatalog *cat = NULL;
    CHECK_EQ(rx_catalog_new(&cat, &lib, &plan), 0);
    if (!ret && cat) {
        test_streams(&cat, (uint32_t)by_file(&lib, "alpha.flac")->id);
        test_library(&lib, &plan, &cat);
        test_tcp(&cat);
        test_swap(&lib, &plan, &cat);
    }
    rx_catalog_free(&cat);
    rx_plan_free(&plan);
    rx_library_close(&lib);
    if (!failures)
        sh("rm -rf '%s'", root);
    return test_result("test_db");
}
