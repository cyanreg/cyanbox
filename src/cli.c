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
 * needs a display. `tag` runs the model (rxbox/ml.h) through the same
 * batch the interface uses (rx_app_tag_tracks), on --device's GPU too,
 * and applies each result as it comes, one line per track.
 *
 * Arguments: options and names mix in any order after the command; `--`
 * ends the options, so every argument after it is a name, even one that
 * starts with '-'. An option's value is never empty. A usage error names
 * the problem, prints the command's usage line, and exits 1. */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
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
#include "rxbox/link.h"
#include "rxbox/ml.h"
#include "rxbox/mpris.h"
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
    bool  no_mpris;             /* --no-mpris: the interface stays off the session bus */
    bool  pro_link;             /* --pro-link: the interface offers PRO DJ LINK */
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
    "  show TRACK...            print the colour, the resolved grid, key, cues,\n"
    "                           analyser versions and the model's tags\n"
    "  tag TRACK...             tag the tracks with the model on the GPU (models/, or\n"
    "                           the setting ml.model): one of the eight colours, the one\n"
    "                           whose words (the settings color.pink ... color.purple)\n"
    "                           fit best, a comment, and a genre where none was typed\n"
    "  color COLOUR TRACK...    set the tracks' colour: one of pink red orange yellow\n"
    "                           green aqua blue purple, none, or a hue in degrees\n"
    "                           (0-360: 0 red, 120 green, 240 blue; the stick gets the\n"
    "                           nearest of the eight)\n"
    "  export STICK             write the settings, every playlist and their tracks to a\n"
    "                           mounted stick (files already there are not copied again)\n"
    "      --all                every track in the library, not only those in playlists\n"
    "      -p, --playlist NAME  only this playlist and its tracks (repeatable)\n"
    "      --prune              delete files on the stick that are no longer exported\n"
    "      --compat, --no-compat\n"
    "                           transcode AAC and hi-res files to 16-bit/44.1 kHz FLAC\n"
    "                           (on by default: export.compat_transcode)\n"
    "      --format F           the audio: copy (as it is), flac, wav or aiff (wav and\n"
    "                           aiff for players before the CDJ-2000NXS2: no FLAC there);\n"
    "                           default export.format\n"
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
    "                           import.comment, export.key_notation,\n"
    "                           export.background_color, player.quantize,\n"
    "                           ml.model, color.red ...; \"\" puts a colour's words back\n"
    "                           to the default)\n"
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
    "  link serve [-i IF]       serve the library to players (PRO DJ LINK, as\n"
    "                           rekordbox's Link Export) on every Ethernet interface,\n"
    "                           as they come and go, until interrupted, listing the\n"
    "                           players heard; the cues they save on the deck and\n"
    "                           the ratings they give go into the library\n"
    "      -i, --interface IF   only on this interface\n"
    "      --any-peer           answer any address, not only players heard (diagnostics)\n"
    "      --wait               join only once a player is heard, as rekordbox does\n"
    "  link browse -i IF [HOST] browse a library served on the network as a player does\n"
    "                           (a fake CDJ-3000): menus, one track's metadata,\n"
    "                           analysis and file; HOST, else the computer source heard\n"
    "      -n, --number N       the player number to use (default 3)\n"
    "      -t, --track ID       the track to load (default the first)\n"
    "      --name NAME          the model it says it is (default CDJ-3000)\n"
    "      --quiet              do not announce on the link (needs HOST)\n"
    "      --hold SEC           wait for a source that long (20 s at least); once\n"
    "                           loaded, stay on the link SEC seconds, playing: the\n"
    "                           beat number and tempo from the track's grid, to its\n"
    "                           end; a track the source tells it to load (Load in\n"
    "                           deck) is taken as a player does: from its start,\n"
    "                           playing on if it was playing, paused if not\n"
    "      --save-cue X@SEC     once loaded, save a cue on the track as a player does on\n"
    "                           the deck: hot cue X (A..H) or a memory cue (M) at SEC\n"
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
    "  --no-mpris               stay off the D-Bus session bus (no MPRIS media controls)\n"
    "  --pro-link               serve the library to players (PRO DJ LINK) on every\n"
    "                           Ethernet interface while the interface runs; without\n"
    "                           it no Link socket is ever opened. The deck is then\n"
    "                           heard only in a DJM's headphone cue (LINK CUE): no\n"
    "                           audio device is opened, --audio does not apply\n"
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
    /* My Settings that are not valid go, as the rest of what is not
     * (rx_library_open's cleaning). */
    rx_settings_clean(&cli->app.lib);
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
        bool dynamic = rx_grid_dynamic(&t->grid);
        printf(" %s BPM (confidence %.2f%s)",
               fmt_bpm(dynamic ? rx_grid_bpm_median(&t->grid, t->duration) : rx_grid_bpm(&t->grid),
                       bpm, sizeof(bpm)),
               t->grid_confidence, t->grid_locked ? ", locked" : "");
        if (dynamic)
            printf(", dynamic");
        else if (t->grid.nb_sections > 1)
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

    /* The name and an existing playlist are checked first, so a refused
     * name leaves no folder behind. */
    const char *last = strrchr(name, '/');
    const char *want = last ? last + 1 : name;
    if (!*want) {
        free(copy);
        return usage_error(cli, "%s: the playlist needs a name", name);
    }
    RXPlaylistFolder *in = lib->root_folder;
    for (const char *c = name; in && c < want;) {
        const char *slash = strchr(c, '/');
        size_t n = (size_t)(slash - c);
        RXPlaylistFolder *sub = NULL;
        for (int i = 0; n && i < in->nb_folders; i++) {
            if (strlen(in->folders[i]->name) == n && !strncmp(in->folders[i]->name, c, n))
                sub = in->folders[i];
        }
        in = n ? sub : in;
        c = slash + 1;
    }
    for (int i = 0; in && i < in->nb_playlists; i++) {
        if (!strcmp(in->playlists[i]->name, want)) {
            free(copy);
            return fail("the playlist %s already exists", name);
        }
    }

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

