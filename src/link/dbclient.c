/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The database server's client side (dbserver.h): what the fake player
 * and the tests browse and load with. It asks as a CDJ-3000 does in
 * rbxport's capture of one browsing rekordbox 7.2.11 (rbl-dbserver
 * tests/corpus.rs): the setup with its second argument 0x14, menus at
 * location 1 and 2, data at 8, renders of eight arguments; set up the
 * older way (`legacy`), as the djl-analysis' clients ask, with renders of
 * six.
 *
 * Blocking, one request at a time, with a timeout on every wait. Replies
 * are read by the same bounded parser the server uses, with a larger cap:
 * a whole track's three-band detail is 1.6 MB an hour. */

#include "dbserver.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cyanbox/common.h"

#include "dbmsg.h"

/* The longest reply taken. */
#define REPLY_MAX   (16 * 1024 * 1024)
/* Rows asked for per render: beat-link's 64 (track_metadata.adoc,
 * "Requesting All Tracks", found to work on Nexus players). */
#define PAGE        64
/* Slot and track type of a Link Export source (rbxport's capture). */
#define LINK_SLOT   3
#define LINK_TYPE   1

struct CBDbClient {
    int      fd;
    int      timeout_ms;
    int      number, server;
    bool     legacy;
    uint32_t txid;
    uint8_t *buf;
    size_t   len, cap, used;    /* bytes held; the last message read is the first `used` */
};

/* ------------------------------------------------------------------ io */

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = events };
    while (1) {
        int r = poll(&p, 1, timeout_ms);
        if (r > 0)
            return 0;
        if (r == 0)
            return CB_ERROR(ETIMEDOUT);
        if (errno != EINTR)
            return CB_ERROR(errno);
    }
}

static int send_all(int fd, const uint8_t *p, size_t n, int timeout_ms)
{
    while (n) {
        int ret = wait_fd(fd, POLLOUT, timeout_ms);
        if (ret < 0)
            return ret;
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return CB_ERROR(errno);
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* More bytes into the buffer: -ECONNRESET when the peer closed. */
static int fill(CBDbClient *c)
{
    if (c->len == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 65536;
        uint8_t *b = realloc(c->buf, cap);
        if (!b)
            return CB_ERROR(ENOMEM);
        c->buf = b;
        c->cap = cap;
    }
    int ret = wait_fd(c->fd, POLLIN, c->timeout_ms);
    if (ret < 0)
        return ret;
    ssize_t r = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
    if (r < 0)
        return errno == EINTR || errno == EAGAIN ? 0 : CB_ERROR(errno);
    if (r == 0)
        return CB_ERROR(ECONNRESET);
    c->len += (size_t)r;
    return 0;
}

/* Drops the last message read, keeping what came after it. */
static void consume(CBDbClient *c)
{
    if (c->used) {
        memmove(c->buf, c->buf + c->used, c->len - c->used);
        c->len -= c->used;
        c->used = 0;
    }
}

static int read_msg(CBDbClient *c, CBDbMsg *m)
{
    consume(c);
    while (1) {
        int r = cb_dbmsg_parse(c->buf, c->len, REPLY_MAX, m);
        if (r > 0) {
            c->used = (size_t)r;
            return 0;
        }
        if (r < 0)
            return r;
        if ((r = fill(c)) < 0)
            return r;
    }
}

static int send_msg(CBDbClient *c, const CBDbMsg *m)
{
    CBWr w = cb_wr_dyn();
    cb_dbmsg_write(&w, m);
    int ret = w.err ? CB_ERROR(ENOMEM) : send_all(c->fd, w.p, w.len, c->timeout_ms);
    cb_wr_free(&w);
    return ret;
}

/* -------------------------------------------------------------- connect */

static int tcp_connect(const char *host, uint16_t port, int timeout_ms, int *out)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1)
        return CB_ERROR(EINVAL);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return CB_ERROR(errno);
    int ret = 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (errno != EINPROGRESS) {
            ret = CB_ERROR(errno);
        } else if ((ret = wait_fd(fd, POLLOUT, timeout_ms)) >= 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                ret = CB_ERROR(errno);
            else if (err)
                ret = CB_ERROR(err);
        }
    }
    if (ret < 0) {
        close(fd);
        return ret;
    }
    *out = fd;
    return 0;
}

