# Plan: DCU Phone-Home Integration into DoIP Server

**Version:** 2.1 — All reviews complete
**Status:** COMPLETE — All 6 code review agents PASS
**Spec:** `docs/DCU_PhoneHome_Specification.md` v1.0.0
**Scope:** DCU-side code only (DoIP server integration + shell scripts)

---

## Scope Clarification

The specification defines components across **4 systems** (DCU, FC-1, Bastion, Backend). This plan covers only the **DCU-side components** that integrate into or ship alongside the existing DoIP server:

| In Scope | Out of Scope (separate repos/systems) |
|---|---|
| `phonehome_handler.c/.h` — DoIP RoutineControl handler | FC-1 signal relay agent (`phonehome-relay.sh`) |
| DoIP server `main.c` integration (SID 0x31 dispatch) | Bastion registration API (`registration_api.py`) |
| `phonehome-keygen.sh` — key generation script | Bastion signal dispatcher (`signal_dispatcher.py`) |
| `phonehome-register.sh` — Bastion registration script | Backend wake API endpoint |
| `phonehome-connect.sh` — reverse SSH tunnel script | Web UI |
| `phonehome.conf` — DCU config file | FC-1 `doip-client` / `doip_send.c` utility |
| systemd unit files + init.d scripts | Database schema (Bastion-side) |
| Unit tests (UT-01 through UT-05) | Bastion sshd_config changes |
| Integration test (IT-01: loopback DoIP trigger) | |

---

## Decisions

### Q1: Crypto — Embedded standalone HMAC-SHA256
No OpenSSL dependency. Standalone SHA-256 (FIPS 180-4) + HMAC (RFC 2104). Validated against RFC 4231 test vectors.

### Q2: Fork Safety — Proceed as specified
`fork()` → `setsid()` → `execl()` is POSIX-safe from multi-threaded context. Comment documents why.

### Q3: Lock File — Check in both handler and script
Handler: atomic lock via `open(O_CREAT|O_EXCL)` → NRC 0x21 if active. Script: re-check as defensive guard.

### Q4: Config — Separate `/etc/phonehome/phonehome.conf`
DoIP config gets one key `phonehome_config` pointing to the phonehome config path.
Phonehome config parsing reuses the existing `config.c` parser infrastructure (shared `parse_line()` helper).

### Q5: Service Files — Systemd + init.d, with split install targets
`make install-systemd`, `make install-initd`, `make install` (installs scripts + server binary).

### Q6: Tests — Unit + integration
UT-01–UT-05 linked directly. IT-01 added to existing `test/test_discovery.c`.

---

## Round 1 Review Fixes Applied

### Fix 1: HMAC key memory clearing (Security #2)
`phonehome_shutdown()` calls `explicit_bzero(hmac_secret, 32)` to clear the HMAC secret from process memory. Cannot use `memset` as compilers may optimize it away.

### Fix 2: bastion_host input validation (Security #7)
The `bastion_host` field extracted from DoIP PDU bytes 44+ is validated against DNS-legal characters `[a-zA-Z0-9._-]` before being passed to `execl()`. Invalid characters → NRC 0x31 (requestOutOfRange).

### Fix 3: HMAC secret file symlink protection (Security #13)
Use `open(path, O_RDONLY | O_NOFOLLOW)` + `fdopen()` instead of plain `fopen()` to prevent symlink-following attacks on the HMAC secret file. Log `strerror(errno)` on failure to distinguish ENOENT vs EACCES vs ELOOP.

### Fix 4: Mutex restructuring — per-case locking (Concurrency #2, Integration #6, KISS #10)
Instead of locking `g_transfer_mutex` at `handle_diagnostic()` entry and unlocking at exit, move the lock/unlock **inside** the three transfer-related cases (`0x34`, `0x36`, `0x37`) and the timeout-touch in `0x3E`. The `0x31` (RoutineControl) case never acquires `g_transfer_mutex` at all.

