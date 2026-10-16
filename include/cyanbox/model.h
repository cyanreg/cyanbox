/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_MODEL_H
#define CYANBOX_MODEL_H

/* The data model shared by analysis, the library, the player, the interface and
 * the stick writers: a track, its beat grid and its cues. Pure data plus the
 * arithmetic on grids; nothing here touches files. */

#include <math.h>
#include "cyanbox/common.h"

/* ------------------------------------------------------------------ grid */

/* A stretch of the track at one tempo. `start` is the time of one of its
 * beats; beats extend forwards at 60/bpm until the next section. The first
 * section's start is the grid's first beat: there is no beat before it
 * (the analyser starts a grid where the music starts, docs/BEATS.md).
 * `beat` is the beat number (1..beats_per_bar) of the beat at `start`, or
 * 0 to continue counting from the previous section. The first section's
 * `beat` is always 1..beats_per_bar (0 is read as 1).
 *
 * A *measured* section is one beat: the beat at `start`, and nothing more
 * until the next section, which begins on the next beat. A run of
 * measured beats is a dynamic grid, one that follows a tempo that moves
 * (a live drummer, rubato, a ramp): the analyser keeps the beats it
 * measured where a straight line would not hold them (docs/BEATS.md,
 * "Dynamic grids"), and the players play such grids as they are, every
 * beat with its own time and tempo (PQTZ, docs/anlz.md 4.3). `bpm` is
 * then the beat's local tempo, 60 over the interval to the next beat:
 * what the deck shows and the stick carries for that beat, and the
 * tempo the last section runs on at past its beat. Between two
 * measured beats the fractional beat index is linear in time, so a
 * quantised cue or a beat jump lands where the player would. A
 * straight section after a measured beat starts on the beat after it;
 * a straight grid has no measured section and is exactly what it
 * was. */
typedef struct CBTempoSection {
    double start;        /* seconds */
    double bpm;
    int    beat;         /* 1..beats_per_bar, or 0 = continue the count */
    bool   measured;     /* one beat at `start`; the next section begins on the next beat */
} CBTempoSection;

typedef struct CBGrid {
    CBTempoSection *sections;    /* sorted by start; nb_sections == 0 means no grid */
    int             nb_sections;
    int             beats_per_bar;   /* 4 */
} CBGrid;

/* One beat of a grid, as the ANLZ files store it. */
typedef struct CBBeat {
    double time;         /* seconds */
    int    number;       /* 1..beats_per_bar; 1 is the downbeat */
    double bpm;          /* tempo of the section this beat is in: a measured beat's own */
} CBBeat;

/* One section at `bpm` with a downbeat at `start`, from the first beat of
 * that line at or after time 0: a grid made by hand covers the track. */
int    cb_grid_init(CBGrid *g, double start, double bpm);
int    cb_grid_copy(CBGrid *dst, const CBGrid *src);
void   cb_grid_free(CBGrid *g);
bool   cb_grid_valid(const CBGrid *g);                         /* >= 1 section, bpm 1..1000, sorted */
double cb_grid_bpm(const CBGrid *g);                           /* of the first section; 0 if none */
double cb_grid_bpm_at(const CBGrid *g, double t);              /* a measured beat's: its local tempo */
/* True when any section is a measured beat: the grid bends. */
bool   cb_grid_dynamic(const CBGrid *g);
/* The tempo that stands for the track (the stick's one tempo, the table):
 * the first section's for a straight grid; on a dynamic grid the median
 * of the local tempos of its beats in [0, duration). 0 if none. */
double cb_grid_bpm_median(const CBGrid *g, double duration);
/* Adds a section at `start` (sorted in), replacing one at the same time. */
int    cb_grid_add_section(CBGrid *g, double start, double bpm, int beat);
/* Adds a measured beat at `time` with the local tempo `bpm` (sorted in,
 * replacing a section at the same time). */
int    cb_grid_add_beat(CBGrid *g, double time, double bpm, int beat);
int    cb_grid_remove_section(CBGrid *g, int index);
int    cb_grid_section_at(const CBGrid *g, double t);          /* index of the section containing t */

/* Every beat of the grid in [0, duration): malloc'd array, count in *nb. */
int    cb_grid_beats(const CBGrid *g, double duration, CBBeat **beats, int *nb);
/* Time of the beat nearest to t, and (optionally) its number. Before the
 * grid's first beat, that beat. */
