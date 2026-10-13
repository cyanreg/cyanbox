/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MPRIS (rxbox/mpris.h) on a private bus of the test's own, never the
 * session's: a dbus-daemon listening on a socket in the scratch directory
 * or, where there is no dbus-daemon (a system on dbus-broker),
 * dbus-broker-launch handed a socket bound there; skipped (77) without
 * either. The app is test_app's: no window, the null backend in manual
 * mode, a scratch library of two copies of the test WAV, one under a name
 * a URL must escape. A second sd-bus connection is the client.
 *
 * It checks the name, Introspect, GetAll on both interfaces and Stopped
 * with nothing loaded; after a load the metadata (track id, length, URL,
 * title, the cover file) and Paused; Play (the deck plays, PropertiesChanged
 * says so), Seek and its Seeked, a seek on the deck itself and its Seeked,
 * none while playing on; Pause; Stop to the cue point from playing and from
 * paused; SetPosition, and a stale track id or a position off the track
 * ignored; CanGoNext and CanGoPrevious over a two-track view; Next loading
 * and playing the other track without a Paused in between, and nothing
 * past the last row; Previous keeping a paused deck paused; OpenUri, and
 * its errors; the volume; the read-only properties; Quit; a second app
 * refused the name and an app without a bus carrying on; after destroy the
 * name and the cover file gone; a bus that goes away ending MPRIS, not
 * the app. Position is never in a PropertiesChanged.
 *
 * The bus is killed at exit on every path: atexit, and a fatal signal
 * (an alarm bounds the run) kills its process group.
 *
 * Usage: test_mpris FILE.wav SCRATCH_DIR */

#include "rxbox/mpris.h"

#include <fcntl.h>
#include <ftw.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

extern char **environ;

#define NAME   "org.mpris.MediaPlayer2.rxbox"
#define PATH   "/org/mpris/MediaPlayer2"
#define ROOT   "org.mpris.MediaPlayer2"
#define PLAYER "org.mpris.MediaPlayer2.Player"
#define PROPS  "org.freedesktop.DBus.Properties"

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);   \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    remove(path);
    return 0;
}

static void remove_tree(const char *dir)
{
    nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

static int copy_file(const char *from, const char *to)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int ret = rx_file_read(from, &data, &size);
    if (ret < 0)
        return ret;
    ret = rx_file_write_atomic(to, data, size);
    free(data);
    return ret;
}

/* ------------------------------------------------------------------ the bus */

/* The bus' process, the leader of a process group of its own (the broker
 * dbus-broker-launch starts is in it too). */
static pid_t bus_pid;

/* At exit: asks the group to go, then makes it. */
static void bus_kill(void)
{
    if (bus_pid <= 0)
        return;
    kill(-bus_pid, SIGTERM);
    for (int i = 0; i < 200; i++) {
        if (waitpid(bus_pid, NULL, WNOHANG) != 0) {
            bus_pid = 0;
            return;
        }
        usleep(10000);
    }
    kill(-bus_pid, SIGKILL);
    waitpid(bus_pid, NULL, 0);
    bus_pid = 0;
}

/* A fatal signal (the alarm, a crash, meson's timeout): only what is
 * async-signal-safe, then the signal's own end. */
static void on_fatal(int sig)
{
    if (bus_pid > 0) {
        kill(-bus_pid, SIGKILL);
        waitpid(bus_pid, NULL, 0);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* `name` on PATH; NULL when it is not there. Allocating. */
static char *find_program(const char *name)
{
    const char *dirs = getenv("PATH");
    if (!dirs || !dirs[0])
        dirs = "/usr/bin:/bin";
    while (*dirs) {
        const char *end = strchrnul(dirs, ':');
        char *full = rx_asprintf("%.*s/%s", (int)(end - dirs), dirs, name);
        if (full && !access(full, X_OK))
            return full;
        free(full);
        dirs = *end ? end + 1 : end;
    }
    return NULL;
}

/* A D-Bus address of a unix socket: its file, or, when `abstract`, its
 * abstract name, every byte but [-0-9A-Za-z_/.\*] as %XX (the
 * specification's escaping). Allocating. */
static char *socket_address(const char *path, bool abstract)
{
    char *address = malloc(sizeof("unix:abstract=") + 3 * strlen(path));
    if (!address)
        return NULL;
    char *o = address + sprintf(address, "unix:%s=", abstract ? "abstract" : "path");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            strchr("-_/.\\*", *p))
            *o++ = (char)*p;
        else
            o += sprintf(o, "%%%02x", *p);
    }
    *o = '\0';
    return address;
}

