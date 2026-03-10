/**
 * @file hmac_sha256.h
 * @brief Standalone SHA-256 and HMAC-SHA256 (no OpenSSL dependency)
 *
 * SHA-256: FIPS 180-4
 * HMAC:    RFC 2104
 */

#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_SIZE  32
#define SHA256_BLOCK_SIZE   64

/**
 * @brief Compute SHA-256 hash
 * @param data   Input data
 * @param len    Input length in bytes
 * @param out    Output buffer (32 bytes)
 */
void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * @brief Compute HMAC-SHA256
 * @param key       HMAC key
 * @param key_len   Key length in bytes
 * @param data      Input data
 * @param data_len  Data length in bytes
 * @param out       Output buffer (32 bytes)
 */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * @brief Constant-time comparison (prevents timing side-channel)
 * @return 0 if equal, non-zero if different
 */
int hmac_sha256_compare(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HMAC_SHA256_H */
