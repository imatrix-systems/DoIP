# Plan: Python DoIP Test Script

## Goal

Create `doip_test.py` — a standalone Python 3 script that connects to the DoIP server and queries basic information via UDP discovery and TCP messaging. No external dependencies beyond Python 3.6+ standard library.

The script must start with `#!/usr/bin/env python3`, include an `if __name__ == "__main__"` guard, and be marked executable (`chmod +x`).

## Server Capabilities to Test

Based on the server implementation, these are the queryable operations:

### UDP (port 13400) — Discovery

| # | Query | DoIP Type | Expected Response |
|---|-------|-----------|-------------------|
| 1 | Vehicle ID Request (broadcast) | 0x0001 | Vehicle Announcement (0x0004): VIN, logical addr, EID, GID, further action, sync status |
| 2 | Vehicle ID by VIN (positive) | 0x0003 | Vehicle Announcement if VIN matches |
| 3 | Vehicle ID by VIN (negative) | 0x0003 | Silence (timeout) if wrong VIN |
| 4 | Vehicle ID by EID (positive) | 0x0002 | Vehicle Announcement if EID matches |

### TCP (port 13400) — Queries

| # | Query | Mechanism | Expected Response |
|---|-------|-----------|-------------------|
| 5 | Routing Activation | DoIP type 0x0005 (SA=0x0E80, type=0x00) | Response 0x0006 with code 0x10 (success) |
| 6 | Entity Status | DoIP type 0x4001 (no payload) | Response 0x4002: node_type, max_sockets, open_sockets, max_data_size |
| 7 | Diagnostic Power Mode | DoIP type 0x4003 (no payload) | Response 0x4004: power_mode=0x01 (ready) |
| 8 | TesterPresent | UDS SID [0x3E, 0x00] via diagnostic msg 0x8001 | Diagnostic ACK (0x8002) then positive response 0x7E via 0x8001 |
| 9 | Unsupported SID | UDS SID 0x22 via diagnostic msg | Diagnostic ACK (0x8002) then negative response 0x7F+0x22+0x11 via 0x8001 |
| 10 | Diagnostic without routing | UDS SID 0x3E before routing activation | Silence (server drops message — expect recv timeout) |

## DoIP Protocol Details

### Header Format (8 bytes)
```
Byte 0:    Protocol version (always 0x03 for server responses; requests may use 0xFF)
Byte 1:    Inverse version (~version & 0xFF)
Bytes 2-3: Payload type (big-endian uint16)
Bytes 4-7: Payload length (big-endian uint32)
```

**Note:** The server sends version 0x03 for ALL responses, including UDP announcements. Discovery requests should use version 0xFF per ISO 13400, but the script must accept 0x03 responses on UDP.

### Vehicle Announcement Response (33 bytes payload)
```
Bytes 0-16:  VIN (17 ASCII chars)
Bytes 17-18: Logical address (big-endian uint16)
Bytes 19-24: EID (6 bytes)
Bytes 25-30: GID (6 bytes)
Byte 31:     Further action required
Byte 32:     VIN/GID sync status
```

### Routing Activation Request (7 bytes payload)
```
Bytes 0-1: Source address (big-endian uint16, e.g., 0x0E80)
Byte 2:    Activation type (0x00 = default)
Bytes 3-6: Reserved (0x00000000)
```

### Routing Activation Response (9 bytes payload)
```
Bytes 0-1: Tester logical address (big-endian uint16, echo of source)
Bytes 2-3: Entity logical address (big-endian uint16, e.g., 0x0001)
Byte 4:    Response code (0x10 = success)
Bytes 5-8: Reserved (0x00000000)
```

### Entity Status Response (7 bytes payload)
```
Byte 0:    Node type (0x00 = gateway)
Byte 1:    Max concurrent TCP sockets
Byte 2:    Currently open TCP sockets
Bytes 3-6: Max data size (big-endian uint32, optional — present when has_max_data_size=true)
```

### Diagnostic Power Mode Response (1 byte payload)
```
Byte 0:    Power mode (0x01 = ready)
```

### Diagnostic Message (4+ bytes payload)
```
Bytes 0-1: Source address (big-endian uint16)
Bytes 2-3: Target address (big-endian uint16)
Bytes 4+:  UDS data (SID + parameters)
```

### Diagnostic Message Flow (two-phase)

When sending a diagnostic message (0x8001), the server responds with **two** TCP messages:
1. **Diagnostic ACK** (0x8002): 5-byte payload = target_addr(2) + source_addr(2) + ack_code(1)
2. **Diagnostic Response** (0x8001): source_addr(2) + target_addr(2) + UDS response data

The script must `recv_exact()` twice: first the ACK, then the actual UDS response. If routing is not activated, the server silently drops the message (no response sent — the script will observe a recv timeout).

