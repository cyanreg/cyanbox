/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Cover art round trip: a picture written here (a smooth gradient between
 * two red bars) made into a PNG and embedded in a FLAC and an MP3 with the
 * ffmpeg CLI, extracted byte for byte, decoded, scaled, encoded as the
 * stick's square JPEG (centre crop, as dj's _jpeg) and decoded again. The
 * JPEG's quantisation tables are checked against libjpeg's at quality 90.
 * Then the covers found next to a file, by name and rank. */

#include "cyanbox/audio.h"

#include <math.h>

#include "util.h"

static int failures;

#define W 320
#define H 240
#define BAR 40

static void pixel(int x, int y, uint8_t *rgb)
{
    if (x < BAR || x >= W - BAR) {
        rgb[0] = 255;
        rgb[1] = rgb[2] = 0;
        return;
    }
    rgb[0] = (uint8_t)(40 + (x - BAR) * 160 / (W - 2 * BAR));
    rgb[1] = (uint8_t)(30 + y * 180 / H);
    rgb[2] = 140;
}

/* Mean and largest absolute difference of the RGB channels */
static void compare(const uint8_t *a, const uint8_t *b, int n, double *mean, int *max)
{
    double sum = 0;
    *max = 0;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 3; c++) {
            int d = abs(a[i * 4 + c] - b[i * 4 + c]);
            sum += d;
            *max = CB_MAX(*max, d);
        }
    }
    *mean = sum / (3.0 * n);
}

static bool same_rgb(const uint8_t *rgba, int w, int h, const uint8_t *rgb)
{
    if (!rgba || w != W || h != H)
        return false;
    for (int i = 0; i < W * H; i++)
        if (memcmp(rgba + i * 4, rgb + i * 3, 3))
            return false;
    return true;
}

typedef struct Place {
    const char *name;
    const char *from;
} Place;

/* A folder `name` under root holding a cover-less FLAC and the files
 * given; the cover found must be `want` (NULL: none), and when `rgb` is
 * given, decode to exactly it. */
static const char *bare_flac;

static void expect_cover(const char *root, const char *name, const Place *files, const char *want,
                         const uint8_t *rgb)
{
    char *dir = test_path(root, name);
    cb_mkdir_p(dir);
    char *track = test_path(dir, "01 track.flac");
    copy_file(bare_flac, track);
    for (const Place *p = files; p->name; p++) {
        char *f = test_path(dir, p->name);
        copy_file(p->from, f);
        free(f);
    }

    char *found = NULL;
    int ret = cb_artwork_find_file(track, &found);
    /* What the probe says is what the deck and the stick will show */
    CBProbe p;
    CHECK(cb_probe(track, &p) >= 0 && p.has_artwork == (want != NULL), "%s: probe has_artwork %d",
          name, p.has_artwork);
    cb_probe_free(&p);
    uint8_t *rgba = NULL, *raw = NULL;
    size_t rawn;
    char codec[16];
    int w, h;
    if (!want) {
        CHECK(ret == -ENOENT && !found, "%s: found %s", name, found ? found : "-");
        CHECK(cb_artwork_rgba(track, &rgba, &w, &h) == -ENOENT, "%s: rgba found", name);
        CHECK(cb_artwork_raw(track, &raw, &rawn, codec) == -ENOENT, "%s: raw found", name);
    } else {
        CHECK(ret >= 0 && found && !strcmp(cb_path_base(found), want), "%s: found %s, want %s", name,
              found ? found : "nothing", want);
        CHECK(cb_artwork_rgba(track, &rgba, &w, &h) >= 0 && w == W && h == H, "%s: rgba", name);
        if (rgb)
            CHECK(same_rgb(rgba, w, h, rgb), "%s: wrong picture", name);
        uint8_t *bytes;
        size_t n;
        char *path = test_path(dir, want);
        CHECK(cb_artwork_raw(track, &raw, &rawn, codec) >= 0 && cb_file_read(path, &bytes, &n) >= 0 &&
              n == rawn && !memcmp(raw, bytes, n), "%s: raw bytes are not %s's", name, want);
        free(bytes);
        free(path);
    }
    free(raw);
    free(rgba);
    free(found);
    free(track);
    free(dir);
}

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* libjpeg's jpeg_set_quality(90) tables (natural order) */
static void ijg_table(int chroma, int quality, int out[64])
{
    static const int luma[64] = {
        16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
        14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
    };
    static const int chr[64] = {
        17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
        24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    };
    int s = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    for (int i = 0; i < 64; i++)
        out[i] = CB_CLAMP(((chroma ? chr : luma)[i] * s + 50) / 100, 1, 255);
}