/* "red (intense, peak time)" for one of the eight colours, in the legend's
 * words; "none" (the library keeps no other: src/core/clean.c). */
static const char *fmt_colour(const RXConfig *cfg, double hue, char *buf, size_t size)
{
    int id = rx_track_color_of_hue(hue);
    if (!id)
        return "none";
    snprintf(buf, size, "%s (%s)", rx_track_color_name(id), rx_config_vibe(cfg, id));
    return buf;
}

/* The colour after the model's pick in its probabilities. */
static int runner_up(const float *p, int best)
{
    int second = best == 1 ? 2 : 1;
    for (int i = 1; i <= RX_TRACK_COLORS; i++) {
        if (i != best && p[i - 1] > p[second - 1])
            second = i;
    }
    return second;
}

/* "red 0.62 (intense, peak time), then orange 0.21": the model's pick, in
 * the legend's words when it was asked with the legend there is now. */
static const char *fmt_pick(const RXConfig *cfg, const float *p, uint32_t legend, char *buf,
                            size_t size)
{
    int best = 1;
    for (int i = 2; i <= RX_TRACK_COLORS; i++) {
        if (p[i - 1] > p[best - 1])
            best = i;
    }
    int second = runner_up(p, best);
    const char *now[RX_TRACK_COLORS];
    rx_config_legend(cfg, now);
    if (legend == rx_ml_legend_hash(now))
        snprintf(buf, size, "%s %.2f (%s), then %s %.2f", rx_track_color_name(best), p[best - 1],
                 now[best - 1], rx_track_color_name(second), p[second - 1]);
    else
        snprintf(buf, size, "%s %.2f, then %s %.2f (asked with other words)",
                 rx_track_color_name(best), p[best - 1], rx_track_color_name(second),
                 p[second - 1]);
    return buf;
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
    char colour[192];
    printf("  colour: %s\n", fmt_colour(&app->lib.cfg, t->meta.hue, colour, sizeof(colour)));
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
            if (!s->measured) {
                printf("  grid: %s BPM from %s (bar %d.%d)\n", fmt_bpm(s->bpm, bpm, sizeof(bpm)),
                       rx_fmt_time(s->start, a, sizeof(a)), bar, beat);
                continue;
            }
            /* A dynamic passage: its beats, where they run, their tempos. */
            int j = i;
            double lo = s->bpm, hi = s->bpm;
            while (j + 1 < g->nb_sections && g->sections[j + 1].measured) {
                j++;
                lo = fmin(lo, g->sections[j].bpm);
                hi = fmax(hi, g->sections[j].bpm);
            }
            printf("  grid: dynamic, %d beats from %s (bar %d.%d) to %s, %s", j - i + 1,
                   rx_fmt_time(s->start, a, sizeof(a)), bar, beat,
                   rx_fmt_time(g->sections[j].start, b, sizeof(b)), fmt_bpm(lo, bpm, sizeof(bpm)));
            printf("-%s BPM\n", fmt_bpm(hi, bpm, sizeof(bpm)));
            i = j;
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

    /* The model's words go where the user edits them; its measures are
     * its own. */
    if (t->meta.genre && t->meta.genre[0])
        printf("  genre: %s\n", t->meta.genre);
    if (t->meta.comment && t->meta.comment[0])
        printf("  comment: %s\n", t->meta.comment);
    if (t->tags_version > 0) {
        printf("  tags:");
        print_version("version", t->tags_version, RX_ML_TAGS_VERSION,
                      t->tags_version < RX_ML_TAGS_VERSION, ":");
        if (!isnan(t->ml_colors[0])) {
            char pick[320];
            printf(" %s;", fmt_pick(&app->lib.cfg, t->ml_colors, t->ml_legend, pick,
                                    sizeof(pick)));
        }
        printf(" intensity %.2f, club %.2f, mood %.2f, %s\n", t->ml_intensity, t->ml_club,
               t->ml_valence, t->ml_vocals >= 0.5f ? "vocals" : "instrumental");
        if (!isnan(t->ml_colors[0])) {
            printf("  colours:");
            for (int i = 0; i < RX_TRACK_COLORS; i++)
                printf("%s %s %.2f", i ? "," : "", rx_track_color_names[i], t->ml_colors[i]);
            printf("\n");
        }
    } else {
        printf("  tags: none (rxbox tag)\n");
    }

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

/* A track's colour is a hue (rxbox/model.h); each of the eight names stands
 * for its colour's hue, so `rxbox color aqua` puts aqua on the stick, as
 * the model's pick and the interface's swatches do. */
static int cmd_color(Cli *cli, int argc, char **argv)
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
            code = usage_error(cli, "color: unknown option %s", argv[i]);
            goto end;
        }
        rx_array_push(&args, &argv[i]);
    }
    if (args.nb < 2) {
        code = usage_error(cli, "color: a hue and the tracks");
        goto end;
    }
    const char *arg = *(char **)rx_array_at(&args, 0);
    double hue;
    if (rx_parse_hue(arg, &hue) < 0) {
        code = usage_error(cli, "color: %s is none of the eight colours (pink red orange yellow "
                           "green aqua blue purple, or their hues in degrees) nor none", arg);
        goto end;
    }

    code = open_library(cli, false);
    if (code)
        goto end;
    RXLibrary *lib = &cli->app.lib;

    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    for (size_t i = 1; i < args.nb; i++) {
        if (resolve_tracks(lib, *(char **)rx_array_at(&args, i), &tracks) < 0)
            code = EXIT_FAIL;
    }
    char buf[192];
    for (size_t i = 0; i < tracks.nb; i++) {
        RXTrack *t = *(RXTrack **)rx_array_at(&tracks, i);
        double old = t->meta.hue;
        t->meta.hue = hue;
        int ret = rx_library_save_track(lib, t);
        if (ret < 0) {
            t->meta.hue = old;
            code = fail("cannot set the colour of %s: %s", t->path, strerror(-ret));
            continue;
        }
        printf("%s: %s\n", t->path, fmt_colour(&lib->cfg, t->meta.hue, buf, sizeof(buf)));
    }
    rx_array_free(&tracks);

