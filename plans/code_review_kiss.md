# KISS Code Review — Phone-Home Feature

**Date:** 2026-03-09
**Reviewer:** Claude (KISS Agent)
**Scope:** hmac_sha256.c/.h, phonehome_handler.c/.h, test_phonehome.c, Makefile changes, main.c changes

---

## 1. hmac_sha256.h

**Verdict: PASS**

Clean, minimal header. Three functions, two constants, include guard, extern C. Nothing to cut.

---

## 2. hmac_sha256.c

**Verdict: PASS**

~228 lines for SHA-256 + HMAC-SHA256 + constant-time compare. This is a textbook implementation of FIPS 180-4 / RFC 2104. The code is necessarily this size — SHA-256 has 64 rounds, a message schedule, padding logic. No abstraction layers, no dynamic allocation, no configuration. The `sha256_ctx_t` struct is file-scoped (static functions), not exposed in the header. This is exactly the right level of complexity.

---

## 3. phonehome_handler.h

**Verdict: PASS**

Four functions, one struct, one define. The `phonehome_config_t` struct is flat with fixed-size buffers — appropriate for embedded. Doc comments are proportionate (describe required vs optional keys, thread safety contract). Nothing to cut.

---

## 4. phonehome_handler.c — Config Parser

**Verdict: FAIL — Duplicated trim functions**

`cfg_trim_leading()` and `cfg_trim_trailing()` at lines 99-112 duplicate the same logic already in `config.c`. This is a second copy of the same pattern in the same project.

**Fix:** Move the trim helpers into a shared location (either expose them from `config.h`/`config.c`, or create a tiny `str_util.h` with two inline functions). Then both `config.c` and `phonehome_handler.c` use the same code. Alternatively, since `phonehome_config_load` follows the exact same fgets/strchr/trim pattern as `doip_config_load`, consider adding a generic `parse_key_value_file()` function in config.c that takes a callback, and have both callers use it. However, that may be over-engineering in the opposite direction — the simpler fix is just sharing the two trim functions.

---

## 5. phonehome_handler.c — Replay Cache

**Verdict: PASS**

64-entry circular buffer with TTL. Simple, bounded, no dynamic allocation. The mutex is proportionate since the handler runs from per-client threads. `check_and_record_nonce()` is 20 lines. Nothing to simplify.

---

## 6. phonehome_handler.c — Lock File Management

**Verdict: PASS**

`check_lock_file()` is 30 lines: open, read PID, check with `kill(pid, 0)`, unlink if stale. The fork mutex that wraps the check+create+fork sequence is necessary to prevent TOCTOU between concurrent clients. The `O_CREAT|O_EXCL` atomic creation is a single syscall. This is the minimum viable lock file implementation.

---

## 7. phonehome_handler.c — Request Handler

**Verdict: PASS**

`phonehome_handle_routine()` is a linear pipeline: check HMAC loaded, check length, verify HMAC, check replay, parse optional args, validate hostname, check lock, fork+exec. Each step either returns an NRC or falls through. No state machines, no callbacks, no abstraction. The one allocation-free `bastion_host[254]` on stack is appropriate.

---

## 8. phonehome_handler.c — NRC Constants

**Verdict: PASS**

All six NRC defines are used:
- 0x13: line 337 (short PDU)
- 0x21: line 405 (tunnel already active)
- 0x22: line 331 (HMAC not loaded)
- 0x24: line 357 (replay detected)
- 0x31: line 373 (invalid hostname)
- 0x35: line 349 (HMAC mismatch)

No dead constants.

---

## 9. phonehome_handler.c — Unnecessary #include

**Verdict: FAIL — `<strings.h>` is unused**

`<strings.h>` (line 28) is included but nothing from it is used. `strcasecmp`/`bzero` are not called. `explicit_bzero` comes from `<string.h>` with `_DEFAULT_SOURCE`.

**Fix:** Remove `#include <strings.h>`.

---

## 10. phonehome_handler.c — Comment Banners

**Verdict: PASS (borderline)**

The `/* ======= */` section banners are heavy but consistent with the existing codebase style in `main.c` and `config.c`. Since this is a house style, not new over-engineering, this passes.

