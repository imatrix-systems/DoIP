# DCU Phone-Home Relay Plan Review: Concurrency -- Round 2

## Review Date: 2026-03-09
## Plan Version: 4.0
## Reviewer: Concurrency (risk-reviewer)
## Overall: PASS

---

### Round 1 Finding Re-verification

#### Finding 1 (was FAIL): `dcu_phonehome_triggered` is not atomic

**Original issue:** `dcu_phonehome_triggered` is written from the upload thread (CoAP handler) and read/cleared from the main loop, but declared as plain `bool`. This is a data race under C11.

**Fix applied in v4.0:** Step 2 now declares `_Atomic bool dcu_phonehome_triggered;` in the context struct. Step 5.5 adds `#include <stdatomic.h>`. The CoAP handler (Step 5) writes `ctx.dcu_phonehome_triggered = true` and the main loop (Step 7) reads and clears it. With `_Atomic`, these are sequentially-consistent by default, providing correct cross-thread visibility.

**Verdict: PASS.** The fix is correct and consistent with the `_Atomic bool running` pattern already established in the DoIP_Server codebase.

**Residual note:** The existing `phone_home_triggered` flag remains a plain `bool` with the same latent cross-thread access pattern. This is a pre-existing defect, not introduced by this plan. The plan is not obligated to fix it, but the implementer should consider fixing it at the same time since the pattern and include are already in place.

---

#### Finding 2 (was FAIL): Lazy URI registration races with CoAP server dispatch

**Original issue:** `try_register_dcu_uri()` wrote to `host_coap_entries[1]` field-by-field (memset + individual assignments), then called `imx_set_host_coap_interface(2, ...)`. The upload thread could observe count=2 while the struct was partially written, potentially dereferencing a NULL `post_function` pointer.

**Fix applied in v4.0:** Step 4 now:
1. Builds the entry in a local `CoAP_entry_t dcu_entry` variable.
2. Copies it to `host_coap_entries[1]` via a single struct assignment (`host_coap_entries[1] = dcu_entry`).
3. Issues `__sync_synchronize()` (full memory barrier).
4. Only then calls `imx_set_host_coap_interface(2, host_coap_entries)`.

The barrier ensures that all stores from the struct copy are globally visible before the count update inside `imx_set_host_coap_interface()`. The reader thread (line 315 of `coap_recv.c`) reads `icb.no_host_coap_entries` and then indexes into `icb.coap_entries`. If it sees count=2, the barrier guarantees the struct data is fully committed.

The struct copy itself (`host_coap_entries[1] = dcu_entry`) is not atomic -- it copies multiple words. However, this is safe because the count is still 1 during the copy. The reader only accesses `host_coap_entries[1]` after observing count=2, which happens after the barrier.

**Verdict: PASS.** The local-variable + struct-copy + barrier + registration ordering is a correct solution. The `__sync_synchronize()` is a full barrier (equivalent to `atomic_thread_fence(memory_order_seq_cst)`) and is the strongest possible guarantee. On ARM this emits a `dmb ish` instruction.

---

#### Finding 3 (was FAIL): `relay_dcu_phonehome()` blocks the main loop for ~2-14+ seconds

**Original issue:** The plan stated "Synchronous operation (~2s)" but actual worst case was 3s discovery + unbounded TCP connect + 10s UDS timeout = 14+ seconds (potentially 75+ seconds if TCP connect blocks on an unreachable but routable IP). During this time, CAN bus processing, sensor collection, and CoAP status pushes are stalled.

**Fix applied in v4.0:** Decision D5 and the function documentation now explicitly state "worst case ~14s (3s UDP discovery timeout + TCP connect + 10s UDS response timeout)". The rationale states this is "Acceptable for an infrequent, operator-initiated action."

**Verdict: PASS with caveat.** The plan now accurately documents the blocking duration rather than understating it. The justification (infrequent, operator-initiated) is reasonable -- this is not a periodic background task but a manual trigger from a human operator.

**Residual concern:** The TCP `connect()` call in the DoIP client library (`doip_client.c` line 380+) uses a blocking connect with no `SO_SNDTIMEO` or non-blocking connect + select pattern. If the DCU IP from discovery is routable but the DCU crashes between discovery and connect, the connect could block for the OS default TCP timeout (20-75 seconds on Linux, typically shorter on embedded Linux). The 60-second rate limiter (Finding 8 fix) prevents repeated stalls but does not cap a single stall. This is a pre-existing limitation of the DoIP client library, not something this plan can reasonably fix. The implementer should be aware of it and may want to set `SO_SNDTIMEO` on the socket before calling connect if the library API permits it.

---

#### Finding 4 (was PASS): State machine transitions: DCU relay vs FC-1 phone-home

**Original assessment:** No conflict between DCU relay and FC-1 tunnel state transitions. The DCU relay does not change state machine state and is fire-and-forget.

**Re-verification in v4.0:** The wiring in Step 7 is unchanged in substance. `state_idle()` checks FC-1 trigger first, then DCU trigger. `state_connected()` also checks DCU trigger independently. The ordering is correct: if both flags are set in `state_idle()`, the FC-1 transition to CONNECTING happens first (just sets state), then `relay_dcu_phonehome()` runs synchronously. On the next iteration the state machine is in CONNECTING, which does not check DCU triggers -- but that is fine because the trigger was already consumed.

**Verdict: PASS.** No change from Round 1.

---

#### Finding 5 (was FAIL): Flag variables are plain `bool`, not atomic or volatile

**Original issue:** Three new bools added: `dcu_phonehome_triggered` (cross-thread), `dcu_uri_registered` (main loop only), `hmac_loaded` (write-once before cross-thread use).

