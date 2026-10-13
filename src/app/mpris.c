/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MPRIS 2 for the deck (rxbox/mpris.h), on sd-bus: the object
 * /org/mpris/MediaPlayer2 under the name org.mpris.MediaPlayer2.rxbox,
 * with the root and Player interfaces of freedesktop.org's Media Player
 * Remote Interfacing Specification 2.2
 * (specifications.freedesktop.org/mpris-spec/latest). There is no
 * TrackList: the browser's current view stands for one, through Next and
 * Previous.
 *
 * The two interfaces are vtables: each method with its handler, each
 * property with its getter (and a setter for Volume and Rate), and the
 * Seeked signal. sd-bus does the rest of D-Bus from them: Introspectable,
 * Properties (Get, GetAll, and Set, which it refuses for a property
 * without a setter as read-only), Peer, a call's arguments checked
 * against its signature, the errors for what the object does not have,
 * and PropertiesChanged built from the getters for the names it is given.
 *
 * Decisions:
 *  - A connection of our own to the user bus (sd_bus_open_user), used on
 *    the owner thread only: sd-bus has no threads and calls into the app
 *    only from sd_bus_process(). rx_mpris_tick() runs that until it has
 *    nothing left to do (CALLS_PER_TICK steps at most), never waiting (no
 *    sd_bus_wait, no sd_bus_flush): a call is answered from inside it and
 *    acts on the deck as the interface's buttons do. sd_bus_send() writes
 *    a message at once when nothing is queued before it; what the socket
 *    did not take goes out from sd_bus_process(), which writes before it
 *    reads. Connecting and taking the name, at startup, are the only
 *    waits.
 *  - The getters work from one view of the deck per message (view()): a
 *    GetAll, or a PropertiesChanged, is one snapshot.
 *  - What the clients see is worked out once per tick and compared with
 *    what they were last told: PropertiesChanged names the properties that
 *    differ, and sd-bus adds their values. Never Position (its vtable
 *    entry emits nothing): the specification has clients read it, and
 *    Seeked tells them when it moved other than by playing, the way
 *    track_plays in app.c tells a seek from playing (further ahead than
 *    the time since the last tick allows, or back), and after a seek asked
 *    for here. A loop wrapping is such a jump too; a scrub is one, when it
 *    ends.
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
 *  - The name is taken without queueing: a second rxbox stays off the bus
 *    rather than wait for the first to quit.
 *  - A bus that goes away ends MPRIS, not rxbox: sd-bus only exits on a
 *    disconnect when told to, and the tick finds the connection closed.
 *  - sd-bus refuses a string that is not UTF-8 by systemd's rules (which
 *    also refuse the noncharacters D-Bus allows), and one refused append
 *    spoils the whole message, a GetAll's or a PropertiesChanged's: a tag
 *    goes out only once utf8_valid() passed it, and the file's path only
 *    percent-encoded, as ASCII. */

#include "rxbox/mpris.h"

#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

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
/* sd_bus_process() steps per tick at most (a call answered is one): a
 * client flooding the bus waits for the next ticks, the frame does not. */
#define CALLS_PER_TICK 64
/* The seek test (jumped): seconds a playing deck may run ahead of what the
 * time since the last tick allows (track_plays' 0.05), step back, or move
 * at all while paused. The playhead never steps back by itself. */
#define PLAY_SLACK   0.05
#define BACK_SLACK   0.02
#define STILL_SLACK  0.001
/* A property PropertiesChanged may carry: every one but Position and
 * CanControl, as the specification has them. */
#define EMITS        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE

enum Status {
    STATUS_STOPPED = 0,
    STATUS_PAUSED,
    STATUS_PLAYING,
};

static const char *const status_names[] = { "Stopped", "Paused", "Playing" };

static const char *const uri_schemes[] = { "file", NULL };

/* What the library adds (rx_is_audio_path: mp3 m4a mp4 aac wav aif aiff
 * flac ogg opus), under the names and the older x- names desktops use. */
