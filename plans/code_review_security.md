# Code Review: Security — Phone-Home Feature

**Reviewer:** Security Agent
**Date:** 2026-03-09
**Scope:** All new/modified source files for the phone-home feature

**Files reviewed:**
- `src/hmac_sha256.c`
- `include/hmac_sha256.h`
- `src/phonehome_handler.c`
- `include/phonehome_handler.h`
- `src/main.c` (phone-home integration, lines ~360-420 and ~570-600)
- `scripts/phonehome-keygen.sh`
- `scripts/phonehome-register.sh`
- `scripts/phonehome-connect.sh`

## Status: PASS

Round 1: 6 findings (2 MEDIUM, 4 LOW). All fixable with minimal changes.
Round 2: All 6 findings verified fixed. No new issues introduced. **PASS.**

## Findings

| # | File | Area | Status | Finding |
|---|------|------|--------|---------|
| 1 | `src/hmac_sha256.c` | SHA-256 constants (K table) | PASS | All 64 round constants match FIPS 180-4 Section 4.2.2. |
| 2 | `src/hmac_sha256.c` | SHA-256 initial hash values | PASS | H0..H7 match FIPS 180-4 Section 5.3.3. |
| 3 | `src/hmac_sha256.c` | SHA-256 macros (ROTR, CH, MAJ, EP0, EP1, SIG0, SIG1) | PASS | Rotation amounts and logic match FIPS 180-4 Sections 4.1.2 and 6.2.2. |
| 4 | `src/hmac_sha256.c` | SHA-256 message schedule | PASS | W[0..15] loaded big-endian; W[16..63] computed per spec. |
| 5 | `src/hmac_sha256.c` | SHA-256 compression rounds | PASS | 64 rounds with correct t1/t2 computation and variable rotation. |
| 6 | `src/hmac_sha256.c` | SHA-256 padding | PASS | Appends 0x80, pads to 56 mod 64, appends 64-bit big-endian bit count. Handles cross-block padding (buflen > 56). |
| 7 | `src/hmac_sha256.c` | SHA-256 bitcount type | PASS | `uint64_t` with `(uint64_t)len * 8` prevents truncation. |
| 8 | `src/hmac_sha256.c` | HMAC key > block size | PASS | Keys > 64 bytes hashed to 32 bytes per RFC 2104 Step 1. |
| 9 | `src/hmac_sha256.c` | HMAC key padding | PASS | Zero-padded to block size via `memset(0)` + `memcpy`. |
| 10 | `src/hmac_sha256.c` | HMAC inner/outer hash | PASS | Inner: `SHA256((K XOR 0x36) \|\| data)`. Outer: `SHA256((K XOR 0x5C) \|\| inner_hash)`. Matches RFC 2104. |
| 11 | `src/hmac_sha256.c` | Constant-time comparison | PASS | `volatile uint8_t result` with XOR-OR accumulation. `volatile` prevents compiler short-circuit optimization. All `len` bytes iterated regardless of mismatch. |
| 12 | `src/hmac_sha256.c` | Key-derived material on stack | ~~FAIL~~ **PASS R2** | `hmac_sha256()` leaves `k_pad`, `inner_key`, `outer_key`, `key_hash`, `inner_hash` on the stack after return. These contain key-derived material. Should `explicit_bzero()` before return. **Severity: LOW** — exploitable only if attacker can read process memory, at which point the static `hmac_secret` is also exposed. Defense-in-depth improvement. **R2: Fixed.** All five buffers cleared via `explicit_bzero()` at lines 219-223. `_DEFAULT_SOURCE` defined at top of file. |
| 13 | `include/hmac_sha256.h` | API surface | PASS | Clean minimal API. Include guards and `extern "C"` present. |
| 14 | `src/phonehome_handler.c` | HMAC secret loading (O_NOFOLLOW) | PASS | `open(path, O_RDONLY \| O_NOFOLLOW)` prevents symlink-following. `fdopen()` failure path closes `fd`. `strerror(errno)` logged. Partial read clears secret via `explicit_bzero()`. |
| 15 | `src/phonehome_handler.c` | HMAC secret file permission check | ~~FAIL~~ **PASS R2** | No verification that the secret file is not world-readable. `O_NOFOLLOW` prevents symlink attacks but a world-readable file (0644 or 0666) negates the authentication model. Should `fstat(fd)` and reject if `st_mode & (S_IROTH \| S_IWOTH)`. **Severity: MEDIUM** — a misconfigured file silently degrades security from "HMAC-authenticated" to "unauthenticated." **R2: Fixed.** `fstat()` at line 189 with `S_IROTH \| S_IWOTH` check at line 194. Error message includes octal mode for diagnostics. |
| 16 | `src/phonehome_handler.c` | HMAC secret shutdown clearing | PASS | `explicit_bzero(hmac_secret, 32)` prevents compiler optimization. `hmac_loaded = 0` set. |
| 17 | `src/phonehome_handler.c` | Constant-time comparison usage | PASS | `hmac_sha256_compare(rx_hmac, exp_hmac, HMAC_LEN)` called with full 32-byte length. |
| 18 | `src/phonehome_handler.c` | bastion_host DNS validation | PASS | `validate_hostname()` checks `[a-zA-Z0-9._-]`, length 1..253. NRC 0x31 on invalid input. Validation occurs before any use in `execl()`. |
| 19 | `src/phonehome_handler.c` | bastion_host null-termination | PASS | `strnlen` with bounded remaining length. Explicit `'\0'` written. |
| 20 | `src/phonehome_handler.c` | Replay cache thread safety | PASS | `replay_mutex` protects all `replay_cache[]` and `replay_index` access. Lock held across search and insertion. |
| 21 | `src/phonehome_handler.c` | Replay cache index overflow | ~~FAIL~~ **PASS R2** | `replay_index` is `int`. After ~2.1 billion valid requests it wraps negative, causing `replay_index % REPLAY_CACHE_SIZE` to produce a negative array index (C99/C11: modulo of negative is implementation-defined, typically negative). Out-of-bounds write to `replay_cache[negative]`. Fix: change to `unsigned int` or use `& (REPLAY_CACHE_SIZE - 1)` since 64 is a power of 2. **Severity: MEDIUM** — at 1 req/s this takes ~68 years, but the fix is trivial and the bug is a memory corruption vulnerability. **R2: Fixed.** Changed to `unsigned int` at line 76. Unsigned modulo is always non-negative. |
| 22 | `src/phonehome_handler.c` | Lock file atomicity (O_CREAT\|O_EXCL) | PASS | Atomic on all POSIX filesystems. `EEXIST` handled correctly. |
| 23 | `src/phonehome_handler.c` | Lock file TOCTOU | PASS | `check_lock_file()` + `open(O_CREAT\|O_EXCL)` serialized under `phonehome_fork_mutex`. Mutex prevents intra-process TOCTOU; `O_EXCL` provides the definitive atomic check against external processes. |
| 24 | `src/phonehome_handler.c` | Lock file stale PID check | PASS | `kill(pid, 0)` tests existence. `strtol` with `<= 0` check handles corrupt content. |
| 25 | `src/phonehome_handler.c` | Fork safety (threaded context) | PASS | `fork()` + `setsid()` + `execl()` is async-signal-safe per POSIX. No mutex held in child. `_exit(1)` on exec failure. Comment documents safety rationale. |
| 26 | `src/phonehome_handler.c` | Lock file FD leak in child | ~~FAIL~~ **PASS R2** | After `fork()`, the child inherits `lock_fd`. `execl()` is called without closing it, leaking the descriptor into the connect script process. Fix: add `O_CLOEXEC` to the lock file `open()` call. **Severity: LOW** — the leaked FD is benign (the script doesn't use it), but violates least-privilege and wastes a file descriptor. **R2: Fixed.** `O_CLOEXEC` added to `open()` flags at line 413. FD auto-closes on `execl()`. |
| 27 | `src/phonehome_handler.c` | execl argument safety | PASS | `bastion_host` DNS-validated. `port_str` snprintf-formatted from `uint16_t`. `nonce_hex` hex-formatted from fixed-length bytes. `execl()` bypasses shell entirely. |
| 28 | `src/phonehome_handler.c` | g_cfg pointer lifetime | PASS | `phonehome_cfg` is `static` in `main()`, lifetime matches process. Header documents requirement. |
| 29 | `src/phonehome_handler.c` | Config file open (no O_NOFOLLOW) | PASS | Config files are not secrets; plain `fopen()` is appropriate. `O_NOFOLLOW` correctly reserved for the HMAC secret file only. |
| 30 | `src/phonehome_handler.c` | HMAC scope (nonce only) | PASS | `hmac_sha256(secret, 32, nonce, 8, ...)` matches the spec's authentication model. Replay protection handled separately. |
| 31 | `src/main.c` | RoutineControl dispatch | PASS | `case 0x31` checks `uds_len >= 4`, subFunc 0x01, routineId 0xF0A0 before dispatch. Unknown routines get NRC 0x12. No transfer mutex for 0x31. |
| 32 | `src/main.c` | phonehome_init failure path | PASS | Failure logs warning; server continues. `hmac_loaded` stays 0, subsequent triggers return NRC 0x22. |
| 33 | `src/main.c` | phonehome_shutdown ordering | PASS | Called after `doip_server_destroy()` which joins all threads. No concurrent access during clearing. |
| 34 | `scripts/phonehome-keygen.sh` | Entropy check | PASS | `/proc/sys/kernel/random/entropy_avail` >= 256 required. |
| 35 | `scripts/phonehome-keygen.sh` | Key file permissions | PASS | Private key 0600, public key 0644, root:root. Directory 0700. |
| 36 | `scripts/phonehome-keygen.sh` | Variable quoting | PASS | All expansions double-quoted. |
| 37 | `scripts/phonehome-keygen.sh` | Serial sanitization | PASS | `tr -d '[:space:]'` strips whitespace. Used only as ssh-keygen comment. |
| 38 | `scripts/phonehome-keygen.sh` | Public key logged | PASS | Public keys are non-secret. Aids operational debugging. |
| 39 | `scripts/phonehome-register.sh` | JSON injection | ~~FAIL~~ **PASS R2** | Lines 40-45 embed `$DCU_SERIAL`, `$FC1_SERIAL`, `$PUBLIC_KEY` directly into a JSON string without escaping. `$PUBLIC_KEY` contains spaces (`ssh-ed25519 AAAA... dcu-serial`). If any value contains a double-quote, the JSON becomes malformed or injectable. Should use `jq` for JSON construction. **Severity: LOW** — on a controlled embedded system, values are generated internally; but violates secure coding practice. **R2: Fixed.** Serial inputs validated at lines 30-31 with `case` patterns rejecting non-`[A-Za-z0-9-]` characters. Public key is base64-encoded output from `ssh-keygen` (no quote characters possible). The `jq` approach was not adopted, but input validation at the boundary is an acceptable alternative for an embedded target without `jq`. |
| 40 | `scripts/phonehome-register.sh` | Provisioning token handling | PASS | Token from file, used in `Authorization: Bearer` header. File shredded after success. Variable double-quoted. |
| 41 | `scripts/phonehome-register.sh` | CA certificate pinning | PASS | `--cacert "$CA_CERT"` pins the CA. |
| 42 | `scripts/phonehome-register.sh` | Response file in /tmp | ~~FAIL~~ **PASS R2** | `/tmp/reg_response.json` is a predictable path. On multi-user systems, subject to symlink attack (attacker pre-creates symlink, curl overwrites target). Should use `mktemp`. **Severity: LOW** — single-user embedded DCU mitigates risk, but still a code quality issue. **R2: Fixed.** `mktemp /tmp/phonehome_reg_XXXXXX.json` at line 33, with `trap 'rm -f "$RESP_FILE"' EXIT` at line 34 for cleanup. |
| 43 | `scripts/phonehome-register.sh` | Variable quoting | PASS | All expansions double-quoted. |
| 44 | `scripts/phonehome-connect.sh` | SSH hardening | PASS | `StrictHostKeyChecking=yes`, pinned `UserKnownHostsFile`, `BatchMode=yes`, `ExitOnForwardFailure=yes`, `ServerAliveInterval/CountMax`, `ConnectTimeout=30`. Comprehensive. |
| 45 | `scripts/phonehome-connect.sh` | Tunnel timeout | PASS | `timeout "$TUNNEL_TIMEOUT"` provides hard kill at 3600s. |
| 46 | `scripts/phonehome-connect.sh` | Lock file in script | PASS | Defensive guard only; authoritative lock created by C handler with `O_CREAT\|O_EXCL`. Script overwrites with own PID as intended. |
| 47 | `scripts/phonehome-connect.sh` | Lock file cleanup | PASS | `trap 'rm -f "$LOCK_FILE"' EXIT` ensures cleanup on all exit paths. |
| 48 | `scripts/phonehome-connect.sh` | Variable quoting | PASS | All variables double-quoted. No unquoted expansions. |
| 49 | `scripts/phonehome-connect.sh` | Username construction | PASS | Serial whitespace-stripped, bastion_host DNS-validated by C handler. `execl()` bypasses shell. No metacharacter injection. |

---

## Severity Assessment

| # | Severity | Description |
|---|----------|-------------|
| 12 | LOW | HMAC key-derived material left on stack in `hmac_sha256()`. Defense-in-depth; exploitable only with process memory read access. |
| 15 | MEDIUM | No permission check on HMAC secret file. A world-readable secret silently negates authentication. |
| 21 | MEDIUM | `replay_index` signed int overflow → negative array index → out-of-bounds write after ~2.1B requests. Trivial fix. |
| 26 | LOW | `lock_fd` leaked to child process via `fork()`/`execl()`. Add `O_CLOEXEC`. |
| 39 | LOW | JSON injection in `phonehome-register.sh` if values contain double-quotes. Use `jq`. |
| 42 | LOW | Predictable `/tmp/reg_response.json` path. Use `mktemp`. |

---

## Recommended Fixes

### Finding 12 — Clear key material from stack

Add at end of `hmac_sha256()`, before return:
```c
explicit_bzero(k_pad, sizeof(k_pad));
explicit_bzero(inner_key, sizeof(inner_key));
explicit_bzero(outer_key, sizeof(outer_key));
explicit_bzero(key_hash, sizeof(key_hash));
explicit_bzero(inner_hash, sizeof(inner_hash));
```
Requires `#define _DEFAULT_SOURCE` and `#include <strings.h>` in `hmac_sha256.c`.

### Finding 15 — Check secret file permissions

Add after `open()` succeeds in `phonehome_init()`:
```c
struct stat st;
if (fstat(fd, &st) != 0 || (st.st_mode & (S_IROTH | S_IWOTH))) {
    LOG_ERROR("phonehome: HMAC secret file has unsafe permissions (world-accessible)");
    close(fd);
    return -1;
}
```

### Finding 21 — Fix replay_index type

Change declaration:
```c
static unsigned int replay_index = 0;
```

### Finding 26 — Add O_CLOEXEC to lock file open

```c
int lock_fd = open(g_cfg->lock_file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
```

### Finding 39 — Use jq for JSON construction

```sh
JSON_BODY=$(jq -n \
    --arg serial "$DCU_SERIAL" \
    --arg fc1 "$FC1_SERIAL" \
    --arg key "$PUBLIC_KEY" \
    '{serial: $serial, fc1_serial: $fc1, device_type: "DCU", public_key: $key}')
```
Then use `-d "$JSON_BODY"` in the curl call.

### Finding 42 — Use mktemp for response file

```sh
RESP_FILE=$(mktemp /tmp/reg_response.XXXXXX)
trap 'rm -f "$RESP_FILE"' EXIT
```
Use `"$RESP_FILE"` instead of `/tmp/reg_response.json` in the curl and cat commands.

---

## Round 2 Re-Review

**Date:** 2026-03-09
**Reviewer:** Security Agent

### Verification Summary

| # | Round 1 | Round 2 | Verification |
|---|---------|---------|--------------|
| 12 | FAIL | PASS | `explicit_bzero()` clears all 5 key-derived buffers (`k_pad`, `key_hash`, `inner_key`, `inner_hash`, `outer_key`) at end of `hmac_sha256()`. `_DEFAULT_SOURCE` defined. |
| 15 | FAIL | PASS | `fstat(fd, &st)` + `st.st_mode & (S_IROTH \| S_IWOTH)` check after `open()`. Rejects world-accessible files with diagnostic error including octal mode. |
| 21 | FAIL | PASS | `replay_index` changed from `int` to `unsigned int`. Unsigned modulo always produces non-negative index. |
| 26 | FAIL | PASS | `O_CLOEXEC` added to lock file `open()` flags. FD automatically closed on `execl()`. |
| 39 | FAIL | PASS | Serial inputs validated with `case` pattern `[!A-Za-z0-9-]` at boundary. Public key from `ssh-keygen` is base64 (no injection-capable characters). Input validation is an acceptable alternative to `jq` on embedded targets. |
| 42 | FAIL | PASS | `mktemp /tmp/phonehome_reg_XXXXXX.json` replaces predictable path. `trap` ensures cleanup on all exit paths. |

### New Issues Introduced

None. All fixes are minimal and correctly scoped. No regressions observed.

### Overall Result: PASS
