/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The database server's fuzzer: random and mutated messages into the
 * codec, the sessions and the client's decoders, under AddressSanitizer
 * and UndefinedBehaviorSanitizer (src/link/meson.build), which abort on
 * the first finding.
 *
 * Deterministic: a fixed seed, RX_FUZZ_SEED to change it, RX_FUZZ_ITERS
 * rounds. Each round makes a request of a kind players send, with ids the
 * catalog has or random ones, offsets and limits up to 2^32 - 1, then
 * mutates it or not (bit flips, truncation, lying lengths, counts and
 * tags, bytes spliced from another), and feeds a session in random cuts.
 * Whatever a session answers must parse whole: the server never writes a
 * message its own codec refuses. The decoders get the catalog's real
 * blobs, mutated.
 *
 * The catalog is over a small library made under RX_TEST_DIR: three WAVs
 * written here (no ffmpeg), with tags, cues, grids, waveforms, a playlist
 * and an artwork put into the plan. */

#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include "rxbox/analysis.h"
#include "rxbox/export.h"

#include "src/link/catalog.h"
#include "src/link/dbmsg.h"
#include "src/link/dbserver.h"

static uint64_t rng, seed;
static long round_no;

/* What a run did, said at the end. */
static struct {
    uint64_t sessions, closed, replies, items, bytes, parsed;
} stats;

static uint32_t rnd(void)
{
    /* xorshift64*: a fixed sequence from the seed. */
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32);
}

static uint32_t below(uint32_t n)
{
    return n ? rnd() % n : 0;
}

[[noreturn]] static void fail(const char *what)
{
    fprintf(stderr, "fuzz_db: %s in round %ld of seed %#llx\n", what, round_no,
            (unsigned long long)seed);
    abort();
}

/* ------------------------------------------------------------ library */

static int write_wav(const char *path, double freq, double seconds)
{
    int rate = 44100, n = (int)(rate * seconds);
    RXWr w = rx_wr_dyn();
    rx_wr_bytes(&w, "RIFF", 4);
    rx_wr_u32le(&w, (uint32_t)(36 + 2 * n));
    rx_wr_bytes(&w, "WAVEfmt ", 8);
    rx_wr_u32le(&w, 16);
    rx_wr_u16le(&w, 1);
    rx_wr_u16le(&w, 1);
    rx_wr_u32le(&w, (uint32_t)rate);
    rx_wr_u32le(&w, (uint32_t)rate * 2);
    rx_wr_u16le(&w, 2);
    rx_wr_u16le(&w, 16);
    rx_wr_bytes(&w, "data", 4);
    rx_wr_u32le(&w, (uint32_t)(2 * n));
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double v = 0.6 * sin(2 * M_PI * freq * t) * exp(-8.0 * fmod(t, 0.5));
        rx_wr_u16le(&w, (uint16_t)(int16_t)lrint(v * 32767.0));
    }
    int ret = w.err ? -ENOMEM : rx_file_write(path, w.p, w.len);
    rx_wr_free(&w);
    return ret;
}

