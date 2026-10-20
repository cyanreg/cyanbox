/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* End to end, dj's test_export in cyanbox's model: a small library of
 * generated tracks in five formats (FLAC, MP3, WAV, AAC and Opus, every
 * one encoded to the stick's FLAC) sharing a folder cover, two playlists,
 * player and device presets; exported to a "stick" directory, inspected,
 * exported again (nothing copied), then changed the way a player would
 * (a hot cue moved and recoloured, a memory cue added, a history list, a
 * preset), which must block the next export until synced; pulled into the
 * library; and pruned when a track leaves the playlists.
 *
 * Then each stick format (CBStickFormat: FLAC, WAV, MP3) to a stick of its
 * own: every file of the type and extension the format says, the stick
 * inspected (each file the size its row says), each file's audio the
 * source's decoded at 44.1 kHz (exactly for the 16-bit WAV, else within
 * the dither; the MP3s lined up with it sample for sample, as close as
 * 320 kbit/s gets, their frames the PVBR tag's), FLAC's PVB2 entries on
 * the file's own frames, exported again with nothing written, and a
 * source changed since encoded again.
 *
 * Needs the ffmpeg CLI with libmp3lame and libopus; skips (77) without. */

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cyanbox/audio.h"
#include "cyanbox/export.h"
#include "cyanbox/colors.h"

#include "src/export/export_priv.h"
#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "tests/formats/test.h"

static char root[512], music[600], libdir[600], stick[600];

typedef struct Stages {
    int copy, skip, prune, check, rewrite, transcode;
    int anlz, art;              /* tracks and artworks with a file written (an item) */
    int anlz_steps;             /* tracks looked at */
    bool quiet;
} Stages;

static bool on_progress(void *opaque, const char *stage, int done, int total, const char *item)
{
    Stages *s = opaque;
    /* Files, not the quarter-second ticks between them (no item). */
    if (!strcmp(stage, "copy") && item)
        s->copy++;
    else if (!strcmp(stage, "skip") && item)
        s->skip++;
    else if (!strcmp(stage, "prune") && item && item[0] == '/')
        s->prune++;
    else if (!strcmp(stage, "rewrite"))
        s->rewrite++;
    else if (!strcmp(stage, "check"))
        s->check++;
    else if (!strcmp(stage, "transcode") && item && total > 0)
        s->transcode++;
    else if (!strcmp(stage, "analysis files") && total > 0)
        s->anlz_steps++, s->anlz += !!item;
    else if (!strcmp(stage, "artwork") && total > 0)
        s->art += !!item;
    if (item && !s->quiet && strcmp(stage, "copy") && strcmp(stage, "skip") &&
        strcmp(stage, "analysis files") && strcmp(stage, "artwork") && strcmp(stage, "rewrite"))
        fprintf(stderr, "  [%s] %s\n", stage, item);
    return true;
}

static void on_line(void *opaque, const char *line)
{
    if (opaque)
        fprintf(stderr, "  %s\n", line);
}

static int sh(const char *fmt, ...) CB_PRINTF(1, 2);
static int sh(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *cmd = cb_vasprintf(fmt, ap);
    va_end(ap);
    int r = cmd ? system(cmd) : -1;
    free(cmd);
    return r;
}

/* A kick on every beat at `bpm` from `downbeat`, with a hat between. */
static int make_track(const char *name, double bpm, double downbeat, const char *codec_args)
{
    double p = 60.0 / bpm;
    return sh("ffmpeg -v error -nostdin -y -f lavfi -i \"aevalsrc='0.7*sin(2*PI*55*t)*exp(-14*mod(t-%.4f+%.4f*64,%.6f))"
              "+0.05*sin(2*PI*7000*t)*exp(-60*mod(t-%.4f+%.4f*64.5,%.6f))':s=44100:d=20\" "
              "-metadata title=%s -metadata artist=Synth -metadata album=Synthetic %s '%s/%s'",
              downbeat, p, p, downbeat, p, p, name, codec_args, music, name);
}

static CBTrack *track_named(CBLibrary *lib, const char *file)
{
    for (int i = 0; i < lib->nb_tracks; i++)
        if (!strcmp(cb_path_base(lib->tracks[i]->path), file))
            return lib->tracks[i];
    return NULL;
}

static int kv_add(CBKv **kv, int *nb, const char *key, const char *value)
{
    CBKv *n = realloc(*kv, (size_t)(*nb + 1) * sizeof(*n));
    if (!n)
        return CB_ERROR(ENOMEM);
    *kv = n;
    n[*nb] = (CBKv) { .key = cb_strdup(key), .value = cb_strdup(value) };
    (*nb)++;
    return 0;
}

static int read_stick(const char *path, uint8_t **b, size_t *n)
{
    char *p = cb_stick_file(stick, path);
    int ret = p ? cb_file_read(p, b, n) : CB_ERROR(ENOMEM);
    free(p);
    return ret;
}

static int count_items(CBLibrary *lib, enum CBSyncKind kind, CBSyncItem **out, int *nb_out)
{
    CBStickState s;
    if (cb_stick_read(lib, stick, &s) < 0)
        return -1;
    CBSyncItem *items;
    int nb, n = 0;
    if (cb_stick_diff(lib, &s, &items, &nb) < 0) {
        cb_stick_state_free(&s);
        return -1;
    }
    for (int i = 0; i < nb; i++)
        n += items[i].kind == kind;
    cb_stick_state_free(&s);
    if (out) {
        *out = items;
        *nb_out = nb;
    } else {
        free(items);
    }
    return n;
}

static int setup_library(CBLibrary *lib)
{
    static const struct { const char *name; double bpm, downbeat; const char *args; } files[] = {
        { "one.flac", 126.0, 0.25, "-c:a flac" },
        { "two.mp3", 128.0, 0.5, "-c:a libmp3lame -b:a 320k" },
        { "three.wav", 140.0, 0.1, "-c:a pcm_s16le" },
        { "four.m4a", 122.0, 0.3, "-c:a aac -b:a 256k" },
        { "five.opus", 130.0, 0.2, "-c:a libopus" },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(files); i++)
        if (make_track(files[i].name, files[i].bpm, files[i].downbeat, files[i].args))
            return 77;
    /* A folder cover the tracks share: one artwork row (dj hashed it). */
    if (sh("ffmpeg -v error -nostdin -y -f lavfi -i color=c=orange:s=320x320 -frames:v 1 '%s/front.jpg'", music))
        return 77;

    CHECK_EQ(cb_library_open(lib, libdir), 0);
    for (size_t i = 0; i < CB_ARRAY_ELEMS(files); i++) {
        char *path = cb_asprintf("%s/%s", music, files[i].name);
        int err = 0;
        CBTrack *t = cb_library_add_file(lib, path, &err);
        free(path);
        CHECK(t);
        if (!t)
            return 1;
        CHECK_EQ(cb_grid_init(&t->grid, files[i].downbeat, files[i].bpm), 0);
        snprintf(t->key, sizeof(t->key), "%s", i % 2 ? "Am" : "F#");
        CBCue c;
        cb_cue_init(&c, files[i].downbeat);
        cb_track_add_cue(t, &c);
        if (i == 0) {
            /* dj's hand edit: a hot cue with a 4-beat loop on bar 5. */
            double p = 60.0 / files[i].bpm;
            cb_cue_init(&c, files[i].downbeat + 16 * p);
            c.hot = 0;
            c.loop_end = c.time + 4 * p;
            snprintf(c.name, sizeof(c.name), "loop");
            snprintf(c.color, sizeof(c.color), "red");
            cb_track_add_cue(t, &c);
        }
        CHECK_EQ(cb_library_save_track(lib, t), 0);

        CBAudio a;
        CHECK_EQ(cb_decode(t->path, 1, 0, &a), 0);
        CBWaveforms w;
        CHECK_EQ(cb_waveforms_compute(a.samples, a.nb_frames, a.rate, &w), 0);
        CHECK_EQ(cb_library_waveforms_save(lib, t->id, &w), 0);
        cb_waveforms_free(&w);
        cb_audio_free(&a);
    }

    CBPlaylist *set = cb_library_playlist_create(lib, lib->root_folder, "Set");
    CBPlaylistFolder *f = cb_library_folder_create(lib, lib->root_folder, "Peak");
    CBPlaylist *peak = f ? cb_library_playlist_create(lib, f, "Peak time") : NULL;
    CHECK(set && f && peak);
    if (!set || !peak)
        return 1;
    cb_library_playlist_add(lib, set, track_named(lib, "two.mp3"), -1);
    cb_library_playlist_add(lib, set, track_named(lib, "one.flac"), -1);
    cb_library_playlist_add(lib, peak, track_named(lib, "three.wav"), -1);
    cb_library_playlist_add(lib, peak, track_named(lib, "four.m4a"), -1);
    cb_library_playlist_add(lib, peak, track_named(lib, "five.opus"), -1);

    kv_add(&lib->cfg.player, &lib->cfg.nb_player, "quantize", "true");
    kv_add(&lib->cfg.device, &lib->cfg.nb_device, "waveform_color", "3band");
    CHECK_EQ(cb_library_config_save(lib), 0);
    return 0;
}

static void check_stick(CBLibrary *lib)
{
    uint8_t *b;
    size_t n;
    CHECK_EQ(read_stick("/PIONEER/rekordbox/export.pdb", &b, &n), 0);
    CBPdb pdb;
    CHECK_EQ(cb_pdb_read(b, n, &pdb), 0);
    CHECK_EQ(pdb.nb_tracks, 5);
    CBPdbRawTrack *raw;
    int nr;
    CHECK_EQ(cb_pdb_raw_tracks(b, n, &raw, &nr), 0);
    static const struct { const char *file; int type; int tempo; const char *ext; } want[] = {
        { "one.flac", 5, 12600, ".flac" }, { "two.mp3", 5, 12800, ".flac" }, { "three.wav", 5, 14000, ".flac" },
        { "four.m4a", 5, 12200, ".flac" }, { "five.opus", 5, 13000, ".flac" },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(want); i++) {
        CBTrack *t = track_named(lib, want[i].file);
        bool found = false;
        for (int k = 0; t && k < nr; k++) {
            if (raw[k].id != t->id)
                continue;
            found = true;
            CHECK_EQ(raw[k].file_type, want[i].type);
            CHECK_EQ(raw[k].tempo, want[i].tempo);
            CHECK(cb_str_ends(raw[k].strings[CB_PDB_STR_FILE_PATH], want[i].ext));
            CHECK_EQ(raw[k].artwork_id, 1);          /* the shared folder cover */
        }
        CHECK(found);
    }
    /* Playlist "Set": two, one, with the library's ids. */
    bool set = false;
    for (int i = 0; i < pdb.nb_playlists; i++) {
        if (strcmp(pdb.playlists[i].name, "Set"))
            continue;
        set = true;
        CHECK(pdb.playlists[i].nb == 2 && pdb.playlists[i].track_ids[0] == track_named(lib, "two.mp3")->id &&
              pdb.playlists[i].track_ids[1] == track_named(lib, "one.flac")->id);
    }
    CHECK(set);

    /* one.flac: the grid from 0.25 s, the hot cue loop in the .DAT. */
    CBTrack *one = track_named(lib, "one.flac");
    for (int k = 0; one && k < pdb.nb_tracks; k++) {
        if (pdb.tracks[k].id != one->id)
            continue;
        uint8_t *dat;
        size_t dn;
        CHECK_EQ(read_stick(pdb.tracks[k].anlz_path, &dat, &dn), 0);
        CBBeat *beats;
        int nb;
        CHECK_EQ(cb_anlz_read_beats(dat, dn, &beats, &nb), 0);
        CHECK(nb > 10 && beats[0].number == 1 && fabs(beats[0].time - 0.25) < 0.0015);
        free(beats);
        CBPlanCue *cues;
        int nc;
        CHECK_EQ(cb_anlz_read_cues(dat, dn, &cues, &nc), 0);
        bool hot = false;
        for (int c = 0; c < nc; c++)
            hot |= cues[c].hot == 0 && cues[c].loop_end > cues[c].time;
        CHECK(hot);
        free(cues);
        free(dat);
    }
    cb_pdb_raw_tracks_free(raw, nr);
    cb_pdb_free(&pdb);
    free(b);

    CHECK_EQ(read_stick("/PIONEER/rekordbox/exportLibrary.db", &b, &n), 0);
    uint8_t *plain;
    size_t pn;
    CHECK_EQ(cb_sqlcipher_decrypt(b, n, cb_onelibrary_passphrase, NULL, 0, &plain, &pn), 0);
    CBOneLibTrack *ol;
    int no;
    CHECK_EQ(cb_onelibrary_read(plain, pn, &ol, &no), 0);
    CHECK_EQ(no, 5);
    for (int i = 0; i < no; i++) {
        free(ol[i].path);
        free(ol[i].title);
    }
    free(ol);
    free(plain);
    free(b);

    char *p;
    CHECK((p = cb_stick_file(stick, "/PIONEER/DEVSETTING.DAT")) && cb_file_exists(p));
    free(p);
    CHECK((p = cb_stick_file(stick, "/PIONEER/MYSETTING.DAT")) && cb_file_exists(p));
    free(p);
    CHECK((p = cb_stick_file(stick, "/PIONEER/DJMMYSETTING.DAT")) && !cb_file_exists(p));
    free(p);
    CHECK((p = cb_stick_file(stick, "/PIONEER/Artwork/00001/a1_m.jpg")) && cb_file_exists(p));
    free(p);
    CHECK((p = cb_stick_file(stick, "/PIONEER/Artwork/00001/b1.jpg")) && cb_file_exists(p));
    free(p);
    CHECK((p = cb_stick_file(stick, CB_SYNC_PATH)) && cb_file_exists(p));
    free(p);
}

/* ------------------------------------------------------------ formats */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* The stick file's audio against the source's decoded at its rate: the
 * same frames, exactly or within `steps` of 16 bits (a WAV holds the
 * plan's frames, which a resampled source's decode may miss by one). */
static void same_audio(const char *src, const char *made, double steps)
{
    CBAudio a, b;
    CHECK_EQ(cb_decode(made, 0, 0, &b), 0);
    CHECK_EQ(cb_decode(src, 0, b.rate, &a), 0);
    CHECK(llabs(a.nb_frames - b.nb_frames) <= 1 && a.channels == b.channels && a.rate == b.rate);
    double worst = 0;
    for (int64_t i = 0; i < (CB_MIN(a.nb_frames, b.nb_frames) - 1) * a.channels && a.channels == b.channels; i++)
        worst = CB_MAX(worst, fabs((double)a.samples[i] - b.samples[i]) * 32768.0);
    if (worst > steps)
        fprintf(stderr, "export: %s is off %s by %.3f steps\n", made, src, worst);
    CHECK(worst <= steps);
    cb_audio_free(&a);
    cb_audio_free(&b);
}

/* An MP3 on the stick against its source decoded at 44.1 kHz: lined up
 * (the difference is least at lag 0: LAME's delay was taken off what it
 * was fed), as close as 320 kbit/s gets, and of the frames its PVBR tag
 * names. */
static void same_mp3(const char *src, const char *made, const char *dir, const char *anlz_dat)
{
    CBAudio a, b;
    CHECK_EQ(cb_decode(made, 1, 0, &b), 0);
    CHECK_EQ(cb_decode(src, 1, b.rate, &a), 0);
    int64_t n = CB_MIN(a.nb_frames, b.nb_frames) - 4096;
    int best = 0;
    double least = INFINITY, err = 0, pow = 0;
    for (int lag = -64; lag <= 64; lag++) {
        double e = 0;
        for (int64_t i = 4096; i < n; i += 3)
            e += ((double)a.samples[i] - b.samples[i + lag]) * ((double)a.samples[i] - b.samples[i + lag]);
        if (e < least) {
            least = e;
            best = lag;
        }
    }
    for (int64_t i = 4096; i < n; i++) {
        err += ((double)a.samples[i] - b.samples[i]) * ((double)a.samples[i] - b.samples[i]);
        pow += (double)a.samples[i] * a.samples[i];
    }
    double snr = 10 * log10(pow / CB_MAX(err, 1e-30));
    if (best || snr <= 25)
        fprintf(stderr, "export: %s is %d frames off its source, %.1f dB\n", made, best, snr);
    CHECK(best == 0 && snr > 25);
    CBMp3Info mi;
    char *dat = cb_asprintf("%s%s", dir, anlz_dat);
    uint8_t *d = NULL;
    size_t dn = 0;
    CHECK(cb_mp3_scan(made, &mi) >= 0 && !mi.vbr && !mi.header_frame && mi.bitrate_kbps == 320);
    CHECK(dat && cb_file_read(dat, &d, &dn) >= 0);
    CBAnlzTag *tags = NULL;
    int nt = 0;
    bool seen = false;
    if (d && cb_anlz_parse(d, dn, &tags, &nt) >= 0)
        for (int i = 0; i < nt; i++)
            if (!strcmp(tags[i].fourcc, "PVBR") && tags[i].size >= 1608) {
                seen = true;
                CHECK_EQ((int64_t)be32(tags[i].body + 1604), mi.nb_frames * 1152);
            }
    CHECK(seen);
    free(tags);
    free(d);
    free(dat);
    cb_audio_free(&a);
    cb_audio_free(&b);
}

/* PVB2 in the track's .EXT: each entry a frame of the FLAC (its sync code
 * at the first frame's offset plus the entry's), the total its frames. */
static void check_pvb2(const char *dir, const char *anlz_dat, const char *flac)
{
    char *ext = cb_asprintf("%s%.*s.EXT", dir, (int)strlen(anlz_dat) - 4, anlz_dat);
    uint8_t *e = NULL, *f = NULL;
    size_t en = 0, fn = 0;
    CHECK(ext && cb_file_read(ext, &e, &en) >= 0 && cb_file_read(flac, &f, &fn) >= 0);
    CBFlacInfo fi;
    CHECK_EQ(cb_flac_scan(flac, &fi), 0);
    CBAnlzTag *tags = NULL;
    int nt = 0;
    bool seen = false;
    if (e && cb_anlz_parse(e, en, &tags, &nt) >= 0) {
        for (int i = 0; i < nt; i++) {
            if (strcmp(tags[i].fourcc, "PVB2") || tags[i].size < 20)
                continue;
            seen = true;
            const uint8_t *b = tags[i].body;
            CHECK_EQ((int64_t)be64(b + 4), fi.total_samples);
            uint32_t count = be32(b + 12);
            CHECK(count == 400 && tags[i].size >= 20 + 20 * (size_t)count);
            int bad = 0;
            for (uint32_t k = 0; k < count && tags[i].size >= 20 + 20 * (size_t)count; k++) {
                uint64_t off = be64(b + 20 + 20 * k + 8);
                uint64_t at = (uint64_t)fi.first_frame_offset + off;
                bad += at + 2 > fn || f[at] != 0xFF || (f[at + 1] & 0xFE) != 0xF8;
            }
            CHECK_EQ(bad, 0);
        }
    }
    CHECK(seen);
    free(tags);
    cb_flac_info_free(&fi);
    free(e);
    free(f);
    free(ext);
}

static void test_formats(CBLibrary *lib)
{
    static const char *const files[5] = { "one.flac", "two.mp3", "three.wav", "four.m4a", "five.opus" };
    static const struct {
        int format;
        const char *name;
        const char *ext[5];
        int type[5];
        double steps[5];                 /* off the source: 0 exact */
    } want[] = {
        /* one.flac is 24-bit (ffmpeg's choice), dithered to 16; three.wav
         * is 16-bit at 44.1 kHz, as it is */
        { CB_STICK_FLAC, "flac", { ".flac", ".flac", ".flac", ".flac", ".flac" }, { 5, 5, 5, 5, 5 },
          { 1.5, 1.5, 0, 1.5, 1.5 } },
        { CB_STICK_WAV, "wav", { ".wav", ".wav", ".wav", ".wav", ".wav" }, { 11, 11, 11, 11, 11 },
          { 1.5, 1.5, 0, 1.5, 1.5 } },
        { CB_STICK_MP3, "mp3", { ".mp3", ".mp3", ".mp3", ".mp3", ".mp3" }, { 1, 1, 1, 1, 1 },
          { -1, -1, -1, -1, -1 } },
    };
    for (size_t w = 0; w < CB_ARRAY_ELEMS(want); w++) {
        char *dir = cb_asprintf("%s/stick-%s", root, want[w].name);
        sh("rm -rf '%s'", dir);
        cb_mkdir_p(dir);
        CBExportOptions opt;
        cb_export_options_default(&opt);
        opt.whole_library = true;
        opt.format = want[w].format;
        Stages st = { .quiet = true };
        CHECK_EQ(cb_export(lib, dir, &opt, on_progress, &st), 0);
        CHECK_EQ(st.copy, 5);
        int problems = cb_inspect(dir, on_line, NULL);
        if (problems)
            cb_inspect(dir, on_line, (void *)1);
        CHECK_EQ(problems, 0);

        char *pdbp = cb_stick_file(dir, "/PIONEER/rekordbox/export.pdb");
        uint8_t *b = NULL;
        size_t n = 0;
        CHECK(pdbp && cb_file_read(pdbp, &b, &n) >= 0);
        CBPdbRawTrack *raw = NULL;
        int nr = 0;
        CHECK(b && cb_pdb_raw_tracks(b, n, &raw, &nr) >= 0);
        CHECK_EQ(nr, 5);
        for (int i = 0; i < 5; i++) {
            CBTrack *t = track_named(lib, files[i]);
            for (int k = 0; t && k < nr; k++) {
                if (raw[k].id != t->id)
                    continue;
                const char *path = raw[k].strings[CB_PDB_STR_FILE_PATH];
                CHECK_EQ(raw[k].file_type, want[w].type[i]);
                CHECK(cb_str_ends(path, want[w].ext[i]));
                char *made = cb_stick_file(dir, path);
                if (want[w].steps[i] < 0)
                    same_mp3(t->path, made, dir, raw[k].strings[CB_PDB_STR_ANALYZE_PATH]);
                else
                    same_audio(t->path, made, want[w].steps[i]);
                if (want[w].type[i] == 5)
                    check_pvb2(dir, raw[k].strings[CB_PDB_STR_ANALYZE_PATH], made);
                free(made);
            }
        }
        cb_pdb_raw_tracks_free(raw, nr);
        free(b);
        free(pdbp);

        /* Again: everything there already. */
        st = (Stages) { .quiet = true };
        CHECK_EQ(cb_export(lib, dir, &opt, on_progress, &st), 0);
        CHECK_EQ(st.copy, 0);
        CHECK_EQ(st.skip, 5);
        CHECK_EQ(st.anlz, 0);
        CHECK_EQ(cb_inspect(dir, on_line, NULL), 0);

        /* A source changed since: encoded again, and only it. */
        CBTrack *three = track_named(lib, "three.wav");
        struct timespec later[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = time(NULL) + 60 + (time_t)w } };
        CHECK(three && utimensat(AT_FDCWD, three->path, later, 0) == 0);
        st = (Stages) { .quiet = true };
        CHECK_EQ(cb_export(lib, dir, &opt, on_progress, &st), 0);
        CHECK_EQ(st.copy, 1);
        CHECK_EQ(cb_inspect(dir, on_line, NULL), 0);
        free(dir);
    }
}

