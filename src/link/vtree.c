/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The virtual tree NFS serves (docs/LINK.md, "A virtual tree"): the
 * directories and files of the export plan's usb_paths and nothing else.
 * No host path is in it; a file node holds the index of its plan track,
 * whose `source` and `file_size` are read when a player asks, so a
 * transcode made after the tree is seen.
 *
 * Decisions:
 *
 * - Names are matched exactly first, then ignoring ASCII case. A player
 *   walks the path the database server's track-info reply gave it, name
 *   by name, as it was given (rbxport's fake CDJ and Deep Symmetry's
 *   FileFetcher, both written against real players), so an exact match is
 *   what it needs. rekordbox serves from a case-insensitive host file
 *   system (APFS, NTFS) and the sticks players read are FAT32 or exFAT,
 *   so a player that changed a name's case would still be served there;
 *   the ASCII fallback covers that (assumed: no player has been seen to
 *   do it). The plan makes paths unique under case folding
 *   (src/export/plan.c, map_taken), so the fallback finds one file. No
 *   Unicode normalisation is done: the plan's names are NFC and players
 *   send back what they were given (rbxport saw decomposed names only in
 *   listings rekordbox itself decomposed on macOS).
 * - A directory's id is a 63-bit hash of its path, so a directory handle
 *   still names it in a tree built again from a new plan; files are named
 *   by track id. Two directories hashing alike (about 1e-11 for 10 000
 *   directories) are told apart by moving the later one's id on.
 * - A component that is empty, "." or "..", not UTF-8, or longer than 255
 *   UTF-16 units (FAT's long-name limit) leaves its track out of the
 *   tree, as does a second track at the same path or with the same id.
 *
 * Built with one hash table pass over the components and one sort of the
 * nodes: a 10 000-track plan takes a few milliseconds. */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rxbox/common.h"

#include "nfs.h"

#define ID_MASK 0x7FFFFFFFFFFFFFFFull

typedef struct VNode {
    uint32_t parent;
    uint32_t name;              /* offset of the NUL-terminated name in the pool */
    uint32_t first, nb;         /* directories: their children, kids[first .. first + nb) */
    int32_t  track;             /* files: the plan track's index; -1 for a directory */
    uint64_t id;                /* directories: the stable id */
} VNode;

typedef struct IdIndex {
    uint64_t id;
    uint32_t node;
} IdIndex;

struct RXVTree {
    const RXPlan *plan;
    VNode    *nodes;
    uint32_t  nb_nodes;
    uint32_t *kids;             /* every node but the root, by parent then name */
    char     *names;
    IdIndex  *dirs;             /* by id */
    uint32_t  nb_dirs;
    IdIndex  *files;            /* by track id */
    uint32_t  nb_files;
    uint64_t  total_size;
    uint32_t  time;
};

/* ------------------------------------------------------------------ names */

static int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Orders as the children are sorted: ignoring ASCII case, then exactly. */
static int cmp_fold(const char *a, const char *b, size_t blen)
{
    for (size_t i = 0; i < blen; i++) {
        int ca = ascii_lower((unsigned char)a[i]), cb = ascii_lower((unsigned char)b[i]);
        if (!a[i] || ca != cb)
            return !a[i] ? -1 : ca - cb;
    }
    return a[blen] ? 1 : 0;
}

static int cmp_exact(const char *a, const char *b, size_t blen)
{
    for (size_t i = 0; i < blen; i++) {
        if (!a[i] || a[i] != b[i])
            return !a[i] ? -1 : (unsigned char)a[i] - (unsigned char)b[i];
    }
    return a[blen] ? 1 : 0;
}

/* The UTF-16 units of a component, or -1 when it is not strict UTF-8
 * (overlong forms, surrogates and code points past U+10FFFF refused). */
static int utf16_units(const uint8_t *s, size_t len)
{
    int units = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 :
                (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (size_t)n >= len - i)
            return -1;
        c &= 0x7Fu >> n;
        for (int k = 1; k <= n; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return -1;
            c = c << 6 | (s[i + k] & 0x3F);
        }
        static const uint32_t min[4] = { 0, 0x80, 0x800, 0x10000 };
        if (c < min[n] || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000))
            return -1;
        units += c >= 0x10000 ? 2 : 1;
        i += (size_t)n + 1;
    }
    return units;
}

