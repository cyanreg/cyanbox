/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MPRIS 2 for the deck (rxbox/mpris.h), on libdbus' low-level API: the
 * object /org/mpris/MediaPlayer2 under the name
 * org.mpris.MediaPlayer2.rxbox, with the root and Player interfaces of
 * freedesktop.org's Media Player Remote Interfacing Specification 2.2
 * (specifications.freedesktop.org/mpris-spec/latest), Properties and
 * Introspectable. There is no TrackList: the browser's current view stands
 * for one, through Next and Previous.
 *
 * Decisions:
 *  - A private connection, used on the owner thread only, so libdbus never
 *    calls into the app from a thread of its own. rx_mpris_tick() reads,
 *    dispatches and writes with a timeout of 0; a call is answered from
 *    inside the dispatch and acts on the deck as the interface's buttons
 *    do. Connecting and taking the name, at startup, are the only waits.
 *  - What the clients see is worked out once per tick and compared with
 *    what they were last told: PropertiesChanged carries the properties
 *    that differ, with their values. Never Position: the specification has
 *    clients read it, and Seeked tells them when it moved other than by
 *    playing, the way track_plays in app.c tells a seek from playing
 *    (further ahead than the time since the last tick allows, or back),
 *    and after a seek asked for here. A loop wrapping is such a jump too;
 *    a scrub is one, when it ends.
 *  - Rate, MinimumRate and MaximumRate are 1: the tempo slider is the
 *    deck's own, not a client's. With the slider off 0 a client's
 *    extrapolated position drifts until it reads Position again.
 *  - Next and Previous load the row after or before the deck's track in
 *    the current view (app->rows, in the table's order, past missing
 *    files, without wrapping). rx_app_load_deck() refuses a playing deck,
 *    so a playing deck pauses for the swap and the tick starts the new
 *    track once its decode is in, as the interface does for a load over a
 *    playing track; the status stays Playing meanwhile.
 *  - Volume is the player's master volume. The player has no getter and
 *    nothing else sets it (the interface has no volume control), so the
 *    value set here, from the player's 1, is the player's.
 *  - The cover is the library's thumbnail, written once per track to
 *    $XDG_RUNTIME_DIR/rxbox/mpris-PID/ID.jpg (a directory per process, so
 *    another rxbox or a test's bus never shares a file) and removed at
 *    exit. A track the library has no thumbnail of yet (the deck's artwork
 *    job makes it) is looked at again when the library changes.
 *  - The name is taken with DO_NOT_QUEUE: a second rxbox stays off the bus
 *    rather than wait for the first to quit.
 *  - libdbus aborts the process on a string that is not UTF-8, so a tag
 *    goes out only once dbus_validate_utf8() passed it, and the file's
 *    path only percent-encoded, as ASCII. */

#include "rxbox/mpris.h"

#include <math.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dbus/dbus.h>

#define BUS_NAME     "org.mpris.MediaPlayer2.rxbox"
#define OBJECT_PATH  "/org/mpris/MediaPlayer2"
#define IFACE_ROOT   "org.mpris.MediaPlayer2"
#define IFACE_PLAYER "org.mpris.MediaPlayer2.Player"
/* A track's mpris:trackid is TRACK_PATH and its library id. */
#define TRACK_PATH   "/org/rxbox/track/"
/* The specification's track id for no track. */
#define NO_TRACK     "/org/mpris/MediaPlayer2/TrackList/NoTrack"
/* The deck MPRIS shows (RX_DECKS is 1). */
#define DECK         0
/* Calls answered per tick at most: a client flooding the bus waits for
 * the next ticks, the frame does not. */
#define CALLS_PER_TICK 64
/* The seek test (jumped): seconds a playing deck may run ahead of what the
 * time since the last tick allows (track_plays' 0.05), step back, or move
 * at all while paused. The playhead never steps back by itself. */
#define PLAY_SLACK   0.05
#define BACK_SLACK   0.02
#define STILL_SLACK  0.001

enum Status {
    STATUS_STOPPED = 0,
    STATUS_PAUSED,
    STATUS_PLAYING,
};

static const char *const status_names[] = { "Stopped", "Paused", "Playing" };

/* The properties: the root interface's, then the Player's. Fullscreen,
 * CanSetFullscreen and DesktopEntry are optional and left out. */
enum Prop {
    P_CAN_QUIT = 0, P_CAN_RAISE, P_HAS_TRACK_LIST, P_IDENTITY, P_URI_SCHEMES, P_MIME_TYPES,
    P_STATUS, P_LOOP, P_RATE, P_SHUFFLE, P_METADATA, P_VOLUME, P_POSITION, P_MIN_RATE,
    P_MAX_RATE, P_CAN_NEXT, P_CAN_PREV, P_CAN_PLAY, P_CAN_PAUSE, P_CAN_SEEK, P_CAN_CONTROL,
    P_COUNT,
};

static const struct {
    const char *iface;
    const char *name;
} props[P_COUNT] = {
    [P_CAN_QUIT]       = { IFACE_ROOT,   "CanQuit" },
    [P_CAN_RAISE]      = { IFACE_ROOT,   "CanRaise" },
    [P_HAS_TRACK_LIST] = { IFACE_ROOT,   "HasTrackList" },
    [P_IDENTITY]       = { IFACE_ROOT,   "Identity" },
    [P_URI_SCHEMES]    = { IFACE_ROOT,   "SupportedUriSchemes" },
    [P_MIME_TYPES]     = { IFACE_ROOT,   "SupportedMimeTypes" },
    [P_STATUS]         = { IFACE_PLAYER, "PlaybackStatus" },
    [P_LOOP]           = { IFACE_PLAYER, "LoopStatus" },
    [P_RATE]           = { IFACE_PLAYER, "Rate" },
    [P_SHUFFLE]        = { IFACE_PLAYER, "Shuffle" },
    [P_METADATA]       = { IFACE_PLAYER, "Metadata" },
    [P_VOLUME]         = { IFACE_PLAYER, "Volume" },
    [P_POSITION]       = { IFACE_PLAYER, "Position" },
    [P_MIN_RATE]       = { IFACE_PLAYER, "MinimumRate" },
    [P_MAX_RATE]       = { IFACE_PLAYER, "MaximumRate" },
    [P_CAN_NEXT]       = { IFACE_PLAYER, "CanGoNext" },
    [P_CAN_PREV]       = { IFACE_PLAYER, "CanGoPrevious" },
    [P_CAN_PLAY]       = { IFACE_PLAYER, "CanPlay" },
    [P_CAN_PAUSE]      = { IFACE_PLAYER, "CanPause" },
    [P_CAN_SEEK]       = { IFACE_PLAYER, "CanSeek" },
    [P_CAN_CONTROL]    = { IFACE_PLAYER, "CanControl" },
};

static const char *const uri_schemes[] = { "file" };

/* What the library adds (rx_is_audio_path: mp3 m4a mp4 aac wav aif aiff
 * flac ogg opus), under the names and the older x- names desktops use. */
static const char *const mime_types[] = {
    "audio/mpeg", "audio/mp4", "audio/x-m4a", "audio/aac", "audio/wav", "audio/x-wav",
    "audio/aiff", "audio/x-aiff", "audio/flac", "audio/x-flac", "audio/ogg", "audio/opus",
};

static const char *const player_methods[] = {
    "Next", "Previous", "Pause", "PlayPause", "Stop", "Play", "Seek", "SetPosition", "OpenUri",
};

static const char introspection[] =
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE
    "<node>\n"
    "  <interface name=\"" DBUS_INTERFACE_INTROSPECTABLE "\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"" DBUS_INTERFACE_PROPERTIES "\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <signal name=\"PropertiesChanged\">\n"
    "      <arg name=\"interface_name\" type=\"s\"/>\n"
    "      <arg name=\"changed_properties\" type=\"a{sv}\"/>\n"
    "      <arg name=\"invalidated_properties\" type=\"as\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"" IFACE_ROOT "\">\n"
    "    <method name=\"Raise\"/>\n"
    "    <method name=\"Quit\"/>\n"
    "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"" IFACE_PLAYER "\">\n"
    "    <method name=\"Next\"/>\n"
    "    <method name=\"Previous\"/>\n"
    "    <method name=\"Pause\"/>\n"
    "    <method name=\"PlayPause\"/>\n"
    "    <method name=\"Stop\"/>\n"
    "    <method name=\"Play\"/>\n"
    "    <method name=\"Seek\">\n"
    "      <arg name=\"Offset\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"SetPosition\">\n"
    "      <arg name=\"TrackId\" type=\"o\" direction=\"in\"/>\n"
    "      <arg name=\"Position\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"OpenUri\">\n"
    "      <arg name=\"Uri\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <signal name=\"Seeked\">\n"
    "      <arg name=\"Position\" type=\"x\"/>\n"
    "    </signal>\n"
    "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"LoopStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Rate\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Shuffle\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
    "    <property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Position\" type=\"x\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\""
    " value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanControl\" type=\"b\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\""
    " value=\"const\"/>\n"
    "    </property>\n"
    "  </interface>\n"
    "</node>\n";

/* The deck as the clients see it now. */
typedef struct View {
    RXDeckState st;
    RXTrack *track;             /* the player's track; NULL: Stopped */
    int      status;
    bool     can_next, can_prev;
} View;

/* What the clients were last told, of what PropertiesChanged carries. */
typedef struct Shown {
    int      status;
    uint64_t meta;              /* the metadata's hash (meta_hash), 0 for no track */
    double   volume;
    bool     can_next, can_prev;
    bool     loaded;            /* CanPlay, CanPause, CanSeek */
} Shown;

typedef struct RXMpris {
    DBusConnection *conn;
    double   volume;            /* the player's master volume, 0..1 */
    Shown    shown;
    bool     seeked;            /* a seek asked for here: Seeked at the end of the tick */

    /* A track a call loaded, played once its decode is in when want_play. */
    uint64_t want_track;
    bool     want_play;

    /* The metadata of meta_track as of the library's meta_generation. */
    uint64_t meta_track;
    unsigned meta_generation;
    uint64_t meta_hash;
    char    *art_url;           /* meta_track's cover, NULL none */
    char    *art_dir;           /* NULL without $XDG_RUNTIME_DIR */
    RXArray  art_files;         /* uint64_t: the track ids with a file in art_dir */

    /* The playhead at the last tick (jumped). */
    uint64_t pos_track;
    double   pos_last;
    int64_t  pos_us;
    bool     pos_playing, pos_scrubbing;
} RXMpris;

/* ------------------------------------------------------------------ helpers */

/* Whether `s` may go out: not empty, and UTF-8 (libdbus aborts otherwise). */
static bool sendable(const char *s)
{
    return s && s[0] && dbus_validate_utf8(s, NULL);
}

static bool unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}