double cb_grid_nearest_beat(const CBGrid *g, double t, int *number);
/* Time of the grid beat at or before / strictly after t (by > 1 ms).
 * With no beat at or before t (before the grid's first), t itself. */
double cb_grid_beat_before(const CBGrid *g, double t);
double cb_grid_beat_after(const CBGrid *g, double t);
/* t moved by `beats` grid beats (fractional allowed, negative = backwards).
 * The step follows the tempo of each section crossed, and the measured
 * beats of a dynamic grid one by one. */
double cb_grid_offset_beats(const CBGrid *g, double t, double beats);
/* Fractional beat index of t counted from the first downbeat of the grid
 * (which is beat 0; the beats before it are negative), and the inverse.
 * Before the grid's first beat the first section's line continues, for
 * this arithmetic only. */
double cb_grid_time_to_beat(const CBGrid *g, double t);
double cb_grid_beat_to_time(const CBGrid *g, double beat);
/* Time of 1-based (bar, beat) counted from the first downbeat of the grid. */
double cb_grid_bar_time(const CBGrid *g, int bar, int beat);
/* Bar (1-based) and beat (1-based) of t; bars count from the first downbeat
 * (a pickup before it is bar 0). Before the grid's first beat: 0 and 0. */
void   cb_grid_bar_of(const CBGrid *g, double t, int *bar, int *beat);
/* Quantise t to the nearest beat (what QUANTIZE does on a CDJ). */
double cb_grid_quantize(const CBGrid *g, double t);
/* Moves every section by `delta` seconds (SHIFT GRID): on a dynamic grid
 * every measured beat with it. */
void   cb_grid_shift(CBGrid *g, double delta);
/* Changes the tempo of the section containing t, keeping its start. On a
 * dynamic passage: the beats from the one at or before t to the end of
 * the passage become one straight section at `bpm` starting on that beat
 * (the beat keeps its number), as typing a tempo or tapping one means
 * "from here, straight at this tempo". */
void   cb_grid_set_bpm_at(CBGrid *g, double t, double bpm);
/* Double or halve the tempo of the section containing t, keeping its
 * start (the deck's x2 and /2). On a dynamic passage the beats
 * themselves double (a beat at every midpoint, each at twice its local
 * tempo) or halve (every other beat from the passage's first stays, each
 * with the number it was given, the first always), and x2 then /2 gives
 * the passage back. */
int    cb_grid_double_at(CBGrid *g, double t);
int    cb_grid_halve_at(CBGrid *g, double t);
/* The dynamic passage containing t becomes one straight section through
 * its beats (the least-squares line through their times, starting on its
 * first beat with that beat's number). Returns the section's index, or
 * -ENOENT when t is not in a dynamic passage. */
int    cb_grid_straighten_at(CBGrid *g, double t);
/* Makes the nearest beat to t the downbeat (beat 1) of its section: the
 * section's count moves, its start does not. */
void   cb_grid_set_downbeat(CBGrid *g, double t);
/* Makes the nearest beat to t the start of a new section at the same tempo
 * (split), or removes the section boundary nearest to t (merge). A split
 * moves no beat: on a measured beat it changes nothing (every beat of a
 * dynamic passage is its own section already), and past the last
 * measured beat of a grid ending in a passage the run-on becomes a
 * straight section from the beat after it. A merge in a dynamic passage,
 * or one whose nearest boundary is a passage's (its first beat, or the
 * start of the section after its last), straightens the passage
 * (cb_grid_straighten_at): the boundaries between its beats are not the
 * kind a DJ removes one by one, and the last measured beat would run on
 * at one interval's tempo through the section after. */
int    cb_grid_split_at(CBGrid *g, double t);
int    cb_grid_merge_at(CBGrid *g, double t);
/* Starts the grid at its first beat at or after t: the sections before
 * that beat's go, and its own starts on it with its number. No beat at or
 * after t moves or changes its number. */
int    cb_grid_start_at(CBGrid *g, double t);

/* ------------------------------------------------------------------- cues */

