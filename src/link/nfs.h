/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The files players read: portmap (UDP 50111), mount version 1 and NFS
 * version 2 (RFC 1057, RFC 1094) as rekordbox serves them, with UTF-16LE
 * names (docs/LINK.md, section 3), over a virtual tree that holds the
 * library's tracks at the paths the export plan gives them and nothing
 * else (docs/LINK.md, "A virtual tree").
 *
 * The protocol is pure functions from a request datagram to a reply
 * datagram: no sockets here. Only reading a file touches the system, and
 * only by the path the plan holds for a track, never by a name a peer
 * sent. The client side (nfsclient.c) is the fake player's and the
 * tests'. */

#ifndef RXBOX_LINK_NFS_H
#define RXBOX_LINK_NFS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "rxbox/formats.h"

#include "link_priv.h"

/* --------------------------------------------------------------- protocol */

/* Programs and versions: portmap (RFC 1057 A), NFS (RFC 1094 2.2), mount
 * (RFC 1094 A.1). Players use these and only these (rbxport's captures of
 * a CDJ-3000 and an XDJ-700 with rekordbox). */
#define RX_RPC_PORTMAP          100000
#define RX_RPC_PORTMAP_VERSION  2
#define RX_RPC_NFS              100003
#define RX_RPC_NFS_VERSION      2
#define RX_RPC_MOUNT            100005
#define RX_RPC_MOUNT_VERSION    1

enum RXPmapProc {
    RX_PMAP_NULL, RX_PMAP_SET, RX_PMAP_UNSET, RX_PMAP_GETPORT, RX_PMAP_DUMP, RX_PMAP_CALLIT,
};

enum RXMntProc {
    RX_MNT_NULL, RX_MNT_MNT, RX_MNT_DUMP, RX_MNT_UMNT, RX_MNT_UMNTALL, RX_MNT_EXPORT,
    RX_MNT_EXPORTALL,
};

enum RXNfsProc {
    RX_NFSPROC_NULL, RX_NFSPROC_GETATTR, RX_NFSPROC_SETATTR, RX_NFSPROC_ROOT,
    RX_NFSPROC_LOOKUP, RX_NFSPROC_READLINK, RX_NFSPROC_READ, RX_NFSPROC_WRITECACHE,
    RX_NFSPROC_WRITE, RX_NFSPROC_CREATE, RX_NFSPROC_REMOVE, RX_NFSPROC_RENAME,
    RX_NFSPROC_LINK, RX_NFSPROC_SYMLINK, RX_NFSPROC_MKDIR, RX_NFSPROC_RMDIR,
    RX_NFSPROC_READDIR, RX_NFSPROC_STATFS,
};

/* RPC accept_stat (RFC 1057 8). */
enum RXRpcAccept {
    RX_RPC_SUCCESS, RX_RPC_PROG_UNAVAIL, RX_RPC_PROG_MISMATCH, RX_RPC_PROC_UNAVAIL,
    RX_RPC_GARBAGE_ARGS, RX_RPC_SYSTEM_ERR,
};

/* nfsstat (RFC 1094 2.3.1); mount's fhstatus uses the same numbers. */
enum RXNfsStat {
    RX_NFS_OK               = 0,
    RX_NFSERR_PERM          = 1,
    RX_NFSERR_NOENT         = 2,
    RX_NFSERR_IO            = 5,
    RX_NFSERR_ACCES         = 13,
    RX_NFSERR_NOTDIR        = 20,
    RX_NFSERR_ISDIR         = 21,
    RX_NFSERR_ROFS          = 30,
    RX_NFSERR_NAMETOOLONG   = 63,
    RX_NFSERR_STALE         = 70,
};

#define RX_NFS_FHSIZE   32              /* RFC 1094 2.3.3 */
/* The longest name, in UTF-16 units: FAT's long names, which the plan's
 * paths are made for; on the wire, twice that in bytes. */
