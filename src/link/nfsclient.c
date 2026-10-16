/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A player's side of the file server: the call encoders and reply
 * decoders (pure, bounded by CBRd, fuzzed with the server), and a small
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

#include "cyanbox/common.h"

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

void cb_nfs_put_call(CBWr *w, uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc)
{
    cb_wr_u32(w, xid);
    cb_wr_u32(w, RPC_CALL);
    cb_wr_u32(w, RPC_VERSION);
    cb_wr_u32(w, prog);
    cb_wr_u32(w, vers);
    cb_wr_u32(w, proc);
    /* authunix_parms: stamp, machine name "", uid 0, gid 0, no groups. */
    cb_wr_u32(w, AUTH_UNIX);
    cb_wr_u32(w, 20);
    cb_wr_u32(w, CDJ_STAMP);
    cb_wr_u32(w, 0);
    cb_wr_u32(w, 0);
    cb_wr_u32(w, 0);
    cb_wr_u32(w, 0);
    cb_wr_u32(w, AUTH_NULL);
    cb_wr_u32(w, 0);
}

void cb_nfs_put_handle(CBWr *w, const uint8_t fh[CB_NFS_FHSIZE])
{
    cb_wr_bytes(w, fh, CB_NFS_FHSIZE);
}

/* --------------------------------------------------------------- decoders */

int cb_nfs_get_reply(const uint8_t *p, size_t len, uint32_t *xid, CBRd *res)
{
    CBRd r = cb_rd(p, len);
    uint32_t x = cb_rd_u32(&r), mtype = cb_rd_u32(&r), stat = cb_rd_u32(&r);
    if (r.err || mtype != RPC_REPLY)
        return CB_ERROR(EBADMSG);
    if (xid)
        *xid = x;
    if (stat == MSG_DENIED) {
        /* rejected_reply: RPC_MISMATCH (0) or AUTH_ERROR (1). */
        uint32_t why = cb_rd_u32(&r);
        return r.err || why > 1 ? CB_ERROR(EBADMSG) : why ? CB_ERROR(EACCES) : CB_ERROR(EPROTO);
    }
    if (stat != MSG_ACCEPTED)
        return CB_ERROR(EBADMSG);
    cb_rd_u32(&r);
    uint32_t vlen = cb_rd_u32(&r);
    if (r.err || vlen > AUTH_MAX || vlen > cb_rd_left(&r))
        return CB_ERROR(EBADMSG);
    cb_rd_skip(&r, pad4(vlen));
    uint32_t accept = cb_rd_u32(&r);
    if (r.err)
        return CB_ERROR(EBADMSG);
    switch (accept) {
    case CB_RPC_SUCCESS:
        if (res)
            *res = cb_rd(r.p + r.pos, r.len - r.pos);
        return 0;
    case CB_RPC_PROG_UNAVAIL:   return CB_ERROR(EPROTONOSUPPORT);
    case CB_RPC_PROG_MISMATCH:  return CB_ERROR(EPROTO);
    case CB_RPC_PROC_UNAVAIL:   return CB_ERROR(EOPNOTSUPP);
    case CB_RPC_GARBAGE_ARGS:   return CB_ERROR(EINVAL);
    case CB_RPC_SYSTEM_ERR:     return CB_ERROR(EIO);
    default:                    return CB_ERROR(EBADMSG);
    }
}

/* nfsstat's numbers are BSD's errno values (RFC 1094 2.3.1); Linux's
 * differ for some, so each is named. */
int cb_nfs_status_errno(uint32_t stat)
{
    switch (stat) {
    case CB_NFS_OK:             return 0;
    case CB_NFSERR_PERM:        return CB_ERROR(EPERM);
    case CB_NFSERR_NOENT:       return CB_ERROR(ENOENT);
    case CB_NFSERR_IO:          return CB_ERROR(EIO);
    case 6:                     return CB_ERROR(ENXIO);
    case CB_NFSERR_ACCES:       return CB_ERROR(EACCES);
    case 17:                    return CB_ERROR(EEXIST);
    case 19:                    return CB_ERROR(ENODEV);
    case CB_NFSERR_NOTDIR:      return CB_ERROR(ENOTDIR);
    case CB_NFSERR_ISDIR:       return CB_ERROR(EISDIR);
    case 27:                    return CB_ERROR(EFBIG);
    case 28:                    return CB_ERROR(ENOSPC);
    case CB_NFSERR_ROFS:        return CB_ERROR(EROFS);
    case CB_NFSERR_NAMETOOLONG: return CB_ERROR(ENAMETOOLONG);
    case 66:                    return CB_ERROR(ENOTEMPTY);
    case 69:                    return CB_ERROR(EDQUOT);
    case CB_NFSERR_STALE:       return CB_ERROR(ESTALE);
    default:                    return CB_ERROR(EPROTO);
    }
}

