/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link on every Ethernet interface (rxbox/link.h, RXLinkSet): a service
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
 * list under the lock never meets a service being freed. */

#include "rxbox/link.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "rxbox/common.h"

#include "net.h"

#define RESCAN_MS   2000
/* A service that would not start is tried again this much later. */
#define RETRY_US    10000000
#define MAX_SERVED  8

typedef struct Served {
    char     name[16];
    uint32_t addr;
    RXLink  *link;              /* NULL while starting or after a failure */
    bool     starting;
    int      err;
    int64_t  retry;             /* when to try a failed one again */
} Served;

struct RXLinkSet {
    RXLinkOptions opt;
    char *library, *interface, *name, *broadcast;
    pthread_t thread;
    bool running;
    int wake;                   /* eventfd: stop, the library changed */
    atomic_bool stop, reload;

    pthread_mutex_t lock;       /* served[] and nb */
    Served served[MAX_SERVED];
    int nb;
};

typedef struct Want {
    char     name[16];
    uint32_t addr;
} Want;

/* What should be served: the interface the options name (its address 0
 * when it has none yet: tried all the same, and the failure says why),
 * else every Ethernet interface up with an address. */
static int wanted(RXLinkSet *s, Want *w)
{
    if (s->interface) {
        RXLinkIf ifc;
        w[0] = (Want) { 0 };
        snprintf(w[0].name, sizeof(w[0].name), "%s", s->interface);
        if (rx_link_if_find(&ifc, s->interface, NULL, NULL) >= 0)
            w[0].addr = ifc.addr;
        return 1;
    }
    RXLinkInterface ifs[MAX_SERVED];
    int n = RX_MIN(rx_link_interfaces(ifs, MAX_SERVED), MAX_SERVED), nb = 0;
    for (int i = 0; i < n; i++) {
        if (!ifs[i].addr)
            continue;
        w[nb] = (Want) { .addr = ifs[i].addr };
        snprintf(w[nb].name, sizeof(w[nb].name), "%.*s", (int)sizeof(w[nb].name) - 1, ifs[i].name);
        nb++;
    }
    return nb;
}

static int find(const RXLinkSet *s, const char *name)
{
    for (int i = 0; i < s->nb; i++)
        if (!strcmp(s->served[i].name, name))
            return i;
    return -1;
}

static int served_order(const void *a, const void *b)
{
    return strcmp(((const Served *)a)->name, ((const Served *)b)->name);
}

/* The services made to follow the interfaces. */
static void rescan(RXLinkSet *s)
{
    Want want[MAX_SERVED];
    int nw = wanted(s, want);

    /* Gone, or another address: stopped (and started again below). */
    while (1) {
        RXLink *gone = NULL;
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
            rx_info("link: %s is gone or has another address: no longer serving there", name);
            rx_link_stop(&gone);
        }
    }

    /* New, or due to be tried again: started. */
    int64_t now = rx_time_us();
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

        RXLinkOptions o = s->opt;
        o.interface = want[k].name;
        RXLink *link = NULL;
        int ret = rx_link_start(&link, &o);

        pthread_mutex_lock(&s->lock);
        i = find(s, want[k].name);
        Served *v = &s->served[i];
        int was = v->err;
        v->starting = false;
        v->link = link;
        v->err = ret < 0 ? ret : 0;
        v->retry = rx_time_us() + RETRY_US;
        pthread_mutex_unlock(&s->lock);
        if (ret < 0 && ret != was)
            rx_warn("link: cannot serve on %s: %s; trying again every %d s", want[k].name,
                    strerror(-ret), RETRY_US / 1000000);
    }
}

