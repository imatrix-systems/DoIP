# Plan: Unify All Uploads to selfreport URI

**Request:** All uploads (gateway, hosted device, CAN controller) must use `selfreport/{mfr_id}/{source_sn}/0` URI with the serial number of the reporting source. If SN=0, do not upload.

**Version:** 2.0 (post-review fixes)
**Date:** 2026-03-17
**Status:** COMPLETE — Implemented, deployed, API-validated 2026-03-17

---

## Round 1 Review Summary

6 agents reviewed v1.0 — all 6 FAILED. Key findings (deduplicated):

1. **CRITICAL (6/6 agents): WINDOW_UNLOCK() deadlock** — Change 2 returned false without releasing mutex
2. **CRITICAL (5/6 agents): Change 1 targets wrong platform** — `imx_app_get_packet()` is `#ifdef APPLIANCE_GATEWAY`, not compiled on CAN_PLATFORM
3. **MAJOR (2/6): Change 3 was vague** — folded into Change 1 as resolved
4. **MAJOR (1/6): Missed `tsd/` in BLE** — `imx_measurments_upload.c:200` uses tsd/ but is BLE-only (out of scope for CAN_PLATFORM)
5. **MINOR (1/6): TOCTOU** — two calls to `get_can_serial_no()` at lines 1209 and 1238 could race; mitigated by reading SN once into local variable

### Fixes Applied in v2.0

- **Removed Change 1** — app upload code is APPLIANCE_GATEWAY-only, not compiled on CAN_PLATFORM
- **Fixed Change 2** — added `WINDOW_UNLOCK()` before `return false`
- **Removed Change 3** — folded into Change 1 (now the only change); function returns `bool`, caller handles `false` via `imx_upload_window_free_slot()`
- **Added TOCTOU fix** — single `get_can_serial_no()` call, cached in local variable for both serial number and CoAP header
- **Corrected URI macro reference** — active definitions are in `iMatrix/coap/imx_coap.h:56,60` (not `imx_coap_message_composer_utility.h` which is inside `#if 0`)
- **Noted BLE tsd/ usage** — `imx_measurments_upload.c:200` uses `tsd/` but is BLE/Wirepas-only, out of scope

---

## Current State

### URI Definitions (`iMatrix/coap/imx_coap.h:56,60`)

| Macro | URI Format | Used By |
|-------|-----------|---------|
| `COAP_PUT_ENDPOINT_TSD_UPSTREAM` | `tsd/{mfr_id}/1` | SN=0 fallback (to be removed), BLE measurements |
| `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` | `selfreport/{mfr_id}/{sn}/0` | Gateway, CAN, HOSTED |

### Current Upload Path Summary (CAN_PLATFORM only)

| Source | CoAP Header Function | URI | Serial | Correct? |
|--------|---------------------|-----|--------|----------|
| GATEWAY | `imx_setup_coap_sync_packet_header()` | `selfreport/{mfr_id}/{gw_sn}/0` | Gateway SN | YES |
| CAN_DEVICE (SN>0) | `imx_setup_coap_sync_packet_header_for_can()` | `selfreport/{mfr_id}/{can_sn}/0` | CAN SN | YES |
| HOSTED_DEVICE (SN>0) | `imx_setup_coap_sync_packet_header_for_can()` | `selfreport/{mfr_id}/{can_sn}/0` | CAN SN | YES |
| CAN/HOSTED (SN=0) | `imx_setup_coap_sync_packet_header_for_tsd_upload()` | `tsd/{mfr_id}/1` | None | **NO — should not upload** |

**Note:** App upload (`imx_app_get_packet`) is `#ifdef APPLIANCE_GATEWAY` only — not compiled on CAN_PLATFORM. No change needed.

**Note:** BLE measurements (`imx_measurments_upload.c:200`) use `tsd/` but are BLE-specific. Out of scope for this change.

---

## Change Required

### Change 1: SN=0 — do not upload (instead of using tsd/)

**File:** `iMatrix/imatrix_upload/imx_upload_window.c`
**Function:** `imx_upload_window_build_packet()` (returns `bool`, acquires `WINDOW_LOCK()` at line 1183)

**Before (lines 1233-1246):**
```c
    else
    {
        /* Use selfreport URI with CAN controller serial number.
         * Guard against SN=0 during re-registration window */
        uint32_t can_sn = get_can_serial_no();
        if (can_sn != 0)
        {
            imx_setup_coap_sync_packet_header_for_can(msg, can_sn);
        }
        else
        {
            imx_setup_coap_sync_packet_header_for_tsd_upload(msg);
        }
    }
```

**After:**
```c
    else
    {
        /* Use selfreport URI with CAN controller serial number.
         * If SN=0 (CAN controller not yet registered), suppress upload.
         * Data stays in ring buffer — read pointers are not advanced
         * until after successful packet build. */
        uint32_t can_sn = get_can_serial_no();
        if (can_sn != 0)
        {
            imx_setup_coap_sync_packet_header_for_can(msg, can_sn);
        }
        else
        {
            WINDOW_UNLOCK();
            return false;
        }
    }
```