#define RX_NFS_NAME_MAX 255
/* The most one READ answers: rekordbox's cap (libFilSiNE's
 * _tkfNFSRead, rbxport's static analysis), above NFS version 2's 8 kB
 * because a CDJ-3000 asks for 32 kB at a time and does not reliably ask
 * again for the rest of a shorter answer: rbxport's 8 kB cap left 155 kB
 * of a track unfetched in a capture (rbxport, KILLA capture 2026-10-06). */
#define RX_NFS_MAX_READ 0xfc00

/* ---------------------------------------------------------- virtual tree */

typedef struct RXVTree RXVTree;

/* The tree of a plan's tracks at their usb_path, each file standing for
 * the track's `source` with the plan's `file_size` (read when asked, so a
 * transcode made later is seen). The plan stays the caller's and must
 * outlive the tree. */
int  rx_vtree_new(RXVTree **out, const RXPlan *plan);
void rx_vtree_free(RXVTree **t);

/* Nodes are indices, the root 0; -1 is none. What the server and the
 * tests ask of a tree. */
int  rx_vtree_root(const RXVTree *t);
bool rx_vtree_is_dir(const RXVTree *t, int node);
const char *rx_vtree_name(const RXVTree *t, int node);         /* UTF-8; "" for the root */
uint64_t rx_vtree_dir_id(const RXVTree *t, int node);          /* stable across rebuilds */
const RXPlanTrack *rx_vtree_track(const RXVTree *t, int node); /* files only */
/* Children in their order (by name ignoring ASCII case, then exactly). */
int  rx_vtree_nb_children(const RXVTree *t, int dir);
int  rx_vtree_child(const RXVTree *t, int dir, int i);
/* The child of `dir` called `name` (UTF-8, `len` bytes): an exact match,
 * else one equal but for ASCII case. */
int  rx_vtree_lookup(const RXVTree *t, int dir, const char *name, size_t len);
int  rx_vtree_find_dir(const RXVTree *t, uint64_t id);
int  rx_vtree_find_file(const RXVTree *t, uint32_t track_id);
/* A whole '/'-separated path from the root, as rx_vtree_lookup walks it. */
int  rx_vtree_walk(const RXVTree *t, const char *path);

typedef struct RXVTreeStats {
    int      nb_files, nb_dirs;     /* the root counts as a directory */
    uint64_t total_size;            /* the files' plan sizes when built */
    uint32_t time;                  /* when built: the directories' times */
} RXVTreeStats;
void rx_vtree_stats(const RXVTree *t, RXVTreeStats *s);

/* ------------------------------------------------------------------- RPC */

enum RXNfsService {
    RX_NFS_PORTMAP,             /* the socket on 50111: program 100000 v2 */
    RX_NFS_MOUNT,               /* mountd: program 100005 v1 */
    RX_NFS_NFS,                 /* 2049: program 100003 v2 */
};

/* One server's state: its handles' tag, the peers that mounted, a few
 * open files. Not thread-safe: one thread (the service's) calls all of
 * it, and changes a plan track's `source` or `file_size` (a transcode
 * made) between calls, not during one. */
typedef struct RXNfs RXNfs;

typedef struct RXNfsConfig {
    uint16_t mount_port, nfs_port;      /* what portmap gives out */
    uint16_t portmap_port;              /* listed by DUMP; 0 leaves it out */
    /* The interface's address and netmask (IPv4, host order). The export
     * list offers the export to that subnet as "a.b.c.d/m.m.m.m", as
     * rekordbox does, and a CDJ-3000 mounts nothing whose list names no
     * group (rbxport, 1605b376); a mount from outside it is refused. Both
     * 0: no group, nobody refused (loopback tests). */
    uint32_t addr, netmask;
    /* The bytes of a track the plan marks `pcm` (a WAV made as it is
     * read, src/link/wav.c), which has no file: how many it put in dst
     * (fewer only at the end), or a negative errno. NULL: such a track
     * reads as an I/O error. Its times are its source's. */
    ssize_t (*read)(void *opaque, const RXPlanTrack *pt, uint64_t offset, uint8_t *dst, size_t n);
    void    *read_opaque;
} RXNfsConfig;

