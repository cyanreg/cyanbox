/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Re-reading and validating a stick: dj's `inspect` (export.pdb's
 * structural invariants, every track's audio and ANLZ files present and in
 * the hashed directory, exportLibrary.db intact and holding the same
 * tracks, the settings files decoded) plus the checks rbxport's verifier
 * makes (docs/reference/usb-export-db.md 11 there; facts only):
 *
 *  - fixed track-row values: subtype 0x24, 0x29 at 0x56, the offset-array
 *    magic 3, the bitmask rekordbox writes, a file type matching the
 *    extension (docs/pdb.md 2.11, G7);
 *  - unique track ids and case-folded audio paths;
 *  - every ANLZ file parses and its PPTH names the audio (docs/anlz.md
 *    4.1, players re-analyse otherwise);
 *  - declared cue counts match the entries parsed, and a non-empty PCOB
 *    memory list does not claim 0xFFFFFFFF (the XDJ-RX2 hang, 4.5);
 *  - previews have no zero column (Nexus firmware rejects them; rbxport);
 *  - both libraries agree on hot cue auto load per track;
 *  - every referenced file exists (audio, ANLZ, artwork, OneLibrary images),
 *    an audio file of the size its row says (a file the export encoded
 *    is not its source: the row has what was written);
 *    playlist entries point at tracks, the history row counts the tracks
 *    (docs/pdb.md G2). */

#include "rxbox/export.h"

#include <stdio.h>
#include <sys/stat.h>

#include "src/export/export_priv.h"
#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"

typedef struct Report {
    rx_report_cb line;
    void        *opaque;
    int          problems;
} Report;

static void say(Report *r, const char *fmt, ...) RX_PRINTF(2, 3);
static void say(Report *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = rx_vasprintf(fmt, ap);
    va_end(ap);
    if (msg && r->line)
        r->line(r->opaque, msg);
    free(msg);
}

static void problem(Report *r, const char *fmt, ...) RX_PRINTF(2, 3);
static void problem(Report *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = rx_vasprintf(fmt, ap);
    va_end(ap);
    r->problems++;
    if (msg && r->line) {
        char *l = rx_asprintf("   %s", msg);
        r->line(r->opaque, l ? l : msg);
        free(l);
    }
    free(msg);
}

static void pdb_problem(void *opaque, const char *p)
{
    problem(opaque, "%s", p);
}

/* Paths come from the stick's databases: one leaving the stick names
 * nothing on it, and only regular files are read (a link or FIFO on a
 * stick with a Unix file system could point at /dev/zero). */
static bool stick_exists(const char *stick, const char *path)
{
    char *p = rx_stick_path_safe(path) ? rx_stick_file(stick, path) : NULL;
    bool ok = p && rx_file_exists(p);
    free(p);
    return ok;
}

/* The file's size on the stick, -1 when it is not there. */
static int64_t stick_size_of(const char *stick, const char *path)
{
    char *p = rx_stick_path_safe(path) ? rx_stick_file(stick, path) : NULL;
    int64_t n = p ? rx_file_size(p) : -1;
    free(p);
    return n;
}

static int read_stick(const char *stick, const char *path, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    if (!rx_stick_path_safe(path))
        return RX_ERROR(EINVAL);
    char *p = rx_stick_file(stick, path);
    if (!p)
        return RX_ERROR(ENOMEM);
    struct stat st;
    int ret = lstat(p, &st) < 0 ? RX_ERROR(errno) : S_ISREG(st.st_mode) ? 0 : RX_ERROR(EINVAL);
    if (ret >= 0)
        ret = rx_file_read(p, data, size);
    free(p);
    return ret;
}

/* The DeviceSQL file type for a file name (docs/pdb.md 2.11: decide by
 * extension; .m4a holds AAC (4) or ALAC (6)). */