int cb_dbclient_query_port(const char *host, uint16_t query_port, int timeout_ms, uint16_t *port)
{
    /* What a player sends to 12523 (track_metadata.adoc "Connecting to
     * the Database"): the name's length with its NUL, the name. */
    static const uint8_t q[] = {
        0x00, 0x00, 0x00, 0x0f, 'R', 'e', 'm', 'o', 't', 'e', 'D', 'B', 'S', 'e', 'r', 'v', 'e', 'r', 0,
    };
    int fd;
    int ret = tcp_connect(host, query_port, timeout_ms, &fd);
    if (ret < 0)
        return ret;
    ret = send_all(fd, q, sizeof(q), timeout_ms);
    uint8_t reply[2];
    size_t got = 0;
    while (ret >= 0 && got < 2 && (ret = wait_fd(fd, POLLIN, timeout_ms)) >= 0) {
        ssize_t r = recv(fd, reply + got, 2 - got, 0);
        if (r == 0)
            ret = CB_ERROR(ECONNRESET);
        else if (r < 0 && errno != EINTR && errno != EAGAIN)
            ret = CB_ERROR(errno);
        else if (r > 0)
            got += (size_t)r;
    }
    close(fd);
    if (ret < 0)
        return ret;
    *port = (uint16_t)(reply[0] << 8 | reply[1]);
    return 0;
}

int cb_dbclient_open(CBDbClient **out, int fd, int number, bool legacy, int timeout_ms)
{
    *out = NULL;
    CBDbClient *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return CB_ERROR(ENOMEM);
    }
    *c = (CBDbClient) { .fd = fd, .timeout_ms = timeout_ms, .number = number, .legacy = legacy,
                        .txid = 1 };
    /* The greeting, answered in kind. */
    int ret = send_all(fd, cb_db_greeting, CB_DB_GREETING_LEN, timeout_ms);
    while (ret >= 0 && c->len < CB_DB_GREETING_LEN)
        ret = fill(c);
    if (ret >= 0 && memcmp(c->buf, cb_db_greeting, CB_DB_GREETING_LEN))
        ret = CB_ERROR(EPROTO);
    c->used = CB_DB_GREETING_LEN;

    CBDbMsg m = { .txid = CB_DB_SETUP_TXID, .kind = CB_DB_SETUP, .nb_args = legacy ? 1 : 2 };
    m.args[0] = (CBDbArg) { .type = CB_DB_NUM, .num = (uint32_t)number };
    m.args[1] = (CBDbArg) { .type = CB_DB_NUM, .num = CB_DB_SETUP_MAGIC };
    if (ret >= 0)
        ret = send_msg(c, &m);
    CBDbMsg r;
    if (ret >= 0)
        ret = read_msg(c, &r);
    /* rekordbox: its own kind, [its number, 0x14]; the older answer:
     * 4000 [0, its number] (dbserver.c). */
    if (ret >= 0) {
        if (!legacy && r.kind == CB_DB_SETUP && r.nb_args >= 1)
            c->server = (int)cb_dbmsg_num(&r, 0);
        else if (legacy && r.kind == CB_DB_AVAILABLE && r.nb_args >= 2)
            c->server = (int)cb_dbmsg_num(&r, 1);
        else
            ret = CB_ERROR(EPROTO);
    }
    if (ret < 0) {
        close(c->fd);
        free(c->buf);
        free(c);
        return ret;
    }
    *out = c;
    return 0;
}

int cb_dbclient_connect(CBDbClient **out, const char *host, uint16_t port, int number,
                        int timeout_ms)
{
    *out = NULL;
    int fd;
    int ret = tcp_connect(host, port, timeout_ms, &fd);
    return ret < 0 ? ret : cb_dbclient_open(out, fd, number, false, timeout_ms);
}

