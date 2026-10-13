/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_ANALYSIS_H
#define RXBOX_ANALYSIS_H

/* Track analysis: beat grid, key and the waveform bytes that go on the
 * stick. Everything is offline and deterministic: the same samples give
 * the same result. Input is decoded audio on the players' timeline
 * (rxbox/audio.h); the module mixes to mono and resamples as each stage
 * needs. No stage reads files. Thread safe: each call owns its state, and
 * the library analyses several tracks at once on a pool. */

#include "rxbox/model.h"
#include "rxbox/audio.h"

/* --------------------------------------------------------------- waveforms */

/* The ANLZ waveform tags. Sizes follow rekordbox: the previews have a fixed
 * number of columns, the details 150 columns per second of the players'
 * timeline (for MP3: nb_frames * 1152 * 150 / rate, rounded as dj does). */
#define RX_WAVE_COLUMNS_PER_SECOND 150
#define RX_WAVE_PWAV_COLUMNS 400     /* blue preview, 1 byte: height (5 bits) | whiteness (3 bits) << 5 */
#define RX_WAVE_PWV2_COLUMNS 100     /* tiny preview, 1 byte */
#define RX_WAVE_PWV4_COLUMNS 1200    /* colour preview, 6 bytes: height, ?, ?, r, g, b */
#define RX_WAVE_PWV6_COLUMNS 1200    /* 3-band preview, 3 bytes: low, mid, high (0..127) */

typedef struct RXWaveforms {
    int      nb_detail;              /* columns of the detail tags */
    uint8_t *pwv3;                   /* nb_detail * 1: blue detail */
    uint8_t *pwv5;                   /* nb_detail * 2: colour detail, big-endian rrrgggbbbhhhhh00 */
    uint8_t *pwv7;                   /* nb_detail * 3: 3-band detail */
    uint8_t  pwav[RX_WAVE_PWAV_COLUMNS];
    uint8_t  pwv2[RX_WAVE_PWV2_COLUMNS];
    uint8_t  pwv4[RX_WAVE_PWV4_COLUMNS * 6];
    uint8_t  pwv6[RX_WAVE_PWV6_COLUMNS * 3];
    uint16_t gains[3];               /* PWVC: dj's low, mid, high gains (docs/anlz.md 4.14) */
    int      rate;                   /* sample rate the columns were cut at (0 if unknown) */
} RXWaveforms;

/* From mono audio at the file's native rate on the players' timeline. A
 * byte-faithful port of dj's waveform.py (PWAV reproduces rekordbox on
 * 99.5 % of columns; the others are fitted approximations), except PWV6
 * and PWV7, which follow rbxport's analyser. Applies the firmware
 * minimums: PWAV height >= 2, PWV2 >= 1. */
int  rx_waveforms_compute(const float *mono, int64_t nb_samples, int rate, RXWaveforms *out);
void rx_waveforms_free(RXWaveforms *w);
/* The packed form stored in the library (and the .rxw file format):
 * "RXWV", version, nb_detail, rate, gains, then the tags. Unpack (and load)
 * return -EPROTO for a blob of another format version (versions 1 to 3
 * held older three-band levels): recompute the waveforms; -EINVAL for
 * damaged data. */
int  rx_waveforms_pack(const RXWaveforms *w, uint8_t **data, size_t *size);
int  rx_waveforms_unpack(const uint8_t *data, size_t size, RXWaveforms *w);
int  rx_waveforms_save(const RXWaveforms *w, const char *path);
int  rx_waveforms_load(const char *path, RXWaveforms *w);

/* ------------------------------------------------------------------- beats */

enum RXGridFlag {
    RX_GRID_FLAG_TEMPO_CHANGE     = 1 << 0,   /* a section at another tempo was needed */
    RX_GRID_FLAG_OCTAVE_UNCERTAIN = 1 << 1,   /* half or double tempo scored close */
    RX_GRID_FLAG_PHASE_UNCERTAIN  = 1 << 2,   /* off-beat phase scored close */
    RX_GRID_FLAG_DOWNBEAT_WEAK    = 1 << 3,   /* bar position chosen by a small margin */
    RX_GRID_FLAG_NO_KICKS         = 1 << 4,   /* no kick band to follow; broadband used */
    RX_GRID_FLAG_LOW_CONFIDENCE   = 1 << 5,   /* confidence below 0.5: check by ear */
    RX_GRID_FLAG_UNSTEADY         = 1 << 6,   /* onsets jitter around the line: live drums? */
    RX_GRID_FLAG_PHASE_CUT        = 1 << 7,   /* a section re-enters at the same tempo, shifted */
};
const char *rx_grid_flag_name(enum RXGridFlag f);      /* "tempo-change" ... */
unsigned    rx_grid_flag_parse(const char *name);     /* 0 if unknown */

