/**
 * @file doip_client.h
 * @brief DoIP Client API - Vehicle discovery and diagnostic communication
 *
 * Provides high-level client operations:
 * - UDP vehicle discovery (broadcast)
 * - TCP connection management
 * - Routing activation
 * - UDS diagnostic message exchange
 * - Alive check handling
 */

#ifndef DOIP_CLIENT_H
#define DOIP_CLIENT_H

#include "doip.h"
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Client Configuration
 * ========================================================================== */

typedef struct {
    uint16_t    tester_address;              /* Tester logical address (source) */
    uint16_t    ecu_address;                 /* Target ECU logical address */
    uint8_t     activation_type;             /* Routing activation type */
    int         tcp_connect_timeout_ms;      /* TCP connection timeout */
    int         tcp_recv_timeout_ms;         /* TCP receive timeout */
    int         udp_recv_timeout_ms;         /* UDP receive timeout */
    uint8_t     protocol_version;            /* DoIP protocol version */
} doip_client_config_t;

/** Default client configuration */
#define DOIP_CLIENT_CONFIG_DEFAULT { \
    .tester_address         = 0x0E80,       \
    .ecu_address            = 0x0001,       \
    .activation_type        = DOIP_ROUTING_ACTIVATION_DEFAULT, \
    .tcp_connect_timeout_ms = DOIP_TCP_GENERAL_TIMEOUT,        \
    .tcp_recv_timeout_ms    = DOIP_DIAGNOSTIC_TIMEOUT,         \
    .udp_recv_timeout_ms    = DOIP_CTRL_TIMEOUT,               \
    .protocol_version       = DOIP_PROTOCOL_VERSION,           \
}

/* ============================================================================
 * Client Handle
 * ========================================================================== */

typedef struct {
    doip_client_config_t config;
    int                  tcp_fd;             /* TCP socket fd (-1 if not connected) */
    int                  udp_fd;             /* UDP socket fd (-1 if not open) */
    struct sockaddr_in   server_addr;        /* Server address */
    bool                 routing_active;     /* Routing activation status */
    uint8_t              recv_buf[DOIP_HEADER_SIZE + DOIP_MAX_DIAGNOSTIC_SIZE];
} doip_client_t;

/* ============================================================================
 * Client Lifecycle
 * ========================================================================== */

/**
 * @brief Initialize a DoIP client
 * @param client    Client handle
 * @param config    Configuration (NULL for defaults)
 * @return DOIP_OK on success
 */
doip_result_t doip_client_init(doip_client_t *client, const doip_client_config_t *config);

/**
 * @brief Destroy a DoIP client and release all resources
 * @param client    Client handle
 */
void doip_client_destroy(doip_client_t *client);

/* ============================================================================
 * Vehicle Discovery (UDP)
 * ========================================================================== */

/** Discovery result entry */
typedef struct {
    doip_vehicle_id_response_t  vehicle;
    struct sockaddr_in          source_addr;
} doip_discovery_result_t;

/**
 * @brief Discover vehicles on the network via UDP broadcast
 * @param client        Client handle
 * @param interface_ip  Local interface IP to bind (NULL for INADDR_ANY)
 * @param results       Array to store results
 * @param max_results   Maximum number of results
 * @param timeout_ms    Total discovery timeout in milliseconds
 * @return Number of vehicles found, or negative error code
 */
int doip_client_discover(doip_client_t *client, const char *interface_ip,
                          doip_discovery_result_t *results, int max_results,
                          int timeout_ms);

/**
 * @brief Discover vehicle by VIN
 * @param client        Client handle
 * @param interface_ip  Local interface IP
 * @param vin           VIN to search for (17 bytes)
 * @param result        Output result
 * @param timeout_ms    Timeout
 * @return DOIP_OK if found, or negative error code
 */
doip_result_t doip_client_discover_by_vin(doip_client_t *client, const char *interface_ip,
                                           const uint8_t vin[DOIP_VIN_LENGTH],
                                           doip_discovery_result_t *result,
                                           int timeout_ms);

/**
 * @brief Discover vehicle by EID (MAC address)
 * @param client        Client handle
 * @param interface_ip  Local interface IP
 * @param eid           EID to search for (6 bytes)
 * @param result        Output result
 * @param timeout_ms    Timeout
 * @return DOIP_OK if found, or negative error code
 */
