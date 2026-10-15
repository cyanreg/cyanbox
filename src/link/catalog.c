/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The catalog: the library as the players' menus see it, over the library
 * and the export plan over all of it (dbserver.h). The plan gives what a
 * stick would: ids, names, the path the NFS tree serves, the artwork, the
 * cues; the analysis replies are the ANLZ tags the export writes
 * (rx_anlz_build), made in memory when a player asks and kept for the last
 * few tracks, since a player asks for several tags of one track in a row
 * (rbxport's capture: grid, PQT2, VBR, PWV6, preview, PWV7, PWV5, detail,
 * PSSI, one after the other).
 *
 * Building is one pass over the plan's tracks (tempo, key, dates, titles);
 * every menu is computed on request, by walking the tracks in the order a
 * sort asks for, each order made the first time it is needed. Ids are the
 * plan's, which are the library's, except an album's: an album row carries
 * its id where a track row carries the track's, and a CDJ-3000 asks for
 * the artwork of both by that id (rbxport's capture: 2003 with an album's
 * id while showing an artist's albums, with a track's while showing
 * tracks), so album ids go out with ALBUM_BIT set to tell the two apart.
 *
 * Layouts are rekordbox 7.2.11's as rbxport captured them serving a
 * CDJ-3000 (rbl-dbserver tests/corpus.rs; its tests/session.rs for the
 * KUVO rows), else the djl-analysis' (track_metadata.adoc, menus.adoc);
 * what neither shows is marked assumed. */

#include "catalog.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rxbox/colors.h"
#include "rxbox/common.h"
#include "rxbox/export.h"
#include "rxbox/notation.h"

#define ALBUM_BIT   0x40000000u
#define NB_ANALYSIS 4

/* Sort ids, which also name the column a track row shows beside its
 * title (track_metadata.adoc "Sort Orders"; play count and date added
 * from rbxport). */
enum Sort {
    SORT_DEFAULT  = 0,
    SORT_TITLE    = 1,
    SORT_ARTIST   = 2,
    SORT_ALBUM    = 3,
    SORT_BPM      = 4,
    SORT_RATING   = 5,
    SORT_GENRE    = 6,
    SORT_COMMENT  = 7,
    SORT_TIME     = 8,
    SORT_REMIXER  = 9,
    SORT_LABEL    = 10,
    SORT_ORIGINAL = 11,
    SORT_KEY      = 12,
    SORT_BITRATE  = 13,
    SORT_COLOR    = 15,
    SORT_PLAYS    = 16,
    SORT_ADDED    = 17,
    SORT_FILE     = 18,         /* ours: FILE NAME's order, no player asks for it */
    NB_SORTS      = 19,
};

/* The column beside a title when the player names none: the artist, the
 * configuration the djl-analysis describes (track_metadata.adoc,
 * "Requesting All Tracks"). rekordbox's is a preference; it sent the
 * comment to the CDJ-3000 rbxport captured. */
#define DEFAULT_COLUMN SORT_ARTIST

typedef struct CatTrack {
    RXPlanTrack *pt;
    const char  *title;         /* the tag, else the file's name without extension */
    const char  *file;          /* the file's name on the virtual stick */
    uint32_t     tempo;         /* BPM x100 */
    uint8_t      key;           /* 1..24 round the Camelot wheel, 0 none */
    uint8_t      color;         /* 1..8, 0 none */
    uint8_t      rating;
    int32_t      added;         /* yyyymmdd, 0 unknown */
    int32_t      played;        /* the day of the last play, yyyymmdd, 0 never */
    uint32_t     album_artist;  /* ORIGINAL ARTIST's (album_artists): an artist id, 0 none */
} CatTrack;

/* One track's ANLZ files as the export would write them, and their tags. */
typedef struct Analysis {
    uint32_t    id;             /* 0: unused */
    uint64_t    used;
    RXAnlzFiles files;
    RXAnlzTag  *tags[3];        /* .DAT, .EXT, .2EX */
    int         nb_tags[3];
} Analysis;

struct RXCatalog {
    RXLibrary *lib;
    RXPlan    *plan;
    CatTrack  *t;               /* by the plan's track index */
    int        nb;
    char      *stems;
    uint32_t  *order[NB_SORTS]; /* track indices by each sort, made when first asked */
    Analysis   analysis[NB_ANALYSIS];
    uint64_t   clock;
    int      (*check)(void *opaque, const RXPlanTrack *pt);
    void      *check_opaque;
    /* Every play of a track served, oldest first (load_plays). */
    struct CatPlay { uint32_t track; int32_t day; int64_t at; int seq; } *plays;
    int        nb_plays;
    void     (*edit)(void *opaque, const RXLinkEdit *e);
    void      *edit_opaque;
};

/* ------------------------------------------------------------- building */

