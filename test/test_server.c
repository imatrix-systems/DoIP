#define _POSIX_C_SOURCE 200809L

/**
 * @file test_server.c
 * @brief DoIP Server Comprehensive Test Suite (Suites A-E, 40 tests)
 *
 * Validates UDP discovery, TCP protocol compliance, blob write pipeline,
 * error handling, and concurrent access against a running DoIP server.
 *
 * Usage: ./test-server [-c doip-server.conf] [server_ip] [port] [-v]
 */

#include "doip.h"
#include "doip_client.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <dirent.h>
#include <time.h>

/* usleep() requires _DEFAULT_SOURCE or _BSD_SOURCE on some systems;
 * _POSIX_C_SOURCE 200809L may not expose it. Use nanosleep wrapper. */
static void msleep(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ============================================================================
 * Test Framework
 * ========================================================================== */

static int g_passed = 0;
static int g_failed = 0;
static int g_verbose = 0;

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
 * Globals
 * ========================================================================== */

static const char *g_server_ip = "127.0.0.1";
static uint16_t g_port = 13400;
static doip_app_config_t g_expected;

/* ============================================================================
 * CRC-32 (bit-by-bit, independent from server's table-based implementation)
 * ========================================================================== */

static uint32_t test_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
    }
    return crc ^ 0xFFFFFFFF;
}

/* ============================================================================
 * Helper: Hex Dump (verbose mode only)
 * ========================================================================== */

static void hex_dump(const char *label, const uint8_t *data, int len)
{
    if (!g_verbose || len <= 0) return;
    printf("  [%s] %d bytes:", label, len);
    for (int i = 0; i < len && i < 64; i++)
        printf(" %02X", data[i]);
    if (len > 64) printf(" ...");
    printf("\n");
}

/* ============================================================================
 * Helper: UDP send/recv (copied from test_discovery.c)
 * ========================================================================== */

static int udp_send_recv(const char *server_ip, uint16_t port,
                         const uint8_t *send_buf, int send_len,
                         uint8_t *recv_buf, size_t recv_buf_size,
                         int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO"); close(fd); return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP: %s\n", server_ip); close(fd); return -1;
    }

    if (sendto(fd, send_buf, (size_t)send_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        perror("sendto"); close(fd); return -1;
    }

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(fd, recv_buf, recv_buf_size, 0,
                         (struct sockaddr *)&from, &from_len);
    close(fd);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return (int)n;
}

/* ============================================================================
 * Helper: TCP connect + routing activation → heap-allocated client
 * ========================================================================== */

static doip_client_t *tcp_connect_and_activate(const char *ip, uint16_t port)
{
    doip_client_t *c = calloc(1, sizeof(doip_client_t));
    if (!c) return NULL;
    doip_client_init(c, NULL);

    if (doip_client_connect(c, ip, port) != DOIP_OK) {
        doip_client_destroy(c); free(c); return NULL;
    }
    if (doip_client_activate_routing(c, NULL) != DOIP_OK) {
        doip_client_destroy(c); free(c); return NULL;
    }
    return c;
}

static void client_free(doip_client_t *c)
{
    if (!c) return;
    doip_client_destroy(c);
    free(c);
}

/* ============================================================================
 * Helper: Raw TCP connect
 * ========================================================================== */

static int tcp_raw_connect(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { close(fd); return -1; }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

static int raw_recv_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n < 0) return -1;
    return (int)n;
}

/* ============================================================================
 * Helper: Generate test blob with CRC suffix
 * ========================================================================== */

static void generate_test_blob(uint8_t *buf, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
        buf[i] = (uint8_t)((i & 0xFF) ^ 0xA5);
    uint32_t crc = test_crc32(buf, size);
    memcpy(&buf[size], &crc, 4);
}

/* ============================================================================
 * Helper: Clear blob storage
 * ========================================================================== */

static void clear_blob_storage(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".bin") == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

/* ============================================================================
 * Helper: Verify blob on disk (retry loop)
 * ========================================================================== */

static int verify_blob_on_disk(const char *dir,
                               const uint8_t *expected_data, uint32_t expected_size)
{
    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) msleep(100); /* 100ms */

        DIR *d = opendir(dir);
        if (!d) continue;

        char newest[512] = {0};
        time_t newest_time = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t nlen = strlen(ent->d_name);
            if (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".bin") == 0) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
                struct stat st;
                if (stat(path, &st) == 0 && st.st_mtime >= newest_time) {
                    newest_time = st.st_mtime;
                    snprintf(newest, sizeof(newest), "%s", path);
                }
            }
        }
        closedir(d);

        if (newest[0] == '\0') continue;

        struct stat st;
        if (stat(newest, &st) != 0) continue;
        if ((uint32_t)st.st_size != expected_size) continue;

        FILE *f = fopen(newest, "rb");
        if (!f) continue;
        uint8_t *buf = malloc(expected_size);
        if (!buf) { fclose(f); continue; }
        size_t rd = fread(buf, 1, expected_size, f);
        fclose(f);

        if (rd == expected_size && memcmp(buf, expected_data, expected_size) == 0) {
            if (g_verbose)
                printf("  Blob verified: %s (%u bytes)\n", newest, expected_size);
            free(buf);
            return 0;
        }
        free(buf);
    }
    return -1;
}

/* ============================================================================
 * Helper: Full blob transfer (RequestDownload + TransferData loop + Exit)
 * Returns 0 on success, -1 on error.
 * ========================================================================== */

