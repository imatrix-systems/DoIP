# Error Handling Review — DOIP_Server

## Round 2 Re-review

**Reviewer**: Error Handling Agent
**Date**: 2026-03-05
**Files Reviewed**: `src/main.c`, `src/config.c`, `test/test_discovery.c`

This re-review verifies the 12 original FAIL findings from Round 1 against the updated source code.

---

### Finding 11: Signal handler installation timing — PASS (fixed)

**Original**: Signal handlers were installed after `doip_server_start()`, leaving a window where SIGINT/SIGTERM would use the default handler and bypass cleanup.

**Verification**: In `main.c:564-567`, signal handlers are now installed *before* `doip_server_start()` (line 570). The comment on line 564 explicitly states "Set up signal handlers before starting threads." SIGPIPE is also handled at this point (see Finding 36).

---

### Finding 16: ensure_storage_dir return code — PASS (fixed)

**Original**: `ensure_storage_dir()` did not return an error code; callers proceeded as if the directory existed.

**Verification**: `ensure_storage_dir()` at `main.c:104` now returns `int` (0 on success, -1 on failure). Error paths at lines 111 and 118 return `-1`. The caller at line 539 checks the return value and prints a descriptive warning. The server continues running (blobs will fail at `fopen` with a clear error), which is a valid design choice.

---

### Finding 21: CRC-32 endianness assumption — DEFERRED

**Original**: `memcpy(&expected_crc, ...)` interprets CRC bytes in host byte order, breaking cross-endian client/server pairs.

**Status**: Code at `main.c:316` is unchanged. This is deferred as it requires a protocol-level decision on CRC byte order convention, and the current deployment is same-endian (x86).

---

### Finding 22: CLI port parsing with atoi — PASS (fixed)

**Original**: `main()` used `atoi()` for port parsing with no validation.

**Verification**: `main.c:482-489` now uses `strtoul()` with full validation: checks for empty parse (`endptr == argv[i]`), trailing garbage (`*endptr != '\0'`), and range (`val > 65535`). Invalid input causes a clear error message and `return 1`.

---

### Finding 23: Test port parsing with atoi — PASS (fixed)

**Original**: `test_discovery.c` used `atoi()` for port parsing with no validation.

**Verification**: `test_discovery.c:411-416` uses the same `strtoul()` pattern with identical validation. Invalid input causes an error message and `return 1`.

---

### Finding 24: setsockopt return value unchecked in test — PASS (fixed)

**Original**: `setsockopt(SO_RCVTIMEO)` return was ignored; failure would cause tests to hang.

**Verification**: `test_discovery.c:69-73` now checks `if (setsockopt(...) < 0)`, prints a `perror`, closes the socket, and returns `-1`.

---

### Finding 25: inet_pton return value unchecked in test — PASS (fixed)

**Original**: `inet_pton()` return was ignored; invalid IP would cause confusing sendto failures.

**Verification**: `test_discovery.c:79-83` now checks `if (inet_pton(...) != 1)`, prints "Invalid IP address: ...", closes the socket, and returns `-1`.

---

### Finding 28: doip_server_send_announcement return value unchecked — PASS (fixed)

**Original**: Announcement send result was discarded silently.

**Verification**: `main.c:584-585` now checks `if (doip_server_send_announcement(&server) != DOIP_OK)` and prints `"Warning: initial announcement failed\n"`.

---

### Finding 30: Config parser does not validate transfer_timeout=0 — PASS (fixed)

**Original**: `transfer_timeout=0` was accepted, causing immediate transfer aborts.

**Verification**: `config.c:248-249` now rejects zero: `if (uval == 0)` prints a warning ("transfer_timeout=0 rejected (minimum 1), using default") and preserves the default value of 30 seconds.

---

### Finding 31: fclose return value unchecked for blob write — PASS (fixed)

**Original**: `fclose()` after blob write was not checked; silent data loss on buffered-write failure.

**Verification**: `main.c:148-150` now checks `if (fclose(f) != 0)` and prints the error with `strerror(errno)`.

---

### Finding 33: doip_server_register_target return value unchecked — PASS (fixed)

**Original**: Target registration failure was ignored; server would run but reject all diagnostics.

**Verification**: `main.c:553-558` now checks `if (ret != DOIP_OK)`, prints an error message, calls `doip_server_destroy(&server)` for cleanup, and exits with `return 1`.

---

### Finding 36: No SIGPIPE handling — PASS (fixed)

**Original**: No SIGPIPE handling; client disconnect during write would crash the server.

**Verification**: `main.c:565` now has `signal(SIGPIPE, SIG_IGN)` installed before the server threads start, causing `write()` to return `EPIPE` instead of terminating the process.

---

### New Findings

**None.** The fixes are clean and introduce no new error handling issues. One notable improvement beyond the FAIL fixes: `localtime()` was replaced with the thread-safe `localtime_r()` at `main.c:127`, which is correct since `save_blob()` can be called from handler threads.

---

## Summary

| Category | Count |
|----------|-------|
| Original FAILs resolved (now PASS) | 11 |
| Original FAILs deferred | 1 |
| Original FAILs still failing | 0 |
| New findings | 0 |

All 12 original FAIL findings have been addressed: 11 are now PASS (correctly fixed), and 1 (CRC endianness, Finding 21) is DEFERRED pending a protocol-level decision.
