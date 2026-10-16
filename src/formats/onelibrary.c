/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* exportLibrary.db ("OneLibrary", formerly Device Library Plus), the
 * SQLCipher 4 database the CDJ-3000X, XDJ-AZ, OPUS-QUAD and OMNIS-DUO read.
 * A port of dj's formats/onelibrary.py: schema, fixed rows and value
 * conventions copied from a pristine rekordbox 7.2.18 export
 * (docs/onelibrary.md 2-3, tests/data/rekordbox-7.2.18-exportLibrary.db).
 * The players take grids, waveforms and cues from the ANLZ files; this
 * database supplies track identity, metadata and playlists.
 *
 * dj built the plaintext in a temporary WAL-mode file whose header already
 * reserved 80 bytes a page. Here SQLite builds it in memory with the same
 * reserve (SQLITE_FCNTL_RESERVE_BYTES on the empty database) and the
 * serialised image gets the header bytes dj's finalize_plain left: WAL
 * mode (bytes 18/19 = 2) and rekordbox's SQLite version at 96. No
 * temporary files. */

#include "cyanbox/formats.h"

#include <stdio.h>

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"

const char cb_onelibrary_passphrase[] =
    "r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls";

/* The exact DDL and creation order of rekordbox 7.2.18. */
const char *const cb_onelibrary_schema[] = {
    "CREATE TABLE content(content_id integer primary key, title varchar, titleForSearch varchar, subtitle varchar, bpmx100 integer, length integer, trackNo integer, discNo integer, artist_id_artist integer, artist_id_remixer integer, artist_id_originalArtist integer, artist_id_composer integer, artist_id_lyricist integer, album_id integer, genre_id integer, label_id integer, key_id integer, color_id integer, image_id integer, djComment varchar, rating integer, releaseYear integer, releaseDate varchar, dateCreated varchar, dateAdded varchar, path varchar, fileName varchar, fileSize integer, fileType integer, bitrate integer, bitDepth integer, samplingRate integer, isrc varchar, djPlayCount integer, isHotCueAutoLoadOn integer, isKuvoDeliverStatusOn integer, kuvoDeliveryComment varchar, masterDbId integer, masterContentId integer, analysisDataFilePath varchar, analysedBits integer, contentLink integer, hasModified integer, cueUpdateCount integer, analysisDataUpdateCount integer, informationUpdateCount integer)",
    "CREATE TABLE genre(genre_id integer primary key, name varchar)",
    "CREATE TABLE artist(artist_id integer primary key, name varchar, nameForSearch varchar)",
    "CREATE TABLE album(album_id integer primary key, name varchar, artist_id integer, image_id integer, isComplation integer, nameForSearch varchar)",
    "CREATE TABLE label(label_id integer primary key, name varchar)",
    "CREATE TABLE key(key_id integer primary key, name varchar)",
    "CREATE TABLE color(color_id integer primary key, name varchar)",
    "CREATE TABLE playlist(playlist_id integer primary key, sequenceNo integer, name varchar, image_id integer, attribute integer, playlist_id_parent integer)",
    "CREATE TABLE playlist_content(playlist_id integer, content_id integer, sequenceNo integer)",
    "CREATE TABLE hotCueBankList(hotCueBankList_id integer primary key, sequenceNo integer, name varchar, image_id integer, attribute integer, hotCueBankList_id_parent integer)",
    "CREATE TABLE hotCueBankList_cue(hotCueBankList_id integer, cue_id integer, sequenceNo integer)",
    "CREATE TABLE history(history_id integer primary key, sequenceNo integer, name varchar, attribute integer, history_id_parent integer)",
    "CREATE TABLE history_content(history_id integer, content_id integer, sequenceNo integer)",
    "CREATE TABLE image(image_id integer primary key, path varchar)",
    "CREATE TABLE cue(cue_id integer primary key, content_id integer, kind integer, colorTableIndex integer, cueComment varchar, isActiveLoop integer, beatLoopNumerator integer, beatLoopDenominator integer, inUsec integer, outUsec integer, in150FramePerSec integer, out150FramePerSec integer, inMpegFrameNumber integer, outMpegFrameNumber integer, inMpegAbs integer, outMpegAbs integer, inDecodingStartFramePosition integer, outDecodingStartFramePosition integer, inFileOffsetInBlock integer, OutFileOffsetInBlock integer, inNumberOfSampleInBlock integer, outNumberOfSampleInBlock integer)",
    "CREATE TABLE menuItem(menuItem_id integer primary key, kind integer, name varchar)",
    "CREATE TABLE category(category_id integer primary key, menuItem_id integer, sequenceNo integer, isVisible integer)",
    "CREATE TABLE sort(sort_id integer primary key, menuItem_id integer, sequenceNo integer, isVisible integer, isSelectedAsSubColumn integer)",
    "CREATE TABLE property(deviceName varchar, dbVersion varchar, numberOfContents integer, createdDate varchar, backGroundColorType integer, myTagMasterDBID integer)",
    "CREATE TABLE recommendedLike(content_id_1 integer, content_id_2 integer, rating integer, createdDate integer)",
    "CREATE TABLE myTag(myTag_id integer primary key, sequenceNo integer, name varchar, attribute integer, myTag_id_parent integer)",
    "CREATE TABLE myTag_content(myTag_id integer, content_id integer)",
    "CREATE INDEX index_playlist_content_playlist_id on playlist_content(playlist_id)",
    "CREATE INDEX index_myTag_content_myTag_id on myTag_content(myTag_id)",
    "CREATE INDEX index_myTag_content_content_id on myTag_content(content_id)",
    "CREATE INDEX index_hotCueBankList_cue_hotCueBankList_id on hotCueBankList_cue(hotCueBankList_id)",
};
const int cb_onelibrary_schema_count = (int)CB_ARRAY_ELEMS(cb_onelibrary_schema);