/* file:// and the absolute `path`, every byte but the unreserved ones
 * (RFC 3986, 2.3) and the slashes percent-encoded: spaces, '#', '%', '?'
 * and UTF-8 survive, and a path that is not UTF-8 still makes ASCII.
 * Allocating. */
static char *file_url(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    char *url = malloc(sizeof("file://") + 3 * strlen(path));
    if (!url)
        return NULL;
    char *o = stpcpy(url, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (unreserved(*p)) {
            *o++ = (char)*p;
        } else {
            *o++ = '%';
            *o++ = hex[*p >> 4];
            *o++ = hex[*p & 15];
        }
    }
    *o = '\0';
    return url;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* The path of a local file URL, file:///PATH or file://localhost/PATH,
 * its escapes decoded, up to a query or a fragment. NULL for another
 * scheme or host, a broken escape or an escaped NUL. Allocating. */
static char *url_path(const char *url)
{
    const char *p = NULL;
    if (!strncasecmp(url, "file:///", 8))
        p = url + 7;
    else if (!strncasecmp(url, "file://localhost/", 17))
        p = url + 16;
    if (!p)
        return NULL;

    char *path = malloc(strlen(p) + 1);
    if (!path)
        return NULL;
    char *o = path;
    for (; *p && *p != '?' && *p != '#'; p++) {
        if (*p != '%') {
            *o++ = *p;
            continue;
        }
        int hi = hex_digit(p[1]);
        int lo = hi >= 0 ? hex_digit(p[2]) : -1;
        if (lo < 0 || (hi | lo) == 0) {
            free(path);
            return NULL;
        }
        *o++ = (char)(hi << 4 | lo);
        p += 2;
    }
    *o = '\0';
    return path;
}

/* mpris:trackid of a track, or of none. */
static void track_path(const RXTrack *t, char *buf, size_t size)
{
    if (t)
        snprintf(buf, size, TRACK_PATH "%llu", (unsigned long long)t->id);
    else
        snprintf(buf, size, "%s", NO_TRACK);
}

static dbus_int64_t usec(double seconds)
{
    return isfinite(seconds) ? (dbus_int64_t)llround(seconds * 1e6) : 0;
}

/* The row `dir` (1 or -1) away from `t` in the current view, past missing
 * files; NULL when `t` is not in the view or no such row follows. */
static RXTrack *neighbour(RXApp *app, const RXTrack *t, int dir)
{
    if (!t)
        return NULL;
    /* rx_app_tick() built them for this generation just before; they are
     * built again only when something changed in between, as next_in_view
     * (app.c) does. */
    if (app->rows_dirty || app->rows_generation != app->lib.generation || !app->rows)
        rx_app_rebuild_rows(app);

    int idx = rx_library_index_of(&app->lib, t);
    if (idx < 0)
        return NULL;
    int at = -1;
    for (int i = 0; i < app->nb_rows && at < 0; i++) {
        if (app->rows[i] == (uint32_t)idx)
            at = i;
    }
    if (at < 0)
        return NULL;
    for (int i = at + dir; i >= 0 && i < app->nb_rows; i += dir) {
        RXTrack *n = app->lib.tracks[app->rows[i]];
        if (!n->missing)
            return n;
    }
    return NULL;
}

/* A track a call loaded is still being decoded. */
static bool swapping(const RXApp *app, const RXMpris *m)
{
    return m->want_track && app->decks[DECK].loading_request;
}

static void view_now(RXApp *app, const RXMpris *m, View *v)
{
    *v = (View) { .status = STATUS_STOPPED };
    rx_deck_state(app->player, DECK, &v->st);
    RXTrack *t = app->decks[DECK].track;
    if (!v->st.loaded || !t)
        return;

    v->track = t;
    v->status = v->st.playing ? STATUS_PLAYING : STATUS_PAUSED;
    /* The deck paused only to take the track a call is loading over it. */
    if (swapping(app, m) && m->want_play)
        v->status = STATUS_PLAYING;
    v->can_next = neighbour(app, t, 1) != NULL;
    v->can_prev = neighbour(app, t, -1) != NULL;
}

/* ---------------------------------------------------------------- the cover */

static char *cover_path(const RXMpris *m, uint64_t id)
{
    return rx_asprintf("%s/%llu.jpg", m->art_dir, (unsigned long long)id);
}

static bool cover_written(const RXMpris *m, uint64_t id)
{
    for (size_t i = 0; i < m->art_files.nb; i++) {
        if (*(const uint64_t *)rx_array_at(&m->art_files, i) == id)
            return true;
    }
    return false;
}

/* The URL of the track's cover, the library's thumbnail, written the
 * first time it is asked for. NULL when there is none (yet). */
static char *cover_url(RXApp *app, RXMpris *m, const RXTrack *t)
{
    if (!m->art_dir)
        return NULL;
    char *path = cover_path(m, t->id);
    if (!path)
        return NULL;

    int ret = 0;
    if (!cover_written(m, t->id)) {
        uint8_t *jpeg = NULL;
        size_t size = 0;
        ret = rx_library_artwork_load(&app->lib, t->id, &jpeg, &size, NULL, NULL);
        if (ret >= 0)
            ret = rx_mkdir_p(m->art_dir);
        if (ret >= 0)
            ret = rx_array_push(&m->art_files, &t->id) ? 0 : RX_ERROR(ENOMEM);
        if (ret >= 0) {
            ret = rx_file_write(path, jpeg, size);
            if (ret < 0) {
                unlink(path);
                rx_array_remove(&m->art_files, m->art_files.nb - 1);
            }
        }
        if (ret < 0 && ret != RX_ERROR(ENOENT))
            rx_debug("mpris: no cover file for track %llu: %s", (unsigned long long)t->id,
                     strerror(-ret));
        free(jpeg);
    }

    char *url = ret >= 0 ? file_url(path) : NULL;
    free(path);
    return url;
}

static void covers_remove(RXMpris *m)
{
    for (size_t i = 0; i < m->art_files.nb; i++) {
        char *path = cover_path(m, *(const uint64_t *)rx_array_at(&m->art_files, i));
        if (path)
            unlink(path);
        free(path);
    }
    if (!m->art_files.nb)
        return;
    /* Our directory, then rxbox/ when nothing else is in it. */
    rmdir(m->art_dir);
    char *slash = strrchr(m->art_dir, '/');
    if (slash) {
        *slash = '\0';
        rmdir(m->art_dir);
        *slash = '/';
    }
}

/* -------------------------------------------------------------- metadata */

/* FNV-1a, 64 bits. */
static uint64_t fnv(uint64_t h, const void *data, size_t size)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t fnv_str(uint64_t h, const char *s)
{
    return s ? fnv(h, s, strlen(s) + 1) : fnv(h, "", 1);
}

/* Everything add_metadata() sends of a track: when this changes, so did
 * the metadata. */
static uint64_t meta_hash(const RXTrack *t, const char *art_url)
{
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, &t->id, sizeof(t->id));
    h = fnv(h, &t->duration, sizeof(t->duration));
    h = fnv(h, &t->meta.rating, sizeof(t->meta.rating));
    h = fnv_str(h, t->path);
    h = fnv_str(h, t->meta.title);
    h = fnv_str(h, t->meta.artist);
    h = fnv_str(h, t->meta.album);
    h = fnv_str(h, t->meta.genre);
    return fnv_str(h, art_url);
}

