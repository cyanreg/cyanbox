/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Playlists and folders (src/core/playlist.c): create, rename, entries
 * (add, move, remove, no duplicates), reparenting, deletion with its
 * cascade, positions dense after every edit (checked in SQL), the tree
 * after a reopen, and m3u8 export and import (relative paths, order
 * prefix, BOM, CRLF, file:// URLs, missing and new files), with export
 * and re-import round trips from every kind of directory relative to the
 * audio. */

#include "cyanbox/library.h"

#include <sqlite3.h>

#include "src/core/core.h"
#include "util.h"

static int sql_int(const CBLibrary *lib, const char *sql)
{
    sqlite3_stmt *st;
    int n = -1;
    if (sqlite3_prepare_v2(lib->db->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

/* n distinct positions in [0, n) among each set of siblings are 0..n-1. */
static bool dense(const CBLibrary *lib)
{
    return sql_int(lib, "SELECT count(*) FROM playlist p WHERE position < 0 OR position >= "
                        "(SELECT count(*) FROM playlist q WHERE q.parent_id IS p.parent_id "
                        "AND q.is_folder = p.is_folder)") == 0 &&
           sql_int(lib, "SELECT count(*) FROM (SELECT count(*) c FROM playlist "
                        "GROUP BY parent_id, is_folder, position HAVING c > 1)") == 0 &&
           sql_int(lib, "SELECT count(*) FROM playlist_entry e WHERE position < 0 OR position >= "
                        "(SELECT count(*) FROM playlist_entry f WHERE f.playlist_id = e.playlist_id)") == 0;
}

static bool ids_are(const CBPlaylist *p, int nb, const uint64_t *ids)
{
    if (!p || p->nb != nb)
        return false;
    for (int i = 0; i < nb; i++) {
        if (p->track_ids[i] != ids[i])
            return false;
    }
    return true;
}

#define IDS(...) (int)CB_ARRAY_ELEMS(((uint64_t[]) { __VA_ARGS__ })), ((uint64_t[]) { __VA_ARGS__ })

static void test_tree(const char *root, const char *music)
{
    CBLibrary lib;
    CHECK(cb_library_open(&lib, root) == 0);
    CHECK(cb_library_add_path(&lib, music, NULL, NULL) == 4);
    CBTrack *t[5] = { NULL };
    for (int i = 1; i <= 4; i++)
        t[i] = cb_library_find_id(&lib, (uint64_t)i);
    CHECK(t[1] && t[4]);
    if (!t[1] || !t[4])
        return;

    CBPlaylistFolder *genres = cb_library_folder_create(&lib, NULL, "Genres");
    CBPlaylistFolder *house = cb_library_folder_create(&lib, genres, "House");
    CBPlaylist *warmup = cb_library_playlist_create(&lib, NULL, "Warmup");
    CBPlaylist *peak = cb_library_playlist_create(&lib, NULL, "Peak");
    CBPlaylist *deep = cb_library_playlist_create(&lib, house, "Deep");
    CHECK(genres && house && warmup && peak && deep);
    CHECK(!cb_library_playlist_create(&lib, NULL, "  "));
    CHECK(lib.root_folder->nb_folders == 1 && lib.root_folder->nb_playlists == 2);
    CHECK(cb_library_folder_of(&lib, deep) == house && cb_library_folder_of(&lib, warmup) == lib.root_folder);
    CHECK(dense(&lib));

    /* Entries. */
    for (int i = 1; i <= 3; i++)
        CHECK(cb_library_playlist_add(&lib, warmup, t[i], -1) == 0);
    CHECK(cb_library_playlist_add(&lib, warmup, t[4], 0) == 0);
    CHECK(ids_are(warmup, IDS(4, 1, 2, 3)));
    CHECK(cb_library_playlist_add(&lib, warmup, t[1], -1) == CB_ERROR(EEXIST));
    CHECK(cb_library_playlist_move(&lib, warmup, 0, 3) == 0);
    CHECK(ids_are(warmup, IDS(1, 2, 3, 4)));
    CHECK(cb_library_playlist_move(&lib, warmup, 3, 1) == 0);
    CHECK(ids_are(warmup, IDS(1, 4, 2, 3)));
    CHECK(cb_library_playlist_remove_at(&lib, warmup, 1) == 0);
    CHECK(ids_are(warmup, IDS(1, 2, 3)));
    CHECK(cb_library_playlist_remove_at(&lib, warmup, 5) == CB_ERROR(EINVAL));
    CHECK(cb_library_playlist_add(&lib, deep, t[4], -1) == 0);
    CHECK(cb_library_playlist_add(&lib, peak, t[2], -1) == 0);
    CHECK(dense(&lib));

    CHECK(cb_library_playlist_rename(&lib, warmup, "Warm-up") == 0 && !strcmp(warmup->name, "Warm-up"));
    CHECK(cb_library_folder_rename(&lib, genres, "Styles") == 0 && !strcmp(genres->name, "Styles"));
    CHECK(cb_library_playlist_rename(&lib, warmup, "") == CB_ERROR(EINVAL));
    CHECK(cb_library_folder_rename(&lib, lib.root_folder, "x") == CB_ERROR(EINVAL));

    /* Reparenting: Peak to the top of House; a folder never into itself. */
    CHECK(cb_library_playlist_reparent(&lib, peak, house, 0) == 0);
    CHECK(house->nb_playlists == 2 && house->playlists[0] == peak && house->playlists[1] == deep);
    CHECK(lib.root_folder->nb_playlists == 1 && lib.root_folder->playlists[0] == warmup);
    CHECK(cb_library_folder_reparent(&lib, genres, house, -1) == CB_ERROR(EINVAL));
    CHECK(cb_library_folder_reparent(&lib, genres, genres, -1) == CB_ERROR(EINVAL));
    CBPlaylistFolder *other = cb_library_folder_create(&lib, NULL, "Other");
    CHECK(cb_library_folder_reparent(&lib, house, other, -1) == 0);
    CHECK(!genres->nb_folders && other->nb_folders == 1 && other->folders[0] == house);
    CHECK(cb_library_folder_reparent(&lib, other, NULL, 0) == 0);
    CHECK(lib.root_folder->folders[0] == other && lib.root_folder->folders[1] == genres);
    CHECK(cb_library_playlist_reparent(&lib, warmup, NULL, 5) == 0);   /* same place */
    CHECK(dense(&lib));

    CBPlaylist *all[8];
    CHECK(cb_library_playlists_flat(&lib, NULL, 0) == 3);
    CHECK(cb_library_playlists_flat(&lib, all, 8) == 3);
    CHECK(all[0] == peak && all[1] == deep && all[2] == warmup);
    CHECK(cb_library_playlists_flat(&lib, all, 1) == 3 && all[0] == peak);
    CHECK(cb_library_playlist_find(&lib, "Deep") == deep && !cb_library_playlist_find(&lib, "Nope"));

    /* Deleting a playlist renumbers its siblings. */
    CHECK(cb_library_playlist_delete(&lib, peak) == 0);
    CHECK(house->nb_playlists == 1 && house->playlists[0] == deep);
    CHECK(dense(&lib));
    CHECK(cb_library_check(&lib) == 0);
    cb_library_close(&lib);

    /* The same tree after a reopen. */
    CHECK(cb_library_open(&lib, root) == 0);
    CBPlaylistFolder *r = lib.root_folder;
    CHECK(r->nb_folders == 2 && r->nb_playlists == 1);
    if (r->nb_folders == 2 && r->nb_playlists == 1) {
        CHECK_STR(r->folders[0]->name, "Other");
        CHECK_STR(r->folders[1]->name, "Styles");
        CHECK(r->folders[0]->nb_folders == 1 && !strcmp(r->folders[0]->folders[0]->name, "House"));
        CBPlaylistFolder *h = r->folders[0]->folders[0];
        CHECK(h->nb_playlists == 1 && !strcmp(h->playlists[0]->name, "Deep"));
        CHECK(ids_are(h->playlists[0], IDS(4)));
        CHECK_STR(r->playlists[0]->name, "Warm-up");
        CHECK(ids_are(r->playlists[0], IDS(1, 2, 3)));

        /* Folder delete takes everything in it, in memory and on disk. */
        CHECK(cb_library_folder_delete(&lib, r->folders[0]) == 0);
        CHECK(r->nb_folders == 1 && !strcmp(r->folders[0]->name, "Styles"));
        CHECK(sql_int(&lib, "SELECT count(*) FROM playlist") == 2);
        CHECK(sql_int(&lib, "SELECT count(*) FROM playlist_entry") == 3);
        CHECK(cb_library_folder_delete(&lib, r) == CB_ERROR(EINVAL));
        CHECK(dense(&lib));
    }

    /* Damaged positions are renumbered at open. */
    sqlite3_exec(lib.db->db, "UPDATE playlist SET position = position + 7; "
                             "UPDATE playlist_entry SET position = position * 3 + 10", NULL, NULL, NULL);
    cb_library_close(&lib);
    CHECK(cb_library_open(&lib, root) == 0);
    CHECK(dense(&lib));
    CHECK(lib.root_folder->nb_playlists == 1 && ids_are(lib.root_folder->playlists[0], IDS(1, 2, 3)));
    CHECK(cb_library_check(&lib) == 0);
    cb_library_close(&lib);
}

static void test_m3u(const char *dir, const char *root, const char *music)
{
    CBLibrary lib;
    CHECK(cb_library_open(&lib, root) == 0);
    CBPlaylist *p = cb_library_playlist_find(&lib, "Warm-up");
    CHECK(p != NULL);
    if (!p)
        return;

    char *exp_dir = cb_asprintf("%s/export", dir);
    cb_mkdir_p(exp_dir);
    char *m3u = cb_asprintf("%s/03 Warm.m3u8", exp_dir);
    CHECK(cb_library_playlist_export_m3u(&lib, p, m3u) == 0);
    uint8_t *text;
    size_t size;
    CHECK(cb_file_read(m3u, &text, &size) == 0);
    CHECK(cb_str_starts((char *)text, "#EXTM3U\n#EXTINF:1,"));
    CHECK(strstr((char *)text, "\n../music/a.wav\n") && strstr((char *)text, "\n../music/b.wav\n"));
    free(text);

    /* Import: the order prefix only orders, the tracks are the same. */
    CBPlaylist *in;
    CHECK(cb_library_playlist_import_m3u(&lib, NULL, m3u, &in) == 3);
    CHECK(in && !strcmp(in->name, "Warm") && ids_are(in, IDS(1, 2, 3)));

    /* Other shapes: BOM, CRLF, comments, absolute, file:// with %20, a
     * duplicate, a missing file, and a file not yet in the library. */
    char *fresh = cb_asprintf("%s/new one.wav", music);
    test_write_wav(fresh, 0.25, 44100, 2, "Fresh", NULL, NULL);
    char *hand = cb_asprintf("%s/hand.m3u", exp_dir);
    FILE *f = fopen(hand, "w");
    fprintf(f, "\xef\xbb\xbf#EXTM3U\r\n\r\n# a comment\r\n../music/b.wav\r\n%s/a.wav\r\n"
               "file://%s/new%%20one.wav\r\n  ../music/./sub/../b.wav  \r\n../music/gone.wav\r\n",
            music, music);
    fclose(f);
    int before = lib.nb_tracks;
    CHECK(cb_library_playlist_import_m3u(&lib, NULL, hand, &in) == 3);
    CHECK(lib.nb_tracks == before + 1);
    CBTrack *t = cb_library_find_path(&lib, fresh);
    CHECK(t && !strcmp(t->meta.title, "Fresh"));
    CHECK(in && !strcmp(in->name, "hand") && t && ids_are(in, IDS(2, 1, t->id)));
    CHECK(dense(&lib));
    CHECK(cb_library_check(&lib) == 0);
    cb_library_close(&lib);

    free(fresh);
    free(hand);
    free(m3u);
    free(exp_dir);
}

static int nb_playlists(const CBLibrary *lib)
{
    return sql_int(lib, "SELECT count(*) FROM playlist WHERE is_folder = 0");
}

/* Export wherever the playlist file goes relative to the audio, and read
 * it back: the same tracks every time. */
static void test_m3u_roundtrip(const char *dir, const char *root)
{
    CBLibrary lib;
    CHECK(cb_library_open(&lib, root) == 0);
    CBPlaylist *p = cb_library_playlist_find(&lib, "Warm-up");
    CHECK(p != NULL);
    if (!p)
        return;

    static const struct { const char *file, *line; } places[] = {
        { "list.m3u8",               "\nmusic/a.wav\n" },          /* above the audio */
        { "music/here.m3u8",         "\na.wav\n" },                /* next to it */
        { "export/deep/er/x.m3u8",   "\n../../../music/a.wav\n" }, /* elsewhere, deeper */
        { "export/side.m3u8",        "\n../music/a.wav\n" },       /* a sibling */
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(places); i++) {
        char *m3u = cb_asprintf("%s/%s", dir, places[i].file);
        char *sub = cb_strdup(m3u);
        *strrchr(sub, '/') = '\0';
        cb_mkdir_p(sub);
        free(sub);

        CHECK(cb_library_playlist_export_m3u(&lib, p, m3u) == 0);
        uint8_t *text = NULL;
        size_t size;
        cb_file_read(m3u, &text, &size);
        if (!text || !strstr((char *)text, places[i].line)) {
            fprintf(stderr, "%s: expected a line '%s' in:\n%s\n", places[i].file, places[i].line + 1,
                    text ? (char *)text : "(unreadable)");
            test_failures++;
        }
        free(text);

        int before = nb_playlists(&lib);
        CBPlaylist *back = NULL;
        CHECK(cb_library_playlist_import_m3u(&lib, NULL, m3u, &back) == 3);
        CHECK(nb_playlists(&lib) == before + 1);
        CHECK(back && ids_are(back, IDS(1, 2, 3)));
        free(m3u);
    }

    /* Nothing in the file can be found: an error, and no playlist. */
    char *gone = cb_asprintf("%s/export/gone.m3u8", dir);
    FILE *f = fopen(gone, "w");
    fputs("#EXTM3U\n../music/none1.wav\n../music/none2.wav\n", f);
    fclose(f);
    int before = nb_playlists(&lib);
    CBPlaylist *back = (CBPlaylist *)1;
    CHECK(cb_library_playlist_import_m3u(&lib, NULL, gone, &back) == CB_ERROR(ENOENT));
    CHECK(!back && nb_playlists(&lib) == before && !cb_library_playlist_find(&lib, "gone"));
    free(gone);

    /* A file with no entries at all is an empty playlist. */
    char *empty = cb_asprintf("%s/export/empty.m3u8", dir);
    f = fopen(empty, "w");
    fputs("#EXTM3U\n", f);
    fclose(f);
    CHECK(cb_library_playlist_import_m3u(&lib, NULL, empty, &back) == 0);
    CHECK(back && back->nb == 0 && !strcmp(back->name, "empty"));
    free(empty);

    CHECK(dense(&lib));
    CHECK(cb_library_check(&lib) == 0);
    cb_library_close(&lib);
}

int main(void)
{
    char *dir = test_dir("playlist");
    if (!dir)
        return 1;
    char *music = cb_asprintf("%s/music", dir), *root = cb_asprintf("%s/lib", dir);
    static const char *const names[] = { "a.wav", "b.wav", "c.wav", "d.wav" };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(names); i++) {
        char *p = cb_asprintf("%s/%s", music, names[i]);
        test_write_wav(p, 1.0, 44100, 2, NULL, NULL, NULL);
        free(p);
    }

    test_tree(root, music);
    test_m3u(dir, root, music);
    test_m3u_roundtrip(dir, root);

    if (!test_failures)
        test_rm_rf(dir);
    free(music);
    free(root);
    free(dir);
    return test_done("core_playlist");
}