/* The DQT and SOF0 segments of a baseline 4:2:0 JPEG */
static void check_jpeg(const uint8_t *j, size_t n, int size, int quality)
{
    CHECK(n > 4 && j[0] == 0xFF && j[1] == 0xD8, "no SOI");
    int tables = 0;
    bool sof0 = false;
    size_t pos = 2;
    while (pos + 4 <= n && j[pos] == 0xFF) {
        int marker = j[pos + 1];
        size_t len = (size_t)(j[pos + 2] << 8 | j[pos + 3]);
        const uint8_t *seg = j + pos + 4;
        if (marker == 0xDB) {
            for (size_t at = 0; at + 65 <= len - 2; at += 65) {
                int id = seg[at] & 0xF;
                int want[64];
                ijg_table(id, quality, want);
                want[0] = 8;    /* the MJPEG encoder's DC step, see artwork.c */
                bool ok = (seg[at] >> 4) == 0;
                for (int i = 0; i < 64; i++)
                    ok &= seg[at + 1 + i] == want[zigzag[i]];
                CHECK(ok, "quantisation table %d is not libjpeg's at quality %d", id, quality);
                tables++;
            }
        } else if (marker == 0xC0) {
            sof0 = true;
            CHECK((seg[1] << 8 | seg[2]) == size && (seg[3] << 8 | seg[4]) == size && seg[5] == 3,
                  "SOF0 size");
            CHECK(seg[7] == 0x22 && seg[10] == 0x11 && seg[13] == 0x11, "not 4:2:0: %02x %02x %02x",
                  seg[7], seg[10], seg[13]);
        } else if (marker == 0xDA) {
            break;
        }
        pos += 2 + len;
    }
    CHECK(tables == 2 && sof0, "%d quantisation tables, baseline %d", tables, sof0);
}

