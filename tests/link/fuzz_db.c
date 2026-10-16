/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The database server's fuzzer: random and mutated messages into the
 * codec, the sessions and the client's decoders, under AddressSanitizer
 * and UndefinedBehaviorSanitizer (src/link/meson.build), which abort on
 * the first finding.
 *
 * Deterministic: a fixed seed, CB_FUZZ_SEED to change it, CB_FUZZ_ITERS
 * rounds. Each round makes a request of a kind players send, with ids the
 * catalog has or random ones, offsets and limits up to 2^32 - 1, then
 * mutates it or not (bit flips, truncation, lying lengths, counts and
 * tags, bytes spliced from another), and feeds a session in random cuts.
 * Whatever a session answers must parse whole: the server never writes a
 * message its own codec refuses. The decoders get the catalog's real
 * blobs, mutated.
 *
 * The catalog is over a small library made under CB_TEST_DIR: three WAVs
 * written here (no ffmpeg), with tags, cues, grids, waveforms, a playlist
 * and an artwork put into the plan. */

#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include "cyanbox/analysis.h"
#include "cyanbox/export.h"

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
    CBWr w = cb_wr_dyn();
    cb_wr_bytes(&w, "RIFF", 4);
    cb_wr_u32le(&w, (uint32_t)(36 + 2 * n));
    cb_wr_bytes(&w, "WAVEfmt ", 8);
    cb_wr_u32le(&w, 16);
    cb_wr_u16le(&w, 1);
    cb_wr_u16le(&w, 1);
    cb_wr_u32le(&w, (uint32_t)rate);
    cb_wr_u32le(&w, (uint32_t)rate * 2);
    cb_wr_u16le(&w, 2);
    cb_wr_u16le(&w, 16);
    cb_wr_bytes(&w, "data", 4);
    cb_wr_u32le(&w, (uint32_t)(2 * n));
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double v = 0.6 * sin(2 * M_PI * freq * t) * exp(-8.0 * fmod(t, 0.5));
        cb_wr_u16le(&w, (uint16_t)(int16_t)lrint(v * 32767.0));
    }
    int ret = w.err ? -ENOMEM : cb_file_write(path, w.p, w.len);
    cb_wr_free(&w);
    return ret;
}

static int make_library(const char *root, CBLibrary *lib)
{
    char *music = cb_asprintf("%s/music", root), *libdir = cb_asprintf("%s/library", root);
    int ret = music && libdir ? cb_mkdir_p(music) : -ENOMEM;
    if (ret >= 0)
        ret = cb_mkdir_p(libdir);
    if (ret >= 0)
        ret = cb_library_open(lib, libdir);
    static const char *const titles[] = { "Alpha Seed", "Bravo Mutant", "" };
    CBTrack *tracks[3] = { 0 };
    for (int i = 0; ret >= 0 && i < 3; i++) {
        char *path = cb_asprintf("%s/t%d.wav", music, i);
        ret = path ? write_wav(path, 55.0 * (i + 1), 2.0) : -ENOMEM;
        int err = 0;
        CBTrack *t = ret >= 0 ? cb_library_add_file(lib, path, &err) : NULL;
        free(path);
        if (!t) {
            ret = ret < 0 ? ret : err ? err : -EIO;
            break;
        }
        tracks[i] = t;
        cb_meta_set(&t->meta.title, titles[i][0] ? titles[i] : NULL);
        cb_meta_set(&t->meta.artist, i == 2 ? NULL : "Fuzz");
        cb_meta_set(&t->meta.album, i ? "Noise" : NULL);
        cb_meta_set(&t->meta.genre, "Glitch");
        cb_meta_set(&t->meta.remixer, i == 1 ? "Re Mix" : NULL);
        t->meta.year = 2000 + i * 7;
        t->meta.rating = i + 2;
        snprintf(t->key, sizeof(t->key), "%s", i == 0 ? "Am" : i == 1 ? "Gb" : "");
        cb_grid_init(&t->grid, 0.05, 120.0 + i);
        CBCue c;
        cb_cue_init(&c, 0.5);
        c.hot = i;
        c.loop_end = i == 1 ? 1.0 : -1.0;
        snprintf(c.name, sizeof(c.name), "cue %d \xe2\x99\xaf", i);
        cb_track_add_cue(t, &c);
        cb_cue_init(&c, 0.25);
        cb_track_add_cue(t, &c);
        cb_library_save_track(lib, t);
        int n = 44100 * 2;
        float *mono = malloc((size_t)n * sizeof(*mono));
        CBWaveforms w;
        if (mono) {
            for (int k = 0; k < n; k++)
                mono[k] = (float)(0.6 * sin(2 * M_PI * 55.0 * (i + 1) * k / 44100.0));
            if (cb_waveforms_compute(mono, n, 44100, &w) >= 0) {
                cb_library_waveforms_save(lib, t->id, &w);
                cb_waveforms_free(&w);
            }
        }
        free(mono);
    }
    if (ret >= 0) {
        CBPlaylist *p = cb_library_playlist_create(lib, lib->root_folder, "Fuzz set");
        CBPlaylistFolder *f = cb_library_folder_create(lib, lib->root_folder, "Folder");
        for (int i = 2; p && i >= 0; i--)
            cb_library_playlist_add(lib, p, tracks[i], -1);
        if (f)
            cb_library_playlist_create(lib, f, "Empty");
        cb_library_record_play(lib, tracks[1]);
    }
    free(music);
    free(libdir);
    return ret;
}

