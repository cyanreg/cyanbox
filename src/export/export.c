/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Writing a stick, rekordbox's sync-manager model (rxbox/export.h): the
 * settings, the chosen playlists (all by default) and the union of their
 * tracks, or the whole library. A port of dj's export.py write_stick with
 * the checks docs/OVERVIEW.md 3.5 asks for.
 *
 * Order matters for a stick that is unplugged half way: audio first, then
 * the ANLZ files, artwork and settings, and the databases last, so a
 * database never points at files that are not there yet (dj). Audio
 * already on the stick with the same size and a modification time within
 * FAT's two seconds is skipped (dj's _same_file); everything else is
 * rewritten every time. Files written atomically (temporary name, rename).
 *
 * What is replaced and what is kept: export.pdb, exportExt.pdb and
 * exportLibrary.db are rebuilt (the history a player appended to them goes
 * with them, which is why a stick with unsynced history blocks the
 * export), exportLibrary.db-wal/-shm and a stale exportExt.pdb are removed
 * (docs/pdb.md 1.1). Files players write and rxbox does not (export.pdb.bak,
 * RBFLTR.DAT, PIONEER/CDJ, LOG, MPJ ...) are never touched. With `prune`,
 * audio, ANLZ and artwork files that are not part of this export are
 * deleted; without it they are only reported.
 *
 * Analysis is the caller's (force_analysis included): a track without a
 * grid is reported and left out, one without waveforms gets ANLZ files
 * without waveform tags, both through the callback.
 *
 * Progress counts are 1-based ("copy" 1 of 58 is the first file). A dry
 * run writes nothing anywhere: the transcodes it would make are listed as
 * "transcode" items (and the plan describes them from the originals), and
 * every file it would write as "copy", "skip" or "rewrite". Real
 * transcodes run on a pool of half the CPUs before the plan is made.
 *
 * What keeps the stick and the library safe:
 *  - the databases are built during the checks, so a library they cannot
 *    hold (a name no DeviceSQL page fits) stops the export before anything
 *    is written;
 *  - the export refuses a stick whose Contents or PIONEER directories are
 *    symbolic links (FAT has none; a staging directory may), so nothing is
 *    written or deleted out of the stick;
 *  - the old sync record is removed before the databases are replaced and
 *    the new one written after them, so a record never describes other
 *    databases than the stick's;
 *  - prune never deletes a file that is, by the file system's own lookup,
 *    one the plan writes (FAT and exFAT fold case further than
 *    rx_plan_fold_case does), a library track's file or anything under the
 *    library's root, wherever those are. */

#include "rxbox/export.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "rxbox/thread.h"
#include "src/export/export_priv.h"
#include "src/formats/formats_priv.h"

#define DB_RESERVE (64ll << 20)

void rx_export_options_default(RxExportOptions *o)
{
    *o = (RxExportOptions) { 0 };
}

typedef struct Ctx {
    RxLibrary              *lib;
    const char             *dest;
    const RxExportOptions  *opt;
    rx_progress_cb          cb;
    void                   *opaque;
    bool                    cancelled;
    /* The databases, built by the checks and written last. */
    uint8_t                *pdb, *ext, *olib;
    size_t                  pdb_size, ext_size, olib_size;
} Ctx;

static bool progress(Ctx *c, const char *stage, int done, int total, const char *item)
{
    if (c->cancelled)
        return false;
    if (c->cb && !c->cb(c->opaque, stage, done, total, item))
        c->cancelled = true;
    return !c->cancelled;
}

static void reportf(Ctx *c, const char *stage, const char *fmt, ...) RX_PRINTF(3, 4);
static void reportf(Ctx *c, const char *stage, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = rx_vasprintf(fmt, ap);
    va_end(ap);
    progress(c, stage, 0, 0, msg);
    free(msg);
}

/* ------------------------------------------------------------- tracks */

static int cmp_ptr_id(const void *a, const void *b)
{
    uint64_t x = (*(RxTrack *const *)a)->id, y = (*(RxTrack *const *)b)->id;
    return x < y ? -1 : x > y;
}

/* The tracks to export: the union of the playlists' tracks, or the whole
 * library; missing files and unanalysed tracks reported and left out. */
static int select_tracks(Ctx *c, const RxPlaylist *const *pls, int nb_pls, RxTrack ***out, int *nb)
{
    RxLibrary *lib = c->lib;
    RxTrack **v = malloc((size_t)RX_MAX(lib->nb_tracks, 1) * sizeof(*v));
    if (!v)
        return RX_ERROR(ENOMEM);
    int n = 0;
    if (c->opt->whole_library) {
        for (int i = 0; i < lib->nb_tracks; i++)
            v[n++] = lib->tracks[i];
    } else {
        uint8_t *in = calloc((size_t)RX_MAX(lib->nb_tracks, 1), 1);
        if (!in) {
            free(v);
            return RX_ERROR(ENOMEM);
        }
        for (int p = 0; p < nb_pls; p++) {
            for (int k = 0; k < pls[p]->nb; k++) {
                RxTrack *t = rx_library_find_id(lib, pls[p]->track_ids[k]);
                int idx = t ? rx_library_index_of(lib, t) : -1;
                if (idx >= 0 && !in[idx]) {
                    in[idx] = 1;
                    v[n++] = t;
                }
            }
        }
        free(in);
        qsort(v, (size_t)n, sizeof(*v), cmp_ptr_id);
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        char name[512];
        rx_track_display(v[i], name, sizeof(name));
        if (v[i]->missing) {
            reportf(c, "check", "skipped, file missing: %s", name);
            continue;
        }
        if (!rx_track_analysed(v[i])) {
            reportf(c, "check", "skipped, not analysed (no beat grid): %s", name);
            continue;
        }
        if (v[i]->id > UINT32_MAX) {
            reportf(c, "check", "skipped, track id beyond 32 bits: %s", name);
            continue;
        }
        v[k++] = v[i];
    }
    *out = v;
    *nb = k;
    return 0;
}

