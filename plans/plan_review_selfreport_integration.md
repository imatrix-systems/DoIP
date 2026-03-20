# Integration Review (Round 2): Unify All Uploads to selfreport URI

**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` v2.0
**Reviewer:** Integration
**Round:** 2 (re-review after v1.0 fixes)
**Date:** 2026-03-17

---

## Correctness & Completeness Review

### Verdict: PASS

---

## Round 1 Findings — Resolution Status

All 5 findings from Round 1 have been addressed:

| # | Finding | Severity | Status | How Resolved |
|---|---------|----------|--------|--------------|
| 1 | WINDOW_UNLOCK() deadlock — return false without releasing mutex | CRITICAL | FIXED | Plan v2.0 line 95-96: `WINDOW_UNLOCK(); return false;` added. Matches existing pattern at lines 1192, 1422-1423, 1849-1850 of `imx_upload_window.c`. |
| 2 | Change 1 targets APPLIANCE_GATEWAY, not CAN_PLATFORM | CRITICAL | FIXED | Change 1 removed entirely (plan v2.0 line 22). Plan now correctly notes at line 49 that `imx_app_get_packet` is `#ifdef APPLIANCE_GATEWAY` only. |
| 3 | Change 3 was vague | MAJOR | FIXED | Folded into Change 1 (plan v2.0 line 24). Caller behavior verified — see below. |
| 4 | BLE `tsd/` usage not addressed | MAJOR | FIXED | Explicitly noted as out of scope at plan v2.0 lines 51, 198-202. BLE measurements are a separate concern from CAN_PLATFORM uploads. |
| 5 | TOCTOU — two calls to `get_can_serial_no()` | MINOR | FIXED | Plan v2.0 lines 101-142: single `can_sn` local variable declared before both usage sites (serial number header and CoAP URI setup). |

---

## Re-verification of Key Concerns

### 1. Wrong-platform issue (Change 1 APPLIANCE_GATEWAY) — RESOLVED

Confirmed in source: `imx_app_get_packet` is defined at `iMatrix/imatrix_upload/imatrix_upload.c` inside `#ifdef APPLIANCE_GATEWAY` (line 1769). The plan correctly removed this change and documents it at lines 22, 49, and 200.

### 2. Deadlock (WINDOW_UNLOCK before return) — RESOLVED

Confirmed in source at `iMatrix/imatrix_upload/imx_upload_window.c`:
- `WINDOW_LOCK()` acquired at line 1183 (non-recursive `pthread_mutex_lock` per line 49)
- All existing `return false` paths call `WINDOW_UNLOCK()` first: lines 1192, 1422-1423, 1849-1850
- Plan v2.0 correctly adds `WINDOW_UNLOCK()` before `return false` at the SN=0 guard
- Caller at `imatrix_upload.c:1285` calls `imx_upload_window_free_slot()` which takes `WINDOW_LOCK()` at line 274 — deadlock would occur without the unlock. Now safe.

### 3. BLE `tsd/` usage — PROPERLY SCOPED OUT

Confirmed: `iMatrix/ble/imx_measurments_upload.c:200` uses `tsd/` but this is BLE/Wirepas-only code. The plan's scope is CAN_PLATFORM uploads. Documented at plan lines 51, 198-202.

### 4. Caller handles `false` return correctly — VERIFIED

At `iMatrix/imatrix_upload/imatrix_upload.c` lines 1282-1296:
```c
if (!imx_upload_window_build_packet(sw_slot_idx, current_time))
{
    consecutive_build_failures++;
    imx_upload_window_free_slot(sw_slot_idx);
    ...
    if (consecutive_build_failures >= IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES)
    {
        break;  /* Exit loop to trigger stall detection and source rotation */
    }
    continue;  /* Try next iteration */
}
```

- `imx_upload_window_free_slot()` at line 1285 releases the message buffer via `msg_release()` (confirmed at `imx_upload_window.c:284`). No memory leak.
- `consecutive_build_failures` is incremented at line 1284. After `IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES`, the source is rotated away. This is acceptable because:
  - The existing guard at `imatrix_upload.c:797` (`imx_is_can_ctrl_sn_valid()`) prevents rotation TO HOSTED_DEVICE when SN=0.
  - However, `canbus_registered()` (lines 814, 831) checks `can_controller_registered` which CAN be true even when SN=0 (per pre-existing issue noted in MEMORY.md). This means CAN_DEVICE could still be selected as an upload source when SN=0, and the SN=0 guard would trigger `return false`. This is a pre-existing condition, not introduced by this change, and the plan correctly describes the SN=0 check as a "safety net" (plan line 149).
- No data loss: `return false` happens before `imx_read_bulk_samples()` is called, so ring buffer read pointers are not advanced.

---

## Remaining Observations (non-blocking)

1. **Source rotation with SN=0 and `canbus_registered()==true`:** If `can_controller_registered` is true but SN=0, the rotation logic at `imatrix_upload.c:814,831` will still rotate to CAN_DEVICE. The SN=0 guard will then reject the build, counting as a failure. After `IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES`, source rotation kicks in again. This creates a harmless spin through rotation cycles until SN becomes non-zero. This is pre-existing behavior (the current code would use `tsd/` fallback instead), and the plan's approach (suppress upload) is actually better than uploading to a wrong URI. No action needed for this plan.

2. **APPLIANCE_GATEWAY `tsd/` at line 1251:** The plan explicitly scopes this out (line 200). If APPLIANCE_GATEWAY also needs to move to `selfreport/`, that is a separate change.

3. **`COAP_PUT_ENDPOINT_TSD_UPSTREAM` macro retained:** Correctly kept for APPLIANCE_GATEWAY and BLE builds (plan line 202).

---

## Verification Notes

Files examined to validate the plan:

- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` — Lines 43-54 (WINDOW_LOCK/UNLOCK macros), 267-299 (free_slot takes lock), 1176-1294 (build_packet function with lock, return paths, current SN=0 fallback code), 1422-1423 and 1849-1850 (existing WINDOW_UNLOCK+return false patterns)
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imatrix_upload.c` — Lines 770-854 (source rotation logic with `imx_is_can_ctrl_sn_valid()` and `canbus_registered()` guards), 1282-1296 (caller handles false return: free_slot + consecutive failure counting)
- `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_imx_upload.c` — Lines 101-111 (`canbus_registered()` checks `can_controller_registered` flag only, not SN value)
- `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_utils.c` — Line 1549 (`imx_save_can_controller_sn` sets `can_controller_registered = true` regardless of SN value)
