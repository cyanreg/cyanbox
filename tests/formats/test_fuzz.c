/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The readers on damaged input: every reader inspect and the sync run on a
 * stick (export.pdb and exportExt.pdb, the ANLZ files, the settings files,
 * exportLibrary.db with its -wal) gets the rekordbox fixtures and files of
 * our own, truncated at many lengths and with bytes flipped, zeroed and
 * saturated at many positions. A reader may return an error or a result;
 * it must not read out of bounds, overflow or leak, which is what the
 * sanitizer build of this test checks (meson -Db_sanitize=address,undefined).
 * The mutations are deterministic (a fixed xorshift seed). */

#include <stdio.h>

#include "rxbox/colors.h"
#include "rxbox/formats.h"

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "tests/formats/test.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static long runs;

/* ---------------------------------------------------------------- readers */

static void problem_cb(void *opaque, const char *p)
{
    (void)opaque;
    (void)p;
}

static void read_pdb(const uint8_t *b, size_t n)
{
    RxPdb pdb;
    if (rx_pdb_read(b, n, &pdb) >= 0) {
        for (int i = 0; i < pdb.nb_tracks; i++)
            CHECK(pdb.tracks[i].path && pdb.tracks[i].title && pdb.tracks[i].artist);
        rx_pdb_free(&pdb);
    }
    RxPdbRawTrack *raw;
    int nr;
    if (rx_pdb_raw_tracks(b, n, &raw, &nr) >= 0)
        rx_pdb_raw_tracks_free(raw, nr);
    rx_pdb_check(b, n, problem_cb, NULL);
    RxPdbHistoryList *hl;
    int nh;
    if (rx_pdb_history_lists(b, n, &hl, &nh) >= 0)
        rx_pdb_history_lists_free(hl, nh);
    uint32_t *ids;
    char **paths;
    int na;
    if (rx_pdb_artwork(b, n, &ids, &paths, &na) >= 0) {
        for (int i = 0; i < na; i++)
            free(paths[i]);
        free(paths);
        free(ids);
    }
    uint32_t num;
    char *date;
    if (rx_pdb_history(b, n, &num, &date) >= 0)
        free(date);
    char *s;
    if (rx_pdb_string_decode(b, n, &s) >= 0)
        free(s);
    runs++;
}

static void read_anlz(const uint8_t *b, size_t n)
{
    RxAnlzTag *tags;
    int nt;
    if (rx_anlz_parse(b, n, &tags, &nt) >= 0) {
        for (int i = 0; i < nt; i++) {
            CHECK(tags[i].raw >= b && tags[i].raw + tags[i].raw_size <= b + n);
            RxAnlzCueEntry *e;
            int ne, declared;
            if ((!memcmp(tags[i].fourcc, "PCOB", 4) || !memcmp(tags[i].fourcc, "PCO2", 4)) &&
                rx_anlz_tag_cues(&tags[i], &e, &ne, &declared) >= 0)
                rx_anlz_cue_entries_free(e, ne);
            uint32_t type, count;
            rx_anlz_pcob_header(&tags[i], &type, &count);
        }
        free(tags);
    }
    char *path;
    if (rx_anlz_read_path(b, n, &path) >= 0)
        free(path);
    RxBeat *beats;
    int nb;
    if (rx_anlz_read_beats(b, n, &beats, &nb) >= 0)
        free(beats);
    RxPlanCue *cues;
    int nc;
    if (rx_anlz_read_cues(b, n, &cues, &nc) >= 0) {
        for (int i = 0; i < nc; i++)
            CHECK(cues[i].hot >= -1 && cues[i].hot < RX_HOT_CUE_SLOTS);
        free(cues);
    }
    RxWaveforms w = { 0 };
    rx_anlz_read_waveforms(b, n, &w);
    free(w.pwv3);
    free(w.pwv5);
    free(w.pwv7);
    runs++;
}

static const char *settings_name;

static void read_settings(const uint8_t *b, size_t n)
{
    RxKv *kv;
    int nk;
    if (rx_settings_decode(settings_name, b, n, &kv, &nk) >= 0)
        rx_kv_free(kv, nk);
    runs++;
}

static void read_plain_db(const uint8_t *b, size_t n)
{
    RxOneLibTrack *t;
    int nt;
    if (rx_onelibrary_read(b, n, &t, &nt) >= 0) {
        for (int i = 0; i < nt; i++) {
            free(t[i].path);
            free(t[i].title);
        }
        free(t);
    }
    runs++;
}

/* -------------------------------------------------------------- mutations */

typedef void (*reader_fn)(const uint8_t *b, size_t n);

/* Each mutation gets its own exact-size copy, so a read one byte past the
 * end is a sanitizer error rather than a read of the next field. */