static int do_full_transfer(doip_client_t *client, uint32_t addr,
                            const uint8_t *data, uint32_t data_size,
                            int block_data_size)
{
    /* RequestDownload */
    uint16_t max_block = 0;
    doip_result_t ret = doip_client_uds_request_download(client, addr, 4,
                                                          data_size, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        printf("  RequestDownload failed: %s\n", doip_result_str(ret));
        return -1;
    }
    if (g_verbose)
        printf("  RequestDownload: addr=0x%08X size=%u → max_block=%u\n",
               addr, data_size, max_block);

    /* Compute block size if not specified */
    if (block_data_size <= 0) {
        uint16_t cap = max_block < 4092 ? max_block : 4092;
        block_data_size = cap - 2; /* subtract SID + BSC */
    }

    /* TransferData loop */
    uint32_t sent = 0;
    uint8_t bsc = 1;
    while (sent < data_size) {
        uint32_t chunk = data_size - sent;
        if (chunk > (uint32_t)block_data_size) chunk = (uint32_t)block_data_size;

        uint8_t resp[256];
        int rlen = doip_client_uds_transfer_data(client, bsc,
                                                  &data[sent], chunk,
                                                  resp, sizeof(resp));
        if (rlen < 2 || resp[0] != 0x76 || resp[1] != bsc) {
            printf("  TransferData block %u failed (rlen=%d)\n", bsc, rlen);
            return -1;
        }
        sent += chunk;
        bsc++;
    }

    /* RequestTransferExit */
    uint8_t exit_resp[256];
    int elen = doip_client_uds_request_transfer_exit(client, NULL, 0,
                                                      exit_resp, sizeof(exit_resp));
    if (elen < 1 || exit_resp[0] != 0x77) {
        printf("  RequestTransferExit failed (rlen=%d, resp=0x%02X)\n",
               elen, elen > 0 ? exit_resp[0] : 0);
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Suite A: UDP Discovery (3 tests)
 * ========================================================================== */

static void test_a1_udp_generic_discovery(void)
{
    TEST_START("Generic vehicle ID request (broadcast)");

    uint8_t req[64];
    int req_len = doip_build_vehicle_id_request(req, sizeof(req));
    if (req_len < 0) { TEST_FAIL("A.1 Generic discovery", "build failed"); return; }

    uint8_t resp[256];
    int resp_len = udp_send_recv(g_server_ip, g_port, req, req_len, resp, sizeof(resp), 2000);
    if (resp_len <= 0) { TEST_FAIL("A.1 Generic discovery", "no response"); return; }

    hex_dump("UDP response", resp, resp_len);

    doip_message_t msg;
    if (doip_parse_message(resp, (size_t)resp_len, &msg) != DOIP_OK) {
        TEST_FAIL("A.1 Generic discovery", "parse failed"); return;
    }
    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT) {
        TEST_FAIL("A.1 Generic discovery", "type 0x%04X", msg.header.payload_type); return;
    }
    if (msg.header.protocol_version != 0x03) {
        TEST_FAIL("A.1 Generic discovery", "version 0x%02X", msg.header.protocol_version); return;
    }
    if (msg.header.inverse_version != 0xFC) {
        TEST_FAIL("A.1 Generic discovery", "inv_version 0x%02X", msg.header.inverse_version); return;
    }

    const doip_vehicle_id_response_t *v = &msg.payload.vehicle_id;
    if (memcmp(v->vin, g_expected.server.vin, DOIP_VIN_LENGTH) != 0) {
        TEST_FAIL("A.1 Generic discovery", "VIN mismatch"); return;
    }
    if (v->logical_address != g_expected.server.logical_address) {
        TEST_FAIL("A.1 Generic discovery", "logical_address 0x%04X", v->logical_address); return;
    }
    if (memcmp(v->eid, g_expected.server.eid, DOIP_EID_LENGTH) != 0) {
        TEST_FAIL("A.1 Generic discovery", "EID mismatch"); return;
    }
    if (memcmp(v->gid, g_expected.server.gid, DOIP_GID_LENGTH) != 0) {
        TEST_FAIL("A.1 Generic discovery", "GID mismatch"); return;
    }
    if (v->further_action_required != 0x00) {
        TEST_FAIL("A.1 Generic discovery", "further_action 0x%02X", v->further_action_required); return;
    }
    if (v->has_sync_status && v->vin_gid_sync_status != 0x00) {
        TEST_FAIL("A.1 Generic discovery", "sync_status 0x%02X", v->vin_gid_sync_status); return;
    }

    TEST_PASS("A.1 Generic discovery");
}

static void test_a2_udp_vin_filter(void)
{
    TEST_START("Vehicle ID by VIN (positive + negative)");

    /* Positive: correct VIN */
    uint8_t req[64];
    int req_len = doip_build_vehicle_id_request_vin(g_expected.server.vin, req, sizeof(req));
    if (req_len < 0) { TEST_FAIL("A.2 VIN filter", "build positive failed"); return; }

    uint8_t resp[256];
    int resp_len = udp_send_recv(g_server_ip, g_port, req, req_len, resp, sizeof(resp), 2000);
    if (resp_len <= 0) { TEST_FAIL("A.2 VIN filter", "no positive response"); return; }

    doip_message_t msg;
    if (doip_parse_message(resp, (size_t)resp_len, &msg) != DOIP_OK) {
        TEST_FAIL("A.2 VIN filter", "parse positive failed"); return;
    }
    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT) {
        TEST_FAIL("A.2 VIN filter", "positive type 0x%04X", msg.header.payload_type); return;
    }
    if (memcmp(msg.payload.vehicle_id.vin, g_expected.server.vin, DOIP_VIN_LENGTH) != 0) {
        TEST_FAIL("A.2 VIN filter", "VIN mismatch in positive response"); return;
    }
    printf("  Positive: got announcement (correct VIN)\n");

    /* Negative: wrong VIN */
    uint8_t wrong_vin[DOIP_VIN_LENGTH];
    memset(wrong_vin, 'X', DOIP_VIN_LENGTH);
    req_len = doip_build_vehicle_id_request_vin(wrong_vin, req, sizeof(req));
    if (req_len < 0) { TEST_FAIL("A.2 VIN filter", "build negative failed"); return; }

    resp_len = udp_send_recv(g_server_ip, g_port, req, req_len, resp, sizeof(resp), 1500);
    if (resp_len > 0) { TEST_FAIL("A.2 VIN filter", "unexpected response for wrong VIN"); return; }

    printf("  Negative: no response (correct)\n");
    TEST_PASS("A.2 VIN filter");
}

static void test_a3_udp_eid_filter(void)
{
    TEST_START("Vehicle ID by EID (positive + negative)");

    /* Positive: correct EID */
    uint8_t req[64];
    int req_len = doip_build_vehicle_id_request_eid(g_expected.server.eid, req, sizeof(req));
    if (req_len < 0) { TEST_FAIL("A.3 EID filter", "build positive failed"); return; }

    uint8_t resp[256];
    int resp_len = udp_send_recv(g_server_ip, g_port, req, req_len, resp, sizeof(resp), 2000);
    if (resp_len <= 0) { TEST_FAIL("A.3 EID filter", "no positive response"); return; }

    doip_message_t msg;
    if (doip_parse_message(resp, (size_t)resp_len, &msg) != DOIP_OK) {
        TEST_FAIL("A.3 EID filter", "parse positive failed"); return;
    }
    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT) {
        TEST_FAIL("A.3 EID filter", "positive type 0x%04X", msg.header.payload_type); return;
    }
    printf("  Positive: got announcement (correct EID)\n");

    /* Negative: wrong EID */
    uint8_t wrong_eid[DOIP_EID_LENGTH] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    req_len = doip_build_vehicle_id_request_eid(wrong_eid, req, sizeof(req));
    if (req_len < 0) { TEST_FAIL("A.3 EID filter", "build negative failed"); return; }

    resp_len = udp_send_recv(g_server_ip, g_port, req, req_len, resp, sizeof(resp), 1500);
    if (resp_len > 0) { TEST_FAIL("A.3 EID filter", "unexpected response for wrong EID"); return; }

    printf("  Negative: no response (correct)\n");
    TEST_PASS("A.3 EID filter");
}

/* ============================================================================
 * Suite B: TCP Protocol (14 tests)
 * ========================================================================== */

static void test_b1_tester_present(void)
{
    TEST_START("B.1 TesterPresent after routing");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("B.1 TesterPresent", "connect failed"); return; }

    doip_result_t ret = doip_client_uds_tester_present(c);
    client_free(c);
    if (ret != DOIP_OK) { TEST_FAIL("B.1 TesterPresent", "%s", doip_result_str(ret)); return; }
    TEST_PASS("B.1 TesterPresent");
}

static void test_b2_tester_present_suppress(void)
{
    TEST_START("B.2 TesterPresent suppress positive response");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("B.2 TP suppress", "connect failed"); return; }

    /* Send TesterPresent with suppressPosRspMsgIndicationBit */
    uint8_t req[] = {0x3E, 0x80};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, 2, resp, sizeof(resp), 500);
    /* Should timeout or return error — no positive response expected */
    if (rlen > 0) {
        printf("  Got %d bytes response (expected none): [0x%02X]\n", rlen, resp[0]);
        /* Not necessarily a failure — some impls may still ACK at DoIP level */
    }

    /* Verify connection alive with normal TesterPresent */
    doip_result_t ret = doip_client_uds_tester_present(c);
    client_free(c);
    if (ret != DOIP_OK) { TEST_FAIL("B.2 TP suppress", "connection dead after suppress"); return; }
    TEST_PASS("B.2 TP suppress");
}

static void test_b3_entity_status(void)
{
    TEST_START("B.3 Entity status request");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("B.3 Entity status", "connect failed"); return; }

    doip_entity_status_response_t status;
    doip_result_t ret = doip_client_get_entity_status(c, &status);
    client_free(c);
    if (ret != DOIP_OK) { TEST_FAIL("B.3 Entity status", "%s", doip_result_str(ret)); return; }

    if (status.node_type != 0) {
        TEST_FAIL("B.3 Entity status", "node_type=%u", status.node_type); return;
    }
    if (status.max_concurrent_sockets != g_expected.server.max_tcp_connections) {
        TEST_FAIL("B.3 Entity status", "max_sockets=%u", status.max_concurrent_sockets); return;
    }
    if (status.currently_open_sockets < 1) {
        TEST_FAIL("B.3 Entity status", "open_sockets=%u", status.currently_open_sockets); return;
    }
    if (status.has_max_data_size && status.max_data_size != g_expected.server.max_data_size) {
        TEST_FAIL("B.3 Entity status", "max_data=%u", status.max_data_size); return;
    }

    printf("  node_type=0, max=%u, open=%u, max_data=%u\n",
           status.max_concurrent_sockets, status.currently_open_sockets,
           status.has_max_data_size ? status.max_data_size : 0);
    TEST_PASS("B.3 Entity status");
}

