/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_NOTATION_H
#define RXBOX_NOTATION_H

/* Positions and key names as people write them: the CLI accepts cue
 * positions in seconds, timestamps or bars/beats, and keys in classic,
 * Camelot or Open Key notation; the interface shows keys in the configured
 * notation. (This replaces the sidecar format of dj; rxbox keeps its
 * library in SQLite, see rxbox/library.h.) */

#include "rxbox/model.h"

/* Position parsing: seconds ("62.5"), "m:ss.mmm", "h:mm:ss", "bar N",
 * "bar N.B" (beat B of bar N), "beat N" (1-based from the first downbeat).
 * Bars and beats need a grid. Returns 0 and the time, or -EINVAL.
 * `grid_beat` receives the beat index for musical positions (the
 * convention of rx_grid_time_to_beat), NAN otherwise. */
int  rx_parse_position(const char *spec, const RXGrid *grid, double *time, double *grid_beat);
/* "bar 17.3" for integer beats, "beat 66.5" for fractional ones. */
const char *rx_format_grid_position(const RXGrid *grid, double grid_beat, char *buf, size_t size);

/* Keys are stored as classic names with dj's spellings: C Db D Eb E F F# G
 * Ab A Bb B, and Cm Dbm ... for minor. rx_key_normalise accepts "Am",
 * "A minor", "A min", "8A", "1m", "F#m", "Gb", "C#" and normalises the
 * enharmonics. The others convert a classic name; "" stays "". */
int  rx_key_normalise(const char *in, char *classic, size_t size);
const char *rx_key_to_camelot(const char *classic, char *buf, size_t size);
const char *rx_key_to_open(const char *classic, char *buf, size_t size);
/* In the notation named "classic", "camelot" or "open". */
const char *rx_key_display(const char *classic, const char *notation, char *buf, size_t size);

#endif /* RXBOX_NOTATION_H */
