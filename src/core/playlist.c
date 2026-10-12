/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Playlists and folders (rxbox/library.h): the `playlist` and
 * `playlist_entry` tables as a tree in memory.
 *
 * Positions. A folder holds its subfolders and its playlists in two
 * arrays, so each kind has its own position sequence among the children of
 * a folder, kept dense (0..n-1) on every edit; reparenting takes a
 * position in the sequence of its kind. Entries are rewritten whole on
 * every edit of a playlist (a few hundred rows at most), which keeps the
 * dense positions trivially right and needs no shifting of a primary key.
 *
 * As everywhere in the library, the database is written and committed
 * first and the tree changed after, so memory never claims an edit the
 * disk does not have.
 *
 * m3u8 import and export follow dj: paths relative to the playlist file
 * (absolute ones and file:// URLs are read too), a UTF-8 byte order mark
 * is skipped, '#' lines are comments, and a leading number in the file
 * name only orders ("03 Peak time.m3u8" imports as "Peak time"). */

#include "rxbox/library.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include "core.h"

/* ----------------------------------------------------------------- nodes */

static RxPlaylist *playlist_new(int64_t id, const char *name)
{
    RxPlaylist *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->id = id;
    p->name = rx_strdup(name);
    if (!p->name) {
        free(p);
        return NULL;
    }
    return p;
}

static void playlist_free(RxPlaylist *p)
{
    if (!p)
        return;
    free(p->name);
    free(p->track_ids);
    free(p);
}

static RxPlaylistFolder *folder_new(int64_t id, const char *name)
{
    RxPlaylistFolder *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->id = id;
    f->name = rx_strdup(name);
    if (!f->name) {
        free(f);
        return NULL;
    }
    return f;
}

static void folder_free(RxPlaylistFolder *f)
{
    if (!f)
        return;
    for (int i = 0; i < f->nb_folders; i++)
        folder_free(f->folders[i]);
    for (int i = 0; i < f->nb_playlists; i++)
        playlist_free(f->playlists[i]);
    free(f->folders);
    free(f->playlists);
    free(f->name);
    free(f);
}

/* Pointer arrays: grown by one, so a failure leaves them as they were. */
static int insert_ptr(void *array, int *nb, int at, void *item)
{
    void **a = *(void ***)array;
    a = realloc(a, (size_t)(*nb + 1) * sizeof(*a));
    if (!a)
        return RX_ERROR(ENOMEM);
    memmove(&a[at + 1], &a[at], (size_t)(*nb - at) * sizeof(*a));
    a[at] = item;
    (*nb)++;
    *(void ***)array = a;
    return 0;
}

static void remove_ptr(void *array, int *nb, int at)
{
    void **a = *(void ***)array;
    memmove(&a[at], &a[at + 1], (size_t)(*nb - at - 1) * sizeof(*a));
    (*nb)--;
}

static RxPlaylistFolder *parent_of_folder(RxPlaylistFolder *in, const RxPlaylistFolder *f,
                                          int *index)
{
    for (int i = 0; i < in->nb_folders; i++) {
        if (in->folders[i] == f) {
            *index = i;
            return in;
        }
        RxPlaylistFolder *r = parent_of_folder(in->folders[i], f, index);
        if (r)
            return r;
    }
    return NULL;
}

static RxPlaylistFolder *parent_of_playlist(RxPlaylistFolder *in, const RxPlaylist *p, int *index)
{
    for (int i = 0; i < in->nb_playlists; i++) {
        if (in->playlists[i] == p) {
            *index = i;
            return in;
        }
    }
    for (int i = 0; i < in->nb_folders; i++) {
        RxPlaylistFolder *r = parent_of_playlist(in->folders[i], p, index);
        if (r)
            return r;
    }
    return NULL;
}

/* The folder holding a playlist / a folder (and the index there), or NULL
 * when it is not in the library's tree. */
static RxPlaylistFolder *holder_of_playlist(const RxLibrary *lib, const RxPlaylist *p, int *index)
{
    return p && lib->root_folder ? parent_of_playlist(lib->root_folder, p, index) : NULL;
}

static RxPlaylistFolder *holder_of_folder(const RxLibrary *lib, const RxPlaylistFolder *f, int *index)
{
    return f && lib->root_folder ? parent_of_folder(lib->root_folder, f, index) : NULL;
}

static bool folder_contains(const RxPlaylistFolder *f, const RxPlaylistFolder *x)
{
    if (f == x)
        return true;
    for (int i = 0; i < f->nb_folders; i++) {
        if (folder_contains(f->folders[i], x))
            return true;
    }
    return false;
}

static bool in_tree(const RxLibrary *lib, const RxPlaylistFolder *f)
{
    return f && lib->root_folder && folder_contains(lib->root_folder, f);
}

/* ------------------------------------------------------------ db writes */

static int write_entries(RxLibrary *lib, int64_t playlist_id, const uint64_t *ids, int nb)
{
    RxDb *db = lib->db;
    sqlite3_stmt *st = rx_db_stmt(db, RX_STMT_ENTRY_CLEAR);
    rx_db_bind_int(db, st, 1, playlist_id);
    int ret = rx_db_done(db, st, "clearing a playlist");
    for (int i = 0; ret >= 0 && i < nb; i++) {
        st = rx_db_stmt(db, RX_STMT_ENTRY_INSERT);
        rx_db_bind_int(db, st, 1, playlist_id);
        rx_db_bind_int(db, st, 2, i);
        rx_db_bind_int(db, st, 3, (int64_t)ids[i]);
        ret = rx_db_done(db, st, "writing a playlist entry");
    }
    return ret;
}

static int place(RxLibrary *lib, int64_t id, int64_t parent, int position)
{
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_PLAYLIST_PLACE);
    rx_db_bind_int(lib->db, st, 1, id);
    if (parent)
        rx_db_bind_int(lib->db, st, 2, parent);
    else
        rx_db_bind_null(lib->db, st, 2);
    rx_db_bind_int(lib->db, st, 3, position);
    return rx_db_done(lib->db, st, "placing a playlist");
}