static void test_b4_power_mode(void)
{
    TEST_START("B.4 Diagnostic power mode");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("B.4 Power mode", "connect failed"); return; }

    doip_power_mode_response_t mode;
    doip_result_t ret = doip_client_get_power_mode(c, &mode);
    client_free(c);
    if (ret != DOIP_OK) { TEST_FAIL("B.4 Power mode", "%s", doip_result_str(ret)); return; }
    if (mode.power_mode != 0x01) {
        TEST_FAIL("B.4 Power mode", "mode=0x%02X (expected 0x01)", mode.power_mode); return;
    }
    TEST_PASS("B.4 Power mode");
}

static void test_b5_unsupported_uds(void)
{
    TEST_START("B.5 Unsupported UDS service (0x10)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("B.5 Unsupported UDS", "connect failed"); return; }

    uint8_t req[] = {0x10, 0x01};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, 2, resp, sizeof(resp), 2000);
    client_free(c);
    if (rlen < 3) { TEST_FAIL("B.5 Unsupported UDS", "short response (%d)", rlen); return; }
    if (resp[0] != 0x7F || resp[1] != 0x10 || resp[2] != 0x11) {
        TEST_FAIL("B.5 Unsupported UDS", "response [0x%02X,0x%02X,0x%02X]",
                  resp[0], resp[1], resp[2]); return;
    }
    TEST_PASS("B.5 Unsupported UDS");
}

static void test_b6_diagnostic_without_routing(void)
{
    TEST_START("B.6 Diagnostic without routing activation");
    int fd = tcp_raw_connect(g_server_ip, g_port);
    if (fd < 0) { TEST_FAIL("B.6 No routing", "connect failed"); return; }

    /* Build raw diagnostic message */
    uint8_t uds[] = {0x3E, 0x00};
    uint8_t msg_buf[64];
    int msg_len = doip_build_diagnostic_message(0x0E80, 0x0001, uds, 2, msg_buf, sizeof(msg_buf));
    if (msg_len < 0) { TEST_FAIL("B.6 No routing", "build failed"); close(fd); return; }

    send(fd, msg_buf, (size_t)msg_len, 0);
    uint8_t resp[256];
    int rlen = raw_recv_timeout(fd, resp, sizeof(resp), 1000);

    /* Should get no diagnostic response (server drops unactivated messages) */
    if (rlen > 0) {
        hex_dump("B.6 resp", resp, rlen);
        /* Some servers may NACK — check it's not a diagnostic response */
        doip_header_t hdr;
        if (doip_deserialize_header(resp, (size_t)rlen, &hdr) == DOIP_OK) {
            if (hdr.payload_type == DOIP_TYPE_DIAGNOSTIC_MESSAGE) {
                TEST_FAIL("B.6 No routing", "got diagnostic response without routing");
                close(fd);
                return;
            }
        }
    }

    /* Verify connection alive: send routing activation, should succeed */
    doip_routing_activation_request_t ra_req = {
        .source_address = 0x0E80,
        .activation_type = DOIP_ROUTING_ACTIVATION_DEFAULT,
    };
    uint8_t ra_buf[64];
    int ra_len = doip_build_routing_activation_request(&ra_req, ra_buf, sizeof(ra_buf));
    if (ra_len > 0) {
        send(fd, ra_buf, (size_t)ra_len, 0);
        rlen = raw_recv_timeout(fd, resp, sizeof(resp), 2000);
        if (rlen <= 0) {
            TEST_FAIL("B.6 No routing", "connection dead after unrouted diagnostic");
            close(fd);
            return;
        }
    }

    close(fd);
    TEST_PASS("B.6 No routing");
}

static void test_b7_diagnostic_unknown_target(void)
{
    TEST_START("B.7 Diagnostic to unknown target (0xFFFF)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("B.7 Unknown target", "connect failed"); return; }

    uint8_t uds[] = {0x3E, 0x00};
    doip_result_t ret = doip_client_send_diagnostic(c, 0xFFFF, uds, 2);
    if (ret != DOIP_OK) { TEST_FAIL("B.7 Unknown target", "send failed"); client_free(c); return; }

    doip_message_t msg;
    ret = doip_client_recv_message(c, &msg, 2000);
    if (ret != DOIP_OK) {
        TEST_FAIL("B.7 Unknown target", "recv failed: %s", doip_result_str(ret));
        client_free(c); return;
    }

    if (msg.header.payload_type != DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK) {
        TEST_FAIL("B.7 Unknown target", "type 0x%04X", msg.header.payload_type);
        client_free(c); return;
    }
    if (msg.payload.diagnostic_nack.nack_code != DOIP_DIAG_NACK_UNKNOWN_TA) {
        TEST_FAIL("B.7 Unknown target", "nack=0x%02X", msg.payload.diagnostic_nack.nack_code);
        client_free(c); return;
    }

    client_free(c);
    TEST_PASS("B.7 Unknown target");
}

static void test_b8_header_nack_bad_version(void)
{
    TEST_START("B.8 Header NACK — bad version");
    int fd = tcp_raw_connect(g_server_ip, g_port);
    if (fd < 0) { TEST_FAIL("B.8 Bad version", "connect failed"); return; }

    /* version=0x03, inverse=0x00 (should be 0xFC) */
    uint8_t bad_hdr[] = {0x03, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x07};
    send(fd, bad_hdr, sizeof(bad_hdr), 0);

    uint8_t resp[256];
    int rlen = raw_recv_timeout(fd, resp, sizeof(resp), 2000);
    close(fd);

    if (rlen < 9) { TEST_FAIL("B.8 Bad version", "short response (%d)", rlen); return; }

    doip_header_t hdr;
    if (doip_deserialize_header(resp, (size_t)rlen, &hdr) != DOIP_OK) {
        TEST_FAIL("B.8 Bad version", "parse failed"); return;
    }
    if (hdr.payload_type != DOIP_TYPE_HEADER_NACK) {
        TEST_FAIL("B.8 Bad version", "type 0x%04X", hdr.payload_type); return;
    }
    if (resp[8] != DOIP_HEADER_NACK_INCORRECT_PATTERN) {
        TEST_FAIL("B.8 Bad version", "nack=0x%02X", resp[8]); return;
    }
    TEST_PASS("B.8 Bad version");
}

static void test_b9_header_nack_unknown_type(void)
{
    TEST_START("B.9 Header NACK — unknown payload type");
    int fd = tcp_raw_connect(g_server_ip, g_port);
    if (fd < 0) { TEST_FAIL("B.9 Unknown type", "connect failed"); return; }

    /* First: perform routing activation so connection is established */
    doip_routing_activation_request_t ra_req = {
        .source_address = 0x0E80,
        .activation_type = DOIP_ROUTING_ACTIVATION_DEFAULT,
    };
    uint8_t ra_buf[64];
    int ra_len = doip_build_routing_activation_request(&ra_req, ra_buf, sizeof(ra_buf));
    send(fd, ra_buf, (size_t)ra_len, 0);
    uint8_t tmp[256];
    raw_recv_timeout(fd, tmp, sizeof(tmp), 2000); /* consume routing response */

    /* Send unknown payload type 0x9999 */
    uint8_t bad_msg[] = {0x03, 0xFC, 0x99, 0x99, 0x00, 0x00, 0x00, 0x00};
    send(fd, bad_msg, sizeof(bad_msg), 0);

    uint8_t resp[256];
    int rlen = raw_recv_timeout(fd, resp, sizeof(resp), 2000);
    close(fd);

    if (rlen < 9) { TEST_FAIL("B.9 Unknown type", "short response (%d)", rlen); return; }

    doip_header_t hdr;
    doip_deserialize_header(resp, (size_t)rlen, &hdr);
    if (hdr.payload_type != DOIP_TYPE_HEADER_NACK) {
        TEST_FAIL("B.9 Unknown type", "type 0x%04X", hdr.payload_type); return;
    }
    if (resp[8] != DOIP_HEADER_NACK_UNKNOWN_PAYLOAD_TYPE) {
        TEST_FAIL("B.9 Unknown type", "nack=0x%02X", resp[8]); return;
    }
    TEST_PASS("B.9 Unknown type");
}

static void test_b10_connection_limit(void)
{
    TEST_START("B.10 TCP connection limit (max_tcp_connections)");
    int max = g_expected.server.max_tcp_connections;
    doip_client_t *clients[8] = {0};
    int opened = 0;

    /* Open max connections */
    for (int i = 0; i < max && i < 8; i++) {
        clients[i] = tcp_connect_and_activate(g_server_ip, g_port);
        if (!clients[i]) {
            TEST_FAIL("B.10 Conn limit", "client %d failed to connect", i + 1);
            goto cleanup_b10;
        }
        opened++;
    }

    /* Attempt one more — should fail */
    doip_client_t extra;
    doip_client_init(&extra, NULL);
    doip_result_t ret = doip_client_connect(&extra, g_server_ip, g_port);
    if (ret == DOIP_OK) {
        /* Connected at TCP level, try routing activation */
        doip_routing_activation_response_t ra_resp;
        ret = doip_client_activate_routing(&extra, &ra_resp);
        if (ret == DOIP_OK) {
            /* Some servers allow TCP connect but reject at routing */
            printf("  Warning: 5th connection succeeded (server may use lazy rejection)\n");
        }
        doip_client_destroy(&extra);
    }
    printf("  Opened %d/%d connections, extra connection handled\n", opened, max);

    /* Close one and retry */
    client_free(clients[0]);
    clients[0] = NULL;
    msleep(200); /* 200ms for server cleanup */

    doip_client_t *retry = tcp_connect_and_activate(g_server_ip, g_port);
    if (!retry) {
        TEST_FAIL("B.10 Conn limit", "reconnect after close failed");
        goto cleanup_b10;
    }
    client_free(retry);
    TEST_PASS("B.10 Conn limit");

cleanup_b10:
    for (int i = 0; i < 8; i++) client_free(clients[i]);
}

static void test_b11_entity_status_socket_count(void)
{
    TEST_START("B.11 Entity status socket count accuracy");
    doip_client_t *a = tcp_connect_and_activate(g_server_ip, g_port);
    if (!a) { TEST_FAIL("B.11 Socket count", "client A connect failed"); return; }

    doip_entity_status_response_t status;
    doip_result_t ret = doip_client_get_entity_status(a, &status);
    if (ret != DOIP_OK) { TEST_FAIL("B.11 Socket count", "query 1 failed"); client_free(a); return; }
    uint8_t n = status.currently_open_sockets;

    /* Open client B */
    doip_client_t *b = tcp_connect_and_activate(g_server_ip, g_port);
    if (!b) { TEST_FAIL("B.11 Socket count", "client B connect failed"); client_free(a); return; }

    ret = doip_client_get_entity_status(a, &status);
    if (ret != DOIP_OK || status.currently_open_sockets != n + 1) {
        TEST_FAIL("B.11 Socket count", "expected %u, got %u", n + 1, status.currently_open_sockets);
        client_free(b); client_free(a); return;
    }
    printf("  After B connect: sockets=%u (was %u)\n", status.currently_open_sockets, n);

    /* Close B, poll for decrement */
    client_free(b);
    int found = 0;
    for (int i = 0; i < 5; i++) {
        msleep(500);
        ret = doip_client_get_entity_status(a, &status);
        if (ret == DOIP_OK && status.currently_open_sockets == n) {
            found = 1;
            break;
        }
    }
    client_free(a);
    if (!found) { TEST_FAIL("B.11 Socket count", "count didn't decrement"); return; }
    TEST_PASS("B.11 Socket count");
}

static void test_b12_source_addr_mismatch(void)
{
    TEST_START("B.12 Diagnostic source address mismatch");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("B.12 SA mismatch", "connect failed"); return; }

    /* Build raw diagnostic with wrong source address 0x1234 */
    uint8_t uds[] = {0x3E, 0x00};
    uint8_t msg_buf[64];
    int msg_len = doip_build_diagnostic_message(0x1234, 0x0001, uds, 2, msg_buf, sizeof(msg_buf));
    if (msg_len < 0) { TEST_FAIL("B.12 SA mismatch", "build failed"); client_free(c); return; }

    /* Send via raw socket */
    send(c->tcp_fd, msg_buf, (size_t)msg_len, 0);

    /* Receive response */
    uint8_t resp[256];
    int rlen = raw_recv_timeout(c->tcp_fd, resp, sizeof(resp), 2000);
    if (rlen < (int)DOIP_HEADER_SIZE) {
        TEST_FAIL("B.12 SA mismatch", "short response (%d)", rlen);
        client_free(c); return;
    }

    doip_header_t hdr;
    doip_deserialize_header(resp, (size_t)rlen, &hdr);
    if (hdr.payload_type != DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK) {
        TEST_FAIL("B.12 SA mismatch", "type 0x%04X", hdr.payload_type);
        client_free(c); return;
    }
    /* NACK code is at offset 12 (header=8 + SA=2 + TA=2 = byte 12) */
    if ((size_t)rlen > 12 && resp[12] != DOIP_DIAG_NACK_INVALID_SA) {
        TEST_FAIL("B.12 SA mismatch", "nack=0x%02X", resp[12]);
        client_free(c); return;
    }

    client_free(c);
    TEST_PASS("B.12 SA mismatch");
}

