/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cb_mp3_scan on MP3 streams built byte by byte: the frame-map rule
 * (rbxport's cases: a LAME Info frame that matches its audio counts as a
 * frame of silence, an ffmpeg one that does not is skipped, a change in the
 * first four frames restarts the map, a later change does not), ID3v2
 * skipping, VBR detection, junk between frames, MPEG-2/2.5, free format. */

#include "cyanbox/audio.h"

#include <inttypes.h>

#include "util.h"

static int failures;

enum { STEREO = 0, JOINT = 1, MONO = 3 };

typedef struct Buf {
    uint8_t *d;
    size_t n;
} Buf;

static void put(Buf *b, const void *data, size_t n)
{
    b->d = realloc(b->d, b->n + n);
    if (!b->d)
        exit(1);
    memcpy(b->d + b->n, data, n);
    b->n += n;
}

/* A Layer III frame. b1 is 0xFB (MPEG-1), 0xF3 (MPEG-2) or 0xE3 (2.5);
 * br the bitrate index; sr the rate index. The body is zero after `body`. */
static void frame(Buf *b, uint8_t b1, int br, int sr, int mode, const void *body, size_t len)
{
    static const int kbps[2][16] = {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
    };
    static const int rates[4][3] = {
        [0] = { 11025, 12000, 8000 }, [2] = { 22050, 24000, 16000 }, [3] = { 44100, 48000, 32000 },
    };
    int version = (b1 >> 3) & 3;
    bool mpeg1 = version == 3;
    int bytes = (mpeg1 ? 144 : 72) * kbps[!mpeg1][br] * 1000 / rates[version][sr];
    uint8_t *f = calloc(1, (size_t)bytes);
    f[0] = 0xFF;
    f[1] = b1;
    f[2] = (uint8_t)(br << 4 | sr << 2);
    f[3] = (uint8_t)(mode << 6);
    if (body)
        memcpy(f + 4, body, len);
    put(b, f, (size_t)bytes);
    free(f);
}

/* A 320 kbit/s 44.1 kHz MPEG-1 frame of 1044 bytes, as in rbxport's tests */
static void audio(Buf *b, int mode)
{
    static const uint8_t side[4] = { 0x00, 0x01, 0x23, 0x45 };
    frame(b, 0xFB, 14, 0, mode, side, sizeof(side));
}

static void tag_frame(Buf *b, int mode, const char *tag)
{
    uint8_t body[48] = { 0 };
    size_t side = mode == MONO ? 17 : 32;
    memcpy(body + side, tag, 4);
    body[side + 7] = 0x0F;
    frame(b, 0xFB, 14, 0, mode, body, sizeof(body));
}

static CBMp3Info scan(const char *dir, const char *name, Buf *b)
{
    char *path = test_path(dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b->d, 1, b->n, f) != b->n)
        exit(1);
    fclose(f);
    CBMp3Info info;
    int ret = cb_mp3_scan(path, &info);
    CHECK(ret >= 0, "%s: cb_mp3_scan %d", name, ret);
    free(path);
    free(b->d);
    *b = (Buf) { 0 };
    return info;
}

