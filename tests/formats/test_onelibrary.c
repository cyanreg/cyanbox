/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* OneLibrary: dj's tests. The SQLCipher codec opens rekordbox 7.2.18's
 * exportLibrary.db with its -wal (every page HMAC verifies), whose schema
 * and browse rows equal ours; a wrong key is rejected; our own database
 * decrypts, passes SQLite's integrity check and holds what the plan said. */

#include <stdio.h>

#include <libavutil/aes.h>
#include <libavutil/hmac.h>
#include <libavutil/mem.h>

#include "rxbox/formats.h"

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "src/formats/sha512.h"
#include "tests/formats/test.h"

static int64_t query_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static char *query_text(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    char *v = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = rx_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return v;
}

static void reads_rekordbox(void)
{
    uint8_t *db_bytes, *wal;
    size_t db_size, wal_size;
    if (rx_file_read("tests/data/rekordbox-7.2.18-exportLibrary.db", &db_bytes, &db_size) < 0 ||
        rx_file_read("tests/data/rekordbox-7.2.18-exportLibrary.db-wal", &wal, &wal_size) < 0) {
        fprintf(stderr, "cannot read the rekordbox fixtures\n");
        failures++;
        return;
    }
    uint8_t *plain;
    size_t plain_size;
    CHECK_EQ(rx_sqlcipher_decrypt(db_bytes, db_size, rx_onelibrary_passphrase, wal, wal_size,
                                  &plain, &plain_size), 0);
    sqlite3 *db;
    CHECK_EQ(rx_onelibrary_open_plain(plain, plain_size, &db), 0);
    char *ok = query_text(db, "PRAGMA integrity_check");
    CHECK_STR(ok, "ok");
    free(ok);
    CHECK_EQ(query_int(db, "select count(*) from content"), 34);

    sqlite3_stmt *st;
    int i = 0;
    sqlite3_prepare_v2(db, "select sql from sqlite_master order by rowid", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *sql = (const char *)sqlite3_column_text(st, 0);
        CHECK(i < rx_onelibrary_schema_count && sql && !strcmp(sql, rx_onelibrary_schema[i]));
        i++;
    }
    sqlite3_finalize(st);
    CHECK_EQ(i, rx_onelibrary_schema_count);

    i = 0;
    sqlite3_prepare_v2(db, "select * from menuItem order by rowid", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW && i < 27) {
        char name[64];
        snprintf(name, sizeof(name), "\xEF\xBF\xBA%s\xEF\xBF\xBB", rx_onelibrary_menu_items[i].name);
        CHECK_EQ(sqlite3_column_int(st, 0), rx_onelibrary_menu_items[i].id);
        CHECK_EQ(sqlite3_column_int(st, 1), rx_onelibrary_menu_items[i].kind);
        CHECK_STR((const char *)sqlite3_column_text(st, 2), name);
        i++;
    }
    sqlite3_finalize(st);
    CHECK_EQ(i, 27);
    i = 0;
    sqlite3_prepare_v2(db, "select * from category order by rowid", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW && i < 22) {
        for (int k = 0; k < 4; k++)
            CHECK_EQ(sqlite3_column_int(st, k), rx_onelibrary_category[i][k]);
        i++;
    }
    sqlite3_finalize(st);
    CHECK_EQ(i, 22);
    i = 0;
    sqlite3_prepare_v2(db, "select * from sort order by rowid", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW && i < 17) {
        for (int k = 0; k < 5; k++)
            CHECK_EQ(sqlite3_column_int(st, k), rx_onelibrary_sort[i][k]);
        i++;
    }
    sqlite3_finalize(st);
    CHECK_EQ(i, 17);
    sqlite3_close(db);

    RxOneLibTrack *tracks;
    int nb;
    CHECK_EQ(rx_onelibrary_read(plain, plain_size, &tracks, &nb), 0);
    CHECK_EQ(nb, 34);
    for (int k = 0; k < nb; k++) {
        CHECK(tracks[k].path && tracks[k].path[0] == '/');
        free(tracks[k].path);
        free(tracks[k].title);
    }
    free(tracks);
    free(plain);

    /* dj's test_sqlcipher_wrong_key, on the first page alone. */
    CHECK_EQ(rx_sqlcipher_decrypt(db_bytes, 4096, "wrong", NULL, 0, &plain, &plain_size),
             RX_ERROR(EBADMSG));
    free(db_bytes);
    free(wal);
}