#define CB_HOT_CUE_SLOTS 8      /* A..H */
#define CB_CUE_NAME_MAX  128
#define CB_COLOR_MAX     16     /* a palette name or "#rrggbb" */

/* A cue. `time` is always valid. `grid_beat`, when not NAN, says the cue is
 * musical: it sits on that (fractional) beat index of the grid (the
 * convention of cb_grid_time_to_beat), so it follows the grid when the grid
 * is corrected: cb_track_resolve_cues() recomputes `time` from it. Loop
 * ends are stored as a beat length in `loop_beats` when musical, in seconds
 * otherwise. */
typedef struct CBCue {
    double time;                    /* seconds */
    double grid_beat;               /* beat index, or NAN for an absolute position */
    int    hot;                     /* -1 memory cue, 0..7 hot cue A..H */
    double loop_end;                /* < 0: not a loop; else loop end in seconds */
    double loop_beats;              /* loop length in beats, or NAN when loop_end is absolute */
    bool   active;                  /* memory loop that engages when reached */
    char   name[CB_CUE_NAME_MAX];   /* UTF-8, may be empty */
    char   color[CB_COLOR_MAX];     /* palette name (cyanbox/colors.h), "#rrggbb" or "" */
} CBCue;

static inline bool cb_cue_is_loop(const CBCue *c) { return c->loop_end >= 0.0; }
static inline bool cb_cue_is_hot(const CBCue *c) { return c->hot >= 0; }
static inline bool cb_cue_is_musical(const CBCue *c) { return !isnan(c->grid_beat); }
static inline char cb_cue_letter(const CBCue *c) { return c->hot >= 0 ? (char)('A' + c->hot) : ' '; }
/* A memory cue at `time`, absolute, no loop, no colour, no name. */
void cb_cue_init(CBCue *c, double time);
/* Marks the cue musical on `grid` (or absolute when grid is NULL/invalid). */
void cb_cue_attach_to_grid(CBCue *c, const CBGrid *grid);

/* --------------------------------------------------------------- metadata */

/* Keys are stored as classic names: "C", "Db", "D", ... "B" for major,
 * "Cm", "Dbm" ... for minor (cyanbox/analysis.h converts to Camelot and Open
 * Key and back). "" means unknown. */
#define CB_KEY_MAX 8

/* A track's colour is a hue: the HSV/HSL hue angle in degrees,
 * 0 <= hue < 360, with 0 red, 60 yellow, 120 green, 180 cyan (rekordbox's
 * aqua is near it), 240 blue, 300 magenta (rekordbox's pink is near it);
 * CB_HUE_NONE (-1) for no colour. It is one of rekordbox's eight (the HSV
 * hue of the RGB it draws them in, cyanbox/colors.h) or none: the library
 * removes any other when it opens (src/core/clean.c), and nothing sets
 * one. The interface draws it at full saturation and its usual value
 * (cb_track_hue_rgb); the stick carries the colour it is
 * (cb_track_color_nearest_hue finds it). A zeroed CBMeta has the hue 0,
 * red: cb_meta_free and cb_track_new set it to none. */
#define CB_HUE_NONE (-1.0)
static inline bool cb_hue_valid(double hue) { return hue >= 0.0 && hue < 360.0; }
/* Into [0, 360) (360 is 0 again); none for a hue below 0 or not a number. */
static inline double cb_hue_wrap(double hue)
{
    return isfinite(hue) && hue >= 0.0 ? fmod(hue, 360.0) : CB_HUE_NONE;
}

/* Tag fields. The library keeps what the file says and the user's
 * overrides separately (an override column is NULL when the file's value
 * is used); a track carries both so an edit never loses the file's tag.
 * Rating, hue and My Tags are the library's only: file_meta leaves them
 * at none. The file's comment tag is not imported unless the setting
 * import.comment says so (cyanbox/library.h): the comment is the user's. */
typedef struct CBMeta {
    char *title, *artist, *album, *album_artist, *genre, *label, *remixer,
         *composer, *original_artist, *comment;
    int   year, track_number, disc_number;
    int   rating;                   /* 0..5 */
    double hue;                     /* the track colour, degrees 0 <= hue < 360, or CB_HUE_NONE */
    char **tags; int nb_tags;       /* My Tags: "Situation/Peak Time", "vocal" ... */
} CBMeta;

