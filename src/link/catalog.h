/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What a session asks of the catalog (catalog.c): menus made from a
 * request and drawn by a render, the blobs, and a few values.
 *
 * A menu is plain data, ids and numbers: the session keeps it at the menu
 * location the request named until the player renders it, perhaps page
 * by page, and the library may change in between (the service replaces
 * the catalog). Rows are drawn from whatever catalog is current then; a
 * row whose id is gone is not drawn. */

#ifndef RXBOX_LINK_CATALOG_H
#define RXBOX_LINK_CATALOG_H

#include <stdbool.h>
#include <stdint.h>

#include "rxbox/export.h"
#include "rxbox/link.h"

#include "dbmsg.h"
#include "dbserver.h"

/* How a menu's rows are drawn. */
enum RXCatWhat {
    RX_CAT_EMPTY = 0,
    RX_CAT_ROOT,                /* ids: root category ids */
    RX_CAT_SORTS,               /* ids: sort ids */
    RX_CAT_TRACKS,              /* ids: tracks; nums: the position shown */
    RX_CAT_FILE_NAMES,          /* ids: tracks, named by their file */
    RX_CAT_NAMES,               /* ids: ids in `table` (RX_DB_ALL the ALL row) */
    RX_CAT_NUMBERS,             /* ids: values the player prints (BPM, years ...), as `type` */
    RX_CAT_COLORS,              /* ids: track colours 1..8 */
    RX_CAT_KEYS,                /* ids: keys 1..24 round the Camelot wheel */
    RX_CAT_RELATED,             /* ids: the key; nums: how far related */
    RX_CAT_PLAYLISTS,           /* ids: folders and playlists; nums: their position */
    RX_CAT_HISTORIES,           /* ids: days, yyyymmdd */
    RX_CAT_METADATA,            /* `track`'s metadata rows */
    RX_CAT_TRACK_INFO,
    RX_CAT_DELIVERY,
};

/* The name tables a RX_CAT_NAMES menu draws from. */
enum RXCatTable {
    RX_CAT_ARTIST = 0,
    RX_CAT_ALBUM,
    RX_CAT_GENRE,
    RX_CAT_LABEL,
};

typedef struct RXCatMenu {
    int       what;             /* enum RXCatWhat */
    int       table;            /* RX_CAT_NAMES: enum RXCatTable */
    uint32_t  type;             /* RX_CAT_NUMBERS: the rows' item type */
    int       sort;             /* track lists: the sort the request asked for */
    uint32_t  flags;            /* track rows: RX_DB_TRACK_* */
    uint32_t  track;            /* metadata, track info, delivery info */
    uint32_t *ids, *nums;       /* nums may be NULL */
    uint32_t  nb;
} RXCatMenu;

/* The menu a request asks for: 0, -ENOENT when the request is not a menu
 * this catalog makes, -ENOMEM. A track info request makes the track's
 * transcode first when the plan says it needs one and it is not made
 * (docs/LINK.md, section 4). */
int  rx_catalog_menu(RXCatalog *c, const RXDbMsg *req, RXCatMenu *out);
void rx_catalog_menu_free(RXCatMenu *m);
/* The row of `id` in a menu, for a player finding its place again: its
 * index, or RX_DB_ALL when it has none. */
uint32_t rx_catalog_menu_find(const RXCatMenu *m, uint32_t id);

/* Track rows show a column beside the title: a sort id names it (artist
 * 2, album 3, BPM 4 ... date added 0x11, track_metadata.adoc "Sort
 * Orders"), 0 the default. */
#define RX_CAT_COLUMN_DEFAULT 0
void rx_catalog_render(RXCatalog *c, const RXCatMenu *m, uint32_t offset, uint32_t count,
                       int column, bool extended, bool latin1, uint32_t txid, RXWr *w);

/* Artwork as JPEG: by its artwork id (`item` false; the older form, the
 * id the older item layout carries), or by a menu item's own id (a track,
 * or an album in an album row), as rekordbox resolves a request from a
 * CDJ-3000; `large` the 240 px one, else 80 px. -ENOENT when none. */
int  rx_catalog_artwork(RXCatalog *c, uint32_t id, bool item, bool large,
                        const uint8_t **jpeg, size_t *size);

/* The analysis replies' blobs for a track, appended to `blob`: -ENOENT
 * when the track or that part of its analysis does not exist. */
int  rx_catalog_preview(RXCatalog *c, uint32_t track, RXWr *blob);
int  rx_catalog_detail(RXCatalog *c, uint32_t track, RXWr *blob);
int  rx_catalog_grid(RXCatalog *c, uint32_t track, RXWr *blob);
int  rx_catalog_vbr(RXCatalog *c, uint32_t track, RXWr *blob);
/* An ANLZ tag by its fourcc and its file's extension, each as the
 * request carries them: the ASCII bytes in a little-endian number. */
int  rx_catalog_tag(RXCatalog *c, uint32_t track, uint32_t fourcc, uint32_t ext, RXWr *blob);
/* The extended cue list and its entry count; 0 with nothing appended for
 * a track without cues. */
int  rx_catalog_cues_ext(RXCatalog *c, uint32_t track, RXWr *blob, uint32_t *count);
/* The older cue list: 36-byte records, their times in ms, and how many
 * hot and memory cues. */
int  rx_catalog_cues(RXCatalog *c, uint32_t track, RXWr *records, RXWr *times,
                     uint32_t *hot, uint32_t *memory);

/* A cue `player` saved on the deck (or, `remove`, deleted) on `track`, as
 * it sent it: made a library cue on the track's grid, applied to the
 * library's track and to the plan's cues the catalog serves, then handed
 * on. 0, also when it changed nothing; -ENOENT for a track not served,
 * -ENOMEM. */
int  rx_catalog_cue_edit(RXCatalog *c, uint32_t track, int player, bool remove,
                         const RXStickCue *cue);

#endif /* RXBOX_LINK_CATALOG_H */
