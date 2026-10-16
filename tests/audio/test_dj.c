/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The port against the Python original (~/projects/dj, or $CB_DJ_PYTHON):
 *
 * - MP3: cb_decode's mono mix against dj's audio.decode_player (ffmpeg CLI,
 *   -flags2 +skip_manual). Sample for sample the same when the Info frame
 *   does not count; one frame (1152 samples) of silence longer when it does,
 *   the rest identical (the frame-map fix, docs/OVERVIEW.md section 3.1);
 *   one frame longer for a damaged frame, which dj's ffmpeg dropped (3.9).
 *   cb_mp3_scan's frame count, VBR flag and bitrate against dj's mp3.scan.
 * - FLAC: cb_flac_scan's table against dj's flac.scan, every frame, and the
 *   400 PVB2 entries dj's seek_entries picks, recomputed here from it.
 * - Artwork: cb_image_jpeg_square against dj's _jpeg (Pillow) on the same
 *   picture: the pixels and the quantisation tables.
 *
 * Skipped (exit 77) when dj's Python is not there. */

#include "cyanbox/audio.h"

#include <inttypes.h>
#include <math.h>
#include <unistd.h>

#include "util.h"

static int failures;

static const char script[] =
    "import sys, numpy as np\n"
    "from pathlib import Path\n"
    "from dj import audio\n"
    "from dj.formats import flac, mp3\n"
    "from dj.export import _jpeg\n"
    "mode, src, out = sys.argv[1], Path(sys.argv[2]), sys.argv[3]\n"
    "if mode == 'decode':\n"
    "    audio.decode_player(src, audio.probe(src)).astype(np.float32).tofile(out)\n"
    "elif mode == 'mp3':\n"
    "    i = mp3.scan(src)\n"
    "    open(out, 'w').write(f'{i.frames} {i.samples_per_frame} {int(i.vbr)} {i.bitrate}\\n')\n"
    "elif mode == 'flac':\n"
    "    fi = flac.scan(src)\n"
    "    with open(out, 'w') as f:\n"
    "        f.write(f'{fi.audio_start} {fi.block_size} {fi.total_samples} {len(fi.frames)}\\n')\n"
    "        for e in fi.frames: f.write('%d %d %d\\n' % e)\n"
    "        for e in flac.seek_entries(fi): f.write('%d %d %d\\n' % e)\n"
    "elif mode == 'jpeg':\n"
    "    open(out, 'wb').write(_jpeg(src.read_bytes(), int(sys.argv[4])))\n";

static const char *python;

static int dj(const char *mode, const char *src, const char *out, const char *extra)
{
    const char *argv[] = { python, "-I", "-c", script, mode, src, out, extra, NULL };
    int ret = run(argv);
    CHECK(ret == 0, "dj %s %s: exit %d", mode, src, ret);
    return ret;
}

static float *read_f32(const char *path, int64_t *n)
{
    uint8_t *d;
    size_t size;
    if (cb_file_read(path, &d, &size) < 0) {
        *n = 0;
        return NULL;
    }
    *n = (int64_t)(size / sizeof(float));
    return (float *)d;
}

static void compare_mp3(const char *dir, const char *name, int extra_frames, const char *what)
{
    char *mp3 = test_path(dir, name);
    char *f32 = test_path(dir, "dj.f32");
    if (dj("decode", mp3, f32, NULL)) {
        free(mp3);
        free(f32);
        return;
    }
    int64_t nd;
    float *x = read_f32(f32, &nd);
    CBAudio a;
    CHECK(cb_decode(mp3, 1, 0, &a) >= 0, "decode %s", name);
    int64_t lead = (int64_t)extra_frames * 1152;
    CHECK(a.nb_frames == nd + lead, "%s: cyanbox %" PRId64 " samples, dj %" PRId64 " (want +%" PRId64 ")",
          what, a.nb_frames, nd, lead);
    fprintf(stderr, "%-34s cyanbox %8" PRId64 "  dj %8" PRId64 "  (%+" PRId64 ")\n", what, a.nb_frames,
            nd, a.nb_frames - nd);
    free(x);
    cb_audio_free(&a);
    free(mp3);
    free(f32);
}

