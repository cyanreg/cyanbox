/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* ANLZ files: dj's tests (the directory hash vectors, the writer's tag
 * layout and cue lists, reading real rekordbox files) plus the readers on
 * those fixtures, the PVB2 rule for FLAC and a write/read cue round trip.
 * The directory vectors are the rekordbox ones from docs/pdb.md 1.3. */

#include <stdio.h>

#include "rxbox/colors.h"
#include "rxbox/formats.h"

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "tests/formats/test.h"

static void dir_hash(void)
{
    static const char *const v[][2] = {
        { "/Contents/Loopmasters/UnknownAlbum/Demo Track 1.mp3", "P016/0000875E" },
        { "/Contents/Loopmasters/UnknownAlbum/Demo Track 2.mp3", "P053/0001D21F" },
        { "/Contents/UnknownArtist/UnknownAlbum/NOISE.wav", "P019/00020AA9" },
        { "/Contents/UnknownArtist/UnknownAlbum/SINEWAVE.wav", "P043/00011517" },
        { "/Contents/UnknownArtist/UnknownAlbum/SIREN.wav", "P017/00009B77" },
        { "/Contents/UnknownArtist/UnknownAlbum/HORN.wav", "P021/00006D2B" },
        { "/Contents/XamarA/UnknownAlbum/Unreal.mp3", "P060/00012938" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(v); i++) {
        char buf[64], want[64];
        snprintf(want, sizeof(want), "/PIONEER/USBANLZ/%s", v[i][1]);
        CHECK_EQ(rx_anlz_dir(v[i][0], buf, sizeof(buf)), 0);
        CHECK_STR(buf, want);
    }
}

static int order_is(const uint8_t *b, size_t n, const char *want)
{
    RXAnlzTag *tags;
    int nb;
    if (rx_anlz_parse(b, n, &tags, &nb) < 0)
        return 0;
    char got[128] = "";
    for (int i = 0; i < nb; i++) {
        strcat(got, tags[i].fourcc);
        if (i + 1 < nb)
            strcat(got, " ");
    }
    free(tags);
    if (strcmp(got, want))
        fprintf(stderr, "tag order \"%s\", want \"%s\"\n", got, want);
    return !strcmp(got, want);
}

static int cues_of(const uint8_t *b, size_t n, const char *fourcc, int which,
                   RXAnlzCueEntry **e, int *declared, RXAnlzTag *tag_out)
{
    RXAnlzTag *tags;
    int nb, seen = 0, count = -1;
    *e = NULL;
    if (rx_anlz_parse(b, n, &tags, &nb) < 0)
        return -1;
    for (int i = 0; i < nb; i++) {
        if (memcmp(tags[i].fourcc, fourcc, 4) || seen++ != which)
            continue;
        if (tag_out)
            *tag_out = tags[i];
        if (rx_anlz_tag_cues(&tags[i], e, &count, declared) < 0)
            count = -1;
        break;
    }
    free(tags);
    return count;
}

static void fill_waves(RXWaveforms *w, int n)
{
    *w = (RXWaveforms) { .nb_detail = n };
    w->pwv3 = malloc((size_t)n);
    w->pwv5 = malloc((size_t)n * 2);
    w->pwv7 = malloc((size_t)n * 3);
    for (int i = 0; i < n; i++)
        w->pwv3[i] = (uint8_t)(i & 0xFF);
    memset(w->pwv5, 0x5A, (size_t)n * 2);
    memset(w->pwv7, 0x11, (size_t)n * 3);
    for (int i = 0; i < RX_WAVE_PWAV_COLUMNS; i++)
        w->pwav[i] = (uint8_t)(2 + i % 20);
    memset(w->pwv2, 3, sizeof(w->pwv2));
    memset(w->pwv4, 7, sizeof(w->pwv4));
    memset(w->pwv6, 9, sizeof(w->pwv6));
    w->gains[0] = 80;
    w->gains[1] = 0x8A;
    w->gains[2] = 0x18C;
}

static void plan_cue(RXPlanCue *c, double time, int hot, double loop_end, bool active,
                     const char *name, const char *color)
{
    *c = (RXPlanCue) { .time = time, .hot = hot, .loop_end = loop_end, .active = active };
    snprintf(c->name, sizeof(c->name), "%s", name);
    if (hot >= 0) {
        rx_hot_cue_color(color, &c->color_code, &c->rgb);
        if (!c->color_code)
            c->rgb = rx_hot_cue_default_rgb(hot);
    } else {
        c->color_code = rx_memory_cue_color_id(color);
        c->rgb = rx_memory_cue_color_rgb(c->color_code);
    }
}

/* dj's test_anlz_layout, on the same grid and cues. */
static void layout(void)
{
    RXTempoSection sec = { .start = 0.2, .bpm = 120.0, .beat = 1 };
    RXPlanCue cues[4];
    plan_cue(&cues[0], 0.5, 0, -1, false, "intro", "red");
    plan_cue(&cues[1], 1.0, 5, -1, false, "", "");
    plan_cue(&cues[2], 2.0, -1, 4.0, true, "", "");
    plan_cue(&cues[3], 3.0, -1, -1, false, "", "");
    RXPlanTrack t = {
        .id       = 1,
        .usb_path = "/Contents/A/B/x.mp3",
        .duration = 5.0,
        .grid     = { .sections = &sec, .nb_sections = 1, .beats_per_bar = 4 },
        .cues     = cues,
        .nb_cues  = 4,
    };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "mp3");
    t.probe.mp3 = (RXMp3Info) { .valid = true, .nb_frames = 190, .frame_samples = 1152 };
    RXWaveforms w;
    fill_waves(&w, 750);
    RXAnlzFiles f;
    CHECK_EQ(rx_anlz_build(&t, &w, &f), 0);

    CHECK(order_is(f.dat, f.dat_size, "PPTH PVBR PQTZ PWAV PWV2 PCOB PCOB"));
    CHECK(order_is(f.ext, f.ext_size, "PPTH PWV3 PCOB PCOB PCO2 PCO2 PQT2 PWV5 PWV4"));
    CHECK(order_is(f.ex2, f.ex2_size, "PPTH PWV7 PWV6 PWVC"));
    const uint8_t *files[3] = { f.dat, f.ext, f.ex2 };
    size_t sizes[3] = { f.dat_size, f.ext_size, f.ex2_size };
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(rx_rd_be32(files[i] + 8), sizes[i]);
        char *path = NULL;
        CHECK_EQ(rx_anlz_read_path(files[i], sizes[i], &path), 0);
        CHECK_STR(path, "/Contents/A/B/x.mp3");
        free(path);
        /* PPTH: len_path counts the UTF-16 NUL (docs/anlz.md 4.1). */
        CHECK_EQ(rx_rd_be32(files[i] + 28 + 12), 2 * (19 + 1));
        CHECK_EQ(rx_rd_be16(files[i] + 28 + 16 + 2 * 19), 0);
    }

    RXBeat *beats;
    int nb;
    CHECK_EQ(rx_anlz_read_beats(f.dat, f.dat_size, &beats, &nb), 0);
    CHECK_EQ(nb, 10);
    CHECK(nb >= 2 && beats[0].number == 1 && beats[0].bpm == 120.0 && beats[0].time == 0.2);
    CHECK(nb >= 2 && beats[1].number == 2 && beats[1].bpm == 120.0 && beats[1].time == 0.7);
    free(beats);

    /* PVBR trailer: CBR MP3 frames x 1152 (docs/anlz.md 4.2). */
    CHECK_EQ(rx_rd_be32(f.dat + 28 + 56 + 0x650), 190 * 1152);

    RXAnlzCueEntry *e;
    int declared;
    RXAnlzTag tag;
    int n = cues_of(f.dat, f.dat_size, "PCOB", 0, &e, &declared, &tag);
    CHECK(n == 1 && e[0].hot == 1);                         /* A-C only in .DAT */
    uint32_t type, count;
    CHECK(!rx_anlz_pcob_header(&tag, &type, &count) && type == 1 && count == 0xFFFFFFFFu);
    rx_anlz_cue_entries_free(e, n);
    n = cues_of(f.dat, f.dat_size, "PCOB", 1, &e, &declared, &tag);
    CHECK(n == 2 && e[0].type == 2 && e[0].status == 4 && e[1].type == 1 && e[1].status == 0);
    CHECK(n == 2 && e[0].prev == 0xFFFF && e[0].next == 1 && e[1].prev == 0 && e[1].next == 0xFFFF);
    CHECK(n == 2 && e[0].time == 2000 && e[0].end == 4000 && e[1].end == 0xFFFFFFFFu);
    CHECK(!rx_anlz_pcob_header(&tag, &type, &count) && type == 0 && count == 1);
    rx_anlz_cue_entries_free(e, n);

    n = cues_of(f.ext, f.ext_size, "PCOB", 0, &e, &declared, NULL);
    CHECK(n == 1 && e[0].hot == 6);                         /* D-H in .EXT */
    rx_anlz_cue_entries_free(e, n);
    n = cues_of(f.ext, f.ext_size, "PCOB", 1, &e, &declared, &tag);
    CHECK(n == 0 && !rx_anlz_pcob_header(&tag, &type, &count) && count == 0xFFFFFFFFu);
    rx_anlz_cue_entries_free(e, n);

    n = cues_of(f.ext, f.ext_size, "PCO2", 0, &e, &declared, NULL);
    CHECK(n == 2 && declared == 2);
    CHECK(n == 2 && e[0].hot == 1 && e[0].comment && !strcmp(e[0].comment, "intro"));
    /* Red is code 0x2A, and the RGB after it the measured FF0000. */
    CHECK(n == 2 && e[0].has_color && e[0].color[0] == 0x2A && e[0].color[1] == 0xFF &&
          e[0].color[2] == 0 && e[0].color[3] == 0);
    /* F without a colour: code 0 and dj's green for slots E-H. */
    CHECK(n == 2 && e[1].hot == 6 && e[1].color[0] == 0 && e[1].color[2] == 0xFF);
    rx_anlz_cue_entries_free(e, n);
    n = cues_of(f.ext, f.ext_size, "PCO2", 1, &e, &declared, NULL);
    CHECK(n == 2 && e[0].num == 4 && e[0].den == 1);         /* 2 s at 120 BPM = 4 beats */
    CHECK(n == 2 && e[1].num == 0 && e[1].den == 0 && !e[1].comment);
    rx_anlz_cue_entries_free(e, n);

    /* The public cue reader: PCO2 from the .EXT, active flags from .DAT. */
    RXPlanCue *rc;
    CHECK_EQ(rx_anlz_read_cues(f.ext, f.ext_size, &rc, &n), 0);
    CHECK_EQ(n, 4);
    CHECK(n == 4 && rc[0].hot == 0 && rc[0].time == 0.5 && !strcmp(rc[0].name, "intro") &&
          rc[0].color_code == 0x2A && rc[0].rgb == 0xFF0000);
    CHECK(n == 4 && rc[2].hot == -1 && rc[2].loop_end == 4.0);
    free(rc);
    CHECK_EQ(rx_anlz_read_cues(f.dat, f.dat_size, &rc, &n), 0);
    CHECK(n == 3 && rc[1].active && rc[1].loop_end == 4.0 && !rc[2].active);
    free(rc);

    /* PWVC carries the gains PWV7 was scaled by (docs/anlz.md 4.14):
     * 0, low, mid, high, big-endian, len_header 0x0E, 20 bytes. */
    RXAnlzTag *tags;
    int nt;
    CHECK_EQ(rx_anlz_parse(f.ex2, f.ex2_size, &tags, &nt), 0);
    const RXAnlzTag *pwvc = nt == 4 ? &tags[3] : NULL;
    static const uint8_t want_pwvc[20] = {
        'P', 'W', 'V', 'C', 0, 0, 0, 0x0E, 0, 0, 0, 0x14, 0, 0, 0, 80, 0, 0x8A, 0x01, 0x8C,
    };
    CHECK(pwvc && pwvc->raw_size == 20 && !memcmp(pwvc->raw, want_pwvc, 20));
    free(tags);
    /* No gains: the neutral 80, 100, 100. */
    RXAnlzFiles g;
    RXWaveforms w0 = w;
    w0.gains[0] = w0.gains[1] = w0.gains[2] = 0;
    CHECK_EQ(rx_anlz_build(&t, &w0, &g), 0);
    CHECK_EQ(rx_anlz_parse(g.ex2, g.ex2_size, &tags, &nt), 0);
    CHECK(nt == 4 && rx_rd_be16(tags[3].raw + 0x0E) == 80 && rx_rd_be16(tags[3].raw + 0x10) == 100 &&
          rx_rd_be16(tags[3].raw + 0x12) == 100);
    free(tags);
    rx_anlz_files_free(&g);

    /* Waveforms read back from the three files. */
    RXWaveforms r = { 0 };
    CHECK_EQ(rx_anlz_read_waveforms(f.dat, f.dat_size, &r), 0);
    CHECK_EQ(rx_anlz_read_waveforms(f.ext, f.ext_size, &r), 0);
    CHECK_EQ(rx_anlz_read_waveforms(f.ex2, f.ex2_size, &r), 0);
    CHECK_EQ(r.nb_detail, 750);
    CHECK(!memcmp(r.pwav, w.pwav, sizeof(w.pwav)) && !memcmp(r.pwv2, w.pwv2, sizeof(w.pwv2)));
    CHECK(!memcmp(r.pwv4, w.pwv4, sizeof(w.pwv4)) && !memcmp(r.pwv6, w.pwv6, sizeof(w.pwv6)));
    CHECK(r.pwv3 && !memcmp(r.pwv3, w.pwv3, 750) && r.pwv5 && !memcmp(r.pwv5, w.pwv5, 1500) &&
          r.pwv7 && !memcmp(r.pwv7, w.pwv7, 2250));
    free(r.pwv3);
    free(r.pwv5);
    free(r.pwv7);

    rx_anlz_files_free(&f);
    free(w.pwv3);
    free(w.pwv5);
    free(w.pwv7);
}

