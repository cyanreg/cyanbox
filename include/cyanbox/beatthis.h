/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_BEATTHIS_H
#define CYANBOX_BEATTHIS_H

/* Beat This! (Foscarin, Schlüter and Widmer, ISMIR 2024; CPJKU, MIT), a beat
 * and downbeat tracker, run on a GPU with Vulkan compute: its frame
 * activations, 50 a second, as its reference code (beat_this.inference's
 * Audio2Frames) makes them. What to make of them is the caller's: this is
 * the network, not a postprocessor.
 *
 * The model is read from a GGUF file (beat-this-f16.gguf: the final0
 * checkpoint in F16, its batch norms folded) onto the GPU, about 40 MB.
 * It has a VkDevice of its own, as cyanbox/ml.h's model has (src/ml/ml_priv.h
 * says why). Thread safety: one thread at a time.
 *
 * Environment: as cyanbox/ml.h (CYANBOX_GPU_DEVICE, CYANBOX_GPU_VALIDATION,
 * CYANBOX_ML_PROFILE). */

#include "cyanbox/common.h"

/* The rate the model wants, and its frames a second. */
#define CB_BEATTHIS_RATE 22050
#define CB_BEATTHIS_FPS  50

typedef struct CBBeatThis CBBeatThis;

typedef struct CBBeatThisOptions {
    const char *device;         /* as CBMLOptions.device */
    bool validation;
    int  batch;                 /* chunks of 30 s through the network at once; 0: 4 (about
                                   90 MB of GPU memory each) */
} CBBeatThisOptions;
void cb_beatthis_options_default(CBBeatThisOptions *o);

/* -ENODEV without a device that can run it, -EINVAL for a file that is not
 * Beat This!. */
int  cb_beatthis_open(CBBeatThis **bt, const char *path, const CBBeatThisOptions *o);
void cb_beatthis_close(CBBeatThis **bt);
const char *cb_beatthis_device_name(const CBBeatThis *bt);

/* The beat and downbeat logits of mono audio at CB_BEATTHIS_RATE, frame f at
 * f / 50 s: *nb_frames = 1 + nb_samples / 441 of each, malloc'd. The beat's
 * are the sum head's (beat plus downbeat), as the model's output. */
int  cb_beatthis_run(CBBeatThis *bt, const float *pcm, int64_t nb_samples, float **beat,
                     float **downbeat, int *nb_frames);

/* Wall time of the last run, in ms: the spectrogram, and the network. */
void cb_beatthis_times(const CBBeatThis *bt, double *mel_ms, double *net_ms);

#endif /* CYANBOX_BEATTHIS_H */
