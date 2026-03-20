# Code Review: Selfreport URI — Data Integrity

**Reviewer:** Data Integrity Agent
**Date:** 2026-03-17
**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` (v2.0)
**Changed File:** `iMatrix/imatrix_upload/imx_upload_window.c`

---

## Verdict: PASS

---

## Findings

### 1. Ring buffer data preserved when SN=0 (PASS)

When `can_sn == 0`, the function returns `false` at line 1250. The ring buffer read function `imx_read_bulk_samples()` is called at lines 1389 and 1697 -- both far after the return point at line 1250. Read pointers are only advanced inside `imx_read_bulk_samples()`. Since execution never reaches that call when SN=0, no ring buffer read pointers are advanced and all sensor data is preserved for the next upload cycle.

### 2. Message buffer properly released by caller (PASS)

The caller at `imatrix_upload.c:1282-1285` handles the `false` return:

```c
if (!imx_upload_window_build_packet(sw_slot_idx, current_time))
{
    consecutive_build_failures++;
    imx_upload_window_free_slot(sw_slot_idx);
```

`imx_upload_window_free_slot()` (line 267) checks `slot->msg != NULL` and calls `msg_release(slot->msg)` followed by `slot->msg = NULL`. This correctly frees the message buffer. No memory leak.

### 3. `can_sn` used consistently for serial number string and URI (PASS)

The TOCTOU fix reads `can_sn` once at line 1202 via `get_can_serial_no()`. This single value is used in both places:

- **Payload serial number** (line 1212): `snprintf(device_serial_number, ..., "%u", can_sn)`
- **CoAP URI** (line 1245): `imx_setup_coap_sync_packet_header_for_can(msg, can_sn)`

Both derive from the same local variable. No second call to `get_can_serial_no()` exists in the CAN_PLATFORM path. The serial number embedded in the payload header will always match the serial number in the URI.

### 4. Gateway data cannot accidentally use `can_sn` (PASS)

The gateway path is fully isolated from `can_sn`:

- **Serial number** (line 1204-1207): `IMX_UPLOAD_GATEWAY` branch uses `device_config.device_serial_number` (the gateway's own SN via `safe_string_copy`), not `can_sn`.
- **CoAP URI** (line 1233-1236): `IMX_UPLOAD_GATEWAY` branch calls `imx_setup_coap_sync_packet_header(msg)` which uses the gateway's own selfreport URI. It does not call `imx_setup_coap_sync_packet_header_for_can()`.

The `can_sn` variable exists in scope but is never referenced by the gateway branches. Gateway data cannot be misattributed to the CAN controller.

### 5. SN=0 does not produce corrupted payload (PASS)

When `can_sn == 0`, the `else` branch at line 1237 is entered (since `upload_source` is CAN_DEVICE or HOSTED_DEVICE, not GATEWAY). The `can_sn != 0` check at line 1243 fails, and the function returns `false` at line 1250. This means:

- `device_serial_number` was set to `"0"` at line 1212 via `snprintf`, but this value is never consumed -- it is only used at lines 1477 and 1791, both unreachable after the return.
- No CoAP header is set with a zero serial number.
- No packet with SN=0 is ever built or transmitted.

### 6. Mutex correctly released on SN=0 path (PASS)

`WINDOW_LOCK()` is acquired at line 1183. The SN=0 path calls `WINDOW_UNLOCK()` at line 1249 before `return false` at line 1250. This matches the existing early-return pattern elsewhere in the function (lines 1192, 1427, 1739, 1854). No deadlock risk.

Note: `imx_upload_window_free_slot()` also acquires `WINDOW_LOCK()` at line 274. Since the lock is released before the `return false`, and `free_slot` acquires its own lock, there is no double-lock issue (the mutex is non-recursive based on the `pthread_mutex_unlock` at line 50).

---

## Edge Cases Verified

| Scenario | Behavior | Data Safe? |
|----------|----------|------------|
| CAN SN=0, source=CAN_DEVICE | `WINDOW_UNLOCK(); return false` | Yes -- ring buffer untouched |
| CAN SN=0, source=HOSTED_DEVICE | `WINDOW_UNLOCK(); return false` | Yes -- ring buffer untouched |
| CAN SN=0, source=GATEWAY | Gateway path taken (line 1233), `can_sn` not used | Yes -- gateway uses own SN |
| CAN SN changes between cycles | Local `can_sn` ensures consistency within one packet build | Yes -- TOCTOU eliminated |
| `build_packet` returns false repeatedly | Caller increments `consecutive_build_failures`, breaks after `IMX_UPLOAD_MAX_CONSECUTIVE_FAILURES` to trigger source rotation | Yes -- prevents infinite loop |

---

## Summary

The implementation correctly preserves data integrity in all paths. Ring buffer read pointers are never advanced when SN=0. The message buffer is freed by the caller. The gateway serial number and CAN serial number are kept in separate, non-overlapping code paths. The TOCTOU fix ensures the same `can_sn` value is used for both the payload header and the CoAP URI within a single packet build.