static bool type_matches(const char *path, int type)
{
    char ext[16];
    rx_path_ext(path, ext, sizeof(ext));
    if (!strcmp(ext, ".mp3"))
        return type == 1;
    if (!strcmp(ext, ".m4a") || !strcmp(ext, ".mp4") || !strcmp(ext, ".aac"))
        return type == 4 || type == 6;
    if (!strcmp(ext, ".flac"))
        return type == 5;
    if (!strcmp(ext, ".wav"))
        return type == 11;
    if (!strcmp(ext, ".aif") || !strcmp(ext, ".aiff"))
        return type == 12;
    return false;
}

/* One ANLZ file: parses, PPTH names the audio, cue counts as declared,
 * previews without zero columns. */
static void check_anlz(Report *r, const char *stick, const char *path, const char *audio)
{
    uint8_t *b;
    size_t n;
    if (read_stick(stick, path, &b, &n) < 0) {
        problem(r, "missing %s (for %s)", path, audio);
        return;
    }
    RXAnlzTag *tags;
    int nt;
    if (rx_anlz_parse(b, n, &tags, &nt) < 0) {
        problem(r, "%s does not parse", path);
        free(b);
        return;
    }
    char *pp = NULL;
    if (rx_anlz_read_path(b, n, &pp) < 0 || strcmp(pp, audio))
        problem(r, "%s: PPTH is \"%s\", not %s", path, pp ? pp : "(none)", audio);
    free(pp);
    for (int i = 0; i < nt; i++) {
        const RXAnlzTag *t = &tags[i];
        if (!memcmp(t->fourcc, "PCOB", 4) || !memcmp(t->fourcc, "PCO2", 4)) {
            RXAnlzCueEntry *e;
            int ne, declared;
            if (rx_anlz_tag_cues(t, &e, &ne, &declared) < 0) {
                problem(r, "%s: unreadable %s", path, t->fourcc);
                continue;
            }
            if (ne != declared)
                problem(r, "%s: %s declares %d cues, %d parse", path, t->fourcc, declared, ne);
            uint32_t type, count;
            if (!rx_anlz_pcob_header(t, &type, &count) && type == 0 && ne > 0 && count != (uint32_t)ne - 1)
                problem(r, "%s: memory cue list of %d with memory_count 0x%x (an XDJ-RX2 drops it and hangs)",
                        path, ne, count);
            rx_anlz_cue_entries_free(e, ne);
        } else if ((!memcmp(t->fourcc, "PWAV", 4) || !memcmp(t->fourcc, "PWV2", 4)) && t->raw_size >= 0x14) {
            uint32_t cols = rx_rd_be32(t->raw + 0x0C);
            uint8_t mask = t->fourcc[3] == 'V' ? 31 : 15;
            int zero = 0;
            for (uint32_t k = 0; k < cols && 0x14 + k < t->raw_size; k++)
                zero += !(t->raw[0x14 + k] & mask);
            if (zero)
                problem(r, "%s: %s has %d zero column%s (Nexus players reject the preview)", path, t->fourcc,
                        zero, zero == 1 ? "" : "s");
        }
    }
    free(tags);
    free(b);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void check_tracks(Report *r, const char *stick, const RXPdbRawTrack *t, int nb, bool verbose)
{
    for (int i = 0; i < nb; i++) {
        const RXPdbRawTrack *x = &t[i];
        const char *path = x->strings[RX_PDB_STR_FILE_PATH];
        const char *anlz = x->strings[RX_PDB_STR_ANALYZE_PATH];
        if (x->subtype != 0x24 || x->unk56 != 0x29 || x->magic != 3 ||
            (x->bitmask != 0x000C0700 && x->bitmask != 0x000D0700))
            problem(r, "%s: track row fixed values %04x/%08x/%04x/%04x, not 0024/000C0700/0029/0003",
                    path, x->subtype, x->bitmask, x->unk56, x->magic);
        if (!type_matches(path, x->file_type))
            problem(r, "%s: file type %d does not match the extension", path, x->file_type);
        int64_t size = stick_size_of(stick, path);
        if (size < 0)
            problem(r, "missing audio %s", path);
        else if ((uint32_t)size != x->file_size)
            problem(r, "%s: %lld bytes, the row says %u", path, (long long)size, x->file_size);
        char dir[64];
        if (rx_anlz_dir(path, dir, sizeof(dir)) < 0 || !rx_str_starts(anlz, dir) || strlen(anlz) < 4) {
            problem(r, "%s: analyze_path %s not under %s", path, anlz, dir);
            continue;
        }
        char *base = rx_strndup(anlz, strlen(anlz) - 4);
        const char *ext[3] = { ".DAT", ".EXT", ".2EX" };
        for (int k = 0; base && k < 3; k++) {
            char *p = rx_asprintf("%s%s", base, ext[k]);
            if (p)
                check_anlz(r, stick, p, path);
            free(p);
        }
        free(base);
        if (verbose)
            say(r, "   %4u %6.2f BPM  %s  (%s)", x->id, x->tempo / 100.0, x->strings[RX_PDB_STR_TITLE], path);
    }

    uint32_t *ids = malloc((size_t)RX_MAX(nb, 1) * sizeof(*ids));
    char **paths = calloc((size_t)RX_MAX(nb, 1), sizeof(*paths));
    for (int i = 0; ids && paths && i < nb; i++) {
        ids[i] = t[i].id;
        paths[i] = rx_plan_fold_case(t[i].strings[RX_PDB_STR_FILE_PATH]);
        if (!paths[i])
            paths[i] = rx_strdup("");
    }
    if (ids && paths) {
        qsort(ids, (size_t)nb, sizeof(*ids), cmp_u32);
        qsort(paths, (size_t)nb, sizeof(*paths), cmp_str);
        for (int i = 1; i < nb; i++) {
            if (ids[i] == ids[i - 1])
                problem(r, "track id %u used twice", ids[i]);
            if (paths[i] && paths[i - 1] && !strcmp(paths[i], paths[i - 1]))
                problem(r, "two tracks at %s (FAT names are case-insensitive)", paths[i]);
        }
    }
    for (int i = 0; paths && i < nb; i++)
        free(paths[i]);
    free(paths);
    free(ids);
}

static void check_pdb_refs(Report *r, const char *stick, const uint8_t *b, size_t size,
                           const RXPdbRawTrack *t, int nb)
{
    uint32_t *art_ids;
    char **art_paths;
    int na;
    if (rx_pdb_artwork(b, size, &art_ids, &art_paths, &na) >= 0) {
        for (int i = 0; i < na; i++) {
            if (!stick_exists(stick, art_paths[i]))
                problem(r, "missing artwork %s", art_paths[i]);
            char *m = rx_strdup(art_paths[i]);
            char *dot = m ? strrchr(m, '.') : NULL;
            if (dot) {
                *dot = '\0';
                char *mp = rx_asprintf("%s_m.jpg", m);
                if (mp && !stick_exists(stick, mp))
                    problem(r, "missing artwork %s", mp);
                free(mp);
            }
            free(m);
        }
        for (int i = 0; i < nb; i++) {
            if (!t[i].artwork_id)
                continue;
            bool found = false;
            for (int k = 0; k < na && !found; k++)
                found = art_ids[k] == t[i].artwork_id;
            if (!found)
                problem(r, "%s: artwork id %u has no artwork row", t[i].strings[RX_PDB_STR_FILE_PATH],
                        t[i].artwork_id);
        }
        for (int i = 0; i < na; i++)
            free(art_paths[i]);
        free(art_paths);
        free(art_ids);
    }

    RXPdb pdb;
    if (rx_pdb_read(b, size, &pdb) >= 0) {
        int nl = 0;
        for (int i = 0; i < pdb.nb_playlists; i++) {
            nl += !pdb.playlists[i].is_folder;
            for (int k = 0; k < pdb.playlists[i].nb; k++) {
                bool found = false;
                for (int j = 0; j < nb && !found; j++)
                    found = t[j].id == pdb.playlists[i].track_ids[k];
                if (!found)
                    problem(r, "playlist '%s' entry %d: no track %u", pdb.playlists[i].name, k + 1,
                            pdb.playlists[i].track_ids[k]);
            }
        }
        say(r, "export.pdb: %d playlists in %d folders", nl, pdb.nb_playlists - nl);
        rx_pdb_free(&pdb);
    }

    uint32_t num;
    char *date = NULL;
    if (rx_pdb_history(b, size, &num, &date) >= 0) {
        if (num != (uint32_t)nb)
            problem(r, "history row counts %u tracks, the table has %d (the CDJ-3000 shows that count)",
                    num, nb);
    } else {
        problem(r, "no history (0x13) row; players need tables 0x11-0x13 populated");
    }
    free(date);
}

static char *query_text(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    char *v = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = rx_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return v;
}

static int cmp_track_path(const void *a, const void *b)
{
    const RXPdbRawTrack *x = *(const RXPdbRawTrack *const *)a, *y = *(const RXPdbRawTrack *const *)b;
    return strcmp(x->strings[RX_PDB_STR_FILE_PATH], y->strings[RX_PDB_STR_FILE_PATH]);
}

/* The first of the sorted tracks whose path is `path`, or NULL. */
static const RXPdbRawTrack *find_track_path(const RXPdbRawTrack **v, int n, const char *path)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strcmp(v[mid]->strings[RX_PDB_STR_FILE_PATH], path) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && !strcmp(v[lo]->strings[RX_PDB_STR_FILE_PATH], path) ? v[lo] : NULL;
}