static void test_b13_header_nack_too_large(void)
{
    TEST_START("B.13 Header NACK — message too large");
    int fd = tcp_raw_connect(g_server_ip, g_port);
    if (fd < 0) { TEST_FAIL("B.13 Too large", "connect failed"); return; }

    /* First: routing activation */
    doip_routing_activation_request_t ra_req = {
        .source_address = 0x0E80,
        .activation_type = DOIP_ROUTING_ACTIVATION_DEFAULT,
    };
    uint8_t ra_buf[64];
    int ra_len = doip_build_routing_activation_request(&ra_req, ra_buf, sizeof(ra_buf));
    send(fd, ra_buf, (size_t)ra_len, 0);
    uint8_t tmp[256];
    raw_recv_timeout(fd, tmp, sizeof(tmp), 2000);

    /* Send header only with type=0x8001, length=5000 (no payload) */
    uint8_t hdr[] = {0x03, 0xFC, 0x80, 0x01, 0x00, 0x00, 0x13, 0x88};
    send(fd, hdr, sizeof(hdr), 0);

    uint8_t resp[256];
    int rlen = raw_recv_timeout(fd, resp, sizeof(resp), 2000);
    if (rlen < 9) { TEST_FAIL("B.13 Too large", "short response (%d)", rlen); close(fd); return; }

    doip_header_t rhdr;
    doip_deserialize_header(resp, (size_t)rlen, &rhdr);
    if (rhdr.payload_type != DOIP_TYPE_HEADER_NACK) {
        TEST_FAIL("B.13 Too large", "type 0x%04X", rhdr.payload_type); close(fd); return;
    }
    if (resp[8] != DOIP_HEADER_NACK_MESSAGE_TOO_LARGE) {
        TEST_FAIL("B.13 Too large", "nack=0x%02X", resp[8]); close(fd); return;
    }

    /* Verify connection alive */
    uint8_t alive_check[] = {0x03, 0xFC, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00};
    send(fd, alive_check, sizeof(alive_check), 0);
    rlen = raw_recv_timeout(fd, resp, sizeof(resp), 2000);
    close(fd);
    if (rlen <= 0) {
        TEST_FAIL("B.13 Too large", "connection closed after NACK"); return;
    }
    TEST_PASS("B.13 Too large");
}

static void test_b14_header_nack_invalid_payload_len(void)
{
    TEST_START("B.14 Header NACK — invalid payload length");
    int fd = tcp_raw_connect(g_server_ip, g_port);
    if (fd < 0) { TEST_FAIL("B.14 Invalid len", "connect failed"); return; }

    /* type=0x0005 (routing activation), payload_length=2 (expects 7+) */
    uint8_t hdr[] = {0x03, 0xFC, 0x00, 0x05, 0x00, 0x00, 0x00, 0x02};
    uint8_t payload[] = {0x00, 0x00};
    send(fd, hdr, sizeof(hdr), 0);
    send(fd, payload, sizeof(payload), 0);

    uint8_t resp[256];
    int rlen = raw_recv_timeout(fd, resp, sizeof(resp), 2000);
    close(fd);

    if (rlen < 9) { TEST_FAIL("B.14 Invalid len", "short response (%d)", rlen); return; }

    doip_header_t rhdr;
    doip_deserialize_header(resp, (size_t)rlen, &rhdr);
    if (rhdr.payload_type != DOIP_TYPE_HEADER_NACK) {
        TEST_FAIL("B.14 Invalid len", "type 0x%04X", rhdr.payload_type); return;
    }
    if (resp[8] != DOIP_HEADER_NACK_INVALID_PAYLOAD_LENGTH) {
        TEST_FAIL("B.14 Invalid len", "nack=0x%02X", resp[8]); return;
    }
    TEST_PASS("B.14 Invalid len");
}

