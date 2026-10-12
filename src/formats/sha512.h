/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* SHA-512 (FIPS 180-4), HMAC-SHA512 (RFC 2104) and PBKDF2-HMAC-SHA512
 * (RFC 8018 5.2), for the SQLCipher pages of exportLibrary.db.
 *
 * Our own rather than libavutil's: SQLCipher's key derivation runs 256000
 * HMACs on every open, and av_hmac hashes both pad blocks again on every
 * call (four compressions a round) behind an opaque hash context. Here the
 * state is a plain struct, so an HMAC keeps the states after its ipad and
 * opad blocks and a PBKDF2 round costs two compressions, run on the words
 * directly. Checked against the FIPS 180-4 and RFC 4231 vectors in
 * tests/formats/test_onelibrary.c. */

#ifndef RXBOX_FORMATS_SHA512_H
#define RXBOX_FORMATS_SHA512_H

#include <stdint.h>
#include <string.h>

#include "src/formats/buf.h"

typedef struct RXSHA512 {
    uint64_t state[8];
    uint64_t count;      /* bytes hashed */
    uint8_t  buf[128];   /* a partial block, count % 128 bytes */
} RXSHA512;

/* HMAC-SHA512 keyed once: `inner` and `outer` are the states after the
 * ipad and opad blocks, `ctx` the message in progress. */
typedef struct RXHMACSHA512 {
    RXSHA512 inner, outer, ctx;
} RXHMACSHA512;

static const uint64_t rx_sha512_k[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
    0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
    0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
    0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
    0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
    0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
    0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

static inline uint64_t rx_sha512_ror(uint64_t x, int n)
{
    return x >> n | x << (64 - n);
}

static inline void rx_sha512_wr_be64(uint8_t *p, uint64_t v)
{
    rx_wr_be32(p, (uint32_t)(v >> 32));
    rx_wr_be32(p + 4, (uint32_t)v);
}

/* One block, as 16 big-endian words already read. */
static inline void rx_sha512_compress(uint64_t st[8], const uint64_t m[16])
{
    uint64_t w[80];
    memcpy(w, m, 16 * sizeof(*w));
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = rx_sha512_ror(w[i - 15], 1) ^ rx_sha512_ror(w[i - 15], 8) ^ w[i - 15] >> 7;
        uint64_t s1 = rx_sha512_ror(w[i - 2], 19) ^ rx_sha512_ror(w[i - 2], 61) ^ w[i - 2] >> 6;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint64_t e = st[4], f = st[5], g = st[6], h = st[7];
    /* Eight rounds a pass with the variables renamed rather than moved:
     * a round leaves its new e in d and its new a in h, which the next
     * round reads as its e and a. */
#define RX_SHA512_ROUND(a, b, c, d, e, f, g, h, i) do {                                      \
        h += (rx_sha512_ror(e, 14) ^ rx_sha512_ror(e, 18) ^ rx_sha512_ror(e, 41)) +          \
             (g ^ (e & (f ^ g))) + rx_sha512_k[i] + w[i];                                     \
        d += h;                                                                               \
        h += (rx_sha512_ror(a, 28) ^ rx_sha512_ror(a, 34) ^ rx_sha512_ror(a, 39)) +          \
             ((a & b) | (c & (a | b)));                                                       \
    } while (0)
    for (int i = 0; i < 80; i += 8) {
        RX_SHA512_ROUND(a, b, c, d, e, f, g, h, i + 0);
        RX_SHA512_ROUND(h, a, b, c, d, e, f, g, i + 1);
        RX_SHA512_ROUND(g, h, a, b, c, d, e, f, i + 2);
        RX_SHA512_ROUND(f, g, h, a, b, c, d, e, i + 3);
        RX_SHA512_ROUND(e, f, g, h, a, b, c, d, i + 4);
        RX_SHA512_ROUND(d, e, f, g, h, a, b, c, i + 5);
        RX_SHA512_ROUND(c, d, e, f, g, h, a, b, i + 6);
        RX_SHA512_ROUND(b, c, d, e, f, g, h, a, i + 7);
    }
#undef RX_SHA512_ROUND
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
    st[5] += f;
    st[6] += g;
    st[7] += h;
}

static inline void rx_sha512_block(uint64_t st[8], const uint8_t *p)
{
    uint64_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = rx_rd_be64(p + 8 * i);
    rx_sha512_compress(st, m);
}

static inline void rx_sha512_init(RXSHA512 *s)
{
    *s = (RXSHA512) {
        .state = {
            0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
            0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
        },
    };
}

static inline void rx_sha512_update(RXSHA512 *s, const uint8_t *p, size_t n)
{
    size_t have = s->count % 128;
    s->count += n;
    if (have) {
        size_t take = n < 128 - have ? n : 128 - have;
        memcpy(s->buf + have, p, take);
        p += take;
        n -= take;
        if (have + take < 128)
            return;
        rx_sha512_block(s->state, s->buf);
    }
    for (; n >= 128; p += 128, n -= 128)
        rx_sha512_block(s->state, p);
    memcpy(s->buf, p, n);
}