static bool valid_component(const char *s, size_t len)
{
    if (!len || (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
        return false;
    int units = utf16_units((const uint8_t *)s, len);
    return units > 0 && units <= RX_NFS_NAME_MAX;
}

/* -------------------------------------------------------------------- ids */

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    return x ^ x >> 31;
}

/* FNV-1a over the parent's id and the name, finished with splitmix64's
 * mixer: the id of the directory `name` in the directory `parent`. */
static uint64_t dir_id(uint64_t parent, const char *name, size_t len)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 8; i++)
        h = (h ^ (uint8_t)(parent >> 8 * i)) * 0x100000001B3ull;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)name[i]) * 0x100000001B3ull;
    return mix64(h) & ID_MASK;
}

static int cmp_index(const void *a, const void *b)
{
    const IdIndex *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id ? 1 : x->node < y->node ? -1 : x->node > y->node;
}

static int cmp_node(const void *a, const void *b)
{
    const IdIndex *x = a, *y = b;
    return x->node < y->node ? -1 : x->node > y->node;
}

/* ------------------------------------------------------------------ build */

typedef struct Build {
    RXVTree  *t;
    uint32_t *map;              /* node + 1 by (parent, name); 0 empty */
    size_t    map_mask;
    size_t    pool;
} Build;

static size_t slot_of(uint32_t parent, const char *name, size_t len, size_t mask)
{
    return (size_t)mix64(dir_id(parent, name, len)) & mask;
}

/* The node called `name` in `parent`, or the slot where it would go. */
static uint32_t *find(Build *b, uint32_t parent, const char *name, size_t len)
{
    size_t i = slot_of(parent, name, len, b->map_mask);
    while (1) {
        uint32_t *s = &b->map[i];
        if (!*s)
            return s;
        const VNode *v = &b->t->nodes[*s - 1];
        if (v->parent == parent && !cmp_exact(b->t->names + v->name, name, len))
            return s;
        i = (i + 1) & b->map_mask;
    }
}

static uint32_t add_node(Build *b, uint32_t *slot, uint32_t parent, const char *name, size_t len,
                         int32_t track)
{
    RXVTree *t = b->t;
    uint32_t k = t->nb_nodes++;
    memcpy(t->names + b->pool, name, len);
    t->names[b->pool + len] = '\0';
    t->nodes[k] = (VNode) {
        .parent = parent,
        .name   = (uint32_t)b->pool,
        .track  = track,
        .id     = track < 0 ? dir_id(t->nodes[parent].id, name, len) : 0,
    };
    b->pool += len + 1;
    *slot = k + 1;
    return k;
}

/* Puts one track at its usb_path: false (nothing added) when the path is
 * not one the tree takes. */
static bool add_track(Build *b, int index, const char *path)
{
    if (!path || path[0] != '/')
        return false;
    /* Every component is checked before anything is added, so a refused
     * track leaves no directory behind. */
    for (const char *p = path + 1;;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (!valid_component(p, len))
            return false;
        if (!e)
            break;
        p = e + 1;
    }
    uint32_t at = 0;
    for (const char *p = path + 1;;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        uint32_t *slot = find(b, at, p, len);
        if (!e) {
            if (*slot)
                return false;               /* a node of that name is there */
            add_node(b, slot, at, p, len, index);
            return true;
        }
        if (*slot && b->t->nodes[*slot - 1].track >= 0)
            return false;                   /* a file where a directory goes */
        at = *slot ? *slot - 1 : add_node(b, slot, at, p, len, -1);
        p = e + 1;
    }
}

static int cmp_kids(const void *a, const void *b, void *opaque)
{
    const RXVTree *t = opaque;
    const VNode *x = &t->nodes[*(const uint32_t *)a], *y = &t->nodes[*(const uint32_t *)b];
    if (x->parent != y->parent)
        return x->parent < y->parent ? -1 : 1;
    const char *nx = t->names + x->name, *ny = t->names + y->name;
    int c = cmp_fold(nx, ny, strlen(ny));
    return c ? c : strcmp(nx, ny);
}

/* Makes the directory ids unique: a later directory sharing an id has
 * its id moved on until none does. */
static void unique_ids(RXVTree *t)
{
    bool again = true;
    while (again) {
        again = false;
        qsort(t->dirs, t->nb_dirs, sizeof(*t->dirs), cmp_index);
        for (uint32_t i = 1; i < t->nb_dirs; i++) {
            if (t->dirs[i].id != t->dirs[i - 1].id)
                continue;
            uint64_t id = mix64(t->dirs[i].id + 1) & ID_MASK;
            t->dirs[i].id = id;
            t->nodes[t->dirs[i].node].id = id;
            again = true;
        }
    }
}

