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
    RxAnlzTag *tags;
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
                   RxAnlzCueEntry **e, int *declared, RxAnlzTag *tag_out)
{
    RxAnlzTag *tags;
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

static void fill_waves(RxWaveforms *w, int n)
{
    *w = (RxWaveforms) { .nb_detail = n };
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

static void plan_cue(RxPlanCue *c, double time, int hot, double loop_end, bool active,
                     const char *name, const char *color)
{
    *c = (RxPlanCue) { .time = time, .hot = hot, .loop_end = loop_end, .active = active };
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
    RxTempoSection sec = { .start = 0.2, .bpm = 120.0, .beat = 1 };
    RxPlanCue cues[4];
    plan_cue(&cues[0], 0.5, 0, -1, false, "intro", "red");
    plan_cue(&cues[1], 1.0, 5, -1, false, "", "");
    plan_cue(&cues[2], 2.0, -1, 4.0, true, "", "");
    plan_cue(&cues[3], 3.0, -1, -1, false, "", "");
    RxPlanTrack t = {
        .id       = 1,
        .usb_path = "/Contents/A/B/x.mp3",
        .duration = 5.0,
        .grid     = { .sections = &sec, .nb_sections = 1, .beats_per_bar = 4 },
        .cues     = cues,
        .nb_cues  = 4,
    };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "mp3");
    t.probe.mp3 = (RxMp3Info) { .valid = true, .nb_frames = 190, .frame_samples = 1152 };
    RxWaveforms w;
    fill_waves(&w, 750);
    RxAnlzFiles f;
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

    RxBeat *beats;
    int nb;
    CHECK_EQ(rx_anlz_read_beats(f.dat, f.dat_size, &beats, &nb), 0);
    CHECK_EQ(nb, 10);
    CHECK(nb >= 2 && beats[0].number == 1 && beats[0].bpm == 120.0 && beats[0].time == 0.2);
    CHECK(nb >= 2 && beats[1].number == 2 && beats[1].bpm == 120.0 && beats[1].time == 0.7);
    free(beats);

    /* PVBR trailer: CBR MP3 frames x 1152 (docs/anlz.md 4.2). */
    CHECK_EQ(rx_rd_be32(f.dat + 28 + 56 + 0x650), 190 * 1152);

    RxAnlzCueEntry *e;
    int declared;
    RxAnlzTag tag;
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
    RxPlanCue *rc;
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
    RxAnlzTag *tags;
    int nt;
    CHECK_EQ(rx_anlz_parse(f.ex2, f.ex2_size, &tags, &nt), 0);
    const RxAnlzTag *pwvc = nt == 4 ? &tags[3] : NULL;
    static const uint8_t want_pwvc[20] = {
        'P', 'W', 'V', 'C', 0, 0, 0, 0x0E, 0, 0, 0, 0x14, 0, 0, 0, 80, 0, 0x8A, 0x01, 0x8C,
    };
    CHECK(pwvc && pwvc->raw_size == 20 && !memcmp(pwvc->raw, want_pwvc, 20));
    free(tags);
    /* No gains: the neutral 80, 100, 100. */
    RxAnlzFiles g;
    RxWaveforms w0 = w;
    w0.gains[0] = w0.gains[1] = w0.gains[2] = 0;
    CHECK_EQ(rx_anlz_build(&t, &w0, &g), 0);
    CHECK_EQ(rx_anlz_parse(g.ex2, g.ex2_size, &tags, &nt), 0);
    CHECK(nt == 4 && rx_rd_be16(tags[3].raw + 0x0E) == 80 && rx_rd_be16(tags[3].raw + 0x10) == 100 &&
          rx_rd_be16(tags[3].raw + 0x12) == 100);
    free(tags);
    rx_anlz_files_free(&g);

    /* Waveforms read back from the three files. */
    RxWaveforms r = { 0 };
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
    RxPlanTrack t = { .usb_path = "/Contents/A/B/y.flac", .duration = 10.0 };
    snprintf(t.probe.codec, sizeof(t.probe.codec), "flac");
    RxFlacInfo *fi = &t.probe.flac;
    int nf = 108;
    *fi = (RxFlacInfo) { .valid = true, .total_samples = 441000, .min_block = 4096, .max_block = 4096 };
    fi->frames = calloc((size_t)nf, sizeof(*fi->frames));
    fi->nb_frames = nf;
    for (int i = 0; i < nf; i++) {
        fi->frames[i].sample = (int64_t)i * 4096;
        fi->frames[i].offset = (int64_t)i * 1000;
        fi->frames[i].block = i == nf - 1 ? (int)(441000 - (int64_t)i * 4096) : 4096;
    }
    RxAnlzFiles f;
    CHECK_EQ(rx_anlz_build(&t, NULL, &f), 0);
    CHECK(order_is(f.ext, f.ext_size, "PPTH PCOB PCOB PCO2 PCO2 PQT2 PVB2"));
    RxAnlzTag *tags;
    int nb;
    CHECK_EQ(rx_anlz_parse(f.ext, f.ext_size, &tags, &nb), 0);
    const RxAnlzTag *p = &tags[nb - 1];
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
        RxAnlzTag *tags;
        int nb;
        CHECK_EQ(rx_anlz_parse(b[i], n[i], &tags, &nb), 0);
        CHECK(nb > 0 && !strcmp(tags[0].fourcc, "PPTH"));
        free(tags);
        char *path_in = NULL;
        CHECK_EQ(rx_anlz_read_path(b[i], n[i], &path_in), 0);
        CHECK(path_in && path_in[0] == '/');
        free(path_in);
    }
    RxAnlzCueEntry *e;
    int declared;
    int c = cues_of(b[1], n[1], "PCO2", 0, &e, &declared, NULL);
    CHECK_EQ(c, 8);
    CHECK_EQ(declared, 8);
    for (int i = 0; i < c; i++)
        CHECK_EQ(e[i].hot, i + 1);
    rx_anlz_cue_entries_free(e, c);

    RxBeat *beats;
    int nb;
    CHECK_EQ(rx_anlz_read_beats(b[0], n[0], &beats, &nb), 0);
    CHECK(nb > 0);
    for (int i = 0; i < nb; i++)
        CHECK(beats[i].number >= 1 && beats[i].number <= 4);
    free(beats);

    /* The fixture is anonymised: no detail waveforms, only the previews. */
    RxWaveforms w = { 0 };
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
    RxPlanCue *rc;
    CHECK_EQ(rx_anlz_read_cues(b[1], n[1], &rc, &c), 0);
    CHECK(c == 9 && rc[0].hot == 0 && rc[0].color_code == 0 && rc[0].rgb == 0xF4D338 && rc[8].hot == -1);
    free(rc);
    for (int i = 0; i < 3; i++)
        free(b[i]);
}

int main(void)
{
    dir_hash();
    layout();
    flac_seek();
    rekordbox_files();
    return test_result("anlz");
}
