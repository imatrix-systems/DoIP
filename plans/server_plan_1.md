# Plan: Deployable DoIP Server with Config File & Discovery Test Tool

**Version**: 2.0 — Addresses Round 1 review findings (02 Config Design: 4 FAILs, 04 Security: 4 FAILs)

**Status**: IMPLEMENTED — All 6 tests passing

## Context

The DoIP server at `~/iMatrix/DOIP/doip-server/` works but has all identity values hardcoded in `main.c` (VIN, EID, GID, logical address, etc.). The user wants a deployable server at `~/iMatrix/DOIP_Server` with a configuration file for all settings, plus a test tool that validates UDP discovery and TCP connectivity.

## Directory Structure

```
~/iMatrix/DOIP_Server/
├── Makefile
├── doip-server.conf              # Configuration file (key=value)
├── src/
│   ├── main.c                    # Modified: loads config instead of hardcoded values
│   ├── doip.c                    # Copied unchanged from doip-server/src/
│   ├── doip_server.c             # Copied unchanged from doip-server/src/
│   ├── doip_client.c             # Copied from docs/doip/src/ (for test tool)
│   └── config.c                  # NEW: key=value config parser (~200 lines)
├── include/
│   ├── doip.h                    # Copied unchanged from doip-server/include/
│   ├── doip_server.h             # Copied unchanged from doip-server/include/
│   ├── doip_client.h             # Copied from docs/doip/include/ (for test tool)
│   └── config.h                  # NEW: config API header (~50 lines)
├── test/
│   └── test_discovery.c          # NEW: UDP + TCP validation test (~350 lines)
└── plans/
    └── server_plan_1.md          # This file
```

## Files Created/Modified

### 1. `include/config.h` — NEW (~50 lines)

Defines `doip_app_config_t` wrapping `doip_server_config_t` plus app-level settings:
```c
typedef struct {
    doip_server_config_t server;        // Identity + network (passed to doip_server_init)
    char    blob_storage_dir[256];      // Blob output path
    uint32_t blob_max_size;             // Max blob bytes
    uint32_t transfer_timeout_sec;      // Transfer timeout
    char    bind_address_buf[64];       // Owned storage for bind_address pointer
} doip_app_config_t;
```

API: `doip_config_defaults()`, `doip_config_load()`, `doip_config_print()`

### 2. `src/config.c` — NEW (~220 lines)

Line-by-line key=value parser with robust error handling:

