# Protocol Review: Selfreport URI Plan (Round 2)

**Reviewer:** Protocol Reviewer
**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` v2.0
**Date:** 2026-03-17
**Round:** 2 (re-review after v1.0 fixes)

---

## Round 1 Finding Verification

All 5 Round 1 findings have been addressed in v2.0:

| # | Finding | Severity | Status in v2.0 |
|---|---------|----------|-----------------|
| 1 | WINDOW_UNLOCK() deadlock on SN=0 return | CRITICAL | FIXED -- `WINDOW_UNLOCK()` added before `return false` (plan lines 95-96) |
| 2 | Change 1 targets APPLIANCE_GATEWAY, not compiled on CAN_PLATFORM | CRITICAL | FIXED -- Change 1 removed entirely; plan correctly notes app upload is `#ifdef APPLIANCE_GATEWAY` only |
| 3 | Change 3 vague / msg cleanup on false return | MAJOR | FIXED -- plan explains caller calls `imx_upload_window_free_slot()` which does `msg_release()` (plan line 147). Verified at `imatrix_upload.c:1285` |
| 4 | BLE `tsd/` usage not mentioned | MINOR | FIXED -- explicitly noted as out of scope (plan line 51) |
| 5 | TOCTOU: two calls to `get_can_serial_no()` | MINOR | FIXED -- single local `can_sn` variable used in both serial number header (line 1208) and CoAP URI setup (line 1238) |

---

## Correctness & Completeness Review

### Verdict: PASS

---

### Critical Issues (blocks implementation)

None.

---

### Warnings (should fix before implementing)

None.

---

### Minor Suggestions

1. **Unnecessary `get_can_serial_no()` call for GATEWAY source.** The plan moves the `uint32_t can_sn = get_can_serial_no()` declaration before line 1205, which means it executes even when `upload_source == IMX_UPLOAD_GATEWAY`. This is functionally harmless -- `get_can_serial_no()` is a simple getter with no side effects -- but a pedantic implementation could guard it inside the CAN/HOSTED branch. Not worth blocking on.

2. **`consecutive_build_failures` counter will increment on SN=0 suppression.** When `build_packet()` returns false due to SN=0, the caller at `imatrix_upload.c:1284` increments `consecutive_build_failures`. If the CAN controller stays unregistered for many upload cycles, this counter could trigger source rotation logic (line 1287+). This is arguably the correct behavior -- rotating away from a source that cannot upload -- so this is informational, not a defect.

---

### Missing Steps

None. The plan correctly identifies the single file requiring modification (`iMatrix/imatrix_upload/imx_upload_window.c`) and the single change needed.

---

### Verification of Specific Review Questions

**Q1: Is the URI macro reference corrected to `iMatrix/coap/imx_coap.h`?**

YES. The plan references `iMatrix/coap/imx_coap.h:56,60` (plan line 33). Verified: `COAP_PUT_ENDPOINT_TSD_UPSTREAM` is at line 56 and `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` is at line 60. The dead `#if 0` definitions in `imx_coap_message_composer_utility.h` are correctly identified as inactive.

**Q2: Is the URI format `selfreport/{mfr_id}/{sn}/0` correct for all CAN_PLATFORM uploads?**

YES. The format string at `imx_coap.h:60` is `"selfreport/%"PRIu32"/%010"PRIu32"/0"`. After this change:
- GATEWAY uploads use `imx_setup_coap_sync_packet_header()` with gateway SN -- correct
- CAN_DEVICE uploads (SN>0) use `imx_setup_coap_sync_packet_header_for_can()` with CAN SN -- correct
- HOSTED_DEVICE uploads (SN>0) use `imx_setup_coap_sync_packet_header_for_can()` with CAN SN -- correct
- CAN/HOSTED (SN=0) suppressed with `WINDOW_UNLOCK(); return false;` -- correct

No CAN_PLATFORM upload will use `tsd/{mfr_id}/1` after this change.

**Q3: Are buffer sizes safe?**

YES. Maximum URI length: `selfreport/` (11) + 10-digit mfr_id (10) + `/` (1) + 10-digit zero-padded SN (10) + `/0` (2) + null terminator (1) = 35 bytes. All three CoAP header functions use `uint8_t uri_path[50]` (verified at `imx_coap_message_composer_utility.c` lines 841, 891, 942). 35 < 50. No overflow possible.

---

### Additional Verification

- **No data loss on SN=0:** The `return false` at line ~1245 occurs before `imx_read_bulk_samples()` at line 1384. Ring buffer read pointers are not advanced. Data remains available for future upload cycles.

- **No memory leak:** Verified that `imx_upload_window_free_slot()` (lines 267-296 of `imx_upload_window.c`) calls `msg_release(slot->msg)` when `msg != NULL`, then sets `msg = NULL` and resets all slot state.

- **No deadlock:** The `WINDOW_UNLOCK()` before `return false` matches the established pattern at lines 1192, 1422 in the same function.

- **Existing SN guard:** `imx_is_can_ctrl_sn_valid()` at `imatrix_upload.c:797` prevents rotation to HOSTED_DEVICE when SN=0. The build_packet SN=0 check is a safety net for the race window between rotation and build.

- **APPLIANCE_GATEWAY correctly excluded:** The `imx_app_get_packet()` function in `imatrix_upload.c` is inside `#ifdef APPLIANCE_GATEWAY` and is not compiled on CAN_PLATFORM. No change needed.

---

### Verification Notes

Files examined to validate this review:
- `/home/greg/iMatrix/DOIP/iMatrix/coap/imx_coap.h` lines 55-61 -- active macro definitions confirmed
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` lines 1176-1260 -- function entry, WINDOW_LOCK, serial number block, CoAP header block
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` lines 267-296 -- `imx_upload_window_free_slot()` message release
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` lines 1415-1424 -- existing WINDOW_UNLOCK+return false pattern
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` line 1384 -- `imx_read_bulk_samples()` call location (after proposed exit point)
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imatrix_upload.c` lines 1282-1285 -- caller handles false return with `imx_upload_window_free_slot()`
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imatrix_upload.c` lines 797, 1499 -- existing SN validity guards
- `/home/greg/iMatrix/DOIP/iMatrix/ble/imx_coap_message_composer_utility.c` lines 841, 891, 942 -- `uri_path[50]` buffer sizes