/* Positions of the children as given, skipping `skip`. */
static int place_folders(RxLibrary *lib, int64_t parent, RxPlaylistFolder *const *f, int nb,
                         const RxPlaylistFolder *skip)
{
    int ret = 0;
    for (int i = 0, pos = 0; ret >= 0 && i < nb; i++) {
        if (f[i] != skip)
            ret = place(lib, f[i]->id, parent, pos++);
    }
    return ret;
}

static int place_playlists(RxLibrary *lib, int64_t parent, RxPlaylist *const *p, int nb,
                           const RxPlaylist *skip)
{
    int ret = 0;
    for (int i = 0, pos = 0; ret >= 0 && i < nb; i++) {
        if (p[i] != skip)
            ret = place(lib, p[i]->id, parent, pos++);
    }
    return ret;
}

/* Runs `ret` = write in a transaction: commit, or roll back on error. */
static int finish(RxLibrary *lib, int ret)
{
    if (ret < 0) {
        rx_db_rollback(lib->db);
        return ret;
    }
    return rx_db_commit(lib->db);
}

/* ---------------------------------------------------------------- loading */

typedef struct Node {
    int64_t id, parent;
    int     position;
    bool    is_folder;
    bool    dirty;          /* entries not dense, or the node had to move */
    bool    attached;
    void   *obj;
} Node;

static int cmp_node(const void *a, const void *b)
{
    const Node *x = a, *y = b;
    return (x->id > y->id) - (x->id < y->id);
}

static Node *node_find(Node *nodes, int nb, int64_t id)
{
    Node key = { .id = id };
    return nb ? bsearch(&key, nodes, (size_t)nb, sizeof(*nodes), cmp_node) : NULL;
}

/* Sibling order: position, then id for ties a damaged table could have. */
static int cmp_sibling(const void *a, const void *b)
{
    const Node *x = *(Node *const *)a, *y = *(Node *const *)b;
    if (x->position != y->position)
        return (x->position > y->position) - (x->position < y->position);
    return (x->id > y->id) - (x->id < y->id);
}

