/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_ML_H
#define RXBOX_ML_H

/* MOSS-Audio (OpenMOSS, Apache-2.0), an audio-language model, run on a GPU
 * with Vulkan compute: what a track is (its genre, its mood, its energy)
 * in words and in numbers. The model is read from a GGUF file (the 4B
 * Instruct model, encoder in F16 and Qwen3-4B in Q4_K); its weights are
 * streamed onto the GPU and stay there, never in system memory, for as
 * long as it is open (4.2 GB, plus the context's cache).
 *
 * The model listens once and answers many: rx_ml_listen encodes a track's
 * audio and reads it into the context, and every question after it is
 * asked against that context without reading the audio again.
 *
 * Thread safety: an RXML is used by one thread at a time. Keep one open per
 * GPU: two processes running the model on one RX 6900 XT at once made the
 * driver reset the GPU (docs/ML.md).
 *
 * Environment:
 *   RXBOX_GPU_DEVICE=NAME  prefer the device whose name contains NAME
 *                          (case-insensitive), unless the options name one
 *   RXBOX_GPU_VALIDATION=1 the Khronos validation layer
 *   RXBOX_ML_PROFILE=1     time every kernel, printed at close */

#include "rxbox/common.h"
#include "rxbox/thread.h"

/* The rate rx_ml_listen wants. */
#define RX_ML_RATE 16000

typedef struct RXML RXML;

typedef struct RXMLOptions {
    const char *device;         /* substring of the GPU's name; NULL: $RXBOX_GPU_DEVICE, else the
                                   best: discrete before integrated, never a CPU one unless named */
    bool validation;
    int  max_context;           /* tokens of context, audio included; 0: 8192 (about 9 minutes) */
} RXMLOptions;
void rx_ml_options_default(RXMLOptions *o);

/* -ENODEV without a device that can run the model, -EINVAL for a file
 * that is not MOSS-Audio. */
int  rx_ml_open(RXML **ml, const char *path, const RXMLOptions *o);
void rx_ml_close(RXML **ml);
const char *rx_ml_device_name(const RXML *ml);

/* Encodes mono audio at RX_ML_RATE and reads it into the context, replacing
 * what was there. Audio longer than the context holds is cut at the end
 * (-E2BIG when nothing would be left for a question). */
int  rx_ml_listen(RXML *ml, const float *pcm, int64_t nb_samples, RXCancel *cancel);
/* Forgets the audio: the questions after it are asked without any. */
void rx_ml_forget(RXML *ml);

/* The model's answer to `question` about the audio (greedy decoding, at
 * most max_tokens), malloc'd. */
int  rx_ml_ask(RXML *ml, const char *question, int max_tokens, char **answer);

/* The probabilities the model gives the next token after `question` and
 * the start of an answer `prefix` (may be NULL), for each of `choices`
 * (strings that are single tokens; -EINVAL otherwise). probs[] is
 * renormalised over the choices; the share of the whole distribution they
 * held goes to *mass (may be NULL). */
int  rx_ml_choose(RXML *ml, const char *question, const char *prefix,
                  const char *const *choices, int nb, float *probs, float *mass);

/* The log-probability, in nats, that the model's whole answer to `question`
 * (after `prefix`, may be NULL) is each of `answers`, ending there: up to 8
 * tokens each (-EINVAL otherwise). Comparable across answers of different
 * lengths, as the end of the answer is counted. */
int  rx_ml_rank(RXML *ml, const char *question, const char *prefix,
                const char *const *answers, int nb, double *logprob);

/* GPU and wall time of the last listen and of all questions since, in ms. */
void rx_ml_times(const RXML *ml, double *listen_ms, double *ask_ms);

/* ------------------------------------------------------------- tagging */

/* What the model says about a track, for the library: a genre, a colour and
 * a comment. The colour is a hue (rxbox's track colour: 0 red, 120 green,
 * 180 cyan, 240 blue) that places the track by mood and intensity, as the
 * angle on their plane:
 *
 *     0 red      intense
 *    45 orange   intense and bright
 *    90 yellow   bright
 *   135 green    bright and relaxed
 *   180 cyan     calm
 *   225 blue     calm and sad
 *   270 violet   dark
 *   315 magenta  dark and intense
 *
 * continuously, so similar tracks get similar hues (src/ml/tag.c says how
 * the two are measured). The measures are kept as well, 0 to 1, for the
 * interface to show.
 *
 * Results change when the prompts or the mapping do: RX_ML_TAGS_VERSION goes
 * up, and a library re-tags what an older version made. */
#define RX_ML_TAGS_VERSION 1

typedef struct RXMLTags {
    char  *genre;               /* "Progressive House", "J-Pop" ..., NULL when none */
    char  *comment;             /* "Female vocals, Japanese. Bright, uplifting." */
    double hue;                 /* 0 <= hue < 360 */
    float  club;                /* made for a dance floor */
    float  intensity;           /* calm .. intense */
    float  valence;             /* dark, sad .. bright, happy */
    float  vocals;              /* 1 when the model hears vocals, else 0 */
} RXMLTags;

/* Listens to the track (rx_ml_listen) and asks what the tags need. */
int  rx_ml_tag(RXML *ml, const float *pcm, int64_t nb_samples, RXMLTags *tags,
               RXCancel *cancel);
void rx_ml_tags_free(RXMLTags *tags);

#endif /* RXBOX_ML_H */