static const char *const mime_types[] = {
    "audio/mpeg", "audio/mp4", "audio/x-m4a", "audio/aac", "audio/wav", "audio/x-wav",
    "audio/aiff", "audio/x-aiff", "audio/flac", "audio/x-flac", "audio/ogg", "audio/opus",
    NULL,
};

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
    RXApp   *app;
    sd_bus  *bus;
    double   volume;            /* the player's master volume, 0..1 */
    Shown    shown;
    bool     seeked;            /* a seek asked for here: Seeked at the end of the tick */

    /* The deck as the message being answered or sent sees it (view()). */
    View     view;
    bool     view_valid;

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

/* Whether `s` is UTF-8 as sd-bus has it (systemd's utf8_is_valid): the
 * shortest encoding of a code point up to U+10FFFF that is neither a
 * surrogate nor a noncharacter (U+FDD0..U+FDEF, U+xxFFFE, U+xxFFFF). */
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        int len = (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 0;
        if (!len)
            return false;
        uint32_t c = *p & (0x7f >> len);
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xc0) != 0x80)
                return false;
            c = c << 6 | (p[i] & 0x3f);
        }
        int shortest = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (len != shortest || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            (c >= 0xfdd0 && c <= 0xfdef) || (c & 0xfffe) == 0xfffe)
            return false;
        p += len;
    }
    return true;
}

/* Whether `s` may go out: not empty, and UTF-8 (utf8_valid). */
static bool sendable(const char *s)
{
    return s && s[0] && utf8_valid(s);
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

static int64_t usec(double seconds)
{
    return isfinite(seconds) ? (int64_t)llround(seconds * 1e6) : 0;
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

/* The deck as the message being answered sees it, worked out on first
 * use: pump() starts every message without one. */
static const View *view(RXMpris *m)
{
    if (!m->view_valid) {
        view_now(m->app, m, &m->view);
        m->view_valid = true;
    }
    return &m->view;
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

/* Everything get_metadata() sends of a track: when this changes, so did
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

/* A text field of the metadata; left out when empty or not UTF-8. */
static int add_text(sd_bus_message *reply, const char *key, const char *s)
{
    return sendable(s) ? sd_bus_message_append(reply, "{sv}", key, "s", s) : 0;
}

/* xesam's lists (artist, genre), holding the one value a track has. */
static int add_list(sd_bus_message *reply, const char *key, const char *s)
{
    return sendable(s) ? sd_bus_message_append(reply, "{sv}", key, "as", 1, s) : 0;
}

/* The metadata of a track, past its track id. The title falls back to the
 * file name, which the library's title already is for a file without
 * one. */
static int add_track(RXMpris *m, const RXTrack *t, sd_bus_message *reply)
{
    meta_refresh(m->app, m, t);
    int64_t length = usec(t->duration);
    const char *title = t->meta.title && t->meta.title[0] ? t->meta.title : rx_path_base(t->path);
    char *url = file_url(t->path);
    if (!url)
        return RX_ERROR(ENOMEM);

    int ret = 0;
    if (length > 0)
        ret = sd_bus_message_append(reply, "{sv}", "mpris:length", "x", length);
    if (ret >= 0)
        ret = add_text(reply, "mpris:artUrl", m->art_url);
    if (ret >= 0)
        ret = add_text(reply, "xesam:url", url);
    if (ret >= 0)
        ret = add_text(reply, "xesam:title", title);
    if (ret >= 0)
        ret = add_list(reply, "xesam:artist", t->meta.artist);
    if (ret >= 0)
        ret = add_text(reply, "xesam:album", t->meta.album);
    if (ret >= 0)
        ret = add_list(reply, "xesam:genre", t->meta.genre);
    if (ret >= 0 && t->meta.rating > 0)
        ret = sd_bus_message_append(reply, "{sv}", "xesam:userRating", "d",
                                    t->meta.rating / 5.0);
    free(url);
    return ret;
}

/* -------------------------------------------------------------- properties */

/* The getters append the value; sd-bus puts it in its variant. A getter
 * that fails fails the whole Get, GetAll or PropertiesChanged. */

static int get_true(sd_bus *bus, const char *path, const char *iface, const char *name,
                    sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "b", true);
}

/* CanRaise, HasTrackList, Shuffle: the deck neither raises a window nor
 * shuffles. */
static int get_false(sd_bus *bus, const char *path, const char *iface, const char *name,
                     sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "b", false);
}

static int get_identity(sd_bus *bus, const char *path, const char *iface, const char *name,
                        sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", "rxbox");
}

static int get_uri_schemes(sd_bus *bus, const char *path, const char *iface, const char *name,
                           sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append_strv(reply, (char **)uri_schemes);
}

static int get_mime_types(sd_bus *bus, const char *path, const char *iface, const char *name,
                          sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append_strv(reply, (char **)mime_types);
}

static int get_status(sd_bus *bus, const char *path, const char *iface, const char *name,
                      sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", status_names[view(opaque)->status]);
}

/* The deck loops a part of a track, never a track or a list. */
static int get_loop(sd_bus *bus, const char *path, const char *iface, const char *name,
                    sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", "None");
}

/* Rate, MinimumRate, MaximumRate. */
static int get_rate(sd_bus *bus, const char *path, const char *iface, const char *name,
                    sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "d", 1.0);
}

