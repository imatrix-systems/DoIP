/**
 * @file doip_client.c
 * @brief DoIP Client implementation
 *
 * Full client implementation including:
 * - UDP broadcast discovery
 * - TCP connection & routing activation
 * - Diagnostic message send/receive
 * - UDS helper functions
 */

#include "doip_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

/* ============================================================================
 * Internal Helpers
 * ========================================================================== */

static doip_result_t set_socket_timeout(int fd, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return DOIP_ERR_SOCKET;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return DOIP_ERR_SOCKET;

    return DOIP_OK;
}

/**
 * @brief Receive exactly `len` bytes from TCP socket
 */
static int tcp_recv_exact(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t received = 0;
    struct timeval tv;
    fd_set fds;

    while (received < len) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int sel = select(fd + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            return DOIP_ERR_RECV;
        }
        if (sel == 0)
            return DOIP_ERR_TIMEOUT;

        ssize_t n = recv(fd, buf + received, len - received, 0);
        if (n <= 0) {
            if (n == 0) return DOIP_ERR_CLOSED;
            if (errno == EINTR) continue;
            return DOIP_ERR_RECV;
        }
        received += (size_t)n;
    }

    return (int)received;
}

/**
 * @brief Receive a complete DoIP message from TCP socket
 */
static doip_result_t tcp_recv_doip_message(int fd, uint8_t *buf, size_t buf_size,
                                             doip_message_t *msg, int timeout_ms)
{
    /* First receive the header */
    int ret = tcp_recv_exact(fd, buf, DOIP_HEADER_SIZE, timeout_ms);
    if (ret < 0)
        return (doip_result_t)ret;

    /* Parse header to get payload length */
    doip_header_t header;
    doip_result_t result = doip_deserialize_header(buf, DOIP_HEADER_SIZE, &header);
    if (result != DOIP_OK)
        return result;

    result = doip_validate_header(&header);
    if (result != DOIP_OK)
        return result;

    if (header.payload_length > 0) {
        if (DOIP_HEADER_SIZE + header.payload_length > buf_size)
            return DOIP_ERR_BUFFER_TOO_SMALL;

        ret = tcp_recv_exact(fd, buf + DOIP_HEADER_SIZE,
                             header.payload_length, timeout_ms);
        if (ret < 0)
            return (doip_result_t)ret;
    }

    return doip_parse_message(buf, DOIP_HEADER_SIZE + header.payload_length, msg);
}

/**
 * @brief Send a complete buffer over TCP
 */
static doip_result_t tcp_send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return DOIP_ERR_SEND;
        }
        sent += (size_t)n;
    }
    return DOIP_OK;
}

/* ============================================================================
 * Client Lifecycle
 * ========================================================================== */

doip_result_t doip_client_init(doip_client_t *client, const doip_client_config_t *config)
{
    if (!client)
        return DOIP_ERR_INVALID_PARAM;

    memset(client, 0, sizeof(doip_client_t));
    client->tcp_fd = -1;
    client->udp_fd = -1;
    client->routing_active = false;

    if (config) {
        client->config = *config;
    } else {
        doip_client_config_t defaults = DOIP_CLIENT_CONFIG_DEFAULT;
        client->config = defaults;
    }

    return DOIP_OK;
}

void doip_client_destroy(doip_client_t *client)
{
    if (!client)
        return;
    doip_client_disconnect(client);
    if (client->udp_fd >= 0) {
        close(client->udp_fd);
        client->udp_fd = -1;
    }
}

/* ============================================================================
 * Vehicle Discovery (UDP)
 * ========================================================================== */

