# Correctness Review — server_test1.md
**Reviewer**: Correctness
**Date**: 2026-03-05
**Status**: ROUND 3

## Summary
43 PASS, 1 FAIL, 4 INFO

## Round 2 FAIL Fix Verification

| Round 2 ID | Issue | v2.1 Status |
|-------------|-------|-------------|
| CR-R2-23 | Server transport limit (max UDS 4092 not 4096) | **PARTIALLY FIXED** — Note 8 in Section 9.3 documents the issue. However, C.2/C.3 procedures still derive block size from server response (4096), not the corrected 4092. See CR-R3-01. |
| CR-R2-39 | Section 10.3 stale counts | **FIXED** — Section 10.3 now shows A=3, B=14, C=5, D=14, E=4, total=40. |
| CR-R2-26 | C.5 byte value mismatch | **FIXED** — C.5 (line 638) now says `generate_test_blob(1)` producing `0xA5`, and line 643 verifies value `0xA5`. Consistent. |

---

## Test Count Consistency Check

All four locations now agree:

| Location | A | B | C | D | E | F | In-process | Total |
|----------|---|---|---|---|---|---|------------|-------|
| Section 9.2 (function list) | 3 | 14 | 5 | 14 | 4 | 7 (shell) | 40 | 47 |
| Section 10.3 (expected output) | 3 | 14 | 5 | 14 | 4 | — | 40 | — |
| Appendix D (summary table) | 3 | 14 | 5 | 14 | 4 | 7 | — | 47 |
| Appendix E (file deliverables) | — | — | — | — | — | — | 40 | — |
| Section 9.1 (line estimate) | — | — | — | — | — | — | 40 | — |
| Actual test headers (Sections 3-7) | 3 | 14 | 5 | 14 | 4 | 7 | 40 | 47 |

**Result**: All consistent. PASS.

---

## Findings

### [PASS] CR-R3-PRE-01: CR-R2-39 Fix — Section 10.3 Counts
**Location**: Section 10.3 (lines 1238-1256)
**Expected**: A=3, B=14, C=5, D=14, E=4, total=40
**Actual (plan)**: Line 1238: "Suite A: UDP Discovery — 3 tests", Line 1243: "Suite B: TCP Protocol — 14 tests", Line 1246: "Suite C: Blob Write — 5 tests", Line 1249: "Suite D: Error Handling — 14 tests", Line 1252: "Suite E: Concurrent Access — 4 tests", Line 1256: "Results: 40 passed, 0 failed"
**Result**: All match. **FIXED from Round 2.**

---

### [PASS] CR-R3-PRE-02: CR-R2-26 Fix — C.5 Byte Value 0xA5
**Location**: Section 5, Test C.5 (lines 638, 643)
**Expected**: Byte value matches `generate_test_blob(1)` output = `(0 & 0xFF) ^ 0xA5` = 0xA5
**Actual (plan)**: Line 638: "using `generate_test_blob(1)` which produces `0xA5`". Line 643: "value `0xA5`".
**Result**: Consistent with helper function at line 492. **FIXED from Round 2.**

---

### [FAIL] CR-R3-01: C.2/C.3 Block Size Still Derived from Server Response (4096), Not Transport-Safe Limit (4092)
**Location**: Section 5, C.2 (lines 561, 569), C.3 (line 589), Section 9.3 note 8 (line 1185)
**Expected**: C.2 and C.3 procedures should compute block sizes that stay within the 4092-byte UDS transport limit.
**Actual (plan)**: Note 8 (line 1185) correctly documents that "effective maximum UDS data per block is 4092 bytes, not 4096" and says tests "should use blocks that stay within the 4092-byte UDS limit." However:
- C.2 description (line 561) says "max_block_length = 4094 bytes of UDS data per block" — this is computed as 4096 (server response) minus 2 (SID+BSC), but the correct computation using the transport-safe limit is 4092 - 2 = 4090.
- C.2 step 6 (line 569) says: `block_data_size = max_block_length - 2` — this derives from the server's 4096 response, yielding 4094 data bytes per block. A TransferData with 4094 data bytes = 4096 UDS bytes, which wraps to 4100 DoIP payload bytes (SA(2)+TA(2)+UDS(4096)). The server transport at `doip_server.c:150` checks `payload_length > 4096` and rejects with HEADER_NACK_MESSAGE_TOO_LARGE.
- C.3 (line 589) says "256 * 4094 + 100" — also uses 4094 (wrong), should be 4090.
- C.2 validation (line 581) says `ceil(8004 / (max_block_length - 2))` — same issue.
**Impact**: **HIGH** — C.2 and C.3 will fail at the transport layer if implemented as written. The note documents awareness but the procedures contradict it.
**Fix**: In C.2 step 6, add: "Cap at transport-safe limit: `if (max_block_length > 4092) max_block_length = 4092;`" before computing `block_data_size`. Update C.2 description from "4094" to "4090". Update C.3 description from "256 * 4094" to "256 * 4090". Or, change C.2 step 6 to: `block_data_size = min(max_block_length, DOIP_MAX_DIAGNOSTIC_SIZE - 4) - 2`.
**Status**: **PARTIALLY FIXED from Round 2 (CR-R2-23).** Note 8 is correct but C.2/C.3 procedures are not updated to match.

