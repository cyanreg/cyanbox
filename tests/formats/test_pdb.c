/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* export.pdb and exportExt.pdb: dj's tests (fixed tables byte-identical to
 * rekordbox 7.2.18, a 400-track round trip through the reader and the
 * structural check, DeviceSQL strings) plus vectors from docs/pdb.md:
 * baken's "Rødhåd" artist row taken from a rekordbox 7 export, the history
 * row, and the whole exportExt.pdb tag page of a rekordbox export
 * (Appendix B, from rekordcrate's complete_export/empty). */

#include <stdio.h>

#include "rxbox/formats.h"

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "tests/formats/test.h"

static void print_problem(void *opaque, const char *problem)
{
    fprintf(stderr, "  %s: %s\n", (const char *)opaque, problem);
}

static void strings(void)
{
    char x126[127], x127[128];
    memset(x126, 'x', 126);
    x126[126] = '\0';
    memset(x127, 'x', 127);
    x127[127] = '\0';
    const char *all[] = { "", "a", x126, x127, "Rødhåd", "日本語", "🅴" };
    const bool lng[] = { false, false, false, true, true, true, true };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(all); i++) {
        uint8_t *enc;
        size_t n;
        char *dec = NULL;
        CHECK_EQ(rx_pdb_string_encode(all[i], &enc, &n), 0);
        CHECK_EQ(!(enc[0] & 1), lng[i]);
        CHECK_EQ(rx_pdb_string_decode(enc, n, &dec), 0);
        CHECK_STR(dec, all[i]);
        free(dec);
        free(enc);
    }
    /* Exact forms (docs/pdb.md 2.9): empty is 0x03, long ASCII 0x40 with
     * len + 4, UTF-16 0x90 without a terminator. */
    uint8_t *enc;
    size_t n;
    rx_pdb_string_encode("", &enc, &n);
    CHECK(n == 1 && enc[0] == 0x03);
    free(enc);
    rx_pdb_string_encode(x127, &enc, &n);
    CHECK(n == 131 && enc[0] == 0x40 && enc[1] == 131 && enc[2] == 0 && enc[3] == 0);
    free(enc);
    rx_pdb_string_encode("🅴", &enc, &n);
    static const uint8_t emoji[] = { 0x90, 0x08, 0x00, 0x00, 0x3C, 0xD8, 0x74, 0xDD };
    CHECK(n == sizeof(emoji) && !memcmp(enc, emoji, n));
    free(enc);
}

static int table_rows(const uint8_t *b, size_t size, int type, uint32_t **pos)
{
    RXPdbHeader h;
    int n = 0;
    *pos = NULL;
    if (rx_pdb_read_header(b, size, &h) < 0 || rx_pdb_table_rows(b, size, &h.tables[type], pos, &n) < 0)
        return 0;
    return n;
}

/* dj's test_pdb_fixed_tables_match_rekordbox: the colour, column, menu and
 * sort pages of an export equal rekordbox 7.2.18's from the row counts on
 * (the first 0x18 bytes hold page numbers and sequences). */
static void fixed_tables(void)
{
    uint8_t *ref;
    size_t ref_size;
    if (rx_file_read("tests/data/rekordbox-7.2.18-export.pdb", &ref, &ref_size) < 0) {
        fprintf(stderr, "cannot read the rekordbox fixture\n");
        failures++;
        return;
    }
    CHECK_EQ(rx_pdb_check(ref, ref_size, print_problem, "rekordbox"), 0);

    RXPlan plan = { .hot_cue_auto_load = true };
    snprintf(plan.date, sizeof(plan.date), "2026-10-13");
    uint8_t *ours;
    size_t size;
    CHECK_EQ(rx_pdb_build(&plan, &ours, &size), 0);
    CHECK_EQ(rx_pdb_check(ours, size, print_problem, "ours"), 0);
    const int types[] = { RX_PDB_COLORS, RX_PDB_COLUMNS, RX_PDB_MENU, RX_PDB_SORT };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(types); i++) {
        uint32_t *po, *pr;
        int no = table_rows(ours, size, types[i], &po);
        int nr = table_rows(ref, ref_size, types[i], &pr);
        CHECK(no > 0 && no == nr);
        if (no > 0 && no == nr) {
            size_t a = po[0] / RX_PDB_PAGE * RX_PDB_PAGE, b = pr[0] / RX_PDB_PAGE * RX_PDB_PAGE;
            if (memcmp(ours + a + 0x18, ref + b + 0x18, RX_PDB_PAGE - 0x18)) {
                fprintf(stderr, "table %d differs from rekordbox 7.2.18\n", types[i]);
                failures++;
            }
        }
        free(po);
        free(pr);
    }
    free(ours);
    free(ref);
}