/* Brings the metadata up to the deck's track `t`: when the track changed,
 * or the library did (an edit saved, a thumbnail made), the cover and the
 * hash are worked out again. */
static void meta_refresh(RXApp *app, RXMpris *m, const RXTrack *t)
{
    uint64_t id = t ? t->id : 0;
    if (id == m->meta_track && app->lib.generation == m->meta_generation)
        return;
    if (id != m->meta_track) {
        free(m->art_url);
        m->art_url = NULL;
    }
    if (t && !m->art_url)
        m->art_url = cover_url(app, m, t);
    m->meta_track = id;
    m->meta_generation = app->lib.generation;
    m->meta_hash = t ? meta_hash(t, m->art_url) : 0;
}

static Shown shown_now(RXApp *app, RXMpris *m, const View *v)
{
    meta_refresh(app, m, v->track);
    return (Shown) {
        .status   = v->status,
        .meta     = m->meta_hash,
        .volume   = m->volume,
        .can_next = v->can_next,
        .can_prev = v->can_prev,
        .loaded   = v->track != NULL,
    };
}

/* ---------------------------------------------------------------- messages */

/* The appenders return false when libdbus is out of memory; they abandon
 * what they opened, and the caller drops the message. */

static bool add_variant(DBusMessageIter *it, int type, const char *sig, const void *value)
{
    DBusMessageIter v = DBUS_MESSAGE_ITER_INIT_CLOSED;
    bool ok = dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &v) &&
              dbus_message_iter_append_basic(&v, type, value) &&
              dbus_message_iter_close_container(it, &v);
    if (!ok)
        dbus_message_iter_abandon_container_if_open(it, &v);
    return ok;
}

