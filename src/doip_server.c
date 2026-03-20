/**
 * @file doip_server.c
 * @brief DoIP Server/Gateway implementation
 *
 * Multi-threaded server supporting:
 * - UDP vehicle identification responder
 * - TCP multi-client handling
 * - Routing activation
 * - Diagnostic message routing via callbacks
 * - Alive check
 */

#include "doip_server.h"
#include "doip_log.h"
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

static int tcp_recv_exact(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t received = 0;
    struct timeval tv;
    fd_set fds;

    while (received < len) {
        if (fd >= FD_SETSIZE) {
            LOG_ERROR("fd %d exceeds FD_SETSIZE", fd);
            return DOIP_ERR_RECV;
        }
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

static bool is_known_target(doip_server_t *server, uint16_t address)
{
    /* Check if target address is this server itself */
    if (address == server->config.logical_address)
        return true;

    for (int i = 0; i < server->config.num_targets; i++) {
        if (server->config.targets[i].active &&
            server->config.targets[i].logical_address == address)
            return true;
    }
    return false;
}

typedef struct {
    doip_server_t *server;
    doip_client_connection_t *conn;
} handler_args_t;

/* ============================================================================
 * Client Handler Thread
 * ========================================================================== */

/**
 * Handle a single TCP client connection
 */
static void *client_handler_thread(void *arg)
{
    handler_args_t *args = (handler_args_t *)arg;
    doip_server_t *server = args->server;
    doip_client_connection_t *conn = args->conn;
    free(args);

    char peer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &conn->peer_addr.sin_addr, peer_ip, sizeof(peer_ip));
    LOG_INFO("Client connected: %s:%d (fd=%d)",
           peer_ip, ntohs(conn->peer_addr.sin_port), conn->fd);

    uint8_t recv_buf[DOIP_HEADER_SIZE + DOIP_MAX_DIAGNOSTIC_SIZE];
    uint8_t send_buf[DOIP_HEADER_SIZE + DOIP_MAX_DIAGNOSTIC_SIZE];

    while (server->running && conn->active) {
        /* Receive header */
        int ret = tcp_recv_exact(conn->fd, recv_buf, DOIP_HEADER_SIZE, 30000);
        if (ret == DOIP_ERR_TIMEOUT)
            continue;
        if (ret < 0) {
            LOG_WARN("Client fd=%d receive error: %s",
                   conn->fd, doip_result_str((doip_result_t)ret));
            break;
        }

        /* Parse header */
        doip_header_t header;
        if (doip_deserialize_header(recv_buf, DOIP_HEADER_SIZE, &header) != DOIP_OK) {
            int nack_len = doip_build_header_nack(DOIP_HEADER_NACK_INCORRECT_PATTERN,
                                                    send_buf, sizeof(send_buf));
            if (nack_len > 0)
                tcp_send_all(conn->fd, send_buf, (size_t)nack_len);
            break;
        }

        if (doip_validate_header(&header) != DOIP_OK) {
            int nack_len = doip_build_header_nack(DOIP_HEADER_NACK_INCORRECT_PATTERN,
                                                    send_buf, sizeof(send_buf));
            if (nack_len > 0)
                tcp_send_all(conn->fd, send_buf, (size_t)nack_len);
            break;
        }

        /* Receive payload */
        if (header.payload_length > 0) {
            if (header.payload_length > DOIP_MAX_DIAGNOSTIC_SIZE) {
                int nack_len = doip_build_header_nack(DOIP_HEADER_NACK_MESSAGE_TOO_LARGE,
                                                        send_buf, sizeof(send_buf));
                if (nack_len > 0)
                    tcp_send_all(conn->fd, send_buf, (size_t)nack_len);
                continue;
            }

            ret = tcp_recv_exact(conn->fd, recv_buf + DOIP_HEADER_SIZE,
                                 header.payload_length, DOIP_TCP_GENERAL_TIMEOUT);
            if (ret < 0)
                break;
        }

        /* Parse complete message */
        doip_message_t msg;
        size_t total_len = DOIP_HEADER_SIZE + header.payload_length;
        if (doip_parse_message(recv_buf, total_len, &msg) != DOIP_OK) {
            int nack_len = doip_build_header_nack(DOIP_HEADER_NACK_INVALID_PAYLOAD_LENGTH,
                                                    send_buf, sizeof(send_buf));
            if (nack_len > 0)
                tcp_send_all(conn->fd, send_buf, (size_t)nack_len);
            continue;
        }

        LOG_INFO("Received from fd=%d: %s",
               conn->fd, doip_payload_type_str((doip_payload_type_t)msg.header.payload_type));

        /* Handle message by type */
        switch (msg.header.payload_type) {

        case DOIP_TYPE_ROUTING_ACTIVATION_REQUEST: {
            doip_routing_activation_request_t *req = &msg.payload.routing_req;

            uint8_t response_code = DOIP_ROUTING_ACTIVATION_SUCCESS;

            /* Call user callback if registered */
            if (server->on_routing_activation) {
                response_code = server->on_routing_activation(
                    server, conn->fd, req->source_address, req->activation_type);
            }

            /* Build response */
            doip_routing_activation_response_t resp = {
                .tester_logical_address = req->source_address,
                .entity_logical_address = server->config.logical_address,
                .response_code = response_code,
                .reserved = 0,
                .has_oem_specific = false,
            };

            int resp_len = doip_build_routing_activation_response(&resp, send_buf,
                                                                    sizeof(send_buf));
            if (resp_len > 0) {
                tcp_send_all(conn->fd, send_buf, (size_t)resp_len);
            }

            if (response_code == DOIP_ROUTING_ACTIVATION_SUCCESS) {
                conn->routing_activated = true;
                conn->tester_address = req->source_address;
                LOG_INFO("Routing activated for tester 0x%04X on fd=%d",
                       req->source_address, conn->fd);
            }
            break;
        }

        case DOIP_TYPE_DIAGNOSTIC_MESSAGE: {
            doip_diagnostic_message_t *diag = &msg.payload.diagnostic;

            /* Verify routing is activated */
            if (!conn->routing_activated) {
                LOG_WARN("Diagnostic msg rejected - routing not active");
                break;
            }

            /* Verify source address matches */
            if (diag->source_address != conn->tester_address) {
                int nack_len = doip_build_diagnostic_nack(
                    server->config.logical_address, diag->source_address,
                    DOIP_DIAG_NACK_INVALID_SA, send_buf, sizeof(send_buf));
                if (nack_len > 0)
                    tcp_send_all(conn->fd, send_buf, (size_t)nack_len);
                break;
            }

            /* Check target is known */
            if (!is_known_target(server, diag->target_address)) {
                int nack_len = doip_build_diagnostic_nack(
                    server->config.logical_address, diag->source_address,
                    DOIP_DIAG_NACK_UNKNOWN_TA, send_buf, sizeof(send_buf));
                if (nack_len > 0)
                    tcp_send_all(conn->fd, send_buf, (size_t)nack_len);
                break;
            }

            /* Send positive ACK */
            int ack_len = doip_build_diagnostic_ack(
                diag->target_address, diag->source_address,
                DOIP_DIAG_ACK_CONFIRMED, send_buf, sizeof(send_buf));
            if (ack_len > 0)
                tcp_send_all(conn->fd, send_buf, (size_t)ack_len);

            /* Call diagnostic callback */
            if (server->on_diagnostic_message) {
                uint8_t uds_resp[DOIP_MAX_DIAGNOSTIC_SIZE];
                int uds_resp_len = server->on_diagnostic_message(
                    server,
                    diag->source_address, diag->target_address,
                    diag->data, diag->data_length,
                    uds_resp, sizeof(uds_resp));

                if (uds_resp_len > 0) {
                    /* Send diagnostic response back */
                    int resp_len = doip_build_diagnostic_message(
                        diag->target_address, diag->source_address,
                        uds_resp, (uint32_t)uds_resp_len,
                        send_buf, sizeof(send_buf));
                    if (resp_len > 0)
                        tcp_send_all(conn->fd, send_buf, (size_t)resp_len);
                }
            }
            break;
        }

        case DOIP_TYPE_ALIVE_CHECK_RESPONSE:
            LOG_INFO("Alive check response from tester 0x%04X",
                   msg.payload.alive_check.source_address);
            break;

        case DOIP_TYPE_ENTITY_STATUS_REQUEST: {
            pthread_mutex_lock(&server->clients_mutex);
            uint8_t open_sockets = (uint8_t)server->num_clients;
            pthread_mutex_unlock(&server->clients_mutex);

            doip_entity_status_response_t status = {
                .node_type = 0,  /* Gateway */
                .max_concurrent_sockets = server->config.max_tcp_connections,
                .currently_open_sockets = open_sockets,
                .max_data_size = server->config.max_data_size,
                .has_max_data_size = true,
            };

            int resp_len = doip_build_entity_status_response(&status, send_buf,
                                                               sizeof(send_buf));
            if (resp_len > 0)
                tcp_send_all(conn->fd, send_buf, (size_t)resp_len);
            break;
        }

        case DOIP_TYPE_DIAG_POWER_MODE_REQUEST: {
            uint8_t payload[] = { 0x01 };  /* Ready */
            int resp_len = doip_build_message(DOIP_TYPE_DIAG_POWER_MODE_RESPONSE,
                                               payload, 1, send_buf, sizeof(send_buf));
            if (resp_len > 0)
                tcp_send_all(conn->fd, send_buf, (size_t)resp_len);
            break;
        }

        default: {
            int nack_len = doip_build_header_nack(DOIP_HEADER_NACK_UNKNOWN_PAYLOAD_TYPE,
                                                    send_buf, sizeof(send_buf));
            if (nack_len > 0)
                tcp_send_all(conn->fd, send_buf, (size_t)nack_len);
            break;
        }
        }
    }

    /* Cleanup */
    LOG_INFO("Client disconnected: fd=%d", conn->fd);
    close(conn->fd);

    pthread_mutex_lock(&server->clients_mutex);
    conn->active = false;
    conn->fd = -1;
    conn->routing_activated = false;
    server->num_clients--;
    pthread_mutex_unlock(&server->clients_mutex);

    pthread_detach(pthread_self());
    return NULL;
}

/* ============================================================================
 * TCP Accept Thread
 * ========================================================================== */

static void *tcp_accept_thread(void *arg)
{
    doip_server_t *server = (doip_server_t *)arg;

    LOG_INFO("TCP accept thread started on port %u",
           server->config.tcp_port ? server->config.tcp_port : DOIP_TCP_DATA_PORT);

    while (server->running) {
        /* Use select with timeout to allow checking server->running */
        if (server->tcp_fd >= FD_SETSIZE) {
            LOG_ERROR("tcp_fd %d exceeds FD_SETSIZE", server->tcp_fd);
            break;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server->tcp_fd, &fds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(server->tcp_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0)
            continue;

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server->tcp_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("[DoIP Server] accept");
            continue;
        }

        /* Disable Nagle */
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        /* Find a free slot */
        pthread_mutex_lock(&server->clients_mutex);
        int slot = -1;
        for (int i = 0; i < DOIP_SERVER_MAX_CLIENTS; i++) {
            if (!server->clients[i].active) {
                slot = i;
                break;
            }
        }

        if (slot < 0 || server->num_clients >= server->config.max_tcp_connections) {
            pthread_mutex_unlock(&server->clients_mutex);
            LOG_WARN("Max clients reached, rejecting connection");
            close(client_fd);
            continue;
        }

        doip_client_connection_t *conn = &server->clients[slot];
        conn->fd = client_fd;
        conn->active = true;
        conn->routing_activated = false;
        conn->peer_addr = client_addr;
        conn->tester_address = 0;
        server->num_clients++;
        pthread_mutex_unlock(&server->clients_mutex);

        /* Create handler thread */
        handler_args_t *args = malloc(sizeof(handler_args_t));
        if (!args) {
            close(client_fd);
            pthread_mutex_lock(&server->clients_mutex);
            conn->active = false;
            server->num_clients--;
            pthread_mutex_unlock(&server->clients_mutex);
            continue;
        }
        args->server = server;
        args->conn = conn;

        if (pthread_create(&conn->thread, NULL, client_handler_thread, args) != 0) {
            free(args);
            close(client_fd);
            pthread_mutex_lock(&server->clients_mutex);
            conn->active = false;
            server->num_clients--;
            pthread_mutex_unlock(&server->clients_mutex);
            perror("[DoIP Server] pthread_create");
        }
    }

    return NULL;
}

/* ============================================================================
 * UDP Handler Thread
 * ========================================================================== */

static void *udp_handler_thread(void *arg)
{
    doip_server_t *server = (doip_server_t *)arg;

    LOG_INFO("UDP handler thread started on port %u",
           server->config.udp_port ? server->config.udp_port : DOIP_UDP_DISCOVERY_PORT);

    uint8_t recv_buf[512];
    uint8_t send_buf[512];

    while (server->running) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        /* Use select for timeout */
        if (server->udp_fd >= FD_SETSIZE) {
            LOG_ERROR("udp_fd %d exceeds FD_SETSIZE", server->udp_fd);
            break;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server->udp_fd, &fds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(server->udp_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0)
            continue;

        ssize_t n = recvfrom(server->udp_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0)
            continue;

        doip_message_t msg;
        if (doip_parse_message(recv_buf, (size_t)n, &msg) != DOIP_OK)
            continue;

        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from_addr.sin_addr, peer_ip, sizeof(peer_ip));

        LOG_INFO("UDP from %s:%d: %s",
               peer_ip, ntohs(from_addr.sin_port),
               doip_payload_type_str((doip_payload_type_t)msg.header.payload_type));

        switch (msg.header.payload_type) {
        case DOIP_TYPE_VEHICLE_ID_REQUEST:
        case DOIP_TYPE_VEHICLE_ID_REQUEST_EID:
        case DOIP_TYPE_VEHICLE_ID_REQUEST_VIN: {
            /* Check VIN/EID filter if applicable */
            if (msg.header.payload_type == DOIP_TYPE_VEHICLE_ID_REQUEST_VIN &&
                msg.header.payload_length >= DOIP_VIN_LENGTH) {
                if (memcmp(msg.raw_payload, server->config.vin, DOIP_VIN_LENGTH) != 0) {
                    break;  /* VIN doesn't match */
                }
            }

            if (msg.header.payload_type == DOIP_TYPE_VEHICLE_ID_REQUEST_EID &&
                msg.header.payload_length >= DOIP_EID_LENGTH) {
                if (memcmp(msg.raw_payload, server->config.eid, DOIP_EID_LENGTH) != 0) {
                    break;  /* EID doesn't match */
                }
            }

            /* Build and send announcement */
            doip_vehicle_id_response_t vehicle = {
                .logical_address = server->config.logical_address,
                .further_action_required = server->config.further_action,
                .vin_gid_sync_status = server->config.vin_gid_sync_status,
                .has_sync_status = true,
            };
            memcpy(vehicle.vin, server->config.vin, DOIP_VIN_LENGTH);
            memcpy(vehicle.eid, server->config.eid, DOIP_EID_LENGTH);
            memcpy(vehicle.gid, server->config.gid, DOIP_GID_LENGTH);

            int resp_len = doip_build_vehicle_announcement(&vehicle, send_buf, sizeof(send_buf));
            if (resp_len > 0) {
                sendto(server->udp_fd, send_buf, (size_t)resp_len, 0,
                       (struct sockaddr *)&from_addr, from_len);
            }
            break;
        }

        default:
            break;
        }
    }

    return NULL;
}

