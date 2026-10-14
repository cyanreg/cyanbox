/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A player's side of the file server: the call encoders and reply
 * decoders (pure, bounded by RXRd, fuzzed with the server), and a small
 * blocking client over one UDP socket for the fake player and the tests.
 * It does what a player does to load a track, as rbxport's fake CDJ
 * (written against rekordbox and a CDJ-3000) and Deep Symmetry's
 * FileFetcher (written against players' own servers) do it: GETPORT on
 * portmap for mount and NFS, EXPORT then MNT of "/", LOOKUP of the
 * track-info path name by name in UTF-16LE, then READ in 32 kB pieces up
 * to the size the attributes give.
 *
 * A reply is only taken from the server's address and port and with the
 * call's xid; a call that is not answered is sent again with the same
 * xid, a few times, then -ETIMEDOUT. Nothing is allocated from a length
 * the server sent: a file's bytes are kept as they arrive. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rxbox/common.h"

#include "nfs.h"

#define RPC_CALL        0
#define RPC_REPLY       1
#define RPC_VERSION     2
#define MSG_ACCEPTED    0
#define MSG_DENIED      1
#define AUTH_NULL       0
#define AUTH_UNIX       1
#define AUTH_MAX        400
#define PROTO_UDP       17
/* The stamp a CDJ puts in its AUTH_UNIX credential (rbxport, rbl-nfs
 * rpc.rs, CDJ_AUTH_STAMP). */
#define CDJ_STAMP       0x967b8703u
/* A CDJ-3000's READ size: 679 of 694 reads in rbxport's capture
 * (6d4161a0). */
#define PLAYER_READ     32768
#define CALL_MAX        1280            /* the longest call: a LOOKUP of 255 units */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static size_t pad4(size_t n)
{
    return (n + 3) & ~(size_t)3;
}

/* --------------------------------------------------------------- encoders */

void rx_nfs_put_call(RXWr *w, uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc)
{
    rx_wr_u32(w, xid);
    rx_wr_u32(w, RPC_CALL);
    rx_wr_u32(w, RPC_VERSION);
    rx_wr_u32(w, prog);
    rx_wr_u32(w, vers);
    rx_wr_u32(w, proc);
    /* authunix_parms: stamp, machine name "", uid 0, gid 0, no groups. */
    rx_wr_u32(w, AUTH_UNIX);
    rx_wr_u32(w, 20);
    rx_wr_u32(w, CDJ_STAMP);
    rx_wr_u32(w, 0);
    rx_wr_u32(w, 0);
    rx_wr_u32(w, 0);
    rx_wr_u32(w, 0);
    rx_wr_u32(w, AUTH_NULL);
    rx_wr_u32(w, 0);
}

void rx_nfs_put_handle(RXWr *w, const uint8_t fh[RX_NFS_FHSIZE])
{
    rx_wr_bytes(w, fh, RX_NFS_FHSIZE);
}

/* --------------------------------------------------------------- decoders */

int rx_nfs_get_reply(const uint8_t *p, size_t len, uint32_t *xid, RXRd *res)
{
    RXRd r = rx_rd(p, len);
    uint32_t x = rx_rd_u32(&r), mtype = rx_rd_u32(&r), stat = rx_rd_u32(&r);
    if (r.err || mtype != RPC_REPLY)
        return RX_ERROR(EBADMSG);
    if (xid)
        *xid = x;
    if (stat == MSG_DENIED) {
        /* rejected_reply: RPC_MISMATCH (0) or AUTH_ERROR (1). */
        uint32_t why = rx_rd_u32(&r);
        return r.err || why > 1 ? RX_ERROR(EBADMSG) : why ? RX_ERROR(EACCES) : RX_ERROR(EPROTO);
    }
    if (stat != MSG_ACCEPTED)
        return RX_ERROR(EBADMSG);
    rx_rd_u32(&r);
    uint32_t vlen = rx_rd_u32(&r);
    if (r.err || vlen > AUTH_MAX || vlen > rx_rd_left(&r))
        return RX_ERROR(EBADMSG);
    rx_rd_skip(&r, pad4(vlen));
    uint32_t accept = rx_rd_u32(&r);
    if (r.err)
        return RX_ERROR(EBADMSG);
    switch (accept) {
    case RX_RPC_SUCCESS:
        if (res)
            *res = rx_rd(r.p + r.pos, r.len - r.pos);
        return 0;
    case RX_RPC_PROG_UNAVAIL:   return RX_ERROR(EPROTONOSUPPORT);
    case RX_RPC_PROG_MISMATCH:  return RX_ERROR(EPROTO);
    case RX_RPC_PROC_UNAVAIL:   return RX_ERROR(EOPNOTSUPP);
    case RX_RPC_GARBAGE_ARGS:   return RX_ERROR(EINVAL);
    case RX_RPC_SYSTEM_ERR:     return RX_ERROR(EIO);
    default:                    return RX_ERROR(EBADMSG);
    }
}

/* nfsstat's numbers are BSD's errno values (RFC 1094 2.3.1); Linux's
 * differ for some, so each is named. */
int rx_nfs_status_errno(uint32_t stat)
{
    switch (stat) {
    case RX_NFS_OK:             return 0;
    case RX_NFSERR_PERM:        return RX_ERROR(EPERM);
    case RX_NFSERR_NOENT:       return RX_ERROR(ENOENT);
    case RX_NFSERR_IO:          return RX_ERROR(EIO);
    case 6:                     return RX_ERROR(ENXIO);
    case RX_NFSERR_ACCES:       return RX_ERROR(EACCES);
    case 17:                    return RX_ERROR(EEXIST);
    case 19:                    return RX_ERROR(ENODEV);
    case RX_NFSERR_NOTDIR:      return RX_ERROR(ENOTDIR);
    case RX_NFSERR_ISDIR:       return RX_ERROR(EISDIR);
    case 27:                    return RX_ERROR(EFBIG);
    case 28:                    return RX_ERROR(ENOSPC);
    case RX_NFSERR_ROFS:        return RX_ERROR(EROFS);
    case RX_NFSERR_NAMETOOLONG: return RX_ERROR(ENAMETOOLONG);
    case 66:                    return RX_ERROR(ENOTEMPTY);
    case 69:                    return RX_ERROR(EDQUOT);
    case RX_NFSERR_STALE:       return RX_ERROR(ESTALE);
    default:                    return RX_ERROR(EPROTO);
    }
}

int rx_nfs_get_status(RXRd *r)
{
    uint32_t stat = rx_rd_u32(r);
    return r->err ? RX_ERROR(EBADMSG) : rx_nfs_status_errno(stat);
}

int rx_nfs_get_handle(RXRd *r, uint8_t fh[RX_NFS_FHSIZE])
{
    const uint8_t *p = rx_rd_bytes(r, RX_NFS_FHSIZE);
    if (!p)
        return RX_ERROR(EBADMSG);
    memcpy(fh, p, RX_NFS_FHSIZE);
    return 0;
}

int rx_nfs_get_fattr(RXRd *r, RXNfsAttr *a)
{
    RXNfsAttr v = {
        .type      = rx_rd_u32(r),
        .mode      = rx_rd_u32(r),
        .nlink     = rx_rd_u32(r),
        .uid       = rx_rd_u32(r),
        .gid       = rx_rd_u32(r),
        .size      = rx_rd_u32(r),
        .blocksize = rx_rd_u32(r),
        .rdev      = rx_rd_u32(r),
        .blocks    = rx_rd_u32(r),
        .fsid      = rx_rd_u32(r),
        .fileid    = rx_rd_u32(r),
    };
    uint32_t *times[3] = { &v.atime, &v.mtime, &v.ctime };
    for (int i = 0; i < 3; i++) {
        *times[i] = rx_rd_u32(r);
        rx_rd_u32(r);
    }
    if (r->err)
        return RX_ERROR(EBADMSG);
    if (a)
        *a = v;
    return 0;
}

int rx_nfs_get_entries(RXRd *r, rx_nfs_entry_cb cb, void *opaque, bool *eof)
{
    while (1) {
        uint32_t follows = rx_rd_u32(r);
        if (r->err || follows > 1)
            return RX_ERROR(EBADMSG);
        if (!follows)
            break;
        uint32_t fileid = rx_rd_u32(r);
        char name[RX_NFS_NAME_MAX * 3 + 1];
        int ret = rx_nfs_get_name(r, RX_NFS_NAME_MAX, name, sizeof(name));
        uint32_t cookie = rx_rd_u32(r);
        if (ret < 0)
            return ret;
        if (r->err)
            return RX_ERROR(EBADMSG);
        if (cb && (ret = cb(opaque, fileid, name, cookie)) < 0)
            return ret;
    }
    uint32_t e = rx_rd_u32(r);
    if (r->err)
        return RX_ERROR(EBADMSG);
    if (eof)
        *eof = e != 0;
    return 0;
}

int rx_nfs_get_exports(RXRd *r, rx_nfs_export_cb cb, void *opaque)
{
    while (1) {
        uint32_t follows = rx_rd_u32(r);
        if (r->err || follows > 1)
            return RX_ERROR(EBADMSG);
        if (!follows)
            return 0;
        char dir[512 * 3 + 1], group[256];
        int ret = rx_nfs_get_name(r, 512, dir, sizeof(dir));
        if (ret < 0)
            return ret;
        /* groups: names<MNTNAMLEN> in ASCII; the first one kept. */
        bool have = false;
        while (1) {
            uint32_t more = rx_rd_u32(r);
            if (r->err || more > 1)
                return RX_ERROR(EBADMSG);
            if (!more)
                break;
            uint32_t len = rx_rd_u32(r);
            if (r->err || len > 255 || len > rx_rd_left(r))
                return RX_ERROR(EBADMSG);
            const uint8_t *p = rx_rd_bytes(r, pad4(len));
            if (!p)
                return RX_ERROR(EBADMSG);
            if (!have) {
                memcpy(group, p, len);
                group[len] = '\0';
                have = true;
            }
        }
        if (cb && (ret = cb(opaque, dir, have ? group : NULL)) < 0)
            return ret;
    }
}

/* ----------------------------------------------------------------- client */

struct RXNfsClient {
    int       fd;
    uint32_t  server;
    uint16_t  ports[3];         /* by RXNfsService */
    int       timeout_ms, tries;
    uint32_t  xid;
    uint8_t   root[RX_NFS_FHSIZE];
    bool      mounted;
    uint8_t  *buf;              /* a reply: RX_NFS_MAX_DATAGRAM */
};

int rx_nfs_client_call(RXNfsClient *c, enum RXNfsService svc, const uint8_t *req, size_t len,
                       uint8_t *reply, size_t cap)
{
    if (!c || !req || len < 4 || !reply || (unsigned)svc > RX_NFS_NFS)
        return RX_ERROR(EINVAL);
    if (!c->ports[svc])
        return RX_ERROR(ENOTCONN);
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port   = htons(c->ports[svc]),
        .sin_addr   = { htonl(c->server) },
    };
    uint32_t xid = be32(req);
    for (int t = 0; t < c->tries; t++) {
        if (sendto(c->fd, req, len, 0, (const struct sockaddr *)&to, sizeof(to)) < 0)
            return RX_ERROR(errno);
        int64_t end = rx_time_us() + (int64_t)c->timeout_ms * 1000;
        while (1) {
            int64_t left = end - rx_time_us();
            if (left <= 0)
                break;
            struct pollfd p = { .fd = c->fd, .events = POLLIN };
            int r = poll(&p, 1, (int)((left + 999) / 1000));
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0)
                return RX_ERROR(errno);
            if (!r)
                break;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(c->fd, reply, cap, MSG_TRUNC, (struct sockaddr *)&from, &fl);
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n < 0)
                return RX_ERROR(errno);
            /* Only the server's answer to this call, whole. */
            if (from.sin_addr.s_addr != to.sin_addr.s_addr || from.sin_port != to.sin_port ||
                (size_t)n > cap || n < 4 || be32(reply) != xid)
                continue;
            return (int)n;
        }
    }
    return RX_ERROR(ETIMEDOUT);
}