static bool add_bool(DBusMessageIter *it, bool b)
{
    dbus_bool_t v = b;
    return add_variant(it, DBUS_TYPE_BOOLEAN, DBUS_TYPE_BOOLEAN_AS_STRING, &v);
}

static bool add_double(DBusMessageIter *it, double d)
{
    return add_variant(it, DBUS_TYPE_DOUBLE, DBUS_TYPE_DOUBLE_AS_STRING, &d);
}

static bool add_int64(DBusMessageIter *it, dbus_int64_t x)
{
    return add_variant(it, DBUS_TYPE_INT64, DBUS_TYPE_INT64_AS_STRING, &x);
}

static bool add_string(DBusMessageIter *it, const char *s)
{
    return add_variant(it, DBUS_TYPE_STRING, DBUS_TYPE_STRING_AS_STRING, &s);
}

static bool add_strings(DBusMessageIter *it, const char *const *s, size_t n)
{
    DBusMessageIter v = DBUS_MESSAGE_ITER_INIT_CLOSED, a = DBUS_MESSAGE_ITER_INIT_CLOSED;
    bool ok = dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "as", &v) &&
              dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING,
                                               &a);
    for (size_t i = 0; ok && i < n; i++)
        ok = dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &s[i]);
    ok = ok && dbus_message_iter_close_container(&v, &a) &&
         dbus_message_iter_close_container(it, &v);
    if (!ok) {
        dbus_message_iter_abandon_container_if_open(&v, &a);
        dbus_message_iter_abandon_container_if_open(it, &v);
    }
    return ok;
}

/* One {sv} entry of a dictionary holding a basic value. */
static bool add_entry(DBusMessageIter *dict, const char *key, int type, const char *sig,
                      const void *value)
{
    DBusMessageIter e = DBUS_MESSAGE_ITER_INIT_CLOSED;
    bool ok = dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e) &&
              dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key) &&
              add_variant(&e, type, sig, value) &&
              dbus_message_iter_close_container(dict, &e);
    if (!ok)
        dbus_message_iter_abandon_container_if_open(dict, &e);
    return ok;
}

/* A text field of the metadata; left out when empty or not UTF-8. */
static bool add_meta_string(DBusMessageIter *dict, const char *key, const char *s)
{
    if (!sendable(s))
        return true;
    return add_entry(dict, key, DBUS_TYPE_STRING, DBUS_TYPE_STRING_AS_STRING, &s);
}

/* xesam's lists (artist, genre), holding the one value a track has. */
static bool add_meta_list(DBusMessageIter *dict, const char *key, const char *s)
{
    if (!sendable(s))
        return true;
    DBusMessageIter e = DBUS_MESSAGE_ITER_INIT_CLOSED;
    bool ok = dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e) &&
              dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key) &&
              add_strings(&e, &s, 1) &&
              dbus_message_iter_close_container(dict, &e);
    if (!ok)
        dbus_message_iter_abandon_container_if_open(dict, &e);
    return ok;
}

/* Metadata, a{sv} in a variant (the specification's metadata page): the
 * track id always, the rest for a track. The title falls back to the file
 * name, which the library's title already is for a file without one. */
static bool add_metadata(RXApp *app, RXMpris *m, const RXTrack *t, DBusMessageIter *it)
{
    char path[64];
    track_path(t, path, sizeof(path));
    const char *object = path;

    DBusMessageIter v = DBUS_MESSAGE_ITER_INIT_CLOSED, d = DBUS_MESSAGE_ITER_INIT_CLOSED;
    bool ok = dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a{sv}", &v) &&
              dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "{sv}", &d) &&
              add_entry(&d, "mpris:trackid", DBUS_TYPE_OBJECT_PATH,
                        DBUS_TYPE_OBJECT_PATH_AS_STRING, &object);
    if (ok && t) {
        meta_refresh(app, m, t);
        dbus_int64_t length = usec(t->duration);
        double rating = t->meta.rating / 5.0;
        const char *title = t->meta.title && t->meta.title[0] ? t->meta.title
                                                              : rx_path_base(t->path);
        char *url = file_url(t->path);
        ok = url &&
             (length <= 0 || add_entry(&d, "mpris:length", DBUS_TYPE_INT64,
                                       DBUS_TYPE_INT64_AS_STRING, &length)) &&
             add_meta_string(&d, "mpris:artUrl", m->art_url) &&
             add_meta_string(&d, "xesam:url", url) &&
             add_meta_string(&d, "xesam:title", title) &&
             add_meta_list(&d, "xesam:artist", t->meta.artist) &&
             add_meta_string(&d, "xesam:album", t->meta.album) &&
             add_meta_list(&d, "xesam:genre", t->meta.genre) &&
             (t->meta.rating <= 0 || add_entry(&d, "xesam:userRating", DBUS_TYPE_DOUBLE,
                                               DBUS_TYPE_DOUBLE_AS_STRING, &rating));
        free(url);
    }
    ok = ok && dbus_message_iter_close_container(&v, &d) &&
         dbus_message_iter_close_container(it, &v);
    if (!ok) {
        dbus_message_iter_abandon_container_if_open(&v, &d);
        dbus_message_iter_abandon_container_if_open(it, &v);
    }
    return ok;
}