/* PVB2 for FLAC: entry i is the frame holding sample i * (total / 400). */
static void flac_seek(void)
{
    RXPlanTrack t = { .usb_path = "/Contents/A/B/y.flac", .duration = 10.0 };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "flac");
    RXFlacInfo *fi = &t.probe.flac;
    int nf = 108;
    *fi = (RXFlacInfo) { .valid = true, .total_samples = 441000, .min_block = 4096, .max_block = 4096 };
    fi->frames = calloc((size_t)nf, sizeof(*fi->frames));
    fi->nb_frames = nf;
    for (int i = 0; i < nf; i++) {
        fi->frames[i].sample = (int64_t)i * 4096;
        fi->frames[i].offset = (int64_t)i * 1000;
        fi->frames[i].block = i == nf - 1 ? (int)(441000 - (int64_t)i * 4096) : 4096;
    }
    RXAnlzFiles f;
    CHECK_EQ(rx_anlz_build(&t, NULL, &f), 0);
    CHECK(order_is(f.ext, f.ext_size, "PPTH PCOB PCOB PCO2 PCO2 PQT2 PVB2"));
    RXAnlzTag *tags;
    int nb;
    CHECK_EQ(rx_anlz_parse(f.ext, f.ext_size, &tags, &nb), 0);
    const RXAnlzTag *p = &tags[nb - 1];
    CHECK_EQ(p->raw_size, 0x20 + 20 * 400);
    CHECK_EQ(rx_rd_be64(p->raw + 0x10), 441000);
    CHECK_EQ(rx_rd_be32(p->raw + 0x18), 400);
    for (int i = 0; i < 400; i++) {
        const uint8_t *e = p->raw + 0x20 + 20 * i;
        int64_t frame = (int64_t)i * (441000 / 400) / 4096;
        CHECK_EQ(rx_rd_be64(e), frame * 4096);
        CHECK_EQ(rx_rd_be64(e + 8), frame * 1000);
        CHECK_EQ(rx_rd_be32(e + 16), frame == nf - 1 ? 441000 - frame * 4096 : 4096);
    }
    free(tags);
    /* No waveform tags without waveforms, and an all-zero PVBR. */
    CHECK(order_is(f.dat, f.dat_size, "PPTH PVBR PQTZ PCOB PCOB"));
    CHECK(order_is(f.ex2, f.ex2_size, "PPTH"));
    rx_anlz_files_free(&f);
    free(fi->frames);
}