static void check_onelibrary(Report *r, const char *stick, const RXPdbRawTrack *t, int nb, bool have_pdb)
{
    uint8_t *data, *wal = NULL, *plain;
    size_t size, wal_size = 0, plain_size;
    if (read_stick(stick, "/PIONEER/rekordbox/exportLibrary.db", &data, &size) < 0)
        return;
    read_stick(stick, "/PIONEER/rekordbox/exportLibrary.db-wal", &wal, &wal_size);
    int ret = rx_sqlcipher_decrypt(data, size, rx_onelibrary_passphrase, wal, wal_size, &plain, &plain_size);
    free(data);
    free(wal);
    if (ret < 0) {
        problem(r, "exportLibrary.db does not decrypt (%s)", strerror(-ret));
        return;
    }
    sqlite3 *db;
    if (rx_onelibrary_open_plain(plain, plain_size, &db) < 0) {
        problem(r, "exportLibrary.db does not open");
        free(plain);
        return;
    }
    char *integrity = query_text(db, "PRAGMA integrity_check");
    if (!integrity || strcmp(integrity, "ok"))
        problem(r, "exportLibrary.db integrity: %s", integrity ? integrity : "(none)");

    /* The pdb's tracks by path, for a lookup per row rather than a scan:
     * a stick of thousands of tracks made the check quadratic. */
    const RXPdbRawTrack **by_path = nb > 0 ? malloc(sizeof(*by_path) * (size_t)nb) : NULL;
    int nb_by_path = by_path ? nb : 0;
    for (int i = 0; i < nb_by_path; i++)
        by_path[i] = &t[i];
    if (nb_by_path)
        qsort(by_path, (size_t)nb_by_path, sizeof(*by_path), cmp_track_path);

    sqlite3_stmt *st;
    int n = 0, same = 0;
    if (sqlite3_prepare_v2(db, "SELECT path, isHotCueAutoLoadOn FROM content", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(st, 0);
            int autoload = sqlite3_column_int(st, 1);
            n++;
            const RXPdbRawTrack *m = NULL;
            if (path && by_path) {
                m = find_track_path(by_path, nb_by_path, path);
            } else {
                /* Out of memory for the index: the plain scan. */
                for (int i = 0; path && i < nb && !m; i++)
                    if (!strcmp(t[i].strings[RX_PDB_STR_FILE_PATH], path))
                        m = &t[i];
            }
            if (m) {
                same++;
                bool on = !strcmp(m->strings[RX_PDB_STR_AUTOLOAD_HOTCUES], "ON");
                if (on != (autoload != 0))
                    problem(r, "%s: hot cue auto load is %s in export.pdb, %s in exportLibrary.db", path,
                            on ? "on" : "off", autoload ? "on" : "off");
            }
            if (path && !have_pdb && !stick_exists(stick, path))
                problem(r, "missing audio %s", path);
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT path FROM image", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(st, 0);
            if (path && !stick_exists(stick, path))
                problem(r, "missing OneLibrary artwork %s", path);
        }
        sqlite3_finalize(st);
    }
    bool agree = !have_pdb || (same == n && n == nb);
    if (!agree)
        problem(r, "exportLibrary.db holds different tracks from export.pdb (%d vs %d, %d shared)", n, nb,
                same);
    say(r, "exportLibrary.db: %d tracks, integrity %s%s", n, integrity ? integrity : "?",
        have_pdb ? (agree ? ", same tracks as export.pdb" : ", DIFFERENT tracks from export.pdb") : "");
    free(integrity);
    free(by_path);
    sqlite3_close(db);
    free(plain);
}