end:
    rx_array_free(&args);
    cli_close(cli);
    return code;
}

/* rxbox-ml -t's line, after analyze's count: the colour the model picked
 * and its runner-up, the measures, the genre the model hears (and the
 * user's, when that stays) and the comment. */
static void print_tagged(const RXConfig *cfg, int n, int total, const RXTrack *t,
                         const RXJobResult *r, int err)
{
    if (err < 0) {
        printf("[%d/%d] FAILED %s: %s\n", n, total, t->path, strerror(-err));
        return;
    }
    const RXMLTags *g = r->tags;
    char name[256];
    rx_track_display(t, name, sizeof(name));
    char pick[320];
    printf("[%d/%d] %s: %s | intensity %.2f club %.2f mood %.2f | %s", n, total, name,
           fmt_pick(cfg, g->colors, g->legend, pick, sizeof(pick)), g->intensity, g->club,
           g->valence, g->genre ? g->genre : "-");
    if (g->genre && t->meta.genre && strcmp(g->genre, t->meta.genre))
        printf(" (the genre stays %s)", t->meta.genre);
    printf(" | %s (%.1f s)\n", g->comment ? g->comment : "", r->seconds);
}

/* The interface's tagging: the tracks join one batch, which loads the
 * model once and unloads it after the last. */
static int cmd_tag(Cli *cli, int argc, char **argv)
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
            code = usage_error(cli, "tag: unknown option %s", argv[i]);
            goto end;
        }
        rx_array_push(&args, &argv[i]);
    }
    if (!args.nb) {
        code = usage_error(cli, "tag: which tracks?");
        goto end;
    }

    code = open_library(cli, false);
    if (code)
        goto end;
    RXApp *app = &cli->app;

    RXArray ids;
    rx_array_init(&ids, sizeof(uint64_t));
    RXArray tracks;
    rx_array_init(&tracks, sizeof(RXTrack *));
    for (size_t i = 0; i < args.nb; i++) {
        if (resolve_tracks(&app->lib, *(char **)rx_array_at(&args, i), &tracks) < 0)
            code = EXIT_FAIL;
    }
    for (size_t i = 0; i < tracks.nb; i++) {
        RXTrack *t = *(RXTrack **)rx_array_at(&tracks, i);
        if (t->missing) {
            fail("%s: the file is missing", t->path);
            code = EXIT_FAIL;
        } else if (!rx_array_push(&ids, &t->id)) {
            code = fail("out of memory");
        }
    }
    rx_array_free(&tracks);
    if (!ids.nb)
        goto out;

    /* One worker is all the batch takes. */
    int ret = rx_jobs_create(&app->jobs, 1);
    if (ret < 0) {
        code = fail("cannot start the tagging thread: %s", strerror(-ret));
        goto out;
    }
    rx_jobs_set_gpu(app->jobs, true, cli->device);
    int queued = rx_app_tag_tracks(app, ids.data, (int)ids.nb);
    if (queued < 0) {
        /* The status bar's message, in the CLI's lower case. */
        code = fail("%c%s", tolower((unsigned char)app->status[0]), app->status + 1);
        goto out;
    }
    printf("tagging %d track%s\n", queued, queued == 1 ? "" : "s");
    fflush(stdout);

    int done = 0, failed = 0;
    while (done < queued) {
        RXJobResult *r = rx_jobs_wait(app->jobs, 1000);
        if (!r) {
            /* The batch posts every result before it ends. */
            if (!rx_jobs_tag_progress(app->jobs, NULL, NULL, NULL) &&
                !(r = rx_jobs_poll(app->jobs)))
                break;
            if (!r)
                continue;
        }
        if (r->kind == RX_JOB_TAG && !r->track_id) {
            fail("cannot load the model: %s", strerror(-r->err));
            rx_job_result_free(r);
            break;
        }
        RXTrack *t = r->kind == RX_JOB_TAG ? rx_library_find_id(&app->lib, r->track_id) : NULL;
        if (t) {
            int err = r->err < 0 ? r->err : r->tags ? 0 : RX_ERROR(EINVAL);
            if (!err)
                err = rx_app_apply_tags(app, t, r->tags);
            print_tagged(&app->lib.cfg, ++done, queued, t, r, err);
            fflush(stdout);
            failed += err < 0;
        }
        rx_job_result_free(r);
    }
    failed += queued - done;
    if (failed)
        code = EXIT_FAIL;

out:
    rx_array_free(&ids);
end:
    rx_array_free(&args);
    cli_close(cli);
    return code;
}

