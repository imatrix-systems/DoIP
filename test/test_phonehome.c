/**
 * @file test_phonehome.c
 * @brief Unit tests for phone-home handler and HMAC-SHA256
 *
 * Tests:
 *   UT-00: HMAC-SHA256 against RFC 4231 test vectors
 *   UT-01: Valid HMAC → positive response
 *   UT-02: Invalid HMAC → NRC 0x35
 *   UT-03: Replay (same nonce twice) → NRC 0x24
 *   UT-04: Short PDU → NRC 0x13
 *   UT-05: HMAC secret not loaded → NRC 0x22
 *
 * Usage: ./test-phonehome
 */

#include "hmac_sha256.h"
#include "phonehome_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ============================================================================
 * Test Framework
 * ========================================================================== */

static int g_passed = 0;
static int g_failed = 0;

#define TEST_START(name) \
    printf("\n--- Test %d: %s ---\n", g_passed + g_failed + 1, (name))

#define TEST_PASS(name) do { \
    printf("  PASS: %s\n", (name)); \
    g_passed++; \
} while (0)

#define TEST_FAIL(name, ...) do { \
    printf("  FAIL: %s — ", (name)); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    g_failed++; \
} while (0)

/* ============================================================================
 * Helpers
 * ========================================================================== */

static void hex_to_bytes(const char *hex, uint8_t *out, size_t max_len, size_t *out_len)
{
    *out_len = 0;
    while (*hex && *(hex + 1) && *out_len < max_len) {
        unsigned int byte;
        sscanf(hex, "%2x", &byte);
        out[(*out_len)++] = (uint8_t)byte;
        hex += 2;
    }
}

static void print_hex(const char *label, const uint8_t *data, size_t len)
{
    printf("  %s: ", label);
    for (size_t i = 0; i < len; i++)
        printf("%02x", data[i]);
    printf("\n");
}

/* ============================================================================
 * UT-00: HMAC-SHA256 RFC 4231 Test Vectors
 * ========================================================================== */

static void test_hmac_sha256_vectors(void)
{
    TEST_START("HMAC-SHA256 RFC 4231 vectors");

    /* Test Case 1: key = 20 bytes of 0x0b */
    {
        uint8_t key[20];
        memset(key, 0x0b, 20);
        const char *data_str = "Hi There";
        uint8_t expected[32];
        size_t exp_len;
        hex_to_bytes("b0344c61d8db38535ca8afceaf0bf12b"
                     "881dc200c9833da726e9376c2e32cff7",
                     expected, sizeof(expected), &exp_len);

        uint8_t result[32];
        hmac_sha256(key, 20, (const uint8_t *)data_str, 8, result);

        if (hmac_sha256_compare(result, expected, 32) != 0) {
            print_hex("expected", expected, 32);
            print_hex("got     ", result, 32);
            TEST_FAIL("RFC 4231 TC1", "HMAC mismatch");
            return;
        }
    }

    /* Test Case 2: key = "Jefe", data = "what do ya want for nothing?" */
    {
        const char *key_str = "Jefe";
        const char *data_str = "what do ya want for nothing?";
        uint8_t expected[32];
        size_t exp_len;
        hex_to_bytes("5bdcc146bf60754e6a042426089575c7"
                     "5a003f089d2739839dec58b964ec3843",
                     expected, sizeof(expected), &exp_len);

        uint8_t result[32];
        hmac_sha256((const uint8_t *)key_str, 4,
                    (const uint8_t *)data_str, 28, result);

        if (hmac_sha256_compare(result, expected, 32) != 0) {
            print_hex("expected", expected, 32);
            print_hex("got     ", result, 32);
            TEST_FAIL("RFC 4231 TC2", "HMAC mismatch");
            return;
        }
    }

    /* Test Case 3: key = 20 bytes of 0xaa, data = 50 bytes of 0xdd */
    {
        uint8_t key[20], data[50];
        memset(key, 0xaa, 20);
        memset(data, 0xdd, 50);
        uint8_t expected[32];
        size_t exp_len;
        hex_to_bytes("773ea91e36800e46854db8ebd09181a7"
                     "2959098b3ef8c122d9635514ced565fe",
                     expected, sizeof(expected), &exp_len);

        uint8_t result[32];
        hmac_sha256(key, 20, data, 50, result);

        if (hmac_sha256_compare(result, expected, 32) != 0) {
            print_hex("expected", expected, 32);
            print_hex("got     ", result, 32);
            TEST_FAIL("RFC 4231 TC3", "HMAC mismatch");
            return;
        }
    }

    TEST_PASS("RFC 4231 test vectors (TC1-TC3)");
}

/* ============================================================================
 * UT-00b: Constant-time comparison
 * ========================================================================== */

