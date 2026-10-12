/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rx_probe, rx_decode, rx_flac_scan and the derived buffers on files made
 * here: a WAV written sample by sample, and FLAC, MP3, Opus and AAC made
 * from it with the ffmpeg CLI. Sample values are checked exactly where the
 * codec is lossless, click positions where it is not, and the MP3 timeline
 * against the frame-map rule: an ffmpeg-written MP3 (stereo Info frame over
 * joint-stereo audio) starts at its first audio frame, the same file with
 * the Info frame patched to joint stereo (what LAME writes) gains a frame of
 * silence, and a damaged frame plays as silence instead of being dropped. */

#include "rxbox/audio.h"

#include <inttypes.h>
#include <math.h>
#include <unistd.h>

#include <libavutil/log.h>

#include "util.h"

static int failures;

/* LAME's encoder delay plus the decoder's: where an MP3's sample 0 lands
 * when nothing is trimmed (libmp3lame reports 576 + 528 + 1). */
#define MP3_DELAY 1105

static float s16f(int16_t v)
{
    return v * (1.0f / 32768);
}

/* The index of the largest |x| of channel c within +-radius of centre. */
static int64_t peak(const RXAudio *a, int c, int64_t centre, int64_t radius)
{
    int64_t best = -1;
    float max = -1.0f;
    int64_t end = RX_MIN(centre + radius + 1, a->nb_frames);
    for (int64_t i = RX_MAX(centre - radius, (int64_t)0); i < end; i++) {
        float v = fabsf(a->samples[i * a->channels + c]);
        if (v > max) {
            max = v;
            best = i;
        }
    }
    return best;
}

/* Every click of the signal at k * click * scale + offset, +-tol */
static void check_clicks(const char *what, const RXAudio *a, double scale, int64_t offset, int tol)
{
    int bad = 0;
    for (int64_t k = 1; k < SIG_SECONDS * 2; k++) {
        int64_t want = llround(k * SIG_CLICK * scale) + offset;
        int64_t got = peak(a, 0, want, 200);
        if (llabs(got - want) > tol && bad++ < 3)
            CHECK(false, "%s: click %" PRId64 " at %" PRId64 ", expected %" PRId64, what, k, got, want);
    }
}

/* Offset of the k-th frame from `pos` in a CBR stream (header lengths) */
static size_t mp3_frame_offset(const uint8_t *d, size_t pos, int k)
{
    static const int kbps[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int rates[3] = { 44100, 48000, 32000 };
    for (int i = 0; i < k; i++) {
        const uint8_t *h = d + pos;
        pos += 144 * kbps[h[2] >> 4] * 1000 / rates[(h[2] >> 2) & 3] + ((h[2] >> 1) & 1);
    }
    return pos;
}

static void write_bytes(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(d, 1, n, f) != n)
        exit(1);
    fclose(f);
}

/* Lines a log sink was given */
typedef struct LogCatch {
    int n;
    enum RXLogLevel level[4];
    char line[4][256];
} LogCatch;

static void catch_line(void *opaque, enum RXLogLevel level, const char *line)
{
    LogCatch *c = opaque;
    if (c->n < 4) {
        c->level[c->n] = level;
        snprintf(c->line[c->n], sizeof(c->line[0]), "%s", line);
    }
    c->n++;
}

/* libav's own lines ("timescale not set") go through rxbox's log and not
 * to stderr, with the name of what said them: warnings and errors as
 * warnings, notes at debug. The level libav is set to still filters
 * first (the tests quiet it that way). */
static void test_logging(void)
{
    rx_audio_init_logging();
    LogCatch lc = { 0 };
    enum RXLogLevel was = rx_log_level();
    int av_was = av_log_get_level();
    rx_log_set_level(RX_LOG_DEBUG);
    av_log_set_level(AV_LOG_INFO);
    rx_log_set_sink(catch_line, &lc);

    static const AVClass cls = {
        .class_name = "testdemux",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };
    const AVClass *ctx = &cls;
    av_log(&ctx, AV_LOG_WARNING, "timescale not set\n");
    av_log(&ctx, AV_LOG_INFO, "a note %d\n", 7);
    av_log(&ctx, AV_LOG_VERBOSE, "chatter\n");
    av_log_set_level(AV_LOG_QUIET);
    av_log(&ctx, AV_LOG_ERROR, "silenced\n");

    rx_log_set_sink(NULL, NULL);
    rx_log_set_level(was);
    av_log_set_level(av_was);
    CHECK(lc.n == 2, "%d lines from libav", lc.n);
    CHECK(lc.n >= 1 && lc.level[0] == RX_LOG_WARN && strstr(lc.line[0], "testdemux") &&
          strstr(lc.line[0], "timescale not set") && !strchr(lc.line[0], '\n'),
          "warning line: %d '%s'", lc.level[0], lc.line[0]);
    CHECK(lc.n >= 2 && lc.level[1] == RX_LOG_DEBUG && strstr(lc.line[1], "testdemux") &&
          strstr(lc.line[1], "a note 7"), "note line: %d '%s'", lc.level[1], lc.line[1]);
}

