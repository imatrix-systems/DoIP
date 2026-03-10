# DCU Phone-Home Relay Plan Review: Security — Round 2

## Review Date: 2026-03-09
## Plan Version: 4.0
## Reviewer: Security (risk-reviewer)
## Overall: PASS (all HIGH findings resolved; 2 accepted deferrals with adequate justification)

---

### Round 1 Finding Re-verification

#### Finding 1: HMAC secret stored in plaintext without file permission enforcement on FC-1

**Original issue:** FC-1 `init_load_hmac_secret()` used bare `open(..., O_RDONLY)` without `O_NOFOLLOW`, without `fstat()` permission check, and without `explicit_bzero()` on teardown.

**Fix applied (Step 3, v4.0):** The function now uses `O_RDONLY | O_NOFOLLOW`, calls `fstat()` and checks `!S_ISREG(st.st_mode) || (st.st_mode & 0077)`. This is actually **stricter** than the DCU side, which only checks `S_IROTH | S_IWOTH` (world-accessible). The FC-1 plan rejects any group or other permissions (mask `0077`), which is the correct conservative choice since the HMAC secret should be root-readable only. The `explicit_bzero()` on teardown is listed as deferred because there is no teardown path in `remote_access.c` — the module runs for the device lifetime. This is an acceptable deferral given the embedded context (no core dumps, physical access required).

**Verdict: PASS**

---

#### Finding 2: host_coap_entries array out-of-bounds write

**Original issue:** `host_coap_entries[1]` was an OOB write on a 1-element array.

**Fix applied (Step 4, v4.0):** Array declaration changed to `static CoAP_entry_t host_coap_entries[2]`. The plan includes a "CRITICAL" callout explaining the resize and its rationale. The existing `imx_set_host_coap_interface(1, ...)` call continues to work correctly (count=1 only reads index 0), and the DCU registration updates to `imx_set_host_coap_interface(2, host_coap_entries)`.

**Verdict: PASS**

---

#### Finding 3: DoIP UDP discovery accepts any responder (no identity validation)

**Original issue:** `doip_client_discover()` accepts the first broadcast responder without validating VIN, EID, or logical address. A rogue DoIP server on the same LAN could intercept the authenticated RoutineControl PDU.

**Fix applied (v4.0):** Explicitly **deferred** in the "Deferred (not in scope)" table with rationale: "Single DCU on LAN assumption is valid for current deployment; would need `doip_client_discover_by_eid()` API verification."

**Assessment:** The deferral rationale is acceptable for the current deployment topology (FC-1 and DCU on a dedicated vehicle LAN segment, not a shared enterprise network). The HMAC nonce+signature does not leak the secret even if intercepted by a rogue — the HMAC is one-way, and each nonce is unique. The worst outcome is a wasted relay attempt (the real DCU never receives the trigger). For a future multi-device LAN or fleet deployment, this must be revisited.

**Verdict: PASS (accepted deferral with adequate justification for current deployment)**

---

#### Finding 4: No rate limiting on DCU relay

**Original issue:** The plan stated "No rate limiting needed — DCU's own replay cache protects" which was incorrect reasoning — each relay generates a fresh nonce, so rapid triggers bypass the replay cache.

**Fix applied (Step 2 + Step 6, v4.0):** Added `imx_time_t dcu_relay_cooldown` field to context. `relay_dcu_phonehome()` checks `now < ctx.dcu_relay_cooldown` and sets `ctx.dcu_relay_cooldown = now + DCU_RELAY_COOLDOWN_SEC` (60 seconds) before proceeding. The edge cases table now correctly states "Rate-limited to 1 per 60s — prevents main loop starvation."

**Note:** The cooldown is set at the start of the relay attempt, not at the end. This means the 60-second window starts when the relay begins, not when it completes. If a relay takes the full ~14s worst case, the effective minimum interval between relay starts is 60 seconds, and between relay completions is ~46 seconds. This is acceptable — the purpose is to prevent rapid trigger storms, not to enforce precise spacing.

**Verdict: PASS**

---

#### Finding 5: DTLS enforcement uses is_plain_coap_mode() which is a static check

