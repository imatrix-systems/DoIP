# Plan Review: Security

**Reviewer:** Security Agent
**Date:** 2026-03-09
**Round:** 2 (re-review of v2.0 plan with Round 1 fixes applied)
**Documents Reviewed:**
- `DoIP_Server/plans/phonehome_plan.md` v2.0
- `DoIP_Server/docs/DCU_PhoneHome_Specification.md` v1.0.0

## Status: PASS

All 3 Round 1 FAILs have been addressed. No new security issues introduced by the fixes.

## Findings

| # | Area | R1 Status | R2 Status | Finding |
|---|---|---|---|---|
| 1 | HMAC-SHA256 constant-time comparison | PASS | PASS | No change. `hmac_sha256_compare()` specified as constant-time. |
| 2 | HMAC key memory clearing on shutdown | FAIL | PASS | **Fixed.** Plan Fix 1 specifies `explicit_bzero(hmac_secret, 32)` in `phonehome_shutdown()`. Also documented in header comment (line 201) and key design points (line 217). Correct — `explicit_bzero` is not subject to dead-store elimination. |
| 3 | HMAC secret file permission check at load | PASS | PASS | No change. |
| 4 | Replay cache nonce eviction race | PASS | PASS | No change. |
| 5 | Replay cache time-of-check consistency | PASS | PASS | No change. |
| 6 | Fork safety from handler context | PASS | PASS | Strengthened by Fix 4 — `g_transfer_mutex` is now never held during the `0x31` case at all (per-case locking pattern). Eliminates any risk of holding it across `fork()`. |
| 7 | Shell injection via bastion_host parameter | FAIL | PASS | **Fixed.** Plan Fix 2 specifies validation against `[a-zA-Z0-9._-]` before `execl()`, returning NRC 0x31 on invalid characters. Struct field `bastion_host[254]` implicitly enforces the 253-byte DNS length limit. Key design point #4 confirms. |
| 8 | Shell injection via remote_port parameter | PASS | PASS | No change. |
| 9 | Config path traversal (phonehome_config) | PASS | PASS | No change. |
| 10 | Config value injection in phonehome.conf | PASS | PASS | No change. |
| 11 | Lock file TOCTOU | PASS | PASS | Strengthened by Fix 5 — `phonehome_fork_mutex` serializes lock-check + fork, and `open(O_CREAT|O_EXCL)` provides atomic creation under the mutex. |
| 12 | Lock file PID reuse | PASS | PASS | No change. |
| 13 | HMAC secret file symlink following | FAIL | PASS | **Fixed.** Plan Fix 3 specifies `open(path, O_RDONLY | O_NOFOLLOW)` + `fdopen()`. Logs `strerror(errno)` to distinguish ENOENT/EACCES/ELOOP. Key design point #1 confirms. Correct defense-in-depth for cryptographic key files. |
| 14 | Nonce hex formatting buffer safety | PASS | PASS | No change. |
| 15 | `_exit(1)` after failed `execl()` | PASS | PASS | No change. |
| 16 | Registration script serial injection | PASS | PASS | No change. |
| 17 | Embedded SHA-256 correctness | PASS | PASS | No change. |
| 18 | Connect script path validation | PASS | PASS | No change. |

---

## Round 1 FAIL Resolution Details

### #2: HMAC key memory clearing on shutdown — PASS

**Round 1 issue:** `phonehome_shutdown()` had no specified behavior for clearing the HMAC secret from memory. `memset()` can be optimized away.

**Resolution in v2.0:** Fix 1 explicitly specifies `explicit_bzero(hmac_secret, 32)` in `phonehome_shutdown()`. This is documented in three places: the fix description (plan line 54), the header API comment (line 201), and the key design points (line 217). `explicit_bzero` is available in glibc 2.25+ and musl, both target platforms.

**Verdict:** Fixed correctly.

---

### #7: Shell injection via bastion_host parameter — PASS

**Round 1 issue:** `bastion_host` is attacker-controlled network input (DoIP PDU bytes 44+) passed through `execl()` to SSH. Needed validation against DNS-legal characters.

**Resolution in v2.0:** Fix 2 specifies character validation against `[a-zA-Z0-9._-]` with NRC 0x31 on rejection. The struct field `bastion_host[254]` provides implicit 253-byte length enforcement. Key design point #4 confirms the validation occurs before `execl()`.

**Verdict:** Fixed correctly. The character whitelist is tight and appropriate for a DNS hostname field.

---

### #13: HMAC secret file symlink following — PASS

**Round 1 issue:** `fopen()` follows symlinks. Needed `O_NOFOLLOW` or `lstat()` check.

**Resolution in v2.0:** Fix 3 specifies `open(path, O_RDONLY | O_NOFOLLOW)` + `fdopen()` instead of plain `fopen()`. Additionally logs `strerror(errno)` on failure to distinguish ENOENT vs EACCES vs ELOOP, which aids operational debugging. Key design point #1 confirms the implementation approach.

**Verdict:** Fixed correctly. The `O_NOFOLLOW` approach is cleaner than `lstat()` (no TOCTOU between stat and open).

---

## New Issues Introduced by Fixes

None identified. The fixes are minimal and well-scoped:

- Fix 4 (per-case mutex) reduces lock scope, which is strictly safer than the original broad lock.
- Fix 5 (fork mutex + O_CREAT|O_EXCL) adds serialization without introducing deadlock risk — `phonehome_fork_mutex` is only ever held in the `0x31` case path, never nested with `g_transfer_mutex`.
- Fix 7 (inline NRC helper) eliminates a cross-module dependency, reducing coupling.
- Fixes 8-12 are structural/organizational and do not affect security properties.

---

## Summary

| Status | Count |
|--------|-------|
| PASS | 18 |
| FAIL | 0 |

**All Round 1 findings resolved. Plan is approved from a security perspective for implementation.**
