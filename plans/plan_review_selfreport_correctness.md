# Plan Review: Unify All Uploads to selfreport URI

**Reviewer:** Correctness
**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` v2.0
**Round:** 2
**Date:** 2026-03-17

---

## Verdict: PASS

All Round 1 findings have been addressed. The plan is correct and complete.

---

## Round 1 Findings -- Resolution Status

### 1. CRITICAL: WINDOW_UNLOCK deadlock -- FIXED

v2.0 now includes `WINDOW_UNLOCK();` before `return false;` in the SN=0 branch (plan lines 95-96). This matches the established pattern used at every other early-return path in `imx_upload_window_build_packet()`: lines 1192, 1422, 1734, 1849 of `iMatrix/imatrix_upload/imx_upload_window.c`.

### 2. CRITICAL: Change 1 targets wrong platform -- FIXED

v2.0 removed Change 1 entirely. `imx_app_get_packet()` at `imatrix_upload.c:1801` is inside `#ifdef APPLIANCE_GATEWAY` (lines 1769-1933), so it is not compiled on CAN_PLATFORM. Correctly marked as out of scope.

### 3. MAJOR: Change 3 was vague -- FIXED

Removed. The plan now has a single change with explicit before/after code blocks.

### 4. MAJOR: BLE tsd/ usage -- ADDRESSED

Noted as out of scope (plan line 51). Verified: `imx_measurments_upload.c` uses `imx_setup_coap_tsd_packet_header()` (a different function entirely from `imx_setup_coap_sync_packet_header_for_tsd_upload()`). BLE is a separate subsystem.

### 5. MINOR: TOCTOU -- FIXED

v2.0 moves `uint32_t can_sn = get_can_serial_no()` before the serial number block and reuses it in both the `snprintf` at line 1208 and the CoAP header setup at line 1238. Eliminates the race between two separate calls to `get_can_serial_no()`.

---

## Re-verification of Plan Correctness

### 1. Is the single change (WINDOW_UNLOCK + return false) correct?

Yes. Verified against the actual code at `imx_upload_window.c:1233-1247`:

- The function acquires `WINDOW_LOCK()` at line 1183.
- Every existing early-return calls `WINDOW_UNLOCK()` first (confirmed at lines 1192, 1422, 1734, 1849).
- The proposed `WINDOW_UNLOCK(); return false;` follows this pattern exactly.
- The caller at `imatrix_upload.c:1282-1285` handles `false` by calling `imx_upload_window_free_slot()`, which releases the message buffer. No leak.
- Returning `false` before `imx_read_bulk_samples()` means ring buffer read pointers are not advanced. Data is retained for the next upload cycle. No data loss.

### 2. Is the TOCTOU fix (single can_sn variable) correct?

Yes. The two `#ifdef CAN_PLATFORM` blocks (lines 1199-1210 and 1228-1247) are in the same function scope after preprocessing. Declaring `can_sn` before the first block makes it visible in both. The plan's pseudo-code omits the `#ifdef` directives, but the intent is clear: the declaration goes inside `#ifdef CAN_PLATFORM`, before the `if (contents->upload_source == IMX_UPLOAD_GATEWAY)` check at line 1200, and the same variable is referenced again at line 1238's block. This eliminates the window where `get_can_serial_no()` could return different values between the two calls.

One implementation note: `can_sn` must be declared inside the `#ifdef CAN_PLATFORM` guard (not before it) to avoid an unused-variable warning on APPLIANCE_GATEWAY and default builds. The plan's pseudo-code is ambiguous on this point, but any implementer following the plan would naturally place it inside the CAN_PLATFORM block.

### 3. Are there remaining paths where tsd/ is used on CAN_PLATFORM?

No. Exhaustive search of `imx_setup_coap_sync_packet_header_for_tsd_upload` call sites:

| File:Line | Context | Compiled on CAN_PLATFORM? |
|-----------|---------|---------------------------|
| `imx_upload_window.c:1245` | `#ifdef CAN_PLATFORM`, SN=0 fallback | Yes -- this is the line being changed |
| `imx_upload_window.c:1251` | `#elif defined(APPLIANCE_GATEWAY)` | No |
| `imatrix_upload.c:1835` | `#ifdef APPLIANCE_GATEWAY` (lines 1769-1933) | No |

After the change, zero CAN_PLATFORM paths will use `tsd/`.

### 4. WINDOW_LOCK/UNLOCK pattern verification

Verified the complete lock/unlock pattern in `imx_upload_window_build_packet()`:

- Lock acquired: line 1183 (`WINDOW_LOCK()`)
- Unlock + return false: lines 1192-1193 (null msg check)
- Unlock + return false: lines 1422-1423 (build failure)
- Unlock + return false: lines 1734-1735 (build failure)
- Unlock + return false: lines 1849-1850 (build failure)
- Unlock + return true: lines 1864-1865 (success path)

The proposed change adds a new `WINDOW_UNLOCK(); return false;` at line 1244, which is consistent with all existing paths.

---

## Minor Observations (not blocking)

1. **can_sn declaration placement ambiguity** -- The plan should specify that `can_sn` goes inside the `#ifdef CAN_PLATFORM` block to avoid unused-variable warnings on other platform builds. The pseudo-code at plan lines 117-141 does not show `#ifdef` directives. This is cosmetic and any implementer would handle it correctly.

2. **Dead macro** -- `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` at `imx_coap_message_composer_utility.c:882` remains dead code. Not in scope for this change but noted for future cleanup.

---

## Summary

| Round 1 Finding | Severity | Status in v2.0 |
|----------------|----------|-----------------|
| WINDOW_UNLOCK deadlock | CRITICAL | FIXED |
| Change 1 wrong platform | CRITICAL | FIXED (removed) |
| Change 3 vague | MAJOR | FIXED (removed) |
| BLE tsd/ usage | MAJOR | ADDRESSED (out of scope) |
| TOCTOU double call | MINOR | FIXED |
| Dead macro cleanup | MINOR | Acknowledged, deferred |

All critical and major findings resolved. Plan is correct and safe to implement.