static int load_entries(RxLibrary *lib, Node *nodes, int nb)
{
    sqlite3_stmt *st = rx_db_prepare(lib->db, "SELECT playlist_id, position, track_id "
                                              "FROM playlist_entry ORDER BY playlist_id, position");
    if (!st)
        return RX_ERROR(EIO);
    int ret;
    while ((ret = rx_db_row(lib->db, st, "loading playlist entries")) > 0) {
        Node *n = node_find(nodes, nb, sqlite3_column_int64(st, 0));
        if (!n || n->is_folder)
            continue;
        RxPlaylist *p = n->obj;
        uint64_t *ids = realloc(p->track_ids, (size_t)(p->nb + 1) * sizeof(*ids));
        if (!ids) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        p->track_ids = ids;
        if (sqlite3_column_int(st, 1) != p->nb)
            n->dirty = true;
        ids[p->nb++] = (uint64_t)sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    return ret;
}

/* True when every stored position is the index the tree has. */
static bool tree_clean(const RxPlaylistFolder *f, Node *nodes, int nb)
{
    for (int i = 0; i < f->nb_folders; i++) {
        Node *n = node_find(nodes, nb, f->folders[i]->id);
        if (!n || n->dirty || n->position != i || !tree_clean(f->folders[i], nodes, nb))
            return false;
    }
    for (int i = 0; i < f->nb_playlists; i++) {
        Node *n = node_find(nodes, nb, f->playlists[i]->id);
        if (!n || n->dirty || n->position != i)
            return false;
    }
    return true;
}

static int renumber_tree(RxLibrary *lib, const RxPlaylistFolder *f, Node *nodes, int nb)
{
    int ret = place_folders(lib, f->id, f->folders, f->nb_folders, NULL);
    if (ret >= 0)
        ret = place_playlists(lib, f->id, f->playlists, f->nb_playlists, NULL);
    for (int i = 0; ret >= 0 && i < f->nb_playlists; i++) {
        const RxPlaylist *p = f->playlists[i];
        Node *n = node_find(nodes, nb, p->id);
        if (n && n->dirty)
            ret = write_entries(lib, p->id, p->track_ids, p->nb);
    }
    for (int i = 0; ret >= 0 && i < f->nb_folders; i++)
        ret = renumber_tree(lib, f->folders[i], nodes, nb);
    return ret;
}

/* The parent a node can hang from: an existing folder that is not the
 * node itself and not under it (no loops). 0 is the root. */
static int64_t valid_parent(Node *nodes, int nb, const Node *n)
{
    Node *up = n->parent ? node_find(nodes, nb, n->parent) : NULL;
    if (!up || !up->is_folder)
        return 0;
    int steps = 0;
    for (Node *a = up; a; a = a->parent ? node_find(nodes, nb, a->parent) : NULL) {
        if (a == n || ++steps > nb)
            return 0;
    }
    return up->id;
}

int rx_core_playlists_load(RxLibrary *lib)
{
    lib->root_folder = folder_new(0, "");
    if (!lib->root_folder)
        return RX_ERROR(ENOMEM);

    sqlite3_stmt *st = rx_db_prepare(lib->db, "SELECT id, parent_id, position, is_folder, name "
                                              "FROM playlist ORDER BY id");
    if (!st)
        return RX_ERROR(EIO);

    Node *nodes = NULL;
    int nb = 0, ret;
    while ((ret = rx_db_row(lib->db, st, "loading playlists")) > 0) {
        Node *n = realloc(nodes, (size_t)(nb + 1) * sizeof(*n));
        if (!n) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        nodes = n;
        n = &nodes[nb];
        /* A NULL parent_id reads as 0, which is the root's id. */
        *n = (Node) {
            .id        = sqlite3_column_int64(st, 0),
            .parent    = sqlite3_column_int64(st, 1),
            .position  = sqlite3_column_int(st, 2),
            .is_folder = sqlite3_column_int(st, 3),
        };
        const char *name = (const char *)sqlite3_column_text(st, 4);
        n->obj = n->is_folder ? (void *)folder_new(n->id, name ? name : "") :
                                (void *)playlist_new(n->id, name ? name : "");
        if (!n->obj) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        nb++;
    }
    sqlite3_finalize(st);
    if (ret >= 0)
        ret = load_entries(lib, nodes, nb);

    Node **order = NULL;
    if (ret >= 0 && nb) {
        order = malloc((size_t)nb * sizeof(*order));
        if (!order)
            ret = RX_ERROR(ENOMEM);
    }

    /* Attach in sibling order. A parent that is gone, not a folder or part
     * of a loop (nothing here writes one) puts the node at the top, and
     * the tree is renumbered below. */
    if (ret >= 0 && nb) {
        for (int i = 0; i < nb; i++)
            order[i] = &nodes[i];
        qsort(order, (size_t)nb, sizeof(*order), cmp_sibling);
        for (int i = 0; ret >= 0 && i < nb; i++) {
            Node *n = order[i];
            int64_t parent_id = valid_parent(nodes, nb, n);
            if (parent_id != n->parent) {
                rx_warn("playlist %lld has no valid parent folder; moved to the top",
                        (long long)n->id);
                n->parent = 0;
                n->dirty = true;
            }
            RxPlaylistFolder *up = parent_id ? node_find(nodes, nb, parent_id)->obj : lib->root_folder;
            if (n->is_folder)
                ret = insert_ptr(&up->folders, &up->nb_folders, up->nb_folders, n->obj);
            else
                ret = insert_ptr(&up->playlists, &up->nb_playlists, up->nb_playlists, n->obj);
            n->attached = ret >= 0;
        }
    }

    if (ret >= 0 && !tree_clean(lib->root_folder, nodes, nb)) {
        rx_info("library: renumbering playlist positions");
        ret = rx_db_begin(lib->db);
        if (ret >= 0)
            ret = finish(lib, renumber_tree(lib, lib->root_folder, nodes, nb));
    }

    for (int i = 0; i < nb; i++) {
        if (nodes[i].attached)
            continue;
        if (nodes[i].is_folder)
            folder_free(nodes[i].obj);
        else
            playlist_free(nodes[i].obj);
    }
    free(nodes);
    free(order);
    return ret;
}

void rx_core_playlists_free(RxLibrary *lib)
{
    folder_free(lib->root_folder);
    lib->root_folder = NULL;
}

/* ---------------------------------------------------------------- lookups */

static void flat(RxPlaylistFolder *f, RxPlaylist **out, int max, int *n)
{
    for (int i = 0; i < f->nb_folders; i++)
        flat(f->folders[i], out, max, n);
    for (int i = 0; i < f->nb_playlists; i++) {
        if (out && *n < max)
            out[*n] = f->playlists[i];
        (*n)++;
    }
}

int rx_library_playlists_flat(const RxLibrary *lib, RxPlaylist **out, int max)
{
    int n = 0;
    if (lib->root_folder)
        flat(lib->root_folder, out, max, &n);
    return n;
}

RxPlaylistFolder *rx_library_folder_of(const RxLibrary *lib, const RxPlaylist *p)
{
    int index;
    return holder_of_playlist(lib, p, &index);
}

static RxPlaylist *find_name(RxPlaylistFolder *f, const char *name)
{
    for (int i = 0; i < f->nb_folders; i++) {
        RxPlaylist *p = find_name(f->folders[i], name);
        if (p)
            return p;
    }
    for (int i = 0; i < f->nb_playlists; i++) {
        if (!strcmp(f->playlists[i]->name, name))
            return f->playlists[i];
    }
    return NULL;
}

RxPlaylist *rx_library_playlist_find(const RxLibrary *lib, const char *name)
{
    return lib->root_folder && name ? find_name(lib->root_folder, name) : NULL;
}

/* ------------------------------------------------------------ tree edits */

static bool name_ok(const char *name)
{
    return name && name[strspn(name, " \t\r\n")];
}

static int64_t insert_row(RxLibrary *lib, int64_t parent, const char *name, bool folder, int position)
{
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_PLAYLIST_INSERT);
    if (parent)
        rx_db_bind_int(lib->db, st, 1, parent);
    else
        rx_db_bind_null(lib->db, st, 1);
    rx_db_bind_text(lib->db, st, 2, name);
    rx_db_bind_int(lib->db, st, 3, folder);
    rx_db_bind_int(lib->db, st, 4, position);
    int ret = rx_db_done(lib->db, st, name);
    return ret < 0 ? ret : sqlite3_last_insert_rowid(lib->db->db);
}

