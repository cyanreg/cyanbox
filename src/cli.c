/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The command line (docs/OVERVIEW.md, section 2). `rxbox` alone runs the
 * interface; the subcommands work on the library and never call into
 * src/gpu, so they run anywhere without a display (ssh, cron, the tests).
 * Results go to stdout, problems to stderr. Exit codes: 0 success, 1 usage,
 * 2 failure (stdout that could not be written included).
 *
 * The analysing subcommands use RXJobs directly with every CPU, and apply
 * each result with rx_app_apply_analysis() on an RXApp that holds only the
 * library and the jobs (no player, no window, no private state): the same
 * rules as the interface (only what is missing or an older analyser made,
 * grid locks, waveforms, automatic cues), printed as one line per track.
 * The library (a directory holding library.db) is -c/--library DIR, else
 * $RXBOX_LIBRARY, else ~/.local/share/rxbox (rx_library_default_root).
 * The analysis' spectral front end runs on a GPU when there is one
 * (rxbox/compute.h, through the jobs pool): --device NAME picks it, as it
 * picks the window's, and --no-gpu keeps the analysis on the CPU. Neither
 * needs a display.
 *
 * Arguments: options and names mix in any order after the command; `--`
 * ends the options, so every argument after it is a name, even one that
 * starts with '-'. An option's value is never empty. A usage error names
 * the problem, prints the command's usage line, and exits 1. */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "rxbox/analysis.h"
#include "rxbox/app.h"
#include "rxbox/colors.h"
#include "rxbox/export.h"
#include "rxbox/formats.h"
#include "rxbox/notation.h"

int rx_cli_main(int argc, char **argv);

enum {
    EXIT_OK    = 0,
    EXIT_USAGE = 1,
    EXIT_FAIL  = 2,
};

typedef struct Cli {
    const char *library;        /* -c/--library, NULL: the default */
    enum RXAudioBackend audio;
    const char *sink;
    const char *device;         /* --device: the window's GPU and the analysis' */
    bool  no_gpu;               /* --no-gpu: analyse on the CPU only */
    int   frames;
    bool  validation;
    bool  verbose;
    const char *cmd;
    const char *usage;          /* the command's usage line, for usage errors */
    bool  args_only;            /* past the command's `--`: no more options */
    RXApp app;                  /* lib and jobs only, see above */
    bool  lib_open;
} Cli;

static const char usage_text[] =
    "usage: rxbox [OPTIONS] [COMMAND [ARGS]]\n"
    "\n"
    "Prepares CDJ USB sticks without rekordbox. Without a command, runs the interface.\n"
    "\n"
    "Commands:\n"
    "  add PATH...              add audio files or folders to the library and analyse them\n"
    "  analyze [TRACK...]       analyse what is missing or was made by an older analyser\n"
    "                           (the whole library by default)\n"
    "      -f, --force          re-analyse what is there too (a locked grid only when\n"
    "                           the track is named)\n"
    "      -w, --what LIST      any of beats,key,waves (default: all)\n"
    "  show TRACK...            print the resolved grid, key, cues and analyser versions\n"
    "  export STICK             write the settings, every playlist and their tracks to a\n"
    "                           mounted stick (files already there are not copied again)\n"
    "      --all                every track in the library, not only those in playlists\n"
    "      -p, --playlist NAME  only this playlist and its tracks (repeatable)\n"
    "      --prune              delete files on the stick that are no longer exported\n"
    "      --compat             transcode AAC and hi-res files to 16-bit/44.1 kHz FLAC\n"
    "      --ignore-unsynced    export although the stick has player changes not synced\n"
    "      -n, --dry-run        print what would be copied and deleted, write nothing\n"
    "  inspect STICK            re-read and validate a stick\n"
    "  sync STICK [--apply]     list the cues, plays and settings a player saved on the\n"
    "                           stick that the library lacks; --apply takes them\n"
    "  pull STICK               take all of them (sync --apply)\n"
    "  rescan                   check which audio files are missing (or back)\n"
    "  check                    check the library database's integrity and count the\n"
    "                           tracks an older analyser analysed\n"
    "  settings                 list the My Settings names and values\n"
    "  config [KEY [VALUE]]     show or change a setting (analysis.bpm_min,\n"
    "                           export.key_notation, player.quantize ...)\n"
    "  init [DIR]               create a library (default: the library directory)\n"
    "  playlist list            list the playlists\n"
    "  playlist create NAME     create a playlist (\"Folder/Name\" creates the folders)\n"
    "  playlist add NAME TRACK...\n"
    "                           append tracks to a playlist\n"
    "  playlist remove NAME TRACK...\n"
    "                           take tracks out of a playlist (they stay in the library)\n"
    "  playlist delete NAME     delete a playlist (its tracks stay in the library)\n"
    "  playlist import FILE.m3u8\n"
    "                           make a playlist from an m3u8 file\n"
    "  playlist export NAME FILE.m3u8\n"
    "                           write a playlist as an m3u8 file\n"
    "\n"
    "  A TRACK is an audio file in the library (any path to it), a folder (every\n"
    "  library track under it) or a library id.\n"
    "\n"
    "Options:\n"
    "  -c, --library DIR        the library (default: $RXBOX_LIBRARY, else\n"
    "                           ~/.local/share/rxbox)\n"
    "  -v, --verbose            more messages\n"
    "  --device NAME            the GPU whose name contains NAME, for the analysis and the\n"
    "                           interface (default: $RXBOX_GPU_DEVICE, else the best one)\n"
    "  --no-gpu                 analyse on the CPU only (as RXBOX_ANALYSIS_GPU=0)\n"
    "  --                       after a command: the rest are names, even ones starting\n"
    "                           with -\n"
    "  -h, --help               this text\n"
    "  -V, --version            the version\n"
    "\n"
    "Interface options:\n"
    "  --audio pipewire|none    audio output; none plays to no device at all\n"
    "  --audio-sink NAME        the PipeWire node to play to\n"
    "  --validation             enable the Vulkan validation layer\n"
    "  --frames N               draw N frames, then exit (tests)\n";

/* ------------------------------------------------------------------ helpers */

static int fail(const char *fmt, ...) RX_PRINTF(1, 2);
static int fail(const char *fmt, ...)
{
    va_list args;
    fputs("rxbox: ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    return EXIT_FAIL;
}

/* cli NULL (or no command yet): no usage line, the pointer to --help only. */
static int usage_error(const Cli *cli, const char *fmt, ...) RX_PRINTF(2, 3);
static int usage_error(const Cli *cli, const char *fmt, ...)
{
    va_list args;
    fputs("rxbox: ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    if (cli && cli->usage)
        fprintf(stderr, "usage: rxbox %s\n", cli->usage);
    fputs("Try 'rxbox --help'.\n", stderr);
    return EXIT_USAGE;
}

/* Matches `-s VALUE`, `--long VALUE` and `--long=VALUE` at argv[*i].
 * Returns 1 with the value (and *i on it), 0 when it is another option or
 * past `--`, -1 when the value is missing or empty. */
static int opt_value(const Cli *cli, int argc, char **argv, int *i, const char *s, const char *l,
                     const char **val)
{
    const char *a = argv[*i];
    if (cli->args_only)
        return 0;
    size_t n = l ? strlen(l) : 0;
    if (l && !strncmp(a, l, n) && a[n] == '=') {
        *val = a + n + 1;
        return **val ? 1 : -1;
    }
    if ((s && !strcmp(a, s)) || (l && !strcmp(a, l))) {
        /* *i stays on the option for the error message. */
        if (*i + 1 >= argc || !argv[*i + 1][0])
            return -1;
        *val = argv[++*i];
        return 1;
    }
    return 0;
}

static bool opt_flag(const Cli *cli, const char *a, const char *s, const char *l)
{
    return !cli->args_only && ((s && !strcmp(a, s)) || (l && !strcmp(a, l)));
}

/* An option this command does not know, unless past `--` ("-" alone is a
 * name). */
static bool is_option(const Cli *cli, const char *a)
{
    return !cli->args_only && a[0] == '-' && a[1];
}

/* A count: decimal digits only, at most `max`. */
static bool parse_count(const char *s, long max, long *out)
{
    if (!s[0])
        return false;
    long n = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9' || n > (max - (*p - '0')) / 10)
            return false;
        n = n * 10 + (*p - '0');
    }
    *out = n;
    return true;
}

/* Options valid before and after the command, and the command's `--`.
 * 1 matched, 0 not, -1 error (printed). */
static int common_option(Cli *cli, int argc, char **argv, int *i)
{
    if (cli->args_only)
        return 0;
    if (cli->cmd && !strcmp(argv[*i], "--")) {
        cli->args_only = true;
        return 1;
    }

    const char *val;
    int r = opt_value(cli, argc, argv, i, "-c", "--library", &val);
    if (r < 0) {
        usage_error(cli, "%s needs a directory", argv[*i]);
        return -1;
    }
    if (r > 0) {
        cli->library = val;
        return 1;
    }
    if (opt_flag(cli, argv[*i], "-v", "--verbose")) {
        cli->verbose = true;
        rx_log_set_level(RX_LOG_DEBUG);
        return 1;
    }
    r = opt_value(cli, argc, argv, i, NULL, "--device", &val);
    if (r < 0) {
        usage_error(cli, "--device needs a GPU name");
        return -1;
    }
    if (r > 0) {
        cli->device = val;
        return 1;
    }
    if (opt_flag(cli, argv[*i], NULL, "--no-gpu")) {
        cli->no_gpu = true;
        return 1;
    }
    return 0;
}

/* An absolute path for one that may not exist yet. */
static char *abs_path(const char *path)
{
    char *real = rx_path_abs(path);
    if (real)
        return real;
    char *p = rx_path_expand(path);
    if (!p || p[0] == '/')
        return p;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        free(p);
        return NULL;
    }
    char *out = rx_path_join(cwd, p);
    free(p);
    return out;
}