static void test_mp3(const char *dir, const char *wav)
{
    char *mp3 = test_path(dir, "sig.mp3");
    CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "320k", mp3, NULL), "mp3 encode");

    RXProbe p;
    CHECK(rx_probe(mp3, &p) >= 0, "probe mp3");
    CHECK(!strcmp(p.codec, "mp3") && !strcmp(p.container, "mp3") && p.bit_depth == 0,
          "mp3 probe: %s %s %d", p.codec, p.container, p.bit_depth);
    RXMp3Info mi = p.mp3;
    CHECK(mi.valid && mi.header_frame && !mi.vbr && mi.bitrate_kbps == 320,
          "mp3: valid %d header %d vbr %d %d kbps", mi.valid, mi.header_frame, mi.vbr,
          mi.bitrate_kbps);
    CHECK(p.total_samples == (mi.nb_frames + mi.header_counts) * 1152,
          "mp3 total %" PRId64 ", %" PRId64 " frames", p.total_samples, mi.nb_frames);
    CHECK(fabs(p.duration - (double)p.total_samples / 44100) < 1e-9, "mp3 duration %f", p.duration);

    /* What the encoder wrote: libmp3lame's audio is joint stereo. Lavf
     * before 2026-09 (6397b2b5b6) wrote a stereo Info frame over it, newer
     * Lavf copies the first audio frame's mode, as LAME itself does. */
    uint8_t *d;
    size_t n;
    CHECK(rx_file_read(mp3, &d, &n) >= 0, "read mp3");
    size_t hdr = (size_t)mi.first_frame_offset;
    size_t first_audio = mp3_frame_offset(d, hdr, 1);
    int info_mode = d[hdr + 3] >> 6, audio_mode = d[first_audio + 3] >> 6;
    CHECK(audio_mode == 1, "libmp3lame audio mode %d, not joint stereo", audio_mode);
    CHECK(mi.header_counts == (info_mode == audio_mode), "Info mode %d, audio %d, counts %d",
          info_mode, audio_mode, mi.header_counts);

    RXAudio a;
    CHECK(rx_decode(mp3, 0, 0, &a) >= 0, "decode mp3");
    CHECK(a.nb_frames == p.total_samples && a.channels == 2 && a.rate == 44100,
          "mp3 decoded %" PRId64 " frames, probe %" PRId64, a.nb_frames, p.total_samples);
    /* Nothing trimmed: the signal sits LAME's delay plus the decoder's late */
    check_clicks("mp3", &a, 1.0, MP3_DELAY + 1152 * mi.header_counts, 1);
    rx_audio_free(&a);

    /* Both kinds of Info frame on the same audio. Old Lavf, stereo over
     * joint stereo: the map restarts, the timeline starts at the audio. */
    d[hdr + 3] = (uint8_t)((d[hdr + 3] & 0x3F) | (audio_mode ^ 1) << 6);
    char *lavf = test_path(dir, "lavf-style.mp3");
    write_bytes(lavf, d, n);
    /* LAME: the Info frame matches and plays as a frame of silence */
    d[hdr + 3] = (uint8_t)((d[hdr + 3] & 0x3F) | audio_mode << 6);
    char *lame = test_path(dir, "lame-style.mp3");
    write_bytes(lame, d, n);

    RXProbe pf, pl;
    CHECK(rx_probe(lavf, &pf) >= 0 && rx_probe(lame, &pl) >= 0, "probe variants");
    CHECK(!pf.mp3.header_counts && pf.total_samples == mi.nb_frames * 1152,
          "lavf-style: counts %d total %" PRId64, pf.mp3.header_counts, pf.total_samples);
    CHECK(pl.mp3.header_counts && pl.total_samples == (mi.nb_frames + 1) * 1152,
          "lame-style: counts %d total %" PRId64, pl.mp3.header_counts, pl.total_samples);

    RXAudio al;
    CHECK(rx_decode(lavf, 0, 0, &a) >= 0 && rx_decode(lame, 0, 0, &al) >= 0, "decode variants");
    CHECK(a.nb_frames == mi.nb_frames * 1152, "lavf-style decoded %" PRId64, a.nb_frames);
    CHECK(al.nb_frames == a.nb_frames + 1152, "lame-style decoded %" PRId64, al.nb_frames);
    if (al.nb_frames == a.nb_frames + 1152) {
        bool silent = true;
        for (int i = 0; i < 1152 * 2; i++)
            silent &= al.samples[i] == 0.0f;
        CHECK(silent, "lame-style: the header frame is not silent");
        CHECK(!memcmp(al.samples + 1152 * 2, a.samples, (size_t)a.nb_frames * 2 * sizeof(float)),
              "lame-style: audio after the silent frame differs");
    }
    check_clicks("lavf-style mp3", &a, 1.0, MP3_DELAY, 1);
    check_clicks("lame-style mp3", &al, 1.0, MP3_DELAY + 1152, 1);
    rx_audio_free(&al);
    rx_probe_free(&pl);
    rx_probe_free(&pf);

    /* A damaged frame (side information with big_values > 288, which
     * libavcodec rejects) keeps its slot, in silence */
    const int k = 100;
    d[hdr + 3] = (uint8_t)((d[hdr + 3] & 0x3F) | (audio_mode ^ 1) << 6);
    size_t at = mp3_frame_offset(d, first_audio, k);
    d[at + 8] = 0xFF;
    d[at + 9] |= 0x80;
    char *damaged = test_path(dir, "damaged.mp3");
    write_bytes(damaged, d, n);
    RXAudio ad;
    CHECK(rx_decode(damaged, 0, 0, &ad) >= 0, "decode damaged");
    CHECK(ad.nb_frames == a.nb_frames, "damaged: %" PRId64 " frames, intact %" PRId64,
          ad.nb_frames, a.nb_frames);
    if (ad.nb_frames == a.nb_frames) {
        bool silent = true;
        for (int64_t i = (int64_t)k * 1152 * 2; i < (int64_t)(k + 1) * 1152 * 2; i++)
            silent &= ad.samples[i] == 0.0f;
        CHECK(silent, "damaged frame not silent");
        CHECK(!memcmp(ad.samples, a.samples, (size_t)k * 1152 * 2 * sizeof(float)),
              "audio before the damaged frame differs");
        /* Back in step after it: the next frames may miss their bit
         * reservoir, the rest is exact */
        int64_t from = (int64_t)(k + 3) * 1152 * 2;
        CHECK(!memcmp(ad.samples + from, a.samples + from,
                      (size_t)(a.nb_frames * 2 - from) * sizeof(float)),
              "audio after the damaged frame moved");
    }
    rx_audio_free(&ad);

    /* Without a Xing frame */
    char *plain = test_path(dir, "noxing.mp3");
    CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "320k", "-write_xing", "0", plain, NULL),
          "mp3 encode");
    RXProbe pp;
    CHECK(rx_probe(plain, &pp) >= 0, "probe noxing");
    CHECK(!pp.mp3.header_frame && !pp.mp3.header_counts && pp.mp3.nb_frames == mi.nb_frames,
          "noxing: header %d counts %d frames %" PRId64, pp.mp3.header_frame,
          pp.mp3.header_counts, pp.mp3.nb_frames);
    RXAudio ap;
    CHECK(rx_decode(plain, 1, 0, &ap) >= 0 && ap.nb_frames == pp.total_samples,
          "noxing decoded %" PRId64 " of %" PRId64, ap.nb_frames, pp.total_samples);
    rx_audio_free(&ap);
    rx_probe_free(&pp);

    /* Mono mix of a lossy decode is still the mean */
    RXAudio m, m2;
    CHECK(rx_decode(lavf, 1, 0, &m) >= 0 && rx_audio_to_mono(&a, &m2) >= 0, "mp3 mono");
    CHECK(m.nb_frames == a.nb_frames &&
          !memcmp(m.samples, m2.samples, (size_t)m.nb_frames * sizeof(float)),
          "mp3 mono mix differs from rx_audio_to_mono");
    rx_audio_free(&m);
    rx_audio_free(&m2);

    free(d);
    rx_audio_free(&a);
    rx_probe_free(&p);
    free(mp3);
    free(lame);
    free(lavf);
    free(damaged);
    free(plain);
}

