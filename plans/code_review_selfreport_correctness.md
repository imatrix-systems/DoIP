# Code Review: selfreport URI Change -- Correctness

**Reviewer:** Correctness
**Plan:** `selfreport_uri_plan.md` v2.0
**File:** `iMatrix/imatrix_upload/imx_upload_window.c`
**Date:** 2026-03-17

---

## Verdict: PASS

---

## Checklist

### 1. Code matches plan exactly

The implementation at lines 1199-1264 matches the plan's "After" code precisely:

- `can_sn` declared at line 1202 before the serial number block (plan: "Move the `can_sn` local variable declaration earlier").
- `snprintf` at line 1212-1213 uses `can_sn` instead of `get_can_serial_no()` (plan: "use it in both places").
- `else` block at lines 1238-1252 replaces `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)` with `WINDOW_UNLOCK(); return false;` when `can_sn == 0` (plan: "SN=0 -- do not upload").
- Comment at lines 1239-1242 matches plan wording ("Data stays in ring buffer...").

### 2. `can_sn` scope is correct

`can_sn` is declared at line 1202, inside `#ifdef CAN_PLATFORM` but before both usage sites:
- Line 1213: `snprintf(..., can_sn)` -- serial number in payload header
- Line 1243: `if (can_sn != 0)` -- CoAP URI selection

Both are within the same `#ifdef CAN_PLATFORM` block, so `can_sn` is visible at both sites. No scope issue.

### 3. `WINDOW_UNLOCK()` before `return false`

Line 1249: `WINDOW_UNLOCK();`
Line 1250: `return false;`

This matches the established pattern in the same function:
- Line 1192: `WINDOW_UNLOCK(); return false;` (NULL msg check)
- Lines 1427-1428: `WINDOW_UNLOCK(); return false;` (NULL data_block check)
- Line 1854: `WINDOW_UNLOCK();` followed by `return false;` (no data added)

No deadlock risk.

### 4. TOCTOU fix is correct

Single call to `get_can_serial_no()` at line 1202. The returned value is stored in `uint32_t can_sn` and used consistently at both consumption points (lines 1213 and 1243). If the CAN serial number changed between these two points, both would now see the same value, eliminating the TOCTOU window.

### 5. APPLIANCE_GATEWAY and non-CAN paths unchanged

- Lines 1215-1229 (`#elif defined(APPLIANCE_GATEWAY)` and `#else` serial number blocks): unchanged. `APPLIANCE_GATEWAY` still uses `safe_string_copy` with `get_app_serial_no()`.
- Lines 1253-1264 (`#elif defined(APPLIANCE_GATEWAY)` and `#else` CoAP header blocks): unchanged. `APPLIANCE_GATEWAY` still calls `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)` for appliance devices and `imx_setup_coap_sync_packet_header(msg)` for gateway.

No regressions to non-CAN platform builds.

---

## Additional Verification

- **Data safety on `return false`:** The function returns before `imx_read_bulk_samples()` is called (that code starts at line 1266+), so ring buffer read pointers are not advanced. Data is preserved for the next upload cycle.
- **Memory safety on `return false`:** The caller (`imx_upload_window_process_slot`) calls `imx_upload_window_free_slot()` on false return, which releases the message buffer. No leak.
- **`can_sn` type:** `uint32_t` matches the return type of `get_can_serial_no()` and the parameter type of `imx_setup_coap_sync_packet_header_for_can()`. No truncation or sign issues.