static void run(reader_fn fn, const uint8_t *b, size_t n)
{
    uint8_t *c = malloc(RX_MAX(n, 1));
    if (!c)
        return;
    memcpy(c, b, n);
    fn(c, n);
    free(c);
}

static void truncations(reader_fn fn, const uint8_t *b, size_t n, size_t step)
{
    for (size_t len = 0; len < n; len += step)
        run(fn, b, len);
    for (size_t k = 1; k <= 16 && k <= n; k++)
        run(fn, b, n - k);
}

static void mutate_at(reader_fn fn, uint8_t *b, size_t n, size_t pos)
{
    static const int ops[] = { 0x01, 0x80, -1, -2, 0x10 };
    uint8_t orig = b[pos];
    for (size_t k = 0; k < RX_ARRAY_ELEMS(ops); k++) {
        b[pos] = ops[k] == -1 ? 0x00 : ops[k] == -2 ? 0xFF : (uint8_t)(orig ^ ops[k]);
        run(fn, b, n);
    }
    b[pos] = orig;
}

/* Every byte of small files; for large ones every byte of `hot` ranges
 * (headers, row indexes) plus `random` positions anywhere. */
static void mutations(reader_fn fn, const uint8_t *in, size_t n, size_t every_below, int random)
{
    uint8_t *b = malloc(RX_MAX(n, 1));
    memcpy(b, in, n);
    if (n <= every_below) {
        for (size_t pos = 0; pos < n; pos++)
            mutate_at(fn, b, n, pos);
    } else {
        for (int i = 0; i < random; i++)
            mutate_at(fn, b, n, next_rand() % n);
    }
    /* Two bytes at once: length fields spanning bytes. */
    for (int i = 0; i < random / 4 && n > 1; i++) {
        size_t pos = next_rand() % (n - 1);
        uint8_t o0 = b[pos], o1 = b[pos + 1];
        b[pos] = (uint8_t)next_rand();
        b[pos + 1] = (uint8_t)next_rand();
        run(fn, b, n);
        b[pos] = o0;
        b[pos + 1] = o1;
    }
    free(b);
}

/* DeviceSQL files: the page headers and row index tails of every page are
 * where the lengths live. */
static void pdb_mutations(const uint8_t *in, size_t n, int random)
{
    uint8_t *b = malloc(n);
    memcpy(b, in, n);
    for (size_t page = 0; page * RX_PDB_PAGE < n; page++) {
        size_t at = page * RX_PDB_PAGE;
        /* The file header's table directory, the page headers. */
        for (size_t k = 0; k < (page ? 0x40u : 0x200u) && at + k < n; k++)
            mutate_at(read_pdb, b, n, at + k);
        for (size_t k = RX_PDB_PAGE - 0x28; k < RX_PDB_PAGE && at + k < n; k += 2)
            mutate_at(read_pdb, b, n, at + k);
    }
    free(b);
    mutations(read_pdb, in, n, 0, random);
    truncations(read_pdb, in, n, 1021);
}

/* ------------------------------------------------------------- our files */