### Diagnostic NACK (0x8003, 5 bytes payload)
```
Bytes 0-1: Source address
Bytes 2-3: Target address
Byte 4:    NACK code (0x02=invalid SA, 0x03=unknown TA, 0x05=out of memory, etc.)
```

### TesterPresent UDS Detail
```
Request:  [0x3E, 0x00]           (SID + sub-function 0x00)
Response: [0x7E, 0x00]           (positive response)
```
If sub-function bit 7 is set (`[0x3E, 0x80]`), the server suppresses the positive response (returns 0 bytes).

## Script Design

### Structure
```
doip_test.py
├── Constants (addresses, timeouts, defaults)
│
├── DoIP protocol helpers
│   ├── build_doip(payload_type, payload, version=0xFF) → bytes
│   ├── parse_header(data) → (version, inv_ver, ptype, length) with validation
│   ├── recv_exact(sock, n) → bytes  (loop until n bytes received)
│   ├── recv_doip(sock) → (ptype, payload)  (header + exact payload read)
│   ├── parse_announcement(payload) → dict
│   └── format_bytes(data) → "XX:XX:XX" string
│
├── UDP discovery functions
│   ├── udp_vehicle_id_request(host, port) → announcement dict or None
│   ├── udp_vehicle_id_by_vin(host, port, vin) → announcement or None
│   └── udp_vehicle_id_by_eid(host, port, eid_bytes) → announcement or None
│
├── TCP session (context manager): DoIPConnection
│   ├── __init__(host, port, verbose)
│   ├── __enter__ / __exit__  (socket cleanup)
│   ├── connect()
│   ├── send_doip(payload_type, payload) → None
│   ├── recv_doip() → (ptype, payload)
│   ├── routing_activation(source_addr) → (code, entity_addr)
│   ├── entity_status() → dict
│   ├── power_mode() → int
│   ├── send_diagnostic(source, target, uds_data) → (ack_code, uds_response)
│   └── tester_present() → bool
│
├── Test runner
│   ├── run_test(name, func) → bool  (catches exceptions, prints PASS/FAIL)
│   └── Individual test functions (10 tests)
│
└── main()
    ├── Parse CLI args (argparse)
    ├── Validate inputs (host, port range)
    ├── Pre-flight: check server reachable (UDP probe with 2s timeout)
    ├── Run all tests (continue on failure)
    └── Print summary, exit 0/1
```

### CLI Interface
```bash
# Run against default server (127.0.0.1:13400, default VIN/EID)
python3 test/scripts/doip_test.py

# Specify server address
python3 test/scripts/doip_test.py --host 192.168.1.100

# Override expected identity values
python3 test/scripts/doip_test.py --vin APTERADOIPSRV0001 --eid 00:1A:2B:3C:4D:5E

# Verbose mode (print raw TX/RX hex)
python3 test/scripts/doip_test.py -v

# Also runnable directly
./test/scripts/doip_test.py -v --host 127.0.0.1
```

| Flag | Short | Default | Description |
|------|-------|---------|-------------|
| `--host` | `-H` | `127.0.0.1` | Server IP address |
| `--port` | `-p` | `13400` | Server port (TCP and UDP) |
| `--vin` | | `APTERADOIPSRV0001` | Expected VIN for validation |
| `--eid` | | `00:1A:2B:3C:4D:5E` | Expected EID for validation |
| `--verbose` | `-v` | off | Print raw TX/RX hex dumps |
| `--help` | `-h` | | Show usage with examples |

**argparse description:** "DoIP server test suite — queries basic identity and status information via UDP discovery and TCP diagnostics."

### Output Format
```
=== DoIP Server Test Suite ===
Server: 127.0.0.1:13400

--- UDP Discovery ---
[PASS] Vehicle ID Request
  VIN:     APTERADOIPSRV0001
  Address: 0x0001
  EID:     00:1A:2B:3C:4D:5E
  GID:     00:1A:2B:3C:4D:5E
[PASS] VIN filter (positive match)
[PASS] VIN filter (negative — no response)
[PASS] EID filter (positive match)

--- TCP Queries ---
[PASS] Routing activation (code=0x10, entity=0x0001)
[PASS] Entity status (node=gateway, max_sockets=4, open=1, max_data=4096)
[PASS] Power mode (ready)
[PASS] TesterPresent (0x7E response)
[PASS] Unsupported SID (NRC=0x11)
[PASS] Diagnostic without routing (silence — timeout)

=== Results: 10 passed, 0 failed ===
```

## Implementation Steps

