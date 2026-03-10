# Plan: CAN Bus Upload — Switch to Selfreport URI Format

**Version:** 2.0
**Status:** COMPLETE — All 6 code review agents PASS
**Scope:** Change CAN bus upload CoAP URI from `tsd/` to `selfreport/` with CAN controller serial number

---

## Problem Statement

CAN bus sensor data uploads currently use the wrong CoAP URI format:

- **Current:** `tsd/<manufacturer_id>/1` — no serial number, generic endpoint
- **Required:** `selfreport/<manufacturer_id>/<can_controller_sn>/0` — identifies the specific CAN controller

The gateway (FC-1) uploads its own data correctly using `selfreport/<mfr_id>/<device_sn>/0`, but CAN device uploads are routed through `imx_setup_coap_sync_packet_header_for_tsd_upload()` which uses the `tsd/` URI without a serial number.

### Root Cause

In `imx_upload_window.c`, the CoAP header setup dispatch (line 1228-1236) sends all non-gateway CAN_PLATFORM uploads through `imx_setup_coap_sync_packet_header_for_tsd_upload()` instead of `imx_setup_coap_sync_packet_header_for_can()`.

The correct function `imx_setup_coap_sync_packet_header_for_can()` already exists (line 883 of `imx_coap_message_composer_utility.c`), already uses the `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` ("selfreport/...") format, and already accepts the CAN controller serial number as a parameter. It is simply not wired up.

---

## Analysis

### Three CoAP Header Setup Functions

| Function | URI Format | Serial Number | Used For |
|----------|-----------|---------------|----------|
| `imx_setup_coap_sync_packet_header()` | `selfreport/<mfr>/<device_sn>/0` | Gateway device SN | Gateway uploads |
| `imx_setup_coap_sync_packet_header_for_can()` | `selfreport/<mfr>/<can_sn>/0` | CAN controller SN | **Should be used for CAN uploads** |
| `imx_setup_coap_sync_packet_header_for_tsd_upload()` | `tsd/<mfr>/1` | None | Appliance gateway uploads |

### Current Dispatch (imx_upload_window.c:1228-1236)

```c
#ifdef CAN_PLATFORM
    if (contents->upload_source == IMX_UPLOAD_GATEWAY)
    {
        imx_setup_coap_sync_packet_header(msg);              // ✓ Correct
    }
    else
    {
        imx_setup_coap_sync_packet_header_for_tsd_upload(msg); // ✗ WRONG — should use _for_can()
    }
```

### Required Dispatch

```c
#ifdef CAN_PLATFORM
    if (contents->upload_source == IMX_UPLOAD_GATEWAY)
    {
        imx_setup_coap_sync_packet_header(msg);              // ✓ Gateway: selfreport/{mfr}/{device_sn}/0
    }
    else if (get_can_serial_no() != 0)
    {
        imx_setup_coap_sync_packet_header_for_can(msg, get_can_serial_no()); // ✓ CAN: selfreport/{mfr}/{can_sn}/0
    }
    else
    {
        imx_setup_coap_sync_packet_header_for_tsd_upload(msg); // Fallback: SN not yet available
    }
```

**Note:** The `get_can_serial_no() != 0` guard prevents uploading with a zero serial number during the brief re-registration window where `can_controller_registered` is true but the SN has been cleared. In this case, the old `tsd/` URI is used as a safe fallback until the SN is available.

### Prerequisites Already Satisfied

- `imx_setup_coap_sync_packet_header_for_can()` declared in `ble/imx_coap_message_composer_utility.h:147` (under `#ifdef CAN_PLATFORM`)
- `imx_setup_coap_sync_packet_header_for_can()` defined in `ble/imx_coap_message_composer_utility.c:883` (under `#ifdef CAN_PLATFORM`)
- `get_can_serial_no()` declared in `canbus/can_imx_upload.h:66`
- `get_can_serial_no()` returns `imx_get_can_controller_sn()` which reads `device_config.canbus.can_controller_sn`
- Both headers already included via `imx_upload_internal.h` (lines 60 and 80)
- The `_for_can()` function uses `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` (selfreport format) at line 901

### CAN Controller SN Availability

CAN uploads only occur after CAN bus registration has completed and `can_controller_sn` is non-zero. The upload source rotation in `imatrix_upload.c` only enables `IMX_UPLOAD_CAN_DEVICE` when the CAN platform is active. Therefore `get_can_serial_no()` will always return a valid serial when called from this code path.

---

## Implementation

### Step 1: Change the CAN Upload Dispatch

**File:** `iMatrix/imatrix_upload/imx_upload_window.c`
**Lines:** 1233-1236

**Before:**
```c
    else
    {
        imx_setup_coap_sync_packet_header_for_tsd_upload(msg);
    }
```

**After:**
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

