/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_ANALYSIS_H
#define CYANBOX_ANALYSIS_H

/* Track analysis: beat grid, key and the waveform bytes that go on the
 * stick. Everything is offline and deterministic: the same samples give
 * the same result. Input is decoded audio on the players' timeline
 * (cyanbox/audio.h); the module mixes to mono and resamples as each stage
 * needs. No stage reads files. Thread safe: each call owns its state, and
 * the library analyses several tracks at once on a pool. */

#include "cyanbox/model.h"
#include "cyanbox/audio.h"
#include "cyanbox/compute.h"

/* --------------------------------------------------------------- waveforms */

/* The ANLZ waveform tags. Sizes follow rekordbox: the previews have a fixed
 * number of columns, the details 150 columns per second of the players'
 * timeline (for MP3: nb_frames * 1152 * 150 / rate, rounded as dj does). */
#define CB_WAVE_COLUMNS_PER_SECOND 150
#define CB_WAVE_PWAV_COLUMNS 400     /* blue preview, 1 byte: height (5 bits) | whiteness (3 bits) << 5 */
#define CB_WAVE_PWV2_COLUMNS 100     /* tiny preview, 1 byte */
#define CB_WAVE_PWV4_COLUMNS 1200    /* colour preview, 6 bytes: height, ?, ?, r, g, b */
#define CB_WAVE_PWV6_COLUMNS 1200    /* 3-band preview, 3 bytes: low, mid, high (0..127) */

typedef struct CBWaveforms {
    int      nb_detail;              /* columns of the detail tags */
    uint8_t *pwv3;                   /* nb_detail * 1: blue detail */
    uint8_t *pwv5;                   /* nb_detail * 2: colour detail, big-endian rrrgggbbbhhhhh00 */
    uint8_t *pwv7;                   /* nb_detail * 3: 3-band detail */
    uint8_t  pwav[CB_WAVE_PWAV_COLUMNS];
    uint8_t  pwv2[CB_WAVE_PWV2_COLUMNS];
    uint8_t  pwv4[CB_WAVE_PWV4_COLUMNS * 6];
    uint8_t  pwv6[CB_WAVE_PWV6_COLUMNS * 3];
    uint16_t gains[3];               /* PWVC: dj's low, mid, high gains (docs/anlz.md 4.14) */
    int      rate;                   /* sample rate the columns were cut at (0 if unknown) */
} CBWaveforms;

/* From mono audio at the file's native rate on the players' timeline. A
 * byte-faithful port of dj's waveform.py (PWAV reproduces rekordbox on
 * 99.5 % of columns; the others are fitted approximations), except PWV6
 * and PWV7, which follow rbxport's analyser. Applies the firmware
 * minimums: PWAV height >= 2, PWV2 >= 1. */
int  cb_waveforms_compute(const float *mono, int64_t nb_samples, int rate, CBWaveforms *out);
void cb_waveforms_free(CBWaveforms *w);
/* The packed form stored in the library (and the .cbw file format):
 * "CBWV", version, nb_detail, rate, gains, then the tags, the previews
 * first: its first CB_WAVE_PACKED_PREVIEW bytes are the header and the
 * previews, which the library keeps apart from the detail after them.
 * Unpack (and load) return -EPROTO for a blob of another format version:
 * recompute the waveforms; -EINVAL for damaged data. unpack_previews
 * takes those first bytes alone and leaves the detail out (nb_detail 0).
 * check_head says the same of a blob's first 8 bytes alone. */
#define CB_WAVE_PACKED_PREVIEW (24 + CB_WAVE_PWAV_COLUMNS + CB_WAVE_PWV2_COLUMNS + \
                                CB_WAVE_PWV4_COLUMNS * 6 + CB_WAVE_PWV6_COLUMNS * 3)
int  cb_waveforms_pack(const CBWaveforms *w, uint8_t **data, size_t *size);
int  cb_waveforms_unpack(const uint8_t *data, size_t size, CBWaveforms *w);
int  cb_waveforms_unpack_previews(const uint8_t *data, size_t size, CBWaveforms *w);
int  cb_waveforms_check_head(const uint8_t *head, size_t size);
int  cb_waveforms_save(const CBWaveforms *w, const char *path);
int  cb_waveforms_load(const char *path, CBWaveforms *w);

/* ------------------------------------------------------------------- beats */