/* A property's value, in a variant. */
static bool add_prop(RXApp *app, RXMpris *m, const View *v, DBusMessageIter *it, int id)
{
    switch (id) {
    case P_CAN_QUIT:
    case P_CAN_CONTROL:
        return add_bool(it, true);
    case P_CAN_RAISE:
    case P_HAS_TRACK_LIST:
    case P_SHUFFLE:
        return add_bool(it, false);
    case P_IDENTITY:
        return add_string(it, "rxbox");
    case P_URI_SCHEMES:
        return add_strings(it, uri_schemes, RX_ARRAY_ELEMS(uri_schemes));
    case P_MIME_TYPES:
        return add_strings(it, mime_types, RX_ARRAY_ELEMS(mime_types));
    case P_STATUS:
        return add_string(it, status_names[v->status]);
    case P_LOOP:
        return add_string(it, "None");
    case P_RATE:
    case P_MIN_RATE:
    case P_MAX_RATE:
        return add_double(it, 1.0);
    case P_METADATA:
        return add_metadata(app, m, v->track, it);
    case P_VOLUME:
        return add_double(it, m->volume);
    case P_POSITION:
        return add_int64(it, v->track ? usec(v->st.position) : 0);
    case P_CAN_NEXT:
        return add_bool(it, v->can_next);
    case P_CAN_PREV:
        return add_bool(it, v->can_prev);
    case P_CAN_PLAY:
    case P_CAN_PAUSE:
    case P_CAN_SEEK:
        return add_bool(it, v->track != NULL);
    }
    return false;
}

static bool add_prop_entry(RXApp *app, RXMpris *m, const View *v, DBusMessageIter *dict, int id)
{
    const char *name = props[id].name;
    DBusMessageIter e = DBUS_MESSAGE_ITER_INIT_CLOSED;
    bool ok = dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e) &&
              dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &name) &&
              add_prop(app, m, v, &e, id) &&
              dbus_message_iter_close_container(dict, &e);
    if (!ok)
        dbus_message_iter_abandon_container_if_open(dict, &e);
    return ok;
}

/* An error reply. Its text must be UTF-8 for libdbus: a strerror() in
 * another locale, or a character cut at the end, gives the bare name. */
static DBusMessage *reply_error(DBusMessage *msg, const char *name, const char *fmt, ...)
    RX_PRINTF(3, 4);
static DBusMessage *reply_error(DBusMessage *msg, const char *name, const char *fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return dbus_message_new_error(msg, name, sendable(text) ? text : name);
}

/* The error dbus_message_get_args() found in a call's arguments. */
static DBusMessage *reply_bad_args(DBusMessage *msg, DBusError *err)
{
    DBusMessage *reply = dbus_message_new_error(msg, err->name ? err->name
                                                               : DBUS_ERROR_INVALID_ARGS,
                                                err->message);
    dbus_error_free(err);
    return reply;
}

/* ------------------------------------------------------------------- calls */

/* Loads `t` for a call, to play once it is in when `play`. A playing deck
 * pauses for the swap (rx_app_load_deck refuses it), and plays on with
 * its track if the load cannot start. */
static int load(RXApp *app, RXMpris *m, RXTrack *t, bool play)
{
    RXDeckState st;
    rx_deck_state(app->player, DECK, &st);
    if (st.playing)
        rx_deck_pause(app->player, DECK);
    int ret = rx_app_load_deck(app, DECK, t);
    if (ret < 0) {
        if (st.playing)
            rx_deck_play(app->player, DECK);
        return ret;
    }
    m->want_track = t->id;
    m->want_play = play;
    return 0;
}

/* Stop: paused at the cue point, as CUE does to a playing deck; a paused
 * one goes there too. */
static void stop(RXApp *app, RXMpris *m, const View *v)
{
    RXDeckCtl *ctl = &app->decks[DECK].ctl;
    m->want_play = false;
    if (!v->track)
        return;
    if (v->st.playing)
        rx_ctl_cue_press(ctl);
    else
        rx_ctl_seek(ctl, ctl->cue_point >= 0.0 ? ctl->cue_point : 0.0);
    m->seeked = true;
}

/* Next and Previous: the row after or before the deck's track (or the
 * track a call is loading) in the current view, playing if the deck is.
 * Without one, nothing, as CanGoNext and CanGoPrevious said. */
static DBusMessage *skip(RXApp *app, RXMpris *m, DBusMessage *msg, const View *v, int dir)
{
    const RXTrack *from = v->track;
    if (swapping(app, m))
        from = rx_library_find_id(&app->lib, m->want_track);
    RXTrack *t = neighbour(app, from, dir);
    if (!t)
        return dbus_message_new_method_return(msg);
    int ret = load(app, m, t, v->status == STATUS_PLAYING);
    if (ret < 0)
        return reply_error(msg, DBUS_ERROR_FAILED, "cannot load the track: %s", strerror(-ret));
    return dbus_message_new_method_return(msg);
}

/* Seek: by an offset, clamped to the track (the end does not skip to the
 * next track, as the specification would have it: the deck stops there). */
static DBusMessage *seek(RXApp *app, RXMpris *m, DBusMessage *msg, const View *v)
{
    dbus_int64_t offset;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_INT64, &offset, DBUS_TYPE_INVALID))
        return reply_bad_args(msg, &err);
    if (v->track) {
        double to = v->st.position + offset * 1e-6;
        rx_ctl_seek(&app->decks[DECK].ctl, RX_CLAMP(to, 0.0, v->st.duration));
        m->seeked = true;
    }
    return dbus_message_new_method_return(msg);
}

/* SetPosition: ignored for another track's id (a stale call) or a
 * position off the track, as the specification says. */