static int create_udp_socket(const char *interface_ip, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    /* Enable broadcast */
    int broadcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    /* Enable address reuse */
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Bind to interface */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(0);  /* Any port */

    if (interface_ip) {
        bind_addr.sin_addr.s_addr = inet_addr(interface_ip);
    } else {
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Set receive timeout */
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

int doip_client_discover(doip_client_t *client, const char *interface_ip,
                          doip_discovery_result_t *results, int max_results,
                          int timeout_ms)
{
    if (!client || !results || max_results <= 0)
        return DOIP_ERR_INVALID_PARAM;

    int fd = create_udp_socket(interface_ip, timeout_ms);
    if (fd < 0)
        return DOIP_ERR_SOCKET;

    /* Build vehicle identification request */
    uint8_t send_buf[DOIP_HEADER_SIZE];
    int msg_len = doip_build_vehicle_id_request(send_buf, sizeof(send_buf));
    if (msg_len < 0) {
        close(fd);
        return msg_len;
    }

    /* Send broadcast */
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DOIP_UDP_DISCOVERY_PORT);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    if (sendto(fd, send_buf, (size_t)msg_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(fd);
        return DOIP_ERR_SEND;
    }

    /* Collect responses */
    int found = 0;
    uint8_t recv_buf[512];
    struct sockaddr_in from_addr;
    socklen_t from_len;

    while (found < max_results) {
        from_len = sizeof(from_addr);
        ssize_t n = recvfrom(fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0) {
            break;  /* Timeout or error */
        }

        doip_message_t msg;
        if (doip_parse_message(recv_buf, (size_t)n, &msg) == DOIP_OK &&
            msg.header.payload_type == DOIP_TYPE_VEHICLE_ANNOUNCEMENT) {
            results[found].vehicle = msg.payload.vehicle_id;
            results[found].source_addr = from_addr;
            found++;
        }
    }

    close(fd);
    return found;
}

doip_result_t doip_client_discover_by_vin(doip_client_t *client, const char *interface_ip,
                                           const uint8_t vin[DOIP_VIN_LENGTH],
                                           doip_discovery_result_t *result,
                                           int timeout_ms)
{
    if (!client || !vin || !result)
        return DOIP_ERR_INVALID_PARAM;

    int fd = create_udp_socket(interface_ip, timeout_ms);
    if (fd < 0)
        return DOIP_ERR_SOCKET;

    uint8_t send_buf[DOIP_HEADER_SIZE + DOIP_VIN_LENGTH];
    int msg_len = doip_build_vehicle_id_request_vin(vin, send_buf, sizeof(send_buf));
    if (msg_len < 0) {
        close(fd);
        return (doip_result_t)msg_len;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DOIP_UDP_DISCOVERY_PORT);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    if (sendto(fd, send_buf, (size_t)msg_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(fd);
        return DOIP_ERR_SEND;
    }

    uint8_t recv_buf[512];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t n = recvfrom(fd, recv_buf, sizeof(recv_buf), 0,
                         (struct sockaddr *)&from_addr, &from_len);
    close(fd);

    if (n <= 0)
        return DOIP_ERR_TIMEOUT;

    doip_message_t msg;
    doip_result_t ret = doip_parse_message(recv_buf, (size_t)n, &msg);
    if (ret != DOIP_OK)
        return ret;

    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT)
        return DOIP_ERR_NACK;

    result->vehicle = msg.payload.vehicle_id;
    result->source_addr = from_addr;
    return DOIP_OK;
}

doip_result_t doip_client_discover_by_eid(doip_client_t *client, const char *interface_ip,
                                           const uint8_t eid[DOIP_EID_LENGTH],
                                           doip_discovery_result_t *result,
                                           int timeout_ms)
{
    if (!client || !eid || !result)
        return DOIP_ERR_INVALID_PARAM;

    int fd = create_udp_socket(interface_ip, timeout_ms);
    if (fd < 0)
        return DOIP_ERR_SOCKET;

    uint8_t send_buf[DOIP_HEADER_SIZE + DOIP_EID_LENGTH];
    int msg_len = doip_build_vehicle_id_request_eid(eid, send_buf, sizeof(send_buf));
    if (msg_len < 0) {
        close(fd);
        return (doip_result_t)msg_len;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DOIP_UDP_DISCOVERY_PORT);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    if (sendto(fd, send_buf, (size_t)msg_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(fd);
        return DOIP_ERR_SEND;
    }

    uint8_t recv_buf[512];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t n = recvfrom(fd, recv_buf, sizeof(recv_buf), 0,
                         (struct sockaddr *)&from_addr, &from_len);
    close(fd);

    if (n <= 0)
        return DOIP_ERR_TIMEOUT;

    doip_message_t msg;
    doip_result_t ret = doip_parse_message(recv_buf, (size_t)n, &msg);
    if (ret != DOIP_OK)
        return ret;

    if (msg.header.payload_type != DOIP_TYPE_VEHICLE_ANNOUNCEMENT)
        return DOIP_ERR_NACK;

    result->vehicle = msg.payload.vehicle_id;
    result->source_addr = from_addr;
    return DOIP_OK;
}

/* ============================================================================
 * TCP Connection & Routing Activation
 * ========================================================================== */

doip_result_t doip_client_connect(doip_client_t *client, const char *host, uint16_t port)
{
    if (!client || !host)
        return DOIP_ERR_INVALID_PARAM;

    if (port == 0)
        port = DOIP_TCP_DATA_PORT;

    /* Close existing connection if any */
    doip_client_disconnect(client);

    /* Create TCP socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return DOIP_ERR_SOCKET;

    /* Disable Nagle for low latency */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Set up server address */
    memset(&client->server_addr, 0, sizeof(client->server_addr));
    client->server_addr.sin_family = AF_INET;
    client->server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &client->server_addr.sin_addr) <= 0) {
        close(fd);
        return DOIP_ERR_INVALID_PARAM;
    }

    /* Set non-blocking for connect with timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(fd, (struct sockaddr *)&client->server_addr,
                      sizeof(client->server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return DOIP_ERR_CONNECT;
    }

    if (ret < 0) {
        /* Wait for connection */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);

        struct timeval tv;
        tv.tv_sec  = client->config.tcp_connect_timeout_ms / 1000;
        tv.tv_usec = (client->config.tcp_connect_timeout_ms % 1000) * 1000;

        int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            close(fd);
            return sel == 0 ? DOIP_ERR_TIMEOUT : DOIP_ERR_CONNECT;
        }

        /* Check for connection errors */
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(fd);
            return DOIP_ERR_CONNECT;
        }
    }

    /* Set back to blocking */
    fcntl(fd, F_SETFL, flags);

    /* Set receive timeout */
    set_socket_timeout(fd, client->config.tcp_recv_timeout_ms);

    client->tcp_fd = fd;
    client->routing_active = false;

    printf("[DoIP Client] Connected to %s:%u\n", host, port);
    return DOIP_OK;
}