/* Browse menu rows (docs/onelibrary.md 3.4); names get U+FFFA/U+FFFB. */
const CBOneLibMenuItem cb_onelibrary_menu_items[27] = {
    { 1, 128, "GENRE" }, { 2, 129, "ARTIST" }, { 3, 130, "ALBUM" }, { 4, 131, "TRACK" },
    { 5, 133, "BPM" }, { 6, 134, "RATING" }, { 7, 135, "YEAR" }, { 8, 136, "REMIXER" },
    { 9, 137, "LABEL" }, { 10, 138, "ORIGINAL ARTIST" }, { 11, 139, "KEY" }, { 12, 141, "CUE" },
    { 13, 142, "COLOR" }, { 14, 146, "TIME" }, { 15, 147, "BITRATE" }, { 16, 148, "FILE NAME" },
    { 17, 132, "PLAYLIST" }, { 18, 152, "HOT CUE BANK" }, { 19, 149, "HISTORY" },
    { 20, 145, "SEARCH" }, { 21, 150, "COMMENTS" }, { 22, 140, "DATE ADDED" },
    { 23, 151, "DJ PLAY COUNT" }, { 24, 144, "FOLDER" }, { 25, 161, "DEFAULT" },
    { 26, 162, "ALPHABET" }, { 27, 170, "MATCHING" },
};
const int cb_onelibrary_category[22][4] = {
    { 1, 1, 0, 0 }, { 2, 2, 1, 1 }, { 3, 3, 2, 1 }, { 4, 4, 3, 1 }, { 5, 17, 5, 1 }, { 6, 5, 0, 0 },
    { 7, 6, 0, 0 }, { 8, 7, 0, 0 }, { 9, 8, 0, 0 }, { 10, 9, 0, 0 }, { 11, 10, 0, 0 }, { 12, 11, 4, 1 },
    { 15, 13, 0, 0 }, { 17, 24, 9, 1 }, { 18, 20, 7, 1 }, { 19, 14, 0, 0 }, { 20, 15, 0, 0 },
    { 21, 16, 0, 0 }, { 22, 19, 6, 1 }, { 23, 18, 0, 0 }, { 26, 27, 8, 1 }, { 27, 22, 10, 0 },
};
const int cb_onelibrary_sort[17][5] = {
    { 0, 25, 1, 1, 0 }, { 1, 26, 2, 1, 0 }, { 2, 2, 3, 1, 0 }, { 3, 3, 4, 1, 0 }, { 4, 5, 5, 1, 0 },
    { 5, 6, 6, 1, 0 }, { 6, 1, 0, 0, 0 }, { 7, 21, 0, 0, 0 }, { 8, 14, 0, 0, 0 }, { 9, 8, 0, 0, 0 },
    { 10, 9, 0, 0, 0 }, { 11, 10, 0, 0, 0 }, { 12, 11, 7, 1, 0 }, { 13, 15, 0, 0, 0 },
    { 15, 13, 0, 0, 0 }, { 16, 23, 0, 0, 0 }, { 17, 22, 0, 0, 0 },
};