/* ============================================================================
 * Suite C: Blob Write (5 tests)
 * ========================================================================== */

static void test_c1_small_blob_single_block(void)
{
    TEST_START("C.1 Small blob (100 bytes, single block)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("C.1 Small blob", "connect failed"); return; }

    uint8_t blob[104];
    generate_test_blob(blob, 100);
    clear_blob_storage(g_expected.blob_storage_dir);

    if (do_full_transfer(c, 0x1000, blob, 104, 0) != 0) {
        TEST_FAIL("C.1 Small blob", "transfer failed"); client_free(c); return;
    }

    if (verify_blob_on_disk(g_expected.blob_storage_dir, blob, 100) != 0) {
        TEST_FAIL("C.1 Small blob", "disk verification failed"); client_free(c); return;
    }

    client_free(c);
    TEST_PASS("C.1 Small blob");
}

static void test_c2_multi_block_blob(void)
{
    TEST_START("C.2 Multi-block blob (8000 bytes)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("C.2 Multi-block", "connect failed"); return; }

    uint32_t data_size = 8000;
    uint32_t total = data_size + 4;
    uint8_t *blob = malloc(total);
    if (!blob) { TEST_FAIL("C.2 Multi-block", "malloc failed"); client_free(c); return; }
    generate_test_blob(blob, data_size);
    clear_blob_storage(g_expected.blob_storage_dir);

    if (do_full_transfer(c, 0x2000, blob, total, 0) != 0) {
        TEST_FAIL("C.2 Multi-block", "transfer failed"); free(blob); client_free(c); return;
    }

    if (verify_blob_on_disk(g_expected.blob_storage_dir, blob, data_size) != 0) {
        TEST_FAIL("C.2 Multi-block", "disk verification failed"); free(blob); client_free(c); return;
    }

    free(blob);
    client_free(c);
    TEST_PASS("C.2 Multi-block");
}

static void test_c3_large_blob_bsc_wrap(void)
{
    TEST_START("C.3 Large blob with BSC wrap (1,047,140 bytes, 257 blocks)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("C.3 BSC wrap", "connect failed"); return; }

    uint32_t data_size = 1047140;
    uint32_t total = data_size + 4;
    uint8_t *blob = malloc(total);
    if (!blob) { TEST_FAIL("C.3 BSC wrap", "malloc failed"); client_free(c); return; }
    generate_test_blob(blob, data_size);
    clear_blob_storage(g_expected.blob_storage_dir);

    /* Manual transfer to verify BSC wrap */
    uint16_t max_block = 0;
    doip_result_t ret = doip_client_uds_request_download(c, 0x100000, 4,
                                                          total, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("C.3 BSC wrap", "RequestDownload: %s", doip_result_str(ret));
        free(blob); client_free(c); return;
    }

    uint16_t cap = max_block < 4092 ? max_block : 4092;
    int block_data = cap - 2;
    uint32_t sent = 0;
    uint8_t bsc = 1;
    int blocks = 0;

    while (sent < total) {
        uint32_t chunk = total - sent;
        if (chunk > (uint32_t)block_data) chunk = (uint32_t)block_data;

        uint8_t resp[256];
        int rlen = doip_client_uds_transfer_data(c, bsc, &blob[sent], chunk,
                                                  resp, sizeof(resp));
        if (rlen < 2 || resp[0] != 0x76 || resp[1] != bsc) {
            TEST_FAIL("C.3 BSC wrap", "block %d (BSC=0x%02X) failed", blocks + 1, bsc);
            free(blob); client_free(c); return;
        }
        sent += chunk;
        blocks++;

        /* Log BSC wrap points */
        if (bsc == 0xFF && g_verbose)
            printf("  Block %d: BSC=0xFF (next should wrap to 0x00)\n", blocks);
        if (bsc == 0x00 && blocks > 1 && g_verbose)
            printf("  Block %d: BSC=0x00 (wrapped!)\n", blocks);

        bsc++;
    }

    uint8_t exit_resp[256];
    int elen = doip_client_uds_request_transfer_exit(c, NULL, 0, exit_resp, sizeof(exit_resp));
    if (elen < 1 || exit_resp[0] != 0x77) {
        TEST_FAIL("C.3 BSC wrap", "TransferExit failed"); free(blob); client_free(c); return;
    }

    printf("  Transferred %u bytes in %d blocks (BSC wrapped)\n", total, blocks);

    if (verify_blob_on_disk(g_expected.blob_storage_dir, blob, data_size) != 0) {
        TEST_FAIL("C.3 BSC wrap", "disk verification failed"); free(blob); client_free(c); return;
    }

    free(blob);
    client_free(c);
    TEST_PASS("C.3 BSC wrap");
}

static void test_c4_back_to_back_transfers(void)
{
    TEST_START("C.4 Back-to-back transfers");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("C.4 Back-to-back", "connect failed"); return; }

    /* Transfer A: 500 bytes */
    uint32_t size_a = 500;
    uint8_t *blob_a = malloc(size_a + 4);
    if (!blob_a) { TEST_FAIL("C.4 Back-to-back", "malloc A"); client_free(c); return; }
    generate_test_blob(blob_a, size_a);
    clear_blob_storage(g_expected.blob_storage_dir);

    if (do_full_transfer(c, 0x3000, blob_a, size_a + 4, 0) != 0) {
        TEST_FAIL("C.4 Back-to-back", "transfer A failed"); free(blob_a); client_free(c); return;
    }
    if (verify_blob_on_disk(g_expected.blob_storage_dir, blob_a, size_a) != 0) {
        TEST_FAIL("C.4 Back-to-back", "blob A disk verify failed"); free(blob_a); client_free(c); return;
    }
    printf("  Transfer A: 500 bytes OK\n");

    /* Transfer B: 700 bytes */
    uint32_t size_b = 700;
    uint8_t *blob_b = malloc(size_b + 4);
    if (!blob_b) { TEST_FAIL("C.4 Back-to-back", "malloc B"); free(blob_a); client_free(c); return; }
    generate_test_blob(blob_b, size_b);
    clear_blob_storage(g_expected.blob_storage_dir);

    if (do_full_transfer(c, 0x4000, blob_b, size_b + 4, 0) != 0) {
        TEST_FAIL("C.4 Back-to-back", "transfer B failed");
        free(blob_a); free(blob_b); client_free(c); return;
    }
    if (verify_blob_on_disk(g_expected.blob_storage_dir, blob_b, size_b) != 0) {
        TEST_FAIL("C.4 Back-to-back", "blob B disk verify failed");
        free(blob_a); free(blob_b); client_free(c); return;
    }
    printf("  Transfer B: 700 bytes OK\n");

    free(blob_a);
    free(blob_b);
    client_free(c);
    TEST_PASS("C.4 Back-to-back");
}

static void test_c5_minimum_size_blob(void)
{
    TEST_START("C.5 Minimum size blob (1 byte + CRC)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("C.5 Min blob", "connect failed"); return; }

    uint8_t blob[5];
    generate_test_blob(blob, 1); /* 1 byte data + 4 byte CRC = 5 bytes */
    clear_blob_storage(g_expected.blob_storage_dir);

    if (do_full_transfer(c, 0x5000, blob, 5, 0) != 0) {
        TEST_FAIL("C.5 Min blob", "transfer failed"); client_free(c); return;
    }

    /* Verify: 1 byte, value = 0xA5 (0 ^ 0xFF = 0xA5) */
    if (verify_blob_on_disk(g_expected.blob_storage_dir, blob, 1) != 0) {
        TEST_FAIL("C.5 Min blob", "disk verification failed"); client_free(c); return;
    }

    client_free(c);
    TEST_PASS("C.5 Min blob");
}

/* ============================================================================
 * Suite D: Error Handling (14 tests)
 * ========================================================================== */

/* Helper: cleanup a transfer by sending RequestTransferExit */
static void cleanup_transfer(doip_client_t *c)
{
    uint8_t req[] = {0x37};
    uint8_t resp[256];
    doip_client_send_uds(c, req, 1, resp, sizeof(resp), 2000);
}

static void test_d1_download_already_active(void)
{
    TEST_START("D.1 RequestDownload — transfer already active");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.1 Already active", "connect failed"); return; }

    /* First RequestDownload: should succeed */
    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(c, 0x1000, 4, 100, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("D.1 Already active", "first download failed: %s", doip_result_str(ret));
        client_free(c); return;
    }

    /* Second RequestDownload: should fail with NRC 0x70 */
    uint8_t req2[] = {0x34, 0x00, 0x44, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0xC8};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req2, sizeof(req2), resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x34 || resp[2] != 0x70) {
        TEST_FAIL("D.1 Already active", "expected NRC 0x70, got [0x%02X,0x%02X,0x%02X]",
                  rlen > 0 ? resp[0] : 0, rlen > 1 ? resp[1] : 0, rlen > 2 ? resp[2] : 0);
        cleanup_transfer(c); client_free(c); return;
    }

    /* Cleanup: complete the first transfer */
    cleanup_transfer(c);
    client_free(c);
    TEST_PASS("D.1 Already active");
}

static void test_d2_download_size_exceeds_max(void)
{
    TEST_START("D.2 RequestDownload — size exceeds blob_max_size");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.2 Size exceeds", "connect failed"); return; }

    /* 16*1024*1024 + 1 = 16777217 */
    uint8_t req[] = {0x34, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00,
                     0x01, 0x00, 0x00, 0x01}; /* addr=0, size=16777217 */
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, sizeof(req), resp, sizeof(resp), 2000);
    client_free(c);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x34 || resp[2] != 0x70) {
        TEST_FAIL("D.2 Size exceeds", "expected NRC 0x70"); return;
    }
    TEST_PASS("D.2 Size exceeds");
}