/* -------------------------------------------------------------- files */

/* dj's _same_file(): same size, mtime within FAT's 2 s resolution. */
static bool same_file(const char *src, const char *dst)
{
    struct stat a, b;
    if (stat(src, &a) < 0 || stat(dst, &b) < 0)
        return false;
    return a.st_size == b.st_size && llabs((long long)a.st_mtime - (long long)b.st_mtime) <= 2;
}

static int copy_file(const char *src, const char *dst)
{
    char *dir = rx_strdup(dst);
    if (!dir)
        return RX_ERROR(ENOMEM);
    char *slash = strrchr(dir, '/');
    int ret = 0;
    if (slash && slash != dir) {
        *slash = '\0';
        ret = rx_mkdir_p(dir);
    }
    free(dir);
    if (ret < 0)
        return ret;
    char *tmp = rx_asprintf("%s.rxbox-tmp", dst);
    if (!tmp)
        return RX_ERROR(ENOMEM);
    int in = open(src, O_RDONLY | O_CLOEXEC);
    int out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    uint8_t *buf = malloc(1 << 20);
    if (in < 0 || out < 0 || !buf) {
        ret = !buf ? RX_ERROR(ENOMEM) : RX_ERROR(errno);
        goto out;
    }
    while (1) {
        ssize_t n = read(in, buf, 1 << 20);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ret = RX_ERROR(errno);
            break;
        }
        if (!n)
            break;
        for (ssize_t o = 0; o < n;) {
            ssize_t w = write(out, buf + o, (size_t)(n - o));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0) {
                ret = RX_ERROR(errno);
                goto out;
            }
            o += w;
        }
    }
    if (!ret) {
        struct stat st;
        if (fstat(in, &st) == 0) {
            struct timespec ts[2] = { st.st_atim, st.st_mtim };
            futimens(out, ts);
        }
        /* On the stick before the rename: a file renamed into place but
         * not written out has the right size and time after an unplug,
         * and same_file() would keep it forever. */
        if (fsync(out) < 0)
            ret = RX_ERROR(errno);
    }
out:
    if (out >= 0 && close(out) < 0 && !ret)
        ret = RX_ERROR(errno);
    if (in >= 0)
        close(in);
    free(buf);
    if (!ret && rename(tmp, dst) < 0)
        ret = RX_ERROR(errno);
    if (ret)
        unlink(tmp);
    free(tmp);
    return ret;
}

static int write_file(Ctx *c, const char *stick_path, const void *data, size_t size)
{
    char *p = rx_stick_file(c->dest, stick_path);
    if (!p)
        return RX_ERROR(ENOMEM);
    int ret = rx_stick_write(p, data, size);
    if (ret < 0)
        reportf(c, "database", "cannot write %s: %s", p, strerror(-ret));
    free(p);
    return ret;
}

static void remove_file(Ctx *c, const char *stick_path)
{
    char *p = rx_stick_file(c->dest, stick_path);
    if (p && unlink(p) == 0)
        reportf(c, "database", "removed stale %s", stick_path);
    free(p);
}

/* ---------------------------------------------------------- our files */

typedef struct Ours {
    char **paths;                /* folded, sorted */
    int    nb, cap;
} Ours;

static int ours_add(Ours *o, const char *stick_path)
{
    if (o->nb == o->cap) {
        int cap = o->cap ? 2 * o->cap : 256;
        char **p = realloc(o->paths, (size_t)cap * sizeof(*p));
        if (!p)
            return RX_ERROR(ENOMEM);
        o->paths = p;
        o->cap = cap;
    }
    if (!(o->paths[o->nb] = rx_plan_fold_case(stick_path)))
        return RX_ERROR(ENOMEM);
    o->nb++;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool ours_has(const Ours *o, const char *stick_path)
{
    if (!o->nb)
        return false;
    char *f = rx_plan_fold_case(stick_path);
    bool has = f && bsearch(&f, o->paths, (size_t)o->nb, sizeof(*o->paths), cmp_str);
    free(f);
    return has;
}

static void ours_free(Ours *o)
{
    for (int i = 0; i < o->nb; i++)
        free(o->paths[i]);
    free(o->paths);
    *o = (Ours) { 0 };
}

static int collect_ours(const RxPlan *plan, Ours *o)
{
    int ret = 0;
    for (int i = 0; i < plan->nb_tracks && ret >= 0; i++) {
        const RxPlanTrack *t = &plan->tracks[i];
        ret = ours_add(o, t->usb_path);
        const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
        for (int k = 0; k < 3 && ret >= 0; k++) {
            char *p = rx_plan_anlz_path(t, ext[k]);
            ret = p ? ours_add(o, p) : RX_ERROR(ENOMEM);
            free(p);
        }
    }
    for (int i = 0; i < plan->nb_artworks && ret >= 0; i++) {
        uint32_t id = plan->artworks[i].id;
        char *dir = rx_plan_artwork_dir(id);
        const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
        for (int k = 0; k < 4 && ret >= 0 && dir; k++) {
            char *p = rx_asprintf(fmt[k], dir, id);
            ret = p ? ours_add(o, p) : RX_ERROR(ENOMEM);
            free(p);
        }
        if (!dir)
            ret = RX_ERROR(ENOMEM);
        free(dir);
    }
    if (o->nb)
        qsort(o->paths, (size_t)o->nb, sizeof(*o->paths), cmp_str);
    return ret;
}

/* Files prune must keep whatever their names: identified by device and
 * inode, which the file system resolves with its own case rules. */
typedef struct Keep {
    dev_t dev;
    ino_t ino;
} Keep;

typedef struct KeepSet {
    RxArray files;               /* Keep, sorted */
    char   *lib_root;            /* the library's root, resolved; NULL if unknown */
} KeepSet;

static int cmp_keep(const void *a, const void *b)
{
    const Keep *x = a, *y = b;
    if (x->dev != y->dev)
        return x->dev < y->dev ? -1 : 1;
    return x->ino < y->ino ? -1 : x->ino > y->ino;
}

static int keep_add(KeepSet *k, const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode))
        return 0;
    Keep e = { st.st_dev, st.st_ino };
    return rx_array_push(&k->files, &e) ? 0 : RX_ERROR(ENOMEM);
}