enum CBGridFlag {
    CB_GRID_FLAG_TEMPO_CHANGE     = 1 << 0,   /* a section at another tempo was needed */
    CB_GRID_FLAG_OCTAVE_UNCERTAIN = 1 << 1,   /* half or double tempo scored close */
    CB_GRID_FLAG_PHASE_UNCERTAIN  = 1 << 2,   /* off-beat phase scored close */
    CB_GRID_FLAG_DOWNBEAT_WEAK    = 1 << 3,   /* bar position chosen by a small margin */
    CB_GRID_FLAG_NO_KICKS         = 1 << 4,   /* no kick band to follow; broadband used */
    CB_GRID_FLAG_LOW_CONFIDENCE   = 1 << 5,   /* confidence below 0.5: check by ear */
    CB_GRID_FLAG_UNSTEADY         = 1 << 6,   /* onsets jitter around the line: live drums? */
    CB_GRID_FLAG_PHASE_CUT        = 1 << 7,   /* a section re-enters at the same tempo, shifted */
    CB_GRID_FLAG_DYNAMIC          = 1 << 8,   /* the grid follows the beats one by one: the tempo moves */
};
const char *cb_grid_flag_name(enum CBGridFlag f);      /* "tempo-change" ... */
unsigned    cb_grid_flag_parse(const char *name);     /* 0 if unknown */

/* A beat network: Beat This!'s (cyanbox/beatthis.h), whose frame
 * activations the grid is made with (cb_beats_hybrid). The analysis does
 * not open it itself: whoever owns the model (the jobs pool, a tool)
 * hands this in. `run` takes mono audio at CB_BEAT_NET_RATE and returns
 * the beat and downbeat logits, CB_BEAT_NET_FPS a second, malloc'd (the
 * caller frees them), or a negative errno; it may be called from any
 * thread, so it serialises its own use of the model. */
#define CB_BEAT_NET_RATE 22050
#define CB_BEAT_NET_FPS  50
typedef struct CBBeatNet {
    int  (*run)(void *opaque, const float *pcm, int64_t nb_samples, float **beat,
                float **downbeat, int *nb_frames);
    void  *opaque;
} CBBeatNet;

typedef struct CBAnalysisOptions {
    double bpm_min, bpm_max;     /* 0, 0 = default search range with octave by evidence */
    double prior_bpm;            /* tie-breaker only; 0 = default */
    bool   snap_bpm;             /* snap to integer/half BPM when the drift allows */
    bool   allow_sections;       /* split on real tempo/phase changes (default true) */
    /* Keep the measured beats where a straight line does not hold them
     * (a dynamic grid, docs/BEATS.md); false: the best straight sections
     * everywhere (default true). */
    bool   allow_dynamic;
    bool   deterministic_threads;/* run single-threaded inside the call (default false) */
    /* The GPU the beat analyser's spectral front end runs on (its STFT and
     * band envelopes, cyanbox/compute.h), NULL: the CPU. The results agree
     * with the CPU's to float rounding (docs/BEATS.md, section 1); should
     * the GPU fail, the CPU takes over. Not owned; it must outlive the
     * call. Default NULL. */
    CBCompute *compute;
    /* Beat This!, which cb_analyse's grid is made with (cb_beats_hybrid,
     * docs/BEATS.md, "With Beat This!"): without it cb_analyse's beats
     * fail (-ENODEV). The jobs pool supplies its own (cb_jobs_set_beat_model).
     * cb_analyse_beats, cyanbox's own analysis, does not use it. Default
     * NULL; not owned. */
    const CBBeatNet *beat_net;
} CBAnalysisOptions;
void cb_analysis_options_default(CBAnalysisOptions *o);

/* How the music starts, measured on the audio's level (docs/BEATS.md,
 * "Where the music starts"). */
enum CBStartKind {
    CB_START_AT_ONCE = 0,        /* from the first frames: no silence or floor before it */
    CB_START_HIT,                /* after silence or a noise floor, with a hit */
    CB_START_FADE,               /* after silence or a noise floor, rising for a second or more */
    CB_START_SWELL,              /* after silence, rising faster than that (a riser, a pad) */
};

typedef struct CBBeatResult {
    CBGrid   grid;               /* its first beat is the first at or after music_start */
    double   confidence;         /* 0..1 */
    unsigned flags;              /* CB_GRID_FLAG_* */
    /* Where the music starts (seconds): 0 when at once, a hit's first
     * sample, where a fade or a swell leaves the floor. */
    double   music_start;
    enum CBStartKind start_kind;
    /* Diagnostics for the evaluation tool; may be empty. */
    double   bpm_second_best;    /* the runner-up tempo, 0 if none */
    double   octave_margin;      /* score ratio of the chosen octave to the other */
    double   phase_margin;
} CBBeatResult;