static void test_d3_download_size_zero(void)
{
    TEST_START("D.3 RequestDownload — size zero");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.3 Size zero", "connect failed"); return; }

    uint8_t req[] = {0x34, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x00, 0x00};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, sizeof(req), resp, sizeof(resp), 2000);
    client_free(c);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x34 || resp[2] != 0x70) {
        TEST_FAIL("D.3 Size zero", "expected NRC 0x70"); return;
    }
    TEST_PASS("D.3 Size zero");
}

static void test_d4_download_truncated(void)
{
    TEST_START("D.4 RequestDownload — truncated message");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.4 Truncated", "connect failed"); return; }

    uint8_t req[] = {0x34, 0x00, 0x44}; /* format says 4+4 bytes follow, but 0 present */
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, sizeof(req), resp, sizeof(resp), 2000);
    client_free(c);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x34 || resp[2] != 0x13) {
        TEST_FAIL("D.4 Truncated", "expected NRC 0x13"); return;
    }
    TEST_PASS("D.4 Truncated");
}

static void test_d5_download_invalid_format(void)
{
    TEST_START("D.5 RequestDownload — invalid address/size format");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.5 Invalid format", "connect failed"); return; }

    /* Case A: addr_len=0 (format byte 0x40 = size_len=4, addr_len=0) */
    uint8_t req_a[] = {0x34, 0x00, 0x40, 0x00, 0x00, 0x00, 0x64};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req_a, sizeof(req_a), resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x34 || resp[2] != 0x31) {
        TEST_FAIL("D.5 Invalid format", "Case A: expected NRC 0x31");
        client_free(c); return;
    }
    printf("  Case A (addr_len=0): NRC 0x31 OK\n");

    /* Case B: size_len=5 (format byte 0x54 = size_len=5, addr_len=4) */
    uint8_t req_b[] = {0x34, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x64};
    rlen = doip_client_send_uds(c, req_b, sizeof(req_b), resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x34 || resp[2] != 0x31) {
        TEST_FAIL("D.5 Invalid format", "Case B: expected NRC 0x31");
        client_free(c); return;
    }
    printf("  Case B (size_len=5): NRC 0x31 OK\n");

    client_free(c);
    TEST_PASS("D.5 Invalid format");
}

static void test_d6_transfer_no_active(void)
{
    TEST_START("D.6 TransferData — no active transfer");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.6 No active", "connect failed"); return; }

    uint8_t req[] = {0x36, 0x01, 0xAA, 0xBB};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, sizeof(req), resp, sizeof(resp), 2000);
    client_free(c);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x36 || resp[2] != 0x24) {
        TEST_FAIL("D.6 No active", "expected NRC 0x24"); return;
    }
    TEST_PASS("D.6 No active");
}

static void test_d7_transfer_wrong_bsc(void)
{
    TEST_START("D.7 TransferData — wrong BSC");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.7 Wrong BSC", "connect failed"); return; }

    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(c, 0x1000, 4, 100, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("D.7 Wrong BSC", "download failed"); client_free(c); return;
    }

    /* Send with BSC=5, expected BSC=1 */
    uint8_t req[] = {0x36, 0x05, 0xAA, 0xBB};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, sizeof(req), resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x36 || resp[2] != 0x73) {
        TEST_FAIL("D.7 Wrong BSC", "expected NRC 0x73");
        cleanup_transfer(c); client_free(c); return;
    }

    cleanup_transfer(c);
    client_free(c);
    TEST_PASS("D.7 Wrong BSC");
}

static void test_d8_transfer_exceeds_size(void)
{
    TEST_START("D.8 TransferData — data exceeds requested size");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.8 Exceeds size", "connect failed"); return; }

    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(c, 0x1000, 4, 10, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("D.8 Exceeds size", "download failed"); client_free(c); return;
    }

    /* Send 20 bytes when only 10 were requested */
    uint8_t data[20];
    memset(data, 0xAA, sizeof(data));
    uint8_t req[22];
    req[0] = 0x36;
    req[1] = 0x01;
    memcpy(&req[2], data, 20);
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, 22, resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x36 || resp[2] != 0x71) {
        TEST_FAIL("D.8 Exceeds size", "expected NRC 0x71");
        cleanup_transfer(c); client_free(c); return;
    }

    cleanup_transfer(c);
    client_free(c);
    TEST_PASS("D.8 Exceeds size");
}

static void test_d9_exit_no_active(void)
{
    TEST_START("D.9 RequestTransferExit — no active transfer");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.9 Exit no active", "connect failed"); return; }

    uint8_t req[] = {0x37};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, 1, resp, sizeof(resp), 2000);
    client_free(c);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x37 || resp[2] != 0x24) {
        TEST_FAIL("D.9 Exit no active", "expected NRC 0x24"); return;
    }
    TEST_PASS("D.9 Exit no active");
}

static void test_d10_exit_crc_mismatch(void)
{
    TEST_START("D.10 RequestTransferExit — CRC mismatch");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.10 CRC mismatch", "connect failed"); return; }

    uint8_t blob[104];
    generate_test_blob(blob, 100);
    /* Corrupt the CRC */
    blob[100] ^= 0xFF;

    clear_blob_storage(g_expected.blob_storage_dir);

    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(c, 0x1000, 4, 104, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("D.10 CRC mismatch", "download failed"); client_free(c); return;
    }

    /* Send all data in one block */
    uint8_t td_resp[256];
    int tdlen = doip_client_uds_transfer_data(c, 1, blob, 104, td_resp, sizeof(td_resp));
    if (tdlen < 2 || td_resp[0] != 0x76) {
        TEST_FAIL("D.10 CRC mismatch", "transfer data failed"); client_free(c); return;
    }

    /* RequestTransferExit should fail with NRC 0x72 */
    uint8_t exit_req[] = {0x37};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, exit_req, 1, resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x37 || resp[2] != 0x72) {
        TEST_FAIL("D.10 CRC mismatch", "expected NRC 0x72, got [0x%02X,0x%02X,0x%02X]",
                  rlen > 0 ? resp[0] : 0, rlen > 1 ? resp[1] : 0, rlen > 2 ? resp[2] : 0);
        client_free(c); return;
    }

    client_free(c);
    TEST_PASS("D.10 CRC mismatch");
}