static bool keep_has(const KeepSet *k, const struct stat *st)
{
    Keep e = { st->st_dev, st->st_ino };
    return k->files.nb && bsearch(&e, k->files.data, k->files.nb, sizeof(e), cmp_keep);
}

static void keep_free(KeepSet *k)
{
    rx_array_free(&k->files);
    free(k->lib_root);
}

/* Everything the export writes, every library track's file and the
 * transcodes, as they are on disk now. */
static int collect_keep(Ctx *c, const RxPlan *plan, const Ours *o, KeepSet *k)
{
    rx_array_init(&k->files, sizeof(Keep));
    k->lib_root = c->lib->root ? realpath(c->lib->root, NULL) : NULL;
    int ret = 0;
    for (int i = 0; i < o->nb && ret >= 0; i++) {
        /* Every file written, by its folded name (which a case-insensitive
         * stick resolves) and, below, the plan's own spelling. */
        char *p = rx_stick_file(c->dest, o->paths[i]);
        ret = p ? keep_add(k, p) : RX_ERROR(ENOMEM);
        free(p);
    }
    for (int i = 0; i < plan->nb_tracks && ret >= 0; i++) {
        const RxPlanTrack *t = &plan->tracks[i];
        char *p = rx_stick_file(c->dest, t->usb_path);
        ret = p ? keep_add(k, p) : RX_ERROR(ENOMEM);
        free(p);
        if (ret >= 0)
            ret = keep_add(k, t->source);
    }
    for (int i = 0; i < c->lib->nb_tracks && ret >= 0; i++)
        ret = keep_add(k, c->lib->tracks[i]->path);
    if (k->files.nb)
        qsort(k->files.data, k->files.nb, sizeof(Keep), cmp_keep);
    return ret;
}

/* Files under the stick's audio, analysis and artwork trees that this
 * export does not write. */
static int find_extra(Ctx *c, const Ours *o, const KeepSet *k, const char *stick_dir, RxArray *extra)
{
    char *dir = rx_stick_file(c->dest, stick_dir);
    DIR *d = dir ? opendir(dir) : NULL;
    int ret = dir ? 0 : RX_ERROR(ENOMEM);
    struct dirent *e;
    while (d && ret >= 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *sub = rx_asprintf("%s/%s", stick_dir, e->d_name);
        char *full = sub ? rx_stick_file(c->dest, sub) : NULL;
        struct stat st;
        if (!sub || !full) {
            ret = RX_ERROR(ENOMEM);
        } else if (lstat(full, &st) < 0) {
            /* Gone meanwhile, or unreadable: not ours to judge. */
        } else if (S_ISDIR(st.st_mode)) {
            char *real = k->lib_root ? realpath(full, NULL) : NULL;
            if (!real || strcmp(real, k->lib_root))
                ret = find_extra(c, o, k, sub, extra);
            free(real);
        } else if (S_ISREG(st.st_mode) && !ours_has(o, sub) && !keep_has(k, &st)) {
            char *keep = rx_strdup(sub);
            if (!keep || !rx_array_push(extra, &keep)) {
                free(keep);
                ret = RX_ERROR(ENOMEM);
            }
        }
        free(sub);
        free(full);
    }
    if (d)
        closedir(d);
    free(dir);
    return ret;
}

/* Removes the directories left empty under `full` (not itself); true
 * when nothing is left in it. */
static bool prune_empty(const char *full)
{
    DIR *d = opendir(full);
    if (!d)
        return false;
    bool empty = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *sub = rx_asprintf("%s/%s", full, e->d_name);
        struct stat st;
        bool removed = sub && lstat(sub, &st) == 0 && S_ISDIR(st.st_mode) && prune_empty(sub) &&
                       rmdir(sub) == 0;
        if (!removed)
            empty = false;
        free(sub);
    }
    closedir(d);
    return empty;
}

/* ---------------------------------------------------------------- checks */


/* The directories the export writes into and prunes: real directories
 * on the stick, never links out of it. */
static const char *const stick_dirs[] = {
    "/Contents", "/PIONEER", "/PIONEER/rekordbox", "/PIONEER/USBANLZ", "/PIONEER/Artwork", "/PIONEER/rxbox",
};

static bool stick_dir_ok(const char *dest, const char *stick_dir)
{
    char *p = rx_stick_file(dest, stick_dir);
    struct stat st;
    bool ok = p && (lstat(p, &st) < 0 ? errno == ENOENT : S_ISDIR(st.st_mode));
    free(p);
    return ok;
}

static int64_t round_up(int64_t n, int64_t cluster)
{
    return n <= 0 ? 0 : (n + cluster - 1) / cluster * cluster;
}

/* One file written beside its old copy and renamed over it: while it is
 * written both exist, and a smaller one frees space only afterwards. */
