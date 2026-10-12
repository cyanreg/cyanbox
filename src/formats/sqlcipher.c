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
 * (rekordbox leaves a 3-frame WAL next to its database). */

#include "rxbox/formats.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"

#define PAGE    RX_SQLCIPHER_PAGE
#define RESERVE RX_SQLCIPHER_RESERVE
#define IV      16
#define SALT    16
#define KDF_ITER 256000

static const uint8_t sqlite_magic[16] = "SQLite format 3";

static int derive_keys(const char *passphrase, const uint8_t salt[SALT], uint8_t key[32], uint8_t mac[32])
{
    uint8_t mac_salt[SALT];
    for (int i = 0; i < SALT; i++)
        mac_salt[i] = salt[i] ^ 0x3A;
    if (!PKCS5_PBKDF2_HMAC(passphrase, (int)strlen(passphrase), salt, SALT, KDF_ITER,
                           EVP_sha512(), 32, key) ||
        !PKCS5_PBKDF2_HMAC((const char *)key, 32, mac_salt, SALT, 2, EVP_sha512(), 32, mac))
        return RX_ERROR(EIO);
    return 0;
}

/* HMAC-SHA512(mac_key, data || pgno LE). */
static int page_hmac(const uint8_t mac[32], const uint8_t *data, size_t n, uint32_t pgno, uint8_t out[64])
{
    uint8_t buf[PAGE + 4];
    memcpy(buf, data, n);
    rx_wr_le32(buf + n, pgno);
    unsigned len = 64;
    if (!HMAC(EVP_sha512(), mac, 32, buf, n + 4, out, &len) || len != 64)
        return RX_ERROR(EIO);
    return 0;
}

static int aes_cbc(const uint8_t key[32], const uint8_t iv[IV], const uint8_t *in, size_t n,
                   uint8_t *out, bool encrypt)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return RX_ERROR(ENOMEM);
    int ret = 0, len = 0, fin = 0;
    if (!EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, encrypt ? 1 : 0) ||
        !EVP_CIPHER_CTX_set_padding(ctx, 0) ||
        !EVP_CipherUpdate(ctx, out, &len, in, (int)n) ||
        !EVP_CipherFinal_ex(ctx, out + len, &fin) || (size_t)(len + fin) != n)
        ret = RX_ERROR(EIO);
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}

/* Decrypts one page into `out` (PAGE bytes): the SQLite magic for page 1,
 * the plaintext, zeros in the reserve. -EBADMSG on an HMAC mismatch
 * (wrong key or a corrupt file). */
static int decrypt_page(const uint8_t *page, uint32_t pgno, const uint8_t key[32], const uint8_t mac[32],
                        uint8_t *out)
{
    size_t off = pgno == 1 ? SALT : 0;
    const uint8_t *body = page + off;
    size_t n = PAGE - RESERVE - off;
    const uint8_t *iv = page + PAGE - RESERVE;
    uint8_t want[64], tmp[PAGE];
    memcpy(tmp, body, n);
    memcpy(tmp + n, iv, IV);
    int ret = page_hmac(mac, tmp, n + IV, pgno, want);
    if (ret < 0)
        return ret;
    if (CRYPTO_memcmp(want, page + PAGE - RESERVE + IV, 64))
        return RX_ERROR(EBADMSG);
    memset(out, 0, PAGE);
    if (pgno == 1)
        memcpy(out, sqlite_magic, 16);
    return aes_cbc(key, iv, body, n, out + off, false);
}