/* ============================================================================
 * Server Lifecycle
 * ========================================================================== */

doip_result_t doip_server_init(doip_server_t *server, const doip_server_config_t *config)
{
    if (!server || !config)
        return DOIP_ERR_INVALID_PARAM;

    memset(server, 0, sizeof(doip_server_t));
    server->config = *config;
    server->tcp_fd = -1;
    server->udp_fd = -1;
    server->running = false;
    server->num_clients = 0;

    if (server->config.tcp_port == 0)
        server->config.tcp_port = DOIP_TCP_DATA_PORT;
    if (server->config.udp_port == 0)
        server->config.udp_port = DOIP_UDP_DISCOVERY_PORT;
    if (server->config.max_tcp_connections == 0)
        server->config.max_tcp_connections = DOIP_SERVER_MAX_CLIENTS;
    if (server->config.max_data_size == 0)
        server->config.max_data_size = DOIP_MAX_DIAGNOSTIC_SIZE;

    /* Initialize client slots */
    for (int i = 0; i < DOIP_SERVER_MAX_CLIENTS; i++) {
        server->clients[i].fd = -1;
        server->clients[i].active = false;
    }

    pthread_mutex_init(&server->clients_mutex, NULL);

    return DOIP_OK;
}

void doip_server_set_routing_callback(doip_server_t *server, doip_routing_activation_cb_t cb)
{
    if (server)
        server->on_routing_activation = cb;
}