**Also fix TOCTOU (line 1208):** Use the same `can_sn` local variable for the device serial number in the payload header, instead of a second call to `get_can_serial_no()`:

**Before (lines 1205-1210):**
```c
    else if ((contents->upload_source == IMX_UPLOAD_CAN_DEVICE) ||
             (contents->upload_source == IMX_UPLOAD_HOSTED_DEVICE))
    {
        snprintf(device_serial_number, IMX_DEVICE_SERIAL_NUMBER_LENGTH + 1, "%u",
                 get_can_serial_no());
    }
```

**After:**
Move the `can_sn` local variable declaration earlier (before the serial number block) and use it in both places:

```c
    /* Read CAN SN once to avoid TOCTOU between serial number header
     * and CoAP URI setup (SN could change between two calls) */
    uint32_t can_sn = get_can_serial_no();

    /* ... (serial number block) ... */
    else if ((contents->upload_source == IMX_UPLOAD_CAN_DEVICE) ||
             (contents->upload_source == IMX_UPLOAD_HOSTED_DEVICE))
    {
        snprintf(device_serial_number, IMX_DEVICE_SERIAL_NUMBER_LENGTH + 1, "%u",
                 can_sn);
    }

    /* ... (CoAP header block) ... */
    else
    {
        if (can_sn != 0)
        {
            imx_setup_coap_sync_packet_header_for_can(msg, can_sn);
        }
        else
        {
            WINDOW_UNLOCK();
            return false;
        }
    }
```

### Why this is safe

1. **No data loss:** The `return false` happens before `imx_read_bulk_samples()` is called — ring buffer read pointers are not advanced. Data stays in memory for the next upload cycle.
2. **No memory leak:** The caller (`imx_upload_window_process_slot`) calls `imx_upload_window_free_slot()` which releases the message buffer via `msg_release()`.
3. **No deadlock:** `WINDOW_UNLOCK()` is called before `return false`, matching the pattern at lines 1192, 1423, 1849 in the same function.
4. **Existing guard:** `imx_is_can_ctrl_sn_valid()` at `imatrix_upload.c:797` prevents source rotation to HOSTED_DEVICE when SN=0. The SN=0 check here is a safety net for the race window.

---

## Verification Plan

### Test Environment
- **FC-1 Gateway:** SN 174664659 (192.168.7.1:22222)
- **CAN Controller:** SN 203849060
- **iMatrix API:** `https://api-dev.imatrixsys.com/api/v1`

### Step 1: Deploy and verify URI switch

1. Build: `cd Fleet-Connect-1 && cmake --preset arm-cross-debug && cd build && make -j4`
2. Deploy: `./scripts/fc1 -d 192.168.7.1 -b ./Fleet-Connect-1/build/FC-1 push -run`
3. Enable debug: `./scripts/fc1 -d 192.168.7.1 cmd "debug DEBUGS_FOR_UPLOADS"`
4. Wait 5 minutes, check log for URIs:

```bash
sshpass -p 'PasswordQConnect' ssh -p 22222 root@192.168.7.1 \
  "grep -i 'selfreport\|tsd/' /var/log/fc-1.log | tail -30"
```

**Expected:** All uploads use `selfreport/{mfr_id}/{sn}/0`. No `tsd/` URIs.

### Step 2: Verify data on iMatrix server

Query the iMatrix API to confirm data arrives under both device serial numbers:

```bash
# Gateway data
curl -s -H "x-auth-token: $TOKEN" \
  "$API/dashboard/174664659/history/$FROM/$NOW?group_by_time=NONE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'Gateway: {len(d)} groups')"

# CAN controller data
curl -s -H "x-auth-token: $TOKEN" \
  "$API/dashboard/203849060/history/$FROM/$NOW?group_by_time=NONE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'CAN: {len(d)} groups')"
```

---

## Affected Files

| File | Change |
|------|--------|
| `iMatrix/imatrix_upload/imx_upload_window.c` | SN=0: `WINDOW_UNLOCK(); return false;` instead of `tsd/` fallback. TOCTOU fix: single `can_sn` local. |

---

## Out of Scope

- **APPLIANCE_GATEWAY app upload** (`imatrix_upload.c:1835`) — different platform, separate change if needed
- **BLE measurements** (`imx_measurments_upload.c:200`) — BLE-specific `tsd/` usage, separate concern
- **`COAP_PUT_ENDPOINT_TSD_UPSTREAM` macro** — kept for APPLIANCE_GATEWAY and BLE builds

---

## Acceptance Criteria

- [ ] All CAN_DEVICE uploads use `selfreport/{mfr_id}/{can_sn}/0`
- [ ] All HOSTED_DEVICE uploads use `selfreport/{mfr_id}/{can_sn}/0`
- [ ] All Gateway uploads use `selfreport/{mfr_id}/{gw_sn}/0` (unchanged)
- [ ] No CAN_PLATFORM uploads use `tsd/{mfr_id}/1`
- [ ] SN=0 condition suppresses upload (no crash, no deadlock, no data loss)
- [ ] iMatrix API shows fresh data under both gateway SN and CAN controller SN
