/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* OneLibrary: dj's tests. The SQLCipher codec opens rekordbox 7.2.18's
 * exportLibrary.db with its -wal (every page HMAC verifies), whose schema
 * and browse rows equal ours; a wrong key is rejected; our own database
 * decrypts, passes SQLite's integrity check and holds what the plan said. */

#include <stdio.h>

#include "rxbox/formats.h"

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
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

int main(void)
{
    reads_rekordbox();
    roundtrip();
    return test_result("onelibrary");
}