static DBusMessage *set_position(RXApp *app, RXMpris *m, DBusMessage *msg, const View *v)
{
    const char *id;
    dbus_int64_t pos;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &id, DBUS_TYPE_INT64, &pos,
                               DBUS_TYPE_INVALID))
        return reply_bad_args(msg, &err);
    char path[64];
    track_path(v->track, path, sizeof(path));
    if (v->track && !strcmp(id, path) && pos >= 0 &&
        pos <= usec(RX_MAX(v->track->duration, v->st.duration))) {
        rx_ctl_seek(&app->decks[DECK].ctl, pos * 1e-6);
        m->seeked = true;
    }
    return dbus_message_new_method_return(msg);
}

/* OpenUri: a file URL of a track in the library loads and plays it.
 * Files are never added from here. */
static DBusMessage *open_uri(RXApp *app, RXMpris *m, DBusMessage *msg)
{
    const char *uri;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &uri, DBUS_TYPE_INVALID))
        return reply_bad_args(msg, &err);

    char *path = url_path(uri);
    if (!path)
        return reply_error(msg, DBUS_ERROR_NOT_SUPPORTED, "rxbox opens file:// URLs only");
    RXTrack *t = rx_library_find_path(&app->lib, path);
    if (!t) {
        /* The library keeps resolved paths. */
        char *abs = rx_path_abs(path);
        t = abs ? rx_library_find_path(&app->lib, abs) : NULL;
        free(abs);
    }
    free(path);
    if (!t)
        return reply_error(msg, DBUS_ERROR_FILE_NOT_FOUND, "not a track in the library");
    if (t->missing)
        return reply_error(msg, DBUS_ERROR_FILE_NOT_FOUND, "the track's file is missing");

    int ret = load(app, m, t, true);
    if (ret < 0)
        return reply_error(msg, DBUS_ERROR_FAILED, "cannot load the track: %s", strerror(-ret));
    return dbus_message_new_method_return(msg);
}

static DBusMessage *player_call(RXApp *app, RXMpris *m, DBusMessage *msg, const char *member)
{
    View v;
    view_now(app, m, &v);
    app->dirty = true;

    /* While a call's track is being decoded, play and pause are about it,
     * not about the track the deck paused to give up. */
    if (!strcmp(member, "Play")) {
        if (swapping(app, m))
            m->want_play = true;
        else if (v.track)
            rx_deck_play(app->player, DECK);
    } else if (!strcmp(member, "Pause")) {
        m->want_play = false;
        if (v.track)
            rx_deck_pause(app->player, DECK);
    } else if (!strcmp(member, "PlayPause")) {
        if (swapping(app, m))
            m->want_play = !m->want_play;
        else
            rx_ctl_play_pause(&app->decks[DECK].ctl);
    } else if (!strcmp(member, "Stop")) {
        stop(app, m, &v);
    } else if (!strcmp(member, "Next")) {
        return skip(app, m, msg, &v, 1);
    } else if (!strcmp(member, "Previous")) {
        return skip(app, m, msg, &v, -1);
    } else if (!strcmp(member, "Seek")) {
        return seek(app, m, msg, &v);
    } else if (!strcmp(member, "SetPosition")) {
        return set_position(app, m, msg, &v);
    } else if (!strcmp(member, "OpenUri")) {
        return open_uri(app, m, msg);
    }
    return dbus_message_new_method_return(msg);
}

/* CanRaise is false: Raise does nothing. Quit is closing the window. */
static DBusMessage *root_call(RXApp *app, DBusMessage *msg, const char *member)
{
    if (!strcmp(member, "Quit")) {
        app->quit = true;
        app->dirty = true;
    }
    return dbus_message_new_method_return(msg);
}

/* A property by interface and name; an empty interface is any. */
static int prop_find(const char *iface, const char *name)
{
    for (int i = 0; i < P_COUNT; i++) {
        if ((!iface[0] || !strcmp(iface, props[i].iface)) && !strcmp(name, props[i].name))
            return i;
    }
    return -1;
}

static DBusMessage *get_all(RXApp *app, RXMpris *m, DBusMessage *msg)
{
    const char *iface;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID))
        return reply_bad_args(msg, &err);
    /* Introspectable and Properties have no properties: an empty dict. */
    if (strcmp(iface, IFACE_ROOT) && strcmp(iface, IFACE_PLAYER) &&
        strcmp(iface, DBUS_INTERFACE_PROPERTIES) && strcmp(iface, DBUS_INTERFACE_INTROSPECTABLE))
        return reply_error(msg, DBUS_ERROR_UNKNOWN_INTERFACE, "no interface %s here", iface);

    View v;
    view_now(app, m, &v);
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply)
        return NULL;
    DBusMessageIter it, d = DBUS_MESSAGE_ITER_INIT_CLOSED;
    dbus_message_iter_init_append(reply, &it);
    bool ok = dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &d);
    for (int i = 0; ok && i < P_COUNT; i++) {
        if (!strcmp(props[i].iface, iface))
            ok = add_prop_entry(app, m, &v, &d, i);
    }
    ok = ok && dbus_message_iter_close_container(&it, &d);
    if (!ok) {
        dbus_message_iter_abandon_container_if_open(&it, &d);
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

static DBusMessage *get(RXApp *app, RXMpris *m, DBusMessage *msg)
{
    const char *iface, *name;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_INVALID))
        return reply_bad_args(msg, &err);
    int id = prop_find(iface, name);
    if (id < 0)
        return reply_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s here", name);

    View v;
    view_now(app, m, &v);
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter it;
    if (!reply)
        return NULL;
    dbus_message_iter_init_append(reply, &it);
    if (!add_prop(app, m, &v, &it, id)) {
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

/* Set: Volume, and Rate as far as 1 goes; the rest is read-only here
 * (LoopStatus and Shuffle among them: the deck neither loops tracks nor
 * shuffles). */
static DBusMessage *set(RXApp *app, RXMpris *m, DBusMessage *msg)
{
    if (!dbus_message_has_signature(msg, "ssv"))
        return reply_error(msg, DBUS_ERROR_INVALID_ARGS, "Set takes an interface, a name, a value");
    const char *iface, *name;
    DBusMessageIter it, val;
    dbus_message_iter_init(msg, &it);
    dbus_message_iter_get_basic(&it, &iface);
    dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &name);
    dbus_message_iter_next(&it);
    dbus_message_iter_recurse(&it, &val);

    int id = prop_find(iface, name);
    if (id < 0)
        return reply_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s here", name);
    if (id != P_VOLUME && id != P_RATE)
        return reply_error(msg, DBUS_ERROR_PROPERTY_READ_ONLY, "%s is read-only", name);
    double d = NAN;
    if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_DOUBLE)
        dbus_message_iter_get_basic(&val, &d);
    if (!isfinite(d))
        return reply_error(msg, DBUS_ERROR_INVALID_ARGS, "%s is a number", name);

    if (id == P_VOLUME) {
        /* Below 0 is 0 (the specification), and the player goes no louder
         * than 1. */
        m->volume = RX_CLAMP(d, 0.0, 1.0);
        rx_player_set_master_volume(app->player, (float)m->volume);
    } else if (d == 0.0) {
        /* A rate of 0 is a pause (the specification). */
        m->want_play = false;
        rx_deck_pause(app->player, DECK);
    } else if (d != 1.0) {
        return reply_error(msg, DBUS_ERROR_INVALID_ARGS, "the rate is 1");
    }
    app->dirty = true;
    return dbus_message_new_method_return(msg);
}