static char *library_root(const Cli *cli)
{
    return cli->library ? abs_path(cli->library) : rx_library_default_root();
}

/* Opens the library; `create` lets a missing one be created (add). */
static int open_library(Cli *cli, bool create)
{
    char *root = library_root(cli);
    if (!root)
        return fail("out of memory");

    char *db = rx_path_join(root, "library.db");
    bool exists = db && rx_file_exists(db);
    free(db);
    if (!exists && !create) {
        int ret = fail("no library at %s (run `rxbox init` or `rxbox add`)", root);
        free(root);
        return ret;
    }

    int ret = rx_library_open(&cli->app.lib, root);
    if (ret < 0) {
        int code = fail("cannot open the library at %s: %s", root, strerror(-ret));
        free(root);
        return code;
    }
    free(root);
    cli->lib_open = true;
    return EXIT_OK;
}

static void cli_close(Cli *cli)
{
    if (cli->app.jobs)
        rx_jobs_destroy(cli->app.jobs);
    cli->app.jobs = NULL;
    if (cli->lib_open)
        rx_library_close(&cli->app.lib);
    cli->lib_open = false;
}

/* "128", "127.5", "174.123": as few digits as the value needs. */
static const char *fmt_bpm(double bpm, char *buf, size_t size)
{
    snprintf(buf, size, "%.3f", bpm);
    char *end = buf + strlen(buf);
    while (end > buf && end[-1] == '0')
        *--end = '\0';
    if (end > buf && end[-1] == '.')
        *--end = '\0';
    return buf;
}

static const char *fmt_flags(unsigned flags, char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int bit = 0; bit < 31 && len < size; bit++) {
        if (!(flags & (1u << bit)))
            continue;
        const char *name = rx_grid_flag_name((enum RXGridFlag)(1u << bit));
        if (!name)
            continue;
        len += snprintf(buf + len, size - len, "%s%s", len ? ", " : "", name);
    }
    return buf;
}

