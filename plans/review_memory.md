# Memory Safety Review — DOIP_Server

## Round 2 Re-review

**Reviewer**: Memory Safety Agent
**Date**: 2026-03-05
**Scope**: Re-verification of 4 FAIL findings from Round 1, plus scan for new issues introduced by fixes

---

### Finding 1: trim_trailing() empty string guard — PASS (fixed)

**Original issue**: `trim_trailing()` computed `end = s + strlen(s) - 1` without guarding against `strlen(s) == 0`, causing undefined behavior (pointer arithmetic below buffer start).

**Verification**: `config.c:29-30` now reads:
```c
size_t len = strlen(s);
if (len == 0) return;
```
The early return correctly prevents the underflow. Fix is correct.

---

### Finding 16: atoi() for port parsing — PASS (fixed)

**Original issue**: `atoi()` used for CLI port parsing in `main.c` and `test_discovery.c` with no overflow/error detection.

**Verification**:
- `main.c:482-489` now uses `strtoul(argv[i], &endptr, 10)` with three validation checks: `endptr == argv[i]` (no digits parsed), `*endptr != '\0'` (trailing garbage), `val > 65535` (range). On any failure, prints error and returns 1.
- `test_discovery.c:411-417` uses the identical pattern.

Both fixes are correct and consistent with how config file parsing already used `parse_uint()` / `strtoul()`.

---

### Finding 18: inet_pton return value unchecked — PASS (fixed)

**Original issue**: `inet_pton()` return value was not checked in `test_discovery.c`, so an invalid IP address would silently send to 0.0.0.0.

**Verification**: `test_discovery.c:79-83` now reads:
```c
if (inet_pton(AF_INET, server_ip, &dest.sin_addr) != 1) {
    fprintf(stderr, "Invalid IP address: %s\n", server_ip);
    close(fd);
    return -1;
}
```
The check for `!= 1` correctly catches both error (-1) and invalid format (0). The fd is closed before returning, preventing a file descriptor leak. Fix is correct.

---

### Finding 20: wrong_vin source string length — PASS (fixed)

**Original issue**: `memcpy(wrong_vin, "XXXXXXXXXXXXXXXXXXX", DOIP_VIN_LENGTH)` used a 19-character string literal for a 17-byte copy, which was misleading (though not a memory error).

**Verification**: `test_discovery.c:228` now reads:
```c
memset(wrong_vin, 'X', DOIP_VIN_LENGTH);
```
Clean and unambiguous. Fix is correct.

---

### Bonus: setsockopt return value now checked (Finding 17 improvement)

The Round 1 review noted Finding 17 as "PASS (with note)" because `setsockopt SO_RCVTIMEO` was unchecked in the test tool. The updated code at `test_discovery.c:69-73` now checks the return value and returns -1 on failure, preventing the test from hanging indefinitely on a setsockopt failure.

---

### New issues scan

Examined all changed code regions for new memory safety issues:

- **main.c:482-489** (strtoul port parsing): `endptr` is initialized by `strtoul`, all dereferences are safe. No new issues.
- **test_discovery.c:411-417** (strtoul port parsing): Same pattern. No new issues.
- **config.c:29-30** (empty string guard): Pure early return, cannot introduce issues. No new issues.
- **test_discovery.c:228** (memset): Single call with correct constant. No new issues.
- **test_discovery.c:69-73** (setsockopt check): `close(fd)` before `return -1` prevents fd leak. No new issues.

**No new memory safety issues found.**

---

## Summary

| Original FAIL | Status |
|---------------|--------|
| Finding 1: trim_trailing() empty string UB | PASS (fixed) |
| Finding 16: atoi() port parsing | PASS (fixed) |
| Finding 18: inet_pton() unchecked | PASS (fixed) |
| Finding 20: wrong_vin string length | PASS (fixed) |

**4 original FAILs resolved, 0 remaining, 0 new issues found.**

All memory safety concerns from Round 1 have been correctly addressed. The codebase passes the memory safety review.