/* posix_spawn in a process group of its own. */
static int spawn(const char *prog, char **argv, posix_spawn_file_actions_t *fa)
{
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    int err = posix_spawn(&bus_pid, prog, fa, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (err) {
        bus_pid = 0;
        fprintf(stderr, "cannot run %s: %s\n", prog, strerror(err));
        return -1;
    }
    return 0;
}

/* dbus-daemon --session on `listen` (a D-Bus address); the address it
 * prints once it listens. */
static char *start_daemon(const char *prog, const char *listen)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return NULL;
    char *opt = rx_asprintf("--address=%s", listen);
    char *argv[] = { (char *)prog, "--session", "--nofork", "--print-address=1", opt, NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    int ret = opt ? spawn(prog, argv, &fa) : -1;
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    free(opt);

    char line[512];
    size_t n = 0;
    struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
    while (ret >= 0 && n < sizeof(line) - 1 && !memchr(line, '\n', n) && poll(&pfd, 1, 10000) > 0) {
        ssize_t got = read(fds[0], line + n, sizeof(line) - 1 - n);
        if (got <= 0)
            break;
        n += (size_t)got;
    }
    close(fds[0]);
    char *nl = memchr(line, '\n', n);
    if (ret < 0 || !nl) {
        fprintf(stderr, "dbus-daemon printed no address\n");
        return NULL;
    }
    *nl = '\0';
    return rx_strdup(line);
}

/* dbus-broker-launch on a socket bound here (it takes its socket as
 * systemd would hand it over: fd 3, LISTEN_FDS and LISTEN_PID, which only
 * a shell exec'ing it can set to its own pid; the LISTEN_ variables this
 * process may have inherited describe someone else's sockets). `path` is
 * the socket's file, or, when `abstract`, its abstract name. The launcher
 * connects to the user bus too, for systemd's activation: it is given its
 * own bus as that, so that nothing of the test reaches the session's. */
static char *start_broker(const char *prog, const char *dir, const char *path, bool abstract)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    size_t n = strlen(path);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + (abstract ? 1 : 0));
    if (abstract) {
        memcpy(sa.sun_path + 1, path, n);
    } else {
        memcpy(sa.sun_path, path, n);
        unlink(path);
    }
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&sa, len) < 0 || listen(s, 64) < 0) {
        fprintf(stderr, "cannot listen on %s: %s\n", path, strerror(errno));
        if (s >= 0)
            close(s);
        return NULL;
    }
    /* Above 3, so that the dup2 to 3 below is a real one. */
    int fd = fcntl(s, F_DUPFD_CLOEXEC, 10);
    close(s);

    static const char conf_xml[] =
        "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
        "<busconfig>\n"
        "  <type>session</type>\n"
        "  <policy context=\"default\">\n"
        "    <allow send_destination=\"*\"/>\n"
        "    <allow receive_sender=\"*\"/>\n"
        "    <allow own=\"*\"/>\n"
        "  </policy>\n"
        "</busconfig>\n";
    char *address = socket_address(path, abstract);
    char *conf = rx_path_join(dir, "bus.conf");
    char *log = rx_path_join(dir, "bus.log");
    int ret = fd >= 0 && address && conf && log &&
              rx_file_write(conf, conf_xml, strlen(conf_xml)) >= 0 ? 0 : -1;
    if (ret >= 0) {
        char *argv[] = {
            "/bin/sh", "-c",
            "unset LISTEN_PIDFDID LISTEN_FDNAMES; export LISTEN_PID=$$ LISTEN_FDS=1 "
            "DBUS_SESSION_BUS_ADDRESS=\"$2\"; exec \"$0\" --scope user --config-file \"$1\"",
            (char *)prog, conf, address, NULL,
        };
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fd, 3);
        posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        ret = spawn("/bin/sh", argv, &fa);
        posix_spawn_file_actions_destroy(&fa);
    }
    if (fd >= 0)
        close(fd);
    free(conf);
    free(log);
    if (ret < 0) {
        free(address);
        return NULL;
    }
    return address;
}

/* A bus on a socket in `dir`, or on an abstract socket when the path is
 * longer than the bus programs take (99 bytes for dbus-daemon,
 * _DBUS_MAX_SUN_PATH_LENGTH; 107 for bind() and sd-bus: a deep build
 * directory); its address. NULL with *skip set when there is no bus
 * program to run. */
static char *start_bus(const char *dir, bool *skip)
{
    *skip = false;
    char *daemon = find_program("dbus-daemon");
    char *broker = daemon ? NULL : find_program("dbus-broker-launch");
    if (!daemon && !broker) {
        *skip = true;
        return NULL;
    }

    char *path = rx_path_join(dir, "bus");
    bool abstract = !path || strlen(path) > 99;
    if (abstract) {
        free(path);
        path = rx_asprintf("rxbox-test-mpris-%d", (int)getpid());
    }

    char *address = NULL;
    if (path && daemon) {
        char *listen = socket_address(path, abstract);
        address = listen ? start_daemon(daemon, listen) : NULL;
        free(listen);
    } else if (path) {
        address = start_broker(broker, dir, path, abstract);
    }
    printf("bus: %s on %s\n", daemon ? daemon : broker, address ? address : "(none)");
    free(path);
    free(daemon);
    free(broker);
    return address;
}

/* ---------------------------------------------------------------- the client */

typedef struct Ctx {
    RXApp app;
    sd_bus *client;
} Ctx;

/* The signals the client saw, in order. */
typedef struct Seen {
    char    member[24];
    int64_t position;           /* Seeked */
    char    changed[512];       /* PropertiesChanged: " Name Name ... " */
    char    status[16];         /* PlaybackStatus, when among them */
} Seen;

#define MAX_SEEN 4096
static Seen seen[MAX_SEEN];
static int nb_seen;

/* Every signal from PATH (the match is on the path). */
static int on_signal(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    if (nb_seen >= MAX_SEEN)
        return 0;

    Seen *s = &seen[nb_seen];
    *s = (Seen) { 0 };
    if (sd_bus_message_is_signal(msg, PLAYER, "Seeked")) {
        if (sd_bus_message_read(msg, "x", &s->position) < 0)
            return 0;
        snprintf(s->member, sizeof(s->member), "Seeked");
    } else if (sd_bus_message_is_signal(msg, PROPS, "PropertiesChanged") &&
               sd_bus_message_has_signature(msg, "sa{sv}as")) {
        const char *iface;
        sd_bus_message_read(msg, "s", &iface);
        snprintf(s->member, sizeof(s->member), "PropertiesChanged");
        snprintf(s->changed, sizeof(s->changed), " %s: ", iface);
        sd_bus_message_enter_container(msg, 'a', "{sv}");
        while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
            const char *name, *status;
            sd_bus_message_read(msg, "s", &name);
            size_t len = strlen(s->changed);
            snprintf(s->changed + len, sizeof(s->changed) - len, "%s ", name);
            if (!strcmp(name, "PlaybackStatus") && sd_bus_message_read(msg, "v", "s", &status) > 0)
                snprintf(s->status, sizeof(s->status), "%s", status);
            else
                sd_bus_message_skip(msg, "v");
            sd_bus_message_exit_container(msg);
        }
    } else {
        return 0;
    }
    nb_seen++;
    return 0;
}

/* One tick of everything: 10 ms of the deck when `play`, the app, MPRIS,
 * and the client's connection. */
static void tick(Ctx *c, bool play)
{
    if (play)
        rx_player_null_advance(c->app.player, 480);
    rx_app_tick(&c->app);
    rx_mpris_tick(&c->app);
    if (c->client) {
        while (sd_bus_process(c->client, NULL) > 0)
            ;
    }
    usleep(1000);
}

static void step(Ctx *c)
{
    tick(c, true);
}

static void steps(Ctx *c, int n)
{
    for (int i = 0; i < n; i++)
        step(c);
}

static bool wait_until(Ctx *c, bool (*done)(Ctx *c), int seconds)
{
    int64_t t0 = rx_time_us();
    while (rx_time_us() - t0 < seconds * 1000000LL) {
        step(c);
        if (done(c))
            return true;
    }
    return false;
}

