/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The fake player (rxbox/link.h): presence in the player role on a thread
 * of its own, so that the server keeps seeing a player on the link (its
 * keep-alives and status) while the caller browses and reads, blocking;
 * the database client (dbserver.h) and the NFS client (nfs.h) for the
 * rest. Presence is not thread-safe: every call into it holds `lock`. */

#include "rxbox/link.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rxbox/common.h"

#include "client.h"
#include "dbserver.h"
#include "link_priv.h"
#include "net.h"
#include "nfs.h"
#include "presence.h"

struct RXLinkClient {
    RXLinkIf ifc;
    bool quiet;
    uint32_t server;            /* named, or 0 */
    RXLinkPorts ports, server_ports;
    int fd_announce, fd_status, wake;
    pthread_t thread;
    bool running;
    atomic_bool stop;
    pthread_mutex_t lock;
    RXPresence *pres;           /* NULL when quiet */
    RXNfsClient *nfs;           /* made on the first fetch */
    RXDbClient *db;             /* made on first use */
    int number;
    uint8_t dgram[2048];
};

void rx_link_client_options_default(RXLinkClientOptions *o)
{
    RXLinkOptions d;
    rx_link_options_default(&d);
    *o = (RXLinkClientOptions) {
        .number = 3,
        .name = "CDJ-3000",
        .ports = { .announce = RX_LINK_PORT_ANNOUNCE, .status = RX_LINK_PORT_STATUS },
        .server_ports = d.ports,
    };
}

static void send_cb(void *opaque, enum RXLinkPort port, uint32_t to, const uint8_t *buf,
                    size_t len)
{
    RXLinkClient *c = opaque;
    uint32_t dst = to == RX_PRESENCE_BROADCAST ? c->ifc.bcast : to;
    if (port == RX_LINK_ANNOUNCE)
        rx_link_send(c->fd_announce, dst, c->server_ports.announce, buf, len);
    else
        rx_link_send(c->fd_status, dst, c->server_ports.status, buf, len);
}

static void drain(RXLinkClient *c, int fd, enum RXLinkPort port)
{
    for (int k = 0; k < 64; k++) {
        uint32_t from;
        ssize_t n = rx_link_recv(&c->ifc, fd, c->dgram, sizeof(c->dgram), &from, NULL);
        if (n < 0)
            break;
        if (!n || from == c->ifc.addr)
            continue;
        pthread_mutex_lock(&c->lock);
        rx_presence_input(c->pres, port, from, c->dgram, (size_t)n, rx_time_us());
        pthread_mutex_unlock(&c->lock);
    }
}

static void *presence_thread(void *arg)
{
    RXLinkClient *c = arg;
    while (!atomic_load(&c->stop)) {
        pthread_mutex_lock(&c->lock);
        int64_t now = rx_time_us(), next = rx_presence_tick(c->pres, now);
        pthread_mutex_unlock(&c->lock);
        struct pollfd p[3] = {
            { .fd = c->fd_announce, .events = POLLIN },
            { .fd = c->fd_status, .events = POLLIN },
            { .fd = c->wake, .events = POLLIN },
        };
        int timeout = next > now ? (int)RX_MIN((next - now + 999) / 1000, 1000) : 0;
        if (poll(p, 3, timeout) < 0 && errno != EINTR)
            break;
        if (p[0].revents & POLLIN)
            drain(c, c->fd_announce, RX_LINK_ANNOUNCE);
        if (p[1].revents & POLLIN)
            drain(c, c->fd_status, RX_LINK_STATUS);
    }
    return NULL;
}

