/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_MODEL_H
#define RXBOX_MODEL_H

/* The data model shared by analysis, the library, the player, the interface and
 * the stick writers: a track, its beat grid and its cues. Pure data plus the
 * arithmetic on grids; nothing here touches files. */

#include <math.h>
#include "rxbox/common.h"

/* ------------------------------------------------------------------ grid */

/* A stretch of the track at one tempo. `start` is the time of one of its
 * beats; beats extend forwards at 60/bpm until the next section. The first
 * section's start is the grid's first beat: there is no beat before it
 * (the analyser starts a grid where the music starts, docs/BEATS.md).
 * `beat` is the beat number (1..beats_per_bar) of the beat at `start`, or
 * 0 to continue counting from the previous section. The first section's
 * `beat` is always 1..beats_per_bar (0 is read as 1). */
typedef struct RXTempoSection {
    double start;        /* seconds */
    double bpm;
    int    beat;         /* 1..beats_per_bar, or 0 = continue the count */
} RXTempoSection;

typedef struct RXGrid {
    RXTempoSection *sections;    /* sorted by start; nb_sections == 0 means no grid */
    int             nb_sections;
    int             beats_per_bar;   /* 4 */
} RXGrid;

/* One beat of a grid, as the ANLZ files store it. */
typedef struct RXBeat {
    double time;         /* seconds */
    int    number;       /* 1..beats_per_bar; 1 is the downbeat */
    double bpm;          /* tempo of the section this beat is in */
} RXBeat;

/* One section at `bpm` with a downbeat at `start`, from the first beat of
 * that line at or after time 0: a grid made by hand covers the track. */
int    rx_grid_init(RXGrid *g, double start, double bpm);
int    rx_grid_copy(RXGrid *dst, const RXGrid *src);
void   rx_grid_free(RXGrid *g);
bool   rx_grid_valid(const RXGrid *g);                         /* >= 1 section, bpm 1..1000, sorted */
double rx_grid_bpm(const RXGrid *g);                           /* of the first section; 0 if none */
double rx_grid_bpm_at(const RXGrid *g, double t);
/* Adds a section at `start` (sorted in), replacing one at the same time. */
int    rx_grid_add_section(RXGrid *g, double start, double bpm, int beat);
int    rx_grid_remove_section(RXGrid *g, int index);
int    rx_grid_section_at(const RXGrid *g, double t);          /* index of the section containing t */

/* Every beat of the grid in [0, duration): malloc'd array, count in *nb. */
int    rx_grid_beats(const RXGrid *g, double duration, RXBeat **beats, int *nb);
/* Time of the beat nearest to t, and (optionally) its number. Before the
 * grid's first beat, that beat. */
double rx_grid_nearest_beat(const RXGrid *g, double t, int *number);
/* Time of the grid beat at or before / strictly after t (by > 1 ms).
 * With no beat at or before t (before the grid's first), t itself. */
double rx_grid_beat_before(const RXGrid *g, double t);
double rx_grid_beat_after(const RXGrid *g, double t);
/* t moved by `beats` grid beats (fractional allowed, negative = backwards).
 * The step follows the tempo of each section crossed. */
double rx_grid_offset_beats(const RXGrid *g, double t, double beats);
/* Fractional beat index of t counted from the first downbeat of the grid
 * (which is beat 0; the beats before it are negative), and the inverse.
 * Before the grid's first beat the first section's line continues, for
 * this arithmetic only. */
double rx_grid_time_to_beat(const RXGrid *g, double t);
double rx_grid_beat_to_time(const RXGrid *g, double beat);
/* Time of 1-based (bar, beat) counted from the first downbeat of the grid. */
double rx_grid_bar_time(const RXGrid *g, int bar, int beat);
/* Bar (1-based) and beat (1-based) of t; bars count from the first downbeat
 * (a pickup before it is bar 0). Before the grid's first beat: 0 and 0. */
void   rx_grid_bar_of(const RXGrid *g, double t, int *bar, int *beat);
/* Quantise t to the nearest beat (what QUANTIZE does on a CDJ). */
double rx_grid_quantize(const RXGrid *g, double t);
/* Moves every section by `delta` seconds (SHIFT GRID). */
void   rx_grid_shift(RXGrid *g, double delta);
/* Changes the tempo of the section containing t, keeping its start. */
void   rx_grid_set_bpm_at(RXGrid *g, double t, double bpm);
/* Makes the nearest beat to t the downbeat (beat 1) of its section: the
 * section's count moves, its start does not. */