/* An artwork put into the plan: the catalog sends its bytes as they are. */
static int add_artwork(CBPlan *plan)
{
    CBPlanArtwork *a = calloc(1, sizeof(*a));
    if (!a)
        return -ENOMEM;
    *a = (CBPlanArtwork) { .id = 1, .jpeg80 = malloc(300), .size80 = 300, .jpeg240 = malloc(900),
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
    char *cmd = cb_asprintf("rm -rf '%s'", root);
    if (cmd && system(cmd) != 0)
        fprintf(stderr, "fuzz_db: could not remove %s\n", root);
    free(cmd);
}

/* ----------------------------------------------------------- requests */

static const uint16_t kinds[] = {
    CB_DB_SETUP, CB_DB_TEARDOWN, CB_DB_ROOT, CB_DB_GENRES, CB_DB_ARTISTS, CB_DB_ALBUMS,
    CB_DB_TRACKS, CB_DB_BPMS, CB_DB_RATINGS, CB_DB_DECADES, CB_DB_LABELS,
    CB_DB_COLORS, CB_DB_TIMES, CB_DB_BITRATES, CB_DB_HISTORIES, CB_DB_FILE_NAMES, CB_DB_KEYS,
    CB_DB_GENRE_ARTISTS, CB_DB_ARTIST_ALBUMS, CB_DB_ALBUM_TRACKS, CB_DB_PLAYLIST, CB_DB_BPM_RANGES,
    CB_DB_RATING_TRACKS, CB_DB_DECADE_YEARS, CB_DB_LABEL_ARTISTS,
    CB_DB_COLOR_TRACKS, CB_DB_TIME_TRACKS, CB_DB_BITRATE_TRACKS, CB_DB_HISTORY_TRACKS,
    CB_DB_RELATED_KEYS, CB_DB_GENRE_ARTIST_ALBUMS, CB_DB_ARTIST_ALBUM_TRACKS,
    CB_DB_BPM_TRACKS, CB_DB_YEAR_TRACKS, CB_DB_LABEL_ARTIST_ALBUMS, CB_DB_KEY_TRACKS, CB_DB_SEARCH,
    CB_DB_GENRE_TRACKS, CB_DB_ORIGINALS, CB_DB_LABEL_TRACKS, CB_DB_SORTS, CB_DB_ORIGINAL_ALBUMS,
    CB_DB_SEARCH_LIVE, CB_DB_ORIGINAL_TRACKS, CB_DB_REMIXERS, CB_DB_REMIXER_ALBUMS,
    CB_DB_ADDED_YEARS, CB_DB_REMIXER_TRACKS, CB_DB_ADDED_MONTHS, CB_DB_ADDED_DAYS,
    CB_DB_ADDED_TRACKS, CB_DB_METADATA, CB_DB_TRACK_INFO, CB_DB_DELIVERY, CB_DB_ARTWORK,
    CB_DB_PREVIEW, CB_DB_CUES, CB_DB_GRID, CB_DB_VBR, CB_DB_DETAIL,
    CB_DB_CUES_EXT, CB_DB_TAG, CB_DB_TAG_2EX, CB_DB_RENDER, CB_DB_USER_INFO, CB_DB_FILTER,
    CB_DB_ITEM_POSITION, CB_DB_SAVE_WAVEFORM, CB_DB_SAVE_CUE,
    CB_DB_SAVE_RATING, CB_DB_SAVE_VBR, CB_DB_SAVE_GRID_OFFSET, CB_DB_SAVE_CUE_EXT, CB_DB_SAVE_ATOM,
    CB_DB_UPDATE_ATOM, CB_DB_HISTORY_ADD, CB_DB_TAG_LIST_ADD, CB_DB_HISTORY_DELETE, CB_DB_ON_AIR,
    CB_DB_TAG_LIST_CLEAR, CB_DB_TAG_LIST, CB_DB_HISTORY_REMOVE, 0x1017, 0x2fff, 0x4101,
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
    case 2:  return CB_DB_ALL;
    case 3:  return below(2) ? 0x40000000u | below(5) : 0x7fffffff;
    case 4:  return cb_db_rmst((int)below(8), (int)below(10), (int)below(5), (int)below(3));
    }
    return rnd();
}