int  rx_nfs_new(RXNfs **out, const RXNfsConfig *c);
void rx_nfs_free(RXNfs **n);
/* The tree served from now on (the caller's; NULL serves nothing). Handles
 * name tracks by id, so a player reading a file keeps reading it across a
 * tree made again from a new plan. */
void rx_nfs_set_tree(RXNfs *n, const RXVTree *t);
/* Answers one request datagram that came in on `svc`'s socket from `peer`
 * (IPv4, host order): the reply's length in `reply`, 0 when nothing is to
 * be sent (not a call, or not worth answering), or a negative errno. */
ssize_t rx_nfs_handle(RXNfs *n, enum RXNfsService svc, uint32_t peer,
                      const uint8_t *req, size_t len, uint8_t *reply, size_t cap);
/* Whether `peer` has mounted (and so may READ). A peer forgotten that
 * mounted in this session is taken back in by its next READ or READDIR
 * with a handle of the session (a deck back from a lapse never mounts
 * again); one that never mounted must. */
bool rx_nfs_mounted(const RXNfs *n, uint32_t peer);
void rx_nfs_forget(RXNfs *n, uint32_t peer);

/* The largest datagram either side sends: a READ reply of RX_NFS_MAX_READ
 * with its headers (100 bytes). Under UDP's 65 507 over IPv4. */
#define RX_NFS_MAX_DATAGRAM (RX_NFS_MAX_READ + 128)

/* ----------------------------------------------------------------- codecs */

/* What both sides read and write: an XDR name as players send it
 * (UTF-16LE, a byte-length prefix, padded to four), and the parts of a
 * reply. Every read is bounded by `r`; a decoder returns a negative errno
 * (-EBADMSG when the bytes are not all there). */

/* Writes a UTF-8 name as UTF-16LE. */
void rx_nfs_put_name(RXWr *w, const char *utf8);
/* Reads a name into `out` (UTF-8, NUL-terminated; `size` at least
 * 3 * max_units + 1): its length, or -EBADMSG (not all there),
 * -ENAMETOOLONG (more than `max_units`), -EILSEQ (an odd length, a NUL or
 * a lone surrogate). Nothing is allocated. */
int  rx_nfs_get_name(RXRd *r, size_t max_units, char *out, size_t size);

/* A call: the header with the credential a CDJ sends (AUTH_UNIX, stamp
 * 0x967b8703, no machine name, uid and gid 0: rbxport, rbl-nfs rpc.rs)
 * and a null verifier; the arguments follow. */
void rx_nfs_put_call(RXWr *w, uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc);
void rx_nfs_put_handle(RXWr *w, const uint8_t fh[RX_NFS_FHSIZE]);

/* A reply's header: 0 with `res` on the results when it is accepted and a
 * success; otherwise a negative errno (-EBADMSG not a reply,
 * -EPROTONOSUPPORT program unavailable, -EPROTO a version mismatch,
 * -EOPNOTSUPP procedure unavailable, -EINVAL garbage arguments, -EACCES
 * an authentication error, -EIO a system error). `xid` is set whenever
 * the reply is one. */
int  rx_nfs_get_reply(const uint8_t *p, size_t len, uint32_t *xid, RXRd *res);
/* An nfsstat as a negative errno (0 for NFS_OK). */
int  rx_nfs_get_status(RXRd *r);
int  rx_nfs_status_errno(uint32_t stat);

typedef struct RXNfsAttr {
    uint32_t type;                  /* 1 file, 2 directory */
    uint32_t mode, nlink, uid, gid, size, blocksize, rdev, blocks, fsid, fileid;
    uint32_t atime, mtime, ctime;   /* seconds */
} RXNfsAttr;

int  rx_nfs_get_fattr(RXRd *r, RXNfsAttr *a);
int  rx_nfs_get_handle(RXRd *r, uint8_t fh[RX_NFS_FHSIZE]);

typedef struct RXNfsStatfs {
    uint32_t tsize, bsize, blocks, bfree, bavail;
} RXNfsStatfs;

/* A READDIR reply's entries, each given to `cb` (name in UTF-8; a
 * negative return stops and is returned); *eof set from the reply. */