---

### [PASS] CR-R3-02: Test A.1 — UDP Generic Discovery Response Fields
**Location**: Section 3 (Test A.1) + `doip_server.c:493-508`, `config.c:90-116`
**Expected (plan)**: VIN=`FC1BLOBSRV0000001`, logical_address=0x0001, EID=00:1A:2B:3C:4D:5E, GID=00:1A:2B:3C:4D:5E, further_action=0x00, sync_status=0x00
**Actual (code)**: Server builds announcement from `server->config` fields, populated from `doip-server.conf`.
**Result**: Identity fields match.

---

### [PASS] CR-R3-03: Test A.2/A.3 — VIN and EID Filtering
**Location**: Section 3 (Tests A.2, A.3) + `doip_server.c:478-489`
**Expected (plan)**: Correct VIN/EID gets response, wrong VIN/EID gets no response.
**Actual (code)**: `memcmp()` on VIN/EID, no response on mismatch.
**Result**: Matches.

---

### [PASS] CR-R3-04: Test B.1 — Routing Activation
**Location**: Section 4 (Test B.1) + `main.c:425`
**Expected (plan)**: `DOIP_ROUTING_ACTIVATION_SUCCESS (0x10)`, entity_logical_address=0x0001, tester_logical_address=0x0E80
**Actual (code)**: Always returns SUCCESS. Response uses `server->config.logical_address` and `req->source_address`.
**Result**: Matches.

---

### [PASS] CR-R3-05: Test B.2 — TesterPresent Suppress
**Location**: Section 4 (Test B.2) + `main.c:396-397`
**Expected (plan)**: No response when subfunction has bit 7 set (0x80)
**Actual (code)**: `if (uds_len >= 2 && (uds_data[1] & 0x80)) { result = 0; }`
**Result**: Matches.

---

### [PASS] CR-R3-06: Test B.3 — Entity Status Response
**Location**: Section 4 (Test B.3) + `doip_server.c:280-296`
**Expected (plan)**: node_type=0, max_concurrent_sockets=4, currently_open_sockets>=1, max_data_size=4096
**Actual (code)**: All from `server->config`, matching `doip-server.conf`.
**Result**: Matches.

---

### [PASS] CR-R3-07: Test B.4 — Power Mode
**Location**: Section 4 (Test B.4) + `doip_server.c:300-305`
**Expected (plan)**: Power mode = 0x01
**Actual (code)**: Hardcoded `{ 0x01 }`.
**Result**: Matches.

---

### [PASS] CR-R3-08: Test B.5 — Unsupported UDS Service NRC
**Location**: Section 4 (Test B.5) + `main.c:405-406`
**Expected (plan)**: `{0x7F, 0x10, 0x11}`
**Actual (code)**: Default case: `build_negative_response(sid, 0x11, ...)`.
**Result**: Matches.

---

### [PASS] CR-R3-09: Test B.6 — Diagnostic Without Routing (Raw Socket)
**Location**: Section 4 (Test B.6) + `doip_server.c:220-222`
**Expected (plan)**: Raw TCP, no routing. Server silently drops.
**Actual (code)**: `!conn->routing_activated` → `break` (stays in while loop).
**Result**: Matches.

---

### [PASS] CR-R3-10: Test B.7 — Diagnostic NACK Unknown Target
**Location**: Section 4 (Test B.7) + `doip_server.c:236-242`
**Expected (plan)**: NACK code = 0x03
**Actual (code)**: `is_known_target()` returns false for 0xFFFF → NACK 0x03.
**Result**: Matches.

---