void   rx_grid_set_downbeat(RXGrid *g, double t);
/* Makes the nearest beat to t the start of a new section at the same tempo
 * (split), or removes the section boundary nearest to t (merge). */
int    rx_grid_split_at(RXGrid *g, double t);
int    rx_grid_merge_at(RXGrid *g, double t);
/* Starts the grid at its first beat at or after t: the sections before
 * that beat's go, and its own starts on it with its number. No beat at or
 * after t moves or changes its number. */
int    rx_grid_start_at(RXGrid *g, double t);

/* ------------------------------------------------------------------- cues */

#define RX_HOT_CUE_SLOTS 8      /* A..H */
#define RX_CUE_NAME_MAX  128
#define RX_COLOR_MAX     16     /* a palette name or "#rrggbb" */

/* A cue. `time` is always valid. `grid_beat`, when not NAN, says the cue is
 * musical: it sits on that (fractional) beat index of the grid (the
 * convention of rx_grid_time_to_beat), so it follows the grid when the grid
 * is corrected: rx_track_resolve_cues() recomputes `time` from it. Loop
 * ends are stored as a beat length in `loop_beats` when musical, in seconds
 * otherwise. */
typedef struct RXCue {
    double time;                    /* seconds */
    double grid_beat;               /* beat index, or NAN for an absolute position */
    int    hot;                     /* -1 memory cue, 0..7 hot cue A..H */
    double loop_end;                /* < 0: not a loop; else loop end in seconds */
    double loop_beats;              /* loop length in beats, or NAN when loop_end is absolute */
    bool   active;                  /* memory loop that engages when reached */
    char   name[RX_CUE_NAME_MAX];   /* UTF-8, may be empty */
    char   color[RX_COLOR_MAX];     /* palette name (rxbox/colors.h), "#rrggbb" or "" */
} RXCue;

static inline bool rx_cue_is_loop(const RXCue *c) { return c->loop_end >= 0.0; }
static inline bool rx_cue_is_hot(const RXCue *c) { return c->hot >= 0; }
static inline bool rx_cue_is_musical(const RXCue *c) { return !isnan(c->grid_beat); }
static inline char rx_cue_letter(const RXCue *c) { return c->hot >= 0 ? (char)('A' + c->hot) : ' '; }
/* A memory cue at `time`, absolute, no loop, no colour, no name. */
void rx_cue_init(RXCue *c, double time);
/* Marks the cue musical on `grid` (or absolute when grid is NULL/invalid). */
void rx_cue_attach_to_grid(RXCue *c, const RXGrid *grid);

/* --------------------------------------------------------------- metadata */

/* Keys are stored as classic names: "C", "Db", "D", ... "B" for major,
 * "Cm", "Dbm" ... for minor (rxbox/analysis.h converts to Camelot and Open
 * Key and back). "" means unknown. */
#define RX_KEY_MAX 8

/* A track's colour is a hue: the HSV/HSL hue angle in degrees,
 * 0 <= hue < 360, with 0 red, 60 yellow, 120 green, 180 cyan (rekordbox's
 * aqua is near it), 240 blue, 300 magenta (rekordbox's pink is near it);
 * RX_HUE_NONE (-1) for no colour. The interface draws it at full
 * saturation and its usual value (rx_track_hue_rgb, rxbox/colors.h). The
 * stick carries one of rekordbox's eight colours: the export writes the
 * one whose hue (the HSV hue of its RGB) is nearest by circular distance
 * (rx_track_color_nearest_hue). A zeroed RXMeta has the hue 0, red:
 * rx_meta_free and rx_track_new set it to none. */
#define RX_HUE_NONE (-1.0)
static inline bool rx_hue_valid(double hue) { return hue >= 0.0 && hue < 360.0; }
/* Into [0, 360) (360 is 0 again); none for a hue below 0 or not a number. */
static inline double rx_hue_wrap(double hue)
{
    return isfinite(hue) && hue >= 0.0 ? fmod(hue, 360.0) : RX_HUE_NONE;
}

/* Tag fields. The library keeps what the file says and the user's
 * overrides separately (an override column is NULL when the file's value
 * is used); a track carries both so an edit never loses the file's tag.
 * Rating, hue and My Tags are the library's only: file_meta leaves them
 * at none. */
