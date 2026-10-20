/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link's WAVs (docs/LINK.md, section 4): what a player reads of a track,
 * a 16-bit WAV at 44.1 kHz of its one or two channels (two for more), made
 * from the track's own file as the player reads it by a pipeline
 * (cyanbox/audio.h) of the player's own. Nothing is written to disk. Used
 * on the service thread only. */

#ifndef CYANBOX_LINK_WAV_H
#define CYANBOX_LINK_WAV_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "cyanbox/formats.h"

#define CB_LINK_WAV_HEADER 44
#define CB_LINK_WAV_RATE   44100
/* Slices a read gives at most, for the largest NFS READ. */
#define CB_LINK_WAV_IOV    16

typedef struct CBLinkWav CBLinkWav;

int  cb_link_wav_new(CBLinkWav **out);
void cb_link_wav_free(CBLinkWav **w);

/* Bytes [offset, offset + n) of the track's WAV, as `peer` reads it: as
 * slices of the header, the pipeline's chunks and silence (*niov of them,
 * at most CB_LINK_WAV_IOV, valid until the next call), how many bytes
 * (fewer only at the end), -EAGAIN when they are not made yet (ask again
 * once one of cb_link_wav_fds is readable), or a negative errno. The WAV
 * is the plan's: its total_samples frames of its channels, whatever the
 * decoder makes of the file (silence after where it ends, nothing past
 * total_samples), so its size never changes under a player. */
ssize_t cb_link_wav_read(CBLinkWav *w, const CBPlanTrack *pt, uint32_t peer, uint64_t offset,
                         size_t n, struct iovec *iov, int *niov);

/* The pipelines' eventfds, readable when one made more (at most `max`);
 * how many. */
int  cb_link_wav_fds(const CBLinkWav *w, int *fds, int max);

/* The header of a WAV of `frames` 16-bit frames of `channels` at `rate`. */
void cb_link_wav_header(uint8_t h[CB_LINK_WAV_HEADER], int rate, int channels, int64_t frames);

#endif /* CYANBOX_LINK_WAV_H */