void doip_server_set_diagnostic_callback(doip_server_t *server, doip_diagnostic_cb_t cb)
{
    if (server)
        server->on_diagnostic_message = cb;
}

void doip_server_set_user_data(doip_server_t *server, void *user_data)
{
    if (server)
        server->user_data = user_data;
}

doip_result_t doip_server_start(doip_server_t *server)
{
    if (!server)
        return DOIP_ERR_INVALID_PARAM;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    if (server->config.bind_address) {
        addr.sin_addr.s_addr = inet_addr(server->config.bind_address);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    /* Create and bind TCP socket */
    server->tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->tcp_fd < 0)
        return DOIP_ERR_SOCKET;

    int reuse = 1;
    setsockopt(server->tcp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addr.sin_port = htons(server->config.tcp_port);
    if (bind(server->tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[DoIP Server] TCP bind");
        close(server->tcp_fd);
        server->tcp_fd = -1;
        return DOIP_ERR_SOCKET;
    }

    if (listen(server->tcp_fd, DOIP_SERVER_MAX_CLIENTS) < 0) {
        perror("[DoIP Server] TCP listen");
        close(server->tcp_fd);
        server->tcp_fd = -1;
        return DOIP_ERR_SOCKET;
    }

    /* Create and bind UDP socket */
    server->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->udp_fd < 0) {
        close(server->tcp_fd);
        server->tcp_fd = -1;
        return DOIP_ERR_SOCKET;
    }

    setsockopt(server->udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int broadcast = 1;
    setsockopt(server->udp_fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    /* UDP must bind to INADDR_ANY to receive broadcast packets (255.255.255.255).
     * Binding to a specific IP would silently drop broadcast discovery requests.
     * TCP stays bound to bind_address for connection filtering. */
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(server->config.udp_port);
    if (bind(server->udp_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("[DoIP Server] UDP bind");
        close(server->tcp_fd);
        close(server->udp_fd);
        server->tcp_fd = -1;
        server->udp_fd = -1;
        return DOIP_ERR_SOCKET;
    }

    server->running = true;

    /* Start accept thread */
    if (pthread_create(&server->tcp_accept_thread, NULL, tcp_accept_thread, server) != 0) {
        server->running = false;
        close(server->tcp_fd);
        close(server->udp_fd);
        return DOIP_ERR_SOCKET;
    }

    /* Start UDP thread */
    if (pthread_create(&server->udp_thread, NULL, udp_handler_thread, server) != 0) {
        server->running = false;
        close(server->tcp_fd);
        close(server->udp_fd);
        pthread_join(server->tcp_accept_thread, NULL);
        return DOIP_ERR_SOCKET;
    }

    LOG_INFO("Started (TCP:%u, UDP:%u, LogAddr:0x%04X)",
           server->config.tcp_port, server->config.udp_port,
           server->config.logical_address);

    return DOIP_OK;
}

void doip_server_stop(doip_server_t *server)
{
    if (!server || !server->running)
        return;

    LOG_INFO("Stopping...");
    server->running = false;

    /* Close listening sockets to unblock accept/recvfrom */
    if (server->tcp_fd >= 0) {
        close(server->tcp_fd);
        server->tcp_fd = -1;
    }
    if (server->udp_fd >= 0) {
        close(server->udp_fd);
        server->udp_fd = -1;
    }

    /* Wait for threads */
    pthread_join(server->tcp_accept_thread, NULL);
    pthread_join(server->udp_thread, NULL);

    /* Close all client connections to unblock recv in handler threads */
    pthread_mutex_lock(&server->clients_mutex);
    pthread_t active_threads[DOIP_SERVER_MAX_CLIENTS];
    int num_active = 0;
    for (int i = 0; i < DOIP_SERVER_MAX_CLIENTS; i++) {
        if (server->clients[i].active) {
            active_threads[num_active++] = server->clients[i].thread;
            close(server->clients[i].fd);
            server->clients[i].fd = -1;
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);

    /* Join all client handler threads (may be detached — EINVAL is benign) */
    for (int i = 0; i < num_active; i++) {
        pthread_join(active_threads[i], NULL);  /* EINVAL if already detached */
    }

    /* Final cleanup under lock */
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < DOIP_SERVER_MAX_CLIENTS; i++) {
        server->clients[i].active = false;
        server->clients[i].fd = -1;
    }
    server->num_clients = 0;
    pthread_mutex_unlock(&server->clients_mutex);

    LOG_INFO("Stopped");
}

void doip_server_destroy(doip_server_t *server)
{
    if (!server)
        return;

    doip_server_stop(server);
    pthread_mutex_destroy(&server->clients_mutex);
}

doip_result_t doip_server_register_target(doip_server_t *server, uint16_t address)
{
    if (!server)
        return DOIP_ERR_INVALID_PARAM;

    if (server->config.num_targets >= DOIP_SERVER_MAX_TARGETS)
        return DOIP_ERR_MEMORY;

    server->config.targets[server->config.num_targets].logical_address = address;
    server->config.targets[server->config.num_targets].active = true;
    server->config.num_targets++;

    return DOIP_OK;
}

doip_result_t doip_server_send_announcement(doip_server_t *server)
{
    if (!server || server->udp_fd < 0)
        return DOIP_ERR_INVALID_PARAM;

    doip_vehicle_id_response_t vehicle = {
        .logical_address = server->config.logical_address,
        .further_action_required = server->config.further_action,
        .vin_gid_sync_status = server->config.vin_gid_sync_status,
        .has_sync_status = true,
    };
    memcpy(vehicle.vin, server->config.vin, DOIP_VIN_LENGTH);
    memcpy(vehicle.eid, server->config.eid, DOIP_EID_LENGTH);
    memcpy(vehicle.gid, server->config.gid, DOIP_GID_LENGTH);

    uint8_t send_buf[256];
    int msg_len = doip_build_vehicle_announcement(&vehicle, send_buf, sizeof(send_buf));
    if (msg_len < 0)
        return (doip_result_t)msg_len;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(server->config.udp_port);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    if (sendto(server->udp_fd, send_buf, (size_t)msg_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        return DOIP_ERR_SEND;
    }

    return DOIP_OK;
}

doip_result_t doip_server_send_alive_check(doip_server_t *server, int client_fd)
{
    if (!server)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t send_buf[DOIP_HEADER_SIZE];
    int msg_len = doip_build_alive_check_request(send_buf, sizeof(send_buf));
    if (msg_len < 0)
        return (doip_result_t)msg_len;

    return tcp_send_all(client_fd, send_buf, (size_t)msg_len);
}
