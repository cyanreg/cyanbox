/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* LINK CUE (rxbox/link.h, RXLinkCue; docs/LINK.md, "LINK CUE"): which
 * mixer is answered, and with what. The mixer goes by what each audio
 * packet asks (byte 0x24; the DJM-A9's firmware 1.22):
 *
 *   LINK CUE  elected        asked          the mixer
 *   off       none, another  engage, take   elects the sender (handover)
 *   off       any            go on          does nothing
 *   on        none, another  engage, take   elects the sender and plays it
 *   on        the sender     go on, take    plays it: counters one apart,
 *                                           a small gap padded, a big one
 *                                           starts over
 *   on        the sender     engage         stops it
 *   on        none           go on          plays it again if it was the
 *                                           last elected, at its counter
 *
 * so rxbox never asks to engage, which from the source elected stops it.
 * It takes over when its decks start (a deck started, a mixer heard while
 * one plays, nobody elected yet) and then goes on: every timing answered
 * with the next 336 frames while the decks play, LINK CUE is on and the
 * mixer names us. After a pause in what it sent the first packet takes
 * over again: a mixer that ran dry let its source go. A player that takes
 * the cue over keeps it until the decks start again: rxbox does not fight
 * a DJ cueing on a deck. */

#include "rxbox/link.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

#include "cue.h"

/* A mixer not heard for this long is gone, and another may be answered. */
#define MIXER_GONE_US 1000000
/* After this long without sending, the next packet takes over again. */
#define PAUSE_US      50000
/* Audio sent this recently is "sending" in the state. */
#define SENDING_US    100000

struct RXLinkCue {
    pthread_mutex_t lock;
    rx_link_cue_take_fn take;
    void *opaque;
    bool playing;               /* the owner's decks */
    bool claim;                 /* take over at the next timing */
    /* The mixer answered: the service that hears it, its address, its
     * last timing and when (0: none); our number there. */
    const void *svc;
    uint32_t addr;
    int64_t heard;
    RXPktCueTiming last;
    int number;
    int64_t sent;               /* our last audio to it, 0 none */
};

int rx_link_cue_new(RXLinkCue **out, rx_link_cue_take_fn take, void *opaque)
{
    *out = NULL;
    if (!take)
        return RX_ERROR(EINVAL);
    RXLinkCue *c = calloc(1, sizeof(*c));
    if (!c)
        return RX_ERROR(ENOMEM);
    pthread_mutex_init(&c->lock, NULL);
    c->take = take;
    c->opaque = opaque;
    *out = c;
    return 0;
}

void rx_link_cue_free(RXLinkCue **c)
{
    if (!c || !*c)
        return;
    pthread_mutex_destroy(&(*c)->lock);
    free(*c);
    *c = NULL;
}

void rx_link_cue_decks(RXLinkCue *c, bool playing, bool started)
{
    pthread_mutex_lock(&c->lock);
    c->playing = playing;
    c->claim = playing && (started || c->claim);
    pthread_mutex_unlock(&c->lock);
}

void rx_link_cue_state_at(RXLinkCue *c, RXLinkCueState *out, int64_t now)
{
    *out = (RXLinkCueState) { 0 };
    pthread_mutex_lock(&c->lock);
    if (c->heard && now - c->heard < MIXER_GONE_US) {
        out->mixer = true;
        memcpy(out->name, c->last.name, sizeof(out->name));
        out->number = c->last.mixer;
        out->on = c->last.on;
        out->source = c->last.source;
        out->ours = c->last.source && c->last.source == c->number;
        out->sending = c->sent && now - c->sent < SENDING_US;
    }
    pthread_mutex_unlock(&c->lock);
}

void rx_link_cue_state(RXLinkCue *c, RXLinkCueState *out)
{
    rx_link_cue_state_at(c, out, rx_time_us());
}

void rx_link_cue_forget(RXLinkCue *c, const void *svc)
{
    pthread_mutex_lock(&c->lock);
    if (c->svc == svc)
        c->heard = 0;
    pthread_mutex_unlock(&c->lock);
}

size_t rx_link_cue_answer(RXLinkCue *c, const void *svc, uint32_t from, int number,
                          const char *name, const RXPktCueTiming *t, int64_t now, uint8_t *out,
                          size_t cap)
{
    size_t len = 0;
    pthread_mutex_lock(&c->lock);
    bool known = c->heard && now - c->heard < MIXER_GONE_US;
    if (known && (c->svc != svc || c->addr != from)) {
        /* Another mixer while ours is there: one at a time. */
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    if (!known) {
        /* A mixer heard first, or again: asked for the cue while the decks
         * play. */
        c->svc = svc;
        c->addr = from;
        c->sent = 0;
        c->claim = c->playing;
    }
    c->heard = now;
    c->last = *t;
    c->number = number;

    int mode = -1;
    if (!c->playing)
        c->claim = false;
    else if (c->claim || !t->source)
        mode = RX_CUE_TAKE;
    else if (t->source == number && t->on)
        mode = c->sent && now - c->sent < PAUSE_US ? RX_CUE_GO_ON : RX_CUE_TAKE;
    if (mode >= 0) {
        RXPktCueAudio a = { .number = number, .mode = mode, .counter = t->counter, .audio = true };
        snprintf(a.name, sizeof(a.name), "%s", name);
        c->take(c->opaque, a.pcm, RX_CUE_FRAMES);
        RXWr w = rx_wr(out, cap);
        rx_pkt_cue_audio(&w, &a);
        if (!w.err) {
            len = w.len;
            c->sent = now;
            c->claim = false;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return len;
}
