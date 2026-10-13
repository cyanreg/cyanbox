/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Positions and key names (src/core/notation.c): dj's position syntax and
 * test cases, the bar/beat writer as the exact inverse of the reader on
 * one-section, tempo-change and restarting grids, and every key in every
 * notation against dj's tables. */

#include "rxbox/notation.h"

#include "util.h"

static double pos(const char *spec, const RXGrid *g, double *gb)
{
    double t = NAN, b = NAN;
    if (rx_parse_position(spec, g, &t, &b) < 0)
        return NAN;
    if (gb)
        *gb = b;
    return t;
}

static void test_positions(void)
{
    /* dj's tests/test_sidecar.py test_positions: beats every 0.5 s,
     * downbeats at 0.5, 2.5, 4.5 ... */
    RXGrid g;
    rx_grid_init(&g, 0.5, 120.0);
    double gb;
    CHECK(pos("12.25", &g, &gb) == 12.25 && isnan(gb));
    CHECK(pos("1:02.5", &g, NULL) == 62.5);
    CHECK(pos("1:02.500", &g, NULL) == 62.5);
    CHECK(pos("1:02:03", &g, NULL) == 3723.0);
    CHECK(pos("1:02:03.25", NULL, NULL) == 3723.25);
    CHECK(pos("bar 1", &g, &gb) == 0.5 && gb == 0.0);
    CHECK(pos("bar 2.3", &g, &gb) == 3.5 && gb == 6.0);
    CHECK(pos("  BAR   2.3 ", &g, NULL) == 3.5);
    CHECK(pos("bar 0.4", &g, NULL) == 0.0);       /* the pickup beat */
    /* "beat N" counts from the first downbeat (rxbox/notation.h); dj
     * counted from the first beat after 0, which made its "beat 1" 0.0 here. */
    CHECK(pos("beat 1", &g, &gb) == 0.5 && gb == 0.0);
    CHECK(pos("beat 2.5", &g, &gb) == 1.25 && gb == 1.5);
    CHECK(pos("beat 129", &g, NULL) == pos("bar 33", &g, NULL));
    CHECK(pos("-3.5", &g, NULL) == -3.5);
    CHECK(pos("beat -0.5", &g, &gb) == -0.25 && gb == -1.5);

    static const char *const bad[] = {
        "bar x", "bar", "bar 2.", "bar -1", "beat", "beat x", "1:2:3:4", "1::2", ":12",
        "abc", "", "   ", "nan", "inf", "12 s", "bar 2.3.4",
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(bad); i++) {
        double t;
        if (rx_parse_position(bad[i], &g, &t, NULL) != RX_ERROR(EINVAL)) {
            fprintf(stderr, "position '%s' should not parse\n", bad[i]);
            test_failures++;
        }
    }
    /* Bars and beats need a grid; seconds do not. */
    RXGrid none = { 0 };
    double t;
    CHECK(rx_parse_position("bar 2", &none, &t, NULL) == RX_ERROR(EINVAL));
    CHECK(rx_parse_position("beat 2", NULL, &t, NULL) == RX_ERROR(EINVAL));
    CHECK(rx_parse_position("2:00", NULL, &t, NULL) == 0 && t == 120.0);

    char buf[48];
    CHECK_STR(rx_format_grid_position(&g, 0.0, buf, sizeof(buf)), "bar 1");
    CHECK_STR(rx_format_grid_position(&g, 6.0, buf, sizeof(buf)), "bar 2.3");
    CHECK_STR(rx_format_grid_position(&g, 6.0 + 1e-9, buf, sizeof(buf)), "bar 2.3");
    CHECK_STR(rx_format_grid_position(&g, -1.0, buf, sizeof(buf)), "bar 0.4");
    CHECK_STR(rx_format_grid_position(&g, 5.5, buf, sizeof(buf)), "beat 6.5");
    CHECK_STR(rx_format_grid_position(&g, 0.123456789, buf, sizeof(buf)), "beat 1.123457");
    CHECK(!rx_format_grid_position(&none, 1.0, buf, sizeof(buf)) && !buf[0]);
    CHECK(!rx_format_grid_position(&g, NAN, buf, sizeof(buf)));
    rx_grid_free(&g);

    /* A beat index past any track (a damaged row): no bar arithmetic
     * overflowing into a made-up bar, just the number. (Starting at 100 s,
     * the index of the beat at 0 is -200.) */
    rx_grid_init(&g, 100.0, 120.0);
    CHECK(rx_format_grid_position(&g, 2e300, buf, sizeof(buf)) && rx_str_starts(buf, "beat "));
    CHECK(rx_format_grid_position(&g, -1e20, buf, sizeof(buf)) && rx_str_starts(buf, "beat -"));
    CHECK_STR(rx_format_grid_position(&g, 4.0, buf, sizeof(buf)), "bar 2");
    rx_grid_free(&g);
}