static int32_t ymd_of(int64_t when)
{
    if (when <= 0)
        return 0;
    time_t tt = (time_t)when;
    struct tm tm;
    if (!localtime_r(&tt, &tm))
        return 0;
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* When the track came into the library, as the stick's databases say it
 * (rx_plan_date_added, src/formats/util.c): the library's date, else the
 * file's time. */
static int32_t added_of(const RXTrack *t)
{
    int64_t when = t->added;
    if (!when && t->path)
        when = rx_file_mtime(t->path);
    if (!when)
        when = t->mtime;
    return ymd_of(when);
}

/* The key's place on the Camelot wheel, 1A 1 .. 12B 24, the ids rekordbox
 * gives the 24 keys of its KEY menu (rbxport's capture: Abm 1, E 24). */
static uint8_t key_index(const char *classic)
{
    char cam[8];
    int n;
    char letter;
    if (!classic || !classic[0] || !rx_key_to_camelot(classic, cam, sizeof(cam))[0] ||
        sscanf(cam, "%d%c", &n, &letter) != 2 || n < 1 || n > 12)
        return 0;
    return (uint8_t)((n - 1) * 2 + (letter == 'A' ? 1 : 2));
}

/* The classic name of each key id. */
static const char *const key_classic[24] = {
    "Abm", "B", "Ebm", "F#", "Bbm", "Db", "Fm", "Ab", "Cm", "Eb", "Gm", "Bb",
    "Dm", "F", "Am", "C", "Em", "G", "Bm", "D", "F#m", "A", "Dbm", "E",
};

static size_t stem_len(const RXTrack *t)
{
    if (!t || (t->meta.title && t->meta.title[0]) || !t->path)
        return 0;
    const char *base = rx_path_base(t->path);
    const char *dot = strrchr(base, '.');
    return (dot && dot != base ? (size_t)(dot - base) : strlen(base)) + 1;
}

static int load_plays(RXCatalog *c);

/* The decks' ORIGINAL ARTIST lists album artists (docs/LINK.md, "The
 * pieces"): a track's is its album's album artist, else the artist all
 * the album's tracks share; 0 for a track of no album, or of an album
 * with neither. */
static int album_artists(RXCatalog *c)
{
    const RXPlan *p = c->plan;
    uint32_t n = (uint32_t)RX_MAX(p->nb_albums, 0);
    /* Each album's own (by id), then the artist its tracks share:
     * UINT32_MAX when they share none. */
    uint32_t *own = calloc((size_t)n + 1, sizeof(*own));
    uint32_t *shared = calloc((size_t)n + 1, sizeof(*shared));
    if (!own || !shared) {
        free(own);
        free(shared);
        return RX_ERROR(ENOMEM);
    }
    for (uint32_t i = 0; i < n; i++)
        if (p->albums[i].id >= 1 && p->albums[i].id <= n)
            own[p->albums[i].id] = p->albums[i].artist_id;
    for (int i = 0; i < c->nb; i++) {
        uint32_t al = c->t[i].pt->album_id, ar = c->t[i].pt->artist_id;
        if (!al || al > n)
            continue;
        uint32_t *v = &shared[al];
        *v = !*v ? (ar ? ar : UINT32_MAX) : *v == ar ? *v : UINT32_MAX;
    }
    for (int i = 0; i < c->nb; i++) {
        uint32_t al = c->t[i].pt->album_id;
        if (al && al <= n)
            c->t[i].album_artist = own[al] ? own[al] : shared[al] != UINT32_MAX ? shared[al] : 0;
    }
    free(own);
    free(shared);
    return 0;
}

int rx_catalog_new(RXCatalog **out, RXLibrary *lib, RXPlan *plan)
{
    *out = NULL;
    RXCatalog *c = calloc(1, sizeof(*c));
    if (!c)
        return RX_ERROR(ENOMEM);
    c->lib = lib;
    c->plan = plan;
    c->nb = plan->nb_tracks;
    c->t = calloc((size_t)RX_MAX(c->nb, 1), sizeof(*c->t));
    size_t stems = 1;
    for (int i = 0; i < c->nb; i++)
        stems += stem_len(plan->tracks[i].track);
    c->stems = malloc(stems);
    if (!c->t || !c->stems) {
        rx_catalog_free(&c);
        return RX_ERROR(ENOMEM);
    }
    c->stems[0] = '\0';
    char *s = c->stems + 1;
    for (int i = 0; i < c->nb; i++) {
        RXPlanTrack *pt = &plan->tracks[i];
        const RXTrack *t = pt->track;
        CatTrack *ct = &c->t[i];
        *ct = (CatTrack) {
            .pt    = pt,
            .title = c->stems,
            .file  = pt->usb_path ? rx_path_base(pt->usb_path) : "",
            .color = (uint8_t)(pt->color_id <= 8 ? pt->color_id : 0),
        };
        /* The tempo the stick's track row carries (src/formats/pdb.c). */
        double bpm = rx_grid_bpm_median(&pt->grid, pt->duration);
        if (bpm > 0 && bpm < 1000)
            ct->tempo = (uint32_t)nearbyint(bpm * 100.0);
        if (!t)
            continue;
        /* The title as both databases write it: the tag, else the file's
         * name without its extension (rx_plan_title, src/formats/util.c). */
        size_t n = stem_len(t);
        if (n) {
            memcpy(s, rx_path_base(t->path), n - 1);
            s[n - 1] = '\0';
            ct->title = s;
            s += n;
        } else if (t->meta.title) {
            ct->title = t->meta.title;
        }
        ct->key = key_index(t->key);
        ct->rating = (uint8_t)RX_CLAMP(t->meta.rating, 0, 5);
        ct->added = added_of(t);
        ct->played = ymd_of(t->last_played);
    }
    if (load_plays(c) < 0 || album_artists(c) < 0) {
        rx_catalog_free(&c);
        return RX_ERROR(ENOMEM);
    }
    *out = c;
    return 0;
}

void rx_catalog_set_check(RXCatalog *c, int (*check)(void *opaque, const RXPlanTrack *pt),
                          void *opaque)
{
    c->check = check;
    c->check_opaque = opaque;
}

void rx_catalog_set_edit(RXCatalog *c, void (*edit)(void *opaque, const RXLinkEdit *e),
                         void *opaque)
{
    c->edit = edit;
    c->edit_opaque = opaque;
}

static void analysis_clear(Analysis *a)
{
    for (int i = 0; i < 3; i++)
        free(a->tags[i]);
    rx_anlz_files_free(&a->files);
    *a = (Analysis) { 0 };
}

void rx_catalog_free(RXCatalog **cp)
{
    RXCatalog *c = *cp;
    if (!c)
        return;
    for (int i = 0; i < NB_SORTS; i++)
        free(c->order[i]);
    for (int i = 0; i < NB_ANALYSIS; i++)
        analysis_clear(&c->analysis[i]);
    free(c->stems);
    free(c->t);
    free(c->plays);
    free(c);
    *cp = NULL;
}

/* -------------------------------------------------------------- lookups */

/* The plan's index of a track by id, -1 when there is none (or no
 * catalog: the service may have none between two libraries). */
static int cmp_play(const void *a, const void *b)
{
    const struct CatPlay *x = a, *y = b;
    return x->at != y->at ? (x->at < y->at ? -1 : 1) : x->seq - y->seq;
}

static int track_index(const RXCatalog *c, uint32_t id);

/* The history (HISTORY): every play the library recorded of a track
 * served, as rekordbox's keeps each session's plays; a track played
 * before the library kept them (a last play and no row) counts that play
 * once. A play table that does not read leaves the last plays alone. */
static int load_plays(RXCatalog *c)
{
    RXPlay *p = NULL;
    int n = 0;
    if (!c->lib || !c->lib->db || rx_library_plays(c->lib, &p, &n) < 0)
        n = 0;
    bool *has = calloc((size_t)RX_MAX(c->nb, 1), sizeof(*has));
    c->plays = malloc((size_t)RX_MAX(n + c->nb, 1) * sizeof(*c->plays));
    if (!has || !c->plays) {
        free(has);
        free(p);
        return RX_ERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++) {
        int k = p[i].track_id > 0 && p[i].track_id <= UINT32_MAX ? track_index(c, (uint32_t)p[i].track_id)
                                                                  : -1;
        if (k < 0 || p[i].played_at <= 0)
            continue;
        has[k] = true;
        c->plays[c->nb_plays] = (struct CatPlay) {
            c->plan->tracks[k].id, ymd_of(p[i].played_at), p[i].played_at, c->nb_plays,
        };
        c->nb_plays++;
    }
    for (int k = 0; k < c->nb; k++) {
        const RXTrack *t = c->plan->tracks[k].track;
        if (has[k] || !t || t->last_played <= 0)
            continue;
        c->plays[c->nb_plays] = (struct CatPlay) {
            c->plan->tracks[k].id, ymd_of(t->last_played), t->last_played, c->nb_plays,
        };
        c->nb_plays++;
    }
    qsort(c->plays, (size_t)c->nb_plays, sizeof(*c->plays), cmp_play);
    free(has);
    free(p);
    return 0;
}

static int track_index(const RXCatalog *c, uint32_t id)
{
    if (!c)
        return -1;
    int lo = 0, hi = c->nb - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t v = c->plan->tracks[mid].id;
        if (v == id)
            return mid;
        if (v < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static const RXMeta *meta_of(const CatTrack *t)
{
    static const RXMeta none = { .hue = RX_HUE_NONE };
    return t->pt->track ? &t->pt->track->meta : &none;
}

static const char *nonnull(const char *s)
{
    return s ? s : "";
}

/* A name by id. The plan numbers each table from 1 in order, so the id
 * is the index; a search otherwise. */
static const char *table_name(const RXCatalog *c, int table, uint32_t id)
{
    const RXPlanName *v = NULL;
    int nb = 0;
    switch (table) {
    case RX_CAT_ARTIST: v = c->plan->artists; nb = c->plan->nb_artists; break;
    case RX_CAT_ALBUM:  v = c->plan->albums;  nb = c->plan->nb_albums;  break;
    case RX_CAT_GENRE:  v = c->plan->genres;  nb = c->plan->nb_genres;  break;
    case RX_CAT_LABEL:  v = c->plan->labels;  nb = c->plan->nb_labels;  break;
    }
    if (!v || !id)
        return NULL;
    if (id <= (uint32_t)nb && v[id - 1].id == id)
        return nonnull(v[id - 1].name);
    for (int i = 0; i < nb; i++)
        if (v[i].id == id)
            return nonnull(v[i].name);
    return NULL;
}

static const char *name_or_empty(const RXCatalog *c, int table, uint32_t id)
{
    const char *s = table_name(c, table, id);
    return s ? s : "";
}

static uint32_t album_wire(uint32_t id)
{
    return id ? id | ALBUM_BIT : 0;
}

static const RXPlanArtwork *artwork_of(const RXPlan *p, uint32_t id)
{
    if (!id)
        return NULL;
    if (id <= (uint32_t)p->nb_artworks && p->artworks[id - 1].id == id)
        return &p->artworks[id - 1];
    for (int i = 0; i < p->nb_artworks; i++)
        if (p->artworks[i].id == id)
            return &p->artworks[i];
    return NULL;
}

/* The track's id in a table a menu narrows by. ORIGINAL ARTIST's field
 * is the album artist (album_artists). */
enum Field { F_ARTIST, F_ALBUM, F_GENRE, F_LABEL, F_ALBUM_ARTIST, F_REMIXER };

static uint32_t field_id(const CatTrack *t, int field)
{
    switch (field) {
    case F_ARTIST:   return t->pt->artist_id;
    case F_ALBUM:    return album_wire(t->pt->album_id);
    case F_GENRE:    return t->pt->genre_id;
    case F_LABEL:    return t->pt->label_id;
    case F_ALBUM_ARTIST: return t->album_artist;
    case F_REMIXER:  return t->pt->remixer_id;
    }
    return 0;
}

static int field_table(int field)
{
    switch (field) {
    case F_ALBUM: return RX_CAT_ALBUM;
    case F_GENRE: return RX_CAT_GENRE;
    case F_LABEL: return RX_CAT_LABEL;
    }
    return RX_CAT_ARTIST;
}

static const char *field_name(const RXCatalog *c, const CatTrack *t, int field)
{
    uint32_t id = field_id(t, field);
    return name_or_empty(c, field_table(field), field == F_ALBUM ? id & ~ALBUM_BIT : id);
}

/* ---------------------------------------------------------------- sorts */

/* Empty names after the others. */
static int cmp_text(const char *a, const char *b)
{
    a = nonnull(a);
    b = nonnull(b);
    if (!*a != !*b)
        return *a ? -1 : 1;
    return rx_strnatcasecmp(a, b);
}

static int cmp_u(uint64_t a, uint64_t b)
{
    return a < b ? -1 : a > b;
}

/* Zero (none) after the others. */
static int cmp_u_set(uint64_t a, uint64_t b)
{
    if (!a != !b)
        return a ? -1 : 1;
    return cmp_u(a, b);
}

typedef struct SortCtx {
    const RXCatalog *c;
    int              sort;
} SortCtx;

/* Rating, play count and date added put the most first, the rest
 * ascending (assumed; rekordbox's directions are not captured). */
static int cmp_by(const RXCatalog *c, int sort, const CatTrack *x, const CatTrack *y)
{
    const RXTrack *tx = x->pt->track, *ty = y->pt->track;
    switch (sort) {
    case SORT_ARTIST:   return cmp_text(field_name(c, x, F_ARTIST), field_name(c, y, F_ARTIST));
    case SORT_ALBUM: {
        int r = cmp_text(field_name(c, x, F_ALBUM), field_name(c, y, F_ALBUM));
        return r ? r : cmp_u_set(meta_of(x)->track_number, meta_of(y)->track_number);
    }
    case SORT_BPM:      return cmp_u_set(x->tempo, y->tempo);
    case SORT_RATING:   return -cmp_u(x->rating, y->rating);
    case SORT_GENRE:    return cmp_text(field_name(c, x, F_GENRE), field_name(c, y, F_GENRE));
    case SORT_COMMENT:  return cmp_text(meta_of(x)->comment, meta_of(y)->comment);
    case SORT_TIME:     return cmp_u((uint64_t)x->pt->duration, (uint64_t)y->pt->duration);
    case SORT_REMIXER:  return cmp_text(field_name(c, x, F_REMIXER), field_name(c, y, F_REMIXER));
    case SORT_LABEL:    return cmp_text(field_name(c, x, F_LABEL), field_name(c, y, F_LABEL));
    case SORT_ORIGINAL: return cmp_text(field_name(c, x, F_ALBUM_ARTIST),
                                        field_name(c, y, F_ALBUM_ARTIST));
    case SORT_KEY:      return cmp_u_set(x->key, y->key);
    case SORT_BITRATE:  return cmp_u_set((uint64_t)RX_MAX(x->pt->bit_rate, 0),
                                         (uint64_t)RX_MAX(y->pt->bit_rate, 0));
    case SORT_COLOR:    return cmp_u_set(x->color, y->color);
    case SORT_PLAYS:    return -cmp_u(tx ? (uint64_t)RX_MAX(tx->play_count, 0) : 0,
                                      ty ? (uint64_t)RX_MAX(ty->play_count, 0) : 0);
    case SORT_ADDED:    return -cmp_u(tx ? (uint64_t)RX_MAX(tx->added, 0) : 0,
                                      ty ? (uint64_t)RX_MAX(ty->added, 0) : 0);
    case SORT_FILE:     return cmp_text(x->file, y->file);
    }
    return 0;
}

static int cmp_tracks(const void *a, const void *b, void *opaque)
{
    const SortCtx *s = opaque;
    const CatTrack *x = &s->c->t[*(const uint32_t *)a], *y = &s->c->t[*(const uint32_t *)b];
    int r = cmp_by(s->c, s->sort, x, y);
    if (!r)
        r = cmp_text(x->title, y->title);
    return r ? r : cmp_u(x->pt->id, y->pt->id);
}

/* The order of a sort, made the first time it is asked for; the default
 * sort is the title's, as rekordbox sent TRACK (rbxport's capture). NULL
 * when out of memory. */
static const uint32_t *order_of(RXCatalog *c, int sort)
{
    if (sort <= SORT_DEFAULT || sort >= NB_SORTS)
        sort = SORT_TITLE;
    if (c->order[sort])
        return c->order[sort];
    uint32_t *o = malloc((size_t)RX_MAX(c->nb, 1) * sizeof(*o));
    if (!o)
        return NULL;
    for (int i = 0; i < c->nb; i++)
        o[i] = (uint32_t)i;
    SortCtx ctx = { .c = c, .sort = sort };
    qsort_r(o, (size_t)c->nb, sizeof(*o), cmp_tracks, &ctx);
    c->order[sort] = o;
    return o;
}

static int sort_id(uint32_t v)
{
    switch (v) {
    case SORT_TITLE: case SORT_ARTIST: case SORT_ALBUM: case SORT_BPM: case SORT_RATING:
    case SORT_GENRE: case SORT_COMMENT: case SORT_TIME: case SORT_REMIXER: case SORT_LABEL:
    case SORT_ORIGINAL: case SORT_KEY: case SORT_BITRATE: case SORT_COLOR: case SORT_PLAYS:
    case SORT_ADDED:
        return (int)v;
    }
    return SORT_DEFAULT;
}

/* --------------------------------------------------------------- menus */

/* What a list of tracks is narrowed to. ids are RX_DB_ALL for any. */
enum FilterKind {
    FILTER_ALL,
    FILTER_GENRE,               /* a genre, b artist, c album */
    FILTER_ARTIST,              /* a artist, b album */
    FILTER_ALBUM,               /* a album */
    FILTER_LABEL,               /* a label, b artist, c album */
    FILTER_ORIGINAL,            /* a album artist (0 none), b album */
    FILTER_REMIXER,             /* a remixer, b album */
    FILTER_BPM,                 /* a BPM x100, b percent */
    FILTER_RATING,
    FILTER_YEAR,                /* a decade, b year */
    FILTER_COLOR,
    FILTER_TIME,                /* a minutes */
    FILTER_BITRATE,
    FILTER_KEYS,                /* keys[] */
    FILTER_SEARCH,              /* text */
    FILTER_ADDED,               /* a year, b month, c day */
    FILTER_HISTORY,             /* a day, yyyymmdd */
};

typedef struct Filter {
    int         kind;
    uint32_t    a, b, c;
    const char *text;
    uint32_t    keys[4];
    int         nb_keys;
} Filter;

static bool any_or(uint32_t want, uint32_t have)
{
    return want == RX_DB_ALL || want == have;
}

static bool search_match(const RXCatalog *c, const CatTrack *t, const char *text)
{
    const RXMeta *m = meta_of(t);
    const char *fields[] = {
        t->title, field_name(c, t, F_ARTIST), field_name(c, t, F_ALBUM),
        field_name(c, t, F_GENRE), field_name(c, t, F_LABEL), field_name(c, t, F_REMIXER),
        field_name(c, t, F_ALBUM_ARTIST), m->comment,
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(fields); i++)
        if (fields[i] && rx_str_contains_fold(fields[i], text))
            return true;
    return false;
}

static bool match(const RXCatalog *c, const CatTrack *t, const Filter *f)
{
    const RXMeta *m = meta_of(t);
    switch (f->kind) {
    case FILTER_ALL:
        return true;
    case FILTER_GENRE:
        return t->pt->genre_id == f->a && any_or(f->b, t->pt->artist_id) &&
               any_or(f->c, field_id(t, F_ALBUM));
    case FILTER_ARTIST:
        return t->pt->artist_id == f->a && any_or(f->b, field_id(t, F_ALBUM));
    case FILTER_ALBUM:
        return field_id(t, F_ALBUM) == f->a;
    case FILTER_LABEL:
        return t->pt->label_id == f->a && any_or(f->b, t->pt->artist_id) &&
               any_or(f->c, field_id(t, F_ALBUM));
    case FILTER_ORIGINAL:
        return t->album_artist == f->a && any_or(f->b, field_id(t, F_ALBUM));
    case FILTER_REMIXER:
        return f->a && t->pt->remixer_id == f->a && any_or(f->b, field_id(t, F_ALBUM));
    case FILTER_BPM: {
        /* Within the percentage of the BPM asked for; at 0 %, the same
         * whole BPM (the BPM menu's rows are whole BPMs, rbxport). */
        if (!t->tempo)
            return false;
        if (!f->b)
            return (t->tempo + 50) / 100 * 100 == f->a;
        uint64_t d = t->tempo > f->a ? t->tempo - f->a : f->a - t->tempo;
        return d * 100 <= (uint64_t)f->a * f->b;
    }
    case FILTER_RATING:
        return t->rating == f->a;
    case FILTER_YEAR:
        return m->year > 0 && (uint32_t)m->year / 10 * 10 == f->a && any_or(f->b, (uint32_t)m->year);
    case FILTER_COLOR:
        return t->color && t->color == f->a;
    case FILTER_TIME:
        return (uint32_t)(t->pt->duration / 60) == f->a;
    case FILTER_BITRATE:
        return (uint32_t)RX_MAX(t->pt->bit_rate, 0) == f->a;
    case FILTER_KEYS:
        for (int i = 0; i < f->nb_keys; i++)
            if (t->key && t->key == f->keys[i])
                return true;
        return false;
    case FILTER_SEARCH:
        return f->text && f->text[0] && search_match(c, t, f->text);
    case FILTER_ADDED:
        return t->added && (uint32_t)t->added / 10000 == f->a &&
               any_or(f->b, (uint32_t)t->added / 100 % 100) && any_or(f->c, (uint32_t)t->added % 100);
    case FILTER_HISTORY:
        return t->played && (uint32_t)t->played == f->a;
    }
    return false;
}

static int menu_alloc(RXCatMenu *m, uint32_t n, bool nums)
{
    m->ids = malloc((size_t)RX_MAX(n, 1) * sizeof(*m->ids));
    m->nums = nums ? calloc((size_t)RX_MAX(n, 1), sizeof(*m->nums)) : NULL;
    if (!m->ids || (nums && !m->nums)) {
        rx_catalog_menu_free(m);
        return RX_ERROR(ENOMEM);
    }
    return 0;
}

void rx_catalog_menu_free(RXCatMenu *m)
{
    free(m->ids);
    free(m->nums);
    *m = (RXCatMenu) { 0 };
}

typedef struct AlbumCtx {
    const RXCatalog *c;
    const uint32_t  *idx;       /* track index of each row */
} AlbumCtx;

/* An album's own order: disc and track number, unnumbered ones after, by
 * title (rbxport puts the track number in the row's position). Tracks of
 * several albums (an album artist's ALL) go album by album, as the ALBUM
 * menu lists the albums. */
static int cmp_album_rows(const void *a, const void *b, void *opaque)
{
    const AlbumCtx *s = opaque;
    const CatTrack *x = &s->c->t[s->idx[*(const uint32_t *)a]];
    const CatTrack *y = &s->c->t[s->idx[*(const uint32_t *)b]];
    const RXMeta *mx = meta_of(x), *my = meta_of(y);
    int r = cmp_text(field_name(s->c, x, F_ALBUM), field_name(s->c, y, F_ALBUM));
    if (!r)
        r = cmp_u(field_id(x, F_ALBUM), field_id(y, F_ALBUM));
    if (!r)
        r = cmp_u_set((uint64_t)RX_MAX(mx->disc_number, 0), (uint64_t)RX_MAX(my->disc_number, 0));
    if (!r)
        r = cmp_u_set((uint64_t)RX_MAX(mx->track_number, 0), (uint64_t)RX_MAX(my->track_number, 0));
    return r ? r : cmp_u(*(const uint32_t *)a, *(const uint32_t *)b);
}

/* The tracks a filter lets through, in the order of the sort, or for an
 * album with its default sort in the album's own order, the track number
 * the position each row shows. */
static int list_tracks(RXCatalog *c, const Filter *f, int sort, bool album, uint32_t flags,
                       RXCatMenu *m)
{
    const uint32_t *o = order_of(c, sort);
    if (!o)
        return RX_ERROR(ENOMEM);
    uint32_t *idx = malloc((size_t)RX_MAX(c->nb, 1) * sizeof(*idx));
    if (!idx)
        return RX_ERROR(ENOMEM);
    uint32_t n = 0;
    for (int i = 0; i < c->nb; i++)
        if (match(c, &c->t[o[i]], f))
            idx[n++] = o[i];
    int ret = menu_alloc(m, n, true);
    if (ret < 0) {
        free(idx);
        return ret;
    }
    *m = (RXCatMenu) { .what = RX_CAT_TRACKS, .sort = sort, .flags = flags, .ids = m->ids,
                       .nums = m->nums, .nb = n };
    uint32_t *rows = malloc((size_t)RX_MAX(n, 1) * sizeof(*rows));
    if (!rows) {
        free(idx);
        rx_catalog_menu_free(m);
        return RX_ERROR(ENOMEM);
    }
    for (uint32_t i = 0; i < n; i++)
        rows[i] = i;
    if (album && sort == SORT_DEFAULT) {
        AlbumCtx ctx = { .c = c, .idx = idx };
        qsort_r(rows, n, sizeof(*rows), cmp_album_rows, &ctx);
    }
    for (uint32_t i = 0; i < n; i++) {
        const CatTrack *t = &c->t[idx[rows[i]]];
        m->ids[i] = t->pt->id;
        m->nums[i] = album ? (uint32_t)RX_MAX(meta_of(t)->track_number, 0) : 0;
    }
    free(rows);
    free(idx);
    return 0;
}

typedef struct OwnCtx {
    const RXCatalog *c;
    const int32_t   *idx;
    int              sort;
} OwnCtx;

static int cmp_own_rows(const void *a, const void *b, void *opaque)
{
    const OwnCtx *s = opaque;
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    const CatTrack *tx = &s->c->t[s->idx[x]], *ty = &s->c->t[s->idx[y]];
    int r = cmp_by(s->c, s->sort == SORT_TITLE ? SORT_DEFAULT : s->sort, tx, ty);
    if (!r && s->sort == SORT_TITLE)
        r = cmp_text(tx->title, ty->title);
    return r ? r : cmp_u(x, y);
}

/* A list in an order of its own (a playlist, a history), each row's
 * position from 1; re-sorted when the player asks for another sort. Ids
 * no longer in the catalog are left out. */
static int own_tracks(RXCatalog *c, const uint32_t *ids, uint32_t nb, int sort, uint32_t flags,
                      RXCatMenu *m)
{
    int32_t *idx = malloc((size_t)RX_MAX(nb, 1) * sizeof(*idx));
    uint32_t *rows = malloc((size_t)RX_MAX(nb, 1) * sizeof(*rows));
    if (!idx || !rows) {
        free(idx);
        free(rows);
        return RX_ERROR(ENOMEM);
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < nb; i++) {
        int ti = track_index(c, ids[i]);
        if (ti >= 0) {
            idx[i] = ti;
            rows[n++] = i;
        }
    }
    int ret = menu_alloc(m, n, true);
    if (ret >= 0) {
        *m = (RXCatMenu) { .what = RX_CAT_TRACKS, .sort = sort, .flags = flags, .ids = m->ids,
                           .nums = m->nums, .nb = n };
        if (sort != SORT_DEFAULT) {
            OwnCtx ctx = { .c = c, .idx = idx, .sort = sort };
            qsort_r(rows, n, sizeof(*rows), cmp_own_rows, &ctx);
        }
        for (uint32_t i = 0; i < n; i++) {
            m->ids[i] = ids[rows[i]];
            m->nums[i] = rows[i] + 1;
        }
    }
    free(idx);
    free(rows);
    return ret;
}

typedef struct NameCtx {
    const RXCatalog *c;
    int              table;
} NameCtx;

static int cmp_name_ids(const void *a, const void *b, void *opaque)
{
    const NameCtx *s = opaque;
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    uint32_t mask = s->table == RX_CAT_ALBUM ? ~ALBUM_BIT : ~0u;
    int r = cmp_text(table_name(s->c, s->table, x & mask), table_name(s->c, s->table, y & mask));
    return r ? r : cmp_u(x, y);
}

static uint32_t table_size(const RXCatalog *c, int table)
{
    switch (table) {
    case RX_CAT_ARTIST: return (uint32_t)RX_MAX(c->plan->nb_artists, 0);
    case RX_CAT_ALBUM:  return (uint32_t)RX_MAX(c->plan->nb_albums, 0);
    case RX_CAT_GENRE:  return (uint32_t)RX_MAX(c->plan->nb_genres, 0);
    case RX_CAT_LABEL:  return (uint32_t)RX_MAX(c->plan->nb_labels, 0);
    }
    return 0;
}

/* The names in `field` of the tracks a filter lets through, by name; an
 * ALL row first when there is more than one row and `all` (rbxport's
 * capture: an artist's albums, ALL then six albums); an "Unknown" album
 * for tracks without one, last, as rekordbox sent it, and so an
 * "Unknown" album artist. */
static int list_names(RXCatalog *c, int field, const Filter *f, bool all, RXCatMenu *m)
{
    int table = field_table(field);
    uint32_t size = table_size(c, table);
    uint8_t *seen = calloc((size_t)size + 1, 1);
    if (!seen)
        return RX_ERROR(ENOMEM);
    bool unknown = false;
    uint32_t n = 0;
    for (int i = 0; i < c->nb; i++) {
        if (!match(c, &c->t[i], f))
            continue;
        uint32_t id = field_id(&c->t[i], field) & ~(field == F_ALBUM ? ALBUM_BIT : 0);
        if (!id) {
            unknown |= (field == F_ALBUM && f->kind != FILTER_ALL) || field == F_ALBUM_ARTIST;
            continue;
        }
        if (id <= size && !seen[id] && table_name(c, table, id)) {
            seen[id] = 1;
            n++;
        }
    }
    uint32_t rows = n + unknown;
    bool with_all = all && rows > 1;
    int ret = menu_alloc(m, rows + with_all, false);
    if (ret < 0) {
        free(seen);
        return ret;
    }
    m->what = RX_CAT_NAMES;
    m->table = table;
    m->nb = 0;
    if (with_all)
        m->ids[m->nb++] = RX_DB_ALL;
    uint32_t first = m->nb;
    for (uint32_t id = 1; id <= size; id++)
        if (seen[id])
            m->ids[m->nb++] = field == F_ALBUM ? album_wire(id) : id;
    NameCtx ctx = { .c = c, .table = table };
    qsort_r(m->ids + first, m->nb - first, sizeof(*m->ids), cmp_name_ids, &ctx);
    if (unknown)
        m->ids[m->nb++] = 0;
    free(seen);
    return 0;
}

static int cmp_u32(const void *a, const void *b)
{
    return cmp_u(*(const uint32_t *)a, *(const uint32_t *)b);
}

/* Values the player prints itself (BPMs, years, minutes ...): distinct,
 * ascending or descending, an ALL row first when asked for (`all` 1 when
 * more than one, 2 always, as rbxport's months and days). */
enum Value { V_BPM, V_RATING, V_DECADE, V_YEAR, V_TIME, V_BITRATE, V_ADDED_YEAR,
             V_ADDED_MONTH, V_ADDED_DAY, V_COLOR, V_HISTORY };

static bool value_of(const CatTrack *t, int what, uint32_t *v)
{
    const RXMeta *m = meta_of(t);
    switch (what) {
    case V_BPM:         *v = (t->tempo + 50) / 100 * 100; return t->tempo > 0;
    case V_RATING:      *v = t->rating; return true;
    case V_DECADE:      *v = (uint32_t)m->year / 10 * 10; return m->year > 0;
    case V_YEAR:        *v = (uint32_t)m->year; return m->year > 0;
    case V_TIME:        *v = (uint32_t)(t->pt->duration / 60); return t->pt->duration > 0;
    case V_BITRATE:     *v = (uint32_t)t->pt->bit_rate; return t->pt->bit_rate > 0;
    case V_ADDED_YEAR:  *v = (uint32_t)t->added / 10000; return t->added > 0;
    case V_ADDED_MONTH: *v = (uint32_t)t->added / 100 % 100; return t->added > 0;
    case V_ADDED_DAY:   *v = (uint32_t)t->added % 100; return t->added > 0;
    case V_COLOR:       *v = t->color; return t->color > 0;
    case V_HISTORY:     *v = (uint32_t)t->played; return t->played > 0;
    }
    return false;
}

static int list_values(RXCatalog *c, int what, const Filter *f, bool descending, int all,
                       int menu_what, uint32_t type, RXCatMenu *m)
{
    uint32_t *v = malloc((size_t)RX_MAX(c->nb, 1) * sizeof(*v));
    if (!v)
        return RX_ERROR(ENOMEM);
    uint32_t n = 0;
    for (int i = 0; i < c->nb; i++)
        if (match(c, &c->t[i], f) && value_of(&c->t[i], what, &v[n]))
            n++;
    qsort(v, n, sizeof(*v), cmp_u32);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!k || v[i] != v[k - 1])
            v[k++] = v[i];
    bool with_all = all == 2 || (all == 1 && k > 1);
    int ret = menu_alloc(m, k + with_all, false);
    if (ret < 0) {
        free(v);
        return ret;
    }
    m->what = menu_what;
    m->type = type;
    m->nb = 0;
    if (with_all)
        m->ids[m->nb++] = RX_DB_ALL;
    for (uint32_t i = 0; i < k; i++)
        m->ids[m->nb++] = descending ? v[k - 1 - i] : v[i];
    free(v);
    return 0;
}

/* The root menu: rekordbox's browse categories (djmdCategory, its ids and
 * the menu item each names, from rbxport's reading of rekordbox 7.2.14's
 * own new library, rbl-db src/new_library.rs) as rekordbox 7.2.11 sends
 * them: the name wrapped in U+FFFA..U+FFFB for the player to translate,
 * the menu's item type (rbxport's capture). A row is sent only when the
 * player's mask, the root request's third argument, has bit menu item - 1
 * (bit 24 for DATE ADDED), which rekordbox applies (rbxport, from its
 * binary; a CDJ-3000 sent 0x05cfffff). rekordbox's default categories in
 * its order (ARTIST ... DATE ADDED; its MATCHING needs rekordbox's related
 * tracks, and FOLDER is never sent), then the others it offers when
 * enabled; the order of those is assumed.
 *
 * ORIGINAL ARTIST's menus list album artists (album_artists), under ALBUM
 * ARTIST, below ALBUM: a player's menus are its firmware's, so a category
 * of our own takes over one it has (docs/LINK.md). Its name is sent as it
 * is, not for the player to translate (`own`). */
static const struct {
    uint32_t    id;
    int         bit;
    const char *name;
    uint32_t    type;
    bool        own;
} root_rows[] = {
    { 2,  1,  "ARTIST",          0x81 },
    { 3,  2,  "ALBUM",           0x82 },
    { 11, 9,  "ALBUM ARTIST",    0x8a, true },
    { 4,  3,  "TRACK",           0x83 },
    { 1,  0,  "GENRE",           0x80 },
    { 12, 10, "KEY",             0x8b },
    { 5,  16, "PLAYLIST",        0x84 },
    { 22, 18, "HISTORY",         0x95 },
    { 18, 19, "SEARCH",          0x91 },
    { 6,  4,  "BPM",             0x85 },
    { 7,  5,  "RATING",          0x86 },
    { 8,  6,  "YEAR",            0x87 },
    { 10, 8,  "LABEL",           0x89 },
    { 15, 12, "COLOR",           0x8e },
    { 19, 13, "TIME",            0x92 },
    { 20, 14, "BITRATE",         0x93 },
    { 9,  7,  "REMIXER",         0x88 },
    { 21, 15, "FILE NAME",       0x94 },
    { 27, 24, "DATE ADDED",      0x8c },
};

/* The sort menu, as rekordbox sent it (rbxport's capture). */
static const struct {
    uint32_t    id;
    const char *name;
    uint32_t    type;
} sort_rows[] = {
    { SORT_DEFAULT, "DEFAULT",  RX_DB_ITEM_MENU_DEFAULT },
    { SORT_TITLE,   "ALPHABET", RX_DB_ITEM_MENU_ALPHABET },
    { SORT_ARTIST,  "ARTIST",   0x81 },
    { SORT_ALBUM,   "ALBUM",    0x82 },
    { SORT_BPM,     "BPM",      0x85 },
    { SORT_RATING,  "RATING",   0x86 },
    { SORT_KEY,     "KEY",      0x8b },
};

static int root_menu(uint32_t mask, RXCatMenu *m)
{
    int ret = menu_alloc(m, RX_ARRAY_ELEMS(root_rows), false);
    if (ret < 0)
        return ret;
    m->what = RX_CAT_ROOT;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(root_rows); i++)
        if (mask & 1u << root_rows[i].bit)
            m->ids[m->nb++] = root_rows[i].id;
    return 0;
}

static int fixed_menu(int what, uint32_t nb, RXCatMenu *m)
{
    int ret = menu_alloc(m, nb, what == RX_CAT_RELATED);
    if (ret < 0)
        return ret;
    m->what = what;
    m->nb = nb;
    return 0;
}

/* Keys that mix with a key, wider with the distance: the key, then its
 * relative major or minor, then its neighbours on the wheel (rbxport's
 * capture: Abm; Abm, B; Abm, B, Dbm, Ebm). */
static int related_keys(uint32_t key, uint32_t distance, uint32_t out[4])
{
    if (key < 1 || key > 24)
        return 0;
    int hour = (int)(key - 1) / 2;
    int minor = (key - 1) % 2 == 0;
    int n = 0;
    out[n++] = key;
    if (distance >= 1)
        out[n++] = (uint32_t)(hour * 2 + minor + 1);
    if (distance >= 2) {
        out[n++] = (uint32_t)((hour + 11) % 12 * 2 + !minor + 1);
        out[n++] = (uint32_t)((hour + 1) % 12 * 2 + !minor + 1);
    }
    return n;
}

static int playlist_folder(RXCatalog *c, uint32_t id, RXCatMenu *m)
{
    const RXPlan *p = c->plan;
    bool exists = id == 0;
    for (int i = 0; i < p->nb_playlists && !exists; i++)
        exists = p->playlists[i].id == id && p->playlists[i].is_folder;
    uint32_t n = 0;
    for (int i = 0; exists && i < p->nb_playlists; i++)
        n += p->playlists[i].parent_id == id && p->playlists[i].id != id;
    int ret = menu_alloc(m, n, true);
    if (ret < 0)
        return ret;
    m->what = RX_CAT_PLAYLISTS;
    /* In the order the plan gives them among their siblings, each with its
     * position from 1 (rbxport's capture shows positions on folder rows). */
    for (int i = 0; exists && i < p->nb_playlists; i++) {
        const RXPlanPlaylist *pl = &p->playlists[i];
        if (pl->parent_id != id || pl->id == id)
            continue;
        uint32_t at = m->nb++;
        while (at > 0 && m->nums[at - 1] > (uint32_t)pl->sort_order + 1) {
            m->ids[at] = m->ids[at - 1];
            m->nums[at] = m->nums[at - 1];
            at--;
        }
        m->ids[at] = pl->id;
        m->nums[at] = (uint32_t)pl->sort_order + 1;
    }
    return 0;
}

static int playlist_tracks(RXCatalog *c, uint32_t id, int sort, RXCatMenu *m)
{
    for (int i = 0; i < c->plan->nb_playlists; i++) {
        const RXPlanPlaylist *pl = &c->plan->playlists[i];
        if (pl->id == id && !pl->is_folder)
            return own_tracks(c, pl->track_ids, (uint32_t)RX_MAX(pl->nb, 0), sort,
                              RX_DB_TRACK_LISTED, m);
    }
    m->what = RX_CAT_EMPTY;
    return 0;
}

/* The histories: each day a track served was played, newest first. */
static int history_days(RXCatalog *c, RXCatMenu *m)
{
    int ret = menu_alloc(m, (uint32_t)c->nb_plays, false);
    if (ret < 0)
        return ret;
    m->what = RX_CAT_HISTORIES;
    m->type = 0;
    m->nb = 0;
    for (int i = c->nb_plays - 1; i >= 0; i--) {
        uint32_t day = (uint32_t)c->plays[i].day;
        bool seen = false;
        for (uint32_t k = 0; k < m->nb && !seen; k++)
            seen = m->ids[k] == day;
        if (!seen)
            m->ids[m->nb++] = day;
    }
    /* Newest day first, whatever the clock did. */
    for (uint32_t i = 1; i < m->nb; i++)
        for (uint32_t k = i; k > 0 && m->ids[k - 1] < m->ids[k]; k--) {
            uint32_t t = m->ids[k];
            m->ids[k] = m->ids[k - 1];
            m->ids[k - 1] = t;
        }
    return 0;
}

/* A day's history: each play that day in the order it was, a track
 * played twice twice. Every row of a history carries the played bit
 * (rbxport's capture). */
static int history_tracks(RXCatalog *c, uint32_t day, int sort, RXCatMenu *m)
{
    uint32_t *ids = malloc((size_t)RX_MAX(c->nb_plays, 1) * sizeof(*ids));
    if (!ids)
        return RX_ERROR(ENOMEM);
    uint32_t n = 0;
    for (int i = 0; i < c->nb_plays; i++)
        if ((uint32_t)c->plays[i].day == day)
            ids[n++] = c->plays[i].track;
    int ret = own_tracks(c, ids, n, sort, RX_DB_TRACK_PLAYED, m);
    free(ids);
    return ret;
}

/* Makes the track's transcode when the plan names one that is not made
 * yet (a dry plan names the cache file before it exists, rx_plan_build_cb),
 * and takes the size of the file that will be read. The plan names the
 * compatibility transcode or the plain one; which is found by asking for
 * both names. The string stays the same: only its file comes to exist. */
static int prepare_source(RXCatalog *c, RXPlanTrack *pt)
{
    const RXTrack *t = pt->track;
    /* A WAV made as it is read: nothing to make, unless the file will not
     * decode that way, when the WAV is made whole now and served instead
     * (the track info reply is the first a player hears of the file). */
    if (pt->pcm) {
        if (!c->check || c->check(c->check_opaque, pt) >= 0 || !t || !t->path || !c->lib)
            return 0;
        char *made = NULL;
        rx_info("link: transcoding %s for a player", t->path);
        int ret = rx_plan_transcode(c->lib, t, RX_TRANSCODE_LINK_ANY, &made);
        if (ret < 0 || !made) {
            rx_warn("link: transcoding %s failed: %s", t->path, strerror(ret < 0 ? -ret : ENOMEM));
            free(made);
            return ret < 0 ? ret : RX_ERROR(ENOMEM);
        }
        free(pt->source);
        pt->source = made;
        pt->pcm = false;
        int64_t size = rx_file_size(made);
        if (size > 0)
            pt->file_size = size;
        return 0;
    }
    if (!t || !t->path || !pt->source || !strcmp(pt->source, t->path) || !c->lib)
        return 0;
    if (!rx_plan_transcode_fresh(t, pt->source)) {
        for (int compat = RX_TRANSCODE_PLAIN; compat <= RX_TRANSCODE_LINK_ANY; compat++) {
            char *cache = NULL;
            int max_rate = 0;
            int ret = rx_plan_transcode_path(c->lib, t, compat, false, &cache, &max_rate);
            bool named = ret > 0 && cache && !strcmp(cache, pt->source);
            free(cache);
            if (!named)
                continue;
            char *made = NULL;
            rx_info("link: transcoding %s for a player", t->path);
            ret = rx_plan_transcode(c->lib, t, compat, &made);
            if (ret < 0) {
                rx_warn("link: transcoding %s failed: %s", t->path, strerror(-ret));
                return ret;
            }
            free(made);
            /* An analysis made before the file was (the preview is asked
             * for first) lacks the MP3's frame count: made again. */
            for (int i = 0; i < NB_ANALYSIS; i++)
                if (c->analysis[i].id == pt->id)
                    analysis_clear(&c->analysis[i]);
            break;
        }
    }
    int64_t size = rx_file_size(pt->source);
    if (size > 0)
        pt->file_size = size;
    return 0;
}

static int track_menu(RXCatalog *c, int what, uint32_t id, RXCatMenu *m)
{
    int ti = track_index(c, id);
    if (ti < 0)
        return 0;
    if (what == RX_CAT_TRACK_INFO && prepare_source(c, c->t[ti].pt) < 0)
        return 0;
    *m = (RXCatMenu) {
        .what  = what,
        .track = id,
        .nb    = what == RX_CAT_METADATA ? 16 : what == RX_CAT_TRACK_INFO ? 7 : 13,
    };
    return 0;
}

/* The search text: the string argument (menus.adoc: sort, the text's size,
 * the text in capitals, 0). */
static char *search_text(const RXDbMsg *q)
{
    for (int i = 1; i < q->nb_args; i++)
        if (q->args[i].type == RX_DB_STR)
            return rx_dbmsg_str(q, i);
    return rx_strdup("");
}

int rx_catalog_menu(RXCatalog *c, const RXDbMsg *q, RXCatMenu *m)
{
    *m = (RXCatMenu) { 0 };
    if (!c)
        return RX_ERROR(ENOENT);
    uint32_t a1 = rx_dbmsg_num(q, 1), a2 = rx_dbmsg_num(q, 2), a3 = rx_dbmsg_num(q, 3);
    uint32_t a4 = rx_dbmsg_num(q, 4);
    int sort = sort_id(a1);
    Filter f = { .kind = FILTER_ALL };
    switch (q->kind) {
    case RX_DB_ROOT:
        /* Without a mask, everything (the older root request has none). */
        return root_menu(q->nb_args > 2 ? a2 : RX_DB_ALL, m);
    case RX_DB_SORTS: {
        int ret = fixed_menu(RX_CAT_SORTS, RX_ARRAY_ELEMS(sort_rows), m);
        for (uint32_t i = 0; ret >= 0 && i < m->nb; i++)
            m->ids[i] = sort_rows[i].id;
        return ret;
    }

    case RX_DB_TRACKS:
        return list_tracks(c, &f, sort, false, 0, m);
    case RX_DB_FILE_NAMES: {
        /* By the file's name unless a sort is asked for (an emulated
         * CDJ-2000 got it in the titles' order, 2026-10-15). */
        int ret = list_tracks(c, &f, sort == SORT_DEFAULT ? SORT_FILE : sort, false, 0, m);
        if (ret >= 0)
            m->what = RX_CAT_FILE_NAMES;
        return ret;
    }

    case RX_DB_GENRES:
        return list_names(c, F_GENRE, &f, false, m);
    case RX_DB_GENRE_ARTISTS:
        f = (Filter) { .kind = FILTER_GENRE, .a = a2, .b = RX_DB_ALL, .c = RX_DB_ALL };
        return list_names(c, F_ARTIST, &f, true, m);
    case RX_DB_GENRE_ARTIST_ALBUMS:
        f = (Filter) { .kind = FILTER_GENRE, .a = a2, .b = a3, .c = RX_DB_ALL };
        return list_names(c, F_ALBUM, &f, true, m);
    case RX_DB_GENRE_TRACKS:
        f = (Filter) { .kind = FILTER_GENRE, .a = a2, .b = a3, .c = a4 };
        return list_tracks(c, &f, sort, a4 != RX_DB_ALL, RX_DB_TRACK_LISTED, m);

    case RX_DB_ARTISTS:
        return list_names(c, F_ARTIST, &f, false, m);
    case RX_DB_ARTIST_ALBUMS:
        f = (Filter) { .kind = FILTER_ARTIST, .a = a2, .b = RX_DB_ALL };
        return list_names(c, F_ALBUM, &f, true, m);
    case RX_DB_ARTIST_ALBUM_TRACKS:
        f = (Filter) { .kind = FILTER_ARTIST, .a = a2, .b = a3 };
        return list_tracks(c, &f, sort, a3 != RX_DB_ALL, RX_DB_TRACK_LISTED, m);

    case RX_DB_ALBUMS:
        return list_names(c, F_ALBUM, &f, false, m);
    case RX_DB_ALBUM_TRACKS:
        f = (Filter) { .kind = FILTER_ALBUM, .a = a2 };
        return list_tracks(c, &f, sort, true, RX_DB_TRACK_LISTED, m);

    case RX_DB_LABELS:
        return list_names(c, F_LABEL, &f, false, m);
    case RX_DB_LABEL_ARTISTS:
        f = (Filter) { .kind = FILTER_LABEL, .a = a2, .b = RX_DB_ALL, .c = RX_DB_ALL };
        return list_names(c, F_ARTIST, &f, true, m);
    case RX_DB_LABEL_ARTIST_ALBUMS:
        f = (Filter) { .kind = FILTER_LABEL, .a = a2, .b = a3, .c = RX_DB_ALL };
        return list_names(c, F_ALBUM, &f, true, m);
    case RX_DB_LABEL_TRACKS:
        f = (Filter) { .kind = FILTER_LABEL, .a = a2, .b = a3, .c = a4 };
        return list_tracks(c, &f, sort, a4 != RX_DB_ALL, RX_DB_TRACK_LISTED, m);

    case RX_DB_ORIGINALS:
    case RX_DB_REMIXERS:
        f.kind = FILTER_ALL;
        return list_names(c, q->kind == RX_DB_ORIGINALS ? F_ALBUM_ARTIST : F_REMIXER, &f, false, m);
    case RX_DB_ORIGINAL_ALBUMS:
    case RX_DB_REMIXER_ALBUMS:
        f = (Filter) { .kind = q->kind == RX_DB_ORIGINAL_ALBUMS ? FILTER_ORIGINAL : FILTER_REMIXER,
                       .a = a2, .b = RX_DB_ALL };
        return list_names(c, F_ALBUM, &f, true, m);
    case RX_DB_ORIGINAL_TRACKS:
    case RX_DB_REMIXER_TRACKS:
        /* An album artist's ALL: its albums one after the other, each in
         * its own order (cmp_album_rows). */
        f = (Filter) { .kind = q->kind == RX_DB_ORIGINAL_TRACKS ? FILTER_ORIGINAL : FILTER_REMIXER,
                       .a = a2, .b = a3 };
        return list_tracks(c, &f, sort, a3 != RX_DB_ALL || q->kind == RX_DB_ORIGINAL_TRACKS,
                           RX_DB_TRACK_LISTED, m);

    /* The numeric menus' rows are values the player prints (rbxport). */
    case RX_DB_BPMS:
        return list_values(c, V_BPM, &f, false, 0, RX_CAT_NUMBERS, RX_DB_ITEM_TEMPO, m);
    case RX_DB_BPM_RANGES: {
        /* The tolerances, 0 to 6 % (menus.adoc 1206). */
        int ret = fixed_menu(RX_CAT_NUMBERS, 7, m);
        for (uint32_t i = 0; ret >= 0 && i < 7; i++)
            m->ids[i] = i;
        m->type = RX_DB_ITEM_TEMPO;
        return ret;
    }
    case RX_DB_BPM_TRACKS:
        f = (Filter) { .kind = FILTER_BPM, .a = a2, .b = RX_MIN(a3, 6u) };
        return list_tracks(c, &f, sort, false, 0, m);
    case RX_DB_RATINGS:
        return list_values(c, V_RATING, &f, true, 0, RX_CAT_NUMBERS, RX_DB_ITEM_RATING, m);
    case RX_DB_RATING_TRACKS:
        f = (Filter) { .kind = FILTER_RATING, .a = a2 };
        return list_tracks(c, &f, sort, false, 0, m);
    case RX_DB_DECADES:
        return list_values(c, V_DECADE, &f, true, 0, RX_CAT_NUMBERS, RX_DB_ITEM_YEAR, m);
    case RX_DB_DECADE_YEARS:
        f = (Filter) { .kind = FILTER_YEAR, .a = a2, .b = RX_DB_ALL };
        return list_values(c, V_YEAR, &f, true, 1, RX_CAT_NUMBERS, RX_DB_ITEM_YEAR, m);
    case RX_DB_YEAR_TRACKS:
        f = (Filter) { .kind = FILTER_YEAR, .a = a2, .b = a3 };
        return list_tracks(c, &f, sort, false, 0, m);
    case RX_DB_COLORS:
        return list_values(c, V_COLOR, &f, false, 0, RX_CAT_COLORS, 0, m);
    case RX_DB_COLOR_TRACKS:
        f = (Filter) { .kind = FILTER_COLOR, .a = a2 };
        return list_tracks(c, &f, sort, false, 0, m);
    case RX_DB_TIMES:
        return list_values(c, V_TIME, &f, false, 0, RX_CAT_NUMBERS, RX_DB_ITEM_DURATION, m);
    case RX_DB_TIME_TRACKS:
        f = (Filter) { .kind = FILTER_TIME, .a = a2 };
        return list_tracks(c, &f, sort, false, 0, m);
    case RX_DB_BITRATES:
        return list_values(c, V_BITRATE, &f, true, 0, RX_CAT_NUMBERS, RX_DB_ITEM_BIT_RATE, m);
    case RX_DB_BITRATE_TRACKS:
        f = (Filter) { .kind = FILTER_BITRATE, .a = a2 };
        return list_tracks(c, &f, sort, false, 0, m);

    case RX_DB_ADDED_YEARS:
        return list_values(c, V_ADDED_YEAR, &f, true, 0, RX_CAT_NUMBERS, RX_DB_ITEM_DATE_ADDED, m);
    case RX_DB_ADDED_MONTHS:
        f = (Filter) { .kind = FILTER_ADDED, .a = a2, .b = RX_DB_ALL, .c = RX_DB_ALL };
        return list_values(c, V_ADDED_MONTH, &f, false, 2, RX_CAT_NUMBERS, RX_DB_ITEM_DATE_ADDED, m);
    case RX_DB_ADDED_DAYS:
        /* ALL months: only the ALL row, the year's tracks come next (rbxport). */
        f = (Filter) { .kind = FILTER_ADDED, .a = a2, .b = a3, .c = RX_DB_ALL };
        if (a3 == RX_DB_ALL)
            f.a = 0;
        return list_values(c, V_ADDED_DAY, &f, false, 2, RX_CAT_NUMBERS, RX_DB_ITEM_DATE_ADDED, m);
    case RX_DB_ADDED_TRACKS:
        f = (Filter) { .kind = FILTER_ADDED, .a = a2, .b = a3, .c = a4 };
        return list_tracks(c, &f, sort, false, 0, m);

    case RX_DB_KEYS:
    case RX_DB_KEYS_2000: {
        /* All 24, by their ids (rbxport's capture of the KEY menu). A
         * CDJ-2000 asks for the menu by the root row's type, not its id:
         * the same list. */
        int ret = fixed_menu(RX_CAT_KEYS, 24, m);
        for (uint32_t i = 0; ret >= 0 && i < 24; i++)
            m->ids[i] = i + 1;
        return ret;
    }
    case RX_DB_RELATED_KEYS: {
        int ret = fixed_menu(RX_CAT_RELATED, a2 >= 1 && a2 <= 24 ? 3 : 0, m);
        for (uint32_t i = 0; ret >= 0 && i < m->nb; i++) {
            m->ids[i] = a2;
            m->nums[i] = i;
        }
        return ret;
    }
    case RX_DB_KEY_TRACKS:
        f = (Filter) { .kind = FILTER_KEYS };
        f.nb_keys = related_keys(a2, RX_MIN(a3, 2u), f.keys);
        return list_tracks(c, &f, sort, false, 0, m);
    case RX_DB_KEY_TRACKS_2000:
        f = (Filter) { .kind = FILTER_KEYS };
        f.nb_keys = related_keys(a2, 0, f.keys);
        return list_tracks(c, &f, sort, false, 0, m);

    case RX_DB_PLAYLIST:
        if (a3 == 1)
            return playlist_folder(c, a2, m);
        return playlist_tracks(c, a2, sort, m);

    case RX_DB_HISTORIES:
        return history_days(c, m);
    case RX_DB_HISTORY_TRACKS:
        return history_tracks(c, a2, sort, m);

    case RX_DB_SEARCH:
    case RX_DB_SEARCH_LIVE: {
        char *text = search_text(q);
        if (!text)
            return RX_ERROR(ENOMEM);
        f = (Filter) { .kind = FILTER_SEARCH, .text = text };
        int ret = list_tracks(c, &f, sort, false, 0, m);
        free(text);
        return ret;
    }

    case RX_DB_METADATA:
        return track_menu(c, RX_CAT_METADATA, a1, m);
    case RX_DB_TRACK_INFO:
        return track_menu(c, RX_CAT_TRACK_INFO, a1, m);
    case RX_DB_DELIVERY:
        return track_menu(c, RX_CAT_DELIVERY, a1, m);
    }
    return RX_ERROR(ENOENT);
}

uint32_t rx_catalog_menu_find(const RXCatMenu *m, uint32_t id)
{
    if (m->ids)
        for (uint32_t i = 0; i < m->nb; i++)
            if (m->ids[i] == id)
                return i;
    return RX_DB_ALL;
}

/* -------------------------------------------------------------- render */

#define FFFA "\xef\xbf\xba"
#define FFFB "\xef\xbf\xbb"

static const char *const color_names[9] = {
    "", "Pink", "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple",
};

typedef struct Texts {
    char t1[512], t2[512];
} Texts;

static void heading(RXDbItem *it, uint32_t id, const char *label, uint32_t type, bool own,
                    Texts *tx)
{
    snprintf(tx->t1, sizeof(tx->t1), own ? "%s" : FFFA "%s" FFFB, label);
    *it = (RXDbItem) { .id = id, .text = tx->t1, .type = type };
}

static const char *key_name(const RXCatalog *c, uint32_t key, char *buf, size_t size)
{
    if (key < 1 || key > 24)
        return "";
    const char *notation = c->plan->key_notation[0] ? c->plan->key_notation : "classic";
    return rx_key_display(key_classic[key - 1], notation, buf, size);
}

static void fmt_date(int32_t ymd, char *buf, size_t size)
{
    if (ymd > 0)
        snprintf(buf, size, "%04d-%02d-%02d", ymd / 10000, ymd / 100 % 100, ymd % 100);
    else if (size)
        buf[0] = '\0';
}

/* A track row (rbxport's capture of rekordbox's, rbl-dbserver src/item.rs):
 * the column's value first, then the id, the title, the column's text,
 * title type with the column's in its second byte, the flags, the id
 * again (the older layout's artwork id), the position, 0x100, the key's
 * place on the wheel, its id and name, the tempo. */
static void track_item(const RXCatalog *c, const CatTrack *t, int column, bool extended,
                       uint32_t flags, uint32_t pos, RXDbItem *it, Texts *tx)
{
    const RXPlanTrack *pt = t->pt;
    const RXMeta *m = meta_of(t);
    uint32_t a = pt->id, sub = 0;
    const char *text2 = "";
    switch (column ? column : DEFAULT_COLUMN) {
    case SORT_ARTIST:   sub = RX_DB_ITEM_ARTIST; a = pt->artist_id;
                        text2 = field_name(c, t, F_ARTIST); break;
    case SORT_ALBUM:    sub = RX_DB_ITEM_ALBUM; a = field_id(t, F_ALBUM);
                        text2 = field_name(c, t, F_ALBUM); break;
    case SORT_GENRE:    sub = RX_DB_ITEM_GENRE; a = pt->genre_id;
                        text2 = field_name(c, t, F_GENRE); break;
    case SORT_LABEL:    sub = RX_DB_ITEM_LABEL; a = pt->label_id;
                        text2 = field_name(c, t, F_LABEL); break;
    case SORT_REMIXER:  sub = RX_DB_ITEM_REMIXER; a = pt->remixer_id;
                        text2 = field_name(c, t, F_REMIXER); break;
    case SORT_ORIGINAL: sub = RX_DB_ITEM_ORIGINAL; a = t->album_artist;
                        text2 = field_name(c, t, F_ALBUM_ARTIST); break;
    case SORT_COMMENT:  sub = RX_DB_ITEM_COMMENT; text2 = nonnull(m->comment); break;
    case SORT_RATING:   sub = RX_DB_ITEM_RATING; a = t->rating; break;
    case SORT_TIME:     sub = RX_DB_ITEM_DURATION; a = (uint32_t)pt->duration; break;
    case SORT_BITRATE:  sub = RX_DB_ITEM_BIT_RATE; a = (uint32_t)RX_MAX(pt->bit_rate, 0); break;
    case SORT_COLOR:    sub = RX_DB_ITEM_COLOR_NONE + t->color; a = t->color; break;
    case SORT_PLAYS:    sub = RX_DB_ITEM_PLAY_COUNT;
                        a = pt->track ? (uint32_t)RX_MAX(pt->track->play_count, 0) : 0; break;
    case SORT_ADDED:    sub = RX_DB_ITEM_DATE_ADDED; fmt_date(t->added, tx->t2, sizeof(tx->t2));
                        text2 = tx->t2; break;
    case SORT_BPM:      sub = RX_DB_ITEM_TEMPO; a = t->tempo; break;
    case SORT_KEY:      sub = RX_DB_ITEM_KEY; a = pt->key_id; text2 = pt->key_name; break;
    }
    *it = (RXDbItem) {
        .a     = a,
        .id    = pt->id,
        .text  = t->title,
        .text2 = text2,
        .type  = sub << 8 | RX_DB_ITEM_TITLE,
        .flags = flags,
        /* The djl-analysis' older layout has the artwork id here, which
         * its clients ask 2003 for; rekordbox's has the track's id, which
         * a CDJ-3000 asks for (rbxport's capture). */
        .c     = extended ? pt->id : pt->artwork_id,
        .d     = pos,
        .e     = 0x100,
        .key   = t->key,
        .art   = pt->key_id,
        .text3 = pt->key_name,
        .f     = t->tempo,
    };
}

static RXDbItem line(uint32_t a, uint32_t id, const char *text, uint32_t type)
{
    return (RXDbItem) { .a = a, .id = id, .text = text, .type = type };
}

/* The rows of a track's metadata (2002): sixteen, in the order rekordbox
 * sent a CDJ-3000 (rbxport's capture, its test
 * metadata_and_track_info_have_the_captured_rows: the year, 0x11, before
 * the bit rate, 0x10); a session set up the older way gets
 * the djl-analysis' eleven in its order first (track_metadata.adoc, items 1
 * to 11), then the five others, and a title row of the plain title type,
 * which clients written from it look for. */
enum Meta { M_TITLE, M_ARTIST, M_ALBUM, M_DURATION, M_TEMPO, M_KEY, M_RATING, M_COLOR, M_GENRE,
            M_ADDED, M_COMMENT, M_BITRATE, M_YEAR, M_LABEL, M_ORIGINAL, M_REMIXER };
static const uint8_t meta_rows[2][16] = {
    { M_TITLE, M_ARTIST, M_ALBUM, M_DURATION, M_TEMPO, M_COMMENT, M_KEY, M_RATING, M_COLOR,
      M_GENRE, M_ADDED, M_BITRATE, M_YEAR, M_LABEL, M_ORIGINAL, M_REMIXER },
    { M_TITLE, M_ARTIST, M_ALBUM, M_DURATION, M_TEMPO, M_KEY, M_RATING, M_COLOR, M_GENRE,
      M_ADDED, M_COMMENT, M_YEAR, M_BITRATE, M_LABEL, M_ORIGINAL, M_REMIXER },
};

static void metadata_item(const RXCatalog *c, const CatTrack *t, uint32_t row, bool extended,
                          RXDbItem *it, Texts *tx)
{
    const RXPlanTrack *pt = t->pt;
    const RXMeta *m = meta_of(t);
    switch (meta_rows[extended][row]) {
    case M_TITLE:
        track_item(c, t, DEFAULT_COLUMN, extended, 0, 0, it, tx);
        if (!extended) {
            it->type = RX_DB_ITEM_TITLE;
            it->text2 = "";
        }
        break;
    case M_ARTIST:
        *it = line(1, pt->artist_id, field_name(c, t, F_ARTIST), RX_DB_ITEM_ARTIST);
        break;
    case M_ALBUM:
        *it = line(1, field_id(t, F_ALBUM), field_name(c, t, F_ALBUM), RX_DB_ITEM_ALBUM);
        break;
    case M_DURATION: *it = line(0, (uint32_t)pt->duration, "", RX_DB_ITEM_DURATION); break;
    case M_TEMPO:    *it = line(0, t->tempo, "", RX_DB_ITEM_TEMPO); break;
    case M_KEY:      *it = line(1, pt->key_id, pt->key_name, RX_DB_ITEM_KEY); break;
    case M_RATING:   *it = line(0, t->rating, "", RX_DB_ITEM_RATING); break;
    /* The colour in the item type and its name in the text, as players
     * send it (track_metadata.adoc, item 9); 0x13 with nothing for none,
     * as rekordbox did (rbxport's capture). */
    case M_COLOR:
        *it = line(0, t->color, color_names[t->color], RX_DB_ITEM_COLOR_NONE + t->color);
        break;
    case M_GENRE:    *it = line(0, pt->genre_id, field_name(c, t, F_GENRE), RX_DB_ITEM_GENRE); break;
    case M_ADDED:
        fmt_date(t->added, tx->t1, sizeof(tx->t1));
        *it = line(1, pt->id, tx->t1, RX_DB_ITEM_DATE_ADDED);
        break;
    case M_COMMENT:  *it = line(0, pt->id, nonnull(m->comment), RX_DB_ITEM_COMMENT); break;
    case M_BITRATE:
        *it = line(0, (uint32_t)RX_MAX(pt->bit_rate, 0), "", RX_DB_ITEM_BIT_RATE);
        break;
    case M_YEAR:     *it = line(0, (uint32_t)RX_MAX(m->year, 0), "", RX_DB_ITEM_YEAR); break;
    case M_LABEL:    *it = line(0, pt->label_id, field_name(c, t, F_LABEL), RX_DB_ITEM_LABEL); break;
    case M_ORIGINAL:
        *it = line(0, t->album_artist, field_name(c, t, F_ALBUM_ARTIST), RX_DB_ITEM_ORIGINAL);
        break;
    case M_REMIXER:
        *it = line(0, pt->remixer_id, field_name(c, t, F_REMIXER), RX_DB_ITEM_REMIXER);
        break;
    }
}

/* Track info (2102), the load (rbxport's capture): a first row that gives
 * the file's type, the duration, the tempo, the comment, then the path the
 * player reads over NFS with the file's size, a row of 1, the key.
 *
 * The first row's second field is the file type, rekordbox's (1 MP3, 4
 * M4A, 5 FLAC, 11 WAV, 12 AIFF), and a player decodes the file as that: a
 * CDJ-2000nexus serving its own USB sends 1, 4, 0xb and 0xc for an MP3,
 * an M4A, a WAV and an AIFF (dysentery's S13-format-ground-truth capture),
 * rekordbox sent 1 for its MP3s (rbxport's capture), and a CDJ-900 told 1
 * for a WAV hunts through it for MPEG frames and never plays it
 * (2026-10-14, two CDJ-900s). In the older layout the row is the nexus's,
 * a title row and nothing more; a CDJ-3000's is rekordbox's, shaped as a
 * track row, in which rekordbox sent a copyright text the file had (rxbox
 * has none: blank). The 2f row is 1 for every format in both captures. */
static void info_item(const CatTrack *t, uint32_t row, bool extended, RXDbItem *it)
{
    const RXPlanTrack *pt = t->pt;
    uint32_t type = (uint32_t)pt->file_type;
    switch (row) {
    case 0:
        if (extended)
            *it = (RXDbItem) { .a = 1, .id = type, .type = 0x2304, .e = 0x100, .key = t->key,
                               .art = pt->key_id, .text3 = pt->key_name, .f = t->tempo };
        else
            *it = (RXDbItem) { .id = type, .type = RX_DB_ITEM_TITLE };
        break;
    case 1: *it = line(0, (uint32_t)pt->duration, "", RX_DB_ITEM_DURATION); break;
    case 2: *it = line(0, t->tempo, "", RX_DB_ITEM_TEMPO); break;
    case 3: *it = line(0, pt->id, nonnull(meta_of(t)->comment), RX_DB_ITEM_COMMENT); break;
    case 4:
        *it = line((uint32_t)RX_CLAMP(pt->file_size, 0, (int64_t)UINT32_MAX), pt->id,
                   nonnull(pt->usb_path), RX_DB_ITEM_PATH);
        break;
    case 5: *it = line(0, 1, "", RX_DB_ITEM_INFO_2F); break;
    default: *it = line(0, pt->key_id, pt->key_name, RX_DB_ITEM_KEY); break;
    }
}

/* Delivery info (2602), what a CDJ-3000 reports to KUVO after a load: the
 * thirteen rows rekordbox 7.2.11 sent in its order (rbxport,
 * verification/link/kuvo-delivery-20260919.txt, as its tests/session.rs
 * holds them). */
static void delivery_item(const RXCatalog *c, const CatTrack *t, uint32_t row, RXDbItem *it)
{
    const RXPlanTrack *pt = t->pt;
    switch (row) {
    case 0:  *it = line(0, 0, "", RX_DB_ITEM_DELIVERY_36); break;
    case 1:  *it = line(0, pt->artist_id, field_name(c, t, F_ARTIST), RX_DB_ITEM_ARTIST); break;
    case 2:  *it = line(0, pt->key_id, "", RX_DB_ITEM_KEY); break;
    case 3:  *it = line(0, (uint32_t)pt->duration, "", RX_DB_ITEM_DURATION); break;
    case 4:
        *it = (RXDbItem) { .a = pt->id, .id = pt->id, .text = t->title, .type = RX_DB_ITEM_TITLE,
                           .flags = RX_DB_TRACK_LISTED, .c = pt->id, .e = 0x100, .f = t->tempo };
        break;
    case 5:  *it = line(0, pt->id, nonnull(meta_of(t)->comment), RX_DB_ITEM_COMMENT); break;
    case 6:  *it = line(0, field_id(t, F_ALBUM), field_name(c, t, F_ALBUM), RX_DB_ITEM_ALBUM); break;
    case 7:  *it = line(0, t->tempo, "", RX_DB_ITEM_TEMPO); break;
    case 8:  *it = line(0, pt->id, "", RX_DB_ITEM_DELIVERY_37); break;
    case 9:  *it = line(0, pt->label_id, field_name(c, t, F_LABEL), RX_DB_ITEM_LABEL); break;
    case 10: *it = line(0, (uint32_t)pt->file_type, "", RX_DB_ITEM_FILE_TYPE); break;
    case 11: *it = line(0, pt->genre_id, field_name(c, t, F_GENRE), RX_DB_ITEM_GENRE); break;
    default: *it = (RXDbItem) { .a = pt->id, .type = RX_DB_ITEM_DELIVERY_ID, .c = 1 }; break;
    }
}

static const RXPlanPlaylist *playlist_of(const RXPlan *p, uint32_t id)
{
    for (int i = 0; i < p->nb_playlists; i++)
        if (p->playlists[i].id == id)
            return &p->playlists[i];
    return NULL;
}

/* One row of a menu; false when its id is gone from the catalog. */
static bool item_of(RXCatalog *c, const RXCatMenu *m, uint32_t i, int column, bool extended,
                    RXDbItem *it, Texts *tx)
{
    uint32_t id = m->ids ? m->ids[i] : 0, num = m->nums ? m->nums[i] : 0;
    char buf[64];
    if (id == RX_DB_ALL && (m->what == RX_CAT_NAMES || m->what == RX_CAT_NUMBERS)) {
        heading(it, RX_DB_ALL, "ALL", RX_DB_ITEM_ALL, false, tx);
        return true;
    }
    switch (m->what) {
    case RX_CAT_ROOT:
        for (size_t k = 0; k < RX_ARRAY_ELEMS(root_rows); k++)
            if (root_rows[k].id == id) {
                heading(it, id, root_rows[k].name, root_rows[k].type, root_rows[k].own, tx);
                return true;
            }
        return false;
    case RX_CAT_SORTS:
        for (size_t k = 0; k < RX_ARRAY_ELEMS(sort_rows); k++)
            if (sort_rows[k].id == id) {
                heading(it, id, sort_rows[k].name, sort_rows[k].type, false, tx);
                return true;
            }
        return false;
    case RX_CAT_TRACKS:
    case RX_CAT_FILE_NAMES: {
        int ti = track_index(c, id);
        if (ti < 0)
            return false;
        track_item(c, &c->t[ti], column, extended, m->flags, num, it, tx);
        if (m->what == RX_CAT_FILE_NAMES)
            it->text = c->t[ti].file;
        return true;
    }
    case RX_CAT_NAMES: {
        /* An album row carries its id again (rbxport's capture); its
         * "Unknown" for tracks without one, as rekordbox sent it. */
        static const uint32_t types[] = {
            [RX_CAT_ARTIST] = RX_DB_ITEM_ARTIST, [RX_CAT_ALBUM] = RX_DB_ITEM_ALBUM,
            [RX_CAT_GENRE] = RX_DB_ITEM_GENRE, [RX_CAT_LABEL] = RX_DB_ITEM_LABEL,
        };
        bool album = m->table == RX_CAT_ALBUM;
        const char *name = id ? table_name(c, m->table, album ? id & ~ALBUM_BIT : id) :
                           album || m->table == RX_CAT_ARTIST ? "Unknown" : NULL;
        if (!name)
            return false;
        *it = (RXDbItem) { .id = id, .text = name, .type = types[m->table], .c = album ? id : 0 };
        return true;
    }
    case RX_CAT_NUMBERS:
        *it = (RXDbItem) { .id = id, .type = m->type };
        return true;
    case RX_CAT_COLORS:
        if (id < 1 || id > 8)
            return false;
        *it = (RXDbItem) { .id = id, .text = color_names[id], .type = RX_DB_ITEM_COLOR_NONE + id };
        return true;
    case RX_CAT_KEYS:
        snprintf(tx->t1, sizeof(tx->t1), "%s", key_name(c, id, buf, sizeof(buf)));
        *it = (RXDbItem) { .id = id, .text = tx->t1, .type = RX_DB_ITEM_KEY };
        return true;
    case RX_CAT_RELATED: {
        uint32_t keys[4];
        int n = related_keys(id, num, keys);
        size_t len = 0;
        tx->t1[0] = '\0';
        for (int k = 0; k < n && len < sizeof(tx->t1); k++)
            len += (size_t)snprintf(tx->t1 + len, sizeof(tx->t1) - len, "%s%s", k ? ", " : "",
                                    key_name(c, keys[k], buf, sizeof(buf)));
        *it = (RXDbItem) { .a = num, .id = id, .text = tx->t1, .type = RX_DB_ITEM_KEY };
        return true;
    }
    case RX_CAT_PLAYLISTS: {
        const RXPlanPlaylist *pl = playlist_of(c->plan, id);
        if (!pl)
            return false;
        *it = (RXDbItem) {
            .id   = id,
            .text = nonnull(pl->name),
            .type = pl->is_folder ? RX_DB_ITEM_FOLDER : RX_DB_ITEM_PLAYLIST,
            .d    = num,
        };
        return true;
    }
    case RX_CAT_HISTORIES:
        fmt_date((int32_t)id, buf, sizeof(buf));
        snprintf(tx->t1, sizeof(tx->t1), "HISTORY %s", buf);
        *it = (RXDbItem) { .id = id, .text = tx->t1, .type = RX_DB_ITEM_HISTORY };
        return true;
    case RX_CAT_METADATA:
    case RX_CAT_TRACK_INFO:
    case RX_CAT_DELIVERY: {
        int ti = track_index(c, m->track);
        if (ti < 0)
            return false;
        if (m->what == RX_CAT_METADATA)
            metadata_item(c, &c->t[ti], i, extended, it, tx);
        else if (m->what == RX_CAT_TRACK_INFO)
            info_item(&c->t[ti], i, extended, it);
        else
            delivery_item(c, &c->t[ti], i, it);
        return true;
    }
    }
    return false;
}

void rx_catalog_render(RXCatalog *c, const RXCatMenu *m, uint32_t offset, uint32_t count,
                       int column, bool extended, bool latin1, uint32_t txid, RXWr *w)
{
    for (uint32_t i = offset; c && i < m->nb && i - offset < count; i++) {
        RXDbItem it;
        Texts tx;
        if (item_of(c, m, i, column, extended, &it, &tx))
            rx_dbmsg_item_latin1(w, txid, &it, extended, latin1);
    }
}

/* ------------------------------------------------------------- artwork */

int rx_catalog_artwork(RXCatalog *c, uint32_t id, bool item, bool large,
                       const uint8_t **jpeg, size_t *size)
{
    *jpeg = NULL;
    *size = 0;
    if (!c)
        return RX_ERROR(ENOENT);
    uint32_t art = id;
    if (item && id & ALBUM_BIT) {
        /* An album's: its first track's that has one (assumed; rekordbox
         * sent no artwork for albums in rbxport's capture). */
        art = 0;
        for (int i = 0; i < c->nb && !art; i++)
            if (field_id(&c->t[i], F_ALBUM) == id)
                art = c->t[i].pt->artwork_id;
    } else if (item) {
        int ti = track_index(c, id);
        art = ti >= 0 ? c->t[ti].pt->artwork_id : 0;
    }
    const RXPlanArtwork *a = artwork_of(c->plan, art);
    if (!a)
        return RX_ERROR(ENOENT);
    *jpeg = large ? a->jpeg240 : a->jpeg80;
    *size = large ? a->size240 : a->size80;
    return *jpeg && *size ? 0 : RX_ERROR(ENOENT);
}

/* ------------------------------------------------------------ analysis */

static Analysis *analysis_of(RXCatalog *c, uint32_t id)
{
    int ti = track_index(c, id);
    if (ti < 0)
        return NULL;
    Analysis *old = &c->analysis[0];
    for (int i = 0; i < NB_ANALYSIS; i++) {
        Analysis *a = &c->analysis[i];
        if (a->id == id) {
            a->used = ++c->clock;
            return a;
        }
        if (a->used < old->used)
            old = a;
    }
    analysis_clear(old);
    /* The frame tables Link's plan leaves out (RX_PLAN_STORED reads no
     * audio) and the analysis needs (anlz.c): an MP3's frame count for its
     * PVBR, a FLAC's seek points for its PVB2, read from this one file now
     * that a player asks for it. */
    RXPlanTrack full = *c->t[ti].pt;
    RXFlacInfo flac = { 0 };
    if (!strcmp(full.probe.codec, "mp3") && !full.probe.mp3.valid) {
        RXMp3Info mp3;
        if (rx_mp3_scan(full.source, &mp3) >= 0)
            full.probe.mp3 = mp3;
    } else if (!strcmp(full.probe.codec, "flac") && !full.probe.flac.valid &&
               rx_file_exists(full.source) && rx_flac_scan(full.source, &flac) >= 0) {
        full.probe.flac = flac;
    }
    RXWaveforms w = { 0 };
    bool waves = c->lib && c->lib->db && rx_library_waveforms_load(c->lib, id, &w) >= 0;
    int ret = rx_anlz_build(&full, waves ? &w : NULL, &old->files);
    rx_flac_info_free(&flac);
    if (waves)
        rx_waveforms_free(&w);
    if (ret < 0) {
        rx_warn("link: the analysis of track %u: %s", id, strerror(-ret));
        analysis_clear(old);
        return NULL;
    }
    const uint8_t *data[3] = { old->files.dat, old->files.ext, old->files.ex2 };
    const size_t size[3] = { old->files.dat_size, old->files.ext_size, old->files.ex2_size };
    for (int i = 0; i < 3; i++)
        if (data[i] && rx_anlz_parse(data[i], size[i], &old->tags[i], &old->nb_tags[i]) < 0)
            old->nb_tags[i] = 0;
    old->id = id;
    old->used = ++c->clock;
    return old;
}

/* A tag's data after its header, as the file holds it. */
static const uint8_t *tag_data(const Analysis *a, int file, const char *fourcc, size_t *len,
                               const RXAnlzTag **tag)
{
    for (int i = 0; i < a->nb_tags[file]; i++) {
        const RXAnlzTag *t = &a->tags[file][i];
        if (memcmp(t->fourcc, fourcc, 4))
            continue;
        RXRd r = rx_rd(t->raw, t->raw_size);
        uint32_t header = rx_rd_at_u32(&r, 4);
        if (header < 12 || header > t->raw_size)
            return NULL;
        if (tag)
            *tag = t;
        *len = t->raw_size - header;
        return t->raw + header;
    }
    return NULL;
}

/* The constant rekordbox appends to every preview reply (rbxport's
 * capture: 9e eb 78 10 after both tracks' columns). */
static const uint8_t preview_tail[4] = { 0x9e, 0xeb, 0x78, 0x10 };

int rx_catalog_preview(RXCatalog *c, uint32_t track, RXWr *blob)
{
    Analysis *a = analysis_of(c, track);
    size_t n, n2 = 0;
    const uint8_t *pwav = a ? tag_data(a, 0, "PWAV", &n, NULL) : NULL;
    if (!pwav)
        return RX_ERROR(ENOENT);
    const uint8_t *pwv2 = tag_data(a, 0, "PWV2", &n2, NULL);
    /* Each PWAV column as two bytes, height then whiteness, then the PWV2
     * columns, the height alone, 1 to 15: a Nexus player drops a preview
     * with a byte above 15 or a zero column (track_metadata.adoc
     * "Waveform Previews": 900 bytes; rbxport's capture: 904 with the
     * tail, and its issue 278). */
    for (size_t i = 0; i < n; i++) {
        rx_wr_u8(blob, pwav[i] & 0x1f);
        rx_wr_u8(blob, pwav[i] >> 5);
    }
    for (size_t i = 0; pwv2 && i < n2; i++)
        rx_wr_u8(blob, (uint8_t)RX_MAX(pwv2[i] & 0x0f, 1));
    rx_wr_bytes(blob, preview_tail, sizeof(preview_tail));
    return 0;
}

int rx_catalog_detail(RXCatalog *c, uint32_t track, RXWr *blob)
{
    Analysis *a = analysis_of(c, track);
    size_t n;
    const uint8_t *pwv3 = a ? tag_data(a, 1, "PWV3", &n, NULL) : NULL;
    if (!pwv3 || n > UINT32_MAX)
        return RX_ERROR(ENOENT);
    /* A header of the column count, 1, the count, 150 columns a second,
     * 1, little-endian, then the PWV3 columns (rbxport's capture). */
    rx_wr_u32le(blob, (uint32_t)n);
    rx_wr_u32le(blob, 1);
    rx_wr_u32le(blob, (uint32_t)n);
    rx_wr_u32le(blob, 150);
    rx_wr_u32le(blob, 1);
    rx_wr_bytes(blob, pwv3, n);
    return 0;
}

int rx_catalog_grid(RXCatalog *c, uint32_t track, RXWr *blob)
{
    Analysis *a = analysis_of(c, track);
    size_t n;
    const RXAnlzTag *tag = NULL;
    const uint8_t *beats = a ? tag_data(a, 0, "PQTZ", &n, &tag) : NULL;
    if (!beats)
        return RX_ERROR(ENOENT);
    RXRd hdr = rx_rd(tag->raw, tag->raw_size);
    uint32_t nb = RX_MIN(rx_rd_at_u32(&hdr, 0x14), (uint32_t)(n / 8));
    /* The PQTZ word 0x00080000, the beat count, the bytes of the entries,
     * 1, 1; then each beat: its number in the bar, 0, the tempo x100 and
     * the time in ms, little-endian, and eight ff bytes (rbxport's capture;
     * track_metadata.adoc "Beat Grids" for the entries). */
    rx_wr_u32le(blob, 0x00080000);
    rx_wr_u32le(blob, nb);
    rx_wr_u32le(blob, nb * 16);
    rx_wr_u32le(blob, 1);
    rx_wr_u32le(blob, 1);
    RXRd r = rx_rd(beats, n);
    for (uint32_t i = 0; i < nb; i++) {
        uint16_t number = rx_rd_u16(&r), tempo = rx_rd_u16(&r);
        uint32_t ms = rx_rd_u32(&r);
        rx_wr_u8(blob, (uint8_t)number);
        rx_wr_u8(blob, 0);
        rx_wr_u16le(blob, tempo);
        rx_wr_u32le(blob, ms);
        rx_wr_u32(blob, 0xffffffff);
        rx_wr_u32(blob, 0xffffffff);
    }
    return 0;
}

int rx_catalog_vbr(RXCatalog *c, uint32_t track, RXWr *blob)
{
    Analysis *a = analysis_of(c, track);
    size_t n;
    const uint8_t *pvbr = a ? tag_data(a, 0, "PVBR", &n, NULL) : NULL;
    if (!pvbr)
        return RX_ERROR(ENOENT);
    /* The PVBR tag's 400 index words and its trailer, the track's samples,
     * little-endian: rekordbox's reply was 1600 zero bytes and then a word
     * that is each track's MPEG frames x 1152 for those CBR MP3s
     * (rbxport's capture: 11106 x 1152 for a 290 s track, read as
     * little-endian), the PVBR trailer (docs/anlz.md 4.2). That a VBR
     * file's index goes the same way is assumed. */
    RXRd r = rx_rd(pvbr, n);
    for (size_t i = 0; i < n / 4; i++)
        rx_wr_u32le(blob, rx_rd_u32(&r));
    return 0;
}

int rx_catalog_tag(RXCatalog *c, uint32_t track, uint32_t fourcc, uint32_t ext, RXWr *blob)
{
    /* Both as the ASCII bytes in a little-endian number, an extension of
     * three padded with a NUL (track_metadata.adoc "Requesting New Tags":
     * PWV4 is 0x34565750, EXT 0x545845). */
    char name[4] = {
        (char)fourcc, (char)(fourcc >> 8), (char)(fourcc >> 16), (char)(fourcc >> 24),
    };
    char e[4] = {
        (char)(ext & 0xdf), (char)(ext >> 8 & 0xdf), (char)(ext >> 16 & 0xdf), (char)(ext >> 24),
    };
    int file = !memcmp(e, "DAT", 4) ? 0 : !memcmp(e, "EXT", 4) ? 1 :
               (ext & 0xff) == '2' && !memcmp(e + 1, "EX", 3) ? 2 : -1;
    Analysis *a = file >= 0 ? analysis_of(c, track) : NULL;
    if (!a)
        return RX_ERROR(ENOENT);
    for (int i = 0; i < a->nb_tags[file]; i++) {
        const RXAnlzTag *t = &a->tags[file][i];
        if (memcmp(t->fourcc, name, 4) || t->raw_size > UINT32_MAX - 3)
            continue;
        /* Its length padded to four, little-endian, then the tag as the
         * file holds it, header and all, zero-padded (rbxport's capture:
         * 381c0000 then "PWV4" ...). */
        uint32_t padded = ((uint32_t)t->raw_size + 3) & ~3u;
        rx_wr_u32le(blob, padded);
        rx_wr_bytes(blob, t->raw, t->raw_size);
        rx_wr_zeros(blob, padded - t->raw_size);
        return 0;
    }
    return RX_ERROR(ENOENT);
}

static int cmp_cue_time(const void *a, const void *b)
{
    const RXPlanCue *x = *(const RXPlanCue *const *)a, *y = *(const RXPlanCue *const *)b;
    if (x->hot != y->hot && (x->hot >= 0 || y->hot >= 0))
        return x->hot < 0 ? 1 : y->hot < 0 ? -1 : x->hot - y->hot;
    return x->time < y->time ? -1 : x->time > y->time;
}

/* Milliseconds as the ANLZ files have them (rx_anlz_ms, dj's ms()). */
static uint32_t ms_of(double t)
{
    double v = floor(t * 1000.0 + 0.5);
    return v <= 0 ? 0 : v >= 4294967295.0 ? 0xffffffffu : (uint32_t)v;
}

/* Hot cues by slot, then memory cues by time. NULL with *nb 0 for none. */
static const RXPlanCue **cues_sorted(const RXPlanTrack *pt, int *nb)
{
    *nb = 0;
    if (pt->nb_cues <= 0)
        return NULL;
    const RXPlanCue **v = malloc((size_t)pt->nb_cues * sizeof(*v));
    if (!v)
        return NULL;
    for (int i = 0; i < pt->nb_cues; i++)
        v[i] = &pt->cues[i];
    qsort(v, (size_t)pt->nb_cues, sizeof(*v), cmp_cue_time);
    *nb = pt->nb_cues;
    return v;
}

int rx_catalog_cues_ext(RXCatalog *c, uint32_t track, RXWr *blob, uint32_t *count)
{
    *count = 0;
    int ti = track_index(c, track);
    if (ti < 0)
        return RX_ERROR(ENOENT);
    const RXPlanTrack *pt = c->t[ti].pt;
    int nb;
    const RXPlanCue **v = cues_sorted(pt, &nb);
    if (pt->nb_cues > 0 && !v)
        return RX_ERROR(ENOMEM);
    int nb_mem = 0, mem = 0;
    for (int i = 0; i < nb; i++)
        nb_mem += v[i]->hot < 0;
    /* Each entry as rekordbox sent them (rbxport's capture, 0x90 bytes
     * with a nine-letter comment): its length, the hot cue's slot (1..8,
     * 0 for a memory cue), 1 a cue or 2 a loop, the time base 1000, in and
     * out in ms (ff.. for none), little-endian; at 0x20 a memory cue's
     * neighbours in the list (ffff none) and ffffffff for a hot cue; 0x56
     * at 0x34; the comment's UTF-16LE length with its NUL at 0x48 and the
     * comment; 2c 00 00 00; a hot cue's colour code and RGB; 0x2a zero
     * bytes. track_metadata.adoc "Requesting Nxs2 Cue Points" for the
     * fields it names. */
    for (int i = 0; i < nb; i++) {
        const RXPlanCue *q = v[i];
        bool hot = q->hot >= 0 && q->hot < 8;
        bool loop = q->loop_end > q->time;
        size_t name_units = q->name[0] ? rx_link_utf16_units(q->name) + 1 : 0;
        size_t len = 0x4a + 2 * name_units + 8 + 0x2a;
        size_t at = blob->len;
        rx_wr_u32le(blob, (uint32_t)len);
        rx_wr_u8(blob, hot ? (uint8_t)(q->hot + 1) : 0);
        rx_wr_u8(blob, 0);
        rx_wr_u8(blob, loop ? 2 : 1);
        rx_wr_zeros(blob, 3);
        rx_wr_u16le(blob, 1000);
        rx_wr_u32le(blob, ms_of(q->time));
        rx_wr_u32le(blob, loop ? ms_of(q->loop_end) : 0xffffffff);
        rx_wr_zeros(blob, 0x20 - 0x14);
        if (hot) {
            rx_wr_u32(blob, 0xffffffff);
        } else {
            rx_wr_u16le(blob, mem > 0 ? (uint16_t)(mem - 1) : 0xffff);
            rx_wr_u16le(blob, mem + 1 < nb_mem ? (uint16_t)(mem + 1) : 0xffff);
            mem++;
        }
        rx_wr_zeros(blob, 0x34 - 0x24);
        rx_wr_u8(blob, 0x56);
        rx_wr_zeros(blob, 0x3a - 0x35);
        /* A memory cue's colour id (1..8) here, where rekordbox reads it from
         * a player's saved cue (rbxport, from rekordbox 7.2.19's
         * SavUsbCueExt): assumed to be where it sends it too. */
        rx_wr_u8(blob, hot ? 0 : (uint8_t)RX_CLAMP(q->color_code, 0, 8));
        rx_wr_zeros(blob, 0x48 - 0x3b);
        rx_wr_u16le(blob, (uint16_t)(2 * name_units));
        if (name_units)
            rx_wr_utf16(blob, q->name, false, true);
        rx_wr_u32le(blob, 0x2c);
        /* The four bytes the stick's PCP2 entry carries (src/formats/anlz.c). */
        rx_wr_u8(blob, hot ? (uint8_t)q->color_code : 0);
        rx_wr_u8(blob, hot ? (uint8_t)(q->rgb >> 16) : 0);
        rx_wr_u8(blob, hot ? (uint8_t)(q->rgb >> 8) : 0);
        rx_wr_u8(blob, hot ? (uint8_t)q->rgb : 0);
        rx_wr_zeros(blob, len - (blob->len - at));
        (*count)++;
    }
    free(v);
    return 0;
}

int rx_catalog_cues(RXCatalog *c, uint32_t track, RXWr *records, RXWr *times,
                    uint32_t *hot, uint32_t *memory)
{
    *hot = *memory = 0;
    int ti = track_index(c, track);
    if (ti < 0)
        return RX_ERROR(ENOENT);
    const RXPlanTrack *pt = c->t[ti].pt;
    int nb;
    const RXPlanCue **v = cues_sorted(pt, &nb);
    if (pt->nb_cues > 0 && !v)
        return RX_ERROR(ENOMEM);
    /* The older list holds hot cues A to C and memory cues
     * (track_metadata.adoc "Requesting Cue Points and Loops"): 36 bytes
     * each, little-endian: the loop flag, the cue flag and the hot cue's
     * letter (1 to 3) in its first three bytes, in and out in 1/150 s at
     * 12 and 16, the rest zeros. Its millisecond times go in the second
     * blob, in and out, eight bytes a cue (rbxport). No out is ff.. here
     * as in the extended list (assumed). The 1/150 s are floored, not
     * rounded: a cue on a kick then starts at most 6.7 ms early, never
     * up to 3.3 ms into its attack.
     *
     * The memory cues come first, then the hot cues: a CDJ-2000 (4.33)
     * takes the first <memory count> records for memory cues and the next
     * <hot count> for hot cues, whatever their flags say; sent hot cues
     * first, it filed hot cue A as a memory cue and its A key did
     * nothing (an emulated one's memory, 2026-10-15). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nb; i++) {
            const RXPlanCue *q = v[i];
            if (q->hot >= 3 || (q->hot >= 0) != (pass == 1))
                continue;
            bool loop = q->loop_end > q->time;
            uint32_t in = ms_of(q->time), out = loop ? ms_of(q->loop_end) : 0xffffffff;
            uint32_t slot = q->hot >= 0 ? (uint32_t)q->hot + 1 : 0;
            const uint32_t words[9] = {
                0x100 | loop | slot << 16, 0, 0, (uint32_t)((uint64_t)in * 3 / 20),
                loop ? (uint32_t)((uint64_t)out * 3 / 20) : 0xffffffff, 0,
                0, 0, 0,
            };
            for (int k = 0; k < 9; k++)
                rx_wr_u32le(records, words[k]);
            rx_wr_u32le(times, in);
            rx_wr_u32le(times, out);
            if (slot)
                (*hot)++;
            else
                (*memory)++;
        }
    }
    free(v);
    return 0;
}

/* ------------------------------------------------------- cues saved */

/* The same place, loop, state, name and colour, to the ms. */
static bool cue_same(const RXCue *a, const RXCue *b)
{
    return a->hot == b->hot && fabs(a->time - b->time) < 0.0015 &&
           (a->loop_end >= 0) == (b->loop_end >= 0) &&
           (a->loop_end < 0 || fabs(a->loop_end - b->loop_end) < 0.0015) &&
           a->active == b->active && !strcmp(a->name, b->name) && !strcmp(a->color, b->color);
}

int rx_link_edit_apply(RXTrack *t, const RXLinkEdit *e)
{
    if (e->what == RX_LINK_EDIT_RATING) {
        if (e->rating < 0 || e->rating > 5)
            return RX_ERROR(EINVAL);
        if (t->meta.rating == e->rating)
            return 0;
        t->meta.rating = e->rating;
        return 1;
    }
    const RXCue *c = &e->cue;
    if (c->hot >= RX_HOT_CUE_SLOTS)
        return RX_ERROR(EINVAL);
    /* A memory cue within 20 ms is the one meant, as a stick's read back
     * finds it (rx_stick_apply). */
    int at = c->hot >= 0 ? rx_track_hot_cue(t, c->hot) : rx_track_memory_cue_at(t, c->time, 0.020);
    if (e->remove) {
        if (at < 0)
            return 0;
        int ret = rx_track_remove_cue(t, at);
        return ret < 0 ? ret : 1;
    }
    if (at >= 0 && cue_same(&t->cues[at], c))
        return 0;
    /* A hot cue takes its slot over by itself (rx_track_add_cue). */
    if (at >= 0 && c->hot < 0) {
        int ret = rx_track_remove_cue(t, at);
        if (ret < 0)
            return ret;
    }
    int ret = rx_track_add_cue(t, c);
    return ret < 0 ? ret : 1;
}

int rx_catalog_cue_edit(RXCatalog *c, uint32_t track, int player, bool remove,
                        const RXStickCue *cue)
{
    int ti = track_index(c, track);
    RXTrack *t = ti >= 0 ? rx_library_find_id(c->lib, track) : NULL;
    if (!t)
        return RX_ERROR(ENOENT);
    RXLinkEdit e = { .track_id = track, .player = player, .remove = remove };
    rx_stick_cue_to_track(cue, rx_track_analysed(t) ? &t->grid : NULL, &e.cue);
    /* A hot cue saved with no colour code comes with the pad colour the
     * deck gave it (00ff00 from a CDJ-3000, 1aff00 from a 3000X, whatever
     * the slot; emulated, 2026-10-15), which rekordbox keeps (rbxport). A
     * stick's 00ff00 is no colour (rx_stick_cue_to_track); here it is so
     * only as the slot's own default. */
    if (cue->hot >= 0 && !cue->code && cue->rgb && !e.cue.color[0] &&
        cue->rgb != rx_hot_cue_default_rgb(cue->hot))
        snprintf(e.cue.color, sizeof(e.cue.color), "#%06x", cue->rgb);
    int ret = rx_link_edit_apply(t, &e);
    if (ret <= 0)
        return ret;
    if ((ret = rx_plan_track_cues(c->t[ti].pt, t)) < 0)
        return ret;
    /* The ANLZ files made for it (their cue tags) are made again. */
    for (int i = 0; i < NB_ANALYSIS; i++)
        if (c->analysis[i].id == track)
            analysis_clear(&c->analysis[i]);
    char what[16], at[32];
    if (e.cue.hot >= 0)
        snprintf(what, sizeof(what), "hot cue %c", rx_cue_letter(&e.cue));
    else
        snprintf(what, sizeof(what), "a memory %s", rx_cue_is_loop(&e.cue) ? "loop" : "cue");
    rx_info("link: player %d %s %s at %s on track %u", player, remove ? "deleted" : "saved", what,
            rx_fmt_time(e.cue.time, at, sizeof(at)), track);
    if (c->edit)
        c->edit(c->edit_opaque, &e);
    return 0;
}

int rx_catalog_rating_edit(RXCatalog *c, uint32_t track, int player, int stars)
{
    int ti = track_index(c, track);
    RXTrack *t = ti >= 0 ? rx_library_find_id(c->lib, track) : NULL;
    if (!t)
        return RX_ERROR(ENOENT);
    RXLinkEdit e = {
        .what = RX_LINK_EDIT_RATING, .track_id = track, .player = player, .rating = stars,
    };
    int ret = rx_link_edit_apply(t, &e);
    if (ret <= 0)
        return ret;
    c->t[ti].rating = (uint8_t)stars;
    free(c->order[SORT_RATING]);
    c->order[SORT_RATING] = NULL;
    rx_info("link: player %d rated track %u %d star%s", player, track, stars, stars == 1 ? "" : "s");
    if (c->edit)
        c->edit(c->edit_opaque, &e);
    return 0;
}