doip_result_t doip_client_activate_routing(doip_client_t *client,
                                            doip_routing_activation_response_t *resp)
{
    if (!client || client->tcp_fd < 0)
        return DOIP_ERR_INVALID_PARAM;

    /* Build routing activation request */
    doip_routing_activation_request_t req = {
        .source_address = client->config.tester_address,
        .activation_type = client->config.activation_type,
        .reserved = 0x00000000,
        .has_oem_specific = false,
    };

    uint8_t send_buf[64];
    int msg_len = doip_build_routing_activation_request(&req, send_buf, sizeof(send_buf));
    if (msg_len < 0)
        return (doip_result_t)msg_len;

    /* Send request */
    doip_result_t result = tcp_send_all(client->tcp_fd, send_buf, (size_t)msg_len);
    if (result != DOIP_OK)
        return result;

    printf("[DoIP Client] Routing activation request sent (SA=0x%04X, type=0x%02X)\n",
           req.source_address, req.activation_type);

    /* Receive response */
    doip_message_t msg;
    result = tcp_recv_doip_message(client->tcp_fd, client->recv_buf,
                                    sizeof(client->recv_buf), &msg,
                                    client->config.tcp_recv_timeout_ms);
    if (result != DOIP_OK)
        return result;

    if (msg.header.payload_type == DOIP_TYPE_HEADER_NACK) {
        printf("[DoIP Client] Received header NACK: 0x%02X\n", msg.payload.nack_code);
        return DOIP_ERR_NACK;
    }

    if (msg.header.payload_type != DOIP_TYPE_ROUTING_ACTIVATION_RESPONSE) {
        printf("[DoIP Client] Unexpected response type: 0x%04X\n", msg.header.payload_type);
        return DOIP_ERR_NACK;
    }

    doip_routing_activation_response_t *ra_resp = &msg.payload.routing_resp;

    if (resp)
        *resp = *ra_resp;

    printf("[DoIP Client] Routing activation response: %s (code=0x%02X)\n",
           doip_routing_response_str(ra_resp->response_code), ra_resp->response_code);

    if (ra_resp->response_code == DOIP_ROUTING_ACTIVATION_SUCCESS) {
        client->routing_active = true;
        return DOIP_OK;
    }

    return DOIP_ERR_ROUTING_DENIED;
}

void doip_client_disconnect(doip_client_t *client)
{
    if (!client)
        return;
    if (client->tcp_fd >= 0) {
        close(client->tcp_fd);
        client->tcp_fd = -1;
    }
    client->routing_active = false;
    printf("[DoIP Client] Disconnected\n");
}

bool doip_client_is_connected(const doip_client_t *client)
{
    return client && client->tcp_fd >= 0 && client->routing_active;
}

/* ============================================================================
 * Diagnostic Communication
 * ========================================================================== */

doip_result_t doip_client_send_diagnostic(doip_client_t *client, uint16_t target,
                                           const uint8_t *data, uint32_t data_len)
{
    if (!client || !data || data_len == 0 || client->tcp_fd < 0)
        return DOIP_ERR_INVALID_PARAM;

    if (!client->routing_active)
        return DOIP_ERR_ROUTING_DENIED;

    uint8_t *send_buf = malloc(DOIP_HEADER_SIZE + 4 + data_len);
    if (!send_buf)
        return DOIP_ERR_MEMORY;

    int msg_len = doip_build_diagnostic_message(client->config.tester_address,
                                                  target, data, data_len,
                                                  send_buf, DOIP_HEADER_SIZE + 4 + data_len);
    if (msg_len < 0) {
        free(send_buf);
        return (doip_result_t)msg_len;
    }

    doip_result_t result = tcp_send_all(client->tcp_fd, send_buf, (size_t)msg_len);
    free(send_buf);
    return result;
}

doip_result_t doip_client_recv_message(doip_client_t *client, doip_message_t *msg,
                                        int timeout_ms)
{
    if (!client || !msg || client->tcp_fd < 0)
        return DOIP_ERR_INVALID_PARAM;

    if (timeout_ms <= 0)
        timeout_ms = client->config.tcp_recv_timeout_ms;

    return tcp_recv_doip_message(client->tcp_fd, client->recv_buf,
                                  sizeof(client->recv_buf), msg, timeout_ms);
}

int doip_client_send_uds(doip_client_t *client,
                          const uint8_t *uds_request, uint32_t request_len,
                          uint8_t *uds_response, uint32_t response_size,
                          int timeout_ms)
{
    if (!client || !uds_request || request_len == 0)
        return DOIP_ERR_INVALID_PARAM;

    if (timeout_ms <= 0)
        timeout_ms = client->config.tcp_recv_timeout_ms;

    /* Send diagnostic request */
    doip_result_t result = doip_client_send_diagnostic(client,
                                                         client->config.ecu_address,
                                                         uds_request, request_len);
    if (result != DOIP_OK)
        return (int)result;

    /* Wait for diagnostic ACK first */
    doip_message_t msg;
    result = tcp_recv_doip_message(client->tcp_fd, client->recv_buf,
                                    sizeof(client->recv_buf), &msg, timeout_ms);
    if (result != DOIP_OK)
        return (int)result;

    /* Handle alive check inline */
    if (msg.header.payload_type == DOIP_TYPE_ALIVE_CHECK_REQUEST) {
        uint8_t alive_buf[DOIP_HEADER_SIZE + 2];
        int alive_len = doip_build_alive_check_response(client->config.tester_address,
                                                          alive_buf, sizeof(alive_buf));
        if (alive_len > 0)
            tcp_send_all(client->tcp_fd, alive_buf, (size_t)alive_len);

        /* Continue waiting for actual response */
        result = tcp_recv_doip_message(client->tcp_fd, client->recv_buf,
                                        sizeof(client->recv_buf), &msg, timeout_ms);
        if (result != DOIP_OK)
            return (int)result;
    }

    /* Check for NACK */
    if (msg.header.payload_type == DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK) {
        printf("[DoIP Client] Diagnostic NACK: %s\n",
               doip_diag_nack_str((doip_diag_nack_code_t)msg.payload.diagnostic_nack.nack_code));
        return DOIP_ERR_NACK;
    }

    /* If we got an ACK, wait for the actual diagnostic response */
    if (msg.header.payload_type == DOIP_TYPE_DIAGNOSTIC_MESSAGE_ACK) {
        result = tcp_recv_doip_message(client->tcp_fd, client->recv_buf,
                                        sizeof(client->recv_buf), &msg, timeout_ms);
        if (result != DOIP_OK)
            return (int)result;
    }

    /* We should now have the diagnostic message response */
    if (msg.header.payload_type != DOIP_TYPE_DIAGNOSTIC_MESSAGE) {
        printf("[DoIP Client] Unexpected message type: 0x%04X\n", msg.header.payload_type);
        return DOIP_ERR_NACK;
    }

    /* Copy UDS response data */
    doip_diagnostic_message_t *diag = &msg.payload.diagnostic;
    if (uds_response && response_size > 0) {
        uint32_t copy_len = diag->data_length;
        if (copy_len > response_size)
            copy_len = response_size;
        memcpy(uds_response, diag->data, copy_len);
        return (int)copy_len;
    }

    return (int)diag->data_length;
}

