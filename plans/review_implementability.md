# Implementability Review -- server_test1.md

**Reviewer**: Implementability
**Date**: 2026-03-05
**Status**: ROUND 3

## Summary

24 PASS, 1 FAIL, 5 INFO

Round 2 had 0 FAILs across 23 PASS + 5 INFO. The v2.1 changes (A.4/A.5 deleted, B.13 header-only send, F.6/F.7 updated, C.5 byte value fixed) introduce 1 new FAIL on F.6 where the test expects behavior the server code does not implement. All other v2.1 changes are correctly implementable.

---

## v2.1 Change Verification

### [FAIL] IM-29: Test F.6 — blob_storage_dir path traversal does NOT cause exit code 1

**Location**: Test F.6 procedure and validation, Section 8
**Plan expectation**: Server rejects config with `blob_storage_dir = /tmp/../etc/doip_blobs`, exits with code 1, and prints an error to stderr.
**Actual server behavior**: The config parser at `config.c:233` checks `strstr(value, "..") != NULL` and if true, prints a warning to stderr but **does not set an error flag or return -1**. `doip_config_load()` returns 0 (success) at line 262. The `blob_storage_dir` field retains its default value (from `doip_config_defaults()`). In `main.c:501-506`, since `doip_config_load()` returns 0, the server continues startup. At `main.c:539`, `ensure_storage_dir()` is called on the default path (not the traversal path), and even if it fails, main.c only prints a warning (line 540-541) and continues.
**Result**: The server starts successfully with the default `blob_storage_dir`, not with exit code 1. The test's validation criteria (`Exit code = 1`, `stderr.tmp contains error about invalid blob_storage_dir path`) will partially fail — stderr will contain a warning (from config.c:234), but exit code will be 0, not 1.
**Fix options**: (a) Change the test to expect exit code 0 and a warning message, reflecting current behavior. (b) Modify `config.c` to return -1 when path traversal is detected, which would cause `doip_config_load()` to fail and `main.c` to exit with code 1. Option (b) is a server code change, not a test plan change.

---

### [PASS] IM-30: Tests A.4/A.5 deleted — no residual references

**Location**: Suite A (Section 3), test count tables, code skeleton
**Detail**: Suite A now has exactly 3 tests (A.1, A.2, A.3). No references to A.4 or A.5 remain anywhere in the plan. The test count table (Appendix D) correctly shows "A: UDP Discovery | 3 tests". The total test count (47) is consistent: 3 + 14 + 5 + 14 + 4 + 7 = 47. No implementability issue.

---

### [PASS] IM-31: Test B.13 — Header-only send is correct and implementable

**Location**: Test B.13 procedure, Section 4 (line 434)
**Detail**: The v2.1 plan specifies sending ONLY the 8-byte DoIP header with `payload_length = 5000` and type `0x8001`, without sending any payload bytes. Verified against `doip_server.c:148-156`: the server checks `header.payload_length > DOIP_MAX_DIAGNOSTIC_SIZE` at line 150 **before** calling `tcp_recv_exact()` for payload data (line 158). On too-large, it sends the NACK at line 151-154 and uses `continue` at line 155, re-entering the recv loop for the next header. The plan's note (line 441) accurately explains why sending payload bytes would desync the connection. Implementation: construct 8 bytes manually (`{0x03, 0xFC, 0x80, 0x01, 0x00, 0x00, 0x13, 0x88}` for type=0x8001, length=5000), send via raw socket. Straightforward.

---

### [PASS] IM-32: Test C.5 — Byte value 0xA5 is correct

**Location**: Test C.5 procedure, Section 5 (line 638)
**Detail**: The plan says `generate_test_blob(1)` produces byte value `0xA5`. The `generate_test_blob()` function (Section 5.2, line 491-492) computes `buf[i] = (uint8_t)((i & 0xFF) ^ 0xA5)`. For size=1, i=0: `(0 & 0xFF) ^ 0xA5 = 0xA5`. Correct. The disk verification at step 7 expects 1 byte with value `0xA5`. Matches.