Concrete pattern:
```c
static int handle_diagnostic(...) {
    uint8_t sid = uds_data[0];
    int result;
    switch (sid) {
    case 0x34:
        pthread_mutex_lock(&g_transfer_mutex);
        result = handle_request_download(uds_data, uds_len, response, resp_size);
        pthread_mutex_unlock(&g_transfer_mutex);
        break;
    case 0x36:
        pthread_mutex_lock(&g_transfer_mutex);
        result = handle_transfer_data(uds_data, uds_len, response, resp_size);
        pthread_mutex_unlock(&g_transfer_mutex);
        break;
    case 0x37:
        pthread_mutex_lock(&g_transfer_mutex);
        result = handle_transfer_exit(uds_data, uds_len, response, resp_size);
        pthread_mutex_unlock(&g_transfer_mutex);
        break;
    case 0x3E:
        /* TesterPresent — no mutex needed (doesn't touch g_transfer) */
        result = handle_tester_present(uds_data, uds_len, response, resp_size);
        break;
    case 0x31:
        /* RoutineControl — phone-home, no transfer mutex */
        if (uds_len >= 4 && uds_data[1] == 0x01 &&
            uds_data[2] == 0xF0 && uds_data[3] == 0xA0) {
            result = phonehome_handle_routine(uds_data, uds_len, response, resp_size);
        } else {
            result = build_negative_response(sid, 0x12, response, resp_size);
        }
        break;
    default:
        result = build_negative_response(sid, 0x11, response, resp_size);
        break;
    }
    return result;
}
```

### Fix 5: Lock file TOCTOU + concurrent RoutineControl (Concurrency #5, #6)
Add a `phonehome_fork_mutex` (static in `phonehome_handler.c`) that serializes the lock-file-check + fork sequence. Combined with atomic lock file creation via `open(O_CREAT|O_EXCL)`:

```c
static pthread_mutex_t phonehome_fork_mutex = PTHREAD_MUTEX_INITIALIZER;

// In phonehome_handle_routine():
pthread_mutex_lock(&phonehome_fork_mutex);
int lock_fd = open(lock_file_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
if (lock_fd < 0) {
    pthread_mutex_unlock(&phonehome_fork_mutex);
    if (errno == EEXIST) {
        // Check if PID in lock file is alive
        // If alive: return NRC 0x21 (busyRepeatRequest)
        // If dead: remove stale lock, retry once
    }
    return build_nrc(0x31, 0x21, response, resp_size);
}
// Write PID placeholder, close lock_fd
// fork() + setsid() + execl()
// Child process will overwrite lock file with its own PID
pthread_mutex_unlock(&phonehome_fork_mutex);
```

### Fix 6: NRC 0x12 for unknown routines (Correctness advisory)
When SID is 0x31 but routineIdentifier is not 0xF0A0, use NRC 0x12 (subFunctionNotSupported) instead of 0x11 (serviceNotSupported). The service (RoutineControl) IS supported; the specific routine is not.

### Fix 7: build_negative_response access (Integration #8)
The phone-home handler builds its own NRC responses internally with a trivial inline helper (3 bytes: `{0x7F, sid, nrc}`). No need to extract the existing `build_negative_response()` from `main.c`.

### Fix 8: Reuse config parser infrastructure (KISS #1)
Instead of a separate `phonehome_config.c` with a duplicate parser, extract a shared `parse_config_line(line, &key, &value)` helper from `config.c` and reuse it in `phonehome_handler.c`'s config loading. The phonehome config uses `KEY=VALUE` (uppercase, no spaces around `=`) which is a subset of the existing parser's `key = value` format.

Alternatively: `phonehome_config_load()` is ~40 lines using the shared helper. No separate `.c` file needed — it lives in `phonehome_handler.c`.

### Fix 9: Collapse phases (KISS #4)
Reduced from 8 phases to 5:

### Fix 10: Required config key validation (Error Handling concern #12)
After parsing phonehome.conf, validate that `HMAC_SECRET_FILE` and `CONNECT_SCRIPT` are present. If missing, log error and return -1 from `phonehome_init()` (phone-home disabled, server continues).

### Fix 11: Lock file permission failure behavior (Error Handling concern #18)
Handler: `open()` fails with EACCES → treat as "cannot determine tunnel state" → NRC 0x22 (conditionsNotCorrect) + log warning.

### Fix 12: Install target improvements (KISS #8)
Split install targets: `install` (binary + scripts), `install-systemd`, `install-initd`. All respect `DESTDIR` and `PREFIX`.

---

## Implementation Plan

### Phase 1: Crypto + Handler + Config + Unit Tests

**New files:**
```
include/hmac_sha256.h          (~20 lines)
src/hmac_sha256.c              (~350 lines — SHA-256 + HMAC per RFC 2104)
include/phonehome_handler.h    (~60 lines)
src/phonehome_handler.c        (~300 lines)
test/test_phonehome.c          (~350 lines)
```

