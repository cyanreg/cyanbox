/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cb_transcode_flac: a 24-bit 48 kHz FLAC with tags and a cover becomes a
 * 16-bit 44.1 kHz FLAC of the same length with the same tags and cover; a
 * 16-bit WAV goes through sample for sample (no dither); an MP3 keeps its
 * players' timeline (the silent header frame and encoder delay become
 * samples of the FLAC).
 *
 * Link's: cb_transcode_wav makes a plain WAV, the 44-byte header and the
 * samples, no tags or cover, a 16-bit source sample for sample;
 * cb_transcode_mp3 a 320 kbps MP3 with no ID3 tag and no Xing frame whose
 * clicks, decoded as a player decodes it (delay and all), are where the
 * source has them. */

#include "cyanbox/audio.h"

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
    CBProbe ph;
    CHECK(cb_probe(hi, &ph) >= 0 && ph.bit_depth == 24 && ph.sample_rate == 48000 && ph.has_artwork,
          "hires source: %d bits %d Hz", ph.bit_depth, ph.sample_rate);

    /* Written where no cover file is beside it, so has_artwork is the
     * embedded picture's */
    char *outdir = test_path(dir, "out");
    cb_mkdir_p(outdir);
    char *out = test_path(outdir, "out:hires?.flac");
    remove(out);
    int ret = cb_transcode_flac(hi, out, 44100);
    CHECK(ret >= 0, "transcode hires: %d", ret);
    CBProbe po;
    CHECK(cb_probe(out, &po) >= 0, "probe transcode");
    CHECK(!strcmp(po.codec, "flac") && po.bit_depth == 16 && po.sample_rate == 44100 &&
          po.channels == 2 && llabs(po.total_samples - n) <= 1 && po.flac.valid,
          "transcode: %s %d bits %d Hz %d ch %" PRId64 " samples", po.codec, po.bit_depth,
          po.sample_rate, po.channels, po.total_samples);
    const char *t = cb_probe_tag(&po, "title", NULL), *ar = cb_probe_tag(&po, "artist", NULL);
    CHECK(t && !strcmp(t, "Hi Res") && ar && !strcmp(ar, "Somebody") &&
          cb_probe_tag(&po, "album", NULL),
          "tags lost: %s / %s", t ? t : "-", ar ? ar : "-");
    CHECK(po.has_artwork, "cover lost");
    uint8_t *a1, *a2;
    size_t n1, n2;
    char c1[16], c2[16];
    CHECK(cb_artwork_raw(hi, &a1, &n1, c1) >= 0 && cb_artwork_raw(out, &a2, &n2, c2) >= 0 &&
          n1 == n2 && !memcmp(a1, a2, n1) && !strcmp(c2, "png"), "cover differs");
    free(a1);
    free(a2);
    /* Clicks still in place after 48k -> 44.1k */
    CBAudio ao;
    CHECK(cb_decode(out, 1, 0, &ao) >= 0, "decode transcode");
    for (int k = 1; k < 8; k++) {
        int64_t at = (int64_t)k * SIG_CLICK, best = at;
        for (int64_t i = at - 50; i <= at + 50; i++)
            if (fabsf(ao.samples[i]) > fabsf(ao.samples[best]))
                best = i;
        CHECK(llabs(best - at) <= 1, "click %d at %" PRId64, k, best);
    }
    cb_audio_free(&ao);
    char *part = cb_asprintf("%s.part", out);
    CHECK(!cb_file_exists(part), "part file left");
    free(part);
    cb_probe_free(&po);
    cb_probe_free(&ph);

    /* 16-bit at its own rate: untouched */
    char *out16 = test_path(dir, "wav.flac");
    CHECK(cb_transcode_flac(wav, out16, 48000) >= 0, "transcode wav");
    CBAudio aw, at;
    CHECK(cb_decode(wav, 0, 0, &aw) >= 0 && cb_decode(out16, 0, 0, &at) >= 0, "decode wav pair");
    CHECK(aw.nb_frames == at.nb_frames && !memcmp(aw.samples, at.samples,
          (size_t)aw.nb_frames * 2 * sizeof(float)), "16-bit transcode not exact");
    cb_audio_free(&at);

    /* MP3: the players' timeline carried over */
    if (have_encoder("libmp3lame")) {
        char *mp3 = test_path(dir, "sig.mp3");
        char *outm = test_path(dir, "mp3.flac");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "256k", mp3, NULL), "mp3");
        CBProbe pm, pt;
        CHECK(cb_probe(mp3, &pm) >= 0 && cb_transcode_flac(mp3, outm, 0) >= 0 &&
              cb_probe(outm, &pt) >= 0, "transcode mp3");
        CHECK(pt.total_samples == pm.total_samples && pt.bit_depth == 16,
              "mp3 transcode %" PRId64 " samples, players' timeline %" PRId64, pt.total_samples,
              pm.total_samples);
        CBAudio am, af;
        CHECK(cb_decode(mp3, 0, 0, &am) >= 0 && cb_decode(outm, 0, 0, &af) >= 0, "decode mp3 pair");
        /* Rounding plus triangular dither: within 1.5 steps of 16 bits,
         * except where the MP3 overshoots full scale and is clipped */
        float diff = 0.0f;
        for (int64_t i = 0; i < CB_MIN(am.nb_frames, af.nb_frames) * 2; i++)
            if (fabsf(am.samples[i]) < 0.999f)
                diff = CB_MAX(diff, fabsf(am.samples[i] - af.samples[i]));
        CHECK(diff * 32768 <= 1.5f, "mp3 transcode differs by %g steps", diff * 32768);
        cb_audio_free(&am);
        cb_audio_free(&af);
        cb_probe_free(&pm);
        cb_probe_free(&pt);
        free(mp3);
        free(outm);
    }

    /* Link's WAV: from the 24-bit FLAC with tags and a cover, a plain
     * 16-bit WAV; from the 16-bit WAV, the same samples */
    char *lw = test_path(outdir, "link.wav");
    CHECK(cb_transcode_wav(hi, lw, 48000) >= 0, "transcode to wav");
    CBProbe pw;
    CHECK(cb_probe(lw, &pw) >= 0 && !strcmp(pw.codec, "pcm_s16le") && pw.sample_rate == 48000 &&
          pw.channels == 2 && !pw.has_artwork && !cb_probe_tag(&pw, "title", NULL),
          "link wav: %s %d Hz %d ch, cover %d", pw.codec, pw.sample_rate, pw.channels,
          pw.has_artwork);
    uint8_t *wb = NULL;
    size_t wn = 0;
    CHECK(cb_file_read(lw, &wb, &wn) >= 0 && wn > 44 && !memcmp(wb, "RIFF", 4) &&
          !memcmp(wb + 8, "WAVEfmt ", 8) && wb[16] == 16 && !memcmp(wb + 36, "data", 4) &&
          wb[40] + (wb[41] << 8) + (wb[42] << 16) + ((size_t)wb[43] << 24) == wn - 44,
          "link wav is not the plain 44-byte header and the samples");
    free(wb);
    cb_probe_free(&pw);
    CHECK(cb_transcode_wav(wav, lw, 48000) >= 0 && cb_decode(lw, 0, 0, &at) >= 0 &&
          aw.nb_frames == at.nb_frames &&
          !memcmp(aw.samples, at.samples, (size_t)aw.nb_frames * 2 * sizeof(float)),
          "16-bit link wav not exact");
    cb_audio_free(&at);
    free(lw);

    /* Link's MP3: no tag, no Xing frame, and the clicks in place */
    if (have_encoder("libmp3lame")) {
        char *lm = test_path(outdir, "link.mp3");
        CHECK(cb_transcode_mp3(wav, lm, 48000) >= 0, "transcode to mp3");
        CBProbe pl;
        CHECK(cb_probe(lm, &pl) >= 0 && !strcmp(pl.codec, "mp3") && pl.sample_rate == SIG_RATE &&
              pl.mp3.valid && !pl.mp3.vbr && pl.mp3.bitrate_kbps == 320,
              "link mp3: %s %d Hz, %d kbps", pl.codec, pl.sample_rate, pl.mp3.bitrate_kbps);
        uint8_t *mb = NULL;
        size_t mn = 0;
        CHECK(cb_file_read(lm, &mb, &mn) >= 0 && mn > 1044 && memcmp(mb, "ID3", 3) &&
              mb[0] == 0xff && (mb[1] & 0xe0) == 0xe0 && !memmem(mb, 1044, "Xing", 4) &&
              !memmem(mb, 1044, "Info", 4) && memcmp(mb + mn - 128, "TAG", 3),
              "link mp3 has a tag or a Xing frame");
        free(mb);
        CBAudio al;
        CHECK(cb_decode(lm, 1, 0, &al) >= 0 && al.nb_frames >= n, "decode link mp3");
        for (int k = 1; k < 8 && al.nb_frames >= n; k++) {
            int64_t at_ = (int64_t)k * SIG_CLICK, best = at_ - 200;
            for (int64_t i = at_ - 200; i <= at_ + 200; i++)
                if (fabsf(al.samples[i]) > fabsf(al.samples[best]))
                    best = i;
            CHECK(llabs(best - at_) <= 2, "link mp3 click %d at %" PRId64 ", not %" PRId64, k,
                  best, at_);
        }
        cb_audio_free(&al);
        cb_probe_free(&pl);
        free(lm);
    }

    /* A source that is not there leaves nothing behind */
    char *missing = test_path(dir, "missing.flac");
    char *nowhere = test_path(dir, "nowhere.flac");
    CHECK(cb_transcode_flac(missing, nowhere, 0) == -ENOENT && !cb_file_exists(nowhere),
          "missing source");

    cb_audio_free(&aw);
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