### [PASS] CR-R3-11: Test B.8 — Header NACK Bad Version
**Location**: Section 4 (Test B.8) + `doip_server.c:130-137`
**Expected (plan)**: NACK code 0x00, connection closes
**Actual (code)**: `doip_deserialize_header()` fails → NACK 0x00 → `break` exits while loop → fd closed.
**Result**: Matches.

---

### [PASS] CR-R3-12: Test B.9 — Header NACK Unknown Payload Type
**Location**: Section 4 (Test B.9) + `doip_server.c:308-312`
**Expected (plan)**: NACK code 0x01, connection stays alive
**Actual (code)**: Default case → NACK 0x01 → `break` exits switch (not while loop).
**Result**: Matches.

---

### [PASS] CR-R3-13: Test B.10 — Connection Limit
**Location**: Section 4 (Test B.10) + `doip_server.c:382-387`
**Expected (plan)**: 5th connection refused or rejected
**Actual (code)**: `num_clients >= max_tcp_connections` → `close(client_fd)`.
**Result**: Matches.

---

### [PASS] CR-R3-14: Test B.12 — Source Address Mismatch NACK
**Location**: Section 4 (Test B.12) + `doip_server.c:226-232`
**Expected (plan)**: NACK code = 0x02
**Actual (code)**: `diag->source_address != conn->tester_address` → NACK 0x02.
**Result**: Matches.

---

### [PASS] CR-R3-15: Test B.13 — Header NACK Message Too Large
**Location**: Section 4 (Test B.13) + `doip_server.c:150-155`
**Expected (plan)**: NACK code = 0x02, connection stays alive
**Actual (code)**: `payload_length > DOIP_MAX_DIAGNOSTIC_SIZE` → NACK 0x02 → `continue`.
**Result**: Matches.

---

### [PASS] CR-R3-16: Test B.14 — Header NACK Invalid Payload Length
**Location**: Section 4 (Test B.14) + `doip_server.c:167-172`
**Expected (plan)**: NACK code = 0x04, connection stays alive
**Actual (code)**: `doip_parse_message()` fails → NACK 0x04 → `continue`.
**Result**: Matches.

---

### [PASS] CR-R3-17: Test C.1 — Small Blob Transfer Byte Encoding
**Location**: Section 5 (Test C.1) + `main.c:245-249`
**Expected (plan)**: Response `{0x74, 0x20, hi, lo}`, request bytes `{0x34, 0x00, 0x44, ...}`
**Actual (code)**: SID=0x34, format=0x44 → 4-byte addr + 4-byte size. Response: `0x74, 0x20, 0x10, 0x00`.
**Result**: Byte encodings verified.

---

### [PASS] CR-R3-18: Test C.1 — TransferData and TransferExit Responses
**Location**: Section 5 (Test C.1) + `main.c:289-294, 330-332`
**Expected (plan)**: TransferData: `{0x76, 0x01}`. TransferExit: `{0x77}`.
**Actual (code)**: `response[0] = 0x76; response[1] = block_seq;` and `response[0] = 0x77;`.
**Result**: Matches.

---

### [PASS] CR-R3-19: Test C.1 — CRC Stripping on Disk
**Location**: Section 5 (Test C.1) + `main.c:314,324`
**Expected (plan)**: Disk file = 100 bytes (CRC stripped from 104)
**Actual (code)**: `data_len = g_transfer.bytes_received - 4;` then `save_blob(g_transfer.buffer, data_len, ...)`.
**Result**: Matches.

---

### [PASS] CR-R3-20: Test C.3 — Block Sequence Counter Wrap
**Location**: Section 5 (Test C.3) + `main.c:77,282`
**Expected (plan)**: BSC wraps 0xFF → 0x00 → 0x01
**Actual (code)**: `g_transfer.block_sequence` is `uint8_t`, wraps naturally. Server starts at 1.
**Result**: Matches (assuming transport-safe block sizes are used).

---

### [PASS] CR-R3-21: Test C.4 — Back-to-Back Transfers
**Location**: Section 5 (Test C.4) + `main.c:353`
**Expected (plan)**: Two consecutive transfers, state resets between them
**Actual (code)**: `transfer_cleanup_locked()` called at end of `handle_transfer_exit()`.
**Result**: Matches.

---

### [PASS] CR-R3-22: Tests D.1-D.3 — Download Rejection NRCs
**Location**: Section 6 (D.1, D.2, D.3) + `main.c:185-219`
**Detail**:
- D.1: active transfer → NRC 0x70 (matches `main.c:187`)
- D.2: size=16777217 > blob_max_size → NRC 0x70 (matches `main.c:215-218`)
- D.3: size=0 → NRC 0x70 (matches `main.c:215`)
**Result**: All match.

