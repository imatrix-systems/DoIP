/**
 * @file test_discovery.c
 * @brief DoIP discovery and connectivity test tool
 *
 * Tests UDP vehicle discovery and TCP routing activation against a running
 * DoIP server. Reads the same config file as the server to validate
 * expected identity values.
 *
 * Usage: ./test-discovery [-c doip-server.conf] [server_ip] [port]
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
#include <arpa/inet.h>

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
 * UDP Helper: send raw DoIP request to specific IP:port (works on loopback)
 * ========================================================================== */

/**
 * Send a UDP DoIP message and wait for response.
 * Returns number of bytes received, or -1 on error/timeout.
 */
static int udp_send_recv(const char *server_ip, uint16_t port,
                         const uint8_t *send_buf, int send_len,
                         uint8_t *recv_buf, size_t recv_buf_size,
                         int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Set receive timeout */
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(fd);
        return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address: %s\n", server_ip);
        close(fd);
        return -1;
    }

    if (sendto(fd, send_buf, (size_t)send_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        perror("sendto");
        close(fd);
        return -1;
    }

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(fd, recv_buf, recv_buf_size, 0,
                         (struct sockaddr *)&from, &from_len);
    close(fd);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;  /* timeout — no response */
        return -1;
    }
    return (int)n;
}

/* ============================================================================
 * Test 1: UDP broadcast discovery (unicast to server)
 * ========================================================================== */

static void test_udp_discovery(const char *server_ip, uint16_t port,
                               const doip_app_config_t *expected)
{
    TEST_START("UDP vehicle discovery");

    uint8_t req[64];
    int req_len = doip_build_vehicle_id_request(req, sizeof(req));
    if (req_len < 0) {
        TEST_FAIL("UDP discovery", "failed to build request");
        return;
    }

    uint8_t resp[256];
    int resp_len = udp_send_recv(server_ip, port, req, req_len, resp, sizeof(resp), 2000);
    if (resp_len <= 0) {
        TEST_FAIL("UDP discovery", "no response (len=%d)", resp_len);
        return;
    }

    doip_message_t msg;
    if (doip_parse_message(resp, (size_t)resp_len, &msg) != DOIP_OK) {
        TEST_FAIL("UDP discovery", "failed to parse response");
        return;
    }

    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT) {
        TEST_FAIL("UDP discovery", "unexpected payload type 0x%04X", msg.header.payload_type);
        return;
    }

    const doip_vehicle_id_response_t *v = &msg.payload.vehicle_id;

    /* Validate VIN */
    if (memcmp(v->vin, expected->server.vin, DOIP_VIN_LENGTH) != 0) {
        TEST_FAIL("UDP discovery", "VIN mismatch");
        return;
    }

    /* Validate logical address */
    if (v->logical_address != expected->server.logical_address) {
        TEST_FAIL("UDP discovery", "logical_address 0x%04X != expected 0x%04X",
                  v->logical_address, expected->server.logical_address);
        return;
    }

    /* Validate EID */
    if (memcmp(v->eid, expected->server.eid, DOIP_EID_LENGTH) != 0) {
        TEST_FAIL("UDP discovery", "EID mismatch");
        return;
    }

    /* Validate GID */
    if (memcmp(v->gid, expected->server.gid, DOIP_GID_LENGTH) != 0) {
        TEST_FAIL("UDP discovery", "GID mismatch");
        return;
    }

    printf("  VIN:      %.17s\n", v->vin);
    printf("  LogAddr:  0x%04X\n", v->logical_address);
    printf("  EID:      %02X:%02X:%02X:%02X:%02X:%02X\n",
           v->eid[0], v->eid[1], v->eid[2], v->eid[3], v->eid[4], v->eid[5]);
    printf("  GID:      %02X:%02X:%02X:%02X:%02X:%02X\n",
           v->gid[0], v->gid[1], v->gid[2], v->gid[3], v->gid[4], v->gid[5]);

    TEST_PASS("UDP discovery");
}

/* ============================================================================
 * Test 2: UDP discovery by VIN (positive)
 * ========================================================================== */