/* Sends the call in `w` and finds the results of its reply. */
static int rpc(RXNfsClient *c, enum RXNfsService svc, RXWr *w, RXRd *res)
{
    if (w->err)
        return RX_ERROR(ENAMETOOLONG);
    int n = rx_nfs_client_call(c, svc, w->p, w->len, c->buf, RX_NFS_MAX_DATAGRAM);
    return n < 0 ? n : rx_nfs_get_reply(c->buf, (size_t)n, NULL, res);
}

static int getport(RXNfsClient *c, uint32_t prog, uint32_t vers, uint16_t *port)
{
    uint8_t b[CALL_MAX];
    RXWr w = rx_wr(b, sizeof(b));
    rx_nfs_put_call(&w, ++c->xid, RX_RPC_PORTMAP, RX_RPC_PORTMAP_VERSION, RX_PMAP_GETPORT);
    rx_wr_u32(&w, prog);
    rx_wr_u32(&w, vers);
    rx_wr_u32(&w, PROTO_UDP);
    rx_wr_u32(&w, 0);
    RXRd r;
    int ret = rpc(c, RX_NFS_PORTMAP, &w, &r);
    if (ret < 0)
        return ret;
    uint32_t p = rx_rd_u32(&r);
    if (r.err || p > 65535)
        return RX_ERROR(EBADMSG);
    if (!p)
        return RX_ERROR(EPROTONOSUPPORT);
    *port = (uint16_t)p;
    return 0;
}