int main(void)
{
    char *dir = test_dir("decode");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, SIG_SECONDS, &n);
    char *wav = test_path(dir, "sig.wav");
    char *flac = test_path(dir, "sig.flac");
    CHECK(write_wav(wav, SIG_RATE, 2, pcm, n) >= 0, "write wav");
    CHECK(!ffmpeg("-i", wav, "-c:a", "flac", "-metadata", "title=  Padded Title \t",
                  "-metadata", "ARTIST=Someone", "-metadata", "genre=\xe3\x80\x80House\xe3\x80\x80",
                  flac, NULL), "flac encode");

    /* WAV and FLAC decode to the exact samples */
    const char *lossless[] = { wav, flac };
    for (int f = 0; f < 2; f++) {
        RXAudio a;
        int ret = rx_decode(lossless[f], 0, 0, &a);
        CHECK(ret >= 0, "decode %s: %d", lossless[f], ret);
        if (ret < 0)
            continue;
        CHECK(a.nb_frames == n && a.channels == 2 && a.rate == SIG_RATE,
              "%s: %" PRId64 " frames %d ch %d Hz", lossless[f], a.nb_frames, a.channels, a.rate);
        bool exact = a.nb_frames == n;
        for (int64_t i = 0; exact && i < n * 2; i++)
            exact = a.samples[i] == s16f(pcm[i]);
        CHECK(exact, "%s: samples differ", lossless[f]);
        rx_audio_free(&a);
    }

    /* Mono: the mean of the channels, the same from the decoder and from
     * rx_audio_to_mono; clicks where they were written */
    RXAudio st, mono, mono2;
    CHECK(rx_decode(flac, 0, 0, &st) >= 0, "decode flac");
    CHECK(rx_decode(flac, 1, 0, &mono) >= 0 && mono.channels == 1 && mono.nb_frames == n, "mono");
    CHECK(rx_audio_to_mono(&st, &mono2) >= 0 && mono2.nb_frames == n, "to_mono");
    bool mean = true;
    for (int64_t i = 0; i < n; i++)
        mean &= mono.samples[i] == (s16f(pcm[2 * i]) + s16f(pcm[2 * i + 1])) / 2.0f &&
                mono2.samples[i] == mono.samples[i];
    CHECK(mean, "mono mix is not the mean");
    check_clicks("flac", &mono, 1.0, 0, 0);

    /* Stereo from mono duplicates it */
    int64_t nm;
    int16_t *pcm1 = make_signal(SIG_RATE, 1, 2, &nm);
    char *wav1 = test_path(dir, "mono.wav");
    CHECK(write_wav(wav1, SIG_RATE, 1, pcm1, nm) >= 0, "write mono wav");
    RXAudio dup;
    CHECK(rx_decode(wav1, 2, 0, &dup) >= 0 && dup.channels == 2 && dup.nb_frames == nm, "dup");
    bool same = true;
    for (int64_t i = 0; i < nm; i++)
        same &= dup.samples[2 * i] == s16f(pcm1[i]) && dup.samples[2 * i + 1] == s16f(pcm1[i]);
    CHECK(same, "mono not duplicated");
    rx_audio_free(&dup);

    /* Resampling: length rate-scaled, clicks in place, the same from the
     * decoder and from a buffer */
    const int rates[] = { 48000, 22050, 96000 };
    for (int r = 0; r < 3; r++) {
        RXAudio rs, rb;
        CHECK(rx_decode(flac, 1, rates[r], &rs) >= 0, "decode at %d", rates[r]);
        CHECK(rx_audio_resample(&mono, rates[r], &rb) >= 0, "resample to %d", rates[r]);
        int64_t want = n * rates[r] / SIG_RATE;
        CHECK(rs.rate == rates[r] && llabs(rs.nb_frames - want) <= 1,
              "at %d Hz: %" PRId64 " frames, want %" PRId64, rates[r], rs.nb_frames, want);
        CHECK(rb.nb_frames == rs.nb_frames, "resampled buffer %" PRId64 " frames, decoder %" PRId64,
              rb.nb_frames, rs.nb_frames);
        float diff = 0.0f;
        for (int64_t i = 0; i < RX_MIN(rb.nb_frames, rs.nb_frames); i++)
            diff = RX_MAX(diff, fabsf(rb.samples[i] - rs.samples[i]));
        CHECK(diff < 1e-6f, "at %d Hz: buffer and decoder differ by %g", rates[r], diff);
        check_clicks("resampled", &rs, (double)rates[r] / SIG_RATE, 0, 1);
        rx_audio_free(&rs);
        rx_audio_free(&rb);
    }

    /* Probe */
    RXProbe p;
    CHECK(rx_probe(wav, &p) >= 0, "probe wav");
    CHECK(!strcmp(p.codec, "pcm_s16le") && !strcmp(p.container, "wav") && p.bit_depth == 16 &&
          p.total_samples == n && p.duration == SIG_SECONDS && p.sample_rate == SIG_RATE &&
          p.channels == 2 && abs(p.bit_rate - SIG_RATE * 32) < 1000 && !p.has_artwork,
          "wav probe: %s %s %d bits %" PRId64 " samples %f s %d bps", p.codec, p.container,
          p.bit_depth, p.total_samples, p.duration, p.bit_rate);
    rx_probe_free(&p);

    CHECK(rx_probe(flac, &p) >= 0, "probe flac");
    CHECK(!strcmp(p.codec, "flac") && !strcmp(p.container, "flac") && p.bit_depth == 16 &&
          p.total_samples == n && p.duration == SIG_SECONDS && p.flac.valid,
          "flac probe: %s %s %d %" PRId64 " %d", p.codec, p.container, p.bit_depth,
          p.total_samples, p.flac.valid);
    const char *t = rx_probe_tag(&p, "TITLE", NULL);
    CHECK(t && !strcmp(t, "Padded Title"), "title tag '%s'", t ? t : "(null)");
    t = rx_probe_tag(&p, "missing", "", "artist", NULL);
    CHECK(t && !strcmp(t, "Someone"), "artist tag '%s'", t ? t : "(null)");
    /* U+3000 around a value goes, as with Python's str.strip() */
    t = rx_probe_tag(&p, "genre", NULL);
    CHECK(t && !strcmp(t, "House"), "genre tag '%s'", t ? t : "(null)");
    CHECK(!rx_probe_tag(&p, "nothing", NULL), "missing tag found");
    bool lower = true;
    for (int i = 0; i < p.nb_tags; i++)
        for (const char *c = p.tags[i].key; *c; c++)
            lower &= !(*c >= 'A' && *c <= 'Z');
    CHECK(lower, "tag keys not lower-cased");

    /* The FLAC frame table against STREAMINFO */
    RXFlacInfo fi;
    CHECK(rx_flac_scan(flac, &fi) >= 0 && fi.valid, "flac scan");
    CHECK(fi.total_samples == n && fi.sample_rate == SIG_RATE && fi.channels == 2 &&
          fi.bit_depth == 16 && fi.max_block > 0 && fi.first_frame_offset > 42,
          "streaminfo: %" PRId64 " %d %d %d %d %" PRId64, fi.total_samples, fi.sample_rate,
          fi.channels, fi.bit_depth, fi.max_block, fi.first_frame_offset);
    int64_t want_frames = (n + fi.max_block - 1) / fi.max_block;
    CHECK(fi.nb_frames == want_frames, "%d frames, want %" PRId64, fi.nb_frames, want_frames);
    bool table = fi.nb_frames > 0 && fi.frames[0].sample == 0 && fi.frames[0].offset == 0;
    int64_t total = 0;
    for (int i = 0; table && i < fi.nb_frames; i++) {
        table = fi.frames[i].sample == (int64_t)i * fi.max_block && fi.frames[i].sample == total &&
                (i == 0 || fi.frames[i].offset > fi.frames[i - 1].offset);
        total += fi.frames[i].block;
    }
    CHECK(table && total == n, "frame table wrong (blocks sum to %" PRId64 ")", total);
    CHECK(p.flac.nb_frames == fi.nb_frames && p.flac.first_frame_offset == fi.first_frame_offset,
          "probe's flac table differs");
    /* Frame 1 starts where the bytes say */
    uint8_t *bytes;
    size_t size;
    CHECK(rx_file_read(flac, &bytes, &size) >= 0, "read flac");
    if (fi.nb_frames > 1) {
        const uint8_t *f1 = bytes + fi.first_frame_offset + fi.frames[1].offset;
        CHECK(f1[0] == 0xFF && (f1[1] & 0xFE) == 0xF8, "frame 1 has no sync code");
    }
    rx_flac_info_free(&fi);
    rx_probe_free(&p);

    /* A name with ':' and '?' (and '%'): not a protocol, not a query */
    char *weird_dir = test_path(dir, "AD:HOUSE 11");
    rx_mkdir_p(weird_dir);
    char *weird = test_path(weird_dir, "01 what?x%41:y.flac");
    CHECK(copy_file(flac, weird) >= 0, "copy weird");
    CHECK(rx_probe(weird, &p) >= 0 && p.total_samples == n && p.flac.valid, "probe weird name");
    rx_probe_free(&p);
    RXAudio aw;
    CHECK(rx_decode(weird, 1, 0, &aw) >= 0 && aw.nb_frames == n, "decode weird name");
    rx_audio_free(&aw);

    /* A WAV cut off mid-frame (an interrupted copy): every whole sample
     * frame decodes, exactly, and nothing more. The last packet decodes but
     * for its stray bytes, which libavcodec then reports as an error: that
     * is not a packet lost, and its length must not come again as silence.
     * The probe says the same length. */
    {
        char *cut = test_path(dir, "cut.wav");
        const int64_t keep = 30000;
        CHECK(write_wav(cut, SIG_RATE, 2, pcm, n) >= 0 && !truncate(cut, 44 + keep * 4 + 3),
              "write cut wav");
        RXProbe pc;
        RXAudio ac;
        CHECK(rx_probe(cut, &pc) >= 0 && rx_decode(cut, 0, 0, &ac) >= 0, "cut wav");
        CHECK(ac.nb_frames == keep && pc.total_samples == keep,
              "cut wav: decoded %" PRId64 ", probe %" PRId64 ", want %" PRId64, ac.nb_frames,
              pc.total_samples, keep);
        bool exact = ac.nb_frames >= keep;
        for (int64_t i = 0; exact && i < keep * 2; i++)
            exact = ac.samples[i] == s16f(pcm[i]);
        CHECK(exact, "cut wav: samples differ");
        rx_audio_free(&ac);
        rx_probe_free(&pc);
        free(cut);
    }

    /* Errors */
    char *missing = test_path(dir, "missing.flac");
    CHECK(rx_probe(missing, &p) == -ENOENT, "missing file");
    CHECK(rx_decode(missing, 0, 0, &aw) == -ENOENT, "missing decode");
    free(missing);
    char *txt = test_path(dir, "text.flac");
    write_bytes(txt, (const uint8_t *)"not audio at all\n", 17);
    CHECK(rx_probe(txt, &p) < 0 && rx_decode(txt, 0, 0, &aw) < 0, "text file accepted");
    CHECK(rx_decode(flac, 3, 0, &aw) == -EINVAL, "3 channels accepted");

    if (have_encoder("libmp3lame"))
        test_mp3(dir, wav);
    else
        fprintf(stderr, "no libmp3lame in ffmpeg: MP3 decode checks skipped "
                "(test_mp3 covers the scanner)\n");

    /* Opus decodes natively at 48 kHz, its pre-skip trimmed by libavcodec */
    if (have_encoder("libopus")) {
        char *opus = test_path(dir, "sig.opus");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libopus", "-b:a", "192k", opus, NULL), "opus encode");
        RXProbe po;
        RXAudio ao;
        CHECK(rx_probe(opus, &po) >= 0 && !strcmp(po.codec, "opus") && !strcmp(po.container, "ogg"),
              "opus probe %s %s", po.codec, po.container);
        CHECK(rx_decode(opus, 1, 0, &ao) >= 0 && ao.rate == 48000, "opus decode");
        CHECK(llabs(ao.nb_frames - 480000) <= 960, "opus: %" PRId64 " frames", ao.nb_frames);
        check_clicks("opus", &ao, 48000.0 / SIG_RATE, 0, 2);
        rx_audio_free(&ao);
        rx_probe_free(&po);
        free(opus);
    }

    /* AAC keeps ffmpeg's priming handling: the edit list's delay is skipped */
    {
        char *m4a = test_path(dir, "sig.m4a");
        CHECK(!ffmpeg("-i", wav, "-c:a", "aac", "-b:a", "256k", m4a, NULL), "aac encode");
        RXAudio aa;
        CHECK(rx_decode(m4a, 1, 0, &aa) >= 0, "aac decode");
        CHECK(llabs(aa.nb_frames - n) <= 1024, "aac: %" PRId64 " frames", aa.nb_frames);
        check_clicks("aac", &aa, 1.0, 0, 2);
        rx_audio_free(&aa);
        free(m4a);
    }

    /* rx_probe's length is rx_decode's for every codec and container the
     * container can say it for: AAC with its priming (ffmpeg's and FDK's
     * encoders) in MP4, AAC in ADTS (bitrate-estimated duration), Opus in
     * Ogg and MP4, Vorbis, ALAC, WAV */
    {
        static const struct {
            const char *name, *encoder;
            const char *args[6];
        } kinds[] = {
            { "aac.m4a",    "aac",        { "-c:a", "aac", "-b:a", "192k" } },
            { "fdk.m4a",    "libfdk_aac", { "-c:a", "libfdk_aac" } },
            { "adts.aac",   "aac",        { "-c:a", "aac", "-b:a", "192k" } },
            { "opus.opus",  "libopus",    { "-c:a", "libopus" } },
            { "opus.mp4",   "libopus",    { "-c:a", "libopus" } },
            { "vorbis.ogg", "libvorbis",  { "-c:a", "libvorbis" } },
            { "alac.m4a",   "alac",       { "-c:a", "alac" } },
            { "pcm.wav",    "pcm_s24le",  { "-c:a", "pcm_s24le" } },
        };
        for (size_t k = 0; k < RX_ARRAY_ELEMS(kinds); k++) {
            if (!have_encoder(kinds[k].encoder)) {
                fprintf(stderr, "no %s encoder: %s skipped\n", kinds[k].encoder, kinds[k].name);
                continue;
            }
            char *file = test_path(dir, kinds[k].name);
            /* ffmpeg() stops at the first NULL: pad short lists with a no-op */
            const char *const *a = kinds[k].args;
            const char *a2 = a[2] ? a[2] : "-hide_banner", *a3 = a[3] ? a[3] : "-hide_banner";
            CHECK(!ffmpeg("-i", wav, a[0], a[1], a2, a3, file, NULL), "encode %s", kinds[k].name);
            RXProbe pk;
            RXAudio ak;
            CHECK(rx_probe(file, &pk) >= 0 && rx_decode(file, 0, 0, &ak) >= 0, "%s", kinds[k].name);
            CHECK(pk.total_samples == ak.nb_frames, "%s (%s in %s): probe %" PRId64
                  " samples, decoder %" PRId64, kinds[k].name, pk.codec, pk.container,
                  pk.total_samples, ak.nb_frames);
            CHECK(fabs(pk.duration * ak.rate - (double)ak.nb_frames) < 1e-6, "%s: duration %f",
                  kinds[k].name, pk.duration);
            rx_audio_free(&ak);
            rx_probe_free(&pk);
            free(file);
        }
    }

    test_logging();

    /* Ten minutes at 96 kHz: the buffer grows without trouble */
    {
        char *big = test_path(dir, "long96k.flac");
        CHECK(!ffmpeg("-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=96000:duration=630",
                      "-ac", "2", "-c:a", "flac", "-compression_level", "0", big, NULL), "long flac");
        int64_t t0 = rx_time_us();
        RXAudio al;
        int ret = rx_decode(big, 1, 0, &al);
        CHECK(ret >= 0 && al.nb_frames == 630 * 96000 && al.rate == 96000,
              "long decode %d: %" PRId64 " frames", ret, al.nb_frames);
        fprintf(stderr, "10.5 min 96 kHz stereo FLAC to mono: %.2f s\n", (rx_time_us() - t0) / 1e6);
        rx_audio_free(&al);
        remove(big);
        free(big);
    }

    rx_audio_free(&st);
    rx_audio_free(&mono);
    rx_audio_free(&mono2);
    free(bytes);
    free(pcm);
    free(pcm1);
    free(wav);
    free(wav1);
    free(flac);
    free(weird);
    free(weird_dir);
    free(txt);
    free(dir);
    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