/* ============================================================================
 * Entity Status & Power Mode
 * ========================================================================== */

doip_result_t doip_client_get_entity_status(doip_client_t *client,
                                             doip_entity_status_response_t *status)
{
    if (!client || !status || client->tcp_fd < 0)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t send_buf[DOIP_HEADER_SIZE];
    int msg_len = doip_build_entity_status_request(send_buf, sizeof(send_buf));
    if (msg_len < 0)
        return (doip_result_t)msg_len;

    doip_result_t result = tcp_send_all(client->tcp_fd, send_buf, (size_t)msg_len);
    if (result != DOIP_OK)
        return result;

    doip_message_t msg;
    result = tcp_recv_doip_message(client->tcp_fd, client->recv_buf,
                                    sizeof(client->recv_buf), &msg,
                                    client->config.tcp_recv_timeout_ms);
    if (result != DOIP_OK)
        return result;

    if (msg.header.payload_type != DOIP_TYPE_ENTITY_STATUS_RESPONSE)
        return DOIP_ERR_NACK;

    *status = msg.payload.entity_status;
    return DOIP_OK;
}

doip_result_t doip_client_get_power_mode(doip_client_t *client,
                                          doip_power_mode_response_t *mode)
{
    if (!client || !mode || client->tcp_fd < 0)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t send_buf[DOIP_HEADER_SIZE];
    int msg_len = doip_build_message(DOIP_TYPE_DIAG_POWER_MODE_REQUEST,
                                      NULL, 0, send_buf, sizeof(send_buf));
    if (msg_len < 0)
        return (doip_result_t)msg_len;

    doip_result_t result = tcp_send_all(client->tcp_fd, send_buf, (size_t)msg_len);
    if (result != DOIP_OK)
        return result;

    doip_message_t msg;
    result = tcp_recv_doip_message(client->tcp_fd, client->recv_buf,
                                    sizeof(client->recv_buf), &msg,
                                    client->config.tcp_recv_timeout_ms);
    if (result != DOIP_OK)
        return result;

    if (msg.header.payload_type != DOIP_TYPE_DIAG_POWER_MODE_RESPONSE)
        return DOIP_ERR_NACK;

    *mode = msg.payload.power_mode;
    return DOIP_OK;
}

/* ============================================================================
 * UDS Helper Functions
 * ========================================================================== */

int doip_client_uds_session_control(doip_client_t *client, uint8_t session,
                                     uint8_t *response, uint32_t resp_size)
{
    uint8_t req[] = { 0x10, session };
    return doip_client_send_uds(client, req, sizeof(req), response, resp_size, 0);
}

doip_result_t doip_client_uds_tester_present(doip_client_t *client)
{
    uint8_t req[] = { 0x3E, 0x00 };
    uint8_t resp[8];
    int ret = doip_client_send_uds(client, req, sizeof(req), resp, sizeof(resp), 0);
    if (ret < 0)
        return (doip_result_t)ret;

    /* Check for positive response (0x7E) */
    if (ret >= 1 && resp[0] == 0x7E)
        return DOIP_OK;

    /* Check for NRC */
    if (ret >= 3 && resp[0] == 0x7F)
        return DOIP_ERR_NACK;

    return DOIP_OK;
}

int doip_client_uds_read_did(doip_client_t *client, uint16_t did,
                              uint8_t *response, uint32_t resp_size)
{
    uint8_t req[3] = { 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };
    return doip_client_send_uds(client, req, sizeof(req), response, resp_size, 0);
}

int doip_client_uds_security_seed(doip_client_t *client, uint8_t access_level,
                                   uint8_t *seed, uint32_t seed_size)
{
    uint8_t req[2] = { 0x27, access_level };
    return doip_client_send_uds(client, req, sizeof(req), seed, seed_size, 0);
}

doip_result_t doip_client_uds_security_key(doip_client_t *client, uint8_t access_level,
                                            const uint8_t *key, uint32_t key_len)
{
    if (!key || key_len == 0)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t *req = malloc(2 + key_len);
    if (!req)
        return DOIP_ERR_MEMORY;

    req[0] = 0x27;
    req[1] = access_level;
    memcpy(&req[2], key, key_len);

    uint8_t resp[8];
    int ret = doip_client_send_uds(client, req, 2 + key_len, resp, sizeof(resp), 0);
    free(req);

    if (ret < 0)
        return (doip_result_t)ret;

    if (ret >= 1 && resp[0] == 0x67)
        return DOIP_OK;
    if (ret >= 3 && resp[0] == 0x7F)
        return DOIP_ERR_NACK;

    return DOIP_OK;
}