#### `include/hmac_sha256.h`
```c
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len, uint8_t out[32]);
int hmac_sha256_compare(const uint8_t *a, const uint8_t *b, size_t len);  // constant-time, returns 0 if equal
```

#### `include/phonehome_handler.h`
```c
#define ROUTINE_ID_PHONEHOME    0xF0A0

typedef struct {
    char bastion_host[254];
    char hmac_secret_path[256];
    char connect_script[256];
    char lock_file[256];
} phonehome_config_t;

// Load phonehome config from KEY=VALUE file. Returns 0 on success.
// Validates required keys (HMAC_SECRET_FILE, CONNECT_SCRIPT) are present.
int phonehome_config_load(phonehome_config_t *cfg, const char *path);

// Initialize phone-home subsystem. Loads HMAC secret from cfg->hmac_secret_path.
// Uses O_NOFOLLOW to prevent symlink attacks. Logs strerror(errno) on failure.
// Returns 0 on success, -1 on failure (phone-home disabled, server continues).
int phonehome_init(const phonehome_config_t *cfg);

// Handle RoutineControl 0x31/0x01/0xF0A0. Returns UDS response length.
int phonehome_handle_routine(const uint8_t *uds_data, uint32_t uds_len,
                             uint8_t *response, uint32_t resp_size);

// Cleanup: clears HMAC secret from memory via explicit_bzero().
void phonehome_shutdown(void);
```

#### `src/phonehome_handler.c` — Key design points

1. **HMAC secret loading:** `open(path, O_RDONLY | O_NOFOLLOW)` → `fdopen()` → `fread(secret, 1, 32)` → `fclose()`. Logs `strerror(errno)` to distinguish ENOENT/EACCES/ELOOP.

2. **Replay cache:** 64-entry circular buffer, own `pthread_mutex_t replay_mutex`.

3. **Fork serialization:** `phonehome_fork_mutex` held across lock-file-check + fork. Lock file created atomically with `open(O_CREAT|O_EXCL)`. Stale lock detection via `kill(pid, 0)`.

4. **bastion_host validation:** Characters checked against `[a-zA-Z0-9._-]` before `execl()`.

5. **NRC construction:** Inline `{0x7F, 0x31, nrc}` — no dependency on `main.c`'s `build_negative_response()`.

6. **Shutdown:** `explicit_bzero(hmac_secret, 32)`.

7. **Config parsing:** `phonehome_config_load()` reuses line-parsing logic (fgets + strchr for `=` + trim). ~40 lines. Required keys validated: `HMAC_SECRET_FILE`, `CONNECT_SCRIPT`.

#### `test/test_phonehome.c`

| Test | Method |
|---|---|
| UT-00 | HMAC-SHA256 against RFC 4231 test vectors |
| UT-01 | Valid HMAC → positive response `{0x71, 0x01, 0xF0, 0xA0, 0x02}` |
| UT-02 | Invalid HMAC → NRC `{0x7F, 0x31, 0x35}` |
| UT-03 | Replay (same nonce twice) → 2nd returns NRC 0x24 |
| UT-04 | Short PDU (< 44 bytes) → NRC 0x13 |
| UT-05 | HMAC secret not loaded → NRC 0x22 |

Connect script set to `/bin/true` for fork tests.

### Phase 2: Server Integration + Config

**Modified files:**
```
src/main.c                     (+~30 lines)
include/config.h               (+1 field)
src/config.c                   (+~10 lines)
doip-server.conf               (+2 lines)
```

#### `main.c` changes:
1. `#include "phonehome_handler.h"`
2. Move `g_transfer_mutex` lock/unlock inside `case 0x34/0x36/0x37` (Fix 4)
3. Add `case 0x31` with routineId check → `phonehome_handle_routine()`
4. Unknown routine in 0x31 → NRC 0x12 (Fix 6)
5. In `main()`: load phonehome config path from `g_app_config.phonehome_config_path`, call `phonehome_config_load()` + `phonehome_init()`, warn on failure
6. In shutdown: call `phonehome_shutdown()`

#### `config.h` / `config.c`:
Add `char phonehome_config_path[256]` to `doip_app_config_t`. Parse `phonehome_config` key in `config.c`.

#### `doip-server.conf`:
```ini
# Phone-Home config path (omit or leave empty to disable)
# phonehome_config = /etc/phonehome/phonehome.conf
```

### Phase 3: Shell Scripts + Service Files