---

### [PASS] CR-R3-23: Tests D.4-D.5 — Format/Length NRCs
**Location**: Section 6 (D.4, D.5) + `main.c:191-199`
**Detail**:
- D.4: uds_len=3 < 4 → NRC 0x13 (matches)
- D.5 Case A: addr_len=0 → NRC 0x31 (matches)
- D.5 Case B: size_len=5 > 4 → NRC 0x31 (matches)
**Result**: All match.

---

### [PASS] CR-R3-24: Tests D.6-D.9 — Transfer State Error NRCs
**Location**: Section 6 (D.6-D.9) + `main.c:257-269, 274-277, 304-305`
**Detail**:
- D.6: No active → NRC 0x24 (matches `main.c:257-258`)
- D.7: Wrong BSC → NRC 0x73 (matches `main.c:266-269`)
- D.8: Overflow → NRC 0x71 (matches `main.c:274-276`). Plan label "transferDataSuspended" correct.
- D.9: Exit without active → NRC 0x24 (matches `main.c:304-305`)
**Result**: All match.

---

### [PASS] CR-R3-25: Test D.10 — CRC Mismatch NRC
**Location**: Section 6 (D.10) + `main.c:333-337`
**Expected (plan)**: NRC 0x72, no blob on disk
**Actual (code)**: CRC mismatch → NRC 0x72, no `save_blob()` on this path.
**Result**: Matches.

---

### [PASS] CR-R3-26: Test D.11 — Transfer Timeout
**Location**: Section 6 (D.11) + `main.c:588-600`
**Expected (plan)**: Sleep 35s (30s timeout + 5s margin), then TransferData → NRC 0x24
**Actual (code)**: Main loop checks every ~1s, cleans up after 30s.
**Result**: Matches. 5-second margin adequate.

---

### [PASS] CR-R3-27: Test D.13 — TransferData Too Short
**Location**: Section 6 (D.13) + `main.c:260-261`
**Expected (plan)**: `{0x36}` (1 byte) → NRC 0x13
**Actual (code)**: `uds_len < 2` → NRC 0x13.
**Result**: Matches.

---

### [PASS] CR-R3-28: Test D.14 — Blob Too Small for CRC
**Location**: Section 6 (D.14) + `main.c:339-351`
**Expected (plan)**: Positive response `{0x77}`, server saves as-is
**Actual (code)**: `bytes_received = 2 < 4` → else branch → `save_blob(buffer, 2, addr)` → `{0x77}`.
**Result**: Matches.

---

### [PASS] CR-R3-29: Tests E.1-E.3 — Concurrent Access
**Location**: Section 7 (E.1-E.3) + `main.c:185-187, 393-407`
**Detail**:
- E.1: Concurrent RequestDownload → NRC 0x70 (matches)
- E.2: Client B disconnect doesn't affect transfer (global state unaffected by connection cleanup)
- E.3: Independent TesterPresent per thread (no contention)
**Result**: All match.

---

### [PASS] CR-R3-30: Test E.4 — Client Disconnect Retry Loop
**Location**: Section 7 (E.4) + `main.c:588-600`, `doip_server.c:318-327`
**Expected (plan)**: Retry up to 35s, NRC 0x70 initially, success after timeout
**Actual (code)**: Disconnect cleans connection but NOT `g_transfer`. Timeout at 30s → cleanup.
**Result**: Matches. Note 7 in Section 9.3 correctly documents this behavior.

---

### [PASS] CR-R3-31: Appendix A — NRC Labels
**Location**: Appendix A (lines 1276-1287)
**Detail**: All NRC hex values and labels verified against ISO 14229-1:
- 0x13 = incorrectMessageLengthOrInvalidFormat
- 0x24 = requestSequenceError
- 0x31 = requestOutOfRange
- 0x70 = uploadDownloadNotAccepted
- 0x71 = transferDataSuspended (correctly labeled since v2.0)
- 0x72 = generalProgrammingFailure
- 0x73 = wrongBlockSequenceCounter
- 0x11 = serviceNotSupported
**Result**: All correct.

---

### [PASS] CR-R3-32: Appendix D — Test Count Summary
**Location**: Appendix D (lines 1357-1369)
**Actual**: A=3, B=14, C=5, D=14, E=4, F=7, Total=47
**Result**: Matches actual test headers and Section 9.2. Correct.

---