static void test_constant_time_compare(void)
{
    TEST_START("Constant-time comparison");

    uint8_t a[32], b[32];
    memset(a, 0x42, 32);
    memcpy(b, a, 32);

    if (hmac_sha256_compare(a, b, 32) != 0) {
        TEST_FAIL("compare equal", "should return 0");
        return;
    }

    b[31] ^= 0x01;
    if (hmac_sha256_compare(a, b, 32) == 0) {
        TEST_FAIL("compare different", "should return non-zero");
        return;
    }

    TEST_PASS("Constant-time comparison");
}

/* ============================================================================
 * Shared test setup: create temp HMAC secret and config
 * ========================================================================== */

static uint8_t test_secret[32];
static char secret_path[256];
static char config_path[256];
static char lock_path[256];
static phonehome_config_t test_cfg;

static int setup_test_env(void)
{
    /* Generate a deterministic test secret */
    for (int i = 0; i < 32; i++)
        test_secret[i] = (uint8_t)(0xA0 + i);

    /* Write secret to temp file */
    snprintf(secret_path, sizeof(secret_path), "/tmp/test_phonehome_secret_%d", getpid());
    FILE *f = fopen(secret_path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot create test secret file\n");
        return -1;
    }
    fwrite(test_secret, 1, 32, f);
    fclose(f);
    chmod(secret_path, 0600);

    /* Write config file */
    snprintf(config_path, sizeof(config_path), "/tmp/test_phonehome_conf_%d", getpid());
    snprintf(lock_path, sizeof(lock_path), "/tmp/test_phonehome_lock_%d", getpid());
    f = fopen(config_path, "w");
    if (!f) {
        fprintf(stderr, "Cannot create test config file\n");
        return -1;
    }
    fprintf(f, "BASTION_HOST=test.bastion.local\n");
    fprintf(f, "HMAC_SECRET_FILE=%s\n", secret_path);
    fprintf(f, "CONNECT_SCRIPT=/bin/true\n");
    fprintf(f, "LOCK_FILE=%s\n", lock_path);
    fprintf(f, "BASTION_CLIENT_KEY=ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITestKeyForUnitTests000000000000000000000 test\n");
    fprintf(f, "SSH_CA_PUBKEY=ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITestCAKeyForUnitTests00000000000000000000 test-ca\n");
    fclose(f);

    return 0;
}

static void cleanup_test_env(void)
{
    unlink(secret_path);
    unlink(config_path);
    unlink(lock_path);
}

/** Build a valid phone-home PDU with correct HMAC */
static int build_valid_pdu(const uint8_t nonce[8], uint8_t *pdu, size_t pdu_size)
{
    if (pdu_size < 44) return -1;

    pdu[0] = 0x31;     /* SID: RoutineControl */
    pdu[1] = 0x01;     /* subFunction: startRoutine */
    pdu[2] = 0xF0;     /* routineIdentifier high */
    pdu[3] = 0xA0;     /* routineIdentifier low */

    memcpy(pdu + 4, nonce, 8);

    /* Compute valid HMAC */
    hmac_sha256(test_secret, 32, nonce, 8, pdu + 12);

    return 44;
}

/* ============================================================================
 * UT-01: Valid HMAC → positive response
 * ========================================================================== */

