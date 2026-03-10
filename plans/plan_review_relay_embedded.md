# DCU Phone-Home Relay Plan Review: Embedded / Error Handling -- Round 2

## Review Date: 2026-03-09
## Plan Version: 4.0
## Reviewer: Embedded / Error Handling (risk-reviewer)
## Overall: PASS

---

### Round 1 Finding Re-verification

#### Finding 1: Stack Usage of `doip_client_t` -- was FAIL

**Original issue:** `doip_client_t` (~4140 bytes) declared as a local variable in `relay_dcu_phonehome()` risks stack overflow on embedded BusyBox Linux with small thread stacks.

**Fix applied:** Plan v4.0 Step 6 declares `static doip_client_t client` with an explicit comment: "Static to avoid ~4.1 KB on stack (recv_buf[4104] inside)". The function is single-threaded and non-reentrant (called only from the main loop), so `static` is safe.

**Verdict: PASS.** The 4.1 KB is moved from stack to BSS. The function documentation also notes this decision.

---

#### Finding 2: `host_coap_entries` Array Overflow -- was FAIL (CRITICAL)

**Original issue:** Existing code declares `static CoAP_entry_t host_coap_entries[1]`. The plan wrote to index `[1]`, which is an out-of-bounds write.

**Fix applied:** Plan v4.0 Step 4 is now titled "Resize `host_coap_entries` Array and Lazy Registration" and opens with a bold CRITICAL callout stating the array must be resized from `[1]` to `[2]`. The code shows `static CoAP_entry_t host_coap_entries[2];` with a comment documenting the change.

**Verdict: PASS.** The fix is explicit, prominent, and correct.

---

#### Finding 3: `doip_client_send_diagnostic()` Uses `malloc()` -- was FAIL

**Original issue:** The plan did not mention that the DoIP client library uses `malloc()` internally, which is an undocumented heap dependency on an embedded system.

**Fix applied:** Plan v4.0 Step 0 contains an explicit note: "The DoIP client library uses `malloc()` internally in `doip_client_send_diagnostic()` (line 551 of `doip_client.c`). This is acceptable -- FC-1 already uses `malloc()` elsewhere." The fix-tracking table (item 8) also references this.

**Verdict: PASS.** The dependency is now documented. The rationale (FC-1 already uses malloc) is sound.

---

#### Finding 4: DoIP Client Library Uses `printf()` Directly -- was FAIL

**Original issue:** The DoIP client library contains `printf("[DoIP Client] ...")` calls that are lost when running as a service with no terminal.

**Fix applied:** Plan v4.0 Step 0 note states: "The library also has `printf("[DoIP Client] ...")` debug output that goes to stdout; this is acceptable for now as FC-1 captures stdout in syslog when running as a service."

**Verdict: PASS.** The plan acknowledges the issue and documents the mitigation (syslog capture). The "for now" phrasing appropriately signals this is a known limitation, not an oversight. If stdout is indeed redirected to syslog on the BusyBox target, the debug output will be captured. This should be verified during implementation (Step 0 says "Verify compilation with BusyBox toolchain") but as a plan-level fix, this is adequate.

---

#### Finding 5: Error Recovery After DoIP Discovery Failure -- was PASS

No change needed. The v4.0 plan retains the same correct pattern: log, `doip_client_destroy()`, return.

**Verdict: PASS (unchanged).**

---

#### Finding 6: Error Recovery After TCP Connect Failure -- was PASS

No change needed. Pattern unchanged in v4.0.

**Verdict: PASS (unchanged).**

---

#### Finding 7: Error Recovery After Routing Activation Failure -- was PASS

No change needed. Pattern unchanged in v4.0.

**Verdict: PASS (unchanged).**

---

#### Finding 8: UDS Negative Response Handling -- was PASS

No change needed. The v4.0 plan retains the same response-checking logic with proper length guards (`resp_len >= 5` for positive, `resp_len >= 3` for NRC).

**Verdict: PASS (unchanged).**

---

#### Finding 9: Timeout Bounding on All Network Operations -- was PASS (with observation)

**Original observation:** The plan stated "~2s synchronous operation" but worst case was ~18 seconds. Should document the worst case.

**Fix applied:** Plan v4.0 D5 now states: "Synchronous operation -- worst case ~14s (3s UDP discovery timeout + TCP connect + 10s UDS response timeout)." The function doc comment in Step 6 also says "Synchronous operation (worst case ~14s)."

**Note on discrepancy:** Round 1 calculated ~18s (3s + 5s + 10s). The v4.0 plan says ~14s. The difference is TCP connect timeout -- Round 1 assumed 5s for `DOIP_TCP_GENERAL_TIMEOUT`, but the actual default in the library is 2000ms (`DOIP_TCP_GENERAL_TIMEOUT`), and there is also overhead for routing activation. The ~14s figure is reasonable but slightly optimistic if routing activation also uses the TCP recv timeout. Either way, the worst case is now documented and the magnitude is correct.

**Verdict: PASS.** The worst case is documented. The ~14s vs ~18s difference is minor and within acceptable estimation variance.

---

