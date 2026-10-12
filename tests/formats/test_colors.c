/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Colours: dj's sixteen hot cue names keep their codes; their RGB is now
 * the measured device table (rbxport DEVICE_PALETTE), which agrees with
 * every LED value dj had measured on rekordbox exports. Spot values of the
 * table, the default pads, track and memory cue colours, "#rrggbb"
 * lookups. */

#include "rxbox/colors.h"

#include "tests/formats/test.h"

int main(void)
{
    /* dj's HOT_CUE_NAMES codes, in our hue order. */
    static const struct { const char *name; int code; uint32_t rgb; } names[] = {
        { "blue", 0x01, 0x0000FF }, { "sky", 0x08, 0x00C4FF }, { "aqua", 0x09, 0x00E0FF },
        { "teal", 0x0D, 0x00FFBA }, { "mint", 0x13, 0x00FF30 }, { "green", 0x15, 0x00FF00 },
        { "lime", 0x17, 0x33FF00 }, { "yellow", 0x20, 0xFFE800 }, { "orange", 0x24, 0xFF8C00 },
        { "red", 0x2A, 0xFF0000 }, { "rose", 0x2B, 0xFF0017 }, { "pink", 0x2D, 0xFF0045 },
        { "magenta", 0x35, 0xFF00FF }, { "violet", 0x38, 0xB300FF }, { "purple", 0x3C, 0x4D00FF },
        { "lavender", 0x3D, 0x3300FF },
    };
    for (int i = 0; i < RX_HOT_CUE_COLORS; i++) {
        int code;
        uint32_t rgb;
        CHECK_STR(rx_hot_cue_color_names[i], names[i].name);
        CHECK_EQ(rx_hot_cue_color(names[i].name, &code, &rgb), 0);
        CHECK_EQ(code, names[i].code);
        CHECK_EQ(rgb, names[i].rgb);
        CHECK_EQ(rx_hot_cue_led(code), (int)names[i].rgb);
        CHECK_STR(rx_cue_palette_name(code), names[i].name);
    }

    /* The LED values dj measured on real exports (docs/anlz.md 4.6.1) are
     * the table's. */
    static const uint32_t measured[][2] = {
        { 0x01, 0x0000FF }, { 0x08, 0x00C4FF }, { 0x13, 0x00FF30 }, { 0x15, 0x00FF00 },
        { 0x17, 0x33FF00 }, { 0x20, 0xFFE800 }, { 0x24, 0xFF8C00 }, { 0x26, 0xFF5E00 },
        { 0x2B, 0xFF0017 }, { 0x3C, 0x4D00FF },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(measured); i++)
        CHECK_EQ(rx_cue_palette_rgb((int)measured[i][0]), measured[i][1]);

    /* Spot values across the table (rbxport's test: 46 is FF005C, 62
     * 1A00FF; 63/64 black and white; 0 none). */
    CHECK_EQ(rx_cue_palette_rgb(0), 0);
    CHECK_EQ(rx_cue_palette_rgb(2), 0x001CFF);
    CHECK_EQ(rx_cue_palette_rgb(10), 0x00FFFF);
    CHECK_EQ(rx_cue_palette_rgb(31), 0xFFFF00);
    CHECK_EQ(rx_cue_palette_rgb(46), 0xFF005C);
    CHECK_EQ(rx_cue_palette_rgb(62), 0x1A00FF);
    CHECK_EQ(rx_cue_palette_rgb(63), 0x000000);
    CHECK_EQ(rx_cue_palette_rgb(64), 0xFFFFFF);
    CHECK_EQ(rx_cue_palette_rgb(65), 0);

    int code;
    uint32_t rgb;
    CHECK_EQ(rx_hot_cue_color("cyan", &code, &rgb), 0);
    CHECK_EQ(code, 0x09);
    CHECK_EQ(rx_hot_cue_color(" Red ", &code, &rgb), 0);
    CHECK_EQ(code, 0x2A);
    CHECK_EQ(rx_hot_cue_color("#ff0000", &code, &rgb), 0);
    CHECK_EQ(code, 42);
    CHECK_EQ(rx_hot_cue_color("#00c4fe", &code, &rgb), 0);
    CHECK_EQ(code, 8);
    CHECK_EQ(rx_hot_cue_color("0x2b", &code, &rgb), 0);
    CHECK_EQ(code, 0x2B);
    CHECK_EQ(rgb, 0xFF0017);
    CHECK_EQ(rx_hot_cue_color("", &code, &rgb), 0);
    CHECK_EQ(code, 0);
    CHECK_EQ(rgb, 0);
    CHECK_EQ(rx_hot_cue_color("chartreuse", &code, &rgb), RX_ERROR(EINVAL));

    /* Default pads: A-D as rekordbox exports them (rbxport), E-H dj's green. */
    CHECK_EQ(rx_hot_cue_default_rgb(0), 0xFF0017);
    CHECK_EQ(rx_hot_cue_default_rgb(1), 0x00C4FF);
    CHECK_EQ(rx_hot_cue_default_rgb(2), 0x33FF00);
    CHECK_EQ(rx_hot_cue_default_rgb(3), 0x4D00FF);
    CHECK_EQ(rx_hot_cue_default_rgb(4), 0x00FF00);

    /* Track colours. */
    CHECK_EQ(rx_track_color_id("pink"), 1);
    CHECK_EQ(rx_track_color_id("Purple"), 8);
    CHECK_EQ(rx_track_color_id("teal"), 0);
    CHECK_STR(rx_track_color_name(6), "aqua");
    CHECK(rx_track_color_name(0) == NULL);
    CHECK_EQ(rx_track_color_rgb(2), 0xF80000);

    /* Memory cues: track colours and dj's aliases. */
    CHECK_EQ(rx_memory_cue_color_id(""), 0);
    CHECK_EQ(rx_memory_cue_color_id("green"), 5);
    CHECK_EQ(rx_memory_cue_color_id("lime"), 5);
    CHECK_EQ(rx_memory_cue_color_id("lavender"), 8);
    CHECK_EQ(rx_memory_cue_color_id("#ff0000"), 2);
    CHECK_EQ(rx_memory_cue_color_id("mauve"), RX_ERROR(EINVAL));
    CHECK_EQ(rx_memory_cue_color_rgb(7), 0x0050F8);

    CHECK_EQ(rx_parse_hex_rgb("#12abEF", &rgb), 0);
    CHECK_EQ(rgb, 0x12ABEF);
    CHECK_EQ(rx_parse_hex_rgb("12abef", &rgb), 0);
    CHECK_EQ(rx_parse_hex_rgb("#12abe", &rgb), RX_ERROR(EINVAL));
    return test_result("colors");
}