static const char *const color_names[8] = {
    "Pink", "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple",
};

/* What rekordbox writes for a track with a plain analysis: the same values
 * as the export.pdb track row's 0x04 and 0x56 fields (docs/onelibrary.md
 * 3.1). */
#define CONTENT_LINK  0x000C0700
#define ANALYSED_BITS 41

/* -------------------------------------------------------------- binding */

/* A prepared INSERT bound column by column: `n` counts the next column. */
typedef struct Ins {
    sqlite3_stmt *st;
    int           n;
    int           err;
} Ins;

static int ins_prepare(sqlite3 *db, Ins *in, const char *table, int columns)
{
    char sql[256];
    int o = snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (", table);
    for (int i = 0; i < columns; i++)
        o += snprintf(sql + o, sizeof(sql) - (size_t)o, i ? ",?" : "?");
    snprintf(sql + o, sizeof(sql) - (size_t)o, ")");
    *in = (Ins) { 0 };
    if (sqlite3_prepare_v2(db, sql, -1, &in->st, NULL) != SQLITE_OK) {
        cb_err("exportLibrary.db: %s", sqlite3_errmsg(db));
        return CB_ERROR(EIO);
    }
    return 0;
}

static void ins_int(Ins *in, int64_t v)
{
    if (sqlite3_bind_int64(in->st, ++in->n, v) != SQLITE_OK)
        in->err = CB_ERROR(EIO);
}

/* dj's `x or None`: ids of 0 are NULL. */
static void ins_id(Ins *in, int64_t v)
{
    int r = v ? sqlite3_bind_int64(in->st, ++in->n, v) : sqlite3_bind_null(in->st, ++in->n);
    if (r != SQLITE_OK)
        in->err = CB_ERROR(EIO);
}

static void ins_null(Ins *in)
{
    if (sqlite3_bind_null(in->st, ++in->n) != SQLITE_OK)
        in->err = CB_ERROR(EIO);
}

static void ins_text(Ins *in, const char *s)
{
    int r = s ? sqlite3_bind_text(in->st, ++in->n, s, -1, SQLITE_TRANSIENT)
              : sqlite3_bind_null(in->st, ++in->n);
    if (r != SQLITE_OK)
        in->err = CB_ERROR(EIO);
}

static int ins_step(Ins *in)
{
    if (!in->err && sqlite3_step(in->st) != SQLITE_DONE)
        in->err = CB_ERROR(EIO);
    sqlite3_reset(in->st);
    sqlite3_clear_bindings(in->st);
    in->n = 0;
    return in->err;
}

static int ins_done(Ins *in)
{
    sqlite3_finalize(in->st);
    in->st = NULL;
    return in->err;
}

/* ----------------------------------------------------------------- rows */

static int names_rows(sqlite3 *db, const char *table, const CBPlanName *v, int nb, int extra_nulls)
{
    Ins in;
    int ret = ins_prepare(db, &in, table, 2 + extra_nulls);
    if (ret < 0)
        return ret;
    for (int i = 0; i < nb && !in.err; i++) {
        ins_int(&in, v[i].id);
        ins_text(&in, v[i].name);
        for (int k = 0; k < extra_nulls; k++)
            ins_null(&in);
        ins_step(&in);
    }
    return ins_done(&in);
}

static int content_rows(sqlite3 *db, const CBPlan *plan)
{
    Ins in;
    int ret = ins_prepare(db, &in, "content", 46);
    if (ret < 0)
        return ret;
    uint32_t library_id = cb_plan_library_id(plan);
    for (int i = 0; i < plan->nb_tracks && !in.err; i++) {
        const CBPlanTrack *t = &plan->tracks[i];
        const CBMeta *m = t->track ? &t->track->meta : NULL;
        char title[1024], date[16];
        cb_plan_title(t, title, sizeof(title));
        cb_plan_date_added(t, date);
        char *anlz = cb_plan_anlz_path(t, ".DAT");
        if (!anlz) {
            in.err = CB_ERROR(ENOMEM);
            break;
        }
        double bpm = t->grid.nb_sections ? t->grid.sections[0].bpm : 0.0;
        const char *path = t->usb_path ? t->usb_path : "";

        ins_int(&in, t->id);
        ins_text(&in, title);
        ins_null(&in);                                  /* titleForSearch */
        ins_text(&in, "");                              /* subtitle */
        ins_int(&in, (int64_t)nearbyint(bpm * 100.0));
        ins_int(&in, (int64_t)t->duration);             /* seconds (docs: not ms) */
        ins_int(&in, m ? m->track_number : 0);
        ins_int(&in, m ? m->disc_number : 0);
        ins_id(&in, t->artist_id);
        ins_id(&in, t->remixer_id);
        ins_id(&in, t->original_artist_id);
        ins_id(&in, t->composer_id);
        ins_null(&in);                                  /* lyricist */
        ins_id(&in, t->album_id);
        ins_id(&in, t->genre_id);
        ins_id(&in, t->label_id);
        ins_id(&in, t->key_id);
        ins_int(&in, t->color_id);                      /* 0 = none */
        ins_id(&in, t->artwork_id);
        ins_text(&in, m && m->comment ? m->comment : "");
        ins_int(&in, m ? CB_CLAMP(m->rating, 0, 5) : 0);
        ins_id(&in, m ? m->year : 0);
        ins_text(&in, "");                              /* releaseDate */
        ins_text(&in, date);                            /* dateCreated */
        ins_text(&in, date);                            /* dateAdded */
        ins_text(&in, path);
        ins_text(&in, cb_path_base(path));
        ins_int(&in, t->file_size);
        ins_int(&in, t->file_type);
        ins_int(&in, t->bit_rate);
        ins_int(&in, t->bit_depth);
        ins_int(&in, t->sample_rate);
        ins_text(&in, "");                              /* isrc */
        ins_int(&in, 0);                                /* djPlayCount */
        ins_int(&in, plan->hot_cue_auto_load ? 1 : 0);
        ins_int(&in, 1);                                /* isKuvoDeliverStatusOn */
        ins_text(&in, "");                              /* kuvoDeliveryComment */
        ins_int(&in, library_id);                       /* masterDbId */
        ins_int(&in, cb_plan_master_id(t));             /* masterContentId */
        ins_text(&in, anlz);
        ins_int(&in, ANALYSED_BITS);
        ins_int(&in, CONTENT_LINK);
        ins_int(&in, 0);                                /* hasModified */
        ins_null(&in);
        ins_null(&in);
        ins_null(&in);
        free(anlz);
        ins_step(&in);
    }
    return ins_done(&in);
}

/* Cue rows mirror the ANLZ cues (docs/onelibrary.md 3.2): kind 0 memory,
 * 1..8 hot cues A..H; times in microseconds as ms x 1000, a non-loop's out
 * repeating its in; the legacy frame columns NULL. colorTableIndex stays
 * 0 as in every rekordbox row seen (players read colours from the ANLZ). */
static int cue_rows(sqlite3 *db, const CBPlan *plan)
{
    Ins in;
    int ret = ins_prepare(db, &in, "cue", 22);
    if (ret < 0)
        return ret;
    int64_t id = 1;
    for (int i = 0; i < plan->nb_tracks && !in.err; i++) {
        const CBPlanTrack *t = &plan->tracks[i];
        for (int k = 0; k < t->nb_cues && !in.err; k++) {
            const CBPlanCue *c = &t->cues[k];
            uint16_t num, den;
            cb_anlz_loop_quantum(c, &t->grid, &num, &den);
            int64_t t_in = (int64_t)cb_anlz_ms(c->time) * 1000;
            int64_t t_out = c->loop_end >= 0 ? (int64_t)cb_anlz_ms(c->loop_end) * 1000 : t_in;
            ins_int(&in, id++);
            ins_int(&in, t->id);
            ins_int(&in, c->hot >= 0 ? c->hot + 1 : 0);
            ins_int(&in, 0);
            ins_text(&in, c->name[0] ? c->name : NULL);
            ins_int(&in, c->active ? 1 : 0);
            ins_id(&in, num);
            ins_id(&in, den);
            ins_int(&in, t_in);
            ins_int(&in, t_out);
            for (int n = 0; n < 12; n++)
                ins_null(&in);
            ins_step(&in);
        }
    }
    return ins_done(&in);
}

static int fixed_rows(sqlite3 *db)
{
    Ins in;
    int ret;
    if ((ret = ins_prepare(db, &in, "menuItem", 3)) < 0)
        return ret;
    for (int i = 0; i < 27; i++) {
        char name[64];
        snprintf(name, sizeof(name), "\xEF\xBF\xBA%s\xEF\xBF\xBB", cb_onelibrary_menu_items[i].name);
        ins_int(&in, cb_onelibrary_menu_items[i].id);
        ins_int(&in, cb_onelibrary_menu_items[i].kind);
        ins_text(&in, name);
        ins_step(&in);
    }
    if ((ret = ins_done(&in)) < 0 || (ret = ins_prepare(db, &in, "category", 4)) < 0)
        return ret;
    for (int i = 0; i < 22; i++) {
        for (int k = 0; k < 4; k++)
            ins_int(&in, cb_onelibrary_category[i][k]);
        ins_step(&in);
    }
    if ((ret = ins_done(&in)) < 0 || (ret = ins_prepare(db, &in, "sort", 5)) < 0)
        return ret;
    for (int i = 0; i < 17; i++) {
        for (int k = 0; k < 5; k++)
            ins_int(&in, cb_onelibrary_sort[i][k]);
        ins_step(&in);
    }
    return ins_done(&in);
}

static int plan_rows(sqlite3 *db, const CBPlan *plan)
{
    int ret;
    Ins in;

    if ((ret = names_rows(db, "genre", plan->genres, plan->nb_genres, 0)) < 0 ||
        (ret = names_rows(db, "artist", plan->artists, plan->nb_artists, 1)) < 0)
        return ret;

    if ((ret = ins_prepare(db, &in, "album", 6)) < 0)
        return ret;
    for (int i = 0; i < plan->nb_albums; i++) {
        ins_int(&in, plan->albums[i].id);
        ins_text(&in, plan->albums[i].name);
        ins_id(&in, plan->albums[i].artist_id);
        ins_null(&in);
        ins_int(&in, 0);
        ins_null(&in);
        ins_step(&in);
    }
    if ((ret = ins_done(&in)) < 0)
        return ret;

    if ((ret = names_rows(db, "label", plan->labels, plan->nb_labels, 0)) < 0 ||
        (ret = names_rows(db, "key", plan->keys, plan->nb_keys, 0)) < 0)
        return ret;

    if ((ret = ins_prepare(db, &in, "color", 2)) < 0)
        return ret;
    for (int i = 0; i < 8; i++) {
        ins_int(&in, i + 1);
        ins_text(&in, color_names[i]);
        ins_step(&in);
    }
    if ((ret = ins_done(&in)) < 0)
        return ret;

    /* Folders attribute 1, root parent 0, sibling sequenceNo from 0;
     * entries numbered from 1 (docs/onelibrary.md 3.3). */
    if ((ret = ins_prepare(db, &in, "playlist", 6)) < 0)
        return ret;
    for (int i = 0; i < plan->nb_playlists; i++) {
        const CBPlanPlaylist *p = &plan->playlists[i];
        ins_int(&in, p->id);
        ins_int(&in, p->sort_order);
        ins_text(&in, p->name);
        ins_null(&in);
        ins_int(&in, p->is_folder ? 1 : 0);
        ins_int(&in, p->parent_id);
        ins_step(&in);
    }
    if ((ret = ins_done(&in)) < 0 || (ret = ins_prepare(db, &in, "playlist_content", 3)) < 0)
        return ret;
    for (int i = 0; i < plan->nb_playlists; i++) {
        const CBPlanPlaylist *p = &plan->playlists[i];
        for (int k = 0; k < p->nb; k++) {
            ins_int(&in, p->id);
            ins_int(&in, p->track_ids[k]);
            ins_int(&in, k + 1);
            ins_step(&in);
        }
    }
    if ((ret = ins_done(&in)) < 0)
        return ret;

    /* OneLibrary's images use the "b" prefix where export.pdb uses "a"
     * (docs/onelibrary.md 3.3); the exporter writes both. */
    if ((ret = ins_prepare(db, &in, "image", 2)) < 0)
        return ret;
    for (int i = 0; i < plan->nb_artworks; i++) {
        uint32_t a = plan->artworks[i].id;
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/Artwork/%05u/b%u.jpg", a / 20 + 1, a);
        ins_int(&in, a);
        ins_text(&in, path);
        ins_step(&in);
    }
    if ((ret = ins_done(&in)) < 0)
        return ret;

    if ((ret = content_rows(db, plan)) < 0 || (ret = cue_rows(db, plan)) < 0 ||
        (ret = fixed_rows(db)) < 0)
        return ret;

    /* One property row: dbVersion '1000', the track count, the date; the
     * background colour is "Background Color : OneLibrary", 0 default,
     * 1..8 pink..purple (rbxport, observed in rekordbox 7.2.14); dj's
     * myTagMasterDBID is the library constant XOR 0x5A5A5A5A. */
    if ((ret = ins_prepare(db, &in, "property", 6)) < 0)
        return ret;
    ins_text(&in, plan->device_name ? plan->device_name : "");
    ins_text(&in, "1000");
    ins_int(&in, plan->nb_tracks);
    ins_text(&in, plan->date);
    ins_int(&in, CB_CLAMP(plan->background_color, 0, 8));
    ins_int(&in, cb_plan_library_id(plan) ^ 0x5A5A5A5Au);
    ins_step(&in);
    if ((ret = ins_done(&in)) < 0)
        return ret;

    /* My Tag columns (attribute 1, parent 0) and the tags in them. */
    if ((ret = ins_prepare(db, &in, "myTag", 5)) < 0)
        return ret;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < plan->nb_tags; i++) {
            const CBPlanTag *tg = &plan->tags[i];
            if (tg->is_category != (pass == 0))
                continue;
            ins_int(&in, tg->id);
            ins_int(&in, tg->position);
            ins_text(&in, tg->name);
            ins_int(&in, tg->is_category ? 1 : 0);
            ins_int(&in, tg->is_category ? 0 : tg->category);
            ins_step(&in);
        }
    }
    if ((ret = ins_done(&in)) < 0 || (ret = ins_prepare(db, &in, "myTag_content", 2)) < 0)
        return ret;
    for (int i = 0; i < plan->nb_tag_tracks; i++) {
        ins_int(&in, plan->tag_tracks[i].tag_id);
        ins_int(&in, plan->tag_tracks[i].track_id);
        ins_step(&in);
    }
    return ins_done(&in);
}

