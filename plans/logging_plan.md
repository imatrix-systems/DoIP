# DoIP Server Logging Module — Implementation Plan

**Project:** DOIP_Server Structured Logging with Rotation
**Date:** 2026-03-07
**Status:** PLAN — Round 3 (post Round 2 review fixes)

---

## Overview

Add a structured logging module to the DOIP Server that replaces 65 of the 82 `printf`/`fprintf` calls with leveled log output to `/var/FC-1-DOIP.log`. The remaining 17 calls (config parser warnings in `doip_config_load()`) stay as `fprintf(stderr, ...)` because they execute before the logger is initialized. The module supports command-line log level selection, automatic log file rotation (5 files, 1 MB each), and simultaneous console output via stderr.

**Target size:** ~100-120 new lines total (~20 line header, ~80-100 line implementation). Proportionate to the ~900-line server.

---

## Requirements

1. **Log file:** `/var/FC-1-DOIP.log` (default path, overridable via `-l PATH`)
2. **Log levels:** ERROR, WARN, INFO, DEBUG (4 levels, increasingly verbose)
3. **Command-line:** `-v` = DEBUG, default = INFO; `-q` = WARN only, `-qq` = ERROR only
4. **Log rotation:** When the active log reaches 1 MB, rotate:
   - `FC-1-DOIP.log` → `.log.1` → `.log.2` → `.log.3` → `.log.4` (oldest deleted)
   - Maximum 5 files total (active + 4 rotated) = 5 MB worst case
5. **Dual output:** All log messages go to both the log file AND stderr
6. **Thread safety:** Mutex-protected writes; `_Atomic bool` for initialized flag
7. **Timestamp format:** ISO 8601 local time — `2026-03-07T14:32:05.123` (millisecond resolution)
8. **Zero dynamic allocation:** Static buffers only (matches server design)
9. **Security:** Path traversal check on log path, `fchmod(0640)` on log files, control character sanitization in log output

---

## Log Level Mapping

| Level | Usage | Example messages |
|-------|-------|------------------|
| **ERROR** | Fatal or unrecoverable | Server init/start failures, malloc failures, storage dir errors |
| **WARN** | Recoverable issues | Config parse warnings, CRC mismatch, transfer timeout, blob write short |
| **INFO** | Normal operations | Server startup/shutdown, config loaded, blob saved, transfer accepted/completed |
| **DEBUG** | Protocol details + block-level | UDS requests, routing activation, TransferData blocks, sequence counters |

---

## Phase 1: Logger Module (2 new files)

### Step 1.1: Create `include/doip_log.h` (Public Header)

**File:** `DOIP_Server/include/doip_log.h`

**Contents (~20 lines):**

- [ ] Header guard: `#ifndef DOIP_LOG_H` / `#define DOIP_LOG_H`
- [ ] Includes: `<stdarg.h>`
- [ ] Log level enum:
  ```c
  typedef enum {
      DOIP_LOG_ERROR = 0,   /* Always shown */
      DOIP_LOG_WARN  = 1,   /* Recoverable problems */
      DOIP_LOG_INFO  = 2,   /* Normal operations (default) */
      DOIP_LOG_DEBUG = 3,   /* Protocol-level detail */
  } doip_log_level_t;
  ```
- [ ] API declarations:
  ```c
  /**
   * @brief Initialize the logging system
   * @param log_file_path  Path to log file (NULL for console-only)
   * @param level          Log level threshold
   * @return 0 on success, -1 on critical failure (mutex init)
   */
  int doip_log_init(const char *log_file_path, doip_log_level_t level);

  /**
   * @brief Shut down the logging system, flush and close log file
   * @note MUST be called only after all server threads have exited
   *       (i.e., after doip_server_destroy() returns)
   */
  void doip_log_shutdown(void);

  /**
   * @brief Log a message at the specified level
   * Thread-safe. Messages below the configured level are discarded
   * before formatting (no wasted cycles).
   */
  void doip_log(doip_log_level_t level, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
  ```