int cb_nfs_get_status(CBRd *r)
{
    uint32_t stat = cb_rd_u32(r);
    return r->err ? CB_ERROR(EBADMSG) : cb_nfs_status_errno(stat);
}

int cb_nfs_get_handle(CBRd *r, uint8_t fh[CB_NFS_FHSIZE])
{
    const uint8_t *p = cb_rd_bytes(r, CB_NFS_FHSIZE);
    if (!p)
        return CB_ERROR(EBADMSG);
    memcpy(fh, p, CB_NFS_FHSIZE);
    return 0;
}

int cb_nfs_get_fattr(CBRd *r, CBNfsAttr *a)
{
    CBNfsAttr v = {
        .type      = cb_rd_u32(r),
        .mode      = cb_rd_u32(r),
        .nlink     = cb_rd_u32(r),
        .uid       = cb_rd_u32(r),
        .gid       = cb_rd_u32(r),
        .size      = cb_rd_u32(r),
        .blocksize = cb_rd_u32(r),
        .rdev      = cb_rd_u32(r),
        .blocks    = cb_rd_u32(r),
        .fsid      = cb_rd_u32(r),
        .fileid    = cb_rd_u32(r),
    };
    uint32_t *times[3] = { &v.atime, &v.mtime, &v.ctime };
    for (int i = 0; i < 3; i++) {
        *times[i] = cb_rd_u32(r);
        cb_rd_u32(r);
    }
    if (r->err)
        return CB_ERROR(EBADMSG);
    if (a)
        *a = v;
    return 0;
}

int cb_nfs_get_entries(CBRd *r, cb_nfs_entry_cb cb, void *opaque, bool *eof)
{
    while (1) {
        uint32_t follows = cb_rd_u32(r);
        if (r->err || follows > 1)
            return CB_ERROR(EBADMSG);
        if (!follows)
            break;
        uint32_t fileid = cb_rd_u32(r);
        char name[CB_NFS_NAME_MAX * 3 + 1];
        int ret = cb_nfs_get_name(r, CB_NFS_NAME_MAX, name, sizeof(name));
        uint32_t cookie = cb_rd_u32(r);
        if (ret < 0)
            return ret;
        if (r->err)
            return CB_ERROR(EBADMSG);
        if (cb && (ret = cb(opaque, fileid, name, cookie)) < 0)
            return ret;
    }
    uint32_t e = cb_rd_u32(r);
    if (r->err)
        return CB_ERROR(EBADMSG);
    if (eof)
        *eof = e != 0;
    return 0;
}