/* dj's test_pdb_roundtrip: 400 tracks with short, long ASCII and UTF-16
 * titles and comments of every length up to 199, 299 artists. */
static void roundtrip(void)
{
    enum { NT = 400, NA = 299 };
    RXTrack *tr = calloc(NT, sizeof(*tr));
    RXPlanTrack *pt = calloc(NT, sizeof(*pt));
    RXPlanName *artists = calloc(NA, sizeof(*artists));
    char (*titles)[200] = calloc(NT, 200);
    char (*comments)[200] = calloc(NT, 200);
    char (*paths)[64] = calloc(NT, 64);
    if (!tr || !pt || !artists || !titles || !comments || !paths) {
        failures++;
        return;
    }
    for (int i = 1; i <= NT; i++) {
        char *t = titles[i - 1];
        int o = snprintf(t, 200, "Track %d%s", i, i % 3 == 0 ? "ü" : "");
        if (i % 7 == 0) {
            memset(t + o, 'x', 150);
            t[o + 150] = '\0';
        }
        memset(comments[i - 1], 'c', (size_t)(i % 200));
        snprintf(paths[i - 1], 64, "/Contents/A/B/%d.flac", i);
        tr[i - 1].meta.title = t;
        tr[i - 1].meta.comment = comments[i - 1];
        pt[i - 1] = (RXPlanTrack) {
            .id          = (uint32_t)i,
            .track       = &tr[i - 1],
            .usb_path    = paths[i - 1],
            .anlz_dir    = "/PIONEER/USBANLZ/P000/00000000",
            .file_type   = 5,
            .file_size   = 1,
            .sample_rate = 44100,
            .bit_depth   = 16,
            .bit_rate    = 1411,
            .duration    = 300,
            .artist_id   = (uint32_t)(i % NA + 1),
        };
    }
    for (int i = 1; i <= NA; i++)
        artists[i - 1] = (RXPlanName) { .id = (uint32_t)i, .name = i % 2 ? "Rødhåd" : "Artist" };
    RXPlan plan = {
        .tracks            = pt,
        .nb_tracks         = NT,
        .artists           = artists,
        .nb_artists        = NA,
        .hot_cue_auto_load = true,
    };
    snprintf(plan.date, sizeof(plan.date), "2026-10-06");

    uint8_t *b;
    size_t size;
    CHECK_EQ(rx_pdb_build(&plan, &b, &size), 0);
    CHECK_EQ(rx_pdb_check(b, size, print_problem, "ours"), 0);

    RXPdb pdb;
    CHECK_EQ(rx_pdb_read(b, size, &pdb), 0);
    CHECK_EQ(pdb.nb_tracks, NT);
    for (int i = 0; i < pdb.nb_tracks && i < NT; i++) {
        CHECK_STR(pdb.tracks[i].title, titles[i]);
        CHECK_STR(pdb.tracks[i].path, paths[i]);
        CHECK_STR(pdb.tracks[i].artist, (i + 1) % NA % 2 == 0 ? "Rødhåd" : "Artist");
        CHECK_EQ(pdb.tracks[i].id, i + 1);
    }
    rx_pdb_free(&pdb);

    RXPdbRawTrack *raw;
    int nraw;
    CHECK_EQ(rx_pdb_raw_tracks(b, size, &raw, &nraw), 0);
    CHECK_EQ(nraw, NT);
    for (int i = 0; i < nraw; i++) {
        CHECK_EQ(raw[i].subtype, 0x24);
        CHECK_EQ(raw[i].unk56, 0x29);
        CHECK_EQ(raw[i].magic, 3);
        CHECK_EQ(raw[i].bitmask, 0x000C0700);
        CHECK_STR(raw[i].strings[RX_PDB_STR_COMMENT], comments[i]);
        CHECK_STR(raw[i].strings[RX_PDB_STR_AUTOLOAD_HOTCUES], "ON");
        CHECK_STR(raw[i].strings[RX_PDB_STR_ANALYZE_DATE], "2026-10-06");
        CHECK_STR(raw[i].strings[RX_PDB_STR_FILENAME], rx_path_base(paths[i]));
    }
    rx_pdb_raw_tracks_free(raw, nraw);

    for (int i = 0; i < NT; i++) {
        size_t len, alloc;
        CHECK_EQ(rx_pdb_track_row_size(&pt[i], &plan, &len, &alloc), 0);
        CHECK(len <= alloc && alloc >= 224);
        CHECK_EQ(rx_pdb_track_row_overflow(&pt[i], &plan), 0);
    }

    /* The history row (docs/pdb.md 2.11): 400 tracks, the date, 19 1E,
     * "1000", the empty label; 40 bytes allocated. */
    uint32_t num;
    char *date = NULL;
    CHECK_EQ(rx_pdb_history(b, size, &num, &date), 0);
    CHECK_EQ(num, NT);
    CHECK_STR(date, "2026-10-06");
    free(date);
    uint32_t *pos;
    int n = table_rows(b, size, RX_PDB_HISTORY, &pos);
    static const uint8_t hist[40] = {
        0x80, 0x02, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x17, '2', '0', '2', '6', '-', '1', '0', '-', '0', '6', 0x19, 0x1E,
        0x0B, '1', '0', '0', '0', 0x03,
    };
    CHECK(n == 1 && !memcmp(b + pos[0], hist, sizeof(hist)));
    CHECK(n == 1 && rx_rd_le16(b + pos[0] / RX_PDB_PAGE * RX_PDB_PAGE + 0x1E) == 40);
    free(pos);
    free(b);

    /* Text past one page is caught before writing (the export refuses such
     * a track by name) and the builder refuses it too. */
    char *big = malloc(5000);
    memset(big, 'y', 4999);
    big[4999] = '\0';
    tr[0].meta.comment = big;
    CHECK(rx_pdb_track_row_overflow(&pt[0], &plan) > 0);
    plan.nb_tracks = 1;
    CHECK(rx_pdb_build(&plan, &b, &size) < 0);
    free(big);

    free(tr);
    free(pt);
    free(artists);
    free(titles);
    free(comments);
    free(paths);
}

