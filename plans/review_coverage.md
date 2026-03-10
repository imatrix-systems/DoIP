# Test Coverage Review — server_test1.md

**Reviewer**: Test Coverage
**Date**: 2026-03-05
**Status**: ROUND 3

## Summary

28 PASS, 1 FAIL, 6 INFO

Round 2 identified 3 FAIL items. The v2.1 test plan fixes 2 of them fully (CV-30R2, CV-35). One remains a FAIL with a different root cause than Round 2 identified (CV-29R2 expected behavior is wrong).

## Round 2 Fix Verification

| Round 2 ID | Issue | v2.1 Fix | Status |
|-------------|-------|----------|--------|
| CV-29R2 | F.6 tests wrong code path (-c arg instead of blob_storage_dir) | F.6 now tests blob_storage_dir value with ".." | FAIL (expected exit code wrong — see CV-29R3) |
| CV-30R2 | F.7 validation too weak (accepts either exit=1 or starts) | F.7 now specifies exact behavior: starts, warns, uses default 30s | PASS |
| CV-35 | validate_header distinct path not tested | Accepted as INFO (low priority, distinct but same NACK) | PASS (accepted as INFO) |

## Findings

---

### [PASS] CV-01: UDP Generic Vehicle ID Request

**Location**: `doip_server.c:474` (DOIP_TYPE_VEHICLE_ID_REQUEST case), Test A.1
**Detail**: The generic vehicle ID request handler is exercised by A.1, with thorough field-by-field validation of the announcement response. All identity fields (VIN, EID, GID, logical address, further_action, sync_status) are checked.

---

### [PASS] CV-02: UDP VIN-Filtered Request (Match and No-Match)

**Location**: `doip_server.c:478-482` (VIN comparison), Tests A.2
**Detail**: Both the positive VIN match path and the negative VIN mismatch path (silent drop via `break`) are tested within A.2.

---

### [PASS] CV-03: UDP EID-Filtered Request (Match and No-Match)

**Location**: `doip_server.c:485-489` (EID comparison), Tests A.3
**Detail**: Both EID match and mismatch paths tested within A.3.

---

### [PASS] CV-04: TCP Routing Activation — Success Path

**Location**: `doip_server.c:181-213` (ROUTING_ACTIVATION_REQUEST case), Test B.1 (via `tcp_connect_and_activate()` used by many tests)
**Detail**: The happy-path routing activation is tested with validation of response code, entity address, and tester address.

---

### [PASS] CV-05: Routing Activation Callback Invocation

**Location**: `doip_server.c:187-189` (on_routing_activation callback)
**Detail**: The server's `handle_routing_activation` callback (main.c:418-426) always returns `DOIP_ROUTING_ACTIVATION_SUCCESS`. Every test that calls `tcp_connect_and_activate()` exercises this path. There is no test for a callback returning a denial code, but since the server's callback is hardcoded to always accept, this is not a realistic test gap.

---

### [PASS] CV-06: Diagnostic Message — Full Path (Routed, Known Target)

**Location**: `doip_server.c:216-271` (DIAGNOSTIC_MESSAGE case), Tests B.1, B.5, C.1-C.5
**Detail**: The normal diagnostic message path (routing activated, source matches, target known, positive ACK sent, callback invoked) is exercised by every test that sends UDS after routing activation.

---

### [PASS] CV-07: Diagnostic Message — Routing Not Active

**Location**: `doip_server.c:220-222` (routing_activated check), Test B.6
**Detail**: B.6 sends a diagnostic message without routing activation using a raw TCP socket and validates that no response is sent. Also validates the connection stays alive afterward.

---

### [PASS] CV-08: Diagnostic Message — Unknown Target Address

**Location**: `doip_server.c:236-241` (DOIP_DIAG_NACK_UNKNOWN_TA), Test B.7
**Detail**: B.7 sends to target 0xFFFF and validates the NACK response with code 0x03. Correctly uses lower-level `send_diagnostic` + `recv_message` to capture the NACK code byte.