static void account(int64_t new_size, int64_t old_size, int64_t cluster, int64_t *need, int64_t *overlap)
{
    int64_t n = round_up(new_size, cluster), o = round_up(old_size, cluster);
    *need += RX_MAX(n - o, 0);
    *overlap = RX_MAX(*overlap, RX_MIN(n, o));
}

static int64_t stick_size(const char *dest, const char *stick_path)
{
    char *p = rx_stick_file(dest, stick_path);
    int64_t n = p ? RX_MAX(rx_file_size(p), 0) : 0;
    free(p);
    return n;
}

int64_t rx_export_space_needed(const RxPlan *plan, const char *dest, int64_t cluster)
{
    cluster = RX_MAX(cluster, 1);
    int64_t need = DB_RESERVE, overlap = 0;
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RxPlanTrack *t = &plan->tracks[i];
        char *dst = rx_stick_file(dest, t->usb_path);
        if (dst && !same_file(t->source, dst))
            account(t->file_size, RX_MAX(rx_file_size(dst), 0), cluster, &need, &overlap);
        free(dst);
        /* The analysis files, rewritten every time: the waveform details
         * take 1 + 2 bytes (.EXT) and 3 bytes (.2EX) a column at 150
         * columns a second (docs/anlz.md 4.9-4.12), the beats 8 bytes each
         * (4.3; up to 5 a second here), the previews, PVBR, PVB2 and cue
         * lists a few KiB a file. */
        double dur = RX_MAX(t->duration, 0.0);
        int64_t cols = (int64_t)ceil(dur * 150.0);
        int64_t sizes[3] = {
            12288 + (int64_t)(40 * dur) + 120 * (int64_t)t->nb_cues,
            20480 + 3 * cols + 200 * (int64_t)t->nb_cues,
            8192 + 3 * cols,
        };
        const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
        for (int k = 0; k < 3; k++) {
            char *p = t->anlz_dir ? rx_plan_anlz_path(t, ext[k]) : NULL;
            account(sizes[k], p ? stick_size(dest, p) : 0, cluster, &need, &overlap);
            free(p);
        }
    }
    for (int i = 0; i < plan->nb_artworks; i++) {
        const RxPlanArtwork *a = &plan->artworks[i];
        char *dir = rx_plan_artwork_dir(a->id);
        const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
        for (int k = 0; dir && k < 4; k++) {
            char *p = rx_asprintf(fmt[k], dir, a->id);
            account((int64_t)(k & 1 ? a->size240 : a->size80), p ? stick_size(dest, p) : 0, cluster, &need,
                    &overlap);
            free(p);
        }
        free(dir);
    }
    return need + overlap;
}

/* Names longer than any export.pdb row holds (docs/pdb.md 2.6: a row and
 * its index entry within one page), reported by name. */
static void report_long_names(Ctx *c, const char *what, const RxPlanName *v, int nb)
{
    for (int i = 0; i < nb; i++) {
        uint8_t *enc;
        size_t n;
        int ret = rx_pdb_string_encode(v[i].name, &enc, &n);
        if (ret >= 0)
            free(enc);
        if (ret < 0 || n > 4000)
            reportf(c, "check", "%s name too long for export.pdb (%zu bytes): %.60s...", what,
                    ret < 0 ? strlen(v[i].name) : n, v[i].name);
    }
}

/* Both databases built now, so nothing is written for a library they
 * cannot hold; write_databases() puts these bytes on the stick. */
static int build_databases(Ctx *c, const RxPlan *plan)
{
    const RxConfig *cfg = &c->lib->cfg;
    int ret = 0;
    if (cfg->export_pdb || !cfg->export_onelibrary) {
        if ((ret = rx_pdb_build(plan, &c->pdb, &c->pdb_size)) < 0) {
            reportf(c, "check", "export.pdb cannot be built: %s", strerror(-ret));
            report_long_names(c, "artist", plan->artists, plan->nb_artists);
            report_long_names(c, "album", plan->albums, plan->nb_albums);
            report_long_names(c, "genre", plan->genres, plan->nb_genres);
            report_long_names(c, "label", plan->labels, plan->nb_labels);
            report_long_names(c, "key", plan->keys, plan->nb_keys);
            for (int i = 0; i < plan->nb_playlists; i++) {
                RxPlanName n = { plan->playlists[i].id, plan->playlists[i].name, 0 };
                report_long_names(c, "playlist", &n, 1);
            }
            for (int i = 0; i < plan->nb_tags; i++) {
                RxPlanName n = { plan->tags[i].id, plan->tags[i].name, 0 };
                report_long_names(c, "My Tag", &n, 1);
            }
            return ret;
        }
        bool tags = false;
        for (int i = 0; i < plan->nb_tags; i++)
            tags |= !plan->tags[i].is_category;
        if (tags && (ret = rx_pdb_ext_build(plan, &c->ext, &c->ext_size)) < 0) {
            reportf(c, "check", "exportExt.pdb cannot be built: %s", strerror(-ret));
            return ret;
        }
    }
    if (cfg->export_onelibrary && (ret = rx_onelibrary_build(plan, &c->olib, &c->olib_size)) < 0)
        reportf(c, "check", "exportLibrary.db cannot be built: %s", strerror(-ret));
    return ret;
}