/* format then parse gives the beat back, on grids with tempo changes and
 * a restarting count (where bars are short). */
static void test_roundtrip(void)
{
    RXGrid grids[3] = { { .beats_per_bar = 4 }, { .beats_per_bar = 4 }, { .beats_per_bar = 4 } };
    rx_grid_add_section(&grids[0], 0.3125, 128.0, 1);
    rx_grid_add_section(&grids[1], 0.3125, 128.0, 1);
    rx_grid_add_section(&grids[1], 61.0, 130.0, 0);
    rx_grid_add_section(&grids[1], 120.25, 90.0, 0);
    rx_grid_add_section(&grids[2], 2.0, 120.0, 1);
    rx_grid_add_section(&grids[2], 5.0, 120.0, 1);       /* bar of 2 beats before */
    rx_grid_add_section(&grids[2], 9.5, 120.0, 3);

    for (int k = 0; k < 3; k++) {
        for (double b = -2; b < 400; b += 0.25) {
            char buf[48];
            double t, back;
            if (!rx_format_grid_position(&grids[k], b, buf, sizeof(buf)) ||
                rx_parse_position(buf, &grids[k], &t, &back) < 0 ||
                fabs(back - b) > 1e-6 || fabs(t - rx_grid_beat_to_time(&grids[k], b)) > 1e-6) {
                fprintf(stderr, "grid %d beat %g: '%s' reads back as %g\n", k, b, buf, back);
                test_failures++;
                break;
            }
        }
        rx_grid_free(&grids[k]);
    }
}

