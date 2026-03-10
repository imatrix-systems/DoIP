# Plan Review: Integration

**Reviewer:** Integration & Implementability
**Plan:** `plans/phonehome_plan.md` v2.0
**Spec:** `docs/DCU_PhoneHome_Specification.md` v1.0.0
**Round:** 2 (re-review of v2.0 fixes)
**Date:** 2026-03-09

## Status: PASS

## Round 1 FAIL Re-verification

### Finding #6 (Mutex restructuring): PASS

The plan now includes a concrete code sketch (Fix 4, lines 66-104) showing per-case locking. Verified against the actual `handle_diagnostic()` in `src/main.c` (lines 361-412):

- The sketch's function signature, variable names (`sid`, `result`, `uds_data`, `uds_len`, `response`, `resp_size`), and switch structure match the existing code exactly.
- `pthread_mutex_lock/unlock(&g_transfer_mutex)` is placed inside only the three transfer-related cases (0x34, 0x36, 0x37), which are the only cases that access `g_transfer` state.
- `case 0x3E` (TesterPresent) correctly omits the mutex. Verified: the existing TesterPresent handler does NOT update `g_transfer.last_activity` — that is done in `handle_request_download` and `handle_transfer_data`. The plan text mentions "timeout-touch in 0x3E" in the Fix 4 description, which is slightly misleading, but the code sketch itself is correct and authoritative.
- `case 0x31` (RoutineControl) correctly omits the mutex, preventing `g_transfer_mutex` from being held during `fork()`.
- The `default` case correctly omits the mutex — `build_negative_response()` is a pure helper with no shared-state access.
- The sketch is copy-paste-ready: an implementer can replace lines 377-410 of the existing `main.c` with lines 70-103 of the sketch (the inner switch body), adding the 0x31 case and adjusting 0x3E. The only adaptation needed is that the existing 0x3E case has inline logic (not a `handle_tester_present()` call), so the implementer keeps the inline logic and wraps it in the no-mutex case — straightforward.

### Finding #8 (build_negative_response access): PASS

Fix 7 explicitly states: "The phone-home handler builds its own NRC responses internally with a trivial inline helper (3 bytes: {0x7F, sid, nrc}). No need to extract the existing build_negative_response() from main.c."

The plan's 0x31 dispatch in the code sketch (lines 92-101) calls `build_negative_response()` for the unknown-routine NRC 0x12 — this is correct because that call happens in `main.c` where the static function is in scope. The phone-home handler's internal NRCs (0x35, 0x24, 0x21, 0x22, 0x13) are built inline within `phonehome_handler.c`. No cross-file dependency issue.

## Full Checklist Re-review

