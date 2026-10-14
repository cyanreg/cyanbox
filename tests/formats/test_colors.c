/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Colours: dj's sixteen hot cue names keep their codes; their RGB is now
 * the measured device table (rbxport DEVICE_PALETTE), which agrees with
 * every LED value dj had measured on rekordbox exports. Spot values of the
 * table, the default pads, track and memory cue colours, "#rrggbb"
 * lookups; the track colours' hues and the nearest one to any hue. */

#include "rxbox/colors.h"

#include <math.h>

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

    /* Each entry's hue is its RGB's: the hues the schema 4 migration
     * (src/core/db.c) gives the old names, which tests/core/test_library.c
     * checks against these functions. */
    static const double hues[RX_TRACK_COLORS] = {
        300.0, 0.0, 33.6, 53.7, 120.0, 193.54838709677418, 220.6451612903226, 276.0,
    };
    for (int i = 0; i < RX_TRACK_COLORS; i++) {
        CHECK(fabs(rx_track_color_hue(i + 1) - hues[i]) < 1e-9);
        /* Each hue maps back to its own entry, and so does a hue a little
         * either side of it. */
        CHECK_EQ(rx_track_color_nearest_hue(hues[i]), i + 1);
        CHECK_EQ(rx_track_color_nearest_hue(fmod(hues[i] + 359.0, 360.0)), i + 1);
        CHECK_EQ(rx_track_color_nearest_hue(hues[i] + 1.0), i + 1);
        /* Red, aqua and blue are at full saturation and the value hues
         * are drawn at: a hue draws as their own RGB. */
        if (i == 1 || i == 5 || i == 6)
            CHECK_EQ(rx_track_hue_rgb(hues[i]), rx_track_color_rgb(i + 1));
    }
    CHECK_EQ(rx_track_color_hue(0), -1.0);
    CHECK_EQ(rx_track_color_hue(9), -1.0);
    /* In between: the nearer neighbour around the circle. Red-orange's
     * midpoint is 16.8, orange-yellow's 43.65, yellow-green's 86.85,
     * green-aqua's 156.77, aqua-blue's 207.1, blue-purple's 248.3,
     * purple-pink's 288, pink-red's 330 (a tie: the lower id, pink). */
    static const struct { double hue; int id; } between[] = {
        { 16.0, 2 }, { 17.0, 3 }, { 43.0, 3 }, { 44.0, 4 }, { 86.0, 4 }, { 87.0, 5 },
        { 156.0, 5 }, { 157.0, 6 }, { 207.0, 6 }, { 208.0, 7 }, { 248.0, 7 }, { 249.0, 8 },
        { 287.9, 8 }, { 288.1, 1 }, { 329.0, 1 }, { 330.0, 1 }, { 331.0, 2 }, { 359.9, 2 },
        { 360.0, 2 }, { 420.0, 4 }, { 180.0, 6 },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(between); i++)
        CHECK_EQ(rx_track_color_nearest_hue(between[i].hue), between[i].id);
    CHECK_EQ(rx_track_color_nearest_hue(-1.0), 0);
    CHECK_EQ(rx_track_color_nearest_hue(NAN), 0);
    CHECK_EQ(rx_track_hue_rgb(-1.0), 0);

    /* A colour picked from the eight (the model's or a swatch's) is its
     * hue, and that hue is the colour again; a hue between them, which an
     * older library may hold, is none of them, and neither is no colour. */
    for (int id = 1; id <= RX_TRACK_COLORS; id++) {
        CHECK_EQ(rx_track_color_of_hue(rx_track_color_hue(id)), id);
        CHECK_EQ(rx_track_color_of_hue(rx_track_color_hue(id) + 360.0), id);
        CHECK_EQ(rx_track_color_of_hue(rx_track_color_hue(id) + 0.5), 0);
        CHECK_EQ(rx_track_color_nearest_hue(rx_track_color_hue(id) + 0.5), id);
        double hue = -1;
        CHECK_EQ(rx_parse_hue(rx_track_color_names[id - 1], &hue), 0);
        CHECK_EQ(rx_track_color_of_hue(hue), id);
    }
    /* A hue stored by the database comes back within a hair of its own. */
    CHECK_EQ(rx_track_color_of_hue(193.548387096774), 6);
    CHECK_EQ(rx_track_color_of_hue(359.999), 2);
    CHECK_EQ(rx_track_color_of_hue(200.0), 0);
    CHECK_EQ(rx_track_color_of_hue(-1.0), 0);
    CHECK_EQ(rx_track_color_of_hue(NAN), 0);

    /* The legend's defaults, in the colours' order. */
    static const char *const vibes[RX_TRACK_COLORS] = {
        RX_TRACK_VIBE_PINK, RX_TRACK_VIBE_RED,   RX_TRACK_VIBE_ORANGE, RX_TRACK_VIBE_YELLOW,
        RX_TRACK_VIBE_GREEN, RX_TRACK_VIBE_AQUA, RX_TRACK_VIBE_BLUE,   RX_TRACK_VIBE_PURPLE,
    };
    CHECK_STR(vibes[0], "dark and intense, hard");
    CHECK_STR(vibes[1], "intense, peak time");
    CHECK_STR(vibes[4], "groovy and relaxed, warm");
    CHECK_STR(vibes[7], "calm and dark, ambient");

    /* The command line's colours: a name, one of the eight in degrees,
     * none; never a hue between them. */
    double hue = 0;
    CHECK_EQ(rx_parse_hue("200", &hue), RX_ERROR(EINVAL));
    CHECK_EQ(rx_parse_hue(" 33.5\xC2\xB0 ", &hue), RX_ERROR(EINVAL));
    CHECK_EQ(rx_parse_hue(" 276\xC2\xB0 ", &hue), 0);
    CHECK(hue == rx_track_color_hue(8));
    CHECK_EQ(rx_parse_hue("360", &hue), 0);
    CHECK(hue == 0.0);
    CHECK_EQ(rx_parse_hue("Aqua", &hue), 0);
    CHECK(hue == rx_track_color_hue(6));
    CHECK_EQ(rx_parse_hue("none", &hue), 0);
    CHECK(hue == -1.0);
    CHECK_EQ(rx_parse_hue("", &hue), 0);
    CHECK(hue == -1.0);
    CHECK_EQ(rx_parse_hue("361", &hue), RX_ERROR(EINVAL));
    CHECK_EQ(rx_parse_hue("-10", &hue), RX_ERROR(EINVAL));
    CHECK_EQ(rx_parse_hue("teal", &hue), RX_ERROR(EINVAL));
    CHECK_EQ(rx_parse_hue("12deg", &hue), RX_ERROR(EINVAL));
    CHECK_EQ(rx_parse_hue("nan", &hue), RX_ERROR(EINVAL));
    char buf[32];
    CHECK_STR(rx_hue_format(rx_track_color_hue(6), buf, sizeof(buf)), "193.5\xC2\xB0 (aqua)");
    CHECK_STR(rx_hue_format(300.0, buf, sizeof(buf)), "300\xC2\xB0 (pink)");
    CHECK_STR(rx_hue_format(10.0, buf, sizeof(buf)), "10\xC2\xB0 (red)");
    CHECK_STR(rx_hue_format(-1.0, buf, sizeof(buf)), "none");

    /* HSV both ways. */
    CHECK_EQ(rx_hue_rgb(0.0, 1.0, 1.0), 0xFF0000);
    CHECK_EQ(rx_hue_rgb(120.0, 1.0, 1.0), 0x00FF00);
    CHECK_EQ(rx_hue_rgb(240.0, 1.0, 1.0), 0x0000FF);
    CHECK_EQ(rx_hue_rgb(60.0, 1.0, 1.0), 0xFFFF00);
    CHECK_EQ(rx_hue_rgb(300.0, 0.0, 1.0), 0xFFFFFF);
    CHECK_EQ(rx_hue_rgb(-60.0, 1.0, 1.0), 0xFF00FF);
    CHECK_EQ(rx_hue_rgb(720.0, 1.0, 1.0), 0xFF0000);
    CHECK_EQ(rx_hue_rgb(-1e-20, 1.0, 1.0), 0xFF0000);
    CHECK_EQ(rx_rgb_hue(0x808080), -1.0);
    CHECK(fabs(rx_rgb_hue(0x00FFFF) - 180.0) < 1e-9);
    CHECK(fabs(rx_rgb_hue(0xFF00FF) - 300.0) < 1e-9);
    for (int h = 0; h < 360; h += 15)
        CHECK(fabs(rx_rgb_hue(rx_hue_rgb(h, 1.0, 1.0)) - h) < 0.5);

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