void cb_dbclient_close(CBDbClient **cp)
{
    CBDbClient *c = *cp;
    if (!c)
        return;
    CBDbMsg m = { .txid = CB_DB_SETUP_TXID, .kind = CB_DB_TEARDOWN };
    send_msg(c, &m);
    close(c->fd);
    free(c->buf);
    free(c);
    *cp = NULL;
}

int cb_dbclient_server_number(const CBDbClient *c)
{
    return c->server;
}

/* ------------------------------------------------------------ requests */

static uint32_t rmst(const CBDbClient *c, int location)
{
    return cb_db_rmst(c->number, location, LINK_SLOT, LINK_TYPE);
}

static int request(CBDbClient *c, uint16_t kind, const CBDbArg *args, int nb, CBDbMsg *reply)
{
    CBDbMsg m = { .txid = c->txid++, .kind = kind, .nb_args = CB_MIN(nb, CB_DB_MAX_ARGS) };
    memcpy(m.args, args, (size_t)m.nb_args * sizeof(*args));
    int ret = send_msg(c, &m);
    if (ret < 0)
        return ret;
    ret = read_msg(c, reply);
    if (ret >= 0 && reply->txid != m.txid)
        ret = CB_ERROR(EPROTO);
    return ret;
}

int cb_dbclient_request(CBDbClient *c, uint16_t kind, int location, const uint32_t *args, int nb,
                        CBDbMsg *reply)
{
    CBDbArg a[CB_DB_MAX_ARGS];
    nb = CB_CLAMP(nb, 0, CB_DB_MAX_ARGS - 1);
    a[0] = (CBDbArg) { .type = CB_DB_NUM, .num = rmst(c, location) };
    for (int i = 0; i < nb; i++)
        a[i + 1] = (CBDbArg) { .type = CB_DB_NUM, .num = args[i] };
    return request(c, kind, a, nb + 1, reply);
}

static uint32_t cue_ms(double t)
{
    double v = floor(t * 1000.0 + 0.5);
    return v <= 0 ? 0 : v >= 4294967294.0 ? 0xfffffffeu : (uint32_t)v;
}

int cb_dbclient_save_cue(CBDbClient *c, uint32_t track, bool remove, const CBPlanCue *cue,
                         bool older, CBDbMsg *reply)
{
    bool hot = cue->hot >= 0, loop = cue->loop_end > cue->time;
    uint32_t in = cue_ms(cue->time), out = loop ? cue_ms(cue->loop_end) : 0xffffffffu;
    CBWr rec = cb_wr_dyn(), ms = cb_wr_dyn();
    if (older) {
        /* The 36-byte record of the older list and its milliseconds. */
        uint32_t flags = (uint32_t)(hot ? cue->hot + 1 : 0) << 16 | loop |
                         (cue->active ? 0x200u : 0);
        const uint32_t words[9] = {
            flags, 0, 0, (uint32_t)((uint64_t)in * 3 / 20),
            loop ? (uint32_t)((uint64_t)out * 3 / 20) : 0xffffffffu, 0, 0, 0, 0,
        };
        for (int k = 0; k < 9; k++)
            cb_wr_u32le(&rec, words[k]);
        cb_wr_u32le(&ms, in);
        cb_wr_u32le(&ms, out);
    } else {
        /* An entry of the extended list as a CDJ-3000X sent one (84 bytes
         * without a comment, cdj3k-emu 2026-10-15): the list's own layout,
         * no neighbours, the comment if there is one, then a length and a
         * hot cue's colour code and RGB, and two bytes to spare. */
        size_t units = cue->name[0] ? cb_link_utf16_units(cue->name) + 1 : 0;
        size_t len = 0x4a + 2 * units + 10;
        cb_wr_u32le(&rec, (uint32_t)len);
        cb_wr_u16le(&rec, hot ? (uint16_t)(cue->hot + 1) : 0);
        cb_wr_u8(&rec, loop ? 2 : 1);
        cb_wr_zeros(&rec, 3);
        cb_wr_u16le(&rec, 1000);
        cb_wr_u32le(&rec, in);
        cb_wr_u32le(&rec, out);
        cb_wr_zeros(&rec, 0x18 - 0x14);
        cb_wr_u16le(&rec, cue->active ? 4 : 0);
        cb_wr_zeros(&rec, 0x20 - 0x1a);
        cb_wr_u32(&rec, 0xffffffff);
        cb_wr_zeros(&rec, 0x34 - 0x24);
        cb_wr_u32le(&rec, (uint32_t)(len - 0x38));
        cb_wr_zeros(&rec, 0x3a - 0x38);
        cb_wr_u8(&rec, hot ? 0 : (uint8_t)CB_CLAMP(cue->color_code, 0, 8));
        cb_wr_zeros(&rec, 0x48 - 0x3b);
        cb_wr_u16le(&rec, (uint16_t)(2 * units));
        if (units)
            cb_wr_utf16(&rec, cue->name, false, true);
        cb_wr_u32le(&rec, 4);
        cb_wr_u8(&rec, hot ? (uint8_t)cue->color_code : 0);
        cb_wr_u8(&rec, hot ? (uint8_t)(cue->rgb >> 16) : 0);
        cb_wr_u8(&rec, hot ? (uint8_t)(cue->rgb >> 8) : 0);
        cb_wr_u8(&rec, hot ? (uint8_t)cue->rgb : 0);
        cb_wr_zeros(&rec, 2);
    }
    int ret = CB_ERROR(ENOMEM);
    if (!rec.err && !ms.err) {
        CBDbArg a[7] = {
            { .type = CB_DB_NUM, .num = rmst(c, 8) },
            { .type = CB_DB_NUM, .num = track },
            { .type = CB_DB_NUM, .num = !remove },
            { .type = CB_DB_NUM, .num = (uint32_t)rec.len },
            { .type = CB_DB_BLOB, .p = rec.p, .len = (uint32_t)rec.len },
            { .type = CB_DB_NUM, .num = (uint32_t)ms.len },
            { .type = CB_DB_BLOB, .p = ms.p, .len = (uint32_t)ms.len },
        };
        ret = request(c, older ? CB_DB_SAVE_CUE : CB_DB_SAVE_CUE_EXT, a, older ? 7 : 5, reply);
    }
    cb_wr_free(&rec);
    cb_wr_free(&ms);
    return ret;
}

