/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link on every Ethernet interface (cyanbox/link.h, CBLinkSet): a service
 * (service.c) per interface, each bound to its own, so that a player on
 * any of them finds the library. The services bind the same ports, each
 * on its own device, which Linux allows: a port bound to one device does
 * not conflict with the same port bound to another (SO_BINDTODEVICE, and
 * the TCP and NFS sockets on the interface's own address).
 *
 * A thread of the set's own looks at the interfaces every RESCAN_MS and
 * makes the services follow them; starting one reads the library and
 * plans over it, and stopping one waits for a request in progress, so
 * both happen there. A service is taken out of the list under the lock
 * before it is stopped and put in after it started: whoever reads the
 * list under the lock never meets a service being freed.
 *
 * Each interface keeps its NFS session (nfs.h) for as long as the set runs,
 * by name: a service started again there, after its cable or its USB
 * adapter came back, gives out the same handles and knows the decks that
 * mounted, so a deck that was playing reads on. Only this thread hands
 * them out, each to one service at a time. */

#include "cyanbox/link.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cyanbox/common.h"

#include "net.h"
#include "nfs.h"
#include "usb.h"

#define RESCAN_MS   2000
/* A service that would not start is tried again this much later. */
#define RETRY_US    10000000
#define MAX_SERVED  8
/* Interfaces whose NFS session is kept; past that, a service's own. */
#define MAX_SESSIONS 16

typedef struct Served {
    char     name[16];
    uint32_t addr;
    CBLink  *link;              /* NULL while starting or after a failure */
    bool     starting;
    int      err;
    int64_t  retry;             /* when to try a failed one again */
} Served;

struct CBLinkSet {
    CBLinkOptions opt;
    char *library, *interface, *name, *broadcast;
    pthread_t thread;
    bool running;
    int wake;                   /* eventfd: stop, the library changed */
    atomic_bool stop, reload;

    pthread_mutex_t lock;       /* served[] and nb */
    Served served[MAX_SERVED];
    int nb;
    /* The TAG LIST every service shares: the owner's, or the set's own. */
    CBLinkTagList *own_tags;
    CBLinkUsb *usb;             /* an all-in-one on USB: its activation */
    /* Each interface's NFS session, by name (the manager thread's). */
    struct {
        char         name[16];
        CBNfsSession nfs;
    } sessions[MAX_SESSIONS];
    int nb_sessions;
};

typedef struct Want {
    char     name[16];
    uint32_t addr;
} Want;

/* What should be served: the interface the options name (its address 0
 * when it has none yet: tried all the same, and the failure says why),
 * else every Ethernet interface up with an address. */
static int wanted(CBLinkSet *s, Want *w)
{
    if (s->interface) {
        CBLinkIf ifc;
        w[0] = (Want) { 0 };
        snprintf(w[0].name, sizeof(w[0].name), "%s", s->interface);
        if (cb_link_if_find(&ifc, s->interface, NULL, NULL) >= 0)
            w[0].addr = ifc.addr;
        return 1;
    }
    CBLinkInterface ifs[MAX_SERVED];
    int n = CB_MIN(cb_link_interfaces(ifs, MAX_SERVED), MAX_SERVED), nb = 0;
    for (int i = 0; i < n; i++) {
        if (!ifs[i].addr)
            continue;
        w[nb] = (Want) { .addr = ifs[i].addr };
        snprintf(w[nb].name, sizeof(w[nb].name), "%.*s", (int)sizeof(w[nb].name) - 1, ifs[i].name);
        nb++;
    }
    return nb;
}

static int find(const CBLinkSet *s, const char *name)
{
    for (int i = 0; i < s->nb; i++)
        if (!strcmp(s->served[i].name, name))
            return i;
    return -1;
}

/* The interface's NFS session, made the first time; NULL past
 * MAX_SESSIONS. */
static CBNfsSession *session_of(CBLinkSet *s, const char *name)
{
    for (int i = 0; i < s->nb_sessions; i++)
        if (!strcmp(s->sessions[i].name, name))
            return &s->sessions[i].nfs;
    if (s->nb_sessions >= MAX_SESSIONS)
        return NULL;
    int i = s->nb_sessions++;
    snprintf(s->sessions[i].name, sizeof(s->sessions[i].name), "%.*s",
             (int)sizeof(s->sessions[i].name) - 1, name);
    return &s->sessions[i].nfs;
}

static int served_order(const void *a, const void *b)
{
    return strcmp(((const Served *)a)->name, ((const Served *)b)->name);
}

/* The services made to follow the interfaces. */
static void rescan(CBLinkSet *s)
{
    Want want[MAX_SERVED];
    int nw = wanted(s, want);

    /* Gone, or another address: stopped (and started again below). */
    while (1) {
        CBLink *gone = NULL;
        char name[16] = "";
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < s->nb && !gone && !name[0]; i++) {
            Served *v = &s->served[i];
            int k = 0;
            while (k < nw && strcmp(want[k].name, v->name))
                k++;
            if (k < nw && (want[k].addr == v->addr || !v->link))
                continue;
            snprintf(name, sizeof(name), "%s", v->name);
            gone = v->link;
            s->served[i] = s->served[--s->nb];
        }
        pthread_mutex_unlock(&s->lock);
        if (!name[0])
            break;
        if (gone) {
            cb_info("link: %s is gone or has another address: no longer serving there", name);
            cb_link_stop(&gone);
        }
    }

    /* New, or due to be tried again: started. */
    int64_t now = cb_time_us();
    for (int k = 0; k < nw && !atomic_load(&s->stop); k++) {
        pthread_mutex_lock(&s->lock);
        int i = find(s, want[k].name);
        bool due = i < 0 ? s->nb < MAX_SERVED
                         : !s->served[i].link && !s->served[i].starting && now >= s->served[i].retry;
        if (due && i < 0) {
            i = s->nb++;
            s->served[i] = (Served) { .addr = want[k].addr };
            snprintf(s->served[i].name, sizeof(s->served[i].name), "%.*s",
                     (int)sizeof(s->served[i].name) - 1, want[k].name);
            qsort(s->served, (size_t)s->nb, sizeof(*s->served), served_order);
            i = find(s, want[k].name);
        }
        if (due) {
            s->served[i].starting = true;
            s->served[i].addr = want[k].addr;
        }
        pthread_mutex_unlock(&s->lock);
        if (!due)
            continue;

        CBLinkOptions o = s->opt;
        o.interface = want[k].name;
        o.nfs_session = session_of(s, want[k].name);
        CBLink *link = NULL;
        int ret = cb_link_start(&link, &o);

        pthread_mutex_lock(&s->lock);
        i = find(s, want[k].name);
        Served *v = &s->served[i];
        int was = v->err;
        v->starting = false;
        v->link = link;
        v->err = ret < 0 ? ret : 0;
        v->retry = cb_time_us() + RETRY_US;
        pthread_mutex_unlock(&s->lock);
        if (ret < 0 && ret != was)
            cb_warn("link: cannot serve on %s: %s; trying again every %d s", want[k].name,
                    strerror(-ret), RETRY_US / 1000000);
    }
}

