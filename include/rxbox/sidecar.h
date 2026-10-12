/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_SIDECAR_H
#define RXBOX_SIDECAR_H

/* Per-track sidecar files: the human-editable source of truth for a track,
 * in dj's TOML format (see README of dj and docs/OVERVIEW.md):
 *
 *   id = 42                       # library id, assigned once (new in rxbox)
 *   source = "/abs/path.flac"
 *   added = 2026-10-13T03:00:00Z  # optional
 *
 *   [meta]                        # overrides of the file's tags only
 *   title = "..."  color = "pink"  rating = 4  tags = ["Situation/Peak Time"]
 *
 *   [grid]
 *   bpm = 128.0
 *   downbeat = 0.3125             # any downbeat; the grid extends both ways
 *   sections = [{start = 191.25, bpm = 130.0, beat = 1}]   # tempo changes
 *   confidence = 0.97
 *   flags = ["tempo-change"]      # informational, from the analysis
 *   locked = true
 *
 *   [key]
 *   key = "Am"                    # classic, Camelot ("8A") or Open Key ("1m")
 *   confidence = 0.8
 *
 *   [[cue]]
 *   at = "bar 33"                 # "bar 33.3" (beat 3), "beat 129", "1:02.500" or 62.5
 *   hot = "A"                     # A..H; omit for a memory cue
 *   loop = 8                      # beats; or loop_end = "bar 19" / seconds
 *   active = true
 *   name = "drop"
 *   color = "red"
 *
 *   cues = []                     # at top level: no cues, and don't add any
 *
 * Reading never fails on unknown keys. Writing rewrites the whole file from
 * the track (comments are not kept). Positions in bars follow the grid. */

#include "rxbox/model.h"

typedef struct RxCueSpec {
    char   at[48];            /* as written: "bar 17", "beat 64", "1:02.5", "62.5" */
    char   hot[2];            /* "A".."H" or "" */
    double loop_beats;        /* > 0 when `loop` given, else NAN */
    char   loop_end[48];      /* "" when not given */
    bool   active;
    char   name[RX_CUE_NAME_MAX];
    char   color[RX_COLOR_MAX];
} RxCueSpec;

typedef struct RxSidecar {
    uint64_t id;
    char    *source;
    int64_t  added;                 /* 0 if absent */
    RxMeta   meta;                  /* only the overrides present in [meta] */
    bool     has_meta_fields[16];   /* which RxMeta fields were present, by RX_META_* index */
    RxGrid   grid;                  /* nb_sections == 0 when [grid] absent */
    double   grid_confidence;       /* NAN if absent */
    unsigned grid_flags;
    bool     grid_locked;
    char     key[RX_KEY_MAX];       /* "" when [key] absent */
    double   key_confidence;        /* NAN if absent */
    bool     has_cues;              /* a [[cue]] table or `cues = []` is present */
    RxCueSpec *cues; int nb_cues;
} RxSidecar;

/* Indices for has_meta_fields. */
enum RxMetaField {
    RX_META_TITLE, RX_META_ARTIST, RX_META_ALBUM, RX_META_ALBUM_ARTIST, RX_META_GENRE,
    RX_META_LABEL, RX_META_REMIXER, RX_META_COMPOSER, RX_META_ORIGINAL_ARTIST, RX_META_COMMENT,
    RX_META_YEAR, RX_META_TRACK_NUMBER, RX_META_DISC_NUMBER, RX_META_RATING, RX_META_COLOR, RX_META_TAGS,
};

void rx_sidecar_free(RxSidecar *sc);

/* Parses a sidecar file / a TOML string. Returns 0, -ENOENT, or -EINVAL
 * with the problem logged (line and key). */
int  rx_sidecar_read(const char *path, RxSidecar *sc);
int  rx_sidecar_parse(const char *toml, size_t len, const char *name_for_errors, RxSidecar *sc);
/* Serialises to TOML (malloc'd, NUL-terminated) / writes atomically. */
int  rx_sidecar_format(const RxSidecar *sc, char **out);
int  rx_sidecar_write(const char *path, const RxSidecar *sc);

/* Applies a parsed sidecar to a track whose file_meta and stream properties
 * are already filled from probing: overrides meta, sets grid/key/cues
 * (resolving bar/beat positions against the grid and duration). */
int  rx_sidecar_apply(const RxSidecar *sc, RxTrack *t);
/* Builds the sidecar for a track: id, source, [meta] with only the fields
 * that differ from file_meta, grid, key, and the cues (musical cues as
 * "bar x.y", others as seconds). */
int  rx_sidecar_from_track(const RxTrack *t, RxSidecar *sc);

/* Position parsing shared with the CLI: seconds, "m:ss.mmm", "h:mm:ss",
 * "bar N", "bar N.B", "beat N". Bars and beats need a grid. Returns 0 and
 * the time, or -EINVAL. `grid_beat` receives the beat index for musical
 * positions, NAN otherwise. */
int  rx_parse_position(const char *spec, const RxGrid *grid, double *time, double *grid_beat);
/* The inverse used by the writer: "bar 17.3" for integer beats, "beat 66.5"
 * for fractional ones. */
const char *rx_format_grid_position(const RxGrid *grid, double grid_beat, char *buf, size_t size);

/* Key notation helpers (also used by the interface and export):
 * anything in -> classic ("Am"); classic -> Camelot ("8A") / Open Key ("1m"). */
int  rx_key_normalise(const char *in, char *classic, size_t size);
const char *rx_key_to_camelot(const char *classic, char *buf, size_t size);
const char *rx_key_to_open(const char *classic, char *buf, size_t size);

#endif /* RXBOX_SIDECAR_H */