**Fix applied in v4.0:** Step 2 declares `_Atomic bool dcu_phonehome_triggered`. The other two (`dcu_uri_registered`, `hmac_loaded`) remain plain `bool`, which is correct per the Round 1 analysis: they are accessed from a single thread or are write-once-then-read.

**Verdict: PASS.** Only the field that requires atomicity was changed.

---

#### Finding 6 (was PASS): Resource cleanup in `relay_dcu_phonehome()`

**Re-verification in v4.0:** The function in Step 6 calls `doip_client_destroy(&client)` on every exit path:
- After discovery failure (line "found <= 0")
- After `/dev/urandom` failure (with `close(urand_fd)` guard)
- After connect failure
- After routing activation failure
- After UDS exchange (final call, unconditional)

The `doip_client_t client` is now `static` (per Finding from embedded review), so it persists across calls. `doip_client_destroy()` closes sockets and resets state, making it safe for reuse on the next call.

**Verdict: PASS.** No change from Round 1.

---

#### Finding 7 (was FAIL): `host_coap_entries` array size is [1] but plan indexes [1]

**Original issue:** Out-of-bounds write on a size-1 array. Memory corruption.

**Fix applied in v4.0:** Step 4 now explicitly states the array must be resized:
```
static CoAP_entry_t host_coap_entries[2];  /* was [1] -- now holds FC-1 + DCU entries */
```
The step title is "Resize `host_coap_entries` Array and Lazy Registration" and includes a bold "CRITICAL" callout explaining the out-of-bounds consequence.

**Verdict: PASS.** The fix is explicit and impossible to miss during implementation.

---

#### Finding 8 (was FAIL): No rate limiting on DCU relay triggers

**Original issue:** Rapid CoAP triggers could cause repeated 2-14 second main loop stalls with no cooldown.

**Fix applied in v4.0:**
- Step 2 adds `imx_time_t dcu_relay_cooldown` to the context struct.
- Step 6 defines `DCU_RELAY_COOLDOWN_SEC 60` and implements a cooldown check at the top of `relay_dcu_phonehome()`. If `now < ctx.dcu_relay_cooldown`, the relay is skipped with a log message showing remaining cooldown time. The cooldown is set to `now + 60` immediately after passing the check (before the relay runs), so even a failed relay consumes the cooldown window.

**Verdict: PASS.** The 60-second cooldown is reasonable for an operator-initiated action. Setting the cooldown before the relay (rather than after) prevents starvation even on repeated failures.

---

#### Finding 9 (was PASS): HMAC secret is not leaked in logs

**Re-verification in v4.0:** No changes affect secret handling in logs. The HMAC value, nonce, and secret are never logged. Error messages log NRC codes and response lengths only.

**Verdict: PASS.** No change from Round 1.

---

### New Observations (v4.0)

#### N1: `static doip_client_t client` is not re-entrant

The `relay_dcu_phonehome()` function declares `static doip_client_t client` to avoid stack pressure. Since the function is only called from the main loop (single-threaded), there is no reentrancy risk from concurrent calls. However, if a signal handler were to call this function, it would corrupt the static state. This is not a real risk because signal handlers in this codebase do not call relay functions.

**Verdict: Not an issue.** Main-loop-only access is guaranteed by design.

#### N2: `imx_set_host_coap_interface` stores pointer, not copy

Verified at `/home/greg/iMatrix/DOIP/iMatrix/coap/imx_coap.c` lines 114-115: the function stores the pointer `icb.coap_entries = host_coap_entries` and the count. Since the plan uses the same static array as the initial registration, the pointer value does not change between the first call (count=1) and the second call (count=2). Only the count is updated. This means there is no window where the reader could see a new pointer with stale data -- the array is always at the same address.

**Verdict: Not an issue.** The pointer stability eliminates one class of race conditions.

#### N3: Count written before pointer in `imx_set_host_coap_interface`

Inside the function, `icb.no_host_coap_entries` is written at line 114 before `icb.coap_entries` at line 115. In theory, if these were independent (different pointer), a reader could see the new count with the old pointer. In practice, the pointer does not change (same static array), so the order is irrelevant. The `__sync_synchronize()` before the function call ensures the struct data is visible before either write.

**Verdict: Not an issue.** Pointer identity makes the write order irrelevant.

---

### Summary

All 5 FAILs from Round 1 have been adequately addressed in v4.0:

| # | Finding | Round 1 | Round 2 | Fix Quality |
|---|---------|---------|---------|-------------|
| 1 | Non-atomic `dcu_phonehome_triggered` | FAIL | PASS | Correct: `_Atomic bool` with `<stdatomic.h>` |
| 2 | Lazy registration race condition | FAIL | PASS | Correct: local var + struct copy + `__sync_synchronize()` + then register |
| 3 | Main loop blocking understated | FAIL | PASS | Documented ~14s worst case; rate-limited; operator-initiated justification |
| 4 | State machine transitions | PASS | PASS | No change needed |
| 5 | Non-atomic flag variables | FAIL | PASS | Only cross-thread flag made atomic; others correctly left as plain `bool` |
| 6 | Resource cleanup | PASS | PASS | No change needed |
| 7 | Array out-of-bounds write | FAIL | PASS | Explicit resize to `[2]` with CRITICAL callout |
| 8 | No rate limiting | FAIL | PASS | 60-second cooldown with log message |
| 9 | HMAC not leaked in logs | PASS | PASS | No change needed |

The plan is sound from a concurrency perspective. The remaining residual concerns (pre-existing `phone_home_triggered` non-atomicity, DoIP client blocking connect timeout) are pre-existing issues outside the scope of this plan.