int rx_vtree_new(RXVTree **out, const RXPlan *plan)
{
    if (!out || !plan || plan->nb_tracks < 0 || (plan->nb_tracks && !plan->tracks))
        return RX_ERROR(EINVAL);
    *out = NULL;

    /* A component per '/' at most, and the names fit in the paths' bytes. */
    size_t max_nodes = 1, pool = 1;
    for (int i = 0; i < plan->nb_tracks; i++) {
        const char *p = plan->tracks[i].usb_path;
        if (!p)
            continue;
        for (const char *s = p; *s; s++)
            max_nodes += *s == '/';
        pool += strlen(p) + 1;
    }
    if (max_nodes > UINT32_MAX / 4 || pool > UINT32_MAX)
        return RX_ERROR(E2BIG);

    int ret = 0;
    IdIndex *order = NULL;
    Build b = { .pool = 1 };
    size_t map_size = 16;
    while (map_size < 2 * max_nodes)
        map_size *= 2;
    b.map_mask = map_size - 1;

    RXVTree *t = calloc(1, sizeof(*t));
    b.t = t;
    b.map = calloc(map_size, sizeof(*b.map));
    if (!t || !b.map)
        goto nomem;
    t->plan  = plan;
    t->nodes = calloc(max_nodes, sizeof(*t->nodes));
    t->names = malloc(pool);
    t->files = calloc((size_t)RX_MAX(plan->nb_tracks, 1), sizeof(*t->files));
    if (!t->nodes || !t->names || !t->files)
        goto nomem;

    /* The root: no name, directories' times are the tree's. */
    t->names[0] = '\0';
    t->nodes[0] = (VNode) { .track = -1, .id = dir_id(0, "", 0) };
    t->nb_nodes = 1;
    t->time = (uint32_t)time(NULL);

    /* A file handle names its track by id, so of two tracks with one id
     * the later is left out, before anything of it is added (the plan's
     * ids are unique: this is a guard). */
    order = malloc((size_t)RX_MAX(plan->nb_tracks, 1) * sizeof(*order));
    if (!order)
        goto nomem;
    for (int i = 0; i < plan->nb_tracks; i++)
        order[i] = (IdIndex) { .id = plan->tracks[i].id, .node = (uint32_t)i };
    qsort(order, (size_t)plan->nb_tracks, sizeof(*order), cmp_index);
    uint64_t prev = UINT64_MAX;             /* no track id: they are 32-bit */
    for (int i = 0; i < plan->nb_tracks; i++) {
        uint64_t id = order[i].id;
        if (id == prev)
            order[i].id = UINT64_MAX;       /* marks the later ones */
        prev = id;
    }
    qsort(order, (size_t)plan->nb_tracks, sizeof(*order), cmp_node);

    for (int i = 0; i < plan->nb_tracks; i++) {
        const RXPlanTrack *pt = &plan->tracks[i];
        bool dup = order[i].id == UINT64_MAX;
        if (dup || !add_track(&b, i, pt->usb_path)) {
            rx_warn("link: track %u is not served over NFS: %s %s", pt->id,
                    dup ? "its id is taken, path" : "path refused,",
                    pt->usb_path ? pt->usb_path : "(none)");
            continue;
        }
        t->files[t->nb_files++] = (IdIndex) { .id = pt->id, .node = t->nb_nodes - 1 };
        t->total_size += (uint64_t)RX_MAX(pt->file_size, 0);
    }
    qsort(t->files, t->nb_files, sizeof(*t->files), cmp_index);

    /* Children: every node but the root, sorted by parent, then name
     * ignoring ASCII case, then exactly. */
    t->kids = malloc((size_t)RX_MAX(t->nb_nodes, 1) * sizeof(*t->kids));
    t->dirs = malloc((size_t)t->nb_nodes * sizeof(*t->dirs));
    if (!t->kids || !t->dirs)
        goto nomem;
    uint32_t nk = t->nb_nodes - 1;
    for (uint32_t i = 1; i < t->nb_nodes; i++)
        t->kids[i - 1] = i;
    qsort_r(t->kids, nk, sizeof(*t->kids), cmp_kids, t);
    for (uint32_t i = 0; i < nk;) {
        uint32_t p = t->nodes[t->kids[i]].parent, j = i;
        while (j < nk && t->nodes[t->kids[j]].parent == p)
            j++;
        t->nodes[p].first = i;
        t->nodes[p].nb = j - i;
        i = j;
    }

    for (uint32_t i = 0; i < t->nb_nodes; i++) {
        if (t->nodes[i].track == -1)
            t->dirs[t->nb_dirs++] = (IdIndex) { .id = t->nodes[i].id, .node = i };
    }
    unique_ids(t);

    free(order);
    free(b.map);
    *out = t;
    return 0;

nomem:
    ret = RX_ERROR(ENOMEM);
    free(order);
    free(b.map);
    rx_vtree_free(&t);
    return ret;
}