int rx_link_client_start(RXLinkClient **out, const RXLinkClientOptions *o)
{
    *out = NULL;
    if (o->quiet && !o->server)
        return RX_ERROR(EINVAL);
    RXLinkClient *c = calloc(1, sizeof(*c));
    if (!c)
        return RX_ERROR(ENOMEM);
    c->fd_announce = c->fd_status = c->wake = -1;
    pthread_mutex_init(&c->lock, NULL);
    c->quiet = o->quiet;
    c->number = o->number > 0 ? o->number : 3;
    c->server_ports = o->server_ports;
    int ret;
    if ((ret = rx_link_if_find(&c->ifc, o->interface, o->address, o->broadcast)) < 0)
        goto fail;
    if (o->server && (ret = rx_link_addr_parse(o->server, &c->server)) < 0)
        goto fail;
    if (c->quiet) {
        *out = c;
        return 0;
    }

    /* With an address of our own (the loopback's 127.0.0.2) the sockets
     * are bound to it, so that what we send comes from it; then only
     * datagrams sent to it arrive, which is what the tests' server does
     * (its "broadcast" is our address). On a real interface they hear
     * the broadcast address. */
    uint32_t bind_addr = o->address ? c->ifc.addr : INADDR_ANY;
    if ((c->fd_announce = rx_link_udp(&c->ifc, bind_addr, o->ports.announce,
                                      &c->ports.announce)) < 0) {
        ret = c->fd_announce;
        goto fail;
    }
    if ((c->fd_status = rx_link_udp(&c->ifc, bind_addr, o->ports.status, &c->ports.status)) < 0) {
        ret = c->fd_status;
        goto fail;
    }
    if ((c->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
        ret = RX_ERROR(errno);
        goto fail;
    }
    RXPresenceConfig pc = {
        .role = RX_PRESENCE_PLAYER,
        .addr = c->ifc.addr,
        .number = o->number > 0 ? o->number : 3,
    };
    memcpy(pc.mac, c->ifc.mac, sizeof(pc.mac));
    /* A MAC of our own on the loopback, whose is zeros: presence tells
     * devices apart by it. */
    if (!(pc.mac[0] | pc.mac[1] | pc.mac[2] | pc.mac[3] | pc.mac[4] | pc.mac[5])) {
        pc.mac[0] = 0x02;
        pc.mac[5] = (uint8_t)pc.number;
    }
    snprintf(pc.name, sizeof(pc.name), "%s", o->name ? o->name : "CDJ-3000");
    if ((ret = rx_presence_new(&c->pres, &pc, send_cb, c)) < 0)
        goto fail;
    if ((ret = pthread_create(&c->thread, NULL, presence_thread, c))) {
        ret = RX_ERROR(ret);
        goto fail;
    }
    c->running = true;
    *out = c;
    return 0;
fail:
    rx_link_client_stop(&c);
    return ret;
}

void rx_link_client_stop(RXLinkClient **cp)
{
    RXLinkClient *c = *cp;
    if (!c)
        return;
    if (c->running) {
        atomic_store(&c->stop, true);
        uint64_t v = 1;
        if (write(c->wake, &v, sizeof(v)) < 0)
            rx_debug("link: wake: %s", strerror(errno));
        pthread_join(c->thread, NULL);
    }
    rx_dbclient_close(&c->db);
    rx_nfs_client_free(&c->nfs);
    rx_presence_free(&c->pres);
    if (c->fd_announce >= 0)
        close(c->fd_announce);
    if (c->fd_status >= 0)
        close(c->fd_status);
    if (c->wake >= 0)
        close(c->wake);
    pthread_mutex_destroy(&c->lock);
    free(c);
    *cp = NULL;
}

void rx_link_client_bound_ports(RXLinkClient *c, RXLinkPorts *out)
{
    *out = (RXLinkPorts) { .announce = c->ports.announce, .status = c->ports.status };
}

int rx_link_client_wait_source(RXLinkClient *c, int timeout_ms, uint32_t *addr, int *number)
{
    if (c->quiet) {
        *addr = c->server;
        if (number)
            *number = 0;
        return 0;
    }
    int64_t end = rx_time_us() + (int64_t)timeout_ms * 1000;
    while (1) {
        uint32_t a = 0;
        int n = 0;
        pthread_mutex_lock(&c->lock);
        bool found = rx_presence_source(c->pres, &n, &a, NULL, NULL);
        pthread_mutex_unlock(&c->lock);
        if (found && (!c->server || a == c->server)) {
            *addr = a;
            if (number)
                *number = n;
            return 0;
        }
        if (rx_time_us() >= end)
            return RX_ERROR(ETIMEDOUT);
        usleep(20000);
    }
}

void rx_link_client_set_loaded(RXLinkClient *c, int device, int slot, uint32_t track_id,
                               bool playing)
{
    if (!c->pres)
        return;
    pthread_mutex_lock(&c->lock);
    rx_presence_set_loaded(c->pres, device, slot, track_id, playing);
    pthread_mutex_unlock(&c->lock);
}

/* NFS from the source's portmap, mounted at "/" as rekordbox exports. */
static int nfs_open(RXLinkClient *c)
{
    if (c->nfs)
        return 0;
    uint32_t server;
    int ret = rx_link_client_wait_source(c, 0, &server, NULL);
    if (ret < 0)
        return ret;
    RXNfsClientOptions no = {
        .server = server,
        .portmap_port = c->server_ports.portmap,
        .local = c->ifc.addr,
    };
    if ((ret = rx_nfs_client_open(&c->nfs, &no)) < 0)
        return ret;
    if ((ret = rx_nfs_client_mount(c->nfs, "/")) < 0)
        rx_nfs_client_free(&c->nfs);
    return ret;
}

int rx_link_client_fetch(RXLinkClient *c, const char *path, uint8_t **data, size_t *size)
{
    int ret = nfs_open(c);
    if (ret < 0)
        return ret;
    return rx_nfs_client_fetch(c->nfs, path, 0, data, size);
}

/* ------------------------------------------------------------- database */

#define DB_TIMEOUT 5000

static int wait_fd(int fd, short events)
{
    struct pollfd p = { .fd = fd, .events = events };
    int n;
    while ((n = poll(&p, 1, DB_TIMEOUT)) < 0 && errno == EINTR)
        continue;
    return n < 0 ? RX_ERROR(errno) : n ? 0 : RX_ERROR(ETIMEDOUT);
}

/* The port query on 12523 (djl-analysis track_metadata.adoc, "Connecting
 * to the Database"), from our address, which the server's fence wants to
 * be a player's. */
static int query_port(RXLinkClient *c, uint32_t server, uint16_t *port)
{
    static const uint8_t q[] = {
        0x00, 0x00, 0x00, 0x0f, 'R', 'e', 'm', 'o', 't', 'e', 'D', 'B', 'S', 'e', 'r', 'v', 'e', 'r', 0,
    };
    int fd = rx_link_tcp_connect(&c->ifc, server, c->server_ports.query, DB_TIMEOUT);
    if (fd < 0)
        return fd;
    int ret = 0;
    size_t sent = 0, got = 0;
    uint8_t reply[2];
    while (!ret && sent < sizeof(q)) {
        if ((ret = wait_fd(fd, POLLOUT)) < 0)
            break;
        ssize_t n = send(fd, q + sent, sizeof(q) - sent, MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EINTR)
            ret = RX_ERROR(errno);
        else if (n > 0)
            sent += (size_t)n;
    }
    while (!ret && got < sizeof(reply)) {
        if ((ret = wait_fd(fd, POLLIN)) < 0)
            break;
        ssize_t n = recv(fd, reply + got, sizeof(reply) - got, 0);
        if (!n)
            ret = RX_ERROR(ECONNRESET);
        else if (n < 0 && errno != EAGAIN && errno != EINTR)
            ret = RX_ERROR(errno);
        else if (n > 0)
            got += (size_t)n;
    }
    close(fd);
    if (!ret)
        *port = (uint16_t)(reply[0] << 8 | reply[1]);
    return ret;
}

RXDbClient *rx_link_client_db(RXLinkClient *c, int *err)
{
    if (c->db)
        return c->db;
    uint32_t server;
    uint16_t port = 0;
    int ret = rx_link_client_wait_source(c, 0, &server, NULL);
    if (ret >= 0)
        ret = query_port(c, server, &port);
    if (ret >= 0 && !port)
        ret = RX_ERROR(ECONNREFUSED);   /* the server is not serving yet */
    int fd = ret < 0 ? ret : rx_link_tcp_connect(&c->ifc, server, port, DB_TIMEOUT);
    if (fd < 0) {
        *err = fd;
        return NULL;
    }
    if ((ret = rx_dbclient_open(&c->db, fd, c->number, false, DB_TIMEOUT)) < 0) {
        *err = ret;
        return NULL;
    }
    return c->db;
}

/* --------------------------------------------------------------- browse */

typedef struct Report {
    rx_link_line_cb cb;
    void *opaque;
} Report;

static void say(Report *r, const char *fmt, ...) RX_PRINTF(2, 3);
static void say(Report *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *line = rx_vasprintf(fmt, ap);
    va_end(ap);
    if (line)
        r->cb(r->opaque, line);
    free(line);
}

/* A row's label without the markers players draw around menu names
 * (U+FFFA and U+FFFB, track_metadata.adoc). */
static const char *label(const char *s, char *buf, size_t size)
{
    size_t o = 0;
    for (const char *p = s ? s : ""; *p && o + 1 < size; p++) {
        if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBF &&
            ((uint8_t)p[2] == 0xBA || (uint8_t)p[2] == 0xBB)) {
            p += 2;
            continue;
        }
        buf[o++] = *p;
    }
    buf[o] = 0;
    return buf;
}

