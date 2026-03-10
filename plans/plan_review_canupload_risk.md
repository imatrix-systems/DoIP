# CAN Upload Selfreport Plan Review: Risk -- Round 2

## Review Date: 2026-03-09
## Plan Version: 2.0
## Overall: PASS

---

### Round 1 Re-verification

#### Finding 1: SN=0 Race Window During Re-Registration [was MEDIUM]

**Round 1 Issue:** `canbus_registered()` can return true while `can_controller_sn` is 0 during re-registration. The original plan had no guard, so `imx_setup_coap_sync_packet_header_for_can(msg, 0)` could produce URI `selfreport/<mfr>/0000000000/0`.

**Round 1 Mitigation Requested:** Add `get_can_serial_no() != 0` guard before calling `_for_can()`.

**v2.0 Fix:** Plan now proposes (Section "Required Dispatch", lines 58-65 and Section "Step 1", lines 102-109):
```c
else if (get_can_serial_no() != 0)
{
    imx_setup_coap_sync_packet_header_for_can(msg, get_can_serial_no());
}
else
{
    imx_setup_coap_sync_packet_header_for_tsd_upload(msg);
}
```

The guard is present. The fallback to `tsd/` is a reasonable safe default -- it preserves pre-existing behavior when SN is unavailable. The plan also documents this in the edge case testing table (line 165-166).

**Minor note:** `get_can_serial_no()` is called twice -- once for the check and once for the argument. This is a TOCTOU (time-of-check-time-of-use) gap where the SN could theoretically change between calls. In practice, this is not a real risk: the SN only transitions from 0 to valid (never valid to different-valid in a single re-registration cycle), so the worst case is entering the `else if` branch with a now-valid SN (harmless) or entering with 0 if it was cleared between calls (falls through to `_for_can(msg, 0)` which produces the same bad URI the guard was meant to prevent). The likelihood is vanishingly small since both calls execute in nanoseconds, but a local variable would eliminate it entirely:

```c
uint32_t can_sn = get_can_serial_no();
if (can_sn != 0) {
    imx_setup_coap_sync_packet_header_for_can(msg, can_sn);
} else {
    imx_setup_coap_sync_packet_header_for_tsd_upload(msg);
}
```

This is a style improvement, not a blocking issue.

**Verdict: PASS**

---

#### Finding 2: No Data Loss During URI Transition [was LOW]

**Round 1 Conclusion:** No risk -- CoAP URI is constructed at send time, not enqueue time.

**v2.0 Status:** Plan unchanged on this point. The analysis remains correct.

**Verdict: PASS** (no fix needed)

---

#### Finding 3: Server-Side Compatibility [was MEDIUM]

**Round 1 Issue:** "The server should already handle it" -- the word "should" was doing heavy lifting.

**v2.0 Status:** Plan text at line 136 still says "the server should already handle it." The wording has not changed, but the fallback to `tsd/` when SN=0 provides some safety. The testing strategy (lines 156-159) includes runtime verification against a real server, and the edge case table includes fallback behavior.

The plan does not add an explicit staging-server validation step as Round 1 recommended. This remains an operational concern but is not a plan defect -- it is a deployment process item.

**Verdict: PASS** (operational concern acknowledged, not a plan defect)

---

#### Finding 4: Rollback Strategy [was LOW]

**Round 1 Conclusion:** Rollback is sound -- single line change, no persistent state changes.

**v2.0 Status:** The change is now 3-5 lines instead of 1, but the same rollback analysis applies. Reverting the `else if / else` back to plain `else` with `_for_tsd_upload()` restores previous behavior. No persistent state changes. No server coupling.

**Verdict: PASS** (no fix needed)

---

#### Finding 5: OTA and Configuration Sync Impact [was LOW]

**Round 1 Conclusion:** No impact -- change is scoped to `#ifdef CAN_PLATFORM` upload dispatch.

**v2.0 Status:** Unchanged. Still correct.

**Verdict: PASS** (no fix needed)

---

#### Finding 6: Blast Radius Assessment [was LOW-MEDIUM]

**Round 1 Conclusion:** Blast radius limited to CAN sensor data uploads on CAN_PLATFORM builds.

**v2.0 Status:** With the SN=0 guard and tsd/ fallback, the blast radius is further reduced. The SN=0 case now degrades gracefully to previous behavior rather than sending to an invalid endpoint. The blast radius table should be updated to reflect this improvement, but the analysis is correct.

**Verdict: PASS**

---

#### Finding 7: `IMX_UPLOAD_HOSTED_DEVICE` Also Goes Through `else` Branch [was LOW]

**Round 1 Issue:** Plan did not acknowledge that `IMX_UPLOAD_HOSTED_DEVICE` is also affected by the change.

