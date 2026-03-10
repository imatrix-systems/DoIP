# CAN Upload Selfreport Plan Review: Correctness
## Review Date: 2026-03-09
## Plan Version: 1.0
## Reviewer: Correctness (correctness-reviewer)
## Overall: PASS

### Findings

1. **Three-function table -- CORRECT.** Verified all three functions in `/home/greg/iMatrix/DOIP/iMatrix/ble/imx_coap_message_composer_utility.c`:
   - `imx_setup_coap_sync_packet_header()` (line 833): uses `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` with `device_config.device_serial_number` -- selfreport URI. Correct.
   - `imx_setup_coap_sync_packet_header_for_can()` (line 883): uses `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` with the `serial_number` parameter -- selfreport URI. Correct.
   - `imx_setup_coap_sync_packet_header_for_tsd_upload()` (line 933): uses `COAP_PUT_ENDPOINT_TSD_UPSTREAM` with only `manufactuer_id` -- tsd URI. Correct.

2. **`_for_can()` uses selfreport, not tsd -- CORRECT.** The plan claims line 901 uses `COAP_PUT_ENDPOINT_SYNC_UPSTREAM`. Verified: line 901 reads `snprintf((char *)&uri_path, 50, COAP_PUT_ENDPOINT_SYNC_UPSTREAM, ...)`. The macro `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` defined at line 882 is dead code (defined but never referenced in the function body).

3. **Function signature match -- CORRECT.** Declaration at `imx_coap_message_composer_utility.h:147` is `void imx_setup_coap_sync_packet_header_for_can( message_t *ptr,uint32_t serial_number );`. The proposed call `imx_setup_coap_sync_packet_header_for_can(msg, get_can_serial_no())` passes `message_t*` and `uint32_t` -- types match.

4. **`get_can_serial_no()` returns valid data -- CORRECT.** Defined at `can_imx_upload.c:134`, returns `imx_get_can_controller_sn()` which reads from `device_config.canbus.can_controller_sn`. The plan's claim that CAN uploads only occur after registration (when `can_controller_sn` is non-zero) is consistent with the `canbus_registered()` guard at line 101-111 of the same file, which checks `device_config.canbus.can_controller_registered`.

5. **Required headers already included -- CORRECT.** Verified in `imx_upload_internal.h`:
   - Line 60: `#include "../canbus/can_imx_upload.h"` (provides `get_can_serial_no()`)
   - Line 80: `#include "../ble/imx_coap_message_composer_utility.h"` (provides `imx_setup_coap_sync_packet_header_for_can()`)
   - Both declarations confirmed at their stated locations (`can_imx_upload.h:66` and `imx_coap_message_composer_utility.h:147`).

6. **APPLIANCE_GATEWAY path unchanged -- CORRECT.** The `#elif defined(APPLIANCE_GATEWAY)` block at lines 1237-1245 of `imx_upload_window.c` is in a separate preprocessor branch from `#ifdef CAN_PLATFORM` (lines 1228-1236). The plan's change only modifies line 1235 inside the CAN_PLATFORM branch. The APPLIANCE_GATEWAY branch is structurally unreachable when CAN_PLATFORM is defined, and vice versa.

7. **Dispatch logic analysis -- CORRECT.** The current code at lines 1228-1236 shows exactly what the plan describes: the `else` branch (non-gateway CAN uploads) calls `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)`. The plan correctly identifies this as the line to change.

8. **URI format macros -- CORRECT.** Verified in `imx_coap.h`:
   - Line 56: `COAP_PUT_ENDPOINT_TSD_UPSTREAM` = `"tsd/%"PRIu32"/1"` (no serial number)
   - Line 60: `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` = `"selfreport/%"PRIu32"/%010"PRIu32"/0"` (with serial number)

9. **`device_serial_number` local variable -- CORRECT.** The plan notes (lines 1199-1210) that the local `device_serial_number` variable already correctly uses `get_can_serial_no()` for CAN uploads. This is used in payload sample block headers, independent of the URI. Verified in the source.

### Warnings

None.

### Minor Suggestions

1. The dead macro `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` at line 882 of `imx_coap_message_composer_utility.c` is unused. Consider removing it in a cleanup pass to avoid confusion, though this is outside the scope of the current plan.

### Missing Steps

None. The plan is a genuine 1-line change with all prerequisites already in place.

### Verification Notes

Verified the following files directly against the plan's claims:
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` lines 1185-1254: dispatch logic, serial number setup, preprocessor branching
- `/home/greg/iMatrix/DOIP/iMatrix/ble/imx_coap_message_composer_utility.c` lines 833-977: all three header setup functions, URI format strings used
- `/home/greg/iMatrix/DOIP/iMatrix/ble/imx_coap_message_composer_utility.h` line 147: function declaration
- `/home/greg/iMatrix/DOIP/iMatrix/coap/imx_coap.h` lines 55-60: URI macro definitions
- `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_imx_upload.c` lines 101-137: `canbus_registered()` and `get_can_serial_no()`
- `/home/greg/iMatrix/DOIP/iMatrix/canbus/can_imx_upload.h` line 66: `get_can_serial_no()` declaration
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_internal.h` lines 60, 80: include verification

### Summary

The plan is correct in all respects. Every claim about file paths, line numbers, function signatures, URI formats, include chains, and preprocessor guards was verified against the source code. The proposed 1-line change at `imx_upload_window.c:1235` will correctly route CAN device uploads through `imx_setup_coap_sync_packet_header_for_can()` using the `selfreport/` URI with the CAN controller serial number, while leaving gateway and APPLIANCE_GATEWAY paths untouched. No additional file changes are required.