/* dj's test_anlz_reads_rekordbox_files, plus the readers on them. */
static void rekordbox_files(void)
{
    const char *names[3] = { "DAT", "EXT", "2EX" };
    uint8_t *b[3] = { 0 };
    size_t n[3] = { 0 };
    for (int i = 0; i < 3; i++) {
        char path[64];
        snprintf(path, sizeof(path), "tests/data/anlz/ANLZ0000.%s", names[i]);
        if (rx_file_read(path, &b[i], &n[i]) < 0) {
            fprintf(stderr, "cannot read %s\n", path);
            failures++;
            return;
        }
        RXAnlzTag *tags;
        int nb;
        CHECK_EQ(rx_anlz_parse(b[i], n[i], &tags, &nb), 0);
        CHECK(nb > 0 && !strcmp(tags[0].fourcc, "PPTH"));
        free(tags);
        char *path_in = NULL;
        CHECK_EQ(rx_anlz_read_path(b[i], n[i], &path_in), 0);
        CHECK(path_in && path_in[0] == '/');
        free(path_in);
    }
    RXAnlzCueEntry *e;
    int declared;
    int c = cues_of(b[1], n[1], "PCO2", 0, &e, &declared, NULL);
    CHECK_EQ(c, 8);
    CHECK_EQ(declared, 8);
    for (int i = 0; i < c; i++)
        CHECK_EQ(e[i].hot, i + 1);
    rx_anlz_cue_entries_free(e, c);

    RXBeat *beats;
    int nb;
    CHECK_EQ(rx_anlz_read_beats(b[0], n[0], &beats, &nb), 0);
    CHECK(nb > 0);
    for (int i = 0; i < nb; i++)
        CHECK(beats[i].number >= 1 && beats[i].number <= 4);
    free(beats);

    /* The fixture is anonymised: no detail waveforms, only the previews. */
    RXWaveforms w = { 0 };
    for (int i = 0; i < 3; i++)
        CHECK_EQ(rx_anlz_read_waveforms(b[i], n[i], &w), 0);
    CHECK(w.nb_detail == 0 && !w.pwv3);
    /* rekordbox normalises the blue preview to 25 at most (docs/anlz.md 4.7). */
    int hmax = 0;
    for (int i = 0; i < RX_WAVE_PWAV_COLUMNS; i++)
        hmax = RX_MAX(hmax, w.pwav[i] & 31);
    CHECK(hmax > 0 && hmax <= 25);

    /* An older writer generation (docs/anlz.md 4.5): hot lists ascending,
     * status 1, A-C in the .DAT and D-H in the .EXT; custom pad colours as
     * code 0 with an RGB; a memory PCO2 entry without colour bytes. */
    c = cues_of(b[0], n[0], "PCOB", 0, &e, &declared, NULL);
    CHECK(c == 3 && e[0].hot == 1 && e[2].hot == 3 && e[0].status == 1);
    rx_anlz_cue_entries_free(e, c);
    c = cues_of(b[1], n[1], "PCOB", 0, &e, &declared, NULL);
    CHECK(c == 5 && e[0].hot == 4 && e[4].hot == 8);
    rx_anlz_cue_entries_free(e, c);
    c = cues_of(b[1], n[1], "PCO2", 0, &e, &declared, NULL);
    CHECK(c == 8 && e[0].has_color && e[0].color[0] == 0 && e[0].color[1] == 0xF4 && e[0].time == 68);
    rx_anlz_cue_entries_free(e, c);
    c = cues_of(b[1], n[1], "PCO2", 1, &e, &declared, NULL);
    CHECK(c == 1 && e[0].hot == 0 && !e[0].has_color && e[0].time == 660);
    rx_anlz_cue_entries_free(e, c);
    RXPlanCue *rc;
    CHECK_EQ(rx_anlz_read_cues(b[1], n[1], &rc, &c), 0);
    CHECK(c == 9 && rc[0].hot == 0 && rc[0].color_code == 0 && rc[0].rgb == 0xF4D338 && rc[8].hot == -1);
    free(rc);
    for (int i = 0; i < 3; i++)
        free(b[i]);
}

