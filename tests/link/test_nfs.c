/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The file server players read audio from (src/link/nfs.c, vtree.c and
 * nfsclient.c), driven as a player drives it and as anyone else on the
 * players' network might. Our server checked only by our own client would
 * agree with itself, so the fixed vectors are other people's: the bytes
 * rbxport captured from players and rekordbox, and the replies it measured
 * rekordbox giving (its rbl-nfs tests/protocol.rs and the commits that
 * carry the captures), and RFC 1057's call layout assembled by hand.
 *
 * A hand-made CBPlan over small files in CB_TEST_DIR (removed at the end):
 * mount, walk and whole reads bit for bit in a player's 32 kB pieces and
 * others, across piece boundaries and at the end; listings in pages; the
 * attributes; every way out of the tree a peer could try; forged, stale
 * and malformed handles; reads from a peer that did not mount; writes;
 * unknown programs, versions and procedures; every truncation of every
 * call answered with nothing or something no larger; a tree built again
 * from a new plan under an open file; a transcode made later. Last, the
 * three programs on ephemeral UDP ports of 127.0.0.1, served by a thread
 * here, and the client reading a file through them. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cyanbox/common.h"

#include "src/link/nfs.h"

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            failures++;                                               \
        }                                                             \
    } while (0)

/* rbxport's capture addresses: rekordbox at 192.168.1.14/24, a CDJ-3000
 * at 192.168.1.35. A second player that never mounts, and a host outside
 * the subnet. */
#define HOST        0xC0A8010Eu
#define MASK        0xFFFFFF00u
#define PLAYER      0xC0A80123u
#define STRANGER    0xC0A80199u
#define OUTSIDER    0x0A000005u

static char dir[512];
static uint8_t reply[CB_NFS_MAX_DATAGRAM];
static uint32_t next_xid = 0x1234;

/* ------------------------------------------------------------------ files */

static uint8_t pattern(uint32_t seed, size_t i)
{
    return (uint8_t)(i * 131 + seed * 7 + (i >> 9));
}

static char *make_file(const char *name, uint32_t seed, size_t size)
{
    char *p = cb_path_join(dir, name);
    uint8_t *d = malloc(size ? size : 1);
    if (!p || !d)
        abort();
    for (size_t i = 0; i < size; i++)
        d[i] = pattern(seed, i);
    if (cb_file_write(p, d, size) < 0)
        abort();
    free(d);
    return p;
}

static bool same_as_file(const char *path, const uint8_t *data, size_t size)
{
    uint8_t *d;
    size_t n;
    if (cb_file_read(path, &d, &n) < 0)
        return false;
    bool same = n == size && (!n || !memcmp(d, data, n));
    free(d);
    return same;
}

/* ---------------------------------------------------------------- fixture */

typedef struct Spec {
    uint32_t    id;
    const char *usb;
    const char *file;           /* in the test directory; NULL: the absolute `host` */
    size_t      size;
    const char *host;
} Spec;

/* Five tracks served; the rest are refused when the tree is built. */
static const Spec specs[] = {
    { 101, "/Contents/Björk/Homogenic/01 Jóga.flac",          "a.bin", 100000, NULL },
    { 102, "/Contents/Björk/Homogenic/02 Unravel.flac",       "b.bin", 65543, NULL },
    { 103, "/Contents/ABBA/Gold/01 Dancing Queen.mp3",        "c.bin", 32768, NULL },
    { 104, "/Contents/Abba/Gold/02 Waterloo.mp3",             "d.bin", 5, NULL },
    { 105, "/Contents/UnknownArtist/UnknownAlbum/\U0001F3B5 Song.wav", "e.bin", 0, NULL },
    /* Refused, with host files behind them that must stay unreachable. */
    { 106, "/Contents/../etc/passwd",                         NULL, 0, "/etc/passwd" },
    { 107, "Contents/relative.mp3",                           NULL, 0, "/etc/passwd" },
    { 108, "/Contents/Björk/Homogenic/01 Jóga.flac",          NULL, 0, "/etc/passwd" },
    { 101, "/Contents/Dup/x.mp3",                             NULL, 0, "/etc/passwd" },
    { 109, "/Contents//empty.mp3",                            NULL, 0, "/etc/passwd" },
    { 110, "/Contents/./dot.mp3",                             NULL, 0, "/etc/passwd" },
    { 111, "/Contents/Trailing/",                             NULL, 0, "/etc/passwd" },
    { 112, NULL,                                              NULL, 0, "/etc/passwd" },
};
#define NB_SERVED 5

typedef struct Fixture {
    CBPlanTrack tracks[CB_ARRAY_ELEMS(specs)];
    CBPlan      plan;
    CBVTree    *tree;
    CBNfs      *nfs;
} Fixture;

static void fixture_new(Fixture *f)
{
    *f = (Fixture) { 0 };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(specs); i++) {
        const Spec *s = &specs[i];
        f->tracks[i] = (CBPlanTrack) {
            .id        = s->id,
            .usb_path  = s->usb ? cb_strdup(s->usb) : NULL,
            .source    = s->file ? make_file(s->file, s->id % 100, s->size) : cb_strdup(s->host),
            .file_size = (int64_t)s->size,
        };
    }
    f->plan = (CBPlan) { .tracks = f->tracks, .nb_tracks = (int)CB_ARRAY_ELEMS(specs) };
    CBNfsConfig c = {
        .mount_port = 12005, .nfs_port = 12049, .portmap_port = 50111,
        .addr = HOST, .netmask = MASK,
    };
    if (cb_vtree_new(&f->tree, &f->plan) < 0 || cb_nfs_new(&f->nfs, &c) < 0)
        abort();
    cb_nfs_set_tree(f->nfs, f->tree);
}

static void fixture_free(Fixture *f)
{
    cb_nfs_free(&f->nfs);
    cb_vtree_free(&f->tree);
    for (size_t i = 0; i < CB_ARRAY_ELEMS(specs); i++) {
        free(f->tracks[i].usb_path);
        free(f->tracks[i].source);
    }
}

static const CBPlanTrack *track(const Fixture *f, uint32_t id)
{
    for (size_t i = 0; i < CB_ARRAY_ELEMS(specs); i++) {
        if (f->tracks[i].id == id)
            return &f->tracks[i];
    }
    abort();
}

/* ------------------------------------------------------------------ calls */

typedef struct Call {
    uint8_t  b[4096];
    CBWr     w;
    uint32_t xid;
} Call;

static void call(Call *c, uint32_t prog, uint32_t vers, uint32_t proc)
{
    c->w = cb_wr(c->b, sizeof(c->b));
    c->xid = next_xid++;
    cb_nfs_put_call(&c->w, c->xid, prog, vers, proc);
}

static ssize_t ask(CBNfs *n, enum CBNfsService svc, uint32_t peer, const Call *c)
{
    return cb_nfs_handle(n, svc, peer, c->b, c->w.len, reply, sizeof(reply));
}

/* The results of the reply to `c`; a refusal as the client's errno,
 * -ENODATA when nothing was answered. */
static int results(ssize_t len, const Call *c, CBRd *res)
{
    if (len <= 0)
        return len < 0 ? (int)len : -ENODATA;
    uint32_t x = 0;
    int ret = cb_nfs_get_reply(reply, (size_t)len, &x, res);
    return x != c->xid ? -EBADMSG : ret;
}

/* The status of an NFS reply to `c`, as an errno. */
static int status_of(ssize_t len, const Call *c, CBRd *res)
{
    int ret = results(len, c, res);
    return ret < 0 ? ret : cb_nfs_get_status(res);
}

static int mnt(CBNfs *n, uint32_t peer, const char *path, uint8_t fh[CB_NFS_FHSIZE])
{
    Call c;
    call(&c, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_MNT);
    cb_nfs_put_name(&c.w, path);
    CBRd r;
    int ret = status_of(ask(n, CB_NFS_MOUNT, peer, &c), &c, &r);
    return ret < 0 ? ret : cb_nfs_get_handle(&r, fh);
}

static int lookup(CBNfs *n, const uint8_t d[CB_NFS_FHSIZE], const char *name,
                  uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a)
{
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_LOOKUP);
    cb_nfs_put_handle(&c.w, d);
    cb_nfs_put_name(&c.w, name);
    CBRd r;
    int ret = status_of(ask(n, CB_NFS_NFS, PLAYER, &c), &c, &r);
    if (ret < 0)
        return ret;
    uint8_t tmp[CB_NFS_FHSIZE];
    if ((ret = cb_nfs_get_handle(&r, fh ? fh : tmp)) < 0)
        return ret;
    return cb_nfs_get_fattr(&r, a);
}

/* A LOOKUP whose name is the raw bytes given, `len` its declared length. */
static int lookup_raw(CBNfs *n, const uint8_t d[CB_NFS_FHSIZE], const uint8_t *name,
                      size_t bytes, uint32_t len)
{
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_LOOKUP);
    cb_nfs_put_handle(&c.w, d);
    cb_wr_u32(&c.w, len);
    cb_wr_bytes(&c.w, name, bytes);
    cb_wr_zeros(&c.w, (4 - (bytes & 3)) & 3);
    CBRd r;
    return status_of(ask(n, CB_NFS_NFS, PLAYER, &c), &c, &r);
}

static int walk(CBNfs *n, const uint8_t root[CB_NFS_FHSIZE], const char *path,
                uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a)
{
    uint8_t at[CB_NFS_FHSIZE];
    memcpy(at, root, sizeof(at));
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *save = NULL, *p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        int ret = lookup(n, at, p, at, a);
        if (ret < 0)
            return ret;
    }
    memcpy(fh, at, CB_NFS_FHSIZE);
    return 0;
}

static int getattr(CBNfs *n, const uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a)
{
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_GETATTR);
    cb_nfs_put_handle(&c.w, fh);
    CBRd r;
    int ret = status_of(ask(n, CB_NFS_NFS, PLAYER, &c), &c, &r);
    return ret < 0 ? ret : cb_nfs_get_fattr(&r, a);
}