- [ ] Convenience macros (readability wrappers, no performance claim):
  ```c
  #define LOG_ERROR(...)  doip_log(DOIP_LOG_ERROR, __VA_ARGS__)
  #define LOG_WARN(...)   doip_log(DOIP_LOG_WARN,  __VA_ARGS__)
  #define LOG_INFO(...)   doip_log(DOIP_LOG_INFO,  __VA_ARGS__)
  #define LOG_DEBUG(...)  doip_log(DOIP_LOG_DEBUG, __VA_ARGS__)
  ```
- [ ] `#endif` guard

**Removed from Round 1:** `doip_log_config_t` struct, `doip_log_set_level()`, `doip_log_get_level()`, `doip_log_level_str()` (public), TRACE level. Config struct replaced by 2-parameter init. Max file size and max files are compile-time constants.

---

### Step 1.2: Create `src/doip_log.c` (Implementation)

**File:** `DOIP_Server/src/doip_log.c`

**Contents (~80-100 lines) — section by section:**

- [ ] **FIRST LINE:** `#define _POSIX_C_SOURCE 200809L` — MUST be before ALL includes (matches `main.c` and `config.c` pattern)
- [ ] Includes: `"doip_log.h"`, `<stdio.h>`, `<string.h>`, `<time.h>`, `<sys/time.h>`, `<sys/stat.h>`, `<pthread.h>`, `<stdarg.h>`, `<errno.h>`, `<stdatomic.h>`, `<libgen.h>`
- [ ] Constants:
  ```c
  #define LOG_LINE_MAX        1024          /* Max formatted message length */
  #define LOG_MAX_FILE_SIZE   (1024 * 1024) /* 1 MB per file */
  #define LOG_MAX_FILES       5             /* active + 4 rotated */
  ```
- [ ] Static level strings (file-scope, not public API):
  ```c
  static const char *const LEVEL_NAMES[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
  ```
- [ ] Static state:
  ```c
  static struct {
      FILE               *fp;              /* Current log file handle (NULL = console-only) */
      char                path[256];       /* Log file path (owned copy) */
      doip_log_level_t    level;           /* Configured level threshold */
      pthread_mutex_t     mutex;           /* Thread safety for file I/O */
      uint32_t            current_size;    /* Bytes written to current file */
      _Atomic bool        initialized;     /* Thread-safe init guard */
  } g_log;
  ```
  **Note:** `initialized` is `_Atomic bool` per Thread Safety review — read without lock in `doip_log()`, written in init/shutdown.

- [ ] **`doip_log_init(const char *log_file_path, doip_log_level_t level)`:**
  1. `memset(&g_log, 0, sizeof(g_log))`
  2. `g_log.level = level`
  3. Check `pthread_mutex_init()` return value. On failure (non-zero): `fprintf(stderr, "doip_log: mutex init failed\n")`, return -1
  4. If `log_file_path` is not NULL:
     - Validate: `strstr(log_file_path, "..") != NULL` → reject with `fprintf(stderr, ...)`, set path to NULL (console-only)
     - Copy path: `strncpy(g_log.path, log_file_path, sizeof(g_log.path) - 1)`
     - **Create parent directory:** extract dir with `strrchr(path, '/')`, call `mkdir(dir, 0755)` (ignore EEXIST)
     - Open file: `fopen(g_log.path, "a")`
     - If open succeeds: `fchmod(fileno(g_log.fp), 0640)` — restrict permissions
     - If open fails: `fprintf(stderr, "doip_log: cannot open %s: %s\n", path, strerror(errno))`, `g_log.fp = NULL` (console-only, server still runs)
     - Get current file size: `fseek(fp, 0, SEEK_END)`, `long pos = ftell(fp)`, if `pos >= 0` set `current_size = (uint32_t)pos`, else set `current_size = 0`
  5. `atomic_store(&g_log.initialized, true)`
  6. `LOG_INFO("=== DoIP Log started (level=%s) ===", LEVEL_NAMES[level])`