int doip_client_uds_read_dtc(doip_client_t *client, uint8_t sub_func,
                              uint8_t *response, uint32_t resp_size)
{
    uint8_t req[3] = { 0x19, sub_func, 0xFF };  /* 0xFF = all DTC groups */
    return doip_client_send_uds(client, req, sizeof(req), response, resp_size, 0);
}

/* ============================================================================
 * Block Transfer / Flash Programming
 * ========================================================================== */

/**
 * Internal helper to build addressAndLengthFormatIdentifier and the
 * address + size fields for RequestDownload / RequestUpload.
 */
static int build_address_and_length(uint8_t *buf, size_t buf_size,
                                     uint32_t memory_address, uint8_t addr_len,
                                     uint32_t memory_size, uint8_t size_len)
{
    if (addr_len < 1 || addr_len > 4 || size_len < 1 || size_len > 4)
        return DOIP_ERR_INVALID_PARAM;

    size_t needed = 1 + (size_t)addr_len + (size_t)size_len;
    if (buf_size < needed)
        return DOIP_ERR_BUFFER_TOO_SMALL;

    /* addressAndLengthFormatIdentifier: high nibble = size bytes, low nibble = addr bytes */
    buf[0] = (uint8_t)((size_len << 4) | addr_len);

    /* Memory address (big-endian, variable length) */
    for (int i = addr_len - 1; i >= 0; i--) {
        buf[1 + (addr_len - 1 - i)] = (uint8_t)(memory_address >> (i * 8));
    }

    /* Memory size (big-endian, variable length) */
    for (int i = size_len - 1; i >= 0; i--) {
        buf[1 + addr_len + (size_len - 1 - i)] = (uint8_t)(memory_size >> (i * 8));
    }

    return (int)needed;
}

/**
 * Parse maxNumberOfBlockLength from RequestDownload/RequestUpload positive response
 */
static uint16_t parse_max_block_length(const uint8_t *resp, int resp_len)
{
    if (resp_len < 3)
        return 0;

    /* resp[0] = positive SID, resp[1] = lengthFormatIdentifier */
    uint8_t len_bytes = (resp[1] >> 4) & 0x0F;
    if (len_bytes == 0 || resp_len < 2 + (int)len_bytes)
        return 0;

    uint32_t max_block = 0;
    for (uint8_t i = 0; i < len_bytes; i++) {
        max_block = (max_block << 8) | resp[2 + i];
    }

    /* Clamp to uint16_t range */
    if (max_block > 0xFFFF)
        return 0xFFFF;

    return (uint16_t)max_block;
}

/**
 * Internal: Send RequestDownload (0x34) or RequestUpload (0x35)
 */
static doip_result_t send_request_transfer(doip_client_t *client, uint8_t sid,
                                            uint32_t memory_address, uint8_t addr_len,
                                            uint32_t memory_size, uint8_t size_len,
                                            const doip_data_format_t *data_format,
                                            uint16_t *max_block_length)
{
    if (!client || !max_block_length)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t req[32];
    size_t offset = 0;

    /* SID */
    req[offset++] = sid;

    /* dataFormatIdentifier */
    if (data_format) {
        req[offset++] = (uint8_t)((data_format->compression_method << 4) |
                                   (data_format->encrypting_method & 0x0F));
    } else {
        req[offset++] = 0x00;  /* No compression, no encryption */
    }

    /* addressAndLengthFormatIdentifier + address + size */
    int al_len = build_address_and_length(&req[offset], sizeof(req) - offset,
                                           memory_address, addr_len,
                                           memory_size, size_len);
    if (al_len < 0)
        return (doip_result_t)al_len;
    offset += (size_t)al_len;

    /* Send and receive */
    uint8_t resp[64];
    int resp_len = doip_client_send_uds(client, req, (uint32_t)offset,
                                         resp, sizeof(resp), 10000);
    if (resp_len < 0)
        return (doip_result_t)resp_len;

    /* Check for negative response */
    if (resp_len >= 3 && resp[0] == 0x7F) {
        printf("[DoIP Client] %s rejected: NRC=0x%02X\n",
               sid == 0x34 ? "RequestDownload" : "RequestUpload", resp[2]);
        return DOIP_ERR_NACK;
    }

    /* Check for positive response (0x74 for download, 0x75 for upload) */
    uint8_t expected_sid = sid + 0x40;
    if (resp[0] != expected_sid) {
        printf("[DoIP Client] Unexpected response SID: 0x%02X\n", resp[0]);
        return DOIP_ERR_NACK;
    }

    *max_block_length = parse_max_block_length(resp, resp_len);
    if (*max_block_length == 0) {
        printf("[DoIP Client] Failed to parse maxNumberOfBlockLength\n");
        return DOIP_ERR_INVALID_HEADER;
    }

    printf("[DoIP Client] %s accepted, maxBlockLength=%u bytes\n",
           sid == 0x34 ? "RequestDownload" : "RequestUpload", *max_block_length);

    return DOIP_OK;
}

doip_result_t doip_client_uds_request_download(doip_client_t *client,
                                                uint32_t memory_address,
                                                uint8_t addr_len_bytes,
                                                uint32_t memory_size,
                                                uint8_t size_len_bytes,
                                                const doip_data_format_t *data_format,
                                                uint16_t *max_block_length)
{
    return send_request_transfer(client, 0x34, memory_address, addr_len_bytes,
                                  memory_size, size_len_bytes, data_format,
                                  max_block_length);
}

doip_result_t doip_client_uds_request_upload(doip_client_t *client,
                                              uint32_t memory_address,
                                              uint8_t addr_len_bytes,
                                              uint32_t memory_size,
                                              uint8_t size_len_bytes,
                                              const doip_data_format_t *data_format,
                                              uint16_t *max_block_length)
{
    return send_request_transfer(client, 0x35, memory_address, addr_len_bytes,
                                  memory_size, size_len_bytes, data_format,
                                  max_block_length);
}

