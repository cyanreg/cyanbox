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

#ifndef CYANBOX_LINK_NFS_H
#define CYANBOX_LINK_NFS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "cyanbox/formats.h"

#include "link_priv.h"

/* --------------------------------------------------------------- protocol */

/* Programs and versions: portmap (RFC 1057 A), NFS (RFC 1094 2.2), mount
 * (RFC 1094 A.1). Players use these and only these (rbxport's captures of
 * a CDJ-3000 and an XDJ-700 with rekordbox). */
#define CB_RPC_PORTMAP          100000
#define CB_RPC_PORTMAP_VERSION  2
#define CB_RPC_NFS              100003
#define CB_RPC_NFS_VERSION      2
#define CB_RPC_MOUNT            100005
#define CB_RPC_MOUNT_VERSION    1

enum CBPmapProc {
    CB_PMAP_NULL, CB_PMAP_SET, CB_PMAP_UNSET, CB_PMAP_GETPORT, CB_PMAP_DUMP, CB_PMAP_CALLIT,
};

enum CBMntProc {
    CB_MNT_NULL, CB_MNT_MNT, CB_MNT_DUMP, CB_MNT_UMNT, CB_MNT_UMNTALL, CB_MNT_EXPORT,
    CB_MNT_EXPORTALL,
};

enum CBNfsProc {
    CB_NFSPROC_NULL, CB_NFSPROC_GETATTR, CB_NFSPROC_SETATTR, CB_NFSPROC_ROOT,
    CB_NFSPROC_LOOKUP, CB_NFSPROC_READLINK, CB_NFSPROC_READ, CB_NFSPROC_WRITECACHE,
    CB_NFSPROC_WRITE, CB_NFSPROC_CREATE, CB_NFSPROC_REMOVE, CB_NFSPROC_RENAME,
    CB_NFSPROC_LINK, CB_NFSPROC_SYMLINK, CB_NFSPROC_MKDIR, CB_NFSPROC_RMDIR,
    CB_NFSPROC_READDIR, CB_NFSPROC_STATFS,
};

/* RPC accept_stat (RFC 1057 8). */
enum CBRpcAccept {
    CB_RPC_SUCCESS, CB_RPC_PROG_UNAVAIL, CB_RPC_PROG_MISMATCH, CB_RPC_PROC_UNAVAIL,
    CB_RPC_GARBAGE_ARGS, CB_RPC_SYSTEM_ERR,
};

/* nfsstat (RFC 1094 2.3.1); mount's fhstatus uses the same numbers. */
enum CBNfsStat {
    CB_NFS_OK               = 0,
    CB_NFSERR_PERM          = 1,
    CB_NFSERR_NOENT         = 2,
    CB_NFSERR_IO            = 5,
    CB_NFSERR_ACCES         = 13,
    CB_NFSERR_NOTDIR        = 20,
    CB_NFSERR_ISDIR         = 21,
    CB_NFSERR_ROFS          = 30,
    CB_NFSERR_NAMETOOLONG   = 63,
    CB_NFSERR_STALE         = 70,
};

#define CB_NFS_FHSIZE   32              /* RFC 1094 2.3.3 */
/* The longest name, in UTF-16 units: FAT's long names, which the plan's
 * paths are made for; on the wire, twice that in bytes. */
#define CB_NFS_NAME_MAX 255
/* The most one READ answers: rekordbox's cap (libFilSiNE's
 * _tkfNFSRead, rbxport's static analysis), above NFS version 2's 8 kB
 * because a CDJ-3000 asks for 32 kB at a time and does not reliably ask
 * again for the rest of a shorter answer: rbxport's 8 kB cap left 155 kB
 * of a track unfetched in a capture (rbxport, KILLA capture 2026-10-06). */
#define CB_NFS_MAX_READ 0xfc00

/* ---------------------------------------------------------- virtual tree */

typedef struct CBVTree CBVTree;

