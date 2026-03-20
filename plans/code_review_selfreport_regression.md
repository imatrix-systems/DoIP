# Regression Review: CAN Upload Selfreport URI Change

**Reviewer:** Regression
**Date:** 2026-03-17
**File under review:** `iMatrix/imatrix_upload/imx_upload_window.c` (lines 1195-1260)
**Supporting file:** `iMatrix/imatrix_upload/imatrix_upload.c` (lines 788-840)

## Verdict: PASS

All five regression criteria verified. No existing functionality is broken.

---

## 1. Gateway uploads (IMX_UPLOAD_GATEWAY) are completely unaffected

**PASS**

Under `#ifdef CAN_PLATFORM`, the gateway path is explicitly separated at both the serial number stage and the CoAP header stage:

- **Serial number (lines 1204-1208):** `IMX_UPLOAD_GATEWAY` takes its own branch, calling `safe_string_copy(device_serial_number, device_config.device_serial_number, ...)`. The `can_sn` variable is never referenced in this branch.
- **CoAP header (lines 1233-1236):** `IMX_UPLOAD_GATEWAY` calls `imx_setup_coap_sync_packet_header(msg)` unconditionally, entering the `if` branch before the `else` block that contains the new `can_sn != 0` guard.

The gateway code path is identical to what it would have been before the change. The `can_sn` variable is declared inside `#ifdef CAN_PLATFORM` but is never read on the gateway path.

Under `#elif defined(APPLIANCE_GATEWAY)` (lines 1215-1225, 1253-1261) and `#else` (lines 1226-1229, 1262-1264), the code has no `can_sn` variable at all -- completely untouched.

## 2. Source rotation guards prevent entering HOSTED_DEVICE when SN is invalid

**PASS**

At `imatrix_upload.c` line 797-800, the rotation from GATEWAY to HOSTED_DEVICE is gated on `imx_is_can_ctrl_sn_valid() == true`. If the CAN controller SN is invalid, the state machine either skips to `IMX_UPLOAD_BLE_DEVICE` (line 804) or stays on GATEWAY (line 806). This means `build_packet` should not normally be called with `upload_source == IMX_UPLOAD_HOSTED_DEVICE` when SN is zero.

However, even if it were (race condition, or `imx_is_can_ctrl_sn_valid()` and `get_can_serial_no()` disagree), the SN=0 guard at line 1243 provides a defense-in-depth `return false` with proper `WINDOW_UNLOCK()`.

## 3. build_packet returning false due to SN=0 does not cause infinite loop or stall

**PASS**

When `imx_upload_window_build_packet()` returns `false` at line 1249-1250 (SN=0 case):

1. The caller at `imatrix_upload.c` line 1282-1296 increments `consecutive_build_failures`, frees the slot, and either `continue`s or `break`s (if failures >= 5).
2. After the loop exits, lines 1398-1405 detect the stuck state (`consecutive_build_failures >= IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES`, which is 5) and force source rotation by setting `imatrix.state = IMATRIX_UPLOAD_COMPLETE`.
3. The source rotation logic (lines 788-840) then advances to the next upload source.

The `WINDOW_UNLOCK()` at line 1249 correctly releases the mutex before returning false, matching the pattern used at lines 1192 and other early-exit points throughout the file.

Net result: at most 5 failed build attempts, then the state machine rotates away. No infinite loop, no stall.

## 4. BLE uploads are unaffected

**PASS**

`IMX_UPLOAD_BLE_DEVICE` does not appear anywhere in `imx_upload_window.c`. BLE upload dispatch is handled in `imx_upload_dispatcher.c` (line 345). The BLE source participates in source rotation in `imatrix_upload.c` but never enters the CAN-specific code paths in `imx_upload_window.c`.

Within the `#ifdef CAN_PLATFORM` block in `build_packet`, only `IMX_UPLOAD_GATEWAY`, `IMX_UPLOAD_CAN_DEVICE`, and `IMX_UPLOAD_HOSTED_DEVICE` are referenced. BLE has its own upload mechanism entirely outside this code.

## 5. The `can_sn` variable is only used in CAN_PLATFORM code paths

**PASS**

The `uint32_t can_sn` declaration is at line 1202, inside `#ifdef CAN_PLATFORM`. All four references to `can_sn` (lines 1202, 1213, 1243, 1245) are within the same `#ifdef CAN_PLATFORM` block that ends at line 1252. The variable does not exist in `APPLIANCE_GATEWAY` or default builds.

Grep confirms exactly 4 occurrences, all within the CAN_PLATFORM guard:
- Line 1202: declaration + assignment from `get_can_serial_no()`
- Line 1213: `snprintf` for HOSTED_DEVICE/CAN_DEVICE serial number
- Line 1243: SN=0 guard condition
- Line 1245: passed to `imx_setup_coap_sync_packet_header_for_can()`

---

## Verification Notes

- Read `imx_upload_window.c` lines 1175-1264 to trace the full `build_packet` function from entry through CoAP header setup.
- Read `imatrix_upload.c` lines 788-840 to verify source rotation guards.
- Read `imatrix_upload.c` lines 1282-1296 to verify build failure handling (increment, free slot, continue/break).
- Read `imatrix_upload.c` lines 1390-1426 to verify post-loop stall detection forces rotation after 5 consecutive failures.
- Verified `IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES` is 5 (defined in `imx_upload_internal.h` line 161).
- Grepped for `IMX_UPLOAD_BLE` in `imx_upload_window.c` -- no matches, confirming BLE is not in this code path.
- Grepped for all `can_sn` references in `imx_upload_window.c` -- all 4 are within `#ifdef CAN_PLATFORM`.
- Verified `WINDOW_UNLOCK()` at line 1249 matches the locking pattern used throughout the file (lock at line 1183, unlock on all early-exit paths).