static void test_valid_hmac(void)
{
    TEST_START("Valid HMAC → positive response");

    uint8_t pdu[64];
    uint8_t nonce[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    int pdu_len = build_valid_pdu(nonce, pdu, sizeof(pdu));

    uint8_t resp[64];
    int resp_len = phonehome_handle_routine(pdu, (uint32_t)pdu_len, resp, sizeof(resp));

    if (resp_len != 5) {
        TEST_FAIL("response length", "expected 5, got %d", resp_len);
        return;
    }
    if (resp[0] != 0x71 || resp[1] != 0x01 || resp[2] != 0xF0 ||
        resp[3] != 0xA0 || resp[4] != 0x02) {
        print_hex("response", resp, (size_t)resp_len);
        TEST_FAIL("response content", "expected {71 01 F0 A0 02}");
        return;
    }

    /* Clean up lock file created by successful trigger */
    unlink(lock_path);

    TEST_PASS("Valid HMAC → positive response {71 01 F0 A0 02}");
}

/* ============================================================================
 * UT-02: Invalid HMAC → NRC 0x35
 * ========================================================================== */

static void test_invalid_hmac(void)
{
    TEST_START("Invalid HMAC → NRC 0x35");

    uint8_t pdu[64];
    uint8_t nonce[8] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
    int pdu_len = build_valid_pdu(nonce, pdu, sizeof(pdu));

    /* Corrupt HMAC */
    pdu[12] ^= 0xFF;
    pdu[13] ^= 0xFF;

    uint8_t resp[64];
    int resp_len = phonehome_handle_routine(pdu, (uint32_t)pdu_len, resp, sizeof(resp));

    if (resp_len != 3 || resp[0] != 0x7F || resp[1] != 0x31 || resp[2] != 0x35) {
        print_hex("response", resp, resp_len > 0 ? (size_t)resp_len : 0);
        TEST_FAIL("NRC 0x35", "expected {7F 31 35}, got len=%d", resp_len);
        return;
    }

    TEST_PASS("Invalid HMAC → NRC {7F 31 35}");
}

/* ============================================================================
 * UT-03: Replay (same nonce twice) → NRC 0x24
 * ========================================================================== */

static void test_replay_nonce(void)
{
    TEST_START("Replay nonce → NRC 0x24");

    uint8_t pdu[64];
    uint8_t nonce[8] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    int pdu_len = build_valid_pdu(nonce, pdu, sizeof(pdu));

    uint8_t resp[64];

    /* First call should succeed */
    int resp_len = phonehome_handle_routine(pdu, (uint32_t)pdu_len, resp, sizeof(resp));
    if (resp_len != 5 || resp[0] != 0x71) {
        TEST_FAIL("first call", "expected positive response, got len=%d", resp_len);
        return;
    }

    /* Clean up lock file from first call */
    unlink(lock_path);

    /* Second call with same nonce should return replay NRC */
    resp_len = phonehome_handle_routine(pdu, (uint32_t)pdu_len, resp, sizeof(resp));
    if (resp_len != 3 || resp[0] != 0x7F || resp[1] != 0x31 || resp[2] != 0x24) {
        print_hex("response", resp, resp_len > 0 ? (size_t)resp_len : 0);
        TEST_FAIL("replay NRC", "expected {7F 31 24}, got len=%d", resp_len);
        return;
    }

    TEST_PASS("Replay nonce → NRC {7F 31 24}");
}

/* ============================================================================
 * UT-04: Short PDU → NRC 0x13
 * ========================================================================== */

static void test_short_pdu(void)
{
    TEST_START("Short PDU → NRC 0x13");

    /* Send only 10 bytes (minimum is 44) */
    uint8_t pdu[10] = {0x31, 0x01, 0xF0, 0xA0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

    uint8_t resp[64];
    int resp_len = phonehome_handle_routine(pdu, 10, resp, sizeof(resp));

    if (resp_len != 3 || resp[0] != 0x7F || resp[1] != 0x31 || resp[2] != 0x13) {
        print_hex("response", resp, resp_len > 0 ? (size_t)resp_len : 0);
        TEST_FAIL("NRC 0x13", "expected {7F 31 13}, got len=%d", resp_len);
        return;
    }

    TEST_PASS("Short PDU → NRC {7F 31 13}");
}

/* ============================================================================
 * UT-05: HMAC secret not loaded → NRC 0x22
 * ========================================================================== */

static void test_not_initialized(void)
{
    TEST_START("HMAC not loaded → NRC 0x22");

    /* Shutdown to clear HMAC state */
    phonehome_shutdown();

    uint8_t pdu[44] = {0x31, 0x01, 0xF0, 0xA0};
    uint8_t resp[64];
    int resp_len = phonehome_handle_routine(pdu, 44, resp, sizeof(resp));

    if (resp_len != 3 || resp[0] != 0x7F || resp[1] != 0x31 || resp[2] != 0x22) {
        print_hex("response", resp, resp_len > 0 ? (size_t)resp_len : 0);
        TEST_FAIL("NRC 0x22", "expected {7F 31 22}, got len=%d", resp_len);
        return;
    }

    TEST_PASS("HMAC not loaded → NRC {7F 31 22}");
}

/* ============================================================================
 * Main
 * ========================================================================== */

int main(void)
{
    printf("=== Phone-Home Unit Tests ===\n");

    /* Crypto tests (no init needed) */
    test_hmac_sha256_vectors();
    test_constant_time_compare();

    /* UT-05 first: test without init */
    test_not_initialized();

    /* Set up test environment */
    if (setup_test_env() != 0) {
        fprintf(stderr, "Failed to set up test environment\n");
        return 1;
    }

    /* Load config and init */
    if (phonehome_config_load(&test_cfg, config_path) != 0) {
        fprintf(stderr, "Failed to load test config\n");
        cleanup_test_env();
        return 1;
    }
    if (phonehome_init(&test_cfg) != 0) {
        fprintf(stderr, "Failed to init phonehome\n");
        cleanup_test_env();
        return 1;
    }

    /* Handler tests */
    test_valid_hmac();
    test_invalid_hmac();
    test_replay_nonce();
    test_short_pdu();

    /* Cleanup */
    phonehome_shutdown();
    cleanup_test_env();

    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