/* The padding: 0x80, zeros, the length in bits as 128 bits big-endian. */
static inline void rx_sha512_final(RXSHA512 *s, uint8_t out[64])
{
    size_t have = s->count % 128;
    s->buf[have++] = 0x80;
    if (have > 112) {
        memset(s->buf + have, 0, 128 - have);
        rx_sha512_block(s->state, s->buf);
        have = 0;
    }
    memset(s->buf + have, 0, 112 - have);
    rx_sha512_wr_be64(s->buf + 112, s->count >> 61);
    rx_sha512_wr_be64(s->buf + 120, s->count << 3);
    rx_sha512_block(s->state, s->buf);
    for (int i = 0; i < 8; i++)
        rx_sha512_wr_be64(out + 8 * i, s->state[i]);
}

/* Keys longer than a block are hashed first (RFC 2104 2). Ready for a
 * message after this and after every final. */
static inline void rx_hmac_sha512_init(RXHMACSHA512 *h, const uint8_t *key, size_t len)
{
    uint8_t k[128] = { 0 }, pad[128];
    if (len > 128) {
        rx_sha512_init(&h->ctx);
        rx_sha512_update(&h->ctx, key, len);
        rx_sha512_final(&h->ctx, k);
    } else {
        memcpy(k, key, len);
    }
    for (int i = 0; i < 128; i++)
        pad[i] = k[i] ^ 0x36;
    rx_sha512_init(&h->inner);
    rx_sha512_update(&h->inner, pad, 128);
    for (int i = 0; i < 128; i++)
        pad[i] = k[i] ^ 0x5c;
    rx_sha512_init(&h->outer);
    rx_sha512_update(&h->outer, pad, 128);
    h->ctx = h->inner;
    memset_explicit(k, 0, sizeof(k));
    memset_explicit(pad, 0, sizeof(pad));
}

static inline void rx_hmac_sha512_update(RXHMACSHA512 *h, const uint8_t *p, size_t n)
{
    rx_sha512_update(&h->ctx, p, n);
}

static inline void rx_hmac_sha512_final(RXHMACSHA512 *h, uint8_t out[64])
{
    uint8_t in[64];
    rx_sha512_final(&h->ctx, in);
    h->ctx = h->outer;
    rx_sha512_update(&h->ctx, in, 64);
    rx_sha512_final(&h->ctx, out);
    h->ctx = h->inner;
}

/* PBKDF2 with HMAC-SHA512 as the PRF: block i of the key is
 * U_1 ^ ... ^ U_c, with U_1 = PRF(P, S || INT_BE(i)) and
 * U_j = PRF(P, U_j-1). From U_2 on each hash is the 128-byte pad block and
 * a 64-byte message, so its last block always has the same padding: the
 * words of U, 0x80, zeros, and the length, 192 bytes. */
static inline void rx_pbkdf2_sha512(const uint8_t *pass, size_t pass_len,
                                    const uint8_t *salt, size_t salt_len, unsigned iter,
                                    uint8_t *dk, size_t dk_len)
{
    RXHMACSHA512 h;
    rx_hmac_sha512_init(&h, pass, pass_len);
    uint64_t m[16] = { [8] = 1ull << 63, [15] = (128 + 64) * 8 };
    for (uint32_t i = 1; dk_len; i++) {
        uint8_t idx[4], u[64];
        rx_wr_be32(idx, i);
        rx_hmac_sha512_update(&h, salt, salt_len);
        rx_hmac_sha512_update(&h, idx, sizeof(idx));
        rx_hmac_sha512_final(&h, u);

        uint64_t t[8];
        for (int k = 0; k < 8; k++)
            t[k] = m[k] = rx_rd_be64(u + 8 * k);
        for (unsigned j = 1; j < iter; j++) {
            uint64_t st[8];
            memcpy(st, h.inner.state, sizeof(st));
            rx_sha512_compress(st, m);
            memcpy(m, st, sizeof(st));
            memcpy(st, h.outer.state, sizeof(st));
            rx_sha512_compress(st, m);
            for (int k = 0; k < 8; k++)
                t[k] ^= m[k] = st[k];
        }

        for (int k = 0; k < 8; k++)
            rx_sha512_wr_be64(u + 8 * k, t[k]);
        size_t n = dk_len < 64 ? dk_len : 64;
        memcpy(dk, u, n);
        dk += n;
        dk_len -= n;
        memset_explicit(u, 0, sizeof(u));
        memset_explicit(t, 0, sizeof(t));
    }
    memset_explicit(m, 0, sizeof(m));
    memset_explicit(&h, 0, sizeof(h));
}

#endif /* RXBOX_FORMATS_SHA512_H */