static void our_pdb(uint8_t **pdb, size_t *pdb_size, uint8_t **ext, size_t *ext_size)
{
    enum { NT = 40 };
    static RxTrack tr[NT];
    static RxPlanTrack pt[NT];
    static char titles[NT][200], paths[NT][256];
    static RxTempoSection sec = { .start = 0.1, .bpm = 128.0, .beat = 1 };
    for (int i = 0; i < NT; i++) {
        /* Short, long ASCII, non-ASCII and astral titles. */
        if (i % 4 == 0)
            snprintf(titles[i], sizeof(titles[i]), "T%d", i);
        else if (i % 4 == 1)
            snprintf(titles[i], sizeof(titles[i]), "%0150d", i);
        else if (i % 4 == 2)
            snprintf(titles[i], sizeof(titles[i]), "Tr\xC3\xA4" "ck %d", i);
        else
            snprintf(titles[i], sizeof(titles[i]), "\xF0\x9F\x8E\xB5 %d", i);
        snprintf(paths[i], sizeof(paths[i]), "/Contents/A/B/%.199s.flac", titles[i]);
        tr[i].meta.title = titles[i];
        tr[i].meta.comment = i % 3 ? "" : "a comment";
        pt[i] = (RxPlanTrack) {
            .id          = (uint32_t)i + 1,
            .track       = &tr[i],
            .usb_path    = paths[i],
            .anlz_dir    = "/PIONEER/USBANLZ/P000/00000000",
            .anlz_index  = i,
            .file_type   = 5,
            .sample_rate = 44100,
            .duration    = 200.0 + i,
            .artist_id   = (uint32_t)(i % 3) + 1,
            .artwork_id  = i % 2 ? 1 : 0,
            .grid        = { .sections = &sec, .nb_sections = 1, .beats_per_bar = 4 },
        };
    }
    static RxPlanName artists[3] = {
        { 1, "Artist" }, { 2, "R\xC3\xB8" "dh\xC3\xA5" "d" }, { 3, "An artist with a long name, longer "
        "than the hundred and twenty six bytes a short DeviceSQL string holds, so it is a long one" },
    };
    static RxPlanName albums[1] = { { 1, "Album", 1 } };
    static RxPlanName keys[2] = { { 1, "Am" }, { 2, "F#" } };
    static RxPlanArtwork art[1] = { { .id = 1 } };
    static uint32_t list[4] = { 3, 1, 2, 40 };
    static RxPlanPlaylist pl[2] = {
        { .id = 1, .parent_id = 0, .is_folder = true, .name = "Folder" },
        { .id = 2, .parent_id = 1, .name = "List", .track_ids = list, .nb = 4 },
    };
    static RxPlanTag tags[5] = {
        { .id = 1, .name = "Genre", .position = 0, .is_category = true },
        { .id = 2, .name = "Components", .position = 1, .is_category = true },
        { .id = 3, .name = "Situation", .position = 2, .is_category = true },
        { .id = 4, .name = "Tags", .position = 3, .is_category = true },
        { .id = 999, .name = "vocal", .category = 4, .position = 0 },
    };
    static RxPlanTagTrack tt[2] = { { 1, 999 }, { 2, 999 } };
    RxPlan plan = {
        .tracks        = pt,     .nb_tracks     = NT,
        .artists       = artists, .nb_artists   = 3,
        .albums        = albums, .nb_albums     = 1,
        .keys          = keys,   .nb_keys       = 2,
        .artworks      = art,    .nb_artworks   = 1,
        .playlists     = pl,     .nb_playlists  = 2,
        .tags          = tags,   .nb_tags       = 5,
        .tag_tracks    = tt,     .nb_tag_tracks = 2,
        .device_name   = "STICK",
        .library_id    = 77,
    };
    snprintf(plan.date, sizeof(plan.date), "2026-10-13");
    CHECK_EQ(rx_pdb_build(&plan, pdb, pdb_size), 0);
    CHECK_EQ(rx_pdb_ext_build(&plan, ext, ext_size), 0);
}

static void our_anlz(RxAnlzFiles *f)
{
    static RxTempoSection sec[2] = { { 0.2, 120.0, 1 }, { 30.0, 126.0, 0 } };
    RxPlanCue cues[5] = {
        { .time = 0.5, .hot = 0, .loop_end = -1, .color_code = 0x2A, .rgb = 0xFF0000, .name = "intro" },
        { .time = 1.0, .hot = 5, .loop_end = 3.0 },
        { .time = 2.0, .hot = -1, .loop_end = 4.0, .active = true, .color_code = 3 },
        { .time = 3.0, .hot = -1, .loop_end = -1, .name = "\xF0\x9F\x8E\xB5" },
        { .time = 4.0, .hot = 2, .loop_end = -1 },
    };
    RxPlanTrack t = {
        .usb_path = "/Contents/A/B/x.flac",
        .duration = 60.0,
        .grid     = { .sections = sec, .nb_sections = 2, .beats_per_bar = 4 },
        .cues     = cues,
        .nb_cues  = 5,
    };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "flac");
    t.probe.flac = (RxFlacInfo) { .valid = true, .total_samples = 16384, .nb_frames = 4 };
    t.probe.flac.frames = calloc(4, sizeof(*t.probe.flac.frames));
    for (int i = 0; i < 4; i++) {
        t.probe.flac.frames[i].sample = 4096 * i;
        t.probe.flac.frames[i].offset = 900 * i;
        t.probe.flac.frames[i].block = 4096;
    }
    int nd = 300;
    RxWaveforms w = { .nb_detail = nd };
    w.pwv3 = calloc((size_t)nd, 1);
    w.pwv5 = calloc((size_t)nd, 2);
    w.pwv7 = calloc((size_t)nd, 3);
    memset(w.pwav, 5, sizeof(w.pwav));
    CHECK_EQ(rx_anlz_build(&t, &w, f), 0);
    free(t.probe.flac.frames);
    free(w.pwv3);
    free(w.pwv5);
    free(w.pwv7);
}

/* ----------------------------------------------------------------- tests */

static int load(const char *path, uint8_t **b, size_t *n)
{
    if (rx_file_read(path, b, n) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        failures++;
        return -1;
    }
    return 0;
}

static void fuzz_pdb(void)
{
    uint8_t *b;
    size_t n;
    if (load("tests/data/rekordbox-7.2.18-export.pdb", &b, &n) < 0)
        return;
    pdb_mutations(b, n, 3000);
    free(b);

    uint8_t *pdb, *ext;
    size_t pdb_size, ext_size;
    our_pdb(&pdb, &pdb_size, &ext, &ext_size);
    pdb_mutations(pdb, pdb_size, 3000);
    pdb_mutations(ext, ext_size, 500);
    free(pdb);
    free(ext);
}

