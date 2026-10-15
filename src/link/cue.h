/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* LINK CUE's decisions (rxbox/link.h, RXLinkCue): which mixer is answered,
 * and with what. No sockets: a service hands every timing packet that came
 * from a device on its link to rx_link_cue_answer and sends what comes
 * back. Time is passed in, so the tests drive the clock. */

#ifndef RXBOX_LINK_CUE_H
#define RXBOX_LINK_CUE_H

#include <stddef.h>
#include <stdint.h>

#include "rxbox/link.h"

#include "packet.h"

/* A timing `t` from the mixer at `from`, heard by the service `svc` (any
 * pointer that tells the services apart), where we are device `number`
 * and called `name`: the audio packet for the mixer, written into `out`,
 * and its length; 0 to send nothing. */
size_t rx_link_cue_answer(RXLinkCue *c, const void *svc, uint32_t from, int number,
                          const char *name, const RXPktCueTiming *t, int64_t now, uint8_t *out,
                          size_t cap);
/* The service `svc` stops: its mixer, if it is the one answered, is
 * forgotten. */
void rx_link_cue_forget(RXLinkCue *c, const void *svc);
/* rx_link_cue_state at `now`. */
void rx_link_cue_state_at(RXLinkCue *c, RXLinkCueState *out, int64_t now);

#endif /* RXBOX_LINK_CUE_H */
