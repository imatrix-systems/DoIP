# Protocol Correctness Review — Round 3

**Reviewer:** Protocol Correctness Agent
**Date:** 2026-03-09
**Scope:** Verify R2 finding (test #10 NACK vs silence) is fixed; check for regressions.
**Cross-referenced:** `src/doip_server.c` lines 220-222, `test/scripts/plan_doip_test_script.md`

---

## R2 Fix Verification

### R-1: Test #10 expects silence/timeout instead of NACK — PASS (partial)

**R2 issue:** Test #10 expected a Diagnostic NACK (0x8003) but the server silently drops messages when routing is not activated (`doip_server.c:220-222`).

**Verification — test table and sample output:**

| Check | Location | Content | Verdict |
|-------|----------|---------|---------|
| Test table (test #10) | Line 31 | "Silence (server drops message — expect recv timeout)" | **PASS** |
| Sample output | Line 206 | "[PASS] Diagnostic without routing (silence — timeout)" | **PASS** |
| Resolution table entry | Line 270 (P-R1) | "Changed to expect silence/timeout" | **PASS** |
| Server source | `doip_server.c:220-222` | `break` with no NACK sent when `!conn->routing_activated` | **Confirmed** |

All four checks confirm the test #10 fix was applied correctly in the table, output, and resolution log.

### R-1b: Diagnostic Message Flow prose — FAIL

**Issue:** The "Diagnostic Message Flow" prose at line 96 was NOT updated to match the fix. It still reads:

> If routing is not activated, the server sends a **Diagnostic NACK** (0x8003) instead, with no second message.

This contradicts:
- The server source code (silent drop, no NACK)
- The corrected test #10 table entry (silence/timeout)
- The corrected sample output (silence — timeout)

**Fix required:** Change line 96 to: "If routing is not activated, the server silently drops the message (no response is sent)."

---

## Regression Check

| Area | Check | Verdict |
|------|-------|---------|
| UDP tests 1-4 | Table descriptions consistent with protocol details and sample output | **PASS** |
| TCP tests 5-7 | Routing activation, entity status, power mode — descriptions match protocol layouts | **PASS** |
| TCP tests 8-9 | TesterPresent and unsupported SID — two-phase diagnostic flow correctly described | **PASS** |
| Header format | Version 0x03 for responses, 0xFF for requests — consistent throughout document | **PASS** |
| Design decisions 1-16 | All internally consistent, no contradictions | **PASS** |
| CLI interface | Flags, defaults, and description consistent with test descriptions | **PASS** |
| Resolution table | All 21 R1 entries + 1 R2 entry (P-R1) present and coherent | **PASS** |

No regressions found.

---

## Summary

| ID | Item | Verdict |
|----|------|---------|
| R-1 | Test #10 table entry: silence/timeout | PASS |
| R-1 | Test #10 sample output: silence/timeout | PASS |
| R-1 | Resolution table documents the fix | PASS |
| R-1b | Diagnostic Message Flow prose (line 96) still says NACK | **FAIL** |
| Regr | UDP tests 1-4 | PASS |
| Regr | TCP tests 5-9 | PASS |
| Regr | Header/version consistency | PASS |
| Regr | Design decisions | PASS |
| Regr | CLI interface | PASS |
| Regr | Resolution table completeness | PASS |

**Totals: 9 PASS, 1 FAIL**

---

## Overall Verdict: FAIL — 1 issue found

The R2 fix for test #10 was correctly applied in the test table (line 31), sample output (line 206), and resolution log (line 270). However, the "Diagnostic Message Flow" prose description at line 96 was not updated and still incorrectly states the server sends a NACK when routing is not activated. The server source (`doip_server.c:220-222`) confirms it silently drops the message. This prose must be corrected for internal consistency.