**Original status:** PASS with caveat (MEDIUM) — consistent with existing security model.

**v4.0 status:** No change needed. The CoAP handler (Step 5) still uses `is_plain_coap_mode()`, matching the existing `coap_post_remote_call_home()` pattern exactly.

**Verdict: PASS (unchanged — was already PASS in Round 1)**

---

#### Finding 6: HMAC-SHA256 relay construction is cryptographically correct

**Original status:** PASS.

**v4.0 status:** The relay PDU construction in Step 6 is unchanged: `[31 01 F0 A0] [nonce:8] [hmac:32]` = 44 bytes. HMAC computation uses the same `hmac_sha256()` function. Verified against DCU handler constants: `NONCE_OFFSET=4`, `HMAC_OFFSET=12`, `MIN_PDU_LEN=44`. All match.

**Verdict: PASS (unchanged)**

---

#### Finding 7: /dev/urandom read without retry or short-read handling

**Original status:** PASS — error handling is correct, short read or failure treated as fatal.

**v4.0 status:** The nonce read code in Step 6 is unchanged. The `read()` return is compared against `sizeof(nonce)` (8 bytes). Any error, short read, or signal interruption causes the relay to abort with a log message. This is the correct fail-safe behavior for a nonce.

**Verdict: PASS (unchanged)**

---

#### Finding 8: UDS response buffer is undersized for diagnostic edge cases

**Original status:** PASS — 64-byte buffer is adequate.

**v4.0 status:** Unchanged. The response parsing logic in Step 6 correctly handles: (a) `resp_len >= 5` for positive response, (b) `resp_len >= 3` for NRC, (c) fallthrough for unexpected/negative `resp_len`. A negative `resp_len` (error from `doip_client_send_uds()`) will fail both `>= 5` and `>= 3` comparisons, logging "unexpected response."

**Verdict: PASS (unchanged)**

---

#### Finding 9: FC-1 main loop blocked for ~3 seconds during relay

**Original issue:** The plan stated "~2s synchronous operation" but worst case was ~13-14 seconds (3s discovery + TCP connect + 10s UDS timeout).

**Fix applied (v4.0):** Design Decision D5 now states "worst case ~14s (3s UDP discovery timeout + TCP connect + 10s UDS response timeout)" and the function docstring in Step 6 says "Synchronous operation (worst case ~14s)." The edge cases table references rate limiting, not timing minimization.

**Assessment:** The true worst case is accurately documented. The plan accepts this blocking as tolerable for an infrequent, operator-initiated action. Combined with the 60-second rate limit, the main loop can be blocked for at most ~14 seconds per minute. Whether this violates CAN bus or sensor timing requirements depends on the specific FC-1 timing constraints, which is an operational concern beyond the security review scope.

**Verdict: PASS**

---

#### Finding 10: HMAC secret not cleared from FC-1 memory

**Original issue:** `ctx.hmac_secret[32]` is never zeroed. If the FC-1 crashes, the secret persists in a core dump.

**Fix applied (v4.0):** Explicitly deferred in the "Deferred (not in scope)" table with rationale: "No teardown path exists in FC-1; `remote_access` runs for device lifetime."

**Assessment:** Acceptable deferral. The embedded target has no core dumps by default, physical access is required for memory extraction, and there is genuinely no teardown function to add the cleanup to. If a teardown path is ever added, `explicit_bzero()` should be included — the MEMORY.md project memory tracks this.

**Verdict: PASS (accepted deferral)**

---

#### Finding 11: DoIP client library adds new attack surface on FC-1

**Original status:** PASS with caveats — minimal attack surface (no new listening sockets, outbound-only connections).

**v4.0 status:** The plan now documents the `malloc()` usage (Step 0 note) and the `printf()` output (Step 0 note). The `doip_client_t` is made `static` to avoid stack pressure (Step 6). No new listening sockets are introduced. The DoIP client is only instantiated during the relay function, not persistently.

**Verdict: PASS (unchanged, with embedded concerns now documented)**

---

#### Finding 12: CAN Controller serial number used as CoAP URI path component