static int checks(Ctx *c, const RxPlan *plan)
{
    int ret = 0;
    char fs[64];

    for (size_t i = 0; i < RX_ARRAY_ELEMS(stick_dirs); i++) {
        if (!stick_dir_ok(c->dest, stick_dirs[i])) {
            reportf(c, "check", "%s%s is not a directory on the stick (a link?); not writing through it",
                    c->dest, stick_dirs[i]);
            ret = ret ? ret : RX_ERROR(ENOTDIR);
        }
    }

    if (!rx_stick_is_mounted(c->dest))
        reportf(c, "check", "warning: %s is not a mount point; writing into a directory", c->dest);
    else if (rx_stick_fs_type(c->dest, fs, sizeof(fs)) >= 0 && strcmp(fs, "vfat") && strcmp(fs, "exfat") &&
             strcmp(fs, "msdos") && strcmp(fs, "fat"))
        /* FAT32 on MBR is what every player reads; exFAT only the newer
         * ones (docs/pdb.md G14). */
        reportf(c, "check", "warning: %s is %s; players read FAT32 (and exFAT on the newer ones)",
                c->dest, fs);

    /* Track rows that do not fit a DeviceSQL page (docs/pdb.md 2.6),
     * named rather than failing half way. */
    for (int i = 0; i < plan->nb_tracks; i++) {
        int over = rx_pdb_track_row_overflow(&plan->tracks[i], plan);
        if (over > 0) {
            char name[512];
            rx_track_display(plan->tracks[i].track, name, sizeof(name));
            reportf(c, "check", "too much text for one export.pdb page (%d bytes over): %s", over, name);
            ret = ret ? ret : RX_ERROR(EFBIG);
        }
    }

    /* Stick paths on the stick and unique (case-insensitively, as FAT). */
    Ours o = { 0 };
    for (int i = 0; i < plan->nb_tracks; i++) {
        if (!rx_stick_path_safe(plan->tracks[i].usb_path)) {
            reportf(c, "check", "not a path on the stick: %s", plan->tracks[i].usb_path);
            ret = ret ? ret : RX_ERROR(EINVAL);
        }
        if (ours_add(&o, plan->tracks[i].usb_path) < 0)
            ret = ret ? ret : RX_ERROR(ENOMEM);
    }
    if (o.nb)
        qsort(o.paths, (size_t)o.nb, sizeof(*o.paths), cmp_str);
    for (int i = 1; i < o.nb; i++) {
        if (!strcmp(o.paths[i], o.paths[i - 1])) {
            reportf(c, "check", "two tracks map to %s", o.paths[i]);
            ret = ret ? ret : RX_ERROR(EEXIST);
        }
    }
    ours_free(&o);

    /* What a player left on the stick and the library does not have yet
     * would be lost: export.pdb and exportLibrary.db are rebuilt, the ANLZ
     * cue lists rewritten. */
    RxStickState s;
    int r = rx_stick_read(c->lib, c->dest, &s);
    if (r >= 0) {
        RxSyncItem *items;
        int nb;
        if ((r = rx_stick_diff(c->lib, &s, &items, &nb)) >= 0) {
            if (nb && !c->opt->ignore_unsynced) {
                reportf(c, "check", "stick has %d unsynced change%s (cues, history or settings a player "
                        "wrote); pull them first or export ignoring them", nb, nb == 1 ? "" : "s");
                ret = ret ? ret : RX_ERROR(EBUSY);
            } else if (nb) {
                reportf(c, "check", "warning: overwriting %d unsynced change%s on the stick", nb,
                        nb == 1 ? "" : "s");
            }
            free(items);
        }
        rx_stick_state_free(&s);
    }
    /* Not knowing is not the same as nothing to lose. */
    if (r < 0 && !c->opt->ignore_unsynced) {
        reportf(c, "check", "cannot read what players left on the stick (%s)", strerror(-r));
        ret = ret ? ret : r;
    }

    if (ret >= 0 && (r = build_databases(c, plan)) < 0)
        ret = r;

    struct statvfs vfs;
    int64_t cluster = statvfs(c->dest, &vfs) == 0 && vfs.f_frsize ? (int64_t)vfs.f_frsize : 4096;
    int64_t need = rx_export_space_needed(plan, c->dest, cluster);
    int64_t free_bytes = rx_fs_free_bytes(c->dest);
    if (free_bytes >= 0 && free_bytes < need) {
        reportf(c, "check", "not enough space on %s: %lld MiB free, %lld MiB needed", c->dest,
                (long long)(free_bytes >> 20), (long long)(need >> 20));
        ret = ret ? ret : RX_ERROR(ENOSPC);
    }
    return ret;
}

/* ------------------------------------------------------------- writing */

