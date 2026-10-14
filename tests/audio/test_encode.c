/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rx_decode_stream and rx_encode (src/audio/decode.c, encode.c):
 *
 *  - the stream is rx_decode's frames exactly, for FLAC, MP3, AAC and a
 *    resampling start;
 *  - into memory through the write and seek callbacks: a 16-bit FLAC, a
 *    WAV and an AIFF of a 16-bit source, and a 24-bit FLAC of a 24-bit
 *    one, decode to the source's samples bit for bit;
 *  - a FLAC's frame table is the file's own (rx_flac_scan finds the same
 *    frames at the same places) and its MD5 the STREAMINFO's;
 *  - `frames` makes the length exact (silence after the source ends);
 *  - a cancel stops it. */

#include "rxbox/audio.h"

#include <inttypes.h>
#include <math.h>

#include "util.h"

static int failures;

/* ---------------------------------------------------------- the stream */

typedef struct Collect {
    float  *x;
    int64_t n, cap;
    int     channels, rate;
} Collect;

static int c_start(void *opaque, int channels, int rate)
{
    Collect *c = opaque;
    c->channels = channels;
    c->rate = rate;
    return 0;
}

static int c_frames(void *opaque, const float *x, int64_t n)
{
    Collect *c = opaque;
    if (c->n + n > c->cap) {
        c->cap = RX_MAX(c->cap * 2, c->n + n);
        c->x = realloc(c->x, (size_t)c->cap * c->channels * sizeof(float));
    }
    memcpy(c->x + c->n * c->channels, x, (size_t)n * c->channels * sizeof(float));
    c->n += n;
    return 0;
}

static void same_stream(const char *path)
{
    RXAudio a;
    Collect c = { 0 };
    RXDecodeTo to = { .start = c_start, .frames = c_frames, .opaque = &c };
    CHECK(rx_decode(path, 0, 0, &a) >= 0 && rx_decode_stream(path, 0, &to) >= 0, "decode %s", path);
    CHECK(c.n == a.nb_frames && c.channels == a.channels && c.rate == a.rate &&
          !memcmp(c.x, a.samples, (size_t)a.nb_frames * a.channels * sizeof(float)),
          "%s: the stream is not rx_decode's (%" PRId64 " against %" PRId64 " frames)", path, c.n,
          a.nb_frames);
    rx_audio_free(&a);
    free(c.x);
}

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
        m->cap = RX_MAX(m->cap * 2, m->pos + n);
        m->p = realloc(m->p, (size_t)m->cap);
    }
    memcpy(m->p + m->pos, buf, (size_t)n);
    m->pos += n;
    m->size = RX_MAX(m->size, m->pos);
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

static int encode(const char *src, RXEncodeOptions o, Mem *m, RXEncoded *e)
{
    *m = (Mem) { .cancel_after = m->cancel_after };
    o.write = m_write;
    o.seek = m_seek;
    o.cancelled = m_cancelled;
    o.opaque = m;
    return rx_encode(src, &o, e);
}

/* The encoded bytes as a file, decoded. */
static int decode_bytes(const char *dir, const char *name, const Mem *m, RXAudio *a)
{
    char *p = test_path(dir, name);
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(m->p, 1, (size_t)m->size, f);
        fclose(f);
    }
    int ret = rx_decode(p, 0, 0, a);
    free(p);
    return ret;
}