---

### [PASS] IM-33: Test F.7 — transfer_timeout=0 uses default, server starts

**Location**: Test F.7 procedure, Section 8 (line 1068-1089)
**Detail**: The plan says the server handles `transfer_timeout=0` gracefully, printing a warning and keeping the default (30 seconds). Verified at `config.c:248-249`: when `uval == 0`, the parser prints a warning and does NOT update `config->transfer_timeout_sec`, preserving the default. `doip_config_load()` returns 0. The server starts normally. This matches the plan's validation: "Server starts successfully" and "stderr contains warning." Implementable.

---

## Round 1 FAIL Resolutions (Re-confirmed)

### [PASS] IM-03: Test B.6 — Raw TCP socket (unchanged from v2.0)
### [PASS] IM-04: Test B.7 — send_diagnostic + recv_message (unchanged from v2.0)
### [PASS] IM-15: Test E.4 — Retry loop for transfer timeout (unchanged from v2.0)

All three Round 1 fixes remain correct in v2.1. No changes to these tests.

---

## Existing PASS Findings (Re-verified, unchanged)

### [PASS] IM-01: doip_client_send_uds() signature matches plan usage
### [PASS] IM-02: doip_client_send_diagnostic() signature and routing_active check
### [PASS] IM-05: doip_client_get_entity_status() exists and works as planned
### [PASS] IM-06: doip_client_get_power_mode() exists and works as planned
### [PASS] IM-07: doip_client_uds_request_download() correct signature
### [PASS] IM-08: doip_client_uds_transfer_data() correct signature
### [PASS] IM-09: doip_client_uds_request_transfer_exit() correct signature
### [PASS] IM-10: doip_client_activate_routing() correct usage
### [PASS] IM-11: Vehicle ID request builders exist
### [PASS] IM-12: Multiple doip_client_t instances can coexist
### [PASS] IM-13: CRC-32 implementations are compatible
### [PASS] IM-14: Raw TCP socket tests are feasible with tcp_raw_connect() helper
### [PASS] IM-16: doip_client_uds_tester_present() exists
### [PASS] IM-17: doip_config_load() exists for test config loading
### [PASS] IM-18: Makefile build targets and source list correct
### [PASS] IM-19: Test B.12 — Raw send on client's tcp_fd is accessible
### [PASS] IM-20: Test B.13 — Header NACK too-large message (updated in v2.1, now header-only)
### [PASS] IM-21: Test B.14 — Invalid payload length for routing activation
### [PASS] IM-22: Test D.4/D.5 — Malformed UDS via doip_client_send_uds()
### [PASS] IM-23: doip_parse_message() exists for UDP response parsing

---

## INFO Findings

### [INFO] IM-24: Test B.2 suppress-positive-response behavior is correct (unchanged)
### [INFO] IM-25: Suite C — Use API calls, not raw bytes (unchanged)
### [INFO] IM-26: Suite F config tests use shell script (unchanged)
### [INFO] IM-27: Test D.11 takes ~35 seconds, skippable (unchanged)

### [INFO] IM-34: Test F.7 procedure does not redirect stderr

**Location**: Test F.7 procedure (line 1077)
**Detail**: The procedure runs `./doip-server -c /tmp/doip_test_timeout.conf &` without a `2>stderr.tmp` redirect. Validation point 2 says "stderr contains warning about invalid transfer_timeout value" but the procedure as written does not capture stderr to a file. The shell script implementer would naturally add `2>stderr.tmp` to the command. Minor pseudocode omission — the test intent is clear and the implementer can trivially add the redirect.

---

## FAIL Summary

| ID | Test | Issue | Status |
|----|------|-------|--------|
| IM-29 | F.6 | Config parser warns but does not fail on path traversal; server starts with default blob_storage_dir; exit code is 0, not 1 | **NEW FAIL** |

**Round 3 result: 1 FAIL (F.6 exit code mismatch with actual server behavior). 24 PASS, 5 INFO.**
