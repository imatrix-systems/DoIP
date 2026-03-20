# Code Review: selfreport URI Change — Integration

**Reviewer:** Integration
**Date:** 2026-03-17
**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` (v2.0)
**File under review:** `iMatrix/imatrix_upload/imx_upload_window.c`

---

## Verdict: PASS

---

## Checklist

### 1. Platform branches correct and unchanged where expected

The function `imx_upload_window_build_packet()` (line 1176) has two sets of `#ifdef` blocks:

**Serial number block (lines 1199-1229):**
- `#ifdef CAN_PLATFORM` (line 1199): GATEWAY uses `device_config.device_serial_number`; CAN_DEVICE/HOSTED_DEVICE uses `can_sn` via snprintf. **Correct.**
- `#elif defined(APPLIANCE_GATEWAY)` (line 1215): GATEWAY uses `device_config.device_serial_number`; APPLIANCE_DEVICE uses `get_app_serial_no()`. **Unchanged, correct.**
- `#else` (line 1226): Uses `device_config.device_serial_number`. **Unchanged, correct.**

**CoAP header block (lines 1232-1264):**
- `#ifdef CAN_PLATFORM` (line 1232): GATEWAY uses `imx_setup_coap_sync_packet_header()`; else branch checks `can_sn != 0` and either calls `imx_setup_coap_sync_packet_header_for_can()` or does `WINDOW_UNLOCK(); return false;`. **Correct -- this is the change under review.**
- `#elif defined(APPLIANCE_GATEWAY)` (line 1253): APPLIANCE_DEVICE uses `imx_setup_coap_sync_packet_header_for_tsd_upload()`; else uses `imx_setup_coap_sync_packet_header()`. **Unchanged, correct.**
- `#else` (line 1262): Uses `imx_setup_coap_sync_packet_header()`. **Unchanged, correct.**

### 2. `can_sn` scoping

`can_sn` is declared at line 1202, inside the `#ifdef CAN_PLATFORM` block that starts at line 1199. All four uses of `can_sn` occur at lines 1202, 1213, 1243, and 1245 -- all within `#ifdef CAN_PLATFORM` guarded sections. The variable does not leak to `APPLIANCE_GATEWAY` or `#else` branches.

The second `#ifdef CAN_PLATFORM` block (line 1232) uses `can_sn` which was declared in the first `#ifdef CAN_PLATFORM` block (line 1199). This works because both blocks share the same preprocessor condition -- if `CAN_PLATFORM` is defined, both blocks compile, and `can_sn` is in scope for the entire function body from line 1202 onward. If `CAN_PLATFORM` is not defined, neither block compiles, and `can_sn` is never referenced. **Correct.**

### 3. APPLIANCE_GATEWAY branch still uses `tsd/`

Line 1256 calls `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)` for `IMX_UPLOAD_APPLIANCE_DEVICE` sources. This is the only remaining call to `imx_setup_coap_sync_packet_header_for_tsd_upload` in the file (confirmed via grep -- single hit at line 1256). The APPLIANCE_GATEWAY path is untouched by this change. **Correct.**

### 4. No remaining CAN_PLATFORM call sites for `tsd_upload`

Grep for `imx_setup_coap_sync_packet_header_for_tsd_upload` in the file returns exactly one hit: line 1256, which is inside the `#elif defined(APPLIANCE_GATEWAY)` block. There are zero calls within `#ifdef CAN_PLATFORM` blocks. The old `tsd/` fallback for SN=0 on CAN_PLATFORM has been fully replaced by `WINDOW_UNLOCK(); return false;`. **Correct.**

### 5. WINDOW_UNLOCK() before return false

The `WINDOW_UNLOCK(); return false;` pattern at lines 1249-1250 matches the established pattern used at lines 337-338, 1192-1193, 1427-1428, 1739-1740, and 1854-1855 in the same file. The lock acquired at line 1183 is properly released before early return. **No deadlock risk.**

### 6. TOCTOU fix

`get_can_serial_no()` is called once at line 1202 and the result cached in `can_sn`. This same value is used for both the device serial number in the payload header (line 1213) and the CoAP URI setup (lines 1243, 1245). No second call to `get_can_serial_no()` exists in the CAN_PLATFORM code path. **Correct.**

---

## Summary

All platform branches are correctly structured. The `can_sn` variable is properly scoped within `CAN_PLATFORM` guards. The `APPLIANCE_GATEWAY` branch retains its `tsd/` upload path unchanged. No `tsd/` fallback remains in any `CAN_PLATFORM` code path. The `WINDOW_UNLOCK()` follows established patterns. No issues found.