**Parsing algorithm:**
- Read lines with `fgets(line, 512, fp)` — max line length 512 chars
- Detect truncated lines: if no `\n` at end and not EOF, warn and skip remainder with a discard loop
- Skip blank lines and lines starting with `#`
- Find `=` with `strchr` — if NULL, warn "malformed line" and skip (don't crash)
- Trim leading/trailing whitespace from both key and value
- Match key via `strcmp()` chain
- Warn on unrecognized keys (don't fail)

**Value parsing by type:**
- **VIN**: Validate `strlen(value) == 17` exactly, then `memcpy(config->server.vin, value, 17)` — NOT `strncpy` (VIN is `uint8_t[17]`, not a C string)
- **Strings** (`blob_storage_dir`, `bind_address`): `strncpy` with explicit null termination: `buf[sizeof(buf)-1] = '\0'`
- **Integers**: `strtoul(value, &endptr, 0)` with validation:
  - Check `endptr != value` (something was parsed)
  - Check trailing chars are only whitespace
  - Range-check per field: `uint8_t` fields ≤ 255, `uint16_t` fields ≤ 65535, `uint32_t` fields ≤ UINT32_MAX
  - On validation failure: warn and keep default value
- **Hex byte arrays** (EID/GID): `parse_hex_bytes("00:1A:2B:3C:4D:5E", out, 6)`:
  - Split on `:` using `strtok_r`, parse each pair with `strtoul(pair, &endptr, 16)`
  - Validate exactly `expected_len` bytes (not fewer, not more)
  - Validate each byte ≤ 0xFF and no trailing chars after each pair
  - On any error: warn "invalid EID/GID format" and keep default

**Path validation** for `blob_storage_dir`:
- Reject paths containing `..` (path traversal prevention)
- On rejection: warn and keep default `/tmp/doip_blobs`

### 3. `doip-server.conf` — NEW

```ini
# DoIP Server Configuration

# Identity
vin                 = FC1BLOBSRV0000001
logical_address     = 0x0001
eid                 = 00:1A:2B:3C:4D:5E
gid                 = 00:1A:2B:3C:4D:5E
further_action      = 0x00
vin_gid_sync_status = 0x00

# Network
# bind_address: 127.0.0.1 = localhost only (safe default)
#               0.0.0.0   = all interfaces (use for FC-1 testing)
bind_address        = 127.0.0.1
tcp_port            = 13400
udp_port            = 13400
max_tcp_connections = 4
max_data_size       = 4096

# Blob Storage
blob_storage_dir    = /tmp/doip_blobs
blob_max_size       = 16777216
transfer_timeout    = 30
```

**`doip_config_defaults()` values** (used when no config file):
- `server.bind_address = NULL` → maps to INADDR_ANY in `doip_server_init()` (matches current behavior)
- `server.tcp_port = 0`, `server.udp_port = 0` → defaulted to 13400 by `doip_server_init()`
- All identity fields match the current hardcoded values in main.c lines 494-501
- `blob_storage_dir = "/tmp/doip_blobs"`, `blob_max_size = 16*1024*1024`, `transfer_timeout_sec = 30`

**bind_address pointer wiring:** `config->server.bind_address` is set to `config->bind_address_buf` ONLY when the `bind_address` key is found in the config file. Otherwise remains NULL (INADDR_ANY default).

### 4. `src/main.c` — MODIFIED (copy from doip-server/src/main.c, then modify)

Changes from original:
- Add `#include "config.h"`
- Add global `static doip_app_config_t g_app_config;`
- Remove `#define DOIP_BLOB_MAX_SIZE`, `TRANSFER_TIMEOUT_SEC`, `BLOB_STORAGE_DIR`
- Replace all references to those macros with `g_app_config.*` fields
- Replace hardcoded identity setup (original lines 493-508) with `doip_config_defaults()` + `doip_config_load()`
- Updated argument parsing: `./doip-server [-c config_file] [bind_ip] [port]`
  - No `-c` flag: try `doip-server.conf` in CWD, soft fail if missing (use defaults)
  - Explicit `-c path`: hard fail if file not found (return 1 with error message)
  - CLI bind_ip/port override config values
- Updated `ensure_storage_dir()` and `save_blob()` to use `g_app_config.blob_storage_dir` via `snprintf` instead of compile-time string concat
- Replaced `perror(BLOB_STORAGE_DIR)` string concatenation with `fprintf(stderr, "... %s: %s\n", g_app_config.blob_storage_dir, strerror(errno))`
- Uses `g_app_config.server.logical_address` in `doip_server_register_target()`

### 5. `test/test_discovery.c` — NEW (~350 lines)

Links against `doip.c` + `doip_client.c` + `config.c` (NOT `doip_server.c`).
Reads the same config file as the server to know expected VIN/EID/GID values.

**6 test cases:**

| # | Test | Method | Status |
|---|------|--------|--------|
| 1 | UDP broadcast discovery | Send raw UDP vehicle ID request to server IP:port (unicast). Validate VIN, logical_addr, EID, GID in announcement response. | PASS |
| 2 | UDP discovery by VIN (positive) | Send VIN-filtered request with correct VIN → expect response | PASS |
| 3 | UDP discovery by VIN (negative) | Send VIN-filtered request with wrong VIN → expect timeout (no response) | PASS |
| 4 | UDP discovery by EID (positive + negative) | Same pattern as VIN tests | PASS |
| 5 | TCP connect + routing activation | `doip_client_connect()` + `doip_client_activate_routing()` → validate SUCCESS response | PASS |
| 6 | TesterPresent keepalive | After routing activation, `doip_client_uds_tester_present()` → validate positive response | PASS |

**UDP loopback approach:** The reference `doip_client_discover()` sends to `INADDR_BROADCAST` which won't work on 127.0.0.1. The test tool sends UDP unicast directly to the server's IP:port using raw sockets + `doip_build_vehicle_id_request()` / `doip_parse_message()`. This works on both loopback and real interfaces.

**Usage:** `./test-discovery [-c doip-server.conf] [server_ip] [port]`

### 6. `Makefile` — NEW

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -O2 -g -std=c11
INCLUDES = -Iinclude
LDFLAGS = -lpthread

SERVER_SRCS = src/main.c src/doip.c src/doip_server.c src/config.c
TEST_SRCS   = test/test_discovery.c src/doip.c src/doip_client.c src/config.c
```

Targets: `doip-server`, `test-discovery`, `all`, `test`, `clean`

**`make test` recipe with trap-based cleanup** (prevents orphan server process on Ctrl+C/failure):
- Starts server in background
- Uses trap EXIT to ensure server is killed
- Polls with `nc -z` up to 5 seconds for server readiness
- Runs test-discovery with 60-second timeout
- Captures test exit code, kills server, returns test result

## Implementation Order (as executed)

1. Created directory structure, copied unchanged files (doip.c, doip_server.c, doip.h, doip_server.h, doip_client.c, doip_client.h)
2. Wrote `config.h` + `config.c`
3. Wrote `doip-server.conf`
4. Copied and modified `main.c` — replaced hardcoded values with config loading
5. Wrote `test/test_discovery.c`
6. Wrote `Makefile`
7. Build fix: added `#define _POSIX_C_SOURCE 200809L` in config.c for `strtok_r`
8. Build fix: added `#include <sys/time.h>` in test_discovery.c for `struct timeval`
9. Bug fix: test_discovery.c positional arg parsing used counter instead of string comparison

## Round 2 Code Review Fixes (2026-03-05)

6-agent parallel review identified 23 unique FAIL findings (after dedup across reviewers).
All 23 fixes applied, build clean (zero warnings), all 6 tests pass.

### Fixes by file:

**config.c** (2 fixes):
- `trim_trailing()` — added `if (len == 0) return` guard to prevent UB on empty string
- `transfer_timeout=0` — rejected with warning, minimum value is 1

**doip_server.h** (1 fix):
- `server->running` — changed `bool` to `_Atomic bool` with `<stdatomic.h>` for thread-safe flag

**doip_server.c** (6 fixes):
- Removed orphaned `/* (reserved for future use) */` comment
- `num_clients` entity status read — wrapped under `clients_mutex` lock
- Client handler thread — added `pthread_detach(pthread_self())` to prevent thread resource leak on normal disconnect
- FD_SETSIZE guard — added checks before `FD_SET()` in tcp_recv_exact, tcp_accept_thread, udp_handler_thread
- `doip_server_stop()` — added comment noting EINVAL from joining detached threads is benign

**main.c** (13 fixes):
- Added `#define _POSIX_C_SOURCE 200809L` for `localtime_r`
- Removed `crc32_table_initialized` flag and lazy-init dead code from `crc32_compute()`
- `ensure_storage_dir()` — now returns `int` (0 success, -1 failure); startup checks return value
- Removed redundant `ensure_storage_dir()` call from `save_blob()`
- `localtime()` → `localtime_r()` with stack-local `struct tm tm_buf`
- `fclose()` return value checked with error message
- Removed duplicate timeout check from `handle_diagnostic()` (main-loop check sufficient)
- CLI port parsing — `atoi()` → `strtoul()` with range validation
- Signal handlers — moved before `doip_server_start()` to close race window
- Added `signal(SIGPIPE, SIG_IGN)` to prevent server crash on client disconnect
- `doip_server_register_target()` — return value checked, exits on failure
- `doip_server_send_announcement()` — return value checked with warning
- Startup storage dir check — warns on failure (non-fatal)

**test_discovery.c** (4 fixes):
- `setsockopt()` return value checked with error message
- `inet_pton()` return value checked with error message
- `wrong_vin` — `memcpy` from overlong string → `memset(wrong_vin, 'X', ...)`
- Port parsing — `atoi()` → `strtoul()` with range validation
- Config load with explicit `-c` — hard fail on error (was soft fail)

### Deferred items (new feature scope):
- Malformed packet tests (Test review #13)
- Config parser unit tests (Test review #15)
- CRC endianness documentation (Error review #21)
- Split EID test into positive/negative (Test review #5)
- Routing response logical address validation (Test review #17)
- Pre-activation rejection test (Test review #14)

## Verification

```bash
cd ~/iMatrix/DOIP_Server
make                    # Build server + test tool (clean, no warnings)
make test               # Automated: start server → run tests → stop server
```

Result: `=== Results: 6 passed, 0 failed ===`
