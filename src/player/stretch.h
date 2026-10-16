/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MASTER TEMPO's pitch shifter: a C interface to Signalsmith Stretch
 * (subprojects/signalsmith-stretch, MIT), a C++ header library, used at a
 * time ratio of one: as many frames come out as go in, transposed by a
 * factor. deck.c feeds it the deck's varispeed audio and sets the factor
 * to 1 / (tempo + bend), which puts the pitch back where the track has it.
 *
 * Threads. cb_stretch_create and cb_stretch_destroy run on the owner
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

#ifndef CYANBOX_PLAYER_STRETCH_H
#define CYANBOX_PLAYER_STRETCH_H

typedef struct CBStretch CBStretch;

/* A shifter for `channels` planar channels at `rate` frames per second. */
int  cb_stretch_create(CBStretch **out, int channels, int rate);
void cb_stretch_destroy(CBStretch *s);

/* Frames from input to output: what comes out is this far behind what went
 * in. Also the number of frames cb_stretch_seek takes. */
int  cb_stretch_latency(const CBStretch *s);
/* Frames from the newest input back to the centre of the block the shifter
 * analyses next: the material the current factor applies to. */
int  cb_stretch_input_latency(const CBStretch *s);

/* The frequency factor, 1.0 = unchanged. Takes effect at the next block. */
void cb_stretch_set_pitch(CBStretch *s, float factor);
/* Forgets all input and output, as after create. */
void cb_stretch_reset(CBStretch *s);
/* Starts over at a new place: forgets everything, then takes `frames`
 * (cb_stretch_latency()) frames of input that begin there, so that the next
 * frame cb_stretch_process returns is the first of them, at full level. */
void cb_stretch_seek(CBStretch *s, const float *const *in, int frames);
/* `frames` frames in, `frames` frames out; in and out must not overlap. */
void cb_stretch_process(CBStretch *s, const float *const *in, float *const *out, int frames);

#endif /* CYANBOX_PLAYER_STRETCH_H */
