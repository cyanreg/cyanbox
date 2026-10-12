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