static int on_reply(sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    sd_bus_message **out = opaque;
    *out = sd_bus_message_ref(reply);
    return 0;
}

/* A call of iface.member on the app's object, with the arguments `types`
 * says (sd_bus_message_append's), the app ticking until its reply is in (a
 * blocking call would wait for a tick that cannot come). The deck does
 * not play meanwhile, so what a call did to the playhead is exact however
 * long the bus takes. NULL without a reply. */
static sd_bus_message *vcall(Ctx *c, const char *iface, const char *member, const char *types,
                             va_list args)
{
    sd_bus_message *msg = NULL, *reply = NULL;
    sd_bus_slot *slot = NULL;
    int ret = sd_bus_message_new_method_call(c->client, &msg, NAME, PATH, iface, member);
    if (ret >= 0 && types)
        ret = sd_bus_message_appendv(msg, types, args);
    if (ret >= 0)
        ret = sd_bus_call_async(c->client, &slot, msg, on_reply, &reply, 5000000);
    sd_bus_message_unref(msg);
    if (ret < 0) {
        fprintf(stderr, "%s.%s: %s\n", iface, member, strerror(-ret));
        return NULL;
    }
    int64_t t0 = rx_time_us();
    while (!reply && rx_time_us() - t0 < 10000000)
        tick(c, false);
    /* Cancels the callback if it never came. */
    sd_bus_slot_unref(slot);
    return reply;
}

static sd_bus_message *call(Ctx *c, const char *iface, const char *member, const char *types,
                            ...)
{
    va_list args;
    va_start(args, types);
    sd_bus_message *reply = vcall(c, iface, member, types, args);
    va_end(args);
    return reply;
}

/* The error a reply is, or "" for a method return; "no reply" for none. */
static const char *reply_error(sd_bus_message *reply)
{
    if (!reply)
        return "no reply";
    if (sd_bus_message_is_method_error(reply, NULL))
        return sd_bus_message_get_error(reply)->name;
    return "";
}

/* A call as call() makes it; "" when it returned, else the error. */
static const char *method(Ctx *c, const char *iface, const char *member, const char *types, ...)
{
    static char err[256];
    va_list args;
    va_start(args, types);
    sd_bus_message *reply = vcall(c, iface, member, types, args);
    va_end(args);
    snprintf(err, sizeof(err), "%s", reply_error(reply));
    sd_bus_message_unref(reply);
    return err;
}

static const char *method_x(Ctx *c, const char *member, int64_t x)
{
    return method(c, PLAYER, member, "x", x);
}

static const char *set_position(Ctx *c, const char *track, int64_t x)
{
    return method(c, PLAYER, "SetPosition", "ox", track, x);
}

/* Properties.Get: the reply, read into the variant, which must hold
 * `sig`; NULL on an error. */
static sd_bus_message *get(Ctx *c, const char *iface, const char *name, const char *sig)
{
    sd_bus_message *reply = call(c, PROPS, "Get", "ss", iface, name);
    const char *err = reply_error(reply);
    if (*err || sd_bus_message_enter_container(reply, 'v', sig) <= 0) {
        fprintf(stderr, "Get %s: %s\n", name, *err ? err : "another type");
        sd_bus_message_unref(reply);
        return NULL;
    }
    return reply;
}

/* A basic property into `out` (int for a boolean, double, int64_t or a
 * const char * valid until the next call). */
static bool get_basic(Ctx *c, const char *iface, const char *name, char type, void *out)
{
    static sd_bus_message *last;
    last = sd_bus_message_unref(last);
    char sig[2] = { type, '\0' };
    last = get(c, iface, name, sig);
    return last && sd_bus_message_read_basic(last, type, out) > 0;
}

static int get_bool(Ctx *c, const char *iface, const char *name)
{
    int b;
    return get_basic(c, iface, name, 'b', &b) ? b : -1;
}

static double get_double(Ctx *c, const char *iface, const char *name)
{
    double d;
    return get_basic(c, iface, name, 'd', &d) ? d : NAN;
}

static int64_t get_int64(Ctx *c, const char *iface, const char *name)
{
    int64_t x;
    return get_basic(c, iface, name, 'x', &x) ? x : INT64_MIN;
}

static const char *get_string(Ctx *c, const char *iface, const char *name)
{
    const char *s;
    return get_basic(c, iface, name, 's', &s) ? s : "(error)";
}

static const char *status(Ctx *c)
{
    return get_string(c, PLAYER, "PlaybackStatus");
}

/* Whether the string array property holds `want`. */
static bool get_strings_has(Ctx *c, const char *iface, const char *name, const char *want)
{
    sd_bus_message *reply = get(c, iface, name, "as");
    bool found = false;
    if (reply && sd_bus_message_enter_container(reply, 'a', "s") > 0) {
        const char *s;
        while (!found && sd_bus_message_read_basic(reply, 's', &s) > 0)
            found = !strcmp(s, want);
    }
    sd_bus_message_unref(reply);
    return found;
}

/* GetAll on `iface` names exactly `names`. */
static bool get_all(Ctx *c, const char *iface, const char *const *names, int n)
{
    sd_bus_message *reply = call(c, PROPS, "GetAll", "s", iface);
    const char *err = reply_error(reply);
    if (*err || !sd_bus_message_has_signature(reply, "a{sv}") ||
        sd_bus_message_enter_container(reply, 'a', "{sv}") <= 0) {
        fprintf(stderr, "GetAll %s: %s\n", iface, *err ? err : "not a{sv}");
        sd_bus_message_unref(reply);
        return false;
    }
    int count = 0, found = 0;
    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *name;
        sd_bus_message_read(reply, "s", &name);
        for (int i = 0; i < n; i++)
            found += !strcmp(name, names[i]);
        count++;
        sd_bus_message_skip(reply, "v");
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    if (count != n || found != n)
        fprintf(stderr, "GetAll %s: %d properties, %d of the %d expected\n", iface, count, found,
                n);
    return count == n && found == n;
}

typedef struct Meta {
    char    trackid[128], url[1024], title[256], artist[256], art[1024];
    int64_t length;
    int     entries;
} Meta;