/* The tree of a plan's tracks at their usb_path, each file standing for
 * the track's `source` with the plan's `file_size` (read when asked, so a
 * transcode made later is seen). The plan stays the caller's and must
 * outlive the tree. */
int  cb_vtree_new(CBVTree **out, const CBPlan *plan);
void cb_vtree_free(CBVTree **t);

/* Nodes are indices, the root 0; -1 is none. What the server and the
 * tests ask of a tree. */
int  cb_vtree_root(const CBVTree *t);
bool cb_vtree_is_dir(const CBVTree *t, int node);
const char *cb_vtree_name(const CBVTree *t, int node);         /* UTF-8; "" for the root */
uint64_t cb_vtree_dir_id(const CBVTree *t, int node);          /* stable across rebuilds */
const CBPlanTrack *cb_vtree_track(const CBVTree *t, int node); /* files only */
/* Children in their order (by name ignoring ASCII case, then exactly). */
int  cb_vtree_nb_children(const CBVTree *t, int dir);
int  cb_vtree_child(const CBVTree *t, int dir, int i);
/* The child of `dir` called `name` (UTF-8, `len` bytes): an exact match,
 * else one equal but for ASCII case. */
int  cb_vtree_lookup(const CBVTree *t, int dir, const char *name, size_t len);
int  cb_vtree_find_dir(const CBVTree *t, uint64_t id);
int  cb_vtree_find_file(const CBVTree *t, uint32_t track_id);
/* A whole '/'-separated path from the root, as cb_vtree_lookup walks it. */
int  cb_vtree_walk(const CBVTree *t, const char *path);

typedef struct CBVTreeStats {
    int      nb_files, nb_dirs;     /* the root counts as a directory */
    uint64_t total_size;            /* the files' plan sizes when built */
    uint32_t time;                  /* when built: the directories' times */
} CBVTreeStats;
void cb_vtree_stats(const CBVTree *t, CBVTreeStats *s);

/* ------------------------------------------------------------------- RPC */

enum CBNfsService {
    CB_NFS_PORTMAP,             /* the socket on 50111: program 100000 v2 */
    CB_NFS_MOUNT,               /* mountd: program 100005 v1 */
    CB_NFS_NFS,                 /* 2049: program 100003 v2 */
};

/* One server's state: its handles' tag, the peers that mounted, a few
 * open files. Not thread-safe: one thread (the service's) calls all of
 * it, and changes a plan track's `source` or `file_size` (a transcode
 * made) between calls, not during one. */
typedef struct CBNfs CBNfs;

/* What a session keeps for its decks: the tag every handle it gives out
 * starts with, and the addresses that mounted (cb_nfs_mounted). A server
 * has one of its own, or the owner's (CBNfsConfig.session), which outlives
 * it: a set gives the same one to each service it starts on an interface,
 * so a deck that reads on through a cable or an adapter that came back is
 * answered by the service there now. One server at a time uses it. */
#define CB_NFS_EVER 256
typedef struct CBNfsSession {
    uint32_t tag;                       /* 0: drawn by the first server */
    uint32_t ever[CB_NFS_EVER];         /* the first CB_NFS_EVER */
    int      nb_ever;
} CBNfsSession;

typedef struct CBNfsConfig {
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
    ssize_t (*read)(void *opaque, const CBPlanTrack *pt, uint64_t offset, uint8_t *dst, size_t n);
    void    *read_opaque;
    /* The owner's session (above); NULL: the server's own, and a handle
     * from before it is stale. */
    CBNfsSession *session;
} CBNfsConfig;

int  cb_nfs_new(CBNfs **out, const CBNfsConfig *c);
void cb_nfs_free(CBNfs **n);
/* The tree served from now on (the caller's; NULL serves nothing). Handles
 * name tracks by id, so a player reading a file keeps reading it across a
 * tree made again from a new plan. */