int main(void)
{
    char *dir = test_dir("mp3");
    Buf b = { 0 };
    CBMp3Info i;

    /* LAME 3.99, joint stereo throughout: the Info frame is a frame of
     * silence on the timeline */
    tag_frame(&b, JOINT, "Info");
    for (int k = 0; k < 5; k++)
        audio(&b, JOINT);
    i = scan(dir, "lame.mp3", &b);
    CHECK(i.valid && i.header_frame && i.header_counts && !i.skip_frames, "lame: %d %d %d",
          i.header_frame, i.header_counts, i.skip_frames);
    CHECK(i.nb_frames == 5, "lame: %" PRId64 " frames", i.nb_frames);
    CHECK(!i.vbr && i.bitrate_kbps == 320, "lame: vbr %d, %d kbps", i.vbr, i.bitrate_kbps);
    CHECK(i.version == 1 && i.sample_rate == 44100 && i.channels == 2 && i.frame_samples == 1152,
          "lame: version %d rate %d ch %d spf %d", i.version, i.sample_rate, i.channels,
          i.frame_samples);
    CHECK(i.first_frame_offset == 0 && i.audio_bytes == 6 * 1044, "lame: offset %" PRId64
          " bytes %" PRId64, i.first_frame_offset, i.audio_bytes);

    /* Lavc: a stereo Info frame over joint-stereo audio restarts the map at
     * the first audio frame */
    tag_frame(&b, STEREO, "Info");
    for (int k = 0; k < 5; k++)
        audio(&b, JOINT);
    i = scan(dir, "lavc.mp3", &b);
    CHECK(i.header_frame && !i.header_counts && !i.skip_frames, "lavc: %d %d %d",
          i.header_frame, i.header_counts, i.skip_frames);
    CHECK(i.nb_frames == 5, "lavc: %" PRId64 " frames", i.nb_frames);

    /* No tag frame: the timeline starts at the first frame */
    for (int k = 0; k < 5; k++)
        audio(&b, JOINT);
    i = scan(dir, "plain.mp3", &b);
    CHECK(!i.header_frame && !i.header_counts && !i.skip_frames && i.nb_frames == 5,
          "plain: %d %d %d %" PRId64, i.header_frame, i.header_counts, i.skip_frames, i.nb_frames);

    /* Audio rekordbox restarts after: off the timeline */
    audio(&b, STEREO);
    for (int k = 0; k < 5; k++)
        audio(&b, JOINT);
    i = scan(dir, "restart.mp3", &b);
    CHECK(!i.header_frame && i.skip_frames == 1 && i.nb_frames == 6, "restart: %d %d %" PRId64,
          i.header_frame, i.skip_frames, i.nb_frames);

    /* A change once four frames are held is kept */
    for (int k = 0; k < 4; k++)
        audio(&b, JOINT);
    audio(&b, STEREO);
    audio(&b, STEREO);
    i = scan(dir, "settled.mp3", &b);
    CHECK(!i.skip_frames && !i.header_counts && i.nb_frames == 6, "settled: %d %" PRId64,
          i.skip_frames, i.nb_frames);

    /* A change at the third frame: the map restarts there, two audio frames
     * after a counted-looking Info frame are dropped */
    tag_frame(&b, JOINT, "Info");
    audio(&b, JOINT);
    audio(&b, JOINT);
    for (int k = 0; k < 4; k++)
        audio(&b, STEREO);
    i = scan(dir, "late_restart.mp3", &b);
    CHECK(i.header_frame && !i.header_counts && i.skip_frames == 2 && i.nb_frames == 6,
          "late restart: %d %d %d %" PRId64, i.header_frame, i.header_counts, i.skip_frames,
          i.nb_frames);

    /* ID3v2 (128 bytes) in front of a LAME file */
    static const uint8_t id3[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 1, 0 };
    uint8_t pad[128] = { 0 };
    put(&b, id3, sizeof(id3));
    put(&b, pad, sizeof(pad));
    tag_frame(&b, JOINT, "Info");
    for (int k = 0; k < 4; k++)
        audio(&b, JOINT);
    i = scan(dir, "id3.mp3", &b);
    CHECK(i.header_counts && i.first_frame_offset == 138 && i.nb_frames == 4,
          "id3: %d %" PRId64 " %" PRId64, i.header_counts, i.first_frame_offset, i.nb_frames);

    /* Mono LAME: Info after 17 bytes of side information, counted */
    tag_frame(&b, MONO, "Info");
    for (int k = 0; k < 4; k++)
        audio(&b, MONO);
    i = scan(dir, "mono.mp3", &b);
    CHECK(i.header_frame && i.header_counts && i.channels == 1, "mono: %d %d %d",
          i.header_frame, i.header_counts, i.channels);

    /* Xing means VBR; the bitrate is the frames' mean, rounded half to even
     * like dj's round(): (320 + 256 + 256 + 320) / 4 = 288 */
    tag_frame(&b, JOINT, "Xing");
    audio(&b, JOINT);
    frame(&b, 0xFB, 13, 0, JOINT, NULL, 0);
    frame(&b, 0xFB, 13, 0, JOINT, NULL, 0);
    audio(&b, JOINT);
    i = scan(dir, "xing.mp3", &b);
    CHECK(i.vbr && i.header_frame && i.bitrate_kbps == 288 && i.nb_frames == 4,
          "xing: vbr %d %d kbps %" PRId64, i.vbr, i.bitrate_kbps, i.nb_frames);
    /* Varying bitrates without a tag frame are VBR too */
    audio(&b, JOINT);
    frame(&b, 0xFB, 9, 0, JOINT, NULL, 0);
    audio(&b, JOINT);
    i = scan(dir, "vbr.mp3", &b);
    CHECK(i.vbr && !i.header_frame && i.bitrate_kbps == 256, "vbr: %d %d %d", i.vbr,
          i.header_frame, i.bitrate_kbps);

    /* The mean is rounded half to even: 15 x 32 + 40 = 520 over 16 frames
     * is 32.5, which Python's round() makes 32; 368 / 3 = 122.7 makes 123 */
    for (int k = 0; k < 15; k++)
        frame(&b, 0xFB, 1, 0, JOINT, NULL, 0);
    frame(&b, 0xFB, 2, 0, JOINT, NULL, 0);
    i = scan(dir, "tie.mp3", &b);
    CHECK(i.bitrate_kbps == 32 && i.nb_frames == 16, "tie: %d", i.bitrate_kbps);
    frame(&b, 0xFB, 9, 0, JOINT, NULL, 0);
    frame(&b, 0xFB, 9, 0, JOINT, NULL, 0);
    frame(&b, 0xFB, 8, 0, JOINT, NULL, 0);
    i = scan(dir, "mean.mp3", &b);
    CHECK(i.bitrate_kbps == 123, "mean: %d", i.bitrate_kbps);

    /* VBRI 32 bytes after the header */
    uint8_t vbri[40] = { 0 };
    memcpy(vbri + 32, "VBRI", 4);
    frame(&b, 0xFB, 14, 0, JOINT, vbri, sizeof(vbri));
    for (int k = 0; k < 3; k++)
        audio(&b, JOINT);
    i = scan(dir, "vbri.mp3", &b);
    CHECK(i.header_frame && i.vbr && i.header_counts && i.nb_frames == 3, "vbri: %d %d %d %" PRId64,
          i.header_frame, i.vbr, i.header_counts, i.nb_frames);

    /* Junk between frames is skipped, an ID3v1 tag at the end ends it, and a
     * truncated last frame still counts */
    for (int k = 0; k < 3; k++)
        audio(&b, JOINT);
    uint8_t junk[100];
    memset(junk, 0x55, sizeof(junk));
    put(&b, junk, sizeof(junk));
    for (int k = 0; k < 3; k++)
        audio(&b, JOINT);
    uint8_t tag[128] = { 'T', 'A', 'G' };
    put(&b, tag, sizeof(tag));
    i = scan(dir, "junk.mp3", &b);
    CHECK(i.nb_frames == 6, "junk: %" PRId64 " frames", i.nb_frames);
    CHECK(i.audio_bytes == 6 * 1044 + 100, "junk: %" PRId64 " bytes", i.audio_bytes);

    for (int k = 0; k < 3; k++)
        audio(&b, JOINT);
    b.n -= 500;
    i = scan(dir, "truncated.mp3", &b);
    CHECK(i.nb_frames == 3 && i.audio_bytes == 3 * 1044 - 500, "truncated: %" PRId64 " %" PRId64,
          i.nb_frames, i.audio_bytes);

    /* MPEG-2 (576 samples, 24 kHz) and MPEG-2.5 (8 kHz) */
    for (int k = 0; k < 4; k++)
        frame(&b, 0xF3, 8, 1, JOINT, NULL, 0);
    i = scan(dir, "mpeg2.mp3", &b);
    CHECK(i.version == 2 && i.frame_samples == 576 && i.sample_rate == 24000 && i.nb_frames == 4,
          "mpeg2: %d %d %d %" PRId64, i.version, i.frame_samples, i.sample_rate, i.nb_frames);
    for (int k = 0; k < 4; k++)
        frame(&b, 0xE3, 8, 2, MONO, NULL, 0);
    i = scan(dir, "mpeg25.mp3", &b);
    CHECK(i.version == 25 && i.frame_samples == 576 && i.sample_rate == 8000 && i.channels == 1,
          "mpeg2.5: %d %d %d %d", i.version, i.frame_samples, i.sample_rate, i.channels);

    /* Free format has no frame length: not an MP3 we can walk */
    uint8_t ff[4] = { 0xFF, 0xFB, 0x00, 0x40 };
    for (int k = 0; k < 64; k++)
        put(&b, ff, sizeof(ff));
    char *path = test_path(dir, "free.mp3");
    FILE *f = fopen(path, "wb");
    fwrite(b.d, 1, b.n, f);
    fclose(f);
    free(b.d);
    CHECK(cb_mp3_scan(path, &i) < 0 && !i.valid, "free format accepted");
    free(path);

    /* A path that does not exist */
    CHECK(cb_mp3_scan("/nonexistent/x.mp3", &i) == -ENOENT, "missing file");

    free(dir);
    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
