/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_COLORS_H
#define RXBOX_COLORS_H

/* rekordbox's colours: the eight track colours, the hot-cue palette (64
 * colour codes, each with the RGB rekordbox stores in PCP2 and the LED
 * value the DAT file carries), the memory cue colours, and the default pad
 * colours. The 64-entry table is the one rbxport captured from rekordbox
 * (crates/rbl-anlz/src/cues.rs, DEVICE_PALETTE); dj computed most of these
 * and got 48 of them wrong. */

#include "rxbox/common.h"

/* Track colours: id 1..8 in rekordbox order, 0 = none. */
#define RX_TRACK_COLORS 8
extern const char *const rx_track_color_names[RX_TRACK_COLORS];   /* pink red orange yellow green aqua blue purple */
int         rx_track_color_id(const char *name);                  /* 0 unknown/none */
const char *rx_track_color_name(int id);                          /* NULL for 0 */
uint32_t    rx_track_color_rgb(int id);                           /* 0xRRGGBB, as the interface draws it */

/* Their RGB, as rekordbox 7.2.19 draws them (rekordbox::cPink ...
 * ColorTheme, as rbxport's tokens.css records). Macros so the library's
 * schema 4 migration (src/core/db.c), which cannot link this module,
 * computes its hues from the same values. */
#define RX_TRACK_RGB_PINK   0xF870F8
#define RX_TRACK_RGB_RED    0xF80000
#define RX_TRACK_RGB_ORANGE 0xF8A030
#define RX_TRACK_RGB_YELLOW 0xF8E330
#define RX_TRACK_RGB_GREEN  0x00E000
#define RX_TRACK_RGB_AQUA   0x00C0F8
#define RX_TRACK_RGB_BLUE   0x0050F8
#define RX_TRACK_RGB_PURPLE 0x9808F8

/* The legend: what each track colour stands for, by default (the settings
 * color.pink ... color.purple change it, rxbox/library.h). The tagging
 * model gives a track the colour whose description fits it best
 * (rxbox/ml.h), so each says what can be heard in the music: its energy,
 * brightness, darkness, groove. Macros, as the RGB above, so the settings
 * in core and the model's tools take them without linking this module. */
#define RX_TRACK_VIBE_PINK   "dark and intense, hard"
#define RX_TRACK_VIBE_RED    "intense, peak time"
#define RX_TRACK_VIBE_ORANGE "energetic and bright, driving"
#define RX_TRACK_VIBE_YELLOW "bright, uplifting"
#define RX_TRACK_VIBE_GREEN  "groovy and relaxed, warm"
#define RX_TRACK_VIBE_AQUA   "calm, smooth"
#define RX_TRACK_VIBE_BLUE   "deep, melancholic"
#define RX_TRACK_VIBE_PURPLE "calm and dark, ambient"

/* A track's colour in the library is a hue (RXMeta.hue: the HSV hue angle
 * in degrees, 0 <= hue < 360, 0 red, 60 yellow, 120 green, 180 cyan, 240
 * blue, 300 magenta); the stick still carries one of the eight. Each
 * entry's hue is the HSV hue of its RGB above (rx_rgb_hue): pink 300, red 0,
 * orange 33.6, yellow 53.7, green 120, aqua 193.5, blue 220.6, purple 276. */
double      rx_track_color_hue(int id);                           /* -1 for 0/unknown */
/* The entry whose hue is nearest to `hue` by circular distance, 1..8; 0
 * when `hue` is none (below 0 or not a number). Hues of 360 and over wrap. */
int         rx_track_color_nearest_hue(double hue);
/* The entry whose hue `hue` is, to a hundredth of a degree, 1..8: a colour
 * picked from the eight (by hand or by the model). 0 for none and for a
 * hue between them, which an older library may hold. */
int         rx_track_color_of_hue(double hue);
/* The RGB a hue is drawn in: full saturation at the value the track
 * colours above are drawn at (0xF8). 0 for none. */
uint32_t    rx_track_hue_rgb(double hue);
/* A colour as the command line takes it: one of the eight names (that
 * entry's hue), that entry's hue in degrees (276 is purple; 360 is 0; a
 * "°" after the number is allowed), or "none" / "" (-1). -EINVAL for
 * anything else, a hue between the eight included: the library keeps
 * none (src/core/clean.c removes one it finds). */
int         rx_parse_hue(const char *s, double *hue);
/* "193.5° (aqua)": the degrees to a tenth and the entry the stick gets; "none". */
const char *rx_hue_format(double hue, char *buf, size_t size);

/* HSV <-> RGB. Hue in degrees (wrapped into [0, 360)), saturation and value
 * 0..1. rx_rgb_hue is the standard RGB -> HSV hue formula; -1 for a grey,
 * which has no hue. */
uint32_t    rx_hue_rgb(double hue, double saturation, double value);
double      rx_rgb_hue(uint32_t rgb);

/* Hot cue colours. Names the interface and the CLI use (rekordbox's
 * sixteen, in its pad-menu order), each mapping to a colour code in the
 * 64-entry palette. "#rrggbb" picks the nearest palette entry. */
#define RX_HOT_CUE_COLORS 16
extern const char *const rx_hot_cue_color_names[RX_HOT_CUE_COLORS];
/* -> colour code (1..63) and RGB. Code 0 / rgb 0 for "" or unknown; returns
 * 0 on success, -EINVAL when the name is unknown. */
int         rx_hot_cue_color(const char *name_or_hex, int *code, uint32_t *rgb);
uint32_t    rx_cue_palette_rgb(int code);                         /* the 64-entry table, 0 for code 0 */
const char *rx_cue_palette_name(int code);                        /* nearest named colour */
/* The pad (LED) RGB the PCP2 entry stores after the colour code: the same
 * measured table as rx_cue_palette_rgb. */
int         rx_hot_cue_led(int code);
/* Pad colour when a hot cue has no colour (per slot A..H). */
uint32_t    rx_hot_cue_default_rgb(int slot);

/* Memory cue colours: the eight track colours (1..8), 0 none. */
int         rx_memory_cue_color_id(const char *name);
uint32_t    rx_memory_cue_color_rgb(int id);

/* Parses "#rrggbb" / "rrggbb". -EINVAL otherwise. */
int         rx_parse_hex_rgb(const char *s, uint32_t *rgb);

#endif /* RXBOX_COLORS_H */