int doip_client_uds_transfer_data(doip_client_t *client,
                                   uint8_t block_sequence,
                                   const uint8_t *data, uint32_t data_len,
                                   uint8_t *response, uint32_t resp_size)
{
    if (!client || (!data && data_len > 0))
        return DOIP_ERR_INVALID_PARAM;

    /* Build TransferData request: SID(1) + blockSequenceCounter(1) + data */
    uint32_t req_len = 2 + data_len;
    uint8_t *req = malloc(req_len);
    if (!req)
        return DOIP_ERR_MEMORY;

    req[0] = 0x36;
    req[1] = block_sequence;
    if (data && data_len > 0)
        memcpy(&req[2], data, data_len);

    int ret = doip_client_send_uds(client, req, req_len, response, resp_size, 30000);
    free(req);
    return ret;
}

int doip_client_uds_request_transfer_exit(doip_client_t *client,
                                           const uint8_t *transfer_req_param,
                                           uint32_t param_len,
                                           uint8_t *response, uint32_t resp_size)
{
    uint32_t req_len = 1 + param_len;
    uint8_t *req = malloc(req_len);
    if (!req)
        return DOIP_ERR_MEMORY;

    req[0] = 0x37;
    if (transfer_req_param && param_len > 0)
        memcpy(&req[1], transfer_req_param, param_len);

    int ret = doip_client_send_uds(client, req, req_len, response, resp_size, 10000);
    free(req);
    return ret;
}

/* ============================================================================
 * High-Level Flash Download (client -> ECU)
 * ========================================================================== */

doip_result_t doip_client_flash_download(doip_client_t *client,
                                          uint32_t memory_address,
                                          uint8_t addr_len_bytes,
                                          const uint8_t *data, uint32_t data_len,
                                          const doip_data_format_t *data_format,
                                          doip_transfer_progress_cb_t progress_cb,
                                          void *user_data,
                                          doip_transfer_result_t *result)
{
    if (!client || !data || data_len == 0)
        return DOIP_ERR_INVALID_PARAM;

    doip_transfer_result_t res = { 0 };
    doip_result_t ret;

    /* Determine size_len_bytes from data_len */
    uint8_t size_len = 4;
    if (data_len <= 0xFF) size_len = 1;
    else if (data_len <= 0xFFFF) size_len = 2;
    else if (data_len <= 0xFFFFFF) size_len = 3;

    /* 1. RequestDownload */
    printf("[DoIP Flash] Requesting download: addr=0x%08X, size=%u bytes\n",
           memory_address, data_len);

    uint16_t max_block = 0;
    ret = doip_client_uds_request_download(client, memory_address, addr_len_bytes,
                                            data_len, size_len, data_format,
                                            &max_block);
    if (ret != DOIP_OK) {
        if (result) { res.success = false; *result = res; }
        return ret;
    }

    res.max_block_length = max_block;

    /* The max_block_length includes the SID + blockSequenceCounter overhead (2 bytes),
     * so the actual data per block is max_block - 2. */
    uint32_t chunk_size = max_block - 2;
    if (chunk_size == 0) {
        printf("[DoIP Flash] Invalid block length from ECU\n");
        if (result) { res.success = false; *result = res; }
        return DOIP_ERR_INVALID_HEADER;
    }

    /* 2. TransferData - send all blocks */
    uint8_t block_seq = 1;
    uint32_t offset = 0;
    uint8_t resp_buf[64];

    printf("[DoIP Flash] Transferring %u bytes in chunks of %u bytes...\n",
           data_len, chunk_size);

    while (offset < data_len) {
        uint32_t remaining = data_len - offset;
        uint32_t this_chunk = (remaining > chunk_size) ? chunk_size : remaining;

        int resp_len = doip_client_uds_transfer_data(client, block_seq,
                                                       &data[offset], this_chunk,
                                                       resp_buf, sizeof(resp_buf));
        if (resp_len < 0) {
            printf("[DoIP Flash] TransferData block %u failed: %s\n",
                   block_seq, doip_result_str((doip_result_t)resp_len));
            if (result) { res.success = false; *result = res; }
            return (doip_result_t)resp_len;
        }

        /* Check for NRC */
        if (resp_len >= 3 && resp_buf[0] == 0x7F) {
            /* NRC 0x78 = responsePending - keep waiting */
            if (resp_buf[2] == 0x78) {
                /* Re-receive the actual response with extended timeout */
                doip_message_t msg;
                ret = doip_client_recv_message(client, &msg, 60000);
                if (ret != DOIP_OK) {
                    if (result) { res.success = false; *result = res; }
                    return ret;
                }
                /* Check actual response */
                if (msg.header.payload_type == DOIP_TYPE_DIAGNOSTIC_MESSAGE) {
                    if (msg.payload.diagnostic.data_length >= 1 &&
                        msg.payload.diagnostic.data[0] == 0x7F) {
                        printf("[DoIP Flash] TransferData block %u NRC=0x%02X\n",
                               block_seq, msg.payload.diagnostic.data[2]);
                        if (result) { res.success = false; *result = res; }
                        return DOIP_ERR_NACK;
                    }
                }
            } else {
                printf("[DoIP Flash] TransferData block %u NRC=0x%02X\n",
                       block_seq, resp_buf[2]);
                if (result) { res.success = false; *result = res; }
                return DOIP_ERR_NACK;
            }
        }

        /* Verify positive response */
        if (resp_buf[0] != 0x76) {
            printf("[DoIP Flash] Unexpected TransferData response: 0x%02X\n", resp_buf[0]);
            if (result) { res.success = false; *result = res; }
            return DOIP_ERR_NACK;
        }

        offset += this_chunk;
        res.total_bytes = offset;
        res.num_blocks++;

        /* Wrap block sequence counter: 0x01..0xFF, then 0x00, 0x01, ... */
        block_seq++;
        /* block_seq wraps naturally at uint8_t boundary */

        /* Progress callback */
        if (progress_cb)
            progress_cb(offset, data_len, user_data);

        /* Periodic progress log */
        if (res.num_blocks % 100 == 0 || offset >= data_len) {
            printf("[DoIP Flash] Progress: %u / %u bytes (%u%%)\n",
                   offset, data_len, (offset * 100) / data_len);
        }
    }

    /* 3. RequestTransferExit */
    printf("[DoIP Flash] Transfer complete, sending RequestTransferExit...\n");
    int exit_len = doip_client_uds_request_transfer_exit(client, NULL, 0,
                                                           resp_buf, sizeof(resp_buf));
    if (exit_len < 0) {
        printf("[DoIP Flash] RequestTransferExit failed: %s\n",
               doip_result_str((doip_result_t)exit_len));
        if (result) { res.success = false; *result = res; }
        return (doip_result_t)exit_len;
    }

    if (resp_buf[0] == 0x7F) {
        printf("[DoIP Flash] RequestTransferExit NRC=0x%02X\n", resp_buf[2]);
        if (result) { res.success = false; *result = res; }
        return DOIP_ERR_NACK;
    }

    if (resp_buf[0] != 0x77) {
        printf("[DoIP Flash] Unexpected TransferExit response: 0x%02X\n", resp_buf[0]);
        if (result) { res.success = false; *result = res; }
        return DOIP_ERR_NACK;
    }

    res.success = true;
    printf("[DoIP Flash] Download complete: %u bytes in %u blocks\n",
           res.total_bytes, res.num_blocks);

    if (result)
        *result = res;

    return DOIP_OK;
}

