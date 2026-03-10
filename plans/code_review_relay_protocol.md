# DCU Phone-Home Relay Code Review: Protocol Correctness

## Review Date: 2026-03-09
## Reviewer: Protocol Correctness (correctness-reviewer)
## Overall: PASS

### Files Reviewed

- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/remote_access/remote_access.c` (relay sender: `relay_dcu_phonehome`, `coap_post_dcu_phone_home`)
- `/home/greg/iMatrix/DOIP/DoIP_Server/src/phonehome_handler.c` (DCU receiver: `phonehome_handle_routine`)
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/DoIP_Client/libdoip/doip_client.h` and `doip_client.c` (DoIP client API)
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/remote_access/hmac_sha256.h` (HMAC API)
- `/home/greg/iMatrix/DOIP/DoIP_Server/include/hmac_sha256.h` (HMAC API, DCU side)
- `/home/greg/iMatrix/DOIP/DoIP_Server/src/main.c` (UDS dispatch)

---

### Findings

#### 1. UDS PDU Layout: PASS

**Relay sender** (remote_access.c lines 1650-1657):
```
pdu[0]  = 0x31   (SID: RoutineControl)
pdu[1]  = 0x01   (subFunction: startRoutine)
pdu[2]  = 0xF0   (routineIdentifier high byte)
pdu[3]  = 0xA0   (routineIdentifier low byte)
pdu[4..11]  = nonce  (8 bytes)
pdu[12..43] = hmac   (32 bytes)
Total = 44 bytes
```

**DCU handler** (phonehome_handler.c lines 42-47):
```
MIN_PDU_LEN  = 4 + 8 + 32 = 44
NONCE_OFFSET = 4
HMAC_OFFSET  = 4 + 8 = 12
```

**Dispatch** (main.c lines 410-416): Passes full `uds_data` (starting from SID byte) to `phonehome_handle_routine` after verifying `uds_data[1]==0x01`, `uds_data[2]==0xF0`, `uds_data[3]==0xA0`.

All offsets match. The handler reads `uds_data + 4` for nonce and `uds_data + 12` for HMAC, which aligns with the PDU layout exactly.

#### 2. HMAC Computation: PASS

**Relay sender** (remote_access.c line 1648):
```c
hmac_sha256(ctx.hmac_secret, 32, nonce, 8, hmac);
```

**DCU handler** (phonehome_handler.c line 344):
```c
hmac_sha256(hmac_secret, sizeof(hmac_secret), nonce, NONCE_LEN, exp_hmac);
```

Where `hmac_secret` is `uint8_t[SHA256_DIGEST_SIZE]` (32 bytes) and `NONCE_LEN` is 8. Both sides use the identical `hmac_sha256()` function signature from matching `hmac_sha256.h` headers. Both load a 32-byte raw binary secret file with identical validation (exact 32 bytes required, O_NOFOLLOW, permission checks).

The HMAC is computed over the nonce only (not the full PDU), which is correct -- the nonce provides freshness and the HMAC proves the sender knows the shared secret.

#### 3. DoIP Client API Lifecycle: PASS

The relay function (lines 1596-1687) follows the correct sequence:

1. `doip_client_init(&client, NULL)` -- line 1617
2. `doip_client_discover(...)` -- line 1620 (UDP broadcast discovery)
3. `doip_client_connect(&client, dcu_ip, dcu_port)` -- line 1660 (TCP connect)
4. `doip_client_activate_routing(&client, NULL)` -- line 1667
5. `doip_client_send_uds(&client, pdu, sizeof(pdu), resp, sizeof(resp), 10000)` -- line 1675
6. `doip_client_destroy(&client)` -- line 1686

Every early-return error path calls `doip_client_destroy()`. The static `doip_client_t` avoids ~4.1 KB stack allocation.

Note: Discovery is not strictly required for sending UDS (the relay could connect directly if the DCU IP is known), but it validates that a DoIP entity is present on the LAN before attempting TCP. This is a reasonable design choice.

#### 4. Response Parsing: PASS (with minor note)

**Positive response check** (line 1678):
```c
if (resp_len >= 5 && resp[0] == 0x71 && resp[4] == 0x02)
```

The handler returns (phonehome_handler.c lines 469-475):
```
response[0] = 0x71  (positive response SID)
response[1] = 0x01  (subFunction)
response[2] = 0xF0  (routineId high)
response[3] = 0xA0  (routineId low)
response[4] = 0x02  (routineStatus: routineRunning)
```

The `doip_client_send_uds()` returns raw UDS payload (confirmed in doip_client.c lines 642-649), so `resp[0]` is indeed the response SID.

The check correctly identifies a positive response. It does not validate `resp[2..3]` (routineId echo), but since only one RoutineControl is sent per session, there is no ambiguity. This is acceptable.

**Negative response check** (line 1680):
```c
else if (resp_len >= 3 && resp[0] == 0x7F)
    REMOTE_LOG("DCU phone-home relay rejected — NRC 0x%02X", resp[2]);