- [ ] **`doip_log_shutdown()`:**
  **Precondition:** All server threads must be stopped (`doip_server_destroy()` has returned) before calling this function. No thread may call `LOG_*` after shutdown begins.
  1. `LOG_INFO("=== DoIP Log stopped ===")`
  2. Lock mutex
  3. `atomic_store(&g_log.initialized, false)` — prevents new callers
  4. If `g_log.fp`: `fflush()` then `fclose()`, set `g_log.fp = NULL`
  5. Unlock mutex
  6. `pthread_mutex_destroy(&g_log.mutex)`

- [ ] **`rotate_log_locked()`** (static, caller holds mutex):
  1. `fclose(g_log.fp)`, set `g_log.fp = NULL`
  2. Build rotated filenames using `snprintf()` from `g_log.path`:
     - Loop `i` from `LOG_MAX_FILES - 1` down to 1:
       - Build `old_name` (`.log.{i-1}` for i>1, or `g_log.path` for i==1) and `new_name` (`.log.{i}`)
       - For i == `LOG_MAX_FILES - 1`: `remove(new_name)` first (delete oldest, ignore ENOENT)
       - `rename(old_name, new_name)` — log failure to stderr if `rename()` returns non-zero
  3. Open fresh file: `g_log.fp = fopen(g_log.path, "w")`
     - If open succeeds: `fchmod(fileno(g_log.fp), 0640)`
     - If open fails: attempt `fopen(g_log.path, "a")` as fallback (append to whatever exists)
     - If both fail: `fprintf(stderr, "doip_log: rotation failed, file logging lost\n")`, `g_log.fp = NULL` (console-only degradation)
  4. Reset `g_log.current_size = 0`

- [ ] **`sanitize_line()`** (static helper):
  - Replace bytes 0x00-0x08, 0x0B-0x1F, 0x7F with `'?'` in the formatted line buffer
  - Preserves 0x09 (tab) and 0x0A (newline)
  - Prevents terminal escape injection from config-sourced strings

- [ ] **`doip_log(doip_log_level_t level, const char *fmt, ...)`:**
  1. Early return if `!atomic_load(&g_log.initialized)`
  2. Early return if `level > g_log.level` (level filter — no formatting overhead)
  3. **Build timestamp:**
     - `struct timeval tv; gettimeofday(&tv, NULL)`
     - `struct tm tm_buf; struct tm *tm = localtime_r(&tv.tv_sec, &tm_buf)`
     - If `localtime_r()` returns NULL: use fallback `"0000-00-00T00:00:00.000"` (do not crash)
     - Format: `"2026-03-07T14:32:05.123"` using `snprintf` with `tm` fields and `tv.tv_usec / 1000`
  4. **Format message:** `char msg[LOG_LINE_MAX]; va_start(args, fmt); int n = vsnprintf(msg, sizeof(msg), fmt, args); va_end(args)`
     - **Truncation indicator:** if `n >= LOG_LINE_MAX`, overwrite last 3 bytes with `"..."` to visually show truncation
  5. **Build full line:** `char line[LOG_LINE_MAX + 64]; snprintf(line, sizeof(line), "[%s] [%s] %s\n", timestamp, LEVEL_NAMES[level], msg)`
  6. **Sanitize:** call `sanitize_line(line)` to scrub control characters
  7. **Lock mutex**
  8. **Write to file** (if `g_log.fp`):
     - `int wr = fputs(line, g_log.fp)`
     - If `wr == EOF`: `clearerr(g_log.fp)` (reset error indicator, continue attempting)
     - `fflush(g_log.fp)` — immediate flush for crash safety
     - Update `g_log.current_size += strlen(line)`
     - If `g_log.current_size >= LOG_MAX_FILE_SIZE`: call `rotate_log_locked()`
  9. **Unlock mutex**
  10. **Write to console:** `fputs(line, stderr)` — all levels go to stderr (standard Unix convention for server processes). Console output is outside the mutex — stderr has its own internal lock.
      **Note:** Console output ordering may differ from file ordering under thread contention. This is cosmetic, not a correctness issue.

