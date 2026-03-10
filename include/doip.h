/**
 * @file doip.h
 * @brief Diagnostics over Internet Protocol (DoIP) - ISO 13400-2 Implementation
 *
 * Full implementation of DoIP protocol including:
 * - Generic header handling
 * - Vehicle identification (UDP)
 * - Routing activation (TCP)
 * - Diagnostic messaging (TCP)
 * - Alive check
 * - Entity status
 */

#ifndef DOIP_H
#define DOIP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Protocol Constants
 * ========================================================================== */

/** DoIP protocol version (ISO 13400-2:2019) */
#define DOIP_PROTOCOL_VERSION       0x03
/** Default protocol version for vehicle identification */
#define DOIP_PROTOCOL_VERSION_DEFAULT 0xFF

/** DoIP ports */
#define DOIP_UDP_DISCOVERY_PORT     13400
#define DOIP_TCP_DATA_PORT          13400

/** Header sizes */
#define DOIP_HEADER_SIZE            8   /* version(1) + inv_version(1) + type(2) + length(4) */

/** Maximum payload sizes */
#define DOIP_MAX_PAYLOAD_SIZE       65535
#define DOIP_MAX_DIAGNOSTIC_SIZE    4096
#define DOIP_VIN_LENGTH             17
#define DOIP_EID_LENGTH             6
#define DOIP_GID_LENGTH             6

/** Timeouts (milliseconds) */
#define DOIP_CTRL_TIMEOUT           2000    /* A_DoIP_Ctrl */
#define DOIP_ANNOUNCE_WAIT          500     /* A_DoIP_Announce_Wait */
#define DOIP_ANNOUNCE_INTERVAL      500     /* A_DoIP_Announce_Interval */
#define DOIP_ANNOUNCE_NUM           3       /* A_DoIP_Announce_Num */
#define DOIP_DIAGNOSTIC_TIMEOUT     2000    /* A_DoIP_Diagnostic_Message */
#define DOIP_TCP_GENERAL_TIMEOUT    5000

/* ============================================================================
 * Payload Type Codes (ISO 13400-2, Table 17)
 * ========================================================================== */

typedef enum {
    /* Generic DoIP header negative acknowledge */
    DOIP_TYPE_HEADER_NACK                       = 0x0000,

    /* Vehicle identification */
    DOIP_TYPE_VEHICLE_ID_REQUEST                = 0x0001,
    DOIP_TYPE_VEHICLE_ID_REQUEST_EID            = 0x0002,
    DOIP_TYPE_VEHICLE_ID_REQUEST_VIN            = 0x0003,
    DOIP_TYPE_VEHICLE_ANNOUNCEMENT              = 0x0004,

    /* Routing activation */
    DOIP_TYPE_ROUTING_ACTIVATION_REQUEST        = 0x0005,
    DOIP_TYPE_ROUTING_ACTIVATION_RESPONSE       = 0x0006,

    /* Alive check */
    DOIP_TYPE_ALIVE_CHECK_REQUEST               = 0x0007,
    DOIP_TYPE_ALIVE_CHECK_RESPONSE              = 0x0008,

    /* DoIP entity status */
    DOIP_TYPE_ENTITY_STATUS_REQUEST             = 0x4001,
    DOIP_TYPE_ENTITY_STATUS_RESPONSE            = 0x4002,

    /* Diagnostic power mode */
    DOIP_TYPE_DIAG_POWER_MODE_REQUEST           = 0x4003,
    DOIP_TYPE_DIAG_POWER_MODE_RESPONSE          = 0x4004,

    /* Diagnostic message */
    DOIP_TYPE_DIAGNOSTIC_MESSAGE                = 0x8001,
    DOIP_TYPE_DIAGNOSTIC_MESSAGE_ACK            = 0x8002,
    DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK           = 0x8003,
} doip_payload_type_t;

/* ============================================================================
 * NACK Codes
 * ========================================================================== */