/* Metadata (the specification's metadata page): the track id always, the
 * rest for a track. */
static int get_metadata(sd_bus *bus, const char *path, const char *iface, const char *name,
                        sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    const RXTrack *t = view(m)->track;
    char id[64];
    track_path(t, id, sizeof(id));

    int ret = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (ret >= 0)
        ret = sd_bus_message_append(reply, "{sv}", "mpris:trackid", "o", id);
    if (ret >= 0 && t)
        ret = add_track(m, t, reply);
    if (ret >= 0)
        ret = sd_bus_message_close_container(reply);
    return ret;
}

static int get_volume(sd_bus *bus, const char *path, const char *iface, const char *name,
                      sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    return sd_bus_message_append(reply, "d", m->volume);
}

static int get_position(sd_bus *bus, const char *path, const char *iface, const char *name,
                        sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    const View *v = view(opaque);
    return sd_bus_message_append(reply, "x", v->track ? usec(v->st.position) : (int64_t)0);
}

static int get_can_next(sd_bus *bus, const char *path, const char *iface, const char *name,
                        sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "b", view(opaque)->can_next);
}

static int get_can_prev(sd_bus *bus, const char *path, const char *iface, const char *name,
                        sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "b", view(opaque)->can_prev);
}

/* CanPlay, CanPause, CanSeek: with a track. */
static int get_loaded(sd_bus *bus, const char *path, const char *iface, const char *name,
                      sd_bus_message *reply, void *opaque, sd_bus_error *err)
{
    return sd_bus_message_append(reply, "b", view(opaque)->track != NULL);
}

/* An error for a reply. Its text must be UTF-8, or sd-bus would not send
 * the reply at all: a strerror() in another locale, or a character cut at
 * the end, gives the bare name. */
static int reply_error(sd_bus_error *err, const char *name, const char *fmt, ...) RX_PRINTF(3, 4);
static int reply_error(sd_bus_error *err, const char *name, const char *fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return sd_bus_error_set(err, name, sendable(text) ? text : name);
}

/* A number for Volume or Rate. */
static int read_number(sd_bus_message *value, const char *name, double *d, sd_bus_error *err)
{
    int ret = sd_bus_message_read(value, "d", d);
    if (ret < 0)
        return ret;
    if (!isfinite(*d))
        return reply_error(err, SD_BUS_ERROR_INVALID_ARGS, "%s is a number", name);
    return 0;
}