/* One READ: the bytes, or an errno. */
static int nfs_read(CBNfs *n, uint32_t peer, const uint8_t fh[CB_NFS_FHSIZE], uint32_t off,
                    uint32_t count, uint8_t *buf, size_t room, CBNfsAttr *a)
{
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_READ);
    cb_nfs_put_handle(&c.w, fh);
    cb_wr_u32(&c.w, off);
    cb_wr_u32(&c.w, count);
    cb_wr_u32(&c.w, 0);
    CBRd r;
    int ret = status_of(ask(n, CB_NFS_NFS, peer, &c), &c, &r);
    if (ret < 0 || (ret = cb_nfs_get_fattr(&r, a)) < 0)
        return ret;
    uint32_t len = cb_rd_u32(&r);
    const uint8_t *p = r.err || len > room ? NULL : cb_rd_bytes(&r, (len + 3) & ~3u);
    if (!p || cb_rd_left(&r))
        return -EBADMSG;
    memcpy(buf, p, len);
    return (int)len;
}

/* A whole file as a player reads it: `chunk` at a time to the size its
 * attributes give. */
static int read_whole(CBNfs *n, const uint8_t fh[CB_NFS_FHSIZE], uint32_t chunk,
                      uint8_t **data, size_t *size)
{
    CBNfsAttr a;
    int ret = getattr(n, fh, &a);
    if (ret < 0)
        return ret;
    uint8_t *d = malloc(a.size ? a.size : 1), *buf = malloc(CB_NFS_MAX_READ);
    if (!d || !buf)
        abort();
    size_t off = 0;
    while (off < a.size) {
        int got = nfs_read(n, PLAYER, fh, (uint32_t)off, chunk, buf, CB_NFS_MAX_READ, NULL);
        if (got <= 0 || off + (size_t)got > a.size) {
            ret = got < 0 ? got : -EIO;
            break;
        }
        memcpy(d + off, buf, (size_t)got);
        off += (size_t)got;
    }
    free(buf);
    if (ret < 0) {
        free(d);
        return ret;
    }
    *data = d;
    *size = off;
    return 0;
}

typedef struct List {
    char    *names[512];
    uint32_t fileids[512];
    int      nb, calls;
} List;

static int add_entry(void *opaque, uint32_t fileid, const char *name, uint32_t cookie)
{
    List *l = opaque;
    if (l->nb >= (int)CB_ARRAY_ELEMS(l->names))
        return -E2BIG;
    l->fileids[l->nb] = fileid;
    l->names[l->nb++] = cb_strdup(name);
    return 0;
}

static void list_free(List *l)
{
    for (int i = 0; i < l->nb; i++)
        free(l->names[i]);
    *l = (List) { 0 };
}

/* READDIR from the start to eof, `count` bytes at a time. */
static int list(CBNfs *n, uint32_t peer, const uint8_t fh[CB_NFS_FHSIZE], uint32_t count, List *l)
{
    *l = (List) { 0 };
    uint32_t cookie = 0;
    for (int k = 0; k < 1000; k++) {
        Call c;
        call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_READDIR);
        cb_nfs_put_handle(&c.w, fh);
        cb_wr_u32(&c.w, cookie);
        cb_wr_u32(&c.w, count);
        CBRd r;
        ssize_t len = ask(n, CB_NFS_NFS, peer, &c);
        int ret = status_of(len, &c, &r);
        if (ret < 0)
            return ret;
        if ((size_t)len > 24 + CB_MAX(count, 1024u) || (size_t)len > 24 + 8192)
            return -E2BIG;
        l->calls++;
        bool eof = false;
        int before = l->nb;
        if ((ret = cb_nfs_get_entries(&r, add_entry, l, &eof)) < 0)
            return ret;
        if (eof)
            return 0;
        if (l->nb == before)
            return -ELOOP;
        cookie += (uint32_t)(l->nb - before);
    }
    return -ELOOP;
}

static bool same_list(const List *l, const char *const *names, int nb)
{
    if (l->nb != nb)
        return false;
    for (int i = 0; i < nb; i++) {
        if (strcmp(l->names[i], names[i]))
            return false;
    }
    return true;
}

static uint32_t be32_of(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int from_hex(const char *hex, uint8_t *out, size_t size)
{
    size_t n = strlen(hex) / 2;
    if (n > size)
        abort();
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return (int)n;
}

/* ---------------------------------------------------------------- vectors */

static void test_vectors(Fixture *f)
{
    /* RFC 1057 8's call, by hand: a NULL to NFS version 2 with AUTH_NULL.
     * The reply is the accepted header and nothing else. */
    static const uint8_t null_call[] = {
        0, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 2,  0, 1, 0x86, 0xA3,  0, 0, 0, 2,  0, 0, 0, 0,
        0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    };
    static const uint8_t null_reply[] = {
        0, 0, 0, 1,  0, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    };
    ssize_t n = cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, null_call, sizeof(null_call), reply,
                              sizeof(reply));
    CHECK(n == sizeof(null_reply) && !memcmp(reply, null_reply, sizeof(null_reply)),
          "NULL answered with %zd bytes", n);

    /* The credential a CDJ sends (rbxport, rbl-nfs rpc.rs and its fake
     * CDJ): AUTH_UNIX, stamp 967b8703, no machine name, uid 0, gid 0, no
     * groups; then a null verifier. */
    static const uint8_t cred[] = {
        0, 0, 0, 1,  0, 0, 0, 20,  0x96, 0x7B, 0x87, 0x03,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    };
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_NULL);
    CHECK(c.w.len == 24 + sizeof(cred) && !memcmp(c.b + 24, cred, sizeof(cred)),
          "the call's credential is a CDJ's");
    n = ask(f->nfs, CB_NFS_NFS, PLAYER, &c);
    CBRd r;
    CHECK(results(n, &c, &r) == 0 && n == 24, "a CDJ's NULL: %zd", n);

    /* Names are UTF-16LE with a byte length: "PIONEER" declares fourteen
     * (rbxport's XDR tests). */
    static const uint8_t pioneer[] = {
        0, 0, 0, 14,  'P', 0, 'I', 0, 'O', 0, 'N', 0, 'E', 0, 'E', 0, 'R', 0, 0, 0,
    };
    uint8_t b[64];
    CBWr w = cb_wr(b, sizeof(b));
    cb_nfs_put_name(&w, "PIONEER");
    CHECK(w.len == sizeof(pioneer) && !memcmp(b, pioneer, sizeof(pioneer)), "PIONEER in UTF-16LE");
    char name[CB_NFS_NAME_MAX * 3 + 1];
    r = cb_rd(b, w.len);
    CHECK(cb_nfs_get_name(&r, CB_NFS_NAME_MAX, name, sizeof(name)) == 7 && !strcmp(name, "PIONEER"),
          "PIONEER read back");
    /* A name above the BMP is a surrogate pair. */
    w = cb_wr(b, sizeof(b));
    cb_nfs_put_name(&w, "\U0001F3B5");
    static const uint8_t pair[] = { 0, 0, 0, 4,  0x3C, 0xD8, 0xB5, 0xDF };
    CHECK(w.len == sizeof(pair) && !memcmp(b, pair, sizeof(pair)), "U+1F3B5 as a pair");

    /* GETPORT, the exact bytes for every program, version and protocol
     * (rbxport's getport_matches_program_service_version_and_transport_
     * exactly, rekordbox's matching). */
    const uint32_t progs[][3] = {
        { CB_RPC_NFS, CB_RPC_NFS_VERSION, 12049 }, { CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, 12005 },
        { CB_RPC_PORTMAP, CB_RPC_PORTMAP_VERSION, 0 }, { 100024, 1, 0 },
    };
    const uint32_t versions[] = { 0, 1, 2, 3, UINT32_MAX }, protos[] = { 17, 6, 0 };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(progs); i++) {
        for (size_t v = 0; v < CB_ARRAY_ELEMS(versions); v++) {
            for (size_t p = 0; p < CB_ARRAY_ELEMS(protos); p++) {
                call(&c, CB_RPC_PORTMAP, CB_RPC_PORTMAP_VERSION, CB_PMAP_GETPORT);
                cb_wr_u32(&c.w, progs[i][0]);
                cb_wr_u32(&c.w, versions[v]);
                cb_wr_u32(&c.w, protos[p]);
                cb_wr_u32(&c.w, 0);
                n = ask(f->nfs, CB_NFS_PORTMAP, PLAYER, &c);
                uint32_t port = versions[v] == progs[i][1] && protos[p] == 17 ? progs[i][2] : 0;
                const uint32_t want[7] = { c.xid, 1, 0, 0, 0, 0, port };
                uint8_t exact[28];
                CBWr e = cb_wr(exact, sizeof(exact));
                for (int k = 0; k < 7; k++)
                    cb_wr_u32(&e, want[k]);
                CHECK(n == 28 && !memcmp(reply, exact, 28), "GETPORT %u v%u proto %u: %zd bytes",
                      progs[i][0], versions[v], protos[p], n);
            }
        }
    }

    /* DUMP: the three mappings, portmap's own included. */
    call(&c, CB_RPC_PORTMAP, CB_RPC_PORTMAP_VERSION, CB_PMAP_DUMP);
    n = ask(f->nfs, CB_NFS_PORTMAP, PLAYER, &c);
    CHECK(results(n, &c, &r) == 0, "DUMP");
    int maps = 0;
    bool nfs = false, mount = false;
    while (cb_rd_u32(&r) == 1 && maps < 8) {
        uint32_t prog = cb_rd_u32(&r), vers = cb_rd_u32(&r), proto = cb_rd_u32(&r);
        uint32_t port = cb_rd_u32(&r);
        nfs |= prog == CB_RPC_NFS && vers == 2 && proto == 17 && port == 12049;
        mount |= prog == CB_RPC_MOUNT && vers == 1 && proto == 17 && port == 12005;
        maps++;
    }
    CHECK(maps == 3 && nfs && mount && !r.err && !cb_rd_left(&r), "DUMP lists %d", maps);

    /* EXPORT: "/" offered to rekordbox's own subnet, "a.b.c.d/m.m.m.m" in
     * ASCII (rbxport's each_export_offers_the_host_group_a_cdj_checks_
     * itself_against; measured on the wire 2026-09-13). */
    static const uint8_t exports[] = {
        0, 0, 0, 1,  0, 0, 0, 2,  '/', 0, 0, 0,  0, 0, 0, 1,  0, 0, 0, 26,
        '1', '9', '2', '.', '1', '6', '8', '.', '1', '.', '1', '4', '/',
        '2', '5', '5', '.', '2', '5', '5', '.', '2', '5', '5', '.', '0', 0, 0,
        0, 0, 0, 0,  0, 0, 0, 0,
    };
    call(&c, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_EXPORT);
    n = ask(f->nfs, CB_NFS_MOUNT, PLAYER, &c);
    CHECK(results(n, &c, &r) == 0 && cb_rd_left(&r) == sizeof(exports) &&
          !memcmp(r.p + r.pos, exports, sizeof(exports)), "EXPORT names the group");

    /* Garbage gets nothing (rbxport's garbage_is_dropped_rather_than_
     * answered), nor does a reply, ours included. */
    uint8_t junk[40];
    memset(junk, 0xFF, sizeof(junk));
    CHECK(cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, junk, 0, reply, sizeof(reply)) == 0, "empty");
    CHECK(cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, (const uint8_t[3]) { 0 }, 3, reply,
                        sizeof(reply)) == 0, "three zeros");
    CHECK(cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, junk, sizeof(junk), reply, sizeof(reply)) == 0,
          "forty 0xff");
    uint8_t ours[24];
    memcpy(ours, null_reply, sizeof(ours));
    CHECK(cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, ours, sizeof(ours), reply, sizeof(reply)) == 0,
          "a reply");

    /* The handles rekordbox gave an XDJ-700, with the player's own tail
     * (rbxport, xdj700_linux_rbxport.pcap and xdj700_windows_rbxport.pcap
     * on its issue 43): another server's, so stale here. */
    static const char *const xdj700[] = {
        "0000000100000001000000010301000000001b58000000001104010002a33812",
        "0000000100000001000000010301000000001b58000000001104010004450197",
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(xdj700); i++) {
        uint8_t fh[CB_NFS_FHSIZE];
        from_hex(xdj700[i], fh, sizeof(fh));
        CHECK(getattr(f->nfs, fh, NULL) == -ESTALE, "rekordbox's handle %zu", i);
        CHECK(lookup(f->nfs, fh, "Contents", NULL, NULL) == -ESTALE, "LOOKUP under it %zu", i);
    }
}