int rx_nfs_client_open(RXNfsClient **out, const RXNfsClientOptions *o)
{
    if (!out || !o || !o->portmap_port)
        return RX_ERROR(EINVAL);
    *out = NULL;
    RXNfsClient *c = calloc(1, sizeof(*c));
    if (!c)
        return RX_ERROR(ENOMEM);
    c->fd = -1;
    c->server = o->server;
    c->ports[RX_NFS_PORTMAP] = o->portmap_port;
    c->timeout_ms = o->timeout_ms > 0 ? o->timeout_ms : 500;
    c->tries = o->tries > 0 ? o->tries : 4;
    if (getrandom(&c->xid, sizeof(c->xid), GRND_NONBLOCK) != (ssize_t)sizeof(c->xid))
        c->xid = (uint32_t)rx_time_us();
    int ret = 0;
    c->buf = malloc(RX_NFS_MAX_DATAGRAM);
    if (!c->buf) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    c->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_addr   = { htonl(o->local) },
    };
    if (c->fd < 0 || bind(c->fd, (const struct sockaddr *)&local, sizeof(local)) < 0) {
        ret = RX_ERROR(errno);
        goto fail;
    }
    /* Room for a whole READ answer as it arrives. */
    int size = 4 * RX_NFS_MAX_DATAGRAM;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if ((ret = getport(c, RX_RPC_MOUNT, RX_RPC_MOUNT_VERSION, &c->ports[RX_NFS_MOUNT])) < 0 ||
        (ret = getport(c, RX_RPC_NFS, RX_RPC_NFS_VERSION, &c->ports[RX_NFS_NFS])) < 0)
        goto fail;
    *out = c;
    return 0;