void cb_nfs_set_tree(CBNfs *n, const CBVTree *t);
/* Answers one request datagram that came in on `svc`'s socket from `peer`
 * (IPv4, host order): the reply's length in `reply`, 0 when nothing is to
 * be sent (not a call, or not worth answering), or a negative errno. */
ssize_t cb_nfs_handle(CBNfs *n, enum CBNfsService svc, uint32_t peer,
                      const uint8_t *req, size_t len, uint8_t *reply, size_t cap);
/* Whether `peer` has mounted (and so may READ). A peer forgotten that
 * mounted in this session is taken back in by its next READ or READDIR
 * with a handle of the session (a deck back from a lapse never mounts
 * again); one that never mounted must. */
bool cb_nfs_mounted(const CBNfs *n, uint32_t peer);
void cb_nfs_forget(CBNfs *n, uint32_t peer);

/* The largest datagram either side sends: a READ reply of CB_NFS_MAX_READ
 * with its headers (100 bytes). Under UDP's 65 507 over IPv4. */
#define CB_NFS_MAX_DATAGRAM (CB_NFS_MAX_READ + 128)

/* ----------------------------------------------------------------- codecs */

/* What both sides read and write: an XDR name as players send it
 * (UTF-16LE, a byte-length prefix, padded to four), and the parts of a
 * reply. Every read is bounded by `r`; a decoder returns a negative errno
 * (-EBADMSG when the bytes are not all there). */

/* Writes a UTF-8 name as UTF-16LE. */
void cb_nfs_put_name(CBWr *w, const char *utf8);
/* Reads a name into `out` (UTF-8, NUL-terminated; `size` at least
 * 3 * max_units + 1): its length, or -EBADMSG (not all there),
 * -ENAMETOOLONG (more than `max_units`), -EILSEQ (an odd length, a NUL or
 * a lone surrogate). Nothing is allocated. */
int  cb_nfs_get_name(CBRd *r, size_t max_units, char *out, size_t size);

/* A call: the header with the credential a CDJ sends (AUTH_UNIX, stamp
 * 0x967b8703, no machine name, uid and gid 0: rbxport, rbl-nfs rpc.rs)
 * and a null verifier; the arguments follow. */
void cb_nfs_put_call(CBWr *w, uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc);
void cb_nfs_put_handle(CBWr *w, const uint8_t fh[CB_NFS_FHSIZE]);

/* A reply's header: 0 with `res` on the results when it is accepted and a
 * success; otherwise a negative errno (-EBADMSG not a reply,
 * -EPROTONOSUPPORT program unavailable, -EPROTO a version mismatch,
 * -EOPNOTSUPP procedure unavailable, -EINVAL garbage arguments, -EACCES
 * an authentication error, -EIO a system error). `xid` is set whenever
 * the reply is one. */
int  cb_nfs_get_reply(const uint8_t *p, size_t len, uint32_t *xid, CBRd *res);
/* An nfsstat as a negative errno (0 for NFS_OK). */
int  cb_nfs_get_status(CBRd *r);
int  cb_nfs_status_errno(uint32_t stat);

typedef struct CBNfsAttr {
    uint32_t type;                  /* 1 file, 2 directory */
    uint32_t mode, nlink, uid, gid, size, blocksize, rdev, blocks, fsid, fileid;
    uint32_t atime, mtime, ctime;   /* seconds */
} CBNfsAttr;

int  cb_nfs_get_fattr(CBRd *r, CBNfsAttr *a);
int  cb_nfs_get_handle(CBRd *r, uint8_t fh[CB_NFS_FHSIZE]);

typedef struct CBNfsStatfs {
    uint32_t tsize, bsize, blocks, bfree, bavail;
} CBNfsStatfs;

/* A READDIR reply's entries, each given to `cb` (name in UTF-8; a
 * negative return stops and is returned); *eof set from the reply. */
