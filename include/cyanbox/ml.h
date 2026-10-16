/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_ML_H
#define CYANBOX_ML_H

/* MOSS-Audio (OpenMOSS, Apache-2.0), an audio-language model, run on a GPU
 * with Vulkan compute: what a track is (its genre, its mood, its energy)
 * in words and in numbers. The model is read from a GGUF file (the 4B
 * Instruct model, encoder in F16 and Qwen3-4B in Q4_K); its weights are
 * streamed onto the GPU and stay there, never in system memory, for as
 * long as it is open (4.2 GB, plus the context's cache).
 *
 * The model listens once and answers many: cb_ml_listen encodes a track's
 * audio and reads it into the context, and every question after it is
 * asked against that context without reading the audio again.
 *
 * Thread safety: an CBML is used by one thread at a time. Keep one open per
 * GPU: two processes running the model on one RX 6900 XT at once made the
 * driver reset the GPU (docs/ML.md).
 *
 * Environment:
 *   CYANBOX_GPU_DEVICE=NAME  prefer the device whose name contains NAME
 *                          (case-insensitive), unless the options name one
 *   CYANBOX_GPU_VALIDATION=1 the Khronos validation layer
 *   CYANBOX_ML_PROFILE=1     time every kernel, printed at close
 *   CYANBOX_ML_WAVE32=0      the matrix products in the driver's subgroup size, not 32
 *                          (the same results; slower on RDNA) */

#include "cyanbox/colors.h"
#include "cyanbox/common.h"
#include "cyanbox/thread.h"

/* The rate cb_ml_listen wants. */
#define CB_ML_RATE 16000

typedef struct CBML CBML;

typedef struct CBMLOptions {
    const char *device;         /* substring of the GPU's name; NULL: $CYANBOX_GPU_DEVICE, else the
                                   best: discrete before integrated, never a CPU one unless named */
    bool validation;
    int  max_context;           /* tokens of context, audio included; 0: 8192 (about 9 minutes) */
} CBMLOptions;
void cb_ml_options_default(CBMLOptions *o);

/* -ENODEV without a device that can run the model, -EINVAL for a file
 * that is not MOSS-Audio. */
int  cb_ml_open(CBML **ml, const char *path, const CBMLOptions *o);
void cb_ml_close(CBML **ml);
const char *cb_ml_device_name(const CBML *ml);

/* Encodes mono audio at CB_ML_RATE and reads it into the context, replacing
 * what was there. Audio longer than the context holds is cut at the end
 * (-E2BIG when nothing would be left for a question). */
int  cb_ml_listen(CBML *ml, const float *pcm, int64_t nb_samples, CBCancel *cancel);
/* Forgets the audio: the questions after it are asked without any. */
void cb_ml_forget(CBML *ml);

/* The model's answer to `question` about the audio (greedy decoding, at
 * most max_tokens), malloc'd. */
int  cb_ml_ask(CBML *ml, const char *question, int max_tokens, char **answer);

/* The probabilities the model gives the next token after `question` and
 * the start of an answer `prefix` (may be NULL), for each of `choices`
 * (strings that are single tokens; -EINVAL otherwise). probs[] is
 * renormalised over the choices; the share of the whole distribution they
 * held goes to *mass (may be NULL). */
int  cb_ml_choose(CBML *ml, const char *question, const char *prefix,
                  const char *const *choices, int nb, float *probs, float *mass);

/* The log-probability, in nats, that the model's whole answer to `question`
 * (after `prefix`, may be NULL) is each of `answers`, ending there: up to 8
 * tokens each (-EINVAL otherwise). Comparable across answers of different
 * lengths, as the end of the answer is counted. */
int  cb_ml_rank(CBML *ml, const char *question, const char *prefix,
                const char *const *answers, int nb, double *logprob);

/* GPU and wall time of the last listen and of all questions since, in ms. */
void cb_ml_times(const CBML *ml, double *listen_ms, double *ask_ms);

/* Multiple choice: `question`, then `options` one a line as "A. ...",
 * "B. ..." in the order given, then "Answer with the letter only." The
 * answer is scored, not sampled: probs[i] is the probability the model
 * gives option i's letter as the first token of its answer, from one
 * forward pass, renormalised over the letters (the share of the whole
 * distribution they held goes to *mass, may be NULL). 2 to 26 options. */
int  cb_ml_pick(CBML *ml, const char *question, const char *const *options, int nb,
                float *probs, float *mass);

/* ------------------------------------------------------------- tagging */

/* What the model says about a track, for the library: a genre, a colour and
 * a comment.
 *
 * The colour is one of rekordbox's eight (cyanbox/colors.h, ids 1 to 8 in its
 * order: pink red orange yellow green aqua blue purple), each standing for
 * a description of the music, the legend, which the user edits (the
 * settings color.pink ... color.purple; CB_TRACK_VIBE_* by default). The
 * model is asked which of the eight descriptions fits the track best
 * (cb_ml_color), so the colour is its choice among them, and the
 * probability of each is kept beside it. The descriptions are listed in
 * every rotation of their order and the answers combined, so no letter or
 * place in the list is favoured (src/ml/tag.c, docs/ML.md).
 *
 * The measures are kept as well, 0 to 1, for the interface to show:
 * intensity, club, mood (valence) and vocals.
 *
 * Results change when the prompts or the scoring do: CB_ML_TAGS_VERSION goes
 * up, and the library shows what an older version made as older. A legend
 * edited since a track was tagged shows by its hash (cb_ml_legend_hash). */
#define CB_ML_TAGS_VERSION 2

/* The orders cb_ml_tag asks the colour question in: every rotation. */
#define CB_ML_COLOR_ORDERS CB_TRACK_COLORS

typedef struct CBMLTags {
    char    *genre;                         /* "Progressive House", "J-Pop" ..., NULL when none */
    char    *comment;                       /* "Female vocals, Japanese. Bright, uplifting." */
    int      color;                         /* 1..CB_TRACK_COLORS: the description that fits best */
    float    colors[CB_TRACK_COLORS];       /* the probability of each, by id - 1; they sum to 1 */
    uint32_t legend;                        /* cb_ml_legend_hash of the descriptions asked */
    float    club;                          /* made for a dance floor */
    float    intensity;                     /* calm .. intense */
    float    valence;                       /* dark, sad .. bright, happy */
    float    vocals;                        /* 1 when the model hears vocals, else 0 */
} CBMLTags;

/* Listens to the track (cb_ml_listen) and asks what the tags need; the
 * colour from `legend`, CB_TRACK_COLORS descriptions by colour id - 1. */
int  cb_ml_tag(CBML *ml, const float *pcm, int64_t nb_samples,
               const char *const legend[CB_TRACK_COLORS], CBMLTags *tags, CBCancel *cancel);
void cb_ml_tags_free(CBMLTags *tags);

/* The colour question about the audio listened to: which of the legend's
 * descriptions fits best, asked in `orders` orders (1: as given; up to
 * CB_TRACK_COLORS, each a rotation of the list by one more place). probs[]
 * gets the geometric mean of the orders' probabilities, renormalised, by
 * colour id - 1; the mean share of the answer the letters held goes to
 * *mass (may be NULL). Returns the id of the most probable. */
int  cb_ml_color(CBML *ml, const char *const legend[CB_TRACK_COLORS], int orders,
                 float probs[CB_TRACK_COLORS], float *mass);

/* A hash of the legend's text (FNV-1a over the descriptions), never 0: a
 * track tagged under another legend has another. */
uint32_t cb_ml_legend_hash(const char *const legend[CB_TRACK_COLORS]);

#endif /* CYANBOX_ML_H */