static void roundtrip(void)
{
    enum { NT = 300 };
    RxTrack *tr = calloc(NT, sizeof(*tr));
    RxPlanTrack *pt = calloc(NT, sizeof(*pt));
    char (*titles)[32] = calloc(NT, 32);
    char (*paths)[64] = calloc(NT, 64);
    RxTempoSection sec = { .start = 0.1, .bpm = 128.0, .beat = 1 };
    RxPlanCue cues[2] = {
        { .time = 1.0, .hot = 0, .loop_end = -1 },
        { .time = 2.0, .hot = -1, .loop_end = 2.0 + 15.0 / 8.0 },
    };
    uint32_t tag = 999;
    for (int i = 1; i <= NT; i++) {
        snprintf(titles[i - 1], 32, "Träck %d", i);
        snprintf(paths[i - 1], 64, "/Contents/A/B/%d.flac", i);
        tr[i - 1].meta.title = titles[i - 1];
        pt[i - 1] = (RxPlanTrack) {
            .id          = (uint32_t)i,
            .track       = &tr[i - 1],
            .usb_path    = paths[i - 1],
            .anlz_dir    = "/x",
            .file_type   = 5,
            .bit_rate    = 1411,
            .bit_depth   = 16,
            .sample_rate = 44100,
            .duration    = 300.0,
            .grid        = { .sections = &sec, .nb_sections = 1, .beats_per_bar = 4 },
            .cues        = cues,
            .nb_cues     = 2,
            .tag_ids     = &tag,
            .nb_tag_ids  = 1,
        };
    }
    uint32_t list[3] = { 3, 1, 2 };
    RxPlanPlaylist pl[2] = {
        { .id = 1, .parent_id = 0, .sort_order = 0, .is_folder = true, .name = "Folder" },
        { .id = 2, .parent_id = 1, .sort_order = 0, .name = "List", .track_ids = list, .nb = 3 },
    };
    RxPlanTag tags[5] = {
        { .id = 1, .name = "Genre", .position = 0, .is_category = true },
        { .id = 2, .name = "Components", .position = 1, .is_category = true },
        { .id = 3, .name = "Situation", .position = 2, .is_category = true },
        { .id = 4, .name = "Tags", .position = 3, .is_category = true },
        { .id = 999, .name = "vocal", .category = 4, .position = 0 },
    };
    RxPlanTagTrack tt[NT];
    for (int i = 0; i < NT; i++)
        tt[i] = (RxPlanTagTrack) { .track_id = (uint32_t)i + 1, .tag_id = 999 };
    RxPlan plan = {
        .tracks            = pt,
        .nb_tracks         = NT,
        .playlists         = pl,
        .nb_playlists      = 2,
        .tags              = tags,
        .nb_tags           = 5,
        .tag_tracks        = tt,
        .nb_tag_tracks     = NT,
        .hot_cue_auto_load = true,
        .background_color  = 7,
        .device_name       = "STICK",
        .library_id        = 1234,
    };
    snprintf(plan.date, sizeof(plan.date), "2026-10-06");

    uint8_t *enc;
    size_t enc_size;
    CHECK_EQ(rx_onelibrary_build(&plan, &enc, &enc_size), 0);
    CHECK(enc_size % 4096 == 0 && memcmp(enc, "SQLite format 3", 16));
    uint8_t *plain;
    size_t plain_size;
    CHECK_EQ(rx_sqlcipher_decrypt(enc, enc_size, rx_onelibrary_passphrase, NULL, 0, &plain, &plain_size), 0);
    CHECK(plain[20] == 80 && plain[18] == 2 && plain[19] == 2);
    CHECK_EQ(rx_rd_be32(plain + 96), RX_SQLCIPHER_SQLITE_VERSION);
    sqlite3 *db;
    CHECK_EQ(rx_onelibrary_open_plain(plain, plain_size, &db), 0);
    char *ok = query_text(db, "PRAGMA integrity_check");
    CHECK_STR(ok, "ok");
    free(ok);
    CHECK_EQ(query_int(db, "select count(*) from content"), NT);
    sqlite3_stmt *st;
    int64_t want[3] = { 3, 1, 2 };
    int i = 0;
    sqlite3_prepare_v2(db, "select content_id from playlist_content where playlist_id = 2 order by sequenceNo",
                       -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW && i < 3)
        CHECK_EQ(sqlite3_column_int64(st, 0), want[i++]);
    sqlite3_finalize(st);
    CHECK_EQ(i, 3);

    /* kind, beatLoopNumerator, inUsec, outUsec: hot cue A (kind 1) with
     * out = in, the memory loop of 4 beats at 128 BPM. */
    sqlite3_prepare_v2(db, "select kind, beatLoopNumerator, inUsec, outUsec from cue where content_id = 1 "
                       "order by cue_id", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1 &&
          sqlite3_column_type(st, 1) == SQLITE_NULL && sqlite3_column_int64(st, 2) == 1000000 &&
          sqlite3_column_int64(st, 3) == 1000000);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0 &&
          sqlite3_column_int(st, 1) == 4 && sqlite3_column_int64(st, 2) == 2000000 &&
          sqlite3_column_int64(st, 3) == 3875000);
    sqlite3_finalize(st);

    CHECK_EQ(query_int(db, "select backGroundColorType from property"), 7);
    CHECK_EQ(query_int(db, "select numberOfContents from property"), NT);
    CHECK_EQ(query_int(db, "select myTagMasterDBID from property"), 1234 ^ 0x5A5A5A5A);
    CHECK_EQ(query_int(db, "select masterDbId from content where content_id = 5"), 1234);
    CHECK_EQ(query_int(db, "select isHotCueAutoLoadOn from content where content_id = 5"), 1);
    CHECK_EQ(query_int(db, "select count(*) from myTag"), 5);
    CHECK_EQ(query_int(db, "select myTag_id_parent from myTag where myTag_id = 999"), 4);
    CHECK_EQ(query_int(db, "select count(*) from myTag_content where myTag_id = 999"), NT);
    char *dev = query_text(db, "select deviceName from property");
    CHECK_STR(dev, "STICK");
    free(dev);
    char *title = query_text(db, "select title from content where content_id = 7");
    CHECK_STR(title, "Träck 7");
    free(title);
    sqlite3_close(db);

    RxOneLibTrack *tracks;
    int nb;
    CHECK_EQ(rx_onelibrary_read(plain, plain_size, &tracks, &nb), 0);
    CHECK_EQ(nb, NT);
    CHECK(nb == NT && tracks[0].nb_cues == 2 && !strcmp(tracks[0].path, "/Contents/A/B/1.flac"));
    for (int k = 0; k < nb; k++) {
        free(tracks[k].path);
        free(tracks[k].title);
    }
    free(tracks);

    /* A fixed salt gives a file whose first 16 bytes are that salt. */
    uint8_t salt[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t *enc2;
    size_t enc2_size;
    CHECK_EQ(rx_sqlcipher_encrypt(plain, plain_size, "pass", salt, &enc2, &enc2_size), 0);
    CHECK(!memcmp(enc2, salt, 16));
    uint8_t *back;
    size_t back_size;
    CHECK_EQ(rx_sqlcipher_decrypt(enc2, enc2_size, "pass", NULL, 0, &back, &back_size), 0);
    /* The reserve comes back zeroed; everything before it on every page is
     * the plaintext. */
    for (size_t p = 0; p < plain_size / 4096; p++)
        CHECK(!memcmp(back + p * 4096, plain + p * 4096, 4096 - 80));
    free(back);
    free(enc2);
    free(plain);
    free(enc);
    free(tr);
    free(pt);
    free(titles);
    free(paths);
}