static const char *fmt_date(int64_t when, char *buf, size_t size)
{
    time_t tt = (time_t)when;
    struct tm tm;
    if (!when || !localtime_r(&tt, &tm))
        snprintf(buf, size, "never");
    else
        strftime(buf, size, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

static const char *title_of(const RXTrack *t)
{
    return t->meta.title && t->meta.title[0] ? t->meta.title : rx_path_base(t->path);
}

static bool push_unique(RXArray *tracks, RXTrack *t)
{
    for (size_t i = 0; i < tracks->nb; i++) {
        if (*(RXTrack **)rx_array_at(tracks, i) == t)
            return true;
    }
    return rx_array_push(tracks, &t) != NULL;
}

static bool all_digits(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
    }
    return true;
}

/* Appends the tracks an argument names: a file in the library (any path to
 * it), a directory (every library track under it) or a library id. Returns
 * the number found, or a negative errno with the problem printed. */
static int resolve_tracks(RXLibrary *lib, const char *arg, RXArray *out)
{
    char *abs = rx_path_abs(arg);
    if (abs && rx_dir_exists(abs)) {
        size_t n = strlen(abs);
        int found = 0;
        for (int i = 0; i < lib->nb_tracks; i++) {
            RXTrack *t = lib->tracks[i];
            if (!strncmp(t->path, abs, n) && (t->path[n] == '/' || (n == 1 && abs[0] == '/'))) {
                if (!push_unique(out, t)) {
                    free(abs);
                    return RX_ERROR(ENOMEM);
                }
                found++;
            }
        }
        free(abs);
        abs = NULL;
        if (found)
            return found;
        /* A number that happens to name a directory here (a year, say)
         * with no library tracks under it is still an id. */
        if (!all_digits(arg)) {
            fail("%s: no library tracks under it (rxbox add it first)", arg);
            return RX_ERROR(ENOENT);
        }
    }

    /* A track whose file went missing is still found by its path. */
    if (!abs)
        abs = abs_path(arg);
    RXTrack *t = abs ? rx_library_find_path(lib, abs) : NULL;
    free(abs);
    if (!t && all_digits(arg))
        t = rx_library_find_id(lib, strtoull(arg, NULL, 10));
    if (!t) {
        if (all_digits(arg))
            fail("%s: no track with this id or path in the library", arg);
        else if (rx_file_exists(arg))
            fail("%s is not in the library (rxbox add it first)", arg);
        else
            fail("%s: no such file", arg);
        return RX_ERROR(ENOENT);
    }
    return push_unique(out, t) ? 1 : RX_ERROR(ENOMEM);
}

/* ----------------------------------------------------------------- analysis */

static void analysis_options(const RXConfig *cfg, RXAnalysisOptions *opt)
{
    rx_analysis_options_default(opt);
    if (cfg->bpm_min > 0 && cfg->bpm_max > cfg->bpm_min) {
        opt->bpm_min = cfg->bpm_min;
        opt->bpm_max = cfg->bpm_max;
    }
    opt->snap_bpm = cfg->snap_bpm;
    opt->allow_sections = cfg->allow_sections;
}

typedef struct Pending {
    uint64_t request;
    bool     override_lock;
} Pending;

static void print_analysed(int n, int total, const RXTrack *t, const RXJobResult *r, int err)
{
    char name[256];
    rx_track_display(t, name, sizeof(name));
    if (err < 0) {
        printf("[%d/%d] FAILED %s: %s\n", n, total, t->path, strerror(-err));
        return;
    }

    printf("[%d/%d] %s:", n, total, name);
    if (rx_grid_valid(&t->grid)) {
        char bpm[32];
        printf(" %s BPM (confidence %.2f%s)", fmt_bpm(rx_grid_bpm(&t->grid), bpm, sizeof(bpm)),
               t->grid_confidence, t->grid_locked ? ", locked" : "");
        if (t->grid.nb_sections > 1)
            printf(", %d sections", t->grid.nb_sections);
    } else {
        printf(" no grid");
    }
    if (t->key[0]) {
        char camelot[8];
        printf(", key %s (%s, confidence %.2f)", t->key,
               rx_key_to_camelot(t->key, camelot, sizeof(camelot)), t->key_confidence);
    }
    char flags[256];
    if (t->grid_flags)
        printf(" [%s]", fmt_flags(t->grid_flags, flags, sizeof(flags)));
    printf(", %.1f s\n", r->seconds);
}

/* Analyses what each track lacks (all of `what` when force), on every CPU,
 * one line per track. override_lock: a forced analysis replaces locked
 * grids (only for tracks named on the command line). Returns the number
 * of failures. */
static int analyse_tracks(Cli *cli, RXTrack **tracks, int nb, unsigned what, bool force,
                          bool override_lock)
{
    RXApp *app = &cli->app;

    unsigned *need = calloc(RX_MAX(nb, 1), sizeof(*need));
    Pending *pending = calloc(RX_MAX(nb, 1), sizeof(*pending));
    if (!need || !pending) {
        free(need);
        free(pending);
        fail("out of memory");
        return RX_MAX(nb, 1);
    }

    /* stale: tracks analysed again for what an older analyser made. */
    int todo = 0, stale = 0;
    for (int i = 0; i < nb; i++) {
        need[i] = rx_app_analysis_needed(app, tracks[i], what, force);
        if (tracks[i]->grid_locked && !override_lock)
            need[i] &= ~RX_ANALYSE_BEATS;
        todo += need[i] != 0;
        stale += (need[i] & rx_app_analysis_stale(app, tracks[i])) != 0;
    }
    if (!todo) {
        free(need);
        free(pending);
        printf("nothing to analyse\n");
        return 0;
    }

    int failed = 0;
    if (!app->jobs) {
        int ret = rx_jobs_create(&app->jobs, 0);
        if (ret < 0) {
            free(need);
            free(pending);
            fail("cannot start the analysis threads: %s", strerror(-ret));
            return todo;
        }
        rx_jobs_set_gpu(app->jobs, !cli->no_gpu, cli->device);
    }

    RXAnalysisOptions opt;
    analysis_options(&app->lib.cfg, &opt);
    /* Several tracks: the pool is the parallelism. */
    opt.deterministic_threads = todo > 1;

    printf("analysing %d track%s", todo, todo == 1 ? "" : "s");
    if (stale)
        printf(" (%d made by an older analyser)", stale);
    printf("\n");
    fflush(stdout);

    int queued = 0, done = 0;
    for (int i = 0; i < nb; i++) {
        if (!need[i])
            continue;
        RXTrack *t = tracks[i];
        uint64_t req = rx_jobs_analyse(app->jobs, t->id, t->path, &opt, need[i]);
        if ((int64_t)req < 0) {
            print_analysed(++done, todo, t, NULL, (int)(int64_t)req);
            failed++;
            continue;
        }
        pending[queued++] = (Pending) {
            .request       = req,
            .override_lock = override_lock && force && t->grid_locked,
        };
    }

    int received = 0;
    while (received < queued) {
        RXJobResult *r = rx_jobs_wait(app->jobs, 1000);
        if (!r) {
            /* Workers post before they count down: nothing pending and
             * nothing posted means nothing more will come. */
            if (!rx_jobs_pending(app->jobs) && !(r = rx_jobs_poll(app->jobs)))
                break;
            if (!r)
                continue;
        }
        received++;

        bool override = false;
        for (int i = 0; i < queued; i++) {
            if (pending[i].request == r->request)
                override = pending[i].override_lock;
        }

        RXTrack *t = rx_library_find_id(&app->lib, r->track_id);
        if (t) {
            int err = r->err < 0 ? r->err : 0;
            if (!err && !r->analysis)
                err = RX_ERROR(EINVAL);
            if (!err && override) {
                /* Replacing a locked grid on request: unlocked for the
                 * apply, then the lock is put back and saved again. */
                t->grid_locked = false;
                err = rx_app_apply_analysis(app, t, r->analysis);
                t->grid_locked = true;
                int ret = rx_library_save_track(&app->lib, t);
                if (!err && ret < 0)
                    err = ret;
            } else if (!err) {
                err = rx_app_apply_analysis(app, t, r->analysis);
            }
            print_analysed(++done, todo, t, r, err);
            fflush(stdout);
            failed += err < 0;
        }
        rx_job_result_free(r);
    }
    failed += queued - received;

    free(need);
    free(pending);
    return failed;
}

/* ---------------------------------------------------------------- playlists */

typedef struct PlaylistRef {
    RXPlaylist *p;
    char path[512];             /* "Folder/Sub/Name" */
} PlaylistRef;

static void collect_playlists(RXPlaylistFolder *f, const char *prefix, RXArray *out)
{
    for (int i = 0; i < f->nb_playlists; i++) {
        PlaylistRef ref = { .p = f->playlists[i] };
        snprintf(ref.path, sizeof(ref.path), "%s%s", prefix, f->playlists[i]->name);
        rx_array_push(out, &ref);
    }
    for (int i = 0; i < f->nb_folders; i++) {
        char sub[512];
        snprintf(sub, sizeof(sub), "%s%s/", prefix, f->folders[i]->name);
        collect_playlists(f->folders[i], sub, out);
    }
}

/* By its full name ("Folder/Name"), else by a name that must be unique. */
static RXPlaylist *find_playlist(RXLibrary *lib, const char *name)
{
    RXArray all;
    rx_array_init(&all, sizeof(PlaylistRef));
    if (lib->root_folder)
        collect_playlists(lib->root_folder, "", &all);

    RXPlaylist *found = NULL;
    int matches = 0;
    for (size_t i = 0; i < all.nb && !found; i++) {
        PlaylistRef *ref = rx_array_at(&all, i);
        if (!strcmp(ref->path, name))
            found = ref->p;
    }
    for (size_t i = 0; i < all.nb && !found; i++) {
        PlaylistRef *ref = rx_array_at(&all, i);
        if (!strcmp(ref->p->name, name))
            matches++;
    }
    if (!found && matches == 1) {
        for (size_t i = 0; i < all.nb; i++) {
            PlaylistRef *ref = rx_array_at(&all, i);
            if (!strcmp(ref->p->name, name))
                found = ref->p;
        }
    }

    if (!found && matches > 1) {
        fail("there are %d playlists named %s; give the folder too:", matches, name);
        for (size_t i = 0; i < all.nb; i++) {
            PlaylistRef *ref = rx_array_at(&all, i);
            if (!strcmp(ref->p->name, name))
                fprintf(stderr, "  %s\n", ref->path);
        }
    } else if (!found) {
        fail("no playlist named %s (rxbox playlist list)", name);
    }
    rx_array_free(&all);
    return found;
}

static void print_folder(const RXPlaylistFolder *f, int depth)
{
    for (int i = 0; i < f->nb_folders; i++) {
        printf("%*s%s/\n", depth * 2, "", f->folders[i]->name);
        print_folder(f->folders[i], depth + 1);
    }
    for (int i = 0; i < f->nb_playlists; i++) {
        const RXPlaylist *p = f->playlists[i];
        printf("%*s%s (%d track%s)\n", depth * 2, "", p->name, p->nb, p->nb == 1 ? "" : "s");
    }
}

static int playlist_list(Cli *cli)
{
    RXLibrary *lib = &cli->app.lib;
    printf("Collection (%d track%s)\n", lib->nb_tracks, lib->nb_tracks == 1 ? "" : "s");
    if (lib->root_folder)
        print_folder(lib->root_folder, 0);
    return EXIT_OK;
}

static int playlist_create(Cli *cli, const char *name)
{
    RXLibrary *lib = &cli->app.lib;
    char *copy = rx_strdup(name);
    if (!copy)
        return fail("out of memory");

    /* Every component but the last is a folder, created when missing. */
    RXPlaylistFolder *folder = lib->root_folder;
    char *leaf = copy;
    for (char *slash; (slash = strchr(leaf, '/')); leaf = slash + 1) {
        *slash = '\0';
        if (!*leaf)
            continue;
        RXPlaylistFolder *next = NULL;
        for (int i = 0; folder && i < folder->nb_folders; i++) {
            if (!strcmp(folder->folders[i]->name, leaf))
                next = folder->folders[i];
        }
        if (!next)
            next = rx_library_folder_create(lib, folder, leaf);
        if (!next) {
            int ret = fail("cannot create the playlist folder %s", leaf);
            free(copy);
            return ret;
        }
        folder = next;
    }

    int ret = EXIT_OK;
    if (!*leaf) {
        ret = usage_error(cli, "%s: the playlist needs a name", name);
        goto end;
    }
    for (int i = 0; folder && i < folder->nb_playlists; i++) {
        if (!strcmp(folder->playlists[i]->name, leaf)) {
            ret = fail("the playlist %s already exists", name);
            goto end;
        }
    }
    RXPlaylist *p = rx_library_playlist_create(lib, folder, leaf);
    if (!p) {
        ret = fail("cannot create the playlist %s", name);
        goto end;
    }
    printf("created playlist %s\n", name);

end:
    free(copy);
    return ret;
}

static int playlist_add(Cli *cli, const char *name, char **args, int nb_args)
{
    RXLibrary *lib = &cli->app.lib;
    RXPlaylist *p = find_playlist(lib, name);
    if (!p)
        return EXIT_FAIL;

    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    int ret = EXIT_OK;
    for (int i = 0; i < nb_args; i++) {
        if (resolve_tracks(lib, args[i], &tracks) < 0)
            ret = EXIT_FAIL;
    }

    int added = 0, present = 0;
    for (size_t i = 0; i < tracks.nb; i++) {
        RXTrack *t = *(RXTrack **)rx_array_at(&tracks, i);
        bool in = false;
        for (int k = 0; k < p->nb && !in; k++)
            in = p->track_ids[k] == t->id;
        if (in) {
            present++;
            continue;
        }
        int err = rx_library_playlist_add(lib, p, t, -1);
        if (err < 0) {
            ret = fail("cannot add %s to %s: %s", t->path, name, strerror(-err));
            continue;
        }
        added++;
    }
    printf("added %d track%s to %s", added, added == 1 ? "" : "s", p->name);
    if (present)
        printf(" (%d already in it)", present);
    printf("; %d in total\n", p->nb);

    rx_array_free(&tracks);
    return ret;
}

static int playlist_remove(Cli *cli, const char *name, char **args, int nb_args)
{
    RXLibrary *lib = &cli->app.lib;
    RXPlaylist *p = find_playlist(lib, name);
    if (!p)
        return EXIT_FAIL;

    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    int ret = EXIT_OK;
    for (int i = 0; i < nb_args; i++) {
        if (resolve_tracks(lib, args[i], &tracks) < 0)
            ret = EXIT_FAIL;
    }

    int removed = 0, absent = 0;
    for (size_t i = 0; i < tracks.nb; i++) {
        RXTrack *t = *(RXTrack **)rx_array_at(&tracks, i);
        int at = -1;
        for (int k = p->nb - 1; k >= 0 && at < 0; k--) {
            if (p->track_ids[k] == t->id)
                at = k;
        }
        if (at < 0) {
            absent++;
            continue;
        }
        int err = rx_library_playlist_remove_at(lib, p, at);
        if (err < 0) {
            ret = fail("cannot remove %s from %s: %s", t->path, name, strerror(-err));
            continue;
        }
        removed++;
    }
    printf("removed %d track%s from %s", removed, removed == 1 ? "" : "s", p->name);
    if (absent)
        printf(" (%d not in it)", absent);
    printf("; %d left\n", p->nb);

    rx_array_free(&tracks);
    return ret;
}

static int playlist_delete(Cli *cli, const char *name)
{
    RXLibrary *lib = &cli->app.lib;
    RXPlaylist *p = find_playlist(lib, name);
    if (!p)
        return EXIT_FAIL;
    int nb = p->nb;
    int err = rx_library_playlist_delete(lib, p);
    if (err < 0)
        return fail("cannot delete the playlist %s: %s", name, strerror(-err));
    printf("deleted playlist %s (its %d track%s stay in the library)\n", name, nb,
           nb == 1 ? "" : "s");
    return EXIT_OK;
}

static int playlist_import(Cli *cli, const char *file)
{
    RXLibrary *lib = &cli->app.lib;
    char *abs = rx_path_abs(file);
    if (!abs)
        return fail("%s: no such file", file);
    RXPlaylist *p = NULL;
    int ret = rx_library_playlist_import_m3u(lib, lib->root_folder, abs, &p);
    free(abs);
    if (ret < 0)
        return fail("cannot import %s: %s", file, strerror(-ret));
    if (p)
        printf("imported %s: %d track%s\n", p->name, p->nb, p->nb == 1 ? "" : "s");
    else
        printf("imported %s\n", file);
    return EXIT_OK;
}

static int playlist_export(Cli *cli, const char *name, const char *file)
{
    RXLibrary *lib = &cli->app.lib;
    RXPlaylist *p = find_playlist(lib, name);
    if (!p)
        return EXIT_FAIL;
    char *abs = abs_path(file);
    if (!abs)
        return fail("out of memory");
    int ret = rx_library_playlist_export_m3u(lib, p, abs);
    if (ret < 0) {
        int code = fail("cannot write %s: %s", abs, strerror(-ret));
        free(abs);
        return code;
    }
    printf("wrote %s: %d track%s\n", abs, p->nb, p->nb == 1 ? "" : "s");
    free(abs);
    return EXIT_OK;
}

/* ----------------------------------------------------------------- commands */

static int cmd_init(Cli *cli, int argc, char **argv)
{
    const char *dir = NULL;
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (r > 0)
            continue;
        if (is_option(cli, argv[i]))
            return usage_error(cli, "init: unknown option %s", argv[i]);
        if (dir)
            return usage_error(cli, "init: one directory only");
        dir = argv[i];
    }

    char *root = dir ? abs_path(dir) : library_root(cli);
    char *db = root ? rx_path_join(root, "library.db") : NULL;
    int code = EXIT_OK;
    if (!db) {
        code = fail("out of memory");
        goto end;
    }

    int ret = rx_mkdir_p(root);
    if (ret < 0) {
        code = fail("cannot create %s: %s", root, strerror(-ret));
        goto end;
    }
    bool existed = rx_file_exists(db);

    /* Opening creates the database. */
    RXLibrary lib = { 0 };
    ret = rx_library_open(&lib, root);
    if (ret < 0) {
        code = fail("cannot create the library in %s: %s", root, strerror(-ret));
        goto end;
    }
    int nb = lib.nb_tracks;
    rx_library_close(&lib);

    if (existed)
        printf("%s already holds a library (%d track%s)\n", root, nb, nb == 1 ? "" : "s");
    else
        printf("created the library in %s\nadd music with `rxbox%s%s add <folder>`\n", root,
               dir || cli->library ? " -c " : "", dir ? dir : cli->library ? cli->library : "");

end:
    free(root);
    free(db);
    return code;
}