static int make_library(const char *root, RXLibrary *lib)
{
    char *music = rx_asprintf("%s/music", root), *libdir = rx_asprintf("%s/library", root);
    int ret = music && libdir ? rx_mkdir_p(music) : -ENOMEM;
    if (ret >= 0)
        ret = rx_mkdir_p(libdir);
    if (ret >= 0)
        ret = rx_library_open(lib, libdir);
    static const char *const titles[] = { "Alpha Seed", "Bravo Mutant", "" };
    RXTrack *tracks[3] = { 0 };
    for (int i = 0; ret >= 0 && i < 3; i++) {
        char *path = rx_asprintf("%s/t%d.wav", music, i);
        ret = path ? write_wav(path, 55.0 * (i + 1), 2.0) : -ENOMEM;
        int err = 0;
        RXTrack *t = ret >= 0 ? rx_library_add_file(lib, path, &err) : NULL;
        free(path);
        if (!t) {
            ret = ret < 0 ? ret : err ? err : -EIO;
            break;
        }
        tracks[i] = t;
        rx_meta_set(&t->meta.title, titles[i][0] ? titles[i] : NULL);
        rx_meta_set(&t->meta.artist, i == 2 ? NULL : "Fuzz");
        rx_meta_set(&t->meta.album, i ? "Noise" : NULL);
        rx_meta_set(&t->meta.genre, "Glitch");
        rx_meta_set(&t->meta.remixer, i == 1 ? "Re Mix" : NULL);
        t->meta.year = 2000 + i * 7;
        t->meta.rating = i + 2;
        snprintf(t->key, sizeof(t->key), "%s", i == 0 ? "Am" : i == 1 ? "Gb" : "");
        rx_grid_init(&t->grid, 0.05, 120.0 + i);
        RXCue c;
        rx_cue_init(&c, 0.5);
        c.hot = i;
        c.loop_end = i == 1 ? 1.0 : -1.0;
        snprintf(c.name, sizeof(c.name), "cue %d \xe2\x99\xaf", i);
        rx_track_add_cue(t, &c);
        rx_cue_init(&c, 0.25);
        rx_track_add_cue(t, &c);
        rx_library_save_track(lib, t);
        int n = 44100 * 2;
        float *mono = malloc((size_t)n * sizeof(*mono));
        RXWaveforms w;
        if (mono) {
            for (int k = 0; k < n; k++)
                mono[k] = (float)(0.6 * sin(2 * M_PI * 55.0 * (i + 1) * k / 44100.0));
            if (rx_waveforms_compute(mono, n, 44100, &w) >= 0) {
                rx_library_waveforms_save(lib, t->id, &w);
                rx_waveforms_free(&w);
            }
        }
        free(mono);
    }
    if (ret >= 0) {
        RXPlaylist *p = rx_library_playlist_create(lib, lib->root_folder, "Fuzz set");
        RXPlaylistFolder *f = rx_library_folder_create(lib, lib->root_folder, "Folder");
        for (int i = 2; p && i >= 0; i--)
            rx_library_playlist_add(lib, p, tracks[i], -1);
        if (f)
            rx_library_playlist_create(lib, f, "Empty");
        rx_library_record_play(lib, tracks[1]);
    }
    free(music);
    free(libdir);
    return ret;
}

/* An artwork put into the plan: the catalog sends its bytes as they are. */
static int add_artwork(RXPlan *plan)
{
    RXPlanArtwork *a = calloc(1, sizeof(*a));
    if (!a)
        return -ENOMEM;
    *a = (RXPlanArtwork) { .id = 1, .jpeg80 = malloc(300), .size80 = 300, .jpeg240 = malloc(900),
                           .size240 = 900 };
    if (!a->jpeg80 || !a->jpeg240) {
        free(a->jpeg80);
        free(a->jpeg240);
        free(a);
        return -ENOMEM;
    }
    memset(a->jpeg80, 0xd8, 300);
    memset(a->jpeg240, 0xd9, 900);
    plan->artworks = a;
    plan->nb_artworks = 1;
    if (plan->nb_tracks)
        plan->tracks[0].artwork_id = 1;
    return 0;
}

static void remove_tree(const char *root)
{
    char *cmd = rx_asprintf("rm -rf '%s'", root);
    if (cmd && system(cmd) != 0)
        fprintf(stderr, "fuzz_db: could not remove %s\n", root);
    free(cmd);
}

/* ----------------------------------------------------------- requests */