| # | Area | Status | Finding |
|---|------|--------|---------|
| 1 | Makefile: compile flags | PASS | Existing `CFLAGS` and `INCLUDES` compile new files without changes. `-lpthread` in LDFLAGS covers the new mutexes. No `-lssl -lcrypto` needed. |
| 2 | Makefile: SERVER_SRCS append | PASS | `SERVER_SRCS += $(PHONEHOME_SRCS)` works correctly with GNU make lazy evaluation of the initial `=` assignment. The server target picks up new files. |
| 3 | Makefile: new targets | PASS | `test-phonehome` does not conflict with existing targets (`test-discovery`, `test-server`). Needs addition to `.PHONY` and `clean` — minor but the plan's Makefile snippet is additive-only and the implementer will naturally extend these. |
| 4 | Makefile: install targets | PASS | `install`, `install-systemd`, `install-initd` are all new targets, no conflicts. All use `DESTDIR` and `PREFIX` correctly. `install -D` auto-creates parent directories. `install -d -m 700` for `/etc/phonehome` correctly restricts permissions. |
| 5 | Config: phonehome_config_path field | PASS | Adding `char phonehome_config_path[256]` to `doip_app_config_t` is binary-compatible. `doip_config_defaults()` zero-fills via `memset`, so the field defaults to empty string (disabled) with no code change to defaults. |
| 6 | Config: config.c parser | PASS | One new `else if` block for `phonehome_config` key matches existing parser pattern. "+~10 lines" estimate is accurate. |
| 7 | Handler signature | PASS | `phonehome_handle_routine(const uint8_t *uds_data, uint32_t uds_len, uint8_t *response, uint32_t resp_size)` matches existing handler signatures exactly. |
| 8 | test_phonehome.c build independence | PASS | The Makefile rule links only `test/test_phonehome.c`, `src/phonehome_handler.c`, `src/hmac_sha256.c`, plus `-lpthread`. No dependency on `doip_server.c`, `doip.c`, `main.c`, or `config.c`. The handler API uses only standard types (`uint8_t`, `uint32_t`). `phonehome_config_load()` has its own parser (~40 lines) within `phonehome_handler.c` — no reference to `config.c`'s `doip_config_load()`. No unresolved symbols expected. |
| 9 | IT-01/IT-02 in test_discovery.c: linkage | ADVISORY | The IT tests need to compute HMAC-SHA256 to build valid RoutineControl PDUs. `test_discovery.c` links via `TEST_SRCS` which does not include `src/hmac_sha256.c`. The implementer must either: (a) add `src/hmac_sha256.c` to `TEST_SRCS`, or (b) hardcode a pre-computed HMAC+nonce test vector in the test. Option (a) is cleaner and consistent with the plan's approach. This is a minor omission in the Makefile snippet — not a design flaw. |
| 10 | IT-01/IT-02: existing test compatibility | PASS | The 6 existing tests (UDP discovery x4, TCP routing activation, TesterPresent) are independent functions called sequentially from `main()`. Adding `test_phonehome_routine()` and `test_phonehome_replay()` calls after `test_tester_present()` does not affect the existing tests. The new tests use the same pattern: connect, activate routing, send UDS, check response, disconnect. The `g_passed`/`g_failed` counters accumulate correctly. If the server is started without `phonehome_config`, the 0x31 SID returns NRC 0x11 (serviceNotSupported) — the test would need the server started with a phonehome config. This means the `make test` recipe needs to provide a test phonehome config file and HMAC secret. This is an integration detail the implementer must handle in the Makefile `test` target — not a plan-level blocker. |
| 11 | fork() in threaded handler | PASS | fork-then-exec is POSIX-safe. `g_transfer_mutex` is NOT held during fork (verified in mutex restructuring above). `phonehome_fork_mutex` is held across the lock-check + fork, but released after fork returns in the parent. The child calls `setsid()` + `execl()` with no mutex access between fork and exec. |
| 12 | Replay cache thread safety | PASS | Dedicated `replay_mutex` separate from `g_transfer_mutex` and `phonehome_fork_mutex`. Correct isolation. |
| 13 | Directory structure | PASS | `scripts/`, `scripts/systemd/`, `scripts/initd/` are new directories with no conflicts. |
| 14 | Phase ordering | PASS | Phase 1 is fully standalone and testable. Each subsequent phase has correct dependencies. Unit tests run before server integration. |
| 15 | NRC code correctness (Fix 6) | PASS | Unknown routine under SID 0x31 returns NRC 0x12 (subFunctionNotSupported), not 0x11. Correct per ISO 14229. |
| 16 | Config parser independence | PASS | `phonehome_config_load()` in `phonehome_handler.c` uses its own simple `KEY=VALUE` parser. No coupling to `config.c`. |
| 17 | Lock file TOCTOU (Fix 5) | PASS | `open(O_CREAT|O_EXCL)` under `phonehome_fork_mutex` eliminates the race. EACCES handled with NRC 0x22. Stale lock detection via `kill(pid, 0)`. |
| 18 | HMAC secret protection (Fixes 1, 3) | PASS | `O_NOFOLLOW` prevents symlink attacks. `explicit_bzero()` clears secret on shutdown. |
| 19 | bastion_host validation (Fix 2) | PASS | DNS-legal character whitelist `[a-zA-Z0-9._-]` before `execl()`. |
| 20 | Required config validation (Fix 10) | PASS | Missing `HMAC_SECRET_FILE` or `CONNECT_SCRIPT` returns -1 from `phonehome_init()`, phone-home disabled, server continues. |

## Summary

20 items reviewed. **20 PASS, 0 FAIL, 1 ADVISORY.**

Both Round 1 FAILs have been resolved:
- **Finding #6:** Concrete per-case locking code sketch provided, verified copy-paste-compatible with existing `main.c` structure.
- **Finding #8:** Plan explicitly states handler builds NRC inline; `main.c` dispatch uses the existing static helper (in scope).

**Advisory (non-blocking):** Finding #9 notes that `TEST_SRCS` in the Makefile will need `src/hmac_sha256.c` added for IT-01/IT-02 to link. This is a one-line Makefile addition the implementer will naturally discover at compile time.

**Overall: PASS** — Plan is ready for implementation.