doip_result_t doip_client_discover_by_eid(doip_client_t *client, const char *interface_ip,
                                           const uint8_t eid[DOIP_EID_LENGTH],
                                           doip_discovery_result_t *result,
                                           int timeout_ms);

/* ============================================================================
 * TCP Connection & Routing Activation
 * ========================================================================== */

/**
 * @brief Connect to a DoIP entity via TCP
 * @param client    Client handle
 * @param host      IP address or hostname
 * @param port      TCP port (0 for default 13400)
 * @return DOIP_OK on success
 */
doip_result_t doip_client_connect(doip_client_t *client, const char *host, uint16_t port);

/**
 * @brief Perform routing activation
 * @param client    Client handle
 * @param resp      Output routing activation response (can be NULL)
 * @return DOIP_OK on success, DOIP_ERR_ROUTING_DENIED if denied
 */
doip_result_t doip_client_activate_routing(doip_client_t *client,
                                            doip_routing_activation_response_t *resp);

/**
 * @brief Disconnect from the DoIP entity
 * @param client    Client handle
 */
void doip_client_disconnect(doip_client_t *client);

/**
 * @brief Check if the client is connected and routing is active
 * @param client    Client handle
 * @return true if connected and routing is active
 */
bool doip_client_is_connected(const doip_client_t *client);

/* ============================================================================
 * Diagnostic Communication
 * ========================================================================== */

/**
 * @brief Send a UDS diagnostic request and receive the response
 * @param client        Client handle
 * @param uds_request   UDS request data
 * @param request_len   Length of UDS request
 * @param uds_response  Buffer for UDS response data
 * @param response_size Size of response buffer
 * @param timeout_ms    Response timeout (0 for default)
 * @return Length of UDS response data, or negative error code
 */
int doip_client_send_uds(doip_client_t *client,
                          const uint8_t *uds_request, uint32_t request_len,
                          uint8_t *uds_response, uint32_t response_size,
                          int timeout_ms);

/**
 * @brief Send a raw diagnostic message (no response wait)
 * @param client    Client handle
 * @param target    Target ECU address
 * @param data      UDS data
 * @param data_len  Length of UDS data
 * @return DOIP_OK on success
 */
doip_result_t doip_client_send_diagnostic(doip_client_t *client, uint16_t target,
                                           const uint8_t *data, uint32_t data_len);

/**
 * @brief Receive a DoIP message (blocking with timeout)
 * @param client        Client handle
 * @param msg           Output message
 * @param timeout_ms    Timeout (0 for configured default)
 * @return DOIP_OK on success
 */
doip_result_t doip_client_recv_message(doip_client_t *client, doip_message_t *msg,
                                        int timeout_ms);

/* ============================================================================
 * Entity Status & Diagnostics
 * ========================================================================== */

/**
 * @brief Request entity status
 * @param client    Client handle
 * @param status    Output status
 * @return DOIP_OK on success
 */
doip_result_t doip_client_get_entity_status(doip_client_t *client,
                                             doip_entity_status_response_t *status);

/**
 * @brief Request diagnostic power mode
 * @param client    Client handle
 * @param mode      Output power mode
 * @return DOIP_OK on success
 */
doip_result_t doip_client_get_power_mode(doip_client_t *client,
                                          doip_power_mode_response_t *mode);

/* ============================================================================
 * Common UDS Helper Functions
 * ========================================================================== */

/**
 * @brief Send DiagnosticSessionControl (0x10)
 * @param client    Client handle
 * @param session   Session type (0x01=default, 0x02=programming, 0x03=extended)
 * @param response  Response buffer
 * @param resp_size Response buffer size
 * @return Response length, or negative error code
 */
int doip_client_uds_session_control(doip_client_t *client, uint8_t session,
                                     uint8_t *response, uint32_t resp_size);

/**
 * @brief Send TesterPresent (0x3E)
 * @param client    Client handle
 * @return DOIP_OK on success
 */
doip_result_t doip_client_uds_tester_present(doip_client_t *client);

/**
 * @brief Send ReadDataByIdentifier (0x22)
 * @param client    Client handle
 * @param did       Data Identifier (e.g., 0xF190 for VIN)
 * @param response  Response buffer
 * @param resp_size Response buffer size
 * @return Response length, or negative error code
 */
int doip_client_uds_read_did(doip_client_t *client, uint16_t did,
                              uint8_t *response, uint32_t resp_size);