---

### [PASS] CV-09: Diagnostic Message — Source Address Mismatch (FIXED in v2.0)

**Location**: `doip_server.c:226-232` (DOIP_DIAG_NACK_INVALID_SA), Test B.12
**Detail**: B.12 activates routing (tester address = 0x0E80), then sends a raw DoIP diagnostic message with `source_address = 0x1234` and validates `DOIP_DIAG_NACK_INVALID_SA` (0x02). This correctly exercises the mismatch path.

---

### [PASS] CV-10: Header NACK — Invalid Header

**Location**: `doip_server.c:131-137` (doip_deserialize_header failure), Test B.8
**Detail**: B.8 sends a header with wrong version/inverse bytes. The server sends a NACK with `DOIP_HEADER_NACK_INCORRECT_PATTERN` (0x00). Also validates the connection is closed afterward.

---

### [PASS] CV-11: Header NACK — Unknown Payload Type

**Location**: `doip_server.c:308-313` (default case in message switch), Test B.9
**Detail**: B.9 sends payload_type 0x9999 and validates the `DOIP_HEADER_NACK_UNKNOWN_PAYLOAD_TYPE` (0x01) NACK.

---

### [PASS] CV-12: Header NACK — Message Too Large (FIXED in v2.0)

**Location**: `doip_server.c:150-156` (payload_length > DOIP_MAX_DIAGNOSTIC_SIZE), Test B.13
**Detail**: B.13 sends a valid DoIP header with `payload_length = 5000`, validates NACK code 0x02 (`DOIP_HEADER_NACK_MESSAGE_TOO_LARGE`), and verifies the connection stays alive.

---

### [PASS] CV-13: Header NACK — Invalid Payload Length (FIXED in v2.0)

**Location**: `doip_server.c:167-172` (doip_parse_message failure), Test B.14
**Detail**: B.14 sends a routing activation request (type 0x0005) with only 2 bytes of payload (expects 7+). Validates NACK code 0x04 (`DOIP_HEADER_NACK_INVALID_PAYLOAD_LENGTH`) and connection stays alive.

---

### [PASS] CV-14: Entity Status Request

**Location**: `doip_server.c:279-296` (ENTITY_STATUS_REQUEST case), Tests B.3, B.11
**Detail**: B.3 validates all entity status fields. B.11 validates the socket count changes dynamically.

---

### [PASS] CV-15: Power Mode Request

**Location**: `doip_server.c:299-305` (DIAG_POWER_MODE_REQUEST case), Test B.4
**Detail**: B.4 validates the power mode response (0x01 = ready).

---

### [PASS] CV-16: Alive Check Response Handling

**Location**: `doip_server.c:274-276` (ALIVE_CHECK_RESPONSE case)
**Detail**: The server merely logs the alive check response. This is a trivial printf-only path with no state changes. Not testing it is reasonable.

---

### [PASS] CV-17: UDS Handler — RequestDownload (All Paths)

**Location**: `main.c:180-249` (handle_request_download)
**Detail**: Tests cover all code paths:
- Happy path: C.1-C.5 (positive response 0x74 with max_block_length)
- Transfer already active: D.1 (NRC 0x70)
- Size exceeds max: D.2 (NRC 0x70)
- Size zero: D.3 (NRC 0x70)
- Truncated message (uds_len < 4): D.4 (NRC 0x13)
- Invalid addr/size format (len=0): D.5 Case A (NRC 0x31)
- Invalid addr/size format (len>4): D.5 Case B (NRC 0x31)

---

### [PASS] CV-18: RequestDownload — Address/Size Length > 4 (FIXED in v2.0)

**Location**: `main.c:198` (`mem_addr_len > 4 || mem_size_len > 4`)
**Detail**: D.5 Case B sends format byte `0x54` (addr_len=4, size_len=5) and validates NRC 0x31.

---

### [PASS] CV-19: UDS Handler — TransferData (All Paths)