RxPlaylist *rx_library_playlist_create(RxLibrary *lib, RxPlaylistFolder *in, const char *name)
{
    if (!in)
        in = lib->root_folder;
    if (!name_ok(name) || !in_tree(lib, in))
        return NULL;
    RxPlaylist *p = playlist_new(0, name);
    size_t n = (size_t)in->nb_playlists + 1;
    RxPlaylist **grown = p ? realloc(in->playlists, n * sizeof(*grown)) : NULL;
    if (!grown) {
        playlist_free(p);
        return NULL;
    }
    in->playlists = grown;

    int ret = rx_db_begin(lib->db);
    if (ret >= 0) {
        int64_t id = insert_row(lib, in->id, name, false, in->nb_playlists);
        p->id = id;
        ret = finish(lib, id < 0 ? (int)id : 0);
    }
    if (ret < 0) {
        playlist_free(p);
        return NULL;
    }
    in->playlists[in->nb_playlists++] = p;
    lib->generation++;
    return p;
}

RxPlaylistFolder *rx_library_folder_create(RxLibrary *lib, RxPlaylistFolder *in, const char *name)
{
    if (!in)
        in = lib->root_folder;
    if (!name_ok(name) || !in_tree(lib, in))
        return NULL;
    RxPlaylistFolder *f = folder_new(0, name);
    size_t n = (size_t)in->nb_folders + 1;
    RxPlaylistFolder **grown = f ? realloc(in->folders, n * sizeof(*grown)) : NULL;
    if (!grown) {
        folder_free(f);
        return NULL;
    }
    in->folders = grown;

    int ret = rx_db_begin(lib->db);
    if (ret >= 0) {
        int64_t id = insert_row(lib, in->id, name, true, in->nb_folders);
        f->id = id;
        ret = finish(lib, id < 0 ? (int)id : 0);
    }
    if (ret < 0) {
        folder_free(f);
        return NULL;
    }
    in->folders[in->nb_folders++] = f;
    lib->generation++;
    return f;
}