static void *manager(void *arg)
{
    CBLinkSet *s = arg;
    while (!atomic_load(&s->stop)) {
        rescan(s);
        if (atomic_exchange(&s->reload, false)) {
            pthread_mutex_lock(&s->lock);
            for (int i = 0; i < s->nb; i++)
                if (s->served[i].link)
                    cb_link_library_changed(s->served[i].link);
            pthread_mutex_unlock(&s->lock);
        }
        struct pollfd p = { .fd = s->wake, .events = POLLIN };
        if (poll(&p, 1, RESCAN_MS) > 0) {
            uint64_t v;
            if (read(s->wake, &v, sizeof(v)) < 0 && errno != EAGAIN)
                cb_debug("link: set wake: %s", strerror(errno));
        }
    }
    while (1) {
        pthread_mutex_lock(&s->lock);
        CBLink *link = s->nb ? s->served[--s->nb].link : NULL;
        bool more = s->nb > 0;
        pthread_mutex_unlock(&s->lock);
        cb_link_stop(&link);
        if (!more)
            break;
    }
    return NULL;
}

static void wake(CBLinkSet *s)
{
    uint64_t v = 1;
    if (write(s->wake, &v, sizeof(v)) < 0)
        cb_debug("link: set wake: %s", strerror(errno));
}

static void set_free(CBLinkSet *s)
{
    if (s->wake >= 0)
        close(s->wake);
    pthread_mutex_destroy(&s->lock);
    free(s->library);
    free(s->interface);
    free(s->name);
    free(s->broadcast);
    cb_link_tag_list_free(&s->own_tags);
    free(s);
}