/* What a player does at a gig: moves hot cue A by a second and makes it
 * blue, adds a memory cue at 10 s (written to the ANLZ files as rekordbox
 * would); appends a history list to exportLibrary.db; saves a preset. */
static void play_a_gig(CBLibrary *lib)
{
    CBTrack *one = track_named(lib, "one.flac");
    CBTrack *tr[1] = { one };
    CBPlan plan;
    CHECK_EQ(cb_plan_build(lib, tr, 1, NULL, 0, NULL, CB_TRANSCODE_STICK_FLAC, &plan), 0);
    CBPlanTrack *pt = &plan.tracks[0];
    /* The FLAC on the stick, as the export learnt it when it wrote it: a
     * player changes the cues, nothing else */
    char *made = cb_stick_file(stick, pt->usb_path);
    CBFlacInfo fi;
    CHECK(made && cb_flac_scan(made, &fi) >= 0);
    cb_flac_info_free(&pt->probe.flac);
    pt->probe.flac = fi;
    pt->total_samples = pt->probe.total_samples = fi.total_samples;
    pt->duration = pt->probe.duration = (double)fi.total_samples / pt->sample_rate;
    pt->file_size = cb_file_size(made);
    free(made);
    CBPlanCue *cues = calloc((size_t)pt->nb_cues + 1, sizeof(*cues));
    memcpy(cues, pt->cues, (size_t)pt->nb_cues * sizeof(*cues));
    for (int i = 0; i < pt->nb_cues; i++) {
        if (cues[i].hot == 0) {
            cues[i].time += 1.0;
            cues[i].loop_end += 1.0;
            cb_hot_cue_color("blue", &cues[i].color_code, &cues[i].rgb);
        }
    }
    cues[pt->nb_cues] = (CBPlanCue) { .time = 10.0, .hot = -1, .loop_end = -1 };
    free(pt->cues);
    pt->cues = cues;
    pt->nb_cues++;
    /* With the waveforms, as a player leaves them: the .EXT's cues sit
     * between PWV3 and the other waveforms, which the sync skips. */
    CBWaveforms w;
    CHECK_EQ(cb_library_waveforms_load(lib, one->id, &w), 0);
    CBAnlzFiles f;
    CHECK_EQ(cb_anlz_build(pt, &w, &f), 0);
    cb_waveforms_free(&w);
    char *dat = cb_plan_anlz_path(pt, ".DAT"), *ext = cb_plan_anlz_path(pt, ".EXT");
    char *pd = cb_stick_file(stick, dat), *pe = cb_stick_file(stick, ext);
    CHECK_EQ(cb_file_write_atomic(pd, f.dat, f.dat_size), 0);
    CHECK_EQ(cb_file_write_atomic(pe, f.ext, f.ext_size), 0);
    free(dat);
    free(ext);
    free(pd);
    free(pe);
    cb_anlz_files_free(&f);
    cb_plan_free(&plan);

    /* History: a OneLibrary player plays two then one. */
    uint8_t *b, *plain;
    size_t n, pn;
    CHECK_EQ(read_stick("/PIONEER/rekordbox/exportLibrary.db", &b, &n), 0);
    CHECK_EQ(cb_sqlcipher_decrypt(b, n, cb_onelibrary_passphrase, NULL, 0, &plain, &pn), 0);
    free(b);
    sqlite3 *db;
    CHECK_EQ(cb_onelibrary_open_plain(plain, pn, &db), 0);
    free(plain);
    char *sql = cb_asprintf("INSERT INTO history VALUES (1, 0, 'HISTORY 001', 0, 0);"
                            "INSERT INTO history_content VALUES (1, %llu, 1), (1, %llu, 2);",
                            (unsigned long long)track_named(lib, "two.mp3")->id,
                            (unsigned long long)one->id);
    CHECK_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    free(sql);
    sqlite3_int64 sz;
    unsigned char *img = sqlite3_serialize(db, "main", &sz, 0);
    sqlite3_close(db);
    CHECK(img != NULL);
    uint8_t *enc;
    size_t en;
    CHECK_EQ(cb_sqlcipher_encrypt(img, (size_t)sz, cb_onelibrary_passphrase, NULL, &enc, &en), 0);
    sqlite3_free(img);
    char *p = cb_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    CHECK_EQ(cb_file_write_atomic(p, enc, en), 0);
    free(p);
    free(enc);

    /* The player saves My Settings with a ±16 % tempo range (the default
     * is ±6 %). */
    CBKv kv[2] = { { "quantize", "true" }, { "tempo_range", "16" } };
    CHECK_EQ(cb_settings_encode("MYSETTING.DAT", kv, 2, &b, &n), 0);
    p = cb_stick_file(stick, "/PIONEER/MYSETTING.DAT");
    CHECK_EQ(cb_file_write_atomic(p, b, n), 0);
    free(p);
    free(b);
}

