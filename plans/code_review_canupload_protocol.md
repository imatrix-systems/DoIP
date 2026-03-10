# CAN Upload Code Review: Protocol

## Review Date: 2026-03-09

## Overall: PASS

### Findings

#### 1. URI Format -- CORRECT

The `_for_can` function (line 901 of `imx_coap_message_composer_utility.c`) calls:

```c
snprintf((char *)&uri_path, 50, COAP_PUT_ENDPOINT_SYNC_UPSTREAM,
        device_config.manufactuer_id, serial_number);
```

`COAP_PUT_ENDPOINT_SYNC_UPSTREAM` is defined in `imx_coap.h` line 60 as:

```c
"selfreport/%"PRIu32"/%010"PRIu32"/0"
```

This produces `selfreport/<mfr_id>/<can_sn_10digits>/0` -- confirmed correct. The `%010` format pads the serial number to exactly 10 digits with leading zeros, matching the gateway selfreport format.

#### 2. CoAP Method -- CORRECT (PUT)

Line 914: `ptr->coap.header.mode.udp.code = ( REQUEST << 5 ) | PUT;`

This is identical to the gateway function at line 864. PUT is the correct method for selfreport.

#### 3. Content Format -- CORRECT (BINARY_MEDIA_TYPE)

Lines 906-907: `add_coap_uint_option( CONTENT_FORMAT, BINARY_MEDIA_TYPE, ... )`

Matches the gateway function at lines 856-857.

#### 4. URI_PATH Option Construction -- IDENTICAL to Gateway

The `_for_can` function (lines 893-907) uses the same pattern as `imx_setup_coap_sync_packet_header` (lines 843-857):

- Same `options[MAX_OPTIONS_LENGTH]` buffer with `memset` init
- Same `add_coap_str_option(URI_PATH, ...)` call
- Same `add_coap_uint_option(CONTENT_FORMAT, BINARY_MEDIA_TYPE, ...)` call
- Same CoAP header fields: ver=1, CONFIRMABLE, PUT, message_id++, tkl=4, UDP transport
- Same token and options packing into `data_block->data`
- Same PAYLOAD_START marker

The only difference is the serial number source: the CAN function takes it as a `uint32_t` parameter directly (no `strtoul` conversion needed), while the gateway function converts `device_config.device_serial_number` from string via `strtoul`.

#### 5. Fallback tsd/ URI -- CORRECT

When `get_can_serial_no()` returns 0 (line 1239-1246 of `imx_upload_window.c`), the code falls back to `imx_setup_coap_sync_packet_header_for_tsd_upload()`. This function (line 933+) uses `COAP_PUT_ENDPOINT_TSD_UPSTREAM` defined as `"tsd/%"PRIu32"/1"`, producing `tsd/<mfr_id>/1`. It uses the same CoAP options (URI_PATH, CONTENT_FORMAT=BINARY_MEDIA_TYPE) and the same PUT method, so it is valid CoAP.

#### 6. Type Safety -- CORRECT

`get_can_serial_no()` returns `uint32_t` (declared in `canbus/can_imx_upload.h` line 66). The `_for_can` function parameter is `uint32_t serial_number`. The format specifier `%010PRIu32` matches. No type mismatch.

### Warnings (non-blocking)

1. **Dead macro on line 882**: `COAP_PUT_CAN_ENDPOINT_TSD_UPSTREAM` is defined inside the `_for_can` function but never used anywhere in the codebase. It appears to be a leftover from an earlier design where CAN uploads used a `tsd/` URI with the CAN serial number. Should be removed to avoid confusion.

2. **Preprocessor guard style on line 929 and 149 of the .h file**: `#if defined (APPLIANCE_GATEWAY) || (CAN_PLATFORM)` should be `#if defined(APPLIANCE_GATEWAY) || defined(CAN_PLATFORM)`. The current form works because `-DCAN_PLATFORM` defines it as `1`, so `(CAN_PLATFORM)` evaluates to `(1)`. But if the define were ever changed to `-DCAN_PLATFORM=0` or similar, the guard would fail. This is a pre-existing issue, not introduced by this change.

### Summary

The CAN upload selfreport URI change is protocol-correct. The `_for_can` function produces the URI `selfreport/<mfr_id>/<can_sn_10digits>/0` using PUT with BINARY_MEDIA_TYPE content format, which is structurally identical to the gateway selfreport path. The CoAP packet construction (header, token, options, payload marker) is a line-for-line match with the existing gateway function. The SN=0 guard with tsd/ fallback is a sound defensive measure. Two minor dead-code / preprocessor-style issues noted but neither affects correctness.
