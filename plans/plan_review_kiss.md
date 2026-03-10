# Plan Review: KISS — Phone-Home Implementation

**Reviewer:** KISS (Keep It Simple, Stupid)
**Round:** 2
**Date:** 2026-03-09
**Document:** `phonehome_plan.md` v2.0
**Specification:** `docs/DCU_PhoneHome_Specification.md` v1.0.0

## Round 2 Status: PASS (10 PASS, 0 FAIL) — with 2 advisories

All 5 Round 1 FAILs have been addressed. Two new advisories noted below (neither warrants a FAIL).

## Round 1 FAIL Re-verification

| # | Round 1 Finding | Round 1 Status | Round 2 Status | Verification |
|---|-----------------|----------------|----------------|--------------|
| 1 | Duplicate config parser | FAIL | PASS | Plan v2.0 Fix 8 reuses `config.c` parser infrastructure. `phonehome_config_load()` is ~40 lines in `phonehome_handler.c` using a shared line-parsing approach (fgets + strchr for `=` + trim). No second parser introduced. Verified that the existing `config.c` `trim_leading()`/`trim_trailing()` pattern is what the plan describes reusing. |
| 4 | Too many phases (8) | FAIL | PASS | Plan v2.0 Fix 9 collapses to 5 phases. The phases are sensible groupings: (1) crypto+handler+unit tests, (2) server integration+config, (3) shell scripts+service files, (4) build system, (5) integration test. |
| 5 | Inflated file count (~18) | FAIL | PASS | Plan v2.0 file summary shows 10 new + 5 modified = 15 files. `phonehome_config.c` eliminated (folded into handler). IT-01 and IT-02 extend existing `test/test_discovery.c` instead of a new file. |
| 8 | Inconsistent install target | FAIL | PASS | Plan v2.0 Fix 12 splits into `install` (binary + scripts), `install-systemd`, `install-initd`. All respect `DESTDIR` and `PREFIX`. The Makefile snippet in Phase 4 matches this split. Server binary is included in base `install`. Init.d scripts are present in `install-initd`. |
| 10 | Mutex restructuring too invasive | FAIL | PASS | Plan v2.0 Fix 4 moves `g_transfer_mutex` lock/unlock inside each transfer case (`0x34`, `0x36`, `0x37`). The `0x31` and `0x3E` cases never acquire the mutex. The concrete code sample matches exactly what Round 1 recommended. Clean per-case locking. |

## Round 1 PASS Re-confirmation

| # | Area | Round 1 | Round 2 | Notes |
|---|------|---------|---------|-------|
| 2 | `phonehome_init()` parameter count | PASS | PASS | Now takes `const phonehome_config_t *cfg` — single struct pointer. Even simpler than 4 individual params. |
| 3 | Separate `phonehome.conf` file | PASS | PASS | Unchanged. |
| 6 | Init.d alongside systemd | PASS | PASS | Unchanged. |
| 7 | Embedded HMAC-SHA256 | PASS | PASS | API unchanged: `sha256()`, `hmac_sha256()`, `hmac_sha256_compare()`. No context structs, no streaming. |
| 9 | Replay cache design | PASS | PASS | Unchanged. |

## New Concerns Introduced by Fixes

### Advisory A: `phonehome_config_t` struct proportionality

The plan introduces a `phonehome_config_t` with 4 fields (`bastion_host[254]`, `hmac_secret_path[256]`, `connect_script[256]`, `lock_file[256]`). For 4 config keys, a struct is the right choice — it groups related data that gets passed from `main()` to `phonehome_init()`. The alternative (4 bare `char[]` fields added directly to `doip_app_config_t`) would clutter the DoIP config struct with unrelated phonehome paths and violate separation of concerns.

The `phonehome_config_load()` function is ~40 lines — proportionate for parsing 4 keys with required-key validation. This is fine; **no change needed**.

**Status: PASS**

### Advisory B: Lock file mechanism complexity

The plan combines three mechanisms for lock file safety:
1. `phonehome_fork_mutex` — serializes concurrent RoutineControl requests within the process
2. `open(O_CREAT|O_EXCL)` — atomic lock file creation
3. Stale PID detection — `kill(pid, 0)` check if lock file exists but process is dead

Each mechanism solves a distinct problem: (1) handles concurrent pthreads, (2) handles the filesystem-level race, (3) handles crash recovery. Removing any one of them would leave a real gap:
- Without the mutex: two threads could race past the `open()` check before either creates the file (unlikely but possible under high concurrency)
- Without `O_CREAT|O_EXCL`: the lock file check has a TOCTOU window
- Without stale PID detection: a crashed tunnel leaves a permanent lock file requiring manual intervention

This is the minimum viable locking for a fork-from-threaded-server pattern. The code is ~15 lines total. **No simpler alternative exists that covers all three failure modes**.

One minor simplification opportunity: the "retry once" after removing a stale lock adds a code path. A simpler approach is to just remove the stale file and return NRC 0x21 (busyRepeatRequest), letting the caller retry naturally. This avoids the retry loop inside the handler. However, this is a minor implementation detail, not a plan-level concern.

**Status: PASS (advisory only)**

## Summary

| Category | Count |
|----------|-------|
| Round 1 FAILs resolved | 5/5 |
| Round 1 PASSes confirmed | 5/5 |
| New FAILs introduced | 0 |
| Advisories | 2 (both PASS) |

**Overall: PASS**

The v2.0 plan is proportionate to the feature scope. The 5 Round 1 simplifications have been applied without introducing new over-engineering. File count (15), phase count (5), and abstraction levels are all reasonable for a feature that adds HMAC authentication, fork-exec tunnel management, and dual init system support.