int main(void)
{
    if (system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libopus") ||
        system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libmp3lame")) {
        fprintf(stderr, "export: ffmpeg with libmp3lame and libopus needed, skipping\n");
        return 77;
    }
    const char *base = getenv("CB_TEST_DIR");
    snprintf(root, sizeof(root), "%s", base ? base : "build-formats/test-stick");
    snprintf(music, sizeof(music), "%s/music", root);
    snprintf(libdir, sizeof(libdir), "%s/lib", root);
    snprintf(stick, sizeof(stick), "%s/stick", root);
    sh("rm -rf '%s'", root);
    cb_mkdir_p(music);
    cb_mkdir_p(stick);

    CBLibrary lib = { 0 };
    int r = setup_library(&lib);
    if (r)
        return r == 77 ? 77 : test_result("export");

    CBExportOptions opt;
    cb_export_options_default(&opt);
    Stages st = { 0 };
    /* A dry run lists every track as one it would encode. */
    opt.dry_run = true;
    st = (Stages) { .quiet = true };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.transcode, 5);
    opt.dry_run = false;
    st = (Stages) { 0 };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.copy, 5);
    CHECK_EQ(st.anlz_steps, 5);
    CHECK_EQ(st.anlz, 5);                    /* every track's analysis files written */
    CHECK_EQ(st.art, 1);                     /* the shared folder cover */
    int problems = cb_inspect(stick, on_line, NULL);
    if (problems)
        cb_inspect(stick, on_line, (void *)1);
    CHECK_EQ(problems, 0);
    check_stick(&lib);

    /* A second export copies nothing and the stick stays valid. */
    st = (Stages) { .quiet = true };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.copy, 0);
    CHECK_EQ(st.skip, 5);
    CHECK_EQ(st.anlz_steps, 5);
    CHECK_EQ(st.anlz, 0);                    /* the stick's analysis files are kept, byte for byte */
    CHECK_EQ(st.art, 0);
    CHECK_EQ(cb_inspect(stick, on_line, NULL), 0);
    CHECK_EQ(count_items(&lib, CB_SYNC_HOT_CUE_SET, NULL, NULL), 0);

    /* Library edits since the export are not unsynced changes. */
    CBTrack *three = track_named(&lib, "three.wav");
    CBCue extra;
    cb_cue_init(&extra, 5.0);
    extra.hot = 2;
    cb_track_add_cue(three, &extra);
    cb_library_save_track(&lib, three);
    CBSyncItem *items;
    int nb;
    CHECK_EQ(count_items(&lib, CB_SYNC_HOT_CUE_CLEARED, &items, &nb), 0);
    CHECK_EQ(nb, 0);
    free(items);

    play_a_gig(&lib);
    CHECK_EQ(count_items(&lib, CB_SYNC_HOT_CUE_SET, &items, &nb), 1);
    int plays = 0, added = 0, settings = 0;
    for (int i = 0; i < nb; i++) {
        plays += items[i].kind == CB_SYNC_PLAY;
        added += items[i].kind == CB_SYNC_MEMORY_ADDED;
        if (items[i].kind == CB_SYNC_SETTING) {
            settings++;
            CHECK_STR(items[i].setting_key, "player.tempo_range");
            CHECK_STR(items[i].setting_value, "16");
            CHECK_STR(items[i].setting_old, "6");
        }
        if (items[i].kind == CB_SYNC_HOT_CUE_SET)
            CHECK_STR(items[i].cue.color, "blue");
    }
    CHECK_EQ(plays, 2);
    CHECK_EQ(added, 1);
    CHECK_EQ(settings, 1);
    CHECK_EQ(nb, 5);
    free(items);

    /* Unsynced changes block the export. */
    st = (Stages) { .quiet = true };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), CB_ERROR(EBUSY));

    CBTrack *one = track_named(&lib, "one.flac");
    int idx = cb_track_hot_cue(one, 0);
    double before = idx >= 0 ? one->cues[idx].time : 0;
    int two_plays = track_named(&lib, "two.mp3")->play_count;
    CHECK_EQ(cb_pull(&lib, stick, NULL, NULL), 5);
    idx = cb_track_hot_cue(one, 0);
    CHECK(idx >= 0 && fabs(one->cues[idx].time - before - 1.0) < 0.002);
    CHECK(idx >= 0 && !strcmp(one->cues[idx].color, "blue"));
    CHECK(cb_track_memory_cue_at(one, 10.0, 0.02) >= 0);
    CHECK_EQ(track_named(&lib, "two.mp3")->play_count, two_plays + 1);
    bool tempo = false;
    for (int i = 0; i < lib.cfg.nb_player; i++)
        tempo |= !strcmp(lib.cfg.player[i].key, "tempo_range") && !strcmp(lib.cfg.player[i].value, "16");
    CHECK(tempo);

    /* Synced: nothing left, plays not proposed twice, the export runs. */
    CHECK_EQ(count_items(&lib, CB_SYNC_PLAY, &items, &nb), 0);
    CHECK_EQ(nb, 0);
    free(items);
    CHECK_EQ(cb_pull(&lib, stick, NULL, NULL), 0);
    st = (Stages) { .quiet = true };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), 0);
    /* Only three, whose cues the library changed, gets its analysis files
     * written again: one's on the stick hold the player's change, pulled
     * since, byte for byte what the export writes. */
    CHECK_EQ(st.anlz, 1);
    CHECK_EQ(cb_inspect(stick, on_line, NULL), 0);

    /* four leaves the playlists: a dry run lists its files for pruning,
     * the export with prune removes them. */
    CBPlaylist *pl[8];
    int npl = cb_library_playlists_flat(&lib, pl, 8);
    for (int i = 0; i < npl; i++)
        for (int k = 0; k < pl[i]->nb; k++)
            if (pl[i]->track_ids[k] == track_named(&lib, "four.m4a")->id)
                cb_library_playlist_remove_at(&lib, pl[i], k);
    opt.dry_run = true;
    st = (Stages) { .quiet = true };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.prune, 4);                   /* the audio and three ANLZ files */
    CHECK(st.rewrite > 0 && st.skip == 4);
    opt.dry_run = false;
    opt.prune = true;
    st = (Stages) { .quiet = true };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.prune, 4);
    char *four = cb_asprintf("%s/Contents/Synth/Synthetic/four.flac", stick);
    CHECK(four && !cb_file_exists(four));
    free(four);
    CHECK_EQ(cb_inspect(stick, on_line, NULL), 0);

    test_formats(&lib);

    cb_library_close(&lib);
    if (!failures)
        sh("rm -rf '%s'", root);
    return test_result("export");
}
