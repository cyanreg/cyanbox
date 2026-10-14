/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A deterministic fuzzer for the file server (src/link/nfs.c, vtree.c)
 * and the client's decoders (nfsclient.c), built with AddressSanitizer
 * and UndefinedBehaviorSanitizer, which abort on the first finding
 * (src/link/meson.build).
 *
 * Into rx_nfs_handle, on each of the three sockets, from a player that
 * mounted, a peer in the subnet that did not, and one outside it: random
 * datagrams, and valid calls of every procedure mutated (bits flipped,
 * cut, extended, lengths and counts that lie, words replaced with the
 * values that break parsers, pieces of two calls spliced), with handles
 * of this session, of another one, random, and ours with a player's own
 * tail; the reply buffer now and then too small. Meanwhile the tree is
 * built again from one plan or another, or taken away. What is checked
 * beyond the sanitizers: a reply fits its buffer and the largest
 * datagram, carries the call's xid and decodes; a refusal is never
 * larger than the call; a peer that has not mounted never gets a file's
 * bytes (one that mounted before and was forgotten since may, through a
 * handle of ours: a deck back from a lapse; one outside the subnet never
 * can); at the end, no file but the plan's is open.
 *
 * Into the client's decoders: the server's own replies, mutated, and
 * random bytes; names round-trip through UTF-16LE.
 *
 * RX_FUZZ_SEED sets the seed, RX_FUZZ_ITERS the number of rounds; the
 * defaults run in a few seconds. Returns 0 when clean. */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rxbox/common.h"

#include "src/link/nfs.h"

#define HOST        0xC0A8010Eu
#define MASK        0xFFFFFF00u
#define PLAYER      0xC0A80123u
#define STRANGER    0xC0A80199u
#define OUTSIDER    0x0A000005u

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            if (++failures > 20)                                      \
                exit(1);                                              \
        }                                                             \
    } while (0)

/* ------------------------------------------------------------------- prng */

static uint64_t rng;

static uint64_t next(void)
{
    uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ z >> 27) * 0x94D049BB133111EBull;
    return z ^ z >> 31;
}

static uint32_t below(uint32_t n)
{
    return n ? (uint32_t)(next() % n) : 0;
}

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)next();
}

/* ------------------------------------------------------------------ setup */

static char dir[512];

static char *make_file(const char *name, size_t size)
{
    char *p = rx_path_join(dir, name);
    uint8_t *d = malloc(size ? size : 1);
    if (!p || !d)
        abort();
    fill(d, size);
    if (rx_file_write(p, d, size) < 0)
        abort();
    free(d);
    return p;
}

typedef struct World {
    RXPlanTrack tracks[2][6];
    RXPlan      plans[2];
    RXVTree    *trees[2];
    RXNfs      *nfs, *other;
    char       *files[3];
    uint8_t     handles[24][RX_NFS_FHSIZE];
    int         nb_handles;
} World;

static const char *const paths[2][6] = {
    { "/Contents/Artist/Album/01 One.flac", "/Contents/Artist/Album/02 Two.mp3",
      "/Contents/Other/Album/03 Three.wav", "/Contents/\U0001F3B5/É/04 Four.flac",
      "/Contents/A/B/C/D/deep.mp3", "/../escape" },
    { "/Contents/Artist/Album (Deluxe)/01 One.flac", "/Contents/Artist/Album/02 Two.mp3",
      "/Contents/New/Album/05 Five.flac", "/Contents/\U0001F3B5/É/04 Four.flac",
      "/x", "/Contents/ARTIST/Album/06 Six.mp3" },
};

static void add_handle(World *w, const uint8_t *fh)
{
    if (w->nb_handles < (int)RX_ARRAY_ELEMS(w->handles))
        memcpy(w->handles[w->nb_handles++], fh, RX_NFS_FHSIZE);
}

/* A call to this world's server, answered: the reply's length. */
static uint8_t out[RX_NFS_MAX_DATAGRAM];

static ssize_t ask(RXNfs *n, enum RXNfsService svc, uint32_t peer, const RXWr *w)
{
    return rx_nfs_handle(n, svc, peer, w->p, w->len, out, sizeof(out));
}