typedef struct AddCtx {
    RXArray  ids;
    int      failed;
    int      added;
    uint64_t first_new_id;
    bool     verbose;
} AddCtx;

static void add_cb(void *opaque, const char *path, RXTrack *t, int err)
{
    AddCtx *ctx = opaque;
    if (err < 0) {
        fprintf(stderr, "rxbox: cannot add %s: %s\n", path, strerror(-err));
        ctx->failed++;
        return;
    }
    if (!t)
        return;
    if (t->id >= ctx->first_new_id) {
        ctx->added++;
        if (ctx->verbose)
            printf("added %s (id %llu)\n", path, (unsigned long long)t->id);
    }
    if (!rx_array_push(&ctx->ids, &t->id))
        ctx->failed++;
}

static int cmd_add(Cli *cli, int argc, char **argv)
{
    RXArray paths;
    rx_array_init(&paths, sizeof(char *));
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0) {
            rx_array_free(&paths);
            return EXIT_USAGE;
        }
        if (r > 0)
            continue;
        if (is_option(cli, argv[i])) {
            rx_array_free(&paths);
            return usage_error(cli, "add: unknown option %s", argv[i]);
        }
        rx_array_push(&paths, &argv[i]);
    }
    if (!paths.nb) {
        rx_array_free(&paths);
        return usage_error(cli, "add: which files or folders?");
    }

    int code = open_library(cli, true);
    if (code) {
        rx_array_free(&paths);
        return code;
    }
    RXLibrary *lib = &cli->app.lib;

    /* Ids only grow: anything above the largest one now is new. */
    AddCtx ctx = {
        .first_new_id = lib->nb_tracks ? lib->tracks[lib->nb_tracks - 1]->id + 1 : 1,
        .verbose      = cli->verbose,
    };
    rx_array_init(&ctx.ids, sizeof(uint64_t));
    int before = lib->nb_tracks;
    for (size_t i = 0; i < paths.nb; i++) {
        const char *arg = *(char **)rx_array_at(&paths, i);
        char *abs = rx_path_abs(arg);
        if (!abs) {
            fail("%s: no such file or directory", arg);
            ctx.failed++;
            continue;
        }
        /* A failure the callback reported is said and counted once. */
        int failed = ctx.failed;
        int ret = rx_library_add_path(lib, abs, add_cb, &ctx);
        if (ret < 0 && ctx.failed == failed) {
            fail("cannot add %s: %s", abs, strerror(-ret));
            ctx.failed++;
        }
        free(abs);
    }
    int added = lib->nb_tracks - before;
    printf("added %d track%s (%d already in the library)\n", added, added == 1 ? "" : "s",
           (int)ctx.ids.nb - ctx.added);

    /* Everything named is analysed as far as it lacks something. */
    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    for (size_t i = 0; i < ctx.ids.nb; i++) {
        RXTrack *t = rx_library_find_id(lib, *(uint64_t *)rx_array_at(&ctx.ids, i));
        if (t)
            push_unique(&tracks, t);
    }
    int failed = analyse_tracks(cli, tracks.data, (int)tracks.nb, RX_ANALYSE_ALL, false, false);

    rx_array_free(&tracks);
    rx_array_free(&ctx.ids);
    rx_array_free(&paths);
    cli_close(cli);
    return ctx.failed || failed ? EXIT_FAIL : EXIT_OK;
}

static int parse_what(const char *list, unsigned *what)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", list);
    *what = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, ", ", &save); w; w = strtok_r(NULL, ", ", &save)) {
        if (!strcmp(w, "beats") || !strcmp(w, "grid") || !strcmp(w, "bpm"))
            *what |= RX_ANALYSE_BEATS;
        else if (!strcmp(w, "key"))
            *what |= RX_ANALYSE_KEY;
        else if (!strcmp(w, "waves") || !strcmp(w, "waveforms"))
            *what |= RX_ANALYSE_WAVES;
        else if (!strcmp(w, "all"))
            *what |= RX_ANALYSE_ALL;
        else
            return RX_ERROR(EINVAL);
    }
    return *what ? 0 : RX_ERROR(EINVAL);
}

static int cmd_analyze(Cli *cli, int argc, char **argv)
{
    bool force = false;
    unsigned what = RX_ANALYSE_ALL;
    RXArray args;
    rx_array_init(&args, sizeof(char *));
    int code = EXIT_OK;

    for (int i = 1; i < argc; i++) {
        const char *val;
        int r = common_option(cli, argc, argv, &i);
        if (r < 0) {
            code = EXIT_USAGE;
            goto end;
        }
        if (r > 0)
            continue;
        if (opt_flag(cli, argv[i], "-f", "--force")) {
            force = true;
        } else if ((r = opt_value(cli, argc, argv, &i, "-w", "--what", &val))) {
            if (r < 0 || parse_what(val, &what) < 0) {
                code = usage_error(cli, "%s: --what takes beats, key and waves, comma separated",
                                   cli->cmd);
                goto end;
            }
        } else if (is_option(cli, argv[i])) {
            code = usage_error(cli, "%s: unknown option %s", cli->cmd, argv[i]);
            goto end;
        } else {
            rx_array_push(&args, &argv[i]);
        }
    }

    code = open_library(cli, false);
    if (code)
        goto end;
    RXLibrary *lib = &cli->app.lib;

    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    for (size_t i = 0; i < args.nb; i++) {
        if (resolve_tracks(lib, *(char **)rx_array_at(&args, i), &tracks) < 0)
            code = EXIT_FAIL;
    }
    if (!args.nb) {
        for (int i = 0; i < lib->nb_tracks; i++) {
            if (!lib->tracks[i]->missing)
                rx_array_push(&tracks, &lib->tracks[i]);
        }
    }

    printf("%d track%s\n", (int)tracks.nb, tracks.nb == 1 ? "" : "s");
    /* Library-wide, locked grids stay even when forced; named tracks get
     * them replaced. */
    int failed = analyse_tracks(cli, tracks.data, (int)tracks.nb, what, force, args.nb > 0);
    if (failed)
        code = EXIT_FAIL;
    rx_array_free(&tracks);

end:
    rx_array_free(&args);
    cli_close(cli);
    return code;
}