/* A dynamic grid (rxbox/model.h): every measured beat goes on the stick
 * at its own time with its own tempo, rekordbox's "dynamic" grids
 * (docs/anlz.md 4.3), and the straight section after them continues. */
static void dynamic(void)
{
    RXGrid g = { .beats_per_bar = 4 };
    CHECK(rx_grid_add_section(&g, 0.5, 120.0, 1) == 0);
    static const double tm[] = { 4.5, 5.0, 5.52, 6.01 };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tm); i++) {
        double next = i + 1 < RX_ARRAY_ELEMS(tm) ? tm[i + 1] : 6.5;
        CHECK(rx_grid_add_beat(&g, tm[i], 60.0 / (next - tm[i]), 0) == (int)i + 1);
    }
    CHECK(rx_grid_add_section(&g, 6.5, 121.0, 0) == 5);
    RXPlanTrack t = {
        .usb_path = "/Contents/A/B/d.wav",
        .duration = 10.0,
        .grid     = g,
    };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "pcm_s16le");
    RXAnlzFiles f;
    CHECK_EQ(rx_anlz_build(&t, NULL, &f), 0);
    RXBeat *b;
    int nb;
    CHECK_EQ(rx_anlz_read_beats(f.dat, f.dat_size, &b, &nb), 0);
    /* 0.5 .. 4.0 straight (8 beats), the four measured, then from 6.5. */
    CHECK(nb == 8 + 4 + 8);
    if (nb == 20) {
        CHECK(b[7].time == 4.0 && b[7].number == 4 && b[7].bpm == 120.0);
        CHECK(b[8].time == 4.5 && b[8].number == 1 && b[8].bpm == 120.0);
        CHECK(b[9].time == 5.0 && b[9].number == 2 && fabs(b[9].bpm - 115.38) < 0.01);
        CHECK(b[10].time == 5.52 && fabs(b[10].bpm - 122.45) < 0.01);
        CHECK(b[11].time == 6.01 && b[11].number == 4 && fabs(b[11].bpm - 122.45) < 0.01);
        CHECK(b[12].time == 6.5 && b[12].number == 1 && b[12].bpm == 121.0);
    }
    free(b);
    /* The loop quantum at a cue reads the local tempo there. */
    RXPlanCue c = { .time = 5.0, .loop_end = 5.52 + 0.49 + 0.49, .hot = -1 };
    uint16_t num, den;
    rx_anlz_loop_quantum(&c, &g, &num, &den);
    CHECK(num == 0 && den == 0);        /* 1.5 s at 115.38 is 2.88 beats: no quantum */
    c.loop_end = 5.52;
    rx_anlz_loop_quantum(&c, &g, &num, &den);
    CHECK(num == 1 && den == 1);        /* one beat at the local tempo */
    rx_anlz_files_free(&f);
    rx_grid_free(&g);
}

