/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A minimal SQLCipher 4 page codec with the default settings, for
 * exportLibrary.db; a port of dj's formats/sqlcipher.py, parameters in
 * docs/onelibrary.md 1.1 (confirmed there by decrypting rekordbox's files
 * with every page HMAC verifying):
 *
 *   4096-byte pages, 80 reserved bytes each (16 IV + 64 HMAC);
 *   AES-256-CBC with a fresh random IV per page, no padding;
 *   HMAC-SHA512 over ciphertext + IV + page number (u32 LE, 1-based);
 *   key = PBKDF2-HMAC-SHA512(passphrase, salt, 256000, 32 bytes);
 *   HMAC key = PBKDF2-HMAC-SHA512(key, salt XOR 0x3A, 2, 32 bytes);
 *   page 1 starts with the 16-byte salt in place of "SQLite format 3\0".
 *
 * Decryption applies the committed frames of a -wal file the way dj does
 * (rekordbox leaves a 3-frame WAL next to its database), with SQLite's own
 * limits, which dj did not check: only frames carrying the WAL header's
 * salts and authenticating count, the first that does not ends the log
 * (a frame of an older generation, or one torn by an unplugged stick), and
 * a page number must lie within what the file and the log can hold.
 *
 * Nothing here links a libcrypto of its own; libavformat already brings
 * in two (its own and libcurl's). AES is libavutil's; SHA-512, HMAC and
 * PBKDF2 are sha512.h's, which keeps the HMAC pad states so the 256000
 * rounds of the key derivation cost two compressions each. */

#include "cyanbox/formats.h"

#include <sys/random.h>

#include <libavutil/aes.h>
#include <libavutil/mem.h>

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "src/formats/sha512.h"

#define PAGE    CB_SQLCIPHER_PAGE
#define RESERVE CB_SQLCIPHER_RESERVE
#define IV      16
#define SALT    16
#define KDF_ITER 256000

static const uint8_t sqlite_magic[16] = "SQLite format 3";

/* The keys of one database: the AES-256 schedule for one direction and the
 * HMAC keyed with the page-HMAC key. */
typedef struct Codec {
    struct AVAES *aes;
    CBHMACSHA512 hmac;
    bool decrypt;
} Codec;

static void codec_free(Codec *c)
{
    if (c->aes)
        memset_explicit(c->aes, 0, av_aes_size);
    av_freep(&c->aes);
    memset_explicit(c, 0, sizeof(*c));
}

/* key = PBKDF2(passphrase, salt, 256000), HMAC key = PBKDF2(key, salt ^ 0x3A, 2),
 * both 32 bytes (SQLCipher 4's defaults, docs/onelibrary.md 1.1). */
static int codec_init(Codec *c, const char *passphrase, const uint8_t salt[SALT], bool decrypt)
{
    *c = (Codec) { .decrypt = decrypt };
    c->aes = av_aes_alloc();
    if (!c->aes)
        return CB_ERROR(ENOMEM);

    uint8_t key[32], mac[32], mac_salt[SALT];
    for (int i = 0; i < SALT; i++)
        mac_salt[i] = salt[i] ^ 0x3A;
    cb_pbkdf2_sha512((const uint8_t *)passphrase, strlen(passphrase), salt, SALT, KDF_ITER,
                     key, sizeof(key));
    cb_pbkdf2_sha512(key, sizeof(key), mac_salt, SALT, 2, mac, sizeof(mac));
    cb_hmac_sha512_init(&c->hmac, mac, sizeof(mac));
    int ret = av_aes_init(c->aes, key, 256, decrypt);
    memset_explicit(key, 0, sizeof(key));
    memset_explicit(mac, 0, sizeof(mac));
    if (ret < 0) {
        codec_free(c);
        return CB_ERROR(EIO);
    }
    return 0;
}

/* HMAC-SHA512(mac key, data || pgno LE). */
static void page_hmac(Codec *c, const uint8_t *data, size_t n, uint32_t pgno, uint8_t out[64])
{
    uint8_t le[4];
    cb_wr_le32(le, pgno);
    cb_hmac_sha512_update(&c->hmac, data, n);
    cb_hmac_sha512_update(&c->hmac, le, sizeof(le));
    cb_hmac_sha512_final(&c->hmac, out);
}

/* AES-256-CBC over whole blocks (a page body is 4016 or 4000 bytes, no
 * padding). av_aes_crypt advances the IV it is given, so it gets a copy. */
static void aes_cbc(Codec *c, const uint8_t iv_in[IV], const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t iv[IV];
    memcpy(iv, iv_in, IV);
    av_aes_crypt(c->aes, out, in, (int)(n / 16), iv, c->decrypt);
}

/* Constant time, as an HMAC comparison should be: how much of a forged
 * page's tag matches must not show in how long its refusal takes. */
static bool differ(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= a[i] ^ b[i];
    return d;
}

/* Salts and IVs from the kernel's CSPRNG. getrandom(2) blocks only until
 * the pool is seeded, a wait a signal can interrupt; after that a request
 * of up to 256 bytes is never short. */
static int random_bytes(uint8_t *buf, size_t n)
{
    while (n) {
        ssize_t got = getrandom(buf, n, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return CB_ERROR(errno);
        }
        buf += got;
        n -= (size_t)got;
    }
    return 0;
}

/* Decrypts one page into `out` (PAGE bytes): the SQLite magic for page 1,
 * the plaintext, zeros in the reserve. -EBADMSG on an HMAC mismatch
 * (wrong key or a corrupt file). */
static int decrypt_page(Codec *c, const uint8_t *page, uint32_t pgno, uint8_t *out)
{
    size_t off = pgno == 1 ? SALT : 0;
    const uint8_t *body = page + off;
    size_t n = PAGE - RESERVE - off;
    const uint8_t *iv = page + PAGE - RESERVE;
    /* The HMAC covers ciphertext and IV, which sit next to each other. */
    uint8_t want[64];
    page_hmac(c, body, n + IV, pgno, want);
    if (differ(want, iv + IV, sizeof(want)))
        return CB_ERROR(EBADMSG);
    memset(out, 0, PAGE);
    if (pgno == 1)
        memcpy(out, sqlite_magic, 16);
    aes_cbc(c, iv, body, n, out + off);
    return 0;
}

int cb_sqlcipher_decrypt(const uint8_t *data, size_t size, const char *passphrase,
                         const uint8_t *wal, size_t wal_size, uint8_t **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0;
    if (size < PAGE)
        return CB_ERROR(EINVAL);
    Codec c;
    int ret = codec_init(&c, passphrase, data, true);
    if (ret < 0)
        return ret;

    size_t nb = size / PAGE;
    uint8_t *pages = malloc(nb * PAGE);
    if (!pages) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    for (size_t i = 0; i < nb; i++)
        if ((ret = decrypt_page(&c, data + i * PAGE, (uint32_t)i + 1, pages + i * PAGE)) < 0)
            goto fail;

    /* WAL: a 32-byte header, then frames of a 24-byte header (page number
     * and, on a commit frame, the database size in pages, big-endian) and
     * a page. Frames apply when their transaction commits. */
    if (wal && wal_size > 32) {
        typedef struct Pending { uint32_t pgno; uint8_t page[PAGE]; } Pending;
        CBArray pending;
        cb_array_init(&pending, sizeof(Pending));
        /* Every page past the end of the file is in some frame. */
        size_t max_pgno = nb + (wal_size - 32) / (24 + PAGE);
        size_t pos = 32;
        while (pos + 24 + PAGE <= wal_size) {
            uint32_t pgno = cb_rd_be32(wal + pos), commit = cb_rd_be32(wal + pos + 4);
            if (!pgno || pgno > max_pgno || memcmp(wal + pos + 8, wal + 16, 8))
                break;
            Pending *p = NULL;
            for (size_t k = 0; k < pending.nb; k++)
                if (((Pending *)cb_array_at(&pending, k))->pgno == pgno)
                    p = cb_array_at(&pending, k);
            uint8_t page[PAGE];
            if (decrypt_page(&c, wal + pos + 24, pgno, page) < 0)
                break;
            if (!p && !(p = cb_array_push(&pending, NULL))) {
                ret = CB_ERROR(ENOMEM);
                cb_array_free(&pending);
                goto fail;
            }
            p->pgno = pgno;
            memcpy(p->page, page, PAGE);
            pos += 24 + PAGE;
            if (!commit)
                continue;
            for (size_t k = 0; k < pending.nb; k++) {
                Pending *pg = cb_array_at(&pending, k);
                if (pg->pgno > nb) {
                    uint8_t *np = realloc(pages, (size_t)pg->pgno * PAGE);
                    if (!np) {
                        ret = CB_ERROR(ENOMEM);
                        cb_array_free(&pending);
                        goto fail;
                    }
                    memset(np + nb * PAGE, 0, ((size_t)pg->pgno - nb) * PAGE);
                    pages = np;
                    nb = pg->pgno;
                }
                memcpy(pages + ((size_t)pg->pgno - 1) * PAGE, pg->page, PAGE);
            }
            if (commit < nb)
                nb = commit;
            cb_array_clear(&pending);
        }
        cb_array_free(&pending);
    }
    *out = pages;
    *out_size = nb * PAGE;
    codec_free(&c);
    return 0;

fail:
    free(pages);
    codec_free(&c);
    return ret;
}

int cb_sqlcipher_encrypt(const uint8_t *plain, size_t size, const char *passphrase,
                         const uint8_t salt_in[16], uint8_t **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0;
    if (size < 100 || memcmp(plain, sqlite_magic, 16))
        return CB_ERROR(EINVAL);
    /* The plaintext must already reserve the IV and HMAC on every page:
     * SQLite keeps the reserve of the file it was given (dj). */
    if (cb_rd_be16(plain + 16) != PAGE || plain[20] != RESERVE || size % PAGE)
        return CB_ERROR(EINVAL);

    uint8_t salt[SALT];
    int ret = 0;
    if (salt_in)
        memcpy(salt, salt_in, SALT);
    else if ((ret = random_bytes(salt, SALT)) < 0)
        return ret;
    Codec c;
    if ((ret = codec_init(&c, passphrase, salt, false)) < 0)
        return ret;

    uint8_t *enc = malloc(size);
    if (!enc) {
        codec_free(&c);
        return CB_ERROR(ENOMEM);
    }
    for (size_t i = 0; i < size / PAGE; i++) {
        const uint8_t *page = plain + i * PAGE;
        uint8_t *dst = enc + i * PAGE;
        size_t off = i == 0 ? SALT : 0;
        size_t n = PAGE - RESERVE - off;
        uint8_t *iv = dst + PAGE - RESERVE;
        if ((ret = random_bytes(iv, IV)) < 0)
            break;
        if (i == 0)
            memcpy(dst, salt, SALT);
        aes_cbc(&c, iv, page + off, n, dst + off);
        /* The HMAC covers ciphertext and IV, which sit next to each other. */
        page_hmac(&c, dst + off, n + IV, (uint32_t)i + 1, dst + PAGE - RESERVE + IV);
    }
    codec_free(&c);
    if (ret < 0) {
        free(enc);
        return ret;
    }
    *out = enc;
    *out_size = size;
    return 0;
}
