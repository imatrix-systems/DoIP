# Test Coverage Review: doip_test.py

**Reviewer:** Test Coverage Agent
**Date:** 2026-03-09
**File:** `test/scripts/doip_test.py`
**Plan:** `test/scripts/plan_doip_test_script.md`

---

## 1. All 10 Planned Tests Implemented?

**PASS**

All 10 tests from the plan are present in `make_tests()`:

| # | Planned Test | Implementation | Present |
|---|-------------|----------------|---------|
| 1 | Vehicle ID Request (broadcast) | `test_udp_discovery` | Yes |
| 2 | VIN filter (positive) | `test_vin_positive` | Yes |
| 3 | VIN filter (negative) | `test_vin_negative` | Yes |
| 4 | EID filter (positive) | `test_eid_positive` | Yes |
| 5 | Routing activation | `test_routing_activation` | Yes |
| 6 | Entity status | `test_entity_status` | Yes |
| 7 | Power mode | `test_power_mode` | Yes |
| 8 | TesterPresent | `test_tester_present` | Yes |
| 9 | Unsupported SID | `test_unsupported_sid` | Yes |
| 10 | Diagnostic without routing | `test_diagnostic_without_routing` | Yes |

---

## 2. UDP Tests: broadcast, VIN+, VIN-, EID+ All Present?

**PASS**

- **Broadcast (0x0001):** `test_udp_discovery` sends `TYPE_VEHICLE_ID_REQUEST`, validates response.
- **VIN positive (0x0003):** `test_vin_positive` sends correct VIN, asserts non-None response and VIN match.
- **VIN negative (0x0003):** `test_vin_negative` sends `"WRONGVIN000000000"`, asserts `None` (silence/timeout).
- **EID positive (0x0002):** `test_eid_positive` sends correct EID bytes, asserts non-None response and EID match.

All four UDP discovery variants present with correct DoIP payload types.

---

## 3. TCP Tests: routing, entity status, power mode, TesterPresent, unsupported SID, no-routing All Present?

**PASS**

- **Routing activation (0x0005):** `test_routing_activation` — sends request, validates response code 0x10 and entity address.
- **Entity status (0x4001):** `test_entity_status` — activates routing first, validates node_type=0 and max_sockets>0.
- **Power mode (0x4003):** `test_power_mode` — activates routing first, validates mode==0x01.
- **TesterPresent (0x3E):** `test_tester_present` — activates routing, sends UDS 0x3E/0x00, validates positive response.
- **Unsupported SID (0x22):** `test_unsupported_sid` — activates routing, sends ReadDataByIdentifier, validates NRC 0x7F+0x22+0x11.
- **No-routing diagnostic:** `test_diagnostic_without_routing` — sends diagnostic WITHOUT routing activation, expects timeout/silence.

All six TCP tests present. Each TCP test creates its own `DoIPConnection` via context manager.

---

## 4. Field-Level Assertions: VIN, EID, Logical Address, Response Codes Validated?

**PASS**

Field-level assertions are thorough:

- **VIN:** Asserted in `test_udp_discovery` (line 55) and `test_vin_positive` (line 67).
- **EID:** Asserted in `test_udp_discovery` (line 56) and `test_eid_positive` (line 80).
- **Logical address:** Printed in `test_udp_discovery` (line 59); asserted as `entity == TARGET_ADDR` in `test_routing_activation` (line 90).
- **Routing response code:** `code == 0x10` in `test_routing_activation` (line 89).
- **Node type:** `node_type == 0` in `test_entity_status` (line 98).
- **Power mode:** `mode == 0x01` in `test_power_mode` (line 108).
- **NRC fields:** Three separate assertions for 0x7F, 0x22, 0x11 in `test_unsupported_sid` (lines 126-128).
- **DoIP header validation:** `parse_header()` validates version/inverse byte consistency and payload length cap on every received message.

---

## 5. Pre-flight Check Implemented?

**PASS**

Lines 489-495: Before running any tests, a UDP Vehicle ID Request probe is sent with the standard timeout. If no response is received, the script prints a clear error message ("is the server running?") to stderr and exits with code 1. This prevents confusing cascading failures when the server is not running.

---

## 6. Test Independence: Each Test Catches Its Own Errors?

**PASS**

The `TestRunner.run()` method (lines 317-332) wraps each test function in a try/except that catches both `AssertionError` and generic `Exception`. Each failure is recorded independently and does not prevent subsequent tests from running. Each TCP test creates its own `DoIPConnection` via `with` block, so a connection failure in one test does not affect others.