**Original status:** PASS — `%u` format on `uint32_t` produces only decimal digits, no injection risk.

**v4.0 status:** Unchanged. `snprintf` with `sizeof(ctx.dcu_phone_home_uri)` (64 bytes) remains safe.

**Verdict: PASS (unchanged)**

---

### New Observations in v4.0

#### N1: Lazy registration race condition fix — memory barrier adequacy

The v4.0 plan (Step 4) uses `__sync_synchronize()` between the struct copy and the `imx_set_host_coap_interface(2, ...)` call. This is a full hardware memory barrier, which is correct for ensuring the CoAP receive thread sees the fully-written `host_coap_entries[1]` before the count is updated. The approach of building the entry in a local variable and doing a single struct copy is clean.

**One subtlety:** `__sync_synchronize()` is a GCC built-in. The plan already uses `_Atomic bool` (C11 `<stdatomic.h>`) for the triggered flag. Using `__sync_synchronize()` instead of `atomic_thread_fence(memory_order_seq_cst)` is a style inconsistency but functionally equivalent. Both are supported by the GCC/BusyBox toolchain. Not a security issue.

**Verdict: No action needed.**

#### N2: static doip_client_t reentrancy concern

The `doip_client_t` is declared `static` in `relay_dcu_phonehome()` to avoid stack allocation of ~4.1 KB. Since the function is only called from the single-threaded main loop, and the 60-second rate limit prevents overlapping calls, this is safe. If the function were ever called from multiple threads, the static variable would be a race condition. The single-threaded guarantee is documented in the project memory ("Client library is synchronous, single-threaded").

**Verdict: No action needed — safe in current architecture.**

#### N3: Permission check mask `0077` vs DCU's `S_IROTH | S_IWOTH`

As noted in Finding 1 above, the FC-1 plan checks `st.st_mode & 0077` which rejects group-readable/writable files as well. The DCU side only checks `S_IROTH | S_IWOTH` (0006), allowing group access. The FC-1 is stricter. This asymmetry means a secret file with mode `0640` would work on the DCU but be rejected on the FC-1. This is the safer direction for the inconsistency, but the provisioning command in the plan (`chmod 600`) produces mode `0600` which passes both checks. No operational issue.

**Verdict: No action needed — stricter is better.**

---

### Summary

All 3 HIGH findings from Round 1 are resolved:

| # | Finding | Round 1 | Round 2 | Resolution |
|---|---------|---------|---------|------------|
| 1 | HMAC file hygiene on FC-1 | FAIL (HIGH) | PASS | `O_NOFOLLOW` + `fstat()` + strict permission mask added (Step 3) |
| 2 | `host_coap_entries[1]` OOB | FAIL (HIGH) | PASS | Array resized to `[2]` (Step 4) |
| 3 | Discovery identity validation | FAIL (HIGH) | PASS (deferred) | Accepted deferral — single-DCU LAN assumption documented |
| 4 | No rate limiting | FAIL (MEDIUM) | PASS | 60-second cooldown added (Step 2 + Step 6) |
| 5 | DTLS enforcement model | PASS (caveat) | PASS | Unchanged |
| 6 | HMAC crypto correctness | PASS | PASS | Unchanged |
| 7 | `/dev/urandom` short read | PASS | PASS | Unchanged |
| 8 | UDS response buffer | PASS | PASS | Unchanged |
| 9 | Main loop blocking | FAIL (MEDIUM) | PASS | Worst case updated to ~14s in D5 and function docs |
| 10 | HMAC secret memory clearing | FAIL (LOW) | PASS (deferred) | Accepted deferral — no teardown path exists |
| 11 | DoIP client attack surface | PASS (caveats) | PASS | `malloc()` and `printf()` documented |
| 12 | URI path injection | PASS | PASS | Unchanged |

**0 STILL-FAIL items. All findings either fixed or acceptably deferred with documented rationale.**

The v4.0 plan is approved from a security perspective. The two deferred items (discovery validation and HMAC memory clearing) should be revisited if the deployment topology changes (multi-device LAN) or if a module teardown path is added.
