/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MASTER TEMPO's pitch shifter: a C interface to Signalsmith Stretch
 * (subprojects/signalsmith-stretch, MIT), a C++ header library, used at a
 * time ratio of one: as many frames come out as go in, transposed by a
 * factor. deck.c feeds it the deck's varispeed audio and sets the factor
 * to 1 / (tempo + bend), which puts the pitch back where the track has it.
 *
 * Threads. rx_stretch_create and rx_stretch_destroy run on the owner
 * thread and allocate or free everything. The rest runs on the real-time
 * thread and never allocates, locks or logs: Stretch sizes every buffer in
 * configure(), and process(), seek(), outputSeek() and reset() only resize
 * vectors within the capacity configure() gave them (signalsmith-stretch.h:
 * tmpProcessBuffer, tmpPreRollBuffer, and peaks, at most bands / 2 with
 * bands a multiple of 16 by signalsmith-linear's fastSizeAbove; the STFT's
 * input and output are copied by assign() at equal sizes). Measured too,
 * with a counting malloc around the player's mix: no allocation in 50
 * rounds of seeks, loops, pauses, scrubs and switches with the shifter on.
 * Errors are negative errno values. */

#ifndef RXBOX_PLAYER_STRETCH_H
#define RXBOX_PLAYER_STRETCH_H

typedef struct RXStretch RXStretch;

/* A shifter for `channels` planar channels at `rate` frames per second. */
int  rx_stretch_create(RXStretch **out, int channels, int rate);
void rx_stretch_destroy(RXStretch *s);

/* Frames from input to output: what comes out is this far behind what went
 * in. Also the number of frames rx_stretch_seek takes. */
int  rx_stretch_latency(const RXStretch *s);
/* Frames from the newest input back to the centre of the block the shifter
 * analyses next: the material the current factor applies to. */
int  rx_stretch_input_latency(const RXStretch *s);

/* The frequency factor, 1.0 = unchanged. Takes effect at the next block. */
void rx_stretch_set_pitch(RXStretch *s, float factor);
/* Forgets all input and output, as after create. */
void rx_stretch_reset(RXStretch *s);
/* Starts over at a new place: forgets everything, then takes `frames`
 * (rx_stretch_latency()) frames of input that begin there, so that the next
 * frame rx_stretch_process returns is the first of them, at full level. */
void rx_stretch_seek(RXStretch *s, const float *const *in, int frames);
/* `frames` frames in, `frames` frames out; in and out must not overlap. */
void rx_stretch_process(RXStretch *s, const float *const *in, float *const *out, int frames);

#endif /* RXBOX_PLAYER_STRETCH_H */