/* ============================================================================
 * High-Level Flash Upload (ECU -> client)
 * ========================================================================== */

int doip_client_flash_upload(doip_client_t *client,
                              uint32_t memory_address,
                              uint8_t addr_len_bytes,
                              uint8_t *buffer, uint32_t buffer_size,
                              uint32_t read_size,
                              const doip_data_format_t *data_format,
                              doip_transfer_progress_cb_t progress_cb,
                              void *user_data,
                              doip_transfer_result_t *result)
{
    if (!client || !buffer || buffer_size == 0 || read_size == 0)
        return DOIP_ERR_INVALID_PARAM;

    doip_transfer_result_t res = { 0 };

    uint8_t size_len = 4;
    if (read_size <= 0xFF) size_len = 1;
    else if (read_size <= 0xFFFF) size_len = 2;
    else if (read_size <= 0xFFFFFF) size_len = 3;

    /* 1. RequestUpload */
    printf("[DoIP Flash] Requesting upload: addr=0x%08X, size=%u bytes\n",
           memory_address, read_size);

    uint16_t max_block = 0;
    doip_result_t ret = doip_client_uds_request_upload(client, memory_address,
                                                         addr_len_bytes,
                                                         read_size, size_len,
                                                         data_format, &max_block);
    if (ret != DOIP_OK) {
        if (result) { res.success = false; *result = res; }
        return (int)ret;
    }

    res.max_block_length = max_block;
    uint32_t chunk_size = max_block - 2;

    /* 2. TransferData - receive all blocks */
    uint8_t block_seq = 1;
    uint32_t total_received = 0;

    /* For upload, we send TransferData with just SID + blockSeq, ECU responds with data */
    uint8_t *resp_buf = malloc(max_block + 64);
    if (!resp_buf)
        return DOIP_ERR_MEMORY;

    printf("[DoIP Flash] Uploading %u bytes in chunks of up to %u...\n",
           read_size, chunk_size);

    while (total_received < read_size) {
        /* Send TransferData request (no data payload - we're receiving) */
        int resp_len = doip_client_uds_transfer_data(client, block_seq,
                                                       NULL, 0,
                                                       resp_buf, max_block + 64);
        if (resp_len < 0) {
            free(resp_buf);
            if (result) { res.success = false; *result = res; }
            return resp_len;
        }

        if (resp_buf[0] == 0x7F) {
            printf("[DoIP Flash] TransferData upload NRC=0x%02X\n", resp_buf[2]);
            free(resp_buf);
            if (result) { res.success = false; *result = res; }
            return DOIP_ERR_NACK;
        }

        if (resp_buf[0] != 0x76 || resp_len < 2) {
            free(resp_buf);
            if (result) { res.success = false; *result = res; }
            return DOIP_ERR_NACK;
        }

        /* Data starts at offset 2 (SID + blockSeq) */
        uint32_t data_in_block = (uint32_t)(resp_len - 2);
        if (total_received + data_in_block > buffer_size)
            data_in_block = buffer_size - total_received;

        memcpy(&buffer[total_received], &resp_buf[2], data_in_block);
        total_received += data_in_block;
        res.total_bytes = total_received;
        res.num_blocks++;

        block_seq++;

        if (progress_cb)
            progress_cb(total_received, read_size, user_data);

        if (data_in_block == 0)
            break;  /* ECU sent empty block = done */
    }

    free(resp_buf);

    /* 3. RequestTransferExit */
    uint8_t exit_resp[64];
    int exit_len = doip_client_uds_request_transfer_exit(client, NULL, 0,
                                                           exit_resp, sizeof(exit_resp));
    if (exit_len < 0) {
        if (result) { res.success = false; *result = res; }
        return exit_len;
    }

    if (exit_resp[0] != 0x77) {
        if (result) { res.success = false; *result = res; }
        return DOIP_ERR_NACK;
    }

    res.success = true;
    printf("[DoIP Flash] Upload complete: %u bytes in %u blocks\n",
           res.total_bytes, res.num_blocks);

    if (result)
        *result = res;

    return (int)total_received;
}

