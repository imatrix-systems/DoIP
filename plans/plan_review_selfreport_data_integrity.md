# Data Integrity Review: selfreport URI Plan (Round 2)

**Reviewer:** Data Integrity
**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` v2.0
**Date:** 2026-03-17
**Round:** 2 (re-review after v1.0 fixes)

---

## Correctness & Completeness Review

### Verdict: PASS

---

## Round 1 Findings -- Resolution Status

All 7 findings from Round 1 are resolved in v2.0:

| # | Severity | Finding | Status |
|---|----------|---------|--------|
| 1 | CRITICAL | `WINDOW_UNLOCK()` missing before `return false` | FIXED -- plan now includes `WINDOW_UNLOCK(); return false;` (plan lines 95-96) |
| 2 | CRITICAL | Change 1 targeted APPLIANCE_GATEWAY-only code | FIXED -- Change 1 removed entirely; plan correctly notes app upload is `#ifdef APPLIANCE_GATEWAY` only |
| 3 | MAJOR | `device_serial_number` set to "0" when SN=0 | ACKNOWLEDGED -- benign because function returns false before the string is used; documented in "Why this is safe" section |
| 4 | MAJOR | Rapid alloc/free cycle under prolonged SN=0 | ACKNOWLEDGED -- plan notes existing guard at `imatrix_upload.c:797` (`imx_is_can_ctrl_sn_valid()`) prevents rotation to HOSTED/CAN when SN invalid; remaining edge case (SN=0 with `can_controller_registered=true`) is pre-existing and outside scope |
| 5 | MAJOR | TOCTOU between two `get_can_serial_no()` calls | FIXED -- single `can_sn` local variable declared before both usage sites (plan lines 117-141) |
| 6 | MINOR | Reference to nonexistent `imx_upload_batch_load_samples` | FIXED -- removed from plan |
| 7 | MINOR | Untestable App upload acceptance criterion on CAN_PLATFORM | FIXED -- removed from acceptance criteria |

---

## Focused Verification

### 1. Data safety when SN=0 suppresses upload

**Verified: SAFE.** Ring buffer read pointers are not advanced before the `return false` at plan line 139.

The proposed `return false` occurs at approximately line 1245 of `imx_upload_window.c`, inside the CoAP header setup block (lines 1228-1247). The function `imx_read_bulk_samples()` -- which is the only call that advances ring buffer read pointers -- is first called at line 1384, well after the SN=0 exit point (139 lines later). No data is consumed or lost when the upload is suppressed.

Call sequence within `imx_upload_window_build_packet()`:
- Line 1183: `WINDOW_LOCK()`
- Lines 1197-1225: serial number string setup (no ring buffer access)
- Lines 1228-1247: CoAP header setup -- **SN=0 return happens here**
- Lines 1261-1284: variable declarations and diagnostics init
- Lines 1287+: hot list scan begins
- Line 1384: first `imx_read_bulk_samples()` call (hot list path)
- Line 1692: second `imx_read_bulk_samples()` call (legacy scan path)

Data remains in the CSD ring buffers for the next upload cycle.

### 2. TOCTOU fix correctness

**Verified: CORRECT.** The plan moves `uint32_t can_sn = get_can_serial_no();` before the serial number block, so the same value is used at both:
- The `snprintf` for `device_serial_number` (originally line 1209)
- The SN=0 guard and `imx_setup_coap_sync_packet_header_for_can()` call (originally line 1238-1241)

This eliminates the race where SN could change between the two calls. The variable is declared within the `#ifdef CAN_PLATFORM` block, so it does not affect other platform builds.

One minor note: `can_sn` will be initialized even for GATEWAY uploads (where `contents->upload_source == IMX_UPLOAD_GATEWAY`), adding one unnecessary function call. This is negligible overhead and not worth additional branching complexity to avoid.

### 3. Message buffer cleanup on false return

**Verified: CORRECT.** The caller at `imatrix_upload.c:1282-1285` handles false returns properly:

```c
if (!imx_upload_window_build_packet(sw_slot_idx, current_time))
{
    consecutive_build_failures++;
    imx_upload_window_free_slot(sw_slot_idx);
```

`imx_upload_window_free_slot()` at `imx_upload_window.c:267-309` acquires `WINDOW_LOCK()`, then:
- Calls `msg_release(slot->msg)` if msg is non-NULL (line 284)
- Sets `slot->msg = NULL` (line 285)
- Resets slot state to `SLOT_STATE_FREE` (line 289)
- Returns slot to the free stack (line 302)

Since the proposed `return false` calls `WINDOW_UNLOCK()` before returning, the lock is properly released before `imx_upload_window_free_slot()` re-acquires it. No deadlock. No message leak.

### 4. WINDOW_UNLOCK pattern consistency

The proposed `WINDOW_UNLOCK(); return false;` matches the existing pattern at three other early-exit points in the same function:
- Line 1192-1193: null msg check
- Line 1849: no data added
- Line 1864: end of function (return true)

Pattern is consistent.

---

## Remaining Observations (non-blocking)

1. **Pre-existing: `imx_save_can_controller_sn(0)` sets `can_controller_registered = true`** -- This allows source rotation to CAN/HOSTED even with SN=0, which is why the SN=0 guard in `build_packet` exists as defense-in-depth. The plan correctly identifies this at "Why this is safe" point 4. Not introduced by this change.

2. **Pre-existing: Ring buffer overflow under prolonged SN=0** -- If the CAN controller never registers, CSD ring buffers will eventually overwrite old samples with new writes. This is inherent circular buffer behavior and cannot be mitigated without architectural changes. Not in scope.

---

## Verification Notes

Files examined to validate this review:

- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` lines 1170-1866: Confirmed `WINDOW_LOCK()` at line 1183, all existing `return` paths preceded by `WINDOW_UNLOCK()`, `imx_read_bulk_samples()` first called at line 1384 (well after proposed SN=0 return), `imx_upload_window_free_slot()` at lines 267-309 releases message buffer via `msg_release()`.

- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imatrix_upload.c` lines 1282-1296: Confirmed caller calls `imx_upload_window_free_slot()` on false return from `build_packet`, properly cleaning up the message buffer and returning the slot.