/* Renders a menu the server said has `count` rows into rows. */
static int render_all(CBDbClient *c, int location, uint32_t count, CBDbRow **rows, int *nb_rows)
{
    *rows = NULL;
    *nb_rows = 0;
    /* Only what a reply can hold is believed (the rows themselves are
     * counted as they come). */
    CBArray a;
    cb_array_init(&a, sizeof(CBDbRow));
    int ret = 0;
    uint32_t word = rmst(c, location);
    for (uint32_t offset = 0; offset < count && ret >= 0;) {
        uint32_t limit = CB_MIN(count - offset, (uint32_t)PAGE);
        /* A CDJ-3000's render: offset, limit, 0, the total, 0x0c, 1, 0
         * (rbxport's capture); the older one: offset, limit, 0, limit, 0. */
        const uint32_t v8[] = { word, offset, limit, 0, count, 0x0c, 1, 0 };
        const uint32_t v6[] = { word, offset, limit, 0, limit, 0 };
        CBDbMsg m = { .txid = c->txid++, .kind = CB_DB_RENDER, .nb_args = c->legacy ? 6 : 8 };
        for (int i = 0; i < m.nb_args; i++)
            m.args[i] = (CBDbArg) { .type = CB_DB_NUM, .num = c->legacy ? v6[i] : v8[i] };
        if ((ret = send_msg(c, &m)) < 0)
            break;
        CBDbMsg r;
        if ((ret = read_msg(c, &r)) < 0)
            break;
        if (r.kind != CB_DB_MENU_HEADER || r.txid != m.txid) {
            ret = CB_ERROR(EPROTO);
            break;
        }
        uint32_t got = 0;
        while (1) {
            if ((ret = read_msg(c, &r)) < 0)
                break;
            if (r.txid != m.txid) {
                ret = CB_ERROR(EPROTO);
                break;
            }
            if (r.kind == CB_DB_MENU_FOOTER)
                break;
            CBDbRow row;
            if ((ret = cb_dbrow_parse(&r, &row)) < 0)
                break;
            if (!cb_array_push(&a, &row)) {
                cb_dbrow_free(&row);
                ret = CB_ERROR(ENOMEM);
                break;
            }
            got++;
        }
        /* A page short of what was asked: rows the server no longer has. */
        offset += limit;
        if (ret >= 0 && !got && limit)
            break;
    }
    CBDbRow *v = a.data;
    if (ret < 0) {
        cb_dbrows_free(&v, (int)a.nb);
        return ret;
    }
    *rows = v;
    *nb_rows = (int)a.nb;
    return 0;
}

