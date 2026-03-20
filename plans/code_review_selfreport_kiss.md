# Code Review: selfreport URI Change -- KISS

**Reviewer:** KISS
**Date:** 2026-03-17
**File:** `iMatrix/imatrix_upload/imx_upload_window.c` (lines 1199-1252)
**Plan:** `DoIP_Server/plans/selfreport_uri_plan.md` (v2.0)

---

## Verdict: PASS

---

## Checklist

### 1. Is the change minimal -- only the necessary lines modified?

**YES.** The diff consists of exactly two logical edits within the `CAN_PLATFORM` ifdef block:

1. **TOCTOU fix (line 1202):** `uint32_t can_sn = get_can_serial_no()` hoisted before the serial-number if/else chain, replacing a second `get_can_serial_no()` call that was at the old line 1209. One declaration moved up, one function call replaced with the local variable.

2. **SN=0 suppression (lines 1247-1251):** The `else` branch that called `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)` was replaced with `WINDOW_UNLOCK(); return false;`. This is the smallest possible change to suppress upload when SN=0.

No other lines in the file were modified. No other files were modified.

### 2. No new functions, macros, types, or abstractions added?

**YES.** The only new identifier is `uint32_t can_sn`, a local variable. No new functions, macros, typedefs, structs, or helper abstractions were introduced.

### 3. Comments are clear and proportional?

**YES.** Two comments were added:

- Lines 1200-1201: 2-line comment explaining why `can_sn` is read once (TOCTOU). Justified -- the reason for hoisting is not obvious without it.
- Lines 1239-1242: 4-line comment explaining the SN=0 suppression behavior and why data is not lost. Justified -- changing from "upload via tsd/" to "do not upload" is a behavior change that warrants explaining the safety argument. The "ring buffer read pointers are not advanced" detail prevents future maintainers from worrying about data loss.

Both use the file's existing `/* ... */` style with em-dash separators, matching lines 64, 67, 228, etc.

### 4. Code style matches the surrounding code?

**YES.** Verified:

- Brace placement: Allman style, matching rest of file
- Indentation: 4 spaces, consistent with surrounding code
- `WINDOW_UNLOCK(); return false;` pattern matches lines 1192-1193, 337-338, 431, 575, 867, 1427-1428, 1739-1740, 1854-1855
- `#ifdef CAN_PLATFORM` / `#elif defined(APPLIANCE_GATEWAY)` structure unchanged

### 5. Any unnecessary changes or over-engineering?

**NO.** Specific things that were correctly avoided:

- No logging added at the SN=0 suppression point (the upload cycle is frequent; logging here would be noisy)
- No retry mechanism or timer for SN=0 (the upload window naturally retries on the next cycle)
- No removal of the `COAP_PUT_ENDPOINT_TSD_UPSTREAM` macro (still used by APPLIANCE_GATEWAY and BLE code paths)
- No changes to the APPLIANCE_GATEWAY branch (lines 1253-1260), which is a different platform
- The `can_sn` variable is not checked for SN=0 in the serial-number block (lines 1209-1213) -- this is correct because the CoAP header block (line 1243) handles SN=0 by returning false before any packet is built

---

## Summary

Two surgical edits, zero new abstractions, proportional comments, consistent style. The change does exactly what is required and nothing more.