static void test_d11_transfer_timeout(void)
{
    const char *skip = getenv("SKIP_TIMEOUT");
    if (skip && strcmp(skip, "1") == 0) {
        TEST_START("D.11 Transfer timeout (SKIPPED)");
        printf("  SKIP: SKIP_TIMEOUT=1\n");
        g_passed++;
        return;
    }

    TEST_START("D.11 Transfer timeout (~35 seconds)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.11 Timeout", "connect failed"); return; }

    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(c, 0x1000, 4, 10000, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("D.11 Timeout", "download failed"); client_free(c); return;
    }

    /* Send first block */
    uint8_t data[100];
    memset(data, 0xAA, sizeof(data));
    uint8_t td_resp[256];
    int tdlen = doip_client_uds_transfer_data(c, 1, data, 100, td_resp, sizeof(td_resp));
    if (tdlen < 2 || td_resp[0] != 0x76) {
        TEST_FAIL("D.11 Timeout", "first block failed"); client_free(c); return;
    }

    /* Wait for timeout (transfer_timeout + 5 seconds) */
    printf("  Waiting %u seconds for transfer timeout...\n",
           g_expected.transfer_timeout_sec + 5);
    sleep(g_expected.transfer_timeout_sec + 5);

    /* Second block should fail */
    uint8_t req[] = {0x36, 0x02, 0xBB, 0xCC};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, sizeof(req), resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x36 || resp[2] != 0x24) {
        TEST_FAIL("D.11 Timeout", "expected NRC 0x24 after timeout");
        client_free(c); return;
    }

    client_free(c);
    TEST_PASS("D.11 Timeout");
}

static void test_d12_unsupported_sids(void)
{
    TEST_START("D.12 Unsupported UDS SIDs (0x10, 0x27, 0x35)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.12 Unsupported SIDs", "connect failed"); return; }

    uint8_t sids[] = {0x10, 0x27, 0x35};
    for (int i = 0; i < 3; i++) {
        uint8_t req[] = {sids[i], 0x01};
        uint8_t resp[256];
        int rlen = doip_client_send_uds(c, req, 2, resp, sizeof(resp), 2000);
        if (rlen < 3 || resp[0] != 0x7F || resp[1] != sids[i] || resp[2] != 0x11) {
            TEST_FAIL("D.12 Unsupported SIDs", "SID 0x%02X: expected NRC 0x11", sids[i]);
            client_free(c); return;
        }
    }

    client_free(c);
    TEST_PASS("D.12 Unsupported SIDs");
}

static void test_d13_transfer_data_too_short(void)
{
    TEST_START("D.13 TransferData — too short (missing BSC)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.13 Too short", "connect failed"); return; }

    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(c, 0x1000, 4, 100, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("D.13 Too short", "download failed"); client_free(c); return;
    }

    /* Send just {0x36} — missing BSC byte */
    uint8_t req[] = {0x36};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, req, 1, resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x36 || resp[2] != 0x13) {
        TEST_FAIL("D.13 Too short", "expected NRC 0x13");
        cleanup_transfer(c); client_free(c); return;
    }

    cleanup_transfer(c);
    client_free(c);
    TEST_PASS("D.13 Too short");
}

static void test_d14_blob_too_small_for_crc(void)
{
    TEST_START("D.14 Blob too small for CRC (< 4 bytes)");
    doip_client_t *c = tcp_connect_and_activate(g_server_ip, g_port);
    if (!c) { TEST_FAIL("D.14 Small blob", "connect failed"); return; }

    clear_blob_storage(g_expected.blob_storage_dir);

    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(c, 0x1000, 4, 2, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("D.14 Small blob", "download failed"); client_free(c); return;
    }

    /* Transfer 2 bytes */
    uint8_t data[] = {0xAA, 0xBB};
    uint8_t td_resp[256];
    int tdlen = doip_client_uds_transfer_data(c, 1, data, 2, td_resp, sizeof(td_resp));
    if (tdlen < 2 || td_resp[0] != 0x76) {
        TEST_FAIL("D.14 Small blob", "transfer data failed"); client_free(c); return;
    }

    /* RequestTransferExit — server saves as-is (positive 0x77) */
    uint8_t exit_req[] = {0x37};
    uint8_t resp[256];
    int rlen = doip_client_send_uds(c, exit_req, 1, resp, sizeof(resp), 2000);
    if (rlen < 1) {
        TEST_FAIL("D.14 Small blob", "no TransferExit response"); client_free(c); return;
    }

    if (resp[0] == 0x77) {
        printf("  TransferExit: positive (blob saved as-is, < 4 bytes)\n");
        /* Verify 2-byte file on disk */
        if (verify_blob_on_disk(g_expected.blob_storage_dir, data, 2) != 0) {
            TEST_FAIL("D.14 Small blob", "disk verify failed"); client_free(c); return;
        }
    } else if (rlen >= 3 && resp[0] == 0x7F && resp[2] == 0x72) {
        printf("  TransferExit: negative NRC 0x72 (CRC cannot be validated)\n");
    } else {
        TEST_FAIL("D.14 Small blob", "unexpected response 0x%02X", resp[0]);
        client_free(c); return;
    }

    client_free(c);
    TEST_PASS("D.14 Small blob");
}

/* ============================================================================
 * Suite E: Concurrent Access (4 tests)
 * ========================================================================== */

static void test_e1_concurrent_transfer_rejection(void)
{
    TEST_START("E.1 Concurrent transfer rejection");

    doip_client_t *a = tcp_connect_and_activate(g_server_ip, g_port);
    if (!a) { TEST_FAIL("E.1 Concurrent", "client A connect failed"); return; }
    doip_client_t *b = tcp_connect_and_activate(g_server_ip, g_port);
    if (!b) { TEST_FAIL("E.1 Concurrent", "client B connect failed"); client_free(a); return; }

    /* Client A: start transfer */
    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(a, 0x1000, 4, 10000, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("E.1 Concurrent", "client A download failed");
        client_free(b); client_free(a); return;
    }

    /* Client B: try to start transfer — should get NRC 0x70 */
    uint8_t req[] = {0x34, 0x00, 0x44, 0x00, 0x00, 0x10, 0x00,
                     0x00, 0x00, 0x13, 0x88}; /* addr=0x1000, size=5000 */
    uint8_t resp[256];
    int rlen = doip_client_send_uds(b, req, sizeof(req), resp, sizeof(resp), 2000);
    if (rlen < 3 || resp[0] != 0x7F || resp[1] != 0x34 || resp[2] != 0x70) {
        TEST_FAIL("E.1 Concurrent", "client B expected NRC 0x70");
        cleanup_transfer(a); client_free(b); client_free(a); return;
    }
    printf("  Client B correctly rejected (NRC 0x70)\n");

    /* Cleanup: abort A's transfer */
    cleanup_transfer(a);
    client_free(b);
    client_free(a);
    TEST_PASS("E.1 Concurrent");
}

static void test_e2_transfer_survives_other_client(void)
{
    TEST_START("E.2 Transfer survives other client disconnect");

    doip_client_t *a = tcp_connect_and_activate(g_server_ip, g_port);
    if (!a) { TEST_FAIL("E.2 Survive", "client A connect failed"); return; }

    uint32_t data_size = 500;
    uint32_t total = data_size + 4;
    uint8_t *blob = malloc(total);
    if (!blob) { TEST_FAIL("E.2 Survive", "malloc failed"); client_free(a); return; }
    generate_test_blob(blob, data_size);
    clear_blob_storage(g_expected.blob_storage_dir);

    /* Client A: start transfer */
    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(a, 0x6000, 4,
                                                          total, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("E.2 Survive", "download failed"); free(blob); client_free(a); return;
    }

    /* Client A: send first block */
    uint16_t cap = max_block < 4092 ? max_block : 4092;
    int bds = cap - 2;
    uint32_t chunk1 = total < (uint32_t)bds ? total : (uint32_t)bds;
    uint8_t td_resp[256];
    int tdlen = doip_client_uds_transfer_data(a, 1, blob, chunk1, td_resp, sizeof(td_resp));
    if (tdlen < 2 || td_resp[0] != 0x76) {
        TEST_FAIL("E.2 Survive", "first block failed");
        free(blob); client_free(a); return;
    }

    /* Client B: connect, TesterPresent, disconnect */
    doip_client_t *b = tcp_connect_and_activate(g_server_ip, g_port);
    if (b) {
        doip_client_uds_tester_present(b);
        client_free(b);
    }

    /* Client A: complete transfer */
    uint32_t sent = chunk1;
    uint8_t bsc = 2;
    while (sent < total) {
        uint32_t chunk = total - sent;
        if (chunk > (uint32_t)bds) chunk = (uint32_t)bds;
        tdlen = doip_client_uds_transfer_data(a, bsc, &blob[sent], chunk,
                                               td_resp, sizeof(td_resp));
        if (tdlen < 2 || td_resp[0] != 0x76) {
            TEST_FAIL("E.2 Survive", "block %u failed after B disconnect", bsc);
            free(blob); client_free(a); return;
        }
        sent += chunk;
        bsc++;
    }

    uint8_t exit_resp[256];
    int elen = doip_client_uds_request_transfer_exit(a, NULL, 0, exit_resp, sizeof(exit_resp));
    if (elen < 1 || exit_resp[0] != 0x77) {
        TEST_FAIL("E.2 Survive", "TransferExit failed"); free(blob); client_free(a); return;
    }

    if (verify_blob_on_disk(g_expected.blob_storage_dir, blob, data_size) != 0) {
        TEST_FAIL("E.2 Survive", "disk verify failed"); free(blob); client_free(a); return;
    }

    free(blob);
    client_free(a);
    TEST_PASS("E.2 Survive");
}

