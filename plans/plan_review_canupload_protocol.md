# CAN Upload Selfreport Plan Review: Protocol
## Review Date: 2026-03-09
## Plan Version: 1.0
## Reviewer: Protocol Correctness (correctness-reviewer)
## Overall: PASS

### Findings

1. **URI format `selfreport/<mfr_id>/<can_sn>/0` -- CORRECT.** The `_for_can()` function at `imx_coap_message_composer_utility.c:901` uses `COAP_PUT_ENDPOINT_SYNC_UPSTREAM`, defined in `imx_coap.h:60` as `"selfreport/%"PRIu32"/%010"PRIu32"/0"`. This produces a URI like `selfreport/1583546208/0012345678/0`. The trailing `/0` is consistent with the gateway function at line 851 which uses the identical format string with the gateway serial number. The `/0` is a fixed path segment present in all selfreport URIs -- it is the expected endpoint suffix for this system.

2. **Serial number format `%010PRIu32` (10-digit zero-padded) -- CORRECT.** The format specifier `%010u` produces a zero-padded 10-digit decimal number. This matches the format used for gateway serial numbers in `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` and in other endpoint macros (e.g., `COAP_ENDPOINT_GET_CFG` at line 79 uses `%010"PRIu32`). The CAN controller serial number is a `uint32_t` (max value 4,294,967,295 = 10 digits), so the format width is appropriate and consistent with the established convention.

3. **CoAP URI_PATH option handling -- ACCEPTABLE (matches codebase convention).** Per RFC 7252 Section 5.10.1, URI_PATH options should contain individual path segments (one option per segment, split at "/" boundaries). The `_for_can()` function passes the entire formatted URI string (including slashes) as a single `add_coap_str_option(URI_PATH, ...)` call rather than using `add_options_from_string(URI_PATH, '/', ...)` which splits at slash boundaries. However, this is the **exact same pattern** used by the gateway function (`imx_setup_coap_sync_packet_header` at line 855), the TSD upload function (line 954), measurement uploads (`imx_measurments_upload.c:202`), event uploads (`imx_event_upload.c:192`), and the vast majority of CoAP requests throughout the codebase. Only two call sites use `add_options_from_string` for proper segmentation (`send_coap_request.c:79` and `imx_requests.c:542`). The iMatrix server clearly expects and correctly handles the single-option-with-slashes format. This is a pre-existing system-wide convention, not a defect introduced by this plan.

4. **Content format `BINARY_MEDIA_TYPE` -- CORRECT.** `BINARY_MEDIA_TYPE` is defined as `42` in `coap.h:209`, which corresponds to `application/octet-stream` in the CoAP Content-Format registry. This is the correct content format for binary sensor data payloads. Both the gateway function (line 856) and the `_for_can()` function (line 906) use this same content format. The payload format is unchanged by this plan.

5. **CoAP method PUT -- CORRECT.** The `_for_can()` function sets `code = (REQUEST << 5) | PUT` at line 914, identical to the gateway function at line 864 and the TSD upload function. PUT is the correct method for uploading sensor data to the selfreport endpoint -- this is consistent with the endpoint naming convention (`COAP_PUT_ENDPOINT_SYNC_UPSTREAM`) and all existing upload functions.

6. **Message type CONFIRMABLE -- CORRECT.** The `_for_can()` function uses `CONFIRMABLE` at line 912, matching the gateway function. This ensures reliable delivery with acknowledgment, which is appropriate for sensor data uploads.

7. **Dead macro `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` -- NOTE.** At `imx_coap_message_composer_utility.c:882`, the macro `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` is defined as `"tsd/%"PRIu32"/%010"PRIu32""` but is never referenced anywhere in the function or codebase. The `_for_can()` function correctly uses `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` instead (line 901). This dead macro appears to be a remnant from before the function was updated to use the selfreport format. It does not affect correctness but could be cleaned up.

### Warnings

None. All protocol aspects are correct and consistent with the existing codebase conventions.

### Minor Suggestions

1. **Dead macro cleanup.** The `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` macro defined at line 882 of `imx_coap_message_composer_utility.c` is unused dead code. Consider removing it in a separate cleanup pass to avoid confusion about which URI format the function actually uses.

### Summary

The plan is protocol-correct on all five review questions. The `_for_can()` function already implements the selfreport URI format identically to the gateway function -- same format string, same content format, same CoAP method, same message type. The plan's single-line change simply wires up this existing, correctly-implemented function. The URI_PATH option encoding uses the single-string-with-slashes convention that is the established pattern throughout the codebase (used by 25+ call sites vs. 2 that use proper segmentation), and the server handles it correctly.

### Files Verified

- `/home/greg/iMatrix/DOIP/iMatrix/coap/imx_coap.h` lines 56-60: endpoint format string macros
- `/home/greg/iMatrix/DOIP/iMatrix/coap/add_coap_option.c` lines 529-559: `add_coap_str_option` (no slash splitting)
- `/home/greg/iMatrix/DOIP/iMatrix/coap/add_coap_option.c` lines 613-666: `add_options_from_string` (splits at separator)
- `/home/greg/iMatrix/DOIP/iMatrix/coap/coap.h` line 209: `BINARY_MEDIA_TYPE` = 42
- `/home/greg/iMatrix/DOIP/iMatrix/ble/imx_coap_message_composer_utility.c` lines 833-877: gateway header setup
- `/home/greg/iMatrix/DOIP/iMatrix/ble/imx_coap_message_composer_utility.c` lines 878-927: CAN header setup (`_for_can`)
- `/home/greg/iMatrix/DOIP/iMatrix/ble/imx_coap_message_composer_utility.c` lines 933-963: TSD header setup
- `/home/greg/iMatrix/DOIP/iMatrix/imatrix_upload/imx_upload_window.c` lines 1228-1248: dispatch logic
- 25+ `add_coap_str_option(URI_PATH, ...)` call sites across the codebase confirming the single-string convention