static int menu(CBDbClient *c, uint16_t kind, int location, const CBDbArg *args, int nb,
                CBDbRow **rows, int *nb_rows)
{
    *rows = NULL;
    *nb_rows = 0;
    CBDbMsg r;
    int ret = request(c, kind, args, nb, &r);
    if (ret < 0)
        return ret;
    if (r.kind == CB_DB_ERROR)
        return CB_ERROR(ENOENT);
    if (r.kind != CB_DB_AVAILABLE || cb_dbmsg_num(&r, 0) != kind)
        return CB_ERROR(EPROTO);
    return render_all(c, location, cb_dbmsg_num(&r, 1), rows, nb_rows);
}

int cb_dbclient_menu(CBDbClient *c, uint16_t kind, int location, const uint32_t *args, int nb,
                     CBDbRow **rows, int *nb_rows)
{
    CBDbArg a[CB_DB_MAX_ARGS];
    nb = CB_CLAMP(nb, 0, CB_DB_MAX_ARGS - 1);
    a[0] = (CBDbArg) { .type = CB_DB_NUM, .num = rmst(c, location) };
    for (int i = 0; i < nb; i++)
        a[i + 1] = (CBDbArg) { .type = CB_DB_NUM, .num = args[i] };
    return menu(c, kind, location, a, nb + 1, rows, nb_rows);
}

int cb_dbclient_search(CBDbClient *c, uint16_t kind, const char *text, CBDbRow **rows,
                       int *nb_rows)
{
    /* Sort, the text's byte length with its NUL, the text, 0 (menus.adoc
     * "Search"). */
    const CBDbArg a[5] = {
        { .type = CB_DB_NUM, .num = rmst(c, 1) },
        { .type = CB_DB_NUM, .num = 0 },
        { .type = CB_DB_NUM, .num = 2 * ((uint32_t)cb_link_utf16_units(text) + 1) },
        { .type = CB_DB_STR, .str = text },
        { .type = CB_DB_NUM, .num = 0 },
    };
    return menu(c, kind, 1, a, 5, rows, nb_rows);
}

int cb_dbclient_metadata(CBDbClient *c, uint32_t track, CBDbRow **rows, int *nb_rows)
{
    return cb_dbclient_menu(c, CB_DB_METADATA, 2, &track, 1, rows, nb_rows);
}

int cb_dbclient_track_info(CBDbClient *c, uint32_t track, char **path, uint64_t *size)
{
    *path = NULL;
    *size = 0;
    CBDbRow *rows;
    int nb;
    int ret = cb_dbclient_menu(c, CB_DB_TRACK_INFO, 8, &track, 1, &rows, &nb);
    if (ret < 0)
        return ret;
    ret = CB_ERROR(ENOENT);
    /* The path row: type 0, the size first, the track's id second
     * (rbxport's capture). */
    for (int i = 0; i < nb; i++) {
        if (rows[i].type == CB_DB_ITEM_PATH && rows[i].id == track && rows[i].text[0]) {
            *path = cb_strdup(rows[i].text);
            *size = rows[i].a;
            ret = *path ? 0 : CB_ERROR(ENOMEM);
            break;
        }
    }
    cb_dbrows_free(&rows, nb);
    return ret;
}

/* A data reply's blob: [kind, status, length, blob ...]; -ENOENT when the
 * status says there is none. */