int cb_nfs_get_exports(CBRd *r, cb_nfs_export_cb cb, void *opaque)
{
    while (1) {
        uint32_t follows = cb_rd_u32(r);
        if (r->err || follows > 1)
            return CB_ERROR(EBADMSG);
        if (!follows)
            return 0;
        char dir[512 * 3 + 1], group[256];
        int ret = cb_nfs_get_name(r, 512, dir, sizeof(dir));
        if (ret < 0)
            return ret;
        /* groups: names<MNTNAMLEN> in ASCII; the first one kept. */
        bool have = false;
        while (1) {
            uint32_t more = cb_rd_u32(r);
            if (r->err || more > 1)
                return CB_ERROR(EBADMSG);
            if (!more)
                break;
            uint32_t len = cb_rd_u32(r);
            if (r->err || len > 255 || len > cb_rd_left(r))
                return CB_ERROR(EBADMSG);
            const uint8_t *p = cb_rd_bytes(r, pad4(len));
            if (!p)
                return CB_ERROR(EBADMSG);
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

struct CBNfsClient {
    int       fd;
    uint32_t  server;
    uint16_t  ports[3];         /* by CBNfsService */
    int       timeout_ms, tries;
    uint32_t  xid;
    uint8_t   root[CB_NFS_FHSIZE];
    bool      mounted;
    uint8_t  *buf;              /* a reply: CB_NFS_MAX_DATAGRAM */
};

int cb_nfs_client_call(CBNfsClient *c, enum CBNfsService svc, const uint8_t *req, size_t len,
                       uint8_t *reply, size_t cap)
{
    if (!c || !req || len < 4 || !reply || (unsigned)svc > CB_NFS_NFS)
        return CB_ERROR(EINVAL);
    if (!c->ports[svc])
        return CB_ERROR(ENOTCONN);
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port   = htons(c->ports[svc]),
        .sin_addr   = { htonl(c->server) },
    };
    uint32_t xid = be32(req);
    for (int t = 0; t < c->tries; t++) {
        if (sendto(c->fd, req, len, 0, (const struct sockaddr *)&to, sizeof(to)) < 0)
            return CB_ERROR(errno);
        int64_t end = cb_time_us() + (int64_t)c->timeout_ms * 1000;
        while (1) {
            int64_t left = end - cb_time_us();
            if (left <= 0)
                break;
            struct pollfd p = { .fd = c->fd, .events = POLLIN };
            int r = poll(&p, 1, (int)((left + 999) / 1000));
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0)
                return CB_ERROR(errno);
            if (!r)
                break;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(c->fd, reply, cap, MSG_TRUNC, (struct sockaddr *)&from, &fl);
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n < 0)
                return CB_ERROR(errno);
            /* Only the server's answer to this call, whole. */
            if (from.sin_addr.s_addr != to.sin_addr.s_addr || from.sin_port != to.sin_port ||
                (size_t)n > cap || n < 4 || be32(reply) != xid)
                continue;
            return (int)n;
        }
    }
    return CB_ERROR(ETIMEDOUT);
}

/* Sends the call in `w` and finds the results of its reply. */
static int rpc(CBNfsClient *c, enum CBNfsService svc, CBWr *w, CBRd *res)
{
    if (w->err)
        return CB_ERROR(ENAMETOOLONG);
    int n = cb_nfs_client_call(c, svc, w->p, w->len, c->buf, CB_NFS_MAX_DATAGRAM);
    return n < 0 ? n : cb_nfs_get_reply(c->buf, (size_t)n, NULL, res);
}

static int getport(CBNfsClient *c, uint32_t prog, uint32_t vers, uint16_t *port)
{
    uint8_t b[CALL_MAX];
    CBWr w = cb_wr(b, sizeof(b));
    cb_nfs_put_call(&w, ++c->xid, CB_RPC_PORTMAP, CB_RPC_PORTMAP_VERSION, CB_PMAP_GETPORT);
    cb_wr_u32(&w, prog);
    cb_wr_u32(&w, vers);
    cb_wr_u32(&w, PROTO_UDP);
    cb_wr_u32(&w, 0);
    CBRd r;
    int ret = rpc(c, CB_NFS_PORTMAP, &w, &r);
    if (ret < 0)
        return ret;
    uint32_t p = cb_rd_u32(&r);
    if (r.err || p > 65535)
        return CB_ERROR(EBADMSG);
    if (!p)
        return CB_ERROR(EPROTONOSUPPORT);
    *port = (uint16_t)p;
    return 0;
}

int cb_nfs_client_open(CBNfsClient **out, const CBNfsClientOptions *o)
{
    if (!out || !o || !o->portmap_port)
        return CB_ERROR(EINVAL);
    *out = NULL;
    CBNfsClient *c = calloc(1, sizeof(*c));
    if (!c)
        return CB_ERROR(ENOMEM);
    c->fd = -1;
    c->server = o->server;
    c->ports[CB_NFS_PORTMAP] = o->portmap_port;
    c->timeout_ms = o->timeout_ms > 0 ? o->timeout_ms : 500;
    c->tries = o->tries > 0 ? o->tries : 4;
    if (getrandom(&c->xid, sizeof(c->xid), GRND_NONBLOCK) != (ssize_t)sizeof(c->xid))
        c->xid = (uint32_t)cb_time_us();
    int ret = 0;
    c->buf = malloc(CB_NFS_MAX_DATAGRAM);
    if (!c->buf) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    c->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_addr   = { htonl(o->local) },
    };
    if (c->fd < 0 || bind(c->fd, (const struct sockaddr *)&local, sizeof(local)) < 0) {
        ret = CB_ERROR(errno);
        goto fail;
    }
    /* Room for a whole READ answer as it arrives. */
    int size = 4 * CB_NFS_MAX_DATAGRAM;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if ((ret = getport(c, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, &c->ports[CB_NFS_MOUNT])) < 0 ||
        (ret = getport(c, CB_RPC_NFS, CB_RPC_NFS_VERSION, &c->ports[CB_NFS_NFS])) < 0)
        goto fail;
    *out = c;
    return 0;