int cb_link_set_start(CBLinkSet **out, const CBLinkOptions *o)
{
    *out = NULL;
    CBLinkSet *s = calloc(1, sizeof(*s));
    if (!s)
        return CB_ERROR(ENOMEM);
    pthread_mutex_init(&s->lock, NULL);
    s->opt = *o;
    s->library = o->library ? cb_strdup(o->library) : NULL;
    s->interface = o->interface ? cb_strdup(o->interface) : NULL;
    s->name = o->name ? cb_strdup(o->name) : NULL;
    s->broadcast = o->broadcast ? cb_strdup(o->broadcast) : NULL;
    s->opt.library = s->library;
    s->opt.name = s->name;
    s->opt.broadcast = s->broadcast;
    s->opt.interface = NULL;
    s->opt.usb_card = s->opt.test_usb_root = NULL;     /* the set's own, below */
    s->opt.nfs_session = NULL;                          /* each interface's, rescan */
    s->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int ret = 0;
    if (!o->tag_list && cb_link_tag_list_new(&s->own_tags) >= 0)
        s->opt.tag_list = s->own_tags;
    if ((o->library && !s->library) || (o->interface && !s->interface) ||
        (o->name && !s->name) || (o->broadcast && !s->broadcast) || !s->opt.tag_list)
        ret = CB_ERROR(ENOMEM);
    else if (s->wake < 0)
        ret = CB_ERROR(errno);
    else if (o->usb_card)
        ret = cb_link_usb_start(&s->usb, o->usb_card, o->test_usb_root);
    if (!ret && (ret = pthread_create(&s->thread, NULL, manager, s)))
        ret = CB_ERROR(ret);
    if (ret) {
        cb_link_usb_stop(&s->usb);
        set_free(s);
        return ret;
    }
    s->running = true;
    *out = s;
    return 0;
}

void cb_link_set_stop(CBLinkSet **sp)
{
    CBLinkSet *s = *sp;
    if (!s)
        return;
    atomic_store(&s->stop, true);
    wake(s);
    if (s->running)
        pthread_join(s->thread, NULL);
    cb_link_usb_stop(&s->usb);
    set_free(s);
    *sp = NULL;
}

int cb_link_set_interfaces(CBLinkSet *s, CBLinkServed *out, int max)
{
    pthread_mutex_lock(&s->lock);
    int n = s->nb;
    for (int i = 0; i < n && i < max; i++) {
        const Served *v = &s->served[i];
        out[i] = (CBLinkServed) {
            .addr    = v->addr,
            .running = v->link != NULL,
            .err     = v->err,
            .state   = CB_LINK_WAITING,
        };
        snprintf(out[i].interface, sizeof(out[i].interface), "%s", v->name);
        if (v->link)
            out[i].state = cb_link_state(v->link, &out[i].number);
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}

int cb_link_set_players(CBLinkSet *s, CBLinkPlayer *out, int *where, int max)
{
    pthread_mutex_lock(&s->lock);
    int n = 0;
    for (int i = 0; i < s->nb; i++) {
        if (!s->served[i].link)
            continue;
        CBLinkPlayer p[64];
        int k = CB_MIN(cb_link_players(s->served[i].link, p, 64), 64);
        for (int j = 0; j < k; j++, n++) {
            if (n >= max)
                continue;
            out[n] = p[j];
            if (where)
                where[n] = i;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}

int cb_link_set_load(CBLinkSet *s, int where, int player, uint32_t track_id)
{
    pthread_mutex_lock(&s->lock);
    int ret = where >= 0 && where < s->nb && s->served[where].link
            ? cb_link_load(s->served[where].link, player, track_id) : CB_ERROR(ENOENT);
    pthread_mutex_unlock(&s->lock);
    return ret;
}

int cb_link_set_push_settings(CBLinkSet *s, const CBConfig *cfg, int *players, int *mixers)
{
    *players = *mixers = 0;
    int ret = CB_ERROR(EAGAIN);
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->nb; i++) {
        int np, nm;
        int r = s->served[i].link ? cb_link_push_settings(s->served[i].link, cfg, &np, &nm)
                                  : CB_ERROR(EAGAIN);
        if (r >= 0) {
            *players += np;
            *mixers += nm;
            if (ret == CB_ERROR(EAGAIN))
                ret = 0;
        } else if (r != CB_ERROR(EAGAIN)) {
            ret = r;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

int cb_link_set_edits(CBLinkSet *s, CBLinkEdit *out, int max)
{
    pthread_mutex_lock(&s->lock);
    int n = 0;
    for (int i = 0; i < s->nb && n < max; i++) {
        if (s->served[i].link)
            n += cb_link_edits(s->served[i].link, out + n, max - n);
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}

void cb_link_set_tracks_changed(CBLinkSet *s, const uint64_t *ids, int nb)
{
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->nb; i++) {
        if (s->served[i].link)
            cb_link_tracks_changed(s->served[i].link, ids, nb);
    }
    pthread_mutex_unlock(&s->lock);
}

void cb_link_set_library_changed(CBLinkSet *s)
{
    atomic_store(&s->reload, true);
    wake(s);
}