static int rename_row(RxLibrary *lib, int64_t id, char **field, const char *name)
{
    if (!name_ok(name))
        return RX_ERROR(EINVAL);
    char *copy = rx_strdup(name);
    if (!copy)
        return RX_ERROR(ENOMEM);
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_PLAYLIST_RENAME);
    rx_db_bind_int(lib->db, st, 1, id);
    rx_db_bind_text(lib->db, st, 2, copy);
    int ret = rx_db_done(lib->db, st, "renaming a playlist");
    if (ret < 0) {
        free(copy);
        return ret;
    }
    free(*field);
    *field = copy;
    lib->generation++;
    return 0;
}

int rx_library_playlist_rename(RxLibrary *lib, RxPlaylist *p, const char *name)
{
    if (!rx_library_folder_of(lib, p))
        return RX_ERROR(ENOENT);
    return rename_row(lib, p->id, &p->name, name);
}

int rx_library_folder_rename(RxLibrary *lib, RxPlaylistFolder *f, const char *name)
{
    if (!in_tree(lib, f) || f == lib->root_folder)
        return RX_ERROR(EINVAL);
    return rename_row(lib, f->id, &f->name, name);
}

static int delete_row(RxLibrary *lib, int64_t id)
{
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_PLAYLIST_DELETE);
    rx_db_bind_int(lib->db, st, 1, id);
    return rx_db_done(lib->db, st, "deleting a playlist");
}

int rx_library_playlist_delete(RxLibrary *lib, RxPlaylist *p)
{
    int index;
    RxPlaylistFolder *in = holder_of_playlist(lib, p, &index);
    if (!in)
        return RX_ERROR(ENOENT);

    int ret = rx_db_begin(lib->db);
    if (ret < 0)
        return ret;
    ret = delete_row(lib, p->id);
    if (ret >= 0)
        ret = place_playlists(lib, in->id, in->playlists, in->nb_playlists, p);
    ret = finish(lib, ret);
    if (ret < 0)
        return ret;

    remove_ptr(&in->playlists, &in->nb_playlists, index);
    playlist_free(p);
    lib->generation++;
    return 0;
}

int rx_library_folder_delete(RxLibrary *lib, RxPlaylistFolder *f)
{
    int index;
    RxPlaylistFolder *in = holder_of_folder(lib, f, &index);
    if (!in)
        return RX_ERROR(f == lib->root_folder ? EINVAL : ENOENT);

    /* ON DELETE CASCADE takes the subfolders, playlists and entries. */
    int ret = rx_db_begin(lib->db);
    if (ret < 0)
        return ret;
    ret = delete_row(lib, f->id);
    if (ret >= 0)
        ret = place_folders(lib, in->id, in->folders, in->nb_folders, f);
    ret = finish(lib, ret);
    if (ret < 0)
        return ret;

    remove_ptr(&in->folders, &in->nb_folders, index);
    folder_free(f);
    lib->generation++;
    return 0;
}

int rx_library_playlist_reparent(RxLibrary *lib, RxPlaylist *p, RxPlaylistFolder *to, int position)
{
    int index;
    if (!to)
        to = lib->root_folder;
    RxPlaylistFolder *from = holder_of_playlist(lib, p, &index);
    if (!from || !in_tree(lib, to))
        return RX_ERROR(ENOENT);

    int nb_after = to->nb_playlists + (from != to);
    if (position < 0 || position >= nb_after)
        position = nb_after - 1;

    /* The destination list as it will be, built first so nothing is
     * touched if memory runs out. */
    RxPlaylist **list = malloc((size_t)nb_after * sizeof(*list));
    if (!list)
        return RX_ERROR(ENOMEM);
    for (int i = 0, n = 0; i < to->nb_playlists; i++) {
        if (to->playlists[i] != p)
            list[n++] = to->playlists[i];
    }
    memmove(&list[position + 1], &list[position], (size_t)(nb_after - 1 - position) * sizeof(*list));
    list[position] = p;

    int ret = rx_db_begin(lib->db);
    if (ret < 0) {
        free(list);
        return ret;
    }
    if (from != to)
        ret = place_playlists(lib, from->id, from->playlists, from->nb_playlists, p);
    if (ret >= 0)
        ret = place_playlists(lib, to->id, list, nb_after, NULL);
    ret = finish(lib, ret);
    if (ret < 0) {
        free(list);
        return ret;
    }

    if (from != to)
        remove_ptr(&from->playlists, &from->nb_playlists, index);
    free(to->playlists);
    to->playlists = list;
    to->nb_playlists = nb_after;
    lib->generation++;
    return 0;
}

