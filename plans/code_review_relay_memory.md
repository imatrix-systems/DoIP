# DCU Phone-Home Relay Code Review: Memory Safety

## Review Date: 2026-03-09
## Reviewer: Memory Safety (correctness-reviewer)
## Overall: PASS

### Findings

**1. `host_coap_entries[2]` array bounds -- PASS**

The array is sized for exactly 2 entries: `[0]` for FC-1 phone_home (populated in `remote_access_init()` at line 343), `[1]` for DCU phone_home relay (populated in `try_register_dcu_uri()` at line 1529). The `imx_set_host_coap_interface()` calls correctly pass count=1 initially (line 355) and count=2 after DCU registration (line 1534). No out-of-bounds access.

**2. `relay_dcu_phonehome()` -- static `doip_client_t`, all error paths call `destroy` -- PASS**

The `doip_client_t client` is declared `static` (line 1614) to avoid placing ~4.1 KB (`recv_buf[4104]`) on the stack. This is appropriate for an embedded target. Since `relay_dcu_phonehome()` is only called from the main loop (`state_idle` line 634 and `state_connected` line 741), there is no reentrancy risk.

Every error path correctly calls `doip_client_destroy(&client)` before returning:
- Discovery failure (line 1623)
- `/dev/urandom` failure (line 1641)
- TCP connect failure (line 1662)
- Routing activation failure (line 1669)
- Normal completion (line 1686)

`doip_client_init()` performs `memset(client, 0, sizeof(*client))` (doip_client.c line 137), so the static variable is fully reinitialized on each call. `doip_client_destroy()` closes any open sockets. Reuse is safe.

**3. `init_load_hmac_secret()` -- fd leak on error paths -- PASS**

The function opens the file at line 1467. Error paths:
- `fstat()` failure or permission check failure (line 1475-1478): `close(fd)` called before return. No leak.
- Wrong file size (line 1483-1485): `close(fd)` was called on line 1482, before the size check. No leak.
- Success path: `close(fd)` called on line 1482 before setting `hmac_loaded`. No leak.

All paths properly close the fd.

**4. `try_register_dcu_uri()` -- struct copy correctness -- PASS**

A local `CoAP_entry_t dcu_entry` is fully initialized with `memset` (line 1522) then populated. The struct copy `host_coap_entries[1] = dcu_entry` (line 1529) is a plain C struct assignment, which is correct for POD types. The URI pointer (`dcu_entry.node.uri`) points to `ctx.dcu_phone_home_uri` which has process lifetime -- no dangling pointer.

The `__sync_synchronize()` memory barrier (line 1532) before `imx_set_host_coap_interface(2, ...)` ensures the struct write is visible to the CoAP receive thread before the count is updated. Correct ordering.

**5. `coap_post_dcu_phone_home()` -- no memory issues -- PASS**

The handler only sets `ctx.dcu_phonehome_triggered = true` (line 1565) and builds a CoAP response. No allocations, no buffer operations, no pointer arithmetic. The `_Atomic bool` ensures the store is visible to the main loop thread.

**6. PDU construction -- buffer sizes, memcpy bounds -- PASS**

PDU buffer `pdu[44]` at line 1651. Contents:
- Bytes 0-3: SID + subFunction + routineIdentifier (4 bytes, direct assignment)
- Bytes 4-11: nonce (8 bytes, `memcpy(pdu + 4, nonce, 8)`) -- 4+8=12, within bounds
- Bytes 12-43: HMAC (32 bytes, `memcpy(pdu + 12, hmac, 32)`) -- 12+32=44, exactly fills buffer

The `nonce[8]` buffer at line 1636 is read from `/dev/urandom` with `sizeof(nonce)` = 8 bytes. The `hmac[32]` buffer at line 1647 receives the HMAC-SHA256 output (always 32 bytes per `SHA256_DIGEST_SIZE`). All bounds are correct.