### [PASS] CR-R3-33: Section 9.2 — Function List Count
**Location**: Section 9.2 (lines 1101-1167)
**Detail**: Lists 3 Suite A + 14 Suite B + 5 Suite C + 14 Suite D + 4 Suite E = 40 in-process functions + 7 shell. Suite headers match.
**Result**: Correct.

---

### [PASS] CR-R3-34: Section 9.1 — File Line Estimate
**Location**: Section 9.1 (line 1097)
**Actual**: "~550-650 lines (40 in-process tests: Suites A-E)"
**Result**: 40 matches actual count. Pass.

---

### [PASS] CR-R3-35: Appendix E — File Deliverables
**Location**: Appendix E (line 1375)
**Actual**: "~550-650 lines, 40 tests: Suites A-E" and "~60 lines, 7 tests: Suite F"
**Result**: Consistent with all other locations.

---

### [PASS] CR-R3-36: Config Values — Plan vs Code vs Config File
**Location**: Section 2.6 + `config.c:90-116` + `doip-server.conf`
**Detail**: VIN, logical_address, EID, GID, ports, max_tcp_connections, max_data_size, blob_max_size, transfer_timeout all consistent across plan, code defaults, and config file.
**Result**: All match.

---

### [PASS] CR-R3-37: Section 9.3 Note 8 — Transport Limit Documentation
**Location**: Section 9.3 note 8 (line 1185)
**Detail**: Note correctly states: (1) server advertises max_block_length = 4096, (2) DoIP transport limit is 4096 payload bytes, (3) a 4096-byte UDS payload wraps to 4100 DoIP bytes, (4) effective max UDS is 4092, (5) tests should use 4092 limit.
**Result**: The note itself is technically correct and complete.

---

### [PASS] CR-R3-38: Test C.5 — generate_test_blob Consistency
**Location**: Section 5 (C.5 line 638) + Section 5.2 (line 489-496)
**Detail**: `generate_test_blob(buf, 1)` → `buf[0] = (0 & 0xFF) ^ 0xA5 = 0xA5`. C.5 says "produces `0xA5`" and verifies "value `0xA5`".
**Result**: Fully consistent.

---

### [PASS] CR-R3-39: Suite A Count — Section 9.2 Header vs Functions
**Location**: Section 9.2 (line 1112)
**Detail**: Header says "Suite A: UDP Discovery (3 tests)", lists `test_a1`, `test_a2`, `test_a3`. Three functions. Previously was 5 in Round 2 (including A.4/A.5 which were consolidated into A.1-A.3 in v2.1).
**Result**: Matches.

---

### [PASS] CR-R3-40: C.2 Validation Formula Consistency
**Location**: Section 5, C.2 validation (line 581)
**Detail**: `ceil(8004 / (max_block_length - 2))` — the formula is internally consistent with step 6, though both use the uncorrected max_block_length. If fixed per CR-R3-01 (cap at 4092), formula would give `ceil(8004 / 4090) = 2` blocks. Currently gives `ceil(8004 / 4094) = 2` blocks. For 8004 bytes, both yield 2 blocks and the individual blocks (4004 and 4000) stay under 4090, so C.2 would actually pass by coincidence — the blocks are small enough not to hit the transport limit.
**Result**: PASS with caveat — C.2 happens to work because 8004 bytes splits into blocks smaller than 4090 each. C.3 (1MB) WILL fail because it sends full-capacity blocks.

---

### [PASS] CR-R3-41: B.11 Entity Status Socket Count
**Location**: Section 4 (Test B.11) + `doip_server.c:280-282`
**Expected (plan)**: Verify `currently_open_sockets` matches expected count
**Actual (code)**: `open_sockets = (uint8_t)server->num_clients` under mutex lock.
**Result**: Matches.

---

### [PASS] CR-R3-42: D.12 Unsupported SIDs
**Location**: Section 6 (Test D.12) + `main.c:405-406`
**Expected (plan)**: Multiple SIDs → NRC 0x11 each
**Actual (code)**: Default case in switch → `build_negative_response(sid, 0x11, ...)`.
**Result**: All non-handled SIDs get 0x11. Matches.

---

### [PASS] CR-R3-43: Section 9.3 Note 7 — E.4 Disconnect Behavior
**Location**: Section 9.3 note 7 (line 1183)
**Detail**: "Client disconnect does NOT reset server transfer state. E.4 uses a retry loop (up to 35 seconds)."
**Actual (code)**: `doip_server.c:318-327` cleans connection state only, not `g_transfer`. `main.c:588-600` handles timeout cleanup.
**Result**: Documentation matches code.

