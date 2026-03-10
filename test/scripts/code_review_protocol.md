# Protocol Correctness Review — doip_test.py

**Reviewer:** Protocol Correctness Agent
**Date:** 2025-03-09
**File:** `test/scripts/doip_test.py`
**References:** `include/doip.h`, `src/doip.c`, `src/doip_server.c`, `test/scripts/plan_doip_test_script.md`

---

## 1. DoIP Header Build: version, inverse, type endianness, length

**PASS**

```python
def build_doip(payload_type, payload=b"", version=DOIP_VERSION_DISCOVERY):
    inv = (~version) & 0xFF
    header = struct.pack("!BBHI", version, inv, payload_type, len(payload))
    return header + payload
```

- `!` = network byte order (big-endian) -- correct per ISO 13400-2.
- `B` (version, 1 byte) + `B` (inverse, 1 byte) + `H` (payload type, 2 bytes BE) + `I` (length, 4 bytes BE) = 8 bytes total -- matches `DOIP_HEADER_SIZE`.
- Inverse computed as `(~version) & 0xFF` -- equivalent to the server's `DOIP_PROTOCOL_VERSION ^ 0xFF`.
- Payload length is `len(payload)`, the actual payload byte count -- correct.

No issues.

---

## 2. Vehicle Announcement Parsing: field offsets correct

**PASS**

```python
def parse_announcement(payload):
    vin = payload[0:17]          # bytes 0-16, 17 chars
    logical_addr = payload[17:19]  # bytes 17-18, uint16 BE
    eid = payload[19:25]          # bytes 19-24, 6 bytes
    gid = payload[25:31]          # bytes 25-30, 6 bytes
    further_action = payload[31]  # byte 31
    sync_status = payload[32]     # byte 32
```

Cross-checked against `doip_build_vehicle_announcement()` in `doip.c` (line 389-414):
- VIN at offset 0, length 17 -- matches.
- Logical address at offset 17, `write_u16` (2 bytes BE) -- matches `struct.unpack("!H", ...)`.
- EID at offset 19, 6 bytes -- matches.
- GID at offset 25, 6 bytes -- matches.
- Further action at offset 31 -- matches.
- Sync status at offset 32 -- matches (server sends it when `has_sync_status=true`, which it always does).

Minimum payload check `len(payload) < 33` is correct.

---

## 3. Routing Activation Request/Response: field layout matches server

**PASS**

Request payload (line 238):
```python
payload = struct.pack("!HB4s", source_addr, 0x00, b"\x00" * 4)
```
This produces: SA(2 bytes BE) + activation_type(1 byte, 0x00) + reserved(4 bytes, all zeros) = 7 bytes.

Server parser `parse_routing_activation_request()` (doip.c line 263-282) expects:
- SA at offset 0, 2 bytes -- matches.
- activation_type at offset 2, 1 byte -- matches.
- reserved at offset 3, 4 bytes -- matches.
- Total minimum 7 bytes -- matches.

Response parsing (line 245):
```python
_tester_addr, entity_addr, code = struct.unpack("!HHB", resp[:5])
```
Server builds via `doip_build_routing_activation_response()` (doip.c line 445-472):
- tester_logical_address at offset 0, 2 bytes BE -- matches.
- entity_logical_address at offset 2, 2 bytes BE -- matches.
- response_code at offset 4, 1 byte -- matches.

The server also appends 4 bytes reserved (total 9 bytes), but the script only unpacks the first 5 bytes, which is sufficient. The minimum check `len(resp) < 5` is correct.

---

## 4. Two-Phase Diagnostic Flow: ACK then response handled correctly

**PASS**

`send_diagnostic()` (line 277-298):
1. Sends diagnostic message (0x8001) with SA(2) + TA(2) + UDS data.
2. Phase 1: receives and expects ACK (0x8002). Also handles NACK (0x8003) gracefully.
3. Phase 2: receives diagnostic response (0x8001). Skips first 4 bytes (SA+TA) to extract UDS data.

Server behavior (doip_server.c line 245-268):
1. Server sends ACK (0x8002) first via `doip_build_diagnostic_ack()`.
2. Server then sends diagnostic response (0x8001) via `doip_build_diagnostic_message()`.

The ACK code extraction `ack_payload[4]` is correct -- the ACK payload is SA(2) + TA(2) + ack_code(1), so byte index 4 is the ack_code.

The response UDS data extraction `resp_payload[4:]` is correct -- diagnostic message payload is SA(2) + TA(2) + UDS data.

---

## 5. Entity Status Request/Response: correct type codes and payload parsing

**PASS**

Request uses `TYPE_ENTITY_STATUS_REQUEST = 0x4001` with no payload -- matches `DOIP_TYPE_ENTITY_STATUS_REQUEST` (0x4001) in doip.h. Server handles this case at doip_server.c line 279.

Response parsing (line 256-259):
```python
node_type = resp[0]
max_sockets = resp[1]
open_sockets = resp[2]
max_data_size = struct.unpack("!I", resp[3:7])[0] if len(resp) >= 7 else None
```

