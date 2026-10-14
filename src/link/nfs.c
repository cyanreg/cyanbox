/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The file server players read audio from: portmap, mount version 1 and
 * NFS version 2 over UDP (RFC 1057, RFC 1094), one request datagram to one
 * reply datagram, over the virtual tree (vtree.c). Only what players use
 * is here (docs/LINK.md, "Security"): portmap NULL, GETPORT and DUMP;
 * mount NULL, MNT, UMNT and EXPORT; NFS NULL, GETATTR, LOOKUP, READ,
 * READDIR and STATFS. Every write is NFSERR_ROFS, everything else
 * PROC_UNAVAIL. The evidence is rbxport's (a Link Export server measured
 * against rekordbox 7.2.11, a CDJ-3000 and an XDJ-700: rbl-nfs and its
 * tests, the commit messages that carry the captures).
 *
 * What differs from a stock NFS server, and why:
 *
 * - Names are UTF-16LE in mount paths, lookups, listings and the export
 *   list (rbxport's captures; Deep Symmetry's FileFetcher sends players
 *   the same). The export is "/", the virtual root.
 * - The export list names a group, the interface's "a.b.c.d/m.m.m.m":
 *   a CDJ-3000 mounts nothing whose list names none (rbxport 1605b376).
 * - READ answers up to 0xfc00 bytes, rekordbox's cap: a CDJ-3000 asks for
 *   32 kB and does not reliably ask again for the rest of a shorter
 *   answer (RX_NFS_MAX_READ). A READ that yields no bytes, at or past the
 *   end or of count 0, is NFSERR_IO, as rekordbox answers it (rbxport's
 *   static analysis of libFilSiNE's _tkfFSReadFile); a player reads to
 *   the size it was given and never asks past it.
 * - Handles: 32 bytes of which only the first 12 mean anything, because
 *   an XDJ-700 (firmware 1.15) writes its own 20 bytes after them in
 *   every LOOKUP and rekordbox ignores them (rbxport 9cdc4f4d, captures
 *   on its issue 43). Ours: the session's random tag; then a directory's
 *   63-bit id with the top bit set, or 0 and a track id for a file; then
 *   20 zero bytes, never read. Anything else is NFSERR_STALE.
 * - Writes are NFSERR_ROFS (docs/LINK.md). rekordbox's own stubs answer
 *   STALE and ACCES instead (rbxport); players do not write, and ROFS is
 *   what a read-only server says.
 * - The lock manager and status monitor (programs 100021, 100024) get no
 *   answer, as rekordbox gives none (rbxport, rbl-nfs lib.rs).
 * - No reply cache: every procedure here is idempotent, so a retransmit
 *   is simply answered again.
 *
 * And for the network it is on (docs/LINK.md, "Security"):
 *
 * - Each socket answers its own program only.
 * - READ and READDIR, the two answers that can be large, answer only a
 *   peer that has mounted, from a small table of addresses (oldest
 *   forgotten first), or one that mounted in this session and reads with
 *   a handle of it, put back in the table: a deck that comes back from a
 *   lapse reads on and never mounts again. UMNT does not take a peer off
 *   it: an XDJ-AZ is two decks on one address (rbxport's
 *   link-testing.md), and one deck unmounting must not cut the other off.
 * - A datagram that is not a call, or whose header cannot be read, gets
 *   nothing; an answer to anything unknown or malformed is sent only when
 *   it is no longer than the request.
 * - Names a peer sends are only ever compared with the tree's; a file is
 *   opened by the plan's `source` alone. A track the plan marks `pcm` has
 *   no file: its bytes come from the config's read (a WAV made as it is
 *   read, wav.h), its size and times as for any other. */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rxbox/common.h"

#include "nfs.h"

#define RPC_CALL            0
#define RPC_REPLY           1
#define RPC_VERSION         2
#define MSG_ACCEPTED        0
#define MSG_DENIED          1
#define REJECT_RPC_MISMATCH 0
#define REJECT_AUTH_ERROR   1
#define AUTH_BADCRED        1
#define AUTH_NULL           0
#define AUTH_UNIX           1
#define AUTH_MAX            400         /* an opaque_auth's body (RFC 1057 9) */
#define UNIX_NAME_MAX       255         /* authunix_parms (RFC 1057 9.2) */
#define UNIX_GIDS_MAX       16
#define MNT_PATH_MAX        1024        /* dirpath<MNTPATHLEN> (RFC 1094 A.2.1) */
#define PROG_NLM            100021
#define PROG_NSM            100024
#define PROTO_UDP           17

#define NFREG               1
#define NFDIR               2
/* A directory as rekordbox's export root showed it to a CDJ: mode 041ed,
 * two links, 64 bytes (rbxport, rbl-nfs vfs.rs). Files are read-only. */
#define DIR_MODE            0040755
#define FILE_MODE           0100444
#define DIR_SIZE            64
#define BLOCK_SIZE          4096
/* fsid 2, as rekordbox reports it (rbxport's tests of attributes). */
#define FSID                2
/* STATFS's preferred transfer size: what players are seen to ask for
 * (a CDJ-3000's 32 kB, rbxport 6d4161a0), which READ answers in full.
 * rekordbox gives its host file system's figure; assumed unread. */
#define TSIZE               32768