/* cyanbox's own analysis of the beats, the first stage of the grid
 * (cb_beats_hybrid takes it from there). Mono audio at `rate` (any;
 * 44.1 kHz is what it works at internally). */
int  cb_analyse_beats(const float *mono, int64_t nb_samples, int rate,
                      const CBAnalysisOptions *opt, CBBeatResult *out);
void cb_beat_result_free(CBBeatResult *r);

/* The grid with Beat This! (docs/BEATS.md, "With Beat This!"): from its
 * beat and downbeat logits of the audio (CBBeatNet) and cyanbox's own
 * result for it (cb_analyse_beats), which it replaces in `res`: the grid;
 * the confidence, how well the grid explains Beat This!'s detections (the
 * F-measure of beats and detections within 50 ms); the flags, the new
 * grid's shape (tempo-change, phase-cut, dynamic), low-confidence, and of
 * cyanbox's unsteady. Where the music starts and the diagnostics stay
 * cyanbox's. Where Beat This! hears too little to make a grid of (no
 * beat, under 8 s), `res` stays as it was. `duration`: the audio's, s.
 * Deterministic, the CPU's alone. */
int  cb_beats_hybrid(const float *beat, const float *downbeat, int nb_frames, double duration,
                     CBBeatResult *res);

/* --------------------------------------------------------------------- key */

typedef struct CBKeyResult {
    char   key[CB_KEY_MAX];      /* classic name, "" if undecidable */
    double confidence;           /* 0..1: calibrated, about the probability the key is right (docs/KEY.md) */
    double tuning_cents;         /* detected tuning offset from A440 */
} CBKeyResult;

int  cb_analyse_key(const float *mono, int64_t nb_samples, int rate, CBKeyResult *out);

/* --------------------------------------------------------------- the whole */

enum CBAnalyseWhat {
    CB_ANALYSE_BEATS = 1 << 0,
    CB_ANALYSE_KEY   = 1 << 1,
    CB_ANALYSE_WAVES = 1 << 2,
    CB_ANALYSE_ALL   = 7,
};

/* The version of each stage's output. Bump one when that stage's output
 * changes (a better grid, a recalibrated key, new waveform levels): the
 * library keeps the version each track's results were made with, and a
 * track whose version is lower is analysed for that stage again, by
 * itself, in the interface and by `cyanbox analyze`. 0 is a result made
 * before versions were kept, so it is always older.
 * CB_WAVES_VERSION is the algorithm; the packed blob has its own format
 * version (cb_waveforms_pack), a storage matter. The two usually bump
 * together, because new levels are what makes old blobs unreadable. */
#define CB_BEATS_VERSION 9
#define CB_KEY_VERSION   1
#define CB_WAVES_VERSION 1

/* Which stages of a track (CBAnalyseWhat) were made by an older analyser:
 * among those it has (a grid, a key, waveforms), each whose version is
 * below the current one. Says nothing of locks or missing files. */
static inline unsigned cb_analysis_stale(const CBTrack *t)
{
    /* A grid or a key an older analyser made, or one an older analyser
     * looked for and did not find (the version is set either way: a
     * track left without a grid by an analyser bug is looked at again
     * once the version moves). */
    unsigned stale = 0;
    if ((t->grid.nb_sections > 0 || t->beats_version > 0) && t->beats_version < CB_BEATS_VERSION)
        stale |= CB_ANALYSE_BEATS;
    if ((t->key[0] || t->key_version > 0) && t->key_version < CB_KEY_VERSION)
        stale |= CB_ANALYSE_KEY;
    if (t->has_waveforms && t->waves_version < CB_WAVES_VERSION)
        stale |= CB_ANALYSE_WAVES;
    return stale;
}

typedef struct CBAnalysis {
    unsigned     done;           /* which of CBAnalyseWhat succeeded */
    double       duration;
    CBBeatResult beats;
    CBKeyResult  key;
    CBWaveforms  waves;
} CBAnalysis;

/* `audio` is the decoded file on the players' timeline, native rate, any
 * channel count. */
int  cb_analyse(const CBAudio *audio, const CBAnalysisOptions *opt, unsigned what, CBAnalysis *out);
void cb_analysis_free(CBAnalysis *a);

/* The automatic cue: a memory cue on the first downbeat at or after the
 * start of the music, added when the track has no cues yet. */
int  cb_analysis_auto_cues(CBTrack *t, const CBBeatResult *beats);

#endif /* CYANBOX_ANALYSIS_H */
