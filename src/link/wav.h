/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link's WAVs (docs/LINK.md, section 4): what a player reads of a track
 * the plan marks `pcm`, a 16-bit stereo WAV made from the track's own file
 * as the player reads it, any range decoded when asked for through the
 * packet store the decks play from (rxbox/audio.h). Nothing is written to
 * disk and nothing is decoded ahead. Used on the service thread only. */

#ifndef RXBOX_LINK_WAV_H
#define RXBOX_LINK_WAV_H

#include <stdint.h>
#include <sys/types.h>

#include "rxbox/formats.h"

#define RX_LINK_WAV_HEADER 44

typedef struct RXLinkWav RXLinkWav;

/* `max_memory`: what the tracks kept open may take (their packets), the
 * least recently read closed first; at least one stays open. */
int  rx_link_wav_new(RXLinkWav **out, size_t max_memory);
void rx_link_wav_free(RXLinkWav **w);

/* Whether the track's file opens as a WAV of it would need: 0, or a
 * negative errno (the plan cannot serve it so, and serves a transcode). */
int  rx_link_wav_check(RXLinkWav *w, const RXPlanTrack *pt);

/* Bytes [offset, offset + n) of the track's WAV into dst: how many (fewer
 * only at its end), or a negative errno. The WAV is the plan's: its
 * total_samples frames at its sample_rate, whatever the decoder makes of
 * the file (silence after where it ends, nothing past total_samples), so
 * its size never changes under a player. */
ssize_t rx_link_wav_read(RXLinkWav *w, const RXPlanTrack *pt, uint64_t offset, uint8_t *dst,
                         size_t n);

/* The header of a WAV of `frames` stereo 16-bit frames at `rate`. */
void rx_link_wav_header(uint8_t h[RX_LINK_WAV_HEADER], int rate, int64_t frames);

#endif /* RXBOX_LINK_WAV_H */