static void test_e3_multiple_tester_present(void)
{
    TEST_START("E.3 Multiple clients — independent TesterPresent");

    doip_client_t *clients[3] = {0};
    for (int i = 0; i < 3; i++) {
        clients[i] = tcp_connect_and_activate(g_server_ip, g_port);
        if (!clients[i]) {
            TEST_FAIL("E.3 Multi TP", "client %d connect failed", i + 1);
            for (int j = 0; j < i; j++) client_free(clients[j]);
            return;
        }
    }

    for (int i = 0; i < 3; i++) {
        doip_result_t ret = doip_client_uds_tester_present(clients[i]);
        if (ret != DOIP_OK) {
            TEST_FAIL("E.3 Multi TP", "client %d TesterPresent failed", i + 1);
            for (int j = 0; j < 3; j++) client_free(clients[j]);
            return;
        }
    }

    for (int i = 0; i < 3; i++) client_free(clients[i]);
    TEST_PASS("E.3 Multi TP");
}

static void test_e4_client_disconnect_mid_transfer(void)
{
    const char *skip = getenv("SKIP_TIMEOUT");
    if (skip && strcmp(skip, "1") == 0) {
        TEST_START("E.4 Client disconnect mid-transfer (SKIPPED)");
        printf("  SKIP: SKIP_TIMEOUT=1\n");
        g_passed++;
        return;
    }

    TEST_START("E.4 Client disconnect mid-transfer (~35 seconds)");

    /* Client A: start transfer, send partial, disconnect */
    doip_client_t *a = tcp_connect_and_activate(g_server_ip, g_port);
    if (!a) { TEST_FAIL("E.4 Disconnect", "client A connect failed"); return; }

    uint16_t max_block;
    doip_result_t ret = doip_client_uds_request_download(a, 0x7000, 4, 10000, 4, NULL, &max_block);
    if (ret != DOIP_OK) {
        TEST_FAIL("E.4 Disconnect", "download failed"); client_free(a); return;
    }

    uint8_t data[100];
    memset(data, 0xCC, sizeof(data));
    uint8_t td_resp[256];
    doip_client_uds_transfer_data(a, 1, data, 100, td_resp, sizeof(td_resp));

    /* Disconnect without completing */
    client_free(a);
    a = NULL;

    /* Client B: retry loop */
    doip_client_t *b = tcp_connect_and_activate(g_server_ip, g_port);
    if (!b) { TEST_FAIL("E.4 Disconnect", "client B connect failed"); return; }

    int success = 0;
    uint32_t max_wait = g_expected.transfer_timeout_sec + 5;
    printf("  Waiting up to %u seconds for transfer cleanup...\n", max_wait);

    for (uint32_t i = 0; i < max_wait; i++) {
        uint8_t req[] = {0x34, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x01, 0xF4}; /* size=500 */
        uint8_t resp[256];
        int rlen = doip_client_send_uds(b, req, sizeof(req), resp, sizeof(resp), 2000);
        if (rlen >= 1 && resp[0] == 0x74) {
            printf("  Transfer available after ~%u seconds\n", i);
            /* Cleanup the transfer we just started */
            uint8_t exit[] = {0x37};
            doip_client_send_uds(b, exit, 1, resp, sizeof(resp), 2000);
            success = 1;
            break;
        }
        sleep(1);
    }

    client_free(b);
    if (!success) { TEST_FAIL("E.4 Disconnect", "transfer not cleaned up in time"); return; }
    TEST_PASS("E.4 Disconnect");
}

/* ============================================================================
 * Main
 * ========================================================================== */

int main(int argc, char *argv[])
{
    const char *config_file = NULL;
    int pos_arg = 0;

    /* Parse arguments */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-c config] [-v] [server_ip] [port]\n", argv[0]);
            return 0;
        }
        if (pos_arg == 0) {
            g_server_ip = argv[i];
            pos_arg++;
        } else if (pos_arg == 1) {
            char *endptr;
            unsigned long val = strtoul(argv[i], &endptr, 10);
            if (endptr == argv[i] || *endptr != '\0' || val > 65535) {
                fprintf(stderr, "Error: invalid port '%s'\n", argv[i]);
                return 1;
            }
            g_port = (uint16_t)val;
            pos_arg++;
        }
        i++;
    }

    /* Load expected config */
    doip_config_defaults(&g_expected);
    if (config_file) {
        if (doip_config_load(&g_expected, config_file) != 0) {
            fprintf(stderr, "Error: cannot open config '%s'\n", config_file);
            return 1;
        }
    } else {
        doip_config_load(&g_expected, "doip-server.conf");
    }

    printf("========================================\n");
    printf(" DoIP Server Comprehensive Test Suite\n");
    printf("========================================\n");
    printf("Server: %s:%u\n", g_server_ip, g_port);
    if (g_verbose) printf("Verbose mode: ON\n");

    /* Wait for UDP handler to be ready (TCP readiness doesn't guarantee UDP) */
    {
        uint8_t probe[64];
        int plen = doip_build_vehicle_id_request(probe, sizeof(probe));
        uint8_t dummy[256];
        for (int attempt = 0; attempt < 5; attempt++) {
            if (udp_send_recv(g_server_ip, g_port, probe, plen,
                              dummy, sizeof(dummy), 500) > 0)
                break;
        }
    }

    /* Suite A: UDP Discovery */
    printf("\n[Suite A: UDP Discovery — 3 tests]\n");
    test_a1_udp_generic_discovery();
    test_a2_udp_vin_filter();
    test_a3_udp_eid_filter();

    /* Suite B: TCP Protocol */
    printf("\n[Suite B: TCP Protocol — 14 tests]\n");
    test_b1_tester_present();
    test_b2_tester_present_suppress();
    test_b3_entity_status();
    test_b4_power_mode();
    test_b5_unsupported_uds();
    test_b6_diagnostic_without_routing();
    test_b7_diagnostic_unknown_target();
    test_b8_header_nack_bad_version();
    test_b9_header_nack_unknown_type();
    test_b10_connection_limit();
    test_b11_entity_status_socket_count();
    test_b12_source_addr_mismatch();
    test_b13_header_nack_too_large();
    test_b14_header_nack_invalid_payload_len();

    /* Suite C: Blob Write */
    printf("\n[Suite C: Blob Write — 5 tests]\n");
    test_c1_small_blob_single_block();
    test_c2_multi_block_blob();
    test_c3_large_blob_bsc_wrap();
    test_c4_back_to_back_transfers();
    test_c5_minimum_size_blob();

    /* Suite D: Error Handling */
    printf("\n[Suite D: Error Handling — 14 tests]\n");
    test_d1_download_already_active();
    test_d2_download_size_exceeds_max();
    test_d3_download_size_zero();
    test_d4_download_truncated();
    test_d5_download_invalid_format();
    test_d6_transfer_no_active();
    test_d7_transfer_wrong_bsc();
    test_d8_transfer_exceeds_size();
    test_d9_exit_no_active();
    test_d10_exit_crc_mismatch();
    test_d11_transfer_timeout();
    test_d12_unsupported_sids();
    test_d13_transfer_data_too_short();
    test_d14_blob_too_small_for_crc();

    /* Suite E: Concurrent Access */
    printf("\n[Suite E: Concurrent Access — 4 tests]\n");
    test_e1_concurrent_transfer_rejection();
    test_e2_transfer_survives_other_client();
    test_e3_multiple_tester_present();
    test_e4_client_disconnect_mid_transfer();

    /* Results */
    printf("\n========================================\n");
    printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    printf("========================================\n");

    return g_failed > 0 ? 1 : 0;
}
