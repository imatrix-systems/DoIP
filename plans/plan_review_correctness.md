# Plan Review: Correctness

**Reviewer:** Protocol Correctness Agent
**Plan:** `phonehome_plan.md` v2.0
**Spec:** `docs/DCU_PhoneHome_Specification.md` v1.0.0

---

## Round 1 — 2026-03-09 (v1.1)

**Status: PASS** — 16/16 items passed. One advisory issued: NRC 0x12 preferred over 0x11 for unknown routines within a supported SID (finding 9).

---

## Round 2 — 2026-03-09 (v2.0)

**Status: PASS**

All Round 1 findings remain valid. The Round 1 advisory has been addressed. Three new items added to verify the v2.0 fixes (Fix 4 mutex restructuring, Fix 5 lock file pattern, Fix 6 NRC correction).

### Re-verification of Round 1 Advisory

| # | Area | Status | Finding |
|---|------|--------|---------|
| 9R | NRC 0x12 for unknown routines (was Round 1 advisory) | PASS | Fix 6 addresses the advisory. The plan's updated `handle_diagnostic()` pseudo-code (lines 92-97) now uses `build_negative_response(sid, 0x12, response, resp_size)` when SID is 0x31 but routineIdentifier is not 0xF0A0. NRC 0x12 (subFunctionNotSupported) is the correct ISO 14229-1 code: the service (RoutineControl) IS supported; the specific routine is not. The `default` case (line 100) retains NRC 0x11 (serviceNotSupported) for completely unknown SIDs, which is also correct. Advisory fully resolved. |

### New Findings: Mutex Restructuring (Fix 4)

| # | Area | Status | Finding |
|---|------|--------|---------|
| 17 | Per-case locking: 0x34/0x36/0x37 coverage | PASS | The plan wraps each transfer handler call individually with `pthread_mutex_lock/unlock(&g_transfer_mutex)`. Verified against `main.c`: `handle_request_download()`, `handle_transfer_data()`, and `handle_transfer_exit()` all access `g_transfer` state and are documented as requiring the caller to hold `g_transfer_mutex`. Each case acquires the mutex before calling its handler and releases it after. The lock scope is identical to the current implementation (entry-lock covers all three), just more granular. No transfer state is accessed between cases, so there is no atomicity gap. Correct. |
| 18 | TesterPresent (0x3E) without mutex | PASS | The plan removes `g_transfer_mutex` from the TesterPresent path. Verified against `main.c` lines 393-402: the current TesterPresent handler reads `uds_data[1]`, writes `response[0]` and `response[1]`, and returns. It does NOT read or write any field of `g_transfer`. The `uds_data` and `response` buffers are per-call (stack or per-client), not shared state. Therefore no mutex is needed. Note: there is no `g_transfer.last_activity` update in TesterPresent (unlike some UDS implementations), so there is no hidden dependency. Correct. |
| 19 | RoutineControl (0x31) without g_transfer_mutex | PASS | The plan's `case 0x31` does not acquire `g_transfer_mutex`. The phone-home handler has its own `phonehome_fork_mutex` and `replay_mutex`. Since `phonehome_handle_routine()` never accesses `g_transfer`, no transfer mutex is needed. This also avoids the POSIX anti-pattern of holding a mutex across `fork()` — even though the child calls `execl()` immediately, not holding the mutex is the correct defensive approach and eliminates transfer-blocking latency during fork/exec. Correct. |
| 20 | Main loop timeout check compatibility | PASS | The main loop (main.c lines 637-645) independently locks `g_transfer_mutex` to check for transfer timeouts. This is fully compatible with per-case locking in `handle_diagnostic()` because both code paths use the same mutex and never hold it across a `switch` boundary. No change needed in the main loop. Correct. |
| 21 | Shutdown cleanup compatibility | PASS | The shutdown sequence (main.c lines 651-653) locks `g_transfer_mutex` around `transfer_cleanup_locked()`. Compatible with per-case locking for the same reasons as finding 20. Correct. |