static void fuzz_anlz(void)
{
    static const char *const names[3] = { "DAT", "EXT", "2EX" };
    for (int i = 0; i < 3; i++) {
        char path[64];
        snprintf(path, sizeof(path), "tests/data/anlz/ANLZ0000.%s", names[i]);
        uint8_t *b;
        size_t n;
        if (load(path, &b, &n) < 0)
            return;
        truncations(read_anlz, b, n, 1);
        mutations(read_anlz, b, n, 1 << 16, 2000);
        free(b);
    }
    RxAnlzFiles f;
    our_anlz(&f);
    const uint8_t *files[3] = { f.dat, f.ext, f.ex2 };
    size_t sizes[3] = { f.dat_size, f.ext_size, f.ex2_size };
    for (int i = 0; i < 3; i++) {
        truncations(read_anlz, files[i], sizes[i], 3);
        mutations(read_anlz, files[i], sizes[i], 8192, 3000);
    }
    rx_anlz_files_free(&f);
}

static void fuzz_settings(void)
{
    static const char *const names[4] = { "MYSETTING.DAT", "MYSETTING2.DAT", "DJMMYSETTING.DAT", "DEVSETTING.DAT" };
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "tests/data/settings/%s", names[i]);
        uint8_t *b;
        size_t n;
        if (load(path, &b, &n) < 0)
            return;
        settings_name = names[i];
        truncations(read_settings, b, n, 1);
        mutations(read_settings, b, n, 4096, 0);
        free(b);
    }
}

/* The SQLCipher layer authenticates every page, so the interesting
 * damage is to the file's length and to the -wal; each decryption runs the
 * 256000-round key derivation, so these are few. The plaintext then goes
 * to SQLite damaged, as a stick written by a buggy player would. */
static void fuzz_onelibrary(void)
{
    uint8_t *db, *wal;
    size_t db_size, wal_size;
    if (load("tests/data/rekordbox-7.2.18-exportLibrary.db", &db, &db_size) < 0 ||
        load("tests/data/rekordbox-7.2.18-exportLibrary.db-wal", &wal, &wal_size) < 0)
        return;
    uint8_t *plain = NULL;
    size_t plain_size = 0;
    static const size_t db_lens[] = { 0, 15, 16, 4095, 4096, 4097, 8192 + 7 };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(db_lens); i++) {
        uint8_t *c = malloc(RX_MAX(db_lens[i], 1));
        memcpy(c, db, db_lens[i]);
        if (rx_sqlcipher_decrypt(c, db_lens[i], rx_onelibrary_passphrase, NULL, 0, &plain, &plain_size) >= 0)
            free(plain);
        free(c);
    }
    static const size_t wal_lens[] = { 0, 1, 31, 32, 33, 32 + 24, 32 + 24 + 4095, 32 + 24 + 4096 + 3 };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(wal_lens); i++) {
        uint8_t *c = malloc(RX_MAX(wal_lens[i], 1));
        memcpy(c, wal, RX_MIN(wal_lens[i], wal_size));
        if (rx_sqlcipher_decrypt(db, db_size, rx_onelibrary_passphrase, c, RX_MIN(wal_lens[i], wal_size),
                                 &plain, &plain_size) >= 0)
            free(plain);
        free(c);
    }
    /* Frame headers: page number, commit size, salts. */
    static const size_t wal_pos[] = { 0, 16, 32, 32 + 3, 32 + 7, 32 + 8, 32 + 24 + 5,
                                      32 + 24 + 4096 + 3, 32 + 2 * (24 + 4096) + 7 };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(wal_pos); i++) {
        if (wal_pos[i] >= wal_size)
            continue;
        uint8_t *c = malloc(wal_size);
        memcpy(c, wal, wal_size);
        c[wal_pos[i]] = 0xFF;
        if (rx_sqlcipher_decrypt(db, db_size, rx_onelibrary_passphrase, c, wal_size, &plain, &plain_size) >= 0)
            free(plain);
        free(c);
    }

    CHECK_EQ(rx_sqlcipher_decrypt(db, db_size, rx_onelibrary_passphrase, wal, wal_size, &plain, &plain_size), 0);
    if (plain) {
        truncations(read_plain_db, plain, plain_size, 4093);
        mutations(read_plain_db, plain, plain_size, 0, 2000);
        free(plain);
    }
    free(db);
    free(wal);
}

int main(void)
{
    fuzz_pdb();
    fuzz_anlz();
    fuzz_settings();
    fuzz_onelibrary();
    fprintf(stderr, "fuzz: %ld reader runs\n", runs);
    return test_result("fuzz");
}