static int set_volume(sd_bus *bus, const char *path, const char *iface, const char *name,
                      sd_bus_message *value, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    double d;
    int ret = read_number(value, name, &d, err);
    if (ret < 0)
        return ret;
    /* Below 0 is 0 (the specification), and the player goes no louder
     * than 1. */
    m->volume = RX_CLAMP(d, 0.0, 1.0);
    rx_player_set_master_volume(m->app->player, (float)m->volume);
    m->app->dirty = true;
    return 0;
}

/* Rate as far as 1 goes. */
static int set_rate(sd_bus *bus, const char *path, const char *iface, const char *name,
                    sd_bus_message *value, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    double d;
    int ret = read_number(value, name, &d, err);
    if (ret < 0)
        return ret;
    if (d != 0.0 && d != 1.0)
        return reply_error(err, SD_BUS_ERROR_INVALID_ARGS, "the rate is 1");
    /* A rate of 0 is a pause (the specification). */
    if (d == 0.0) {
        m->want_play = false;
        rx_deck_pause(m->app->player, DECK);
    }
    m->app->dirty = true;
    return 0;
}

/* ------------------------------------------------------------------- calls */

/* Every call to a Player method, before it acts: the frame shows what it
 * did. */
static RXApp *player_call(RXMpris *m)
{
    m->app->dirty = true;
    return m->app;
}

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

/* Next and Previous: the row after or before the deck's track (or the
 * track a call is loading) in the current view, playing if the deck is.
 * Without one, nothing, as CanGoNext and CanGoPrevious said. */
static int skip(RXMpris *m, sd_bus_message *msg, sd_bus_error *err, int dir)
{
    RXApp *app = player_call(m);
    const View *v = view(m);
    const RXTrack *from = v->track;
    if (swapping(app, m))
        from = rx_library_find_id(&app->lib, m->want_track);
    RXTrack *t = neighbour(app, from, dir);
    if (t) {
        int ret = load(app, m, t, v->status == STATUS_PLAYING);
        if (ret < 0)
            return reply_error(err, SD_BUS_ERROR_FAILED, "cannot load the track: %s",
                               strerror(-ret));
    }
    return sd_bus_reply_method_return(msg, NULL);
}

static int method_next(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    return skip(opaque, msg, err, 1);
}

static int method_previous(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    return skip(opaque, msg, err, -1);
}

/* While a call's track is being decoded, Play, Pause and PlayPause are
 * about it, not about the track the deck paused to give up. */

static int method_play(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    RXApp *app = player_call(m);
    if (swapping(app, m))
        m->want_play = true;
    else if (view(m)->track)
        rx_deck_play(app->player, DECK);
    return sd_bus_reply_method_return(msg, NULL);
}

static int method_pause(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    RXApp *app = player_call(m);
    m->want_play = false;
    if (view(m)->track)
        rx_deck_pause(app->player, DECK);
    return sd_bus_reply_method_return(msg, NULL);
}

static int method_play_pause(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    RXApp *app = player_call(m);
    if (swapping(app, m))
        m->want_play = !m->want_play;
    else
        rx_ctl_play_pause(&app->decks[DECK].ctl);
    return sd_bus_reply_method_return(msg, NULL);
}

/* Stop: paused at the cue point, as CUE does to a playing deck; a paused
 * one goes there too. */
static int method_stop(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    RXApp *app = player_call(m);
    RXDeckCtl *ctl = &app->decks[DECK].ctl;
    const View *v = view(m);
    m->want_play = false;
    if (v->track) {
        if (v->st.playing)
            rx_ctl_cue_press(ctl);
        else
            rx_ctl_seek(ctl, ctl->cue_point >= 0.0 ? ctl->cue_point : 0.0);
        m->seeked = true;
    }
    return sd_bus_reply_method_return(msg, NULL);
}

/* Seek: by an offset, clamped to the track (the end does not skip to the
 * next track, as the specification would have it: the deck stops there). */