/* LOOKUPs a path from a handle, keeping every handle found. */
static void collect(World *w, RXNfs *n, const uint8_t *root, const char *path)
{
    uint8_t at[RX_NFS_FHSIZE];
    memcpy(at, root, sizeof(at));
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *save = NULL, *p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        uint8_t b[1024];
        RXWr c = rx_wr(b, sizeof(b));
        rx_nfs_put_call(&c, (uint32_t)next(), RX_RPC_NFS, 2, RX_NFSPROC_LOOKUP);
        rx_nfs_put_handle(&c, at);
        rx_nfs_put_name(&c, p);
        ssize_t len = ask(n, RX_NFS_NFS, PLAYER, &c);
        RXRd r;
        if (len <= 0 || rx_nfs_get_reply(out, (size_t)len, NULL, &r) < 0 ||
            rx_nfs_get_status(&r) < 0 || rx_nfs_get_handle(&r, at) < 0)
            return;
        add_handle(w, at);
    }
}

static void mount_root(World *w, RXNfs *n, uint8_t root[RX_NFS_FHSIZE])
{
    uint8_t b[256];
    RXWr c = rx_wr(b, sizeof(b));
    rx_nfs_put_call(&c, (uint32_t)next(), RX_RPC_MOUNT, 1, RX_MNT_MNT);
    rx_nfs_put_name(&c, "/");
    ssize_t len = ask(n, RX_NFS_MOUNT, PLAYER, &c);
    RXRd r;
    if (len <= 0 || rx_nfs_get_reply(out, (size_t)len, NULL, &r) < 0 ||
        rx_nfs_get_status(&r) < 0 || rx_nfs_get_handle(&r, root) < 0)
        abort();
}

static void world_new(World *w)
{
    *w = (World) { 0 };
    w->files[0] = make_file("one.bin", 70001);
    w->files[1] = make_file("two.bin", 3);
    w->files[2] = make_file("three.bin", 0);
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 6; i++) {
            w->tracks[k][i] = (RXPlanTrack) {
                .id        = (uint32_t)(i + 1 + (k && i == 2 ? 10 : 0)),
                .usb_path  = (char *)paths[k][i],
                .source    = w->files[i % 3],
                .file_size = i % 3 == 0 ? 70001 : i % 3 == 1 ? 3 : 0,
            };
        }
        w->plans[k] = (RXPlan) { .tracks = w->tracks[k], .nb_tracks = 6 };
        if (rx_vtree_new(&w->trees[k], &w->plans[k]) < 0)
            abort();
    }
    RXNfsConfig c = {
        .mount_port = 1234, .nfs_port = 2049, .portmap_port = 50111,
        .addr = HOST, .netmask = MASK,
    };
    if (rx_nfs_new(&w->nfs, &c) < 0 || rx_nfs_new(&w->other, &c) < 0)
        abort();
    rx_nfs_set_tree(w->nfs, w->trees[0]);
    rx_nfs_set_tree(w->other, w->trees[0]);

    uint8_t root[RX_NFS_FHSIZE];
    for (int k = 0; k < 2; k++) {
        rx_nfs_set_tree(w->nfs, w->trees[k]);
        mount_root(w, w->nfs, root);
        add_handle(w, root);
        for (int i = 0; i < 6; i++)
            collect(w, w->nfs, root, paths[k][i]);
    }
    rx_nfs_set_tree(w->nfs, w->trees[0]);
    mount_root(w, w->other, root);
    add_handle(w, root);
    collect(w, w->other, root, paths[0][0]);
}

static void world_free(World *w)
{
    rx_nfs_free(&w->nfs);
    rx_nfs_free(&w->other);
    for (int k = 0; k < 2; k++)
        rx_vtree_free(&w->trees[k]);
    for (int i = 0; i < 3; i++) {
        unlink(w->files[i]);
        free(w->files[i]);
    }
}

/* ------------------------------------------------------------------ calls */

static const char *const names[] = {
    "Contents", "Artist", "Album", "01 One.flac", "..", ".", "", "/etc/passwd", "ARTIST",
    "\U0001F3B5", "É", "artist", "x", "Album (Deluxe)",
};

/* A handle: one of ours (a file's, when `file` and there is one), ours
 * with a tail or a bit flipped, or random. */
