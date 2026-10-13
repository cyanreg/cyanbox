/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The packet store (rxbox/audio.h) against rx_decode, on the known signal
 * as WAV, FLAC, AIFF, MP3 (an Info frame rekordbox counts, so a frame of
 * silence first, and the same file with one it does not), Opus, Vorbis and
 * AAC (MP4 with its edit list, and raw ADTS), all made here.
 *
 * For each: the length (the container's at open, the count once indexed,
 * both rx_decode's), the whole track decoded in chunks in order, sample for
 * sample, the first and the last second and a stretch in the middle each
 * decoded straight after a jump, and 64 more jumps: sample for sample
 * where the codec allows a cold start to be (lossless codecs, MP3, Vorbis,
 * AAC-LC, with the pre-roll packets.c gives them), within 1e-4 for Opus
 * (src/audio/packets.c has the measurements). Then the edges: frames before
 * 0 and past the end are silence, a store decodes before it is indexed
 * (indexing as far as it needs), and Matroska is refused. */

#include "rxbox/audio.h"

#include <inttypes.h>
#include <math.h>

#include "util.h"

static int failures;

#define CHUNK 4096

/* The largest difference between n interleaved frames */
static double diff(const float *a, const float *b, int64_t n, int ch)
{
    double m = 0.0;
    for (int64_t i = 0; i < n * ch; i++)
        m = fmax(m, fabs((double)a[i] - b[i]));
    return m;
}

/* The reference's frames [at, at + n), silence outside it. */
static void ref_frames(const RXAudio *ref, int64_t at, int64_t n, float *out)
{
    int ch = ref->channels;
    for (int64_t i = 0; i < n; i++) {
        int64_t f = at + i;
        for (int c = 0; c < ch; c++)
            out[i * ch + c] = f >= 0 && f < ref->nb_frames ? ref->samples[f * ch + c] : 0.0f;
    }
}

/* Decodes [at, at + n) after a jump (a fresh call that does not follow
 * the last) and compares; returns the difference. */
static double jump(RXPacketDecoder *d, const RXAudio *ref, int64_t at, int64_t n, float *x,
                   float *want)
{
    int64_t got = rx_packets_decode(d, at, n, x, 0);
    int64_t inside = RX_CLAMP(ref->nb_frames - at, (int64_t)0, n);
    CHECK(got == inside, "decode at %" PRId64 ": %" PRId64 " frames, want %" PRId64, at, got,
          inside);
    ref_frames(ref, at, n, want);
    return diff(x, want, n, ref->channels);
}

static void check_file(const char *what, const char *path, double tolerance)
{
    RXAudio ref;
    int ret = rx_decode(path, 0, 0, &ref);
    CHECK(ret >= 0, "%s: rx_decode %d", what, ret);
    if (ret < 0)
        return;

    int64_t t0 = rx_time_us();
    RXPackets *pk = NULL;
    ret = rx_packets_open(path, &pk);
    int64_t t1 = rx_time_us();
    CHECK(ret >= 0, "%s: rx_packets_open %d", what, ret);
    if (ret < 0) {
        rx_audio_free(&ref);
        return;
    }
    CHECK(rx_packets_rate(pk) == ref.rate && rx_packets_channels(pk) == ref.channels,
          "%s: %d Hz %d ch, rx_decode %d Hz %d ch", what, rx_packets_rate(pk),
          rx_packets_channels(pk), ref.rate, ref.channels);
    bool exact;
    int64_t at_open = rx_packets_length(pk, &exact);
    /* What the container states: Ogg Vorbis's leading packet makes it 128
     * frames long until the index has counted */
    CHECK(llabs(at_open - ref.nb_frames) <= 128, "%s: %" PRId64 " frames at open, rx_decode %"
          PRId64, what, at_open, ref.nb_frames);

    int ch = ref.channels, rate = ref.rate;
    int64_t n = ref.nb_frames;
    float *all = malloc(sizeof(float) * (size_t)(n + 2 * CHUNK) * ch);
    float *x = malloc(sizeof(float) * (size_t)48000 * ch);
    float *want = malloc(sizeof(float) * (size_t)48000 * ch);
    RXPacketDecoder *d = NULL;
    CHECK(all && x && want && rx_packets_decoder(pk, &d) >= 0, "%s: decoder", what);
    if (!d)
        goto end;

    /* In order, in chunks, to the end and a chunk past it */
    int64_t total = 0;
    for (int64_t at = 0; at < n + CHUNK; at += CHUNK) {
        int64_t got = rx_packets_decode(d, at, CHUNK, all + at * ch, 0);
        CHECK(got >= 0, "%s: decode at %" PRId64 ": %" PRId64, what, at, got);
        total += RX_MAX(got, (int64_t)0);
    }
    int64_t length = rx_packets_length(pk, &exact);
    CHECK(exact && length == n && total == n, "%s: %" PRId64 " frames (exact %d), %" PRId64
          " decoded, rx_decode %" PRId64, what, length, exact, total, n);
    double seq = diff(all, ref.samples, n, ch);
    CHECK(seq == 0.0, "%s: in order, %g off", what, seq);
    bool silent = true;
    for (int64_t i = n * ch; i < (n + CHUNK) * ch; i++)
        silent &= all[i] == 0.0f;
    CHECK(silent, "%s: sound past the end", what);

    /* After jumps: the first second, the last, the middle, and 64 more */
    double worst = 0.0;
    worst = fmax(worst, jump(d, &ref, n / 2, rate / 4, x, want));
    worst = fmax(worst, jump(d, &ref, 0, rate, x, want));
    worst = fmax(worst, jump(d, &ref, n - rate, rate, x, want));
    worst = fmax(worst, jump(d, &ref, n / 2 - rate / 8, rate / 4, x, want));
    unsigned seed = 7;
    for (int i = 0; i < 64; i++) {
        seed = seed * 1103515245u + 12345u;
        int64_t at = (int64_t)((seed >> 4) % (uint64_t)n);
        worst = fmax(worst, jump(d, &ref, at, CHUNK, x, want));
    }
    CHECK(worst <= tolerance, "%s: %g off after a jump (allowed %g)", what, worst, tolerance);

    /* Before 0: silence, then the track */
    worst = jump(d, &ref, -1000, 4000, x, want);
    CHECK(worst <= tolerance, "%s: %g off across 0", what, worst);

    printf("%s: open %.2f ms, %" PRId64 " frames, %.0f kB of packets, %g off after jumps\n",
           what, (t1 - t0) / 1e3, n, rx_packets_memory(pk) / 1024.0, worst);

end:
    rx_packets_decoder_free(d);
    rx_packets_free(pk);

    /* A store not indexed yet decodes the end of the track first, indexing
     * as far as it needs */
    if (rx_packets_open(path, &pk) >= 0) {
        CHECK(rx_packets_decoder(pk, &d) >= 0, "%s: decoder", what);
        if (d) {
            double off = jump(d, &ref, n - rate / 2, rate / 2, x, want);
            CHECK(off <= tolerance, "%s: %g off at the end before indexing", what, off);
            CHECK(rx_packets_length(pk, &exact) == n && exact, "%s: indexed to the end", what);
        }
        rx_packets_decoder_free(d);
        rx_packets_free(pk);
    }
    free(all);
    free(x);
    free(want);
    rx_audio_free(&ref);
}

int main(void)
{
    char *dir = test_dir("packets");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, SIG_SECONDS, &n);
    char *wav = test_path(dir, "sig.wav");
    CHECK(write_wav(wav, SIG_RATE, 2, pcm, n) >= 0, "write wav");
    free(pcm);

    check_file("wav", wav, 0.0);

    char *flac = test_path(dir, "sig.flac");
    CHECK(!ffmpeg("-i", wav, "-c:a", "flac", flac, NULL), "flac encode");
    check_file("flac", flac, 0.0);

    char *aiff = test_path(dir, "sig.aiff");
    CHECK(!ffmpeg("-i", wav, "-c:a", "pcm_s24be", aiff, NULL), "aiff encode");
    check_file("aiff", aiff, 0.0);

    if (have_encoder("libmp3lame")) {
        /* ffmpeg's Info frame has the audio's channel mode, so rekordbox
         * counts it: a frame of silence first. Flipped, it does not count. */
        char *mp3 = test_path(dir, "sig.mp3");
        char *mp3b = test_path(dir, "sig-restart.mp3");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "128k", mp3, NULL), "mp3 encode");
        RXMp3Info mi;
        uint8_t *d = NULL;
        size_t size = 0;
        if (rx_mp3_scan(mp3, &mi) >= 0 && mi.header_frame && rx_file_read(mp3, &d, &size) >= 0) {
            size_t hdr = (size_t)mi.first_frame_offset;
            d[hdr + 3] ^= 0x40;
            CHECK(rx_file_write_atomic(mp3b, d, size) >= 0, "write %s", mp3b);
            RXMp3Info mb;
            CHECK(rx_mp3_scan(mp3b, &mb) >= 0 && mb.header_counts != mi.header_counts,
                  "the flipped Info frame counts the same (%d)", mi.header_counts);
            check_file(mi.header_counts ? "mp3 (header frame counts)" : "mp3", mp3, 0.0);
            check_file(mi.header_counts ? "mp3" : "mp3 (header frame counts)", mp3b, 0.0);
        } else {
            CHECK(0, "%s: no Info frame", mp3);
        }
        free(d);
        free(mp3);
        free(mp3b);
    }

    if (have_encoder("libopus")) {
        char *opus = test_path(dir, "sig.opus");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libopus", "-b:a", "160k", opus, NULL), "opus encode");
        check_file("opus", opus, 1e-4);

        /* Matroska's times are milliseconds: refused */
        char *mka = test_path(dir, "sig.mka");
        CHECK(!ffmpeg("-i", opus, "-c", "copy", mka, NULL), "mka remux");
        RXPackets *pk = NULL;
        CHECK(rx_packets_open(mka, &pk) == RX_ERROR(ENOTSUP), "Matroska accepted");
        rx_packets_free(pk);
        free(mka);
        free(opus);
    }

    if (have_encoder("libvorbis")) {
        char *ogg = test_path(dir, "sig.ogg");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libvorbis", "-q:a", "5", ogg, NULL), "vorbis encode");
        check_file("vorbis", ogg, 0.0);
        free(ogg);
    }

    char *m4a = test_path(dir, "sig.m4a");
    CHECK(!ffmpeg("-i", wav, "-c:a", "aac", "-b:a", "192k", m4a, NULL), "aac encode");
    check_file("aac (mp4)", m4a, 0.0);
    char *adts = test_path(dir, "sig.aac");
    CHECK(!ffmpeg("-i", wav, "-c:a", "aac", "-b:a", "192k", adts, NULL), "adts encode");
    check_file("aac (adts)", adts, 0.0);

    RXPackets *pk = NULL;
    CHECK(rx_packets_open("/nonexistent/rxbox/x.flac", &pk) < 0 && !pk, "a missing file opened");

    free(m4a);
    free(adts);
    free(aiff);
    free(flac);
    free(wav);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("packets: all tests passed\n");
    return 0;
}