typedef int (*cb_nfs_entry_cb)(void *opaque, uint32_t fileid, const char *name, uint32_t cookie);
int  cb_nfs_get_entries(CBRd *r, cb_nfs_entry_cb cb, void *opaque, bool *eof);
/* A mount EXPORT reply: each export's name and its first group (NULL
 * when it has none) to `cb`. */
typedef int (*cb_nfs_export_cb)(void *opaque, const char *dir, const char *group);
int  cb_nfs_get_exports(CBRd *r, cb_nfs_export_cb cb, void *opaque);

/* ----------------------------------------------------------------- client */

/* A player's side, blocking, over one UDP socket: GETPORT on portmap for
 * mount and NFS, then EXPORT and MNT, LOOKUP name by name and READ, as
 * rbxport's fake CDJ and a CDJ-3000 do it. Every function returns 0 (or
 * a count) or a negative errno: an NFS refusal as its errno (-ENOENT,
 * -ESTALE, -EACCES, -EROFS ...), -ETIMEDOUT when every try timed out. */

typedef struct CBNfsClient CBNfsClient;

typedef struct CBNfsClientOptions {
    uint32_t server;            /* IPv4, host order */
    uint16_t portmap_port;      /* 50111 for rekordbox and cyanbox */
    uint32_t local;             /* the address to send from, 0 any */
    int      timeout_ms;        /* per try; 0 is 500 */
    int      tries;             /* 0 is 4 */
} CBNfsClientOptions;

/* Binds, and asks portmap where mount and NFS are. */
int  cb_nfs_client_open(CBNfsClient **out, const CBNfsClientOptions *o);
void cb_nfs_client_free(CBNfsClient **c);
void cb_nfs_client_ports(const CBNfsClient *c, uint16_t *mount, uint16_t *nfs);
/* EXPORT, then MNT of `dir` if it is listed ("/" for rekordbox and
 * cyanbox): the root handle is kept for the walks. */
int  cb_nfs_client_mount(CBNfsClient *c, const char *dir);
const uint8_t *cb_nfs_client_root(const CBNfsClient *c);
int  cb_nfs_client_lookup(CBNfsClient *c, const uint8_t dir[CB_NFS_FHSIZE], const char *name,
                          uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a);
/* LOOKUP of each component of `path` (UTF-8, '/'-separated) from the
 * root; `a` may be NULL. */
int  cb_nfs_client_walk(CBNfsClient *c, const char *path, uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a);
int  cb_nfs_client_getattr(CBNfsClient *c, const uint8_t fh[CB_NFS_FHSIZE], CBNfsAttr *a);
/* One READ into `buf` (room for `count`): the bytes read; `a` may be
 * NULL. */
int  cb_nfs_client_read(CBNfsClient *c, const uint8_t fh[CB_NFS_FHSIZE], uint32_t offset,
                        uint32_t count, uint8_t *buf, CBNfsAttr *a);
/* A whole file: walks `path`, then READs `chunk` bytes at a time (0 is
 * 32 kB, a CDJ-3000's size) up to the size the attributes give, as a
 * player does (it never asks past the end: rekordbox answers that with
 * NFSERR_IO). Each piece goes to `cb`; a negative return stops. */
typedef int (*cb_nfs_data_cb)(void *opaque, const uint8_t *data, size_t n, uint64_t offset);
int  cb_nfs_client_read_file(CBNfsClient *c, const char *path, uint32_t chunk,
                             cb_nfs_data_cb cb, void *opaque);
/* The same into memory: *data malloc'd. */
int  cb_nfs_client_fetch(CBNfsClient *c, const char *path, uint32_t chunk,
                         uint8_t **data, size_t *size);
/* One call already encoded (cb_nfs_put_call ...), sent to `svc`'s port
 * with retries: the reply's length in `reply`, which only matches the
 * call's xid. */
int  cb_nfs_client_call(CBNfsClient *c, enum CBNfsService svc, const uint8_t *req, size_t len,
                        uint8_t *reply, size_t cap);

#endif /* CYANBOX_LINK_NFS_H */