typedef int (*rx_nfs_entry_cb)(void *opaque, uint32_t fileid, const char *name, uint32_t cookie);
int  rx_nfs_get_entries(RXRd *r, rx_nfs_entry_cb cb, void *opaque, bool *eof);
/* A mount EXPORT reply: each export's name and its first group (NULL
 * when it has none) to `cb`. */
typedef int (*rx_nfs_export_cb)(void *opaque, const char *dir, const char *group);
int  rx_nfs_get_exports(RXRd *r, rx_nfs_export_cb cb, void *opaque);

/* ----------------------------------------------------------------- client */

/* A player's side, blocking, over one UDP socket: GETPORT on portmap for
 * mount and NFS, then EXPORT and MNT, LOOKUP name by name and READ, as
 * rbxport's fake CDJ and a CDJ-3000 do it. Every function returns 0 (or
 * a count) or a negative errno: an NFS refusal as its errno (-ENOENT,
 * -ESTALE, -EACCES, -EROFS ...), -ETIMEDOUT when every try timed out. */

typedef struct RXNfsClient RXNfsClient;

typedef struct RXNfsClientOptions {
    uint32_t server;            /* IPv4, host order */
    uint16_t portmap_port;      /* 50111 for rekordbox and rxbox */
    uint32_t local;             /* the address to send from, 0 any */
    int      timeout_ms;        /* per try; 0 is 500 */
    int      tries;             /* 0 is 4 */
} RXNfsClientOptions;

/* Binds, and asks portmap where mount and NFS are. */
int  rx_nfs_client_open(RXNfsClient **out, const RXNfsClientOptions *o);
void rx_nfs_client_free(RXNfsClient **c);
void rx_nfs_client_ports(const RXNfsClient *c, uint16_t *mount, uint16_t *nfs);
/* EXPORT, then MNT of `dir` if it is listed ("/" for rekordbox and
 * rxbox): the root handle is kept for the walks. */
int  rx_nfs_client_mount(RXNfsClient *c, const char *dir);
const uint8_t *rx_nfs_client_root(const RXNfsClient *c);
int  rx_nfs_client_lookup(RXNfsClient *c, const uint8_t dir[RX_NFS_FHSIZE], const char *name,
                          uint8_t fh[RX_NFS_FHSIZE], RXNfsAttr *a);
/* LOOKUP of each component of `path` (UTF-8, '/'-separated) from the
 * root; `a` may be NULL. */
int  rx_nfs_client_walk(RXNfsClient *c, const char *path, uint8_t fh[RX_NFS_FHSIZE], RXNfsAttr *a);
int  rx_nfs_client_getattr(RXNfsClient *c, const uint8_t fh[RX_NFS_FHSIZE], RXNfsAttr *a);
/* One READ into `buf` (room for `count`): the bytes read; `a` may be
 * NULL. */
int  rx_nfs_client_read(RXNfsClient *c, const uint8_t fh[RX_NFS_FHSIZE], uint32_t offset,
                        uint32_t count, uint8_t *buf, RXNfsAttr *a);
/* A whole file: walks `path`, then READs `chunk` bytes at a time (0 is
 * 32 kB, a CDJ-3000's size) up to the size the attributes give, as a
 * player does (it never asks past the end: rekordbox answers that with
 * NFSERR_IO). Each piece goes to `cb`; a negative return stops. */
typedef int (*rx_nfs_data_cb)(void *opaque, const uint8_t *data, size_t n, uint64_t offset);
int  rx_nfs_client_read_file(RXNfsClient *c, const char *path, uint32_t chunk,
                             rx_nfs_data_cb cb, void *opaque);
/* The same into memory: *data malloc'd. */
int  rx_nfs_client_fetch(RXNfsClient *c, const char *path, uint32_t chunk,
                         uint8_t **data, size_t *size);
/* One call already encoded (rx_nfs_put_call ...), sent to `svc`'s port
 * with retries: the reply's length in `reply`, which only matches the
 * call's xid. */
int  rx_nfs_client_call(RXNfsClient *c, enum RXNfsService svc, const uint8_t *req, size_t len,
                        uint8_t *reply, size_t cap);

#endif /* RXBOX_LINK_NFS_H */
