# Thread Safety Code Review — DoIP Logging Module

**Date:** 2026-03-09
**Files reviewed:**
- `/home/greg/iMatrix/DOIP_Server/src/doip_log.c`
- `/home/greg/iMatrix/DOIP_Server/src/main.c` (LOG_* call sites)

---

## 1. Mutex Usage in doip_log() — Lock/Unlock Pairing

**Verdict: PASS**

In `doip_log()` (lines 278-289), the mutex is acquired with `pthread_mutex_lock(&g_log.mutex)`, the file write + flush + size tracking + conditional rotation all execute under the lock, and then `pthread_mutex_unlock(&g_log.mutex)` is called unconditionally. There is no early return or `goto` between lock and unlock. The `rotate_log_locked()` helper is correctly called while the mutex is already held (no recursive lock attempt — the mutex is a default non-recursive POSIX mutex, and `rotate_log_locked()` does not re-lock it).

In `doip_log_shutdown()` (lines 207-217), the pattern is also correct: lock, set initialized to false, flush, close, unlock, then destroy. No path skips the unlock.

---

## 2. _Atomic bool initialized — Proper Use of atomic_load/atomic_store

**Verdict: PASS**

- `doip_log_init()` line 191: `atomic_store(&g_log.initialized, true)` — correct, uses atomic store after all initialization is complete (mutex init, file open). This provides a release fence ensuring prior writes are visible to other threads.
- `doip_log_shutdown()` line 208: `atomic_store(&g_log.initialized, false)` — correct, done under the mutex for defense-in-depth.
- `doip_log()` line 234: `if (!atomic_load(&g_log.initialized)) return;` — correct early-exit fast path using atomic load.

The atomic flag is used as a guard, not as a replacement for the mutex. The mutex protects the actual file I/O. This is the correct two-level pattern.

---

## 3. Shutdown Race — Thread Calls LOG_* During/After Shutdown

**Verdict: PASS (with documented precondition)**

The documented precondition in `doip_log.h` (line 33-34) and `doip_log_shutdown()` states: "MUST be called only after all server threads have exited (i.e., after `doip_server_destroy()` returns)."

In `main.c`, the shutdown sequence (lines 655-657) is:
```
doip_server_destroy(&server);   // stops all threads, waits for joins
LOG_INFO("Server stopped.");    // safe — only main thread remains
doip_log_shutdown();            // safe — only main thread
```

This ordering is correct. After `doip_server_destroy()` returns, no worker threads exist to call LOG_*.

**Residual risk:** If a thread somehow outlives `doip_server_destroy()` and calls `doip_log()` after `doip_log_shutdown()` completes and `pthread_mutex_destroy()` has been called, the `pthread_mutex_lock()` in `doip_log()` is undefined behavior. However, the `atomic_load(&g_log.initialized)` check on line 234 returns false and early-exits *before* the mutex lock, which provides a practical safety net. This is acceptable — the atomic check is not a guarantee (a thread could read `true` right before shutdown flips it to `false` and proceeds into the mutex region), but the precondition makes this the caller's responsibility, and the early-exit is a reasonable defense-in-depth measure.

---

## 4. Lock Ordering — g_transfer_mutex vs g_log.mutex

**Verdict: PASS**

The two mutexes are `g_transfer_mutex` (in main.c, protects transfer state) and `g_log.mutex` (in doip_log.c, protects log file I/O).

Call pattern analysis:
- `handle_diagnostic()` acquires `g_transfer_mutex`, then calls UDS handlers that call LOG_* macros, which acquire `g_log.mutex`. **Order: transfer -> log.**
- Main loop timeout check (lines 637-645): acquires `g_transfer_mutex`, calls `LOG_WARN()` inside, which acquires `g_log.mutex`. **Order: transfer -> log.**
- Other LOG_* calls in main.c (startup, shutdown sequences): only `g_log.mutex` acquired, `g_transfer_mutex` not held.
- `doip_log_shutdown()`: only `g_log.mutex` acquired.
- `rotate_log_locked()`: only `g_log.mutex` held (already acquired by caller).

The lock ordering is consistently **transfer -> log** everywhere. There is no code path that acquires `g_log.mutex` first and then `g_transfer_mutex`. **No deadlock is possible.**

---

## 5. File Rotation Under Concurrent Writes

**Verdict: PASS**

`rotate_log_locked()` is only called from `doip_log()` at line 286-287, inside the critical section protected by `g_log.mutex`. The function comment (line 80) explicitly documents "caller must hold g_log.mutex." Since all file writes go through `doip_log()` and all file writes are serialized by the mutex, concurrent rotation cannot occur. The sequence is: write -> flush -> update size -> check threshold -> rotate if needed — all atomic with respect to other threads.

The `rename()` calls in `rotate_log_locked()` operate on the filesystem level. Since the mutex serializes all log file operations, no thread can be writing to the file while another rotates it. The `fclose()` on line 84 closes the old handle before `fopen()` on line 114 opens the new one, so there is no file descriptor leak.

---

## 6. stderr Output Outside Mutex

**Verdict: PASS**

`doip_log()` writes to stderr on line 292 (`fputs(line, stderr)`) outside the mutex. This is intentional and acceptable:

- POSIX requires that `stdio` functions (including `fputs`) use internal locking on FILE* streams. Two concurrent `fputs(line, stderr)` calls will not corrupt the FILE* internal state.
- Individual `fputs()` calls write a complete line (the `line` buffer includes `\n`). POSIX guarantees that each `fputs()` call is atomic with respect to other stdio calls on the same stream, so lines will not be interleaved mid-character.
- Keeping stderr outside the mutex avoids holding the file mutex longer than necessary, which is good for latency.

The `fprintf(stderr, ...)` calls in `rotate_log_locked()` (lines 108-110, 123) happen while `g_log.mutex` is held, but these are error paths that write to stderr. Since they don't acquire any additional mutex, and stderr's internal lock handles concurrency, this is safe.

---

## 7. No Mutex Held During vsnprintf/gettimeofday

**Verdict: PASS**

In `doip_log()`, the expensive operations are performed *before* the mutex is acquired:
- `gettimeofday()` — line 240, before mutex lock on line 278
- `localtime_r()` — line 243, before mutex lock (and using the reentrant `_r` variant, correct)
- `snprintf()` for timestamp — line 245, before mutex lock
- `vsnprintf()` for message formatting — line 258, before mutex lock
- `snprintf()` for full line assembly — line 271, before mutex lock
- `sanitize_line()` — line 275, before mutex lock

The mutex on lines 278-289 only protects `fputs()`, `fflush()`, size accounting, and conditional rotation. This is optimal — the critical section is minimal, reducing contention between threads.

---

## Summary

| # | Check Item | Verdict |
|---|-----------|---------|
| 1 | Mutex lock/unlock pairing in doip_log() | **PASS** |
| 2 | _Atomic bool initialized usage | **PASS** |
| 3 | Shutdown race conditions | **PASS** |
| 4 | Lock ordering (transfer vs log) | **PASS** |
| 5 | File rotation under concurrent writes | **PASS** |
| 6 | stderr output outside mutex | **PASS** |
| 7 | No mutex during formatting (good practice) | **PASS** |

## Overall Verdict: PASS

All seven thread safety checks pass. The logging module uses a correct and well-structured concurrency design: an atomic flag for fast-path filtering, a single mutex for file I/O serialization, formatting outside the critical section for minimal contention, consistent lock ordering with the transfer mutex, and a documented precondition that shutdown occurs only after all worker threads have exited.