| Step | Description | Lines (est.) |
|------|-------------|-------------|
| 1 | Shebang, imports, constants (addresses, timeouts, defaults) | ~15 |
| 2 | DoIP helpers: build_doip, parse_header, recv_exact, recv_doip, parse_announcement, format_bytes | ~60 |
| 3 | UDP discovery functions (3 variants) | ~45 |
| 4 | DoIPConnection class (context manager, connect, send/recv, routing, entity_status, power_mode, diagnostic, tester_present) | ~100 |
| 5 | Test runner + 10 test functions | ~90 |
| 6 | CLI arg parsing (argparse with validation) + main | ~40 |

**Total: ~350 lines**

## Key Design Decisions

1. **No external dependencies** — uses only `socket`, `struct`, `argparse`, `sys`, `time` from stdlib
2. **No config file parser** — VIN and EID passed via `--vin`/`--eid` flags with hardcoded defaults matching `doip-server.conf`. Avoids maintaining a second parser in a different language. (KISS R1 finding)
3. **Timeouts** — 2-second UDP receive timeout (no response = pass for negative tests), 5-second TCP timeout
4. **Source address** — 0x0E80 (standard external tester address), matching the C test tool
5. **Target address** — 0x0001 (server's default logical address)
6. **Two-phase diagnostic reads** — after sending diagnostic 0x8001, read ACK (0x8002) first, then UDS response (0x8001). (Protocol R1 finding)
7. **recv_exact()** — loop on TCP `recv()` until exactly N bytes received, preventing partial-read bugs. (Error Handling R1 finding)
8. **Header validation** — every received DoIP header checked for valid version (0x03), correct inverse byte, sane payload length (<=4096). (Error Handling R1 finding)
9. **recv buffer cap** — reject any DoIP message with length > 4096 bytes. (Security R1 finding)
10. **Port validation** — argparse validates port in range 1–65535. (Security R1 finding)
11. **Context manager** — DoIPConnection uses `__enter__`/`__exit__` to guarantee socket cleanup. (Error Handling R1 finding)
12. **Pre-flight check** — UDP probe before running tests; clean "server not reachable" error. (Coverage R1 finding)
13. **Test independence** — each test catches its own exceptions; one failure doesn't block others
14. **Verbose mode** — `-v` prints raw TX/RX hex for protocol debugging. (Usability R1 finding)
15. **Exit code** — 0 if all pass, 1 if any fail (CI integration)
16. **Field-level assertions** — validate DoIP header version, entity logical address in routing response, payload length consistency. (Coverage R1 finding)

## Round 1 Review Resolution

| ID | Agent | Issue | Fix Applied |
|----|-------|-------|-------------|
| P-1 | Protocol | Diagnostic ACK (0x8002) before UDS response missing | Added two-phase flow in protocol details and design decision #6 |
| P-2 | Protocol | Version 0xFF for UDP incorrect | Fixed: server always sends 0x03; documented clearly |
| P-3 | Protocol | Missing response layouts | Added routing activation response, entity status response, diag ACK/NACK, power mode response, TesterPresent sub-function |
| C-1 | Coverage | Missing Entity Status and Power Mode tests | Added tests #6 and #7 |
| C-2 | Coverage | Missing pre-routing diagnostic negative test | Added test #10 |
| C-3 | Coverage | No server-not-running handling | Added pre-flight UDP probe |
| C-4 | Coverage | Weak field-level assertions | Added design decision #16 |
| C-5 | Coverage | Test count mismatch (6 functions vs 7 results) | Expanded to 10 explicit tests |
| E-1 | Error Handling | Two-phase diagnostic read missing | Same as P-1 |
| E-2 | Error Handling | No recv_exact loop for TCP | Added recv_exact() helper, design decision #7 |
| E-3 | Error Handling | No connection refused handling | Pre-flight check + exception catching in run_test |
| E-4 | Error Handling | No header validation | Added design decision #8 |
| E-5 | Error Handling | No context manager for cleanup | Added __enter__/__exit__, design decision #11 |
| E-6 | Error Handling | No config file error handling | Config file parser removed entirely (KISS) |
| U-1 | Usability | CLI inconsistency with C tool | Added -H/-p short flags |
| U-2 | Usability | No help text specification | Added argparse description |
| U-3 | Usability | No verbose flag | Added -v/--verbose, design decision #14 |
| U-4 | Usability | Path assumptions for config | Config parser removed, no path issues |
| U-5 | Usability | No shebang/permissions | Added to Goal section |
| S-1 | Security | No input validation | Added port range check, design decision #10 |
| S-2 | Security | No recv buffer cap | Added 4096-byte cap, design decision #9 |
| K-1 | KISS | Config parser unnecessary | Removed, use --vin/--eid flags |
| K-2 | KISS | Drop --port | Kept --port (server allows port override) but defaults to 13400 |
| P-R1 | Protocol R2 | Test #10 expects NACK but server silently drops | Changed to expect silence/timeout |

## File Location

```
DoIP_Server/test/scripts/doip_test.py
```