/* The basic value of `type` next in `msg`, a string, into `buf`. */
static void copy_string(sd_bus_message *msg, char type, char *buf, size_t size)
{
    const char *s;
    if (sd_bus_message_read_basic(msg, type, &s) > 0)
        snprintf(buf, size, "%s", s);
}

static bool get_meta(Ctx *c, Meta *out)
{
    *out = (Meta) { .length = -1 };
    sd_bus_message *reply = get(c, PLAYER, "Metadata", "a{sv}");
    bool ok = reply && sd_bus_message_enter_container(reply, 'a', "{sv}") > 0;
    while (ok && sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key, *type;
        ok = sd_bus_message_read(reply, "s", &key) > 0 &&
             sd_bus_message_peek_type(reply, NULL, &type) > 0 &&
             sd_bus_message_enter_container(reply, 'v', type) > 0;
        if (!ok)
            break;
        if (!strcmp(key, "mpris:trackid") && !strcmp(type, "o")) {
            copy_string(reply, 'o', out->trackid, sizeof(out->trackid));
        } else if (!strcmp(key, "mpris:length") && !strcmp(type, "x")) {
            sd_bus_message_read_basic(reply, 'x', &out->length);
        } else if (!strcmp(key, "xesam:url") && !strcmp(type, "s")) {
            copy_string(reply, 's', out->url, sizeof(out->url));
        } else if (!strcmp(key, "xesam:title") && !strcmp(type, "s")) {
            copy_string(reply, 's', out->title, sizeof(out->title));
        } else if (!strcmp(key, "mpris:artUrl") && !strcmp(type, "s")) {
            copy_string(reply, 's', out->art, sizeof(out->art));
        } else if (!strcmp(key, "xesam:artist") && !strcmp(type, "as") &&
                   sd_bus_message_enter_container(reply, 'a', "s") > 0) {
            /* The first artist; the rest read past. */
            const char *s;
            for (int i = 0; sd_bus_message_read_basic(reply, 's', &s) > 0; i++) {
                if (!i)
                    snprintf(out->artist, sizeof(out->artist), "%s", s);
            }
            sd_bus_message_exit_container(reply);
        } else {
            sd_bus_message_skip(reply, type);
        }
        out->entries++;
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    return ok;
}

/* The first signal at or after `from` that is `member` (with `prop` among
 * the changed properties, and PlaybackStatus `status`, when given),
 * ticking until it comes; -1 after 5 s. */
static int wait_signal(Ctx *c, int from, const char *member, const char *prop, const char *st)
{
    char key[64];
    snprintf(key, sizeof(key), " %s ", prop ? prop : "");
    int64_t t0 = rx_time_us();
    while (1) {
        for (int i = from; i < nb_seen; i++) {
            if (strcmp(seen[i].member, member) || (prop && !strstr(seen[i].changed, key)) ||
                (st && strcmp(seen[i].status, st)))
                continue;
            return i;
        }
        if (rx_time_us() - t0 > 5000000)
            return -1;
        step(c);
    }
}

/* A Seeked at or after `from` to within `tolerance` of `seconds`. */
static int wait_seeked(Ctx *c, int from, double seconds, double tolerance)
{
    int64_t t0 = rx_time_us();
    while (1) {
        for (int i = from; i < nb_seen; i++) {
            if (!strcmp(seen[i].member, "Seeked") &&
                fabs(seen[i].position * 1e-6 - seconds) <= tolerance)
                return i;
        }
        if (rx_time_us() - t0 > 5000000)
            return -1;
        step(c);
    }
}

/* As wait_signal, among those already in, without ticking. */
static int find_signal(int from, const char *member, const char *prop, const char *st)
{
    char key[64];
    snprintf(key, sizeof(key), " %s ", prop ? prop : "");
    for (int i = from; i < nb_seen; i++) {
        if (strcmp(seen[i].member, member) || (prop && !strstr(seen[i].changed, key)) ||
            (st && strcmp(seen[i].status, st)))
            continue;
        return i;
    }
    return -1;
}

static int count_seen(int from, const char *member)
{
    int n = 0;
    for (int i = from; i < nb_seen; i++)
        n += !strcmp(seen[i].member, member);
    return n;
}

/* What xesam:url must be: RFC 3986's unreserved characters and '/' as they
 * are, every other byte as %XX. */
static char *expect_url(const char *path)
{
    char *url = malloc(8 + 3 * strlen(path));
    if (!url)
        abort();
    char *o = url + sprintf(url, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            strchr("-._~/", *p))
            *o++ = (char)*p;
        else
            o += sprintf(o, "%%%02X", *p);
    }
    *o = '\0';
    return url;
}

/* The client's own connection to the bus at `address`, seeing every
 * signal from PATH; NULL when it cannot be made. */
static sd_bus *client_open(const char *address)
{
    sd_bus *bus = NULL;
    int ret = sd_bus_new(&bus);
    if (ret >= 0)
        ret = sd_bus_set_address(bus, address);
    if (ret >= 0)
        ret = sd_bus_set_bus_client(bus, true);
    if (ret >= 0)
        ret = sd_bus_start(bus);
    if (ret >= 0)
        ret = sd_bus_match_signal(bus, NULL, NULL, PATH, NULL, NULL, on_signal, NULL);
    if (ret < 0) {
        fprintf(stderr, "client: %s\n", strerror(-ret));
        sd_bus_close(bus);
        bus = sd_bus_unref(bus);
    }
    return bus;
}

static void client_close(Ctx *c)
{
    if (c->client) {
        sd_bus_close(c->client);
        sd_bus_unref(c->client);
        c->client = NULL;
    }
}

/* ----------------------------------------------------------------- the deck */

static RXTrack *want_track;

static bool analysis_done(Ctx *c)
{
    return !c->app.analysing_total && !rx_jobs_pending(c->app.jobs);
}

static bool deck_loaded(Ctx *c)
{
    return c->app.decks[0].track == want_track && !c->app.decks[0].loading_request;
}

static bool deck_playing_want(Ctx *c)
{
    RXDeckState st;
    rx_deck_state(c->app.player, 0, &st);
    return deck_loaded(c) && st.playing;
}

static RXDeckState deck(Ctx *c)
{
    RXDeckState st;
    rx_deck_state(c->app.player, 0, &st);
    return st;
}

