# DCU Phone-Home Relay Code Review: Security

## Review Date: 2026-03-09
## Reviewer: Security (risk-reviewer)
## Overall: PASS

All six verification items pass. Two medium-severity observations and several low-severity notes are documented below for consideration but none warrant a FAIL verdict.

---

### Findings

**1. HMAC secret file loading: O_NOFOLLOW + fstat + permission checks -- PASS**

`init_load_hmac_secret()` at line 1465 of `remote_access.c` correctly:
- Opens with `O_RDONLY | O_NOFOLLOW` (prevents symlink-to-world-readable-file attacks)
- Uses `fstat()` on the open fd (not `stat()` on the path, which would be a TOCTOU race)
- Rejects non-regular files via `!S_ISREG(st.st_mode)`
- Rejects group/other permissions via `(st.st_mode & 0077)`
- Closes the fd on every error path
- Does not log the secret contents

One minor note: the `read()` call at line 1481 does not loop on `EINTR`. On Linux reading 32 bytes from a regular file is extremely unlikely to be interrupted or return a short read, so this is acceptable for an embedded target. If this code were ever ported to a network-backed filesystem, a retry loop would be needed.

**2. No buffer overflows in PDU construction or URI formatting -- PASS**

- `pdu[44]` at line 1651: exactly 4 (header) + 8 (nonce) + 32 (hmac) = 44 bytes. All `memcpy` offsets and sizes are compile-time constants that sum correctly. No user-controlled length.
- `snprintf(ctx.dcu_phone_home_uri, sizeof(...), "/remote_call_home/%u", can_sn)` at line 1513: a `uint32_t` formatted as `%u` produces at most 10 digits. `/remote_call_home/` is 19 chars. Total worst case: 19 + 10 + 1 (null) = 30 bytes, well within the 64-byte buffer.
- `resp[64]` at line 1674: passed to `doip_client_send_uds()` with `sizeof(resp)` as the size limit. The library is documented to respect this bound.
- `dcu_ip[INET_ADDRSTRLEN]` at line 1628: `inet_ntop()` is given `sizeof(dcu_ip)` and will not overflow.

**3. CoAP handler rejects plain (non-DTLS) triggers -- PASS**

`coap_post_dcu_phone_home()` at line 1560 checks `is_plain_coap_mode()` and returns `COAP_NO_RESPONSE` if true, identical to the existing FC-1 handler at line 1373. The security log message clearly identifies the rejection. This prevents an attacker on the local network from injecting phone-home triggers over unencrypted CoAP.

**4. Nonce generated from /dev/urandom with proper error handling -- PASS**

At line 1637: opens `/dev/urandom` with `O_RDONLY`, checks for open failure (`fd < 0`), checks for short read (`!= (ssize_t)sizeof(nonce)`), closes the fd on the error path before returning, and calls `doip_client_destroy()` to clean up the client. The cast to `ssize_t` prevents a signed/unsigned comparison warning -- correctly done.

8 bytes of nonce provides 64 bits of entropy per relay attempt. Combined with the 60-second cooldown, this is adequate against replay attacks for this use case. An attacker would need to observe the nonce+HMAC pair on the wire (DoIP TCP on the local LAN, not encrypted) and replay it before the DCU processes it. See finding 8 for discussion.

**5. HMAC secret not leaked in logs -- PASS**

Searched all `REMOTE_LOG` calls in the DCU relay code. None log `ctx.hmac_secret`, the computed `hmac[]`, or the `nonce[]`. The only logged values are: status messages, IP addresses, port numbers, NRC codes, and response lengths. The HMAC secret remains confidential in logs.

**6. No command injection or path traversal vulnerabilities -- PASS**

- The HMAC secret path `/etc/phonehome/hmac_secret` is a hardcoded constant (line 1467). No user input influences the path.
- The CAN Controller SN used in the URI comes from `imx_get_can_controller_sn()` which returns a `uint32_t`. Formatted with `%u`, this cannot contain path separators, null bytes, or shell metacharacters.
- The DCU IP address comes from `inet_ntop()` which produces only digits and dots. Passed directly to `doip_client_connect()` as a string -- no shell invocation.
- No `system()`, `popen()`, or `exec*()` calls in the new code.

**7. Discovery accepts any DoIP responder on the LAN -- MEDIUM (accepted risk)**

`doip_client_discover()` at line 1620 accepts the first DoIP entity that responds to the UDP broadcast. If a rogue device on the LAN responds first, the FC-1 would connect to it and send the nonce+HMAC. The attacker would learn an HMAC for a nonce they did not choose (the FC-1 chose it), which does not directly compromise the shared secret (HMAC-SHA256 is a PRF). However, the attacker could prevent the real DCU from receiving the trigger (denial of service).

This was explicitly deferred in the plan as an accepted risk for the current single-DCU-on-LAN deployment. If multi-device LANs become possible, filtering by VIN or EID from the discovery response should be added.

Verdict: PASS (documented accepted risk, not a secret leak)