**v2.0 Fix:** Plan now includes a "Pre-existing Issues" table (lines 183-187) that explicitly documents: "The `else` branch covers both `IMX_UPLOAD_CAN_DEVICE` and `IMX_UPLOAD_HOSTED_DEVICE`." The table explains that `get_host_serial_no()` returns the FC-1 device serial on that platform, and that this is pre-existing behavior not introduced by this change.

**Verified in source:** Lines 1205-1206 of `imx_upload_window.c` confirm both `IMX_UPLOAD_CAN_DEVICE` and `IMX_UPLOAD_HOSTED_DEVICE` enter the same branch for payload SN. The proposed change applies the same `_for_can()` URI to both, which is consistent with the existing payload behavior.

**Verdict: PASS**

---

### Testing Gaps (Round 1 Re-check)

#### Gap 1: No test for SN=0 edge case

**v2.0 Status:** The edge case table (lines 165-166) now includes two SN=0 scenarios: "CAN controller SN = 0 (not registered)" and "CAN controller SN = 0 during re-registration", both with expected behavior "Falls back to `tsd/` URI." This is documented but remains a runtime-only verification. No automated test is proposed.

**Assessment:** Acceptable for a 3-5 line change. The SN=0 path falls through to existing, proven code (`_for_tsd_upload`). An automated test would require mocking `get_can_serial_no()` which may not be practical in this embedded codebase.

#### Gap 2: No server-side integration test

**v2.0 Status:** Still relies on runtime verification (line 156-159). This is an operational gap, not a plan gap.

#### Gap 3: No test for `IMX_UPLOAD_HOSTED_DEVICE` path

**v2.0 Status:** Now documented as pre-existing behavior. The runtime verification step should cover this implicitly if hosted devices are active on the test unit.

#### Gap 4: No negative test for server rejection

**v2.0 Status:** Not addressed. The upload retry/error handling is pre-existing CoAP infrastructure and not changed by this plan. Not a blocking concern.

---

### Pre-existing Issues Documented in v2.0

The plan now includes a "Pre-existing Issues (Not In Scope)" table with three items:

1. `HOSTED_DEVICE` serial number behavior -- documented, not introduced by this change.
2. Dead macro `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` -- deferred cleanup.
3. `imx_save_can_controller_sn(0)` setting `can_controller_registered = true` -- root cause of the race, mitigated by the SN=0 guard.

Verified at `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_utils.c:1545-1549`: `imx_save_can_controller_sn()` unconditionally sets `can_controller_registered = true` regardless of SN value. The plan's guard is the correct mitigation at the call site.

---

### New Observation (Round 2 Only)

**Double call to `get_can_serial_no()` -- TOCTOU [LOW]**

The proposed code calls `get_can_serial_no()` twice: once in the condition and once as the argument. Both calls read `device_config.canbus.can_controller_sn` (via `imx_get_can_controller_sn()` at `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_utils.c:1568-1570`). Since `device_config` can be modified by the CAN registration thread, there is a theoretical window where the value changes between the two reads.

Practical impact is negligible (nanosecond window, and the only dangerous transition is valid-to-zero which only happens during re-registration), but using a local variable is trivially better. This is a style recommendation, not a blocking finding.

---

### Rollback Assessment

The rollback plan is sound. The change is a 3-5 line modification in one file with no persistent state changes. Reverting restores exact previous behavior. The `tsd/` fallback path is itself a form of graceful degradation that reduces rollback urgency.

---

### Summary

All 7 Round 1 findings have been addressed in v2.0:

| # | Finding | Round 1 | v2.0 Status | Round 2 |
|---|---------|---------|-------------|---------|
| 1 | SN=0 race window | MEDIUM | Guard added with tsd/ fallback | PASS |
| 2 | Data loss during URI transition | LOW | No change needed | PASS |
| 3 | Server-side compatibility | MEDIUM | Operational concern, not plan defect | PASS |
| 4 | Rollback strategy | LOW | Still sound | PASS |
| 5 | OTA/config sync impact | LOW | No change needed | PASS |
| 6 | Blast radius | LOW-MEDIUM | Reduced by fallback | PASS |
| 7 | HOSTED_DEVICE in else branch | LOW | Documented as pre-existing | PASS |

**One non-blocking recommendation:** Use a local variable for `get_can_serial_no()` to eliminate the double-call TOCTOU gap. This is a style improvement, not a correctness issue.

**Risk Level: LOW.** The v2.0 plan addresses all Round 1 concerns. The SN=0 guard with tsd/ fallback is the right approach -- it defends against the race condition while preserving backward compatibility. The change is minimal, well-scoped, and safely reversible.