static void same_samples(const RXAudio *a, const RXAudio *b, const char *what)
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
    char *flac = test_path(dir, "sig.flac");
    char *flac24 = test_path(dir, "sig24.flac");
    CHECK(!ffmpeg("-i", wav, "-c:a", "flac", flac, NULL), "flac");
    CHECK(!ffmpeg("-i", wav, "-af", "volume=0.7", "-c:a", "flac", "-sample_fmt", "s32",
                  "-bits_per_raw_sample", "24", "-ar", "48000", flac24, NULL), "flac24");

    /* The stream */
    same_stream(flac);
    same_stream(flac24);
    if (have_encoder("libmp3lame")) {
        char *mp3 = test_path(dir, "sig.mp3");
        CHECK(!ffmpeg("-i", wav, "-c:a", "libmp3lame", "-b:a", "256k", mp3, NULL), "mp3");
        same_stream(mp3);
        free(mp3);
    }
    char *m4a = test_path(dir, "sig.m4a");
    CHECK(!ffmpeg("-i", wav, "-c:a", "aac", m4a, NULL), "m4a");
    same_stream(m4a);
    free(m4a);

    RXAudio src, src24, out;
    CHECK(rx_decode(flac, 0, 0, &src) >= 0 && rx_decode(flac24, 0, 0, &src24) >= 0, "decode sources");

    /* 16-bit FLAC, WAV and AIFF of the 16-bit source: its samples. */
    static const struct { enum RXEncodeFormat f; const char *name; } fmts[] = {
        { RX_ENCODE_FLAC, "o16.flac" }, { RX_ENCODE_WAV, "o16.wav" }, { RX_ENCODE_AIFF, "o16.aiff" },
    };
    Mem m = { 0 };
    RXEncoded e;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(fmts); i++) {
        CHECK(encode(flac, (RXEncodeOptions) { .format = fmts[i].f, .bits = 16, .exact = true }, &m,
                     &e) >= 0, "encode %s", fmts[i].name);
        CHECK(e.frames == src.nb_frames && e.size == m.size && e.rate == SIG_RATE && e.channels == 2,
              "%s: %" PRId64 " frames, %" PRId64 " bytes", fmts[i].name, e.frames, e.size);
        CHECK(decode_bytes(dir, fmts[i].name, &m, &out) >= 0, "decode %s", fmts[i].name);
        same_samples(&src, &out, fmts[i].name);
        rx_audio_free(&out);
        if (fmts[i].f == RX_ENCODE_WAV)
            CHECK(e.size == 44 + src.nb_frames * 4 && !memcmp(m.p + 36, "data", 4), "wav header");

        /* The FLAC's frame table and MD5, as the file has them. */
        if (fmts[i].f == RX_ENCODE_FLAC) {
            char *p = test_path(dir, fmts[i].name);
            RXFlacInfo fi;
            CHECK(rx_flac_scan(p, &fi) >= 0 && fi.valid, "scan");
            CHECK(fi.nb_frames == e.flac.nb_frames && fi.first_frame_offset == e.flac.first_frame_offset &&
                  fi.total_samples == e.flac.total_samples,
                  "frame table: %d frames at %" PRId64 " against %d at %" PRId64, e.flac.nb_frames,
                  e.flac.first_frame_offset, fi.nb_frames, fi.first_frame_offset);
            for (int k = 0; k < RX_MIN(fi.nb_frames, e.flac.nb_frames); k++)
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
            rx_flac_info_free(&fi);
            free(p);
        }
        rx_flac_info_free(&e.flac);
    }

    /* 24-bit FLAC of the 24-bit source: its samples. */
    CHECK(encode(flac24, (RXEncodeOptions) { .format = RX_ENCODE_FLAC, .bits = 24, .exact = true },
                 &m, &e) >= 0, "encode 24");
    CHECK(decode_bytes(dir, "o24.flac", &m, &out) >= 0, "decode o24");
    same_samples(&src24, &out, "24-bit flac");
    rx_audio_free(&out);
    rx_flac_info_free(&e.flac);

    /* 16-bit WAV of it: within a step of dither, never more. */
    CHECK(encode(flac24, (RXEncodeOptions) { .format = RX_ENCODE_WAV, .bits = 16 }, &m, &e) >= 0,
          "encode 24 to 16");
    CHECK(decode_bytes(dir, "o24.wav", &m, &out) >= 0 && out.nb_frames == src24.nb_frames, "decode");
    float worst = 0;
    for (int64_t k = 0; k < out.nb_frames * 2 && k < src24.nb_frames * 2; k++)
        worst = RX_MAX(worst, fabsf(out.samples[k] - src24.samples[k]) * 32768.0f);
    CHECK(worst <= 1.5f, "24 to 16 bits: off by %g steps", worst);
    rx_audio_free(&out);

    /* Exactly `frames`: shorter, and longer (silence after). */
    int64_t want[2] = { src.nb_frames / 2, src.nb_frames + 1000 };
    for (int k = 0; k < 2; k++) {
        CHECK(encode(flac, (RXEncodeOptions) { .format = RX_ENCODE_WAV, .bits = 16, .exact = true,
                                               .frames = want[k] }, &m, &e) >= 0, "encode frames");
        CHECK(e.frames == want[k] && e.size == 44 + want[k] * 4, "frames %" PRId64 ": %" PRId64,
              want[k], e.frames);
        CHECK(decode_bytes(dir, "len.wav", &m, &out) >= 0 && out.nb_frames == want[k], "decode len");
        int64_t keep = RX_MIN(want[k], src.nb_frames);
        CHECK(!memcmp(out.samples, src.samples, (size_t)keep * 2 * sizeof(float)), "head differs");
        bool silent = true;
        for (int64_t j = keep * 2; j < want[k] * 2; j++)
            silent &= out.samples[j] == 0.0f;
        CHECK(silent, "the padding is not silence");
        rx_audio_free(&out);
    }

    /* A cancel stops it. */
    m.cancel_after = 3;
    CHECK(encode(flac, (RXEncodeOptions) { .format = RX_ENCODE_FLAC, .bits = 16 }, &m, &e) ==
          -ECANCELED, "cancel");
    m.cancel_after = 0;

    rx_audio_free(&src);
    rx_audio_free(&src24);
    free(m.p);
    free(pcm);
    free(wav);
    free(flac);
    free(flac24);
    free(dir);
    if (failures)
        fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