---

## 7. Positive AND Negative Validation in Each Test?

**PASS**

Each test includes both positive validation (expected values match) and negative/boundary validation:

| Test | Positive Check | Negative / Boundary Check |
|------|---------------|--------------------------|
| UDP discovery | VIN and EID match expected | `ann is not None` guards against no-response |
| VIN positive | VIN matches | `ann is not None` |
| VIN negative | N/A (negative test) | `ann is None` confirms silence |
| EID positive | EID matches | `ann is not None` |
| Routing | code==0x10, entity matches | `routing_activation()` raises on wrong ptype or short payload |
| Entity status | node_type==0 | max_sockets>0 validates non-degenerate response |
| Power mode | mode==0x01 | Method raises on wrong ptype or empty response |
| TesterPresent | uds[0]==0x7E via `ok` | `assert ok` catches negative response or None |
| Unsupported SID | 0x7F+0x22+0x11 all checked | `uds is not None`, `len(uds) >= 3` guards |
| No-routing | Timeout = correct behavior | Reaching `recv_doip()` without timeout triggers `assert False` |

---

## 8. CI Integration: Exit Code 0/1 Based on Results?

**PASS**

Line 511: `sys.exit(0 if success else 1)` where `success = runner.summary()` returns `self.failed == 0`. Additionally, pre-flight failure exits with code 1 (line 494), and input validation errors (bad port, bad EID format) also exit with code 1.

---

## 9. Expected Values Configurable via --vin/--eid?

**PASS**

CLI arguments implemented via argparse:
- `--vin` (default: `APTERADOIPSRV0001`) — line 461
- `--eid` (default: `00:1A:2B:3C:4D:5E`) — line 463
- `-H`/`--host` (default: `127.0.0.1`) — line 457
- `-p`/`--port` (default: 13400, validated 1-65535) — line 459
- `-v`/`--verbose` — line 465

EID format is validated at startup (lines 475-479). All expected values flow into `make_tests()` as parameters.

---

## 10. Sample Output Matches Plan?

**PASS**

The output structure matches the plan's specified format:
- `=== DoIP Server Test Suite ===` header with `Server:` line (lines 484-486)
- `--- UDP Discovery ---` section header (line 501)
- `--- TCP Queries ---` section header (line 505)
- `[PASS]`/`[FAIL]` prefix on each test result
- Detail lines (VIN, Address, EID, GID) printed for test 1
- `=== Results: N passed, M failed ===` summary line (line 336)

The format matches the plan's sample output section exactly.

---

## Additional Observations

**Implementation quality notes (not plan deviations):**

1. **DoIP header version for TCP sends:** `DoIPConnection.send_doip()` defaults to `version=DOIP_VERSION` (0x03) for TCP, while `build_doip()` defaults to `DOIP_VERSION_DISCOVERY` (0xFF). This is correct — ISO 13400 uses 0xFF for discovery requests and a negotiated version for TCP.

2. **Two-phase diagnostic read:** `send_diagnostic()` correctly handles ACK (0x8002) then response (0x8001), and also handles NACK (0x8003) as an alternative to ACK. This matches the plan's design decision #6.

3. **recv_exact() loop:** Properly loops on `recv()` until exactly N bytes received, with connection-closed detection. Matches design decision #7.

4. **Context manager cleanup:** `DoIPConnection.__exit__` calls `close()` which swallows `OSError` on socket close. Each test uses `with` to guarantee cleanup. Matches design decision #11.

5. **Shebang present:** `#!/usr/bin/env python3` on line 1. Plan requires `chmod +x` separately.

6. **No external dependencies:** Only stdlib imports (`argparse`, `socket`, `struct`, `sys`, `time`). Matches design decision #1.

---

## Summary

| # | Check | Verdict |
|---|-------|---------|
| 1 | All 10 planned tests implemented | PASS |
| 2 | UDP: broadcast, VIN+, VIN-, EID+ | PASS |
| 3 | TCP: routing, entity, power, tester, unsupported, no-routing | PASS |
| 4 | Field-level assertions | PASS |
| 5 | Pre-flight check | PASS |
| 6 | Test independence | PASS |
| 7 | Positive AND negative validation | PASS |
| 8 | CI integration (exit codes) | PASS |
| 9 | Expected values configurable | PASS |
| 10 | Sample output matches plan | PASS |

## Overall Verdict: PASS

All 10 review criteria pass. The implementation faithfully follows the plan, incorporates all Round 1 review fixes, and provides a complete, independent, CI-ready test suite with no external dependencies.