fail:
    cb_nfs_client_free(&c);
    return ret;
}

void cb_nfs_client_free(CBNfsClient **pc)
{
    if (!pc || !*pc)
        return;
    CBNfsClient *c = *pc;
    if (c->fd >= 0)
        close(c->fd);
    free(c->buf);
    free(c);
    *pc = NULL;
}

void cb_nfs_client_ports(const CBNfsClient *c, uint16_t *mount, uint16_t *nfs)
{
    if (mount)
        *mount = c->ports[CB_NFS_MOUNT];
    if (nfs)
        *nfs = c->ports[CB_NFS_NFS];
}

const uint8_t *cb_nfs_client_root(const CBNfsClient *c)
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

int cb_nfs_client_mount(CBNfsClient *c, const char *dir)
{
    if (!c || !dir)
        return CB_ERROR(EINVAL);
    /* EXPORT first: a CDJ-3000 reads the list before it mounts
     * (rbxport 1605b376). */
    uint8_t b[CALL_MAX];
    CBWr w = cb_wr(b, sizeof(b));
    cb_nfs_put_call(&w, ++c->xid, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_EXPORT);
    CBRd r;
    int ret = rpc(c, CB_NFS_MOUNT, &w, &r);
    FindExport f = { .dir = dir };
    if (ret < 0 || (ret = cb_nfs_get_exports(&r, find_export, &f)) < 0)
        return ret;
    if (!f.found)
        return CB_ERROR(ENOENT);

    w = cb_wr(b, sizeof(b));
    cb_nfs_put_call(&w, ++c->xid, CB_RPC_MOUNT, CB_RPC_MOUNT_VERSION, CB_MNT_MNT);
    cb_nfs_put_name(&w, dir);
    if ((ret = rpc(c, CB_NFS_MOUNT, &w, &r)) < 0 || (ret = cb_nfs_get_status(&r)) < 0 ||
        (ret = cb_nfs_get_handle(&r, c->root)) < 0)
        return ret;
    c->mounted = true;
    return 0;
}

int cb_nfs_client_lookup(CBNfsClient *c, const uint8_t dir[CB_NFS_FHSIZE], const char *name,
                         uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a)
{
    if (!c || !dir || !name || !fh)
        return CB_ERROR(EINVAL);
    uint8_t b[CALL_MAX];
    CBWr w = cb_wr(b, sizeof(b));
    cb_nfs_put_call(&w, ++c->xid, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_LOOKUP);
    cb_nfs_put_handle(&w, dir);
    cb_nfs_put_name(&w, name);
    CBRd r;
    int ret = rpc(c, CB_NFS_NFS, &w, &r);
    if (ret < 0 || (ret = cb_nfs_get_status(&r)) < 0 || (ret = cb_nfs_get_handle(&r, fh)) < 0)
        return ret;
    return cb_nfs_get_fattr(&r, a);
}

int cb_nfs_client_getattr(CBNfsClient *c, const uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a)
{
    if (!c || !fh)
        return CB_ERROR(EINVAL);
    uint8_t b[CALL_MAX];
    CBWr w = cb_wr(b, sizeof(b));
    cb_nfs_put_call(&w, ++c->xid, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_GETATTR);
    cb_nfs_put_handle(&w, fh);
    CBRd r;
    int ret = rpc(c, CB_NFS_NFS, &w, &r);
    if (ret < 0 || (ret = cb_nfs_get_status(&r)) < 0)
        return ret;
    return cb_nfs_get_fattr(&r, a);
}