#define NB_MOUNTS           16
#define NB_FILES            4
#define NB_EVER             256         /* addresses that mounted, remembered */
/* A READDIR answer's room: what the call asks, within these. The floor
 * fits one entry of the longest name, so a listing always moves on. */
#define READDIR_MIN         1024
#define READDIR_MAX         8192

#define REPLY_HEAD          24          /* xid, REPLY, ACCEPTED, null verifier, accept_stat */
#define FATTR_SIZE          68
#define READ_HEAD           (REPLY_HEAD + 4 + FATTR_SIZE + 4)

#define HANDLE_DIR          0x80000000u

typedef struct Mount {
    uint32_t addr;
    uint64_t used;              /* 0: a free slot */
} Mount;

typedef struct OpenFile {
    int         fd;
    uint32_t    id;             /* the track's */
    char       *path;
    struct stat st;
    uint64_t    used;
} OpenFile;

struct RXNfs {
    RXNfsConfig    cfg;
    const RXVTree *tree;
    uint32_t       tag;
    uint64_t       clock;       /* counts calls: when a mount or a file was last used */
    Mount          mounts[NB_MOUNTS];
    OpenFile       files[NB_FILES];
    /* Every address that mounted in this session, which a handle of ours
     * takes back in (may_read_node); the first NB_EVER. */
    uint32_t       ever[NB_EVER];
    int            nb_ever;
};

/* One call being answered. */
typedef struct Req {
    RXNfs   *n;
    uint32_t peer;
    uint32_t xid;
    RXRd     args;
    RXWr     w;
    bool     small;             /* an answer to the unknown or malformed */
    uint32_t prog, proc;        /* for the debug log */
    char     note[160];
} Req;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static size_t pad4(size_t n)
{
    return (n + 3) & ~(size_t)3;
}

/* The body of an XDR opaque whose length was just read: the bytes and
 * their padding, checked against what is left first. */
static const uint8_t *opaque_body(RXRd *r, uint32_t len)
{
    if (len > rx_rd_left(r)) {
        r->err = true;
        return NULL;
    }
    return rx_rd_bytes(r, pad4(len));
}

/* ------------------------------------------------------------------ names */

void rx_nfs_put_name(RXWr *w, const char *utf8)
{
    size_t bytes = 2 * rx_link_utf16_units(utf8);
    if (bytes > UINT32_MAX) {
        w->err = true;
        return;
    }
    rx_wr_u32(w, (uint32_t)bytes);
    rx_wr_utf16(w, utf8, false, false);
    rx_wr_zeros(w, pad4(bytes) - bytes);
}