int rx_library_folder_reparent(RxLibrary *lib, RxPlaylistFolder *f, RxPlaylistFolder *to, int position)
{
    int index;
    if (!to)
        to = lib->root_folder;
    RxPlaylistFolder *from = holder_of_folder(lib, f, &index);
    if (!from || !in_tree(lib, to))
        return RX_ERROR(ENOENT);
    /* Into itself or its own subtree would cut it off the tree. */
    if (folder_contains(f, to))
        return RX_ERROR(EINVAL);

    int nb_after = to->nb_folders + (from != to);
    if (position < 0 || position >= nb_after)
        position = nb_after - 1;

    RxPlaylistFolder **list = malloc((size_t)nb_after * sizeof(*list));
    if (!list)
        return RX_ERROR(ENOMEM);
    for (int i = 0, n = 0; i < to->nb_folders; i++) {
        if (to->folders[i] != f)
            list[n++] = to->folders[i];
    }
    memmove(&list[position + 1], &list[position], (size_t)(nb_after - 1 - position) * sizeof(*list));
    list[position] = f;

    int ret = rx_db_begin(lib->db);
    if (ret < 0) {
        free(list);
        return ret;
    }
    if (from != to)
        ret = place_folders(lib, from->id, from->folders, from->nb_folders, f);
    if (ret >= 0)
        ret = place_folders(lib, to->id, list, nb_after, NULL);
    ret = finish(lib, ret);
    if (ret < 0) {
        free(list);
        return ret;
    }

    if (from != to)
        remove_ptr(&from->folders, &from->nb_folders, index);
    free(to->folders);
    to->folders = list;
    to->nb_folders = nb_after;
    lib->generation++;
    return 0;
}

/* --------------------------------------------------------------- entries */

/* Writes `ids` as the playlist's entries and adopts the array. */
static int set_entries(RxLibrary *lib, RxPlaylist *p, uint64_t *ids, int nb)
{
    int ret = rx_db_begin(lib->db);
    if (ret >= 0)
        ret = finish(lib, write_entries(lib, p->id, ids, nb));
    if (ret < 0) {
        free(ids);
        return ret;
    }
    free(p->track_ids);
    p->track_ids = ids;
    p->nb = nb;
    lib->generation++;
    return 0;
}

static uint64_t *copy_ids(const RxPlaylist *p, int extra)
{
    uint64_t *ids = malloc((size_t)RX_MAX(p->nb + extra, 1) * sizeof(*ids));
    if (ids && p->nb)
        memcpy(ids, p->track_ids, (size_t)p->nb * sizeof(*ids));
    return ids;
}

int rx_library_playlist_add(RxLibrary *lib, RxPlaylist *p, const RxTrack *t, int at)
{
    if (!rx_library_folder_of(lib, p) || !t || rx_library_find_id(lib, t->id) != t)
        return RX_ERROR(ENOENT);
    for (int i = 0; i < p->nb; i++) {
        if (p->track_ids[i] == t->id)
            return RX_ERROR(EEXIST);
    }
    if (at < 0 || at > p->nb)
        at = p->nb;
    uint64_t *ids = copy_ids(p, 1);
    if (!ids)
        return RX_ERROR(ENOMEM);
    memmove(&ids[at + 1], &ids[at], (size_t)(p->nb - at) * sizeof(*ids));
    ids[at] = t->id;
    return set_entries(lib, p, ids, p->nb + 1);
}

int rx_library_playlist_remove_at(RxLibrary *lib, RxPlaylist *p, int index)
{
    if (!rx_library_folder_of(lib, p))
        return RX_ERROR(ENOENT);
    if (index < 0 || index >= p->nb)
        return RX_ERROR(EINVAL);
    uint64_t *ids = copy_ids(p, 0);
    if (!ids)
        return RX_ERROR(ENOMEM);
    memmove(&ids[index], &ids[index + 1], (size_t)(p->nb - index - 1) * sizeof(*ids));
    return set_entries(lib, p, ids, p->nb - 1);
}

int rx_library_playlist_move(RxLibrary *lib, RxPlaylist *p, int from, int to)
{
    if (!rx_library_folder_of(lib, p))
        return RX_ERROR(ENOENT);
    if (from < 0 || from >= p->nb)
        return RX_ERROR(EINVAL);
    if (to < 0 || to >= p->nb)
        to = p->nb - 1;
    if (from == to)
        return 0;
    uint64_t *ids = copy_ids(p, 0);
    if (!ids)
        return RX_ERROR(ENOMEM);
    uint64_t id = ids[from];
    if (from < to)
        memmove(&ids[from], &ids[from + 1], (size_t)(to - from) * sizeof(*ids));
    else
        memmove(&ids[to + 1], &ids[to], (size_t)(from - to) * sizeof(*ids));
    ids[to] = id;
    return set_entries(lib, p, ids, p->nb);
}