static int menu(RXDbClient *db, Report *r, const char *what, uint16_t kind, int loc,
                const uint32_t *args, int nb, RXDbRow **rows, int *nb_rows)
{
    int64_t t0 = rx_time_us();
    int ret = rx_dbclient_menu(db, kind, loc, args, nb, rows, nb_rows);
    if (ret < 0) {
        say(r, "%s (%04x): %s", what, kind, strerror(-ret));
        *rows = NULL;
        *nb_rows = 0;
        return ret;
    }
    say(r, "%s (%04x): %d row%s in %.1f ms", what, kind, *nb_rows, *nb_rows == 1 ? "" : "s",
        (double)(rx_time_us() - t0) / 1000.0);
    return 0;
}

static void print_rows(Report *r, const RXDbRow *rows, int nb, int max)
{
    for (int i = 0; i < nb && i < max; i++) {
        char a[256], b[256];
        say(r, "  %08x %-30s %-20s type %04x", rows[i].id, label(rows[i].text, a, sizeof(a)),
            label(rows[i].text2, b, sizeof(b)), rows[i].type);
    }
    if (nb > max)
        say(r, "  ... %d more", nb - max);
}

static void blob(RXDbClient *db, Report *r, const char *what, uint16_t kind, uint32_t track)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int ret = rx_dbclient_blob(db, kind, track, &data, &size);
    if (ret == RX_ERROR(ENOENT)) {
        say(r, "  %s (%04x): none", what, kind);
    } else if (ret < 0) {
        say(r, "  %s (%04x): %s", what, kind, strerror(-ret));
    } else if (kind == RX_DB_GRID) {
        RXBeat *beats = NULL;
        int nb = 0;
        if (rx_db_grid_parse(data, size, &beats, &nb) >= 0 && nb > 1)
            say(r, "  %s (%04x): %zu bytes, %d beats, the first at %.3f s, %.2f BPM", what, kind,
                size, nb, beats[0].time, 60.0 / (beats[1].time - beats[0].time));
        else
            say(r, "  %s (%04x): %zu bytes, %d beats", what, kind, size, nb);
        free(beats);
    } else if (kind == RX_DB_CUES_EXT) {
        RXPlanCue *cues = NULL;
        int nb = 0;
        rx_db_cues_ext_parse(data, size, &cues, &nb);
        say(r, "  %s (%04x): %zu bytes, %d cues", what, kind, size, nb);
        for (int i = 0; i < nb; i++)
            say(r, "    %s %d at %.3f s%s", cues[i].hot >= 0 ? "hot" : "memory",
                cues[i].hot >= 0 ? cues[i].hot : i, cues[i].time, cues[i].loop_end >= 0 ? ", a loop" : "");
        free(cues);
    } else {
        say(r, "  %s (%04x): %zu bytes", what, kind, size);
    }
    free(data);
}