int rx_nfs_get_name(RXRd *r, size_t max_units, char *out, size_t size)
{
    if (!size)
        return RX_ERROR(EINVAL);
    out[0] = '\0';
    uint32_t len = rx_rd_u32(r);
    const uint8_t *p = r->err ? NULL : opaque_body(r, len);
    if (!p)
        return RX_ERROR(EBADMSG);
    if (len > 2 * max_units)
        return RX_ERROR(ENAMETOOLONG);
    if (len & 1)
        return RX_ERROR(EILSEQ);
    size_t units = len / 2, o = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = (uint32_t)p[2 * i + 1] << 8 | p[2 * i];
        if (!c)
            return RX_ERROR(EILSEQ);
        if (c >= 0xD800 && c < 0xDC00) {
            uint32_t d = i + 1 < units ? (uint32_t)p[2 * i + 3] << 8 | p[2 * i + 2] : 0;
            if (d < 0xDC00 || d >= 0xE000)
                return RX_ERROR(EILSEQ);
            c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
            i++;
        } else if (c >= 0xDC00 && c < 0xE000) {
            return RX_ERROR(EILSEQ);
        }
        size_t k = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (o + k >= size)
            return RX_ERROR(ENAMETOOLONG);
        if (k == 1) {
            out[o++] = (char)c;
        } else if (k == 2) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (k == 3) {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | c >> 18);
            out[o++] = (char)(0x80 | (c >> 12 & 0x3F));
            out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* ---------------------------------------------------------------- session */

int rx_nfs_new(RXNfs **out, const RXNfsConfig *c)
{
    if (!out || !c)
        return RX_ERROR(EINVAL);
    *out = NULL;
    RXNfs *n = calloc(1, sizeof(*n));
    if (!n)
        return RX_ERROR(ENOMEM);
    n->cfg = *c;
    for (int i = 0; i < NB_FILES; i++)
        n->files[i].fd = -1;
    /* The tag tells this session's handles from any other's: a handle a
     * player kept from before a restart is stale. Never 0. */
    while (!n->tag) {
        if (getrandom(&n->tag, sizeof(n->tag), 0) != (ssize_t)sizeof(n->tag) && errno != EINTR) {
            int err = errno;
            free(n);
            return RX_ERROR(err);
        }
    }
    *out = n;
    return 0;
}

static void close_file(OpenFile *f)
{
    if (f->fd >= 0)
        close(f->fd);
    free(f->path);
    *f = (OpenFile) { .fd = -1 };
}

void rx_nfs_free(RXNfs **pn)
{
    if (!pn || !*pn)
        return;
    for (int i = 0; i < NB_FILES; i++)
        close_file(&(*pn)->files[i]);
    free(*pn);
    *pn = NULL;
}

void rx_nfs_set_tree(RXNfs *n, const RXVTree *t)
{
    if (!n)
        return;
    n->tree = t;
    /* Files of tracks gone from the library are closed now rather than
     * when the cache wants their slot. */
    for (int i = 0; i < NB_FILES; i++) {
        if (n->files[i].fd >= 0 && rx_vtree_find_file(t, n->files[i].id) < 0)
            close_file(&n->files[i]);
    }
}

/* ----------------------------------------------------------------- mounts */

static Mount *find_mount(const RXNfs *n, uint32_t peer)
{
    for (int i = 0; i < NB_MOUNTS; i++) {
        if (n->mounts[i].used && n->mounts[i].addr == peer)
            return (Mount *)&n->mounts[i];
    }
    return NULL;
}

bool rx_nfs_mounted(const RXNfs *n, uint32_t peer)
{
    return n && find_mount(n, peer);
}

void rx_nfs_forget(RXNfs *n, uint32_t peer)
{
    Mount *m = n ? find_mount(n, peer) : NULL;
    if (m)
        *m = (Mount) { 0 };
}

static bool mounted_ever(const RXNfs *n, uint32_t peer)
{
    for (int i = 0; i < n->nb_ever; i++)
        if (n->ever[i] == peer)
            return true;
    return false;
}

static void add_mount(RXNfs *n, uint32_t peer)
{
    if (!mounted_ever(n, peer) && n->nb_ever < NB_EVER)
        n->ever[n->nb_ever++] = peer;
    Mount *m = find_mount(n, peer);
    if (!m) {
        m = &n->mounts[0];
        for (int i = 1; i < NB_MOUNTS && m->used; i++) {
            if (n->mounts[i].used < m->used)
                m = &n->mounts[i];
        }
        rx_info("link: %u.%u.%u.%u mounted the library", peer >> 24, peer >> 16 & 0xFF,
                peer >> 8 & 0xFF, peer & 0xFF);
    }
    *m = (Mount) { .addr = peer, .used = n->clock };
}

/* Whether a peer may read, and if so it is in use now. */
static bool may_read(RXNfs *n, uint32_t peer)
{
    Mount *m = find_mount(n, peer);
    if (m)
        m->used = n->clock;
    return m;
}

/* Whether a peer may READ or READDIR through the handle of `node`: one
 * that is in the table, or one that mounted in this session, is in the
 * subnet still and reads with a handle of this session's (node >= 0),
 * taken back in. A deck that dropped off the table (forgotten, or the
 * oldest of NB_MOUNTS) comes back reading, never mounting again, and a
 * refusal there would hang its load for good (an emulated CDJ-2000NXS2
 * retried a refused READ 43 times a second for minutes, its load never
 * ending). */
static bool in_subnet(const RXNfsConfig *c, uint32_t peer);

static bool may_read_node(RXNfs *n, uint32_t peer, int node)
{
    if (may_read(n, peer))
        return true;
    if (node < 0 || !mounted_ever(n, peer) || !in_subnet(&n->cfg, peer))
        return false;
    rx_info("link: %u.%u.%u.%u reads with a handle of ours: taken back in", peer >> 24,
            peer >> 16 & 0xFF, peer >> 8 & 0xFF, peer & 0xFF);
    add_mount(n, peer);
    return true;
}

/* ------------------------------------------------------------------ files */

/* The track's file, open: one of a few kept open between READs (a player
 * reads a track in hundreds of pieces). Opened again when the plan names
 * another file (a transcode made since) or the path is another file now
 * (replaced on disk). Only the plan's absolute path is ever opened, and
 * only a regular file is read (O_NONBLOCK: a FIFO there cannot block). */
static OpenFile *open_track(RXNfs *n, const RXPlanTrack *pt)
{
    struct stat st;
    if (!pt->source || pt->source[0] != '/' || stat(pt->source, &st) < 0 || !S_ISREG(st.st_mode))
        return NULL;
    OpenFile *f = NULL;
    for (int i = 0; i < NB_FILES; i++) {
        OpenFile *e = &n->files[i];
        if (e->fd < 0 || e->id != pt->id)
            continue;
        if (e->st.st_dev == st.st_dev && e->st.st_ino == st.st_ino && !strcmp(e->path, pt->source)) {
            e->st = st;
            e->used = n->clock;
            return e;
        }
        f = e;
    }
    if (!f) {
        f = &n->files[0];
        for (int i = 1; i < NB_FILES && f->fd >= 0; i++) {
            if (n->files[i].fd < 0 || n->files[i].used < f->used)
                f = &n->files[i];
        }
    }
    close_file(f);
    int fd = open(pt->source, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        rx_debug("link: cannot open %s for a player: %s", pt->source, strerror(errno));
        return NULL;
    }
    char *path = rx_strdup(pt->source);
    if (!path || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        free(path);
        close(fd);
        return NULL;
    }
    *f = (OpenFile) { .fd = fd, .id = pt->id, .path = path, .st = st, .used = n->clock };
    return f;
}

static ssize_t pread_full(int fd, uint8_t *buf, size_t n, off_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, buf + got, n - got, off + (off_t)got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return got ? (ssize_t)got : -1;
        if (!r)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* ---------------------------------------------------------------- replies */

static void put_accepted(RXWr *w, uint32_t xid, uint32_t stat)
{
    rx_wr_u32(w, xid);
    rx_wr_u32(w, RPC_REPLY);
    rx_wr_u32(w, MSG_ACCEPTED);
    rx_wr_u32(w, AUTH_NULL);
    rx_wr_u32(w, 0);
    rx_wr_u32(w, stat);
}

static void put_denied(RXWr *w, uint32_t xid, uint32_t why, uint32_t a, uint32_t b, bool two)
{
    rx_wr_u32(w, xid);
    rx_wr_u32(w, RPC_REPLY);
    rx_wr_u32(w, MSG_DENIED);
    rx_wr_u32(w, why);
    rx_wr_u32(w, a);
    if (two)
        rx_wr_u32(w, b);
}

/* An accepted reply with no results: a void success, or a refusal. */
static void reply_rpc(Req *q, uint32_t stat)
{
    put_accepted(&q->w, q->xid, stat);
    q->small = stat != RX_RPC_SUCCESS;
}

/* A procedure's refusal: its status and nothing more. */
static void reply_nfs(Req *q, uint32_t stat)
{
    put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
    rx_wr_u32(&q->w, stat);
    q->small = true;
}

static void put_string(RXWr *w, const char *s)
{
    size_t n = strlen(s);
    rx_wr_u32(w, (uint32_t)n);
    rx_wr_bytes(w, s, n);
    rx_wr_zeros(w, pad4(n) - n);
}

/* ---------------------------------------------------------------- portmap */

static void pmap_call(Req *q, uint32_t proc)
{
    const RXNfsConfig *c = &q->n->cfg;
    switch (proc) {
    case RX_PMAP_NULL:
        reply_rpc(q, RX_RPC_SUCCESS);
        return;
    case RX_PMAP_GETPORT: {
        /* A mapping (RFC 1057 A): program, version, protocol, port. Zero
         * is "not registered", the answer for anything but mount version
         * 1 and NFS version 2 over UDP, which is all a player asks for
         * (rbxport's fake CDJ; rekordbox matches all three the same way,
         * rbxport dc107952). */
        uint32_t prog = rx_rd_u32(&q->args), vers = rx_rd_u32(&q->args);
        uint32_t proto = rx_rd_u32(&q->args);
        rx_rd_u32(&q->args);
        if (q->args.err) {
            reply_rpc(q, RX_RPC_GARBAGE_ARGS);
            return;
        }
        uint16_t port = 0;
        if (proto == PROTO_UDP && prog == RX_RPC_MOUNT && vers == RX_RPC_MOUNT_VERSION)
            port = c->mount_port;
        else if (proto == PROTO_UDP && prog == RX_RPC_NFS && vers == RX_RPC_NFS_VERSION)
            port = c->nfs_port;
        put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
        rx_wr_u32(&q->w, port);
        return;
    }
    case RX_PMAP_DUMP: {
        /* pmaplist: what GETPORT gives out, and portmap itself, as
         * rekordbox lists them (rbxport, rbl-nfs lib.rs). */
        const uint32_t maps[3][3] = {
            { RX_RPC_PORTMAP, RX_RPC_PORTMAP_VERSION, c->portmap_port },
            { RX_RPC_NFS,     RX_RPC_NFS_VERSION,     c->nfs_port },
            { RX_RPC_MOUNT,   RX_RPC_MOUNT_VERSION,   c->mount_port },
        };
        put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
        for (int i = 0; i < 3; i++) {
            if (!maps[i][2])
                continue;
            rx_wr_u32(&q->w, 1);
            rx_wr_u32(&q->w, maps[i][0]);
            rx_wr_u32(&q->w, maps[i][1]);
            rx_wr_u32(&q->w, PROTO_UDP);
            rx_wr_u32(&q->w, maps[i][2]);
        }
        rx_wr_u32(&q->w, 0);
        return;
    }
    default:
        /* SET and UNSET (nothing registers with us) and CALLIT (an
         * indirect call, a classic amplifier) included. */
        reply_rpc(q, RX_RPC_PROC_UNAVAIL);
        return;
    }
}

/* ------------------------------------------------------------------ mount */

static bool in_subnet(const RXNfsConfig *c, uint32_t peer)
{
    return !c->netmask || !((peer ^ c->addr) & c->netmask);
}

static void put_handle(RXWr *w, const RXNfs *n, int node);

static void mount_call(Req *q, uint32_t proc)
{
    RXNfs *n = q->n;
    switch (proc) {
    case RX_MNT_NULL:
        reply_rpc(q, RX_RPC_SUCCESS);
        return;
    case RX_MNT_MNT:
    case RX_MNT_UMNT: {
        /* dirpath, UTF-16LE as players send it (rbxport's captures of a
         * CDJ-3000 and an XDJ-700 mounting rekordbox's "/"). */
        char path[MNT_PATH_MAX / 2 * 3 + 1];
        int len = rx_nfs_get_name(&q->args, MNT_PATH_MAX / 2, path, sizeof(path));
        if (len == RX_ERROR(EBADMSG) || len == RX_ERROR(ENAMETOOLONG)) {
            reply_rpc(q, RX_RPC_GARBAGE_ARGS);
            return;
        }
        /* UMNT is void; the peer stays able to read (see the top). */
        if (proc == RX_MNT_UMNT) {
            reply_rpc(q, RX_RPC_SUCCESS);
            return;
        }
        /* The one export is the virtual root, and none until there is a
         * tree. Anything else, and a peer outside the subnet, is
         * NFSERR_ACCES, as rekordbox answers both (rbxport). */
        if (len < 0 || strcmp(path, "/") || !n->tree || !in_subnet(&n->cfg, q->peer)) {
            reply_nfs(q, RX_NFSERR_ACCES);
            return;
        }
        add_mount(n, q->peer);
        put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
        rx_wr_u32(&q->w, RX_NFS_OK);
        put_handle(&q->w, n, rx_vtree_root(n->tree));
        return;
    }
    case RX_MNT_EXPORT: {
        /* exports: the root, its name UTF-16LE and its group, the
         * interface's subnet in ASCII, as rekordbox 7.2.11 sends them
         * (rbxport, measured 2026-09-13). Nothing before there is a tree:
         * rekordbox lists its exports only once the link is up. */
        put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
        if (n->tree) {
            rx_wr_u32(&q->w, 1);
            rx_nfs_put_name(&q->w, "/");
            const RXNfsConfig *c = &n->cfg;
            if (c->addr) {
                char group[40];
                snprintf(group, sizeof(group), "%u.%u.%u.%u/%u.%u.%u.%u", c->addr >> 24,
                         c->addr >> 16 & 0xFF, c->addr >> 8 & 0xFF, c->addr & 0xFF,
                         c->netmask >> 24, c->netmask >> 16 & 0xFF, c->netmask >> 8 & 0xFF,
                         c->netmask & 0xFF);
                rx_wr_u32(&q->w, 1);
                put_string(&q->w, group);
            }
            rx_wr_u32(&q->w, 0);
        }
        rx_wr_u32(&q->w, 0);
        return;
    }
    default:
        /* DUMP is refused as rekordbox refuses it (rbxport dc107952);
         * UMNTALL and EXPORTALL are not used by players. */
        reply_rpc(q, RX_RPC_PROC_UNAVAIL);
        return;
    }
}

/* -------------------------------------------------------------------- NFS */

static void put_handle(RXWr *w, const RXNfs *n, int node)
{
    rx_wr_u32(w, n->tag);
    if (rx_vtree_is_dir(n->tree, node)) {
        uint64_t id = rx_vtree_dir_id(n->tree, node);
        rx_wr_u32(w, HANDLE_DIR | (uint32_t)(id >> 32));
        rx_wr_u32(w, (uint32_t)id);
    } else {
        rx_wr_u32(w, 0);
        rx_wr_u32(w, rx_vtree_track(n->tree, node)->id);
    }
    rx_wr_zeros(w, RX_NFS_FHSIZE - 12);
}

/* The node a handle names, or -1: stale, from another session, or not a
 * handle of ours. Bytes 12 to 31 are not read (the XDJ-700's). */
static int node_of(const RXNfs *n, const uint8_t *fh)
{
    if (!n->tree || be32(fh) != n->tag)
        return -1;
    uint32_t kind = be32(fh + 4), id = be32(fh + 8);
    if (kind & HANDLE_DIR)
        return rx_vtree_find_dir(n->tree, (uint64_t)(kind & ~HANDLE_DIR) << 32 | id);
    return kind ? -1 : rx_vtree_find_file(n->tree, id);
}

static uint32_t fileid_of(const RXVTree *t, int node)
{
    if (rx_vtree_is_dir(t, node))
        return HANDLE_DIR | (uint32_t)rx_vtree_dir_id(t, node);
    return rx_vtree_track(t, node)->id;
}

/* The source's own times, when it is there to say them. */
static const struct stat *stat_source(const RXPlanTrack *pt, struct stat *st)
{
    return pt->source && pt->source[0] == '/' && !stat(pt->source, st) ? st : NULL;
}

/* fattr (RFC 1094 2.3.5). A file's size is the plan's, read now. */
static void put_fattr(RXWr *w, const RXNfs *n, int node, const struct stat *st)
{
    RXVTreeStats s;
    rx_vtree_stats(n->tree, &s);
    bool dir = rx_vtree_is_dir(n->tree, node);
    uint32_t size = DIR_SIZE;
    if (!dir) {
        int64_t fs = rx_vtree_track(n->tree, node)->file_size;
        size = (uint32_t)RX_CLAMP(fs, 0, (int64_t)UINT32_MAX);
    }
    rx_wr_u32(w, dir ? NFDIR : NFREG);
    rx_wr_u32(w, dir ? DIR_MODE : FILE_MODE);
    rx_wr_u32(w, dir ? 2 : 1);
    rx_wr_u32(w, 0);                        /* uid */
    rx_wr_u32(w, 0);                        /* gid */
    rx_wr_u32(w, size);
    rx_wr_u32(w, BLOCK_SIZE);
    rx_wr_u32(w, 0);                        /* rdev */
    /* 512-byte blocks, as rekordbox passes on the host's st_blocks. */
    rx_wr_u32(w, dir ? 0 : (uint32_t)(((uint64_t)size + 511) / 512));
    rx_wr_u32(w, FSID);
    rx_wr_u32(w, fileid_of(n->tree, node));
    if (dir || !st) {
        uint32_t t = dir ? s.time : 0;
        for (int i = 0; i < 3; i++) {
            rx_wr_u32(w, t);
            rx_wr_u32(w, 0);
        }
        return;
    }
    const struct timespec *ts[3] = { &st->st_atim, &st->st_mtim, &st->st_ctim };
    for (int i = 0; i < 3; i++) {
        rx_wr_u32(w, (uint32_t)ts[i]->tv_sec);
        rx_wr_u32(w, (uint32_t)(ts[i]->tv_nsec / 1000));
    }
}

static void put_attrs_ok(Req *q, int node)
{
    struct stat st;
    const RXPlanTrack *pt = rx_vtree_track(q->n->tree, node);
    put_fattr(&q->w, q->n, node, pt ? stat_source(pt, &st) : NULL);
}

static void nfs_getattr(Req *q)
{
    const uint8_t *fh = rx_rd_bytes(&q->args, RX_NFS_FHSIZE);
    if (!fh) {
        reply_rpc(q, RX_RPC_GARBAGE_ARGS);
        return;
    }
    int node = node_of(q->n, fh);
    if (node < 0) {
        reply_nfs(q, RX_NFSERR_STALE);
        return;
    }
    put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
    rx_wr_u32(&q->w, RX_NFS_OK);
    put_attrs_ok(q, node);
}

static void nfs_lookup(Req *q)
{
    /* diropargs: the directory and one name. A player walks its path a
     * name at a time (rbxport's fake CDJ, Deep Symmetry's FileFetcher);
     * a name is only ever compared with the tree's, so "/etc/passwd",
     * "..", "." and the empty name find nothing. */
    const uint8_t *fh = rx_rd_bytes(&q->args, RX_NFS_FHSIZE);
    char name[RX_NFS_NAME_MAX * 3 + 1];
    int len = rx_nfs_get_name(&q->args, RX_NFS_NAME_MAX, name, sizeof(name));
    if (!fh || len == RX_ERROR(EBADMSG)) {
        reply_rpc(q, RX_RPC_GARBAGE_ARGS);
        return;
    }
    int dir = node_of(q->n, fh);
    if (dir < 0) {
        reply_nfs(q, RX_NFSERR_STALE);
        return;
    }
    if (!rx_vtree_is_dir(q->n->tree, dir)) {
        reply_nfs(q, RX_NFSERR_NOTDIR);
        return;
    }
    if (len == RX_ERROR(ENAMETOOLONG)) {
        reply_nfs(q, RX_NFSERR_NAMETOOLONG);
        return;
    }
    snprintf(q->note, sizeof(q->note), "\"%.120s\" (%d)", len >= 0 ? name : "", len);
    int node = len <= 0 || !strcmp(name, ".") || !strcmp(name, "..") ? -1 :
               rx_vtree_lookup(q->n->tree, dir, name, (size_t)len);
    if (node < 0) {
        reply_nfs(q, RX_NFSERR_NOENT);
        return;
    }
    put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
    rx_wr_u32(&q->w, RX_NFS_OK);
    put_handle(&q->w, q->n, node);
    put_attrs_ok(q, node);
}

static void nfs_read(Req *q)
{
    /* readargs: the file, offset, count, and totalcount, which is unused
     * (RFC 1094 2.2.7) and so not required. */
    RXNfs *n = q->n;
    const uint8_t *fh = rx_rd_bytes(&q->args, RX_NFS_FHSIZE);
    uint32_t offset = rx_rd_u32(&q->args), count = rx_rd_u32(&q->args);
    snprintf(q->note, sizeof(q->note), "offset %u count %u", offset, count);
    if (q->args.err) {
        reply_rpc(q, RX_RPC_GARBAGE_ARGS);
        return;
    }
    int node = node_of(n, fh);
    if (!may_read_node(n, q->peer, node)) {
        reply_nfs(q, RX_NFSERR_ACCES);
        return;
    }
    if (node < 0) {
        reply_nfs(q, RX_NFSERR_STALE);
        return;
    }
    const RXPlanTrack *pt = rx_vtree_track(n->tree, node);
    if (!pt) {
        reply_nfs(q, RX_NFSERR_ISDIR);
        return;
    }
    uint32_t size = (uint32_t)RX_CLAMP(pt->file_size, 0, (int64_t)UINT32_MAX);
    if (!count || offset >= size) {
        reply_nfs(q, RX_NFSERR_IO);
        return;
    }
    if (q->w.cap < READ_HEAD + 4) {
        q->w.err = true;
        return;
    }
    uint32_t want = RX_MIN(RX_MIN(count, RX_NFS_MAX_READ), size - offset);
    want = (uint32_t)RX_MIN((size_t)want, (q->w.cap - READ_HEAD) & ~(size_t)3);
    /* A WAV made as it is read, or a file. */
    OpenFile *f = NULL;
    struct stat st;
    const struct stat *sp = NULL;
    if (pt->pcm) {
        sp = stat_source(pt, &st);
    } else if ((f = open_track(n, pt))) {
        sp = &f->st;
    }
    if ((pt->pcm && !n->cfg.read) || (!pt->pcm && !f)) {
        reply_nfs(q, RX_NFSERR_IO);
        return;
    }
    put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
    rx_wr_u32(&q->w, RX_NFS_OK);
    put_fattr(&q->w, n, node, sp);
    size_t at = q->w.len;
    rx_wr_u32(&q->w, 0);
    uint8_t *d = rx_wr_room(&q->w, pad4(want));
    ssize_t got = !d ? -1 : pt->pcm ? n->cfg.read(n->cfg.read_opaque, pt, offset, d, want)
                                    : pread_full(f->fd, d, want, (off_t)offset);
    if (got <= 0) {
        /* The file is shorter than the plan says, or unreadable: the
         * answer so far goes, the status alone instead. */
        q->w.len = 0;
        reply_nfs(q, RX_NFSERR_IO);
        return;
    }
    memset(d + got, 0, pad4((size_t)got) - (size_t)got);
    rx_wr_at_u32(&q->w, at, (uint32_t)got);
    q->w.len = at + 4 + pad4((size_t)got);
}

static void nfs_readdir(Req *q)
{
    /* readdirargs: the directory, a cookie (here the place to go on
     * from, 0 the start) and how many bytes of entries to send. */
    RXNfs *n = q->n;
    const uint8_t *fh = rx_rd_bytes(&q->args, RX_NFS_FHSIZE);
    uint32_t cookie = rx_rd_u32(&q->args), count = rx_rd_u32(&q->args);
    if (q->args.err) {
        reply_rpc(q, RX_RPC_GARBAGE_ARGS);
        return;
    }
    int dir = node_of(n, fh);
    if (!may_read_node(n, q->peer, dir)) {
        reply_nfs(q, RX_NFSERR_ACCES);
        return;
    }
    if (dir < 0) {
        reply_nfs(q, RX_NFSERR_STALE);
        return;
    }
    if (!rx_vtree_is_dir(n->tree, dir)) {
        reply_nfs(q, RX_NFSERR_NOTDIR);
        return;
    }
    size_t budget = RX_CLAMP((size_t)count, (size_t)READDIR_MIN, (size_t)READDIR_MAX);
    if (q->w.cap < REPLY_HEAD + READDIR_MIN) {
        q->w.err = true;
        return;
    }
    budget = RX_MIN(budget, q->w.cap - REPLY_HEAD);
    put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
    rx_wr_u32(&q->w, RX_NFS_OK);
    /* The status, the list's end and eof are counted with the entries. */
    size_t used = 12;
    uint32_t nb = (uint32_t)rx_vtree_nb_children(n->tree, dir), i = RX_MIN(cookie, nb);
    for (; i < nb; i++) {
        const char *name = rx_vtree_name(n->tree, rx_vtree_child(n->tree, dir, (int)i));
        size_t bytes = 2 * rx_link_utf16_units(name), e = 16 + pad4(bytes);
        if (used + e > budget)
            break;
        rx_wr_u32(&q->w, 1);
        rx_wr_u32(&q->w, fileid_of(n->tree, rx_vtree_child(n->tree, dir, (int)i)));
        rx_nfs_put_name(&q->w, name);
        rx_wr_u32(&q->w, i + 1);
        used += e;
    }
    rx_wr_u32(&q->w, 0);
    rx_wr_u32(&q->w, i >= nb);
}

static void nfs_statfs(Req *q)
{
    const uint8_t *fh = rx_rd_bytes(&q->args, RX_NFS_FHSIZE);
    if (!fh) {
        reply_rpc(q, RX_RPC_GARBAGE_ARGS);
        return;
    }
    if (node_of(q->n, fh) < 0) {
        reply_nfs(q, RX_NFSERR_STALE);
        return;
    }
    /* A full, read-only file system the size of the library's files. */
    RXVTreeStats s;
    rx_vtree_stats(q->n->tree, &s);
    uint64_t blocks = (s.total_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    put_accepted(&q->w, q->xid, RX_RPC_SUCCESS);
    rx_wr_u32(&q->w, RX_NFS_OK);
    rx_wr_u32(&q->w, TSIZE);
    rx_wr_u32(&q->w, BLOCK_SIZE);
    rx_wr_u32(&q->w, (uint32_t)RX_MIN(blocks, (uint64_t)UINT32_MAX));
    rx_wr_u32(&q->w, 0);
    rx_wr_u32(&q->w, 0);
}

static void nfs_call(Req *q, uint32_t proc)
{
    switch (proc) {
    case RX_NFSPROC_NULL:
        reply_rpc(q, RX_RPC_SUCCESS);
        return;
    case RX_NFSPROC_GETATTR:
        nfs_getattr(q);
        return;
    case RX_NFSPROC_LOOKUP:
        nfs_lookup(q);
        return;
    case RX_NFSPROC_READ:
        nfs_read(q);
        return;
    case RX_NFSPROC_READDIR:
        nfs_readdir(q);
        return;
    case RX_NFSPROC_STATFS:
        nfs_statfs(q);
        return;
    case RX_NFSPROC_SETATTR:
    case RX_NFSPROC_WRITE:
    case RX_NFSPROC_CREATE:
    case RX_NFSPROC_REMOVE:
    case RX_NFSPROC_RENAME:
    case RX_NFSPROC_LINK:
    case RX_NFSPROC_SYMLINK:
    case RX_NFSPROC_MKDIR:
    case RX_NFSPROC_RMDIR:
        /* Every write, whatever its arguments: the status alone. */
        reply_nfs(q, RX_NFSERR_ROFS);
        return;
    default:
        /* ROOT and WRITECACHE (never used, RFC 1094 2.2.4, 2.2.8),
         * READLINK (no links here) and anything past STATFS. */
        reply_rpc(q, RX_RPC_PROC_UNAVAIL);
        return;
    }
}

/* ------------------------------------------------------------------ calls */

/* An AUTH_UNIX credential is read to check it is one (RFC 1057 9.2), then
 * ignored: NFS version 2 takes a client's word, and players send uid 0
 * (rbxport's fake CDJ). AUTH_NULL is taken as it is; nothing else is. */
static bool auth_ok(uint32_t flavor, const uint8_t *body, uint32_t len)
{
    if (len > AUTH_MAX)
        return false;
    if (flavor == AUTH_NULL)
        return true;
    if (flavor != AUTH_UNIX)
        return false;
    RXRd a = rx_rd(body, len);
    rx_rd_u32(&a);                          /* stamp */
    uint32_t name = rx_rd_u32(&a);
    if (a.err || name > UNIX_NAME_MAX)
        return false;
    opaque_body(&a, name);
    rx_rd_u32(&a);                          /* uid */
    rx_rd_u32(&a);                          /* gid */
    uint32_t gids = rx_rd_u32(&a);
    if (a.err || gids > UNIX_GIDS_MAX)
        return false;
    rx_rd_skip(&a, 4 * (size_t)gids);
    return !a.err;
}

/* Answers one call into q->w: false when nothing is to be sent. */
static bool answer(Req *q, enum RXNfsService svc, const uint8_t *req, size_t len)
{
    static const uint32_t progs[3][2] = {
        [RX_NFS_PORTMAP] = { RX_RPC_PORTMAP, RX_RPC_PORTMAP_VERSION },
        [RX_NFS_MOUNT]   = { RX_RPC_MOUNT,   RX_RPC_MOUNT_VERSION },
        [RX_NFS_NFS]     = { RX_RPC_NFS,     RX_RPC_NFS_VERSION },
    };

    /* call_body (RFC 1057 8): a datagram that is not a call, or whose
     * header up to the arguments is not all there, gets nothing. */
    RXRd r = rx_rd(req, len);
    q->xid = rx_rd_u32(&r);
    uint32_t mtype = rx_rd_u32(&r), rpcvers = rx_rd_u32(&r);
    if (r.err || mtype != RPC_CALL)
        return false;
    if (rpcvers != RPC_VERSION) {
        put_denied(&q->w, q->xid, REJECT_RPC_MISMATCH, RPC_VERSION, RPC_VERSION, true);
        q->small = true;
        return true;
    }
    uint32_t prog = rx_rd_u32(&r), vers = rx_rd_u32(&r), proc = rx_rd_u32(&r);
    uint32_t cred = rx_rd_u32(&r), cred_len = rx_rd_u32(&r);
    const uint8_t *cred_body = r.err ? NULL : opaque_body(&r, cred_len);
    rx_rd_u32(&r);
    uint32_t verf_len = rx_rd_u32(&r);
    if (!r.err)
        opaque_body(&r, verf_len);
    if (r.err)
        return false;
    if (!auth_ok(cred, cred_body, cred_len) || verf_len > AUTH_MAX) {
        put_denied(&q->w, q->xid, REJECT_AUTH_ERROR, AUTH_BADCRED, 0, false);
        q->small = true;
        return true;
    }
    q->args = rx_rd(r.p + r.pos, r.len - r.pos);
    q->prog = prog;
    q->proc = proc;

    if (prog == PROG_NLM || prog == PROG_NSM)
        return false;
    if (prog != progs[svc][0]) {
        reply_rpc(q, RX_RPC_PROG_UNAVAIL);
    } else if (vers != progs[svc][1]) {
        put_accepted(&q->w, q->xid, RX_RPC_PROG_MISMATCH);
        rx_wr_u32(&q->w, progs[svc][1]);
        rx_wr_u32(&q->w, progs[svc][1]);
        q->small = true;
    } else if (svc == RX_NFS_PORTMAP) {
        pmap_call(q, proc);
    } else if (svc == RX_NFS_MOUNT) {
        mount_call(q, proc);
    } else {
        nfs_call(q, proc);
    }
    return true;
}

ssize_t rx_nfs_handle(RXNfs *n, enum RXNfsService svc, uint32_t peer,
                      const uint8_t *req, size_t len, uint8_t *reply, size_t cap)
{
    if (!n || (!req && len) || !reply || (unsigned)svc > RX_NFS_NFS)
        return RX_ERROR(EINVAL);
    n->clock++;
    Req q = { .n = n, .peer = peer, .w = rx_wr(reply, cap) };
    bool any = answer(&q, svc, req, len);
    if (rx_log_level() >= RX_LOG_DEBUG) {
        /* The accept status and the first word of the result. */
        const uint8_t *o = q.w.p;
        rx_debug("link: rpc %u.%u from %u.%u.%u.%u %s: %s accept %d, status %d, %zu bytes",
                 q.prog, q.proc, peer >> 24, peer >> 16 & 0xFF, peer >> 8 & 0xFF, peer & 0xFF,
                 q.note, any ? "" : "(no answer)", q.w.len >= 24 ? (int)be32(o + 20) : -1,
                 q.w.len >= 28 ? (int)be32(o + 24) : -1, q.w.len);
    }
    if (!any)
        return 0;
    if (q.w.err)
        return RX_ERROR(ENOBUFS);
    /* No amplification: what answers the unknown or malformed is never
     * larger than what asked. */
    if (q.small && q.w.len > len)
        return 0;
    return (ssize_t)q.w.len;
}
