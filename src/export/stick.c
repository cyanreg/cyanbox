/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Reading back what a player left on a stick, for review and sync into the
 * library: the cues it saved into the ANLZ files, the history it appended,
 * the My Settings it wrote.
 *
 * Cues are a three-way comparison. The export leaves a sync record on the
 * stick (export_priv.h) with the cues as written; a cue the stick holds
 * that differs from the record was changed by a player, and is proposed
 * unless the library already agrees with it. Without a record (a stick
 * rekordbox or dj wrote) only additions and changes are proposed, never
 * removals: a library cue the stick lacks is more likely one added since
 * that export than one the player deleted. Memory cues match within 20 ms,
 * hot cues by slot.
 *
 * Where players write: the OMNIS-DUO saved pads into the ANLZ files only
 * and play history into exportLibrary.db (docs/onelibrary.md, Seabass's
 * hardware notes); the CDJ-3000 appends history to export.pdb's tables
 * 0x0B/0x0C (crate-digger history_playlist_row / history_entry_row). The
 * cues are read from the .EXT's PCO2 lists when present (A-H, names,
 * colours) and the active loop from the .DAT's PCOB memory list; a file
 * without PCO2 is read from its PCOB lists.
 *
 * History lists carry no times. A play is recorded when it is applied, and
 * the library remembers per stick and list how many entries were applied
 * (setting "sync.history.<stick>.<list>" = "count:crc"), so a second pull
 * of the same stick adds nothing.
 *
 * Everything on the stick is input another program wrote: paths read from
 * its databases must stay on the stick and name regular files, a damaged
 * export.pdb leaves exportLibrary.db to name the tracks, and setting values
 * the library could not store are not proposed. */

#include "rxbox/export.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef SYS_openat2
#include <linux/openat2.h>
#endif

#include "rxbox/colors.h"
#include "src/export/export_priv.h"
#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"

static const char *const settings_files[4] = {
    "MYSETTING.DAT", "MYSETTING2.DAT", "DJMMYSETTING.DAT", "DEVSETTING.DAT",
};

static const char *settings_section(const char *file)
{
    if (!strcmp(file, "DJMMYSETTING.DAT"))
        return "mixer";
    if (!strcmp(file, "DEVSETTING.DAT"))
        return "device";
    return "player";
}

/* ---------------------------------------------------------------- files */

char *rx_stick_file(const char *mount, const char *stick_path)
{
    size_t n = strlen(mount);
    while (n > 1 && mount[n - 1] == '/')
        n--;
    return rx_asprintf("%.*s%s%s", (int)n, mount, stick_path[0] == '/' ? "" : "/", stick_path);
}

bool rx_stick_path_safe(const char *p)
{
    if (!p || p[0] != '/')
        return false;
    while (*p == '/') {
        const char *e = strchr(p + 1, '/');
        size_t n = e ? (size_t)(e - p - 1) : strlen(p + 1);
        if (!n || (n == 1 && p[1] == '.') || (n == 2 && p[1] == '.' && p[2] == '.'))
            return false;
        p += 1 + n;
    }
    return true;
}

/* Every file the export writes or deletes, and every file the sync reads,
 * is resolved from a descriptor of the stick's root with openat2's
 * RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV: a symbolic link
 * anywhere on the way (FAT has none; a staging directory may, at any
 * depth) or another file system mounted inside the stick fails the call
 * (ELOOP, EXDEV) instead of leading out of it. Kernels without openat2
 * (before 5.6, or a sandbox that hides it) get the same rules one
 * component at a time: O_NOFOLLOW, and the device compared with the
 * root's. */
static int open_beneath(int dir, const char *rel, int flags, mode_t mode)
{
#ifdef SYS_openat2
    struct open_how how = {
        .flags   = (uint64_t)(flags | O_CLOEXEC),
        .mode    = flags & O_CREAT ? mode : 0,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV,
    };
    for (int tries = 0; tries < 8; tries++) {
        long fd = syscall(SYS_openat2, dir, rel, &how, sizeof(how));
        if (fd >= 0)
            return (int)fd;
        /* EAGAIN: a rename or mount raced the resolution; try again. */
        if (errno != EAGAIN)
            break;
    }
    if (errno != ENOSYS && errno != EPERM)
        return RX_ERROR(errno);
#endif
    struct stat root;
    if (fstat(dir, &root) < 0)
        return RX_ERROR(errno);
    int at = dir, ret = 0;
    const char *p = rel;
    const char *slash;
    while ((slash = strchr(p, '/'))) {
        /* The link itself, if it is one, and refused below. */
        char *name = rx_strndup(p, (size_t)(slash - p));
        int next = name ? openat(at, name, O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1;
        ret = !name ? RX_ERROR(ENOMEM) : next < 0 ? RX_ERROR(errno) : 0;
        free(name);
        struct stat st;
        if (ret >= 0 && fstat(next, &st) < 0)
            ret = RX_ERROR(errno);
        else if (ret >= 0 && S_ISLNK(st.st_mode))
            ret = RX_ERROR(ELOOP);
        else if (ret >= 0 && !S_ISDIR(st.st_mode))
            ret = RX_ERROR(ENOTDIR);
        else if (ret >= 0 && st.st_dev != root.st_dev)
            ret = RX_ERROR(EXDEV);
        if (at != dir)
            close(at);
        at = next;
        if (ret < 0) {
            if (next >= 0)
                close(next);
            return ret;
        }
        p = slash + 1;
    }
    int fd = openat(at, p, flags | O_NOFOLLOW | O_CLOEXEC, mode);
    ret = fd < 0 ? RX_ERROR(errno) : 0;
    struct stat st;
    /* A link opened as a directory is refused as openat2 refuses it. */
    if (ret == RX_ERROR(ENOTDIR) && !fstatat(at, p, &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode))
        ret = RX_ERROR(ELOOP);
    else if (ret >= 0 && fstat(fd, &st) < 0)
        ret = RX_ERROR(errno);
    else if (ret >= 0 && st.st_dev != root.st_dev)
        ret = RX_ERROR(EXDEV);
    if (at != dir)
        close(at);
    if (ret < 0 && fd >= 0)
        close(fd);
    return ret < 0 ? ret : fd;
}

int rx_stick_root(const char *mount)
{
    int fd = open(mount, O_PATH | O_DIRECTORY | O_CLOEXEC);
    return fd < 0 ? RX_ERROR(errno) : fd;
}

int rx_stick_mkdirs(int root, const char *stick_dir)
{
    if (!rx_stick_path_safe(stick_dir))
        return RX_ERROR(EINVAL);
    /* Down from the root, each directory made where missing and then
     * opened by the same rules, so a link put in its place is refused. */
    int at = root, ret = 0;
    const char *p = stick_dir + 1;
    while (*p && ret >= 0) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        char *name = rx_strndup(p, n);
        if (!name) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
        if (mkdirat(at, name, 0755) < 0 && errno != EEXIST)
            ret = RX_ERROR(errno);
        int next = ret >= 0 ? open_beneath(at, name, O_PATH | O_DIRECTORY, 0) : -1;
        if (ret >= 0 && next < 0)
            ret = next;
        free(name);
        if (at != root)
            close(at);
        at = next;
        p += n + (slash != NULL);
    }
    if (at >= 0 && at != root)
        close(at);
    return ret;
}

int rx_stick_open(int root, const char *stick_path, int flags, mode_t mode)
{
    if (!rx_stick_path_safe(stick_path))
        return RX_ERROR(EINVAL);
    int fd = open_beneath(root, stick_path + 1, flags, mode);
    if (fd != RX_ERROR(ENOENT) || !(flags & O_CREAT))
        return fd;
    /* A directory on the way is missing. */
    char *dir = rx_strndup(stick_path, (size_t)(strrchr(stick_path, '/') - stick_path));
    int ret = !dir ? RX_ERROR(ENOMEM) : dir[0] ? rx_stick_mkdirs(root, dir) : fd;
    free(dir);
    return ret < 0 ? ret : open_beneath(root, stick_path + 1, flags, mode);
}