static void *manager(void *arg)
{
    RXLinkSet *s = arg;
    while (!atomic_load(&s->stop)) {
        rescan(s);
        if (atomic_exchange(&s->reload, false)) {
            pthread_mutex_lock(&s->lock);
            for (int i = 0; i < s->nb; i++)
                if (s->served[i].link)
                    rx_link_library_changed(s->served[i].link);
            pthread_mutex_unlock(&s->lock);
        }
        struct pollfd p = { .fd = s->wake, .events = POLLIN };
        if (poll(&p, 1, RESCAN_MS) > 0) {
            uint64_t v;
            if (read(s->wake, &v, sizeof(v)) < 0 && errno != EAGAIN)
                rx_debug("link: set wake: %s", strerror(errno));
        }
    }
    while (1) {
        pthread_mutex_lock(&s->lock);
        RXLink *link = s->nb ? s->served[--s->nb].link : NULL;
        bool more = s->nb > 0;
        pthread_mutex_unlock(&s->lock);
        rx_link_stop(&link);
        if (!more)
            break;
    }
    return NULL;
}

static void wake(RXLinkSet *s)
{
    uint64_t v = 1;
    if (write(s->wake, &v, sizeof(v)) < 0)
        rx_debug("link: set wake: %s", strerror(errno));
}

static void set_free(RXLinkSet *s)
{
    if (s->wake >= 0)
        close(s->wake);
    pthread_mutex_destroy(&s->lock);
    free(s->library);
    free(s->interface);
    free(s->name);
    free(s->broadcast);
    free(s);
}

int rx_link_set_start(RXLinkSet **out, const RXLinkOptions *o)
{
    *out = NULL;
    RXLinkSet *s = calloc(1, sizeof(*s));
    if (!s)
        return RX_ERROR(ENOMEM);
    pthread_mutex_init(&s->lock, NULL);
    s->opt = *o;
    s->library = o->library ? rx_strdup(o->library) : NULL;
    s->interface = o->interface ? rx_strdup(o->interface) : NULL;
    s->name = o->name ? rx_strdup(o->name) : NULL;
    s->broadcast = o->broadcast ? rx_strdup(o->broadcast) : NULL;
    s->opt.library = s->library;
    s->opt.name = s->name;
    s->opt.broadcast = s->broadcast;
    s->opt.interface = NULL;
    s->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int ret = 0;
    if ((o->library && !s->library) || (o->interface && !s->interface) ||
        (o->name && !s->name) || (o->broadcast && !s->broadcast))
        ret = RX_ERROR(ENOMEM);
    else if (s->wake < 0)
        ret = RX_ERROR(errno);
    else if ((ret = pthread_create(&s->thread, NULL, manager, s)))
        ret = RX_ERROR(ret);
    if (ret) {
        set_free(s);
        return ret;
    }
    s->running = true;
    *out = s;
    return 0;
}

void rx_link_set_stop(RXLinkSet **sp)
{
    RXLinkSet *s = *sp;
    if (!s)
        return;
    atomic_store(&s->stop, true);
    wake(s);
    if (s->running)
        pthread_join(s->thread, NULL);
    set_free(s);
    *sp = NULL;
}

int rx_link_set_interfaces(RXLinkSet *s, RXLinkServed *out, int max)
{
    pthread_mutex_lock(&s->lock);
    int n = s->nb;
    for (int i = 0; i < n && i < max; i++) {
        const Served *v = &s->served[i];
        out[i] = (RXLinkServed) {
            .addr    = v->addr,
            .running = v->link != NULL,
            .err     = v->err,
            .state   = RX_LINK_WAITING,
        };
        snprintf(out[i].interface, sizeof(out[i].interface), "%s", v->name);
        if (v->link)
            out[i].state = rx_link_state(v->link, &out[i].number);
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}

int rx_link_set_players(RXLinkSet *s, RXLinkPlayer *out, int *where, int max)
{
    pthread_mutex_lock(&s->lock);
    int n = 0;
    for (int i = 0; i < s->nb; i++) {
        if (!s->served[i].link)
            continue;
        RXLinkPlayer p[64];
        int k = RX_MIN(rx_link_players(s->served[i].link, p, 64), 64);
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

void rx_link_set_library_changed(RXLinkSet *s)
{
    atomic_store(&s->reload, true);
    wake(s);
}
