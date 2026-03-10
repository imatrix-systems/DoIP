# CAN Upload Code Review: Regression

## Review Date: 2026-03-09
## Overall: PASS

### Findings

#### 1. APPLIANCE_GATEWAY path is completely untouched -- PASS

Lines 1248-1256 of `imx_upload_window.c` contain the `#elif defined(APPLIANCE_GATEWAY)` block.
This block continues to call `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)` for
appliance devices and `imx_setup_coap_sync_packet_header(msg)` for the gateway itself.
No lines in this block were modified. The `device_serial_number` logic in the
APPLIANCE_GATEWAY branch (lines 1211-1221) is also unchanged.

#### 2. No other CAN_PLATFORM code paths are affected -- PASS

The CAN_PLATFORM `#ifdef` block (lines 1228-1247) changes only the CoAP header setup.
The change introduces a conditional: when `can_sn != 0`, it calls
`imx_setup_coap_sync_packet_header_for_can(msg, can_sn)` which uses the
`selfreport/<mfr_id>/<serial_no>/0` URI. When `can_sn == 0` (re-registration window),
it falls back to `imx_setup_coap_sync_packet_header_for_tsd_upload(msg)` which uses
the `tsd/<mfr_id>/1` URI. Previously, CAN uploads always used the gateway's
`imx_setup_coap_sync_packet_header(msg)` which routes via the gateway serial number.

The `IMX_UPLOAD_GATEWAY` sub-path (line 1229-1232) is unchanged -- gateway uploads
still use `imx_setup_coap_sync_packet_header(msg)`.

#### 3. Payload building logic is unaffected -- PASS

Lines 1261 onward (packet building, hot list fast path, legacy scan, sensor iteration)
are entirely after the `#endif` at line 1259 and are not conditionally compiled.
The header setup functions only write to the CoAP header and options fields of the
message buffer; they do not touch the payload area beyond setting `msg_length` to
mark where payload begins. The payload building code appends starting at the
current `msg_length` offset, so the change in URI (which may differ in byte length)
is correctly handled by the existing `msg_length` bookkeeping.

#### 4. Response handling is unaffected -- PASS

Response matching uses `imx_upload_window_find_by_message_id()` (line 174) which
correlates ACKs by CoAP message ID (`header.mode.udp.id`), not by URI path.
Both `_for_can` and `_for_tsd_upload` set `response_processing_method = IMX_RUN_RESPONSE_CALLBACK`
and both use `CONFIRMABLE` message type. The response callback is set later in the
send path at line 353 (`slot->msg->coap.response_fn = _imx_upload_window_response_handler`),
which is URI-agnostic. No regression risk here.

#### 5. `device_serial_number` local variable is unchanged -- PASS

Lines 1197-1225 define the `device_serial_number` local variable used for payload
building (not for URI construction). The CAN_PLATFORM branch (lines 1199-1210)
still uses `device_config.device_serial_number` for `IMX_UPLOAD_GATEWAY` and
`get_can_serial_no()` for CAN/hosted devices. This is entirely separate from the
CoAP header setup and is not modified.

#### 6. `imx_setup_coap_sync_packet_header_for_can` was previously unused in upload window -- CONFIRMED

Grep confirms this function's only call site in the upload path is the new line 1241.
It was previously defined in `imx_coap_message_composer_utility.c` (line 883) but
had no callers in `imx_upload_window.c`. Its other callers: none found outside this
file, confirming it was available but unused in the upload window prior to this change.

#### 7. `imx_setup_coap_sync_packet_header_for_tsd_upload` callers are not affected -- PASS

This function has three call sites:
- `imx_upload_window.c:1245` -- new CAN fallback (can_sn == 0)
- `imx_upload_window.c:1251` -- existing APPLIANCE_GATEWAY path (unchanged)
- `imatrix_upload.c:1841` -- legacy single-packet upload path (unchanged)

No call sites were removed or had their arguments modified.

#### 8. Observation: Dead `#define` for `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` -- LOW (cosmetic)

Line 882 defines `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` as `"tsd/%"PRIu32"/%010"PRIu32""`,
but `imx_setup_coap_sync_packet_header_for_can` at line 901 actually uses
`COAP_PUT_ENDPOINT_SYNC_UPSTREAM` (`"selfreport/%"PRIu32"/%010"PRIu32"/0"`).
The `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` macro is dead code. This is pre-existing
and not introduced by this change, but worth noting for future cleanup.

### Summary

The change is narrowly scoped: within the `CAN_PLATFORM` `#ifdef` block, non-gateway
uploads switch from the gateway's `imx_setup_coap_sync_packet_header()` (which
used the gateway's own serial number in the selfreport URI) to
`imx_setup_coap_sync_packet_header_for_can()` (which uses the CAN controller's
serial number in the selfreport URI), with a safe fallback to the tsd URI when the
CAN serial number is temporarily zero during re-registration.

No regressions found:
- APPLIANCE_GATEWAY path: untouched
- Payload building: unaffected (starts after `#endif`, uses `msg_length` offset)
- Response handling: matches by message ID, URI-agnostic
- `device_serial_number` variable: unchanged
- No existing callers of modified functions were altered
- The `can_sn != 0` guard prevents uploads with an invalid serial number