/* A request: of a kind players send (now and then any kind), its first
 * argument the r:m:s:t word at `location`, then a sort and ids as the
 * menus take them, or strings, blobs, anything. */
static void request(CBWr *w, uint32_t txid, int location, const uint32_t *ids, int nb_ids)
{
    CBDbMsg m = { .txid = below(8) ? txid : rnd(), .kind = kinds[below(CB_ARRAY_ELEMS(kinds))] };
    if (!below(40))
        m.kind = (uint16_t)rnd();
    uint8_t blob[64];
    for (size_t i = 0; i < sizeof(blob); i++)
        blob[i] = (uint8_t)rnd();
    m.nb_args = below(8) ? 2 + (int)below(4) : (int)below(13);
    m.args[0] = (CBDbArg) { .type = CB_DB_NUM, .num = cb_db_rmst(1, location, 3, 1) };
    for (int i = 1; i < m.nb_args; i++) {
        uint32_t pick = below(24);
        if (pick == 0)
            m.args[i] = (CBDbArg) { .type = CB_DB_STR, .str = texts[below(CB_ARRAY_ELEMS(texts))] };
        else if (pick == 1)
            m.args[i] = (CBDbArg) { .type = CB_DB_BLOB, .p = blob, .len = below(sizeof(blob)) };
        else if (i == 1 && pick < 16)
            m.args[i] = (CBDbArg) { .type = CB_DB_NUM, .num = below(18) };      /* a sort, or a track */
        else
            m.args[i] = (CBDbArg) { .type = CB_DB_NUM, .num = value(ids, nb_ids) };
    }
    if (m.kind == CB_DB_TAG || m.kind == CB_DB_TAG_2EX) {
        m.nb_args = 4;
        m.args[1] = (CBDbArg) { .type = CB_DB_NUM, .num = value(ids, nb_ids) };
        m.args[2] = (CBDbArg) { .type = CB_DB_NUM, .num = fourccs[below(CB_ARRAY_ELEMS(fourccs))] };
        m.args[3] = (CBDbArg) { .type = CB_DB_NUM, .num = exts[below(CB_ARRAY_ELEMS(exts))] };
    } else if (m.kind == CB_DB_SEARCH || m.kind == CB_DB_SEARCH_LIVE) {
        m.nb_args = 5;
        m.args[1] = (CBDbArg) { .type = CB_DB_NUM, .num = below(18) };
        m.args[2] = (CBDbArg) { .type = CB_DB_NUM, .num = rnd() };
        m.args[3] = (CBDbArg) { .type = CB_DB_STR, .str = texts[below(CB_ARRAY_ELEMS(texts))] };
    } else if (m.kind == CB_DB_SETUP) {
        m.txid = CB_DB_SETUP_TXID;
    }
    cb_dbmsg_write(w, &m);
}

/* A render of what the request before asked for, at its location: offsets
 * and limits in and past the menu, and up to 2^32 - 1. */
static void render(CBWr *w, uint32_t txid, int location)
{
    uint32_t offset = below(4) ? below(20) : rnd(), limit = below(4) ? below(40) : rnd();
    const uint32_t v[8] = { cb_db_rmst(1, location, 3, 1), offset, limit, 0, limit, 12,
                            below(2), below(20) };
    cb_dbmsg_numbers(w, txid, CB_DB_RENDER, v, below(3) ? 8 : below(2) ? 6 : (int)below(9));
}