The `get_can_serial_no() != 0` guard handles the re-registration edge case where `can_controller_registered` is true but the SN has been temporarily cleared to 0. In this window, the old `tsd/` URI is used as a safe fallback.

### No Other Changes Required

- No new includes needed (both `can_imx_upload.h` and `imx_coap_message_composer_utility.h` already included)
- No changes to the `_for_can()` function (already correct)
- No changes to `get_can_serial_no()` (already returns the right value)
- No changes to CMakeLists.txt or Makefile
- No changes to the APPLIANCE_GATEWAY path (line 1237-1245, remains `_for_tsd_upload()`)

---

## Impact Analysis

### URI Change

| Before | After |
|--------|-------|
| `tsd/1583546208/1` | `selfreport/1583546208/0012345678/0` |
| No CAN controller identity | CAN controller SN in URI path |
| Generic endpoint | Device-specific endpoint |

### Server-Side Impact

The iMatrix cloud server must accept the `selfreport/` URI for CAN device data. This is the same URI pattern already used for gateway uploads — the server should already handle it. The CAN controller serial number in the URI allows the server to associate data with the specific CAN controller device.

### Binary Payload Format

No change — the binary payload format (sample block headers with serial number, sensor_id, etc.) remains identical. Only the CoAP URI path changes.

### Upload Source Serialization

The `device_serial_number` local variable (lines 1200-1210) already correctly sets the CAN controller SN for `IMX_UPLOAD_CAN_DEVICE`. This variable is used in the payload's sample block headers, independent of the URI. No change needed.

---

## Testing Strategy

### Build Verification
- Cross-compile with CAN_PLATFORM defined — verify zero new warnings
- Verify APPLIANCE_GATEWAY build is unaffected

### Runtime Verification
1. Deploy to FC-1 device
2. Enable upload debug: `debug DEBUGS_TSD_UPLOAD`
3. Monitor CoAP packets — verify URI contains `selfreport/<mfr_id>/<can_sn>/0`
4. Verify CAN sensor data appears in iMatrix cloud dashboard under the correct device
5. Verify gateway uploads still use `selfreport/<mfr_id>/<gateway_sn>/0` (no regression)

### Edge Cases

| Scenario | Expected |
|----------|----------|
| CAN controller SN = 0 (not registered) | Falls back to `tsd/` URI (SN=0 guard) |
| CAN controller SN = 0 during re-registration | Falls back to `tsd/` URI until SN is restored |
| Gateway upload | Uses `imx_setup_coap_sync_packet_header()` — unchanged |
| APPLIANCE_GATEWAY build | Uses `imx_setup_coap_sync_packet_header_for_tsd_upload()` — unchanged |
| Multiple CAN devices | `get_can_serial_no()` returns the registered controller SN |

---

## File Changes Summary

| File | Change |
|------|--------|
| `iMatrix/imatrix_upload/imx_upload_window.c` | Line 1235: replace `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)` with `imx_setup_coap_sync_packet_header_for_can(msg, get_can_serial_no())` |

**Total: 3-line change in 1 file** (replace 1-line `else` with 5-line `else if / else`).

### Pre-existing Issues (Not In Scope)

| Issue | Notes |
|-------|-------|
| `HOSTED_DEVICE` uses `get_can_serial_no()` in payload header (line 1208) | The `else` branch covers both `IMX_UPLOAD_CAN_DEVICE` and `IMX_UPLOAD_HOSTED_DEVICE`. On FC-1, `get_host_serial_no()` returns the FC-1 device serial (same as gateway). This is a pre-existing behavior — not introduced by this change. |
| Dead macro `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` at line 882 of `imx_coap_message_composer_utility.c` | Defined but never used. Cleanup deferred. |
| `imx_save_can_controller_sn(0)` sets `can_controller_registered = true` with SN=0 | Root cause of the re-registration race. Mitigated by the SN=0 guard in this change. |

---

## Review History

### Round 1 (v1.0) — 6 Reviewers

| Reviewer | Verdict | Key Findings |
|----------|---------|-------------|
| Correctness | PASS | All claims verified against source |
| Integration | PASS | No other CAN callers affected |
| Risk | PASS (conditional) | SN=0 race window needs guard |
| Protocol | PASS | URI format, method, content type correct |
| Regression | PASS (2 MEDIUM) | HOSTED_DEVICE serial mismatch; server compatibility |
| KISS | PASS | Proportionate, no over-engineering |

### Fixes Applied in v2.0

| # | Finding | Fix |
|---|---------|-----|
| 1 | SN=0 race window during re-registration | Added `get_can_serial_no() != 0` guard with tsd/ fallback |
| 2 | HOSTED_DEVICE serial in else branch | Documented as pre-existing (not introduced by this change) |
| 3 | Dead macro cleanup | Deferred (out of scope) |