/* " beats 1", " beats 0 (older than 1)" for a result an older analyser
 * made, " waves 1 (older format)" for waveforms stored in a format this
 * build no longer reads. */
static void print_version(const char *stage, int version, int current, bool stale,
                          const char *sep)
{
    printf(" %s %d", stage, version);
    if (stale && version < current)
        printf(" (older than %d)", current);
    else if (stale)
        printf(" (older format)");
    printf("%s", sep);
}

static void show_track(const RXApp *app, const RXTrack *t)
{
    char a[32], b[32], bpm[32];

    printf("%s\n", t->path);
    printf("  id %llu%s, added %s", (unsigned long long)t->id, t->missing ? ", FILE MISSING" : "",
           fmt_date(t->added, a, sizeof(a)));
    if (t->analysed)
        printf(", analysed %s", fmt_date(t->analysed, a, sizeof(a)));
    if (t->play_count)
        printf(", played %d time%s, last %s", t->play_count, t->play_count == 1 ? "" : "s",
               fmt_date(t->last_played, a, sizeof(a)));
    printf("\n");
    printf("  %s - %s", t->meta.artist && t->meta.artist[0] ? t->meta.artist : "?", title_of(t));
    if (t->meta.album && t->meta.album[0])
        printf("  [%s]", t->meta.album);
    printf("  %s\n", rx_fmt_time(t->duration, a, sizeof(a)));
    printf("  %s, %d Hz, %d channel%s", t->codec[0] ? t->codec : "?", t->sample_rate,
           t->channels, t->channels == 1 ? "" : "s");
    if (t->bit_depth)
        printf(", %d bit", t->bit_depth);
    if (t->bit_rate)
        printf(", %d kbps", t->bit_rate / 1000);
    printf("\n");

    const RXGrid *g = &t->grid;
    if (rx_grid_valid(g)) {
        for (int i = 0; i < g->nb_sections; i++) {
            const RXTempoSection *s = &g->sections[i];
            int bar = 0, beat = 0;
            rx_grid_bar_of(g, s->start + 1e-6, &bar, &beat);
            printf("  grid: %s BPM from %s (bar %d.%d)\n", fmt_bpm(s->bpm, bpm, sizeof(bpm)),
                   rx_fmt_time(s->start, a, sizeof(a)), bar, beat);
        }
        printf("  grid confidence %.2f%s\n", t->grid_confidence, t->grid_locked ? ", locked" : "");
        if (t->grid_flags) {
            char flags[256];
            printf("  flags: %s\n", fmt_flags(t->grid_flags, flags, sizeof(flags)));
        }
    } else {
        printf("  grid: not analysed\n");
    }

    if (t->key[0])
        printf("  key: %s (Camelot %s, Open Key %s), confidence %.2f\n", t->key,
               rx_key_to_camelot(t->key, a, sizeof(a)), rx_key_to_open(t->key, b, sizeof(b)),
               t->key_confidence);
    else
        printf("  key: unknown\n");

    unsigned stale = rx_app_analysis_stale(app, t);
    printf("  analyser versions:");
    print_version("beats", t->beats_version, RX_BEATS_VERSION, stale & RX_ANALYSE_BEATS, ",");
    print_version("key", t->key_version, RX_KEY_VERSION, stale & RX_ANALYSE_KEY, ",");
    print_version("waves", t->waves_version, RX_WAVES_VERSION, stale & RX_ANALYSE_WAVES, "\n");

    if (!t->nb_cues)
        printf("  cues: none\n");
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        char kind[16], pos[48] = "", loop[64] = "";
        if (rx_cue_is_hot(c))
            snprintf(kind, sizeof(kind), "hot %c", rx_cue_letter(c));
        else
            snprintf(kind, sizeof(kind), "memory");

        if (rx_grid_valid(g)) {
            if (rx_cue_is_musical(c)) {
                char p[48];
                snprintf(pos, sizeof(pos), "%s",
                         rx_format_grid_position(g, c->grid_beat, p, sizeof(p)));
            } else {
                int bar = 0, beat = 0;
                rx_grid_bar_of(g, c->time + 1e-6, &bar, &beat);
                snprintf(pos, sizeof(pos), "in bar %d.%d", bar, beat);
            }
        }
        if (rx_cue_is_loop(c)) {
            if (!isnan(c->loop_beats))
                snprintf(loop, sizeof(loop), "  loop %s beats to %s",
                         fmt_bpm(c->loop_beats, bpm, sizeof(bpm)),
                         rx_fmt_time(c->loop_end, b, sizeof(b)));
            else
                snprintf(loop, sizeof(loop), "  loop to %s",
                         rx_fmt_time(c->loop_end, b, sizeof(b)));
        }
        printf("  cue %-8s %s", kind, rx_fmt_time(c->time, a, sizeof(a)));
        if (pos[0])
            printf("  %s", pos);
        printf("%s%s", loop, c->active ? "  active" : "");
        if (c->name[0])
            printf("  \"%s\"", c->name);
        if (c->color[0])
            printf("  %s", c->color);
        printf("\n");
    }
}

static int cmd_show(Cli *cli, int argc, char **argv)
{
    RXArray args;
    rx_array_init(&args, sizeof(char *));
    int code = EXIT_OK;
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0) {
            code = EXIT_USAGE;
            goto end;
        }
        if (r > 0)
            continue;
        if (is_option(cli, argv[i])) {
            code = usage_error(cli, "show: unknown option %s", argv[i]);
            goto end;
        }
        rx_array_push(&args, &argv[i]);
    }
    if (!args.nb) {
        code = usage_error(cli, "show: which tracks?");
        goto end;
    }

    code = open_library(cli, false);
    if (code)
        goto end;

    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    for (size_t i = 0; i < args.nb; i++) {
        if (resolve_tracks(&cli->app.lib, *(char **)rx_array_at(&args, i), &tracks) < 0)
            code = EXIT_FAIL;
    }
    for (size_t i = 0; i < tracks.nb; i++)
        show_track(&cli->app, *(RXTrack **)rx_array_at(&tracks, i));
    rx_array_free(&tracks);

end:
    rx_array_free(&args);
    cli_close(cli);
    return code;
}

typedef struct ProgressCtx {
    char stage[32];
    bool tty;
    bool verbose;
    bool all_items;             /* a dry run prints everything it would do */
    bool pruning;               /* the export deletes what it reports as "prune" */
    bool line_open;
    int  prune;                 /* stick files beyond the export */
    int  unsynced;              /* "check" items about unsynced player changes */
} ProgressCtx;

/* The check that blocks an export over player changes not synced back
 * (rxbox/export.h) is recognised by its wording. */
static bool unsynced_check(const char *item)
{
    return rx_str_contains_fold(item, "unsynced") || rx_str_contains_fold(item, "not synced") ||
           rx_str_contains_fold(item, "sync first");
}

static void progress_end_line(ProgressCtx *ctx)
{
    if (ctx->line_open)
        printf("\r\033[K");
    ctx->line_open = false;
}

static bool progress_cb(void *opaque, const char *stage, int done, int total, const char *item)
{
    ProgressCtx *ctx = opaque;
    if (!stage)
        stage = "";

    if (strcmp(stage, ctx->stage)) {
        progress_end_line(ctx);
        snprintf(ctx->stage, sizeof(ctx->stage), "%s", stage);
        if (total > 0)
            printf("%s (%d)\n", stage, total);
        else
            printf("%s\n", stage);
    }

    if (item) {
        bool path = item[0] == '/';
        if (!strcmp(stage, "check")) {
            /* The checks name their problems. */
            progress_end_line(ctx);
            printf("  %s\n", item);
            ctx->unsynced += unsynced_check(item);
        } else if (!strcmp(stage, "prune")) {
            /* Stick paths are the files beyond the export, anything else a
             * message about them. */
            ctx->prune += path;
            if (!path || ctx->all_items || ctx->pruning || ctx->verbose) {
                progress_end_line(ctx);
                printf("  %s%s\n", path && ctx->pruning && !ctx->all_items ? "deleted " : "", item);
            }
        } else if (ctx->all_items) {
            progress_end_line(ctx);
            printf("  %s\n", item);
        } else if (ctx->tty) {
            printf("\r\033[K  [%d/%d] %.100s", done, total, item);
            ctx->line_open = true;
        } else if (ctx->verbose) {
            printf("  [%d/%d] %s\n", done, total, item);
        }
    }
    fflush(stdout);
    return true;
}