---

## 11. test_phonehome.c — Test Framework

**Verdict: PASS**

The inline test framework (TEST_START/TEST_PASS/TEST_FAIL macros, g_passed/g_failed counters) matches the pattern already used in `test_discovery.c`. No external test library pulled in. Clean and readable.

---

## 12. test_phonehome.c — Test Coverage & Readability

**Verdict: PASS**

Six tests (UT-00 through UT-05), each in its own function, each self-contained. The `build_valid_pdu` helper is well-factored — used by three tests. `setup_test_env` creates temp files with PID-based names and `cleanup_test_env` removes them. The test ordering is deliberate (UT-05 runs before init to test the "not initialized" path). No unnecessary mocks or fixtures.

---

## 13. test_phonehome.c — `hex_to_bytes` helper

**Verdict: FAIL — No output bounds check**

`hex_to_bytes()` uses `sscanf(hex, "%2x", &byte)` in a while loop with no limit on `out_len`. The caller passes fixed-size `expected[32]` but the function has no `max_len` parameter to prevent overflow.

**Fix:** Add a `max_len` parameter:
```c
static void hex_to_bytes(const char *hex, uint8_t *out, size_t max_len, size_t *out_len)
```
Or since it is only called with known-good 64-char hex strings for test vectors, this is low-risk. But a bounds parameter costs nothing and prevents future misuse.

---

## 14. Makefile — Structure

**Verdict: PASS**

The additions are minimal:
- `PHONEHOME_SRCS` variable appended to `SERVER_SRCS`
- `TEST_PH_SRCS` and `TEST_PH_TARGET` for the new test binary
- `run-test-phonehome` target
- `ci-test` target chains phone-home + smoke + full tests
- `install` adds three scripts and the config dir

No recursive make, no auto-dependency generation, no pattern rules. Clean target updated.

---

## 15. Makefile — Install References

**Verdict: FAIL — install references files that may not exist yet**

Lines 88-92 install `scripts/phonehome-keygen.sh`, `scripts/phonehome-register.sh`, `scripts/phonehome-connect.sh`, and `etc/phonehome/phonehome.conf`. If these files do not exist in the tree, `make install` will fail with a confusing error.

**Fix:** Either (a) add these files to the tree now, or (b) guard the phonehome install lines behind a separate `install-phonehome` target so the base `install` does not break, or (c) add a comment noting these are created by a separate step. The simplest fix is to ensure the files exist.

---

## 16. main.c — Phone-Home Integration

**Verdict: PASS**

The integration in `main.c` is minimal and clean:
- `#include "phonehome_handler.h"` (line 22)
- Config load + init in a 12-line block (lines 588-601) with graceful degradation
- SID 0x31 dispatch in the switch (lines 410-417) — 7 lines
- `phonehome_shutdown()` in the cleanup path (line 685)
- `static phonehome_config_t phonehome_cfg` scoped inside main() — correct lifetime management

The phone-home config path is threaded through the existing `doip_app_config_t` via `phonehome_config_path[256]`, which avoids a second CLI flag.

---

## 17. File Organization

**Verdict: PASS — file split is correct**

- `hmac_sha256.c/.h` is a standalone crypto primitive with no project dependencies. Correct to keep separate — reusable and independently testable.
- `phonehome_handler.c/.h` is a feature module with its own config, state, and mutex. Correct to keep separate from main.c.
- `test_phonehome.c` tests both the crypto and the handler. Could theoretically split HMAC tests into their own file, but that would be over-engineering for 3 test vectors.

---

## Summary