static void test_udp_discovery_vin_positive(const char *server_ip, uint16_t port,
                                            const doip_app_config_t *expected)
{
    TEST_START("UDP discovery by VIN (positive)");

    uint8_t req[64];
    int req_len = doip_build_vehicle_id_request_vin(expected->server.vin, req, sizeof(req));
    if (req_len < 0) {
        TEST_FAIL("VIN positive", "failed to build request");
        return;
    }

    uint8_t resp[256];
    int resp_len = udp_send_recv(server_ip, port, req, req_len, resp, sizeof(resp), 2000);
    if (resp_len <= 0) {
        TEST_FAIL("VIN positive", "no response (server should reply to matching VIN)");
        return;
    }

    doip_message_t msg;
    if (doip_parse_message(resp, (size_t)resp_len, &msg) != DOIP_OK) {
        TEST_FAIL("VIN positive", "failed to parse response");
        return;
    }

    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT) {
        TEST_FAIL("VIN positive", "unexpected payload type 0x%04X", msg.header.payload_type);
        return;
    }

    if (memcmp(msg.payload.vehicle_id.vin, expected->server.vin, DOIP_VIN_LENGTH) != 0) {
        TEST_FAIL("VIN positive", "VIN mismatch in response");
        return;
    }

    TEST_PASS("VIN positive");
}

/* ============================================================================
 * Test 3: UDP discovery by VIN (negative)
 * ========================================================================== */

static void test_udp_discovery_vin_negative(const char *server_ip, uint16_t port)
{
    TEST_START("UDP discovery by VIN (negative)");

    uint8_t wrong_vin[DOIP_VIN_LENGTH];
    memset(wrong_vin, 'X', DOIP_VIN_LENGTH);

    uint8_t req[64];
    int req_len = doip_build_vehicle_id_request_vin(wrong_vin, req, sizeof(req));
    if (req_len < 0) {
        TEST_FAIL("VIN negative", "failed to build request");
        return;
    }

    uint8_t resp[256];
    int resp_len = udp_send_recv(server_ip, port, req, req_len, resp, sizeof(resp), 1500);
    if (resp_len > 0) {
        TEST_FAIL("VIN negative", "unexpected response (server should ignore wrong VIN)");
        return;
    }

    printf("  No response (correct — wrong VIN was ignored)\n");
    TEST_PASS("VIN negative");
}

/* ============================================================================
 * Test 4: UDP discovery by EID (positive + negative)
 * ========================================================================== */

static void test_udp_discovery_eid(const char *server_ip, uint16_t port,
                                   const doip_app_config_t *expected)
{
    TEST_START("UDP discovery by EID (positive + negative)");

    /* Positive: correct EID */
    uint8_t req[64];
    int req_len = doip_build_vehicle_id_request_eid(expected->server.eid, req, sizeof(req));
    if (req_len < 0) {
        TEST_FAIL("EID test", "failed to build positive request");
        return;
    }

    uint8_t resp[256];
    int resp_len = udp_send_recv(server_ip, port, req, req_len, resp, sizeof(resp), 2000);
    if (resp_len <= 0) {
        TEST_FAIL("EID test", "no response for correct EID");
        return;
    }

    doip_message_t msg;
    if (doip_parse_message(resp, (size_t)resp_len, &msg) != DOIP_OK) {
        TEST_FAIL("EID test", "failed to parse positive response");
        return;
    }

    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT) {
        TEST_FAIL("EID test", "unexpected type 0x%04X for positive case", msg.header.payload_type);
        return;
    }

    printf("  EID positive: got announcement (correct)\n");

    /* Negative: wrong EID */
    uint8_t wrong_eid[DOIP_EID_LENGTH] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    req_len = doip_build_vehicle_id_request_eid(wrong_eid, req, sizeof(req));
    if (req_len < 0) {
        TEST_FAIL("EID test", "failed to build negative request");
        return;
    }

    resp_len = udp_send_recv(server_ip, port, req, req_len, resp, sizeof(resp), 1500);
    if (resp_len > 0) {
        TEST_FAIL("EID test", "unexpected response for wrong EID");
        return;
    }

    printf("  EID negative: no response (correct)\n");
    TEST_PASS("EID test");
}

/* ============================================================================
 * Test 5: TCP connect + routing activation
 * ========================================================================== */

