# CAN Upload Selfreport Plan Review: Integration
## Review Date: 2026-03-09
## Plan Version: 1.0
## Reviewer: Integration (architecture-reviewer)
## Overall: PASS

---

### Findings

#### 1. Upload Source Rotation: No Breakage (PASS)

The `else` branch at `imx_upload_window.c:1233-1236` catches all non-gateway
uploads under `CAN_PLATFORM`, which includes both `IMX_UPLOAD_CAN_DEVICE` and
`IMX_UPLOAD_HOSTED_DEVICE`. This is correct and consistent with the serial
number logic at lines 1205-1210, which already uses `get_can_serial_no()` for
both of these upload sources. The rotation state machine in `imatrix_upload.c`
(lines 462-502) is not touched and continues to control which source is active.
The CoAP header change has no interaction with rotation.

#### 2. Binary Payload Format Compatibility (PASS)

The binary payload is built after the CoAP header setup, starting at
`imx_upload_window.c:1250` ("Build packet from pending sensor data"). The serial
number embedded in sample block headers (`device_serial_number` at lines
1197-1210) is set independently of the URI path. The `_for_can()` function and
`_for_tsd_upload()` function produce identical CoAP message structures
(CONFIRMABLE PUT, same token length, same content format option, same payload
marker). The only difference is the URI_PATH option string. Payload bytes after
the 0xFF marker are untouched.

#### 3. Other Callers of `_for_tsd_upload()` on CAN_PLATFORM (PASS)

Grep confirms three call sites for `imx_setup_coap_sync_packet_header_for_tsd_upload()`:

- `imx_upload_window.c:1235` -- the call being changed (CAN_PLATFORM else branch)
- `imx_upload_window.c:1240` -- APPLIANCE_GATEWAY branch, not CAN_PLATFORM (unaffected)
- `imatrix_upload.c:1841` -- inside `#ifdef APPLIANCE_GATEWAY` block (line 1775), not CAN_PLATFORM (unaffected)

There are no other callers on CAN_PLATFORM. The plan correctly identifies this as the only change needed.

#### 4. Server-Side Impact Analysis: INCOMPLETE (ADVISORY)

The plan states the cloud server "should already handle" selfreport URIs with
CAN serial numbers because gateway uploads already use the same pattern. This is
a reasonable assumption but not a confirmed fact. The URI changes from
`tsd/1583546208/1` (no device identity) to `selfreport/1583546208/0012345678/0`
(with CAN controller serial number). Two potential server-side concerns:

- **Device registration**: The server may need the CAN controller serial number
  to be registered/provisioned before it accepts `selfreport/` uploads for it.
  If CAN device registration already creates this server-side entry (which CAN
  registration in `canbus/coap/registration.c` handles), this is likely fine.
- **Data routing**: The `tsd/` and `selfreport/` endpoints may have different
  server-side processing pipelines. Verify the selfreport endpoint handles the
  same binary TSD payload format.

This is not a code change concern -- it is a deployment concern. The plan should
note that server-side acceptance should be confirmed before production rollout.

#### 5. ACK/Response Handling (PASS)

Response handling is not affected. The response callback
(`_imx_upload_window_response_handler`) is set at slot send time
(`imx_upload_window.c:352-353`), not during header setup. Both `_for_can()` and
`_for_tsd_upload()` set `response_processing_method = IMX_RUN_RESPONSE_CALLBACK`
identically. The CoAP message ID and token generation are the same in both
functions. ACK matching is done by message ID and token, not by URI path.

#### 6. Build Configuration Combinations (PASS)

- **CAN_PLATFORM defined**: The changed line is reached. `_for_can()` is defined
  under `#ifdef CAN_PLATFORM` (line 878). `get_can_serial_no()` is declared in
  `canbus/can_imx_upload.h` (line 66), included via `imx_upload_internal.h`
  (line 60). `imx_coap_message_composer_utility.h` (with `_for_can()` declaration
  at line 147, also under `#ifdef CAN_PLATFORM`) is included at line 80. No new
  includes needed.
- **APPLIANCE_GATEWAY defined**: The `#elif` branch at line 1237 is taken
  instead. The changed line is unreachable. No impact.
- **Neither defined**: The `#else` branch at line 1246 is taken. The changed line
  is unreachable. No impact.
- **Both CAN_PLATFORM and APPLIANCE_GATEWAY defined**: Not a valid configuration
  (mutually exclusive `#ifdef`/`#elif`). No concern.

#### 7. Dead Macro in `_for_can()` Function (ADVISORY)

At `imx_coap_message_composer_utility.c:882`, the macro
`COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` is defined as `"tsd/%PRIu32/%010PRIu32"`
but is never referenced anywhere in the codebase. The function body at line 901
correctly uses `COAP_PUT_ENDPOINT_SYNC_UPSTREAM` (the selfreport format). This
dead macro is harmless but could confuse future readers into thinking CAN uploads
use a TSD URI. Consider removing it as a cleanup in this change.

---

### Summary

The plan is correct, minimal, and well-analyzed. The one-line change correctly
wires up the existing `imx_setup_coap_sync_packet_header_for_can()` function,
which already uses the selfreport URI format and accepts a serial number
parameter. All include dependencies are already satisfied. The change is
properly scoped to CAN_PLATFORM only and does not affect APPLIANCE_GATEWAY or
default builds.

**Two advisory items (neither blocks the change):**

1. **Server-side confirmation**: Validate that the iMatrix cloud server accepts
   `selfreport/<mfr_id>/<can_sn>/0` for CAN device data before production
   deployment. This is a deployment checklist item, not a code issue.

2. **Dead macro cleanup**: The unused `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` macro
   at `imx_coap_message_composer_utility.c:882` should be removed to avoid
   confusion. This is optional but recommended as part of this change.