`doip_client_send_uds(&client, pdu, sizeof(pdu), resp, sizeof(resp), 10000)` passes `sizeof(pdu)=44` as request length and `sizeof(resp)=64` as response buffer size. The 44-byte request is well within `DOIP_MAX_DIAGNOSTIC_SIZE` (4096). The 64-byte response buffer is sufficient for UDS RoutineControl positive response (5+ bytes) or NRC (3 bytes).

**7. `_Atomic bool` usage correctness -- PASS**

`dcu_phonehome_triggered` is `_Atomic bool` (line 195), correctly used for cross-thread signaling between the CoAP receive thread (writer in `coap_post_dcu_phone_home`, line 1565) and the main loop thread (reader/clearer in `state_idle` line 632 and `state_connected` line 739). `<stdatomic.h>` is included (line 80).

The clear on line 415 (`ctx.dcu_phonehome_triggered = false`) and reads on lines 632/739 use simple assignment/read, which is valid for `_Atomic bool` in C11 -- these are implicitly `memory_order_seq_cst` operations.

Note: `memset(&ctx, 0, sizeof(ctx))` on line 313 zeroes the `_Atomic bool` via byte-level zeroing rather than atomic store. This is technically not guaranteed by the C11 standard for atomic types, but is safe in practice on all real ARM and x86 targets where `_Atomic bool` has the same representation as `bool`. Since this only happens during single-threaded initialization (before CoAP threads exist), there is no race. Acceptable.

**8. `/dev/urandom` fd handling -- PASS**

Line 1637-1644: The urandom fd open and read are handled correctly. If `open` fails (`urand_fd < 0`), the conditional `if (urand_fd >= 0) close(urand_fd)` on line 1640 correctly avoids closing an invalid fd. If `read` returns short, the fd is still closed. `doip_client_destroy` is called on the error path (line 1641).

**9. `phone_home_triggered` is NOT `_Atomic` -- WARNING (pre-existing, not new code)**

The FC-1's own `phone_home_triggered` flag (line 187) is a plain `bool`, yet it is written by the CoAP thread (`coap_post_remote_call_home`, line 1385) and read by the main loop (`state_idle`, line 625). This is a data race under C11 rules. The new DCU code correctly used `_Atomic bool` for `dcu_phonehome_triggered`, but the original `phone_home_triggered` has the same cross-thread pattern without atomic qualification. This is a pre-existing issue, not introduced by the DCU code.

**10. Cooldown arithmetic -- PASS**

`dcu_relay_cooldown` is `imx_time_t` (milliseconds). `SEC_TO_MS(60)` = `((uint32_t)(60) * 1000U)` = 60000. The comparison `now < ctx.dcu_relay_cooldown` (line 1606) and the remaining-time calculation `(ctx.dcu_relay_cooldown - now) / 1000` (line 1608) are correct, assuming `imx_time_t` does not wrap during the 60-second window (which is safe for any reasonable time representation).

**11. Discovery uses single-result buffer -- PASS**

`doip_client_discover(&client, NULL, &discovery, 1, 3000)` at line 1620 passes `max_results=1` with a single `doip_discovery_result_t discovery` on the stack. No overflow possible -- the function will write at most 1 result.

**12. Response buffer access guards -- PASS**

Line 1678: `resp_len >= 5 && resp[0] == 0x71 && resp[4] == 0x02` -- the length check guards the array accesses. If `doip_client_send_uds` returns a negative error code, `resp_len >= 5` is false and short-circuits. Line 1680: `resp_len >= 3 && resp[0] == 0x7F` then `resp[2]` -- similarly guarded. No out-of-bounds read.

### Summary

All new DCU phone-home relay code passes memory safety review. Buffer sizes are correct, all file descriptors are properly closed on every path, the static `doip_client_t` avoids stack pressure and is safely reinitialized on each use, `_Atomic bool` is correctly used for cross-thread signaling, and all error paths call `doip_client_destroy()`.

One pre-existing issue noted: `phone_home_triggered` (the FC-1 flag, not the new DCU flag) lacks `_Atomic` qualification despite identical cross-thread usage. This predates the DCU changes and is not a regression.
