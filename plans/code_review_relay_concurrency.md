# DCU Phone-Home Relay Code Review: Concurrency

## Review Date: 2026-03-09
## Reviewer: Concurrency (risk-reviewer)
## File: /home/greg/iMatrix/DOIP/Fleet-Connect-1/remote_access/remote_access.c
## Overall: FAIL

---

### Findings

#### 1. FAIL -- `phone_home_triggered` is plain `bool`, not `_Atomic` (cross-thread write)

**Lines**: 187, 1385

`ctx.phone_home_triggered` is declared as plain `bool` (line 187), yet it is written by `coap_post_remote_call_home()` (line 1385) which runs on the CoAP receive thread, and read/cleared by the main loop in `state_idle()` (line 625) and `state_connected()` (line 745).

This is the exact same cross-thread pattern as `dcu_phonehome_triggered`, which correctly uses `_Atomic bool` (line 195). The omission of `_Atomic` on `phone_home_triggered` is a data race (undefined behavior per C11 5.1.2.4). On ARM (the embedded target), this is unlikely to cause visible corruption for a single bool, but it is technically UB and inconsistent with the DCU flag treatment.

**Fix**: Change `bool phone_home_triggered` to `_Atomic bool phone_home_triggered`.

---

#### 2. PASS (with caveat) -- `_Atomic bool dcu_phonehome_triggered` flag signaling

**Lines**: 195, 1565, 632-633, 739-740, 415

The DCU flag correctly uses `_Atomic bool`, providing well-defined cross-thread visibility. The CoAP thread sets it to `true` (line 1565); the main loop reads and clears it (lines 632-633, 739-740). The DTLS reset path clears it (line 415), which is fine since that also runs on the main loop.

**Caveat**: There is a narrow window where a trigger could be lost. If the CoAP thread sets the flag between the main loop's read at line 632 and the clear at line 633, the trigger would be consumed by the current iteration's `relay_dcu_phonehome()` call anyway, so this is actually safe. The clear-then-act pattern (read true, set false, act) is correct for a single-producer single-consumer boolean flag.

No issue for the intended use case. PASS.

---

#### 3. FAIL -- `try_register_dcu_uri()` modifies `host_coap_entries[1]` while CoAP thread may read

**Lines**: 1505-1538, 278, 315 (coap_recv.c)

The registration sequence is:

1. Write `host_coap_entries[1]` struct (line 1529)
2. `__sync_synchronize()` memory barrier (line 1532)
3. Call `imx_set_host_coap_interface(2, host_coap_entries)` which sets `icb.no_host_coap_entries = 2` and `icb.coap_entries = host_coap_entries` (imx_coap.c line 114-115)

The memory barrier at step 2 ensures the struct data is flushed before the count update. However, `imx_set_host_coap_interface()` performs two separate non-atomic stores:

```c
icb.no_host_coap_entries = no_coap_entries;  // store 1
icb.coap_entries = host_coap_entries;         // store 2
```

The CoAP receive thread reads both values together at coap_recv.c line 315:

```c
matched_entry = match_uri(cd.uri, icb.coap_entries, icb.no_host_coap_entries);
```

There is no corresponding acquire barrier on the reader side. On ARM, the reader could see `no_host_coap_entries = 2` but read stale data from `host_coap_entries[1]` if the compiler or CPU reorders the loads.

**Practical risk**: LOW. The pointer `icb.coap_entries` is the same pointer both times (it was already set to `host_coap_entries` at init with count=1, line 355). So `imx_set_host_coap_interface(2, ...)` only changes the count. The real question is whether the count becoming 2 is visible after the struct write. The `__sync_synchronize()` on the writer side provides a full fence, but without a corresponding fence on the reader, the C memory model does not guarantee visibility. In practice on Linux/ARM with the GCC full barrier, this is likely safe, but it is not formally correct.

**Fix**: Either (a) make `icb.no_host_coap_entries` `_Atomic` with release/acquire semantics, or (b) accept the practical safety on this platform and document the assumption.

---

#### 4. PASS -- `static doip_client_t client` in `relay_dcu_phonehome()` is reentrancy-safe

**Lines**: 1596, 1614

`relay_dcu_phonehome()` is only called from two places: `state_idle()` (line 634) and `state_connected()` (line 741). Both are in the main loop, which is single-threaded. The function cannot be called concurrently with itself.

The `static` declaration is a stack-pressure optimization (the comment explains the 4.1 KB recv buffer), not a shared-state concern. Since the function runs exclusively on the main loop thread, the static client is safe.