Server builds via `doip_build_entity_status_response()` (doip.c line 546-564):
- Byte 0: node_type -- matches.
- Byte 1: max_concurrent_sockets -- matches.
- Byte 2: currently_open_sockets -- matches.
- Bytes 3-6: max_data_size (4 bytes BE, optional) -- matches.

Response type check against `TYPE_ENTITY_STATUS_RESPONSE = 0x4002` -- matches `DOIP_TYPE_ENTITY_STATUS_RESPONSE`.

---

## 6. Power Mode Request/Response: correct type codes and payload

**PASS**

Request uses `TYPE_POWER_MODE_REQUEST = 0x4003` -- matches `DOIP_TYPE_DIAG_POWER_MODE_REQUEST` (0x4003).

Response parsing (line 272-275): reads `resp[0]` as the power mode byte.

Server (doip_server.c line 299-304) builds response with `DOIP_TYPE_DIAG_POWER_MODE_RESPONSE` (0x4004) and 1-byte payload `{ 0x01 }` (ready).

Response type check `TYPE_POWER_MODE_RESPONSE = 0x4004` -- matches.

Test asserts `mode == 0x01` -- matches the server's hardcoded "ready" value.

---

## 7. TesterPresent: sends [0x3E, 0x00] sub-function byte

**PASS**

```python
def tester_present(self):
    ack_code, uds = self.send_diagnostic(bytes([0x3E, 0x00]))
    ...
    return len(uds) >= 1 and uds[0] == 0x7E
```

- SID 0x3E with sub-function 0x00 -- correct per UDS/ISO 14229.
- Sub-function 0x00 means "no suppress positive response", so a positive response is expected.
- Positive response SID = request SID + 0x40 = 0x3E + 0x40 = 0x7E -- correctly checked.

---

## 8. Unsupported SID: negative response parsing (0x7F + SID + NRC)

**PASS**

```python
_ack, uds = conn.send_diagnostic(bytes([0x22, 0xF1, 0x90]))
assert uds[0] == 0x7F    # negative response indicator
assert uds[1] == 0x22    # echo of requested SID
assert uds[2] == 0x11    # NRC: serviceNotSupported
```

- 0x7F is the UDS negative response service ID -- correct.
- Byte 1 echoes the rejected SID (0x22 = ReadDataByIdentifier) -- correct.
- NRC 0x11 = serviceNotSupported -- correct per ISO 14229.
- The 3-byte minimum length check `len(uds) >= 3` is correct for a negative response.

---

## 9. Protocol Version Usage: 0xFF for discovery requests, 0x03 for TCP

**PASS**

- `DOIP_VERSION_DISCOVERY = 0xFF` is the default for `build_doip()`.
- UDP functions call `build_doip(TYPE_VEHICLE_ID_REQUEST)` without overriding version, so they use 0xFF -- correct per ISO 13400-2 for vehicle identification requests.
- `DoIPConnection.send_doip()` defaults to `DOIP_VERSION = 0x03` -- correct for TCP data communication.
- The header validation in `parse_header()` checks `(ver ^ inv) & 0xFF != 0xFF`, which accepts any valid version/inverse pair -- correctly handles server responses using version 0x03.

---

## 10. Test #10 (no routing): expects silence/timeout, not NACK

**PASS**

```python
def test_diagnostic_without_routing():
    with DoIPConnection(host, port) as conn:
        payload = struct.pack("!HH", SOURCE_ADDR, TARGET_ADDR) + bytes([0x3E, 0x00])
        conn.send_doip(TYPE_DIAGNOSTIC_MESSAGE, payload)
        try:
            conn.sock.settimeout(2.0)
            _ptype, _resp = conn.recv_doip()
            assert False, "Server responded to diagnostic without routing (expected silence)"
        except (socket.timeout, ConnectionError):
            print(f"[PASS] Diagnostic without routing (silence — timeout)")
```

Server behavior (doip_server.c line 220-222):
```c
if (!conn->routing_activated) {
    printf("[DoIP Server] Diagnostic msg rejected - routing not active\n");
    break;  /* silently drops -- no response sent */
}
```

The server silently drops the message (no NACK, no response). The test correctly expects a `socket.timeout` or `ConnectionError`, not a NACK response. The 2-second timeout is reasonable.

---

## Overall Verdict: PASS

All 10 protocol correctness checks pass. The Python test script correctly implements:
- DoIP header format (version, inverse, big-endian type and length)
- Vehicle announcement field offsets (VIN, logical addr, EID, GID, further action, sync status)
- Routing activation request/response wire format
- Two-phase diagnostic flow (ACK then response)
- Entity status and power mode type codes and payload layouts
- UDS TesterPresent with correct sub-function byte and positive response check
- UDS negative response parsing (0x7F + SID + NRC)
- Correct protocol version usage (0xFF for UDP discovery, 0x03 for TCP)
- Silence/timeout expectation for unrouted diagnostic messages

No protocol correctness issues found.
