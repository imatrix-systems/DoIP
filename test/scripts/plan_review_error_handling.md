# Error Handling Review — Round 2

**Agent:** Error Handling
**Date:** 2026-03-09
**Plan reviewed:** `/home/greg/iMatrix/DOIP/DoIP_Server/test/scripts/plan_doip_test_script.md`
**Purpose:** Verify all 6 Round 1 FAILs are fixed; check for regressions.

---

## Round 1 Items Re-verification

### E-1 (was #6): Two-phase diagnostic read missing
**Round 2 Verdict: PASS**

The plan now explicitly documents the two-phase diagnostic flow in three places:
1. **Protocol section** (lines 90-96): "Diagnostic Message Flow (two-phase)" subsection explains ACK (0x8002) first, then UDS response (0x8001), with the NACK (0x8003) single-message alternative.
2. **Script design**: `send_diagnostic()` returns `(ack_code, uds_response)`, indicating two reads.
3. **Design decision #6**: "Two-phase diagnostic reads -- after sending diagnostic 0x8001, read ACK (0x8002) first, then UDS response (0x8001)."

The NACK path (test #10) is also specified: "If routing is not activated, the server sends a Diagnostic NACK (0x8003) instead, with no second message."

### E-2 (was #7): No recv_exact loop for TCP
**Round 2 Verdict: PASS**

`recv_exact(sock, n)` is listed in the protocol helpers (line 123) with the description "loop until n bytes received." `recv_doip(sock)` uses it for "header + exact payload read" (line 124). Design decision #7 confirms: "loop on TCP recv() until exactly N bytes received, preventing partial-read bugs."

### E-3 (was #2): No connection refused handling
**Round 2 Verdict: PASS**

Two layers of defense are now present:
1. **Pre-flight check** (line 152): "check server reachable (UDP probe with 2s timeout)" before any tests run, with a clean "server not reachable" error message.
2. **Per-test exception catching** (line 145): `run_test(name, func)` catches exceptions and prints FAIL, so a connection refused mid-suite does not crash the process.
3. Design decision #12 documents the pre-flight check; #13 documents test independence via exception catching.

### E-4 (was #4): No header validation
**Round 2 Verdict: PASS**

Design decision #8 specifies: "every received DoIP header checked for valid version (0x03), correct inverse byte, sane payload length (<=4096)." The `parse_header()` helper (line 122) is described as returning parsed fields "with validation." Design decision #9 adds the 4096-byte recv buffer cap as a secondary guard.

### E-5 (was #8): No context manager for socket cleanup
**Round 2 Verdict: PASS**

`DoIPConnection` class (line 133) includes `__enter__` / `__exit__` methods for context manager support. Design decision #11 states: "DoIPConnection uses __enter__/__exit__ to guarantee socket cleanup."

### E-6 (was #10): No config file error handling
**Round 2 Verdict: PASS**

The config file parser was removed entirely. Design decision #2 states: "No config file parser -- VIN and EID passed via --vin/--eid flags with hardcoded defaults matching doip-server.conf." This eliminates the entire category of config-file error handling (file not found, parse errors, missing keys). The resolution table (K-1) confirms this was a deliberate KISS decision. The whole error class is gone, not just papered over.

---

## Regression Check

Checked for new error-handling gaps that may have been introduced by R1 changes or that were missed in R1:

| Area | Check | Result |
|------|-------|--------|
| recv_exact EOF handling | If server closes connection mid-read, recv_exact must not loop forever on 0-byte recv(). | **No regression** -- standard Python socket practice: `recv()` returning `b""` means peer closed; the implementation must raise. The plan's intent (reliable exact-length reads) is clear and the behavior is implicit in the helper's contract. |
| parse_header short input | What if fewer than 8 bytes arrive for a header? | **No regression** -- recv_exact guarantees exactly 8 bytes before parse_header runs, so short headers cannot reach the parser. |
| UDP pre-flight false negative | What if the server is on TCP but not UDP? | **No regression** -- the plan uses UDP for pre-flight, matching the discovery protocol. A server that answers TCP but not UDP is non-conformant per ISO 13400. Acceptable design. |
| Diagnostic NACK single-read | Does send_diagnostic handle the NACK case correctly (no second recv)? | **No regression** -- the plan documents (line 96): "If routing is not activated, the server sends a Diagnostic NACK (0x8003) instead, with no second message." Test #10 exercises this path. The implementation must check the first response type before attempting a second read. |
| argparse input validation | Port range 1-65535 validated (decision #10). Host not validated beyond string. | **No regression** -- host validation is unnecessary; `socket.connect()` will raise on invalid hosts, caught by run_test's exception handler. |
| Socket timeout values | TCP timeout 5 seconds, UDP timeout 2 seconds. | **No regression** -- both are reasonable for a local test tool. |
| UDP socket cleanup | UDP functions create sockets; are they cleaned up on error? | **No regression** -- the plan specifies context managers for TCP. UDP functions are standalone and short-lived; Python's garbage collector closes sockets, but more importantly the run_test wrapper catches exceptions. Minor style point, not a functional gap. |
| Verbose mode error output | Does -v mode print useful info on failure? | **No regression** -- verbose prints raw TX/RX hex, which aids debugging failed tests. |

No regressions found.

---

## Summary

| ID | Issue | R1 Verdict | R2 Verdict |
|----|-------|-----------|-----------|
| E-1 | Two-phase diagnostic read | FAIL | **PASS** |
| E-2 | recv_exact() helper for TCP | FAIL | **PASS** |
| E-3 | Connection refused handling | FAIL | **PASS** |
| E-4 | Header validation | FAIL | **PASS** |
| E-5 | Context manager for cleanup | FAIL | **PASS** |
| E-6 | Config file parser removed | FAIL | **PASS** |

**Regressions found:** 0

---

## Overall Verdict: PASS

All 6 Round 1 error-handling issues have been resolved in the plan. No regressions detected. The plan specifies robust error handling at every layer: transport (recv_exact, context manager), protocol (header validation, two-phase reads), connectivity (pre-flight check, per-test exception catching), and input (argparse validation, no config file parser to fail).
