/**
 * @file doip.c
 * @brief Core DoIP protocol implementation
 *
 * Handles serialization/deserialization of all DoIP message types
 * per ISO 13400-2.
 */

#include "doip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

/* ============================================================================
 * Internal Helpers
 * ========================================================================== */

static inline void write_u16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

static inline void write_u32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val & 0xFF);
}

static inline uint16_t read_u16(const uint8_t *buf)
{
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

static inline uint32_t read_u32(const uint8_t *buf)
{
    return (uint32_t)((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

/* ============================================================================
 * Header Serialization / Deserialization
 * ========================================================================== */

int doip_serialize_header(const doip_header_t *header, uint8_t *buf, size_t buf_size)
{
    if (!header || !buf)
        return DOIP_ERR_INVALID_PARAM;
    if (buf_size < DOIP_HEADER_SIZE)
        return DOIP_ERR_BUFFER_TOO_SMALL;

    buf[0] = header->protocol_version;
    buf[1] = header->inverse_version;
    write_u16(&buf[2], header->payload_type);
    write_u32(&buf[4], header->payload_length);

    return DOIP_HEADER_SIZE;
}

doip_result_t doip_deserialize_header(const uint8_t *buf, size_t buf_size, doip_header_t *header)
{
    if (!buf || !header)
        return DOIP_ERR_INVALID_PARAM;
    if (buf_size < DOIP_HEADER_SIZE)
        return DOIP_ERR_BUFFER_TOO_SMALL;

    header->protocol_version = buf[0];
    header->inverse_version  = buf[1];
    header->payload_type     = read_u16(&buf[2]);
    header->payload_length   = read_u32(&buf[4]);

    return DOIP_OK;
}

doip_result_t doip_validate_header(const doip_header_t *header)
{
    if (!header)
        return DOIP_ERR_INVALID_PARAM;

    /* Check version / inverse version match */
    if ((header->protocol_version ^ header->inverse_version) != 0xFF)
        return DOIP_ERR_INVALID_HEADER;

    /* Check payload length is reasonable */
    if (header->payload_length > DOIP_MAX_PAYLOAD_SIZE)
        return DOIP_ERR_INVALID_HEADER;

    return DOIP_OK;
}

/* ============================================================================
 * Generic Message Builder
 * ========================================================================== */

int doip_build_message(doip_payload_type_t type, const uint8_t *payload,
                       uint32_t payload_len, uint8_t *buf, size_t buf_size)
{
    if (!buf)
        return DOIP_ERR_INVALID_PARAM;
    if (buf_size < (size_t)(DOIP_HEADER_SIZE + payload_len))
        return DOIP_ERR_BUFFER_TOO_SMALL;

    doip_header_t header = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = (uint8_t)(DOIP_PROTOCOL_VERSION ^ 0xFF),
        .payload_type     = (uint16_t)type,
        .payload_length   = payload_len,
    };

    int ret = doip_serialize_header(&header, buf, buf_size);
    if (ret < 0)
        return ret;

    if (payload && payload_len > 0) {
        memcpy(buf + DOIP_HEADER_SIZE, payload, payload_len);
    }

    return DOIP_HEADER_SIZE + (int)payload_len;
}

/* ============================================================================
 * Parse incoming message
 * ========================================================================== */

static doip_result_t parse_vehicle_id_response(const uint8_t *payload, uint32_t len,
                                                 doip_vehicle_id_response_t *vid)
{
    /* Minimum size: VIN(17) + LogAddr(2) + EID(6) + GID(6) + FurtherAction(1) = 32 */
    if (len < 32)
        return DOIP_ERR_INVALID_HEADER;

    size_t offset = 0;
    memcpy(vid->vin, &payload[offset], DOIP_VIN_LENGTH);
    offset += DOIP_VIN_LENGTH;

    vid->logical_address = read_u16(&payload[offset]);
    offset += 2;

    memcpy(vid->eid, &payload[offset], DOIP_EID_LENGTH);
    offset += DOIP_EID_LENGTH;

    memcpy(vid->gid, &payload[offset], DOIP_GID_LENGTH);
    offset += DOIP_GID_LENGTH;

    vid->further_action_required = payload[offset++];

    if (len > 32) {
        vid->vin_gid_sync_status = payload[offset];
        vid->has_sync_status = true;
    } else {
        vid->has_sync_status = false;
    }

    return DOIP_OK;
}

static doip_result_t parse_routing_activation_response(const uint8_t *payload, uint32_t len,
                                                         doip_routing_activation_response_t *resp)
{
    /* Minimum: TesterAddr(2) + EntityAddr(2) + Code(1) + Reserved(4) = 9 */
    if (len < 9)
        return DOIP_ERR_INVALID_HEADER;

    size_t offset = 0;
    resp->tester_logical_address = read_u16(&payload[offset]);
    offset += 2;
    resp->entity_logical_address = read_u16(&payload[offset]);
    offset += 2;
    resp->response_code = payload[offset++];
    resp->reserved = read_u32(&payload[offset]);
    offset += 4;

    if (len >= 13) {
        memcpy(resp->oem_specific, &payload[offset], 4);
        resp->has_oem_specific = true;
    } else {
        resp->has_oem_specific = false;
    }

    return DOIP_OK;
}

static doip_result_t parse_diagnostic_message(const uint8_t *payload, uint32_t len,
                                                doip_diagnostic_message_t *diag)
{
    /* Minimum: SA(2) + TA(2) + at least 1 byte data = 5 */
    if (len < 5)
        return DOIP_ERR_INVALID_HEADER;

    diag->source_address = read_u16(&payload[0]);
    diag->target_address = read_u16(&payload[2]);
    diag->data = (uint8_t *)&payload[4];  /* Point into the raw payload */
    diag->data_length = len - 4;

    return DOIP_OK;
}

static doip_result_t parse_diagnostic_ack(const uint8_t *payload, uint32_t len,
                                            doip_diagnostic_ack_t *ack)
{
    /* Minimum: SA(2) + TA(2) + Code(1) = 5 */
    if (len < 5)
        return DOIP_ERR_INVALID_HEADER;

    ack->source_address = read_u16(&payload[0]);
    ack->target_address = read_u16(&payload[2]);
    ack->ack_code = payload[4];

    if (len > 5) {
        ack->prev_diagnostic_data = (uint8_t *)&payload[5];
        ack->prev_data_length = len - 5;
    } else {
        ack->prev_diagnostic_data = NULL;
        ack->prev_data_length = 0;
    }

    return DOIP_OK;
}

static doip_result_t parse_diagnostic_nack(const uint8_t *payload, uint32_t len,
                                             doip_diagnostic_nack_t *nack)
{
    if (len < 5)
        return DOIP_ERR_INVALID_HEADER;

    nack->source_address = read_u16(&payload[0]);
    nack->target_address = read_u16(&payload[2]);
    nack->nack_code = payload[4];

    if (len > 5) {
        nack->prev_diagnostic_data = (uint8_t *)&payload[5];
        nack->prev_data_length = len - 5;
    } else {
        nack->prev_diagnostic_data = NULL;
        nack->prev_data_length = 0;
    }

    return DOIP_OK;
}

static doip_result_t parse_entity_status_response(const uint8_t *payload, uint32_t len,
                                                    doip_entity_status_response_t *status)
{
    if (len < 3)
        return DOIP_ERR_INVALID_HEADER;

    status->node_type = payload[0];
    status->max_concurrent_sockets = payload[1];
    status->currently_open_sockets = payload[2];

    if (len >= 7) {
        status->max_data_size = read_u32(&payload[3]);
        status->has_max_data_size = true;
    } else {
        status->has_max_data_size = false;
    }

    return DOIP_OK;
}

static doip_result_t parse_routing_activation_request(const uint8_t *payload, uint32_t len,
                                                        doip_routing_activation_request_t *req)
{
    /* Minimum: SA(2) + Type(1) + Reserved(4) = 7 */
    if (len < 7)
        return DOIP_ERR_INVALID_HEADER;

    req->source_address = read_u16(&payload[0]);
    req->activation_type = payload[2];
    req->reserved = read_u32(&payload[3]);

    if (len >= 11) {
        memcpy(req->oem_specific, &payload[7], 4);
        req->has_oem_specific = true;
    } else {
        req->has_oem_specific = false;
    }

    return DOIP_OK;
}

doip_result_t doip_parse_message(const uint8_t *buf, size_t buf_size, doip_message_t *msg)
{
    if (!buf || !msg)
        return DOIP_ERR_INVALID_PARAM;

    memset(msg, 0, sizeof(doip_message_t));

    doip_result_t ret = doip_deserialize_header(buf, buf_size, &msg->header);
    if (ret != DOIP_OK)
        return ret;

    ret = doip_validate_header(&msg->header);
    if (ret != DOIP_OK)
        return ret;

    uint32_t payload_len = msg->header.payload_length;
    if (buf_size < DOIP_HEADER_SIZE + payload_len)
        return DOIP_ERR_BUFFER_TOO_SMALL;

    const uint8_t *payload = buf + DOIP_HEADER_SIZE;
    msg->raw_payload = (uint8_t *)payload;
    msg->raw_payload_size = payload_len;

    switch (msg->header.payload_type) {
    case DOIP_TYPE_HEADER_NACK:
        if (payload_len >= 1)
            msg->payload.nack_code = payload[0];
        break;

    case DOIP_TYPE_VEHICLE_ANNOUNCEMENT:
        return parse_vehicle_id_response(payload, payload_len, &msg->payload.vehicle_id);

    case DOIP_TYPE_ROUTING_ACTIVATION_REQUEST:
        return parse_routing_activation_request(payload, payload_len, &msg->payload.routing_req);

    case DOIP_TYPE_ROUTING_ACTIVATION_RESPONSE:
        return parse_routing_activation_response(payload, payload_len, &msg->payload.routing_resp);

    case DOIP_TYPE_DIAGNOSTIC_MESSAGE:
        return parse_diagnostic_message(payload, payload_len, &msg->payload.diagnostic);

    case DOIP_TYPE_DIAGNOSTIC_MESSAGE_ACK:
        return parse_diagnostic_ack(payload, payload_len, &msg->payload.diagnostic_ack);

    case DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK:
        return parse_diagnostic_nack(payload, payload_len, &msg->payload.diagnostic_nack);

    case DOIP_TYPE_ENTITY_STATUS_RESPONSE:
        return parse_entity_status_response(payload, payload_len, &msg->payload.entity_status);

    case DOIP_TYPE_DIAG_POWER_MODE_RESPONSE:
        if (payload_len >= 1)
            msg->payload.power_mode.power_mode = payload[0];
        break;

    case DOIP_TYPE_ALIVE_CHECK_REQUEST:
        /* No payload */
        break;

    case DOIP_TYPE_ALIVE_CHECK_RESPONSE:
        if (payload_len >= 2)
            msg->payload.alive_check.source_address = read_u16(payload);
        break;

    case DOIP_TYPE_VEHICLE_ID_REQUEST:
    case DOIP_TYPE_VEHICLE_ID_REQUEST_EID:
    case DOIP_TYPE_VEHICLE_ID_REQUEST_VIN:
    case DOIP_TYPE_ENTITY_STATUS_REQUEST:
    case DOIP_TYPE_DIAG_POWER_MODE_REQUEST:
        /* Requests with optional/no payload - parsed from raw */
        break;

    default:
        /* Unknown payload type - keep raw data */
        break;
    }

    return DOIP_OK;
}

/* ============================================================================
 * Payload Builders
 * ========================================================================== */

int doip_build_vehicle_id_request(uint8_t *buf, size_t buf_size)
{
    return doip_build_message(DOIP_TYPE_VEHICLE_ID_REQUEST, NULL, 0, buf, buf_size);
}

int doip_build_vehicle_id_request_eid(const uint8_t eid[DOIP_EID_LENGTH],
                                       uint8_t *buf, size_t buf_size)
{
    if (!eid)
        return DOIP_ERR_INVALID_PARAM;
    return doip_build_message(DOIP_TYPE_VEHICLE_ID_REQUEST_EID, eid, DOIP_EID_LENGTH, buf, buf_size);
}

int doip_build_vehicle_id_request_vin(const uint8_t vin[DOIP_VIN_LENGTH],
                                       uint8_t *buf, size_t buf_size)
{
    if (!vin)
        return DOIP_ERR_INVALID_PARAM;
    return doip_build_message(DOIP_TYPE_VEHICLE_ID_REQUEST_VIN, vin, DOIP_VIN_LENGTH, buf, buf_size);
}

int doip_build_vehicle_announcement(const doip_vehicle_id_response_t *vehicle,
                                     uint8_t *buf, size_t buf_size)
{
    if (!vehicle || !buf)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t payload[33];
    size_t offset = 0;

    memcpy(&payload[offset], vehicle->vin, DOIP_VIN_LENGTH);
    offset += DOIP_VIN_LENGTH;

    write_u16(&payload[offset], vehicle->logical_address);
    offset += 2;

    memcpy(&payload[offset], vehicle->eid, DOIP_EID_LENGTH);
    offset += DOIP_EID_LENGTH;

    memcpy(&payload[offset], vehicle->gid, DOIP_GID_LENGTH);
    offset += DOIP_GID_LENGTH;

    payload[offset++] = vehicle->further_action_required;

    if (vehicle->has_sync_status) {
        payload[offset++] = vehicle->vin_gid_sync_status;
    }

    return doip_build_message(DOIP_TYPE_VEHICLE_ANNOUNCEMENT, payload, (uint32_t)offset, buf, buf_size);
}

int doip_build_routing_activation_request(const doip_routing_activation_request_t *req,
                                           uint8_t *buf, size_t buf_size)
{
    if (!req || !buf)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t payload[11];
    size_t offset = 0;

    write_u16(&payload[offset], req->source_address);
    offset += 2;

    payload[offset++] = req->activation_type;

    write_u32(&payload[offset], req->reserved);
    offset += 4;

    if (req->has_oem_specific) {
        memcpy(&payload[offset], req->oem_specific, 4);
        offset += 4;
    }

    return doip_build_message(DOIP_TYPE_ROUTING_ACTIVATION_REQUEST, payload,
                              (uint32_t)offset, buf, buf_size);
}

int doip_build_routing_activation_response(const doip_routing_activation_response_t *resp,
                                            uint8_t *buf, size_t buf_size)
{
    if (!resp || !buf)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t payload[13];
    size_t offset = 0;

    write_u16(&payload[offset], resp->tester_logical_address);
    offset += 2;

    write_u16(&payload[offset], resp->entity_logical_address);
    offset += 2;

    payload[offset++] = resp->response_code;

    write_u32(&payload[offset], resp->reserved);
    offset += 4;

    if (resp->has_oem_specific) {
        memcpy(&payload[offset], resp->oem_specific, 4);
        offset += 4;
    }

    return doip_build_message(DOIP_TYPE_ROUTING_ACTIVATION_RESPONSE, payload,
                              (uint32_t)offset, buf, buf_size);
}

int doip_build_diagnostic_message(uint16_t source_addr, uint16_t target_addr,
                                   const uint8_t *uds_data, uint32_t uds_len,
                                   uint8_t *buf, size_t buf_size)
{
    if (!uds_data || !buf || uds_len == 0)
        return DOIP_ERR_INVALID_PARAM;

    uint32_t payload_len = 4 + uds_len;  /* SA(2) + TA(2) + data */
    if (buf_size < DOIP_HEADER_SIZE + payload_len)
        return DOIP_ERR_BUFFER_TOO_SMALL;

    uint8_t payload_header[4];
    write_u16(&payload_header[0], source_addr);
    write_u16(&payload_header[2], target_addr);

    /* Build header */
    doip_header_t header = {
        .protocol_version = DOIP_PROTOCOL_VERSION,
        .inverse_version  = (uint8_t)(DOIP_PROTOCOL_VERSION ^ 0xFF),
        .payload_type     = DOIP_TYPE_DIAGNOSTIC_MESSAGE,
        .payload_length   = payload_len,
    };

    int ret = doip_serialize_header(&header, buf, buf_size);
    if (ret < 0)
        return ret;

    memcpy(buf + DOIP_HEADER_SIZE, payload_header, 4);
    memcpy(buf + DOIP_HEADER_SIZE + 4, uds_data, uds_len);

    return DOIP_HEADER_SIZE + (int)payload_len;
}

int doip_build_diagnostic_ack(uint16_t source_addr, uint16_t target_addr,
                               uint8_t ack_code, uint8_t *buf, size_t buf_size)
{
    uint8_t payload[5];
    write_u16(&payload[0], source_addr);
    write_u16(&payload[2], target_addr);
    payload[4] = ack_code;

    return doip_build_message(DOIP_TYPE_DIAGNOSTIC_MESSAGE_ACK, payload, 5, buf, buf_size);
}

int doip_build_diagnostic_nack(uint16_t source_addr, uint16_t target_addr,
                                uint8_t nack_code, uint8_t *buf, size_t buf_size)
{
    uint8_t payload[5];
    write_u16(&payload[0], source_addr);
    write_u16(&payload[2], target_addr);
    payload[4] = nack_code;

    return doip_build_message(DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK, payload, 5, buf, buf_size);
}

int doip_build_alive_check_request(uint8_t *buf, size_t buf_size)
{
    return doip_build_message(DOIP_TYPE_ALIVE_CHECK_REQUEST, NULL, 0, buf, buf_size);
}

int doip_build_alive_check_response(uint16_t source_addr, uint8_t *buf, size_t buf_size)
{
    uint8_t payload[2];
    write_u16(payload, source_addr);
    return doip_build_message(DOIP_TYPE_ALIVE_CHECK_RESPONSE, payload, 2, buf, buf_size);
}

int doip_build_entity_status_request(uint8_t *buf, size_t buf_size)
{
    return doip_build_message(DOIP_TYPE_ENTITY_STATUS_REQUEST, NULL, 0, buf, buf_size);
}

int doip_build_entity_status_response(const doip_entity_status_response_t *status,
                                       uint8_t *buf, size_t buf_size)
{
    if (!status || !buf)
        return DOIP_ERR_INVALID_PARAM;

    uint8_t payload[7];
    payload[0] = status->node_type;
    payload[1] = status->max_concurrent_sockets;
    payload[2] = status->currently_open_sockets;

    uint32_t len = 3;
    if (status->has_max_data_size) {
        write_u32(&payload[3], status->max_data_size);
        len = 7;
    }

    return doip_build_message(DOIP_TYPE_ENTITY_STATUS_RESPONSE, payload, len, buf, buf_size);
}

int doip_build_header_nack(uint8_t nack_code, uint8_t *buf, size_t buf_size)
{
    return doip_build_message(DOIP_TYPE_HEADER_NACK, &nack_code, 1, buf, buf_size);
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

const char *doip_payload_type_str(doip_payload_type_t type)
{
    switch (type) {
    case DOIP_TYPE_HEADER_NACK:                     return "Header NACK";
    case DOIP_TYPE_VEHICLE_ID_REQUEST:              return "Vehicle ID Request";
    case DOIP_TYPE_VEHICLE_ID_REQUEST_EID:          return "Vehicle ID Request (EID)";
    case DOIP_TYPE_VEHICLE_ID_REQUEST_VIN:          return "Vehicle ID Request (VIN)";
    case DOIP_TYPE_VEHICLE_ANNOUNCEMENT:            return "Vehicle Announcement";
    case DOIP_TYPE_ROUTING_ACTIVATION_REQUEST:      return "Routing Activation Request";
    case DOIP_TYPE_ROUTING_ACTIVATION_RESPONSE:     return "Routing Activation Response";
    case DOIP_TYPE_ALIVE_CHECK_REQUEST:             return "Alive Check Request";
    case DOIP_TYPE_ALIVE_CHECK_RESPONSE:            return "Alive Check Response";
    case DOIP_TYPE_ENTITY_STATUS_REQUEST:           return "Entity Status Request";
    case DOIP_TYPE_ENTITY_STATUS_RESPONSE:          return "Entity Status Response";
    case DOIP_TYPE_DIAG_POWER_MODE_REQUEST:         return "Diagnostic Power Mode Request";
    case DOIP_TYPE_DIAG_POWER_MODE_RESPONSE:        return "Diagnostic Power Mode Response";
    case DOIP_TYPE_DIAGNOSTIC_MESSAGE:              return "Diagnostic Message";
    case DOIP_TYPE_DIAGNOSTIC_MESSAGE_ACK:          return "Diagnostic Message ACK";
    case DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK:         return "Diagnostic Message NACK";
    default:                                        return "Unknown";
    }
}

const char *doip_nack_code_str(doip_header_nack_code_t code)
{
    switch (code) {
    case DOIP_HEADER_NACK_INCORRECT_PATTERN:        return "Incorrect pattern format";
    case DOIP_HEADER_NACK_UNKNOWN_PAYLOAD_TYPE:     return "Unknown payload type";
    case DOIP_HEADER_NACK_MESSAGE_TOO_LARGE:        return "Message too large";
    case DOIP_HEADER_NACK_OUT_OF_MEMORY:            return "Out of memory";
    case DOIP_HEADER_NACK_INVALID_PAYLOAD_LENGTH:   return "Invalid payload length";
    default:                                        return "Unknown NACK code";
    }
}

const char *doip_routing_response_str(uint8_t code)
{
    switch (code) {
    case 0x00: return "Denied - Unknown source address";
    case 0x01: return "Denied - All sockets registered/active";
    case 0x02: return "Denied - SA different from already registered";
    case 0x03: return "Denied - SA already active on different socket";
    case 0x04: return "Denied - Missing authentication";
    case 0x05: return "Denied - Rejected confirmation";
    case 0x06: return "Denied - Unsupported activation type, TLS required";
    case 0x10: return "Success";
    case 0x11: return "Success - Confirmation required";
    default:   return "Unknown routing response code";
    }
}

const char *doip_diag_nack_str(doip_diag_nack_code_t code)
{
    switch (code) {
    case DOIP_DIAG_NACK_INVALID_SA:                 return "Invalid source address";
    case DOIP_DIAG_NACK_UNKNOWN_TA:                 return "Unknown target address";
    case DOIP_DIAG_NACK_MESSAGE_TOO_LARGE:          return "Diagnostic message too large";
    case DOIP_DIAG_NACK_OUT_OF_MEMORY:              return "Out of memory";
    case DOIP_DIAG_NACK_TARGET_UNREACHABLE:         return "Target unreachable";
    case DOIP_DIAG_NACK_UNKNOWN_NETWORK:            return "Unknown network";
    case DOIP_DIAG_NACK_TRANSPORT_PROTOCOL_ERROR:   return "Transport protocol error";
    default:                                        return "Unknown diagnostic NACK code";
    }
}

const char *doip_result_str(doip_result_t result)
{
    switch (result) {
    case DOIP_OK:                   return "OK";
    case DOIP_ERR_INVALID_PARAM:    return "Invalid parameter";
    case DOIP_ERR_BUFFER_TOO_SMALL: return "Buffer too small";
    case DOIP_ERR_INVALID_HEADER:   return "Invalid header";
    case DOIP_ERR_SOCKET:           return "Socket error";
    case DOIP_ERR_CONNECT:          return "Connection error";
    case DOIP_ERR_TIMEOUT:          return "Timeout";
    case DOIP_ERR_SEND:             return "Send error";
    case DOIP_ERR_RECV:             return "Receive error";
    case DOIP_ERR_NACK:             return "NACK received";
    case DOIP_ERR_ROUTING_DENIED:   return "Routing activation denied";
    case DOIP_ERR_MEMORY:           return "Memory allocation error";
    case DOIP_ERR_CLOSED:           return "Connection closed";
    default:                        return "Unknown error";
    }
}

void doip_hex_dump(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (i % 16) == 0)
            printf("\n");
        printf("%02X ", data[i]);
    }
    printf("\n");
}

void doip_print_message(const doip_message_t *msg)
{
    if (!msg)
        return;

    printf("=== DoIP Message ===\n");
    printf("  Version:  0x%02X (inv: 0x%02X)\n",
           msg->header.protocol_version, msg->header.inverse_version);
    printf("  Type:     0x%04X (%s)\n",
           msg->header.payload_type,
           doip_payload_type_str((doip_payload_type_t)msg->header.payload_type));
    printf("  Length:   %u bytes\n", msg->header.payload_length);

    switch (msg->header.payload_type) {
    case DOIP_TYPE_VEHICLE_ANNOUNCEMENT: {
        const doip_vehicle_id_response_t *v = &msg->payload.vehicle_id;
        printf("  VIN:      %.17s\n", v->vin);
        printf("  LogAddr:  0x%04X\n", v->logical_address);
        printf("  EID:      %02X:%02X:%02X:%02X:%02X:%02X\n",
               v->eid[0], v->eid[1], v->eid[2], v->eid[3], v->eid[4], v->eid[5]);
        printf("  GID:      %02X:%02X:%02X:%02X:%02X:%02X\n",
               v->gid[0], v->gid[1], v->gid[2], v->gid[3], v->gid[4], v->gid[5]);
        printf("  Action:   0x%02X\n", v->further_action_required);
        break;
    }
    case DOIP_TYPE_ROUTING_ACTIVATION_RESPONSE: {
        const doip_routing_activation_response_t *r = &msg->payload.routing_resp;
        printf("  Tester:   0x%04X\n", r->tester_logical_address);
        printf("  Entity:   0x%04X\n", r->entity_logical_address);
        printf("  Code:     0x%02X (%s)\n", r->response_code,
               doip_routing_response_str(r->response_code));
        break;
    }
    case DOIP_TYPE_DIAGNOSTIC_MESSAGE: {
        const doip_diagnostic_message_t *d = &msg->payload.diagnostic;
        printf("  SA:       0x%04X\n", d->source_address);
        printf("  TA:       0x%04X\n", d->target_address);
        printf("  UDS Data: ");
        doip_hex_dump(d->data, d->data_length > 32 ? 32 : d->data_length);
        if (d->data_length > 32)
            printf("  ... (%u bytes total)\n", d->data_length);
        break;
    }
    case DOIP_TYPE_DIAGNOSTIC_MESSAGE_ACK:
        printf("  SA:       0x%04X\n", msg->payload.diagnostic_ack.source_address);
        printf("  TA:       0x%04X\n", msg->payload.diagnostic_ack.target_address);
        printf("  ACK Code: 0x%02X\n", msg->payload.diagnostic_ack.ack_code);
        break;

    case DOIP_TYPE_DIAGNOSTIC_MESSAGE_NACK:
        printf("  SA:       0x%04X\n", msg->payload.diagnostic_nack.source_address);
        printf("  TA:       0x%04X\n", msg->payload.diagnostic_nack.target_address);
        printf("  NACK:     0x%02X (%s)\n", msg->payload.diagnostic_nack.nack_code,
               doip_diag_nack_str((doip_diag_nack_code_t)msg->payload.diagnostic_nack.nack_code));
        break;

    default:
        if (msg->raw_payload_size > 0) {
            printf("  Raw:      ");
            doip_hex_dump(msg->raw_payload,
                          msg->raw_payload_size > 64 ? 64 : msg->raw_payload_size);
        }
        break;
    }
    printf("====================\n");
}