/* Bit flips, a cut, a lying length or count or tag, bytes from elsewhere. */
static void mutate(CBWr *w)
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
                cb_wr_at_u32(w, at, below(2) ? rnd() : 0x7ffffff0u + below(16));
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
            uint8_t *d = cb_wr_room(w, len);
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
        if (len < CB_DB_GREETING_LEN || memcmp(p, cb_db_greeting, CB_DB_GREETING_LEN))
            fail("no greeting");
        pos = CB_DB_GREETING_LEN;
    }
    while (pos < len) {
        CBDbMsg m;
        int r = cb_dbmsg_parse(p + pos, len - pos, 64 << 20, &m);
        if (r <= 0)
            fail("a reply does not parse");
        stats.replies++;
        if (m.kind == CB_DB_MENU_ITEM) {
            stats.items++;
            CBDbRow row;
            if (cb_dbrow_parse(&m, &row) < 0)
                fail("a menu item does not read back");
            cb_dbrow_free(&row);
        }
        pos += (size_t)r;
    }
}

static void fuzz_session(CBCatalog **cat, const uint32_t *ids, int nb_ids, uint32_t round)
{
    CBDbSession *s;
    if (cb_dbsession_new(&s, cat, 17) < 0)
        fail("out of memory");
    CBWr in = cb_wr_dyn();
    bool greet = below(16) != 0;
    if (greet)
        cb_wr_bytes(&in, cb_db_greeting, CB_DB_GREETING_LEN);
    if (below(4)) {
        CBDbMsg setup = { .txid = CB_DB_SETUP_TXID, .kind = CB_DB_SETUP, .nb_args = 1 + (int)below(2) };
        setup.args[0] = (CBDbArg) { .type = CB_DB_NUM, .num = 1 + below(6) };
        setup.args[1] = (CBDbArg) { .type = CB_DB_NUM, .num = CB_DB_SETUP_MAGIC };
        cb_dbmsg_write(&in, &setup);
    }
    int nb = 1 + (int)below(12);
    bool mutating = !below(4);
    for (int i = 0; i < nb; i++) {
        CBWr one = cb_wr_dyn();
        int location = (int)below(10);
        request(&one, round * 32 + (uint32_t)i, location, ids, nb_ids);
        if (below(3))
            render(&one, round * 32 + 16 + (uint32_t)i, below(8) ? location : (int)below(10));
        if (mutating && !below(4))
            mutate(&one);
        cb_wr_bytes(&in, one.p, one.len);
        cb_wr_free(&one);
    }
    if (mutating && !below(4)) {
        /* Garbage, or random bytes, at the end. */
        int n = (int)below(64);
        for (int i = 0; i < n; i++)
            cb_wr_u8(&in, (uint8_t)rnd());
    }
    if (in.err)
        fail("out of memory");

    CBWr out = cb_wr_dyn();
    size_t pos = 0;
    int ret = 0;
    while (pos < in.len && ret >= 0) {
        size_t n = 1 + below(below(4) ? 200 : 4);
        n = CB_MIN(n, in.len - pos);
        ret = cb_dbsession_input(s, in.p + pos, n, &out);
        pos += n;
    }
    if (ret == -ENOMEM || out.err)
        fail("out of memory");
    stats.sessions++;
    stats.closed += ret < 0;
    stats.bytes += out.len;
    /* What was written before a closing error is still whole messages. */
    check_replies(out.p, out.len, greet && out.len >= CB_DB_GREETING_LEN);
    cb_wr_free(&out);
    cb_wr_free(&in);
    cb_dbsession_free(&s);
}

/* The codec alone: random bytes, and messages mutated; whatever parses
 * writes back into something that parses the same. */
static void fuzz_codec(void)
{
    CBWr w = cb_wr_dyn();
    if (below(3)) {
        request(&w, rnd(), (int)below(10), NULL, 0);
        mutate(&w);
    } else {
        int n = (int)below(80);
        for (int i = 0; i < n; i++)
            cb_wr_u8(&w, (uint8_t)rnd());
    }
    if (w.err)
        fail("out of memory");
    CBDbMsg m;
    size_t max = below(2) ? 1 << 20 : below(256);
    int r = cb_dbmsg_parse(w.p, w.len, max, &m);
    if (r > (int)w.len || r > (int)max)
        fail("a parse went past what it had");
    if (r > 0) {
        stats.parsed++;
        CBWr back = cb_wr_dyn();
        cb_dbmsg_write(&back, &m);
        CBDbMsg m2;
        int r2 = cb_dbmsg_parse(back.p, back.len, 1 << 20, &m2);
        if (r2 != (int)back.len || m2.nb_args != m.nb_args || m2.kind != m.kind || m2.txid != m.txid)
            fail("a parsed message does not write back");
        for (int i = 0; i < m.nb_args; i++)
            if (m.args[i].type != m2.args[i].type || m.args[i].num != m2.args[i].num ||
                m.args[i].len != m2.args[i].len ||
                (m.args[i].len && memcmp(m.args[i].p, m2.args[i].p, m.args[i].len)))
                fail("an argument changed on the way back");
        CBDbRow row;
        if (cb_dbrow_parse(&m, &row) >= 0)
            cb_dbrow_free(&row);
        char *s = cb_dbmsg_str(&m, (int)below(4));
        free(s);
        cb_wr_free(&back);
    }
    cb_wr_free(&w);
    uint8_t q[24];
    size_t qn = below(sizeof(q) + 1);
    for (size_t i = 0; i < qn; i++)
        q[i] = (uint8_t)rnd();
    if (cb_dbquery_parse(q, qn) > (int)qn)
        fail("the port query went past what it had");
}