static const uint16_t kinds[] = {
    RX_DB_SETUP, RX_DB_TEARDOWN, RX_DB_ROOT, RX_DB_GENRES, RX_DB_ARTISTS, RX_DB_ALBUMS,
    RX_DB_TRACKS, RX_DB_BPMS, RX_DB_RATINGS, RX_DB_DECADES, RX_DB_LABELS,
    RX_DB_COLORS, RX_DB_TIMES, RX_DB_BITRATES, RX_DB_HISTORIES, RX_DB_FILE_NAMES, RX_DB_KEYS,
    RX_DB_GENRE_ARTISTS, RX_DB_ARTIST_ALBUMS, RX_DB_ALBUM_TRACKS, RX_DB_PLAYLIST, RX_DB_BPM_RANGES,
    RX_DB_RATING_TRACKS, RX_DB_DECADE_YEARS, RX_DB_LABEL_ARTISTS,
    RX_DB_COLOR_TRACKS, RX_DB_TIME_TRACKS, RX_DB_BITRATE_TRACKS, RX_DB_HISTORY_TRACKS,
    RX_DB_RELATED_KEYS, RX_DB_GENRE_ARTIST_ALBUMS, RX_DB_ARTIST_ALBUM_TRACKS,
    RX_DB_BPM_TRACKS, RX_DB_YEAR_TRACKS, RX_DB_LABEL_ARTIST_ALBUMS, RX_DB_KEY_TRACKS, RX_DB_SEARCH,
    RX_DB_GENRE_TRACKS, RX_DB_ORIGINALS, RX_DB_LABEL_TRACKS, RX_DB_SORTS, RX_DB_ORIGINAL_ALBUMS,
    RX_DB_SEARCH_LIVE, RX_DB_ORIGINAL_TRACKS, RX_DB_REMIXERS, RX_DB_REMIXER_ALBUMS,
    RX_DB_ADDED_YEARS, RX_DB_REMIXER_TRACKS, RX_DB_ADDED_MONTHS, RX_DB_ADDED_DAYS,
    RX_DB_ADDED_TRACKS, RX_DB_METADATA, RX_DB_TRACK_INFO, RX_DB_DELIVERY, RX_DB_ARTWORK,
    RX_DB_PREVIEW, RX_DB_CUES, RX_DB_GRID, RX_DB_VBR, RX_DB_DETAIL,
    RX_DB_CUES_EXT, RX_DB_TAG, RX_DB_TAG_2EX, RX_DB_RENDER, RX_DB_USER_INFO, RX_DB_FILTER,
    RX_DB_ITEM_POSITION, RX_DB_SAVE_WAVEFORM, RX_DB_SAVE_CUE,
    RX_DB_SAVE_RATING, RX_DB_SAVE_VBR, RX_DB_SAVE_GRID_OFFSET, RX_DB_SAVE_CUE_EXT, RX_DB_SAVE_ATOM,
    RX_DB_UPDATE_ATOM, RX_DB_HISTORY_ADD, RX_DB_SAVE_TAG, RX_DB_HISTORY_DELETE, RX_DB_ON_AIR,
    RX_DB_CLEAR_TAGS, RX_DB_HISTORY_REMOVE, 0x1017, 0x2fff, 0x4101,
};

static const char *const texts[] = { "", "A", "ALPHA", "FUZZ", "\xe2\x99\xaf", "NOISE GLITCH" };
static const uint32_t fourccs[] = { 0x34565750, 0x35565750, 0x36565750, 0x37565750, 0x5a545150,
                                    0x32545150, 0x56415750, 0x49535350, 0x43565750 };
static const uint32_t exts[] = { 0x544144, 0x545845, 0x584532, 0x787865, 0 };

/* A value a request might carry: an id the catalog has, a small number, a
 * value with meaning (ALL, the menus' sorts), or anything. */
static uint32_t value(const uint32_t *ids, int nb_ids)
{
    switch (below(6)) {
    case 0:  return nb_ids ? ids[below((uint32_t)nb_ids)] : 1;
    case 1:  return below(30);
    case 2:  return RX_DB_ALL;
    case 3:  return below(2) ? 0x40000000u | below(5) : 0x7fffffff;
    case 4:  return rx_db_rmst((int)below(8), (int)below(10), (int)below(5), (int)below(3));
    }
    return rnd();
}

/* A request: of a kind players send (now and then any kind), its first
 * argument the r:m:s:t word at `location`, then a sort and ids as the
 * menus take them, or strings, blobs, anything. */
static void request(RXWr *w, uint32_t txid, int location, const uint32_t *ids, int nb_ids)
{
    RXDbMsg m = { .txid = below(8) ? txid : rnd(), .kind = kinds[below(RX_ARRAY_ELEMS(kinds))] };
    if (!below(40))
        m.kind = (uint16_t)rnd();
    uint8_t blob[64];
    for (size_t i = 0; i < sizeof(blob); i++)
        blob[i] = (uint8_t)rnd();
    m.nb_args = below(8) ? 2 + (int)below(4) : (int)below(13);
    m.args[0] = (RXDbArg) { .type = RX_DB_NUM, .num = rx_db_rmst(1, location, 3, 1) };
    for (int i = 1; i < m.nb_args; i++) {
        uint32_t pick = below(24);
        if (pick == 0)
            m.args[i] = (RXDbArg) { .type = RX_DB_STR, .str = texts[below(RX_ARRAY_ELEMS(texts))] };
        else if (pick == 1)
            m.args[i] = (RXDbArg) { .type = RX_DB_BLOB, .p = blob, .len = below(sizeof(blob)) };
        else if (i == 1 && pick < 16)
            m.args[i] = (RXDbArg) { .type = RX_DB_NUM, .num = below(18) };      /* a sort, or a track */
        else
            m.args[i] = (RXDbArg) { .type = RX_DB_NUM, .num = value(ids, nb_ids) };
    }
    if (m.kind == RX_DB_TAG || m.kind == RX_DB_TAG_2EX) {
        m.nb_args = 4;
        m.args[1] = (RXDbArg) { .type = RX_DB_NUM, .num = value(ids, nb_ids) };
        m.args[2] = (RXDbArg) { .type = RX_DB_NUM, .num = fourccs[below(RX_ARRAY_ELEMS(fourccs))] };
        m.args[3] = (RXDbArg) { .type = RX_DB_NUM, .num = exts[below(RX_ARRAY_ELEMS(exts))] };
    } else if (m.kind == RX_DB_SEARCH || m.kind == RX_DB_SEARCH_LIVE) {
        m.nb_args = 5;
        m.args[1] = (RXDbArg) { .type = RX_DB_NUM, .num = below(18) };
        m.args[2] = (RXDbArg) { .type = RX_DB_NUM, .num = rnd() };
        m.args[3] = (RXDbArg) { .type = RX_DB_STR, .str = texts[below(RX_ARRAY_ELEMS(texts))] };
    } else if (m.kind == RX_DB_SETUP) {
        m.txid = RX_DB_SETUP_TXID;
    }
    rx_dbmsg_write(w, &m);
}