static void exact_mp3(const char *dir, const char *name, int64_t lead, const char *what)
{
    char *mp3 = test_path(dir, name);
    char *f32 = test_path(dir, "dj.f32");
    dj("decode", mp3, f32, NULL);
    int64_t nd;
    float *x = read_f32(f32, &nd);
    CBAudio a;
    CHECK(cb_decode(mp3, 1, 0, &a) >= 0, "decode %s", name);
    if (x && a.nb_frames == nd + lead) {
        bool silent = true;
        for (int64_t i = 0; i < lead; i++)
            silent &= a.samples[i] == 0.0f;
        CHECK(silent && !memcmp(a.samples + lead, x, (size_t)nd * sizeof(float)),
              "%s: samples differ from dj's", what);
    }
    free(x);
    cb_audio_free(&a);
    free(mp3);
    free(f32);
}

static void test_mp3(const char *dir, const char *wav)
{
    char *mp3 = test_path(dir, "sig.mp3");
    CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "192k", mp3, NULL), "mp3");
    uint8_t *d;
    size_t n;
    CBMp3Info mi;
    CHECK(cb_file_read(mp3, &d, &n) >= 0 && cb_mp3_scan(mp3, &mi) >= 0, "scan");

    /* Info frame mode vs the audio's (joint stereo from libmp3lame) */
    size_t hdr = (size_t)mi.first_frame_offset;
    char *lavf = test_path(dir, "lavf-style.mp3"), *lame = test_path(dir, "lame-style.mp3");
    char *damaged = test_path(dir, "damaged.mp3");
    d[hdr + 3] = (uint8_t)((d[hdr + 3] & 0x3F) | 0x00);
    FILE *f = fopen(lavf, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
    d[hdr + 3] = (uint8_t)((d[hdr + 3] & 0x3F) | 0x40);
    f = fopen(lame, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
    /* damage frame 50 of the Lavf-style file: big_values > 288 */
    d[hdr + 3] = (uint8_t)(d[hdr + 3] & 0x3F);
    size_t at = hdr;
    for (int k = 0; k < 51; k++) {
        const uint8_t *h = d + at;
        static const int kbps[16] = {
            0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0,
        };
        at += 144 * kbps[h[2] >> 4] * 1000 / 44100 + ((h[2] >> 1) & 1);
    }
    d[at + 8] = 0xFF;
    d[at + 9] |= 0x80;
    f = fopen(damaged, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
    free(d);

    compare_mp3(dir, "lavf-style.mp3", 0, "stereo Info over joint (old Lavf)");
    compare_mp3(dir, "lame-style.mp3", 1, "joint Info over joint (LAME)");
    compare_mp3(dir, "damaged.mp3", 1, "old Lavf, one damaged frame");
    exact_mp3(dir, "lavf-style.mp3", 0, "lavf-style");
    exact_mp3(dir, "lame-style.mp3", 1152, "lame-style");

    /* Scanner fields */
    char *txt = test_path(dir, "dj.txt");
    if (!dj("mp3", lame, txt, NULL)) {
        uint8_t *t;
        size_t tn;
        cb_file_read(txt, &t, &tn);
        long long frames = 0, spf = 0, vbr = 0, kbps = 0;
        sscanf((char *)t, "%lld %lld %lld %lld", &frames, &spf, &vbr, &kbps);
        CBMp3Info li;
        cb_mp3_scan(lame, &li);
        CHECK(li.nb_frames == frames && li.frame_samples == spf && li.vbr == vbr &&
              li.bitrate_kbps == kbps, "mp3.scan: dj %lld %lld %lld %lld, cyanbox %" PRId64 " %d %d %d",
              frames, spf, vbr, kbps, li.nb_frames, li.frame_samples, li.vbr, li.bitrate_kbps);
        free(t);
    }

    free(txt);
    free(mp3);
    free(lavf);
    free(lame);
    free(damaged);
}

static void test_flac(const char *dir, const char *wav)
{
    /* Odd block sizes and a variable-blocksize stream too */
    const char *opts[][2] = { { "flac0.flac", "4608" }, { "flac1.flac", "1152" } };
    for (int v = 0; v < 2; v++) {
        char *fl = test_path(dir, opts[v][0]);
        CHECK(!ffmpeg("-i", wav, "-c:a", "flac", "-frame_size", opts[v][1], fl, NULL), "flac");
        char *txt = test_path(dir, "dj.txt");
        if (dj("flac", fl, txt, NULL)) {
            free(fl);
            free(txt);
            continue;
        }
        FILE *f = fopen(txt, "r");
        long long start, block, total, nb;
        CHECK(fscanf(f, "%lld %lld %lld %lld", &start, &block, &total, &nb) == 4, "dj output");
        CBFlacInfo fi;
        CHECK(cb_flac_scan(fl, &fi) >= 0, "cb_flac_scan");
        CHECK(fi.first_frame_offset == start && fi.max_block == block && fi.total_samples == total &&
              fi.nb_frames == nb, "flac: dj %lld %lld %lld %lld, cyanbox %" PRId64 " %d %" PRId64 " %d",
              start, block, total, nb, fi.first_frame_offset, fi.max_block, fi.total_samples,
              fi.nb_frames);
        bool same = fi.nb_frames == nb;
        for (int i = 0; i < nb; i++) {
            long long s, o, b;
            if (fscanf(f, "%lld %lld %lld", &s, &o, &b) != 3)
                same = false;
            else if (same)
                same = fi.frames[i].sample == s && fi.frames[i].offset == o && fi.frames[i].block == b;
        }
        CHECK(same, "%s: frame table differs from dj's", opts[v][0]);

        /* seek_entries: for i < 400, the last frame starting at or before
         * i * (total // 400) */
        int64_t step = fi.total_samples / 400;
        int j = 0;
        bool entries = true;
        for (int i = 0; i < 400; i++) {
            while (j + 1 < fi.nb_frames && fi.frames[j + 1].sample <= i * step)
                j++;
            long long s, o, b;
            if (fscanf(f, "%lld %lld %lld", &s, &o, &b) != 3 || fi.frames[j].sample != s ||
                fi.frames[j].offset != o || fi.frames[j].block != b)
                entries = false;
        }
        CHECK(entries, "%s: PVB2 entries differ from dj's", opts[v][0]);
        fclose(f);
        cb_flac_info_free(&fi);
        free(fl);
        free(txt);
    }
}

static void test_jpeg(const char *dir)
{
    /* A photo-like picture: smooth areas and edges */
    const int w = 300, h = 200;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgb + (y * w + x) * 3;
            double r = hypot(x - 150, y - 100);
            p[0] = (uint8_t)(128 + 100 * sin(x / 17.0));
            p[1] = (uint8_t)(r < 60 ? 220 : 40 + y / 2);
            p[2] = (uint8_t)((x / 25 + y / 25) % 2 ? 200 : 60);
        }
    }
    char *raw = test_path(dir, "pic.rgb"), *png = test_path(dir, "pic.png");
    FILE *f = fopen(raw, "wb");
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
    CHECK(!ffmpeg("-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "300x200", "-i", raw, png, NULL), "png");

    uint8_t *pngb;
    size_t pngn;
    uint8_t *rgba;
    int iw, ih;
    CHECK(cb_file_read(png, &pngb, &pngn) >= 0 && cb_image_decode(pngb, pngn, &rgba, &iw, &ih) >= 0,
          "decode png");
    const int sizes[] = { 80, 240 };
    for (int s = 0; s < 2; s++) {
        char sz[8];
        snprintf(sz, sizeof(sz), "%d", sizes[s]);
        char *out = test_path(dir, "dj.jpg");
        if (dj("jpeg", png, out, sz))
            continue;
        uint8_t *pj, *mine, *a, *b;
        size_t pjn, minen;
        int aw, ah, bw, bh;
        CHECK(cb_file_read(out, &pj, &pjn) >= 0, "read dj jpeg");
        CHECK(cb_image_jpeg_square(rgba, iw, ih, sizes[s], 90, &mine, &minen) >= 0, "cb jpeg");
        CHECK(cb_image_decode(pj, pjn, &a, &aw, &ah) >= 0 &&
              cb_image_decode(mine, minen, &b, &bw, &bh) >= 0 &&
              aw == bw && ah == bh && aw == sizes[s] && ah == sizes[s], "decode both");
        double sum = 0;
        int max = 0;
        for (int i = 0; i < aw * ah; i++) {
            for (int c = 0; c < 3; c++) {
                int d = abs(a[i * 4 + c] - b[i * 4 + c]);
                sum += d;
                max = CB_MAX(max, d);
            }
        }
        double mean = sum / (3.0 * aw * ah);
        fprintf(stderr, "%3d px JPEG vs Pillow: %zu vs %zu bytes, mean diff %.2f, max %d\n",
                sizes[s], minen, pjn, mean, max);
        /* libavcodec's MJPEG encoder fixes the DC step at 8 (libjpeg: 3) and
         * rounds AC with a 3/8 bias (artwork.c); Pillow's own JPEG of this
         * picture is 6.4 levels from the image it encoded at 80 px */
        CHECK(mean < 4.0, "%d px: mean difference to Pillow %.2f", sizes[s], mean);

        /* The quantisation tables: Pillow's DQT against ours, DC aside */
        const uint8_t *dq[2] = { NULL, NULL };
        const uint8_t *jp[2] = { pj, mine };
        size_t jn[2] = { pjn, minen };
        for (int k = 0; k < 2; k++) {
            size_t p = 2;
            while (p + 4 < jn[k] && jp[k][p] == 0xFF) {
                if (jp[k][p + 1] == 0xDB) {
                    dq[k] = jp[k] + p;
                    break;
                }
                p += 2 + (size_t)(jp[k][p + 2] << 8 | jp[k][p + 3]);
            }
        }
        CHECK(dq[0] && dq[1], "no DQT");
        if (dq[0] && dq[1]) {
            /* Pillow writes the two tables in one segment or two: compare
             * the first (luma) table's 63 AC entries */
            CHECK(!memcmp(dq[0] + 6, dq[1] + 6, 63), "%d px: luma quantisation differs from Pillow's",
                  sizes[s]);
        }
        free(a);
        free(b);
        free(pj);
        free(mine);
        free(out);
    }
    free(rgba);
    free(pngb);
    free(rgb);
    free(raw);
    free(png);
}

int main(void)
{
    char *dflt = NULL;
    python = getenv("CB_DJ_PYTHON");
    if (!python) {
        const char *home = getenv("HOME");
        dflt = cb_asprintf("%s/projects/dj/.venv/bin/python", home ? home : "");
        python = dflt;
    }
    if (!python || access(python, X_OK)) {
        fprintf(stderr, "dj's Python (%s) not found: skipped\n", python ? python : "-");
        free(dflt);
        return 77;
    }

    char *dir = test_dir("dj");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, 6, &n);
    char *wav = test_path(dir, "sig.wav");
    write_wav(wav, SIG_RATE, 2, pcm, n);

    if (have_encoder("libmp3lame"))
        test_mp3(dir, wav);
    test_flac(dir, wav);
    test_jpeg(dir);

    free(pcm);
    free(wav);
    free(dir);
    free(dflt);
    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
