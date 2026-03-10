/**
 * @file doip_server.h
 * @brief DoIP Server/Gateway API - Vehicle-side DoIP entity
 *
 * Implements:
 * - UDP vehicle identification responder
 * - TCP diagnostic server (multi-client)
 * - Routing activation handling
 * - Diagnostic message routing via callbacks
 * - Alive check
 */

#ifndef DOIP_SERVER_H
#define DOIP_SERVER_H

#include "doip.h"
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ========================================================================== */

#define DOIP_SERVER_MAX_CLIENTS     8
#define DOIP_SERVER_MAX_TARGETS     32

/** Registered target ECU info */
typedef struct {
    uint16_t    logical_address;
    bool        active;
} doip_target_entry_t;

/** Server configuration */
typedef struct {
    /* Identity */
    uint8_t     vin[DOIP_VIN_LENGTH];
    uint16_t    logical_address;             /* This entity's logical address */
    uint8_t     eid[DOIP_EID_LENGTH];        /* Entity ID (MAC address) */
    uint8_t     gid[DOIP_GID_LENGTH];        /* Group ID */
    uint8_t     further_action;
    uint8_t     vin_gid_sync_status;

    /* Network */
    const char *bind_address;                /* NULL for INADDR_ANY */
    uint16_t    tcp_port;                    /* 0 for default 13400 */
    uint16_t    udp_port;                    /* 0 for default 13400 */

    /* Limits */
    uint8_t     max_tcp_connections;
    uint32_t    max_data_size;

    /* Known target addresses */
    doip_target_entry_t targets[DOIP_SERVER_MAX_TARGETS];
    int         num_targets;
} doip_server_config_t;

/* ============================================================================
 * Callback Types
 * ========================================================================== */

/** Forward declaration */
typedef struct doip_server doip_server_t;

/**
 * @brief Callback for routing activation requests
 * @param server        Server handle
 * @param client_fd     Client socket fd
 * @param source_addr   Tester logical address
 * @param act_type      Activation type
 * @return Response code (DOIP_ROUTING_ACTIVATION_SUCCESS to accept)
 */
typedef uint8_t (*doip_routing_activation_cb_t)(doip_server_t *server, int client_fd,
                                                  uint16_t source_addr, uint8_t act_type);

/**
 * @brief Callback for incoming diagnostic messages
 * @param server        Server handle
 * @param source_addr   Source (tester) logical address
 * @param target_addr   Target (ECU) logical address
 * @param uds_data      UDS request data
 * @param uds_len       UDS request length
 * @param response      Buffer for UDS response (pre-allocated)
 * @param resp_size     Size of response buffer
 * @return Length of UDS response, 0 if no response, or negative error code
 */
typedef int (*doip_diagnostic_cb_t)(doip_server_t *server,
                                     uint16_t source_addr, uint16_t target_addr,
                                     const uint8_t *uds_data, uint32_t uds_len,
                                     uint8_t *response, uint32_t resp_size);

/* ============================================================================
 * Client Connection State
 * ========================================================================== */

typedef struct {
    int         fd;
    bool        active;
    bool        routing_activated;
    uint16_t    tester_address;
    struct sockaddr_in peer_addr;
    pthread_t   thread;
} doip_client_connection_t;

/* ============================================================================
 * Server Handle
 * ========================================================================== */

struct doip_server {
    doip_server_config_t        config;
    int                         tcp_fd;
    int                         udp_fd;
    _Atomic bool                running;

    /* Client connections */
    doip_client_connection_t    clients[DOIP_SERVER_MAX_CLIENTS];
    int                         num_clients;
    pthread_mutex_t             clients_mutex;

    /* Callbacks */
    doip_routing_activation_cb_t    on_routing_activation;
    doip_diagnostic_cb_t            on_diagnostic_message;

    /* Server threads */
    pthread_t                   tcp_accept_thread;
    pthread_t                   udp_thread;

    /* User context */
    void                       *user_data;
};

/* ============================================================================
 * Server Lifecycle
 * ========================================================================== */

/**
 * @brief Initialize the DoIP server
 * @param server    Server handle
 * @param config    Server configuration
 * @return DOIP_OK on success
 */
doip_result_t doip_server_init(doip_server_t *server, const doip_server_config_t *config);

/**
 * @brief Set the routing activation callback
 */
void doip_server_set_routing_callback(doip_server_t *server,
                                       doip_routing_activation_cb_t cb);

/**
 * @brief Set the diagnostic message callback
 */
void doip_server_set_diagnostic_callback(doip_server_t *server,
                                          doip_diagnostic_cb_t cb);

/**
 * @brief Set user context data
 */
void doip_server_set_user_data(doip_server_t *server, void *user_data);

/**
 * @brief Start the DoIP server (non-blocking, spawns threads)
 * @param server    Server handle
 * @return DOIP_OK on success
 */
doip_result_t doip_server_start(doip_server_t *server);

/**
 * @brief Stop the DoIP server and close all connections
 * @param server    Server handle
 */
void doip_server_stop(doip_server_t *server);

/**
 * @brief Destroy the server and free resources
 * @param server    Server handle
 */
void doip_server_destroy(doip_server_t *server);

/**
 * @brief Register a target ECU address
 * @param server    Server handle
 * @param address   Logical address of target ECU
 * @return DOIP_OK on success
 */
doip_result_t doip_server_register_target(doip_server_t *server, uint16_t address);

/**
 * @brief Send a vehicle announcement via UDP broadcast
 * @param server    Server handle
 * @return DOIP_OK on success
 */
doip_result_t doip_server_send_announcement(doip_server_t *server);

/**
 * @brief Send an alive check to a specific client
 * @param server    Server handle
 * @param client_fd Client socket fd
 * @return DOIP_OK on success
 */
doip_result_t doip_server_send_alive_check(doip_server_t *server, int client_fd);

#ifdef __cplusplus
}
#endif

#endif /* DOIP_SERVER_H */