**Location**: `main.c:252-295` (handle_transfer_data)
**Detail**: All major paths covered:
- No active transfer: D.6 (NRC 0x24)
- Wrong BSC: D.7 (NRC 0x73)
- Data exceeds size: D.8 (NRC 0x71)
- Minimal message (uds_len < 2): D.13 (NRC 0x13)
- Happy path: C.1-C.5

---

### [PASS] CV-20: TransferData — Minimal Message (FIXED in v2.0)

**Location**: `main.c:260-261` (uds_len < 2 check), Test D.13
**Detail**: D.13 starts a transfer, then sends TransferData with only 1 byte `{0x36}`. Validates NRC 0x13. Correctly exercises the minimum length check after an active transfer exists.

---

### [PASS] CV-21: UDS Handler — RequestTransferExit (All Paths)

**Location**: `main.c:297-355` (handle_transfer_exit)
**Detail**: All paths covered:
- No active transfer: D.9 (NRC 0x24)
- CRC mismatch: D.10 (NRC 0x72)
- CRC match + save: C.1-C.5
- Blob < 4 bytes (no CRC): D.14

---

### [PASS] CV-22: RequestTransferExit — Blob Too Small for CRC (FIXED in v2.0)

**Location**: `main.c:339-351` (bytes_received < 4 path), Test D.14
**Detail**: D.14 transfers exactly 2 bytes, calls RequestTransferExit, and validates the positive response (0x77). The server saves blobs < 4 bytes as-is without CRC verification.

---

### [PASS] CV-23: UDS Handler — TesterPresent (Both Paths)

**Location**: `main.c:393-407`
**Detail**: Normal response (B.1), suppress bit (B.2), and unsupported SID default (B.5, D.12) are all covered.

---

### [PASS] CV-24: Transfer Timeout

**Location**: `main.c:591-598` (main loop timeout check), Test D.11
**Detail**: D.11 explicitly waits 35 seconds and validates the transfer was cleaned up. The validation checks that a subsequent TransferData returns requestSequenceError (0x24), confirming `transfer_cleanup_locked()` ran.

---

### [INFO] CV-25: Diagnostic Callback Returns 0 (No UDS Response)

**Location**: `doip_server.c:261-262` (uds_resp_len > 0 check)
**Detail**: TesterPresent with suppress bit (B.2) exercises this path — callback returns 0, so only the diagnostic ACK is sent. Implicitly covered.

---

### [INFO] CV-26: Server Graceful Shutdown (SIGINT/SIGTERM)

**Location**: `main.c:432-438`, `main.c:603-611`
**Detail**: The shutdown path is exercised implicitly when the Makefile `test-full` target kills the server. No explicit test validates cleanup behavior. Low risk.

---

### [INFO] CV-27: ensure_storage_dir() — Not-a-Directory Error

**Location**: `main.c:108-111` (S_ISDIR check)
**Detail**: If `blob_storage_dir` exists but is a regular file, the server logs an error and returns -1. Purely defensive; low risk.

---

### [INFO] CV-28: save_blob() — fopen/fwrite Failures

**Location**: `main.c:141-157`
**Detail**: I/O error paths that are difficult to trigger reliably in automated tests. Correctly excluded.

---

### [FAIL] CV-29R3: Config Parser — blob_storage_dir Path Traversal (Wrong Expected Behavior)

**Location**: `config.c:233-234` (strstr(value, "..") check), Test F.6
**Detail**: F.6 in v2.1 now correctly targets the `blob_storage_dir` config key (fixing the Round 2 finding that the test was targeting the `-c` arg). The test creates a config with `blob_storage_dir = /tmp/../etc/doip_blobs` and runs the server. However, the **expected exit code is wrong**.

The actual server behavior at config.c:233-234:
1. `strstr(value, "..")` detects the path traversal
2. `fprintf(stderr, ...)` prints a warning
3. The value is **skipped** — the default `/tmp/doip_blobs` is preserved
4. `doip_config_load()` returns 0 (success) — it does NOT return an error for this
5. The server continues initialization and **starts normally**