fail:
    rx_nfs_client_free(&c);
    return ret;
}

void rx_nfs_client_free(RXNfsClient **pc)
{
    if (!pc || !*pc)
        return;
    RXNfsClient *c = *pc;
    if (c->fd >= 0)
        close(c->fd);
    free(c->buf);
    free(c);
    *pc = NULL;
}

void rx_nfs_client_ports(const RXNfsClient *c, uint16_t *mount, uint16_t *nfs)
{
    if (mount)
        *mount = c->ports[RX_NFS_MOUNT];
    if (nfs)
        *nfs = c->ports[RX_NFS_NFS];
}

const uint8_t *rx_nfs_client_root(const RXNfsClient *c)
{
    return c && c->mounted ? c->root : NULL;
}

typedef struct FindExport {
    const char *dir;
    bool found;
} FindExport;

static int find_export(void *opaque, const char *dir, const char *group)
{
    FindExport *f = opaque;
    f->found |= !strcmp(dir, f->dir);
    return 0;
}

int rx_nfs_client_mount(RXNfsClient *c, const char *dir)
{
    if (!c || !dir)
        return RX_ERROR(EINVAL);
    /* EXPORT first: a CDJ-3000 reads the list before it mounts
     * (rbxport 1605b376). */
    uint8_t b[CALL_MAX];
    RXWr w = rx_wr(b, sizeof(b));
    rx_nfs_put_call(&w, ++c->xid, RX_RPC_MOUNT, RX_RPC_MOUNT_VERSION, RX_MNT_EXPORT);
    RXRd r;
    int ret = rpc(c, RX_NFS_MOUNT, &w, &r);
    FindExport f = { .dir = dir };
    if (ret < 0 || (ret = rx_nfs_get_exports(&r, find_export, &f)) < 0)
        return ret;
    if (!f.found)
        return RX_ERROR(ENOENT);

    w = rx_wr(b, sizeof(b));
    rx_nfs_put_call(&w, ++c->xid, RX_RPC_MOUNT, RX_RPC_MOUNT_VERSION, RX_MNT_MNT);
    rx_nfs_put_name(&w, dir);
    if ((ret = rpc(c, RX_NFS_MOUNT, &w, &r)) < 0 || (ret = rx_nfs_get_status(&r)) < 0 ||
        (ret = rx_nfs_get_handle(&r, c->root)) < 0)
        return ret;
    c->mounted = true;
    return 0;
}

