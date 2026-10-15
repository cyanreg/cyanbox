/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The remote database server players browse (djl-analysis,
 * track_metadata.adoc and menus.adoc): the port query on TCP 12523, then
 * framed messages on the database port. Three layers, each testable alone:
 *
 *  - the message codec (dbmsg.c): fields, messages, menu items;
 *  - the catalog (catalog.c): the library as the menus see it, built from
 *    the library and the export plan over all of it; metadata, artwork and
 *    the analysis blobs, made on request;
 *  - a session (dbserver.c) per connection: requests in, replies out.
 *
 * No sockets here either: the service thread hands a connection's bytes
 * to its session and sends what comes back. The client side (dbclient.c)
 * is the fake player's and the tests'. */

#ifndef RXBOX_LINK_DBSERVER_H
#define RXBOX_LINK_DBSERVER_H

#include <stddef.h>
#include <stdint.h>

#include "rxbox/formats.h"
#include "rxbox/library.h"
#include "rxbox/link.h"

#include "dbmsg.h"
#include "link_priv.h"

/* --------------------------------------------------------------- catalog */

typedef struct RXCatalog RXCatalog;

/* What is served, from the library and a plan over all of it (the plan's
 * usb_path is the path the track-info reply names, which the NFS tree
 * serves; its ids are the library's). Both stay the caller's and must
 * outlive the catalog. The catalog may update a plan track's `source` and
 * `file_size` (a transcode made when a player asks for the track). */
int  rx_catalog_new(RXCatalog **out, RXLibrary *lib, RXPlan *plan);
void rx_catalog_free(RXCatalog **c);
/* What a track info request checks of a track served as a WAV made as it
 * is read (RXPlanTrack.pcm; src/link/wav.c): 0 when it can be, else the
 * catalog has the WAV made as a file first (RX_TRANSCODE_LINK_ANY) and
 * serves that. Without one, such a track is served as it is planned. */
void rx_catalog_set_check(RXCatalog *c, int (*check)(void *opaque, const RXPlanTrack *pt),
                          void *opaque);
/* Where the catalog hands the cues players save on the deck (the
 * service's queue for the library's owner, rxbox/link.h). */
void rx_catalog_set_edit(RXCatalog *c, void (*edit)(void *opaque, const RXLinkEdit *e),
                         void *opaque);
/* An edit a player made, applied again to a catalog over a library read
 * again (one read before the owner saved it): to the library's track and
 * what is served of it, handed on to no one. 1 when it changed something,
 * 0 when it was there already, -ENOENT for a track not served, -ENOMEM. */
int  rx_catalog_apply_edit(RXCatalog *c, const RXLinkEdit *e);

/* --------------------------------------------------------------- session */

typedef struct RXDbSession RXDbSession;

/* A connection's session. `cat` points at the service's current catalog,
 * which a library change replaces between two calls; `our_number` is the
 * device number we are on the link. */
int  rx_dbsession_new(RXDbSession **out, RXCatalog *const *cat, int our_number);
void rx_dbsession_free(RXDbSession **s);
/* Texts as Latin-1 (romaji and all), for a player that shows only Latin-1
 * (rx_dbmsg_item_latin1). */
void rx_dbsession_set_latin1(RXDbSession *s, bool latin1);
/* Bytes that arrived on the connection: whole requests are answered, the
 * replies appended to `out`, a partial one kept for the next call.
 * Returns 0, or a negative errno when the connection is to be closed (a
 * malformed or oversized message, a teardown). The connection starts with
 * the player's five-byte greeting, which the session answers in kind.
 *
 * On an error, what was appended to `out` before it (the answers to the
 * requests before a teardown in the same read) is still to be sent, then
 * the connection closed: -ESHUTDOWN for a teardown (no answer: the server
 * closes, beat-link's Client.close), -EPROTO for bytes that are not a
 * message, -E2BIG for one longer than the session takes, -ENOMEM. A
 * request may take long: a track info request makes the track's transcode
 * first when the plan names one not made yet (seconds for a long track). */
int  rx_dbsession_input(RXDbSession *s, const uint8_t *buf, size_t len, RXWr *out);

/* The port query on 12523: whether `buf` holds a whole query (its length,
 * which is consumed), 0 when more is needed, -EPROTO when it is not one;
 * a whole one is answered in `reply` with the database port. */
int  rx_dbquery_parse(const uint8_t *buf, size_t len);
void rx_dbquery_reply(uint16_t port, uint8_t reply[2]);

/* ---------------------------------------------------------------- client */