static int take_blob(const CBDbMsg *r, uint16_t want, uint8_t **blob, size_t *size)
{
    *blob = NULL;
    *size = 0;
    if (r->kind == CB_DB_ERROR)
        return CB_ERROR(ENOENT);
    if (r->kind != want || r->nb_args < 4)
        return CB_ERROR(EPROTO);
    uint32_t len;
    const uint8_t *p = cb_dbmsg_blob(r, 3, &len);
    if (cb_dbmsg_num(r, 1) != 0 || !p || !len)
        return CB_ERROR(ENOENT);
    if (!(*blob = malloc(len)))
        return CB_ERROR(ENOMEM);
    memcpy(*blob, p, len);
    *size = len;
    return 0;
}

int cb_dbclient_artwork(CBDbClient *c, uint32_t id, bool large, uint8_t **jpeg, size_t *size)
{
    CBDbMsg r;
    const uint32_t args[2] = { id, 1 };
    int ret = cb_dbclient_request(c, CB_DB_ARTWORK, 8, args, large ? 2 : 1, &r);
    return ret < 0 ? ret : take_blob(&r, CB_DB_ARTWORK_REPLY, jpeg, size);
}

int cb_dbclient_blob(CBDbClient *c, uint16_t kind, uint32_t track, uint8_t **blob, size_t *size)
{
    *blob = NULL;
    *size = 0;
    uint16_t want;
    CBDbArg a[5];
    int nb = 2;
    a[0] = (CBDbArg) { .type = CB_DB_NUM, .num = rmst(c, 8) };
    a[1] = (CBDbArg) { .type = CB_DB_NUM, .num = track };
    switch (kind) {
    case CB_DB_PREVIEW:
        /* 4, the track, 0 and a blob declared and not sent
         * (track_metadata.adoc "Waveform Previews"). */
        want = CB_DB_PREVIEW_REPLY;
        a[1].num = 4;
        a[2] = (CBDbArg) { .type = CB_DB_NUM, .num = track };
        a[3] = (CBDbArg) { .type = CB_DB_NUM, .num = 0 };
        a[4] = (CBDbArg) { .type = CB_DB_BLOB };
        nb = 5;
        break;
    case CB_DB_DETAIL:
    case CB_DB_CUES_EXT:
        want = kind == CB_DB_DETAIL ? CB_DB_DETAIL_REPLY : CB_DB_CUES_EXT_REPLY;
        a[2] = (CBDbArg) { .type = CB_DB_NUM, .num = 0 };
        nb = 3;
        break;
    case CB_DB_GRID:  want = CB_DB_GRID_REPLY; break;
    case CB_DB_VBR:   want = CB_DB_VBR_REPLY; break;
    case CB_DB_CUES:  want = CB_DB_CUES_REPLY; break;
    default:
        return CB_ERROR(EINVAL);
    }
    CBDbMsg r;
    int ret = request(c, kind, a, nb, &r);
    return ret < 0 ? ret : take_blob(&r, want, blob, size);
}

static uint32_t ascii_le(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 4 && s[i]; i++)
        v |= (uint32_t)(uint8_t)s[i] << 8 * i;
    return v;
}

int cb_dbclient_tag(CBDbClient *c, uint32_t track, const char *fourcc, const char *ext,
                    uint8_t **blob, size_t *size)
{
    /* The .2EX file's tags by 2d04, as a CDJ-3000 asks; the others by
     * 2c04 (rbxport's capture). */
    uint16_t kind = !strcmp(ext, "2EX") ? CB_DB_TAG_2EX : CB_DB_TAG;
    const uint32_t args[3] = { track, ascii_le(fourcc), ascii_le(ext) };
    CBDbMsg r;
    int ret = cb_dbclient_request(c, kind, 8, args, 3, &r);
    return ret < 0 ? ret : take_blob(&r, CB_DB_TAG_REPLY, blob, size);
}

/* ------------------------------------------------------------ decoders */