int rx_stick_stat(int root, const char *stick_path, struct stat *st)
{
    int fd = rx_stick_open(root, stick_path, O_PATH | O_NOFOLLOW, 0);
    if (fd < 0)
        return fd;
    int ret = fstat(fd, st) < 0 ? RX_ERROR(errno) : 0;
    close(fd);
    return ret;
}

int rx_stick_unlink(int root, const char *stick_path, int flags)
{
    if (!rx_stick_path_safe(stick_path))
        return RX_ERROR(EINVAL);
    const char *base = strrchr(stick_path, '/') + 1;
    int dir = root;
    if (base - 1 != stick_path) {
        char *parent = rx_strndup(stick_path, (size_t)(base - 1 - stick_path));
        dir = parent ? rx_stick_open(root, parent, O_PATH | O_DIRECTORY, 0) : RX_ERROR(ENOMEM);
        free(parent);
        if (dir < 0)
            return dir;
    }
    int ret = unlinkat(dir, base, flags) < 0 ? RX_ERROR(errno) : 0;
    if (dir != root)
        close(dir);
    return ret;
}

int rx_stick_write(int root, const char *stick_path, const void *data, size_t size)
{
    int fd = rx_stick_open(root, stick_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fd;
    int ret = 0;
    const uint8_t *p = data;
    size_t left = size;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ret = RX_ERROR(errno);
            break;
        }
        p += n;
        left -= (size_t)n;
    }
    if (close(fd) < 0 && !ret)
        ret = RX_ERROR(errno);
    return ret;
}

int rx_stick_sync(const char *mount)
{
    int fd = open(mount, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return RX_ERROR(errno);
    int ret = syncfs(fd) < 0 ? RX_ERROR(errno) : 0;
    close(fd);
    return ret;
}

/* Only regular files: a link or a FIFO on a stick formatted with a Unix
 * file system could point the reader at /dev/zero. */
int rx_stick_read_file(int root, const char *stick_path, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    int fd = rx_stick_open(root, stick_path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    struct stat st;
    int ret = fstat(fd, &st) < 0 ? RX_ERROR(errno) : S_ISREG(st.st_mode) ? 0 : RX_ERROR(EINVAL);
    uint8_t *buf = ret >= 0 ? malloc((size_t)RX_MAX(st.st_size, 1)) : NULL;
    if (ret >= 0 && !buf)
        ret = RX_ERROR(ENOMEM);
    size_t n = 0;
    while (ret >= 0 && n < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + n, (size_t)st.st_size - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            ret = RX_ERROR(errno);
        else if (!r)
            break;
        else
            n += (size_t)r;
    }
    close(fd);
    if (ret < 0) {
        free(buf);
        return ret;
    }
    *data = buf;
    *size = n;
    return 0;
}

/* The same by mount point, for the readers that look at a file or two. */
static int read_stick_file(const char *mount, const char *stick_path, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    int root = rx_stick_root(mount);
    if (root < 0)
        return root;
    int ret = rx_stick_read_file(root, stick_path, data, size);
    close(root);
    return ret;
}

/* ------------------------------------------------------------ stick cues */

/* A hot cue without a colour: code 0 with no RGB, the slot's default pad
 * colour (rekordbox 7, rbxport), or green 00FF00 (dj, and the most common
 * code-0 RGB on rekordbox 7 sticks; docs/anlz.md 4.6). */
static bool default_pad(const RXStickCue *c)
{
    return c->hot >= 0 && !c->code && (!c->rgb || c->rgb == 0x00FF00 || c->rgb == rx_hot_cue_default_rgb(c->hot));
}

bool rx_stick_cue_equal(const RXStickCue *a, const RXStickCue *b)
{
    if (a->hot != b->hot || (a->out == 0xFFFFFFFFu) != (b->out == 0xFFFFFFFFu))
        return false;
    if (llabs((long long)a->in - (long long)b->in) > 1)
        return false;
    if (a->out != 0xFFFFFFFFu && llabs((long long)a->out - (long long)b->out) > 1)
        return false;
    if (a->hot >= 0 && (default_pad(a) || default_pad(b))) {
        if (default_pad(a) != default_pad(b))
            return false;
    } else if (a->code != b->code || (a->hot >= 0 && !a->code && a->rgb != b->rgb)) {
        return false;
    }
    if (a->hot < 0 && a->active != b->active)
        return false;
    return !strcmp(a->name, b->name);
}

void rx_stick_cue_to_track(const RXStickCue *s, const RXGrid *grid, RXCue *c)
{
    *c = (RXCue) {
        .time       = s->in / 1000.0,
        .grid_beat  = NAN,
        .hot        = s->hot,
        .loop_end   = s->out != 0xFFFFFFFFu ? s->out / 1000.0 : -1.0,
        .loop_beats = NAN,
        .active     = s->active && s->hot < 0,
    };
    snprintf(c->name, sizeof(c->name), "%s", s->name);
    if (s->hot >= 0) {
        /* A default pad is "no colour"; a named code keeps its name;
         * anything else its RGB. */
        if (s->code) {
            int code;
            uint32_t rgb;
            const char *name = rx_cue_palette_name(s->code);
            if (name && !rx_hot_cue_color(name, &code, &rgb) && code == s->code)
                snprintf(c->color, sizeof(c->color), "%s", name);
            else
                snprintf(c->color, sizeof(c->color), "#%06x", rx_cue_palette_rgb(s->code));
        } else if (!default_pad(s)) {
            snprintf(c->color, sizeof(c->color), "#%06x", s->rgb);
        }
    } else if (s->code >= 1 && s->code <= RX_TRACK_COLORS) {
        snprintf(c->color, sizeof(c->color), "%s", rx_track_color_name(s->code));
    }

    /* A cue on a beat (the player's quantize puts it there, to the ms)
     * follows the grid; so does a loop a whole number of 1/32 beats long. */
    if (!grid || !rx_grid_valid(grid))
        return;
    double b = rx_grid_time_to_beat(grid, c->time);
    double nb = round(b);
    double tb = rx_grid_beat_to_time(grid, nb);
    if (fabs(tb - c->time) > 0.0015)
        return;
    c->grid_beat = nb;
    c->time = tb;
    if (c->loop_end >= 0) {
        double lb = rx_grid_time_to_beat(grid, c->loop_end) - nb;
        double q = round(lb * 32) / 32;
        double te = rx_grid_beat_to_time(grid, nb + q);
        if (q > 0 && fabs(te - c->loop_end) <= 0.0015) {
            c->loop_beats = q;
            c->loop_end = te;
        }
    }
}

/* --------------------------------------------------------- sync record */

static void escape(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '%' || *s == '\n' || *s == '\r')
            fprintf(f, "%%%02X", (uint8_t)*s);
        else
            fputc(*s, f);
    }
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        unsigned v;
        if (*s == '%' && sscanf(s + 1, "%2x", &v) == 1 && s[1] && s[2]) {
            *o++ = (char)v;
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

void rx_sync_record_free(RXSyncRecord *r)
{
    for (int i = 0; i < r->nb_tracks; i++)
        free(r->tracks[i].cues);
    free(r->tracks);
    for (int i = 0; i < r->nb_settings; i++)
        free(r->settings[i].data);
    *r = (RXSyncRecord) { 0 };
}

static int cmp_sync_track(const void *a, const void *b)
{
    uint64_t x = ((const RXSyncTrack *)a)->id, y = ((const RXSyncTrack *)b)->id;
    return x < y ? -1 : x > y;
}

const RXSyncTrack *rx_sync_record_track(const RXSyncRecord *r, uint64_t id)
{
    if (!r->nb_tracks)
        return NULL;
    RXSyncTrack key = { .id = id };
    return bsearch(&key, r->tracks, (size_t)r->nb_tracks, sizeof(key), cmp_sync_track);
}

int rx_sync_record_from_plan(const RXPlan *plan, RXSyncRecord *r)
{
    *r = (RXSyncRecord) { 0 };
    uint8_t u[8];
    if (getrandom(u, sizeof(u), 0) != (ssize_t)sizeof(u))
        return RX_ERROR(EIO);
    for (int i = 0; i < 8; i++)
        snprintf(r->uuid + 2 * i, 3, "%02x", u[i]);
    snprintf(r->date, sizeof(r->date), "%s", plan->date);
    r->tracks = calloc((size_t)RX_MAX(plan->nb_tracks, 1), sizeof(*r->tracks));
    if (!r->tracks)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < plan->nb_tracks; i++) {
        const RXPlanTrack *t = &plan->tracks[i];
        RXSyncTrack *st = &r->tracks[r->nb_tracks++];
        st->id = t->id;
        if (!t->nb_cues)
            continue;
        if (!(st->cues = calloc((size_t)t->nb_cues, sizeof(*st->cues)))) {
            rx_sync_record_free(r);
            return RX_ERROR(ENOMEM);
        }
        for (int k = 0; k < t->nb_cues; k++)
            rx_stick_cue_from_plan(&t->cues[k], &st->cues[k]);
        st->nb_cues = t->nb_cues;
    }
    qsort(r->tracks, (size_t)r->nb_tracks, sizeof(*r->tracks), cmp_sync_track);
    return 0;
}

int rx_sync_library_key(RXLibrary *lib, char key[33])
{
    char *v = NULL;
    if (rx_library_setting_get(lib, "sync.library", &v) >= 0 && v && strlen(v) == 32) {
        memcpy(key, v, 33);
        free(v);
        return 0;
    }
    free(v);
    uint8_t u[16];
    if (getrandom(u, sizeof(u), 0) != (ssize_t)sizeof(u))
        return RX_ERROR(EIO);
    for (int i = 0; i < 16; i++)
        snprintf(key + 2 * i, 3, "%02x", u[i]);
    return rx_library_setting_set(lib, "sync.library", key);
}

/* Whether `lib` wrote the record: its key is in it. */
static bool record_is_ours(const RXLibrary *lib, const RXSyncRecord *r)
{
    char *v = NULL;
    bool ours = r->library[0] && rx_library_setting_get(lib, "sync.library", &v) >= 0 && v &&
                !strcmp(v, r->library);
    free(v);
    return ours;
}

int rx_sync_record_add_settings(RXSyncRecord *r, int root)
{
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/%s", settings_files[i]);
        uint8_t *d;
        size_t n;
        if (rx_stick_read_file(root, path, &d, &n) < 0)
            continue;
        snprintf(r->settings[r->nb_settings].name, sizeof(r->settings[0].name), "%s", settings_files[i]);
        r->settings[r->nb_settings].data = d;
        r->settings[r->nb_settings].size = n;
        r->nb_settings++;
    }
    return 0;
}