static void check_hex(const uint8_t *d, size_t n, const char *want, const char *what)
{
    char hex[2 * 128 + 1];
    for (size_t i = 0; i < n; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    if (strcmp(hex, want)) {
        fprintf(stderr, "%s: %s, want %s\n", what, hex, want);
        failures++;
    }
}

/* sha512.h against the published vectors: FIPS 180-4's examples (and the
 * empty message), RFC 4231's HMAC-SHA-512 cases 1-4 and 6 (a key longer
 * than a block), and PBKDF2-HMAC-SHA512 of "password"/"salt" as the RFC
 * 6070 cases give it for SHA-1 (values from Python's hashlib, which match
 * the commonly published ones). */
static void crypto_vectors(void)
{
    static const char m896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                               "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    static const struct { const char *msg; const char *want; } sha[] = {
        { "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                 "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f" },
        { m896,  "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                 "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909" },
        { "",    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                 "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" },
    };
    uint8_t out[128];
    for (size_t i = 0; i < RX_ARRAY_ELEMS(sha); i++) {
        RxSHA512 s;
        rx_sha512_init(&s);
        rx_sha512_update(&s, (const uint8_t *)sha[i].msg, strlen(sha[i].msg));
        rx_sha512_final(&s, out);
        check_hex(out, 64, sha[i].want, "sha512");
    }
    /* The two-block message in uneven pieces, through the partial-block
     * paths of update. */
    RxSHA512 pieces;
    rx_sha512_init(&pieces);
    for (size_t pos = 0, step = 1; pos < strlen(m896); pos += step, step = step * 3 % 37 + 1)
        rx_sha512_update(&pieces, (const uint8_t *)m896 + pos, RX_MIN(step, strlen(m896) - pos));
    rx_sha512_final(&pieces, out);
    check_hex(out, 64, sha[1].want, "sha512 in pieces");

    uint8_t key1[20], key3[20], key4[25], key6[131], data3[50], data4[50];
    memset(key1, 0x0b, sizeof(key1));
    memset(key3, 0xaa, sizeof(key3));
    memset(data3, 0xdd, sizeof(data3));
    for (int i = 0; i < 25; i++)
        key4[i] = (uint8_t)(i + 1);
    memset(data4, 0xcd, sizeof(data4));
    memset(key6, 0xaa, sizeof(key6));
    static const char *case6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    const struct {
        const uint8_t *key, *data;
        size_t key_len, len;
        const char *want;
    } mac[] = {
        { key1, (const uint8_t *)"Hi There", sizeof(key1), 8,
          "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
          "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854" },
        { (const uint8_t *)"Jefe", (const uint8_t *)"what do ya want for nothing?", 4, 28,
          "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
          "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737" },
        { key3, data3, sizeof(key3), sizeof(data3),
          "fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39"
          "bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb" },
        { key4, data4, sizeof(key4), sizeof(data4),
          "b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3db"
          "a91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd" },
        { key6, (const uint8_t *)case6, sizeof(key6), strlen(case6),
          "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
          "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(mac); i++) {
        RxHMACSHA512 h;
        rx_hmac_sha512_init(&h, mac[i].key, mac[i].key_len);
        /* Twice: final leaves the context keyed for the next message. */
        for (int k = 0; k < 2; k++) {
            rx_hmac_sha512_update(&h, mac[i].data, mac[i].len);
            rx_hmac_sha512_final(&h, out);
            check_hex(out, 64, mac[i].want, "hmac-sha512");
        }
    }

    static const struct { unsigned iter; size_t len; const char *pass, *salt, *want; } kdf[] = {
        { 1, 64, "password", "salt",
          "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252"
          "c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce" },
        { 2, 64, "password", "salt",
          "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53c"
          "f76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e" },
        { 4096, 64, "password", "salt",
          "d197b1b33db0143e018b12f3d1d1479e6cdebdcc97c5c0f87f6902e072f457b5"
          "143f30602641b3d55cd335988cb36b84376060ecd532e039b742a239434af2d5" },
        /* Two blocks of output, the second cut short. */
        { 4096, 100, "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt",
          "8c0511f4c6e597c6ac6315d8f0362e225f3c501495ba23b868c005174dc4ee71"
          "115b59f9e60cd9532fa33e0f75aefe30225c583a186cd82bd4daea9724a3d3b8"
          "04f75bdd41494fa324cab24bcc680fb3b96a30cf5d21fac3c2875913919f3399"
          "b1d9ce7e" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(kdf); i++) {
        rx_pbkdf2_sha512((const uint8_t *)kdf[i].pass, strlen(kdf[i].pass),
                         (const uint8_t *)kdf[i].salt, strlen(kdf[i].salt), kdf[i].iter,
                         out, kdf[i].len);
        check_hex(out, kdf[i].len, kdf[i].want, "pbkdf2-hmac-sha512");
    }
}

/* The keys of the database whose page 1 starts with `salt`
 * (docs/onelibrary.md 1.1). */
static void frame_keys(const uint8_t salt[16], uint8_t key[32], uint8_t mac[32])
{
    uint8_t mac_salt[16];
    for (int i = 0; i < 16; i++)
        mac_salt[i] = salt[i] ^ 0x3A;
    rx_pbkdf2_sha512((const uint8_t *)rx_onelibrary_passphrase, strlen(rx_onelibrary_passphrase),
                     salt, 16, 256000, key, 32);
    rx_pbkdf2_sha512(key, 32, mac_salt, 16, 2, mac, 32);
}

/* A WAL frame the way SQLCipher writes one: page `pgno` (zeros), encrypted
 * and authenticated with the database's keys, so that only the reader's
 * own checks can refuse it. The tag is av_hmac's, apart from the codec's
 * HMAC. */
static void wal_frame(uint8_t *f, const uint8_t key[32], const uint8_t mac[32], uint32_t pgno,
                      uint32_t commit, const uint8_t wal_salt[8])
{
    memset(f, 0, 24 + 4096);
    rx_wr_be32(f, pgno);
    rx_wr_be32(f + 4, commit);
    memcpy(f + 8, wal_salt, 8);
    uint8_t *page = f + 24, plain[4096 - 80] = { 0 }, iv[16] = { 7 };
    memcpy(page + 4096 - 80, iv, 16);
    struct AVAES *aes = av_aes_alloc();
    av_aes_init(aes, key, 256, 0);
    av_aes_crypt(aes, page, plain, sizeof(plain) / 16, iv, 0);
    av_free(aes);
    uint8_t buf[4096 - 80 + 16 + 4];
    memcpy(buf, page, 4096 - 80 + 16);
    rx_wr_le32(buf + 4096 - 80 + 16, pgno);
    AVHMAC *h = av_hmac_alloc(AV_HMAC_SHA512);
    av_hmac_calc(h, buf, sizeof(buf), mac, 32, page + 4096 - 80 + 16, 64);
    av_hmac_free(h);
}

/* A -wal next to the database is input another program wrote: a frame
 * naming a page far beyond the file must not make the reader allocate
 * (and zero) the space up to it, and frames of an older WAL generation
 * (other salts, docs: sqlite.org/fileformat2.html#walformat) or after a
 * torn one are not applied, as SQLite would not apply them. */
static void hostile_wal(void)
{
    uint8_t *db_bytes, *wal;
    size_t db_size, wal_size;
    if (rx_file_read("tests/data/rekordbox-7.2.18-exportLibrary.db", &db_bytes, &db_size) < 0 ||
        rx_file_read("tests/data/rekordbox-7.2.18-exportLibrary.db-wal", &wal, &wal_size) < 0) {
        fprintf(stderr, "cannot read the rekordbox fixtures\n");
        failures++;
        return;
    }
    uint8_t *plain, *ref;
    size_t plain_size, ref_size;
    CHECK_EQ(rx_sqlcipher_decrypt(db_bytes, db_size, rx_onelibrary_passphrase, wal, wal_size, &ref, &ref_size), 0);
    uint8_t key[32], mac[32];
    frame_keys(db_bytes, key, mac);

    /* The fixture's WAL plus one committed frame for page 2^18 (1 GiB). */
    size_t n = wal_size + 24 + 4096;
    uint8_t *w = malloc(n);
    memcpy(w, wal, wal_size);
    wal_frame(w + wal_size, key, mac, 1u << 18, 1u << 18, wal + 16);
    int ret = rx_sqlcipher_decrypt(db_bytes, db_size, rx_onelibrary_passphrase, w, n, &plain, &plain_size);
    CHECK(ret < 0 || plain_size == ref_size);
    if (ret >= 0)
        free(plain);

    /* A frame of an older generation (salts differ) after the valid ones:
     * ignored, the database is the fixture's. */
    uint8_t old_salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    wal_frame(w + wal_size, key, mac, 2, (uint32_t)(db_size / 4096), old_salt);
    CHECK_EQ(rx_sqlcipher_decrypt(db_bytes, db_size, rx_onelibrary_passphrase, w, n, &plain, &plain_size), 0);
    CHECK(plain_size == ref_size && !memcmp(plain, ref, ref_size));
    free(plain);

    /* A torn last frame (its page does not authenticate): the committed
     * frames before it still count. */
    memcpy(w, wal, wal_size);
    wal_frame(w + wal_size, key, mac, 2, (uint32_t)(db_size / 4096), wal + 16);
    w[wal_size + 24 + 100] ^= 1;
    CHECK_EQ(rx_sqlcipher_decrypt(db_bytes, db_size, rx_onelibrary_passphrase, w, n, &plain, &plain_size), 0);
    CHECK(plain_size == ref_size && !memcmp(plain, ref, ref_size));
    free(plain);

    free(w);
    free(ref);
    free(db_bytes);
    free(wal);
}

/* A track without artwork, key, colour, album or year: NULL ids and colour
 * 0 (docs/onelibrary.md 3.1, "color_id 0 = none"). */
static void bare_track(void)
{
    RxTrack tr = { 0 };
    RxPlanTrack pt = {
        .id        = 42,
        .track     = &tr,
        .usb_path  = "/Contents/UnknownArtist/UnknownAlbum/x.wav",
        .anlz_dir  = "/PIONEER/USBANLZ/P000/00000000",
        .file_type = 11,
        .duration  = 1.0,
    };
    RxPlan plan = { .tracks = &pt, .nb_tracks = 1 };
    uint8_t *plain;
    size_t plain_size;
    CHECK_EQ(rx_onelibrary_build_plain(&plan, &plain, &plain_size), 0);
    sqlite3 *db;
    CHECK_EQ(rx_onelibrary_open_plain(plain, plain_size, &db), 0);
    CHECK_EQ(query_int(db, "select count(*) from content where content_id = 42 and image_id is null and "
                           "key_id is null and album_id is null and artist_id_artist is null and "
                           "genre_id is null and releaseYear is null and color_id = 0"), 1);
    CHECK_EQ(query_int(db, "select count(*) from image"), 0);
    CHECK_EQ(query_int(db, "select count(*) from cue"), 0);
    char *title = query_text(db, "select title from content");
    CHECK_STR(title, "");
    free(title);
    sqlite3_close(db);
    free(plain);
}

int main(void)
{
    crypto_vectors();
    reads_rekordbox();
    roundtrip();
    hostile_wal();
    bare_track();
    return test_result("onelibrary");
}
