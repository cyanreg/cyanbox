/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* An all-in-one on USB (usb.h): rekordbox's activation on the unit's MIDI
 * port, written to the raw MIDI device (/dev/snd/midiCnD0) as bytes, with
 * no MIDI library. */

#include "usb.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "cyanbox/common.h"

/* rbxport's capture of rekordbox with an XDJ-RX3: Pioneer's SysEx, command
 * 0x50, which the RX3's PcControlViewServer::assortSysExCmd takes as
 * "activate" (the cdj session's reading of its firmware 1.20). */
static const uint8_t activate[12] = {
    0xf0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x03, 0x0d, 0x00, 0x50, 0x01, 0xf7,
};

/* rekordbox's cadence, well inside the one-second lease. */
#define EVERY_MS    200
#define RETRY_MS    1000
#define MAX_CARDS   32

struct CBLinkUsb {
    char *card, *root;
    pthread_t thread;
    int wake;                   /* eventfd: stop */
    atomic_bool stop;
    atomic_int sent;
};

/* The card's number: `card` itself when it is one, else the card whose id
 * it is; -1 none. */
static int card_number(const CBLinkUsb *u)
{
    const char *c = u->card;
    if (*c && strspn(c, "0123456789") == strlen(c))
        return strlen(c) <= 2 ? atoi(c) : -1;
    for (int n = 0; n < MAX_CARDS; n++) {
        char path[4096], id[64];
        snprintf(path, sizeof(path), "%s/sys/class/sound/card%d/id", u->root, n);
        FILE *f = fopen(path, "re");
        if (!f)
            continue;
        bool got = fgets(id, sizeof(id), f) != NULL;
        fclose(f);
        size_t len = got ? strlen(id) : 0;
        while (len && isspace((unsigned char)id[len - 1]))
            id[--len] = 0;
        if (got && !strcasecmp(id, c))
            return n;
    }
    return -1;
}

static int open_port(const CBLinkUsb *u, char *path, size_t cap)
{
    int n = card_number(u);
    if (n < 0) {
        snprintf(path, cap, "no such sound card");
        errno = ENOENT;
        return -1;
    }
    snprintf(path, cap, "%s/dev/snd/midiC%dD0", u->root, n);
    return open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
}

static void *run(void *arg)
{
    CBLinkUsb *u = arg;
    int fd = -1;
    bool told = false;          /* that the port is missing, once until it is back */
    int64_t retry = 0;
    while (!atomic_load(&u->stop)) {
        int64_t now = cb_time_us();
        if (fd < 0 && now >= retry) {
            char path[4096];
            fd = open_port(u, path, sizeof(path));
            if (fd >= 0) {
                cb_info("link: Link Export on %s: rekordbox's activation to %s every %d ms",
                        u->card, path, EVERY_MS);
                told = false;
            } else {
                if (!told)
                    cb_warn("link: Link Export on %s: %s (%s); looking again every second",
                            u->card, strerror(errno), path);
                told = true;
                retry = now + RETRY_MS * INT64_C(1000);
            }
        }
        if (fd >= 0) {
            ssize_t w = write(fd, activate, sizeof(activate));
            if (w == (ssize_t)sizeof(activate)) {
                atomic_fetch_add(&u->sent, 1);
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                /* Unplugged: looked for again, by its id. */
                cb_warn("link: Link Export on %s: %s; looking again every second", u->card,
                        strerror(errno));
                told = true;
                close(fd);
                fd = -1;
                retry = now + RETRY_MS * INT64_C(1000);
            }
        }
        struct pollfd p = { .fd = u->wake, .events = POLLIN };
        if (poll(&p, 1, EVERY_MS) > 0) {
            uint64_t v;
            if (read(u->wake, &v, sizeof(v)) < 0 && errno != EAGAIN)
                cb_debug("link: usb wake: %s", strerror(errno));
        }
    }
    if (fd >= 0)
        close(fd);
    return NULL;
}

static void usb_free(CBLinkUsb *u)
{
    if (u->wake >= 0)
        close(u->wake);
    free(u->card);
    free(u->root);
    free(u);
}

int cb_link_usb_start(CBLinkUsb **out, const char *card, const char *root)
{
    *out = NULL;
    if (!card || !*card)
        return CB_ERROR(EINVAL);
    CBLinkUsb *u = calloc(1, sizeof(*u));
    if (!u)
        return CB_ERROR(ENOMEM);
    u->card = cb_strdup(card);
    u->root = cb_strdup(root ? root : "");
    u->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int ret = 0;
    if (!u->card || !u->root)
        ret = CB_ERROR(ENOMEM);
    else if (u->wake < 0)
        ret = CB_ERROR(errno);
    else if ((ret = pthread_create(&u->thread, NULL, run, u)))
        ret = CB_ERROR(ret);
    if (ret) {
        usb_free(u);
        return ret;
    }
    *out = u;
    return 0;
}

void cb_link_usb_stop(CBLinkUsb **up)
{
    CBLinkUsb *u = *up;
    if (!u)
        return;
    atomic_store(&u->stop, true);
    uint64_t v = 1;
    if (write(u->wake, &v, sizeof(v)) < 0)
        cb_debug("link: usb wake: %s", strerror(errno));
    pthread_join(u->thread, NULL);
    usb_free(u);
    *up = NULL;
}

int cb_link_usb_sent(CBLinkUsb *u)
{
    return atomic_load(&u->sent);
}