int cb_nfs_client_walk(CBNfsClient *c, const char *path, uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a)
{
    if (!c || !path || !fh)
        return CB_ERROR(EINVAL);
    if (!c->mounted)
        return CB_ERROR(ENOTCONN);
    uint8_t at[CB_NFS_FHSIZE];
    memcpy(at, c->root, sizeof(at));
    CBNfsAttr attr;
    bool any = false;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        const char *e = strchr(path, '/');
        size_t len = e ? (size_t)(e - path) : strlen(path);
        char name[CB_NFS_NAME_MAX * 4 + 1];
        if (len >= sizeof(name))
            return CB_ERROR(ENAMETOOLONG);
        memcpy(name, path, len);
        name[len] = '\0';
        int ret = cb_nfs_client_lookup(c, at, name, at, &attr);
        if (ret < 0)
            return ret;
        any = true;
        path += len;
    }
    if (!any) {
        int ret = cb_nfs_client_getattr(c, at, &attr);
        if (ret < 0)
            return ret;
    }
    memcpy(fh, at, CB_NFS_FHSIZE);
    if (a)
        *a = attr;
    return 0;
}

int cb_nfs_client_read(CBNfsClient *c, const uint8_t fh[CB_NFS_FHSIZE], uint32_t offset,
                       uint32_t count, uint8_t *buf, CBNfsAttr *a)
{
    if (!c || !fh || (!buf && count))
        return CB_ERROR(EINVAL);
    uint8_t b[CALL_MAX];
    CBWr w = cb_wr(b, sizeof(b));
    cb_nfs_put_call(&w, ++c->xid, CB_RPC_NFS, CB_RPC_NFS_VERSION, CB_NFSPROC_READ);
    cb_nfs_put_handle(&w, fh);
    cb_wr_u32(&w, offset);
    cb_wr_u32(&w, count);
    cb_wr_u32(&w, 0);
    CBRd r;
    int ret = rpc(c, CB_NFS_NFS, &w, &r);
    if (ret < 0 || (ret = cb_nfs_get_status(&r)) < 0 || (ret = cb_nfs_get_fattr(&r, a)) < 0)
        return ret;
    uint32_t n = cb_rd_u32(&r);
    if (r.err || n > count || n > cb_rd_left(&r))
        return CB_ERROR(EBADMSG);
    const uint8_t *p = cb_rd_bytes(&r, pad4(n));
    if (!p)
        return CB_ERROR(EBADMSG);
    if (n)
        memcpy(buf, p, n);
    return (int)n;
}

int cb_nfs_client_read_file(CBNfsClient *c, const char *path, uint32_t chunk,
                            cb_nfs_data_cb cb, void *opaque)
{
    if (!c || !path || !cb)
        return CB_ERROR(EINVAL);
    chunk = chunk ? CB_MIN(chunk, (uint32_t)CB_NFS_MAX_READ) : PLAYER_READ;
    uint8_t fh[CB_NFS_FHSIZE];
    CBNfsAttr a;
    int ret = cb_nfs_client_walk(c, path, fh, &a);
    if (ret < 0)
        return ret;
    if (a.type != 1)
        return CB_ERROR(EISDIR);
    uint8_t *buf = malloc(chunk);
    if (!buf)
        return CB_ERROR(ENOMEM);
    /* Up to the size the server gives, which each READ's attributes say
     * again (rbxport's fake CDJ reads to it, as a player does). */
    uint64_t off = 0, size = a.size;
    while (off < size) {
        uint32_t want = (uint32_t)CB_MIN((uint64_t)chunk, size - off);
        int n = cb_nfs_client_read(c, fh, (uint32_t)off, want, buf, &a);
        if (n <= 0) {
            ret = n < 0 ? n : CB_ERROR(EIO);
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
        size_t cap = CB_MAX(f->cap * 2, f->size + n);
        uint8_t *p = realloc(f->data, cap);
        if (!p)
            return CB_ERROR(ENOMEM);
        f->data = p;
        f->cap = cap;
    }
    memcpy(f->data + f->size, data, n);
    f->size += n;
    return 0;
}

int cb_nfs_client_fetch(CBNfsClient *c, const char *path, uint32_t chunk,
                        uint8_t **data, size_t *size)
{
    if (!data || !size)
        return CB_ERROR(EINVAL);
    Fetch f = { 0 };
    int ret = cb_nfs_client_read_file(c, path, chunk, fetch_cb, &f);
    if (ret < 0) {
        free(f.data);
        return ret;
    }
    *data = f.data ? f.data : malloc(1);
    *size = f.size;
    return *data ? 0 : CB_ERROR(ENOMEM);
}