static int method_seek(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    RXApp *app = player_call(m);
    int64_t offset;
    int ret = sd_bus_message_read(msg, "x", &offset);
    if (ret < 0)
        return ret;
    const View *v = view(m);
    if (v->track) {
        double to = v->st.position + offset * 1e-6;
        rx_ctl_seek(&app->decks[DECK].ctl, RX_CLAMP(to, 0.0, v->st.duration));
        m->seeked = true;
    }
    return sd_bus_reply_method_return(msg, NULL);
}

/* SetPosition: ignored for another track's id (a stale call) or a
 * position off the track, as the specification says. */
static int method_set_position(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    RXApp *app = player_call(m);
    const char *id;
    int64_t pos;
    int ret = sd_bus_message_read(msg, "ox", &id, &pos);
    if (ret < 0)
        return ret;
    const View *v = view(m);
    char path[64];
    track_path(v->track, path, sizeof(path));
    if (v->track && !strcmp(id, path) && pos >= 0 &&
        pos <= usec(RX_MAX(v->track->duration, v->st.duration))) {
        rx_ctl_seek(&app->decks[DECK].ctl, pos * 1e-6);
        m->seeked = true;
    }
    return sd_bus_reply_method_return(msg, NULL);
}

/* OpenUri: a file URL of a track in the library loads and plays it.
 * Files are never added from here. */
static int method_open_uri(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    RXApp *app = player_call(m);
    const char *uri;
    int ret = sd_bus_message_read(msg, "s", &uri);
    if (ret < 0)
        return ret;

    char *path = url_path(uri);
    if (!path)
        return reply_error(err, SD_BUS_ERROR_NOT_SUPPORTED, "rxbox opens file:// URLs only");
    RXTrack *t = rx_library_find_path(&app->lib, path);
    if (!t) {
        /* The library keeps resolved paths. */
        char *abs = rx_path_abs(path);
        t = abs ? rx_library_find_path(&app->lib, abs) : NULL;
        free(abs);
    }
    free(path);
    if (!t)
        return reply_error(err, SD_BUS_ERROR_FILE_NOT_FOUND, "not a track in the library");
    if (t->missing)
        return reply_error(err, SD_BUS_ERROR_FILE_NOT_FOUND, "the track's file is missing");

    ret = load(app, m, t, true);
    if (ret < 0)
        return reply_error(err, SD_BUS_ERROR_FAILED, "cannot load the track: %s",
                           strerror(-ret));
    return sd_bus_reply_method_return(msg, NULL);
}

/* CanRaise is false: Raise does nothing. */
static int method_raise(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    return sd_bus_reply_method_return(msg, NULL);
}

/* Quit is closing the window. */
static int method_quit(sd_bus_message *msg, void *opaque, sd_bus_error *err)
{
    RXMpris *m = opaque;
    m->app->quit = true;
    m->app->dirty = true;
    return sd_bus_reply_method_return(msg, NULL);
}

/* ------------------------------------------------------------------ vtables */

/* The root interface. Fullscreen, CanSetFullscreen and DesktopEntry are
 * optional and left out. */
static const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", method_raise, 0),
    SD_BUS_METHOD("Quit", "", "", method_quit, 0),
    SD_BUS_PROPERTY("CanQuit", "b", get_true, 0, EMITS),
    SD_BUS_PROPERTY("CanRaise", "b", get_false, 0, EMITS),
    SD_BUS_PROPERTY("HasTrackList", "b", get_false, 0, EMITS),
    SD_BUS_PROPERTY("Identity", "s", get_identity, 0, EMITS),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", get_uri_schemes, 0, EMITS),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", get_mime_types, 0, EMITS),
    SD_BUS_VTABLE_END,
};

/* The Player interface. LoopStatus and Shuffle have no setter: read-only
 * here (the deck neither loops tracks nor shuffles), where the
 * specification has them writable. Position emits nothing (clients read
 * it), CanControl never changes. */