F.6 expects `Exit code = 1 (server does not start)`, but the server WILL start. The config parser treats this as a warning, not a fatal error. The path traversal is rejected (default is preserved), but `doip_config_load()` returns 0.

**Fix**: Change F.6 validation to:
1. Server starts successfully (exit code 0 while running, can accept connections)
2. stderr contains the path traversal warning: `"blob_storage_dir contains '..'"`
3. Server uses default blob_storage_dir `/tmp/doip_blobs` — verify via startup log output `"Blob storage: /tmp/doip_blobs/"` (main.c:580 prints `"Storage: %s/"`)

The test procedure should also be updated: start the server in the background, verify it's running, capture stdout/stderr, kill it, and check the output. The current procedure expects immediate failure which won't happen.

---

### [PASS] CV-30R3: Config Parser — transfer_timeout=0 Uses Default (FIXED in v2.1)

**Location**: `config.c:248-249` (explicit `uval == 0` check), Test F.7
**Detail**: F.7 now correctly specifies the exact expected behavior:
1. Server starts successfully (does not crash)
2. stderr contains warning about invalid transfer_timeout value
3. Server uses default timeout (30 seconds) — verified via startup log

This matches the actual code: config.c:248-249 prints `"transfer_timeout=0 rejected (minimum 1), using default"` and does not update `config->transfer_timeout_sec`, so the default 30 remains. The server continues and starts normally. main.c:579 prints `"Transfer timeout: 30 seconds"`, which the test can check.

The F.7 procedure starts the server, captures output, kills it, and validates all three criteria. This is precise and correct.

---

### [INFO] CV-35R3: Header NACK — doip_validate_header() Distinct Path (Accepted as INFO)

**Location**: `doip_server.c:140-146` (doip_validate_header failure)
**Detail**: After reviewing the implementation of `doip_validate_header()` (doip.c:77-91), the function checks two things:
1. Version/inverse match: `(protocol_version ^ 0xFF) != inverse_version`
2. Payload length: `payload_length > DOIP_MAX_PAYLOAD_SIZE` (65535)

`doip_deserialize_header()` (doip.c:62-75) does NOT check version/inverse — it only parses bytes. So the version/inverse check in `doip_validate_header()` IS a distinct, reachable code path. A header with mismatched version/inverse that somehow passes deserialization would be caught here. However, since `doip_deserialize_header()` always succeeds for any 8 bytes, B.8's bad header (wrong version/inverse) actually passes deserialization and is caught by `doip_validate_header()` at line 140 — NOT at line 132.