**8. DoIP TCP channel is unencrypted (nonce+HMAC visible on LAN) -- MEDIUM (design constraint)**

The relay sends the nonce and HMAC over plaintext DoIP TCP on the local vehicle LAN. An attacker with LAN access can observe the nonce+HMAC pair. Since the FC-1 generates a fresh random nonce each time and the DCU should consume it immediately (single-use), replay is mitigated by the DCU rejecting duplicate nonces. However, the plan does not document whether the DCU implements nonce replay protection.

If the DCU does NOT track used nonces, an attacker who captures a valid nonce+HMAC pair could replay it to trigger the DCU's reverse SSH tunnel at will. The 60-second cooldown on the FC-1 side does not protect against an attacker sending the captured pair directly to the DCU.

Mitigation: Confirm the DCU's RoutineControl handler rejects replayed nonces. If it does not, this becomes a replay vulnerability for an attacker with LAN access.

Verdict: PASS (the FC-1 relay code itself is correct; DCU-side replay protection is a separate implementation concern)

**9. Static `doip_client_t` reentrancy -- LOW**

`relay_dcu_phonehome()` uses `static doip_client_t client` at line 1614 to avoid stack pressure. Since this function is only called from the main loop (never from the CoAP thread), and the main loop is single-threaded, there is no reentrancy risk. The `_Atomic bool dcu_phonehome_triggered` flag correctly decouples the CoAP thread from the main loop, and the flag is cleared before `relay_dcu_phonehome()` is called (lines 633, 740), preventing re-entry.

Verdict: PASS

**10. Rate limiting implementation -- PASS**

The 60-second cooldown at line 1606 uses `imx_time_t` comparison. The cooldown is set *before* the relay attempt (line 1611), meaning even a failed attempt triggers the cooldown. This prevents an attacker from flooding the CoAP endpoint to cause repeated 14-second main loop blocks.

One edge case: if `imx_time_t` wraps (e.g., 32-bit millisecond counter wraps at ~49.7 days), the comparison `now < ctx.dcu_relay_cooldown` could produce incorrect results for one cycle. This is a pre-existing pattern in the codebase (the FC-1 tunnel uses the same time type) and is not specific to this change.

Verdict: PASS

**11. Memory barrier and lazy registration thread safety -- PASS**

`try_register_dcu_uri()` at line 1505 builds the CoAP entry in a local variable, copies it to `host_coap_entries[1]` via struct assignment, issues `__sync_synchronize()`, then calls `imx_set_host_coap_interface(2, ...)`. This ensures the CoAP receive thread sees a fully-populated entry. The `ctx.dcu_phone_home_uri` buffer is written once (line 1513) before being referenced by the CoAP entry, and persists for the process lifetime.

The `dcu_uri_registered` flag is not atomic, but it is only written and read from the main loop thread, so no atomicity is needed.

Verdict: PASS

**12. HMAC secret not cleared from memory -- LOW (deferred)**

`ctx.hmac_secret[32]` is never zeroed after use. Since the `remote_access` module runs for the device lifetime with no teardown path, this is a non-issue in practice. If a teardown path is ever added, `explicit_bzero(ctx.hmac_secret, 32)` should be called.

Already documented as deferred in the plan.

Verdict: PASS

**13. Error path resource cleanup in `relay_dcu_phonehome()` -- PASS**

Every early-return path calls `doip_client_destroy(&client)` before returning. The `/dev/urandom` fd is closed on both success and error paths (lines 1640, 1644). No resource leaks.

Verdict: PASS

---

### Summary

The implementation is well-crafted from a security perspective. All six requested verification items pass. The code follows defensive patterns throughout: hardcoded paths (no injection surfaces), bounds-checked buffers, proper fd cleanup, DTLS enforcement, rate limiting, and no secret leakage in logs.

Two medium-severity observations are noted:

1. **Discovery spoofing** (finding 7): An attacker on the vehicle LAN could respond to the DoIP broadcast before the real DCU, causing a denial-of-service. Accepted risk for single-DCU deployments.

2. **Nonce replay on unencrypted LAN** (finding 8): The nonce+HMAC travels over plaintext TCP. Security depends on the DCU implementing nonce replay rejection. This should be verified against the DCU's RoutineControl handler.

Neither observation represents a flaw in the FC-1 relay code itself. The code correctly generates fresh nonces, computes proper HMACs, enforces DTLS on the CoAP trigger path, and handles all error cases cleanly.

**Files reviewed:**
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/remote_access/remote_access.c` (lines 104-105, 180-199, 260-365, 395-422, 610-636, 720-756, 1355-1396, 1448-1687)
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/remote_access/hmac_sha256.h`
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/remote_access/hmac_sha256.c`
- `/home/greg/iMatrix/DOIP/DoIP_Server/plans/dcu_phonehome_relay_plan.md`
- `/home/greg/iMatrix/DOIP/Fleet-Connect-1/DoIP_Client/libdoip/doip_client.h` (send_uds signature)