int rx_nfs_client_lookup(RXNfsClient *c, const uint8_t dir[RX_NFS_FHSIZE], const char *name,
                         uint8_t fh[RX_NFS_FHSIZE], RXNfsAttr *a)
{
    if (!c || !dir || !name || !fh)
        return RX_ERROR(EINVAL);
    uint8_t b[CALL_MAX];
    RXWr w = rx_wr(b, sizeof(b));
    rx_nfs_put_call(&w, ++c->xid, RX_RPC_NFS, RX_RPC_NFS_VERSION, RX_NFSPROC_LOOKUP);
    rx_nfs_put_handle(&w, dir);
    rx_nfs_put_name(&w, name);
    RXRd r;
    int ret = rpc(c, RX_NFS_NFS, &w, &r);
    if (ret < 0 || (ret = rx_nfs_get_status(&r)) < 0 || (ret = rx_nfs_get_handle(&r, fh)) < 0)
        return ret;
    return rx_nfs_get_fattr(&r, a);
}

int rx_nfs_client_getattr(RXNfsClient *c, const uint8_t fh[RX_NFS_FHSIZE], RXNfsAttr *a)
{
    if (!c || !fh)
        return RX_ERROR(EINVAL);
    uint8_t b[CALL_MAX];
    RXWr w = rx_wr(b, sizeof(b));
    rx_nfs_put_call(&w, ++c->xid, RX_RPC_NFS, RX_RPC_NFS_VERSION, RX_NFSPROC_GETATTR);
    rx_nfs_put_handle(&w, fh);
    RXRd r;
    int ret = rpc(c, RX_NFS_NFS, &w, &r);
    if (ret < 0 || (ret = rx_nfs_get_status(&r)) < 0)
        return ret;
    return rx_nfs_get_fattr(&r, a);
}

int rx_nfs_client_walk(RXNfsClient *c, const char *path, uint8_t fh[RX_NFS_FHSIZE], RXNfsAttr *a)
{
    if (!c || !path || !fh)
        return RX_ERROR(EINVAL);
    if (!c->mounted)
        return RX_ERROR(ENOTCONN);
    uint8_t at[RX_NFS_FHSIZE];
    memcpy(at, c->root, sizeof(at));
    RXNfsAttr attr;
    bool any = false;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        const char *e = strchr(path, '/');
        size_t len = e ? (size_t)(e - path) : strlen(path);
        char name[RX_NFS_NAME_MAX * 4 + 1];
        if (len >= sizeof(name))
            return RX_ERROR(ENAMETOOLONG);
        memcpy(name, path, len);
        name[len] = '\0';
        int ret = rx_nfs_client_lookup(c, at, name, at, &attr);
        if (ret < 0)
            return ret;
        any = true;
        path += len;
    }
    if (!any) {
        int ret = rx_nfs_client_getattr(c, at, &attr);
        if (ret < 0)
            return ret;
    }
    memcpy(fh, at, RX_NFS_FHSIZE);
    if (a)
        *a = attr;
    return 0;
}