static void test_tcp_routing_activation(const char *server_ip, uint16_t port)
{
    TEST_START("TCP connect + routing activation");

    doip_client_t client;
    doip_client_init(&client, NULL);

    doip_result_t ret = doip_client_connect(&client, server_ip, port);
    if (ret != DOIP_OK) {
        TEST_FAIL("TCP routing", "connect failed: %s", doip_result_str(ret));
        doip_client_destroy(&client);
        return;
    }

    printf("  TCP connected to %s:%u\n", server_ip, port);

    doip_routing_activation_response_t ra_resp;
    ret = doip_client_activate_routing(&client, &ra_resp);
    if (ret != DOIP_OK) {
        TEST_FAIL("TCP routing", "routing activation failed: %s", doip_result_str(ret));
        doip_client_destroy(&client);
        return;
    }

    if (ra_resp.response_code != DOIP_ROUTING_ACTIVATION_SUCCESS) {
        TEST_FAIL("TCP routing", "routing activation code 0x%02X != 0x10",
                  ra_resp.response_code);
        doip_client_destroy(&client);
        return;
    }

    printf("  Routing activation: SUCCESS (entity 0x%04X)\n",
           ra_resp.entity_logical_address);

    doip_client_destroy(&client);
    TEST_PASS("TCP routing");
}

/* ============================================================================
 * Test 6: TesterPresent keepalive
 * ========================================================================== */

static void test_tester_present(const char *server_ip, uint16_t port)
{
    TEST_START("TesterPresent keepalive");

    doip_client_t client;
    doip_client_init(&client, NULL);

    doip_result_t ret = doip_client_connect(&client, server_ip, port);
    if (ret != DOIP_OK) {
        TEST_FAIL("TesterPresent", "connect failed: %s", doip_result_str(ret));
        doip_client_destroy(&client);
        return;
    }

    ret = doip_client_activate_routing(&client, NULL);
    if (ret != DOIP_OK) {
        TEST_FAIL("TesterPresent", "routing activation failed: %s", doip_result_str(ret));
        doip_client_destroy(&client);
        return;
    }

    ret = doip_client_uds_tester_present(&client);
    if (ret != DOIP_OK) {
        TEST_FAIL("TesterPresent", "TesterPresent failed: %s", doip_result_str(ret));
        doip_client_destroy(&client);
        return;
    }

    printf("  TesterPresent: positive response received\n");

    doip_client_destroy(&client);
    TEST_PASS("TesterPresent");
}

/* ============================================================================
 * Main
 * ========================================================================== */

int main(int argc, char *argv[])
{
    const char *config_file = NULL;
    const char *server_ip = "127.0.0.1";
    uint16_t port = 13400;
    int pos_arg = 0;

    /* Parse arguments */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-c config] [server_ip] [port]\n", argv[0]);
            return 0;
        }
        /* Positional args */
        if (pos_arg == 0) {
            server_ip = argv[i];
            pos_arg++;
        } else if (pos_arg == 1) {
            char *endptr;
            unsigned long val = strtoul(argv[i], &endptr, 10);
            if (endptr == argv[i] || *endptr != '\0' || val > 65535) {
                fprintf(stderr, "Error: invalid port '%s'\n", argv[i]);
                return 1;
            }
            port = (uint16_t)val;
            pos_arg++;
        }
        i++;
    }

    /* Load expected config for validation */
    doip_app_config_t expected;
    doip_config_defaults(&expected);

    if (config_file) {
        if (doip_config_load(&expected, config_file) != 0) {
            fprintf(stderr, "Error: cannot open config '%s'\n", config_file);
            return 1;
        }
    } else {
        doip_config_load(&expected, "doip-server.conf");
    }

    printf("========================================\n");
    printf(" DoIP Discovery Test Tool\n");
    printf("========================================\n");
    printf("Server: %s:%u\n", server_ip, port);
    printf("Expected VIN: %.17s\n", (const char *)expected.server.vin);
    printf("Expected EID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           expected.server.eid[0], expected.server.eid[1], expected.server.eid[2],
           expected.server.eid[3], expected.server.eid[4], expected.server.eid[5]);

    /* Run tests */
    test_udp_discovery(server_ip, port, &expected);
    test_udp_discovery_vin_positive(server_ip, port, &expected);
    test_udp_discovery_vin_negative(server_ip, port);
    test_udp_discovery_eid(server_ip, port, &expected);
    test_tcp_routing_activation(server_ip, port);
    test_tester_present(server_ip, port);

    /* Results */
    printf("\n========================================\n");
    printf("=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    printf("========================================\n");

    return g_failed > 0 ? 1 : 0;
}