Wait — re-examining: B.8 sends "wrong version/inverse bytes." If version=0xFF and inverse=0xFF, then `doip_deserialize_header()` succeeds (it doesn't validate), and `doip_validate_header()` fails because `0xFF ^ 0xFF = 0x00 != 0xFF`. So B.8 **already exercises** the `doip_validate_header()` path at line 140, not the deserialization failure at line 132.

For `doip_deserialize_header()` to actually fail (line 132), the buffer would need to be too small or NULL — neither of which can happen in the server since it always reads exactly `DOIP_HEADER_SIZE` bytes before calling the function (line 121). So line 132 is actually the unreachable path, not line 140.

This means B.8 already covers the validate_header path. The deserialization failure path (line 132) is the one that's unreachable in the server context (defensive code). Reclassifying.

**Status**: INFO — B.8 already covers `doip_validate_header()` at line 140. The `doip_deserialize_header()` failure at line 132 is unreachable defensive code (server always provides exactly DOIP_HEADER_SIZE bytes).

---

### [INFO] CV-36: BSC Wrap Arithmetic in TransferData

**Location**: `main.c:282` (`g_transfer.block_sequence++`)
**Detail**: C.3 explicitly tests BSC wrap from 0xFF to 0x00. Thorough — the implicit `uint8_t` overflow behavior is exercised and validated.

---

### [PASS] CV-33: Transfer State — Back-to-Back and Cleanup

**Location**: `main.c:164-174` (transfer_cleanup_locked)
**Detail**: C.4 validates back-to-back transfers. E.4 validates cleanup after client disconnect (with retry loop for timeout).

---

### [PASS] CV-34: Section 1.3 Accuracy — "What Is NOT Tested"

**Location**: Test plan Section 1.3
**Detail**: The exclusions listed are accurate:
- RequestUpload (0x35): not implemented, correctly excluded
- Security access: server has no security, correctly excluded
- TLS: not implemented, correctly excluded
- Multi-hop routing: single entity, correctly excluded

---

## Additional Observations

### D.11 Enhancement Suggestion (Informational, Unchanged from R2)

D.11 confirms the old transfer is aborted (TransferData returns 0x24) but does not verify a new RequestDownload succeeds afterward. Low risk since `transfer_cleanup_locked()` is straightforward.

### D.8 NRC Code Naming (Informational, Unchanged from R2)

D.8 validates NRC 0x71, which ISO 14229 defines as `transferDataSuspended`. The server code comments say "requestOutOfRange." Minor naming inconsistency in server comments only — not a test gap.

### CV-35 Reclassification Note

In Round 2, CV-35 was classified as FAIL because it was believed that B.8 exercises `doip_deserialize_header()` failure (line 132) rather than `doip_validate_header()` failure (line 140). After reviewing the actual implementations: `doip_deserialize_header()` never validates content — it just reads bytes and always returns DOIP_OK for valid-sized input. The version/inverse check happens only in `doip_validate_header()`. Therefore B.8 already exercises the validate_header path (line 140), and the deserialize failure (line 132) is unreachable defensive code.

---

## Summary Table

| ID | Status | Title |
|----|--------|-------|
| CV-01 | PASS | UDP Generic Vehicle ID Request |
| CV-02 | PASS | UDP VIN-Filtered Request |
| CV-03 | PASS | UDP EID-Filtered Request |
| CV-04 | PASS | TCP Routing Activation Success |
| CV-05 | PASS | Routing Activation Callback |
| CV-06 | PASS | Diagnostic Message Full Path |
| CV-07 | PASS | Diagnostic Without Routing |
| CV-08 | PASS | Diagnostic Unknown Target |
| CV-09 | PASS | Diagnostic Source Address Mismatch (FIXED) |
| CV-10 | PASS | Header NACK Invalid Header |
| CV-11 | PASS | Header NACK Unknown Payload Type |
| CV-12 | PASS | Header NACK Message Too Large (FIXED) |
| CV-13 | PASS | Header NACK Invalid Payload Length (FIXED) |
| CV-14 | PASS | Entity Status Request |
| CV-15 | PASS | Power Mode Request |
| CV-16 | PASS | Alive Check Response Handling |
| CV-17 | PASS | RequestDownload All Paths |
| CV-18 | PASS | RequestDownload Addr/Size Length > 4 (FIXED) |
| CV-19 | PASS | TransferData All Paths |
| CV-20 | PASS | TransferData Minimal Message (FIXED) |
| CV-21 | PASS | RequestTransferExit All Paths |
| CV-22 | PASS | TransferExit Blob Too Small for CRC (FIXED) |
| CV-23 | PASS | TesterPresent Both Paths |
| CV-24 | PASS | Transfer Timeout |
| CV-25 | INFO | Diagnostic Callback Returns 0 |
| CV-26 | INFO | Server Graceful Shutdown |
| CV-27 | INFO | ensure_storage_dir Not-a-Dir |
| CV-28 | INFO | save_blob I/O Failures |
| CV-29R3 | FAIL | Config blob_storage_dir Path Traversal — Wrong Expected Exit Code |
| CV-30R3 | PASS | Config transfer_timeout=0 Uses Default (FIXED) |
| CV-35R3 | INFO | Header NACK validate_header — B.8 Already Covers It |
| CV-36 | INFO | BSC Wrap Arithmetic |
| CV-33 | PASS | Transfer State Cleanup |
| CV-34 | PASS | Section 1.3 Accuracy |