int rx_sync_record_write(int root, const RXSyncRecord *r)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&buf, &size);
    if (!f)
        return RX_ERROR(ENOMEM);
    fprintf(f, "rxbox-sync 1\nuuid %s\n", r->uuid);
    if (r->library[0])
        fprintf(f, "library %s\n", r->library);
    fprintf(f, "date %s\n", r->date);
    for (int i = 0; i < r->nb_tracks; i++) {
        const RXSyncTrack *t = &r->tracks[i];
        fprintf(f, "track %llu\n", (unsigned long long)t->id);
        for (int k = 0; k < t->nb_cues; k++) {
            const RXStickCue *c = &t->cues[k];
            long long out = c->out == 0xFFFFFFFFu ? -1 : (long long)c->out;
            if (c->hot >= 0)
                fprintf(f, "hot %d %u %lld %d %06x ", c->hot, c->in, out, c->code, c->rgb);
            else
                fprintf(f, "mem %u %lld %d %d ", c->in, out, c->code, c->active ? 1 : 0);
            escape(f, c->name);
            fputc('\n', f);
        }
    }
    for (int i = 0; i < r->nb_settings; i++) {
        fprintf(f, "setting %s ", r->settings[i].name);
        for (size_t k = 0; k < r->settings[i].size; k++)
            fprintf(f, "%02x", r->settings[i].data[k]);
        fputc('\n', f);
    }
    if (fclose(f)) {
        free(buf);
        return RX_ERROR(EIO);
    }
    int ret = rx_stick_write(root, RX_SYNC_PATH, buf, size);
    free(buf);
    return ret;
}

/* The name after `skip` space-separated fields. */
static char *field_rest(char *line, int skip)
{
    char *p = line;
    for (int i = 0; i < skip; i++) {
        p = strchr(p, ' ');
        if (!p)
            return NULL;
        p++;
    }
    return p;
}

int rx_sync_record_read(const char *mount, RXSyncRecord *r)
{
    *r = (RXSyncRecord) { 0 };
    uint8_t *data;
    size_t size;
    int ret = read_stick_file(mount, RX_SYNC_PATH, &data, &size);
    if (ret < 0)
        return ret;
    if (strncmp((char *)data, "rxbox-sync 1\n", 13)) {
        free(data);
        return RX_ERROR(EINVAL);
    }
    RXArray tracks, cues;
    rx_array_init(&tracks, sizeof(RXSyncTrack));
    rx_array_init(&cues, sizeof(RXStickCue));
    RXSyncTrack *cur = NULL;
    char *save = NULL;
    for (char *line = strtok_r((char *)data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        RXStickCue c = { 0 };
        long long out;
        unsigned long long id;
        char name[24];
        int code, active, slot;
        unsigned in, rgb;
        if (sscanf(line, "uuid %16s", r->uuid) == 1 || sscanf(line, "date %15s", r->date) == 1 ||
            sscanf(line, "library %32s", r->library) == 1)
            continue;
        if (sscanf(line, "track %llu", &id) == 1) {
            if (cur && cues.nb) {
                cur->cues = cues.data;
                cur->nb_cues = (int)cues.nb;
                rx_array_init(&cues, sizeof(RXStickCue));
            }
            RXSyncTrack t = { .id = id };
            if (!(cur = rx_array_push(&tracks, &t))) {
                ret = RX_ERROR(ENOMEM);
                break;
            }
            continue;
        }
        if (sscanf(line, "hot %d %u %lld %d %x", &slot, &in, &out, &code, &rgb) == 5) {
            c = (RXStickCue) {
                .hot  = slot,
                .in   = in,
                .out  = out < 0 ? 0xFFFFFFFFu : (uint32_t)out,
                .code = code,
                .rgb  = rgb,
            };
            char *n = field_rest(line, 6);
            snprintf(c.name, sizeof(c.name), "%s", n ? n : "");
        } else if (sscanf(line, "mem %u %lld %d %d", &in, &out, &code, &active) == 4) {
            c = (RXStickCue) {
                .hot    = -1,
                .in     = in,
                .out    = out < 0 ? 0xFFFFFFFFu : (uint32_t)out,
                .code   = code,
                .active = active != 0,
            };
            char *n = field_rest(line, 5);
            snprintf(c.name, sizeof(c.name), "%s", n ? n : "");
        } else if (sscanf(line, "setting %23s", name) == 1 && r->nb_settings < 4) {
            char *hex = field_rest(line, 2);
            size_t n = hex ? strlen(hex) / 2 : 0;
            uint8_t *d = malloc(RX_MAX(n, 1));
            if (!d) {
                ret = RX_ERROR(ENOMEM);
                break;
            }
            for (size_t k = 0; k < n; k++) {
                unsigned v = 0;
                sscanf(hex + 2 * k, "%2x", &v);
                d[k] = (uint8_t)v;
            }
            snprintf(r->settings[r->nb_settings].name, sizeof(r->settings[0].name), "%s", name);
            r->settings[r->nb_settings].data = d;
            r->settings[r->nb_settings].size = n;
            r->nb_settings++;
            continue;
        } else {
            continue;
        }
        if (!cur)
            continue;
        unescape(c.name);
        if (!rx_array_push(&cues, &c)) {
            ret = RX_ERROR(ENOMEM);
            break;
        }
    }
    if (cur && cues.nb && ret >= 0) {
        cur->cues = cues.data;
        cur->nb_cues = (int)cues.nb;
        rx_array_init(&cues, sizeof(RXStickCue));
    }
    rx_array_free(&cues);
    free(data);
    r->tracks = tracks.data;
    r->nb_tracks = (int)tracks.nb;
    if (ret < 0) {
        rx_sync_record_free(r);
        return ret;
    }
    if (r->nb_tracks)
        qsort(r->tracks, (size_t)r->nb_tracks, sizeof(*r->tracks), cmp_sync_track);
    return 0;
}

/* ------------------------------------------------------------ the read */

typedef struct HistList {
    char      source;            /* 'p' export.pdb, 'o' exportLibrary.db */
    uint32_t  id;
    char     *name;
    uint32_t *track_ids;
    int       nb;
} HistList;

typedef struct StickTrack {
    RXStickCue *cues;
    int         nb;
} StickTrack;

struct RXStickPriv {
    RXSyncRecord record;
    bool         has_record;
    StickTrack  *cues;           /* parallel to RXStickState.tracks */
    HistList    *hist;
    int          nb_hist;
    struct { char name[24]; uint8_t *data; size_t size; } files[4];
    int          nb_files;
    char         stick_key[40];  /* the record's uuid, or the stick's label */
};

void rx_stick_state_free(RXStickState *s)
{
    for (int i = 0; i < s->nb_track_states; i++) {
        free(s->tracks[i].cues);
        free(s->tracks[i].stick_path);
    }
    free(s->tracks);
    for (int i = 0; i < s->nb_plays; i++)
        free(s->plays[i].label);
    free(s->plays);
    rx_kv_free(s->settings, s->nb_settings);
    free(s->mount);
    struct RXStickPriv *p = s->priv;
    if (p) {
        rx_sync_record_free(&p->record);
        for (int i = 0; i < s->nb_track_states; i++)
            free(p->cues[i].cues);
        free(p->cues);
        for (int i = 0; i < p->nb_hist; i++) {
            free(p->hist[i].name);
            free(p->hist[i].track_ids);
        }
        free(p->hist);
        for (int i = 0; i < p->nb_files; i++)
            free(p->files[i].data);
        free(p);
    }
    *s = (RXStickState) { 0 };
}

/* pread(2) of exactly `n` bytes at `off`; a file shorter than that is
 * damaged. */
static int pread_all(int fd, void *buf, size_t n, off_t off)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, (uint8_t *)buf + done, n - done, off + (off_t)done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return RX_ERROR(errno);
        if (!r)
            return RX_ERROR(EINVAL);
        done += (size_t)r;
    }
    return 0;
}