typedef struct RXMeta {
    char *title, *artist, *album, *album_artist, *genre, *label, *remixer,
         *composer, *original_artist, *comment;
    int   year, track_number, disc_number;
    int   rating;                   /* 0..5 */
    double hue;                     /* the track colour, degrees 0 <= hue < 360, or RX_HUE_NONE */
    char **tags; int nb_tags;       /* My Tags: "Situation/Peak Time", "vocal" ... */
} RXMeta;

/* Frees the strings and tags and leaves an empty RXMeta with no hue. */
void rx_meta_free(RXMeta *m);
int  rx_meta_copy(RXMeta *dst, const RXMeta *src);
int  rx_meta_set(char **field, const char *value);     /* takes a copy; NULL clears */
int  rx_meta_add_tag(RXMeta *m, const char *tag);
void rx_meta_clear_tags(RXMeta *m);
bool rx_meta_has_tag(const RXMeta *m, const char *tag);

/* ------------------------------------------------------------------ track */

typedef struct RXTrack {
    /* Identity */
    uint64_t id;                /* library id, > 0 once added; the id on every stick */
    char    *path;              /* absolute path of the audio file */

    /* Metadata */
    RXMeta meta;                /* effective: file tags with the user's overrides applied */
    RXMeta file_meta;           /* as read from the file */

    /* Analysis results */
    char   key[RX_KEY_MAX];
    double key_confidence;          /* 0..1, 0 if unknown */
    RXGrid grid;                    /* nb_sections == 0 when unanalysed */
    double grid_confidence;         /* 0..1 */
    unsigned grid_flags;            /* RX_GRID_FLAG_* from rxbox/analysis.h */
    bool   grid_locked;             /* analysis must not replace it */
    bool   has_waveforms;           /* the library holds its waveform bytes */
    /* The analyser versions (RX_*_VERSION in rxbox/analysis.h) the grid,
     * the key and the waveforms were made by; 0 before versions were
     * kept. Set only when an analysis stores that result: a grid edited
     * by hand keeps the version of the one it was edited from. */
    int    beats_version, key_version, waves_version;
    RXCue *cues; int nb_cues;

    /* Stream properties from probing */
    double  duration;               /* seconds, on the players' timeline */
    int64_t total_samples;          /* on the players' timeline, at sample_rate */
    int     sample_rate, channels, bit_rate, bit_depth;
    char    codec[16];              /* libavcodec name: flac, mp3, aac, opus, pcm_s16le ... */
    int64_t file_size;
    int64_t mtime;                  /* of the audio file when it was probed */
    bool    has_artwork;

    /* Library bookkeeping */
    int64_t added;                  /* epoch seconds */
    int64_t analysed;               /* epoch seconds of the last analysis, 0 never */
    int     play_count;
    int64_t last_played;            /* epoch seconds, 0 never */
    bool    missing;                /* audio file not found at last scan: shown greyed out */
} RXTrack;

RXTrack *rx_track_new(const char *path);
void     rx_track_free(RXTrack *t);
RXTrack *rx_track_dup(const RXTrack *t);                       /* deep copy */
static inline bool rx_track_analysed(const RXTrack *t) { return t->grid.nb_sections > 0; }

/* Cue list edits. Hot cue slots are unique: adding a hot cue in an occupied
 * slot replaces the old one. Returns the index of the cue, or a negative
 * errno. The list is kept sorted by time. */
int      rx_track_add_cue(RXTrack *t, const RXCue *cue);
int      rx_track_remove_cue(RXTrack *t, int index);
int      rx_track_hot_cue(const RXTrack *t, int slot);                 /* index or -1 */
/* Memory cues (not hot cues) around a time. "At" means within tolerance;
 * before/after mean strictly beyond it. Index or -1. */
int      rx_track_memory_cue_at(const RXTrack *t, double time, double tolerance);
int      rx_track_memory_cue_before(const RXTrack *t, double time, double tolerance);
int      rx_track_memory_cue_after(const RXTrack *t, double time, double tolerance);
int      rx_track_memory_cue_nth(const RXTrack *t, int n);             /* 1-based, index or -1 */
/* Recomputes the time of every musical cue from the grid. Call after any
 * grid edit. Absolute cues are untouched. */
void     rx_track_resolve_cues(RXTrack *t);
/* "Artist - Title" or the file name, into buf. */
const char *rx_track_display(const RXTrack *t, char *buf, size_t size);

#endif /* RXBOX_MODEL_H */