int main(void)
{
    char *dir = test_dir("artwork");

    /* The picture, as raw RGB, then PNG and JPEG through the ffmpeg CLI */
    uint8_t *rgb = malloc(W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            pixel(x, y, rgb + (y * W + x) * 3);
    char *raw = test_path(dir, "cover.rgb");
    FILE *f = fopen(raw, "wb");
    fwrite(rgb, 1, W * H * 3, f);
    fclose(f);
    char *png = test_path(dir, "cover.png");
    char *jpg = test_path(dir, "cover.jpg");
    CHECK(!ffmpeg("-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "320x240", "-i", raw, png, NULL), "png");
    CHECK(!ffmpeg("-i", png, "-q:v", "2", jpg, NULL), "jpg");

    /* Audio to hang it on */
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, 2, &n);
    char *wav = test_path(dir, "short.wav");
    write_wav(wav, SIG_RATE, 2, pcm, n);
    char *bare = test_path(dir, "bare.flac");
    char *art = test_path(dir, "art.flac");
    CHECK(!ffmpeg("-i", wav, bare, NULL), "flac");
    bare_flac = bare;
    CHECK(!ffmpeg("-i", bare, "-i", png, "-map", "0:a", "-map", "1:v", "-c", "copy",
                  "-disposition:v", "attached_pic", art, NULL), "embed png");

    /* Embedded: the PNG's own bytes */
    uint8_t *pngb, *rawb;
    size_t pngn, rawn;
    char codec[16];
    CHECK(cb_file_read(png, &pngb, &pngn) >= 0, "read png");
    CHECK(cb_artwork_raw(art, &rawb, &rawn, codec) >= 0, "artwork raw");
    CHECK(!strcmp(codec, "png") && rawn == pngn && !memcmp(rawb, pngb, pngn),
          "embedded bytes differ (%s, %zu vs %zu)", codec, rawn, pngn);
    /* Nothing embedded: the folder's cover.jpg (before cover.png) */
    free(rawb);
    CHECK(cb_artwork_raw(bare, &rawb, &rawn, codec) >= 0 && !strcmp(codec, "mjpeg"),
          "bare file: folder cover %s", codec);
    free(rawb);

    CBProbe p;
    CHECK(cb_probe(art, &p) >= 0 && p.has_artwork, "probe: no artwork");
    cb_probe_free(&p);
    /* Nothing embedded, but the folder's cover is its cover all the same */
    CHECK(cb_probe(bare, &p) >= 0 && p.has_artwork, "probe: the folder's cover not seen");
    cb_probe_free(&p);

    /* Decoded exactly (PNG is lossless) */
    uint8_t *rgba;
    int w, h;
    CHECK(cb_artwork_rgba(art, &rgba, &w, &h) >= 0 && w == W && h == H, "rgba %dx%d", w, h);
    bool exact = w == W && h == H;
    for (int i = 0; exact && i < W * H; i++)
        exact = !memcmp(rgba + i * 4, rgb + i * 3, 3) && rgba[i * 4 + 3] == 255;
    CHECK(exact, "decoded PNG differs");

    /* Thumbnail: aspect kept, never enlarged */
    uint8_t *thumb;
    int tw, th;
    CHECK(cb_image_scale(rgba, W, H, 100, 100, &thumb, &tw, &th) >= 0 && tw == 100 && th == 75,
          "thumbnail %dx%d", tw, th);
    if (tw == 100 && th == 75) {
        uint8_t want[3];
        pixel(160, 120, want);
        const uint8_t *c = thumb + (37 * 100 + 50) * 4;
        CHECK(abs(c[0] - want[0]) <= 2 && abs(c[1] - want[1]) <= 2 && abs(c[2] - want[2]) <= 2,
              "thumbnail centre %d %d %d, want %d %d %d", c[0], c[1], c[2], want[0], want[1], want[2]);
        CHECK(thumb[(37 * 100 + 2) * 4] > 250 && thumb[(37 * 100 + 2) * 4 + 1] < 5, "thumbnail bar");
    }
    free(thumb);
    CHECK(cb_image_scale(rgba, W, H, 1000, 1000, &thumb, &tw, &th) >= 0 && tw == W && th == H &&
          !memcmp(thumb, rgba, W * H * 4), "no-op scale changed the image");
    free(thumb);

    /* The stick's JPEGs: the centre square (the red bars cropped away, not
     * padded), size x size, libjpeg's quality 90 tables */
    const int sizes[] = { 80, 240 };
    for (int s = 0; s < 2; s++) {
        int size = sizes[s];
        uint8_t *jpeg;
        size_t jn;
        CHECK(cb_image_jpeg_square(rgba, W, H, size, 90, &jpeg, &jn) >= 0, "jpeg %d", size);
        check_jpeg(jpeg, jn, size, 90);

        uint8_t *back;
        int bw, bh;
        CHECK(cb_image_decode(jpeg, jn, &back, &bw, &bh) >= 0 && bw == size && bh == size,
              "jpeg decode %dx%d", bw, bh);
        /* Reference: the centre square, area-scaled */
        uint8_t *square = malloc(H * H * 4), *ref;
        for (int y = 0; y < H; y++)
            memcpy(square + y * H * 4, rgba + (y * W + (W - H) / 2) * 4, H * 4);
        int rw, rh;
        CHECK(cb_image_scale(square, H, H, size, size, &ref, &rw, &rh) >= 0, "reference");
        double mean;
        int max;
        compare(back, ref, size * size, &mean, &max);
        fprintf(stderr, "%d px jpeg: %zu bytes, against the area-scaled square: mean %.2f max %d\n",
                size, jn, mean, max);
        CHECK(mean < 1.5 && max < 16, "%d px jpeg differs from the centre square: mean %.2f max %d",
              size, mean, max);
        int red = 0;
        for (int y = 0; y < size; y++)
            red += back[(y * size) * 4] > 230 && back[(y * size) * 4 + 1] < 40;
        CHECK(!red, "%d px jpeg: %d red pixels on the left edge", size, red);
        free(square);
        free(ref);
        free(back);
        free(jpeg);
    }
    free(rgba);

    /* An MP3 with an ID3 APIC JPEG */
    if (have_encoder("libmp3lame")) {
        char *mp3 = test_path(dir, "art.mp3");
        CHECK(!ffmpeg("-i", wav, "-i", jpg, "-map", "0:a", "-map", "1:v", "-c:a", "libmp3lame",
                      "-c:v", "copy", "-disposition:v", "attached_pic", mp3, NULL), "mp3 art");
        CHECK(cb_artwork_raw(mp3, &rawb, &rawn, codec) >= 0 && !strcmp(codec, "mjpeg"),
              "mp3 cover codec %s", codec);
        free(rawb);
        CHECK(cb_artwork_rgba(mp3, &rgba, &w, &h) >= 0 && w == W && h == H, "mp3 cover %dx%d", w, h);
        free(rgba);
        free(mp3);
    }

    /* Next to the file (cyanbox/audio.h's rule), each case in its own folder
     * under one that holds no image, so the parent lookup finds nothing */
    char *root = test_path(dir, "lookup");
    const char *rm[] = { "rm", "-rf", root, NULL };
    run(rm);
    cb_mkdir_p(root);
    char *webp = NULL;
    if (have_encoder("libwebp")) {
        webp = test_path(dir, "cover.webp");
        CHECK(!ffmpeg("-i", png, "-c:v", "libwebp", "-lossless", "1", webp, NULL), "webp");
    }

    /* front.png beside back.png: front, decoded exactly, raw bytes too */
    expect_cover(root, "front-back", (const Place[]) {
        { "front.png", png }, { "back.png", jpg }, { NULL } }, "front.png", rgb);
    /* front before cover before folder; any case */
    expect_cover(root, "order", (const Place[]) {
        { "Folder.JPG", jpg }, { "cover.png", png }, { "FRONT.jpeg", jpg }, { NULL } },
        "FRONT.jpeg", NULL);
    expect_cover(root, "order2", (const Place[]) {
        { "folder.jpg", jpg }, { "Cover.Png", png }, { "booklet.png", png }, { NULL } },
        "Cover.Png", rgb);
    expect_cover(root, "album-art", (const Place[]) {
        { "art.png", png }, { "ALBUM.jpg", jpg }, { NULL } }, "ALBUM.jpg", NULL);
    /* numbered firsts after the plain names, never a second */
    expect_cover(root, "numbered", (const Place[]) {
        { "cover_02.png", png }, { "Cover_01.png", png }, { "back.jpg", jpg }, { NULL } },
        "Cover_01.png", rgb);
    expect_cover(root, "numbered2", (const Place[]) {
        { "front1.png", png }, { "folder.jpg", jpg }, { NULL } }, "folder.jpg", NULL);
    /* a lone image of any name; never a lone back/obi/scan */
    expect_cover(root, "lone", (const Place[]) {
        { "IMG_2034.PNG", png }, { "notes.txt", jpg }, { "._front.jpg", jpg }, { NULL } },
        "IMG_2034.PNG", rgb);
    expect_cover(root, "only-back", (const Place[]) { { "back.jpg", jpg }, { NULL } }, NULL, NULL);
    expect_cover(root, "only-scan", (const Place[]) { { "Scan 03.png", png }, { NULL } }, NULL, NULL);
    expect_cover(root, "only-cd", (const Place[]) { { "cd1.jpg", jpg }, { NULL } }, NULL, NULL);
    expect_cover(root, "two-others", (const Place[]) {
        { "a.png", png }, { "b.png", png }, { NULL } }, NULL, NULL);
    expect_cover(root, "empty", (const Place[]) { { NULL } }, NULL, NULL);
    /* an unreadable front: the next candidate */
    expect_cover(root, "broken-front", (const Place[]) {
        { "front.png", wav }, { "cover.png", png }, { NULL } }, "cover.png", rgb);
    if (webp)
        expect_cover(root, "webp", (const Place[]) { { "front.webp", webp }, { NULL } },
                     "front.webp", rgb);

    /* A disc subfolder takes its album's cover */
    char *album = test_path(root, "album");
    char *disc = test_path(album, "CD1");
    cb_mkdir_p(disc);
    char *afront = test_path(album, "front.png");
    char *dtrack = test_path(disc, "01 track.flac");
    copy_file(png, afront);
    copy_file(bare, dtrack);
    char *found = NULL;
    CHECK(cb_artwork_find_file(dtrack, &found) >= 0 && found && cb_str_ends(found, "album/front.png"),
          "disc subfolder: %s", found ? found : "nothing");
    free(found);
    CHECK(cb_artwork_rgba(dtrack, &rgba, &w, &h) >= 0 && w == W, "disc subfolder rgba");
    free(rgba);
    /* ... unless the disc folder has its own */
    char *dcover = test_path(disc, "cover.jpg");
    copy_file(jpg, dcover);
    CHECK(cb_artwork_find_file(dtrack, &found) >= 0 && found && cb_str_ends(found, "CD1/cover.jpg"),
          "disc's own cover: %s", found ? found : "nothing");
    free(found);

    /* "Disc 2" is a disc folder too; an album folder under an artist's is
     * not, so the artist's picture is not its cover */
    char *disc2 = test_path(album, "[Disc 2] bonus");
    char *d2track = test_path(disc2, "01 bonus.flac");
    cb_mkdir_p(disc2);
    copy_file(bare, d2track);
    CHECK(cb_artwork_find_file(d2track, &found) >= 0 && found && cb_str_ends(found, "album/front.png"),
          "[Disc 2]: %s", found ? found : "nothing");
    free(found);
    char *artist = test_path(root, "artist");
    char *aalbum = test_path(artist, "Some Album");
    char *atrack = test_path(aalbum, "01 track.flac");
    char *apic = test_path(artist, "cover.jpg");
    cb_mkdir_p(aalbum);
    copy_file(bare, atrack);
    copy_file(jpg, apic);
    found = NULL;
    CHECK(cb_artwork_find_file(atrack, &found) == -ENOENT, "artist picture used: %s",
          found ? found : "-");
    free(found);
    free(disc2);
    free(d2track);
    free(artist);
    free(aalbum);
    free(atrack);
    free(apic);

    /* Embedded wins over the folder */
    char *emb_dir = test_path(root, "embedded");
    cb_mkdir_p(emb_dir);
    char *emb = test_path(emb_dir, "02 art.flac");
    char *emb_front = test_path(emb_dir, "front.jpg");
    copy_file(art, emb);
    copy_file(jpg, emb_front);
    CHECK(cb_artwork_raw(emb, &rawb, &rawn, codec) >= 0 && !strcmp(codec, "png") && rawn == pngn,
          "embedded raw not chosen (%s)", codec);
    free(rawb);
    CHECK(cb_artwork_rgba(emb, &rgba, &w, &h) >= 0 && same_rgb(rgba, w, h, rgb),
          "folder cover chosen over the embedded one");
    free(rgba);

    free(webp);
    free(album);
    free(disc);
    free(afront);
    free(dtrack);
    free(dcover);
    free(emb_dir);
    free(emb);
    free(emb_front);
    free(root);

    /* Not images */
    CHECK(cb_image_decode((const uint8_t *)"hello world, not an image", 25, &rgba, &w, &h) < 0,
          "text decoded as an image");
    static const uint8_t bad_png[16] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13 };
    CHECK(cb_image_decode(bad_png, sizeof(bad_png), &rgba, &w, &h) < 0, "broken png decoded");

    free(pngb);
    free(pcm);
    free(rgb);
    free(raw);
    free(png);
    free(jpg);
    free(wav);
    free(bare);
    free(art);
    free(dir);
    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
