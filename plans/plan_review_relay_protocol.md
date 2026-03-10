# DCU Phone-Home Relay Plan Review: Protocol Correctness -- Round 2

## Review Date: 2026-03-09
## Plan Version: 4.0
## Reviewer: Protocol Correctness (correctness-reviewer)
## Overall: PASS

---

### Round 1 Finding Re-verification

#### Finding 1 (UDS PDU Construction): PASS in Round 1 --> PASS

No changes needed. Verified again: plan PDU layout (44 bytes: `31 01 F0 A0 <nonce:8> <hmac:32>`) matches `phonehome_handler.c` defines at lines 42-47 (`MIN_PDU_LEN=44`, `NONCE_OFFSET=4`, `HMAC_OFFSET=12`). No issues.

---

#### Finding 2 (DoIP Logical Addressing): PASS in Round 1 --> PASS

No changes needed. Plan still uses `doip_client_init(&client, NULL)` which gets default `ecu_address=0x0001`. Matches DCU config. No issues.

---

#### Finding 3 (CoAP URI Format): PASS in Round 1 --> PASS

No changes needed. URI format `/remote_call_home/%u` using CAN Controller SN as unsigned decimal is consistent with Bastion `device_id_int` convention. No issues.

---

#### Finding 4 (HMAC Computation): PASS in Round 1 --> PASS

No changes needed. Plan Step 6 calls `hmac_sha256(ctx.hmac_secret, 32, nonce, 8, hmac)`. DCU handler at line 344 calls `hmac_sha256(hmac_secret, sizeof(hmac_secret), nonce, NONCE_LEN, exp_hmac)`. Both compute HMAC over the 8-byte nonce with the 32-byte key. No issues.

---

#### Finding 5 (Nonce Generation): PASS in Round 1 --> PASS

No changes needed. 8 bytes from `/dev/urandom`, matches `NONCE_LEN=8`. No issues.

---

#### Finding 6 (DoIP Discovery): PASS in Round 1 --> PASS

No changes needed. Discovery API usage and port override to 13400 are correct. No issues.

---

#### Finding 7 (Response Handling): PASS in Round 1 --> PASS

No changes needed. Verified again against `phonehome_handler.c` lines 470-474: positive response is `71 01 F0 A0 02` (5 bytes). Plan checks `resp_len >= 5 && resp[0] == 0x71 && resp[4] == 0x02`. NRC check `resp_len >= 3 && resp[0] == 0x7F` with `resp[2]` as NRC matches `build_nrc()` output of `7F 31 NRC`. No issues.

---

#### Finding 8 (host_coap_entries Array Overflow): FAIL in Round 1 --> PASS

**Original issue:** `host_coap_entries[1]` was an out-of-bounds write on a `[1]`-sized array.

**Fix applied in v4.0:** Step 4 now explicitly resizes the array to `[2]` with a bold "CRITICAL" callout. The plan states: `static CoAP_entry_t host_coap_entries[2];  /* was [1] */`. The existing source at line 254 of `remote_access.c` confirms the current declaration is `[1]`, so this change is required and correctly specified.

**Verdict:** PASS. The fix directly addresses the buffer overflow.

---

#### Finding 9 (imx_set_host_coap_interface Re-registration): CONCERNS in Round 1 --> PASS

**Original issue:** Unclear whether `imx_set_host_coap_interface()` supports being called a second time with a larger count.