/* ------------------------------------------------------------------- tree */

static void test_tree(Fixture *f)
{
    CBVTreeStats s;
    cb_vtree_stats(f->tree, &s);
    /* Ten directories: the root, Contents, Björk/Homogenic, ABBA/Gold,
     * Abba/Gold, UnknownArtist/UnknownAlbum. */
    CHECK(s.nb_files == NB_SERVED && s.nb_dirs == 10, "%d files, %d directories", s.nb_files,
          s.nb_dirs);
    CHECK(s.total_size == 100000 + 65543 + 32768 + 5, "total %llu",
          (unsigned long long)s.total_size);

    for (int i = 0; i < NB_SERVED; i++) {
        int node = cb_vtree_walk(f->tree, specs[i].usb);
        const CBPlanTrack *pt = cb_vtree_track(f->tree, node);
        CHECK(pt && pt->id == specs[i].id && cb_vtree_find_file(f->tree, pt->id) == node,
              "%s", specs[i].usb);
    }
    /* Exact first, then ignoring ASCII case: "ABBA" and "Abba" are two
     * directories, each found by its own name; "abba" finds the first. */
    CHECK(cb_vtree_walk(f->tree, "/Contents/ABBA/Gold/01 Dancing Queen.mp3") >= 0, "ABBA");
    CHECK(cb_vtree_walk(f->tree, "/Contents/Abba/Gold/02 Waterloo.mp3") >= 0, "Abba");
    CHECK(cb_vtree_walk(f->tree, "/Contents/ABBA/Gold/02 Waterloo.mp3") < 0, "not in ABBA");
    CHECK(cb_vtree_walk(f->tree, "/contents/abba/GOLD/01 dancing queen.MP3") ==
          cb_vtree_walk(f->tree, "/Contents/ABBA/Gold/01 Dancing Queen.mp3"), "folded");
    CHECK(cb_vtree_walk(f->tree, "/CONTENTS/björk/homogenic/01 jóga.flac") ==
          cb_vtree_walk(f->tree, specs[0].usb), "folded around UTF-8");
    /* Only ASCII folds: Ö is not ö. */
    CHECK(cb_vtree_walk(f->tree, "/Contents/BJÖRK") < 0, "BJÖRK");

    /* What the tree refused is not in it. */
    const char *const refused[] = {
        "/etc/passwd", "/etc", "/Contents/Dup", "/Contents/relative.mp3", "/relative.mp3",
        "/Contents/empty.mp3", "/Contents/dot.mp3", "/Contents/Trailing",
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(refused); i++)
        CHECK(cb_vtree_walk(f->tree, refused[i]) < 0, "%s is not in the tree", refused[i]);
    CHECK(cb_vtree_find_file(f->tree, 106) < 0 && cb_vtree_find_file(f->tree, 108) < 0 &&
          cb_vtree_find_file(f->tree, 112) < 0, "refused tracks have no file");
    CHECK(!strcmp(cb_vtree_name(f->tree, cb_vtree_find_file(f->tree, 101)), "01 Jóga.flac"),
          "101 is the first of its id");

    /* Ten thousand tracks: built in well under a second, every path found. */
    int nb = 10000;
    CBPlanTrack *tracks = calloc((size_t)nb, sizeof(*tracks));
    if (!tracks)
        abort();
    for (int i = 0; i < nb; i++) {
        tracks[i] = (CBPlanTrack) {
            .id        = (uint32_t)i + 1,
            .usb_path  = cb_asprintf("/Contents/Artist %d/Album %d/%02d Track %d.flac", i / 40,
                                     i / 10, i % 10, i),
            .source    = cb_strdup("/nonexistent"),
            .file_size = 1000,
        };
    }
    CBPlan plan = { .tracks = tracks, .nb_tracks = nb };
    CBVTree *big = NULL;
    int64_t t0 = cb_time_us();
    int ret = cb_vtree_new(&big, &plan);
    int64_t t1 = cb_time_us();
    cb_vtree_stats(big, &s);
    CHECK(ret == 0 && s.nb_files == nb && s.nb_dirs == 1 + 1 + 250 + 1000, "10k: %d files %d dirs",
          s.nb_files, s.nb_dirs);
    CHECK(t1 - t0 < 1000000, "10k tracks built in %lld us", (long long)(t1 - t0));
    int found = 0;
    for (int i = 0; i < nb; i++) {
        const CBPlanTrack *pt = cb_vtree_track(big, cb_vtree_walk(big, tracks[i].usb_path));
        found += pt && pt->id == tracks[i].id;
    }
    CHECK(found == nb, "%d of 10k found", found);
    printf("10k tracks: tree built in %.1f ms, %d paths walked in %.1f ms\n", (t1 - t0) / 1000.0,
           nb, (cb_time_us() - t1) / 1000.0);
    cb_vtree_free(&big);
    for (int i = 0; i < nb; i++) {
        free(tracks[i].usb_path);
        free(tracks[i].source);
    }
    free(tracks);
}

/* ------------------------------------------------------------------- read */