void rx_vtree_free(RXVTree **pt)
{
    if (!pt || !*pt)
        return;
    RXVTree *t = *pt;
    free(t->nodes);
    free(t->kids);
    free(t->names);
    free(t->dirs);
    free(t->files);
    free(t);
    *pt = NULL;
}

/* ---------------------------------------------------------------- queries */

static bool is_node(const RXVTree *t, int node)
{
    return t && node >= 0 && (uint32_t)node < t->nb_nodes;
}

int rx_vtree_root(const RXVTree *t)
{
    return t ? 0 : -1;
}

bool rx_vtree_is_dir(const RXVTree *t, int node)
{
    return is_node(t, node) && t->nodes[node].track < 0;
}

const char *rx_vtree_name(const RXVTree *t, int node)
{
    return is_node(t, node) ? t->names + t->nodes[node].name : NULL;
}

uint64_t rx_vtree_dir_id(const RXVTree *t, int node)
{
    return rx_vtree_is_dir(t, node) ? t->nodes[node].id : 0;
}

const RXPlanTrack *rx_vtree_track(const RXVTree *t, int node)
{
    return is_node(t, node) && t->nodes[node].track >= 0 ? &t->plan->tracks[t->nodes[node].track]
                                                        : NULL;
}

int rx_vtree_nb_children(const RXVTree *t, int dir)
{
    return rx_vtree_is_dir(t, dir) ? (int)t->nodes[dir].nb : 0;
}

int rx_vtree_child(const RXVTree *t, int dir, int i)
{
    if (!rx_vtree_is_dir(t, dir) || i < 0 || (uint32_t)i >= t->nodes[dir].nb)
        return -1;
    return (int)t->kids[t->nodes[dir].first + (uint32_t)i];
}

int rx_vtree_lookup(const RXVTree *t, int dir, const char *name, size_t len)
{
    if (!rx_vtree_is_dir(t, dir) || !name || !len || memchr(name, '\0', len))
        return -1;
    const uint32_t *k = t->kids + t->nodes[dir].first;
    uint32_t lo = 0, hi = t->nodes[dir].nb;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cmp_fold(t->names + t->nodes[k[mid]].name, name, len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    /* The names equal but for ASCII case are together, sorted exactly
     * among themselves: the exact one if it is there, else the first. */
    int first = -1;
    for (uint32_t i = lo; i < t->nodes[dir].nb; i++) {
        const char *n = t->names + t->nodes[k[i]].name;
        if (cmp_fold(n, name, len))
            break;
        if (!cmp_exact(n, name, len))
            return (int)k[i];
        if (first < 0)
            first = (int)k[i];
    }
    return first;
}

static int find_index(const IdIndex *idx, uint32_t nb, uint64_t id)
{
    uint32_t lo = 0, hi = nb;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (idx[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < nb && idx[lo].id == id ? (int)idx[lo].node : -1;
}

int rx_vtree_find_dir(const RXVTree *t, uint64_t id)
{
    return t ? find_index(t->dirs, t->nb_dirs, id) : -1;
}

int rx_vtree_find_file(const RXVTree *t, uint32_t track_id)
{
    return t ? find_index(t->files, t->nb_files, track_id) : -1;
}

int rx_vtree_walk(const RXVTree *t, const char *path)
{
    if (!t || !path)
        return -1;
    int at = 0;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        const char *e = strchr(path, '/');
        size_t len = e ? (size_t)(e - path) : strlen(path);
        if ((at = rx_vtree_lookup(t, at, path, len)) < 0)
            return -1;
        path += len;
    }
    return at;
}

void rx_vtree_stats(const RXVTree *t, RXVTreeStats *s)
{
    *s = (RXVTreeStats) { 0 };
    if (!t)
        return;
    s->nb_files   = (int)t->nb_files;
    s->nb_dirs    = (int)t->nb_dirs;
    s->total_size = t->total_size;
    s->time       = t->time;
}