int rx_nfs_client_read(RXNfsClient *c, const uint8_t fh[RX_NFS_FHSIZE], uint32_t offset,
                       uint32_t count, uint8_t *buf, RXNfsAttr *a)
{
    if (!c || !fh || (!buf && count))
        return RX_ERROR(EINVAL);
    uint8_t b[CALL_MAX];
    RXWr w = rx_wr(b, sizeof(b));
    rx_nfs_put_call(&w, ++c->xid, RX_RPC_NFS, RX_RPC_NFS_VERSION, RX_NFSPROC_READ);
    rx_nfs_put_handle(&w, fh);
    rx_wr_u32(&w, offset);
    rx_wr_u32(&w, count);
    rx_wr_u32(&w, 0);
    RXRd r;
    int ret = rpc(c, RX_NFS_NFS, &w, &r);
    if (ret < 0 || (ret = rx_nfs_get_status(&r)) < 0 || (ret = rx_nfs_get_fattr(&r, a)) < 0)
        return ret;
    uint32_t n = rx_rd_u32(&r);
    if (r.err || n > count || n > rx_rd_left(&r))
        return RX_ERROR(EBADMSG);
    const uint8_t *p = rx_rd_bytes(&r, pad4(n));
    if (!p)
        return RX_ERROR(EBADMSG);
    if (n)
        memcpy(buf, p, n);
    return (int)n;
}

int rx_nfs_client_read_file(RXNfsClient *c, const char *path, uint32_t chunk,
                            rx_nfs_data_cb cb, void *opaque)
{
    if (!c || !path || !cb)
        return RX_ERROR(EINVAL);
    chunk = chunk ? RX_MIN(chunk, (uint32_t)RX_NFS_MAX_READ) : PLAYER_READ;
    uint8_t fh[RX_NFS_FHSIZE];
    RXNfsAttr a;
    int ret = rx_nfs_client_walk(c, path, fh, &a);
    if (ret < 0)
        return ret;
    if (a.type != 1)
        return RX_ERROR(EISDIR);
    uint8_t *buf = malloc(chunk);
    if (!buf)
        return RX_ERROR(ENOMEM);
    /* Up to the size the server gives, which each READ's attributes say
     * again (rbxport's fake CDJ reads to it, as a player does). */
    uint64_t off = 0, size = a.size;
    while (off < size) {
        uint32_t want = (uint32_t)RX_MIN((uint64_t)chunk, size - off);
        int n = rx_nfs_client_read(c, fh, (uint32_t)off, want, buf, &a);
        if (n <= 0) {
            ret = n < 0 ? n : RX_ERROR(EIO);
            break;
        }
        if ((ret = cb(opaque, buf, (size_t)n, off)) < 0)
            break;
        off += (uint64_t)n;
        size = a.size;
    }
    free(buf);
    return ret < 0 ? ret : 0;
}

typedef struct Fetch {
    uint8_t *data;
    size_t   size, cap;
} Fetch;

static int fetch_cb(void *opaque, const uint8_t *data, size_t n, uint64_t offset)
{
    Fetch *f = opaque;
    if (n > f->cap - f->size) {
        size_t cap = RX_MAX(f->cap * 2, f->size + n);
        uint8_t *p = realloc(f->data, cap);
        if (!p)
            return RX_ERROR(ENOMEM);
        f->data = p;
        f->cap = cap;
    }
    memcpy(f->data + f->size, data, n);
    f->size += n;
    return 0;
}

int rx_nfs_client_fetch(RXNfsClient *c, const char *path, uint32_t chunk,
                        uint8_t **data, size_t *size)
{
    if (!data || !size)
        return RX_ERROR(EINVAL);
    Fetch f = { 0 };
    int ret = rx_nfs_client_read_file(c, path, chunk, fetch_cb, &f);
    if (ret < 0) {
        free(f.data);
        return ret;
    }
    *data = f.data ? f.data : malloc(1);
    *size = f.size;
    return *data ? 0 : RX_ERROR(ENOMEM);
}