/* Beat numbers across tempo sections: a section that restarts the count
 * (beat 1) and one that continues it (beat 0) (docs/anlz.md 4.3: every
 * beat carries its section's tempo; rxbox/model.h for the numbering). */
static void sections(void)
{
    for (int restart = 0; restart < 2; restart++) {
        RXTempoSection sec[2] = {
            { .start = 0.0, .bpm = 120.0, .beat = 1 },
            { .start = 10.25, .bpm = 128.0, .beat = restart ? 1 : 0 },
        };
        RXPlanTrack t = {
            .usb_path = "/Contents/A/B/s.wav",
            .duration = 20.0,
            .grid     = { .sections = sec, .nb_sections = 2, .beats_per_bar = 4 },
        };
        snprintf(t.probe.codec, sizeof(t.probe.codec), "pcm_s16le");
        RXAnlzFiles f;
        CHECK_EQ(rx_anlz_build(&t, NULL, &f), 0);
        RXBeat *b;
        int nb;
        CHECK_EQ(rx_anlz_read_beats(f.dat, f.dat_size, &b, &nb), 0);
        int first = -1;
        for (int i = 0; i < nb; i++) {
            CHECK(b[i].number >= 1 && b[i].number <= 4);
            CHECK(i == 0 || b[i].time > b[i - 1].time);
            if (first < 0 && b[i].time >= 10.25 - 1e-9)
                first = i;
        }
        CHECK(first > 0);
        if (first > 0) {
            /* 10.0 s is beat 21 of the first section, a downbeat. */
            CHECK(b[first - 1].bpm == 120.0 && b[first - 1].number == 1);
            CHECK(b[first].bpm == 128.0 && b[first].time == 10.25);
            CHECK_EQ(b[first].number, restart ? 1 : 2);
            CHECK_EQ(b[first + 1].number, restart ? 2 : 3);
        }
        free(b);
        rx_anlz_files_free(&f);
    }
}

