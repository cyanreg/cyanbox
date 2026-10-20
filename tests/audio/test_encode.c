/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cb_encode (src/audio/encode.c), into memory through the write and seek
 * callbacks, from a pipeline at 44.1 kHz and 16 bits:
 *
 *  - a FLAC and a WAV of a 16-bit 44.1 kHz source decode to its samples bit
 *    for bit; the FLAC's frame table is the file's own (cb_flac_scan finds
 *    the same frames at the same places), its blocks the subset's 4608 at
 *    most, its MD5 the STREAMINFO's;
 *  - a 24-bit 48 kHz source comes out at 44.1 kHz in 16 bits, within the
 *    dither of the source decoded at that rate; a 5.1 one in stereo, a
 *    mono one in mono;
 *  - an MP3 is 320 kbit/s CBR with no Xing frame, of the frames it says,
 *    and lines up with the source sample for sample (LAME's delay was
 *    taken off what it was fed), as close as the bitrate gets;
 *  - with tags, a FLAC and an MP3 (ID3v2.3) have the source's title and
 *    cover;
 *  - `frames` makes a WAV's length exact (silence after the source ends);
 *  - a cancel stops it. */

#include "cyanbox/audio.h"

#include <inttypes.h>
#include <math.h>

#include "util.h"

static int failures;

/* ------------------------------------------------------- into memory */

typedef struct Mem {
    uint8_t *p;
    int64_t  size, pos, cap;
    int      cancel_after;      /* writes; 0 never */
    int      writes;
} Mem;

static int m_write(void *opaque, const uint8_t *buf, int n)
{
    Mem *m = opaque;
    if (m->pos + n > m->cap) {
        m->cap = CB_MAX(m->cap * 2, m->pos + n);
        m->p = realloc(m->p, (size_t)m->cap);
    }
    memcpy(m->p + m->pos, buf, (size_t)n);
    m->pos += n;
    m->size = CB_MAX(m->size, m->pos);
    m->writes++;
    return n;
}

static int64_t m_seek(void *opaque, int64_t off, int whence)
{
    Mem *m = opaque;
    if (whence != SEEK_SET || off < 0 || off > m->size)
        return -EINVAL;
    m->pos = off;
    return off;
}

static bool m_cancelled(void *opaque)
{
    Mem *m = opaque;
    return m->cancel_after && m->writes >= m->cancel_after;
}

static int encode(const char *src, CBEncodeOptions o, Mem *m, CBEncoded *e)
{
    free(m->p);
    *m = (Mem) { .cancel_after = m->cancel_after };
    o.write = m_write;
    o.seek = m_seek;
    o.cancelled = m_cancelled;
    o.opaque = m;
    return cb_encode(src, &o, e);
}

/* The encoded bytes as a file, its path. */
static char *as_file(const char *dir, const char *name, const Mem *m)
{
    char *p = test_path(dir, name);
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(m->p, 1, (size_t)m->size, f);
        fclose(f);
    }
    return p;
}

static int decode_bytes(const char *dir, const char *name, const Mem *m, CBAudio *a)
{
    char *p = as_file(dir, name, m);
    int ret = cb_decode(p, 0, 0, a);
    free(p);
    return ret;
}

static void same_samples(const CBAudio *a, const CBAudio *b, const char *what)
{
    CHECK(a->nb_frames == b->nb_frames && a->channels == b->channels && a->rate == b->rate &&
          !memcmp(a->samples, b->samples, (size_t)a->nb_frames * a->channels * sizeof(float)),
          "%s: not the source's samples (%" PRId64 " against %" PRId64 " frames)", what,
          b->nb_frames, a->nb_frames);
}

