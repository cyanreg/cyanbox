/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rx_transcode_flac: a 24-bit 48 kHz FLAC with tags and a cover becomes a
 * 16-bit 44.1 kHz FLAC of the same length with the same tags and cover; a
 * 16-bit WAV goes through sample for sample (no dither); an MP3 keeps its
 * players' timeline (the silent header frame and encoder delay become
 * samples of the FLAC). */

#include "rxbox/audio.h"

#include <inttypes.h>
#include <math.h>

#include "util.h"

static int failures;

int main(void)
{
    char *dir = test_dir("transcode");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, 4, &n);
    char *wav = test_path(dir, "sig.wav");
    write_wav(wav, SIG_RATE, 2, pcm, n);

    char *raw = test_path(dir, "cover.rgb");
    uint8_t *rgb = calloc(64 * 48, 3);
    for (int i = 0; i < 64 * 48 * 3; i++)
        rgb[i] = (uint8_t)(i * 7);
    FILE *f = fopen(raw, "wb");
    fwrite(rgb, 3, 64 * 48, f);
    fclose(f);
    char *png = test_path(dir, "cover.png");
    CHECK(!ffmpeg("-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "64x48", "-i", raw, png, NULL), "png");

    /* 24-bit 48 kHz FLAC with tags and a cover */
    char *hi = test_path(dir, "hires.flac");
    CHECK(!ffmpeg("-i", wav, "-i", png, "-map", "0:a", "-map", "1:v", "-c:a", "flac",
                  "-sample_fmt", "s32", "-bits_per_raw_sample", "24", "-ar", "48000", "-c:v", "copy",
                  "-disposition:v", "attached_pic", "-metadata", "title=Hi Res",
                  "-metadata", "artist=Somebody", "-metadata", "album=Album", hi, NULL), "hires");
    RXProbe ph;
    CHECK(rx_probe(hi, &ph) >= 0 && ph.bit_depth == 24 && ph.sample_rate == 48000 && ph.has_artwork,
          "hires source: %d bits %d Hz", ph.bit_depth, ph.sample_rate);

    /* Written where no cover file is beside it, so has_artwork is the
     * embedded picture's */
    char *outdir = test_path(dir, "out");
    rx_mkdir_p(outdir);
    char *out = test_path(outdir, "out:hires?.flac");
    remove(out);
    int ret = rx_transcode_flac(hi, out, 44100);
    CHECK(ret >= 0, "transcode hires: %d", ret);
    RXProbe po;
    CHECK(rx_probe(out, &po) >= 0, "probe transcode");
    CHECK(!strcmp(po.codec, "flac") && po.bit_depth == 16 && po.sample_rate == 44100 &&
          po.channels == 2 && llabs(po.total_samples - n) <= 1 && po.flac.valid,
          "transcode: %s %d bits %d Hz %d ch %" PRId64 " samples", po.codec, po.bit_depth,
          po.sample_rate, po.channels, po.total_samples);
    const char *t = rx_probe_tag(&po, "title", NULL), *ar = rx_probe_tag(&po, "artist", NULL);
    CHECK(t && !strcmp(t, "Hi Res") && ar && !strcmp(ar, "Somebody") &&
          rx_probe_tag(&po, "album", NULL),
          "tags lost: %s / %s", t ? t : "-", ar ? ar : "-");
    CHECK(po.has_artwork, "cover lost");
    uint8_t *a1, *a2;
    size_t n1, n2;
    char c1[16], c2[16];
    CHECK(rx_artwork_raw(hi, &a1, &n1, c1) >= 0 && rx_artwork_raw(out, &a2, &n2, c2) >= 0 &&
          n1 == n2 && !memcmp(a1, a2, n1) && !strcmp(c2, "png"), "cover differs");
    free(a1);
    free(a2);
    /* Clicks still in place after 48k -> 44.1k */
    RXAudio ao;
    CHECK(rx_decode(out, 1, 0, &ao) >= 0, "decode transcode");
    for (int k = 1; k < 8; k++) {
        int64_t at = (int64_t)k * SIG_CLICK, best = at;
        for (int64_t i = at - 50; i <= at + 50; i++)
            if (fabsf(ao.samples[i]) > fabsf(ao.samples[best]))
                best = i;
        CHECK(llabs(best - at) <= 1, "click %d at %" PRId64, k, best);
    }
    rx_audio_free(&ao);
    char *part = rx_asprintf("%s.part", out);
    CHECK(!rx_file_exists(part), "part file left");
    free(part);
    rx_probe_free(&po);
    rx_probe_free(&ph);

    /* 16-bit at its own rate: untouched */
    char *out16 = test_path(dir, "wav.flac");
    CHECK(rx_transcode_flac(wav, out16, 48000) >= 0, "transcode wav");
    RXAudio aw, at;
    CHECK(rx_decode(wav, 0, 0, &aw) >= 0 && rx_decode(out16, 0, 0, &at) >= 0, "decode wav pair");
    CHECK(aw.nb_frames == at.nb_frames && !memcmp(aw.samples, at.samples,
          (size_t)aw.nb_frames * 2 * sizeof(float)), "16-bit transcode not exact");
    rx_audio_free(&at);

    /* MP3: the players' timeline carried over */
    if (have_encoder("libmp3lame")) {
        char *mp3 = test_path(dir, "sig.mp3");
        char *outm = test_path(dir, "mp3.flac");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "256k", mp3, NULL), "mp3");
        RXProbe pm, pt;
        CHECK(rx_probe(mp3, &pm) >= 0 && rx_transcode_flac(mp3, outm, 0) >= 0 &&
              rx_probe(outm, &pt) >= 0, "transcode mp3");
        CHECK(pt.total_samples == pm.total_samples && pt.bit_depth == 16,
              "mp3 transcode %" PRId64 " samples, players' timeline %" PRId64, pt.total_samples,
              pm.total_samples);
        RXAudio am, af;
        CHECK(rx_decode(mp3, 0, 0, &am) >= 0 && rx_decode(outm, 0, 0, &af) >= 0, "decode mp3 pair");
        /* Rounding plus triangular dither: within 1.5 steps of 16 bits,
         * except where the MP3 overshoots full scale and is clipped */
        float diff = 0.0f;
        for (int64_t i = 0; i < RX_MIN(am.nb_frames, af.nb_frames) * 2; i++)
            if (fabsf(am.samples[i]) < 0.999f)
                diff = RX_MAX(diff, fabsf(am.samples[i] - af.samples[i]));
        CHECK(diff * 32768 <= 1.5f, "mp3 transcode differs by %g steps", diff * 32768);
        rx_audio_free(&am);
        rx_audio_free(&af);
        rx_probe_free(&pm);
        rx_probe_free(&pt);
        free(mp3);
        free(outm);
    }

    /* A source that is not there leaves nothing behind */
    char *missing = test_path(dir, "missing.flac");
    char *nowhere = test_path(dir, "nowhere.flac");
    CHECK(rx_transcode_flac(missing, nowhere, 0) == -ENOENT && !rx_file_exists(nowhere),
          "missing source");

    rx_audio_free(&aw);
    free(pcm);
    free(rgb);
    free(raw);
    free(png);
    free(wav);
    free(hi);
    free(out);
    free(outdir);
    free(out16);
    free(missing);
    free(nowhere);
    free(dir);
    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