/* Paths beyond the BMP: PPTH counts UTF-16 code units, a surrogate pair
 * being two (docs/anlz.md 4.1), and reads back unchanged. */
static void emoji_path(void)
{
    const char *path = "/Contents/A/B/\xF0\x9F\x8E\xB5 x.flac";   /* U+1F3B5 */
    RXPlanTrack t = { .usb_path = (char *)path, .duration = 1.0 };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "flac");
    RXAnlzFiles f;
    CHECK_EQ(rx_anlz_build(&t, NULL, &f), 0);
    size_t units = strlen("/Contents/A/B/") + 2 + strlen(" x.flac");
    CHECK_EQ(rx_rd_be32(f.dat + 28 + 12), 2 * (units + 1));
    CHECK_EQ(rx_rd_be16(f.dat + 28 + 16 + 2 * strlen("/Contents/A/B/")), 0xD83C);
    char *back = NULL;
    CHECK_EQ(rx_anlz_read_path(f.dat, f.dat_size, &back), 0);
    CHECK_STR(back, path);
    free(back);
    rx_anlz_files_free(&f);
}

/* Files a player or another program wrote: a header length that does not
 * fit is an error, and cue slots beyond H never reach the sync as slots
 * (a PCO2 hot cue numbered 0x80000000 overflowed the slot arithmetic). */