static void test_read(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    CHECK(cb_nfs_mounted(f->nfs, PLAYER), "the player is mounted");

    /* Every file whole, in a CDJ-3000's 32 kB, NFS version 2's 8 kB, an
     * odd size, and rekordbox's cap. */
    const uint32_t chunks[] = { 32768, 8192, 1000, CB_NFS_MAX_READ };
    for (int i = 0; i < NB_SERVED; i++) {
        CBNfsAttr a;
        int ret = walk(f->nfs, root, specs[i].usb, fh, &a);
        CHECK(ret == 0 && a.type == 1 && a.size == specs[i].size, "%s: %d, size %u", specs[i].usb,
              ret, a.size);
        for (size_t k = 0; k < CB_ARRAY_ELEMS(chunks); k++) {
            uint8_t *d = NULL;
            size_t n = 0;
            ret = read_whole(f->nfs, fh, chunks[k], &d, &n);
            CHECK(ret == 0 && same_as_file(track(f, specs[i].id)->source, d, n),
                  "%s in %u: %d, %zu bytes", specs[i].usb, chunks[k], ret, n);
            free(d);
        }
    }

    CHECK(walk(f->nfs, root, specs[0].usb, fh, NULL) == 0, "a.bin");
    uint8_t *want;
    size_t size;
    if (cb_file_read(track(f, 101)->source, &want, &size) < 0)
        abort();
    uint8_t *buf = malloc(CB_NFS_MAX_READ);
    if (!buf)
        abort();
    /* However much is asked, rekordbox's cap at most (rbxport's a_read_is_
     * capped_at_the_protocol_limit_however_much_is_asked_for). */
    int got = nfs_read(f->nfs, PLAYER, fh, 0, UINT32_MAX, buf, CB_NFS_MAX_READ, NULL);
    CHECK(got == CB_NFS_MAX_READ && !memcmp(buf, want, (size_t)got), "capped: %d", got);
    /* A CDJ's 32 kB whole before the end, at 0 and elsewhere (rbxport's
     * cdj_32_kib_reads_are_complete_before_the_end_of_the_file). */
    got = nfs_read(f->nfs, PLAYER, fh, 4096, 32768, buf, CB_NFS_MAX_READ, NULL);
    CHECK(got == 32768 && !memcmp(buf, want + 4096, 32768), "32 kB at 4096: %d", got);
    got = nfs_read(f->nfs, PLAYER, fh, 32760, 16, buf, CB_NFS_MAX_READ, NULL);
    CHECK(got == 16 && !memcmp(buf, want + 32760, 16), "across a 32 kB boundary: %d", got);
    got = nfs_read(f->nfs, PLAYER, fh, (uint32_t)size - 3, 32768, buf, CB_NFS_MAX_READ, NULL);
    CHECK(got == 3 && !memcmp(buf, want + size - 3, 3), "the last three: %d", got);
    /* At and past the end, and of nothing: NFSERR_IO, as rekordbox
     * (rbxport's read_edge tests, libFilSiNE's _tkfFSReadFile). */
    const uint32_t eof[][2] = { { (uint32_t)size, 1 }, { (uint32_t)size, 32768 },
                                { (uint32_t)size + 1, 4096 }, { UINT32_MAX, 4096 }, { 0, 0 },
                                { 2, 0 } };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(eof); i++) {
        Call c;
        call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_READ);
        cb_nfs_put_handle(&c.w, fh);
        cb_wr_u32(&c.w, eof[i][0]);
        cb_wr_u32(&c.w, eof[i][1]);
        cb_wr_u32(&c.w, 0);
        CBRd r;
        ssize_t n = ask(f->nfs, CB_NFS_NFS, PLAYER, &c);
        CHECK(status_of(n, &c, &r) == -EIO && n == 28, "READ %u+%u: %zd bytes", eof[i][0],
              eof[i][1], n);
    }
    free(want);

    /* An empty file is IO from the start. */
    CHECK(walk(f->nfs, root, specs[4].usb, fh, NULL) == 0, "e.bin");
    CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 4096, buf, CB_NFS_MAX_READ, NULL) == -EIO, "empty");

    /* A short read's whole reply: the attributes, three bytes, a zero to
     * pad (rbxport's read_edge_positive_short_read_keeps_attributes_
     * length_data_and_padding). */
    CHECK(walk(f->nfs, root, specs[3].usb, fh, NULL) == 0, "d.bin");
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_READ);
    cb_nfs_put_handle(&c.w, fh);
    cb_wr_u32(&c.w, 2);
    cb_wr_u32(&c.w, 4096);
    cb_wr_u32(&c.w, 0);
    ssize_t n = ask(f->nfs, CB_NFS_NFS, PLAYER, &c);
    const uint8_t d3[3] = { pattern(4, 2), pattern(4, 3), pattern(4, 4) };
    CHECK(n == 24 + 4 + 68 + 4 + 4 && reply[24 + 3] == 0 && reply[24 + 4 + 68 + 3] == 3 &&
          !memcmp(reply + 100, d3, 3) && reply[103] == 0, "short read: %zd bytes", n);

    /* A directory is not read. */
    CHECK(nfs_read(f->nfs, PLAYER, root, 0, 4096, buf, CB_NFS_MAX_READ, NULL) == -EISDIR, "root");
    free(buf);
}

/* ---------------------------------------------------------------- listing */

static void test_readdir(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    List l;
    CHECK(list(f->nfs, PLAYER, root, 8192, &l) == 0 && same_list(&l, (const char *[]) {
          "Contents" }, 1), "the root lists Contents alone");
    list_free(&l);

    /* By name ignoring ASCII case, then exactly. */
    CHECK(lookup(f->nfs, root, "Contents", fh, NULL) == 0, "Contents");
    const char *const contents[] = { "ABBA", "Abba", "Björk", "UnknownArtist" };
    CHECK(list(f->nfs, PLAYER, fh, 8192, &l) == 0 && same_list(&l, contents, 4), "Contents: %d",
          l.nb);
    for (int i = 0; i < l.nb; i++)
        CHECK(l.fileids[i] & 0x80000000u, "a directory's file id");
    list_free(&l);

    CHECK(walk(f->nfs, root, "/Contents/UnknownArtist/UnknownAlbum", fh, NULL) == 0, "album");
    CHECK(list(f->nfs, PLAYER, fh, 8192, &l) == 0 && same_list(&l, (const char *[]) {
          "\U0001F3B5 Song.wav" }, 1) && l.fileids[0] == 105, "a name above the BMP");
    list_free(&l);

    /* A file is not listed; a cookie past the end is the end. */
    CHECK(walk(f->nfs, root, specs[0].usb, fh, NULL) == 0, "a.bin");
    CHECK(list(f->nfs, PLAYER, fh, 8192, &l) == -ENOTDIR, "a file");
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_READDIR);
    cb_nfs_put_handle(&c.w, root);
    cb_wr_u32(&c.w, 999);
    cb_wr_u32(&c.w, 8192);
    CBRd r;
    bool eof = false;
    CHECK(status_of(ask(f->nfs, CB_NFS_NFS, PLAYER, &c), &c, &r) == 0 &&
          cb_nfs_get_entries(&r, NULL, NULL, &eof) == 0 && eof, "past the end");

    /* Three hundred in one directory: in pages of 1 kB (the smallest
     * answer) as in one, whole, in order, each once. */
    CBPlanTrack tracks[300];
    for (int i = 0; i < 300; i++) {
        tracks[i] = (CBPlanTrack) {
            .id       = (uint32_t)i + 1,
            .usb_path = cb_asprintf("/Contents/Many/Album/track %03d with a long name.flac", i),
            .source   = cb_strdup("/nonexistent"),
        };
    }
    CBPlan plan = { .tracks = tracks, .nb_tracks = 300 };
    CBVTree *t = NULL;
    CHECK(cb_vtree_new(&t, &plan) == 0, "300");
    cb_nfs_set_tree(f->nfs, t);
    CHECK(walk(f->nfs, root, "/Contents/Many/Album", fh, NULL) == 0, "Many/Album");
    List small, big;
    CHECK(list(f->nfs, PLAYER, fh, 0, &small) == 0 && small.nb == 300 && small.calls > 5,
          "small pages: %d in %d", small.nb, small.calls);
    CHECK(list(f->nfs, PLAYER, fh, 8192, &big) == 0 && big.nb == 300, "8 kB pages");
    bool same = small.nb == big.nb;
    for (int i = 0; i < small.nb && same; i++) {
        char want[64];
        snprintf(want, sizeof(want), "track %03d with a long name.flac", i);
        same = !strcmp(small.names[i], want) && !strcmp(big.names[i], want) &&
               small.fileids[i] == (uint32_t)i + 1;
    }
    CHECK(same, "pages agree, in order");
    list_free(&small);
    list_free(&big);
    cb_nfs_set_tree(f->nfs, f->tree);
    cb_vtree_free(&t);
    for (int i = 0; i < 300; i++) {
        free(tracks[i].usb_path);
        free(tracks[i].source);
    }
}

/* ------------------------------------------------------------- attributes */

static void test_attrs(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    CBNfsAttr a;
    CHECK(walk(f->nfs, root, specs[1].usb, fh, NULL) == 0 && getattr(f->nfs, fh, &a) == 0, "b");
    struct stat st;
    stat(track(f, 102)->source, &st);
    /* A read-only regular file of the plan's size; fsid 2 as rekordbox
     * (rbxport's attributes_describe_a_read_only_tree). */
    CHECK(a.type == 1 && a.mode == 0100444 && a.nlink == 1 && a.uid == 0 && a.gid == 0 &&
          a.size == 65543 && a.blocksize == 4096 && a.rdev == 0 && a.blocks == (65543 + 511) / 512 &&
          a.fsid == 2 && a.fileid == 102 && a.mtime == (uint32_t)st.st_mtime,
          "file: type %u mode %o size %u fileid %u", a.type, a.mode, a.size, a.fileid);
    /* A directory as rekordbox's export root showed a CDJ: 041ed, two
     * links, 64 bytes. */
    CHECK(getattr(f->nfs, root, &a) == 0 && a.type == 2 && a.mode == 040755 && a.nlink == 2 &&
          a.size == 64 && a.fsid == 2 && (a.fileid & 0x80000000u),
          "root: type %u mode %o size %u", a.type, a.mode, a.size);
    CBNfsAttr la;
    CHECK(lookup(f->nfs, root, "Contents", fh, &la) == 0 && la.type == 2 &&
          la.fileid != a.fileid, "Contents");

    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_STATFS);
    cb_nfs_put_handle(&c.w, root);
    CBRd r;
    ssize_t n = ask(f->nfs, CB_NFS_NFS, PLAYER, &c);
    CHECK(status_of(n, &c, &r) == 0, "STATFS");
    uint32_t v[5];
    for (int i = 0; i < 5; i++)
        v[i] = cb_rd_u32(&r);
    CHECK(!r.err && !cb_rd_left(&r) && v[0] == 32768 && v[1] == 4096 &&
          v[2] == (100000 + 65543 + 32768 + 5 + 4095) / 4096 && v[3] == 0 && v[4] == 0,
          "STATFS: %u %u %u %u %u", v[0], v[1], v[2], v[3], v[4]);
}

/* -------------------------------------------------------------- traversal */