/* The client's decoders, on the catalog's blobs mutated. */
static void fuzz_decoders(CBCatalog *cat, const uint32_t *ids, int nb_ids)
{
    CBWr b = cb_wr_dyn();
    uint32_t id = nb_ids ? ids[below((uint32_t)nb_ids)] : 1, count;
    int which = (int)below(3);
    if (which == 0)
        cb_catalog_grid(cat, id, &b);
    else if (which == 1)
        cb_catalog_cues_ext(cat, id, &b, &count);
    else
        cb_catalog_tag(cat, id, fourccs[below(CB_ARRAY_ELEMS(fourccs))], exts[below(3)], &b);
    if (below(4))
        mutate(&b);
    if (b.err)
        fail("out of memory");
    /* A copy of exactly its length, so a read past it is caught. */
    uint8_t *p = malloc(CB_MAX(b.len, 1));
    if (!p)
        fail("out of memory");
    if (b.len)
        memcpy(p, b.p, b.len);
    CBBeat *beats;
    CBPlanCue *cues;
    CBAnlzTag tag;
    int nb;
    if (cb_db_grid_parse(p, b.len, &beats, &nb) >= 0)
        free(beats);
    if (cb_db_cues_ext_parse(p, b.len, &cues, &nb) >= 0)
        free(cues);
    if (cb_db_tag_parse(p, b.len, &tag) >= 0 && (tag.raw < p || tag.raw + tag.raw_size > p + b.len))
        fail("a tag past its blob");
    free(p);
    cb_wr_free(&b);
}

int main(void)
{
    const char *s = getenv("CB_FUZZ_SEED"), *it = getenv("CB_FUZZ_ITERS");
    seed = s ? strtoull(s, NULL, 0) : 0x5eedc0de;
    long iters = it ? strtol(it, NULL, 0) : 300000;
    rng = seed ? seed : 1;

    const char *base = getenv("CB_TEST_DIR");
    char root[600];
    snprintf(root, sizeof(root), "%s", base ? base : "fuzz-link-db");
    remove_tree(root);
    CBLibrary lib = { 0 };
    int ret = make_library(root, &lib);
    if (ret < 0) {
        fprintf(stderr, "fuzz_db: cannot make the library: %s\n", strerror(-ret));
        cb_library_close(&lib);
        return 1;
    }
    CBPlan plan;
    ret = cb_plan_build_cb(&lib, lib.tracks, lib.nb_tracks, NULL, 0, NULL, false,
                           CB_PLAN_STORED, &plan, NULL, NULL);
    if (ret < 0 || add_artwork(&plan) < 0) {
        fprintf(stderr, "fuzz_db: cannot plan: %s\n", strerror(-ret));
        cb_library_close(&lib);
        return 1;
    }
    CBCatalog *cat = NULL;
    if (cb_catalog_new(&cat, &lib, &plan) < 0)
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

    int64_t t0 = cb_time_us();
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
            cb_catalog_free(&cat);
            if (below(4) && cb_catalog_new(&cat, &lib, &plan) < 0)
                fail("cannot make the catalog");
        }
    }
    fprintf(stderr, "fuzz_db: %ld rounds from seed %#llx in %.1f s: clean\n", iters,
            (unsigned long long)seed, (double)(cb_time_us() - t0) / 1e6);
    fprintf(stderr, "fuzz_db: %llu sessions (%llu closed early), %llu replies, %llu items, "
            "%.1f MB answered; %llu fuzzed messages parsed\n",
            (unsigned long long)stats.sessions, (unsigned long long)stats.closed,
            (unsigned long long)stats.replies, (unsigned long long)stats.items, stats.bytes / 1e6,
            (unsigned long long)stats.parsed);

    cb_catalog_free(&cat);
    cb_plan_free(&plan);
    cb_library_close(&lib);
    remove_tree(root);
    return 0;
}