/* A render of what the request before asked for, at its location: offsets
 * and limits in and past the menu, and up to 2^32 - 1. */
static void render(RXWr *w, uint32_t txid, int location)
{
    uint32_t offset = below(4) ? below(20) : rnd(), limit = below(4) ? below(40) : rnd();
    const uint32_t v[8] = { rx_db_rmst(1, location, 3, 1), offset, limit, 0, limit, 12,
                            below(2), below(20) };
    rx_dbmsg_numbers(w, txid, RX_DB_RENDER, v, below(3) ? 8 : below(2) ? 6 : (int)below(9));
}

/* Bit flips, a cut, a lying length or count or tag, bytes from elsewhere. */
static void mutate(RXWr *w)
{
    if (!w->len)
        return;
    int n = 1 + (int)below(4);
    for (int k = 0; k < n && w->len; k++) {
        size_t at = below((uint32_t)w->len);
        switch (below(7)) {
        case 0:
            w->p[at] ^= (uint8_t)(1u << below(8));
            break;
        case 1:
            w->len = at;
            break;
        case 2:
            /* A 4-byte length or count, wherever one may be. */
            if (at + 4 <= w->len)
                rx_wr_at_u32(w, at, below(2) ? rnd() : 0x7ffffff0u + below(16));
            break;
        case 3:
            if (w->len > 14)
                w->p[14] = (uint8_t)rnd();          /* the argument count */
            break;
        case 4:
            if (w->len > 20)
                w->p[20 + below((uint32_t)(w->len - 20))] = (uint8_t)below(8);    /* a tag */
            break;
        case 5:
            w->p[at] = (uint8_t)rnd();
            break;
        default: {
            size_t from = below((uint32_t)w->len), len = below((uint32_t)(w->len - from) + 1);
            uint8_t *d = rx_wr_room(w, len);
            if (d)
                memmove(d, w->p + from, len);
            break;
        }
        }
    }
}

/* Every reply a session writes parses. */
static void check_replies(const uint8_t *p, size_t len, bool greeted_now)
{
    size_t pos = 0;
    if (greeted_now) {
        if (len < RX_DB_GREETING_LEN || memcmp(p, rx_db_greeting, RX_DB_GREETING_LEN))
            fail("no greeting");
        pos = RX_DB_GREETING_LEN;
    }
    while (pos < len) {
        RXDbMsg m;
        int r = rx_dbmsg_parse(p + pos, len - pos, 64 << 20, &m);
        if (r <= 0)
            fail("a reply does not parse");
        stats.replies++;
        if (m.kind == RX_DB_MENU_ITEM) {
            stats.items++;
            RXDbRow row;
            if (rx_dbrow_parse(&m, &row) < 0)
                fail("a menu item does not read back");
            rx_dbrow_free(&row);
        }
        pos += (size_t)r;
    }
}