static bool iface_is(const char *iface, const char *name)
{
    /* A call may name no interface: any whose member it is. */
    return !iface || !strcmp(iface, name);
}

static bool player_method(const char *member)
{
    for (size_t i = 0; i < RX_ARRAY_ELEMS(player_methods); i++) {
        if (!strcmp(member, player_methods[i]))
            return true;
    }
    return false;
}

/* Every call to OBJECT_PATH, from inside rx_mpris_tick()'s dispatch. One
 * the object does not have goes back to libdbus, which answers
 * UnknownMethod. A reply that cannot be made (out of memory) is not sent;
 * the call is not run again. */
static DBusHandlerResult on_message(DBusConnection *conn, DBusMessage *msg, void *opaque)
{
    RXApp *app = opaque;
    RXMpris *m = app->mpris;
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!m || dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply;
    if (iface_is(iface, DBUS_INTERFACE_INTROSPECTABLE) && !strcmp(member, "Introspect")) {
        const char *xml = introspection;
        reply = dbus_message_new_method_return(msg);
        if (reply && !dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply = NULL;
        }
    } else if (iface_is(iface, DBUS_INTERFACE_PROPERTIES) && !strcmp(member, "Get")) {
        reply = get(app, m, msg);
    } else if (iface_is(iface, DBUS_INTERFACE_PROPERTIES) && !strcmp(member, "GetAll")) {
        reply = get_all(app, m, msg);
    } else if (iface_is(iface, DBUS_INTERFACE_PROPERTIES) && !strcmp(member, "Set")) {
        reply = set(app, m, msg);
    } else if (iface_is(iface, IFACE_ROOT) && (!strcmp(member, "Raise") ||
                                               !strcmp(member, "Quit"))) {
        reply = root_call(app, msg, member);
    } else if (iface_is(iface, IFACE_PLAYER) && player_method(member)) {
        reply = player_call(app, m, msg, member);
    } else {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (reply && !dbus_message_get_no_reply(msg))
        dbus_connection_send(conn, reply, NULL);
    if (reply)
        dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ----------------------------------------------------------------- signals */

/* PropertiesChanged on the Player interface for what differs from what
 * the clients were last told. */
static void send_changes(RXApp *app, RXMpris *m, const View *v)
{
    Shown now = shown_now(app, m, v);
    int ids[P_COUNT];
    int n = 0;
    if (now.status != m->shown.status)
        ids[n++] = P_STATUS;
    if (now.meta != m->shown.meta)
        ids[n++] = P_METADATA;
    if (now.volume != m->shown.volume)
        ids[n++] = P_VOLUME;
    if (now.can_next != m->shown.can_next)
        ids[n++] = P_CAN_NEXT;
    if (now.can_prev != m->shown.can_prev)
        ids[n++] = P_CAN_PREV;
    if (now.loaded != m->shown.loaded) {
        ids[n++] = P_CAN_PLAY;
        ids[n++] = P_CAN_PAUSE;
        ids[n++] = P_CAN_SEEK;
    }
    if (!n)
        return;

    DBusMessage *sig = dbus_message_new_signal(OBJECT_PATH, DBUS_INTERFACE_PROPERTIES,
                                               "PropertiesChanged");
    if (!sig)
        return;
    const char *iface = IFACE_PLAYER;
    DBusMessageIter it, d = DBUS_MESSAGE_ITER_INIT_CLOSED, none = DBUS_MESSAGE_ITER_INIT_CLOSED;
    dbus_message_iter_init_append(sig, &it);
    bool ok = dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface) &&
              dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &d);
    for (int i = 0; ok && i < n; i++)
        ok = add_prop_entry(app, m, v, &d, ids[i]);
    ok = ok && dbus_message_iter_close_container(&it, &d) &&
         dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING,
                                          &none) &&
         dbus_message_iter_close_container(&it, &none);
    if (!ok) {
        dbus_message_iter_abandon_container_if_open(&it, &d);
        dbus_message_iter_abandon_container_if_open(&it, &none);
    }
    /* Told only once sent: what could not be goes at the next tick. */
    if (ok && dbus_connection_send(m->conn, sig, NULL))
        m->shown = now;
    dbus_message_unref(sig);
}

static void send_seeked(RXMpris *m, const View *v)
{
    DBusMessage *sig = dbus_message_new_signal(OBJECT_PATH, IFACE_PLAYER, "Seeked");
    if (!sig)
        return;
    dbus_int64_t us = usec(v->st.position);
    if (dbus_message_append_args(sig, DBUS_TYPE_INT64, &us, DBUS_TYPE_INVALID))
        dbus_connection_send(m->conn, sig, NULL);
    dbus_message_unref(sig);
}

/* Whether the playhead moved other than by playing since the last tick:
 * back, or further ahead than the time since allows at the deck's tempo
 * (as track_plays in app.c tells it), or at all while paused. Another
 * track is not a seek (its Metadata says enough), and a scrub is one
 * seek, when it ends. */