**Fix applied in v4.0:** The review history table (fix #12) states: "To verify during implementation -- `imx_set_host_coap_interface()` behavior." The plan also adds a memory barrier (`__sync_synchronize()`) before the re-registration call, and builds the entry in a local variable before copying to the array. This is a pragmatic approach: the concern is acknowledged, deferred to implementation-time verification, and the code is structured defensively to minimize risk (fully populated entry before making it visible).

**Verdict:** PASS. The plan cannot resolve this without testing the iMatrix API, and it correctly flags it as an implementation-time verification item. The defensive coding (local var + barrier + struct copy) is the right approach regardless.

---

#### Finding 10 (HMAC Secret Path Mismatch): CONCERNS in Round 1 --> PASS

**Original issue:** No documentation that the same 32-byte secret must be provisioned to both FC-1 and DCU.

**Fix applied in v4.0:** A dedicated "HMAC Secret Provisioning" section has been added to the plan (after the File Changes Summary table). It explicitly states:
- DCU path: `/etc/phonehome/hmac_secret`
- FC-1 path: `/etc/phonehome/hmac_secret`
- Generation command: `dd if=/dev/urandom bs=32 count=1 of=...`
- Mismatch consequence: NRC 0x35

**Verdict:** PASS. The provisioning requirement is now fully documented with actionable deployment instructions.

---

#### Finding 11 (Routing Activation Without Custom Config): PASS in Round 1 --> PASS

No changes needed. No issues.

---

#### Finding 12 (DoIP Client Lifecycle After Discovery): PASS in Round 1 --> PASS

No changes needed. Lifecycle sequence (init, discover, connect, activate, send_uds, destroy) is correct. No issues.

---

#### Finding 13 (Blocking DoIP Operation in Main Loop): PASS in Round 1 --> PASS

**Improvement in v4.0:** The blocking time estimate was updated from "~2s" to "worst case ~14s" (3s UDP discovery + TCP connect + routing activation + 10s UDS response timeout). This is documented in both D5 (Design Decisions) and the `relay_dcu_phonehome()` function doc comment. The accuracy improvement is appreciated.

**Verdict:** PASS.

---

#### Finding 14 (Discovery Timeout When Multiple DoIP Entities): CONCERNS in Round 1 --> PASS

**Original issue:** No VIN/EID filtering; the plan accepts the first discovery responder.

**Fix applied in v4.0:** Added to the "Deferred" table at the bottom of the plan: "Discovery validation (VIN/EID filtering) -- Single DCU on LAN assumption is valid for current deployment; would need `doip_client_discover_by_eid()` API verification." The assumption is now explicitly documented.

**Verdict:** PASS. For a single-DCU deployment this is acceptable, and the limitation is now documented with a clear path forward if multi-DCU support is needed.

---

### Summary

All 14 Round 1 findings have been re-verified against plan v4.0:

| # | Finding | Round 1 | Round 2 | Notes |
|---|---------|---------|---------|-------|
| 1 | UDS PDU Construction | PASS | PASS | No change needed |
| 2 | DoIP Logical Addressing | PASS | PASS | No change needed |
| 3 | CoAP URI Format | PASS | PASS | No change needed |
| 4 | HMAC Computation | PASS | PASS | No change needed |
| 5 | Nonce Generation | PASS | PASS | No change needed |
| 6 | DoIP Discovery | PASS | PASS | No change needed |
| 7 | Response Handling | PASS | PASS | No change needed |
| 8 | host_coap_entries OOB | FAIL | PASS | Array resized to [2] in Step 4 |
| 9 | CoAP Re-registration API | CONCERNS | PASS | Deferred to implementation with defensive coding |
| 10 | HMAC Secret Provisioning | CONCERNS | PASS | Provisioning section added |
| 11 | Routing Activation | PASS | PASS | No change needed |
| 12 | Client Lifecycle | PASS | PASS | No change needed |
| 13 | Blocking Time Estimate | PASS | PASS | Updated to accurate ~14s worst case |
| 14 | Multi-DCU Discovery | CONCERNS | PASS | Single-DCU assumption documented |

### Verification Notes

Verified the following source files to validate plan claims:

- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/remote_access/remote_access.c` -- confirmed `host_coap_entries[1]` at line 254, `remote_access_ctx_t` struct at lines 171-183, `imx_set_host_coap_interface(1, ...)` call at line 331
- `/home/greg/iMatrix/DOIP/DoIP_Server/src/phonehome_handler.c` -- confirmed PDU defines (lines 42-47), HMAC computation (line 344), positive response format (lines 470-474), `build_nrc()` (line 86)
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/DoIP_Client/libdoip/doip_client.h` -- confirmed `doip_client_t` struct (lines 52-59) with `recv_buf[4104]` making it ~4.1 KB
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/DoIP_Client/libdoip/doip_client.c` -- confirmed `doip_client_send_uds()` signature (line 581), `malloc()` in `doip_client_send_diagnostic()` (line 551)
- `/home/greg/iMatrix/DOIP/DoIP_Server/src/hmac_sha256.c` -- confirmed `explicit_bzero()` usage (lines 219-223) and `_DEFAULT_SOURCE` define (line 8)

All protocol correctness claims in the plan match the actual source code. No new issues found.