static void put_some_handle(World *w, RXWr *c, bool file)
{
    uint8_t fh[RX_NFS_FHSIZE];
    if (file && below(2)) {
        for (int tries = 0; tries < 64; tries++) {
            const uint8_t *h = w->handles[below((uint32_t)w->nb_handles)];
            if (!h[4] && !h[5] && !h[6] && !h[7] && h[11] % 3 == 1) {
                rx_nfs_put_handle(c, h);
                return;
            }
        }
    }
    switch (below(5)) {
    case 0:
        fill(fh, sizeof(fh));
        break;
    case 1:
        /* Ours with a player's own tail, as an XDJ-700 sends them. */
        memcpy(fh, w->handles[below((uint32_t)w->nb_handles)], sizeof(fh));
        fill(fh + 12, 20);
        break;
    case 2:
        memcpy(fh, w->handles[below((uint32_t)w->nb_handles)], sizeof(fh));
        fh[below(12)] ^= (uint8_t)(1 << below(8));
        break;
    default:
        memcpy(fh, w->handles[below((uint32_t)w->nb_handles)], sizeof(fh));
        break;
    }
    rx_nfs_put_handle(c, fh);
}

static void put_some_name(RXWr *c)
{
    if (below(4)) {
        rx_nfs_put_name(c, names[below(RX_ARRAY_ELEMS(names))]);
        return;
    }
    uint8_t b[600];
    uint32_t n = below(4) ? below(64) : below(sizeof(b));
    fill(b, n);
    rx_wr_u32(c, below(8) ? n : (uint32_t)next());
    rx_wr_bytes(c, b, n);
    rx_wr_zeros(c, (4 - (n & 3)) & 3);
}

/* A valid call of a random procedure: which socket it is for in *svc. */
static void make_call(World *w, RXWr *c, enum RXNfsService *svc)
{
    uint32_t xid = (uint32_t)next();
    switch (below(3)) {
    case 0: {
        *svc = RX_NFS_PORTMAP;
        uint32_t proc = below(8);
        rx_nfs_put_call(c, xid, RX_RPC_PORTMAP, 2, proc);
        if (proc == RX_PMAP_GETPORT || proc == RX_PMAP_SET || proc == RX_PMAP_CALLIT) {
            const uint32_t progs[] = { RX_RPC_MOUNT, RX_RPC_NFS, RX_RPC_PORTMAP, 100024 };
            rx_wr_u32(c, progs[below(4)]);
            rx_wr_u32(c, below(4));
            rx_wr_u32(c, below(2) ? 17 : 6);
            rx_wr_u32(c, below(65536));
        }
        return;
    }
    case 1: {
        *svc = RX_NFS_MOUNT;
        uint32_t proc = below(8);
        rx_nfs_put_call(c, xid, RX_RPC_MOUNT, 1, proc);
        if (proc == RX_MNT_MNT || proc == RX_MNT_UMNT)
            rx_nfs_put_name(c, below(3) ? "/" : names[below(RX_ARRAY_ELEMS(names))]);
        return;
    }
    default: {
        *svc = RX_NFS_NFS;
        uint32_t proc = below(4) ? (const uint32_t[]) { 1, 4, 6, 6, 16, 17 }[below(6)] : below(20);
        rx_nfs_put_call(c, xid, RX_RPC_NFS, 2, proc);
        if (proc)
            put_some_handle(w, c, proc == RX_NFSPROC_READ);
        if (proc == RX_NFSPROC_LOOKUP || (proc >= RX_NFSPROC_CREATE && proc <= RX_NFSPROC_RMDIR))
            put_some_name(c);
        if (proc == RX_NFSPROC_READ) {
            rx_wr_u32(c, below(3) ? below(80000) : (uint32_t)next());
            rx_wr_u32(c, below(3) ? (const uint32_t[]) { 32768, 8192, 0xfc00, 1 }[below(4)]
                                  : (uint32_t)next());
            rx_wr_u32(c, 0);
        }
        if (proc == RX_NFSPROC_READDIR) {
            rx_wr_u32(c, below(3) ? below(8) : (uint32_t)next());
            rx_wr_u32(c, below(2) ? 8192 : (uint32_t)next());
        }
        if (proc == RX_NFSPROC_WRITE)
            rx_wr_zeros(c, 16 + below(64));
        return;
    }
    }
}