### New Findings: Lock File Pattern (Fix 5)

| # | Area | Status | Finding |
|---|------|--------|---------|
| 22 | Atomic lock file creation (O_CREAT\|O_EXCL) | PASS | `open(lock_file_path, O_WRONLY | O_CREAT | O_EXCL, 0644)` is the POSIX-standard atomic file creation pattern. The kernel guarantees that if the file already exists, `open()` fails with `errno == EEXIST`. Two threads or processes racing to create the lock file will have exactly one succeed. This is the correct primitive for lock files. |
| 23 | phonehome_fork_mutex serialization | PASS | The `phonehome_fork_mutex` serializes the lock-file-check + fork sequence within the same process (multiple handler threads). Combined with the atomic `O_CREAT|O_EXCL`, this provides two-layer protection: the mutex prevents intra-process races, and the filesystem atomic create prevents inter-process races (relevant if the server is restarted while a tunnel is still active). Correct. |
| 24 | Stale lock detection and mutex re-acquisition | PASS (with note) | The pseudo-code shows: (1) `open()` fails, (2) unlock mutex, (3) check `errno == EEXIST`, (4) verify PID liveness via `kill(pid, 0)`, (5) if dead, remove stale lock and "retry once." The retry-once path would need to re-acquire `phonehome_fork_mutex` before re-attempting `open(O_CREAT|O_EXCL)`. The pseudo-code elides this detail, but the textual description ("retry once") implies a full re-attempt of the lock-check + fork sequence. At implementation time, the stale-lock-retry path must re-lock the mutex before re-calling `open()`. This is a plan-level detail gap, not a correctness error — the pattern is sound if implemented as described. |
| 25 | EACCES handling (Fix 11) | PASS | When `open()` fails with EACCES (permission denied on lock file path), the handler returns NRC 0x22 (conditionsNotCorrect). This is the correct NRC: the server cannot determine whether a tunnel is active, so preconditions for the service cannot be verified. Returning 0x22 rather than 0x21 (busyRepeatRequest) avoids falsely implying a tunnel is active. Correct. |

### New Findings: NRC and Response Handling (Fix 7)

| # | Area | Status | Finding |
|---|------|--------|---------|
| 26 | Inline NRC construction in phonehome_handler.c | PASS | Fix 7 specifies that the phone-home handler builds its own NRC responses as `{0x7F, 0x31, nrc}` via an inline helper, avoiding a dependency on `main.c`'s `build_negative_response()`. This produces the same ISO 14229 negative response format. The SID byte is hardcoded to 0x31 (RoutineControl), which is correct since the handler only serves that SID. The `case 0x31` in `handle_diagnostic()` also calls `build_negative_response(sid, 0x12, ...)` for unknown routines, which uses `main.c`'s version — no conflict since that call is in `main.c` scope. Correct. |

### Round 1 Findings 1-8, 10-16: Status

All 15 previously-passing findings remain valid in v2.0. The plan changes (Fixes 1-12) do not alter any of the PDU formats, byte offsets, NRC assignments, config struct layout, callback signatures, or replay cache threading model reviewed in Round 1. No re-review needed.

## Summary

**Round 2: PASS** — 26/26 items pass (16 from Round 1 + 10 new in Round 2).

The Round 1 advisory (NRC 0x12 vs 0x11) is fully addressed by Fix 6. The per-case mutex restructuring (Fix 4) is correct: all three transfer cases retain proper locking, TesterPresent is verified to not touch transfer state, and the main loop / shutdown paths are compatible. The lock file pattern (Fix 5) using `open(O_CREAT|O_EXCL)` under `phonehome_fork_mutex` is the standard two-layer approach for atomic lock files. One implementation note (finding 24): the stale-lock retry path must re-acquire the mutex before re-attempting `open()`.