static void test_traversal(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE], contents[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    CHECK(lookup(f->nfs, root, "Contents", contents, NULL) == 0, "Contents");

    /* No name is a path, and nothing but the tree's names is found:
     * rbxport's a_name_containing_a_separator_matches_nothing and more. */
    const char *const names[] = {
        "..", ".", "/etc/passwd", "../../etc/passwd", "etc", "Contents/Björk", "Contents\\Björk",
        "/", "//", "~", "proc", "home", "tmp", "passwd", "Dup",
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(names); i++) {
        CHECK(lookup(f->nfs, root, names[i], fh, NULL) == -ENOENT, "root/%s", names[i]);
        CHECK(lookup(f->nfs, contents, names[i], fh, NULL) == -ENOENT, "Contents/%s", names[i]);
    }
    /* The empty name, a NUL inside or at the end, an odd length, lone
     * surrogates either way round: not names. */
    CHECK(lookup_raw(f->nfs, root, NULL, 0, 0) == -ENOENT, "empty");
    static const uint8_t nul_in[] = { 'C', 0, 'o', 0, 0, 0, 'x', 0 };
    static const uint8_t nul_end[] = { 'C', 0, 'o', 0, 'n', 0, 't', 0, 'e', 0, 'n', 0, 't', 0,
                                       's', 0, 0, 0 };
    static const uint8_t odd[] = { 'C', 0, 'o' };
    static const uint8_t high[] = { 'a', 0, 0x00, 0xD8 };
    static const uint8_t low[] = { 0x00, 0xDC, 'a', 0 };
    static const uint8_t swapped[] = { 0x00, 0xDC, 0x00, 0xD8 };
    const struct { const uint8_t *p; size_t n; } bad[] = {
        { nul_in, sizeof(nul_in) }, { nul_end, sizeof(nul_end) }, { odd, sizeof(odd) },
        { high, sizeof(high) }, { low, sizeof(low) }, { swapped, sizeof(swapped) },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(bad); i++)
        CHECK(lookup_raw(f->nfs, root, bad[i].p, bad[i].n, (uint32_t)bad[i].n) == -ENOENT,
              "bad name %zu", i);
    /* "Contents" sent in ASCII, as a stock client would: not UTF-16. */
    CHECK(lookup_raw(f->nfs, root, (const uint8_t *)"Contents", 8, 8) == -ENOENT, "ASCII");
    /* 255 units is a name; 256 is too long. */
    uint8_t longname[2 * 256];
    for (int i = 0; i < 256; i++) {
        longname[2 * i] = 'a';
        longname[2 * i + 1] = 0;
    }
    CHECK(lookup_raw(f->nfs, root, longname, 510, 510) == -ENOENT, "255 units");
    CHECK(lookup_raw(f->nfs, root, longname, 512, 512) == -ENAMETOOLONG, "256 units");
    /* A length past what arrived is garbage, not a name. */
    CHECK(lookup_raw(f->nfs, root, longname, 8, 0xFFFFFFF0u) == -EINVAL, "absurd length");
    CHECK(lookup_raw(f->nfs, root, longname, 8, 12) == -EINVAL, "short by four");

    /* A file has no names under it. */
    CHECK(walk(f->nfs, root, specs[0].usb, fh, NULL) == 0, "a.bin");
    CHECK(lookup(f->nfs, fh, "x", NULL, NULL) == -ENOTDIR, "under a file");

    /* Host paths, walked: nothing past the first name. */
    const char *const paths[] = {
        "/etc/passwd", "/proc/self/environ", "/home", "/tmp", "/Contents/../etc/passwd",
        "/Contents/Dup/x.mp3", "/Contents/Björk/../../etc/passwd",
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(paths); i++)
        CHECK(walk(f->nfs, root, paths[i], fh, NULL) == -ENOENT, "%s", paths[i]);

    /* Only "/" mounts, UTF-16LE (rbxport's mounting_an_export_that_does_
     * not_exist_says_so: ACCES, as rekordbox). */
    const char *const mounts[] = { "/etc", "/..", "", "/Contents", "/C/", "//", "/.", "/home" };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(mounts); i++)
        CHECK(mnt(f->nfs, PLAYER, mounts[i], fh) == -EACCES, "mount %s", mounts[i]);
    Call c;
    call(&c, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_MNT);
    cb_wr_u32(&c.w, 1);
    cb_wr_bytes(&c.w, "/\0\0\0", 4);
    CBRd r;
    CHECK(status_of(ask(f->nfs, CB_NFS_MOUNT, PLAYER, &c), &c, &r) == -EACCES, "ASCII /");
    call(&c, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_MNT);
    cb_wr_u32(&c.w, 2000);
    cb_wr_zeros(&c.w, 2000);
    CHECK(status_of(ask(f->nfs, CB_NFS_MOUNT, PLAYER, &c), &c, &r) == -EINVAL,
          "a path past MNTPATHLEN");
}

/* ---------------------------------------------------------------- handles */

static void test_handles(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE], again[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    static const uint8_t zeros[20];
    CHECK(!memcmp(root + 12, zeros, 20), "bytes 12 to 31 are zero");

    /* The XDJ-700 puts its own twenty bytes after the first twelve in
     * every LOOKUP (rbxport, xdj700_windows_rekordbox.pcap on its issue
     * 43, xids 1018-1024): read as ours all the same, the same node. */
    uint8_t tail[20];
    from_hex("0301000000001b5800000000110401000cce428f", tail, sizeof(tail));
    uint8_t at[CB_NFS_FHSIZE];
    memcpy(at, root, sizeof(at));
    const char *const parts[] = { "Contents", "Björk", "Homogenic", "01 Jóga.flac" };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(parts); i++) {
        uint8_t sent[CB_NFS_FHSIZE];
        memcpy(sent, at, 12);
        memcpy(sent + 12, tail, 20);
        CHECK(lookup(f->nfs, sent, parts[i], fh, NULL) == 0 && lookup(f->nfs, at, parts[i], again,
              NULL) == 0 && !memcmp(fh, again, sizeof(fh)), "XDJ-700 LOOKUP %s", parts[i]);
        memcpy(at, fh, sizeof(at));
    }
    memcpy(at + 12, tail, 20);
    uint8_t *data = NULL;
    size_t size = 0;
    CHECK(read_whole(f->nfs, at, 32768, &data, &size) == 0 &&
          same_as_file(track(f, 101)->source, data, size), "XDJ-700 reads");
    free(data);

    /* Forged: random, another tag, a kind that is neither, an id that is
     * not there, a directory id that is not one. */
    uint8_t forged[6][CB_NFS_FHSIZE];
    memset(forged[0], 0xAB, CB_NFS_FHSIZE);
    memcpy(forged[1], fh, CB_NFS_FHSIZE);
    forged[1][3] ^= 1;
    memcpy(forged[2], fh, CB_NFS_FHSIZE);
    forged[2][7] = 1;
    memcpy(forged[3], fh, CB_NFS_FHSIZE);
    forged[3][11] = 0x77;
    memcpy(forged[4], root, CB_NFS_FHSIZE);
    forged[4][11] ^= 0x10;
    memset(forged[5], 0, CB_NFS_FHSIZE);
    for (int i = 0; i < 6; i++) {
        CHECK(getattr(f->nfs, forged[i], NULL) == -ESTALE, "forged %d", i);
        uint8_t buf[16];
        CHECK(nfs_read(f->nfs, PLAYER, forged[i], 0, 16, buf, sizeof(buf), NULL) == -ESTALE,
              "READ forged %d", i);
    }

    /* Another session's handles are stale here, and ours there. */
    CBNfs *other = NULL;
    CBNfsConfig cfg = { .mount_port = 1, .nfs_port = 2 };
    CHECK(cb_nfs_new(&other, &cfg) == 0, "another session");
    cb_nfs_set_tree(other, f->tree);
    uint8_t oroot[CB_NFS_FHSIZE];
    CHECK(mnt(other, PLAYER, "/", oroot) == 0 && memcmp(oroot, root, 4), "its own tag");
    CHECK(getattr(f->nfs, oroot, NULL) == -ESTALE && getattr(other, root, NULL) == -ESTALE &&
          getattr(other, fh, NULL) == -ESTALE, "stale across sessions");
    CHECK(getattr(other, oroot, NULL) == 0, "its own handle");
    cb_nfs_free(&other);

    /* A session the owner keeps, given to one server and then the next,
     * as a set gives it to each service on an interface: the next takes
     * the first one's handles, and the deck that mounted reads on with
     * them, never mounting again; a stranger with them does not, and a
     * server of its own finds them stale. */
    CBNfsSession *kept = calloc(1, sizeof(*kept));
    CBNfsConfig kc = {
        .mount_port = 1, .nfs_port = 2, .addr = HOST, .netmask = MASK, .session = kept,
    };
    CBNfs *before = NULL, *after = NULL, *own = NULL;
    uint8_t kroot[CB_NFS_FHSIZE], kfh[CB_NFS_FHSIZE], kbuf[16];
    CHECK(kept && cb_nfs_new(&before, &kc) == 0, "a kept session");
    cb_nfs_set_tree(before, f->tree);
    CHECK(mnt(before, PLAYER, "/", kroot) == 0 && walk(before, kroot, specs[0].usb, kfh, NULL) == 0,
          "mounted before");
    cb_nfs_free(&before);
    CHECK(kept->tag && kept->nb_ever == 1 && kept->ever[0] == PLAYER, "kept: the tag, the mount");
    CHECK(cb_nfs_new(&after, &kc) == 0, "the next server");
    cb_nfs_set_tree(after, f->tree);
    CHECK(!cb_nfs_mounted(after, PLAYER), "not mounted there");
    CHECK(nfs_read(after, STRANGER, kfh, 0, 16, kbuf, sizeof(kbuf), NULL) == -EACCES,
          "a stranger with them");
    CHECK(nfs_read(after, PLAYER, kfh, 0, 16, kbuf, sizeof(kbuf), NULL) == 16 &&
          cb_nfs_mounted(after, PLAYER), "the deck reads on");
    cb_nfs_free(&after);
    kc.session = NULL;
    CHECK(cb_nfs_new(&own, &kc) == 0, "a server of its own");
    cb_nfs_set_tree(own, f->tree);
    CHECK(getattr(own, kroot, NULL) == -ESTALE && getattr(own, kfh, NULL) == -ESTALE &&
          nfs_read(own, PLAYER, kfh, 0, 16, kbuf, sizeof(kbuf), NULL) == -EACCES,
          "stale without the session, and no read");
    cb_nfs_free(&own);
    free(kept);

    /* Short of a handle: garbage. */
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_GETATTR);
    cb_wr_bytes(&c.w, root, 31);
    CBRd r;
    CHECK(results(ask(f->nfs, CB_NFS_NFS, PLAYER, &c), &c, &r) == -EINVAL, "31 bytes");
}

/* ----------------------------------------------------------------- access */