/** Generic header NACK codes */
typedef enum {
    DOIP_HEADER_NACK_INCORRECT_PATTERN          = 0x00,
    DOIP_HEADER_NACK_UNKNOWN_PAYLOAD_TYPE       = 0x01,
    DOIP_HEADER_NACK_MESSAGE_TOO_LARGE          = 0x02,
    DOIP_HEADER_NACK_OUT_OF_MEMORY              = 0x03,
    DOIP_HEADER_NACK_INVALID_PAYLOAD_LENGTH     = 0x04,
} doip_header_nack_code_t;

/** Routing activation response codes */
typedef enum {
    DOIP_ROUTING_ACTIVATION_DENIED_UNKNOWN_SA   = 0x00,
    DOIP_ROUTING_ACTIVATION_DENIED_ALL_SOCKETS  = 0x01,
    DOIP_ROUTING_ACTIVATION_DENIED_SA_DIFFERENT = 0x02,
    DOIP_ROUTING_ACTIVATION_DENIED_SA_ACTIVE    = 0x03,
    DOIP_ROUTING_ACTIVATION_DENIED_MISSING_AUTH = 0x04,
    DOIP_ROUTING_ACTIVATION_DENIED_REJECTED     = 0x05,
    DOIP_ROUTING_ACTIVATION_DENIED_TLS_REQUIRED = 0x06,
    DOIP_ROUTING_ACTIVATION_SUCCESS_CONFIRMATION_REQUIRED = 0x11,
    DOIP_ROUTING_ACTIVATION_SUCCESS             = 0x10,
} doip_routing_activation_response_code_t;

/** Diagnostic message NACK codes */
typedef enum {
    DOIP_DIAG_NACK_INVALID_SA                   = 0x02,
    DOIP_DIAG_NACK_UNKNOWN_TA                   = 0x03,
    DOIP_DIAG_NACK_MESSAGE_TOO_LARGE            = 0x04,
    DOIP_DIAG_NACK_OUT_OF_MEMORY                = 0x05,
    DOIP_DIAG_NACK_TARGET_UNREACHABLE           = 0x06,
    DOIP_DIAG_NACK_UNKNOWN_NETWORK              = 0x07,
    DOIP_DIAG_NACK_TRANSPORT_PROTOCOL_ERROR     = 0x08,
} doip_diag_nack_code_t;

/** Diagnostic message positive ACK codes */
typedef enum {
    DOIP_DIAG_ACK_CONFIRMED                     = 0x00,
} doip_diag_ack_code_t;

/** Routing activation types */
typedef enum {
    DOIP_ROUTING_ACTIVATION_DEFAULT             = 0x00,
    DOIP_ROUTING_ACTIVATION_WWH_OBD             = 0x01,
    DOIP_ROUTING_ACTIVATION_CENTRAL_SECURITY    = 0xE0,
} doip_routing_activation_type_t;

/* ============================================================================
 * Return Codes
 * ========================================================================== */

typedef enum {
    DOIP_OK                     = 0,
    DOIP_ERR_INVALID_PARAM      = -1,
    DOIP_ERR_BUFFER_TOO_SMALL   = -2,
    DOIP_ERR_INVALID_HEADER     = -3,
    DOIP_ERR_SOCKET             = -4,
    DOIP_ERR_CONNECT            = -5,
    DOIP_ERR_TIMEOUT            = -6,
    DOIP_ERR_SEND               = -7,
    DOIP_ERR_RECV               = -8,
    DOIP_ERR_NACK               = -9,
    DOIP_ERR_ROUTING_DENIED     = -10,
    DOIP_ERR_MEMORY             = -11,
    DOIP_ERR_CLOSED             = -12,
} doip_result_t;

/* ============================================================================
 * Data Structures
 * ========================================================================== */

/** Generic DoIP header */
typedef struct {
    uint8_t     protocol_version;
    uint8_t     inverse_version;
    uint16_t    payload_type;
    uint32_t    payload_length;
} doip_header_t;