static void fuzz_session(RXCatalog **cat, const uint32_t *ids, int nb_ids, uint32_t round)
{
    RXDbSession *s;
    if (rx_dbsession_new(&s, cat, 17) < 0)
        fail("out of memory");
    RXWr in = rx_wr_dyn();
    bool greet = below(16) != 0;
    if (greet)
        rx_wr_bytes(&in, rx_db_greeting, RX_DB_GREETING_LEN);
    if (below(4)) {
        RXDbMsg setup = { .txid = RX_DB_SETUP_TXID, .kind = RX_DB_SETUP, .nb_args = 1 + (int)below(2) };
        setup.args[0] = (RXDbArg) { .type = RX_DB_NUM, .num = 1 + below(6) };
        setup.args[1] = (RXDbArg) { .type = RX_DB_NUM, .num = RX_DB_SETUP_MAGIC };
        rx_dbmsg_write(&in, &setup);
    }
    int nb = 1 + (int)below(12);
    bool mutating = !below(4);
    for (int i = 0; i < nb; i++) {
        RXWr one = rx_wr_dyn();
        int location = (int)below(10);
        request(&one, round * 32 + (uint32_t)i, location, ids, nb_ids);
        if (below(3))
            render(&one, round * 32 + 16 + (uint32_t)i, below(8) ? location : (int)below(10));
        if (mutating && !below(4))
            mutate(&one);
        rx_wr_bytes(&in, one.p, one.len);
        rx_wr_free(&one);
    }
    if (mutating && !below(4)) {
        /* Garbage, or random bytes, at the end. */
        int n = (int)below(64);
        for (int i = 0; i < n; i++)
            rx_wr_u8(&in, (uint8_t)rnd());
    }
    if (in.err)
        fail("out of memory");

    RXWr out = rx_wr_dyn();
    size_t pos = 0;
    int ret = 0;
    while (pos < in.len && ret >= 0) {
        size_t n = 1 + below(below(4) ? 200 : 4);
        n = RX_MIN(n, in.len - pos);
        ret = rx_dbsession_input(s, in.p + pos, n, &out);
        pos += n;
    }
    if (ret == -ENOMEM || out.err)
        fail("out of memory");
    stats.sessions++;
    stats.closed += ret < 0;
    stats.bytes += out.len;
    /* What was written before a closing error is still whole messages. */
    check_replies(out.p, out.len, greet && out.len >= RX_DB_GREETING_LEN);
    rx_wr_free(&out);
    rx_wr_free(&in);
    rx_dbsession_free(&s);
}

/* The codec alone: random bytes, and messages mutated; whatever parses
 * writes back into something that parses the same. */
static void fuzz_codec(void)
{
    RXWr w = rx_wr_dyn();
    if (below(3)) {
        request(&w, rnd(), (int)below(10), NULL, 0);
        mutate(&w);
    } else {
        int n = (int)below(80);
        for (int i = 0; i < n; i++)
            rx_wr_u8(&w, (uint8_t)rnd());
    }
    if (w.err)
        fail("out of memory");
    RXDbMsg m;
    size_t max = below(2) ? 1 << 20 : below(256);
    int r = rx_dbmsg_parse(w.p, w.len, max, &m);
    if (r > (int)w.len || r > (int)max)
        fail("a parse went past what it had");
    if (r > 0) {
        stats.parsed++;
        RXWr back = rx_wr_dyn();
        rx_dbmsg_write(&back, &m);
        RXDbMsg m2;
        int r2 = rx_dbmsg_parse(back.p, back.len, 1 << 20, &m2);
        if (r2 != (int)back.len || m2.nb_args != m.nb_args || m2.kind != m.kind || m2.txid != m.txid)
            fail("a parsed message does not write back");
        for (int i = 0; i < m.nb_args; i++)
            if (m.args[i].type != m2.args[i].type || m.args[i].num != m2.args[i].num ||
                m.args[i].len != m2.args[i].len ||
                (m.args[i].len && memcmp(m.args[i].p, m2.args[i].p, m.args[i].len)))
                fail("an argument changed on the way back");
        RXDbRow row;
        if (rx_dbrow_parse(&m, &row) >= 0)
            rx_dbrow_free(&row);
        char *s = rx_dbmsg_str(&m, (int)below(4));
        free(s);
        rx_wr_free(&back);
    }
    rx_wr_free(&w);
    uint8_t q[24];
    size_t qn = below(sizeof(q) + 1);
    for (size_t i = 0; i < qn; i++)
        q[i] = (uint8_t)rnd();
    if (rx_dbquery_parse(q, qn) > (int)qn)
        fail("the port query went past what it had");
}