static int exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        cb_err("exportLibrary.db: %s: %s", sql, err ? err : "?");
        sqlite3_free(err);
        return CB_ERROR(EIO);
    }
    return 0;
}

int cb_onelibrary_build_plain(const CBPlan *plan, uint8_t **out, size_t *size)
{
    *out = NULL;
    *size = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return CB_ERROR(ENOMEM);
    }
    int ret = exec(db, "PRAGMA page_size = 4096");
    /* SQLite keeps the reserve of the database it starts with, so every
     * page it adds has room for SQLCipher's IV and HMAC. */
    int reserve = CB_SQLCIPHER_RESERVE;
    if (!ret && sqlite3_file_control(db, "main", SQLITE_FCNTL_RESERVE_BYTES, &reserve) != SQLITE_OK)
        ret = CB_ERROR(EIO);
    for (int i = 0; !ret && i < cb_onelibrary_schema_count; i++)
        ret = exec(db, cb_onelibrary_schema[i]);
    if (!ret)
        ret = exec(db, "BEGIN");
    if (!ret && (ret = plan_rows(db, plan)) < 0)
        cb_err("exportLibrary.db: %s", sqlite3_errmsg(db));
    if (!ret)
        ret = exec(db, "COMMIT");
    if (ret < 0)
        goto fail;

    sqlite3_int64 n = 0;
    unsigned char *img = sqlite3_serialize(db, "main", &n, 0);
    if (!img) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    uint8_t *b = malloc((size_t)n);
    if (!b) {
        sqlite3_free(img);
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    memcpy(b, img, (size_t)n);
    sqlite3_free(img);
    /* rekordbox's files are WAL mode (docs/onelibrary.md 1.1) and carry its
     * SQLite version, 3.45.3 (dj's finalize_plain). */
    b[18] = b[19] = 2;
    cb_wr_be32(b + 96, CB_SQLCIPHER_SQLITE_VERSION);
    if (b[20] != CB_SQLCIPHER_RESERVE || cb_rd_be16(b + 16) != CB_SQLCIPHER_PAGE) {
        cb_err("exportLibrary.db: SQLite dropped the page reserve; cannot encrypt");
        free(b);
        ret = CB_ERROR(EIO);
        goto fail;
    }
    *out = b;
    *size = (size_t)n;

fail:
    sqlite3_close(db);
    return ret;
}