/* The values that break parsers, for words a mutation replaces. */
static uint32_t nasty(size_t len)
{
    const uint32_t v[] = {
        0, 1, 2, 3, 4, 17, 20, 32, 255, 256, 400, 401, 510, 511, 512, 1024, 1025, 0xfc00,
        0x7FFFFFFF, 0x80000000, 0xFFFFFFFC, 0xFFFFFFFF, (uint32_t)len, (uint32_t)len + 1,
        100003, 100005, 100000, 100021,
    };
    return v[below(RX_ARRAY_ELEMS(v))];
}

static size_t mutate(uint8_t *p, size_t len, size_t cap, const uint8_t *other, size_t olen)
{
    int rounds = 1 + (int)below(4);
    for (int k = 0; k < rounds; k++) {
        switch (below(8)) {
        case 0:                             /* flip bits */
            for (uint32_t i = 0, n = 1 + below(8); i < n && len; i++)
                p[below((uint32_t)len)] ^= (uint8_t)(1 << below(8));
            break;
        case 1:                             /* cut */
            len = below((uint32_t)len + 1);
            break;
        case 2: {                           /* extend */
            size_t n = RX_MIN((size_t)below(below(2) ? 16 : 600), cap - len);
            fill(p + len, n);
            len += n;
            break;
        }
        case 3:                             /* a word replaced */
            if (len >= 4) {
                size_t at = below((uint32_t)len / 4) * 4;
                uint32_t v = nasty(len);
                p[at] = (uint8_t)(v >> 24);
                p[at + 1] = (uint8_t)(v >> 16);
                p[at + 2] = (uint8_t)(v >> 8);
                p[at + 3] = (uint8_t)v;
            }
            break;
        case 4:                             /* a random byte */
            if (len)
                p[below((uint32_t)len)] = (uint8_t)next();
            break;
        case 5: {                           /* four bytes out */
            if (len >= 8) {
                size_t at = below((uint32_t)len - 4);
                memmove(p + at, p + at + 4, len - at - 4);
                len -= 4;
            }
            break;
        }
        case 6: {                           /* four bytes in */
            if (len + 4 <= cap) {
                size_t at = below((uint32_t)len + 1);
                memmove(p + at + 4, p + at, len - at);
                fill(p + at, 4);
                len += 4;
            }
            break;
        }
        default: {                          /* another call's tail spliced on */
            size_t at = below((uint32_t)len + 1), from = below((uint32_t)olen + 1);
            size_t n = RX_MIN(olen - from, cap - at);
            memcpy(p + at, other + from, n);
            len = at + n;
            break;
        }
        }
    }
    return len;
}

/* ----------------------------------------------------------------- checks */

static bool status_first(const uint8_t *call, size_t len)
{
    if (len < 24)
        return false;
    uint32_t prog = (uint32_t)call[12] << 24 | (uint32_t)call[13] << 16 | (uint32_t)call[14] << 8 |
                    call[15];
    uint32_t proc = (uint32_t)call[20] << 24 | (uint32_t)call[21] << 16 | (uint32_t)call[22] << 8 |
                    call[23];
    return (prog == RX_RPC_NFS && proc) || (prog == RX_RPC_MOUNT && proc == RX_MNT_MNT);
}

static void check_reply(const uint8_t *req, size_t len, ssize_t n, size_t cap, bool mounted,
                        uint64_t round)
{
    CHECK(n >= 0 || n == -ENOBUFS, "round %llu: %zd", (unsigned long long)round, n);
    if (n <= 0)
        return;
    CHECK((size_t)n <= cap && n <= RX_NFS_MAX_DATAGRAM, "round %llu: %zd bytes in %zu",
          (unsigned long long)round, n, cap);
    CHECK(len >= 12 && !memcmp(out, req, 4), "round %llu: the xid", (unsigned long long)round);
    RXRd r;
    int ret = rx_nfs_get_reply(out, (size_t)n, NULL, &r);
    CHECK(ret != -EBADMSG, "round %llu: our reply does not decode", (unsigned long long)round);
    bool refusal = ret < 0 || (status_first(req, len) && rx_rd_left(&r) >= 4 &&
                               rx_nfs_get_status(&r) < 0);
    CHECK(!refusal || (size_t)n <= len, "round %llu: a refusal of %zd bytes to %zu",
          (unsigned long long)round, n, len);
    /* The largest answer that is not a file's bytes or a listing is a
     * LOOKUP's: 128. */
    CHECK(mounted || n <= 160, "round %llu: %zd bytes to a peer that did not mount",
          (unsigned long long)round, n);
}

