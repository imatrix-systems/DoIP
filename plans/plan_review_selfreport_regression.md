# Regression Review: Unify All Uploads to selfreport URI

**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` v2.0
**Reviewer:** Regression
**Round:** 2
**Date:** 2026-03-17

---

## Correctness & Completeness Review

### Verdict: PASS

---

### Round 1 Findings -- Resolution Status

All 5 findings from Round 1 are resolved in v2.0:

1. **CRITICAL: WINDOW_UNLOCK() deadlock** -- RESOLVED. Plan now includes `WINDOW_UNLOCK();` before `return false;` at the SN=0 path (plan lines 95-96). Matches the existing pattern at lines 1192-1193, 1422-1423, and 1849-1850 of `imx_upload_window.c`.

2. **CRITICAL: Change 1 targets APPLIANCE_GATEWAY code** -- RESOLVED. Change 1 removed entirely. Plan correctly notes that `imx_app_get_packet()` is `#ifdef APPLIANCE_GATEWAY` only and not compiled on CAN_PLATFORM.

3. **MAJOR: Change 3 was vague** -- RESOLVED. Change 3 removed; the `return false` handling is now fully specified in the single remaining change.

4. **MAJOR: BLE tsd/ not addressed** -- RESOLVED. Plan explicitly marks BLE measurements (`imx_measurments_upload.c:200`) as out of scope at lines 51 and 200-202.

5. **MINOR: TOCTOU double call to get_can_serial_no()** -- RESOLVED. Plan moves `uint32_t can_sn = get_can_serial_no();` before the serial number block, reuses it for both `snprintf` (line 1208) and CoAP header setup (line 1238). Single read eliminates the race.

---

### Specific Review Questions

**1. Will WICED and APPLIANCE_GATEWAY builds be unaffected?**

Yes. The change is entirely within `#ifdef CAN_PLATFORM` blocks (lines 1199-1225 and 1228-1247 of `imx_upload_window.c`). The `#elif defined(APPLIANCE_GATEWAY)` branch (lines 1211-1221, 1248-1256) and the `#else` branch (lines 1222-1225, 1257-1259) are untouched. The `COAP_PUT_ENDPOINT_TSD_UPSTREAM` macro is kept for those builds.

**2. Will BLE uploads continue to work?**

Yes. BLE uploads use `imx_setup_coap_tsd_packet_header()` (a different function from `imx_setup_coap_sync_packet_header_for_tsd_upload()`), called from `imx_upload_dispatcher.c:205` inside `#ifdef IMX_BLE_ENABLED`. BLE data does not flow through `imx_upload_window_build_packet()` CoAP header setup. No BLE code paths are modified.

**3. Could the SN=0 suppression cause source rotation to stall?**

No. When `build_packet` returns false, the caller at `imatrix_upload.c:1282-1296` increments `consecutive_build_failures`, frees the slot via `imx_upload_window_free_slot()`, and after 5 failures (`IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES`, defined in `imx_upload_internal.h:161`) breaks out of the upload loop to trigger source rotation. Rotation moves to GATEWAY which always proceeds. No stall.

**4. Does `imx_is_can_ctrl_sn_valid()` prevent rotation to HOSTED_DEVICE when SN=0?**

Partially. `imx_is_can_ctrl_sn_valid()` at `imatrix_upload.c:797` and `:1499` checks `device_config.canbus.can_controller_registered` (`can_utils.c:1513`). It does NOT check whether `get_can_serial_no()` returns 0. There is a known edge case where `imx_save_can_controller_sn(0)` sets `can_controller_registered = true` with SN=0. In that window, rotation to HOSTED_DEVICE would proceed, and `build_packet` would be called with SN=0. The plan's SN=0 guard in `build_packet` correctly handles this as a safety net -- it returns false, the slot is freed, and rotation continues after 5 failures. This is defense in depth and is adequate.

---

### Critical Issues (blocks implementation)

None.

### Warnings (should fix before implementing)

None.

### Minor Suggestions

1. **Plan line 149 is slightly misleading.** It states `imx_is_can_ctrl_sn_valid()` "prevents source rotation to HOSTED_DEVICE when SN=0". In reality it prevents rotation when `can_controller_registered` is false, which is not the same as SN=0 (the known `imx_save_can_controller_sn(0)` edge case can have registered=true with SN=0). The plan does acknowledge this is a "safety net for the race window" which is the correct framing. Consider rewording to: "prevents rotation when CAN is not registered; the SN=0 check here catches the edge case where registration occurs with SN=0."

2. **APPLIANCE_GATEWAY `tsd/` usage at line 1249-1252.** The plan's "Out of Scope" section covers this. If unifying APPLIANCE_GATEWAY uploads to selfreport is desired in the future, that would be a separate change requiring `get_app_serial_no()` integration. No action needed now.

---

### Missing Steps

None.

---

### Verification Notes

**Files examined:**

- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` lines 1175-1264: Confirmed `WINDOW_LOCK()` at line 1183, existing `WINDOW_UNLOCK(); return false;` patterns at lines 1192-1193, 1422-1423, 1849-1850. Confirmed two separate `#ifdef CAN_PLATFORM` blocks (1199-1225 and 1228-1259) share function scope so a `can_sn` variable declared in the first block is visible in the second. Confirmed the `else` branch at line 1233 handles both `IMX_UPLOAD_CAN_DEVICE` and `IMX_UPLOAD_HOSTED_DEVICE` (any non-GATEWAY source on CAN_PLATFORM).

- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imatrix_upload.c` lines 788-806: Confirmed `imx_is_can_ctrl_sn_valid()` gates GATEWAY->HOSTED_DEVICE rotation. Lines 1282-1296: Confirmed `build_packet` returning false triggers `consecutive_build_failures++`, `imx_upload_window_free_slot()`, and rotation after 5 failures. Lines 1496-1544: Confirmed full rotation order GATEWAY->HOSTED->BLE->CAN->GATEWAY.

- `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_utils.c` lines 1511-1514: Confirmed `imx_is_can_ctrl_sn_valid()` checks `can_controller_registered` flag only, not SN value. `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_imx_upload.c` lines 101-106: Confirmed `canbus_registered()` checks the same flag. Neither function validates SN!=0.

- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_internal.h` line 161: Confirmed `IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES` is 5.