int cb_onelibrary_build(const CBPlan *plan, uint8_t **out, size_t *size)
{
    uint8_t *plain;
    size_t n;
    int ret = cb_onelibrary_build_plain(plan, &plain, &n);
    if (ret < 0)
        return ret;
    ret = cb_sqlcipher_encrypt(plain, n, cb_onelibrary_passphrase, NULL, out, size);
    free(plain);
    return ret;
}

/* ------------------------------------------------------------- reading */

/* A stick's exportLibrary.db is anyone's, and SQLite runs what a schema
 * says: a view is a query (an endless recursive one kept every read of
 * `content` running), a trigger is code, a generated column computes a
 * value of any size from none (a megabyte title a row: 24 KB of file
 * read as 958 MB). rekordbox's schema is plain tables and indexes
 * (cb_onelibrary_schema), so the connection runs no views, triggers or
 * functions of the schema (defensive, untrusted schema), and a database
 * holding a view, a trigger, a virtual table or a generated column is
 * refused. A crafted b-tree could still hand the same overflow pages to
 * every row; quick_check finds a page used twice, so what the readers
 * copy out is bounded by the file. And SQLite's own work on it is
 * budgeted (BUDGET_*): a query that runs past it is interrupted. */

/* Virtual machine steps a byte of database, and at least: the checks
 * here, inspect's integrity check and the reads of inspect, the sync and
 * cb_onelibrary_read took 4000 steps on rekordbox 7.2.18's fixture and
 * one a byte on a 5000-track database of ours (2 MB); 256 a byte leaves
 * room for a library with many histories, and stops an endless query
 * within seconds. */