/* --------------------------------------------------------------- decoders */

static int entry_cb(void *opaque, uint32_t fileid, const char *name, uint32_t cookie)
{
    *(size_t *)opaque += strlen(name);
    return below(50) ? 0 : -ECANCELED;
}

static int export_cb(void *opaque, const char *dirpath, const char *group)
{
    *(size_t *)opaque += strlen(dirpath) + (group ? strlen(group) : 0);
    return 0;
}

static void decode_all(const uint8_t *p, size_t len)
{
    RXRd res;
    size_t sink = 0;
    if (rx_nfs_get_reply(p, len, NULL, &res) < 0)
        res = rx_rd(p, len);
    RXNfsAttr a;
    uint8_t fh[RX_NFS_FHSIZE];
    bool eof;
    RXRd r = res;
    rx_nfs_get_status(&r);
    rx_nfs_get_fattr(&r, &a);
    rx_nfs_get_handle(&r, fh);
    r = res;
    rx_nfs_get_status(&r);
    rx_nfs_get_entries(&r, entry_cb, &sink, &eof);
    r = res;
    rx_nfs_get_exports(&r, export_cb, &sink);
    r = res;
    char name[RX_NFS_NAME_MAX * 3 + 1];
    size_t size = below(4) ? sizeof(name) : 1 + below(16);
    int n = rx_nfs_get_name(&r, below(2) ? RX_NFS_NAME_MAX : below(300), name, size);
    CHECK(n < 0 || (size_t)n < size, "a name of %d in %zu", n, size);
    if (n >= 0)
        CHECK(strlen(name) == (size_t)n, "a name's length");
}