static void test_keys(void)
{
    static const struct { const char *in, *out; } cases[] = {
        { "Am", "Am" }, { "A minor", "Am" }, { "A min", "Am" }, { "a Minor", "Am" },
        { "8A", "Am" }, { "8a", "Am" }, { "1m", "Am" }, { "1M", "Am" }, { " Am ", "Am" },
        { "C", "C" }, { "8B", "C" }, { "1d", "C" }, { "C major", "C" }, { "Cmaj", "C" },
        { "CM", "C" }, { "B#", "C" }, { "F#m", "F#m" }, { "Gbm", "F#m" }, { "Gb", "F#" },
        { "C#", "Db" }, { "c#", "Db" }, { "Db", "Db" }, { "A#m", "Bbm" }, { "bbm", "Bbm" },
        { "E♭", "Eb" }, { "F♯m", "F#m" }, { "Cb", "B" }, { "12A", "Dbm" },
        { "12B", "E" }, { "", "" }, { "  ", "" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(cases); i++) {
        char k[RX_KEY_MAX] = "x";
        int ret = rx_key_normalise(cases[i].in, k, sizeof(k));
        if (ret < 0 || strcmp(k, cases[i].out)) {
            fprintf(stderr, "key '%s' -> '%s' (%d), expected '%s'\n", cases[i].in, k, ret, cases[i].out);
            test_failures++;
        }
    }
    static const char *const bad[] = { "H", "13A", "0A", "8C", "Am7", "A minor key", "#A", "1x" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(bad); i++) {
        char k[RX_KEY_MAX];
        CHECK(rx_key_normalise(bad[i], k, sizeof(k)) == RX_ERROR(EINVAL) && !k[0]);
    }
    char small[3];
    CHECK(rx_key_normalise("F#m", small, sizeof(small)) == RX_ERROR(ERANGE));

    char buf[8];
    CHECK_STR(rx_key_to_camelot("Am", buf, sizeof(buf)), "8A");
    CHECK_STR(rx_key_to_camelot("C", buf, sizeof(buf)), "8B");
    CHECK_STR(rx_key_to_camelot("F#m", buf, sizeof(buf)), "11A");
    CHECK_STR(rx_key_to_camelot("B", buf, sizeof(buf)), "1B");
    CHECK_STR(rx_key_to_camelot("", buf, sizeof(buf)), "");
    CHECK_STR(rx_key_to_open("Am", buf, sizeof(buf)), "1m");
    CHECK_STR(rx_key_to_open("C", buf, sizeof(buf)), "1d");
    CHECK_STR(rx_key_to_open("F#m", buf, sizeof(buf)), "4m");
    CHECK_STR(rx_key_to_open("junk", buf, sizeof(buf)), "");
    CHECK_STR(rx_key_display("Am", "camelot", buf, sizeof(buf)), "8A");
    CHECK_STR(rx_key_display("Am", "open", buf, sizeof(buf)), "1m");
    CHECK_STR(rx_key_display("Am", "classic", buf, sizeof(buf)), "Am");
    CHECK_STR(rx_key_display("", "camelot", buf, sizeof(buf)), "");

    /* Every key through every notation and back; Camelot numbers all used. */
    static const char *const names[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    int seen[2][13] = { { 0 } };
    for (int m = 0; m < 2; m++) {
        for (int i = 0; i < 12; i++) {
            char key[8], c[8], o[8], back[8];
            snprintf(key, sizeof(key), "%s%s", names[i], m ? "m" : "");
            rx_key_to_camelot(key, c, sizeof(c));
            rx_key_to_open(key, o, sizeof(o));
            CHECK(rx_key_normalise(c, back, sizeof(back)) == 0 && !strcmp(back, key));
            CHECK(rx_key_normalise(o, back, sizeof(back)) == 0 && !strcmp(back, key));
            CHECK(c[strlen(c) - 1] == (m ? 'A' : 'B'));
            int n = atoi(c);
            if (n >= 1 && n <= 12)
                seen[m][n]++;
        }
    }
    for (int m = 0; m < 2; m++) {
        for (int n = 1; n <= 12; n++)
            CHECK(seen[m][n] == 1);
    }
}

/* The Traffic Light (rbxport src/lib/camelot.ts's rule, rekordbox's 2A
 * example), every reach and the wrap from 12 to 1. */
static void test_traffic_light(void)
{
    /* Playing a track in A (11B): F#m (11A), D and E (10B, 12B), Bm and Dbm
     * (10A, 12A) light at Related Key 3; Abm (1A) is two steps away. */
    static const char *const lit[] = { "A", "F#m", "D", "E", "Bm", "Dbm", "11B", "C#m" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(lit); i++) {
        if (!rx_key_traffic_light(lit[i], "A", RX_KEY_REACH_RELATED3))
            fprintf(stderr, "%s does not light against A\n", lit[i]);
        CHECK(rx_key_traffic_light(lit[i], "A", RX_KEY_REACH_RELATED3));
    }
    static const char *const dark[] = { "Abm", "B", "C", "Am", "G", "Ebm", "", "junk" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(dark); i++) {
        if (rx_key_traffic_light(dark[i], "A", RX_KEY_REACH_RELATED3))
            fprintf(stderr, "%s lights against A\n", dark[i]);
        CHECK(!rx_key_traffic_light(dark[i], "A", RX_KEY_REACH_RELATED3));
    }

    /* rekordbox's tooltip for 2A (Ebm): 2B is F#, 1A Abm, 3A Bbm, 1B B, 3B Db. */
    CHECK(rx_key_traffic_light("Ebm", "Ebm", RX_KEY_REACH_SAME));
    CHECK(!rx_key_traffic_light("F#", "Ebm", RX_KEY_REACH_SAME));
    CHECK(rx_key_traffic_light("F#", "Ebm", RX_KEY_REACH_RELATED1));
    CHECK(!rx_key_traffic_light("Abm", "Ebm", RX_KEY_REACH_RELATED1));
    CHECK(rx_key_traffic_light("Abm", "Ebm", RX_KEY_REACH_RELATED2));
    CHECK(rx_key_traffic_light("Bbm", "Ebm", RX_KEY_REACH_RELATED2));
    CHECK(!rx_key_traffic_light("B", "Ebm", RX_KEY_REACH_RELATED2));
    CHECK(rx_key_traffic_light("B", "Ebm", RX_KEY_REACH_RELATED3));
    CHECK(rx_key_traffic_light("Db", "Ebm", RX_KEY_REACH_RELATED3));
    CHECK(!rx_key_traffic_light("Fm", "Ebm", RX_KEY_REACH_RELATED3));

    /* 12A (Dbm) and 1A (Abm) are neighbours; any notation reads. */
    CHECK(rx_key_traffic_light("Abm", "Dbm", RX_KEY_REACH_RELATED2));
    CHECK(rx_key_traffic_light("12A", "1A", RX_KEY_REACH_RELATED2));
    CHECK(rx_key_traffic_light("1m", "C", RX_KEY_REACH_RELATED1));
    CHECK(!rx_key_traffic_light("Am", "Am", RX_KEY_REACH_OFF));
    CHECK(!rx_key_traffic_light("Am", "", RX_KEY_REACH_RELATED3));
    CHECK(!rx_key_traffic_light("Am", NULL, RX_KEY_REACH_RELATED3));

    CHECK_STR(rx_key_reach_name(RX_KEY_REACH_RELATED3), "related3");
    CHECK_STR(rx_key_reach_name(RX_KEY_REACH_OFF), "off");
    CHECK(rx_key_reach_parse("same") == RX_KEY_REACH_SAME);
    CHECK(rx_key_reach_parse("related4") == -1 && rx_key_reach_parse(NULL) == -1);
}

int main(void)
{
    test_positions();
    test_roundtrip();
    test_keys();
    test_traffic_light();
    return test_done("core_notation");
}