typedef struct RXAnalysisOptions {
    double bpm_min, bpm_max;     /* 0, 0 = default search range with octave by evidence */
    double prior_bpm;            /* tie-breaker only; 0 = default */
    bool   snap_bpm;             /* snap to integer/half BPM when the drift allows */
    bool   allow_sections;       /* split on real tempo/phase changes (default true) */
    bool   deterministic_threads;/* run single-threaded inside the call (default false) */
} RXAnalysisOptions;
void rx_analysis_options_default(RXAnalysisOptions *o);

/* How the music starts, measured on the audio's level (docs/BEATS.md,
 * "Where the music starts"). */
enum RXStartKind {
    RX_START_AT_ONCE = 0,        /* from the first frames: no silence or floor before it */
    RX_START_HIT,                /* after silence or a noise floor, with a hit */
    RX_START_FADE,               /* after silence or a noise floor, rising for a second or more */
    RX_START_SWELL,              /* after silence, rising faster than that (a riser, a pad) */
};

typedef struct RXBeatResult {
    RXGrid   grid;               /* its first beat is the first at or after music_start */
    double   confidence;         /* 0..1 */
    unsigned flags;              /* RX_GRID_FLAG_* */
    /* Where the music starts (seconds): 0 when at once, a hit's first
     * sample, where a fade or a swell leaves the floor. */
    double   music_start;
    enum RXStartKind start_kind;
    /* Diagnostics for the evaluation tool; may be empty. */
    double   bpm_second_best;    /* the runner-up tempo, 0 if none */
    double   octave_margin;      /* score ratio of the chosen octave to the other */
    double   phase_margin;
} RXBeatResult;

/* Mono audio at `rate` (any; 44.1 kHz is what it works at internally). */
int  rx_analyse_beats(const float *mono, int64_t nb_samples, int rate,
                      const RXAnalysisOptions *opt, RXBeatResult *out);
void rx_beat_result_free(RXBeatResult *r);

/* --------------------------------------------------------------------- key */

typedef struct RXKeyResult {
    char   key[RX_KEY_MAX];      /* classic name, "" if undecidable */
    double confidence;           /* 0..1: calibrated, about the probability the key is right (docs/KEY.md) */
    double tuning_cents;         /* detected tuning offset from A440 */
} RXKeyResult;

int  rx_analyse_key(const float *mono, int64_t nb_samples, int rate, RXKeyResult *out);

/* --------------------------------------------------------------- the whole */

enum RXAnalyseWhat {
    RX_ANALYSE_BEATS = 1 << 0,
    RX_ANALYSE_KEY   = 1 << 1,
    RX_ANALYSE_WAVES = 1 << 2,
    RX_ANALYSE_ALL   = 7,
};

/* The version of each stage's output. Bump one when that stage's output
 * changes (a better grid, a recalibrated key, new waveform levels): the
 * library keeps the version each track's results were made with, and a
 * track whose version is lower is analysed for that stage again, by
 * itself, in the interface and by `rxbox analyze`. 0 is a result made
 * before versions were kept, so it is always older.
 * RX_WAVES_VERSION is the algorithm; the packed blob has its own format
 * version (rx_waveforms_pack), a storage matter. The two usually bump
 * together, because new levels are what makes old blobs unreadable. */
#define RX_BEATS_VERSION 2
#define RX_KEY_VERSION   1
#define RX_WAVES_VERSION 1

/* Which stages of a track (RXAnalyseWhat) were made by an older analyser:
 * among those it has (a grid, a key, waveforms), each whose version is
 * below the current one. Says nothing of locks or missing files. */
static inline unsigned rx_analysis_stale(const RXTrack *t)
{
    unsigned stale = 0;
    if (t->grid.nb_sections > 0 && t->beats_version < RX_BEATS_VERSION)
        stale |= RX_ANALYSE_BEATS;
    if (t->key[0] && t->key_version < RX_KEY_VERSION)
        stale |= RX_ANALYSE_KEY;
    if (t->has_waveforms && t->waves_version < RX_WAVES_VERSION)
        stale |= RX_ANALYSE_WAVES;
    return stale;
}

typedef struct RXAnalysis {
    unsigned     done;           /* which of RXAnalyseWhat succeeded */
    double       duration;
    RXBeatResult beats;
    RXKeyResult  key;
    RXWaveforms  waves;
} RXAnalysis;

/* `audio` is the decoded file on the players' timeline, native rate, any
 * channel count. */
int  rx_analyse(const RXAudio *audio, const RXAnalysisOptions *opt, unsigned what, RXAnalysis *out);
void rx_analysis_free(RXAnalysis *a);

/* The automatic cue: a memory cue on the first downbeat at or after the
 * start of the music, added when the track has no cues yet. */
int  rx_analysis_auto_cues(RXTrack *t, const RXBeatResult *beats);

#endif /* RXBOX_ANALYSIS_H */