static const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", method_next, 0),
    SD_BUS_METHOD("Previous", "", "", method_previous, 0),
    SD_BUS_METHOD("Pause", "", "", method_pause, 0),
    SD_BUS_METHOD("PlayPause", "", "", method_play_pause, 0),
    SD_BUS_METHOD("Stop", "", "", method_stop, 0),
    SD_BUS_METHOD("Play", "", "", method_play, 0),
#ifdef SD_BUS_METHOD_WITH_ARGS
    SD_BUS_METHOD_WITH_ARGS("Seek", SD_BUS_ARGS("x", Offset), SD_BUS_NO_RESULT, method_seek, 0),
    SD_BUS_METHOD_WITH_ARGS("SetPosition", SD_BUS_ARGS("o", TrackId, "x", Position),
                            SD_BUS_NO_RESULT, method_set_position, 0),
    SD_BUS_METHOD_WITH_ARGS("OpenUri", SD_BUS_ARGS("s", Uri), SD_BUS_NO_RESULT,
                            method_open_uri, 0),
    SD_BUS_SIGNAL_WITH_ARGS("Seeked", SD_BUS_ARGS("x", Position), 0),
#else
    /* basu, the standalone sd-bus, lacks the macros that name arguments
     * (systemd 246): its introspection goes without the names. */
    SD_BUS_METHOD("Seek", "x", "", method_seek, 0),
    SD_BUS_METHOD("SetPosition", "ox", "", method_set_position, 0),
    SD_BUS_METHOD("OpenUri", "s", "", method_open_uri, 0),
    SD_BUS_SIGNAL("Seeked", "x", 0),
#endif
    SD_BUS_PROPERTY("PlaybackStatus", "s", get_status, 0, EMITS),
    SD_BUS_PROPERTY("LoopStatus", "s", get_loop, 0, EMITS),
    SD_BUS_WRITABLE_PROPERTY("Rate", "d", get_rate, set_rate, 0, EMITS),
    SD_BUS_PROPERTY("Shuffle", "b", get_false, 0, EMITS),
    SD_BUS_PROPERTY("Metadata", "a{sv}", get_metadata, 0, EMITS),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", get_volume, set_volume, 0, EMITS),
    SD_BUS_PROPERTY("Position", "x", get_position, 0, 0),
    SD_BUS_PROPERTY("MinimumRate", "d", get_rate, 0, EMITS),
    SD_BUS_PROPERTY("MaximumRate", "d", get_rate, 0, EMITS),
    SD_BUS_PROPERTY("CanGoNext", "b", get_can_next, 0, EMITS),
    SD_BUS_PROPERTY("CanGoPrevious", "b", get_can_prev, 0, EMITS),
    SD_BUS_PROPERTY("CanPlay", "b", get_loaded, 0, EMITS),
    SD_BUS_PROPERTY("CanPause", "b", get_loaded, 0, EMITS),
    SD_BUS_PROPERTY("CanSeek", "b", get_loaded, 0, EMITS),
    SD_BUS_PROPERTY("CanControl", "b", get_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END,
};

/* ----------------------------------------------------------------- signals */

/* PropertiesChanged on the Player interface for what differs from what
 * the clients were last told; the values are the getters', from `v`. */
static void send_changes(RXApp *app, RXMpris *m, const View *v)
{
    Shown now = shown_now(app, m, v);
    const char *names[16];
    int n = 0;
    if (now.status != m->shown.status)
        names[n++] = "PlaybackStatus";
    if (now.meta != m->shown.meta)
        names[n++] = "Metadata";
    if (now.volume != m->shown.volume)
        names[n++] = "Volume";
    if (now.can_next != m->shown.can_next)
        names[n++] = "CanGoNext";
    if (now.can_prev != m->shown.can_prev)
        names[n++] = "CanGoPrevious";
    if (now.loaded != m->shown.loaded) {
        names[n++] = "CanPlay";
        names[n++] = "CanPause";
        names[n++] = "CanSeek";
    }
    names[n] = NULL;
    if (!n)
        return;

    m->view = *v;
    m->view_valid = true;
    /* Told only once sent: what could not be goes at the next tick. */
    if (sd_bus_emit_properties_changed_strv(m->bus, OBJECT_PATH, IFACE_PLAYER,
                                            (char **)names) >= 0)
        m->shown = now;
    m->view_valid = false;
}