int cb_db_grid_parse(const uint8_t *blob, size_t size, CBBeat **beats, int *nb)
{
    *beats = NULL;
    *nb = 0;
    CBRd r = cb_rd(blob, size);
    /* The layout dbserver.c's grid reply writes (catalog.c). */
    uint32_t magic = cb_rd_u32le(&r), count = cb_rd_u32le(&r), bytes = cb_rd_u32le(&r);
    cb_rd_skip(&r, 8);
    if (r.err || magic != 0x00080000 || bytes != (uint64_t)count * 16 ||
        count > cb_rd_left(&r) / 16)
        return CB_ERROR(EPROTO);
    CBBeat *v = calloc(CB_MAX(count, 1), sizeof(*v));
    if (!v)
        return CB_ERROR(ENOMEM);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t number = cb_rd_u8(&r);
        cb_rd_skip(&r, 1);
        uint16_t tempo = cb_rd_u16le(&r);
        uint32_t ms = cb_rd_u32le(&r);
        cb_rd_skip(&r, 8);
        v[i] = (CBBeat) { .time = ms / 1000.0, .number = number, .bpm = tempo / 100.0 };
    }
    *beats = v;
    *nb = (int)count;
    return 0;
}

int cb_db_cues_ext_parse(const uint8_t *blob, size_t size, CBPlanCue **cues, int *nb)
{
    *cues = NULL;
    *nb = 0;
    CBArray a;
    cb_array_init(&a, sizeof(CBPlanCue));
    size_t pos = 0;
    int ret = 0;
    while (pos < size) {
        CBRd r = cb_rd(blob + pos, size - pos);
        uint32_t len = cb_rd_u32le(&r);
        /* An entry ends before its comment at 0x48 at the shortest; one
         * may end before its comment or colour (track_metadata.adoc). */
        if (r.err || len < 0x14 || len > size - pos) {
            ret = CB_ERROR(EPROTO);
            break;
        }
        CBRd e = cb_rd(blob + pos, len);
        CBPlanCue q = { .hot = -1, .loop_end = -1.0 };
        uint8_t slot = cb_rd_at_u8(&e, 4), shape = cb_rd_at_u8(&e, 6);
        e.pos = 0x0c;
        uint32_t in = cb_rd_u32le(&e), out = cb_rd_u32le(&e);
        q.time = in / 1000.0;
        if (shape == 2 && out != 0xffffffff)
            q.loop_end = out / 1000.0;
        if (slot >= 1 && slot <= 8)
            q.hot = slot - 1;
        if (len >= 0x4a) {
            e.pos = 0x48;
            uint16_t clen = cb_rd_u16le(&e);
            const uint8_t *text = cb_rd_bytes(&e, clen);
            if (text) {
                char *s = cb_link_utf16_to_utf8(text, clen, false);
                if (!s) {
                    ret = CB_ERROR(ENOMEM);
                    break;
                }
                snprintf(q.name, sizeof(q.name), "%s", s);
                free(s);
                cb_rd_skip(&e, 4);
                const uint8_t *col = cb_rd_bytes(&e, 4);
                if (col && q.hot >= 0) {
                    q.color_code = col[0];
                    q.rgb = (uint32_t)col[1] << 16 | (uint32_t)col[2] << 8 | col[3];
                }
            }
            if (q.hot < 0 && len > 0x3a)
                q.color_code = cb_rd_at_u8(&e, 0x3a);
        }
        if (!cb_array_push(&a, &q)) {
            ret = CB_ERROR(ENOMEM);
            break;
        }
        pos += len;
    }
    if (ret < 0) {
        cb_array_free(&a);
        return ret;
    }
    *cues = a.data;
    *nb = (int)a.nb;
    return 0;
}

int cb_db_tag_parse(const uint8_t *blob, size_t size, CBAnlzTag *tag)
{
    *tag = (CBAnlzTag) { 0 };
    CBRd r = cb_rd(blob, size);
    uint32_t padded = cb_rd_u32le(&r);
    const uint8_t *raw = cb_rd_bytes(&r, 12);
    if (r.err || padded > size - 4 || padded < 12)
        return CB_ERROR(EPROTO);
    uint32_t header = cb_rd_at_u32(&r, 8), len = cb_rd_at_u32(&r, 12);
    if (header < 12 || len < header || len > padded)
        return CB_ERROR(EPROTO);
    memcpy(tag->fourcc, raw, 4);
    tag->raw = raw;
    tag->raw_size = len;
    tag->body = raw + 12;
    tag->size = len - 12;
    return 0;
}
