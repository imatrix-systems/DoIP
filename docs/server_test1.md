# DoIP Server Comprehensive Test Plan — `server_test1`

**Version**: 2.2 — Addresses Round 1-3 review findings (22 fixes R1, 9 fixes R2, 2 fixes R3)
**Date**: 2026-03-05
**Scope**: Script-based validation of UDP discovery, TCP diagnostics, and blob read/write transfer
**Target**: `~/iMatrix/DOIP_Server/` (deployable DoIP blob server)

---

## Table of Contents

1. [Overview](#1-overview)
2. [Test Infrastructure](#2-test-infrastructure)
3. [Test Suite A: UDP Discovery & Broadcast](#3-test-suite-a-udp-discovery--broadcast)
4. [Test Suite B: TCP Connection & Protocol Compliance](#4-test-suite-b-tcp-connection--protocol-compliance)
5. [Test Suite C: Blob Write (Download to Server)](#5-test-suite-c-blob-write-download-to-server)
6. [Test Suite D: Error Handling & Negative Tests](#6-test-suite-d-error-handling--negative-tests)
7. [Test Suite E: Concurrent Access & Stress](#7-test-suite-e-concurrent-access--stress)
8. [Test Suite F: Configuration & CLI Validation](#8-test-suite-f-configuration--cli-validation)
9. [Implementation Details](#9-implementation-details)
10. [Build & Run](#10-build--run)
11. [Appendices](#11-appendices)

---

## 1. Overview

### 1.1 Purpose

This plan defines a comprehensive, script-driven test suite that validates the DoIP blob server beyond the existing 6-test discovery/connectivity suite (`test_discovery.c`). The new test tool (`test_server.c`) exercises the full DoIP protocol stack: UDP broadcast responses, TCP diagnostic message exchange, and the complete blob transfer pipeline (RequestDownload, TransferData, RequestTransferExit with CRC-32 verification and disk storage).

### 1.2 Scope

| Area | Coverage |
|------|----------|
| UDP Discovery | Broadcast, VIN/EID filtering, announcement format validation |
| TCP Protocol | Routing activation, entity status, power mode, alive check, header NACK |
| Blob Write | Full transfer cycle: allocate, send blocks, CRC verify, save to disk |
| Error Handling | UDS negative responses, sequence errors, size overflow, CRC mismatch |
| Concurrency | Multiple TCP clients, concurrent transfer rejection, connection limits |
| Configuration | Config file loading, CLI overrides, default fallback |

### 1.3 What Is NOT Tested

- **Blob upload (read from server)**: The server implements `RequestDownload` (client→server write) only. `RequestUpload` (0x35, server→client read) is not implemented in the server's UDS handler. The client library has `doip_client_flash_upload()` but the server does not service it.
- **Security access**: The server has no session/security requirements — routing activation always succeeds.
- **TLS/encryption**: Not implemented.
- **Multi-hop routing**: Single-entity server, no ECU forwarding.

### 1.4 Relationship to Existing Tests

The existing `test_discovery.c` (6 tests) remains unchanged and continues to serve as a fast smoke test. This new `test_server.c` is a superset that includes equivalent discovery checks plus the full transfer pipeline and protocol compliance tests.

---

## 2. Test Infrastructure

### 2.1 Test Tool Binary

```
~/iMatrix/DOIP_Server/
├── test/
│   ├── test_discovery.c      # Existing: 6-test smoke suite
│   └── test_server.c         # NEW: comprehensive server validation
```

**Build target**: `test-server`
**Link dependencies**: `src/doip.c`, `src/doip_client.c`, `src/config.c` (same as `test-discovery`)

### 2.2 Usage

```
./test-server [-c doip-server.conf] [server_ip] [port] [-v]
```

| Flag | Description | Default |
|------|-------------|---------|
| `-c path` | Config file for expected identity values | `doip-server.conf` in CWD |
| `server_ip` | Server IP address (positional arg 1) | `127.0.0.1` |
| `port` | Server TCP/UDP port (positional arg 2) | `13400` |
| `-v` | Verbose output (print hex dumps of packets) | off |

### 2.3 Test Framework

Reuse the lightweight macro framework from `test_discovery.c`:

```c
static int g_passed = 0;
static int g_failed = 0;

#define TEST_START(name)  printf("\n--- Test %d: %s ---\n", g_passed+g_failed+1, (name))
#define TEST_PASS(name)   do { printf("  PASS: %s\n", (name)); g_passed++; } while(0)
#define TEST_FAIL(name, ...) do { printf("  FAIL: %s — ", (name)); printf(__VA_ARGS__); printf("\n"); g_failed++; } while(0)
```

### 2.4 Helper Functions

The test tool needs these internal helpers (some reused from `test_discovery.c`):

| Helper | Purpose |
|--------|---------|
| `udp_send_recv()` | Send raw UDP packet, receive with timeout (existing) |
| `udp_drain_recv()` | Drain stale responses from UDP socket (new — prevents cross-test contamination) |
| `tcp_connect_and_activate()` | Connect + routing activation in one call (new) |
| `tcp_raw_connect()` | Open raw TCP socket for protocol-level tests B.7, B.9, B.10 (new) |
| `send_uds_expect()` | Send UDS via `doip_client_send_uds()`, validate response SID/NRC (new) |
| `build_request_download()` | Build UDS 0x34 request with address/size format (new) |
| `build_transfer_data()` | Build UDS 0x36 request with block sequence + payload (new) |
| `build_transfer_exit()` | Build UDS 0x37 request (new) |
| `crc32_compute()` | CRC-32 (polynomial 0xEDB88320) for test data verification (new) |
| `generate_test_blob()` | Fill buffer with known pattern + appended CRC-32 (new) |
| `verify_blob_on_disk()` | Poll for latest .bin file in storage dir, compare contents (retry loop, up to 500ms) (new) |
| `clear_blob_storage()` | Remove all `*.bin` files from storage dir before blob write tests (new) |
| `hex_dump()` | Print hex dump of buffer for verbose mode (new) |

### 2.5 Makefile Integration

```makefile
TEST_SERVER_SRCS = test/test_server.c src/doip.c src/doip_client.c src/config.c

test-server: $(TEST_SERVER_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(TEST_SERVER_SRCS) $(LDFLAGS)

test-full: doip-server test-server
	@echo "Cleaning up stale server processes..."
	@pkill -f 'doip-server.*13400' 2>/dev/null; sleep 0.5
	@echo "Starting server..."
	@./doip-server -c doip-server.conf & SERVER_PID=$$!; \
	trap "kill $$SERVER_PID 2>/dev/null; wait $$SERVER_PID 2>/dev/null" EXIT; \
	for i in $$(seq 1 10); do \
	    (echo > /dev/tcp/127.0.0.1/13400) 2>/dev/null && break; sleep 0.5; \
	done; \
	echo "Running full test suite..."; \
	timeout 180 ./test-server -c doip-server.conf 127.0.0.1 13400; \
	TEST_RESULT=$$?; \
	kill $$SERVER_PID 2>/dev/null; wait $$SERVER_PID 2>/dev/null; \
	trap - EXIT; \
	exit $$TEST_RESULT
```

### 2.6 Server Configuration for Testing

The tests use the standard `doip-server.conf` with these values:

```ini
vin                 = FC1BLOBSRV0000001
logical_address     = 0x0001
eid                 = 00:1A:2B:3C:4D:5E
gid                 = 00:1A:2B:3C:4D:5E
bind_address        = 127.0.0.1
tcp_port            = 13400
udp_port            = 13400
max_tcp_connections = 4
max_data_size       = 4096
blob_storage_dir    = /tmp/doip_blobs
blob_max_size       = 16777216
transfer_timeout    = 30
```

Tests validate against these expected values loaded via `doip_config_load()`.

---

## 3. Test Suite A: UDP Discovery & Broadcast

**Purpose**: Validate the server's UDP vehicle identification responder handles all request types correctly and produces spec-compliant announcement responses.

### Test A.1: Generic Vehicle ID Request (Broadcast)

**Description**: Send a `DOIP_TYPE_VEHICLE_ID_REQUEST` (0x0001) to the server's UDP port and validate the announcement response.

**Procedure**:
1. Build request: `doip_build_vehicle_id_request(req, sizeof(req))`
2. Send via `udp_send_recv(server_ip, port, req, req_len, resp, 256, 2000)`
3. Parse response: `doip_parse_message(resp, resp_len, &msg)`

**Validation**:
- [ ] Response received within 2000 ms
- [ ] `msg.header.payload_type == DOIP_TYPE_VEHICLE_ANNOUNCEMENT` (0x0004)
- [ ] `msg.header.protocol_version == 0x03`
- [ ] `msg.header.inverse_version == 0xFC`
- [ ] `msg.payload.vehicle_id.vin` matches config VIN (`FC1BLOBSRV0000001`)
- [ ] `msg.payload.vehicle_id.logical_address` matches config (0x0001)
- [ ] `msg.payload.vehicle_id.eid` matches config EID (00:1A:2B:3C:4D:5E)
- [ ] `msg.payload.vehicle_id.gid` matches config GID (00:1A:2B:3C:4D:5E)
- [ ] `msg.payload.vehicle_id.further_action_required == 0x00`
- [ ] `msg.payload.vehicle_id.vin_gid_sync_status == 0x00` (if `has_sync_status`)

**Pass Criteria**: All fields match expected config values.

---

### Test A.2: Vehicle ID Request by VIN (Positive + Negative)

**Description**: Send a VIN-filtered request with the correct VIN (expect response), then with a wrong VIN (expect no response).

**Procedure**:
1. **Positive**: Build request: `doip_build_vehicle_id_request_vin(expected.server.vin, req, sizeof(req))`
2. Send via `udp_send_recv()` with 2000 ms timeout
3. Parse response, verify payload type and VIN match
4. **Negative**: Drain UDP socket with `udp_drain_recv()` to clear stale responses
5. Build request with wrong VIN: `memset(wrong_vin, 'X', 17)`
6. Send via `udp_send_recv()` with 1500 ms timeout

**Validation**:
- [ ] Positive: Response received with `DOIP_TYPE_VEHICLE_ANNOUNCEMENT` and matching VIN
- [ ] Negative: No response received (timeout)

**Pass Criteria**: Correct VIN gets response; wrong VIN gets silence.

---

### Test A.3: Vehicle ID Request by EID (Positive + Negative)

**Description**: Send an EID-filtered request with the correct EID (expect response), then with wrong EID (expect no response).

**Procedure**:
1. **Positive**: Build request: `doip_build_vehicle_id_request_eid(expected.server.eid, req, sizeof(req))`
2. Send via `udp_send_recv()` with 2000 ms timeout
3. Parse response, verify payload type and EID match
4. **Negative**: Drain UDP socket with `udp_drain_recv()`
5. Build request with wrong EID: `{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}`
6. Send via `udp_send_recv()` with 1500 ms timeout

**Validation**:
- [ ] Positive: Response received with matching EID
- [ ] Negative: No response received (timeout)

**Pass Criteria**: Correct EID gets response; wrong EID gets silence.

---

---

## 4. Test Suite B: TCP Connection & Protocol Compliance

**Purpose**: Validate TCP connection lifecycle, routing activation, entity status, power mode, alive check, and protocol error responses.

### Test B.1: TesterPresent (0x3E) After Routing

**Description**: Send TesterPresent keepalive and validate the positive response.

**Procedure**:
1. Connect + activate routing
2. `doip_client_uds_tester_present(&client)`

**Validation**:
- [ ] Returns `DOIP_OK`
- [ ] (Verbose mode: verify response bytes are `0x7E 0x00`)

**Cleanup**: Disconnect

---

### Test B.2: TesterPresent with Suppress Positive Response Bit

**Description**: Send TesterPresent with the `suppressPosRspMsgIndicationBit` set (subfunction byte = 0x80). The server should NOT send a response.

**Procedure**:
1. Connect + activate routing
2. Build raw UDS: `{0x3E, 0x80}`
3. Send via `doip_client_send_uds()` with a short timeout (500 ms)

**Validation**:
- [ ] Return value indicates timeout or zero-length response (no positive response sent)
- [ ] Connection remains alive (subsequent TesterPresent without suppress bit works)

---

### Test B.3: Entity Status Request

**Description**: Query the server's entity status (node type, connection counts, max data size).

**Procedure**:
1. Connect + activate routing
2. `doip_client_get_entity_status(&client, &status)`

**Validation**:
- [ ] Returns `DOIP_OK`
- [ ] `status.node_type == 0` (gateway)
- [ ] `status.max_concurrent_sockets == 4` (from config `max_tcp_connections`)
- [ ] `status.currently_open_sockets >= 1` (at least this test's connection)
- [ ] `status.max_data_size == 4096` (from config `max_data_size`)

---

### Test B.4: Diagnostic Power Mode Request

**Description**: Query the server's diagnostic power mode.

**Procedure**:
1. Connect + activate routing
2. `doip_client_get_power_mode(&client, &mode)`

**Validation**:
- [ ] Returns `DOIP_OK`
- [ ] Power mode value is `0x01` (ready)

---

### Test B.5: Unsupported UDS Service

**Description**: Send an unsupported UDS SID (e.g., 0x10 DiagnosticSessionControl) and expect a negative response.

**Procedure**:
1. Connect + activate routing
2. Build UDS: `{0x10, 0x01}` (DiagnosticSessionControl, default session)
3. Send via `doip_client_send_uds()` with 2000 ms timeout

**Validation**:
- [ ] Response received
- [ ] Response bytes: `0x7F, 0x10, 0x11` (negative response, SID=0x10, NRC=serviceNotSupported)

---

### Test B.6: Diagnostic Message Without Routing Activation

**Description**: Send a diagnostic message over TCP without first performing routing activation. The server should reject it silently (no diagnostic response).

**Procedure**:
1. Open raw TCP socket via `tcp_raw_connect()` (do NOT use client library — `doip_client_send_diagnostic()` rejects calls without routing locally)
2. Manually build a DoIP diagnostic message (type 0x8001) using `doip_build_diagnostic_message(0x0E80, 0x0001, uds_data, len, buf, sizeof(buf))`
3. Send with raw `send()`, receive with raw `recv()` using 1000 ms timeout

**Validation**:
- [ ] No diagnostic response or NACK received (server drops unactivated messages silently)
- [ ] Connection is not terminated (can send routing activation request afterward and succeed)

---

### Test B.7: Diagnostic Message to Unknown Target Address

**Description**: Send a diagnostic message to an unregistered target address (e.g., 0xFFFF). Server should respond with a diagnostic NACK.

**Procedure**:
1. Connect + activate routing
2. Send diagnostic via `doip_client_send_diagnostic(&client, 0xFFFF, uds_data, uds_len)`
3. Receive response via `doip_client_recv_message(&client, &msg, 2000)` to capture the full NACK message

**Validation**:
- [ ] Receive `DOIP_TYPE_DIAGNOSTIC_NACK` (0x8003)
- [ ] `msg.payload.diagnostic_nack.nack_code == DOIP_DIAG_NACK_UNKNOWN_TA` (0x03)

**Note**: Cannot use `doip_client_send_uds()` here because it returns `DOIP_ERR_NACK` without exposing the NACK code byte. Must use the lower-level `send_diagnostic` + `recv_message` pair.

---

### Test B.8: Header NACK — Invalid Protocol Version

**Description**: Send a TCP message with an invalid DoIP header (wrong version byte). Server should respond with a header NACK and close the connection.

**Procedure**:
1. Open a raw TCP socket to server (do NOT use client library)
2. Send 8 bytes with mismatched version/inverse: `{0x03, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x07}` (version=0x03, inverse=0x00 — should be 0xFC)
3. Receive response

**Validation**:
- [ ] Response received: header NACK (payload type 0x0000)
- [ ] NACK code byte = `DOIP_HEADER_NACK_INCORRECT_PATTERN` (0x00)
- [ ] Server closes the TCP connection after sending the NACK

---

### Test B.9: Header NACK — Unknown Payload Type

**Description**: Send a message with a valid DoIP header but unknown payload type (e.g., 0x9999).

**Procedure**:
1. Open raw TCP socket
2. Send valid DoIP header with routing activation first (to establish connection)
3. Send message with payload_type = 0x9999, payload_length = 0

**Validation**:
- [ ] Response: header NACK with code `DOIP_HEADER_NACK_UNKNOWN_PAYLOAD_TYPE` (0x01)

---

### Test B.10: TCP Connection Limit (max_tcp_connections)

**Description**: Open `max_tcp_connections` (4) simultaneous TCP connections, then attempt a 5th. The 5th should be rejected.

**Procedure**:
1. Open 4 TCP connections, activate routing on each
2. Attempt 5th connection
3. Send routing activation on 5th connection

**Validation**:
- [ ] First 4 connections succeed (routing activation returns SUCCESS)
- [ ] 5th connection is either refused at TCP level OR routing activation returns a rejection code
- [ ] After closing one of the first 4, a new connection succeeds

---

### Test B.11: Entity Status — Socket Count Accuracy

**Description**: Open multiple connections and verify the entity status `currently_open_sockets` count is accurate.

**Procedure**:
1. Connect client A, activate routing
2. Query entity status via client A → note `currently_open_sockets` = N
3. Connect client B, activate routing
4. Query entity status via client A → should now be N+1
5. Disconnect client B
6. Poll entity status via client A up to 5 times (500 ms intervals) until `currently_open_sockets` decrements back to N

**Validation**:
- [ ] Socket count increments on new connection
- [ ] Socket count decrements after disconnection (within 2.5 seconds)

---

### Test B.12: Diagnostic Message — Source Address Mismatch

**Description**: Send a diagnostic message with a source address that doesn't match the tester address established during routing activation. Server should respond with a diagnostic NACK.

**Procedure**:
1. Connect + activate routing (tester address = 0x0E80)
2. Build a raw DoIP diagnostic message (type 0x8001) with `source_address = 0x1234` (mismatched) and `target_address = 0x0001`
3. Send via raw TCP on the client's socket, receive response

**Validation**:
- [ ] Receive `DOIP_TYPE_DIAGNOSTIC_NACK` (0x8003)
- [ ] NACK code = `DOIP_DIAG_NACK_INVALID_SA` (0x02)

---

### Test B.13: Header NACK — Message Too Large

**Description**: Send a DoIP message with a payload_length exceeding `DOIP_MAX_DIAGNOSTIC_SIZE` (4096). Server should respond with a header NACK but keep the connection open.

**Procedure**:
1. Open raw TCP socket
2. Perform routing activation via raw socket (build and send routing activation request)
3. Send ONLY the 8-byte DoIP header with `payload_length = 5000` and a recognized payload type (e.g., 0x8001). Do NOT send any payload bytes — the server checks the length field in the header before reading payload data.
4. Receive response

**Validation**:
- [ ] Response: header NACK with code `DOIP_HEADER_NACK_MESSAGE_TOO_LARGE` (0x02)
- [ ] Connection remains alive (server uses `continue`, not `break`)

**Note**: Sending the full 5000 bytes of payload data would desync the connection — the server sends the NACK after reading just the header, then re-enters the recv loop expecting a new header. Any trailing payload bytes would be misinterpreted as the next header, causing a second NACK and connection close.

---

### Test B.14: Header NACK — Invalid Payload Length (Parse Failure)

**Description**: Send a routing activation request (type 0x0005) with a payload shorter than expected. Server should respond with a header NACK for invalid payload length.

**Procedure**:
1. Open raw TCP socket
2. Build a DoIP header: version=0x03, inverse=0xFC, type=0x0005, payload_length=2
3. Send header + 2 bytes of payload (routing activation expects 7+ bytes)
4. Receive response

**Validation**:
- [ ] Response: header NACK with code `DOIP_HEADER_NACK_INVALID_PAYLOAD_LENGTH` (0x04)
- [ ] Connection remains alive

---

## 5. Test Suite C: Blob Write (Download to Server)

**Purpose**: Validate the complete blob transfer pipeline — RequestDownload, TransferData blocks with sequencing, RequestTransferExit with CRC-32 verification, and blob persistence to disk.

### 5.1 CRC-32 Reference Implementation

The test tool includes its own CRC-32 implementation (polynomial `0xEDB88320`, same as zlib/IEEE 802.3) to generate and verify blob data independently from the server's CRC-32:

```c
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
```

### 5.2 Test Blob Generation

```c
/* Generate a test blob of 'size' bytes with known pattern + 4-byte CRC-32 suffix.
 * Total buffer needed: size + 4 bytes.
 * Pattern: each byte = (index & 0xFF) XOR 0xA5 for easy visual identification.
 */
static void generate_test_blob(uint8_t *buf, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
        buf[i] = (uint8_t)((i & 0xFF) ^ 0xA5);

    uint32_t crc = test_crc32(buf, size);
    memcpy(&buf[size], &crc, 4);  /* Append CRC in host byte order */
}
```

### 5.3 Blob Disk Verification

```c
/* Find the most recently modified .bin file in blob_storage_dir.
 * Read its contents and compare to expected data.
 * Returns 0 on match, -1 on mismatch or error.
 */
static int verify_blob_on_disk(const char *storage_dir,
                               const uint8_t *expected_data, uint32_t expected_size);
```

The verification function:
1. Scans `storage_dir` for `*.bin` files using `opendir()`/`readdir()`
2. Finds the file with the most recent `st_mtime`
3. Verifies file size matches `expected_size`
4. Reads file contents and does `memcmp()` against `expected_data`
5. Returns 0 on match

---

### Test C.1: Small Blob Transfer (Single Block)

**Description**: Transfer a small blob (100 bytes + 4 CRC = 104 bytes total) that fits in a single TransferData block.

**Procedure**:
1. Connect + activate routing
2. Generate 100-byte test blob with CRC suffix (104 bytes total)
3. Clear storage dir: `clear_blob_storage()` (removes all `*.bin` files)
4. **RequestDownload** (0x34):
   - Memory address: `0x00001000` (4-byte address)
   - Memory size: `104` (4-byte size)
   - Address/Length format: `0x44` (4 bytes addr, 4 bytes size)
   - Send: `{0x34, 0x00, 0x44, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x68}`
5. Validate positive response: `{0x74, 0x20, max_block_hi, max_block_lo}`
   - Extract `max_block_length` from response
6. **TransferData** (0x36):
   - Block sequence counter: 1
   - Data: full 104 bytes
   - Send: `{0x36, 0x01, <104 bytes>}`
7. Validate positive response: `{0x76, 0x01}`
8. **RequestTransferExit** (0x37):
   - Send: `{0x37}`
9. Validate positive response: `{0x77}`
10. Verify blob on disk (retry loop up to 500ms, 50ms intervals):
    - File exists in `/tmp/doip_blobs/`
    - File size = 100 bytes (data without CRC)
    - File contents match the first 100 bytes of generated data

**Validation**:
- [ ] RequestDownload returns positive response (SID 0x74)
- [ ] `max_block_length` value is reasonable (expected: 4096)
- [ ] TransferData returns positive response with echoed block sequence (SID 0x76, BSC=1)
- [ ] RequestTransferExit returns positive response (SID 0x77)
- [ ] Blob file written to disk with correct size (100 bytes, CRC stripped)
- [ ] Blob file contents match expected data byte-for-byte

**Pass Criteria**: Complete transfer cycle succeeds, disk file verified.

---

### Test C.2: Multi-Block Blob Transfer

**Description**: Transfer a larger blob (8000 bytes + 4 CRC = 8004 bytes) that requires multiple TransferData blocks. The server advertises `max_block_length = 4096` but the DoIP transport layer limits payloads to 4096 bytes (which includes 4 bytes of SA+TA overhead), so effective max UDS data per block is 4090 bytes (4096 - 2 SID/BSC - 4 SA/TA).

**Procedure**:
1. Connect + activate routing
2. Generate 8000-byte test blob with CRC suffix (8004 bytes total)
3. Clear storage dir
4. **RequestDownload**: addr=0x00002000, size=8004
5. Validate positive response, extract `max_block_length`
6. Compute per-block data capacity: `block_data_size = min(max_block_length, 4092) - 2` (cap at 4092 for DoIP transport framing, subtract SID + BSC = 4090 bytes of user data per block)
7. **TransferData loop**:
   - Block 1 (BSC=1): bytes [0 .. block_data_size-1]
   - Block 2 (BSC=2): bytes [block_data_size .. 2*block_data_size-1]
   - Block N: remaining bytes
   - For each block: validate positive response `{0x76, BSC}`
8. **RequestTransferExit**: validate `{0x77}`
9. Verify blob on disk: 8000 bytes, contents match

**Validation**:
- [ ] RequestDownload accepted
- [ ] All TransferData blocks accepted with correct BSC echoed
- [ ] Number of blocks = `ceil(8004 / block_data_size)` where `block_data_size = min(max_block_length, 4092) - 2`
- [ ] RequestTransferExit accepted (CRC verification passed on server)
- [ ] Disk file = 8000 bytes, contents match generated data

---

### Test C.3: Large Blob Transfer with BSC Wrap-Around

**Description**: Transfer a blob large enough to make the block sequence counter wrap from 0xFF to 0x00 (>255 blocks). This exercises sustained throughput, large malloc, and BSC modular arithmetic. Size chosen to guarantee at least 257 blocks: `256 * 4090 + 100` bytes of data = 1,047,140 bytes + 4 CRC = 1,047,144 bytes total.

**Procedure**:
1. Connect + activate routing
2. Generate 1,047,140-byte test blob with CRC suffix (1,047,144 bytes total)
3. Clear storage dir: `clear_blob_storage()`
4. RequestDownload: addr=0x00100000, size=1047144
5. TransferData loop (257 blocks at 4090 bytes each, last block partial)
   - At block 255 (BSC=0xFF): verify response `{0x76, 0xFF}`
   - At block 256 (BSC=0x00): verify response `{0x76, 0x00}` (wrap!)
   - At block 257 (BSC=0x01): verify response `{0x76, 0x01}`
6. RequestTransferExit
7. Verify blob on disk (retry loop): 1,047,140 bytes, byte-for-byte match

**Validation**:
- [ ] All TransferData blocks accepted
- [ ] BSC wraps from 0xFF → 0x00 → 0x01 without error
- [ ] RequestTransferExit succeeds (CRC verified)
- [ ] Disk file = 1,048,164 bytes, contents match

**Pass Criteria**: Full transfer succeeds with BSC wrap verified.

---

### Test C.4: Back-to-Back Transfers

**Description**: Perform two consecutive blob transfers to verify the server correctly resets state between transfers.

**Procedure**:
1. Connect + activate routing
2. Transfer blob A (500 bytes + CRC): full cycle (RequestDownload → TransferData → RequestTransferExit)
3. Verify blob A on disk (500 bytes)
4. Transfer blob B (700 bytes + CRC): full cycle, different memory address (0x3000)
5. Verify blob B on disk (700 bytes, different content from blob A)

**Validation**:
- [ ] Both transfers succeed independently
- [ ] Two distinct blob files on disk
- [ ] Each file has correct size and contents
- [ ] No state leakage from transfer A to transfer B

---

### Test C.5: Minimum Size Blob (1 byte + CRC)

**Description**: Transfer the smallest possible blob (1 byte of data + 4-byte CRC = 5 bytes total).

**Procedure**:
1. Connect + activate routing
2. Generate 1-byte blob (using `generate_test_blob(1)` which produces `0xA5`) with CRC suffix
3. Clear storage dir: `clear_blob_storage()`
4. RequestDownload: addr=0x0000, size=5
5. TransferData: BSC=1, data=5 bytes
6. RequestTransferExit
7. Verify disk (retry loop): 1 byte, value `0xA5`

**Validation**:
- [ ] Transfer succeeds
- [ ] Disk file is exactly 1 byte
- [ ] Byte value matches

---

## 6. Test Suite D: Error Handling & Negative Tests

**Purpose**: Validate that the server correctly rejects malformed, out-of-sequence, and out-of-range requests with appropriate UDS negative response codes.

### Test D.1: RequestDownload — Transfer Already Active

**Description**: Start a transfer (RequestDownload), then send a second RequestDownload without completing the first.

**Procedure**:
1. Connect + activate routing
2. RequestDownload #1: addr=0x1000, size=100 → expect positive response
3. RequestDownload #2: addr=0x2000, size=200 → expect negative response

**Validation**:
- [ ] First RequestDownload succeeds (SID 0x74)
- [ ] Second RequestDownload returns negative: `{0x7F, 0x34, 0x70}` (NRC = uploadDownloadNotAccepted)
- [ ] First transfer is NOT affected (can still send TransferData for it)

---

### Test D.2: RequestDownload — Size Exceeds blob_max_size

**Description**: Request a download with `memory_size` exceeding `blob_max_size` (16 MB).

**Procedure**:
1. Connect + activate routing
2. RequestDownload: addr=0x0000, size=`16*1024*1024 + 1` (16,777,217 bytes)

**Validation**:
- [ ] Returns negative: `{0x7F, 0x34, 0x70}` (uploadDownloadNotAccepted)

---

### Test D.3: RequestDownload — Size Zero

**Description**: Request a download with `memory_size = 0`.

**Procedure**:
1. Connect + activate routing
2. RequestDownload: addr=0x0000, size=0

**Validation**:
- [ ] Returns negative: `{0x7F, 0x34, 0x70}` (uploadDownloadNotAccepted, size == 0 check)

---

### Test D.4: RequestDownload — Truncated Message

**Description**: Send a RequestDownload with fewer bytes than the format byte specifies.

**Procedure**:
1. Connect + activate routing
2. Send UDS: `{0x34, 0x00, 0x44}` (format says 4+4=8 bytes follow, but only 0 bytes present)

**Validation**:
- [ ] Returns negative: `{0x7F, 0x34, 0x13}` (incorrectMessageLength)

---

### Test D.5: RequestDownload — Invalid Address/Size Format

**Description**: Send RequestDownload with invalid format byte values: (a) length = 0, (b) length > 4.

**Procedure**:
1. Connect + activate routing
2. **Case A**: Send UDS: `{0x34, 0x00, 0x40, ...}` (addr_len=0, size_len=4)
3. Validate NRC 0x31
4. **Case B**: Send UDS: `{0x34, 0x00, 0x54, ...}` (addr_len=4, size_len=5 — exceeds max 4)
5. Validate NRC 0x31

**Validation**:
- [ ] Case A returns negative: `{0x7F, 0x34, 0x31}` (requestOutOfRange — addr_len=0)
- [ ] Case B returns negative: `{0x7F, 0x34, 0x31}` (requestOutOfRange — size_len>4)

---

### Test D.6: TransferData — No Active Transfer

**Description**: Send TransferData without a preceding RequestDownload.

**Procedure**:
1. Connect + activate routing
2. Send UDS: `{0x36, 0x01, 0xAA, 0xBB}` (TransferData, BSC=1, 2 bytes payload)

**Validation**:
- [ ] Returns negative: `{0x7F, 0x36, 0x24}` (requestSequenceError)

---

### Test D.7: TransferData — Wrong Block Sequence Counter

**Description**: Start a transfer, send TransferData with an incorrect BSC (e.g., send BSC=5 when server expects BSC=1).

**Procedure**:
1. Connect + activate routing
2. RequestDownload: size=100
3. Send TransferData with BSC=5 (expected: BSC=1)

**Validation**:
- [ ] Returns negative: `{0x7F, 0x36, 0x73}` (wrongBlockSequenceCounter)

---

### Test D.8: TransferData — Data Exceeds Requested Size

**Description**: Start a transfer for 10 bytes, then send 20 bytes of transfer data.

**Procedure**:
1. Connect + activate routing
2. RequestDownload: size=10
3. TransferData BSC=1: send 20 bytes of payload

**Validation**:
- [ ] Returns negative: `{0x7F, 0x36, 0x71}` (transferDataSuspended)
- [ ] Transfer state is NOT corrupted (BSC not incremented — retry with same BSC and correct data works)

---

### Test D.9: RequestTransferExit — No Active Transfer

**Description**: Send RequestTransferExit without a preceding RequestDownload.

**Procedure**:
1. Connect + activate routing
2. Send UDS: `{0x37}` (RequestTransferExit)

**Validation**:
- [ ] Returns negative: `{0x7F, 0x37, 0x24}` (requestSequenceError)

---

### Test D.10: RequestTransferExit — CRC Mismatch

**Description**: Transfer data with an intentionally wrong CRC suffix, then call RequestTransferExit.

**Procedure**:
1. Connect + activate routing
2. Generate 100-byte blob with CORRECT CRC → 104 bytes
3. Corrupt the CRC: flip a bit in the last 4 bytes
4. RequestDownload: size=104
5. TransferData: send all 104 bytes (with corrupt CRC)
6. RequestTransferExit

**Validation**:
- [ ] RequestTransferExit returns negative: `{0x7F, 0x37, 0x72}` (generalProgrammingFailure)
- [ ] No blob file is written to disk (blob discarded)
- [ ] Server logs "CRC-32 MISMATCH" (check verbose output)

---

### Test D.11: Transfer Timeout

**Description**: Start a transfer, send one block, then wait longer than `transfer_timeout_sec` (30 seconds) without sending more data. The server should abort the transfer.

**Procedure**:
1. Connect + activate routing
2. RequestDownload: size=10000 (larger than one block)
3. TransferData BSC=1: send first block (partial transfer)
4. Sleep for `transfer_timeout_sec + 5` seconds (35 seconds — accounts for server's 1-second polling granularity)
5. TransferData BSC=2: send second block

**Validation**:
- [ ] Second TransferData returns negative: `{0x7F, 0x36, 0x24}` (requestSequenceError — transfer was cleaned up)
- [ ] No blob file on disk (transfer was incomplete and aborted)

**Note**: This test takes ~35 seconds. It is the last test in Suite D to minimize impact on iteration speed.

---

### Test D.12: Unsupported UDS Services Across All Common SIDs

**Description**: Send each common UDS SID that the server does NOT implement and verify `serviceNotSupported` (NRC 0x11) for each.

**Procedure**:
For each SID in `{0x10, 0x27, 0x35}` (DiagnosticSessionControl, SecurityAccess, RequestUpload):
1. Connect + activate routing (reuse single connection)
2. Send UDS: `{SID, 0x01}`
3. Validate negative response

**Validation** (per SID):
- [ ] Returns `{0x7F, SID, 0x11}` (serviceNotSupported)

**Pass Criteria**: All 3 unsupported SIDs return NRC 0x11. (The server uses a single `default:` handler for all unrecognized SIDs; 3 representative samples provide the same coverage as testing all 11.)

---

### Test D.13: TransferData Minimal Message (Too Short)

**Description**: Send a TransferData message with only the SID byte (no BSC). The server must reject it as too short (minimum valid TransferData is 2 bytes: SID + BSC).

**Procedure**:
1. Connect + activate routing
2. RequestDownload: address=0x00001000, size=100 → success
3. Send UDS: `{0x36}` (1 byte — missing block sequence counter)

**Validation**:
- [ ] Returns `{0x7F, 0x36, 0x13}` (incorrectMessageLengthOrInvalidFormat)

---

### Test D.14: Blob Too Small for CRC (< 4 Bytes Received)

**Description**: Complete a transfer where total received data is less than 4 bytes. The server cannot extract a CRC-32 from fewer than 4 bytes. Verify the server handles this gracefully.

**Procedure**:
1. Connect + activate routing
2. RequestDownload: address=0x00001000, size=2, format=0x44
3. TransferData: BSC=1, data=`{0xAA, 0xBB}` (2 bytes — less than 4-byte CRC)
4. RequestTransferExit

**Validation**:
- [ ] RequestTransferExit returns either:
  - Positive response `{0x77}` (server saves blob as-is, no CRC check for < 4 bytes), OR
  - Negative response `{0x7F, 0x37, 0x72}` (generalProgrammingFailure — CRC cannot be validated)
- [ ] Behavior matches actual server code path (main.c lines 339-351: blob < 4 bytes is saved as-is)

**Note**: This test documents actual server behavior. The server saves blobs < 4 bytes without CRC verification since there aren't enough bytes to contain a CRC-32 suffix.

---

## 7. Test Suite E: Concurrent Access & Stress

**Purpose**: Validate server behavior under concurrent client access and resource contention.

### Test E.1: Concurrent Transfer Rejection

**Description**: Client A starts a blob transfer. Client B attempts to start a second transfer simultaneously.

**Procedure**:
1. Client A: connect + activate + RequestDownload (size=10000)
2. Client B: connect + activate + RequestDownload (size=5000)

**Validation**:
- [ ] Client A's RequestDownload succeeds
- [ ] Client B's RequestDownload returns NRC 0x70 (uploadDownloadNotAccepted)
- [ ] Client A can complete its transfer normally

**Cleanup**: Complete Client A's transfer, disconnect both

---

### Test E.2: Transfer Continues After Client B Disconnect

**Description**: Client A is mid-transfer. Client B connects, does some diagnostics, then disconnects. Client A's transfer should be unaffected.

**Procedure**:
1. Client A: connect + activate + RequestDownload (size=500)
2. Client A: TransferData BSC=1 (partial, first block)
3. Client B: connect + activate + TesterPresent → should succeed
4. Client B: disconnect
5. Client A: TransferData BSC=2 (remaining data)
6. Client A: RequestTransferExit

**Validation**:
- [ ] Client A's transfer completes successfully despite Client B's connect/disconnect
- [ ] Blob verified on disk

---

### Test E.3: Multiple Clients — Independent TesterPresent

**Description**: Open 3 simultaneous connections and send TesterPresent from each.

**Procedure**:
1. Connect clients A, B, C — activate routing on each
2. TesterPresent from A → expect success
3. TesterPresent from B → expect success
4. TesterPresent from C → expect success

**Validation**:
- [ ] All 3 TesterPresent calls succeed
- [ ] No interference between clients

---

### Test E.4: Client Disconnect During Active Transfer

**Description**: Start a transfer, send partial data, then close the TCP connection without completing the transfer. The server should clean up the transfer state so a new transfer can start.

**Procedure**:
1. Client A: connect + activate + RequestDownload (size=10000) + TransferData (partial)
2. Client A: close TCP connection (without RequestTransferExit)
3. Client B: connect + activate routing
4. Retry loop (up to `transfer_timeout_sec + 5` seconds, i.e., 35 seconds with default config):
   - Send RequestDownload (size=500) from Client B
   - If positive response → break (success)
   - If NRC 0x70 (uploadDownloadNotAccepted) → sleep 1 second, retry
   - If other error → fail immediately

**Validation**:
- [ ] Client B's RequestDownload eventually succeeds (transfer state cleaned up after timeout)
- [ ] No stale blob file from Client A's incomplete transfer
- [ ] Total wait time < `transfer_timeout_sec + 5` seconds

**Note**: The server does NOT clean up transfer state on client disconnect. The `client_handler_thread` closes the fd and marks the connection inactive, but `g_transfer` remains `active = true` until the main-loop timeout fires (`transfer_timeout_sec`, default 30s). Client B must retry until the timeout clears the stale transfer. This is expected server behavior, not a bug.

---

## 8. Test Suite F: Configuration & CLI Validation

**Purpose**: Validate that configuration file loading and CLI overrides work correctly.

**Implementation**: Shell script (`test/test_config.sh`, ~60 lines). Process lifecycle testing is naturally suited to shell — each test starts/stops the server with different arguments and checks exit codes and output. The shell script uses the same `TEST_PASS`/`TEST_FAIL` output format as the C test tool for consistent reporting.

### Test F.1: Server Starts with Explicit Config File

**Description**: Start the server with `-c doip-server.conf` and verify it loads the correct identity.

**Procedure**:
```bash
./doip-server -c doip-server.conf &
SERVER_PID=$!
sleep 0.5
# Send UDP vehicle ID request using doip_build_vehicle_id_request pattern (or nc/socat)
# Validate VIN matches config file
kill $SERVER_PID; wait $SERVER_PID 2>/dev/null
```

**Validation**:
- [ ] Server starts without errors (exit code 0 while running)
- [ ] VIN in announcement matches config file

---

### Test F.2: Server Starts Without Config File (Defaults)

**Description**: Start the server with no config file and verify default identity values.

**Procedure**:
```bash
cd /tmp && $DOIP_SERVER_BIN &
SERVER_PID=$!
sleep 0.5
# Send UDP vehicle ID request to default port 13400
kill $SERVER_PID; wait $SERVER_PID 2>/dev/null
```

**Validation**:
- [ ] Server starts (stderr contains "No config file found, using defaults")
- [ ] VIN = `FC1BLOBSRV0000001` (default)
- [ ] Logical address = 0x0001 (default)

---

### Test F.3: CLI Port Override

**Description**: Start the server with a non-standard port via CLI argument.

**Procedure**:
```bash
./doip-server -c doip-server.conf 127.0.0.1 13401 &
SERVER_PID=$!
sleep 0.5
# Verify nothing responds on port 13400 (timeout)
# Send UDP request to port 13401 → expect valid announcement
kill $SERVER_PID; wait $SERVER_PID 2>/dev/null
```

**Validation**:
- [ ] No response on port 13400
- [ ] Valid announcement on port 13401

---

### Test F.4: Config File — Invalid Port Rejected

**Description**: Verify the server rejects an invalid CLI port argument.

**Procedure**:
```bash
./doip-server 127.0.0.1 abc 2>stderr.tmp
EXIT_CODE=$?
```

**Validation**:
- [ ] Exit code = 1
- [ ] stderr.tmp contains "Error: invalid port 'abc'"

---

### Test F.5: Config File — Missing Explicit -c File

**Description**: Verify the server exits with an error when `-c` points to a non-existent file.

**Procedure**:
```bash
./doip-server -c /nonexistent/path.conf 2>stderr.tmp
EXIT_CODE=$?
```

**Validation**:
- [ ] Exit code = 1
- [ ] stderr.tmp contains "Error: cannot open config file"

---

### Test F.6: Config File — blob_storage_dir Path Traversal Warning

**Description**: Verify the server handles a `blob_storage_dir` config value containing `..` path traversal sequences. The config parser (`config.c:233-234`) prints a warning and preserves the default blob_storage_dir.

**Procedure**:
```bash
cat > /tmp/doip_test_traversal.conf << 'CONF'
blob_storage_dir = /tmp/../etc/doip_blobs
CONF
./doip-server -c /tmp/doip_test_traversal.conf &
SERVER_PID=$!
sleep 0.5
# Check server started successfully
kill $SERVER_PID; wait $SERVER_PID 2>/dev/null
rm -f /tmp/doip_test_traversal.conf
```

**Validation**:
- [ ] Server starts successfully (does not crash or exit with error)
- [ ] stderr contains warning about invalid blob_storage_dir path
- [ ] Server uses default blob_storage_dir (path traversal value is ignored)

---

### Test F.7: Config File — transfer_timeout=0 Uses Default

**Description**: Verify the server handles `transfer_timeout=0` gracefully. The config parser (`config.c:248-249`) prints a warning and keeps the default value (30 seconds) when transfer_timeout is 0.

**Procedure**:
```bash
cat > /tmp/doip_test_timeout.conf << 'CONF'
transfer_timeout=0
CONF
./doip-server -c /tmp/doip_test_timeout.conf &
SERVER_PID=$!
sleep 0.5
# Capture server startup output (stderr)
kill $SERVER_PID; wait $SERVER_PID 2>/dev/null
rm -f /tmp/doip_test_timeout.conf
```

**Validation**:
- [ ] Server starts successfully (does not crash)
- [ ] stderr contains warning about invalid transfer_timeout value
- [ ] Server uses default timeout (30 seconds) — verify via entity status or startup log

---

## 9. Implementation Details

### 9.1 File Structure

```
test/test_server.c       ~550-650 lines (40 in-process tests: Suites A-E)
test/test_config.sh      ~60 lines (7 shell tests: Suite F)
```

### 9.2 Source Organization

```c
/* ========== Includes & Framework ========== */
/* Test macros (TEST_START, TEST_PASS, TEST_FAIL) */
/* CRC-32 implementation */
/* Helper functions: tcp_connect_and_activate(), send_uds_expect(),
   udp_send_recv(), udp_drain_recv(), tcp_raw_connect(),
   generate_test_blob(), crc32_compute(), verify_blob_on_disk(),
   clear_blob_storage() */

/* ========== Suite A: UDP Discovery (3 tests) ========== */
static void test_a1_udp_generic_discovery(...);
static void test_a2_udp_vin_filter(...);          /* positive + negative */
static void test_a3_udp_eid_filter(...);          /* positive + negative */

/* ========== Suite B: TCP Protocol (14 tests) ========== */
static void test_b1_tester_present(...);
static void test_b2_tester_present_suppress(...);
static void test_b3_entity_status(...);
static void test_b4_power_mode(...);
static void test_b5_unsupported_uds_service(...);
static void test_b6_diagnostic_without_routing(...);   /* raw TCP socket */
static void test_b7_diagnostic_unknown_target(...);    /* send_diagnostic + recv_message */
static void test_b8_header_nack_bad_version(...);      /* raw TCP socket */
static void test_b9_header_nack_unknown_type(...);     /* raw TCP socket */
static void test_b10_connection_limit(...);
static void test_b11_entity_status_socket_count(...);  /* polling loop */
static void test_b12_source_addr_mismatch(...);
static void test_b13_header_nack_too_large(...);       /* raw TCP socket */
static void test_b14_header_nack_invalid_payload_len(...); /* raw TCP socket */

/* ========== Suite C: Blob Write (5 tests) ========== */
static void test_c1_small_blob_single_block(...);
static void test_c2_multi_block_blob(...);
static void test_c3_large_blob_bsc_wrap(...);     /* 1,048,164 bytes, BSC wraps at 256+ blocks */
static void test_c4_back_to_back_transfers(...);
static void test_c5_minimum_size_blob(...);       /* 5 bytes + CRC */

/* ========== Suite D: Error Handling (14 tests) ========== */
static void test_d1_download_already_active(...);
static void test_d2_download_size_exceeds_max(...);
static void test_d3_download_size_zero(...);
static void test_d4_download_truncated_message(...);
static void test_d5_download_invalid_format(...); /* case A: nibble=0, case B: nibble>4 */
static void test_d6_transfer_no_active(...);
static void test_d7_transfer_wrong_bsc(...);
static void test_d8_transfer_exceeds_size(...);   /* NRC 0x71 transferDataSuspended */
static void test_d9_exit_no_active(...);
static void test_d10_exit_crc_mismatch(...);
static void test_d11_transfer_timeout(...);       /* ~35 seconds, skip via SKIP_TIMEOUT=1 */
static void test_d12_unsupported_sids(...);       /* 3 representative SIDs */
static void test_d13_transfer_data_too_short(...);
static void test_d14_blob_too_small_for_crc(...);

/* ========== Suite E: Concurrent (4 tests) ========== */
static void test_e1_concurrent_transfer_rejection(...);
static void test_e2_transfer_survives_other_client(...);
static void test_e3_multiple_tester_present(...);
static void test_e4_client_disconnect_mid_transfer(...); /* retry loop, ~35 seconds */

/* ========== Suite F: Config (7 tests — test_config.sh) ========== */
/* F.1-F.7 are shell tests, not in test_server.c */

/* ========== Main ========== */
int main(int argc, char *argv[]) { ... }
```

### 9.3 Key Implementation Notes

1. **Client library usage**: Tests C.1-C.5 use the high-level client API (`doip_client_uds_request_download`, `doip_client_uds_transfer_data`, `doip_client_uds_request_transfer_exit`). Tests D.1-D.14 may use `doip_client_send_uds()` for malformed messages. Tests B.6, B.8, B.12, B.13, B.14 use raw TCP sockets (`tcp_raw_connect()`) because the client library prevents sending certain invalid messages.

2. **Blob disk verification**: `verify_blob_on_disk()` uses a retry loop (up to 5 attempts, 100ms apart) to handle the race between server write and test read. The server completes `fclose()` before sending the UDS response, so retries are a safety margin, not a requirement.

3. **Connection reuse**: Tests within a suite reuse connections where possible (e.g., Suite D error tests all use one connection). Each suite starts with a fresh connection. Raw socket tests (B.6, B.8, B.12, B.13, B.14) explicitly `close(raw_fd)` after each test to prevent fd leaks.

4. **Transfer timeout test (D.11)**: Takes ~35 seconds. Skip via `SKIP_TIMEOUT=1` environment variable (`getenv("SKIP_TIMEOUT")` check, 3 lines of code). No suite selector needed.

5. **Config tests (Suite F)**: `test/test_config.sh` handles F.1-F.7. Shell is the natural language for process lifecycle testing (start/stop server, check exit codes, capture stderr). Invoked via `make test-config` target.

6. **UDP socket hygiene**: All negative UDP tests (A.2 negative half, A.3 negative half) call `udp_drain_recv()` before sending to clear any stale responses from previous tests.

7. **Transfer state cleanup (E.4)**: Client disconnect does NOT reset server transfer state. E.4 uses a retry loop (up to 35 seconds) waiting for the server's main-loop timeout to clear the stale transfer. This is the longest-running test after D.11.

8. **Known server transport limit**: The server advertises `max_block_length = 4096` (UDS level) in the RequestDownload positive response. However, the DoIP transport layer (`doip_server.c:150`) rejects payloads exceeding 4096 bytes. A full-size TransferData block of 4096 UDS bytes wraps to 4100 DoIP payload bytes (2-byte SA + 2-byte TA + 4096 UDS). This means the effective maximum UDS data per block is 4092 bytes, not 4096. Suite C tests should use blocks that stay within the 4092-byte UDS limit, or the server's `max_block_length` response should be treated as 4092. This is a server implementation detail to be aware of during test implementation.

### 9.4 Error Reporting

Each failing test prints:
- Test number and name
- Expected vs actual values (hex for protocol fields)
- File:line where the failure occurred (via `__LINE__` in macro)

Example output:
```
--- Test 15: Small blob transfer (single block) ---
  RequestDownload: addr=0x00001000 size=104 → accepted (max_block=4096)
  TransferData: block 1, 104 bytes → accepted
  RequestTransferExit → accepted
  Blob on disk: /tmp/doip_blobs/2026-03-05_143022_addr_00001000_100bytes.bin (100 bytes)
  PASS: Small blob transfer (single block)
```

---

## 10. Build & Run

### 10.1 Build Commands

```bash
cd ~/iMatrix/DOIP_Server
make all              # Build server + both test tools
make test             # Quick: existing 6-test smoke suite
make test-full        # Comprehensive: new test_server suite (~2 min with timeout test)
make test-full SKIP_TIMEOUT=1  # Skip 30-second timeout test for fast iteration
```

### 10.2 Quick Iteration (Manual)

```bash
# Terminal 1: start server
./doip-server -c doip-server.conf

# Terminal 2: run tests
./test-server -c doip-server.conf         # Run all in-process tests (Suites A-E)
./test-server -c doip-server.conf -v      # Run with verbose hex dumps
```

### 10.3 Expected Results

**In-process tests (test_server.c — Suites A-E):**
```
========================================
 DoIP Server Comprehensive Test Suite
========================================
Server: 127.0.0.1:13400

[Suite A: UDP Discovery — 3 tests]
--- Test 1: Generic vehicle ID request (broadcast) ---
  PASS: Generic discovery
...

[Suite B: TCP Protocol — 14 tests]
...

[Suite C: Blob Write — 5 tests]
...

[Suite D: Error Handling — 14 tests]
...

[Suite E: Concurrent Access — 4 tests]
...

========================================
=== Results: 40 passed, 0 failed ===
========================================
```

### 10.4 CI Integration

```makefile
.PHONY: ci-test
ci-test: doip-server test-discovery test-server
	@echo "=== Smoke Tests ==="
	@$(MAKE) test
	@echo ""
	@echo "=== Full Server Tests ==="
	@$(MAKE) test-full SKIP_TIMEOUT=1
```

---

## 11. Appendices

### Appendix A: UDS Negative Response Code Reference

| NRC | Hex | Meaning | Triggered By |
|-----|-----|---------|-------------|
| incorrectMessageLengthOrInvalidFormat | 0x13 | Request too short | RequestDownload with truncated fields |
| requestSequenceError | 0x24 | Wrong state for service | TransferData/Exit without active transfer |
| requestOutOfRange | 0x31 | Invalid parameter value | RequestDownload with addr/size len = 0 or > 4 |
| uploadDownloadNotAccepted | 0x70 | Cannot start transfer | Active transfer exists, size exceeds max, malloc fail |
| transferDataSuspended | 0x71 | Data exceeds size | TransferData total > requested memory_size |
| generalProgrammingFailure | 0x72 | CRC mismatch | RequestTransferExit with bad CRC |
| wrongBlockSequenceCounter | 0x73 | BSC out of order | TransferData with unexpected sequence number |
| serviceNotSupported | 0x11 | Unknown SID | Any SID not in {0x34, 0x36, 0x37, 0x3E} |

### Appendix B: DoIP Payload Type Reference

| Type | Hex | Direction | Description |
|------|-----|-----------|-------------|
| Header NACK | 0x0000 | Server→Client | Protocol error |
| Vehicle ID Request | 0x0001 | Client→Server (UDP) | Generic discovery |
| Vehicle ID Request by EID | 0x0002 | Client→Server (UDP) | EID-filtered discovery |
| Vehicle ID Request by VIN | 0x0003 | Client→Server (UDP) | VIN-filtered discovery |
| Vehicle Announcement | 0x0004 | Server→Client (UDP) | Discovery response |
| Routing Activation Request | 0x0005 | Client→Server (TCP) | Activate diagnostic session |
| Routing Activation Response | 0x0006 | Server→Client (TCP) | Activation result |
| Alive Check Request | 0x0007 | Server→Client (TCP) | Keepalive probe |
| Alive Check Response | 0x0008 | Client→Server (TCP) | Keepalive reply |
| Entity Status Request | 0x4001 | Client→Server (TCP) | Query server capacity |
| Entity Status Response | 0x4002 | Server→Client (TCP) | Capacity info |
| Power Mode Request | 0x4003 | Client→Server (TCP) | Query power state |
| Power Mode Response | 0x4004 | Server→Client (TCP) | Power state info |
| Diagnostic Message | 0x8001 | Bidirectional (TCP) | UDS payload carrier |
| Diagnostic ACK | 0x8002 | Server→Client (TCP) | Message accepted |
| Diagnostic NACK | 0x8003 | Server→Client (TCP) | Message rejected |

### Appendix C: Blob Transfer State Machine

```
                    ┌──────────┐
                    │   IDLE   │
                    └────┬─────┘
                         │ RequestDownload (0x34)
                         │ ✓ size > 0 && size <= max
                         │ ✓ no active transfer
                         │ ✓ malloc succeeds
                         ▼
                    ┌──────────┐
              ┌────►│  ACTIVE  │◄───────┐
              │     └────┬─────┘        │
              │          │              │
              │  TransferData (0x36)    │ TransferData (0x36)
              │  BSC matches, data     │ more blocks...
              │  fits in buffer        │
              │          │              │
              │          ▼              │
              │     ┌──────────┐       │
              │     │RECEIVING │───────┘
              │     └────┬─────┘
              │          │
              │          │ RequestTransferExit (0x37)
              │          ▼
              │     ┌──────────┐
              │     │ VERIFY   │
              │     │  CRC-32  │
              │     └────┬─────┘
              │        ╱   ╲
              │   match     mismatch
              │      │         │
              │      ▼         ▼
              │  ┌───────┐ ┌───────┐
              │  │ SAVE  │ │DISCARD│
              │  │ BLOB  │ │ NRC   │
              │  └───┬───┘ │ 0x72  │
              │      │     └───┬───┘
              │      ▼         │
              └──── IDLE ◄─────┘

  Timeout (30s inactivity) ──► IDLE (cleanup, no blob saved)
  Server shutdown           ──► IDLE (cleanup)
  Client disconnect         ──► transfer stays ACTIVE until timeout
```

### Appendix D: Test Count Summary

| Suite | Tests | Estimated Time | Implementation |
|-------|-------|----------------|----------------|
| A: UDP Discovery | 3 | ~5 seconds | test_server.c |
| B: TCP Protocol | 14 | ~12 seconds | test_server.c |
| C: Blob Write | 5 | ~20 seconds | test_server.c |
| D: Error Handling | 14 | ~45 seconds (D.11 = 35s) | test_server.c |
| E: Concurrent | 4 | ~40 seconds (E.4 = 35s) | test_server.c |
| F: Config/CLI | 7 | ~5 seconds | test_config.sh |
| **Total** | **47** | **~125 seconds** | C + shell |

**Note**: E.4 takes up to 35 seconds due to the transfer timeout retry loop. D.11 takes ~35 seconds. All other tests complete in <2 seconds each. The `timeout 180` in the Makefile provides adequate margin.

### Appendix E: File Deliverables

| File | Description |
|------|-------------|
| `test/test_server.c` | C test tool source (~550-650 lines, 40 tests: Suites A-E) |
| `test/test_config.sh` | Shell test script (~60 lines, 7 tests: Suite F) |
| `docs/server_test1.md` | This plan document |
| `Makefile` | Updated with `test-server`, `test-config`, `test-full`, `ci-test` targets |