/* An analysis file's header and cue tags (PCOB, PCO2), as an analysis file
 * of their own for rx_anlz_read_cues: a sync, and every export's check,
 * reads the cues of a stick of five thousand tracks, not the gigabyte of
 * waveforms around them. Of the other tags only the headers are read, up
 * to the waveforms after the cues (rekordbox's order, docs/anlz.md 1.2:
 * the .DAT ends with the cues; the .EXT has PWV3 before them, PQT2 and
 * the other waveforms after); a file that puts its cues anywhere else is
 * still read, header by header. */
static int read_cue_tags(int root, const char *stick_path, uint8_t **out, size_t *size)
{
    *out = NULL;
    *size = 0;
    int fd = rx_stick_open(root, stick_path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    struct stat st;
    int ret = fstat(fd, &st) < 0 ? RX_ERROR(errno) : S_ISREG(st.st_mode) ? 0 : RX_ERROR(EINVAL);
    /* The pages asked for, no read-ahead into the waveforms. */
    if (ret >= 0)
        posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    uint64_t end = ret >= 0 ? (uint64_t)st.st_size : 0;
    uint8_t h[12];
    if (ret >= 0)
        ret = pread_all(fd, h, sizeof(h), 0);
    uint64_t len = ret >= 0 ? rx_rd_be32(h + 4) : 0;
    if (ret >= 0 && (memcmp(h, "PMAI", 4) || len < 12 || len > end))
        ret = RX_ERROR(EINVAL);
    RXBuf b = { 0 };
    if (ret >= 0 && (ret = rx_buf_reserve(&b, len)) >= 0 && (ret = pread_all(fd, b.data, len, 0)) >= 0)
        b.len = len;
    bool cues = false;
    for (uint64_t off = len; ret >= 0 && off + 12 <= end; off += len) {
        if ((ret = pread_all(fd, h, sizeof(h), (off_t)off)) < 0)
            break;
        len = rx_rd_be32(h + 8);
        if (len < 12 || off + len > end) {
            ret = RX_ERROR(EINVAL);
            break;
        }
        bool cue = !memcmp(h, "PCOB", 4) || !memcmp(h, "PCO2", 4);
        if (cues && !cue && !memcmp(h, "PWV", 3))
            break;
        if (!cue)
            continue;
        cues = true;
        if ((ret = rx_buf_reserve(&b, len)) < 0 || (ret = pread_all(fd, b.data + b.len, len, (off_t)off)) < 0)
            break;
        b.len += len;
    }
    close(fd);
    if (ret < 0) {
        rx_buf_free(&b);
        return ret;
    }
    return rx_buf_finish(&b, out, size);
}

/* The cues of one track as the stick holds them. */
static int stick_cues(int root, const char *dat_path, RXStickCue **out, int *nb)
{
    *out = NULL;
    *nb = 0;
    if (!dat_path || strlen(dat_path) < 4)
        return RX_ERROR(EINVAL);
    char *ext_path = rx_strdup(dat_path);
    if (!ext_path)
        return RX_ERROR(ENOMEM);
    memcpy(ext_path + strlen(ext_path) - 4, ".EXT", 4);
    uint8_t *dat = NULL, *ext = NULL;
    size_t dat_size = 0, ext_size = 0;
    int ret = read_cue_tags(root, dat_path, &dat, &dat_size);
    if (ret >= 0)
        read_cue_tags(root, ext_path, &ext, &ext_size);
    free(ext_path);
    if (ret < 0)
        return ret;

    RXPlanCue *dc = NULL, *ec = NULL;
    int nd = 0, ne = 0;
    if ((ret = rx_anlz_read_cues(dat, dat_size, &dc, &nd)) < 0)
        goto out;
    if (ext && rx_anlz_read_cues(ext, ext_size, &ec, &ne) < 0)
        ne = 0;
    /* PCO2 present when the .EXT gave names or colours or any memory cue;
     * a .EXT read through its PCOB has only hot cues D-H. */
    RXAnlzTag *tags = NULL;
    int nt = 0;
    bool has_pco2 = false;
    if (ext && rx_anlz_parse(ext, ext_size, &tags, &nt) >= 0)
        for (int i = 0; i < nt; i++)
            has_pco2 |= !memcmp(tags[i].fourcc, "PCO2", 4);
    free(tags);

    RXStickCue *c = calloc((size_t)RX_MAX(nd + ne, 1), sizeof(*c));
    if (!c) {
        ret = RX_ERROR(ENOMEM);
        goto out;
    }
    int n = 0;
    if (has_pco2) {
        for (int i = 0; i < ne; i++)
            rx_stick_cue_from_plan(&ec[i], &c[n++]);
    } else {
        for (int i = 0; i < nd; i++)
            rx_stick_cue_from_plan(&dc[i], &c[n++]);
        for (int i = 0; i < ne; i++)
            if (ec[i].hot >= 0)
                rx_stick_cue_from_plan(&ec[i], &c[n++]);
    }
    /* Only the .DAT's PCOB memory list marks the active loop. */
    for (int i = 0; i < n; i++) {
        if (c[i].hot >= 0)
            continue;
        c[i].active = false;
        for (int k = 0; k < nd; k++)
            if (dc[k].hot < 0 && dc[k].active && llabs((long long)rx_anlz_ms(dc[k].time) - c[i].in) <= 1)
                c[i].active = true;
    }
    *out = c;
    *nb = n;
    ret = 0;

out:
    free(dc);
    free(ec);
    free(dat);
    free(ext);
    return ret;
}

typedef struct StickRow {
    uint64_t id;
    char    *path, *anlz;
} StickRow;

static int onelibrary_rows(sqlite3 *db, RXArray *rows, RXArray *hist)
{
    sqlite3_stmt *st;
    const char *sql = "SELECT content_id, path, analysisDataFilePath FROM content ORDER BY content_id";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *p = (const char *)sqlite3_column_text(st, 1);
            const char *a = (const char *)sqlite3_column_text(st, 2);
            StickRow r = {
                .id   = (uint64_t)sqlite3_column_int64(st, 0),
                .path = rx_strdup(p ? p : ""),
                .anlz = rx_strdup(a ? a : ""),
            };
            if (!rows || !r.path || !r.anlz || !rx_array_push(rows, &r)) {
                free(r.path);
                free(r.anlz);
            }
        }
        sqlite3_finalize(st);
    }
    /* history(history_id, sequenceNo, name, attribute, parent) and
     * history_content(history_id, content_id, sequenceNo). */
    if (sqlite3_prepare_v2(db, "SELECT history_id, name FROM history ORDER BY sequenceNo, history_id",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 1);
        HistList h = {
            .source = 'o',
            .id     = (uint32_t)sqlite3_column_int64(st, 0),
            .name   = rx_strdup(name ? name : ""),
        };
        sqlite3_stmt *e;
        if (sqlite3_prepare_v2(db, "SELECT content_id FROM history_content WHERE history_id = ? "
                               "ORDER BY sequenceNo", -1, &e, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(e, 1, h.id);
            RXArray ids;
            rx_array_init(&ids, sizeof(uint32_t));
            while (sqlite3_step(e) == SQLITE_ROW) {
                uint32_t id = (uint32_t)sqlite3_column_int64(e, 0);
                rx_array_push(&ids, &id);
            }
            sqlite3_finalize(e);
            h.track_ids = ids.data;
            h.nb = (int)ids.nb;
        }
        if (!h.name || !h.nb || !rx_array_push(hist, &h)) {
            free(h.name);
            free(h.track_ids);
        }
    }
    sqlite3_finalize(st);
    return 0;
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int rx_stick_read(const RXLibrary *lib, const char *mount, RXStickState *out)
{
    *out = (RXStickState) { 0 };
    struct RXStickPriv *p = calloc(1, sizeof(*p));
    out->mount = rx_strdup(mount);
    if (!p || !out->mount) {
        free(p);
        free(out->mount);
        out->mount = NULL;
        return RX_ERROR(ENOMEM);
    }
    out->priv = p;
    int ret = 0;
    RXArray rows, hist, states, plays, settings;
    rx_array_init(&rows, sizeof(StickRow));
    rx_array_init(&hist, sizeof(HistList));
    rx_array_init(&states, sizeof(out->tracks[0]));
    rx_array_init(&plays, sizeof(out->plays[0]));
    rx_array_init(&settings, sizeof(RXKv));
    StickTrack *cues = NULL;
    int nb_cues = 0;
    char **pdb_paths = NULL;
    uint64_t *ids = NULL;
    int root = rx_stick_root(mount);
    if (root < 0) {
        ret = root;
        goto fail;
    }

    p->has_record = rx_sync_record_read(mount, &p->record) >= 0;
    if (p->has_record && !record_is_ours(lib, &p->record)) {
        rx_sync_record_free(&p->record);
        p->has_record = false;
    }
    if (p->has_record)
        snprintf(p->stick_key, sizeof(p->stick_key), "%s", p->record.uuid);
    else if (rx_stick_label(mount, p->stick_key, sizeof(p->stick_key)) < 0)
        snprintf(p->stick_key, sizeof(p->stick_key), "stick");

    uint8_t *data;
    size_t size;
    size_t pdb_rows = 0;
    if (rx_stick_read_file(root, "/PIONEER/rekordbox/export.pdb", &data, &size) >= 0) {
        RXPdb pdb;
        out->has_pdb = true;
        if (rx_pdb_read(data, size, &pdb) >= 0) {
            for (int i = 0; i < pdb.nb_tracks; i++) {
                StickRow r = {
                    .id   = pdb.tracks[i].id,
                    .path = pdb.tracks[i].path,
                    .anlz = pdb.tracks[i].anlz_path,
                };
                pdb.tracks[i].path = pdb.tracks[i].anlz_path = NULL;
                if (!rx_array_push(&rows, &r)) {
                    free(r.path);
                    free(r.anlz);
                }
            }
            rx_pdb_free(&pdb);
        }
        pdb_rows = rows.nb;
        RXPdbHistoryList *hl;
        int nh;
        if (rx_pdb_history_lists(data, size, &hl, &nh) >= 0) {
            for (int i = 0; i < nh; i++) {
                HistList h = {
                    .source    = 'p',
                    .id        = hl[i].id,
                    .name      = hl[i].name,
                    .track_ids = hl[i].track_ids,
                    .nb        = hl[i].nb,
                };
                hl[i].name = NULL;
                hl[i].track_ids = NULL;
                if (!h.nb || !rx_array_push(&hist, &h)) {
                    free(h.name);
                    free(h.track_ids);
                }
            }
            rx_pdb_history_lists_free(hl, nh);
        }
        free(data);
    }
    if (rx_stick_read_file(root, "/PIONEER/rekordbox/exportLibrary.db", &data, &size) >= 0) {
        out->has_onelibrary = true;
        uint8_t *wal = NULL, *plain;
        size_t wal_size = 0, plain_size;
        rx_stick_read_file(root, "/PIONEER/rekordbox/exportLibrary.db-wal", &wal, &wal_size);
        if (rx_sqlcipher_decrypt(data, size, rx_onelibrary_passphrase, wal, wal_size, &plain,
                                 &plain_size) >= 0) {
            sqlite3 *db;
            if (rx_onelibrary_open_plain(plain, plain_size, &db) >= 0) {
                /* Track identity from export.pdb; the OneLibrary adds the
                 * tracks export.pdb does not have (it may be damaged, or
                 * be a player's half-written one) and its history. Its
                 * rows are looked up among export.pdb's by path, sorted. */
                RXArray more;
                rx_array_init(&more, sizeof(StickRow));
                onelibrary_rows(db, &more, &hist);
                pdb_paths = malloc(RX_MAX(pdb_rows, 1) * sizeof(*pdb_paths));
                if (!pdb_paths)
                    ret = RX_ERROR(ENOMEM);
                for (size_t k = 0; pdb_paths && k < pdb_rows; k++)
                    pdb_paths[k] = ((StickRow *)rx_array_at(&rows, k))->path;
                if (pdb_paths && pdb_rows)
                    qsort(pdb_paths, pdb_rows, sizeof(*pdb_paths), cmp_path);
                for (size_t i = 0; i < more.nb; i++) {
                    StickRow *m = rx_array_at(&more, i);
                    bool dup = !pdb_paths ||
                               (pdb_rows && bsearch(&m->path, pdb_paths, pdb_rows, sizeof(*pdb_paths), cmp_path));
                    if (dup || !rx_array_push(&rows, m)) {
                        free(m->path);
                        free(m->anlz);
                    }
                }
                free(pdb_paths);
                pdb_paths = NULL;
                rx_array_free(&more);
                sqlite3_close(db);
            }
            free(plain);
        }
        free(wal);
        free(data);
    }
    if (ret < 0)
        goto fail;
    out->nb_tracks = (int)rows.nb;

    cues = calloc(RX_MAX(rows.nb, 1), sizeof(*cues));
    if (!cues) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    for (size_t i = 0; i < rows.nb; i++) {
        StickRow *r = rx_array_at(&rows, i);
        RXTrack *t = rx_library_find_id(lib, r->id);
        /* Our own stick: the record names the ids. Otherwise the file name
         * must be the one the plan gives the track. */
        bool ours = t && (p->has_record ? rx_sync_record_track(&p->record, r->id) != NULL
                                        : rx_plan_usb_name_matches(t, r->path));
        if (!ours) {
            out->nb_tracks_unknown++;
            continue;
        }
        /* An analysis path that leaves the stick is not followed; players
         * compute the directory from the audio path anyway (docs/pdb.md
         * 1.3). */
        char dat[128];
        const char *anlz = r->anlz && rx_stick_path_safe(r->anlz) ? r->anlz : NULL;
        if (!anlz && rx_anlz_dir(r->path, dat, sizeof(dat) - 16) >= 0) {
            strcat(dat, "/ANLZ0000.DAT");
            anlz = dat;
        }
        RXStickCue *sc = NULL;
        int nsc = 0;
        if (stick_cues(root, anlz, &sc, &nsc) < 0)
            continue;
        RXCue *rc = calloc((size_t)RX_MAX(nsc, 1), sizeof(*rc));
        typeof(out->tracks[0]) st = {
            .track_id   = r->id,
            .cues       = rc,
            .nb_cues    = nsc,
            .stick_path = rx_strdup(r->path),
        };
        if (!rc || !st.stick_path || !rx_array_push(&states, &st)) {
            free(rc);
            free(st.stick_path);
            free(sc);
            ret = RX_ERROR(ENOMEM);
            goto fail;
        }
        for (int k = 0; k < nsc; k++)
            rx_stick_cue_to_track(&sc[k], &t->grid, &rc[k]);
        cues[nb_cues++] = (StickTrack) { .cues = sc, .nb = nsc };
    }

    /* The plays of the tracks read, looked up by id, sorted. */
    ids = malloc(RX_MAX(states.nb, 1) * sizeof(*ids));
    if (!ids) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    for (size_t i = 0; i < states.nb; i++)
        ids[i] = ((typeof(out->tracks[0]) *)rx_array_at(&states, i))->track_id;
    if (states.nb)
        qsort(ids, states.nb, sizeof(*ids), cmp_u64);
    for (size_t i = 0; i < hist.nb; i++) {
        HistList *h = rx_array_at(&hist, i);
        for (int k = 0; k < h->nb; k++) {
            uint64_t id = h->track_ids[k];
            if (!states.nb || !bsearch(&id, ids, states.nb, sizeof(*ids), cmp_u64))
                continue;
            typeof(out->plays[0]) pl = {
                .track_id  = h->track_ids[k],
                .played_at = 0,
                .label     = rx_strdup(h->name),
            };
            if (!pl.label || !rx_array_push(&plays, &pl)) {
                free(pl.label);
                ret = RX_ERROR(ENOMEM);
                goto fail;
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/PIONEER/%s", settings_files[i]);
        if (rx_stick_read_file(root, path, &data, &size) < 0)
            continue;
        RXKv *kv;
        int nk;
        if (rx_settings_decode(settings_files[i], data, size, &kv, &nk) < 0) {
            free(data);
            continue;
        }
        snprintf(p->files[p->nb_files].name, sizeof(p->files[0].name), "%s", settings_files[i]);
        p->files[p->nb_files].data = data;
        p->files[p->nb_files].size = size;
        p->nb_files++;
        for (int k = 0; k < nk; k++) {
            RXKv e = { .key = rx_asprintf("%s.%s", settings_section(settings_files[i]), kv[k].key),
                       .value = kv[k].value };
            kv[k].value = NULL;
            if (!e.key || !rx_array_push(&settings, &e)) {
                free(e.key);
                free(e.value);
            }
        }
        rx_kv_free(kv, nk);
    }

    /* Neither database nor a settings file: not a stick (a mistyped mount
     * point, or a stick not mounted yet), which must not pass for one
     * with nothing to sync. */
    if (!out->has_pdb && !out->has_onelibrary && !p->nb_files) {
        ret = RX_ERROR(ENOENT);
        goto fail;
    }

    out->tracks = states.data;
    out->nb_track_states = (int)states.nb;
    states.data = NULL;
    states.nb = 0;
    out->plays = plays.data;
    out->nb_plays = (int)plays.nb;
    plays.data = NULL;
    plays.nb = 0;
    out->settings = settings.data;
    out->nb_settings = (int)settings.nb;
    settings.data = NULL;
    settings.nb = 0;
    p->cues = cues;
    cues = NULL;
    p->hist = hist.data;
    p->nb_hist = (int)hist.nb;
    hist.data = NULL;
    hist.nb = 0;

fail:
    for (size_t i = 0; i < rows.nb; i++) {
        StickRow *r = rx_array_at(&rows, i);
        free(r->path);
        free(r->anlz);
    }
    rx_array_free(&rows);
    for (size_t i = 0; i < hist.nb; i++) {
        HistList *h = rx_array_at(&hist, i);
        free(h->name);
        free(h->track_ids);
    }
    rx_array_free(&hist);
    for (size_t i = 0; i < states.nb; i++) {
        typeof(out->tracks[0]) *st = rx_array_at(&states, i);
        free(st->cues);
        free(st->stick_path);
    }
    rx_array_free(&states);
    for (size_t i = 0; i < plays.nb; i++)
        free(((typeof(out->plays[0]) *)rx_array_at(&plays, i))->label);
    rx_array_free(&plays);
    for (size_t i = 0; i < settings.nb; i++) {
        RXKv *e = rx_array_at(&settings, i);
        free(e->key);
        free(e->value);
    }
    rx_array_free(&settings);
    if (cues) {
        for (int i = 0; i < nb_cues; i++)
            free(cues[i].cues);
        free(cues);
    }
    free(ids);
    if (root >= 0)
        close(root);
    if (ret < 0)
        rx_stick_state_free(out);
    return ret;
}

/* ------------------------------------------------------------- the diff */

static int push_item(RXArray *items, const RXSyncItem *it)
{
    return rx_array_push(items, it) ? 0 : RX_ERROR(ENOMEM);
}

/* A stick cue against the library's: equal as stored, or the same once
 * read back into the library's form (a custom pad colour the library can
 * only name by its RGB). */
static bool same_as_library(const RXStickCue *s, const RXCue *lib, const RXGrid *grid)
{
    RXStickCue l;
    rx_stick_cue_from_track(lib, &l);
    if (rx_stick_cue_equal(s, &l))
        return true;
    RXCue back;
    rx_stick_cue_to_track(s, grid, &back);
    RXStickCue colourless = *s;
    colourless.code = l.code;
    colourless.rgb = l.rgb;
    return rx_stick_cue_equal(&colourless, &l) && !strcasecmp(back.color, lib->color);
}

static const RXStickCue *find_hot(const RXStickCue *c, int n, int slot)
{
    for (int i = 0; i < n; i++)
        if (c[i].hot == slot)
            return &c[i];
    return NULL;
}

/* Memory cues match within 20 ms (export.h). */
static const RXStickCue *find_memory(const RXStickCue *c, int n, uint32_t in)
{
    for (int i = 0; i < n; i++)
        if (c[i].hot < 0 && llabs((long long)c[i].in - (long long)in) <= 20)
            return &c[i];
    return NULL;
}

static int cue_items(const RXLibrary *lib, const RXStickState *s, int k, RXArray *items)
{
    const struct RXStickPriv *p = s->priv;
    uint64_t id = s->tracks[k].track_id;
    const RXTrack *t = rx_library_find_id(lib, id);
    if (!t)
        return 0;
    const RXStickCue *sc = p->cues[k].cues;
    int ns = p->cues[k].nb;
    const RXSyncTrack *base = p->has_record ? rx_sync_record_track(&p->record, id) : NULL;
    int ret = 0;

    RXStickCue *lc = calloc((size_t)RX_MAX(t->nb_cues, 1), sizeof(*lc));
    if (!lc)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < t->nb_cues; i++)
        rx_stick_cue_from_track(&t->cues[i], &lc[i]);

    for (int slot = 0; slot < RX_HOT_CUE_SLOTS && ret >= 0; slot++) {
        const RXStickCue *sv = find_hot(sc, ns, slot);
        const RXStickCue *bv = base ? find_hot(base->cues, base->nb_cues, slot) : NULL;
        int li = rx_track_hot_cue(t, slot);
        if (base && (sv ? bv && rx_stick_cue_equal(sv, bv) : !bv))
            continue;
        RXSyncItem it = {
            .track_id = id,
            .slot     = slot,
            .accepted = true,
        };
        if (sv) {
            if (li >= 0 && same_as_library(sv, &t->cues[li], &t->grid))
                continue;
            it.kind = RX_SYNC_HOT_CUE_SET;
            rx_stick_cue_to_track(sv, &t->grid, &it.cue);
            ret = push_item(items, &it);
        } else if (base && li >= 0) {
            it.kind = RX_SYNC_HOT_CUE_CLEARED;
            it.cue = t->cues[li];
            ret = push_item(items, &it);
        }
    }
    for (int i = 0; i < ns && ret >= 0; i++) {
        if (sc[i].hot >= 0)
            continue;
        if (base && find_memory(base->cues, base->nb_cues, sc[i].in))
            continue;
        if (find_memory(lc, t->nb_cues, sc[i].in))
            continue;
        RXSyncItem it = {
            .kind     = RX_SYNC_MEMORY_ADDED,
            .track_id = id,
            .slot     = -1,
            .accepted = true,
        };
        rx_stick_cue_to_track(&sc[i], &t->grid, &it.cue);
        ret = push_item(items, &it);
    }
    /* Removals only against the record: a memory cue the export wrote, the
     * stick no longer has and the library still has. */
    for (int i = 0; base && i < base->nb_cues && ret >= 0; i++) {
        const RXStickCue *b = &base->cues[i];
        if (b->hot >= 0 || find_memory(sc, ns, b->in))
            continue;
        for (int l = 0; l < t->nb_cues; l++) {
            if (lc[l].hot >= 0 || llabs((long long)lc[l].in - (long long)b->in) > 20)
                continue;
            RXSyncItem it = {
                .kind     = RX_SYNC_MEMORY_REMOVED,
                .track_id = id,
                .slot     = -1,
                .cue      = t->cues[l],
                .accepted = true,
            };
            ret = push_item(items, &it);
            break;
        }
    }
    free(lc);
    return ret;
}

static uint32_t history_crc(const HistList *h, int count)
{
    uint32_t crc = 0;
    for (int i = 0; i < count && i < h->nb; i++) {
        uint8_t le[4];
        rx_wr_le32(le, h->track_ids[i]);
        crc = rx_crc32(crc, le, 4);
    }
    return crc;
}

static int play_items(const RXLibrary *lib, const RXStickState *s, RXArray *items)
{
    const struct RXStickPriv *p = s->priv;
    for (int i = 0; i < p->nb_hist; i++) {
        const HistList *h = &p->hist[i];
        char key[48];
        snprintf(key, sizeof(key), "sync.history.%.16s.%c%u", p->stick_key, h->source, h->id);
        /* Entries a previous pull applied (same list, same leading tracks). */
        int done = 0;
        char *v = NULL;
        if (rx_library_setting_get(lib, key, &v) >= 0 && v) {
            int count;
            unsigned crc;
            if (sscanf(v, "%d:%x", &count, &crc) == 2 && count <= h->nb && history_crc(h, count) == crc)
                done = count;
        }
        free(v);
        for (int k = done; k < h->nb; k++) {
            const RXTrack *t = rx_library_find_id(lib, h->track_ids[k]);
            bool known = false;
            for (int n = 0; t && n < s->nb_track_states && !known; n++)
                known = s->tracks[n].track_id == h->track_ids[k];
            if (!known)
                continue;
            RXSyncItem it = {
                .kind       = RX_SYNC_PLAY,
                .track_id   = t->id,
                .slot       = -1,
                .play_index = k,
                .accepted   = true,
            };
            snprintf(it.setting_key, sizeof(it.setting_key), "%s", key);
            snprintf(it.setting_value, sizeof(it.setting_value), "%d:%08x", k + 1, history_crc(h, k + 1));
            if (push_item(items, &it) < 0)
                return RX_ERROR(ENOMEM);
        }
    }
    return 0;
}

/* The canonical (decoded) form of a library value for a setting, or the
 * file's default when the library has none. */
static int library_setting(const RXLibrary *lib, const char *file, const char *key, char *buf, size_t size)
{
    const char *section = settings_section(file);
    const RXKv *kv = lib->cfg.device;
    int nb = lib->cfg.nb_device;
    if (!strcmp(section, "player")) {
        kv = lib->cfg.player;
        nb = lib->cfg.nb_player;
    } else if (!strcmp(section, "mixer")) {
        kv = lib->cfg.mixer;
        nb = lib->cfg.nb_mixer;
    }
    RXKv one = { 0 };
    for (int i = 0; i < nb; i++)
        if (!strcmp(kv[i].key, key))
            one = kv[i];
    uint8_t *data;
    size_t n;
    if (rx_settings_encode(file, one.key ? &one : NULL, one.key ? 1 : 0, &data, &n) < 0 &&
        rx_settings_encode(file, NULL, 0, &data, &n) < 0)
        return RX_ERROR(EINVAL);
    RXKv *dec;
    int nd;
    int ret = rx_settings_decode(file, data, n, &dec, &nd);
    free(data);
    if (ret < 0)
        return ret;
    ret = RX_ERROR(ENOENT);
    for (int i = 0; i < nd; i++) {
        if (!strcmp(dec[i].key, key)) {
            snprintf(buf, size, "%s", dec[i].value);
            ret = 0;
        }
    }
    rx_kv_free(dec, nd);
    return ret;
}

static int setting_items(const RXLibrary *lib, const RXStickState *s, RXArray *items)
{
    const struct RXStickPriv *p = s->priv;
    for (int i = 0; i < p->nb_files; i++) {
        const char *file = p->files[i].name;
        const uint8_t *base = NULL;
        size_t base_size = 0;
        for (int k = 0; p->has_record && k < p->record.nb_settings; k++) {
            if (!strcmp(p->record.settings[k].name, file)) {
                base = p->record.settings[k].data;
                base_size = p->record.settings[k].size;
            }
        }
        /* Untouched since the export. */
        if (base && base_size == p->files[i].size && !memcmp(base, p->files[i].data, base_size))
            continue;
        RXKv *now = NULL, *was = NULL;
        int nn = 0, nw = 0;
        if (rx_settings_decode(file, p->files[i].data, p->files[i].size, &now, &nn) < 0)
            continue;
        if (base)
            rx_settings_decode(file, base, base_size, &was, &nw);
        for (int k = 0; k < nn; k++) {
            if (was && k < nw && !strcmp(was[k].value, now[k].value))
                continue;
            /* A byte the decoder has no name for (it gives "0x99": a newer
             * firmware, or damage) would make the library's settings
             * invalid and block every export after it. */
            if (!strncmp(now[k].value, "0x", 2))
                continue;
            char lv[32];
            if (library_setting(lib, file, now[k].key, lv, sizeof(lv)) < 0 || !strcmp(lv, now[k].value))
                continue;
            RXSyncItem it = {
                .kind     = RX_SYNC_SETTING,
                .slot     = -1,
                .accepted = true,
            };
            snprintf(it.setting_key, sizeof(it.setting_key), "%s.%s", settings_section(file), now[k].key);
            snprintf(it.setting_value, sizeof(it.setting_value), "%s", now[k].value);
            snprintf(it.setting_old, sizeof(it.setting_old), "%s", lv);
            if (push_item(items, &it) < 0) {
                rx_kv_free(now, nn);
                rx_kv_free(was, nw);
                return RX_ERROR(ENOMEM);
            }
        }
        rx_kv_free(now, nn);
        rx_kv_free(was, nw);
    }
    return 0;
}

int rx_stick_diff(const RXLibrary *lib, const RXStickState *s, RXSyncItem **items, int *nb)
{
    *items = NULL;
    *nb = 0;
    if (!s->priv)
        return RX_ERROR(EINVAL);
    RXArray a;
    rx_array_init(&a, sizeof(RXSyncItem));
    int ret = 0;
    for (int k = 0; k < s->nb_track_states && ret >= 0; k++)
        ret = cue_items(lib, s, k, &a);
    if (ret >= 0)
        ret = play_items(lib, s, &a);
    if (ret >= 0)
        ret = setting_items(lib, s, &a);
    if (ret < 0) {
        rx_array_free(&a);
        return ret;
    }
    *items = a.data;
    *nb = (int)a.nb;
    return 0;
}

/* ------------------------------------------------------------ the apply */

static int set_kv(RXKv **kv, int *nb, const char *key, const char *value)
{
    for (int i = 0; i < *nb; i++) {
        if (!strcmp((*kv)[i].key, key)) {
            char *v = rx_strdup(value);
            if (!v)
                return RX_ERROR(ENOMEM);
            free((*kv)[i].value);
            (*kv)[i].value = v;
            return 0;
        }
    }
    RXKv *n = realloc(*kv, (size_t)(*nb + 1) * sizeof(*n));
    if (!n)
        return RX_ERROR(ENOMEM);
    *kv = n;
    n[*nb] = (RXKv) { .key = rx_strdup(key), .value = rx_strdup(value) };
    if (!n[*nb].key || !n[*nb].value) {
        free(n[*nb].key);
        free(n[*nb].value);
        return RX_ERROR(ENOMEM);
    }
    (*nb)++;
    return 0;
}

int rx_stick_apply(RXLibrary *lib, const RXSyncItem *items, int nb, rx_progress_cb cb, void *opaque)
{
    int applied = 0, ret = 0;
    bool settings = false;
    RXTrack **dirty = calloc((size_t)RX_MAX(nb, 1), sizeof(*dirty));
    int nd = 0;
    if (!dirty)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < nb && ret >= 0; i++) {
        const RXSyncItem *it = &items[i];
        if (!it->accepted)
            continue;
        if (cb && !cb(opaque, "apply", i, nb, it->setting_key[0] ? it->setting_key : NULL)) {
            ret = RX_ERROR(ECANCELED);
            break;
        }
        if (it->kind == RX_SYNC_SETTING) {
            const char *dot = strchr(it->setting_key, '.');
            if (!dot)
                continue;
            size_t sl = (size_t)(dot - it->setting_key);
            RXConfig *c = &lib->cfg;
            if (sl == 6 && !strncmp(it->setting_key, "player", 6))
                ret = set_kv(&c->player, &c->nb_player, dot + 1, it->setting_value);
            else if (sl == 5 && !strncmp(it->setting_key, "mixer", 5))
                ret = set_kv(&c->mixer, &c->nb_mixer, dot + 1, it->setting_value);
            else if (sl == 6 && !strncmp(it->setting_key, "device", 6))
                ret = set_kv(&c->device, &c->nb_device, dot + 1, it->setting_value);
            else
                continue;
            settings = true;
            applied++;
            continue;
        }
        RXTrack *t = rx_library_find_id(lib, it->track_id);
        if (!t)
            continue;
        int idx;
        switch (it->kind) {
        case RX_SYNC_HOT_CUE_SET:
        case RX_SYNC_MEMORY_ADDED:
            ret = rx_track_add_cue(t, &it->cue);
            break;
        case RX_SYNC_HOT_CUE_CLEARED:
            if ((idx = rx_track_hot_cue(t, it->slot)) >= 0)
                ret = rx_track_remove_cue(t, idx);
            break;
        case RX_SYNC_MEMORY_REMOVED:
            if ((idx = rx_track_memory_cue_at(t, it->cue.time, 0.020)) >= 0)
                ret = rx_track_remove_cue(t, idx);
            break;
        case RX_SYNC_PLAY:
            ret = rx_library_record_play(lib, t);
            if (ret >= 0 && it->setting_key[0])
                ret = rx_library_setting_set(lib, it->setting_key, it->setting_value);
            applied += ret >= 0;
            continue;
        default:
            continue;
        }
        if (ret < 0)
            break;
        applied++;
        bool seen = false;
        for (int k = 0; k < nd && !seen; k++)
            seen = dirty[k] == t;
        if (!seen)
            dirty[nd++] = t;
    }
    for (int k = 0; k < nd; k++) {
        int r = rx_library_save_track(lib, dirty[k]);
        if (r < 0 && ret >= 0)
            ret = r;
    }
    free(dirty);
    if (settings) {
        int r = rx_library_config_save(lib);
        if (r < 0 && ret >= 0)
            ret = r;
    }
    return ret < 0 ? ret : applied;
}

int rx_pull(RXLibrary *lib, const char *stick, rx_progress_cb cb, void *opaque)
{
    RXStickState s;
    int ret = rx_stick_read(lib, stick, &s);
    if (ret < 0)
        return ret;
    RXSyncItem *items;
    int nb;
    if ((ret = rx_stick_diff(lib, &s, &items, &nb)) >= 0) {
        ret = rx_stick_apply(lib, items, nb, cb, opaque);
        free(items);
    }
    rx_stick_state_free(&s);
    return ret;
}

/* ------------------------------------------------------- mount and label */

/* Undoes /proc/self/mountinfo's octal escapes (\040 for a space). */
static void unoctal(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' &&
            s[3] <= '7') {
            *o++ = (char)((s[1] - '0') << 6 | (s[2] - '0') << 3 | (s[3] - '0'));
            s += 3;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

/* The mount entry for `path` (its mount point exactly): source device and
 * file system type. */
static bool mount_entry(const char *path, char *dev, size_t dev_size, char *fstype, size_t fs_size)
{
    char *real = realpath(path, NULL);
    if (!real)
        return false;
    FILE *f = fopen("/proc/self/mountinfo", "r");
    bool found = false;
    char line[4096];
    while (f && fgets(line, sizeof(line), f)) {
        char mp[1024], fs[64], src[1024];
        char *sep = strstr(line, " - ");
        if (!sep || sscanf(line, "%*s %*s %*s %*s %1023s", mp) != 1 ||
            sscanf(sep + 3, "%63s %1023s", fs, src) != 2)
            continue;
        unoctal(mp);
        if (strcmp(mp, real))
            continue;
        unoctal(src);
        if (dev)
            snprintf(dev, dev_size, "%s", src);
        if (fstype)
            snprintf(fstype, fs_size, "%s", fs);
        found = true;
    }
    if (f)
        fclose(f);
    free(real);
    return found;
}

bool rx_stick_is_mounted(const char *mount)
{
    return mount_entry(mount, NULL, 0, NULL, 0);
}

int rx_stick_fs_type(const char *mount, char *buf, size_t size)
{
    return mount_entry(mount, NULL, 0, buf, size) ? 0 : RX_ERROR(ENOENT);
}

int rx_stick_label(const char *mount, char *buf, size_t size)
{
    char dev[1024];
    if (mount_entry(mount, dev, sizeof(dev), NULL, 0)) {
        char *real_dev = realpath(dev, NULL);
        DIR *d = real_dev ? opendir("/dev/disk/by-label") : NULL;
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char *link = rx_asprintf("/dev/disk/by-label/%s", e->d_name);
            char *target = link ? realpath(link, NULL) : NULL;
            if (target && !strcmp(target, real_dev)) {
                /* udev escapes unsafe characters as \xHH. */
                char *o = buf;
                for (const char *s = e->d_name; *s && (size_t)(o - buf) + 1 < size; s++) {
                    unsigned v;
                    if (s[0] == '\\' && s[1] == 'x' && isxdigit((unsigned char)s[2]) &&
                        isxdigit((unsigned char)s[3]) && sscanf(s + 2, "%2x", &v) == 1) {
                        *o++ = (char)v;
                        s += 3;
                    } else {
                        *o++ = *s;
                    }
                }
                *o = '\0';
                free(target);
                free(link);
                closedir(d);
                free(real_dev);
                return 0;
            }
            free(target);
            free(link);
        }
        if (d)
            closedir(d);
        free(real_dev);
    }
    /* udisks mounts sticks at /run/media/<user>/<label>. */
    char *real = realpath(mount, NULL);
    snprintf(buf, size, "%s", rx_path_base(real ? real : mount));
    free(real);
    return 0;
}