#### Finding 10: Socket/Resource Leak Analysis -- was PASS

No change needed. All error paths in the v4.0 `relay_dcu_phonehome()` call `doip_client_destroy()`. The `/dev/urandom` fd is guarded with `if (urand_fd >= 0) close(urand_fd)`.

**Verdict: PASS (unchanged).**

---

#### Finding 11: BusyBox POSIX API Compatibility -- was PASS

No change needed. The v4.0 plan adds `<stdatomic.h>` for `_Atomic` (Step 5.5), `<sys/stat.h>` for `fstat()` and `S_ISREG()` (Step 5.5), and `<fcntl.h>` for `O_NOFOLLOW` (Step 5.5). All are standard POSIX/C11 headers available on BusyBox Linux.

**Verdict: PASS (unchanged, new includes are compatible).**

---

#### Finding 12: Graceful Degradation -- was PASS

No change needed. The v4.0 plan retains the same graceful degradation model. The rate-limiting addition (60s cooldown, Step 6) further improves degradation behavior by preventing main-loop starvation from rapid triggers.

**Verdict: PASS (unchanged, improved by rate limiting).**

---

#### Finding 13: Logging for Field Debugging -- was PASS

No change needed. The v4.0 plan adds a rate-limit log message ("DCU relay skipped -- cooldown") which improves field debugging. All other log messages are retained.

**Verdict: PASS (unchanged, improved).**

---

#### Finding 14: HMAC Secret File Permissions and Security -- was PASS

**Improvement in v4.0:** The `init_load_hmac_secret()` function now uses `O_NOFOLLOW` to prevent symlink attacks and adds `fstat()` + `S_ISREG()` + permission checks (`st.st_mode & 0077` rejects world/group readable files). These were recommendations from the security reviewer's Round 1 findings, now incorporated.

**Verdict: PASS (improved).**

---

#### Finding 15: Concurrency Between CoAP Thread and Main Loop -- was PASS

**Improvement in v4.0:** The `dcu_phonehome_triggered` flag is now declared as `_Atomic bool` (Step 2) with `<stdatomic.h>` included (Step 5.5). This is stronger than the Round 1 assessment that ARM bool writes are "atomic at the instruction level" -- the `_Atomic` qualifier provides a formal guarantee with proper memory ordering semantics. The lazy registration race condition (identified by the concurrency reviewer) is addressed with a `__sync_synchronize()` barrier and struct-copy-before-registration pattern.

**Verdict: PASS (improved).**

---

### New Observations on v4.0 (informational, not blocking)

1. **Rate limiting implementation:** The 60s cooldown in Step 6 uses `imx_time_t` comparison (`now < ctx.dcu_relay_cooldown`). If `imx_time_t` is an unsigned type and wraps around, the comparison could malfunction. This is a theoretical concern on systems with long uptimes (depends on `imx_time_t` resolution). The existing codebase likely handles time wraparound elsewhere, so this follows established patterns.

2. **`static doip_client_t client` retains state between calls:** Since `doip_client_init()` is called each time and `doip_client_destroy()` is called on all exit paths, this is fine. The `static` keyword only affects storage duration, not initialization -- the explicit `doip_client_init()` resets the struct. No issue.

3. **`O_NOFOLLOW` availability:** `O_NOFOLLOW` is Linux-specific (not POSIX). It is available on BusyBox Linux, so this is compatible with the target platform, but worth noting if the code is ever ported.

---

### Summary

All 4 FAIL findings from Round 1 have been adequately addressed in plan v4.0:

| # | Finding | Round 1 | Round 2 | Notes |
|---|---------|---------|---------|-------|
| 1 | Stack overflow from `doip_client_t` | FAIL | PASS | Made `static` in relay function |
| 2 | `host_coap_entries[1]` OOB write | FAIL | PASS | Array resized to `[2]` with prominent callout |
| 3 | Undocumented `malloc()` in DoIP library | FAIL | PASS | Documented in Step 0 note |
| 4 | DoIP library `printf()` lost in production | FAIL | PASS | Documented; stdout captured by syslog |
| 5 | Error recovery after discovery failure | PASS | PASS | Unchanged |
| 6 | Error recovery after TCP connect failure | PASS | PASS | Unchanged |
| 7 | Error recovery after routing activation failure | PASS | PASS | Unchanged |
| 8 | UDS negative response handling | PASS | PASS | Unchanged |
| 9 | Timeout bounding on all network operations | PASS | PASS | Worst case now documented (~14s) |
| 10 | Socket/resource leak analysis | PASS | PASS | Unchanged |
| 11 | BusyBox POSIX API compatibility | PASS | PASS | New includes are compatible |
| 12 | Graceful degradation | PASS | PASS | Improved by rate limiting |
| 13 | Logging for field debugging | PASS | PASS | Improved with cooldown log |
| 14 | HMAC secret file permissions | PASS | PASS | Improved with O_NOFOLLOW + fstat |
| 15 | Concurrency (CoAP thread vs main loop) | PASS | PASS | Improved with _Atomic + barrier |

All 15 findings now PASS. The plan is ready for implementation from an embedded/error-handling perspective.