static void settings_line(Report *r, const char *stick)
{
    static const char *const files[4] = {
        "MYSETTING.DAT", "MYSETTING2.DAT", "DJMMYSETTING.DAT", "DEVSETTING.DAT",
    };
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/%s", files[i]);
        uint8_t *b;
        size_t n;
        if (read_stick(stick, path, &b, &n) < 0)
            continue;
        RXKv *kv;
        int nk;
        if (rx_settings_decode(files[i], b, n, &kv, &nk) < 0) {
            problem(r, "%s does not decode", files[i]);
            free(b);
            continue;
        }
        char *line = rx_asprintf("%s:", files[i]);
        for (int k = 0; line && k < nk; k++) {
            char *l = rx_asprintf("%s%s %s=%s", line, k ? "," : "", kv[k].key, kv[k].value);
            free(line);
            line = l;
        }
        if (line)
            say(r, "%s", line);
        free(line);
        rx_kv_free(kv, nk);
        free(b);
    }
}

int rx_inspect(const char *stick, rx_report_cb line, void *opaque)
{
    Report r = { .line = line, .opaque = opaque };
    uint8_t *b = NULL;
    size_t size = 0;
    bool have_pdb = read_stick(stick, "/PIONEER/rekordbox/export.pdb", &b, &size) >= 0;
    RXPdbRawTrack *t = NULL;
    int nb = 0;
    if (have_pdb) {
        int before = r.problems;
        rx_pdb_check(b, size, pdb_problem, &r);
        if (rx_pdb_raw_tracks(b, size, &t, &nb) < 0)
            problem(&r, "export.pdb: track table unreadable");
        say(&r, "export.pdb: %d tracks, %zu pages, %s", nb, size / RX_PDB_PAGE,
            r.problems == before ? "OK" : "PROBLEMS");
        check_tracks(&r, stick, t, nb, false);
        check_pdb_refs(&r, stick, b, size, t, nb);
    } else if (!stick_exists(stick, "/PIONEER/rekordbox/exportLibrary.db")) {
        say(&r, "%s: no export.pdb and no exportLibrary.db", stick);
        return RX_ERROR(ENOENT);
    }
    check_onelibrary(&r, stick, t, nb, have_pdb);

    uint8_t *ext;
    size_t ext_size;
    if (read_stick(stick, "/PIONEER/rekordbox/exportExt.pdb", &ext, &ext_size) >= 0) {
        int before = r.problems;
        rx_pdb_check(ext, ext_size, pdb_problem, &r);
        say(&r, "exportExt.pdb: %zu pages, %s", ext_size / RX_PDB_PAGE,
            r.problems == before ? "OK" : "PROBLEMS");
        free(ext);
    }
    /* The count is the caller's to report (the CLI prints its own
     * verdict line). */
    settings_line(&r, stick);
    rx_pdb_raw_tracks_free(t, nb);
    free(b);
    return r.problems;
}