static void hostile(void)
{
    uint8_t pmai[28] = { 'P', 'M', 'A', 'I', 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 28 };
    RXAnlzTag *tags = NULL;
    int nb = -1;
    CHECK(rx_anlz_parse(pmai, sizeof(pmai), &tags, &nb) < 0);
    rx_wr_be32(pmai + 4, 4);
    CHECK(rx_anlz_parse(pmai, sizeof(pmai), &tags, &nb) < 0);
    rx_wr_be32(pmai + 4, 28);
    CHECK_EQ(rx_anlz_parse(pmai, sizeof(pmai), &tags, &nb), 0);
    CHECK_EQ(nb, 0);
    free(tags);

    RXTempoSection sec = { .start = 0.0, .bpm = 120.0, .beat = 1 };
    RXPlanCue cues[3];
    plan_cue(&cues[0], 1.0, 0, -1, false, "", "red");
    plan_cue(&cues[1], 2.0, 4, -1, false, "", "");
    plan_cue(&cues[2], 3.0, -1, -1, false, "", "");
    RXPlanTrack t = {
        .usb_path = "/Contents/A/B/h.flac",
        .duration = 10.0,
        .grid     = { .sections = &sec, .nb_sections = 1, .beats_per_bar = 4 },
        .cues     = cues,
        .nb_cues  = 3,
    };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "flac");
    RXAnlzFiles f;
    CHECK_EQ(rx_anlz_build(&t, NULL, &f), 0);
    /* The PCO2 hot list holds A then E: renumber A to 0x80000000 and E
     * to 9; the .DAT's PCOB hot list (A) to 0xFFFFFFFF. */
    CHECK_EQ(rx_anlz_parse(f.ext, f.ext_size, &tags, &nb), 0);
    for (int i = 0; i < nb; i++) {
        if (!memcmp(tags[i].fourcc, "PCO2", 4) && rx_rd_be32(tags[i].raw + 0x0C) == 1) {
            uint8_t *e = f.ext + (tags[i].raw - f.ext) + 0x14;
            rx_wr_be32(e + 0x0C, 0x80000000u);
            e += rx_rd_be32(e + 8);
            rx_wr_be32(e + 0x0C, 9);
        }
    }
    free(tags);
    CHECK_EQ(rx_anlz_parse(f.dat, f.dat_size, &tags, &nb), 0);
    for (int i = 0; i < nb; i++)
        if (!memcmp(tags[i].fourcc, "PCOB", 4) && rx_rd_be32(tags[i].raw + 0x0C) == 1)
            rx_wr_be32(f.dat + (tags[i].raw - f.dat) + 0x18 + 0x0C, 0xFFFFFFFFu);
    free(tags);
    const uint8_t *file[2] = { f.ext, f.dat };
    size_t size[2] = { f.ext_size, f.dat_size };
    for (int k = 0; k < 2; k++) {
        RXPlanCue *rc;
        int n;
        CHECK_EQ(rx_anlz_read_cues(file[k], size[k], &rc, &n), 0);
        CHECK_EQ(n, 1);                       /* the memory cue alone */
        for (int i = 0; i < n; i++)
            CHECK(rc[i].hot >= -1 && rc[i].hot < RX_HOT_CUE_SLOTS);
        CHECK(n == 1 && rc[0].hot == -1 && rc[0].time == 3.0);
        free(rc);
    }
    rx_anlz_files_free(&f);
}

int main(void)
{
    dir_hash();
    layout();
    flac_seek();
    rekordbox_files();
    sections();
    dynamic();
    emoji_path();
    hostile();
    return test_result("anlz");
}