---

### [INFO] CR-R3-44: C.2 Accidental Pass — Block Size Below Transport Limit
**Location**: Section 5 (Test C.2)
**Detail**: C.2 transfers 8004 bytes with `block_data_size = 4094`. This gives 2 blocks: block 1 = 4094 bytes, block 2 = 3910 bytes. UDS payloads = 4096 and 3912 respectively. Block 1 wraps to DoIP payload 4100 > 4096 transport limit. **C.2 block 1 WILL be rejected**, not just C.3.
**Correction to CR-R3-40**: On further analysis, C.2's first block at 4094 data bytes = 4096 UDS bytes = 4100 DoIP bytes, which exceeds the transport limit. C.2 does NOT pass by coincidence. Both C.2 and C.3 are affected by CR-R3-01.
**Note**: This reinforces the severity of CR-R3-01.

---

### [INFO] CR-R3-45: main.c:276 — Code Comment Mislabels NRC 0x71
**Location**: `main.c:276`
**Detail**: Comment says `/* requestOutOfRange */` but NRC 0x71 is "transferDataSuspended". The code behavior is correct (sends 0x71), only the comment is wrong. This is a server code issue, not a test plan issue. The test plan correctly labels 0x71 as "transferDataSuspended" in D.8 and Appendix A.
**Note**: No test plan change needed. Server code comment should be fixed separately.

---

### [INFO] CR-R3-46: B.9 — Routing Activation Before Unknown Type
**Location**: Section 4 (Test B.9)
**Detail**: Server's `default:` case at `doip_server.c:308-313` does not check `routing_activated`. Routing before sending unknown type is unnecessary but harmless. If routing fails for any reason, the test could fail for the wrong reason.
**Note**: Carried from Round 2 (CR-R2-43). No change needed — routing first mimics realistic behavior.

---

### [INFO] CR-R3-47: CRC-32 Host Byte Order Consistency
**Location**: Section 5.2 + `main.c:316-317`
**Detail**: Both test and server use `memcpy()` for CRC — host byte order. Consistent for localhost testing. Would only matter for cross-architecture testing (out of scope).
**Note**: Carried from Round 2 (CR-R2-46). No issue.

---

## FAIL Summary

| ID | Test(s) | Issue | Severity | History |
|----|---------|-------|----------|---------|
| CR-R3-01 | C.2, C.3 | C.2/C.3 procedures derive block_data_size from server's max_block_length (4096) via `max_block_length - 2 = 4094`. This produces UDS payloads of 4096 bytes → DoIP payloads of 4100 bytes, exceeding the 4096 transport limit. Section 9.3 note 8 correctly documents the 4092 limit but C.2/C.3 procedures are not updated to use it. C.2's first block and all of C.3's full blocks will be transport-rejected. | **HIGH** | Carried from Round 1 CR-24 → Round 2 CR-R2-23 (note added but procedures not updated) |

## Comparison: Round 1 → Round 2 → Round 3

| Metric | Round 1 | Round 2 | Round 3 |
|--------|---------|---------|---------|
| Total findings | 53 | 48 | 48 |
| PASS | 33 | 37 | 43 |
| FAIL | 8 (3 HIGH) | 3 (1 HIGH, 1 MEDIUM, 1 LOW) | 1 (1 HIGH) |
| INFO | 7 | 6 | 4 |
| Round N-1 FAILs fixed | — | 4 of 7 | 2 of 3 |

## Final Assessment

**43 PASS, 1 FAIL (1 HIGH), 4 INFO**

v2.1 resolved 2 of 3 Round 2 issues:
- **CR-R2-39 (Section 10.3 counts)**: Fully fixed. All count locations now consistent (40 in-process, 47 total).
- **CR-R2-26 (C.5 byte value)**: Fully fixed. Now uses `generate_test_blob(1)` producing 0xA5 consistently.

One issue remains:
1. **CR-R3-01 (HIGH)**: The transport layer limit vs max_block_length mismatch. Section 9.3 note 8 correctly documents the problem and the correct limit (4092), but C.2 and C.3 test procedures still compute block sizes from the server's 4096 response. This means both C.2 (first block) and C.3 (all full blocks) will be transport-rejected at runtime. **Fix**: Add an explicit cap in C.2 step 6: `max_block_length = min(max_block_length, 4092)` before computing `block_data_size`, and update C.2/C.3 descriptions to use 4090 as the per-block data capacity.