static void test_access(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE], buf[64];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    CHECK(walk(f->nfs, root, specs[0].usb, fh, NULL) == 0, "a.bin");

    /* A peer that did not mount may look, not read or list; the answer is
     * the status alone. */
    CHECK(!cb_nfs_mounted(f->nfs, STRANGER), "a stranger");
    Call c;
    call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_READ);
    cb_nfs_put_handle(&c.w, fh);
    cb_wr_u32(&c.w, 0);
    cb_wr_u32(&c.w, 32768);
    cb_wr_u32(&c.w, 0);
    CBRd r;
    ssize_t n = ask(f->nfs, CB_NFS_NFS, STRANGER, &c);
    CHECK(status_of(n, &c, &r) == -EACCES && n == 28, "READ by a stranger: %zd", n);
    List l;
    CHECK(list(f->nfs, STRANGER, root, 8192, &l) == -EACCES, "READDIR by a stranger");
    CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 16, buf, sizeof(buf), NULL) == 16, "the player reads");

    /* Outside the subnet: no mount, so no reads. */
    CHECK(mnt(f->nfs, OUTSIDER, "/", buf) == -EACCES && !cb_nfs_mounted(f->nfs, OUTSIDER),
          "outside the subnet");
    CHECK(nfs_read(f->nfs, OUTSIDER, fh, 0, 16, buf, sizeof(buf), NULL) == -EACCES, "outsider");

    /* UMNT is answered and changes nothing (an XDJ-AZ's two decks share
     * an address); a peer forgotten (a deck back from a lapse of its
     * keep-alives) that reads with a handle of ours is taken back in, as
     * it never mounts again; a stranger with one is not. */
    call(&c, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_UMNT);
    cb_nfs_put_name(&c.w, "/");
    n = ask(f->nfs, CB_NFS_MOUNT, PLAYER, &c);
    CHECK(results(n, &c, &r) == 0 && n == 24, "UMNT");
    CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 16, buf, sizeof(buf), NULL) == 16, "after UMNT");
    cb_nfs_forget(f->nfs, PLAYER);
    CHECK(!cb_nfs_mounted(f->nfs, PLAYER), "forgotten");
    CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 16, buf, sizeof(buf), NULL) == 16 &&
          cb_nfs_mounted(f->nfs, PLAYER), "taken back in by its handle");
    CHECK(nfs_read(f->nfs, STRANGER, fh, 0, 16, buf, sizeof(buf), NULL) == -EACCES &&
          !cb_nfs_mounted(f->nfs, STRANGER), "a stranger with the handle");
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount again");
    CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 16, buf, sizeof(buf), NULL) == 16, "reads again");

    /* The table is small: forty others mounting push out the oldest, not
     * a player reading meanwhile. */
    for (uint32_t i = 0; i < 40; i++) {
        CHECK(mnt(f->nfs, 0xC0A80140u + i, "/", buf) == 0, "peer %u", i);
        CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 16, buf, sizeof(buf), NULL) == 16, "reading %u", i);
    }
    CHECK(!cb_nfs_mounted(f->nfs, 0xC0A80140u) && cb_nfs_mounted(f->nfs, 0xC0A80140u + 39) &&
          cb_nfs_mounted(f->nfs, PLAYER), "the oldest forgotten");

    /* No tree: nothing exported, nothing mounted, every handle stale; the
     * same tree again and the handles are good again. */
    cb_nfs_set_tree(f->nfs, NULL);
    call(&c, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_EXPORT);
    n = ask(f->nfs, CB_NFS_MOUNT, PLAYER, &c);
    CHECK(results(n, &c, &r) == 0 && cb_rd_left(&r) == 4 && cb_rd_u32(&r) == 0, "no exports");
    CHECK(mnt(f->nfs, PLAYER, "/", buf) == -EACCES, "no mount");
    CHECK(getattr(f->nfs, root, NULL) == -ESTALE && getattr(f->nfs, fh, NULL) == -ESTALE,
          "stale without a tree");
    cb_nfs_set_tree(f->nfs, f->tree);
    CHECK(getattr(f->nfs, root, NULL) == 0 && getattr(f->nfs, fh, NULL) == 0, "good again");
}

/* ----------------------------------------------------------------- writes */

static void test_writes(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    const uint32_t writes[] = {
        CB_NFSPROC_SETATTR, CB_NFSPROC_WRITE, CB_NFSPROC_CREATE, CB_NFSPROC_REMOVE,
        CB_NFSPROC_RENAME, CB_NFSPROC_LINK, CB_NFSPROC_SYMLINK, CB_NFSPROC_MKDIR, CB_NFSPROC_RMDIR,
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(writes); i++) {
        for (int args = 0; args < 2; args++) {
            Call c;
            call(&c, CB_RPC_NFS, CB_RPC_NFS_VERSION, writes[i]);
            if (args) {
                cb_nfs_put_handle(&c.w, root);
                cb_nfs_put_name(&c.w, "Contents");
                cb_wr_zeros(&c.w, 64);
            }
            CBRd r;
            ssize_t n = ask(f->nfs, CB_NFS_NFS, PLAYER, &c);
            CHECK(status_of(n, &c, &r) == -EROFS && n == 28, "write %u: %zd", writes[i], n);
        }
    }
    CHECK(same_as_file(track(f, 104)->source, (const uint8_t[]) { pattern(4, 0), pattern(4, 1),
          pattern(4, 2), pattern(4, 3), pattern(4, 4) }, 5), "files untouched");
}

/* ---------------------------------------------------------------- unknown */

static void expect(Fixture *f, enum CBNfsService svc, uint32_t prog, uint32_t vers, uint32_t proc,
                   int want, const char *what)
{
    Call c;
    call(&c, prog, vers, proc);
    CBRd r;
    ssize_t n = ask(f->nfs, svc, PLAYER, &c);
    int ret = results(n, &c, &r);
    CHECK(ret == want && n <= (ssize_t)c.w.len, "%s: %d (%zd bytes)", what, ret, n);
}

static void test_unknown(Fixture *f)
{
    const uint32_t nfs_procs[] = { CB_NFSPROC_ROOT, CB_NFSPROC_READLINK, CB_NFSPROC_WRITECACHE,
                                   18, 1000, UINT32_MAX };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(nfs_procs); i++)
        expect(f, CB_NFS_NFS, CB_RPC_NFS, 2, nfs_procs[i], -EOPNOTSUPP, "NFS procedure");
    const uint32_t mnt_procs[] = { CB_MNT_DUMP, CB_MNT_UMNTALL, CB_MNT_EXPORTALL, 7, 99 };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(mnt_procs); i++)
        expect(f, CB_NFS_MOUNT, CB_RPC_MOUNT, 1, mnt_procs[i], -EOPNOTSUPP, "mount procedure");
    const uint32_t pmap_procs[] = { CB_PMAP_SET, CB_PMAP_UNSET, CB_PMAP_CALLIT, 6, 1000 };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(pmap_procs); i++)
        expect(f, CB_NFS_PORTMAP, CB_RPC_PORTMAP, 2, pmap_procs[i], -EOPNOTSUPP, "portmap proc");

    /* Each socket its own program. */
    expect(f, CB_NFS_PORTMAP, CB_RPC_NFS, 2, 0, -EPROTONOSUPPORT, "NFS on portmap");
    expect(f, CB_NFS_NFS, CB_RPC_MOUNT, 1, 1, -EPROTONOSUPPORT, "mount on NFS");
    expect(f, CB_NFS_MOUNT, CB_RPC_PORTMAP, 2, 3, -EPROTONOSUPPORT, "portmap on mount");
    expect(f, CB_NFS_NFS, 999999, 1, 0, -EPROTONOSUPPORT, "program 999999");

    /* A version we do not speak names the one we do (rbxport's a_wrong_
     * program_version_names_the_version_we_serve). */
    const struct { enum CBNfsService svc; uint32_t prog, vers, ours; } mism[] = {
        { CB_NFS_NFS, CB_RPC_NFS, 3, 2 }, { CB_NFS_NFS, CB_RPC_NFS, 4, 2 },
        { CB_NFS_MOUNT, CB_RPC_MOUNT, 3, 1 }, { CB_NFS_PORTMAP, CB_RPC_PORTMAP, 4, 2 },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(mism); i++) {
        Call c;
        call(&c, mism[i].prog, mism[i].vers, 0);
        ssize_t n = ask(f->nfs, mism[i].svc, PLAYER, &c);
        CBRd r = cb_rd(reply, n > 0 ? (size_t)n : 0);
        uint32_t w[8];
        for (int k = 0; k < 8; k++)
            w[k] = cb_rd_u32(&r);
        CHECK(n == 32 && w[0] == c.xid && w[2] == 0 && w[5] == CB_RPC_PROG_MISMATCH &&
              w[6] == mism[i].ours && w[7] == mism[i].ours, "version %u of %u", mism[i].vers,
              mism[i].prog);
    }

    /* RPC version 3: denied, RPC_MISMATCH 2..2 (rbxport's a_wrong_rpc_
     * version_is_answered_with_the_version_we_speak). */
    Call c;
    call(&c, CB_RPC_NFS, 2, 0);
    c.b[11] = 3;
    ssize_t n = ask(f->nfs, CB_NFS_NFS, PLAYER, &c);
    static const uint8_t mismatch[] = { 0, 0, 0, 1,  0, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 2,
                                        0, 0, 0, 2 };
    CHECK(n == 24 && !memcmp(reply + 4, mismatch, sizeof(mismatch)), "RPC version 3: %zd", n);
    /* The same in twelve bytes is answered with nothing: 24 > 12. */
    CHECK(cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, c.b, 12, reply, sizeof(reply)) == 0,
          "too short to answer");

    /* The lock manager and status monitor get nothing, as from rekordbox
     * (rbxport's lock_manager_calls_are_dropped_without_a_reply). */
    for (uint32_t p = 100021; p <= 100024; p += 3) {
        call(&c, p, 1, 0);
        CHECK(ask(f->nfs, CB_NFS_NFS, PLAYER, &c) == 0, "program %u", p);
    }

    /* Credentials: AUTH_NULL and AUTH_UNIX taken; AUTH_DES, or AUTH_UNIX
     * that is not one, refused. */
    const struct { uint32_t flavor; const uint32_t *body; int nb; int want; } creds[] = {
        { 0, NULL, 0, 0 },
        { 1, (const uint32_t[]) { 1, 4, 0x41424344, 1000, 1000, 2, 4, 5 }, 8, 0 },
        { 3, (const uint32_t[]) { 0, 0 }, 2, -EACCES },
        { 1, (const uint32_t[]) { 1, 300, 0, 0, 0 }, 5, -EACCES },
        { 1, (const uint32_t[]) { 1, 0, 0, 0, 17 }, 5, -EACCES },
        { 1, (const uint32_t[]) { 1, 0, 0 }, 3, -EACCES },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(creds); i++) {
        c.w = cb_wr(c.b, sizeof(c.b));
        c.xid = next_xid++;
        const uint32_t head[] = { c.xid, 0, 2, CB_RPC_NFS, 2, 0, creds[i].flavor,
                                  4 * (uint32_t)creds[i].nb };
        for (size_t k = 0; k < CB_ARRAY_ELEMS(head); k++)
            cb_wr_u32(&c.w, head[k]);
        for (int k = 0; k < creds[i].nb; k++)
            cb_wr_u32(&c.w, creds[i].body[k]);
        cb_wr_u32(&c.w, 0);
        cb_wr_u32(&c.w, 0);
        CBRd r;
        n = ask(f->nfs, CB_NFS_NFS, PLAYER, &c);
        CHECK(results(n, &c, &r) == creds[i].want, "credential %zu: %zd bytes", i, n);
    }
    /* An opaque_auth over 400 bytes. */
    c.w = cb_wr(c.b, sizeof(c.b));
    c.xid = next_xid++;
    const uint32_t big[] = { c.xid, 0, 2, CB_RPC_NFS, 2, 0, 0, 404 };
    for (size_t k = 0; k < CB_ARRAY_ELEMS(big); k++)
        cb_wr_u32(&c.w, big[k]);
    cb_wr_zeros(&c.w, 404 + 8);
    CBRd r;
    CHECK(results(ask(f->nfs, CB_NFS_NFS, PLAYER, &c), &c, &r) == -EACCES, "404-byte credential");
}