```

The NRC format is `{0x7F, SID, NRC}` per ISO 14229. `resp[2]` is the NRC byte. Correct.

#### 5. CoAP Response Format: PASS

**Handler** (lines 1552-1580):

- Rejects non-DTLS CoAP requests (security check, line 1560)
- Sets `dcu_phonehome_triggered = true` (atomic bool, thread-safe)
- Determines response type: ACKNOWLEDGEMENT for CON requests, NON_CONFIRMABLE otherwise
- Returns `CHANGED` (2.04) via `coap_store_response_header()`
- Returns `COAP_SEND_RESPONSE` on success, `COAP_NO_RESPONSE` on error/rejection

This follows standard CoAP semantics: CON requests get ACK responses, NON requests get NON responses. The 2.04 Changed code is appropriate for a POST that triggers an action.

#### 6. Thread Safety of Trigger Flag: PASS

The `dcu_phonehome_triggered` field is declared as `_Atomic bool` (remote_access.c line 195). The CoAP handler sets it from the CoAP receive thread; the main loop reads and clears it in `state_idle()` and `state_connected()`. The `_Atomic` qualifier ensures visibility across threads without explicit locks.

#### 7. Resource Cleanup on All Error Paths: PASS

Traced all exit paths in `relay_dcu_phonehome()`:

| Exit Point | Line | `destroy` called? |
|---|---|---|
| HMAC not loaded | 1600 | N/A (no init) |
| Cooldown active | 1609 | N/A (no init) |
| urandom fail | 1641 | Yes |
| Discover fail | 1623 | Yes |
| Connect fail | 1663 | Yes |
| Routing fail | 1669 | Yes |
| Normal exit | 1686 | Yes |

All paths that call `doip_client_init()` also call `doip_client_destroy()`.

#### 8. Nonce Generation Quality: PASS

The relay uses `/dev/urandom` for 8-byte nonce generation (lines 1637-1644). The `read()` return value is checked against the expected 8 bytes. The file descriptor is closed on both success and error paths.

#### 9. Rate Limiting: PASS

The 60-second cooldown (`DCU_RELAY_COOLDOWN_SEC`) prevents rapid repeated DoIP sessions from blocking the main loop. The cooldown is applied immediately upon entry, before any network operations.

---

### Summary

The DCU phone-home relay implementation is protocol-correct across all five verification areas:

1. **UDS PDU layout**: Byte-exact match between sender and receiver (SID 0x31, subFunc 0x01, routineId 0xF0A0, nonce[8], hmac[32] = 44 bytes).
2. **HMAC computation**: Identical function calls on both sides -- `hmac_sha256(secret, 32, nonce, 8, out)` -- using matching header files with the same API.
3. **DoIP client lifecycle**: Correct sequence (init, discover, connect, activate_routing, send_uds, destroy) with destroy on every error path.
4. **Response parsing**: Positive (0x71 + status 0x02) and negative (0x7F + NRC at byte 2) correctly handled.
5. **CoAP response**: 2.04 Changed with proper ACK for CON / NON for NON-CON, DTLS-only enforcement.

No protocol-level bugs found. The relay and handler will interoperate correctly when sharing the same 32-byte HMAC secret file.