---

## Phase 2: Command-Line Integration

### Step 2.1: Add CLI Options to `main.c`

- [ ] Add `#include "doip_log.h"` at top of `main.c`
- [ ] Add new CLI options to argument parsing. New flag checks go INSIDE the `while (i < argc)` loop, BEFORE the positional argument fallback:
  ```
  -v          DEBUG level (strcmp(argv[i], "-v"))
  -vv         Same as -v (strcmp(argv[i], "-vv") — convenience alias, same effect)
  -q          WARN level (strcmp(argv[i], "-q"))
  -qq         ERROR level (strcmp(argv[i], "-qq"))
  -l PATH     Set log file path — requires i+1 < argc guard (same pattern as -c)
  ```
  **Path validation for `-l`:** `strstr(path, "..") != NULL` → `fprintf(stderr, "Error: log path must not contain '..'\n"); return 1;`
- [ ] Track verbosity as int: start at 2 (INFO), `-v`/`-vv` sets to 3, `-q` sets to 1, `-qq` sets to 0. Clamp to [0,3].
- [ ] Initialize logger **immediately after CLI argument parsing completes, BEFORE config load status messages**. The `-l`/`-v`/`-q` values are known at this point. This ensures the config load status messages ("Loaded config: ..." / "No config file found...") go through the logger:
  ```c
  /* After CLI parsing loop, before config load */
  const char *log_path = cli_log_path ? cli_log_path : "/var/FC-1-DOIP.log";
  if (doip_log_init(log_path, (doip_log_level_t)verbosity) != 0) {
      fprintf(stderr, "Warning: logger init failed, continuing with stderr only\n");
  }

  /* Then: doip_config_defaults(), doip_config_load(), CLI overrides, doip_config_print() */
  ```
  **Note:** Logger init does NOT depend on the config file (log settings are CLI-only). This ordering eliminates the pre-init LOG_* problem — all messages after this point go through the logger.
- [ ] Call `doip_log_shutdown()` at end of main, AFTER `doip_server_destroy()` and before `return`. This ensures all client handler threads have exited and will not call `LOG_*` after shutdown.
- [ ] Update `print_usage()` to include new options

**Removed from Round 1:** `--no-log`, `--no-console` flags. Use `-l /dev/null` for no file logging. Use shell redirection `2>/dev/null` for no console output. Standard Unix practice.

---

## Phase 3: Replace All printf/fprintf Calls

### Step 3.1: Convert `main.c` (50 calls)

- [ ] Add `#include "doip_log.h"` at top (if not already added in Step 2.1)

Replace every `printf`/`fprintf` with the appropriate `LOG_*` macro. Mapping:

**Startup/shutdown (→ INFO):**
- [ ] `printf("========================================\n")` and banner → `LOG_INFO("DoIP Blob Server starting")`
- [ ] `printf("Loaded config: %s\n", ...)` → `LOG_INFO("Loaded config: %s", ...)`
- [ ] `printf("No config file found, using defaults\n")` → `LOG_INFO("No config file found, using defaults")`
- [ ] `printf("Max blob size: ...")` → `LOG_INFO("Max blob size: %u bytes (%u MB)", ...)`
- [ ] `printf("Transfer timeout: ...")` → `LOG_INFO("Transfer timeout: %u seconds", ...)`
- [ ] `printf("Storage: %s/\n", ...)` → `LOG_INFO("Storage: %s/", ...)`
- [ ] `printf("Server running. Press Ctrl+C to stop.\n")` → `LOG_INFO("Server running (Ctrl+C to stop)")`
- [ ] `printf("Shutting down...\n")` → `LOG_INFO("Shutting down...")`
- [ ] `printf("Server stopped.\n")` → `LOG_INFO("Server stopped")`