/* -------------------------------------------------------------- malformed */

/* Whether a reply refuses: the RPC's refusals, and the status of a
 * procedure whose results start with one. */
static bool is_error(const uint8_t *p, size_t len, bool has_status)
{
    CBRd r;
    if (cb_nfs_get_reply(p, len, NULL, &r) < 0)
        return true;
    return has_status && cb_rd_left(&r) >= 4 && cb_nfs_get_status(&r) < 0;
}

static bool status_first(const uint8_t *call)
{
    uint32_t prog = be32_of(call + 12), proc = be32_of(call + 20);
    return (prog == CB_RPC_NFS && proc) || (prog == CB_RPC_MOUNT && proc == CB_MNT_MNT);
}

static void test_malformed(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0 && walk(f->nfs, root, specs[0].usb, fh, NULL) == 0,
          "set up");
    Call calls[12];
    int nb = 0;
    call(&calls[nb], CB_RPC_PORTMAP, 2, CB_PMAP_GETPORT);
    cb_wr_u32(&calls[nb].w, CB_RPC_NFS);
    cb_wr_u32(&calls[nb].w, 2);
    cb_wr_u32(&calls[nb].w, 17);
    cb_wr_u32(&calls[nb++].w, 0);
    call(&calls[nb++], CB_RPC_PORTMAP, 2, CB_PMAP_DUMP);
    call(&calls[nb], CB_RPC_MOUNT, 1, CB_MNT_MNT);
    cb_nfs_put_name(&calls[nb++].w, "/");
    call(&calls[nb++], CB_RPC_MOUNT, 1, CB_MNT_EXPORT);
    call(&calls[nb], CB_RPC_NFS, 2, CB_NFSPROC_GETATTR);
    cb_nfs_put_handle(&calls[nb++].w, fh);
    call(&calls[nb], CB_RPC_NFS, 2, CB_NFSPROC_LOOKUP);
    cb_nfs_put_handle(&calls[nb].w, root);
    cb_nfs_put_name(&calls[nb++].w, "Contents");
    call(&calls[nb], CB_RPC_NFS, 2, CB_NFSPROC_READ);
    cb_nfs_put_handle(&calls[nb].w, fh);
    cb_wr_u32(&calls[nb].w, 0);
    cb_wr_u32(&calls[nb].w, 32768);
    cb_wr_u32(&calls[nb++].w, 0);
    call(&calls[nb], CB_RPC_NFS, 2, CB_NFSPROC_READDIR);
    cb_nfs_put_handle(&calls[nb].w, root);
    cb_wr_u32(&calls[nb].w, 0);
    cb_wr_u32(&calls[nb++].w, 8192);
    call(&calls[nb], CB_RPC_NFS, 2, CB_NFSPROC_STATFS);
    cb_nfs_put_handle(&calls[nb++].w, root);
    call(&calls[nb], CB_RPC_NFS, 2, CB_NFSPROC_WRITE);
    cb_nfs_put_handle(&calls[nb++].w, fh);
    const enum CBNfsService svcs[3] = { CB_NFS_PORTMAP, CB_NFS_MOUNT, CB_NFS_NFS };

    /* Every cut of every call, to every socket, from a player and from
     * outside the subnet (which cannot mount): nothing, an answer to the
     * unknown or malformed no larger than what came, or (when the cut left
     * a whole call) its answer; a READ's bytes only for the player. */
    int answered = 0;
    for (int i = 0; i < nb; i++) {
        for (size_t cut = 0; cut <= calls[i].w.len; cut++) {
            for (int s = 0; s < 3; s++) {
                for (int who = 0; who < 2; who++) {
                    uint32_t peer = who ? OUTSIDER : PLAYER;
                    ssize_t n = cb_nfs_handle(f->nfs, svcs[s], peer, calls[i].b, cut, reply,
                                              sizeof(reply));
                    CHECK(n >= 0 && n <= CB_NFS_MAX_DATAGRAM, "call %d cut %zu: %zd", i, cut, n);
                    if (n <= 0)
                        continue;
                    answered++;
                    if (is_error(reply, (size_t)n, cut >= 24 && status_first(calls[i].b)))
                        CHECK((size_t)n <= cut, "call %d cut %zu on %d: %zd > request", i, cut, s,
                              n);
                    if (who && (size_t)n > 200)
                        CHECK(false, "an outsider got %zd bytes (call %d cut %zu)", n, i, cut);
                }
            }
        }
    }
    CHECK(answered > 100, "answered %d", answered);

    /* Random bytes after a good header: never more than came. */
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 2000; i++) {
        uint8_t b[256];
        Call c;
        call(&c, CB_RPC_NFS, 2, (uint32_t)(i % 20));
        size_t extra = (size_t)(i % 200);
        for (size_t k = 0; k < extra; k++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b[k] = (uint8_t)x;
        }
        cb_wr_bytes(&c.w, b, extra);
        ssize_t n = ask(f->nfs, CB_NFS_NFS, STRANGER, &c);
        if (n > 0 && is_error(reply, (size_t)n, i % 20 != 0))
            CHECK((size_t)n <= c.w.len, "random %d: %zd > %zu", i, n, c.w.len);
        CHECK(n <= 1024, "a stranger's random call %d: %zd bytes", i, n);
    }

    /* A reply buffer too small for the answer is an error, not an
     * overrun. */
    ssize_t n = cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, calls[6].b, calls[6].w.len, reply, 64);
    CHECK(n == 28 || n == -ENOBUFS, "READ into 64 bytes: %zd", n);
    n = cb_nfs_handle(f->nfs, CB_NFS_NFS, PLAYER, calls[6].b, calls[6].w.len, reply, 1000);
    CHECK(n > 0 && n <= 1000, "READ into 1000 bytes: %zd", n);
}

/* ---------------------------------------------------------------- rebuild */

static void test_rebuild(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], file[CB_NFS_FHSIZE], b2[CB_NFS_FHSIZE];
    uint8_t contents[CB_NFS_FHSIZE], homogenic[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    CHECK(walk(f->nfs, root, specs[0].usb, file, NULL) == 0 &&
          walk(f->nfs, root, specs[1].usb, b2, NULL) == 0 &&
          lookup(f->nfs, root, "Contents", contents, NULL) == 0 &&
          walk(f->nfs, root, "/Contents/Björk/Homogenic", homogenic, NULL) == 0, "handles");

    uint8_t *want;
    size_t size;
    if (cb_file_read(track(f, 101)->source, &want, &size) < 0)
        abort();
    uint8_t *got = malloc(size), *buf = malloc(CB_NFS_MAX_READ);
    if (!got || !buf)
        abort();
    int n = nfs_read(f->nfs, PLAYER, file, 0, 32768, buf, CB_NFS_MAX_READ, NULL);
    CHECK(n == 32768, "first piece");
    memcpy(got, buf, 32768);

    /* The library changed under the player: 101 moved, 102 gone, a new
     * track. The tree is made again from a new plan, the old one freed. */
    CBPlanTrack tracks[3] = {
        { .id = 101, .usb_path = "/Contents/Björk (Remastered)/Homogenic/01 Jóga.flac",
          .source = track(f, 101)->source, .file_size = (int64_t)size },
        { .id = 103, .usb_path = "/Contents/ABBA/Gold/01 Dancing Queen.mp3",
          .source = track(f, 103)->source, .file_size = 32768 },
        { .id = 120, .usb_path = "/Contents/New/Album/new.flac",
          .source = track(f, 104)->source, .file_size = 5 },
    };
    CBPlan plan = { .tracks = tracks, .nb_tracks = 3 };
    CBVTree *t = NULL;
    CHECK(cb_vtree_new(&t, &plan) == 0, "the new tree");
    cb_nfs_set_tree(f->nfs, t);

    /* The open file reads on to its end; a directory still there is the
     * same handle; what is gone is stale. */
    size_t off = 32768;
    while (off < size) {
        n = nfs_read(f->nfs, PLAYER, file, (uint32_t)off, 32768, buf, CB_NFS_MAX_READ, NULL);
        if (n <= 0)
            break;
        memcpy(got + off, buf, (size_t)n);
        off += (size_t)n;
    }
    CHECK(off == size && !memcmp(got, want, size), "read on across the rebuild: %zu", off);
    CBNfsAttr a;
    uint8_t fh[CB_NFS_FHSIZE];
    CHECK(getattr(f->nfs, file, &a) == 0 && a.fileid == 101, "101's handle");
    CHECK(lookup(f->nfs, contents, "New", fh, NULL) == 0, "Contents is the same handle");
    CHECK(walk(f->nfs, root, "/Contents/Björk (Remastered)/Homogenic/01 Jóga.flac", fh, NULL) == 0 &&
          !memcmp(fh, file, sizeof(fh)), "101 by its new path, the same handle");
    CHECK(getattr(f->nfs, b2, NULL) == -ESTALE, "102 is gone");
    CHECK(getattr(f->nfs, homogenic, NULL) == -ESTALE, "Björk/Homogenic is gone");

    cb_nfs_set_tree(f->nfs, f->tree);
    cb_vtree_free(&t);
    free(want);
    free(got);
    free(buf);
}