/* `write`: the playlists holding `id` are written without it (memory left
 * alone, the caller may still roll back); otherwise memory drops it. */
static int drop_in(RxLibrary *lib, RxPlaylistFolder *f, uint64_t id, bool write)
{
    int ret = 0;
    for (int i = 0; ret >= 0 && i < f->nb_folders; i++)
        ret = drop_in(lib, f->folders[i], id, write);
    for (int i = 0; ret >= 0 && i < f->nb_playlists; i++) {
        RxPlaylist *p = f->playlists[i];
        int k = 0;
        while (k < p->nb && p->track_ids[k] != id)
            k++;
        if (k == p->nb)
            continue;
        if (!write) {
            memmove(&p->track_ids[k], &p->track_ids[k + 1],
                    (size_t)(p->nb - k - 1) * sizeof(*p->track_ids));
            p->nb--;
            continue;
        }
        uint64_t *ids = copy_ids(p, 0);
        if (!ids)
            return RX_ERROR(ENOMEM);
        memmove(&ids[k], &ids[k + 1], (size_t)(p->nb - k - 1) * sizeof(*ids));
        ret = write_entries(lib, p->id, ids, p->nb - 1);
        free(ids);
    }
    return ret;
}

int rx_core_playlists_drop_track(RxLibrary *lib, uint64_t id)
{
    return lib->root_folder ? drop_in(lib, lib->root_folder, id, true) : 0;
}

void rx_core_playlists_forget_track(RxLibrary *lib, uint64_t id)
{
    if (lib->root_folder)
        drop_in(lib, lib->root_folder, id, false);
}

/* ------------------------------------------------------------------- m3u */

/* Collapses "//", "/./" and "/../" in an absolute path, in place. */
static void normalise_path(char *path)
{
    char *out = path;
    const char *in = path;
    while (*in) {
        while (*in == '/')
            in++;
        const char *end = strchrnul(in, '/');
        size_t n = (size_t)(end - in);
        if (n == 1 && in[0] == '.') {
            /* nothing */
        } else if (n == 2 && in[0] == '.' && in[1] == '.') {
            while (out > path && *--out != '/')
                ;
        } else if (n) {
            *out++ = '/';
            memmove(out, in, n);
            out += n;
        }
        in = end;
    }
    if (out == path)
        *out++ = '/';
    *out = '\0';
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* A line of an m3u as an absolute path, the way dj's read_m3u resolved it. */
static char *resolve_entry(const char *dir, const char *line)
{
    char *p;
    if (rx_str_starts(line, "file://")) {
        /* file:///abs or file://host/abs: the path is from the first '/'. */
        const char *s = strchr(line + 7, '/');
        if (!s)
            return NULL;
        p = malloc(strlen(s) + 1);
        if (!p)
            return NULL;
        char *o = p;
        while (*s) {
            int hi, lo;
            if (s[0] == '%' && (hi = hex_digit(s[1])) >= 0 && (lo = hex_digit(s[2])) >= 0) {
                *o++ = (char)(hi << 4 | lo);
                s += 3;
            } else {
                *o++ = *s++;
            }
        }
        *o = '\0';
    } else {
        p = rx_path_expand(line);
        if (!p)
            return NULL;
    }

    if (p[0] != '/') {
        char *joined = rx_path_join(dir, p);
        free(p);
        p = joined;
        if (!p)
            return NULL;
    }
    normalise_path(p);
    char *real = realpath(p, NULL);
    if (real) {
        free(p);
        return real;
    }
    return p;
}

/* dj's _display_name: the file name without its extension and without a
 * leading order number ("03 Peak time" -> "Peak time"). */
static char *display_name(const char *path)
{
    const char *base = rx_path_base(path);
    const char *dot = strrchr(base, '.');
    size_t len = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    char *stem = rx_strndup(base, len);
    if (!stem)
        return NULL;
    size_t digits = strspn(stem, "0123456789");
    size_t seps = digits ? strspn(stem + digits, " _.-") : 0;
    if (digits && seps && stem[digits + seps]) {
        char *name = rx_strdup(stem + digits + seps);
        free(stem);
        return name;
    }
    return stem;
}

int rx_library_playlist_import_m3u(RxLibrary *lib, RxPlaylistFolder *in, const char *m3u_path,
                                   RxPlaylist **out)
{
    if (out)
        *out = NULL;
    char *abs = rx_path_abs(m3u_path);
    if (!abs)
        return RX_ERROR(ENOENT);
    uint8_t *data;
    size_t size;
    int ret = rx_file_read(abs, &data, &size);
    if (ret < 0) {
        rx_err("cannot read %s: %s", abs, strerror(-ret));
        free(abs);
        return ret;
    }

    char *dir = rx_strdup(abs);
    uint64_t *ids = malloc(sizeof(*ids));
    if (!dir || !ids) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    *strrchr(dir, '/') = '\0';
    if (!dir[0])
        strcpy(dir, "/");

    int nb = 0, skipped = 0;
    char *text = (char *)data;
    if (rx_str_starts(text, "\xef\xbb\xbf"))
        text += 3;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (isspace((unsigned char)*line))
            line++;
        size_t n = strlen(line);
        while (n && isspace((unsigned char)line[n - 1]))
            line[--n] = '\0';
        if (!n || line[0] == '#')
            continue;

        char *path = resolve_entry(dir, line);
        if (!path) {
            ret = RX_ERROR(ENOMEM);
            goto end;
        }
        RxTrack *t = rx_library_find_path(lib, path);
        int err = 0;
        if (!t && rx_file_exists(path))
            t = rx_library_add_file(lib, path, &err);
        if (!t) {
            rx_warn("%s: %s is not in the library and cannot be added (%s); skipped", abs, path,
                    strerror(err < 0 ? -err : ENOENT));
            skipped++;
            free(path);
            continue;
        }
        free(path);

        bool dup = false;
        for (int i = 0; i < nb && !dup; i++)
            dup = ids[i] == t->id;
        if (dup)
            continue;
        uint64_t *grown = realloc(ids, (size_t)(nb + 1) * sizeof(*ids));
        if (!grown) {
            ret = RX_ERROR(ENOMEM);
            goto end;
        }
        ids = grown;
        ids[nb++] = t->id;
    }

    char *name = display_name(abs);
    RxPlaylist *p = name ? rx_library_playlist_create(lib, in, name) : NULL;
    free(name);
    if (!p) {
        ret = RX_ERROR(EIO);
        goto end;
    }
    ret = set_entries(lib, p, ids, nb);
    ids = NULL;
    if (ret < 0) {
        rx_library_playlist_delete(lib, p);
        goto end;
    }
    if (skipped)
        rx_warn("%s: %d entr%s skipped", abs, skipped, skipped == 1 ? "y" : "ies");
    if (out)
        *out = p;
    ret = nb;

end:
    free(ids);
    free(dir);
    free(data);
    free(abs);
    return ret;
}

