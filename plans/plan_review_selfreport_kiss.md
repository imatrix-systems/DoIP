# KISS Review: Unify All Uploads to selfreport URI (Round 2)

**Reviewer:** KISS (Keep It Simple, Stupid)
**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` v2.0
**Date:** 2026-03-17

---

## Verdict: PASS

---

## Round 1 Findings — Resolution Check

### 1. CRITICAL: Change 1 targets APPLIANCE_GATEWAY-only code
**Status: FIXED.** Change 1 removed entirely. Plan now correctly states app upload is `#ifdef APPLIANCE_GATEWAY` only (line 49) and lists it as out of scope (line 200).

### 2. CRITICAL: WINDOW_UNLOCK() deadlock on return false
**Status: FIXED.** The "After" code at plan line 95-96 shows `WINDOW_UNLOCK(); return false;` matching the existing pattern at lines 1192, 1422, 1734, and 1849 of `imx_upload_window.c`.

### 3. MAJOR: Change 3 was vague
**Status: FIXED.** Change 3 removed. The plan now has a single change with the mutex and return behavior fully resolved inline.

### 4. MINOR: TOCTOU — two calls to get_can_serial_no()
**Status: FIXED.** Plan moves `get_can_serial_no()` to a single `uint32_t can_sn` local variable used in both the serial number snprintf (plan line 125-126) and the CoAP header setup (plan line 132-134). This eliminates the race between lines 1209 and 1238 of the source.

### 5. MINOR: Misleading "App upload" table row
**Status: FIXED.** The "Current Upload Path Summary" table at plan line 40 is now scoped to "CAN_PLATFORM only" and no longer includes an app upload row.

### 6. MINOR: Disproportionate verification plan
**Status: IMPROVED.** Verification reduced from 4 steps (~80 lines) to 2 steps (~30 lines). The remaining steps (deploy, check logs, query API) are proportional to a single-file change that affects production uploads.

---

## Re-Review of v2.0

### Correctness

1. **Single file, single change:** The plan modifies only `iMatrix/imatrix_upload/imx_upload_window.c`. Two edits in the same function: (a) move `can_sn` declaration earlier to fix TOCTOU, (b) replace `tsd/` fallback with `WINDOW_UNLOCK(); return false;`. This is minimal and correct.

2. **Mutex safety verified:** I confirmed the existing `WINDOW_UNLOCK(); return false;` pattern at 5 locations in the same file (lines 337-338, 1192-1193, 1422-1423, 1734-1735, 1849-1850). The proposed change matches this pattern exactly.

3. **Data safety claim verified:** The plan states ring buffer read pointers are not advanced until after successful packet build (plan line 146). This is correct -- `imx_read_bulk_samples()` is called later in the function (after the CoAP header setup), so returning false at line ~1245 means no data is consumed.

4. **Variable scoping for TOCTOU fix:** The plan proposes declaring `uint32_t can_sn = get_can_serial_no();` before the serial number block but inside `#ifdef CAN_PLATFORM`. Since both the serial number block (lines 1199-1210) and CoAP header block (lines 1228-1247) are in separate `#ifdef CAN_PLATFORM` regions, the variable must be declared at function scope or both `#ifdef` blocks must be merged. The plan's pseudocode (lines 117-141) is slightly ambiguous on placement -- it shows the declaration before the `#ifdef` boundary. The implementer should declare `can_sn` inside the first `#ifdef CAN_PLATFORM` block (before line 1200), which keeps it in function scope when `CAN_PLATFORM` is defined, accessible to the second `#ifdef CAN_PLATFORM` block. This is a minor implementation detail, not a plan defect.

### Completeness

5. **Out-of-scope items documented:** BLE `tsd/` usage (line 51), APPLIANCE_GATEWAY app upload (line 200), and the `COAP_PUT_ENDPOINT_TSD_UPSTREAM` macro (line 202) are all correctly noted as out of scope with rationale.

6. **No missing steps.** No migrations, no config changes, no new dependencies. Single C source file edit.

### Simplicity

7. **Plan structure is now proportional.** One file in the "Affected Files" table. One logical change (suppress SN=0 uploads instead of falling back to tsd/). The TOCTOU fix is a sensible secondary cleanup in the same code region.

---

## Remaining Notes

- **No issues found.** All Round 1 findings are resolved. The plan is minimal, correct, and ready for implementation.