**Errors (→ ERROR):**
- [ ] All `fprintf(stderr, "Error: ...")` → `LOG_ERROR(...)`
- [ ] `fprintf(stderr, "Failed to init server: ...")` → `LOG_ERROR("Failed to init server: %s", ...)`
- [ ] `fprintf(stderr, "Failed to start server: ...")` → `LOG_ERROR("Failed to start server: %s", ...)`
- [ ] `fprintf(stderr, "Failed to register target: ...")` → `LOG_ERROR("Failed to register target: %s", ...)`
- [ ] `perror("[Blob Server] Failed to open blob file")` → `LOG_ERROR("Failed to open blob file: %s", strerror(errno))`
- [ ] malloc failure → `LOG_ERROR("RequestDownload rejected: malloc(%u) failed", ...)`

**Warnings (→ WARN):**
- [ ] `fprintf(stderr, "Warning: ...")` → `LOG_WARN(...)`
- [ ] CRC mismatch → `LOG_WARN("CRC-32 MISMATCH: computed=0x%08X, expected=0x%08X", ...)`
- [ ] Transfer timeout → `LOG_WARN("Transfer timed out, aborting")`
- [ ] Blob write short → `LOG_WARN("Wrote %zu of %u bytes to %s", ...)`
- [ ] Storage dir unavailable → `LOG_WARN("Storage directory '%s' unavailable", ...)`
- [ ] Announcement failed → `LOG_WARN("Initial announcement failed")`
- [ ] Blob too small for CRC → `LOG_WARN("Blob too small for CRC verification (%u bytes)", ...)`
- [ ] `fclose` failed → `LOG_WARN("fclose failed: %s", strerror(errno))`

**Protocol operations (→ DEBUG):**
- [ ] `printf("[Blob Server] UDS request from ...")` → `LOG_DEBUG("UDS request from 0x%04X: SID=0x%02X, len=%u", ...)`
- [ ] `printf("[Blob Server] Routing activation: ...")` → `LOG_DEBUG("Routing activation: SA=0x%04X, type=0x%02X — accepted", ...)`
- [ ] `printf("[Blob Server] RequestDownload accepted: ...")` → `LOG_DEBUG("RequestDownload accepted: addr=0x%08X, size=%u", ...)`
- [ ] `printf("[Blob Server] RequestDownload rejected: ...")` → `LOG_DEBUG("RequestDownload rejected: ...")`
- [ ] `printf("[Blob Server] TransferExit: ...")` → `LOG_DEBUG("TransferExit: %u/%u bytes received", ...)`
- [ ] `printf("[Blob Server] CRC-32 verified: ...")` → `LOG_DEBUG("CRC-32 verified: OK (0x%08X)", ...)`
- [ ] `printf("[Blob Server] Saved: %s\n", ...)` → `LOG_INFO("Blob saved: %s", ...)`
- [ ] `printf("[Blob Server] TransferData block %u: ...")` → `LOG_DEBUG("TransferData block %u: %u bytes (total: %u/%u)", ...)`
- [ ] `printf("[Blob Server] TransferData wrong sequence: ...")` → `LOG_DEBUG("TransferData wrong sequence: expected %u, got %u", ...)`
- [ ] `printf("[Blob Server] TransferData exceeds requested size\n")` → `LOG_DEBUG("TransferData exceeds requested size")`

### Step 3.2: Convert `config.c` (32 calls)

- [ ] Add `#include "doip_log.h"` at top of `config.c`
- [ ] **DO NOT convert** the 17 `fprintf(stderr, ...)` calls in `doip_config_load()` — these run before `doip_log_init()` and must stay as `fprintf` to avoid silent loss
- [ ] Convert all 15 `printf(...)` calls in `doip_config_print()` → `LOG_INFO(...)` — this function is called AFTER logger init, so conversion is safe

