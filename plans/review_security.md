# Security Review: DOIP_Server — Round 2 Re-review

**Reviewer**: Security Reviewer (automated)
**Date**: 2026-03-05
**Scope**: Verify fixes for 5 FAIL findings from Round 1, scan for regressions

---

## Finding 6 (Round 1 FAIL): CLI port parsing with atoi — PASS (fixed)

**File**: `src/main.c:482-489`

**Round 1**: `cli_port = (uint16_t)atoi(argv[i]);` — no error detection.

**Fix applied**:
```c
char *endptr;
unsigned long val = strtoul(argv[i], &endptr, 10);
if (endptr == argv[i] || *endptr != '\0' || val > 65535) {
    fprintf(stderr, "Error: invalid port '%s'\n", argv[i]);
    return 1;
}
cli_port = (uint16_t)val;
```

Correctly rejects non-numeric input (`endptr == argv[i]`), trailing garbage (`*endptr != '\0'`), and out-of-range values (`val > 65535`). Consistent with config.c's `parse_uint()` pattern.

---

## Finding 7 (Round 1 FAIL): Test tool port parsing with atoi — PASS (fixed)

**File**: `test/test_discovery.c:411-417`

**Round 1**: `port = (uint16_t)atoi(argv[i]);` — same atoi issue.

**Fix applied**: Identical `strtoul` + range check pattern as Finding 6. Correctly validates test tool port argument.

---

## Finding 12 (Round 1 FAIL): CRC-32 table initialization race condition — PASS (fixed)

**File**: `src/main.c:41-64`

**Round 1**: Lazy init pattern `if (!crc32_table_initialized) crc32_init_table();` in `crc32_compute()` was a data race if called from multiple threads before initialization.

**Fix applied**: The `crc32_table_initialized` flag and the lazy init check inside `crc32_compute()` have been completely removed. The function now has a precondition comment: "Table must be initialized by crc32_init_table() before first call". `main()` calls `crc32_init_table()` at line 533 before any threads are created. The race condition is eliminated.

---

## Finding 22 (Round 1 FAIL): localtime() thread safety — PASS (fixed)

**File**: `src/main.c:126-131`

**Round 1**: `struct tm *tm = localtime(&now);` — returns pointer to static buffer, not thread-safe.

**Fix applied**:
```c
struct tm tm_buf;
struct tm *tm = localtime_r(&now, &tm_buf);
if (!tm) {
    fprintf(stderr, "[Blob Server] localtime_r() failed\n");
    return;
}
```

Uses thread-safe `localtime_r()` with a local `struct tm` buffer. Error return is checked. No shared state.

---

## Finding 28 (Round 1 FAIL): select() fd value exceeding FD_SETSIZE — PASS (fixed)

**Files**: `src/doip_server.c:36-39`, `src/doip_server.c:346-349`, `src/doip_server.c:444-447`

**Round 1**: `FD_SET(fd, &fds)` called without verifying `fd < FD_SETSIZE`, causing undefined behavior if fd >= 1024.

**Fix applied**: All three `select()` call sites now guard against `fd >= FD_SETSIZE`:

1. **`tcp_recv_exact` (line 36-39)**: Checks `if (fd >= FD_SETSIZE)` before `FD_SET`, returns `DOIP_ERR_RECV`. This also covers accepted client fds used in handler threads.
2. **`tcp_accept_thread` (line 346-349)**: Checks `if (server->tcp_fd >= FD_SETSIZE)` before `FD_SET`, breaks out of accept loop with error message.
3. **`udp_handler_thread` (line 444-447)**: Checks `if (server->udp_fd >= FD_SETSIZE)` before `FD_SET`, breaks out of UDP loop with error message.

All guards print a diagnostic message identifying the offending fd value.

---

## New Issues Scan

Reviewed all changed code for regressions or newly introduced issues:

- **strtoul edge cases**: `strtoul("0", ...)` returns 0 which passes validation — port 0 means "use default" in the server, which is acceptable behavior. No issue.
- **Removed CRC lazy init**: If a future code path calls `crc32_compute()` before `main()` initializes the table, it will silently compute wrong CRCs (table is zero-initialized). This is a design contract documented by the precondition comment, not a security vulnerability.
- **FD_SETSIZE guard in tcp_recv_exact**: An accepted client fd could exceed FD_SETSIZE even though the listening socket fd is below it. The guard in `tcp_recv_exact` correctly catches this case since all client receives go through this function.
- **No new security issues found.**

---

## Summary

| Original FAIL | Status |
|---------------|--------|
| #6: CLI port atoi | PASS (fixed) |
| #7: Test port atoi | PASS (fixed) |
| #12: CRC lazy-init race | PASS (fixed) |
| #22: localtime thread safety | PASS (fixed) |
| #28: select FD_SETSIZE guard | PASS (fixed) |

**5 original FAILs resolved, 0 remaining, 0 new issues.**

All Round 1 FAIL findings have been correctly addressed. The fixes are clean, consistent with existing code patterns, and introduce no regressions.
