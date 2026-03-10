# Security Review — Round 2

**Reviewed:** `test/scripts/plan_doip_test_script.md`
**Reviewer:** Security Agent
**Date:** 2026-03-09

---

## Round 1 Findings Re-verification

### S-1: Port range validation (1-65535) via argparse

**Round 1 Verdict:** FAIL
**Round 2 Verdict: PASS**

The updated plan specifies port validation in multiple locations:
- Design decision #10: "argparse validates port in range 1-65535"
- CLI table lists `--port` / `-p` with default 13400
- Round 1 resolution table entry S-1 confirms the fix
- Step 6 of implementation steps covers "CLI arg parsing (argparse with validation)"

argparse `type=int` with a range-checking function is the standard Python approach. The plan is clear that invalid ports are rejected at parse time before any network operations occur.

### S-2: recv buffer cap at 4096 bytes with oversized length rejection

**Round 1 Verdict:** FAIL
**Round 2 Verdict: PASS**

The updated plan specifies the recv buffer cap in multiple locations:
- Design decision #9: "reject any DoIP message with length > 4096 bytes"
- Design decision #8: "sane payload length (<=4096)" as part of header validation
- Round 1 resolution table entry S-2 confirms the fix
- `parse_header` in the structure section includes "with validation"

The 4096-byte cap prevents memory exhaustion from malformed or malicious DoIP headers claiming very large payload lengths. The cap is applied at header parse time, before allocating or reading the payload body. The largest expected legitimate response is a Vehicle Announcement at 41 bytes (8 header + 33 payload), so 4096 provides ample headroom without risk.

---

## Regression Check

No regressions found. Items verified against the updated plan:

1. **Config parser removed (KISS)** — The R1 conditional passes on config file parsing (#2) and file path handling (#8) are no longer applicable. The plan now uses `--vin`/`--eid` CLI flags instead, eliminating the config file attack surface entirely. This is a security improvement.

2. **No unbounded reads** — `recv_exact()` reads a fixed number of bytes determined by the validated header length. The 4096-byte cap ensures this is bounded.

3. **No raw user input in dangerous contexts** — Host and port go through argparse; VIN is a 17-char ASCII string; EID is parsed from colon-separated hex. None are passed to shell commands, eval, or format strings that could cause injection.

4. **Socket timeouts specified** — 2s UDP, 5s TCP. No risk of indefinite blocking on recv.

5. **Context manager cleanup** — Sockets cleaned up via `__enter__`/`__exit__`, preventing FD leaks on exception paths.

6. **No secrets or credentials** — The script handles no sensitive data.

7. **No retry loops** — Each test runs once; failure is caught and reported. No unbounded retry risk.

8. **Broadcast safety** — Single UDP packet per discovery test, not retried in a loop.

9. **Dependencies remain stdlib-only** — No new imports introduced.

---

## Summary

| # | R1 Finding | R1 Verdict | R2 Verdict |
|---|-----------|------------|------------|
| S-1 | Port range validation (1-65535) | FAIL | **PASS** |
| S-2 | recv buffer cap (4096 bytes) | FAIL | **PASS** |

## Overall Verdict: PASS

Both R1 security findings have been properly addressed in the updated plan. No regressions or new security concerns identified. The removal of the config file parser (KISS fix) also eliminated two conditional-pass items from R1, improving the overall security posture.