int main(void)
{
    char *dir = test_dir("encode");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, 6, &n);
    char *wav = test_path(dir, "sig.wav");
    write_wav(wav, SIG_RATE, 2, pcm, n);
    char *flac = test_path(dir, "sig.flac"), *flac24 = test_path(dir, "sig24.flac");
    char *png = test_path(dir, "cover.png"), *tagged = test_path(dir, "tagged.flac");
    char *six = test_path(dir, "six.flac"), *mono = test_path(dir, "mono.wav");
    CHECK(!ffmpeg("-i", wav, "-c:a", "flac", flac, NULL), "flac");
    CHECK(!ffmpeg("-i", wav, "-af", "volume=0.7", "-c:a", "flac", "-sample_fmt", "s32",
                  "-bits_per_raw_sample", "24", "-ar", "48000", flac24, NULL), "flac24");
    CHECK(!ffmpeg("-f", "lavfi", "-i", "color=c=teal:s=64x48", "-frames:v", "1", png, NULL), "png");
    CHECK(!ffmpeg("-i", wav, "-i", png, "-map", "0", "-map", "1", "-c:a", "flac", "-c:v", "copy",
                  "-disposition:v", "attached_pic", "-metadata", "title=Encoded Title", tagged, NULL),
          "tagged flac");
    CHECK(!ffmpeg("-i", wav, "-af", "pan=5.1|FL=c0|FR=c1|FC=0.5*c0|LFE=0.1*c0|BL=0.3*c0|BR=0.3*c1",
                  "-c:a", "flac", six, NULL), "5.1 flac");
    CHECK(!ffmpeg("-i", wav, "-ac", "1", mono, NULL), "mono wav");

    CBAudio src, out;
    CHECK(cb_decode(flac, 0, 0, &src) >= 0, "decode the source");

    /* FLAC and WAV of the 16-bit 44.1 kHz source: its samples. */
    static const struct { enum CBEncodeFormat f; const char *name; } fmts[] = {
        { CB_ENCODE_FLAC, "o16.flac" }, { CB_ENCODE_WAV, "o16.wav" },
    };
    Mem m = { 0 };
    CBEncoded e;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(fmts); i++) {
        CHECK(encode(flac, (CBEncodeOptions) { .format = fmts[i].f }, &m, &e) >= 0, "encode %s",
              fmts[i].name);
        CHECK(e.frames == src.nb_frames && e.size == m.size && e.rate == CB_ENCODE_RATE &&
              e.channels == 2, "%s: %" PRId64 " frames, %" PRId64 " bytes", fmts[i].name, e.frames,
              e.size);
        CHECK(decode_bytes(dir, fmts[i].name, &m, &out) >= 0, "decode %s", fmts[i].name);
        same_samples(&src, &out, fmts[i].name);
        cb_audio_free(&out);
        if (fmts[i].f == CB_ENCODE_WAV)
            CHECK(e.size == 44 + src.nb_frames * 4 && !memcmp(m.p + 36, "data", 4), "wav header");

        /* The FLAC's frame table and MD5, as the file has them. */
        if (fmts[i].f == CB_ENCODE_FLAC) {
            char *p = test_path(dir, fmts[i].name);
            CBFlacInfo fi;
            CHECK(cb_flac_scan(p, &fi) >= 0 && fi.valid, "scan");
            CHECK(fi.nb_frames == e.flac.nb_frames && fi.first_frame_offset == e.flac.first_frame_offset &&
                  fi.total_samples == e.flac.total_samples && fi.max_block <= 4608,
                  "frame table: %d frames at %" PRId64 " against %d at %" PRId64 ", blocks of %d",
                  e.flac.nb_frames, e.flac.first_frame_offset, fi.nb_frames, fi.first_frame_offset,
                  fi.max_block);
            for (int k = 0; k < CB_MIN(fi.nb_frames, e.flac.nb_frames); k++)
                if (fi.frames[k].sample != e.flac.frames[k].sample ||
                    fi.frames[k].offset != e.flac.frames[k].offset ||
                    fi.frames[k].block != e.flac.frames[k].block) {
                    CHECK(0, "frame %d differs", k);
                    break;
                }
            /* STREAMINFO: the first block after "fLaC", its MD5 at 18. */
            CHECK(!memcmp(m.p, "fLaC", 4) && !memcmp(m.p + 8 + 18, e.md5, 16), "md5");
            uint8_t zero[16] = { 0 };
            CHECK(memcmp(e.md5, zero, 16), "no md5");
            cb_flac_info_free(&fi);
            free(p);
        }
        cb_flac_info_free(&e.flac);
    }

    /* The 24-bit 48 kHz source: 16 bits at 44.1 kHz, within the dither. */
    CBAudio ref;
    CHECK(cb_decode(flac24, 0, CB_ENCODE_RATE, &ref) >= 0, "decode flac24 at 44.1 kHz");
    CHECK(encode(flac24, (CBEncodeOptions) { .format = CB_ENCODE_FLAC }, &m, &e) >= 0, "encode 24");
    CHECK(decode_bytes(dir, "o24.flac", &m, &out) >= 0 && out.rate == CB_ENCODE_RATE &&
          llabs(out.nb_frames - ref.nb_frames) <= 1, "decode o24");
    float worst = 0;
    for (int64_t k = 0; k < (CB_MIN(out.nb_frames, ref.nb_frames) - 1) * 2; k++)
        worst = CB_MAX(worst, fabsf(out.samples[k] - ref.samples[k]) * 32768.0f);
    CHECK(worst <= 1.5f, "24-bit 48 kHz to 16 at 44.1: off by %g steps", worst);
    cb_audio_free(&out);
    cb_flac_info_free(&e.flac);

    /* 5.1 in stereo, mono in mono. */
    CHECK(encode(six, (CBEncodeOptions) { .format = CB_ENCODE_WAV }, &m, &e) >= 0 && e.channels == 2,
          "5.1: %d channels", e.channels);
    CHECK(encode(mono, (CBEncodeOptions) { .format = CB_ENCODE_WAV }, &m, &e) >= 0 && e.channels == 1,
          "mono: %d channels", e.channels);

    /* MP3: CBR 320, no Xing frame, its frames, lined up with the source. */
    if (have_encoder("libmp3lame")) {
        CHECK(encode(tagged, (CBEncodeOptions) { .format = CB_ENCODE_MP3, .tags = true }, &m, &e) >= 0,
              "encode mp3");
        char *p = as_file(dir, "o.mp3", &m);
        CBMp3Info mi;
        CHECK(cb_mp3_scan(p, &mi) >= 0 && !mi.vbr && !mi.header_frame && mi.bitrate_kbps == 320 &&
              mi.nb_frames == e.mp3_frames && e.frames == e.mp3_frames * 1152,
              "mp3: %" PRId64 " frames (%" PRId64 " encoded), %d kbit/s, vbr %d, header %d",
              mi.nb_frames, e.mp3_frames, mi.bitrate_kbps, mi.vbr, mi.header_frame);
        CHECK(m.size > 10 && !memcmp(m.p, "ID3", 3) && m.p[3] == 3, "no ID3v2.3 tag");
        CBProbe pr;
        CHECK(cb_probe(p, &pr) >= 0 && pr.has_artwork, "mp3 cover");
        const char *title = cb_probe_tag(&pr, "title", NULL);
        CHECK(title && !strcmp(title, "Encoded Title"), "mp3 title %s", title ? title : "(none)");
        cb_probe_free(&pr);
        CHECK(cb_decode(p, 0, 0, &out) >= 0, "decode o.mp3");
        int best = 0;
        double least = INFINITY, err = 0, pow = 0;
        int64_t len = CB_MIN(out.nb_frames, src.nb_frames) - 4096;
        for (int lag = -64; lag <= 64; lag++) {
            double d = 0;
            for (int64_t i = 4096; i < len; i += 3) {
                double x = (double)src.samples[i * 2] - out.samples[(i + lag) * 2];
                d += x * x;
            }
            if (d < least) {
                least = d;
                best = lag;
            }
        }
        for (int64_t i = 4096 * 2; i < len * 2; i++) {
            err += ((double)src.samples[i] - out.samples[i]) * ((double)src.samples[i] - out.samples[i]);
            pow += (double)src.samples[i] * src.samples[i];
        }
        double snr = 10 * log10(pow / CB_MAX(err, 1e-30));
        CHECK(best == 0 && snr > 25, "mp3: %d frames off the source, %.1f dB", best, snr);
        cb_audio_free(&out);
        free(p);
    }

    /* FLAC with the tags: the title and the cover. */
    CHECK(encode(tagged, (CBEncodeOptions) { .format = CB_ENCODE_FLAC, .tags = true }, &m, &e) >= 0,
          "encode tagged");
    char *ot = as_file(dir, "tagged-out.flac", &m);
    CBProbe pt;
    CHECK(cb_probe(ot, &pt) >= 0 && pt.has_artwork, "flac cover");
    const char *ttitle = cb_probe_tag(&pt, "title", NULL);
    CHECK(ttitle && !strcmp(ttitle, "Encoded Title"), "flac title");
    cb_probe_free(&pt);
    cb_flac_info_free(&e.flac);
    free(ot);

    /* Exactly `frames`: shorter, and longer (silence after). */
    int64_t want[2] = { src.nb_frames / 2, src.nb_frames + 1000 };
    for (int k = 0; k < 2; k++) {
        CHECK(encode(flac, (CBEncodeOptions) { .format = CB_ENCODE_WAV, .frames = want[k] }, &m, &e) >= 0,
              "encode frames");
        CHECK(e.frames == want[k] && e.size == 44 + want[k] * 4, "frames %" PRId64 ": %" PRId64,
              want[k], e.frames);
        CHECK(decode_bytes(dir, "len.wav", &m, &out) >= 0 && out.nb_frames == want[k], "decode len");
        int64_t keep = CB_MIN(want[k], src.nb_frames);
        CHECK(!memcmp(out.samples, src.samples, (size_t)keep * 2 * sizeof(float)), "head differs");
        bool silent = true;
        for (int64_t j = keep * 2; j < want[k] * 2; j++)
            silent &= out.samples[j] == 0.0f;
        CHECK(silent, "the padding is not silence");
        cb_audio_free(&out);
    }

    /* A cancel stops it. */
    m.cancel_after = 3;
    CHECK(encode(flac, (CBEncodeOptions) { .format = CB_ENCODE_FLAC }, &m, &e) == -ECANCELED, "cancel");
    m.cancel_after = 0;

    cb_audio_free(&ref);
    cb_audio_free(&src);
    free(m.p);
    free(pcm);
    free(wav);
    free(flac);
    free(flac24);
    free(png);
    free(tagged);
    free(six);
    free(mono);
    free(dir);
    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
