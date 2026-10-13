/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The plan's naming rules without a library: dj's usb_path (rekordbox's
 * /Contents/<Artist>/<Album>/<file> layout, FAT-illegal characters, the
 * 43-character stem, collisions as -1, -2 ...; docs/pdb.md 1.2), NFC, the
 * name check the sync uses on foreign sticks, cue conversions between the
 * library and the stick, and the sync record's round trip. */

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/export/export_priv.h"
#include "tests/formats/test.h"

static RXTrack track(const char *path, const char *artist, const char *album_artist, const char *album)
{
    RXTrack t = { .path = (char *)path };
    t.meta.artist = (char *)artist;
    t.meta.album_artist = (char *)album_artist;
    t.meta.album = (char *)album;
    return t;
}

static void check_path(const RXTrack *t, const char *source, const char *const *used, int nb, const char *want)
{
    char *p = rx_plan_usb_path(t, source, used, nb);
    CHECK_STR(p, want);
    free(p);
}

static void usb_paths(void)
{
    RXTrack t = track("/music/One.flac", "Synth", NULL, NULL);
    check_path(&t, NULL, NULL, 0, "/Contents/Synth/UnknownAlbum/One.flac");
    t = track("/music/x/Track.FLAC", NULL, NULL, NULL);
    check_path(&t, NULL, NULL, 0, "/Contents/UnknownArtist/UnknownAlbum/Track.flac");
    /* The album artist wins; illegal characters become '_'; trailing dots
     * and spaces go. */
    t = track("/m/a.mp3", "Someone", "AC/DC", "Back: in Black?.. ");
    check_path(&t, NULL, NULL, 0, "/Contents/AC_DC/Back_ in Black_/a.mp3");
    /* A transcode: the name from the library file, the extension from what
     * is copied. */
    t = track("/m/Ambient Piece.opus", "A", NULL, "B");
    check_path(&t, "/lib/cache/transcode/12.flac", NULL, 0, "/Contents/A/B/Ambient Piece.flac");
    /* NFD names (HFS+) come out NFC; the stem is cut to 43 characters. */
    t = track("/m/Ame\xCC\x81lie and a very long title that goes on and on.wav", "A", NULL, "B");
    check_path(&t, NULL, NULL, 0, "/Contents/A/B/Am\xC3\xA9lie and a very long title that goes on a.wav");
    /* Collisions, case-insensitive as FAT: stem[:40] + -N. */
    const char *used[] = { "/contents/a/b/am\xC3\xA9lie and a very long title that goes on a.wav" };
    check_path(&t, NULL, used, 1, "/Contents/A/B/Am\xC3\xA9lie and a very long title that goes o-1.wav");
    const char *used2[] = { "/Contents/A/B/x.mp3", "/CONTENTS/A/B/X-1.MP3" };
    t = track("/m/x.mp3", "A", NULL, "B");
    check_path(&t, NULL, used2, 2, "/Contents/A/B/x-2.mp3");
    /* Nothing left of a name: "_"; a name of dots has no suffix (dj on
     * Python 3.14). */
    t = track("/m/....mp3", "...", NULL, "B");
    check_path(&t, NULL, NULL, 0, "/Contents/_/B/....mp3");

    char *f = rx_plan_fat_name("\xE3\x80\x80 J-Pop \xE3\x81\x8B\xE3\x82\x99 \xE3\x80\x80", 64);
    CHECK_STR(f, "J-Pop \xE3\x81\x8C");
    free(f);

    t = track("/m/x.mp3", "A", NULL, "B");
    CHECK(rx_plan_usb_name_matches(&t, "/Contents/A/B/x.mp3"));
    CHECK(rx_plan_usb_name_matches(&t, "/Contents/A/B/x-3.mp3"));
    CHECK(!rx_plan_usb_name_matches(&t, "/Contents/A/B/y.mp3"));
    CHECK(!rx_plan_usb_name_matches(&t, "/Contents/A/B/x-a.mp3"));

    /* The key is the name up-cased as exFAT does. */
    char *fc = rx_plan_fold_case("ÄBC/Ωmega");
    CHECK_STR(fc, "ÄBC/ΩMEGA");
    free(fc);

    /* FAT and exFAT compare names without case, exFAT with its up-case
     * table (the specification's, which every formatter writes): Latin
     * Extended-A with its capitals on odd code points in U+0139..U+0148 and
     * U+0179..U+017E (Ł ł, Ž ž), Ÿ and U+00FF, and beyond what dj's
     * str.lower() covered here: Ș ș (Romanian), the DŽ digraph, Vietnamese,
     * full width, Armenian, Georgian, circled letters. Two names that
     * differ only there are one file on the stick. */
    static const char *const same[][2] = {
        { "Łódź", "łódź" }, { "Žena", "žena" }, { "ŃŇĽĹĻĿŹŻŽ", "ńňľĺļŀźżž" }, { "Ÿ", "ÿ" },
        { "ĀĒĪŌŪ", "āēīōū" }, { "ΆΈΉΊΌΎΏ", "άέήίόύώ" }, { "ЀЁЂ", "ѐёђ" }, { "ΣΑΣ", "σας" },
        { "Șerban", "șerban" }, { "ȚARA", "țara" }, { "Ǆ", "ǆ" }, { "ẠẢẤỸ", "ạảấỹ" },
        { "ＡＢＣ", "ａｂｃ" }, { "ԱԲ", "աբ" }, { "ႠႡ", "ⴀⴁ" }, { "ⒶⓏ", "ⓐⓩ" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(same); i++) {
        char *a = rx_plan_fold_case(same[i][0]), *b = rx_plan_fold_case(same[i][1]);
        CHECK_STR(a, b);
        free(a);
        free(b);
    }
    /* And different letters stay different, where exFAT keeps them apart
     * too: the title-case digraph, dotless i, the Kelvin sign, and letters
     * beyond the BMP, which it compares as they are. */
    static const char *const apart[][2] = {
        { "ł", "Ń" }, { "ǅ", "ǆ" }, { "ı", "i" }, { "\u212A", "k" }, { "\U00010400", "\U00010428" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(apart); i++) {
        char *a = rx_plan_fold_case(apart[i][0]), *b = rx_plan_fold_case(apart[i][1]);
        CHECK(a && b && strcmp(a, b));
        free(a);
        free(b);
    }
    t = track("/m/žena.mp3", "A", NULL, "B");
    const char *used3[] = { "/Contents/A/B/Žena.mp3" };
    check_path(&t, NULL, used3, 1, "/Contents/A/B/žena-1.mp3");
    t = track("/m/song.wav", "șerban", NULL, "Album");
    const char *used4[] = { "/Contents/Șerban/Album/song.wav" };
    check_path(&t, NULL, used4, 1, "/Contents/șerban/Album/song-1.wav");

    /* The extension comes from the file copied: ASCII letters and digits
     * only, as every audio extension is, so nothing FAT refuses (":", "?",
     * a trailing space) or a cut UTF-8 sequence reaches the stick. */
    t = track("/m/a.mp3", "A", NULL, "B");
    check_path(&t, "/lib/x.Fl:ac?", NULL, 0, "/Contents/A/B/a.fl_ac_");
    check_path(&t, "/lib/x.w\xC3\xA4v ", NULL, 0, "/Contents/A/B/a.w__v_");

    /* Stick paths read from a stick's databases: absolute, no ".."
     * component, no empty one (export_priv.h). */
    CHECK(rx_stick_path_safe("/Contents/A/B/x.mp3"));
    CHECK(rx_stick_path_safe("/PIONEER/USBANLZ/P016/0000875E/ANLZ0000.DAT"));
    CHECK(rx_stick_path_safe("/Contents/A/..x/y..mp3"));
    CHECK(!rx_stick_path_safe("/Contents/../../etc/passwd"));
    CHECK(!rx_stick_path_safe("/PIONEER/USBANLZ/P016/0000875E/../../../../dev/zero"));
    CHECK(!rx_stick_path_safe("/.."));
    CHECK(!rx_stick_path_safe("/Contents/A/.."));
    CHECK(!rx_stick_path_safe("Contents/x.mp3"));
    CHECK(!rx_stick_path_safe("/Contents//x.mp3"));
    CHECK(!rx_stick_path_safe("/Contents/./x.mp3"));
    CHECK(!rx_stick_path_safe(""));
    CHECK(!rx_stick_path_safe(NULL));
}

static void cues(void)
{
    RXTempoSection sec = { .start = 0.25, .bpm = 120.0, .beat = 1 };
    RXGrid grid = { .sections = &sec, .nb_sections = 1, .beats_per_bar = 4 };
    RXCue c = {
        .time = 2.25, .grid_beat = NAN, .hot = 0, .loop_end = 4.25, .loop_beats = NAN,
        .name = "drop", .color = "red",
    };
    RXStickCue s;
    rx_stick_cue_from_track(&c, &s);
    CHECK(s.hot == 0 && s.in == 2250 && s.out == 4250 && s.code == 0x2A && s.rgb == 0xFF0000);
    RXCue back;
    rx_stick_cue_to_track(&s, &grid, &back);
    /* On a beat: musical again, the loop 4 beats long. */
    CHECK(back.hot == 0 && fabs(back.time - 2.25) < 1e-9 && back.grid_beat == 4.0 && back.loop_beats == 4.0);
    CHECK_STR(back.color, "red");
    CHECK_STR(back.name, "drop");

    /* No colour: code 0 and the slot's default pad colour, read back as "". */
    RXCue plain = { .time = 1.0, .grid_beat = NAN, .hot = 1, .loop_end = -1, .loop_beats = NAN };
    rx_stick_cue_from_track(&plain, &s);
    CHECK(s.code == 0 && s.rgb == 0x00C4FF && s.out == 0xFFFFFFFFu);
    rx_stick_cue_to_track(&s, &grid, &back);
    CHECK_STR(back.color, "");

    /* A custom pad colour (code 0 with an RGB, as rekordbox writes them)
     * becomes "#rrggbb"; an off-beat time stays absolute. */
    s = (RXStickCue) { .hot = 2, .in = 1234, .out = 0xFFFFFFFFu, .code = 0, .rgb = 0xF4D338 };
    rx_stick_cue_to_track(&s, &grid, &back);
    CHECK_STR(back.color, "#f4d338");
    CHECK(isnan(back.grid_beat) && back.time == 1.234);

    /* dj's green and rekordbox's per-slot default both mean no colour. */
    RXStickCue g1 = { .hot = 0, .in = 1000, .out = 0xFFFFFFFFu, .rgb = 0x00FF00 };
    RXStickCue g2 = { .hot = 0, .in = 1000, .out = 0xFFFFFFFFu, .rgb = 0xFF0017 };
    CHECK(rx_stick_cue_equal(&g1, &g2));
    rx_stick_cue_to_track(&g1, &grid, &back);
    CHECK_STR(back.color, "");
    g2.code = 0x2A;
    g2.rgb = 0xFF0000;
    CHECK(!rx_stick_cue_equal(&g1, &g2));

    RXStickCue a = { .hot = -1, .in = 1000, .out = 0xFFFFFFFFu, .code = 3 }, b = a;
    b.in = 1001;
    CHECK(rx_stick_cue_equal(&a, &b));
    b.in = 1003;
    CHECK(!rx_stick_cue_equal(&a, &b));
    b = a;
    b.active = true;
    CHECK(!rx_stick_cue_equal(&a, &b));
}

static void sync_record(void)
{
    const char *base = getenv("RX_TEST_DIR");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/plan-record", base ? base : "build-formats/test-stick");
    char *pio = rx_asprintf("%s/PIONEER", dir);
    rx_mkdir_p(pio);
    free(pio);

    RXStickCue c1[2] = {
        { .hot = 0, .in = 500, .out = 0xFFFFFFFFu, .code = 0x2A, .rgb = 0xFF0000, .name = "100% intro\nx" },
        { .hot = -1, .in = 2000, .out = 4000, .code = 2, .active = true },
    };
    RXSyncTrack tracks[2] = { { .id = 7, .cues = c1, .nb_cues = 2 }, { .id = 9 } };
    uint8_t setting[4] = { 1, 2, 0xAB, 0xFF };
    RXSyncRecord r = { .uuid = "0123456789abcdef", .date = "2026-10-13", .tracks = tracks, .nb_tracks = 2 };
    snprintf(r.settings[0].name, sizeof(r.settings[0].name), "DEVSETTING.DAT");
    r.settings[0].data = setting;
    r.settings[0].size = 4;
    r.nb_settings = 1;
    CHECK_EQ(rx_sync_record_write(dir, &r), 0);

    RXSyncRecord back;
    CHECK_EQ(rx_sync_record_read(dir, &back), 0);
    CHECK_STR(back.uuid, "0123456789abcdef");
    CHECK_STR(back.date, "2026-10-13");
    CHECK_EQ(back.nb_tracks, 2);
    const RXSyncTrack *t = rx_sync_record_track(&back, 7);
    CHECK(t && t->nb_cues == 2);
    if (t && t->nb_cues == 2) {
        CHECK(rx_stick_cue_equal(&t->cues[0], &c1[0]));
        CHECK(rx_stick_cue_equal(&t->cues[1], &c1[1]));
        CHECK_STR(t->cues[0].name, "100% intro\nx");
    }
    t = rx_sync_record_track(&back, 9);
    CHECK(t && t->nb_cues == 0);
    CHECK(!rx_sync_record_track(&back, 8));
    CHECK(back.nb_settings == 1 && back.settings[0].size == 4 && !memcmp(back.settings[0].data, setting, 4));
    rx_sync_record_free(&back);

    /* The record is a file on the stick anyone can edit: cut and damaged
     * copies read back or fail, never past their end. */
    char *path = rx_stick_file(dir, RX_SYNC_PATH);
    uint8_t *text;
    size_t n;
    CHECK_EQ(rx_file_read(path, &text, &n), 0);
    uint32_t x = 12345;
    for (size_t i = 0; i < 3 * n; i++) {
        uint8_t *c = malloc(n);
        memcpy(c, text, n);
        size_t len = i < n ? i : n;
        if (i >= n) {
            x = x * 1103515245 + 12345;
            c[x % n] = (uint8_t)(i & 1 ? '%' : x >> 16);
        }
        CHECK_EQ(rx_file_write_atomic(path, c, len), 0);
        RXSyncRecord f;
        if (rx_sync_record_read(dir, &f) >= 0) {
            for (int k = 0; k < f.nb_tracks; k++)
                CHECK(f.tracks[k].nb_cues >= 0);
            rx_sync_record_free(&f);
        }
        free(c);
    }
    free(text);
    free(path);
    char *rm = rx_asprintf("rm -rf '%s'", dir);
    if (rm && system(rm))
        fprintf(stderr, "cannot remove %s\n", dir);
    free(rm);
}

int main(void)
{
    usb_paths();
    cues();
    sync_record();
    return test_result("plan");
}