/* `to` relative to the directory `from`, both absolute and normalised. */
static char *relative_path(const char *from, const char *to)
{
    size_t i = 0, common = 0;
    while (from[i] && from[i] == to[i]) {
        if (from[i] == '/')
            common = i + 1;
        i++;
    }
    if (!from[i] && (to[i] == '/' || !to[i]))
        common = to[i] ? i + 1 : i;

    int ups = 0;
    for (const char *s = from + common; *s; ) {
        ups++;
        s = strchrnul(s, '/');
        while (*s == '/')
            s++;
    }

    size_t rest = strlen(to + common);
    char *out = malloc((size_t)ups * 3 + rest + 1);
    if (!out)
        return NULL;
    char *o = out;
    for (int k = 0; k < ups; k++, o += 3)
        memcpy(o, "../", 3);
    memcpy(o, to + common, rest + 1);
    return out;
}

int rx_library_playlist_export_m3u(const RxLibrary *lib, const RxPlaylist *p, const char *m3u_path)
{
    char *abs = rx_path_abs(m3u_path);
    if (!abs)
        return RX_ERROR(ENOENT);
    char *dir = rx_strdup(abs);
    if (!dir) {
        free(abs);
        return RX_ERROR(ENOMEM);
    }
    *strrchr(dir, '/') = '\0';
    if (!dir[0])
        strcpy(dir, "/");

    char *text = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&text, &size);
    int ret = f ? 0 : RX_ERROR(ENOMEM);
    if (f) {
        fputs("#EXTM3U\n", f);
        for (int i = 0; ret >= 0 && i < p->nb; i++) {
            const RxTrack *t = rx_library_find_id(lib, p->track_ids[i]);
            if (!t)
                continue;
            char display[512];
            char *rel = relative_path(dir, t->path);
            if (!rel) {
                ret = RX_ERROR(ENOMEM);
                break;
            }
            /* #EXTINF is for other players; dj and rxbox skip '#' lines. */
            fprintf(f, "#EXTINF:%lld,%s\n%s\n", (long long)llround(t->duration),
                    rx_track_display(t, display, sizeof(display)), rel);
            free(rel);
        }
        if (fclose(f) && ret >= 0)
            ret = RX_ERROR(ENOMEM);
    }
    if (ret >= 0)
        ret = rx_file_write_atomic(abs, text, size);
    if (ret < 0)
        rx_err("cannot write %s: %s", abs, strerror(-ret));
    free(text);
    free(dir);
    free(abs);
    return ret;
}