static int cmd_export(Cli *cli, int argc, char **argv)
{
    const char *stick = NULL;
    RXExportOptions opt;
    rx_export_options_default(&opt);
    bool compat = false;
    RXArray names;
    rx_array_init(&names, sizeof(char *));
    RXArray playlists;
    rx_array_init(&playlists, sizeof(RXPlaylist *));
    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    RXPlaylist **all = NULL;
    int code = EXIT_OK;

    for (int i = 1; i < argc; i++) {
        const char *val;
        int r = common_option(cli, argc, argv, &i);
        if (r < 0) {
            code = EXIT_USAGE;
            goto end;
        }
        if (r > 0)
            continue;
        if (opt_flag(cli, argv[i], NULL, "--all")) {
            opt.whole_library = true;
        } else if (opt_flag(cli, argv[i], NULL, "--prune")) {
            opt.prune = true;
        } else if (opt_flag(cli, argv[i], NULL, "--compat")) {
            compat = true;
        } else if (opt_flag(cli, argv[i], NULL, "--ignore-unsynced")) {
            opt.ignore_unsynced = true;
        } else if (opt_flag(cli, argv[i], "-n", "--dry-run")) {
            opt.dry_run = true;
        } else if ((r = opt_value(cli, argc, argv, &i, "-p", "--playlist", &val))) {
            if (r < 0) {
                code = usage_error(cli, "export: --playlist needs a name");
                goto end;
            }
            rx_array_push(&names, &val);
        } else if (is_option(cli, argv[i])) {
            code = usage_error(cli, "export: unknown option %s", argv[i]);
            goto end;
        } else if (stick) {
            code = usage_error(cli, "export: one stick at a time");
            goto end;
        } else {
            stick = argv[i];
        }
    }
    if (!stick) {
        code = usage_error(cli, "export: where is the stick mounted?");
        goto end;
    }
    if (opt.whole_library && names.nb) {
        code = usage_error(cli, "export: --all or --playlist, not both");
        goto end;
    }
    if (!rx_dir_exists(stick)) {
        code = fail("%s is not a directory (mount the stick first)", stick);
        goto end;
    }

    code = open_library(cli, false);
    if (code)
        goto end;
    RXLibrary *lib = &cli->app.lib;

    for (size_t i = 0; i < names.nb; i++) {
        RXPlaylist *p = find_playlist(lib, *(char **)rx_array_at(&names, i));
        if (!p) {
            code = EXIT_FAIL;
            goto end;
        }
        rx_array_push(&playlists, &p);
    }

    /* What goes on the stick: every track, the chosen playlists' tracks,
     * or by default every playlist's. It is analysed first where it lacks
     * anything; rx_export skips (and names) what still has no grid. */
    int nb_pls = (int)playlists.nb;
    RXPlaylist **pls = playlists.data;
    if (opt.whole_library) {
        for (int i = 0; i < lib->nb_tracks; i++)
            rx_array_push(&tracks, &lib->tracks[i]);
    } else {
        if (!nb_pls) {
            nb_pls = rx_library_playlists_flat(lib, NULL, 0);
            all = calloc(RX_MAX(nb_pls, 1), sizeof(*all));
            if (!all) {
                code = fail("out of memory");
                goto end;
            }
            nb_pls = rx_library_playlists_flat(lib, all, nb_pls);
            pls = all;
        }
        for (int i = 0; i < nb_pls; i++) {
            for (int k = 0; k < pls[i]->nb; k++) {
                RXTrack *t = rx_library_find_id(lib, pls[i]->track_ids[k]);
                if (t)
                    push_unique(&tracks, t);
            }
        }
    }
    int missing = 0;
    for (size_t i = 0; i < tracks.nb; i++)
        missing += (*(RXTrack **)rx_array_at(&tracks, i))->missing;
    printf("%d track%s", (int)tracks.nb, tracks.nb == 1 ? "" : "s");
    if (!opt.whole_library)
        printf(" in %d playlist%s", nb_pls, nb_pls == 1 ? "" : "s");
    if (missing)
        printf(", %d missing (skipped)", missing);
    printf("; analysing what is missing\n");
    int failed = analyse_tracks(cli, tracks.data, (int)tracks.nb, RX_ANALYSE_ALL, false, false);
    if (failed)
        printf("%d track%s could not be analysed\n", failed, failed == 1 ? "" : "s");

    opt.playlists = playlists.nb ? (const RXPlaylist *const *)playlists.data : NULL;
    opt.nb_playlists = (int)playlists.nb;
    opt.compat_transcode = compat || lib->cfg.compat_transcode;

    ProgressCtx ctx = {
        .tty       = isatty(STDOUT_FILENO),
        .verbose   = cli->verbose,
        .all_items = opt.dry_run,
        .pruning   = opt.prune,
    };
    int ret = rx_export(lib, stick, &opt, progress_cb, &ctx);
    progress_end_line(&ctx);
    if (ret < 0) {
        code = fail("the export to %s failed: %s", stick, strerror(-ret));
        if (ctx.unsynced && !opt.ignore_unsynced)
            fprintf(stderr, "the stick has changes a player made: `rxbox sync %s` shows them, "
                    "`rxbox pull %s` takes them; --ignore-unsynced exports anyway\n", stick, stick);
        goto end;
    }
    /* A real run says so itself. */
    if (opt.dry_run && ctx.prune && !opt.prune)
        printf("%d file%s on the stick %s no longer exported; --prune deletes them\n", ctx.prune,
               ctx.prune == 1 ? "" : "s", ctx.prune == 1 ? "is" : "are");
    if (opt.dry_run)
        printf("dry run: nothing was written to %s\n", stick);
    else
        printf("done. Eject the stick properly; don't open it in rekordbox "
               "(it rewrites device libraries).\n");

end:
    free(all);
    rx_array_free(&tracks);
    rx_array_free(&playlists);
    rx_array_free(&names);
    cli_close(cli);
    return code;
}

/* Commands with one stick argument and no library options of their own. */
static int stick_arg(Cli *cli, int argc, char **argv, const char **stick)
{
    *stick = NULL;
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (r > 0)
            continue;
        if (is_option(cli, argv[i]))
            return usage_error(cli, "%s: unknown option %s", cli->cmd, argv[i]);
        if (*stick)
            return usage_error(cli, "%s: one stick at a time", cli->cmd);
        *stick = argv[i];
    }
    if (!*stick)
        return usage_error(cli, "%s: where is the stick mounted?", cli->cmd);
    if (!rx_dir_exists(*stick))
        return fail("%s is not a directory (mount the stick first)", *stick);
    return EXIT_OK;
}

static void print_line(void *opaque, const char *line)
{
    printf("%s\n", line);
}

static int cmd_inspect(Cli *cli, int argc, char **argv)
{
    const char *stick;
    int code = stick_arg(cli, argc, argv, &stick);
    if (code)
        return code;

    int problems = rx_inspect(stick, print_line, NULL);
    if (problems < 0)
        return fail("cannot inspect %s: %s", stick, strerror(-problems));
    if (problems) {
        printf("%d problem%s found\n", problems, problems == 1 ? "" : "s");
        return EXIT_FAIL;
    }
    printf("OK: no problems found\n");
    return EXIT_OK;
}

static int cmd_pull(Cli *cli, int argc, char **argv)
{
    const char *stick;
    int code = stick_arg(cli, argc, argv, &stick);
    if (code)
        return code;
    code = open_library(cli, false);
    if (code)
        return code;

    ProgressCtx ctx = {
        .tty     = isatty(STDOUT_FILENO),
        .verbose = cli->verbose,
    };
    int ret = rx_pull(&cli->app.lib, stick, progress_cb, &ctx);
    progress_end_line(&ctx);
    if (ret < 0) {
        code = fail("cannot pull cues from %s: %s", stick, strerror(-ret));
    } else {
        printf("updated %d track%s from %s\n", ret, ret == 1 ? "" : "s", stick);
        code = EXIT_OK;
    }
    cli_close(cli);
    return code;
}

static void print_sync_item(const RXLibrary *lib, const RXSyncItem *it)
{
    char name[256], a[32], b[32];
    const RXTrack *t = it->track_id ? rx_library_find_id(lib, it->track_id) : NULL;
    if (t)
        rx_track_display(t, name, sizeof(name));
    else
        snprintf(name, sizeof(name), "track %llu", (unsigned long long)it->track_id);

    const RXCue *c = &it->cue;
    char loop[48] = "";
    if (rx_cue_is_loop(c))
        snprintf(loop, sizeof(loop), " loop to %s", rx_fmt_time(c->loop_end, b, sizeof(b)));

    switch (it->kind) {
    case RX_SYNC_HOT_CUE_SET:
        printf("hot cue %c set at %s%s%s%s: %s\n", 'A' + it->slot,
               rx_fmt_time(c->time, a, sizeof(a)), loop, c->name[0] ? " " : "", c->name, name);
        break;
    case RX_SYNC_HOT_CUE_CLEARED:
        printf("hot cue %c cleared (was at %s): %s\n", 'A' + it->slot,
               rx_fmt_time(c->time, a, sizeof(a)), name);
        break;
    case RX_SYNC_MEMORY_ADDED:
        printf("memory %s added at %s%s: %s\n", rx_cue_is_loop(c) ? "loop" : "cue",
               rx_fmt_time(c->time, a, sizeof(a)), loop, name);
        break;
    case RX_SYNC_MEMORY_REMOVED:
        printf("memory %s removed at %s: %s\n", rx_cue_is_loop(c) ? "loop" : "cue",
               rx_fmt_time(c->time, a, sizeof(a)), name);
        break;
    case RX_SYNC_PLAY:
        if (it->played_at)
            printf("played %s: %s\n", fmt_date(it->played_at, a, sizeof(a)), name);
        else
            printf("played (history entry %d): %s\n", it->play_index + 1, name);
        break;
    case RX_SYNC_SETTING:
        printf("setting %s: %s -> %s\n", it->setting_key,
               it->setting_old[0] ? it->setting_old : "(unset)", it->setting_value);
        break;
    }
}