/* Frees the strings and tags and leaves an empty CBMeta with no hue. */
void cb_meta_free(CBMeta *m);
int  cb_meta_copy(CBMeta *dst, const CBMeta *src);
int  cb_meta_set(char **field, const char *value);     /* takes a copy, in NFC; NULL clears */
/* Every text in NFC (an older library's, or tags from macOS): how many
 * texts changed, or a negative errno. */
int  cb_meta_normalise(CBMeta *m);
int  cb_meta_add_tag(CBMeta *m, const char *tag);
void cb_meta_clear_tags(CBMeta *m);
bool cb_meta_has_tag(const CBMeta *m, const char *tag);

/* ------------------------------------------------------------------ track */

typedef struct CBTrack {
    /* Identity */
    uint64_t id;                /* library id, > 0 once added; the id on every stick */
    char    *path;              /* absolute path of the audio file */

    /* Metadata */
    CBMeta meta;                /* effective: file tags with the user's overrides applied */
    CBMeta file_meta;           /* as read from the file */

    /* Analysis results */
    char   key[CB_KEY_MAX];
    double key_confidence;          /* 0..1, 0 if unknown */
    CBGrid grid;                    /* nb_sections == 0 when unanalysed */
    double grid_confidence;         /* 0..1 */
    unsigned grid_flags;            /* CB_GRID_FLAG_* from cyanbox/analysis.h */
    bool   grid_locked;             /* analysis must not replace it */
    bool   has_waveforms;           /* the library holds its waveform bytes */
    /* The analyser versions (CB_*_VERSION in cyanbox/analysis.h) the grid,
     * the key and the waveforms were made by; 0 before versions were
     * kept. Set only when an analysis stores that result: a grid edited
     * by hand keeps the version of the one it was edited from. */
    int    beats_version, key_version, waves_version;
    CBCue *cues; int nb_cues;

    /* What the model said of the track (cyanbox/ml.h), only ever when the
     * user asked: the CB_ML_TAGS_VERSION of the tagging (0 never tagged)
     * and its four measures, 0 to 1 (NAN never tagged): club music,
     * intensity (calm .. intense), valence (dark .. bright), vocals (1
     * heard, 0 instrumental). The genre, hue and comment it gave are in
     * meta, where the user edits them. ml_colors is the probability it
     * gave each of the eight track colours (by colour id - 1, cyanbox/colors.h;
     * NAN when not tagged so, as before tagging version 2) and ml_legend
     * the hash of the legend it was asked with (cb_ml_legend_hash, 0 none). */
    int    tags_version;
    float  ml_club, ml_intensity, ml_valence, ml_vocals;
    float  ml_colors[8];
    uint32_t ml_legend;

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
} CBTrack;

CBTrack *cb_track_new(const char *path);
void     cb_track_free(CBTrack *t);
CBTrack *cb_track_dup(const CBTrack *t);                       /* deep copy */
static inline bool cb_track_analysed(const CBTrack *t) { return t->grid.nb_sections > 0; }

/* Cue list edits. Hot cue slots are unique: adding a hot cue in an occupied
 * slot replaces the old one. Returns the index of the cue, or a negative
 * errno. The list is kept sorted by time. */
int      cb_track_add_cue(CBTrack *t, const CBCue *cue);
int      cb_track_remove_cue(CBTrack *t, int index);
int      cb_track_hot_cue(const CBTrack *t, int slot);                 /* index or -1 */
/* Memory cues (not hot cues) around a time. "At" means within tolerance;
 * before/after mean strictly beyond it. Index or -1. */
int      cb_track_memory_cue_at(const CBTrack *t, double time, double tolerance);
int      cb_track_memory_cue_before(const CBTrack *t, double time, double tolerance);
int      cb_track_memory_cue_after(const CBTrack *t, double time, double tolerance);
int      cb_track_memory_cue_nth(const CBTrack *t, int n);             /* 1-based, index or -1 */
/* Recomputes the time of every musical cue from the grid. Call after any
 * grid edit. Absolute cues are untouched. */
void     cb_track_resolve_cues(CBTrack *t);
/* "Artist - Title" or the file name, into buf. */
const char *cb_track_display(const CBTrack *t, char *buf, size_t size);

#endif /* CYANBOX_MODEL_H */