static int write_anlz(Ctx *c, const RxPlan *plan)
{
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RxPlanTrack *t = &plan->tracks[i];
        char name[512];
        rx_track_display(t->track, name, sizeof(name));
        if (!progress(c, "analysis", i + 1, plan->nb_tracks, name))
            return RX_ERROR(ECANCELED);
        RxWaveforms w = { 0 };
        bool have = rx_library_waveforms_load(c->lib, t->id, &w) >= 0;
        if (!have)
            reportf(c, "analysis", "no waveforms for %s (analyse it to add them)", name);
        RxAnlzFiles f;
        int ret = rx_anlz_build(t, have ? &w : NULL, &f);
        if (have)
            rx_waveforms_free(&w);
        if (ret < 0)
            return ret;
        const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
        const uint8_t *data[3] = { f.dat, f.ext, f.ex2 };
        size_t size[3] = { f.dat_size, f.ext_size, f.ex2_size };
        for (int k = 0; k < 3 && ret >= 0; k++) {
            char *p = rx_plan_anlz_path(t, ext[k]);
            ret = p ? write_file(c, p, data[k], size[k]) : RX_ERROR(ENOMEM);
            free(p);
        }
        rx_anlz_files_free(&f);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* a<id>.jpg (80 px) and a<id>_m.jpg (240 px) for export.pdb, the same as
 * b<id>... for OneLibrary's image paths (docs/onelibrary.md 3.3). */
static int write_artwork(Ctx *c, const RxPlan *plan)
{
    for (int i = 0; i < plan->nb_artworks; i++) {
        const RxPlanArtwork *a = &plan->artworks[i];
        if (!progress(c, "artwork", i + 1, plan->nb_artworks, a->path80))
            return RX_ERROR(ECANCELED);
        char *dir = rx_plan_artwork_dir(a->id);
        if (!dir)
            return RX_ERROR(ENOMEM);
        int ret = 0;
        const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
        for (int k = 0; k < 4 && ret >= 0; k++) {
            char *p = rx_asprintf(fmt[k], dir, a->id);
            ret = p ? write_file(c, p, k & 1 ? a->jpeg240 : a->jpeg80, k & 1 ? a->size240 : a->size80)
                    : RX_ERROR(ENOMEM);
            free(p);
        }
        free(dir);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int write_databases(Ctx *c, const RxPlan *plan)
{
    const RxConfig *cfg = &c->lib->cfg;
    int ret;

    /* The old sync record first: from here on it would describe databases
     * the stick no longer has, and the sync would trust its ids. Without
     * one the sync only proposes additions. */
    char *rec = rx_stick_file(c->dest, RX_SYNC_PATH);
    if (!rec)
        return RX_ERROR(ENOMEM);
    ret = unlink(rec) < 0 && errno != ENOENT ? RX_ERROR(errno) : 0;
    if (ret < 0)
        reportf(c, "database", "cannot replace %s: %s", rec, strerror(-ret));
    free(rec);
    if (ret < 0)
        return ret;

    RxSettingsFile files[4];
    int nf = 0;
    if ((ret = rx_settings_from_config(cfg, files, &nf)) < 0) {
        reportf(c, "database", "the My Settings in the library are invalid; see `rxbox settings`");
        return ret;
    }
    for (int i = 0; i < nf; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/%s", files[i].name);
        progress(c, "database", i + 1, nf + 3, path);
        if (ret >= 0)
            ret = write_file(c, path, files[i].data, files[i].size);
        free(files[i].data);
    }
    if (ret < 0)
        return ret;

    /* The databases last (built by the checks). Both by default: one stick
     * works on every player generation (docs/OVERVIEW.md 1); a library left
     * out is removed so no player shows a stale one (docs/pdb.md 1.1, G12). */
    if (c->pdb) {
        progress(c, "database", nf + 1, nf + 3, "export.pdb");
        if ((ret = write_file(c, "/PIONEER/rekordbox/export.pdb", c->pdb, c->pdb_size)) < 0)
            return ret;
        if (c->ext) {
            progress(c, "database", nf + 2, nf + 3, "exportExt.pdb");
            if ((ret = write_file(c, "/PIONEER/rekordbox/exportExt.pdb", c->ext, c->ext_size)) < 0)
                return ret;
        } else {
            remove_file(c, "/PIONEER/rekordbox/exportExt.pdb");
        }
    } else {
        remove_file(c, "/PIONEER/rekordbox/export.pdb");
        remove_file(c, "/PIONEER/rekordbox/exportExt.pdb");
    }
    remove_file(c, "/PIONEER/rekordbox/exportLibrary.db-wal");
    remove_file(c, "/PIONEER/rekordbox/exportLibrary.db-shm");
    if (c->olib) {
        progress(c, "database", nf + 3, nf + 3, "exportLibrary.db");
        if ((ret = write_file(c, "/PIONEER/rekordbox/exportLibrary.db", c->olib, c->olib_size)) < 0)
            return ret;
    } else {
        remove_file(c, "/PIONEER/rekordbox/exportLibrary.db");
    }

    /* The sync record: what the stick holds now, so later changes can be
     * told to be a player's, and whose library wrote it. */
    RxSyncRecord r;
    if ((ret = rx_sync_record_from_plan(plan, &r)) < 0)
        return ret;
    if ((ret = rx_sync_library_key(c->lib, r.library)) >= 0) {
        rx_sync_record_add_settings(&r, c->dest);
        ret = rx_sync_record_write(c->dest, &r);
    }
    rx_sync_record_free(&r);
    return ret;
}

static int prune(Ctx *c, const RxPlan *plan, bool dry)
{
    Ours o = { 0 };
    KeepSet k = { 0 };
    RxArray extra;
    rx_array_init(&extra, sizeof(char *));
    int ret = collect_ours(plan, &o);
    if (ret >= 0)
        ret = collect_keep(c, plan, &o, &k);
    const char *roots[3] = { "/Contents", "/PIONEER/USBANLZ", "/PIONEER/Artwork" };
    for (int i = 0; i < 3 && ret >= 0; i++) {
        /* checks() refused links already; this keeps it so. */
        if (!stick_dir_ok(c->dest, roots[i]) || !stick_dir_ok(c->dest, "/PIONEER")) {
            ret = RX_ERROR(ENOTDIR);
            break;
        }
        ret = find_extra(c, &o, &k, roots[i], &extra);
    }
    for (size_t i = 0; i < extra.nb && ret >= 0; i++) {
        const char *p = *(char **)rx_array_at(&extra, i);
        if (!progress(c, "prune", (int)i + 1, (int)extra.nb, p)) {
            ret = RX_ERROR(ECANCELED);
            break;
        }
        if (dry || !c->opt->prune)
            continue;
        char *full = rx_stick_file(c->dest, p);
        if (full && unlink(full) < 0)
            reportf(c, "prune", "cannot remove %s: %s", p, strerror(errno));
        free(full);
    }
    if (!dry && c->opt->prune && ret >= 0) {
        for (int i = 0; i < 3; i++) {
            char *full = rx_stick_file(c->dest, roots[i]);
            if (full)
                prune_empty(full);
            free(full);
        }
    } else if (!dry && extra.nb && ret >= 0) {
        reportf(c, "prune", "%zu file%s on the stick %s not part of this export (prune to remove)",
                extra.nb, extra.nb == 1 ? "" : "s", extra.nb == 1 ? "is" : "are");
    }
    for (size_t i = 0; i < extra.nb; i++)
        free(*(char **)rx_array_at(&extra, i));
    rx_array_free(&extra);
    keep_free(&k);
    ours_free(&o);
    return ret;
}

/* Every file a real run would write, in the order it would. */
static void dry_run_report(Ctx *c, const RxPlan *plan)
{
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RxPlanTrack *t = &plan->tracks[i];
        char *dst = rx_stick_file(c->dest, t->usb_path);
        progress(c, dst && same_file(t->source, dst) ? "skip" : "copy", i + 1, plan->nb_tracks, t->usb_path);
        free(dst);
    }
    RxSettingsFile files[4];
    int nf = 0;
    if (rx_settings_from_config(&c->lib->cfg, files, &nf) < 0)
        nf = 0;
    int total = 3 * plan->nb_tracks + 4 * plan->nb_artworks + nf + !!c->pdb + !!c->ext + !!c->olib + 1;
    int n = 0;
    const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
    for (int i = 0; i < plan->nb_tracks; i++) {
        for (int k = 0; k < 3; k++) {
            char *p = rx_plan_anlz_path(&plan->tracks[i], ext[k]);
            progress(c, "rewrite", ++n, total, p);
            free(p);
        }
    }
    const char *fmt[4] = { "%s/a%u.jpg", "%s/a%u_m.jpg", "%s/b%u.jpg", "%s/b%u_m.jpg" };
    for (int i = 0; i < plan->nb_artworks; i++) {
        uint32_t id = plan->artworks[i].id;
        char *dir = rx_plan_artwork_dir(id);
        for (int k = 0; dir && k < 4; k++) {
            char *p = rx_asprintf(fmt[k], dir, id);
            progress(c, "rewrite", ++n, total, p);
            free(p);
        }
        free(dir);
    }
    for (int i = 0; i < nf; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/%s", files[i].name);
        progress(c, "rewrite", ++n, total, path);
        free(files[i].data);
    }
    if (c->pdb)
        progress(c, "rewrite", ++n, total, "/PIONEER/rekordbox/export.pdb");
    if (c->ext)
        progress(c, "rewrite", ++n, total, "/PIONEER/rekordbox/exportExt.pdb");
    if (c->olib)
        progress(c, "rewrite", ++n, total, "/PIONEER/rekordbox/exportLibrary.db");
    progress(c, "rewrite", ++n, total, RX_SYNC_PATH);
}

/* ------------------------------------------------------------ transcodes */

typedef struct Transcode {
    const RxTrack *t;
    char          *cache;
    int            max_rate;
    int            ret;
    RxMailbox     *done;
    RxCancel      *cancel;
    int           *finished;
} Transcode;

/* On a pool worker: into a temporary name, renamed into the cache when
 * complete, so a cancelled or failed one leaves nothing that looks made. */
static void transcode_job(void *arg)
{
    Transcode *x = arg;
    char *tmp = rx_asprintf("%s.tmp.flac", x->cache);
    if (rx_cancelled(x->cancel))
        x->ret = RX_ERROR(ECANCELED);
    else if (!tmp)
        x->ret = RX_ERROR(ENOMEM);
    else if ((x->ret = rx_transcode_flac(x->t->path, tmp, x->max_rate)) >= 0 && rename(tmp, x->cache) < 0)
        x->ret = RX_ERROR(errno);
    if (tmp && x->ret < 0)
        unlink(tmp);
    free(tmp);
    rx_mailbox_post(x->done, x);
    __atomic_add_fetch(x->finished, 1, __ATOMIC_RELEASE);
}

/* The transcodes the plan needs and the cache lacks: listed in a dry run,
 * made now otherwise, on half the CPUs (each is single-threaded). */
static int transcodes(Ctx *c, RxTrack **tracks, int nb_tracks, bool compat)
{
    Transcode *x = calloc((size_t)RX_MAX(nb_tracks, 1), sizeof(*x));
    if (!x)
        return RX_ERROR(ENOMEM);
    int n = 0, ret = 0;
    for (int i = 0; i < nb_tracks && ret >= 0; i++) {
        char *cache;
        int max_rate = 0;
        int r = rx_plan_transcode_path(c->lib, tracks[i], compat, !c->opt->dry_run, &cache, &max_rate);
        if (r < 0)
            ret = r;
        else if (r > 0 && !rx_plan_transcode_fresh(tracks[i], cache))
            x[n++] = (Transcode) { .t = tracks[i], .cache = cache, .max_rate = max_rate };
        else
            free(cache);
    }

    if (ret >= 0 && c->opt->dry_run) {
        for (int i = 0; i < n; i++) {
            char name[512];
            rx_track_display(x[i].t, name, sizeof(name));
            progress(c, "transcode", i + 1, n, name);
        }
    } else if (ret >= 0 && n) {
        RxThreadPool *pool = NULL;
        RxMailbox *mb = NULL;
        RxCancel cancel = { 0 };
        int finished = 0;
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int workers = (int)RX_CLAMP(cpus / 2, 1, n);
        if ((ret = rx_mailbox_create(&mb)) >= 0 && (ret = rx_pool_create(&pool, workers)) >= 0) {
            int queued = 0;
            for (int i = 0; i < n && ret >= 0; i++) {
                x[i].done = mb;
                x[i].cancel = &cancel;
                x[i].finished = &finished;
                if ((ret = rx_pool_submit(pool, transcode_job, &x[i])) >= 0)
                    queued++;
            }
            /* Progress from this thread as each one finishes. */
            for (int k = 0; k < queued;) {
                Transcode *d = rx_mailbox_wait(mb, 250);
                if (!d) {
                    /* All done but a report lost (the mailbox was out of
                     * memory): nothing more will come. */
                    if (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < queued)
                        continue;
                    if (!(d = rx_mailbox_take(mb)))
                        break;
                }
                char name[512];
                rx_track_display(d->t, name, sizeof(name));
                if (!progress(c, "transcode", ++k, n, name))
                    rx_cancel(&cancel);
            }
            rx_pool_wait(pool);
            for (int i = 0; i < queued && ret >= 0; i++) {
                if (x[i].ret < 0 && x[i].ret != RX_ERROR(ECANCELED)) {
                    char name[512];
                    rx_track_display(x[i].t, name, sizeof(name));
                    reportf(c, "transcode", "cannot transcode %s: %s", name, strerror(-x[i].ret));
                    ret = x[i].ret;
                }
            }
            if (ret >= 0 && rx_cancelled(&cancel))
                ret = RX_ERROR(ECANCELED);
        }
        if (pool)
            rx_pool_destroy(pool);
        if (mb)
            rx_mailbox_destroy(mb);
    }
    for (int i = 0; i < n; i++)
        free(x[i].cache);
    free(x);
    return ret;
}

int rx_export(RxLibrary *lib, const char *dest, const RxExportOptions *opt_in, rx_progress_cb cb,
              void *opaque)
{
    RxExportOptions defaults;
    rx_export_options_default(&defaults);
    const RxExportOptions *opt = opt_in ? opt_in : &defaults;
    Ctx c = {
        .lib    = lib,
        .dest   = dest,
        .opt    = opt,
        .cb     = cb,
        .opaque = opaque,
    };
    if (!rx_dir_exists(dest)) {
        reportf(&c, "check", "%s is not a directory (mount the stick first)", dest);
        return RX_ERROR(ENOTDIR);
    }

    /* Every playlist unless some were chosen. */
    RxPlaylist **all = NULL;
    const RxPlaylist *const *pls = opt->playlists;
    int nb_pls = opt->nb_playlists;
    if (!pls) {
        int n = rx_library_playlists_flat(lib, NULL, 0);
        all = calloc((size_t)RX_MAX(n, 1), sizeof(*all));
        if (!all)
            return RX_ERROR(ENOMEM);
        nb_pls = rx_library_playlists_flat(lib, all, RX_MAX(n, 0));
    }

    RxTrack **tracks = NULL;
    int nb_tracks = 0;
    RxPlan plan = { 0 };
    int ret = select_tracks(&c, all ? (const RxPlaylist *const *)all : pls, nb_pls, &tracks, &nb_tracks);
    if (ret < 0)
        goto out;

    /* Transcodes first: the plan probes what is copied. */
    bool compat = opt->compat_transcode || lib->cfg.compat_transcode;
    if ((ret = transcodes(&c, tracks, nb_tracks, compat)) < 0 || c.cancelled)
        goto out;
    if ((ret = rx_plan_build_cb(lib, tracks, nb_tracks, opt->playlists,
                                opt->playlists ? opt->nb_playlists : 0, opt->device_name, compat,
                                opt->dry_run, &plan, cb, opaque)) < 0)
        goto out;
    if (!opt->device_name && !lib->cfg.device_name) {
        char label[64];
        if (rx_stick_label(dest, label, sizeof(label)) >= 0) {
            free(plan.device_name);
            plan.device_name = rx_strdup(label);
        }
    }

    if ((ret = checks(&c, &plan)) < 0 || c.cancelled)
        goto out;

    if (opt->dry_run) {
        dry_run_report(&c, &plan);
        ret = prune(&c, &plan, true);
        goto out;
    }

    /* Audio. */
    int copied = 0;
    for (int i = 0; i < plan.nb_tracks; i++) {
        const RxPlanTrack *t = &plan.tracks[i];
        char *dst = rx_stick_file(dest, t->usb_path);
        if (!dst) {
            ret = RX_ERROR(ENOMEM);
            goto out;
        }
        if (same_file(t->source, dst)) {
            progress(&c, "skip", i + 1, plan.nb_tracks, t->usb_path);
        } else if (progress(&c, "copy", i + 1, plan.nb_tracks, t->usb_path)) {
            if ((ret = copy_file(t->source, dst)) < 0)
                reportf(&c, "copy", "cannot copy %s: %s", t->source, strerror(-ret));
            copied++;
        }
        free(dst);
        if (ret < 0)
            goto out;
        if (c.cancelled) {
            ret = RX_ERROR(ECANCELED);
            goto out;
        }
    }

    if ((ret = write_anlz(&c, &plan)) < 0 || (ret = write_artwork(&c, &plan)) < 0 ||
        (ret = write_databases(&c, &plan)) < 0)
        goto out;
    if ((ret = prune(&c, &plan, false)) < 0)
        goto out;
    reportf(&c, "done", "%d tracks, %d copied, %d playlists, %d artworks", plan.nb_tracks, copied,
            plan.nb_playlists, plan.nb_artworks);
    ret = 0;

out:
    if (c.cancelled && ret >= 0)
        ret = RX_ERROR(ECANCELED);
    free(c.pdb);
    free(c.ext);
    free(c.olib);
    rx_plan_free(&plan);
    free(tracks);
    free(all);
    return ret;
}