static int cmd_sync(Cli *cli, int argc, char **argv)
{
    bool apply = false;
    const char *stick = NULL;
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (r > 0)
            continue;
        if (opt_flag(cli, argv[i], NULL, "--apply"))
            apply = true;
        else if (is_option(cli, argv[i]))
            return usage_error(cli, "sync: unknown option %s", argv[i]);
        else if (stick)
            return usage_error(cli, "sync: one stick at a time");
        else
            stick = argv[i];
    }
    if (!stick)
        return usage_error(cli, "sync: where is the stick mounted?");
    if (!rx_dir_exists(stick))
        return fail("%s is not a directory (mount the stick first)", stick);

    int code = open_library(cli, false);
    if (code)
        return code;
    RXLibrary *lib = &cli->app.lib;

    RXStickState state = { 0 };
    RXSyncItem *items = NULL;
    int nb = 0;
    int ret = rx_stick_read(lib, stick, &state);
    if (ret < 0) {
        code = fail("cannot read %s: %s", stick, strerror(-ret));
        goto end;
    }
    ret = rx_stick_diff(lib, &state, &items, &nb);
    if (ret < 0) {
        code = fail("cannot compare %s with the library: %s", stick, strerror(-ret));
        goto end;
    }

    for (int i = 0; i < nb; i++)
        print_sync_item(lib, &items[i]);
    if (state.nb_tracks_unknown)
        printf("%d track%s on the stick %s not in the library\n", state.nb_tracks_unknown,
               state.nb_tracks_unknown == 1 ? "" : "s",
               state.nb_tracks_unknown == 1 ? "is" : "are");
    if (!nb) {
        printf("nothing to sync: the library has everything on %s\n", stick);
    } else if (!apply) {
        printf("%d change%s; `rxbox sync --apply %s` takes them\n", nb, nb == 1 ? "" : "s", stick);
    } else {
        for (int i = 0; i < nb; i++)
            items[i].accepted = true;
        ProgressCtx ctx = {
            .tty     = isatty(STDOUT_FILENO),
            .verbose = cli->verbose,
        };
        ret = rx_stick_apply(lib, items, nb, progress_cb, &ctx);
        progress_end_line(&ctx);
        if (ret < 0)
            code = fail("cannot apply the changes: %s", strerror(-ret));
        else
            printf("applied %d change%s\n", ret, ret == 1 ? "" : "s");
    }

end:
    free(items);
    rx_stick_state_free(&state);
    cli_close(cli);
    return code;
}

static int cmd_rescan(Cli *cli, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (!r)
            return usage_error(cli, "rescan: takes no arguments");
    }
    int code = open_library(cli, false);
    if (code)
        return code;
    RXLibrary *lib = &cli->app.lib;

    int ret = rx_library_rescan_missing(lib);
    if (ret < 0) {
        code = fail("cannot rescan: %s", strerror(-ret));
    } else {
        int missing = 0;
        for (int i = 0; i < lib->nb_tracks; i++) {
            if (lib->tracks[i]->missing) {
                printf("missing: %s\n", lib->tracks[i]->path);
                missing++;
            }
        }
        /* Opening the library already rescans, so this mostly reports. */
        printf("%d track%s, %d missing", lib->nb_tracks, lib->nb_tracks == 1 ? "" : "s", missing);
        if (ret)
            printf(" (%d changed just now)", ret);
        printf("\n");
    }
    cli_close(cli);
    return code;
}

static int cmd_check(Cli *cli, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (!r)
            return usage_error(cli, "check: takes no arguments");
    }
    int code = open_library(cli, false);
    if (code)
        return code;

    /* The problems themselves are logged by the check. */
    const RXLibrary *lib = &cli->app.lib;
    int ret = rx_library_check(lib);
    if (ret < 0)
        code = fail("the library database has problems: %s", strerror(-ret));
    else
        printf("OK: the library database is intact (%d track%s)\n", lib->nb_tracks,
               lib->nb_tracks == 1 ? "" : "s");

    /* What `rxbox analyze` would analyse again because it is stale (an
     * older analyser, or an older waveform format, made it): locked grids
     * stay, so they are counted apart, and a missing file cannot be
     * analysed. */
    int stale = 0, locked = 0;
    for (int i = 0; i < lib->nb_tracks; i++) {
        const RXTrack *t = lib->tracks[i];
        unsigned s = t->missing ? 0 : rx_app_analysis_stale(&cli->app, t);
        if (t->grid_locked && (s & RX_ANALYSE_BEATS)) {
            locked++;
            s &= ~RX_ANALYSE_BEATS;
        }
        stale += s != 0;
    }
    if (stale)
        printf("%d track%s results from an older analyser; `rxbox analyze` analyses %s "
               "again\n", stale, stale == 1 ? " has" : "s have", stale == 1 ? "it" : "them");
    else
        printf("no track has results from an older analyser\n");
    if (locked)
        printf("%d locked grid%s from an older analyser %s kept (`rxbox analyze --force TRACK` "
               "replaces one)\n", locked, locked == 1 ? "" : "s", locked == 1 ? "is" : "are");
    cli_close(cli);
    return code;
}

static void print_kv(const char *section, const RXKv *kv, int nb)
{
    for (int i = 0; i < nb; i++)
        printf("%s.%s = %s\n", section, kv[i].key, kv[i].value ? kv[i].value : "");
}

static int cmd_config(Cli *cli, int argc, char **argv)
{
    const char *key = NULL, *value = NULL;
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (r > 0)
            continue;
        if (is_option(cli, argv[i]))
            return usage_error(cli, "config: unknown option %s", argv[i]);
        if (!key)
            key = argv[i];
        else if (!value)
            value = argv[i];
        else
            return usage_error(cli, "config: KEY VALUE");
    }

    static const char *const sections[] = {
        "analysis.", "export.", "player.", "mixer.", "device.", "audio.", "ui.",
    };
    if (key) {
        bool known = false;
        for (size_t i = 0; i < RX_ARRAY_ELEMS(sections); i++)
            known |= rx_str_starts(key, sections[i]);
        if (!known)
            return usage_error(cli, "config: keys start with analysis., export., player., mixer., "
                               "device., audio. or ui.");
    }

    int code = open_library(cli, false);
    if (code)
        return code;
    RXLibrary *lib = &cli->app.lib;

    if (key && value) {
        int ret = rx_library_setting_set(lib, key, value);
        if (ret < 0)
            code = fail("cannot set %s: %s", key, strerror(-ret));
        else
            printf("%s = %s\n", key, value);
    } else if (key) {
        char *v = NULL;
        int ret = rx_library_setting_get(lib, key, &v);
        if (ret == RX_ERROR(ENOENT))
            printf("%s is not set\n", key);
        else if (ret < 0)
            code = fail("cannot read %s: %s", key, strerror(-ret));
        else
            printf("%s = %s\n", key, v ? v : "");
        free(v);
    } else {
        const RXConfig *c = &lib->cfg;
        char bpm[32];
        printf("analysis.bpm_min = %s\n",
               c->bpm_min > 0 ? fmt_bpm(c->bpm_min, bpm, sizeof(bpm)) : "(auto)");
        printf("analysis.bpm_max = %s\n",
               c->bpm_max > 0 ? fmt_bpm(c->bpm_max, bpm, sizeof(bpm)) : "(auto)");
        printf("analysis.snap_bpm = %s\n", c->snap_bpm ? "true" : "false");
        printf("analysis.auto_cues = %s\n", c->auto_cues ? "true" : "false");
        printf("analysis.allow_sections = %s\n", c->allow_sections ? "true" : "false");
        static const struct {
            unsigned    bit;
            const char *name;
        } what_names[] = {
            { RX_ANALYSE_BEATS, "beats" },
            { RX_ANALYSE_KEY,   "key"   },
            { RX_ANALYSE_WAVES, "waves" },
        };
        char what[32] = "";
        for (size_t i = 0; i < RX_ARRAY_ELEMS(what_names); i++) {
            size_t len = strlen(what);
            if (c->analyse_what & what_names[i].bit)
                snprintf(what + len, sizeof(what) - len, "%s%s", len ? "," : "", what_names[i].name);
        }
        printf("analysis.what = %s\n", what);
        printf("export.key_notation = %s\n", c->key_notation);
        printf("export.pdb = %s\n", c->export_pdb ? "true" : "false");
        printf("export.onelibrary = %s\n", c->export_onelibrary ? "true" : "false");
        printf("export.compat_transcode = %s\n", c->compat_transcode ? "true" : "false");
        printf("export.device_name = %s\n",
               c->device_name ? c->device_name : "(the stick's label)");
        print_kv("player", c->player, c->nb_player);
        print_kv("mixer", c->mixer, c->nb_mixer);
        print_kv("device", c->device, c->nb_device);
        printf("audio.sink = %s\n", c->audio_sink ? c->audio_sink : "(the default)");
        printf("ui.traffic_light = %s\n", c->traffic_light);
        printf("ui.detail_bars = %d\n", c->detail_bars);
    }
    cli_close(cli);
    return code;
}