/* The fake player's side (dbclient.c): a small blocking client over TCP
 * with a timeout on every read and write, which asks what a CDJ-3000 asks
 * (rbxport's capture): the port, the greeting and setup as a device
 * number, menus rendered whole, metadata, track info, the data. Every
 * reply is parsed by the same codec as the server's requests, bounded. */

typedef struct RXDbClient RXDbClient;

/* Asks the port query service at host:query_port (12523 on rekordbox or
 * a player) for the database port. 0 or a negative errno. */
int  rx_dbclient_query_port(const char *host, uint16_t query_port, int timeout_ms, uint16_t *port);
/* Connects to the database server at host:port (IPv4 dotted), greets it
 * and sets up as device `number`, the way a CDJ-3000 does. */
int  rx_dbclient_connect(RXDbClient **out, const char *host, uint16_t port, int number,
                         int timeout_ms);
/* The same over a connected stream socket, which the client then owns (a
 * socketpair in the tests); `legacy` sets up the older way, as the
 * djl-analysis' clients do (twelve-argument items). */
int  rx_dbclient_open(RXDbClient **out, int fd, int number, bool legacy, int timeout_ms);
/* Sends the teardown, closes, frees; *c is NULL after. Safe on NULL. */
void rx_dbclient_close(RXDbClient **c);
/* The server's device number from its setup reply. */
int  rx_dbclient_server_number(const RXDbClient *c);

/* One request, its first argument the r:m:s:t word for `location` (slot 3
 * and track type 1, as a CDJ-3000 asks a Link Export source), then `args`;
 * the one reply in `reply`, its blobs and strings valid until the next
 * call. 0 or a negative errno (-ETIMEDOUT, -EPROTO, -ECONNRESET). */
int  rx_dbclient_request(RXDbClient *c, uint16_t kind, int location, const uint32_t *args, int nb,
                         RXDbMsg *reply);
/* A cue saved on the deck, or with `remove` deleted, as a player sends
 * it: the extended form a CDJ-3000X sends (2705, an entry of the 4e02
 * list), or with `older` the older one (2105, the 36-byte record and its
 * milliseconds). The reply in `reply`, as rx_dbclient_request's. */
int  rx_dbclient_save_cue(RXDbClient *c, uint32_t track, bool remove, const RXPlanCue *cue,
                          bool older, RXDbMsg *reply);
/* A menu request and its rows, rendered whole page by page: -ENOENT when
 * the server answers it with an error. Free with rx_dbrows_free. */
int  rx_dbclient_menu(RXDbClient *c, uint16_t kind, int location, const uint32_t *args, int nb,
                      RXDbRow **rows, int *nb_rows);
/* A search (RX_DB_SEARCH or RX_DB_SEARCH_LIVE) for `text`, in capitals as
 * a player sends it. */
int  rx_dbclient_search(RXDbClient *c, uint16_t kind, const char *text, RXDbRow **rows,
                        int *nb_rows);
/* A track's metadata rows (2002 at location 2). */
int  rx_dbclient_metadata(RXDbClient *c, uint32_t track, RXDbRow **rows, int *nb_rows);
/* What a player loads: the path to read over NFS and the file's size
 * (2102). -ENOENT when the server has no such track. */
int  rx_dbclient_track_info(RXDbClient *c, uint32_t track, char **path, uint64_t *size);
/* The data replies, allocated: artwork by the id an item carries (2003,
 * 240 px when `large`); for a track the preview (RX_DB_PREVIEW), detail
 * (RX_DB_DETAIL), grid (RX_DB_GRID), VBR (RX_DB_VBR) or extended cues
 * (RX_DB_CUES_EXT); an ANLZ tag ("PWV4", "EXT"). -ENOENT when the server
 * says it has none. */
int  rx_dbclient_artwork(RXDbClient *c, uint32_t id, bool large, uint8_t **jpeg, size_t *size);
int  rx_dbclient_blob(RXDbClient *c, uint16_t kind, uint32_t track, uint8_t **blob, size_t *size);
int  rx_dbclient_tag(RXDbClient *c, uint32_t track, const char *fourcc, const char *ext,
                     uint8_t **blob, size_t *size);

/* The replies' blobs decoded (pure, bounded): the beats of a grid reply;
 * the cues of an extended cue list (times in seconds, loop_end -1 for a
 * cue, hot -1 for a memory cue); an ANLZ tag reply's tag (pointing into
 * `blob`). */
int  rx_db_grid_parse(const uint8_t *blob, size_t size, RXBeat **beats, int *nb);
int  rx_db_cues_ext_parse(const uint8_t *blob, size_t size, RXPlanCue **cues, int *nb);
int  rx_db_tag_parse(const uint8_t *blob, size_t size, RXAnlzTag *tag);

#endif /* RXBOX_LINK_DBSERVER_H */