/** Vehicle identification response / announcement */
typedef struct {
    uint8_t     vin[DOIP_VIN_LENGTH];
    uint16_t    logical_address;
    uint8_t     eid[DOIP_EID_LENGTH];       /* Entity ID (MAC) */
    uint8_t     gid[DOIP_GID_LENGTH];       /* Group ID */
    uint8_t     further_action_required;
    uint8_t     vin_gid_sync_status;        /* Optional */
    bool        has_sync_status;
} doip_vehicle_id_response_t;

/** Routing activation request */
typedef struct {
    uint16_t    source_address;
    uint8_t     activation_type;
    uint32_t    reserved;                    /* ISO reserved, set to 0 */
    uint8_t     oem_specific[4];             /* Optional OEM-specific data */
    bool        has_oem_specific;
} doip_routing_activation_request_t;

/** Routing activation response */
typedef struct {
    uint16_t    tester_logical_address;
    uint16_t    entity_logical_address;
    uint8_t     response_code;
    uint32_t    reserved;
    uint8_t     oem_specific[4];
    bool        has_oem_specific;
} doip_routing_activation_response_t;

/** Diagnostic message */
typedef struct {
    uint16_t    source_address;
    uint16_t    target_address;
    uint8_t    *data;
    uint32_t    data_length;
} doip_diagnostic_message_t;

/** Diagnostic message ACK */
typedef struct {
    uint16_t    source_address;
    uint16_t    target_address;
    uint8_t     ack_code;
    uint8_t    *prev_diagnostic_data;        /* Optional previous diagnostic data */
    uint32_t    prev_data_length;
} doip_diagnostic_ack_t;

/** Diagnostic message NACK */
typedef struct {
    uint16_t    source_address;
    uint16_t    target_address;
    uint8_t     nack_code;
    uint8_t    *prev_diagnostic_data;
    uint32_t    prev_data_length;
} doip_diagnostic_nack_t;

/** Entity status response */
typedef struct {
    uint8_t     node_type;                   /* 0=gateway, 1=node */
    uint8_t     max_concurrent_sockets;
    uint8_t     currently_open_sockets;
    uint32_t    max_data_size;               /* Optional */
    bool        has_max_data_size;
} doip_entity_status_response_t;

/** Diagnostic power mode response */
typedef struct {
    uint8_t     power_mode;                  /* 0x00=not ready, 0x01=ready, 0x02=not supported */
} doip_power_mode_response_t;

/** Alive check response */
typedef struct {
    uint16_t    source_address;
} doip_alive_check_response_t;

/* ============================================================================
 * Generic DoIP Message Container
 * ========================================================================== */

typedef struct {
    doip_header_t header;
    union {
        uint8_t                         nack_code;           /* Header NACK */
        doip_vehicle_id_response_t      vehicle_id;
        doip_routing_activation_request_t   routing_req;
        doip_routing_activation_response_t  routing_resp;
        doip_diagnostic_message_t       diagnostic;
        doip_diagnostic_ack_t           diagnostic_ack;
        doip_diagnostic_nack_t          diagnostic_nack;
        doip_entity_status_response_t   entity_status;
        doip_power_mode_response_t      power_mode;
        doip_alive_check_response_t     alive_check;
    } payload;
    /* Raw payload buffer for diagnostic data references */
    uint8_t    *raw_payload;
    uint32_t    raw_payload_size;
} doip_message_t;

/* ============================================================================
 * Core Protocol API - Serialization / Deserialization
 * ========================================================================== */

/**
 * @brief Serialize a DoIP header into a buffer
 * @param header    Pointer to the header structure
 * @param buf       Output buffer (must be >= DOIP_HEADER_SIZE bytes)
 * @param buf_size  Size of the output buffer
 * @return Number of bytes written, or negative error code
 */
int doip_serialize_header(const doip_header_t *header, uint8_t *buf, size_t buf_size);

/**
 * @brief Deserialize a DoIP header from a buffer
 * @param buf       Input buffer
 * @param buf_size  Size of the input buffer
 * @param header    Output header structure
 * @return DOIP_OK on success, or negative error code
 */
doip_result_t doip_deserialize_header(const uint8_t *buf, size_t buf_size, doip_header_t *header);

