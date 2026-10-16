/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What a session asks of the catalog (catalog.c): menus made from a
 * request and drawn by a render, the blobs, and a few values.
 *
 * A menu is plain data, ids and numbers: the session keeps it at the menu
 * location the request named until the player renders it, perhaps page
 * by page, and the library may change in between (the service replaces
 * the catalog). Rows are drawn from whatever catalog is current then; a
 * row whose id is gone is not drawn. */

#ifndef CYANBOX_LINK_CATALOG_H
#define CYANBOX_LINK_CATALOG_H

#include <stdbool.h>
#include <stdint.h>

#include "cyanbox/export.h"
#include "cyanbox/link.h"

#include "dbmsg.h"
#include "dbserver.h"

/* How a menu's rows are drawn. */
enum CBCatWhat {
    CB_CAT_EMPTY = 0,
    CB_CAT_ROOT,                /* ids: root category ids */
    CB_CAT_SORTS,               /* ids: sort ids */
    CB_CAT_TRACKS,              /* ids: tracks; nums: the position shown */
    CB_CAT_FILE_NAMES,          /* ids: tracks, named by their file */
    CB_CAT_NAMES,               /* ids: ids in `table` (CB_DB_ALL the ALL row) */
    CB_CAT_NUMBERS,             /* ids: values the player prints (BPM, years ...), as `type` */
    CB_CAT_COLORS,              /* ids: track colours 1..8 */
    CB_CAT_KEYS,                /* ids: keys 1..24 round the Camelot wheel */
    CB_CAT_RELATED,             /* ids: the key; nums: how far related */
    CB_CAT_PLAYLISTS,           /* ids: folders and playlists; nums: their position */
    CB_CAT_HISTORIES,           /* ids: days, yyyymmdd */
    CB_CAT_METADATA,            /* `track`'s metadata rows */
    CB_CAT_TRACK_INFO,
    CB_CAT_DELIVERY,
};

/* The name tables a CB_CAT_NAMES menu draws from. */
enum CBCatTable {
    CB_CAT_ARTIST = 0,
    CB_CAT_ALBUM,
    CB_CAT_GENRE,
    CB_CAT_LABEL,
};

typedef struct CBCatMenu {
    int       what;             /* enum CBCatWhat */
    int       table;            /* CB_CAT_NAMES: enum CBCatTable */
    uint32_t  type;             /* CB_CAT_NUMBERS: the rows' item type */
    int       sort;             /* track lists: the sort the request asked for */
    uint32_t  flags;            /* track rows: CB_DB_TRACK_* */
    uint32_t  track;            /* metadata, track info, delivery info */
    uint32_t *ids, *nums;       /* nums may be NULL */
    uint32_t  nb;
} CBCatMenu;

/* The menu a request asks for: 0, -ENOENT when the request is not a menu
 * this catalog makes, -ENOMEM. A track info request makes the track's
 * transcode first when the plan says it needs one and it is not made
 * (docs/LINK.md, section 4). */
int  cb_catalog_menu(CBCatalog *c, const CBDbMsg *req, CBCatMenu *out);
void cb_catalog_menu_free(CBCatMenu *m);
/* The row of `id` in a menu, for a player finding its place again: its
 * index, or CB_DB_ALL when it has none. */
uint32_t cb_catalog_menu_find(const CBCatMenu *m, uint32_t id);

/* Track rows show a column beside the title: a sort id names it (artist
 * 2, album 3, BPM 4 ... date added 0x11, track_metadata.adoc "Sort
 * Orders"), 0 the default. */
#define CB_CAT_COLUMN_DEFAULT 0
void cb_catalog_render(CBCatalog *c, const CBCatMenu *m, uint32_t offset, uint32_t count,
                       int column, bool extended, bool latin1, uint32_t txid, CBWr *w);

/* Artwork as JPEG: by its artwork id (`item` false; the older form, the
 * id the older item layout carries), or by a menu item's own id (a track,
 * or an album in an album row), as rekordbox resolves a request from a
 * CDJ-3000; `large` the 240 px one, else 80 px. -ENOENT when none. */
int  cb_catalog_artwork(CBCatalog *c, uint32_t id, bool item, bool large,
                        const uint8_t **jpeg, size_t *size);

/* The analysis replies' blobs for a track, appended to `blob`: -ENOENT
 * when the track or that part of its analysis does not exist. */
int  cb_catalog_preview(CBCatalog *c, uint32_t track, CBWr *blob);
int  cb_catalog_detail(CBCatalog *c, uint32_t track, CBWr *blob);
int  cb_catalog_grid(CBCatalog *c, uint32_t track, CBWr *blob);
int  cb_catalog_vbr(CBCatalog *c, uint32_t track, CBWr *blob);
/* An ANLZ tag by its fourcc and its file's extension, each as the
 * request carries them: the ASCII bytes in a little-endian number. */
int  cb_catalog_tag(CBCatalog *c, uint32_t track, uint32_t fourcc, uint32_t ext, CBWr *blob);
/* The extended cue list and its entry count; 0 with nothing appended for
 * a track without cues. */
int  cb_catalog_cues_ext(CBCatalog *c, uint32_t track, CBWr *blob, uint32_t *count);
/* The older cue list: 36-byte records, their times in ms, and how many
 * hot and memory cues. */
int  cb_catalog_cues(CBCatalog *c, uint32_t track, CBWr *records, CBWr *times,
                     uint32_t *hot, uint32_t *memory);

/* A cue `player` saved on the deck (or, `remove`, deleted) on `track`, as
 * it sent it: made a library cue on the track's grid, applied to the
 * library's track and to the plan's cues the catalog serves, then handed
 * on. 0, also when it changed nothing; -ENOENT for a track not served,
 * -ENOMEM. */
int  cb_catalog_cue_edit(CBCatalog *c, uint32_t track, int player, bool remove,
                         const CBStickCue *cue);
/* The rating `player` gave `track` on the deck, 0 to 5 stars: applied to
 * the library's track and what the catalog serves, then handed on. 0,
 * also when it changed nothing; -ENOENT for a track not served, -EINVAL
 * for a rating past 5. */
int  cb_catalog_rating_edit(CBCatalog *c, uint32_t track, int player, int stars);
/* The TAG LIST it serves (cb_catalog_set_tag_list), NULL none; whether
 * `track` is one it serves. */
CBLinkTagList *cb_catalog_tag_list(CBCatalog *c);
bool cb_catalog_serves(CBCatalog *c, uint32_t track);

#endif /* CYANBOX_LINK_CATALOG_H */