typedef struct ProgressCtx {
    char stage[32];
    char item[256];             /* the last item of the stage, for the ticks without one */
    bool tty;
    bool verbose;
    bool all_items;             /* a dry run prints everything it would do */
    bool pruning;               /* the export deletes what it reports as "prune" */
    bool line_open;
    int  prune;                 /* stick files beyond the export */
    int  unsynced;              /* "check" items about unsynced player changes */
    RXExportStats stats;        /* the copy's rate, shown on the line */
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
        ctx->item[0] = '\0';
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
        } else if (!strcmp(stage, "flush")) {
            /* A failed or cancelled export's sync, or the sync failing. */
            progress_end_line(ctx);
            printf("  %s\n", item);
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
        } else if (ctx->verbose && !ctx->tty) {
            printf("  [%d/%d] %s\n", done, total, item);
        }
        if (total > 0)
            snprintf(ctx->item, sizeof(ctx->item), "%s", item);
    }
    /* On a terminal the stage's line: the step, the copy's rate and time
     * left, the item (a tick without one redraws it with the new rate). */
    if (ctx->tty && !ctx->all_items && total > 0 && strcmp(stage, "check") && strcmp(stage, "prune")) {
        char rate[64];
        rx_export_rate_text(!strcmp(stage, "copy") ? ctx->stats.rate : 0, ctx->stats.seconds_left, rate,
                            sizeof(rate));
        printf("\r\033[K  [%d/%d]%s%s %.100s", done, total, rate[0] ? " " : "", rate, ctx->item);
        ctx->line_open = true;
    }
    fflush(stdout);
    return true;
}