/**
 * @brief Validate a DoIP header
 * @param header    Pointer to header to validate
 * @return DOIP_OK if valid, or negative error code
 */
doip_result_t doip_validate_header(const doip_header_t *header);

/**
 * @brief Build a complete DoIP message buffer
 * @param type          Payload type
 * @param payload       Payload data (can be NULL for empty payloads)
 * @param payload_len   Length of payload data
 * @param buf           Output buffer
 * @param buf_size      Size of output buffer
 * @return Total message size, or negative error code
 */
int doip_build_message(doip_payload_type_t type, const uint8_t *payload,
                       uint32_t payload_len, uint8_t *buf, size_t buf_size);

/**
 * @brief Parse a complete DoIP message from a buffer
 * @param buf       Input buffer containing header + payload
 * @param buf_size  Size of input buffer
 * @param msg       Output message structure
 * @return DOIP_OK on success, or negative error code
 */
doip_result_t doip_parse_message(const uint8_t *buf, size_t buf_size, doip_message_t *msg);

/* ============================================================================
 * Payload Builders
 * ========================================================================== */

/** Build vehicle identification request (no parameters) */
int doip_build_vehicle_id_request(uint8_t *buf, size_t buf_size);

/** Build vehicle identification request with EID filter */
int doip_build_vehicle_id_request_eid(const uint8_t eid[DOIP_EID_LENGTH],
                                       uint8_t *buf, size_t buf_size);

/** Build vehicle identification request with VIN filter */
int doip_build_vehicle_id_request_vin(const uint8_t vin[DOIP_VIN_LENGTH],
                                       uint8_t *buf, size_t buf_size);

/** Build vehicle announcement / identification response */
int doip_build_vehicle_announcement(const doip_vehicle_id_response_t *vehicle,
                                     uint8_t *buf, size_t buf_size);

/** Build routing activation request */
int doip_build_routing_activation_request(const doip_routing_activation_request_t *req,
                                           uint8_t *buf, size_t buf_size);

/** Build routing activation response */
int doip_build_routing_activation_response(const doip_routing_activation_response_t *resp,
                                            uint8_t *buf, size_t buf_size);

/** Build diagnostic message */
int doip_build_diagnostic_message(uint16_t source_addr, uint16_t target_addr,
                                   const uint8_t *uds_data, uint32_t uds_len,
                                   uint8_t *buf, size_t buf_size);

/** Build diagnostic message positive ACK */
int doip_build_diagnostic_ack(uint16_t source_addr, uint16_t target_addr,
                               uint8_t ack_code, uint8_t *buf, size_t buf_size);

/** Build diagnostic message negative ACK */
int doip_build_diagnostic_nack(uint16_t source_addr, uint16_t target_addr,
                                uint8_t nack_code, uint8_t *buf, size_t buf_size);

/** Build alive check request */
int doip_build_alive_check_request(uint8_t *buf, size_t buf_size);

/** Build alive check response */
int doip_build_alive_check_response(uint16_t source_addr, uint8_t *buf, size_t buf_size);

/** Build entity status request */
int doip_build_entity_status_request(uint8_t *buf, size_t buf_size);

/** Build entity status response */
int doip_build_entity_status_response(const doip_entity_status_response_t *status,
                                       uint8_t *buf, size_t buf_size);

/** Build header NACK */
int doip_build_header_nack(uint8_t nack_code, uint8_t *buf, size_t buf_size);

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/** Get human-readable string for payload type */
const char *doip_payload_type_str(doip_payload_type_t type);

/** Get human-readable string for NACK code */
const char *doip_nack_code_str(doip_header_nack_code_t code);

/** Get human-readable string for routing activation response code */
const char *doip_routing_response_str(uint8_t code);

/** Get human-readable string for diagnostic NACK code */
const char *doip_diag_nack_str(doip_diag_nack_code_t code);

/** Get human-readable string for result code */
const char *doip_result_str(doip_result_t result);

/** Print a DoIP message to stdout (for debugging) */
void doip_print_message(const doip_message_t *msg);

/** Print hex dump of buffer */
void doip_hex_dump(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DOIP_H */