#define BUDGET_PER_BYTE 256
#define BUDGET_MIN      (64ull << 20)
#define BUDGET_STEP     1000
/* The longest value a read returns: the database's strings are names and
 * paths. */
#define VALUE_MAX       (1 << 20)

typedef struct Budget {
    sqlite3_uint64 left;
} Budget;

static int spend(void *opaque)
{
    Budget *b = opaque;
    if (b->left < BUDGET_STEP)
        return 1;
    b->left -= BUDGET_STEP;
    return 0;
}

static int query_count(sqlite3 *db, const char *sql, int64_t *n)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return CB_ERROR(EINVAL);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : CB_ERROR(EINVAL);
}

/* Plain tables and indexes, whole: no view, trigger, virtual table or
 * generated column, then no page used twice (the schema first: the
 * check would compute generated columns). The tables' kinds are SQLite's
 * own reading of their SQL, not what the type column claims. */
static int check_plain(sqlite3 *db)
{
    int64_t other = -1, generated = -1;
    int ret = query_count(db, "SELECT (SELECT count(*) FROM pragma_table_list "
                              "        WHERE schema = 'main' AND type != 'table') + "
                              "       (SELECT count(*) FROM sqlite_schema "
                              "        WHERE type NOT IN ('table', 'index'))", &other);
    if (ret >= 0)
        ret = query_count(db, "SELECT count(*) FROM sqlite_schema AS s, "
                              "pragma_table_xinfo(s.name) AS c "
                              "WHERE s.type = 'table' AND c.hidden IN (2, 3)", &generated);
    if (ret < 0 || other || generated) {
        cb_warn("exportLibrary.db: not rekordbox's schema (views, triggers, virtual tables or "
                "computed columns); not read");
        return CB_ERROR(EINVAL);
    }

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &st, NULL) != SQLITE_OK)
        return CB_ERROR(EINVAL);
    int rc = sqlite3_step(st);
    const char *res = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
    bool ok = res && !strcmp(res, "ok");
    if (!ok)
        cb_warn("exportLibrary.db is damaged: %s", res ? res : sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok ? 0 : CB_ERROR(EINVAL);
}