int rx_sqlcipher_decrypt(const uint8_t *data, size_t size, const char *passphrase,
                         const uint8_t *wal, size_t wal_size, uint8_t **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0;
    if (size < PAGE)
        return RX_ERROR(EINVAL);
    uint8_t key[32], mac[32];
    int ret = derive_keys(passphrase, data, key, mac);
    if (ret < 0)
        return ret;

    size_t nb = size / PAGE;
    uint8_t *pages = malloc(nb * PAGE);
    if (!pages)
        return RX_ERROR(ENOMEM);
    for (size_t i = 0; i < nb; i++)
        if ((ret = decrypt_page(data + i * PAGE, (uint32_t)i + 1, key, mac, pages + i * PAGE)) < 0)
            goto fail;

    /* WAL: a 32-byte header, then frames of a 24-byte header (page number
     * and, on a commit frame, the database size in pages, big-endian) and
     * a page. Frames apply when their transaction commits. */
    if (wal && wal_size > 32) {
        typedef struct Pending { uint32_t pgno; uint8_t page[PAGE]; } Pending;
        RxArray pending;
        rx_array_init(&pending, sizeof(Pending));
        size_t pos = 32;
        while (pos + 24 + PAGE <= wal_size) {
            uint32_t pgno = rx_rd_be32(wal + pos), commit = rx_rd_be32(wal + pos + 4);
            Pending *p = NULL;
            for (size_t k = 0; k < pending.nb; k++)
                if (((Pending *)rx_array_at(&pending, k))->pgno == pgno)
                    p = rx_array_at(&pending, k);
            if (!p && !(p = rx_array_push(&pending, NULL))) {
                ret = RX_ERROR(ENOMEM);
                rx_array_free(&pending);
                goto fail;
            }
            p->pgno = pgno;
            if (!pgno || (ret = decrypt_page(wal + pos + 24, pgno, key, mac, p->page)) < 0) {
                ret = ret < 0 ? ret : RX_ERROR(EINVAL);
                rx_array_free(&pending);
                goto fail;
            }
            pos += 24 + PAGE;
            if (!commit)
                continue;
            for (size_t k = 0; k < pending.nb; k++) {
                Pending *pg = rx_array_at(&pending, k);
                if (pg->pgno > nb) {
                    uint8_t *np = realloc(pages, (size_t)pg->pgno * PAGE);
                    if (!np) {
                        ret = RX_ERROR(ENOMEM);
                        rx_array_free(&pending);
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
            rx_array_clear(&pending);
        }
        rx_array_free(&pending);
    }
    *out = pages;
    *out_size = nb * PAGE;
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(mac, sizeof(mac));
    return 0;

fail:
    free(pages);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(mac, sizeof(mac));
    return ret;
}

int rx_sqlcipher_encrypt(const uint8_t *plain, size_t size, const char *passphrase,
                         const uint8_t salt_in[16], uint8_t **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0;
    if (size < 100 || memcmp(plain, sqlite_magic, 16))
        return RX_ERROR(EINVAL);
    /* The plaintext must already reserve the IV and HMAC on every page:
     * SQLite keeps the reserve of the file it was given (dj). */
    if (rx_rd_be16(plain + 16) != PAGE || plain[20] != RESERVE || size % PAGE)
        return RX_ERROR(EINVAL);

    uint8_t salt[SALT];
    if (salt_in)
        memcpy(salt, salt_in, SALT);
    else if (RAND_bytes(salt, SALT) != 1)
        return RX_ERROR(EIO);
    uint8_t key[32], mac[32];
    int ret = derive_keys(passphrase, salt, key, mac);
    if (ret < 0)
        return ret;

    uint8_t *enc = malloc(size);
    if (!enc)
        return RX_ERROR(ENOMEM);
    for (size_t i = 0; i < size / PAGE; i++) {
        const uint8_t *page = plain + i * PAGE;
        uint8_t *dst = enc + i * PAGE;
        size_t off = i == 0 ? SALT : 0;
        size_t n = PAGE - RESERVE - off;
        uint8_t *iv = dst + PAGE - RESERVE;
        if (RAND_bytes(iv, IV) != 1) {
            ret = RX_ERROR(EIO);
            break;
        }
        if (i == 0)
            memcpy(dst, salt, SALT);
        if ((ret = aes_cbc(key, iv, page + off, n, dst + off, true)) < 0)
            break;
        /* The HMAC covers ciphertext and IV, which sit next to each other. */
        if ((ret = page_hmac(mac, dst + off, n + IV, (uint32_t)i + 1, dst + PAGE - RESERVE + IV)) < 0)
            break;
    }
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(mac, sizeof(mac));
    if (ret < 0) {
        free(enc);
        return ret;
    }
    *out = enc;
    *out_size = size;
    return 0;
}