| # | Item | Verdict |
|---|------|---------|
| 1 | hmac_sha256.h | PASS |
| 2 | hmac_sha256.c | PASS |
| 3 | phonehome_handler.h | PASS |
| 4 | phonehome_handler.c — config parser trim duplication | **FAIL** |
| 5 | phonehome_handler.c — replay cache | PASS |
| 6 | phonehome_handler.c — lock file management | PASS |
| 7 | phonehome_handler.c — request handler | PASS |
| 8 | phonehome_handler.c — NRC constants | PASS |
| 9 | phonehome_handler.c — unnecessary `<strings.h>` | **FAIL** |
| 10 | phonehome_handler.c — comment banners | PASS |
| 11 | test_phonehome.c — test framework | PASS |
| 12 | test_phonehome.c — coverage & readability | PASS |
| 13 | test_phonehome.c — hex_to_bytes bounds | **FAIL** |
| 14 | Makefile — structure | PASS |
| 15 | Makefile — install references missing files | **FAIL** |
| 16 | main.c — phone-home integration | PASS |
| 17 | File organization | PASS |

**Result: 13 PASS, 4 FAIL**

### FAIL Summary (actionable fixes)

1. **Duplicated trim functions** (`phonehome_handler.c` lines 99-112): Share with `config.c` via header or refactor into common utility.
2. **Unnecessary `#include <strings.h>`** (`phonehome_handler.c` line 28): Remove it.
3. **`hex_to_bytes` has no output bounds check** (`test_phonehome.c` line 50): Add `max_len` parameter.
4. **Install targets reference files not in tree** (`Makefile` lines 88-92): Ensure `scripts/phonehome-*.sh` and `etc/phonehome/phonehome.conf` exist, or separate into `install-phonehome` target.

---

## Round 2 Re-Review

**Date:** 2026-03-09
**Reviewer:** Claude (KISS Agent)

### Finding 1: Duplicated trim functions — PASS

**Fixed.** A shared header `include/config_parse.h` now provides `cfg_trim_leading()` and `cfg_trim_trailing()` as `static inline` functions. Both `config.c` (line 8: `#include "config_parse.h"`) and `phonehome_handler.c` (line 23: `#include "config_parse.h"`) use them. The local duplicates in `phonehome_handler.c` are gone — only call sites remain (lines 123, 133-135). The `config.c` file likewise uses `cfg_trim_leading` (line 125) and `cfg_trim_trailing` (lines 143-145) from the shared header.

The header itself is 29 lines: include guard, two `#include`s, two inline functions. This is the minimum viable shared utility — no unnecessary abstraction, no `.c` file needed. Does not violate KISS.

### Finding 2: Unused `#include <strings.h>` — PASS

**Fixed.** The include list in `phonehome_handler.c` (lines 26-36) no longer contains `<strings.h>`. The includes are: `stdio.h`, `stdlib.h`, `string.h`, `ctype.h`, `errno.h`, `fcntl.h`, `signal.h`, `time.h`, `unistd.h`, `pthread.h`, `sys/stat.h`. All are used.

### Finding 3: `hex_to_bytes` bounds check — PASS

**Fixed.** `hex_to_bytes()` in `test_phonehome.c` (line 50) now has the signature:
```c
static void hex_to_bytes(const char *hex, uint8_t *out, size_t max_len, size_t *out_len)
```
The loop condition (line 53) checks `*out_len < max_len` before each write. All three call sites pass `sizeof(expected)` as `max_len` (lines 86, 105, 128), which matches the 32-byte `expected` buffer.

### Finding 4: Makefile install references files not in tree — PASS (false positive corrected)

**Round 1 finding was incorrect.** All referenced files exist in the tree:
- `scripts/phonehome-keygen.sh` ✓
- `scripts/phonehome-register.sh` ✓
- `scripts/phonehome-connect.sh` ✓
- `etc/phonehome/phonehome.conf` ✓
- `scripts/systemd/phonehome-keygen.service` ✓
- `scripts/systemd/phonehome-register.service` ✓
- `scripts/initd/phonehome-keygen` ✓
- `scripts/initd/phonehome-register` ✓

---

### Round 2 Summary

| # | Finding | Round 1 | Round 2 |
|---|---------|---------|---------|
| 1 | Duplicated trim functions | FAIL | **PASS** |
| 2 | Unused `<strings.h>` | FAIL | **PASS** |
| 3 | `hex_to_bytes` bounds check | FAIL | **PASS** |
| 4 | Install references missing files | FAIL | **PASS** (false positive — files exist) |

**Round 2 Result: 4 of 4 findings resolved. All PASS.**

**Overall: PASS**