/* ============================================================================
 * Flash Preparation / Finalization Helpers
 * ========================================================================== */

doip_result_t doip_client_prepare_flash(doip_client_t *client,
                                         uint8_t security_level,
                                         doip_compute_key_cb_t compute_key,
                                         void *key_user_data)
{
    if (!client || !compute_key)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t resp[256];
    int ret;

    /* 1. Switch to programming session */
    printf("[DoIP Flash Prep] Switching to programming session...\n");
    ret = doip_client_uds_session_control(client, 0x02, resp, sizeof(resp));
    if (ret < 0) return (doip_result_t)ret;
    if (resp[0] == 0x7F) {
        printf("[DoIP Flash Prep] Session change failed: NRC=0x%02X\n", resp[2]);
        return DOIP_ERR_NACK;
    }

    /* 2. Disable DTC setting */
    printf("[DoIP Flash Prep] Disabling DTC setting...\n");
    uint8_t dtc_req[] = { 0x85, 0x02 };  /* off */
    ret = doip_client_send_uds(client, dtc_req, sizeof(dtc_req), resp, sizeof(resp), 0);
    if (ret < 0) return (doip_result_t)ret;
    if (resp[0] == 0x7F) {
        printf("[DoIP Flash Prep] ControlDTCSetting off warning: NRC=0x%02X (continuing)\n", resp[2]);
        /* Non-fatal - some ECUs don't support this */
    }

    /* 3. Disable communication */
    printf("[DoIP Flash Prep] Disabling non-diagnostic communication...\n");
    uint8_t comm_req[] = { 0x28, 0x03, 0x01 };  /* disableRxAndTx, normalMessage */
    ret = doip_client_send_uds(client, comm_req, sizeof(comm_req), resp, sizeof(resp), 0);
    if (ret < 0) return (doip_result_t)ret;
    if (resp[0] == 0x7F) {
        printf("[DoIP Flash Prep] CommunicationControl warning: NRC=0x%02X (continuing)\n", resp[2]);
    }

    /* 4. Security Access - request seed */
    printf("[DoIP Flash Prep] Security access (level 0x%02X)...\n", security_level);
    uint8_t seed[64];
    ret = doip_client_uds_security_seed(client, security_level, seed, sizeof(seed));
    if (ret < 0) return (doip_result_t)ret;
    if (seed[0] == 0x7F) {
        printf("[DoIP Flash Prep] SecurityAccess seed failed: NRC=0x%02X\n", seed[2]);
        return DOIP_ERR_NACK;
    }

    /* Extract seed data (skip SID + subfunction) */
    if (ret < 3) return DOIP_ERR_NACK;
    uint32_t seed_data_len = (uint32_t)(ret - 2);
    uint8_t *seed_data = &seed[2];

    /* Compute key using callback */
    uint8_t key[64];
    uint32_t key_len = sizeof(key);
    doip_result_t key_ret = compute_key(seed_data, seed_data_len,
                                         key, &key_len, key_user_data);
    if (key_ret != DOIP_OK) {
        printf("[DoIP Flash Prep] Key computation failed\n");
        return key_ret;
    }

    /* Send key */
    key_ret = doip_client_uds_security_key(client, security_level + 1, key, key_len);
    if (key_ret != DOIP_OK) {
        printf("[DoIP Flash Prep] SecurityAccess key rejected\n");
        return key_ret;
    }

    printf("[DoIP Flash Prep] ECU ready for flashing\n");
    return DOIP_OK;
}

doip_result_t doip_client_finalize_flash(doip_client_t *client)
{
    if (!client)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t resp[64];
    int ret;

    /* 1. Re-enable DTC setting */
    printf("[DoIP Flash Final] Re-enabling DTC setting...\n");
    uint8_t dtc_req[] = { 0x85, 0x01 };  /* on */
    ret = doip_client_send_uds(client, dtc_req, sizeof(dtc_req), resp, sizeof(resp), 0);
    if (ret < 0) {
        printf("[DoIP Flash Final] ControlDTCSetting warning: %s\n",
               doip_result_str((doip_result_t)ret));
    }

    /* 2. Re-enable communication */
    printf("[DoIP Flash Final] Re-enabling communication...\n");
    uint8_t comm_req[] = { 0x28, 0x00, 0x01 };  /* enableRxAndTx, normalMessage */
    ret = doip_client_send_uds(client, comm_req, sizeof(comm_req), resp, sizeof(resp), 0);
    if (ret < 0) {
        printf("[DoIP Flash Final] CommunicationControl warning: %s\n",
               doip_result_str((doip_result_t)ret));
    }

    /* 3. ECU Reset - hard reset */
    printf("[DoIP Flash Final] Performing hard reset...\n");
    uint8_t reset_req[] = { 0x11, 0x01 };
    ret = doip_client_send_uds(client, reset_req, sizeof(reset_req), resp, sizeof(resp), 5000);
    if (ret >= 1 && resp[0] == 0x51) {
        printf("[DoIP Flash Final] ECU reset successful\n");
    } else if (ret < 0) {
        /* Connection may drop during reset - that's expected */
        printf("[DoIP Flash Final] ECU reset sent (connection may have dropped)\n");
    }

    return DOIP_OK;
}