```
scripts/
├── phonehome-keygen.sh          # From spec Section 5.1
├── phonehome-register.sh        # From spec Section 5.2
├── phonehome-connect.sh         # From spec Section 5.3
├── systemd/
│   ├── phonehome-keygen.service
│   └── phonehome-register.service
└── initd/
    ├── phonehome-keygen
    └── phonehome-register
```

Shell scripts verbatim from spec. Init.d scripts with LSB headers wrapping the same logic.

### Phase 4: Build System

**Makefile additions:**
```makefile
PHONEHOME_SRCS = src/phonehome_handler.c src/hmac_sha256.c
SERVER_SRCS += $(PHONEHOME_SRCS)

test-phonehome: test/test_phonehome.c src/phonehome_handler.c src/hmac_sha256.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lpthread

install: all
	install -D -m 755 doip-server $(DESTDIR)$(PREFIX)/sbin/doip-server
	install -D -m 755 scripts/phonehome-keygen.sh $(DESTDIR)$(PREFIX)/sbin/phonehome-keygen.sh
	install -D -m 755 scripts/phonehome-register.sh $(DESTDIR)$(PREFIX)/sbin/phonehome-register.sh
	install -D -m 755 scripts/phonehome-connect.sh $(DESTDIR)$(PREFIX)/sbin/phonehome-connect.sh
	install -d -m 700 $(DESTDIR)/etc/phonehome

install-systemd: install
	install -D -m 644 scripts/systemd/phonehome-keygen.service $(DESTDIR)/etc/systemd/system/
	install -D -m 644 scripts/systemd/phonehome-register.service $(DESTDIR)/etc/systemd/system/

install-initd: install
	install -D -m 755 scripts/initd/phonehome-keygen $(DESTDIR)/etc/init.d/
	install -D -m 755 scripts/initd/phonehome-register $(DESTDIR)/etc/init.d/
```

### Phase 5: Integration Test

**Extend `test/test_discovery.c`** with 2 new test cases:
- IT-01: DoIP RoutineControl with valid HMAC → positive response
- IT-02: Same nonce replayed → NRC 0x24

Uses test HMAC secret + `/bin/true` as connect script. Server started with `phonehome_config` pointing to test config.

---

## Implementation Order

1. Phase 1: `hmac_sha256` + `phonehome_handler` + unit tests (standalone, buildable independently)
2. Phase 2: `main.c` + config integration (server can now handle 0x31)
3. Phase 3: Shell scripts + service files
4. Phase 4: Makefile updates + `make test` integration
5. Phase 5: Integration tests added to test suite

---

## Risk Assessment

| Risk | Mitigation |
|---|---|
| Embedded SHA-256 correctness | Validate against RFC 4231 test vectors (UT-00) |
| `fork()` in threaded handler | fork-then-exec is POSIX-safe; commented; `g_transfer_mutex` NOT held |
| Concurrent phone-home triggers | `phonehome_fork_mutex` serializes lock-check + fork |
| Lock file TOCTOU | Atomic `open(O_CREAT\|O_EXCL)` under mutex |
| HMAC secret in memory | `explicit_bzero()` in `phonehome_shutdown()` |
| bastion_host from network input | DNS-character validation before `execl()` |
| HMAC secret symlink attack | `O_NOFOLLOW` on open |
| BusyBox vs Ubuntu init | Ship both systemd units and init.d scripts |

---

## File Change Summary

| File | Action | Lines (est.) |
|---|---|---|
| `include/hmac_sha256.h` | NEW | ~20 |
| `src/hmac_sha256.c` | NEW | ~350 |
| `include/phonehome_handler.h` | NEW | ~60 |
| `src/phonehome_handler.c` | NEW | ~300 |
| `include/config.h` | MODIFY | +1 field |
| `src/config.c` | MODIFY | +~10 lines |
| `src/main.c` | MODIFY | +~30 lines |
| `doip-server.conf` | MODIFY | +2 lines |
| `test/test_phonehome.c` | NEW | ~350 |
| `test/test_discovery.c` | MODIFY | +~80 lines (IT-01, IT-02) |
| `scripts/phonehome-keygen.sh` | NEW | ~40 |
| `scripts/phonehome-register.sh` | NEW | ~45 |
| `scripts/phonehome-connect.sh` | NEW | ~50 |
| `scripts/systemd/phonehome-keygen.service` | NEW | ~15 |
| `scripts/systemd/phonehome-register.service` | NEW | ~15 |
| `scripts/initd/phonehome-keygen` | NEW | ~40 |
| `scripts/initd/phonehome-register` | NEW | ~40 |
| `Makefile` | MODIFY | +~30 lines |