/* The client's decoders, on the catalog's blobs mutated. */
static void fuzz_decoders(RXCatalog *cat, const uint32_t *ids, int nb_ids)
{
    RXWr b = rx_wr_dyn();
    uint32_t id = nb_ids ? ids[below((uint32_t)nb_ids)] : 1, count;
    int which = (int)below(3);
    if (which == 0)
        rx_catalog_grid(cat, id, &b);
    else if (which == 1)
        rx_catalog_cues_ext(cat, id, &b, &count);
    else
        rx_catalog_tag(cat, id, fourccs[below(RX_ARRAY_ELEMS(fourccs))], exts[below(3)], &b);
    if (below(4))
        mutate(&b);
    if (b.err)
        fail("out of memory");
    /* A copy of exactly its length, so a read past it is caught. */
    uint8_t *p = malloc(RX_MAX(b.len, 1));
    if (!p)
        fail("out of memory");
    if (b.len)
        memcpy(p, b.p, b.len);
    RXBeat *beats;
    RXPlanCue *cues;
    RXAnlzTag tag;
    int nb;
    if (rx_db_grid_parse(p, b.len, &beats, &nb) >= 0)
        free(beats);
    if (rx_db_cues_ext_parse(p, b.len, &cues, &nb) >= 0)
        free(cues);
    if (rx_db_tag_parse(p, b.len, &tag) >= 0 && (tag.raw < p || tag.raw + tag.raw_size > p + b.len))
        fail("a tag past its blob");
    free(p);
    rx_wr_free(&b);
}

int main(void)
{
    const char *s = getenv("RX_FUZZ_SEED"), *it = getenv("RX_FUZZ_ITERS");
    seed = s ? strtoull(s, NULL, 0) : 0x5eedc0de;
    long iters = it ? strtol(it, NULL, 0) : 300000;
    rng = seed ? seed : 1;

    const char *base = getenv("RX_TEST_DIR");
    char root[600];
    snprintf(root, sizeof(root), "%s", base ? base : "fuzz-link-db");
    remove_tree(root);
    RXLibrary lib = { 0 };
    int ret = make_library(root, &lib);
    if (ret < 0) {
        fprintf(stderr, "fuzz_db: cannot make the library: %s\n", strerror(-ret));
        rx_library_close(&lib);
        return 1;
    }
    RXPlan plan;
    ret = rx_plan_build_cb(&lib, lib.tracks, lib.nb_tracks, NULL, 0, NULL, false, true, &plan,
                           NULL, NULL);
    if (ret < 0 || add_artwork(&plan) < 0) {
        fprintf(stderr, "fuzz_db: cannot plan: %s\n", strerror(-ret));
        rx_library_close(&lib);
        return 1;
    }
    RXCatalog *cat = NULL;
    if (rx_catalog_new(&cat, &lib, &plan) < 0)
        fail("cannot make the catalog");
    uint32_t ids[64];
    int nb_ids = 0;
    for (int i = 0; i < plan.nb_tracks && nb_ids < 32; i++)
        ids[nb_ids++] = plan.tracks[i].id;
    for (int i = 0; i < plan.nb_playlists && nb_ids < 48; i++)
        ids[nb_ids++] = plan.playlists[i].id;
    for (int i = 0; i < plan.nb_albums && nb_ids < 56; i++)
        ids[nb_ids++] = plan.albums[i].id | 0x40000000u;
    ids[nb_ids++] = 15;

    int64_t t0 = rx_time_us();
    for (long i = 0; i < iters; i++) {
        round_no = i;
        fuzz_codec();
        fuzz_session(&cat, ids, nb_ids, (uint32_t)i);
        if (cat)
            fuzz_decoders(cat, ids, nb_ids);
        /* Now and then the library "changes": a new catalog between
         * requests, as the service makes one. */
        if (!below(500)) {
            /* Sometimes none for a while, as between two libraries. */
            rx_catalog_free(&cat);
            if (below(4) && rx_catalog_new(&cat, &lib, &plan) < 0)
                fail("cannot make the catalog");
        }
    }
    fprintf(stderr, "fuzz_db: %ld rounds from seed %#llx in %.1f s: clean\n", iters,
            (unsigned long long)seed, (double)(rx_time_us() - t0) / 1e6);
    fprintf(stderr, "fuzz_db: %llu sessions (%llu closed early), %llu replies, %llu items, "
            "%.1f MB answered; %llu fuzzed messages parsed\n",
            (unsigned long long)stats.sessions, (unsigned long long)stats.closed,
            (unsigned long long)stats.replies, (unsigned long long)stats.items, stats.bytes / 1e6,
            (unsigned long long)stats.parsed);

    rx_catalog_free(&cat);
    rx_plan_free(&plan);
    rx_library_close(&lib);
    remove_tree(root);
    return 0;
}