/* Random UTF-8 strings through UTF-16LE and back. */
static void round_trip(void)
{
    char s[256];
    size_t o = 0;
    for (uint32_t i = 0, n = below(40); i < n; i++) {
        uint32_t c;
        do {
            c = below(4) ? 0x20 + below(0x5F) : below(2) ? 0x80 + below(0xF780) : 0x10000 + below(0x100000);
        } while ((c >= 0xD800 && c < 0xE000) || !c);
        if (c < 0x80) {
            s[o++] = (char)c;
        } else if (c < 0x800) {
            s[o++] = (char)(0xC0 | c >> 6);
            s[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s[o++] = (char)(0xE0 | c >> 12);
            s[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            s[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            s[o++] = (char)(0xF0 | c >> 18);
            s[o++] = (char)(0x80 | (c >> 12 & 0x3F));
            s[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            s[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    s[o] = '\0';
    uint8_t b[512];
    RXWr w = rx_wr(b, sizeof(b));
    rx_nfs_put_name(&w, s);
    RXRd r = rx_rd(b, w.len);
    char back[RX_NFS_NAME_MAX * 3 + 1];
    int n = rx_nfs_get_name(&r, RX_NFS_NAME_MAX, back, sizeof(back));
    CHECK(!w.err && n == (int)o && !strcmp(s, back) && !rx_rd_left(&r), "round trip of %zu", o);
}

/* ------------------------------------------------------------- open files */

/* Every regular file open now is one of the plan's. */
static void check_open_files(const World *w)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char link[300], target[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0 || target[0] != '/' || !strncmp(target, "/dev/", 5) ||
            !strncmp(target, "/proc/", 6))
            continue;
        target[n] = '\0';
        bool ours = false;
        for (int i = 0; i < 3; i++)
            ours |= !strcmp(target, w->files[i]);
        /* The sanitizers' and the loader's own files are not ours to
         * judge; nothing of ours is opened outside the test directory. */
        CHECK(ours || strncmp(target, dir, strlen(dir)), "%s is open", target);
    }
    closedir(d);
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    const char *seed = getenv("RX_FUZZ_SEED"), *iters = getenv("RX_FUZZ_ITERS");
    rng = seed ? strtoull(seed, NULL, 0) : 0x5EED0F5EEDull;
    uint64_t rounds = iters ? strtoull(iters, NULL, 0) : 1000000;
    const char *base = getenv("RX_TEST_DIR");
    snprintf(dir, sizeof(dir), "%s/fuzz-%d", base ? base : "/tmp/rxbox-fuzz-nfs", (int)getpid());
    if (rx_mkdir_p(dir) < 0) {
        fprintf(stderr, "cannot make %s\n", dir);
        return 1;
    }
    rx_log_set_level(RX_LOG_ERROR);
    printf("fuzz-nfs: seed 0x%llx, %llu rounds\n", (unsigned long long)rng,
           (unsigned long long)rounds);

    World w;
    world_new(&w);
    if (getenv("RX_FUZZ_DEBUG")) {
        for (int i = 0; i < w.nb_handles; i++) {
            for (int k = 0; k < 12; k++)
                fprintf(stderr, "%02x", w.handles[i][k]);
            fputc('\n', stderr);
        }
    }
    static uint8_t req[RX_NFS_MAX_DATAGRAM + 64], prev[4096];
    size_t prev_len = 0;
    static uint8_t replies[8][2048];
    size_t reply_len[8] = { 0 };
    const uint32_t peers[3] = { PLAYER, STRANGER, OUTSIDER };
    uint64_t answered = 0, reads = 0, lists = 0;

    for (uint64_t round = 0; round < rounds; round++) {
        /* The library changes now and then, or Link has nothing to serve. */
        if (!below(2000)) {
            uint32_t k = below(3);
            rx_nfs_set_tree(w.nfs, k < 2 ? w.trees[k] : NULL);
        }
        if (!below(500))
            rx_nfs_forget(w.nfs, STRANGER);

        size_t len;
        enum RXNfsService svc;
        if (!below(10)) {
            len = below(below(2) ? 64 : 1500);
            fill(req, len);
            svc = (enum RXNfsService)below(3);
        } else {
            RXWr c = rx_wr(req, 4096);
            make_call(&w, &c, &svc);
            len = c.len;
            uint8_t keep[4096];
            memcpy(keep, req, len);
            if (below(2))
                len = mutate(req, len, 4096, prev, prev_len);
            memcpy(prev, keep, c.len);
            prev_len = c.len;
            if (!below(4))
                svc = (enum RXNfsService)below(3);
        }
        int who = below(4) ? 0 : 1 + (int)below(2);
        uint32_t peer = peers[who];
        size_t cap = below(20) ? sizeof(out) : below(300);
        /* Mounted now, or seen mounted before (and forgotten since). */
        static bool ever[3];
        bool mounted = rx_nfs_mounted(w.nfs, peer) || ever[who];
        ever[who] |= rx_nfs_mounted(w.nfs, peer);
        ssize_t n = rx_nfs_handle(w.nfs, svc, peer, req, len, out, cap);
        check_reply(req, len, n, cap, mounted, round);
        ever[who] |= rx_nfs_mounted(w.nfs, peer);
        CHECK(!ever[2], "round %llu: the outsider mounted", (unsigned long long)round);
        if (n > 0) {
            answered++;
            RXRd r;
            bool ok = svc == RX_NFS_NFS && rx_nfs_get_reply(out, (size_t)n, NULL, &r) == 0 &&
                      rx_nfs_get_status(&r) == 0;
            reads += ok && req[23] == RX_NFSPROC_READ;
            lists += ok && req[23] == RX_NFSPROC_READDIR;
            uint32_t k = below(8);
            reply_len[k] = RX_MIN((size_t)n, sizeof(replies[k]));
            memcpy(replies[k], out, reply_len[k]);
        }

        /* The client's decoders: a reply of ours mutated, or noise. */
        uint8_t dec[4096];
        uint32_t k = below(8);
        size_t dl = reply_len[k];
        memcpy(dec, replies[k], dl);
        if (below(3))
            dl = mutate(dec, dl, sizeof(dec), req, RX_MIN(len, sizeof(dec)));
        if (!below(8)) {
            dl = below(512);
            fill(dec, dl);
        }
        decode_all(dec, dl);
        if (!below(16))
            round_trip();
    }
    check_open_files(&w);
    printf("fuzz-nfs: %llu answered, %llu with a file's bytes, %llu listings\n",
           (unsigned long long)answered, (unsigned long long)reads, (unsigned long long)lists);
    CHECK(answered > rounds / 4 && reads > rounds / 200 && lists > rounds / 1000,
          "too few answered: %llu, %llu, %llu", (unsigned long long)answered,
          (unsigned long long)reads, (unsigned long long)lists);

    world_free(&w);
    rmdir(dir);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("fuzz-nfs: clean\n");
    return 0;
}