int cb_onelibrary_open_plain(const uint8_t *plain, size_t size, sqlite3 **db)
{
    *db = NULL;
    if (size < 100)
        return CB_ERROR(EINVAL);
    sqlite3 *d = NULL;
    if (sqlite3_open_v2(":memory:", &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        sqlite3_close(d);
        return CB_ERROR(ENOMEM);
    }
    Budget *budget = sqlite3_malloc(sizeof(*budget));
    if (!budget) {
        sqlite3_close(d);
        return CB_ERROR(ENOMEM);
    }
    budget->left = CB_MAX(BUDGET_MIN, (sqlite3_uint64)size * BUDGET_PER_BYTE);
    if (sqlite3_set_clientdata(d, "cb_budget", budget, sqlite3_free) != SQLITE_OK) {
        sqlite3_free(budget);
        sqlite3_close(d);
        return CB_ERROR(ENOMEM);
    }
    sqlite3_progress_handler(d, BUDGET_STEP, spend, budget);
    sqlite3_db_config(d, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    sqlite3_db_config(d, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
    sqlite3_db_config(d, SQLITE_DBCONFIG_ENABLE_VIEW, 0, NULL);
    sqlite3_db_config(d, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, NULL);
    sqlite3_limit(d, SQLITE_LIMIT_LENGTH, VALUE_MAX);

    unsigned char *b = sqlite3_malloc64(size);
    if (!b) {
        sqlite3_close(d);
        return CB_ERROR(ENOMEM);
    }
    memcpy(b, plain, size);
    /* Memory databases cannot use WAL: legacy journal flags. */
    b[18] = b[19] = 1;
    unsigned flags = SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE;
    if (sqlite3_deserialize(d, "main", b, (sqlite3_int64)size, (sqlite3_int64)size, flags) != SQLITE_OK ||
        check_plain(d) < 0) {
        sqlite3_close(d);
        return CB_ERROR(EINVAL);
    }
    *db = d;
    return 0;
}

int cb_onelibrary_read(const uint8_t *plain, size_t size, CBOneLibTrack **tracks, int *nb)
{
    *tracks = NULL;
    *nb = 0;
    sqlite3 *db;
    int ret = cb_onelibrary_open_plain(plain, size, &db);
    if (ret < 0)
        return ret;
    sqlite3_stmt *st = NULL;
    /* The cues counted once, grouped: a count per row scanned every cue
     * for every track (cue.content_id has no index), 400 million steps
     * for 5000 tracks. */
    const char *sql = "SELECT c.content_id, c.path, c.title, coalesce(n.k, 0) FROM content AS c "
                      "LEFT JOIN (SELECT content_id, count(*) AS k FROM cue GROUP BY content_id) AS n "
                      "ON n.content_id = c.content_id ORDER BY c.content_id";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return CB_ERROR(EINVAL);
    }
    CBArray a;
    cb_array_init(&a, sizeof(CBOneLibTrack));
    int r;
    while ((r = sqlite3_step(st)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(st, 1);
        const char *title = (const char *)sqlite3_column_text(st, 2);
        CBOneLibTrack t = {
            .id      = (uint32_t)sqlite3_column_int64(st, 0),
            .path    = cb_strdup(path ? path : ""),
            .title   = cb_strdup(title ? title : ""),
            .nb_cues = sqlite3_column_int(st, 3),
        };
        if (!t.path || !t.title || !cb_array_push(&a, &t)) {
            free(t.path);
            free(t.title);
            ret = CB_ERROR(ENOMEM);
            break;
        }
    }
    if (!ret && r != SQLITE_DONE)
        ret = CB_ERROR(EINVAL);
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (ret < 0) {
        for (size_t i = 0; i < a.nb; i++) {
            CBOneLibTrack *t = cb_array_at(&a, i);
            free(t->path);
            free(t->title);
        }
        cb_array_free(&a);
        return ret;
    }
    *tracks = a.data;
    *nb = (int)a.nb;
    return 0;
}