### Step 3.3: Remove `[Blob Server]` Prefix

- [ ] The logger adds `[TIMESTAMP] [LEVEL]` automatically — remove the manual `[Blob Server]` prefix from all messages to avoid double-tagging

---

## Phase 4: Build Integration

### Step 4.1: Update Makefile

- [ ] Update all three `*_SRCS` variables:
  ```makefile
  SERVER_SRCS      = src/main.c src/doip.c src/doip_server.c src/config.c src/doip_log.c
  TEST_SRCS        = test/test_discovery.c src/doip.c src/doip_client.c src/config.c src/doip_log.c
  TEST_SERVER_SRCS = test/test_server.c src/doip.c src/doip_client.c src/config.c src/doip_log.c
  ```
- [ ] No changes needed to `clean`, `.PHONY`, or build rules
- [ ] **Test binaries:** Tests link `doip_log.c` but do not call `doip_log_init()`. Since config parser warnings stay as `fprintf` (Step 3.2 decision), test behavior is unchanged. Logger calls from `doip_config_print()` will be silently discarded in tests (acceptable — tests don't need config pretty-printing).

### Step 4.2: Build Verification

- [ ] Clean build: `make clean && make` — zero warnings with `-Wall -Wextra -Wpedantic`
- [ ] Verify log file created on startup: `ls -la /var/FC-1-DOIP.log`
- [ ] Verify `-v` shows DEBUG messages (UDS requests, block details)
- [ ] Verify `-q` shows only WARN+ERROR messages
- [ ] Verify `-l /tmp/test.log` redirects to custom path

---

## Phase 5: Testing

### Step 5.1: Manual Testing

- [ ] Start server with default level: `./doip-server -c doip-server.conf`
  - Verify log file created at `/var/FC-1-DOIP.log` with permissions 0640
  - Verify startup messages at INFO level in both file and stderr
  - Run a blob transfer, verify INFO-level completion message in log
- [ ] Start with `-v`: verify DEBUG messages appear (UDS requests, block details, sequence counters)
- [ ] Start with `-q`: verify only WARN and ERROR messages appear
- [ ] Test rotation:
  - Temporarily change `LOG_MAX_FILE_SIZE` to 4096, rebuild
  - Run multiple transfers to generate >20 KB of log
  - Verify exactly 5 log files exist (`.log`, `.log.1`, `.log.2`, `.log.3`, `.log.4`)
  - Verify `.log` is the newest, `.log.4` is the oldest
- [ ] Test `-l /dev/null`: verify no log file written, stderr still works
- [ ] Test path traversal rejection: `-l /tmp/../etc/test` should be rejected

### Step 5.2: Thread Safety Verification

- [ ] Run multiple concurrent clients sending blobs
- [ ] Verify no interleaved/corrupted log lines
- [ ] Verify no deadlocks during rotation under load

---

## File Change Summary

| File | Action | Description |
|------|--------|-------------|
| `include/doip_log.h` | **CREATE** | Log level enum, init/shutdown/log declarations, 4 convenience macros |
| `src/doip_log.c` | **CREATE** | Logger: init, shutdown, rotate, sanitize, thread-safe log writer |
| `src/main.c` | **MODIFY** | Add `#include "doip_log.h"`, `-v`/`-q`/`-l` CLI parsing, init/shutdown calls, convert 50 printf/fprintf to LOG_* |
| `src/config.c` | **MODIFY** | Add `#include "doip_log.h"`, convert 15 printf in `doip_config_print()` to LOG_INFO (17 fprintf in `doip_config_load()` stay as-is) |
| `Makefile` | **MODIFY** | Add `src/doip_log.c` to SERVER_SRCS, TEST_SRCS, TEST_SERVER_SRCS |

**New code estimate:** ~100-120 lines total (~20 header, ~80-100 implementation)
**Modified code:** ~65 call-site conversions in `main.c` (50) and `config.c` (15 of 32)

**Removed from Round 1:** `config.h` modifications (no log config fields), `doip-server.conf` modifications (no log config keys), `doip_log_config_t` struct, `doip_log_set_level()`/`doip_log_get_level()`/`doip_log_level_str()` public API, `--no-log`/`--no-console` flags, TRACE level.

---

## Implementation Order

1. Step 1.1 — Header (`doip_log.h`)
2. Step 1.2 — Implementation (`doip_log.c`)
3. Step 4.1 — Makefile (get it building)
4. Step 2.1 — CLI options (`main.c` arg parsing + init/shutdown)
5. Step 3.1 — Convert `main.c` printf/fprintf (50 calls)
6. Step 3.2 — Convert `config.c` doip_config_print() (15 calls)
7. Step 3.3 — Remove `[Blob Server]` prefixes
8. Step 4.2 — Build verification
9. Step 5.1 — Manual testing
10. Step 5.2 — Thread safety testing

---

## Key Design Decisions

1. **Append mode on open** — On restart, continue writing to existing log. Rotation handles size limits.
2. **Immediate flush** — `fflush()` after every write for crash safety. Acceptable for ~1-10 writes/sec. **Production note:** Use INFO level or above for production. DEBUG under high traffic can impact throughput due to per-message fflush and mutex contention.
3. **All console output to stderr** — Standard Unix convention for server processes. Do not split between stdout/stderr by level.
4. **Console output outside mutex** — stderr has its own internal lock. Ordering may differ from file under contention (cosmetic, not a correctness issue).
5. **Level check before format** — `doip_log()` checks level before `vsnprintf()`, so DEBUG messages cost nothing when running at INFO.
6. **Static state, no heap** — Logger uses a single static struct. No malloc, no cleanup complexity.
7. **perror() replacement** — `perror()` calls replaced with `LOG_ERROR("msg: %s", strerror(errno))`.
8. **Config parser warnings stay as fprintf** — `doip_config_load()` runs before logger init. Converting those to LOG_WARN would silently discard them. Keeping fprintf preserves existing behavior.
9. **`_Atomic bool initialized`** — Read without lock for fast path in `doip_log()`, written with atomic_store in init/shutdown. Matches project's existing `_Atomic bool running` pattern.
10. **Log file permissions 0640** — Not world-readable. Applied via `fchmod()` after `fopen()`.
11. **No symlink check on rotation** — `rename()` operates on directory entries, not symlink targets. Symlink protection is not needed here (KISS R2 finding).
12. **Control character sanitization** — Prevents terminal escape injection from config-sourced strings logged at WARN/INFO.

---

## Known Limitations

1. **Log file deleted while running** — If an external process deletes the log file, writes continue to the deleted inode (POSIX behavior). The logger does not detect this. Use `logrotate` with `copytruncate` if external rotation is needed.
2. **Pre-init warnings** — Config parser warnings emitted before `doip_log_init()` go to stderr only (not to the log file). This is by design (Step 3.2).

---

## Round 1 Review Resolutions

6-agent review (Thread Safety, Correctness, Error Handling, Integration, Security, KISS). Round 1 found 24 FAILs across agents (20 unique after dedup). All 20 fixes applied.

| ID | Issue | Source Agent(s) | Resolution |
|----|-------|----------------|------------|
| R1-1 | 5 levels over-engineered | KISS | Reduced to 4 levels: ERROR, WARN, INFO, DEBUG. TRACE merged into DEBUG |
| R1-2 | doip_log_config_t unnecessary | KISS | Replaced with 2-param init: `(path, level)`. Max size/files are `#define` constants |
| R1-3 | --no-log/--no-console unnecessary | KISS | Removed. Use `-l /dev/null` or shell redirection |
| R1-4 | set/get level have no callers | KISS | Removed `doip_log_set_level()` and `doip_log_get_level()` |
| R1-5 | doip_log_level_str() needlessly public | KISS | Made static (`LEVEL_NAMES[]` array in doip_log.c) |
| R1-6 | Config file keys unnecessary | KISS | Removed log_file/log_level/log_max_size from config.h/config.c. CLI flags suffice |
| R1-7 | 310 lines disproportionate | KISS | Target ~100-120 lines. Removed unnecessary abstractions |
| R1-8 | stdout/stderr split unnecessary | KISS | All console output to stderr |
| R1-9 | `initialized` not atomic | Thread Safety | Changed to `_Atomic bool`, uses `atomic_load`/`atomic_store` |
| R1-10 | Shutdown race with mutex_destroy | Thread Safety | Document precondition: call after doip_server_destroy(). Set initialized=false under lock before close |
| R1-11 | Sub-counts wrong (21/11 → 17/15) | Correctness | Fixed: 17 fprintf + 15 printf in config.c |
| R1-12 | Rotation rename failure unhandled | Correctness, Error Handling | Log rename failures to stderr; attempt append fallback if fresh open fails |
| R1-13 | fputs return value unchecked | Error Handling | Check fputs return, clearerr on EOF |
| R1-14 | pthread_mutex_init unchecked | Error Handling | Check return, return -1 on failure |
| R1-15 | Log directory may not exist | Error Handling | mkdir parent directory before fopen |
| R1-16 | localtime_r may return NULL | Error Handling | Fallback timestamp string on NULL |
| R1-17 | vsnprintf truncation silent | Error Handling | Overwrite last 3 bytes with "..." on truncation |
| R1-18 | _POSIX_C_SOURCE after includes | Integration | Reordered: first bullet in Step 1.2 |
| R1-19 | Missing #include "doip_log.h" | Integration | Added explicit include instructions for main.c and config.c |
| R1-20 | Makefile incomplete | Integration | Show all three *_SRCS lines explicitly |
| R1-21 | Log path traversal not validated | Security | strstr ".." check on both -l CLI and init path |
| R1-22 | Log file world-readable | Security | fchmod(fileno(fp), 0640) after fopen |
| R1-23 | Symlink attack on rotation | Security | lstat() check before remove/rename (subsequently removed in R2 — see below) |
| R1-24 | Control chars in log output | Security | sanitize_line() scrubs 0x00-0x08, 0x0B-0x1F, 0x7F → '?' |

**Items confirmed PASS (no changes needed):**
- Mutex scope / deadlock with g_transfer_mutex (lock ordering always g_transfer_mutex → g_log.mutex)
- Rotation rename chain order (correct: delete oldest first, shift down)
- CLI flags non-conflicting
- Config parsing order (defaults → config → CLI)
- No circular dependencies (doip_log.h has no project includes)
- Format string safety (__attribute__((format)))
- VIN/identity data logging (bounded format specifiers, not sensitive)
- EINTR handling (not needed for stdio functions)

---

## Round 2 Review Resolutions

6-agent Round 2 review. 4 agents PASS, 2 agents FAIL (4 unique issues). All 4 fixes applied.

| ID | Issue | Source Agent(s) | Resolution |
|----|-------|----------------|------------|
| R2-1 | lstat() symlink check unnecessary — rename() doesn't follow symlinks | KISS | Removed lstat() check from rotate_log_locked() |
| R2-2 | Overview says "all 82" but only 65 are converted | Correctness | Fixed: "replaces 65 of the 82" with explanation of remaining 17 |
| R2-3 | Step 3.2 contradiction — checkbox says convert 17 fprintf, decision says keep them | Correctness | Removed contradictory checkbox, kept only the DO NOT convert instruction |
| R2-4 | Pre-init LOG_* calls in main.c (~7 calls before logger init) | Correctness | Moved logger init to right after CLI parsing, before config load. Logger doesn't depend on config file |