static bool jumped(RXMpris *m, const View *v, int64_t now)
{
    uint64_t id = v->track ? v->track->id : 0;
    bool same = id && id == m->pos_track;
    double step = v->st.position - m->pos_last;
    double since = (now - m->pos_us) * 1e-6;
    bool moving = v->st.playing || m->pos_playing;
    bool scrubbed = m->pos_scrubbing && !v->st.scrubbing;

    m->pos_track = id;
    m->pos_last = v->st.position;
    m->pos_us = now;
    m->pos_playing = v->st.playing;
    m->pos_scrubbing = v->st.scrubbing;

    if (!same || v->st.scrubbing)
        return false;
    if (scrubbed)
        return true;
    if (!moving)
        return fabs(step) > STILL_SLACK;
    double ahead = since * RX_MAX(v->st.tempo, 1.0) * 1.25 + PLAY_SLACK;
    return step < -BACK_SLACK || step > ahead;
}

/* ---------------------------------------------------------------- lifecycle */

/* The session bus' address: DBUS_SESSION_BUS_ADDRESS, else the socket a
 * systemd session keeps in $XDG_RUNTIME_DIR, as libdbus' own lookup does,
 * without its autolaunch, which could start a bus from here. Allocating;
 * NULL when there is none. */
static char *session_address(void)
{
    const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env && env[0])
        return rx_strdup(env);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/')
        return NULL;

    char *sock = rx_path_join(runtime, "bus");
    char *address = NULL;
    struct stat st;
    if (sock && !stat(sock, &st) && S_ISSOCK(st.st_mode)) {
        char *escaped = dbus_address_escape_value(sock);
        if (escaped)
            address = rx_asprintf("unix:path=%s", escaped);
        dbus_free(escaped);
    }
    free(sock);
    return address;
}

int rx_mpris_create(RXApp *app)
{
    if (app->mpris)
        return 0;
    if (!app->player)
        return RX_ERROR(EINVAL);

    char *address = session_address();
    if (!address) {
        rx_warn("mpris: no session bus (DBUS_SESSION_BUS_ADDRESS is not set); "
                "no media controls");
        return RX_ERROR(ENOENT);
    }

    RXMpris *m = calloc(1, sizeof(*m));
    if (!m) {
        free(address);
        return RX_ERROR(ENOMEM);
    }
    rx_array_init(&m->art_files, sizeof(uint64_t));
    m->volume = 1.0;
    m->meta_generation = app->lib.generation;

    static const DBusObjectPathVTable vtable = {
        .message_function = on_message,
    };
    DBusError err;
    dbus_error_init(&err);
    const char *why = NULL;
    int ret = RX_ERROR(ECONNREFUSED);

    m->conn = dbus_connection_open_private(address, &err);
    free(address);
    if (!m->conn)
        goto fail;
    /* A bus going away ends MPRIS, not rxbox (libdbus would _exit). */
    dbus_connection_set_exit_on_disconnect(m->conn, FALSE);
    if (!dbus_bus_register(m->conn, &err) ||
        !dbus_connection_try_register_object_path(m->conn, OBJECT_PATH, &vtable, app, &err))
        goto fail;
    int owner = dbus_bus_request_name(m->conn, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (owner < 0)
        goto fail;
    if (owner != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        owner != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        why = BUS_NAME " has an owner (another rxbox?)";
        ret = RX_ERROR(EEXIST);
        goto fail;
    }

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] == '/')
        m->art_dir = rx_asprintf("%s/rxbox/mpris-%d", runtime, (int)getpid());

    /* The clients are told of changes from here on; what is there now they
     * read. */
    app->mpris = m;
    View v;
    view_now(app, m, &v);
    m->shown = shown_now(app, m, &v);
    jumped(m, &v, rx_time_us());
    rx_debug("mpris: %s on the session bus", BUS_NAME);
    return 0;

fail:
    if (!why)
        why = dbus_error_is_set(&err) ? err.message : strerror(ENOMEM);
    /* libdbus' messages may end in a newline. */
    rx_warn("mpris: not on the session bus (%.*s); no media controls",
            (int)strcspn(why, "\n"), why);
    dbus_error_free(&err);
    if (m->conn) {
        dbus_connection_close(m->conn);
        dbus_connection_unref(m->conn);
    }
    rx_array_free(&m->art_files);
    free(m);
    return ret;
}

void rx_mpris_tick(RXApp *app)
{
    RXMpris *m = app->mpris;
    if (!m)
        return;

    /* What came in, answered; nothing here waits. */
    bool alive = dbus_connection_read_write_dispatch(m->conn, 0);
    for (int n = 0; alive && n < CALLS_PER_TICK &&
         dbus_connection_get_dispatch_status(m->conn) == DBUS_DISPATCH_DATA_REMAINS; n++) {
        if (dbus_connection_dispatch(m->conn) == DBUS_DISPATCH_NEED_MEMORY)
            break;
    }
    if (!alive || !dbus_connection_get_is_connected(m->conn)) {
        rx_warn("mpris: the session bus went away; no media controls");
        rx_mpris_destroy(app);
        return;
    }

    /* A track a call loaded plays once its decode is in, if the call said
     * so; a load that failed, or another track loaded since, ends the
     * wait. */
    RXDeckView *dv = &app->decks[DECK];
    if (m->want_track && !dv->loading_request) {
        if (m->want_play && dv->track && dv->track->id == m->want_track &&
            rx_deck_loaded(app->player, DECK)) {
            rx_deck_play(app->player, DECK);
            app->dirty = true;
        }
        m->want_track = 0;
        m->want_play = false;
    }

    View v;
    view_now(app, m, &v);
    bool seeked = jumped(m, &v, rx_time_us()) || m->seeked;
    m->seeked = false;
    send_changes(app, m, &v);
    if (seeked && v.track)
        send_seeked(m, &v);

    /* Out with the replies and signals, as far as the socket takes them. */
    dbus_connection_read_write(m->conn, 0);
}

void rx_mpris_destroy(RXApp *app)
{
    RXMpris *m = app->mpris;
    if (!m)
        return;
    app->mpris = NULL;

    /* Closing gives up the name. */
    dbus_connection_close(m->conn);
    dbus_connection_unref(m->conn);
    covers_remove(m);
    rx_array_free(&m->art_files);
    free(m->art_url);
    free(m->art_dir);
    free(m);
}
