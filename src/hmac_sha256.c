/**
 * @file hmac_sha256.c
 * @brief Standalone SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104)
 *
 * Zero external dependencies. Suitable for embedded targets without OpenSSL.
 */

#define _DEFAULT_SOURCE  /* for explicit_bzero */

#include "hmac_sha256.h"
#include <string.h>

/* Torizon/musl may lack explicit_bzero — provide a safe fallback */
#ifdef PLATFORM_TORIZON
static void secure_zero(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--) *p++ = 0;
}
#define explicit_bzero(buf, len) secure_zero(buf, len)
#endif

/* ============================================================================
 * SHA-256 Constants (FIPS 180-4 Section 4.2.2)
 * ========================================================================== */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* ============================================================================
 * SHA-256 Helper Macros
 * ========================================================================== */

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/* ============================================================================
 * SHA-256 Context
 * ========================================================================== */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[SHA256_BLOCK_SIZE];
    uint32_t buflen;
} sha256_ctx_t;

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
    ctx->buflen = 0;
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t block[SHA256_BLOCK_SIZE])
{
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;

    /* Prepare message schedule */
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[i * 4    ] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] <<  8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        W[i] = SIG1(W[i - 2]) + W[i - 7] + SIG0(W[i - 15]) + W[i - 16];
    }

    /* Initialize working variables */
    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    /* 64 rounds */
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + W[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->bitcount += (uint64_t)len * 8;

    while (len > 0) {
        uint32_t space = SHA256_BLOCK_SIZE - ctx->buflen;
        uint32_t copy = (len < space) ? (uint32_t)len : space;

        memcpy(ctx->buffer + ctx->buflen, data, copy);
        ctx->buflen += copy;
        data += copy;
        len -= copy;

        if (ctx->buflen == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE])
{
    /* Pad: append 0x80, then zeros, then 64-bit big-endian bit count */
    ctx->buffer[ctx->buflen++] = 0x80;

    if (ctx->buflen > 56) {
        /* Not enough room for length — fill this block and process */
        memset(ctx->buffer + ctx->buflen, 0, SHA256_BLOCK_SIZE - ctx->buflen);
        sha256_transform(ctx, ctx->buffer);
        ctx->buflen = 0;
    }

    memset(ctx->buffer + ctx->buflen, 0, 56 - ctx->buflen);

    /* Append bit count as big-endian 64-bit */
    for (int i = 7; i >= 0; i--) {
        ctx->buffer[56 + (7 - i)] = (uint8_t)(ctx->bitcount >> (i * 8));
    }

    sha256_transform(ctx, ctx->buffer);

    /* Output hash as big-endian */
    for (int i = 0; i < 8; i++) {
        out[i * 4    ] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >>  8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* ============================================================================
 * Public API: SHA-256
 * ========================================================================== */

void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ============================================================================
 * Public API: HMAC-SHA256 (RFC 2104)
 * ========================================================================== */

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t k_pad[SHA256_BLOCK_SIZE];
    uint8_t key_hash[SHA256_DIGEST_SIZE];

    /* If key > block size, hash it first */
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, key_hash);
        key = key_hash;
        key_len = SHA256_DIGEST_SIZE;
    }

    /* Prepare padded key */
    memset(k_pad, 0, SHA256_BLOCK_SIZE);
    memcpy(k_pad, key, key_len);

    /* Inner hash: SHA256((K ^ ipad) || data) */
    uint8_t inner_key[SHA256_BLOCK_SIZE];
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++)
        inner_key[i] = k_pad[i] ^ 0x36;

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, inner_key, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, data, data_len);
    uint8_t inner_hash[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, inner_hash);

    /* Outer hash: SHA256((K ^ opad) || inner_hash) */
    uint8_t outer_key[SHA256_BLOCK_SIZE];
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++)
        outer_key[i] = k_pad[i] ^ 0x5c;

    sha256_init(&ctx);
    sha256_update(&ctx, outer_key, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, inner_hash, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, out);

    /* Clear key-derived material from stack */
    explicit_bzero(k_pad, sizeof(k_pad));
    explicit_bzero(key_hash, sizeof(key_hash));
    explicit_bzero(inner_key, sizeof(inner_key));
    explicit_bzero(inner_hash, sizeof(inner_hash));
    explicit_bzero(outer_key, sizeof(outer_key));
}

/* ============================================================================
 * Public API: Constant-Time Comparison
 * ========================================================================== */

int hmac_sha256_compare(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t result = 0;
    for (size_t i = 0; i < len; i++)
        result |= a[i] ^ b[i];
    return (int)result;
}
