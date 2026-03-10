# Concurrency Review: DOIP_Server

## Round 2 Re-review

**Reviewer:** Concurrency Reviewer (automated)
**Date:** 2026-03-05
**Files reviewed:**
- `src/main.c`
- `src/doip_server.c`
- `include/doip_server.h`

This re-review verifies the 6 FAIL findings from Round 1.

---

### Finding 2: `server->running` Not Atomic — PASS (fixed)

**Round 1:** `running` was a plain `bool` read/written across threads without synchronization.

**Fix applied:**
- `doip_server.h:19` — `#include <stdatomic.h>` added
- `doip_server.h:117` — Field declared as `_Atomic bool running;`

All reads and writes to `server->running` now use C11 atomic operations with sequentially-consistent ordering (the default for `_Atomic` qualified types). This eliminates the data race and ensures cross-thread visibility.

**Verdict:** Correctly fixed.

---

### Finding 4: Signal Handler Registration Timing — PASS (fixed)

**Round 1:** `signal(SIGINT, ...)` and `signal(SIGTERM, ...)` were registered after `doip_server_start()`, leaving a race window during startup.

**Fix applied:**
- `main.c:564-567` — Signal handlers (`SIGPIPE`, `SIGINT`, `SIGTERM`) are now registered before `doip_server_start()` at line 570.

The ordering is now: signal setup (lines 565-567) -> server start (line 570). No race window remains.

**Verdict:** Correctly fixed.

---

### Finding 8: `localtime()` Not Thread-Safe — PASS (fixed)

**Round 1:** `save_blob()` used `localtime()` which returns a pointer to static storage, making it non-reentrant.

**Fix applied:**
- `main.c:126-127` — Uses `localtime_r()` with a stack-local `struct tm tm_buf`:
  ```c
  struct tm tm_buf;
  struct tm *tm = localtime_r(&now, &tm_buf);
  ```
- `main.c:128-131` — Error check on `localtime_r()` return value added.

**Verdict:** Correctly fixed. The reentrant variant with a caller-supplied buffer eliminates the thread-safety concern.

---

### Finding 10: `num_clients` Read Without Lock — PASS (fixed)

**Round 1:** `server->num_clients` was read without holding `clients_mutex` in the entity status response handler.

**Fix applied:**
- `doip_server.c:280-282` — The read is now wrapped in a lock/unlock pair:
  ```c
  pthread_mutex_lock(&server->clients_mutex);
  uint8_t open_sockets = (uint8_t)server->num_clients;
  pthread_mutex_unlock(&server->clients_mutex);
  ```
- The local `open_sockets` value is then used in the response struct (line 287), outside the lock.

**Verdict:** Correctly fixed. The data race is eliminated.

---

### Finding 13: CRC Lazy-Init Dead Code — PASS (fixed)

**Round 1:** `crc32_compute()` contained a lazy-init check (`if (!crc32_table_initialized)`) that was a theoretical race even though unreachable due to explicit init in `main()`.

**Fix applied:**
- `main.c:57-64` — The lazy-init check has been removed entirely. `crc32_compute()` now assumes the table is already initialized.
- `main.c:57` — A comment documents the precondition: "Table must be initialized by crc32_init_table() before first call"
- `main.c:533` — `crc32_init_table()` is called in `main()` before any threads are spawned.

**Verdict:** Correctly fixed. Dead code removed; precondition documented.

---

### Finding 16: Thread Resource Leak — PASS (fixed)

**Round 1:** Client handler threads were joinable (default) but never joined on normal disconnect. Each un-joined thread leaked its stack and resources.

**Fix applied:**
- `doip_server.c:329` — `pthread_detach(pthread_self());` is called at the end of `client_handler_thread`, after cleanup under lock, immediately before `return NULL`.
- `doip_server.c:695-698` — `doip_server_stop()` still calls `pthread_join()` on active threads collected during shutdown, with a comment acknowledging that `EINVAL` is benign for already-detached threads.

**Race analysis for `pthread_detach` + `pthread_join` interaction during shutdown:**

1. **Handler already exited and detached before stop collects threads:** The slot has `active = false`, so `doip_server_stop()` skips it. Resources already freed by detach + thread exit. Safe.

2. **Handler still running when stop collects threads:** `doip_server_stop()` captures the `pthread_t` handle while `active` is still `true`, then closes the fd to force the handler to exit. Two sub-cases:
   - If `pthread_join` runs before `pthread_detach`: join blocks until the handler finishes (including the detach call). The thread resources are freed by join. The subsequent `pthread_detach` on the now-invalid thread ID returns `EINVAL` (return value not checked, harmless on Linux/glibc).
   - If `pthread_detach` runs before `pthread_join`: the thread becomes detached. `pthread_join` returns `EINVAL` (benign, as noted in the comment).

Both orderings are safe. No resource leak in either case.

**Verdict:** Correctly fixed.

---

## New Issues Scan

Scanned all three source files for new concurrency issues introduced by the fixes. None found.

Specific checks performed:
- `_Atomic bool` writes in `doip_server_init()`, `doip_server_start()`, `doip_server_stop()` are all correct
- `pthread_detach`/`pthread_join` interaction is safe in all orderings (analyzed above)
- No new shared-state access patterns introduced
- Lock discipline for `clients_mutex` remains correct

---

## Summary

| Metric | Count |
|--------|-------|
| Original FAILs resolved | 6/6 |
| Original FAILs remaining | 0 |
| New issues found | 0 |

All 6 concurrency findings from Round 1 have been correctly addressed. No new concurrency issues were introduced by the fixes.