int rx_link_client_browse(RXLinkClient *c, uint32_t track, rx_link_line_cb line, void *opaque)
{
    Report r = { line, opaque };
    uint32_t server;
    int number = 0, ret = rx_link_client_wait_source(c, 0, &server, &number);
    if (ret < 0)
        return ret;
    char a[16];
    rx_link_addr_text(server, a);
    RXDbClient *db = rx_link_client_db(c, &ret);
    if (!db) {
        say(&r, "source %s: no database: %s", a, strerror(-ret));
        return ret;
    }
    say(&r, "source %s, device %d; set up as player %d, the server says it is %d", a, number,
        c->number, rx_dbclient_server_number(db));

    RXDbRow *rows;
    int nb;
    /* The root menu with every category a player may show (the mask a
     * CDJ-3000 sends, rbxport's capture), and the sort menu. */
    if ((ret = menu(db, &r, "root", RX_DB_ROOT, 1, (const uint32_t[]) { 0, 0x00ffffff }, 2,
                    &rows, &nb)) < 0)
        return ret;
    print_rows(&r, rows, nb, 64);
    rx_dbrows_free(&rows, nb);
    if (menu(db, &r, "sort", RX_DB_SORTS, 1, (const uint32_t[]) { 0 }, 1, &rows, &nb) >= 0)
        print_rows(&r, rows, nb, 64);
    rx_dbrows_free(&rows, nb);

    if ((ret = menu(db, &r, "all tracks", RX_DB_TRACKS, 1, (const uint32_t[]) { 0 }, 1, &rows,
                    &nb)) < 0)
        return ret;
    print_rows(&r, rows, nb, 20);
    if (!track && nb)
        track = rows[0].id;
    rx_dbrows_free(&rows, nb);
    if (menu(db, &r, "playlists", RX_DB_PLAYLIST, 1, (const uint32_t[]) { 0, 0, 1 }, 3, &rows,
             &nb) >= 0)
        print_rows(&r, rows, nb, 20);
    rx_dbrows_free(&rows, nb);
    if (!track) {
        say(&r, "no track to load");
        return 0;
    }

    say(&r, "track %u", track);
    int64_t t0 = rx_time_us();
    if ((ret = rx_dbclient_metadata(db, track, &rows, &nb)) < 0) {
        say(&r, "  metadata: %s", strerror(-ret));
        return ret;
    }
    say(&r, "  metadata (2002): %d rows in %.1f ms", nb, (double)(rx_time_us() - t0) / 1000.0);
    uint32_t art = 0;
    for (int i = 0; i < nb; i++) {
        char x[256], y[256];
        say(&r, "    %04x %-30s %s", rows[i].type, label(rows[i].text, x, sizeof(x)),
            label(rows[i].text2, y, sizeof(y)));
        if (rows[i].type == RX_DB_ITEM_TITLE)
            art = rows[i].c;
    }
    rx_dbrows_free(&rows, nb);

    char *path = NULL;
    uint64_t size = 0;
    if ((ret = rx_dbclient_track_info(db, track, &path, &size)) < 0) {
        say(&r, "  track info: %s", strerror(-ret));
        return ret;
    }
    say(&r, "  track info (2102): %s, %llu bytes", path, (unsigned long long)size);

    uint8_t *jpeg = NULL;
    size_t jn = 0;
    ret = art ? rx_dbclient_artwork(db, art, false, &jpeg, &jn) : RX_ERROR(ENOENT);
    say(&r, "  artwork (2003): %s%s", ret >= 0 ? "" : ret == RX_ERROR(ENOENT) ? "none" :
        strerror(-ret), "");
    if (ret >= 0)
        say(&r, "    %zu bytes of JPEG", jn);
    free(jpeg);
    blob(db, &r, "beat grid", RX_DB_GRID, track);
    blob(db, &r, "preview", RX_DB_PREVIEW, track);
    blob(db, &r, "detail", RX_DB_DETAIL, track);
    blob(db, &r, "cues", RX_DB_CUES_EXT, track);
    uint8_t *tag = NULL;
    size_t tn = 0;
    ret = rx_dbclient_tag(db, track, "PWV5", "EXT", &tag, &tn);
    say(&r, "  PWV5 of the EXT (2c04): %s", ret >= 0 ? "" : strerror(-ret));
    if (ret >= 0)
        say(&r, "    %zu bytes", tn);
    free(tag);

    /* What a CDJ-3000 asks after a load before it browses again
     * (rbxport's capture). */
    RXDbMsg m;
    ret = rx_dbclient_request(db, RX_DB_USER_INFO, 3, NULL, 0, &m);
    if (ret >= 0)
        say(&r, "  user info (3006): %04x, %u arguments", m.kind, (unsigned)m.nb_args);
    else
        say(&r, "  user info (3006): %s", strerror(-ret));
    if (menu(db, &r, "  delivery info", RX_DB_DELIVERY, 3, (const uint32_t[]) { track }, 1,
             &rows, &nb) >= 0)
        rx_dbrows_free(&rows, nb);

    /* The file, as the player reads it. */
    t0 = rx_time_us();
    uint8_t *data = NULL;
    size_t got = 0;
    ret = rx_link_client_fetch(c, path, &data, &got);
    if (ret < 0) {
        say(&r, "  file over NFS: %s", strerror(-ret));
        free(path);
        return ret;
    }
    double s = (double)(rx_time_us() - t0) / 1e6;
    say(&r, "  file over NFS: %zu bytes in %.0f ms (%.1f MB/s)%s", got, s * 1e3,
        (double)got / 1e6 / RX_MAX(s, 1e-6), got == size ? "" : ", NOT the size the track info gave");
    free(data);
    free(path);
    /* Our status says so from now on, every 200 ms: a second is five of
     * them for the server to see. */
    rx_link_client_set_loaded(c, number, 4, track, true);
    say(&r, "  our status: track %u from device %d loaded, playing", track, number);
    if (!c->quiet)
        usleep(1000000);
    return got == size ? 0 : RX_ERROR(EIO);
}