/* ------------------------------------------------------------ size change */

static void test_size_change(Fixture *f)
{
    uint8_t root[CB_NFS_FHSIZE], fh[CB_NFS_FHSIZE];
    CHECK(mnt(f->nfs, PLAYER, "/", root) == 0, "mount /");
    CHECK(walk(f->nfs, root, specs[3].usb, fh, NULL) == 0, "d.bin");
    CBNfsAttr a;
    CHECK(getattr(f->nfs, fh, &a) == 0 && a.size == 5, "5 bytes");
    uint8_t buf[64];
    CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 16, buf, sizeof(buf), NULL) == 5, "read 5");

    /* The transcode made later: the plan's source and size change, the
     * tree does not. */
    CBPlanTrack *pt = (CBPlanTrack *)track(f, 104);
    char *old = pt->source;
    pt->source = make_file("d-transcode.bin", 77, 70001);
    pt->file_size = 70001;
    uint8_t *data = NULL;
    size_t size = 0;
    CHECK(getattr(f->nfs, fh, &a) == 0 && a.size == 70001, "the new size: %u", a.size);
    CHECK(read_whole(f->nfs, fh, 32768, &data, &size) == 0 && size == 70001 &&
          same_as_file(pt->source, data, size), "the transcode read whole");
    free(data);

    /* The same path replaced on disk (renamed over): the new file, not
     * the one held open. */
    char *tmp = make_file("d-replaced.bin", 78, 70001);
    CHECK(rename(tmp, pt->source) == 0, "renamed over");
    free(tmp);
    CHECK(read_whole(f->nfs, fh, 32768, &data, &size) == 0 &&
          same_as_file(pt->source, data, size) && data[0] == pattern(78, 0), "the replacement");
    free(data);

    /* The file shorter than the plan says: what is there, then IO. */
    pt->file_size = 80000;
    CHECK(nfs_read(f->nfs, PLAYER, fh, 69990, 64, buf, sizeof(buf), NULL) == 11, "the last 11");
    CHECK(nfs_read(f->nfs, PLAYER, fh, 70001, 64, buf, sizeof(buf), NULL) == -EIO, "then IO");
    /* Missing altogether: attributes from the plan, reads IO. */
    unlink(pt->source);
    CHECK(getattr(f->nfs, fh, &a) == 0 && a.size == 80000 && a.mtime == 0, "missing: attributes");
    CHECK(nfs_read(f->nfs, PLAYER, fh, 0, 64, buf, sizeof(buf), NULL) == -EIO, "missing: IO");

    free(pt->source);
    pt->source = old;
    pt->file_size = 5;
}

/* ---------------------------------------------------------------- sockets */

typedef struct Server {
    CBNfs      *nfs;
    int         fd[3];
    atomic_bool stop;
    int         served;
} Server;

static void *serve(void *opaque)
{
    Server *s = opaque;
    uint8_t *in = malloc(CB_NFS_MAX_DATAGRAM), *out = malloc(CB_NFS_MAX_DATAGRAM);
    if (!in || !out)
        abort();
    while (!atomic_load(&s->stop)) {
        struct pollfd p[3];
        for (int i = 0; i < 3; i++)
            p[i] = (struct pollfd) { .fd = s->fd[i], .events = POLLIN };
        if (poll(p, 3, 20) <= 0)
            continue;
        for (int i = 0; i < 3; i++) {
            if (!(p[i].revents & POLLIN))
                continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(s->fd[i], in, CB_NFS_MAX_DATAGRAM, MSG_TRUNC,
                                 (struct sockaddr *)&from, &fl);
            if (n <= 0 || n > CB_NFS_MAX_DATAGRAM)
                continue;
            ssize_t r = cb_nfs_handle(s->nfs, (enum CBNfsService)i, ntohl(from.sin_addr.s_addr),
                                      in, (size_t)n, out, CB_NFS_MAX_DATAGRAM);
            if (r > 0) {
                sendto(s->fd[i], out, (size_t)r, 0, (const struct sockaddr *)&from, fl);
                s->served++;
            }
        }
    }
    free(in);
    free(out);
    return NULL;
}

static int bind_loopback(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr   = { htonl(INADDR_LOOPBACK) },
    };
    socklen_t al = sizeof(a);
    if (fd < 0 || bind(fd, (const struct sockaddr *)&a, sizeof(a)) < 0 ||
        getsockname(fd, (struct sockaddr *)&a, &al) < 0)
        abort();
    /* rekordbox's send and receive buffers (rbxport, net.rs, from
     * _tkfTransportOpen): room for a whole READ answer. */
    int size = 0x10000;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    *port = ntohs(a.sin_port);
    return fd;
}

static void test_sockets(Fixture *f)
{
    Server s = { 0 };
    uint16_t ports[3];
    for (int i = 0; i < 3; i++)
        s.fd[i] = bind_loopback(&ports[i]);
    CBNfsConfig cfg = {
        .portmap_port = ports[CB_NFS_PORTMAP],
        .mount_port   = ports[CB_NFS_MOUNT],
        .nfs_port     = ports[CB_NFS_NFS],
    };
    if (cb_nfs_new(&s.nfs, &cfg) < 0)
        abort();
    cb_nfs_set_tree(s.nfs, f->tree);
    pthread_t th;
    if (pthread_create(&th, NULL, serve, &s))
        abort();

    CBNfsClient *c = NULL;
    CBNfsClientOptions o = {
        .server       = INADDR_LOOPBACK,
        .portmap_port = ports[CB_NFS_PORTMAP],
        .local        = INADDR_LOOPBACK,
        .timeout_ms   = 1000,
    };
    int ret = cb_nfs_client_open(&c, &o);
    CHECK(ret == 0, "client: %s", strerror(-ret));
    if (!ret) {
        uint16_t mp = 0, np = 0;
        cb_nfs_client_ports(c, &mp, &np);
        CHECK(mp == ports[CB_NFS_MOUNT] && np == ports[CB_NFS_NFS], "GETPORT: %u %u", mp, np);
        CHECK(cb_nfs_client_mount(c, "/nope") == -ENOENT, "not exported");
        CHECK((ret = cb_nfs_client_mount(c, "/")) == 0, "mount: %s", strerror(-ret));
        for (int i = 0; i < NB_SERVED; i++) {
            uint8_t *data = NULL;
            size_t size = 0;
            ret = cb_nfs_client_fetch(c, specs[i].usb, 0, &data, &size);
            CHECK(ret == 0 && same_as_file(track(f, specs[i].id)->source, data, size),
                  "%s over UDP: %s, %zu bytes", specs[i].usb, strerror(-ret), size);
            free(data);
        }
        uint8_t *data = NULL;
        size_t size = 0;
        CHECK(cb_nfs_client_fetch(c, specs[1].usb, CB_NFS_MAX_READ, &data, &size) == 0 &&
              same_as_file(track(f, 102)->source, data, size), "in 63 kB pieces");
        free(data);
        uint8_t fh[CB_NFS_FHSIZE];
        CHECK(cb_nfs_client_walk(c, "/etc/passwd", fh, NULL) == -ENOENT, "no host paths");
        CHECK(cb_nfs_client_fetch(c, "/Contents", 0, &data, &size) == -EISDIR, "a directory");
    }
    cb_nfs_client_free(&c);

    atomic_store(&s.stop, true);
    pthread_join(th, NULL);
    CHECK(s.served > 10, "served %d", s.served);
    cb_nfs_free(&s.nfs);
    for (int i = 0; i < 3; i++)
        close(s.fd[i]);
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    const char *base = getenv("CB_TEST_DIR");
    snprintf(dir, sizeof(dir), "%s/test-%d", base ? base : "/tmp/cyanbox-test-nfs", (int)getpid());
    if (cb_mkdir_p(dir) < 0) {
        fprintf(stderr, "cannot make %s\n", dir);
        return 1;
    }
    cb_log_set_level(CB_LOG_ERROR);

    Fixture f;
    fixture_new(&f);
    test_vectors(&f);
    test_tree(&f);
    test_read(&f);
    test_readdir(&f);
    test_attrs(&f);
    test_traversal(&f);
    test_handles(&f);
    test_access(&f);
    test_writes(&f);
    test_unknown(&f);
    test_malformed(&f);
    test_rebuild(&f);
    test_size_change(&f);
    test_sockets(&f);

    for (size_t i = 0; i < CB_ARRAY_ELEMS(specs); i++) {
        if (specs[i].file && f.tracks[i].source)
            unlink(f.tracks[i].source);
    }
    fixture_free(&f);
    char *p = cb_path_join(dir, "d-transcode.bin");
    unlink(p);
    free(p);
    rmdir(dir);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("nfs: all passed\n");
    return 0;
}