static RXTrack *track_named(RXApp *app, const char *base)
{
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        if (!strcmp(rx_path_base(app->lib.tracks[i]->path), base))
            return app->lib.tracks[i];
    }
    return NULL;
}

/* Asked of the bus itself, which answers without the app's tick. */
static bool name_owned(Ctx *c)
{
    sd_bus_message *reply = NULL;
    int owned = 0;
    if (sd_bus_call_method(c->client, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "NameHasOwner", NULL, &reply, "s", NAME) >= 0)
        sd_bus_message_read(reply, "b", &owned);
    sd_bus_message_unref(reply);
    return owned;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: test_mpris FILE.wav SCRATCH_DIR\n");
        return 2;
    }
    const char *wav = argv[1];

    /* Whatever happens from here, the bus goes with the test. */
    struct sigaction sa = { .sa_handler = on_fatal };
    sigemptyset(&sa.sa_mask);
    static const int fatal[] = { SIGTERM, SIGINT, SIGHUP, SIGALRM, SIGABRT, SIGSEGV, SIGBUS,
                                 SIGFPE, SIGILL };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(fatal); i++)
        sigaction(fatal[i], &sa, NULL);
    atexit(bus_kill);
    alarm(240);

    remove_tree(argv[2]);
    if (rx_mkdir_p(argv[2]) < 0) {
        fprintf(stderr, "cannot make %s\n", argv[2]);
        return 2;
    }
    char *dir = rx_path_abs(argv[2]);
    char *music = dir ? rx_path_join(dir, "music") : NULL;
    char *run = dir ? rx_path_join(dir, "run") : NULL;
    char *root = dir ? rx_path_join(dir, "lib") : NULL;
    char *root2 = dir ? rx_path_join(dir, "lib2") : NULL;
    char *a_wav = music ? rx_path_join(music, "a.wav") : NULL;
    /* A name a URL must escape: spaces, '#' and UTF-8 (U+00E9). */
    char *b_wav = music ? rx_path_join(music, "b side #2 \xc3\xa9.wav") : NULL;
    if (!root || !root2 || !run || !a_wav || !b_wav || rx_mkdir_p(music) < 0 ||
        mkdir(run, 0700) < 0 || copy_file(wav, a_wav) < 0 || copy_file(wav, b_wav) < 0) {
        fprintf(stderr, "cannot set up %s\n", argv[2]);
        return 2;
    }

    bool skip;
    char *address = start_bus(dir, &skip);
    if (skip) {
        printf("no dbus-daemon or dbus-broker-launch: skipped\n");
        return 77;
    }
    if (!address) {
        fprintf(stderr, "the private bus did not start\n");
        return 1;
    }
    /* The app's connection goes to the private bus, and its cover files
     * into the scratch directory. */
    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
    setenv("XDG_RUNTIME_DIR", run, 1);

    /* Manual: nothing plays unless the test advances it. */
    RXPlayerOptions po;
    rx_player_options_default(&po);
    po.backend = RX_AUDIO_NULL;
    po.period = -480;

    static Ctx ctx;
    Ctx *c = &ctx;
    int ret = rx_app_create(&c->app, root, &po, NULL);
    CHECK(ret == 0, "rx_app_create: %s", strerror(-ret));
    if (ret < 0)
        return 1;
    ret = rx_app_add_path(&c->app, music);
    CHECK(ret == 2, "added %d", ret);
    CHECK(wait_until(c, analysis_done, 300), "the analysis did not finish");

    RXTrack *a = track_named(&c->app, "a.wav");
    RXTrack *b = track_named(&c->app, rx_path_base(b_wav));
    CHECK(a && b, "tracks a %p b %p", (void *)a, (void *)b);
    if (!a || !b)
        goto end;
    char a_id[64], b_id[64];
    snprintf(a_id, sizeof(a_id), "/org/rxbox/track/%llu", (unsigned long long)a->id);
    snprintf(b_id, sizeof(b_id), "/org/rxbox/track/%llu", (unsigned long long)b->id);

    /* A thumbnail for a, as the artwork job stores one: the cover file
     * must hold these bytes. */
    static const uint8_t jpeg[] = { 0xff, 0xd8, 0xff, 0xe0, 'r', 'x', 'b', 'o', 'x', 0xff, 0xd9 };
    ret = rx_library_artwork_save(&c->app.lib, a->id, jpeg, sizeof(jpeg), 240, 240);
    CHECK(ret >= 0, "artwork save: %s", strerror(-ret));

    /* The view: a then b, by path. */
    c->app.sort_column = RX_COL_PATH;
    c->app.sort_descending = false;
    rx_app_rebuild_rows(&c->app);
    CHECK(c->app.nb_rows == 2 && c->app.lib.tracks[c->app.rows[0]] == a &&
          c->app.lib.tracks[c->app.rows[1]] == b, "the view is not a, b");

    ret = rx_mpris_create(&c->app);
    CHECK(ret == 0 && c->app.mpris, "rx_mpris_create: %s", strerror(-ret));
    if (ret < 0)
        goto end;

    c->client = client_open(address);
    CHECK(c->client, "no client connection");
    if (!c->client)
        goto end;

    /* The name, the introspection, every property. */
    CHECK(name_owned(c), "nobody owns %s", NAME);

    sd_bus_message *reply = call(c, "org.freedesktop.DBus.Introspectable", "Introspect", NULL);
    const char *xml = NULL;
    CHECK(!*reply_error(reply) && sd_bus_message_read(reply, "s", &xml) > 0,
          "Introspect: %s", reply_error(reply));
    static const char *const root_props[] = {
        "CanQuit", "CanRaise", "HasTrackList", "Identity", "SupportedUriSchemes",
        "SupportedMimeTypes",
    };
    static const char *const player_props[] = {
        "PlaybackStatus", "LoopStatus", "Rate", "Shuffle", "Metadata", "Volume", "Position",
        "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause",
        "CanSeek", "CanControl",
    };
    if (xml) {
        CHECK(strstr(xml, "<interface name=\"" ROOT "\">") &&
              strstr(xml, "<interface name=\"" PLAYER "\">") &&
              strstr(xml, "<interface name=\"" PROPS "\">") &&
              strstr(xml, "<signal name=\"Seeked\">") && strstr(xml, "<method name=\"OpenUri\">"),
              "the introspection lacks an interface:\n%s", xml);
        for (size_t i = 0; i < RX_ARRAY_ELEMS(root_props); i++) {
            char want[96];
            snprintf(want, sizeof(want), "<property name=\"%s\"", root_props[i]);
            CHECK(strstr(xml, want), "the introspection lacks %s", root_props[i]);
        }
        for (size_t i = 0; i < RX_ARRAY_ELEMS(player_props); i++) {
            char want[96];
            snprintf(want, sizeof(want), "<property name=\"%s\"", player_props[i]);
            CHECK(strstr(xml, want), "the introspection lacks %s", player_props[i]);
        }
    }
    sd_bus_message_unref(reply);

    CHECK(get_all(c, ROOT, root_props, RX_ARRAY_ELEMS(root_props)), "GetAll root");
    CHECK(get_all(c, PLAYER, player_props, RX_ARRAY_ELEMS(player_props)), "GetAll player");
    CHECK(!strcmp(get_string(c, ROOT, "Identity"), "rxbox"), "Identity");
    CHECK(get_bool(c, ROOT, "CanQuit") == 1, "CanQuit");
    CHECK(get_bool(c, ROOT, "CanRaise") == 0, "CanRaise");
    CHECK(get_bool(c, ROOT, "HasTrackList") == 0, "HasTrackList");
    CHECK(get_strings_has(c, ROOT, "SupportedUriSchemes", "file"), "SupportedUriSchemes");
    CHECK(get_strings_has(c, ROOT, "SupportedMimeTypes", "audio/flac") &&
          get_strings_has(c, ROOT, "SupportedMimeTypes", "audio/mpeg"), "SupportedMimeTypes");
    CHECK(!strcmp(status(c), "Stopped"), "nothing loaded: %s", status(c));
    CHECK(!strcmp(get_string(c, PLAYER, "LoopStatus"), "None"), "LoopStatus");
    CHECK(get_bool(c, PLAYER, "Shuffle") == 0, "Shuffle");
    CHECK(get_double(c, PLAYER, "Rate") == 1.0 && get_double(c, PLAYER, "MinimumRate") == 1.0 &&
          get_double(c, PLAYER, "MaximumRate") == 1.0, "the rates");
    CHECK(get_double(c, PLAYER, "Volume") == 1.0, "Volume %f", get_double(c, PLAYER, "Volume"));
    CHECK(get_int64(c, PLAYER, "Position") == 0, "Position with nothing loaded");
    CHECK(get_bool(c, PLAYER, "CanPlay") == 0 && get_bool(c, PLAYER, "CanSeek") == 0 &&
          get_bool(c, PLAYER, "CanGoNext") == 0, "the Can* with nothing loaded");
    CHECK(get_bool(c, PLAYER, "CanControl") == 1, "CanControl");
    Meta meta;
    CHECK(get_meta(c, &meta) && !strcmp(meta.trackid, "/org/mpris/MediaPlayer2/TrackList/NoTrack")
          && meta.entries == 1, "no track: trackid %s, %d entries", meta.trackid, meta.entries);
    const char *e = method(c, PLAYER, "Frobnicate", NULL);
    CHECK(!strcmp(e, SD_BUS_ERROR_UNKNOWN_METHOD), "an unknown method: %s", e);

    /* A load: the metadata, Paused, the signal. */
    int mark = nb_seen;
    want_track = a;
    ret = rx_app_load_deck(&c->app, 0, a);
    CHECK(ret == 0, "load: %s", strerror(-ret));
    CHECK(wait_until(c, deck_loaded, 60), "the deck did not load");
    CHECK(get_meta(c, &meta), "Metadata");
    char *a_url = expect_url(a->path);
    char *a_art = rx_asprintf("%s/rxbox/mpris-%d/%llu.jpg", run, (int)getpid(),
                              (unsigned long long)a->id);
    char *a_art_url = expect_url(a_art);
    CHECK(!strcmp(meta.trackid, a_id), "trackid %s, want %s", meta.trackid, a_id);
    CHECK(meta.length == 12000000, "length %lld", (long long)meta.length);
    CHECK(!strcmp(meta.url, a_url), "url %s, want %s", meta.url, a_url);
    CHECK(!strcmp(meta.title, "a"), "title %s", meta.title);
    CHECK(!meta.artist[0], "artist %s for a file without one", meta.artist);
    CHECK(!strcmp(meta.art, a_art_url), "artUrl %s, want %s", meta.art, a_art_url);
    uint8_t *art = NULL;
    size_t art_size = 0;
    CHECK(rx_file_read(a_art, &art, &art_size) >= 0 && art_size == sizeof(jpeg) &&
          !memcmp(art, jpeg, sizeof(jpeg)), "the cover file %s", a_art);
    free(art);
    CHECK(!strcmp(status(c), "Paused"), "loaded: %s", status(c));
    CHECK(get_bool(c, PLAYER, "CanPlay") == 1 && get_bool(c, PLAYER, "CanPause") == 1 &&
          get_bool(c, PLAYER, "CanSeek") == 1, "the Can* with a track");
    CHECK(get_bool(c, PLAYER, "CanGoNext") == 1, "a, b: no next after a");
    CHECK(get_bool(c, PLAYER, "CanGoPrevious") == 0, "a, b: a previous before a");
    CHECK(wait_signal(c, mark, "PropertiesChanged", "Metadata", NULL) >= 0,
          "no PropertiesChanged with the Metadata");
    CHECK(wait_signal(c, mark, "PropertiesChanged", "PlaybackStatus", "Paused") >= 0,
          "no PropertiesChanged to Paused");
    CHECK(wait_signal(c, mark, "PropertiesChanged", "CanGoNext", NULL) >= 0,
          "no PropertiesChanged with CanGoNext");
    CHECK(wait_signal(c, mark, "PropertiesChanged", "CanPlay", NULL) >= 0,
          "no PropertiesChanged with CanPlay");

    /* Play. */
    mark = nb_seen;
    e = method(c, PLAYER, "Play", NULL);
    CHECK(!*e, "Play: %s", e);
    CHECK(deck(c).playing, "the deck does not play after Play");
    CHECK(!strcmp(status(c), "Playing"), "after Play: %s", status(c));
    CHECK(wait_signal(c, mark, "PropertiesChanged", "PlaybackStatus", "Playing") >= 0,
          "no PropertiesChanged to Playing");

    /* Playing on is no seek, nor any other change. */
    steps(c, 20);
    mark = nb_seen;
    steps(c, 50);
    CHECK(nb_seen == mark, "%d signals while playing on (%s)", nb_seen - mark,
          nb_seen > mark ? seen[mark].member : "");

    /* Seek, then a seek on the deck as the interface makes one. */
    mark = nb_seen;
    double before = deck(c).position;
    CHECK(!*method_x(c, "Seek", 3000000), "Seek");
    RXDeckState st = deck(c);
    CHECK(fabs(st.position - (before + 3.0)) < 1e-3, "Seek +3 s from %.3f: at %.3f", before,
          st.position);
    int64_t pos = get_int64(c, PLAYER, "Position");
    CHECK(llabs(pos - (int64_t)llround(st.position * 1e6)) <= 1,
          "Position %lld, the deck %.6f", (long long)pos, st.position);
    CHECK(wait_seeked(c, mark, st.position, 1e-3) >= 0, "no Seeked after Seek");
    mark = nb_seen;
    rx_ctl_seek(&c->app.decks[0].ctl, 1.0);
    CHECK(wait_seeked(c, mark, 1.0, 0.25) >= 0, "no Seeked after a seek on the deck");
    mark = nb_seen;
    CHECK(!*method_x(c, "Seek", -60000000), "Seek back past the start");
    CHECK(deck(c).position < 0.25, "Seek back past the start: at %.3f", deck(c).position);

    /* Pause, then Stop from paused and from playing: the cue point. */
    mark = nb_seen;
    CHECK(!*method(c, PLAYER, "Pause", NULL), "Pause");
    CHECK(!deck(c).playing, "the deck plays after Pause");
    CHECK(wait_signal(c, mark, "PropertiesChanged", "PlaybackStatus", "Paused") >= 0,
          "no PropertiesChanged to Paused");
    double cue = c->app.decks[0].ctl.cue_point;
    printf("cue point %.3f\n", cue);
    rx_ctl_seek(&c->app.decks[0].ctl, 6.0);
    steps(c, 3);
    CHECK(!*method(c, PLAYER, "Stop", NULL), "Stop");
    st = deck(c);
    CHECK(!st.playing && fabs(st.position - RX_MAX(cue, 0.0)) < 1e-3,
          "Stop from paused: at %.3f, playing %d", st.position, st.playing);
    CHECK(!*method(c, PLAYER, "PlayPause", NULL), "PlayPause");
    CHECK(deck(c).playing, "PlayPause did not play");
    steps(c, 30);
    CHECK(!*method(c, PLAYER, "Stop", NULL), "Stop");
    st = deck(c);
    CHECK(!st.playing && fabs(st.position - RX_MAX(cue, 0.0)) < 1e-3,
          "Stop from playing: at %.3f, playing %d", st.position, st.playing);
    CHECK(!strcmp(status(c), "Paused"), "after Stop: %s", status(c));

    /* SetPosition, and the calls it ignores. */
    mark = nb_seen;
    CHECK(!*set_position(c, a_id, 5000000), "SetPosition");
    CHECK(fabs(deck(c).position - 5.0) < 1e-3, "SetPosition 5 s: at %.3f", deck(c).position);
    CHECK(wait_seeked(c, mark, 5.0, 1e-3) >= 0, "no Seeked after SetPosition");
    CHECK(!*set_position(c, "/org/rxbox/track/999999", 2000000), "SetPosition, a stale id");
    CHECK(!*set_position(c, a_id, -1), "SetPosition before the start");
    CHECK(!*set_position(c, a_id, 13000000), "SetPosition past the end");
    CHECK(fabs(deck(c).position - 5.0) < 1e-3, "an ignored SetPosition moved the deck to %.3f",
          deck(c).position);

    /* Next from a playing deck: b loads and plays, and the status stays
     * Playing through the swap. Nothing follows b. */
    CHECK(!*method(c, PLAYER, "Play", NULL), "Play");
    steps(c, 5);
    mark = nb_seen;
    want_track = b;
    CHECK(!*method(c, PLAYER, "Next", NULL), "Next");
    CHECK(wait_until(c, deck_playing_want, 60), "Next: the deck has %p, want b playing",
          (void *)c->app.decks[0].track);
    char *b_url = expect_url(b->path);
    CHECK(get_meta(c, &meta) && !strcmp(meta.trackid, b_id) && !strcmp(meta.url, b_url),
          "after Next: trackid %s url %s, want %s %s", meta.trackid, meta.url, b_id, b_url);
    CHECK(rx_str_ends(meta.url, "/b%20side%20%232%20%C3%A9.wav"), "url %s", meta.url);
    CHECK(!strcmp(meta.title, "b side #2 \xc3\xa9"), "title %s", meta.title);
    CHECK(!meta.art[0], "b has a cover: %s", meta.art);
    CHECK(!strcmp(status(c), "Playing"), "after Next: %s", status(c));
    CHECK(get_bool(c, PLAYER, "CanGoNext") == 0 && get_bool(c, PLAYER, "CanGoPrevious") == 1,
          "after Next: CanGoNext %d CanGoPrevious %d", get_bool(c, PLAYER, "CanGoNext"),
          get_bool(c, PLAYER, "CanGoPrevious"));
    CHECK(wait_signal(c, mark, "PropertiesChanged", "Metadata", NULL) >= 0 &&
          wait_signal(c, mark, "PropertiesChanged", "CanGoPrevious", NULL) >= 0,
          "no PropertiesChanged for the new track");
    CHECK(find_signal(mark, "PropertiesChanged", "PlaybackStatus", "Paused") < 0,
          "Paused while Next swapped the tracks");
    CHECK(!*method(c, PLAYER, "Next", NULL), "Next past the last row");
    steps(c, 10);
    CHECK(c->app.decks[0].track == b && !c->app.decks[0].loading_request && deck(c).playing,
          "Next past the last row changed the deck");

    /* Previous from a paused deck: a loads and waits. */
    CHECK(!*method(c, PLAYER, "Pause", NULL), "Pause");
    want_track = a;
    CHECK(!*method(c, PLAYER, "Previous", NULL), "Previous");
    CHECK(wait_until(c, deck_loaded, 60), "Previous: the deck has %p, want a",
          (void *)c->app.decks[0].track);
    steps(c, 10);
    CHECK(!deck(c).playing && !strcmp(status(c), "Paused"), "Previous played a paused deck");

    /* OpenUri: b by its URL, playing; a file not in the library and
     * another scheme are errors. */
    want_track = b;
    e = method(c, PLAYER, "OpenUri", "s", b_url);
    CHECK(!*e, "OpenUri %s: %s", b_url, e);
    CHECK(wait_until(c, deck_playing_want, 60), "OpenUri: the deck has %p, want b playing",
          (void *)c->app.decks[0].track);
    char *gone = rx_asprintf("file://%s/nothing.wav", music);
    e = method(c, PLAYER, "OpenUri", "s", gone);
    CHECK(!strcmp(e, SD_BUS_ERROR_FILE_NOT_FOUND), "OpenUri of a file not in the library: %s", e);
    free(gone);
    e = method(c, PLAYER, "OpenUri", "s", "http://example.com/a.mp3");
    CHECK(!strcmp(e, SD_BUS_ERROR_NOT_SUPPORTED), "OpenUri http: %s", e);
    CHECK(c->app.decks[0].track == b && deck(c).playing, "a refused OpenUri changed the deck");

    /* The volume; the read-only properties. */
    mark = nb_seen;
    CHECK(!*method(c, PROPS, "Set", "ssv", PLAYER, "Volume", "d", 0.5), "Set Volume");
    CHECK(get_double(c, PLAYER, "Volume") == 0.5, "Volume %f", get_double(c, PLAYER, "Volume"));
    CHECK(wait_signal(c, mark, "PropertiesChanged", "Volume", NULL) >= 0,
          "no PropertiesChanged for the volume");
    CHECK(!*method(c, PROPS, "Set", "ssv", PLAYER, "Volume", "d", -1.0), "Set Volume -1");
    CHECK(get_double(c, PLAYER, "Volume") == 0.0, "Volume -1 is %f",
          get_double(c, PLAYER, "Volume"));
    CHECK(!strcmp(method(c, PROPS, "Set", "ssv", PLAYER, "LoopStatus", "s", "Track"),
                  SD_BUS_ERROR_PROPERTY_READ_ONLY), "Set LoopStatus");
    CHECK(!strcmp(method(c, PROPS, "Set", "ssv", PLAYER, "Shuffle", "b", true),
                  SD_BUS_ERROR_PROPERTY_READ_ONLY), "Set Shuffle");
    CHECK(!strcmp(method(c, PROPS, "Set", "ssv", ROOT, "Identity", "s", "Track"),
                  SD_BUS_ERROR_PROPERTY_READ_ONLY), "Set Identity");
    CHECK(!strcmp(method(c, PROPS, "Set", "ssv", PLAYER, "Rate", "d", 2.0),
                  SD_BUS_ERROR_INVALID_ARGS), "Set Rate 2");

    /* Position never went out in a PropertiesChanged. */
    for (int i = 0; i < nb_seen; i++)
        CHECK(!strstr(seen[i].changed, " Position "), "Position in a PropertiesChanged");
    printf("%d signals: %d PropertiesChanged, %d Seeked\n", nb_seen,
           count_seen(0, "PropertiesChanged"), count_seen(0, "Seeked"));

    /* Raise does nothing, Quit asks the loop to end. */
    CHECK(!*method(c, ROOT, "Raise", NULL), "Raise");
    CHECK(!c->app.quit, "quit before Quit");
    CHECK(!*method(c, ROOT, "Quit", NULL), "Quit");
    CHECK(c->app.quit, "Quit did not quit");

    /* A second app finds the name taken, and one without a bus carries
     * on: neither is on the bus. Without a bus is without an address and
     * without a socket in $XDG_RUNTIME_DIR (when its path fits a socket's:
     * sd-bus refuses a longer one as E2BIG), then without either
     * variable. */
    RXApp app2;
    ret = rx_app_create(&app2, root2, &po, NULL);
    CHECK(ret == 0, "the second app: %s", strerror(-ret));
    if (ret == 0) {
        ret = rx_mpris_create(&app2);
        CHECK(ret == RX_ERROR(EEXIST) && !app2.mpris, "the name was not refused: %s",
              strerror(-ret));
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        if (strlen(run) + sizeof("/bus") <= sizeof(((struct sockaddr_un *)0)->sun_path)) {
            ret = rx_mpris_create(&app2);
            CHECK(ret == RX_ERROR(ENOENT) && !app2.mpris, "no bus in %s: %s", run,
                  strerror(-ret));
        }
        unsetenv("XDG_RUNTIME_DIR");
        ret = rx_mpris_create(&app2);
        CHECK(ret == RX_ERROR(ENOENT) && !app2.mpris, "no bus: %s", strerror(-ret));
        setenv("XDG_RUNTIME_DIR", run, 1);
        setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
        rx_mpris_tick(&app2);
        rx_app_destroy(&app2);
    }

    /* Destroy: the name and the cover go. */
    rx_mpris_destroy(&c->app);
    CHECK(!c->app.mpris, "still on the bus");
    CHECK(!rx_file_exists(a_art), "%s is still there", a_art);
    char *art_dir = rx_asprintf("%s/rxbox/mpris-%d", run, (int)getpid());
    CHECK(art_dir && !rx_dir_exists(art_dir), "%s is still there", art_dir);
    free(art_dir);
    int64_t t0 = rx_time_us();
    while (name_owned(c) && rx_time_us() - t0 < 2000000)
        usleep(10000);
    CHECK(!name_owned(c), "%s still has an owner", NAME);

    /* A bus that goes away ends MPRIS, not the app. */
    ret = rx_mpris_create(&c->app);
    CHECK(ret == 0, "on the bus again: %s", strerror(-ret));
    client_close(c);
    bus_kill();
    t0 = rx_time_us();
    while (c->app.mpris && rx_time_us() - t0 < 5000000)
        step(c);
    CHECK(!c->app.mpris, "MPRIS outlived its bus");

    free(a_url);
    free(b_url);
    free(a_art);
    free(a_art_url);

end:
    client_close(c);
    rx_app_destroy(&c->app);
    bus_kill();
    free(address);
    free(dir);
    free(music);
    free(run);
    free(root);
    free(root2);
    free(a_wav);
    free(b_wav);
    if (failures)
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