static int cmd_export(Cli *cli, int argc, char **argv)
{
    const char *stick = NULL;
    RXExportOptions opt;
    rx_export_options_default(&opt);
    bool compat = false, no_compat = false;
    int format = -1;
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
        } else if (opt_flag(cli, argv[i], NULL, "--no-compat")) {
            no_compat = true;
        } else if ((r = opt_value(cli, argc, argv, &i, NULL, "--format", &val))) {
            if (r < 0 || (format = rx_stick_format_parse(val)) < 0) {
                code = usage_error(cli, "export: --format takes copy, flac, wav or aiff");
                goto end;
            }
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
    if (opt.dry_run) {
        /* A dry run writes nothing, the library included: what the export
         * would analyse first is only counted, as analyse_tracks counts
         * it, and the dry run describes the tracks as they are. */
        int todo = 0, stale = 0;
        for (size_t i = 0; i < tracks.nb; i++) {
            RXTrack *t = *(RXTrack **)rx_array_at(&tracks, i);
            unsigned need = rx_app_analysis_needed(&cli->app, t, RX_ANALYSE_ALL, false);
            if (t->grid_locked)
                need &= ~RX_ANALYSE_BEATS;
            todo += need != 0;
            stale += (need & rx_app_analysis_stale(&cli->app, t)) != 0;
        }
        printf("\n");
        if (todo) {
            printf("%d track%s would be analysed first", todo, todo == 1 ? "" : "s");
            if (stale)
                printf(" (%d made by an older analyser)", stale);
            printf("; a dry run analyses nothing\n");
        } else {
            printf("nothing to analyse\n");
        }
    } else {
        printf("; analysing what is missing\n");
        int failed = analyse_tracks(cli, tracks.data, (int)tracks.nb, RX_ANALYSE_ALL, false, false);
        if (failed)
            printf("%d track%s could not be analysed\n", failed, failed == 1 ? "" : "s");
    }

    opt.playlists = playlists.nb ? (const RXPlaylist *const *)playlists.data : NULL;
    opt.nb_playlists = (int)playlists.nb;
    opt.compat_transcode = (compat || lib->cfg.compat_transcode) && !no_compat;
    opt.format = format >= 0 ? format : lib->cfg.export_format;

    ProgressCtx ctx = {
        .tty       = isatty(STDOUT_FILENO),
        .verbose   = cli->verbose,
        .all_items = opt.dry_run,
        .pruning   = opt.prune,
    };
    opt.stats = &ctx.stats;
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
    if (ret == RX_ERROR(ENOENT)) {
        code = fail("%s holds no export.pdb, exportLibrary.db or My Settings: not a stick, or not "
                    "mounted there", stick);
    } else if (ret < 0) {
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
    if (ret == RX_ERROR(ENOENT)) {
        code = fail("%s holds no export.pdb, exportLibrary.db or My Settings: not a stick, or not "
                    "mounted there", stick);
        goto end;
    } else if (ret < 0) {
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

static const char *settings_section(const char *file);

/* A My Setting (player.NAME, mixer.NAME, device.NAME) as `rxbox settings`
 * lists it: the file it is in and its values, for the check and the
 * error. */
typedef struct SettingFind {
    char        section[16];
    const char *name, *file;
    char        values[512];
} SettingFind;

static void find_setting(void *opaque, const char *file, const char *name,
                         const char *const *values, int nb_values, const char *deflt)
{
    SettingFind *f = opaque;
    if (f->file || strcmp(settings_section(file), f->section) || strcmp(name, f->name))
        return;
    f->file = file;
    size_t n = 0;
    for (int i = 0; i < nb_values && n < sizeof(f->values); i++)
        n += (size_t)snprintf(f->values + n, sizeof(f->values) - n, "%s%s",
                              !i ? "" : i + 1 < nb_values ? ", " : " or ", values[i]);
}

/* `rxbox config KEY VALUE` stores only what the key takes: a value the
 * library would warn of at every command, or the export refuse, is a
 * usage error here. Keys under ui. that are none of RXConfig's are the
 * interface's own (its layout) and taken as they are. */
static int check_setting(const Cli *cli, const char *key, const char *value)
{
    if (rx_str_starts(key, "player.") || rx_str_starts(key, "mixer.") || rx_str_starts(key, "device.")) {
        SettingFind f = { .name = strchr(key, '.') + 1 };
        snprintf(f.section, sizeof(f.section), "%.*s", (int)(f.name - 1 - key), key);
        rx_settings_list(find_setting, &f);
        if (!f.file)
            return usage_error(cli, "config: %s is no My Setting (`rxbox settings` lists them)", key);
        if (!rx_settings_value(f.file, f.name, value))
            return usage_error(cli, "config: %s takes %s, not \"%s\"", key, f.values, value);
        return 0;
    }
    RXConfig c;
    rx_config_defaults(&c);
    int ret = rx_config_set(&c, key, value);
    rx_config_free(&c);
    if (ret == RX_ERROR(E2BIG))
        return usage_error(cli, "config: %s: a colour's description is at most %d bytes, what the "
                           "tagger's question has room for", key, RX_CONFIG_VIBE_MAX);
    if (ret == RX_ERROR(EINVAL))
        return usage_error(cli, "config: %s takes %s, not \"%s\"", key, rx_config_takes(key), value);
    if (ret == RX_ERROR(ENOENT) && !rx_str_starts(key, "ui."))
        return usage_error(cli, "config: there is no setting %s (`rxbox config` lists them)", key);
    if (ret == RX_ERROR(ENOMEM))
        return fail("out of memory");
    return 0;
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
        "analysis.", "import.", "export.", "player.", "mixer.", "device.", "audio.", "ml.", "ui.",
        "color.",
    };
    /* color.<name>: one of the eight colours, its words in the legend. */
    int vibe = 0;
    for (int id = 1; key && id <= RX_TRACK_COLORS && !vibe; id++)
        vibe = !strcmp(key, rx_config_vibe_key(id)) ? id : 0;
    if (key) {
        bool known = false;
        for (size_t i = 0; i < RX_ARRAY_ELEMS(sections); i++)
            known |= rx_str_starts(key, sections[i]);
        if (!known)
            return usage_error(cli, "config: keys start with analysis., import., export., player., "
                               "mixer., device., audio., ml., ui. or color.");
        if (rx_str_starts(key, "color.") && !vibe)
            return usage_error(cli, "config: the colours are color.pink, color.red, color.orange, "
                               "color.yellow, color.green, color.aqua, color.blue and "
                               "color.purple");
        /* An empty value unsets the key: the default from then on. */
        int bad = value && *value ? check_setting(cli, key, value) : 0;
        if (bad)
            return bad;
    }

    int code = open_library(cli, false);
    if (code)
        return code;
    RXLibrary *lib = &cli->app.lib;

    if (key && value && vibe) {
        /* As the settings dialog keeps it: trimmed, and no row for the
         * default's own words or none. */
        int ret = rx_config_set_vibe(&lib->cfg, vibe, value);
        if (ret >= 0)
            ret = rx_library_setting_set(lib, key, lib->cfg.vibes[vibe - 1]);
        if (ret < 0)
            code = fail("cannot set %s: %s", key, strerror(-ret));
        else
            printf("%s = %s%s\n", key, rx_config_vibe(&lib->cfg, vibe),
                   lib->cfg.vibes[vibe - 1] ? "" : " (the default)");
    } else if (key && vibe) {
        printf("%s = %s%s\n", key, rx_config_vibe(&lib->cfg, vibe),
               lib->cfg.vibes[vibe - 1] ? "" : " (the default)");
    } else if (key && value) {
        int ret = rx_library_setting_set(lib, key, *value ? value : NULL);
        const char *deflt = rx_settings_get(NULL, key);
        if (ret < 0)
            code = fail("cannot set %s: %s", key, strerror(-ret));
        else if (*value)
            printf("%s = %s\n", key, value);
        else if (deflt)
            printf("%s is not set (the default, %s)\n", key, deflt);
        else
            printf("%s is not set (the default)\n", key);
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
        printf("import.comment = %s\n", c->import_comment ? "true" : "false");
        printf("export.key_notation = %s\n", c->key_notation);
        printf("export.pdb = %s\n", c->export_pdb ? "true" : "false");
        printf("export.onelibrary = %s\n", c->export_onelibrary ? "true" : "false");
        printf("export.compat_transcode = %s\n", c->compat_transcode ? "true" : "false");
        printf("export.format = %s\n", rx_stick_format_name(c->export_format));
        printf("export.device_name = %s\n",
               c->device_name ? c->device_name : "(the stick's label)");
        printf("export.background_color = %s\n",
               c->background_color ? rx_track_color_name(c->background_color) : "default");
        printf("link.mp3 = %s\n", c->link_mp3 ? "true" : "false");
        print_kv("player", c->player, c->nb_player);
        print_kv("mixer", c->mixer, c->nb_mixer);
        print_kv("device", c->device, c->nb_device);
        printf("audio.sink = %s\n", c->audio_sink ? c->audio_sink : "(the default)");
        printf("ml.model = %s\n", c->ml_model ? c->ml_model : "(models/" RX_APP_ML_MODEL ")");
        printf("ui.traffic_light = %s\n", c->traffic_light);
        printf("ui.detail_bars = %d\n", c->detail_bars);
        for (int id = 1; id <= RX_TRACK_COLORS; id++)
            printf("%s = %s\n", rx_config_vibe_key(id), rx_config_vibe(c, id));
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
    /* With --pro-link the deck is heard only in a mixer's LINK CUE, which
     * takes its audio from the ring (src/app/link.c). */
    if (cli->pro_link) {
        po.backend = RX_AUDIO_RING;
        po.sink = NULL;
    }

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
    /* The deck on the session bus; without one it runs on as it is. */
    if (!cli->no_mpris)
        rx_mpris_create(app);
    /* Link only when asked for, and then from the start, on every
     * Ethernet interface. */
    app->link_state.enabled = cli->pro_link;
    if (cli->pro_link)
        rx_app_link_start(app, NULL);
    ret = rx_app_run(app);
    rx_app_destroy(app);
    free(app);
    free(root);
    if (ret < 0)
        return fail("the interface stopped: %s", strerror(-ret));
    return EXIT_OK;
}

/* ------------------------------------------------------------------ link */

static bool player_same(const RXLinkPlayer *a, const RXLinkPlayer *b)
{
    return a->number == b->number && a->addr == b->addr && a->has_status == b->has_status &&
           a->track_device == b->track_device && a->track_slot == b->track_slot &&
           a->track_id == b->track_id && a->playing == b->playing;
}

static void print_player(const RXLinkPlayer *p, int ours, const RXLibrary *lib)
{
    printf("  %2d %-20s %u.%u.%u.%u", p->number, p->name, p->addr >> 24, p->addr >> 16 & 0xFF,
           p->addr >> 8 & 0xFF, p->addr & 0xFF);
    if (p->has_status && p->track_id) {
        printf("  %s track %u from %d", p->playing ? "playing" : "loaded", p->track_id,
               p->track_device);
        RXTrack *t = ours && p->track_device == ours ? rx_library_find_id(lib, p->track_id) : NULL;
        if (t) {
            char name[512];
            rx_track_display(t, name, sizeof(name));
            printf(" (%s)", name);
        }
    }
    putchar('\n');
}

/* Serves until SIGINT or SIGTERM, printing the devices whenever they
 * change. */
static bool served_same(const RXLinkServed *a, const RXLinkServed *b)
{
    return !strcmp(a->interface, b->interface) && a->addr == b->addr &&
           a->running == b->running && a->err == b->err && a->state == b->state &&
           a->number == b->number;
}

/* The cues players saved on the deck or deleted and the ratings they
 * gave, into the library: the services have served them since
 * (rx_link_set_edits). True when the library changed. */
static bool link_take_edits(Cli *cli, RXLinkSet *set)
{
    bool changed = false;
    RXLinkEdit e[16];
    int n;
    do {
        n = rx_link_set_edits(set, e, 16);
        for (int i = 0; i < n; i++) {
            RXTrack *t = rx_library_find_id(&cli->app.lib, e[i].track_id);
            if (!t)
                continue;
            char name[512], at[32], what[16];
            rx_track_display(t, name, sizeof(name));
            if (e[i].what == RX_LINK_EDIT_RATING) {
                int ret = rx_link_edit_apply(t, &e[i]);
                if (ret > 0 && (ret = rx_library_save_track(&cli->app.lib, t)) >= 0)
                    changed = true;
                if (ret < 0)
                    printf("player %d: cannot keep the rating of %s: %s\n", e[i].player, name,
                           strerror(-ret));
                else
                    printf("player %d rated %s %d star%s\n", e[i].player, name, e[i].rating,
                           e[i].rating == 1 ? "" : "s");
                continue;
            }
            if (e[i].cue.hot >= 0)
                snprintf(what, sizeof(what), "hot cue %c", rx_cue_letter(&e[i].cue));
            else
                snprintf(what, sizeof(what), "a memory %s",
                         rx_cue_is_loop(&e[i].cue) ? "loop" : "cue");
            int ret = rx_link_edit_apply(t, &e[i]);
            if (ret > 0 && (ret = rx_library_save_track(&cli->app.lib, t)) >= 0)
                changed = true;
            if (ret < 0)
                printf("player %d: cannot keep %s on %s: %s\n", e[i].player, what, name,
                       strerror(-ret));
            else
                printf("player %d %s %s at %s on %s\n", e[i].player,
                       e[i].remove ? "deleted" : "saved", what,
                       rx_fmt_time(e[i].cue.time, at, sizeof(at)), name);
        }
    } while (n == 16);
    return changed;
}

/* Served on every Ethernet interface, or on the one -i names, until
 * interrupted: each interface's state and the devices said as they
 * change, and the cues players save kept in the library. */
static int link_serve(Cli *cli, const RXLinkOptions *opt)
{
    char *root = library_root(cli);
    if (!root)
        return fail("out of memory");
    RXLinkOptions o = *opt;
    o.library = root;
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    /* The ids players report are the library's: a connection of our own
     * to name them, and to keep the cues they save (the services never
     * write the library). */
    int code = open_library(cli, false);
    if (code) {
        free(root);
        return code;
    }
    RXLinkSet *set;
    int ret = rx_link_set_start(&set, &o);
    free(root);
    if (ret < 0)
        return fail("cannot serve: %s", strerror(-ret));
    printf("serving on %s; Ctrl-C stops\n", o.interface ? o.interface : "every Ethernet interface");
    fflush(stdout);

    RXLinkServed was[8];
    int nb_was = 0;
    RXLinkPlayer seen[64], now[64];
    int nb_seen = 0, where[64];
    /* The services read the tracks saved again once the edits have
     * settled for this long, as the interface does (src/app/link.c). */
    int64_t edited_us = 0;
    while (1) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 250000000 };
        if (sigtimedwait(&sigs, NULL, &ts) >= 0)
            break;
        if (link_take_edits(cli, set))
            edited_us = rx_time_us();
        if (edited_us && rx_time_us() - edited_us >= 2000000) {
            uint64_t *ids = NULL;
            int nb = 0;
            if (rx_library_take_changes(&cli->app.lib, &ids, &nb) != 0)
                rx_link_set_library_changed(set);
            else if (nb)
                rx_link_set_tracks_changed(set, ids, nb);
            free(ids);
            edited_us = 0;
        }
        RXLinkServed ifs[8];
        int nb_ifs = RX_MIN(rx_link_set_interfaces(set, ifs, 8), 8);
        for (int i = 0; i < nb_ifs; i++) {
            int k = 0;
            while (k < nb_was && strcmp(was[k].interface, ifs[i].interface))
                k++;
            if (k < nb_was && served_same(&was[k], &ifs[i]))
                continue;
            const RXLinkServed *v = &ifs[i];
            if (!v->running && v->err)
                printf("%s: cannot serve: %s\n", v->interface, v->err == RX_ERROR(EPROTONOSUPPORT)
                       ? "not Ethernet (Link runs on Ethernet only)" : strerror(-v->err));
            else if (!v->running)
                printf("%s: starting\n", v->interface);
            else if (v->state == RX_LINK_UP)
                printf("link up as device %d on %s\n", v->number, v->interface);
            else
                printf("%s: %s\n", v->interface, v->state == RX_LINK_JOINING ? "joining"
                                                                            : "waiting for a player");
        }
        for (int k = 0; k < nb_was; k++) {
            int i = 0;
            while (i < nb_ifs && strcmp(was[k].interface, ifs[i].interface))
                i++;
            if (i == nb_ifs)
                printf("%s: gone\n", was[k].interface);
        }
        memcpy(was, ifs, (size_t)nb_ifs * sizeof(*ifs));
        nb_was = nb_ifs;

        int nb = RX_MIN(rx_link_set_players(set, now, where, 64), 64);
        bool same = nb == nb_seen;
        for (int i = 0; same && i < nb; i++)
            same = player_same(&now[i], &seen[i]);
        if (!same) {
            printf("devices:%s\n", nb ? "" : " none");
            for (int i = 0; i < nb; i++) {
                const RXLinkServed *v = where[i] < nb_ifs ? &ifs[where[i]] : NULL;
                print_player(&now[i], v && v->state == RX_LINK_UP ? v->number : 0,
                             &cli->app.lib);
            }
            memcpy(seen, now, (size_t)nb * sizeof(*now));
            nb_seen = nb;
        }
        fflush(stdout);
    }
    link_take_edits(cli, set);
    rx_link_set_stop(&set);
    printf("stopped\n");
    return EXIT_OK;
}


/* A fake player against a server: what rbxport's browse example does, and
 * the after-load requests. */
// Holding: follows what is loaded, the first track and every one the
// source tells it to load (playing or paused as it was), until `hold`
// seconds have passed.
static void link_hold(RXLinkClient *c, long hold)
{
    int64_t end = rx_time_us() + (int64_t)hold * 1000000;
    uint32_t played = 0;
    while (rx_time_us() < end) {
        int device, slot;
        uint32_t id;
        rx_link_client_loaded(c, &device, &slot, &id);
        if (id && id != played) {
            int nb = rx_link_client_play(c, id);
            if (nb >= 0)
                printf("track %u loaded (%d beats)\n", id, nb);
            else
                printf("track %u loaded, no beat grid (%s): no beat numbers\n", id,
                       strerror(-nb));
            fflush(stdout);
            played = id;
        }
        usleep(100000);
    }
}

/* A cue to save once loaded (--save-cue): `hot` 0..7, -1 a memory cue,
 * -2 none. */
typedef struct SaveCue {
    int    hot;
    double time;
} SaveCue;

static bool parse_save_cue(const char *s, SaveCue *c)
{
    char x = (char)(*s & ~0x20);
    if (!((x >= 'A' && x <= 'H') || x == 'M') || s[1] != '@')
        return false;
    char *end;
    c->time = strtod(s + 2, &end);
    c->hot = x == 'M' ? -1 : x - 'A';
    return end != s + 2 && !*end && c->time >= 0 && c->time < 86400;
}

static int link_browse(const RXLinkClientOptions *o, uint32_t track, long hold, SaveCue save)
{
    RXLinkClient *c;
    int ret = rx_link_client_start(&c, o);
    if (ret < 0)
        return fail("cannot start a player on %s: %s", o->interface, strerror(-ret));
    printf("player %d (%s) on %s, waiting for a source\n", o->number, o->name, o->interface);
    fflush(stdout);
    uint32_t addr;
    int number;
    int wait_ms = (int)RX_MIN(RX_MAX(hold * 1000, 20000L), (long)INT32_MAX);
    if ((ret = rx_link_client_wait_source(c, wait_ms, &addr, &number)) < 0) {
        rx_link_client_stop(&c);
        return fail("no computer source answered on %s", o->interface);
    }
    ret = rx_link_client_browse(c, track, print_line, NULL);
    if (ret >= 0 && save.hot > -2) {
        int device, slot, nb = 0;
        uint32_t id;
        rx_link_client_loaded(c, &device, &slot, &id);
        char at[32], what[16];
        rx_fmt_time(save.time, at, sizeof(at));
        if (save.hot >= 0)
            snprintf(what, sizeof(what), "hot cue %c", 'A' + save.hot);
        else
            snprintf(what, sizeof(what), "a memory cue");
        ret = rx_link_client_save_cue(c, id, save.hot, save.time, &nb);
        if (ret >= 0)
            printf("saved %s at %s on track %u: the source lists %d cue%s\n", what, at, id, nb,
                   nb == 1 ? "" : "s");
        else
            printf("saving %s at %s on track %u failed: %s\n", what, at, id, strerror(-ret));
    }
    fflush(stdout);
    if (ret >= 0 && hold > 0)
        link_hold(c, hold);
    rx_link_client_stop(&c);
    if (ret < 0)
        return fail("browsing failed: %s", strerror(-ret));
    return EXIT_OK;
}

static int cmd_link(Cli *cli, int argc, char **argv)
{
    RXLinkOptions opt;
    rx_link_options_default(&opt);
    RXLinkClientOptions copt;
    rx_link_client_options_default(&copt);
    long number = 3, track = 0, hold = 0;
    SaveCue save = { .hot = -2 };
    const char *sub = NULL, *host = NULL;
    for (int i = 1; i < argc; i++) {
        const char *val;
        int r = common_option(cli, argc, argv, &i);
        if (r < 0)
            return EXIT_USAGE;
        if (r > 0)
            continue;
        if ((r = opt_value(cli, argc, argv, &i, "-i", "--interface", &val))) {
            if (r < 0)
                return usage_error(cli, "link: --interface needs a network interface");
            opt.interface = val;
        } else if (opt_flag(cli, argv[i], NULL, "--any-peer")) {
            opt.any_peer = true;
        } else if (opt_flag(cli, argv[i], NULL, "--wait")) {
            opt.start_up = false;
        } else if (opt_flag(cli, argv[i], NULL, "--quiet")) {
            copt.quiet = true;
        } else if ((r = opt_value(cli, argc, argv, &i, "-n", "--number", &val))) {
            if (r < 0 || !parse_count(val, 6, &number) || number < 1)
                return usage_error(cli, "link: --number takes a player number, 1 to 6");
        } else if ((r = opt_value(cli, argc, argv, &i, NULL, "--hold", &val))) {
            if (r < 0 || !parse_count(val, 86400, &hold))
                return usage_error(cli, "link: --hold takes seconds");
        } else if ((r = opt_value(cli, argc, argv, &i, "-t", "--track", &val))) {
            if (r < 0 || !parse_count(val, UINT32_MAX, &track))
                return usage_error(cli, "link: --track takes a track id");
        } else if ((r = opt_value(cli, argc, argv, &i, NULL, "--save-cue", &val))) {
            if (r < 0 || !parse_save_cue(val, &save))
                return usage_error(cli, "link: --save-cue takes a hot cue A..H or M, @ and "
                                        "seconds (A@1.5)");
        } else if ((r = opt_value(cli, argc, argv, &i, NULL, "--name", &val))) {
            if (r < 0 || strlen(val) > 20)
                return usage_error(cli, "link: --name takes a model name, 20 characters at most");
            copt.name = val;
        } else if (is_option(cli, argv[i])) {
            return usage_error(cli, "link: unknown option %s", argv[i]);
        } else if (!sub) {
            sub = argv[i];
        } else if (!host && !strcmp(sub, "browse")) {
            host = argv[i];
        } else {
            return usage_error(cli, "link: one command at a time");
        }
    }
    if (!sub || (strcmp(sub, "serve") && strcmp(sub, "browse")))
        return usage_error(cli, "link: serve or browse");
    if (!strcmp(sub, "serve"))
        return link_serve(cli, &opt);
    if (!opt.interface)
        return usage_error(cli, "link %s: which network interface (-i)?", sub);
    if (copt.quiet && !host)
        return usage_error(cli, "link browse: --quiet needs the server's address");
    copt.interface = opt.interface;
    copt.server = host;
    copt.number = (int)number;
    return link_browse(&copt, (uint32_t)track, hold, save);
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
    { "tag",      cmd_tag,      "tag TRACK..." },
    { "color",    cmd_color,    "color HUE|NAME|none TRACK..." },
    { "colour",   cmd_color,    "colour HUE|NAME|none TRACK..." },
    { "export",   cmd_export,   "export [--all | -p NAME...] [--prune] [--compat | --no-compat] [--format F] "
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
    { "link",     cmd_link,     "link serve [-i IF] [--any-peer] [--wait] | "
                                "link browse -i IF [-n N] [-t ID] [--name NAME] [--hold SEC] "
                                "[--save-cue X@SEC] [--quiet] [HOST]" },
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
        if (opt_flag(&cli, a, NULL, "--no-mpris")) {
            cli.no_mpris = true;
            continue;
        }
        if (opt_flag(&cli, a, NULL, "--pro-link")) {
            cli.pro_link = true;
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

    /* `rxbox help CMD` and `rxbox CMD -h|--help` (before a `--`): the
     * command's usage line, on stdout, and success. */
    bool help = !strcmp(argv[i], "help");
    const char *name = help && i + 1 < argc ? argv[i + 1] : argv[i];
    for (int k = i + 1; !help && k < argc && strcmp(argv[k], "--"); k++)
        help = !strcmp(argv[k], "-h") || !strcmp(argv[k], "--help");

    cli.cmd = name;
    for (size_t k = 0; k < RX_ARRAY_ELEMS(commands); k++) {
        if (strcmp(commands[k].name, name))
            continue;
        if (help) {
            printf("usage: rxbox %s\n", commands[k].usage);
            return EXIT_OK;
        }
        cli.usage = commands[k].usage;
        return commands[k].fn(&cli, argc - i, argv + i);
    }
    if (help && !strcmp(name, "help")) {
        fputs(usage_text, stdout);
        return EXIT_OK;
    }
    return usage_error(NULL, "unknown command %s", name);
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