/**
 * @brief Send SecurityAccess (0x27) seed request
 * @param client        Client handle
 * @param access_level  Security access level (odd number for seed request)
 * @param seed          Buffer for seed data
 * @param seed_size     Size of seed buffer
 * @return Seed length, or negative error code
 */
int doip_client_uds_security_seed(doip_client_t *client, uint8_t access_level,
                                   uint8_t *seed, uint32_t seed_size);

/**
 * @brief Send SecurityAccess (0x27) key response
 * @param client        Client handle
 * @param access_level  Security access level (even number for key send)
 * @param key           Key data
 * @param key_len       Key data length
 * @return DOIP_OK on success
 */
doip_result_t doip_client_uds_security_key(doip_client_t *client, uint8_t access_level,
                                            const uint8_t *key, uint32_t key_len);

/**
 * @brief Send ReadDTCInformation (0x19)
 * @param client    Client handle
 * @param sub_func  Sub-function
 * @param response  Response buffer
 * @param resp_size Response buffer size
 * @return Response length, or negative error code
 */
int doip_client_uds_read_dtc(doip_client_t *client, uint8_t sub_func,
                              uint8_t *response, uint32_t resp_size);

/* ============================================================================
 * Block Transfer / Flash Programming (0x34, 0x35, 0x36, 0x37)
 * ========================================================================== */

/** Data format identifier for RequestDownload/RequestUpload */
typedef struct {
    uint8_t     compression_method;         /* High nibble of dataFormatIdentifier */
    uint8_t     encrypting_method;          /* Low nibble of dataFormatIdentifier */
} doip_data_format_t;

/** Transfer progress callback */
typedef void (*doip_transfer_progress_cb_t)(uint32_t bytes_transferred,
                                             uint32_t total_bytes, void *user_data);

/** Transfer result info */
typedef struct {
    uint32_t    total_bytes;                /* Total bytes transferred */
    uint32_t    num_blocks;                 /* Number of TransferData blocks sent */
    uint16_t    max_block_length;           /* Block length negotiated with ECU */
    bool        success;
} doip_transfer_result_t;

/**
 * @brief Send RequestDownload (0x34) - Initiate data transfer TO the ECU
 * @param client            Client handle
 * @param memory_address    Target memory address on ECU
 * @param addr_len_bytes    Size of address field in bytes (1-4)
 * @param memory_size       Total number of bytes to download
 * @param size_len_bytes    Size of the memory size field in bytes (1-4)
 * @param data_format       Compression/encryption format (NULL for uncompressed)
 * @param max_block_length  [out] Maximum number of data bytes per TransferData
 * @return DOIP_OK on success, or negative error code
 */
doip_result_t doip_client_uds_request_download(doip_client_t *client,
                                                uint32_t memory_address,
                                                uint8_t addr_len_bytes,
                                                uint32_t memory_size,
                                                uint8_t size_len_bytes,
                                                const doip_data_format_t *data_format,
                                                uint16_t *max_block_length);

/**
 * @brief Send RequestUpload (0x35) - Initiate data transfer FROM the ECU
 * @param client            Client handle
 * @param memory_address    Source memory address on ECU
 * @param addr_len_bytes    Size of address field in bytes (1-4)
 * @param memory_size       Number of bytes to upload
 * @param size_len_bytes    Size of the memory size field in bytes (1-4)
 * @param data_format       Compression/encryption format (NULL for uncompressed)
 * @param max_block_length  [out] Maximum number of data bytes per TransferData
 * @return DOIP_OK on success, or negative error code
 */
doip_result_t doip_client_uds_request_upload(doip_client_t *client,
                                              uint32_t memory_address,
                                              uint8_t addr_len_bytes,
                                              uint32_t memory_size,
                                              uint8_t size_len_bytes,
                                              const doip_data_format_t *data_format,
                                              uint16_t *max_block_length);

/**
 * @brief Send a single TransferData (0x36) block
 * @param client            Client handle
 * @param block_sequence    Block sequence counter (starts at 1, wraps at 0xFF->0x00)
 * @param data              Block data
 * @param data_len          Block data length
 * @param response          Response buffer (can be NULL)
 * @param resp_size         Response buffer size
 * @return Response length, or negative error code
 */
int doip_client_uds_transfer_data(doip_client_t *client,
                                   uint8_t block_sequence,
                                   const uint8_t *data, uint32_t data_len,
                                   uint8_t *response, uint32_t resp_size);