PASS.

---

#### 5. PASS -- Rate limiting with `dcu_relay_cooldown` has no race condition

**Lines**: 198, 1606-1611

`ctx.dcu_relay_cooldown` is read and written exclusively in `relay_dcu_phonehome()`, which runs only on the main loop thread (see Finding 4). There is no cross-thread access. The comparison `now < ctx.dcu_relay_cooldown` and subsequent update are not subject to TOCTOU races because only one thread touches this field.

PASS.

---

#### 6. PASS (with caveat) -- DTLS reset path clears `dcu_phonehome_triggered` correctly

**Lines**: 393-418

When DTLS drops, the main loop detects `!is_dtls_active()` (line 393), cleans up, and clears both trigger flags (lines 414-415). This is correct behavior: a trigger received over a DTLS session that has since dropped should be discarded, because the tunnel infrastructure (bastion connection) is no longer valid.

**Caveat**: There is a race where the CoAP thread could set `dcu_phonehome_triggered = true` (line 1565) after the main loop clears it at line 415 but before the state transitions to `REMOTE_STATE_WAIT_ONLINE` (line 417). On the next iteration, the main loop would skip the DTLS-reset check (state is already WAIT_ONLINE) and the stale trigger would persist until the state machine reaches IDLE again. At that point `relay_dcu_phonehome()` would execute, but DTLS would still be down, so the relay would likely fail at the DoIP/network level and log an error. This is a benign race -- the relay attempt fails gracefully. No data corruption or crash.

PASS.

---

#### 7. LOW -- `imx_time_t` (uint32_t) wrap-around in cooldown comparison

**Lines**: 1606, 198

`imx_time_t` is `uint32_t` milliseconds, which wraps every ~49.7 days. The comparison `now < ctx.dcu_relay_cooldown` will malfunction at the wrap boundary. If `dcu_relay_cooldown` was set near `UINT32_MAX` and `now` wraps to a small value, the condition `now < dcu_relay_cooldown` is true (cooldown appears active) and could block relay attempts for up to ~49 days.

**Practical risk**: LOW. The system would need to be running for exactly ~49 days and a trigger would need to arrive within the 60-second cooldown window that straddles the wrap point. If it does happen, the next reboot (or DTLS reset, which clears the cooldown indirectly via re-initialization) resolves it.

**Fix**: Use signed difference: `(int32_t)(now - ctx.dcu_relay_cooldown) < 0` instead of `now < ctx.dcu_relay_cooldown`. This handles wrap-around correctly for intervals under ~24 days.

---

#### 8. PASS -- `coap_post_dcu_phone_home()` DTLS check is thread-safe

**Lines**: 1559-1563

`is_plain_coap_mode()` is called on the CoAP thread. This function presumably reads a global state variable. Since DTLS mode is established at init time and does not change during operation (it only "resets" at the connection level, not the mode level), this read is safe. The check prevents trigger injection over unauthenticated channels.

PASS.

---

### Summary

**2 FAIL, 6 PASS** (including 3 with caveats)

| # | Finding | Verdict | Severity |
|---|---------|---------|----------|
| 1 | `phone_home_triggered` missing `_Atomic` | FAIL | MEDIUM |
| 2 | `dcu_phonehome_triggered` atomic flag pattern | PASS | -- |
| 3 | `host_coap_entries[1]` write vs reader barrier | FAIL | LOW |
| 4 | Static `doip_client_t` reentrancy | PASS | -- |
| 5 | Rate limiting race conditions | PASS | -- |
| 6 | DTLS reset clears trigger flag | PASS | -- |
| 7 | uint32_t wrap-around in cooldown | PASS (advisory) | LOW |
| 8 | DTLS security check thread safety | PASS | -- |

**Required fixes**:

1. **Finding 1** (MEDIUM): Add `_Atomic` to `phone_home_triggered` to match `dcu_phonehome_triggered`. This is a one-word change that eliminates a data race / undefined behavior.

2. **Finding 3** (LOW): Either add `_Atomic` to `icb.no_host_coap_entries` with appropriate acquire semantics on the reader, or add a code comment documenting that the `__sync_synchronize()` full barrier on the writer side is relied upon for practical (not formally correct) safety on this target.

**Recommended fix**:

3. **Finding 7** (LOW): Use signed-difference wrap-safe comparison for the cooldown timer to prevent a ~49-day edge case.