static const char *settings_section(const char *file)
{
    if (!rx_strcasecmp(file, "MYSETTING.DAT") || !rx_strcasecmp(file, "MYSETTING2.DAT"))
        return "player";
    if (!rx_strcasecmp(file, "DJMMYSETTING.DAT"))
        return "mixer";
    if (!rx_strcasecmp(file, "DEVSETTING.DAT"))
        return "device";
    /* background_color is a device.* key too, though the databases carry
     * it rather than DEVSETTING.DAT (src/formats/settings.c, rx_settings_list). */
    if (!strcmp(file, "export.pdb, exportLibrary.db"))
        return "device";
    return file;
}

static void settings_cb(void *opaque, const char *file, const char *name,
                        const char *const *values, int nb_values, const char *deflt)
{
    const char **section = opaque;
    const char *s = settings_section(file);
    if (!*section || strcmp(*section, s)) {
        printf("\n[%s]\n", s);
        *section = s;
    }
    printf("  %-24s ", name);
    for (int i = 0; i < nb_values; i++)
        printf("%s%s", i ? " | " : "", values[i]);
    if (deflt && deflt[0])
        printf("  (default %s)", deflt);
    printf("\n");
}

static int cmd_settings(Cli *cli, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (!r)
            return usage_error(cli, "settings: takes no arguments");
    }
    printf("# set with `rxbox config player.NAME VALUE` (mixer., device. likewise);\n"
           "# players apply them when you LOAD My Settings\n");
    const char *section = NULL;
    rx_settings_list(settings_cb, &section);
    return EXIT_OK;
}

static int cmd_playlist(Cli *cli, int argc, char **argv)
{
    RXArray args;
    rx_array_init(&args, sizeof(char *));
    int code = EXIT_OK;
    for (int i = 1; i < argc; i++) {
        int r = common_option(cli, argc, argv, &i);
        if (r < 0) {
            code = EXIT_USAGE;
            goto end;
        }
        if (r > 0)
            continue;
        if (is_option(cli, argv[i])) {
            code = usage_error(cli, "playlist: unknown option %s", argv[i]);
            goto end;
        }
        rx_array_push(&args, &argv[i]);
    }
    char **a = args.data;
    int nb = (int)args.nb;
    static const struct {
        const char *name;
        int min, max;           /* arguments after the subcommand */
        const char *usage;
    } subs[] = {
        { "list",   0, 0,       "playlist list" },
        { "create", 1, 1,       "playlist create NAME" },
        { "add",    2, INT_MAX, "playlist add NAME TRACK..." },
        { "remove", 2, INT_MAX, "playlist remove NAME TRACK..." },
        { "delete", 1, 1,       "playlist delete NAME" },
        { "import", 1, 1,       "playlist import FILE.m3u8" },
        { "export", 2, 2,       "playlist export NAME FILE.m3u8" },
    };
    if (!nb) {
        code = usage_error(cli, "playlist: list, create, add, remove, delete, import or export");
        goto end;
    }
    size_t k = 0;
    while (k < RX_ARRAY_ELEMS(subs) && strcmp(subs[k].name, a[0]))
        k++;
    if (k == RX_ARRAY_ELEMS(subs)) {
        code = usage_error(cli, "playlist: unknown command %s", a[0]);
        goto end;
    }
    cli->usage = subs[k].usage;
    if (nb - 1 < subs[k].min || nb - 1 > subs[k].max) {
        code = usage_error(cli, "playlist %s: %s arguments", a[0],
                           nb - 1 < subs[k].min ? "missing" : "too many");
        goto end;
    }

    code = open_library(cli, false);
    if (code)
        goto end;
    if (!strcmp(a[0], "list"))
        code = playlist_list(cli);
    else if (!strcmp(a[0], "create"))
        code = playlist_create(cli, a[1]);
    else if (!strcmp(a[0], "add"))
        code = playlist_add(cli, a[1], a + 2, nb - 2);
    else if (!strcmp(a[0], "remove"))
        code = playlist_remove(cli, a[1], a + 2, nb - 2);
    else if (!strcmp(a[0], "delete"))
        code = playlist_delete(cli, a[1]);
    else if (!strcmp(a[0], "import"))
        code = playlist_import(cli, a[1]);
    else
        code = playlist_export(cli, a[1], a[2]);

end:
    rx_array_free(&args);
    cli_close(cli);
    return code;
}

/* The interface: the only path that opens a window or an audio device. */
static int run_interface(Cli *cli)
{
    RXPlayerOptions po;
    rx_player_options_default(&po);
    po.backend = cli->audio;
    po.sink = cli->sink;
    po.app_name = "rxbox";

    RXWindowOptions wo;
    rx_window_options_default(&wo);
    wo.title = "rxbox";
    wo.validation = cli->validation;
    wo.device = cli->device;
    wo.cpu_analysis = cli->no_gpu;
    wo.max_frames = cli->frames;

    char *root = cli->library ? abs_path(cli->library) : NULL;
    RXApp *app = calloc(1, sizeof(*app));
    if (!app || (cli->library && !root)) {
        free(app);
        free(root);
        return fail("out of memory");
    }

    int ret = rx_app_create(app, root, &po, &wo);
    if (ret < 0) {
        free(app);
        free(root);
        return fail("cannot start the interface: %s", strerror(-ret));
    }
    ret = rx_app_run(app);
    rx_app_destroy(app);
    free(app);
    free(root);
    if (ret < 0)
        return fail("the interface stopped: %s", strerror(-ret));
    return EXIT_OK;
}

static const struct {
    const char *name;
    int (*fn)(Cli *cli, int argc, char **argv);
    const char *usage;
} commands[] = {
    { "add",      cmd_add,      "add PATH..." },
    { "analyze",  cmd_analyze,  "analyze [-f] [-w beats,key,waves] [TRACK...]" },
    { "analyse",  cmd_analyze,  "analyse [-f] [-w beats,key,waves] [TRACK...]" },
    { "show",     cmd_show,     "show TRACK..." },
    { "export",   cmd_export,   "export [--all | -p NAME...] [--prune] [--compat] "
                                "[--ignore-unsynced] [-n] STICK" },
    { "inspect",  cmd_inspect,  "inspect STICK" },
    { "pull",     cmd_pull,     "pull STICK" },
    { "sync",     cmd_sync,     "sync [--apply] STICK" },
    { "rescan",   cmd_rescan,   "rescan" },
    { "check",    cmd_check,    "check" },
    { "config",   cmd_config,   "config [KEY [VALUE]]" },
    { "settings", cmd_settings, "settings" },
    { "init",     cmd_init,     "init [DIR]" },
    { "playlist", cmd_playlist, "playlist list|create|add|remove|delete|import|export ..." },
};

static int cli_run(int argc, char **argv)
{
    Cli cli = {
        .audio = RX_AUDIO_PIPEWIRE,
    };

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        const char *val;
        if (a[0] != '-' || !a[1])
            break;
        if (!strcmp(a, "--")) {
            i++;
            break;
        }

        int r = common_option(&cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (r > 0)
            continue;

        if (opt_flag(&cli, a, "-h", "--help")) {
            fputs(usage_text, stdout);
            return EXIT_OK;
        }
        if (opt_flag(&cli, a, "-V", "--version")) {
            printf("rxbox %s\n", RXBOX_VERSION);
            return EXIT_OK;
        }
        if (opt_flag(&cli, a, NULL, "--validation")) {
            cli.validation = true;
            continue;
        }
        if ((r = opt_value(&cli, argc, argv, &i, NULL, "--audio", &val))) {
            if (r > 0 && !strcmp(val, "none"))
                cli.audio = RX_AUDIO_NULL;
            else if (r > 0 && !strcmp(val, "pipewire"))
                cli.audio = RX_AUDIO_PIPEWIRE;
            else
                return usage_error(NULL, "--audio is pipewire or none");
            continue;
        }
        if ((r = opt_value(&cli, argc, argv, &i, NULL, "--audio-sink", &val))) {
            if (r < 0)
                return usage_error(NULL, "--audio-sink needs a PipeWire node name");
            cli.sink = val;
            continue;
        }
        if ((r = opt_value(&cli, argc, argv, &i, NULL, "--frames", &val))) {
            long n;
            if (r < 0 || !parse_count(val, INT_MAX, &n))
                return usage_error(NULL, "--frames needs a number");
            cli.frames = (int)n;
            continue;
        }
        return usage_error(NULL, "unknown option %s", a);
    }

    if (i >= argc)
        return run_interface(&cli);

    cli.cmd = argv[i];
    for (size_t k = 0; k < RX_ARRAY_ELEMS(commands); k++) {
        if (!strcmp(commands[k].name, argv[i])) {
            cli.usage = commands[k].usage;
            return commands[k].fn(&cli, argc - i, argv + i);
        }
    }
    if (!strcmp(argv[i], "help")) {
        fputs(usage_text, stdout);
        return EXIT_OK;
    }
    return usage_error(NULL, "unknown command %s", argv[i]);
}

int rx_cli_main(int argc, char **argv)
{
    int code = cli_run(argc, argv);

    /* Output that could not all be written (a full disk, a closed pipe
     * that does not kill us) is a failure, not a silent success. */
    int err = fflush(stdout) == EOF ? errno : ferror(stdout) ? EIO : 0;
    if (err) {
        fprintf(stderr, "rxbox: cannot write the output: %s\n", strerror(err));
        if (code == EXIT_OK)
            code = EXIT_FAIL;
    }
    return code;
}