static void send_seeked(RXMpris *m, const View *v)
{
    sd_bus_emit_signal(m->bus, OBJECT_PATH, IFACE_PLAYER, "Seeked", "x", usec(v->st.position));
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

/* Calls in, replies and signals out, as far as the socket goes without
 * waiting: sd_bus_process() until it has nothing left to do, each message
 * with a fresh view of the deck. False once the bus went away. */
static bool pump(RXMpris *m)
{
    int ret = 1;
    for (int n = 0; ret > 0 && n < CALLS_PER_TICK; n++) {
        m->view_valid = false;
        ret = sd_bus_process(m->bus, NULL);
    }
    m->view_valid = false;
    return sd_bus_is_open(m->bus) > 0;
}

static void bus_close(sd_bus *bus)
{
    /* Closing gives up the name. Nothing is flushed: that would wait. */
    sd_bus_close(bus);
    sd_bus_unref(bus);
}

int rx_mpris_create(RXApp *app)
{
    if (app->mpris)
        return 0;
    if (!app->player)
        return RX_ERROR(EINVAL);

    RXMpris *m = calloc(1, sizeof(*m));
    if (!m)
        return RX_ERROR(ENOMEM);
    m->app = app;
    rx_array_init(&m->art_files, sizeof(uint64_t));
    m->volume = 1.0;
    m->meta_generation = app->lib.generation;

    /* DBUS_SESSION_BUS_ADDRESS, else $XDG_RUNTIME_DIR/bus; sd-bus never
     * starts a bus of its own. Without either, or without a socket there:
     * ENOENT (ENOMEDIUM without $XDG_RUNTIME_DIR). */
    int ret = sd_bus_open_user(&m->bus);
    if (ret == RX_ERROR(ENOMEDIUM))
        ret = RX_ERROR(ENOENT);
    if (ret == RX_ERROR(ENOENT)) {
        rx_warn("mpris: no session bus; no media controls");
        goto fail;
    }
    if (ret >= 0)
        ret = sd_bus_add_object_vtable(m->bus, NULL, OBJECT_PATH, IFACE_ROOT, root_vtable, m);
    if (ret >= 0)
        ret = sd_bus_add_object_vtable(m->bus, NULL, OBJECT_PATH, IFACE_PLAYER, player_vtable, m);
    if (ret >= 0)
        ret = sd_bus_request_name(m->bus, BUS_NAME, 0);
    if (ret < 0) {
        rx_warn("mpris: not on the session bus (%s); no media controls",
                ret == RX_ERROR(EEXIST) ? BUS_NAME " has an owner (another rxbox?)"
                                        : strerror(-ret));
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
    if (m->bus)
        bus_close(m->bus);
    rx_array_free(&m->art_files);
    free(m);
    return ret;
}

void rx_mpris_tick(RXApp *app)
{
    RXMpris *m = app->mpris;
    if (!m)
        return;

    if (!pump(m)) {
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

    /* What the socket did not take of the replies and signals; a bus that
     * went away meanwhile is found at the next tick. */
    int events = sd_bus_get_events(m->bus);
    if (events > 0 && (events & POLLOUT))
        pump(m);
}

void rx_mpris_destroy(RXApp *app)
{
    RXMpris *m = app->mpris;
    if (!m)
        return;
    app->mpris = NULL;

    bus_close(m->bus);
    covers_remove(m);
    rx_array_free(&m->art_files);
    free(m->art_url);
    free(m->art_dir);
    free(m);
}