/* baken's vector from a rekordbox 7 export: "Rødhåd", slot 10, id 11, the
 * long name at 0x0C after two pad bytes, 32 bytes allocated. */
static void artist_row(void)
{
    RXPlanName a[12];
    for (int i = 0; i < 12; i++)
        a[i] = (RXPlanName) { .id = (uint32_t)i + 1, .name = i == 10 ? "Rødhåd" : "A" };
    RXPlan plan = { .artists = a, .nb_artists = 12 };
    uint8_t *b;
    size_t size;
    CHECK_EQ(rx_pdb_build(&plan, &b, &size), 0);
    uint32_t *pos;
    int n = table_rows(b, size, RX_PDB_ARTISTS, &pos);
    static const uint8_t want[32] = {
        0x60, 0x00, 0x40, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x03, 0x0c, 0x00, 0x00,
        0x90, 0x10, 0x00, 0x00, 0x52, 0x00, 0xf8, 0x00, 0x64, 0x00, 0x68, 0x00,
        0xe5, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK(n == 12 && !memcmp(b + pos[10], want, sizeof(want)));
    CHECK(n == 12 && pos[11] - pos[10] == 32);
    CHECK_EQ(rx_pdb_check(b, size, print_problem, "ours"), 0);
    free(pos);
    free(b);
}

/* rekordbox's default My Tag page (docs/pdb.md Appendix B): every row at
 * its heap offset and the page header. */
static const struct { int at; const char *hex; } ext_rows[] = {
    { 0, "80060000000000000000000000000000000000000100000000000001031f250d47656e72650300000000000000000000" },
    { 48, "8006200000000000000000000100000000000000c26ee78100000000031f2a174163696420486f75736503000000000000000000" },
    { 100, "80064000000000000000000001000000010000007a8f9e1500000000031f2a174465657020486f75736503000000000000000000" },
    { 152, "80066000000000000000000001000000020000008468d8fd00000000031f260f546563686e6f03000000000000000000" },
    { 200, "800680000000000000000000010000000300000016a268bf00000000031f28134e7520446973636f030000000000000000000000" },
    { 252, "8006a00000000000000000000100000004000000d8a9846d00000000031f2d1d456c656374726f20486f7573650300000000000000000000" },
    { 308, "8006c00000000000000000000100000005000000a005a58900000000031f2a1742617373204d7573696303000000000000000000" },
    { 360, "8006e0000000000000000000010000000600000034f8f58500000000031f240b54726170030000000000000000000000" },
    { 408, "80060001000000000000000000000000010000000200000000000001031f2a17436f6d706f6e656e747303000000000000000000" },
    { 460, "80062001000000000000000002000000000000003ae610f100000000031f250d53796e74680300000000000000000000" },
    { 508, "8006400100000000000000000200000001000000b8940b0a00000000031f250d566f63616c0300000000000000000000" },
    { 556, "80066001000000000000000002000000020000002e25232700000000031f240b42656174030000000000000000000000" },
    { 604, "800680010000000000000000020000000300000044e7023a00000000031f28135375622042617373030000000000000000000000" },
    { 656, "8006a00100000000000000000200000004000000fd0b382b00000000031f2a1750657263757373696f6e03000000000000000000" },
    { 708, "8006c001000000000000000002000000050000006b204aa300000000031f250d5069616e6f0300000000000000000000" },
    { 756, "8006e0010000000000000000020000000600000024225b9900000000031f240b4461726b030000000000000000000000" },
    { 804, "8006000200000000000000000200000007000000e1f2d2d200000000031f250d55707065720300000000000000000000" },
    { 852, "80062002000000000000000000000000020000000300000000000001031f2915536974756174696f6e0300000000000000000000" },
    { 904, "80064002000000000000000003000000000000003d2c7fce00000000031f2a174d61696e20466c6f6f7203000000000000000000" },
    { 956, "80066002000000000000000003000000010000007ff092e000000000031f2c1b5365636f6e6420466c6f6f72030000000000000000000000" },
    { 1012, "80068002000000000000000003000000020000005b80337300000000031f260f4c6f756e676503000000000000000000" },
    { 1060, "8006a00200000000000000000300000003000000033ed68100000000031f29154d6964204e696768740300000000000000000000" },
    { 1112, "8006c002000000000000000003000000040000009d0a812400000000031f27114d6f726e696e67030000000000000000" },
    { 1160, "8006e0020000000000000000030000000500000091d208be00000000031f28134275696c64207570030000000000000000000000" },
    { 1212, "8006000300000000000000000300000006000000aaafee6700000000031f29155065616b2054696d650300000000000000000000" },
    { 1264, "80062003000000000000000003000000070000009d6f0e0000000000031f2a174275696c6420646f776e03000000000000000000" },
    { 1316, "80064003000000000000000000000000030000000400000000000001031f2f21556e7469746c656420436f6c756d6e030000000000000000" },
    { 1372, "80066003000000000000000004000000000000006ca7d42900000000031f2a174d7920436f6d6d656e7403000000000000000000" },
};

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

static void ext_tags(void)
{
    enum { N = RX_ARRAY_ELEMS(ext_rows) };
    RXPlanTag tags[N];
    char names[N][64];
    uint8_t rows[N][64];
    size_t lens[N];
    for (int i = 0; i < N; i++) {
        lens[i] = strlen(ext_rows[i].hex) / 2;
        for (size_t k = 0; k < lens[i]; k++)
            rows[i][k] = (uint8_t)(hexval(ext_rows[i].hex[2 * k]) << 4 | hexval(ext_rows[i].hex[2 * k + 1]));
        uint8_t *r = rows[i];
        size_t nl = (size_t)(r[0x1F] >> 1) - 1;
        memcpy(names[i], r + 0x20, nl);
        names[i][nl] = '\0';
        tags[i] = (RXPlanTag) {
            .id          = rx_rd_le32(r + 0x14),
            .name        = names[i],
            .category    = (int)rx_rd_le32(r + 0x0C),
            .position    = (int)rx_rd_le32(r + 0x10),
            .is_category = rx_rd_le32(r + 0x18) == 0x01000000,
        };
    }
    RXPlan plan = { .tags = tags, .nb_tags = N };
    uint8_t *b;
    size_t size;
    CHECK_EQ(rx_pdb_ext_build(&plan, &b, &size), 0);
    CHECK_EQ(rx_pdb_check(b, size, print_problem, "ours"), 0);
    RXPdbHeader h;
    CHECK_EQ(rx_pdb_read_header(b, size, &h), 0);
    CHECK_EQ(h.num_tables, 9);
    CHECK_EQ(h.unknown, 5);
    uint32_t *pos;
    int n = table_rows(b, size, RX_PDB_EXT_TAGS, &pos);
    CHECK_EQ(n, N);
    for (int i = 0; i < n && i < N; i++) {
        size_t page = pos[i] / RX_PDB_PAGE * RX_PDB_PAGE;
        CHECK_EQ(pos[i] - page - RX_PDB_HEAP, ext_rows[i].at);
        if (memcmp(b + pos[i], rows[i], lens[i])) {
            fprintf(stderr, "tag row %d differs from rekordbox's\n", i);
            failures++;
        }
    }
    static const uint8_t head[16] = {
        0x1c, 0x80, 0x03, 0x24, 0x08, 0x0a, 0x90, 0x05, 0x1c, 0x00, 0, 0, 0, 0, 0, 0,
    };
    CHECK(n > 0 && !memcmp(b + pos[0] / RX_PDB_PAGE * RX_PDB_PAGE + 0x18, head, sizeof(head)));
    free(pos);
    /* Table 7's row: library constant + 1 at 0x18 (docs/pdb.md 3). */
    n = table_rows(b, size, RX_PDB_EXT_T7, &pos);
    CHECK(n == 1 && b[pos[0] + 1] == 0x07 && rx_rd_le32(b + pos[0] + 0x18) == 1);
    free(pos);
    free(b);
}

/* Text the builder cannot place: refused with an error the export can
 * report (never an assertion), whichever row holds it; a string past the
 * 16-bit DeviceSQL length overflows by definition. Astral characters in
 * every kind of row keep long strings 4-aligned (docs/pdb.md G1), and a
 * title cut to the plan's buffer stays valid UTF-8. */
static void limits(void)
{
    RXTrack tr = { 0 };
    tr.meta.title = "\xF0\x9F\x8E\xB5 title";
    tr.meta.comment = "";
    RXPlanTrack pt = {
        .id = 1, .track = &tr, .usb_path = "/Contents/\xF0\x9F\x8E\xB5/B/x.flac",
        .anlz_dir = "/PIONEER/USBANLZ/P000/00000000", .file_type = 5, .artist_id = 1, .album_id = 1,
    };
    RXPlanName artist = { 1, "\xF0\x9F\x8E\xB5 artist" }, album = { 1, "\xC3\xA4lbum \xF0\x9F\x8E\xB5", 1 };
    RXPlanPlaylist pl = { .id = 1, .name = "\xF0\x9F\x8E\xB5 list" };
    RXPlan plan = {
        .tracks = &pt, .nb_tracks = 1, .artists = &artist, .nb_artists = 1, .albums = &album, .nb_albums = 1,
        .playlists = &pl, .nb_playlists = 1,
    };
    snprintf(plan.date, sizeof(plan.date), "2026-10-13");
    uint8_t *b;
    size_t size;
    CHECK_EQ(rx_pdb_build(&plan, &b, &size), 0);
    CHECK_EQ(rx_pdb_check(b, size, print_problem, "astral"), 0);
    RXPdb pdb;
    CHECK_EQ(rx_pdb_read(b, size, &pdb), 0);
    CHECK(pdb.nb_tracks == 1 && !strcmp(pdb.tracks[0].title, tr.meta.title) &&
          !strcmp(pdb.tracks[0].path, pt.usb_path) && !strcmp(pdb.tracks[0].artist, artist.name));
    CHECK(pdb.nb_playlists == 1 && !strcmp(pdb.playlists[0].name, pl.name));
    rx_pdb_free(&pdb);
    free(b);

    size_t big_n = 70000;
    char *big = malloc(big_n + 1);
    memset(big, 'z', big_n);
    big[big_n] = '\0';
    tr.meta.comment = big;
    CHECK_EQ(rx_pdb_track_row_overflow(&pt, &plan), 0x10000);
    CHECK(rx_pdb_build(&plan, &b, &size) < 0);
    tr.meta.comment = "";
    big[5000] = '\0';
    char *keep = artist.name;
    artist.name = big;
    CHECK_EQ(rx_pdb_track_row_overflow(&pt, &plan), 0);
    CHECK(rx_pdb_build(&plan, &b, &size) < 0);
    artist.name = keep;
    pl.name = big;
    CHECK(rx_pdb_build(&plan, &b, &size) < 0);
    pl.name = "list";
    album.name = big;
    CHECK(rx_pdb_build(&plan, &b, &size) < 0);
    free(big);

    /* 1100 x U+00E9: 2200 bytes, cut by the 1024-byte title buffer. */
    char *e = malloc(2201);
    for (int i = 0; i < 1100; i++)
        memcpy(e + 2 * i, "\xC3\xA9", 2);
    e[2200] = '\0';
    tr.meta.title = e;
    char t[1024];
    rx_plan_title(&pt, t, sizeof(t));
    CHECK(strlen(t) == 1022);
    const char *p = t;
    bool valid = true;
    while (*p)
        valid &= rx_utf8_next(&p) == 0xE9;
    CHECK(valid);
    free(e);
}

int main(void)
{
    strings();
    fixed_tables();
    roundtrip();
    artist_row();
    ext_tags();
    limits();
    return test_result("pdb");
}