/**
 * @brief Send RequestTransferExit (0x37)
 * @param client            Client handle
 * @param transfer_req_param Optional parameter data (CRC etc., can be NULL)
 * @param param_len         Length of parameter data
 * @param response          Response buffer
 * @param resp_size         Response buffer size
 * @return Response length, or negative error code
 */
int doip_client_uds_request_transfer_exit(doip_client_t *client,
                                           const uint8_t *transfer_req_param,
                                           uint32_t param_len,
                                           uint8_t *response, uint32_t resp_size);

/**
 * @brief High-level: Flash/download an entire data block to the ECU
 *
 * Performs the complete sequence:
 *   1. RequestDownload (0x34) with address + size
 *   2. TransferData (0x36) x N blocks (auto-chunked to max_block_length)
 *   3. RequestTransferExit (0x37)
 *
 * Handles block sequencing, chunking, and NRC 0x78 (responsePending).
 *
 * @param client            Client handle
 * @param memory_address    Target memory address on ECU
 * @param addr_len_bytes    Size of address field in bytes (1-4)
 * @param data              Firmware / data to write
 * @param data_len          Total bytes to write
 * @param data_format       Compression/encryption (NULL for none)
 * @param progress_cb       Optional progress callback (can be NULL)
 * @param user_data         User data passed to progress callback
 * @param result            [out] Transfer result details (can be NULL)
 * @return DOIP_OK on success, or negative error code
 */
doip_result_t doip_client_flash_download(doip_client_t *client,
                                          uint32_t memory_address,
                                          uint8_t addr_len_bytes,
                                          const uint8_t *data, uint32_t data_len,
                                          const doip_data_format_t *data_format,
                                          doip_transfer_progress_cb_t progress_cb,
                                          void *user_data,
                                          doip_transfer_result_t *result);

/**
 * @brief High-level: Upload/read an entire data block from the ECU
 *
 * Performs the complete sequence:
 *   1. RequestUpload (0x35) with address + size
 *   2. TransferData (0x36) x N blocks (receives data)
 *   3. RequestTransferExit (0x37)
 *
 * @param client            Client handle
 * @param memory_address    Source memory address on ECU
 * @param addr_len_bytes    Size of address field in bytes (1-4)
 * @param buffer            Buffer to receive data
 * @param buffer_size       Buffer capacity
 * @param read_size         Number of bytes to read from ECU
 * @param data_format       Compression/encryption (NULL for none)
 * @param progress_cb       Optional progress callback (can be NULL)
 * @param user_data         User data passed to progress callback
 * @param result            [out] Transfer result details (can be NULL)
 * @return Number of bytes received, or negative error code
 */
int doip_client_flash_upload(doip_client_t *client,
                              uint32_t memory_address,
                              uint8_t addr_len_bytes,
                              uint8_t *buffer, uint32_t buffer_size,
                              uint32_t read_size,
                              const doip_data_format_t *data_format,
                              doip_transfer_progress_cb_t progress_cb,
                              void *user_data,
                              doip_transfer_result_t *result);

/**
 * @brief Convenience: Prepare ECU for flashing
 *
 * Performs the standard pre-flash sequence:
 *   1. DiagnosticSessionControl -> Programming (0x02)
 *   2. ControlDTCSetting -> Off (0x02)
 *   3. CommunicationControl -> Disable Rx/Tx (0x03)
 *   4. SecurityAccess (seed/key with user callback)
 *
 * @param client            Client handle
 * @param security_level    Security access level (e.g. 0x01)
 * @param compute_key       Callback to compute key from seed
 * @param key_user_data     User data for key callback
 * @return DOIP_OK on success
 */
typedef doip_result_t (*doip_compute_key_cb_t)(const uint8_t *seed, uint32_t seed_len,
                                                uint8_t *key, uint32_t *key_len,
                                                void *user_data);

doip_result_t doip_client_prepare_flash(doip_client_t *client,
                                         uint8_t security_level,
                                         doip_compute_key_cb_t compute_key,
                                         void *key_user_data);

/**
 * @brief Convenience: Finalize after flashing
 *
 * Performs:
 *   1. ControlDTCSetting -> On (0x01)
 *   2. CommunicationControl -> Enable Rx/Tx (0x00)
 *   3. ECUReset -> Hard Reset (0x01)
 *
 * @param client            Client handle
 * @return DOIP_OK on success
 */
doip_result_t doip_client_finalize_flash(doip_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* DOIP_CLIENT_H */
