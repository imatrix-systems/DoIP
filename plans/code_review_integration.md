# Integration and Build Review: Phone-Home Integration

**Reviewer:** Integration and Build Review Agent
**Date:** 2026-03-09
**Files reviewed:**
- `Makefile`
- `src/main.c`
- `src/config.c`
- `include/config.h`
- `doip-server.conf`
- `test/test_phonehome.c`
- `scripts/systemd/phonehome-keygen.service`
- `scripts/systemd/phonehome-register.service`
- `scripts/initd/phonehome-keygen`
- `scripts/initd/phonehome-register`
- `etc/phonehome/phonehome.conf`

---

## 1. Makefile: All targets build cleanly, no circular dependencies, install paths correct

**Verdict: PASS**

Build verification performed with `make clean && make all`. All four targets compile with zero warnings under `-Wall -Wextra -Wpedantic -O2 -g -std=c11`:

```
doip-server:     main.c doip.c doip_server.c config.c doip_log.c phonehome_handler.c hmac_sha256.c
test-discovery:  test_discovery.c doip.c doip_client.c config.c doip_log.c
test-server:     test_server.c doip.c doip_client.c config.c doip_log.c
test-phonehome:  test_phonehome.c phonehome_handler.c hmac_sha256.c doip_log.c
```

Key observations:
- `PHONEHOME_SRCS` factored out as a variable (line 7), DRY reference in `SERVER_SRCS` via `$(PHONEHOME_SRCS)`.
- `TEST_PH_SRCS` (line 12) is independent -- no shared objects with server targets, no link-order issues.
- `all` target (line 23) builds all four binaries. No circular target dependencies.
- `clean` target (line 103) removes all four binaries.
- `ci-test` target (line 76) depends on all four binaries, runs phone-home unit tests first (fast/offline), then smoke and full tests. Ordering is correct.
- `run-test-phonehome` (line 50) is a standalone phony target for quick phone-home-only testing.

Install paths (lines 87-100):
- Server binary: `$(DESTDIR)$(PREFIX)/sbin/doip-server` -- correct for system daemons.
- Helper scripts: `$(DESTDIR)$(PREFIX)/sbin/phonehome-*.sh` -- correct, matches ExecStart in systemd units.
- Config dir: `$(DESTDIR)/etc/phonehome/` with mode 700 -- correct, restricts access to root.
- Config file: `$(DESTDIR)/etc/phonehome/phonehome.conf` with mode 644 -- Note: this file itself contains no secrets (HMAC secret is a separate file), so 644 is appropriate.
- Systemd units: `$(DESTDIR)/etc/systemd/system/` with mode 644.
- Init.d scripts: `$(DESTDIR)/etc/init.d/` with mode 755.

---

## 2. Config struct: new field at end, properly zeroed by memset in defaults

**Verdict: PASS**

In `include/config.h` (line 24):
```c
char    phonehome_config_path[256]; /* Path to phonehome.conf, empty = disabled */
```

This is the last field in `doip_app_config_t`. Adding a field at the end of a struct is ABI-safe -- no existing field offsets change.

In `config.c` line 93, `doip_config_defaults()` starts with:
```c
memset(config, 0, sizeof(*config));
```

This zeroes all bytes including `phonehome_config_path[256]`, giving `phonehome_config_path[0] == '\0'` (disabled by default). The subsequent field initializations don't touch `phonehome_config_path`, leaving it zeroed. Correct.

---

## 3. main.c: phonehome_cfg declared as static local -- lifetime valid for handler's g_cfg pointer

**Verdict: PASS**

In `main.c` line 588:
```c
static phonehome_config_t phonehome_cfg;
```

The `static` keyword gives this variable static storage duration -- it lives for the entire program lifetime, not just the enclosing function scope. This is critical because `phonehome_init()` (in `phonehome_handler.c` line 219) stores the pointer:
```c
g_cfg = cfg;
```

And the handler later reads `g_cfg->connect_script`, `g_cfg->bastion_host`, `g_cfg->lock_file` from worker threads. Since `phonehome_cfg` has static storage duration, `g_cfg` remains valid until `phonehome_shutdown()` sets `g_cfg = NULL` on line 489, which happens after `doip_server_destroy()` on line 684. The lifetime is correct.

Additional notes:
- Phone-home init is after config load (line 587-601), so the `phonehome_config_path` is populated.
- Phone-home failure is non-fatal: `LOG_WARN` and the server continues without phone-home capability. This is the correct degradation behavior.
- `phonehome_shutdown()` is called on line 685, after `doip_server_destroy()` ensures all server threads have exited. No use-after-shutdown race.

---

## 4. doip-server.conf: phonehome_config commented out by default

**Verdict: PASS**

Line 26-27 of `doip-server.conf`:
```
# Phone-Home config path (uncomment to enable phone-home capability)
# phonehome_config    = /etc/phonehome/phonehome.conf
```

Both lines are comments (leading `#`). The config parser in `config.c` skips comment lines at line 149 (`if (*p == '\0' || *p == '#')`). Existing users who upgrade will see no behavior change -- `phonehome_config_path` remains zeroed, and `main.c` line 589 checks `phonehome_config_path[0]` which will be `'\0'`, hitting the else branch at line 600 that logs "Phone-home not configured" and continues.

---

## 5. test_phonehome.c: links without doip_server.c, config.c, doip.c

**Verdict: PASS**

`TEST_PH_SRCS` on line 12 of `Makefile`:
```
TEST_PH_SRCS = test/test_phonehome.c src/phonehome_handler.c src/hmac_sha256.c src/doip_log.c
```

This is a minimal, self-contained link set. Verified:
- `test_phonehome.c` includes `hmac_sha256.h` and `phonehome_handler.h` -- both provided by linked sources.
- `phonehome_handler.c` includes `phonehome_handler.h`, `hmac_sha256.h`, `doip_log.h` -- all satisfied.
- No symbol references to `doip_server_*`, `doip_client_*`, `doip_config_*`, or anything from `doip.c`.
- `doip_log.c` is included for the `LOG_*` macros used by `phonehome_handler.c`. Without `doip_log_init()` being called in the test, the logger fast-paths to no-op (guarded by `atomic_load(&g_log.initialized)`).
- Build confirms: zero warnings, zero unresolved symbols.

The test binary is lightweight and can run offline without a server, which is ideal for CI pre-flight checks.

---

## 6. Systemd units: correct ordering (keygen before register, register after network)

**Verdict: PASS**

**phonehome-keygen.service:**
- `Before=network-pre.target phonehome-register.service` -- runs before network and before register. Correct: key generation needs no network and must complete before registration.
- `ConditionPathExists=!/etc/phonehome/.keygen_complete` -- idempotent, only runs on first boot. The `!` prefix means "skip if file exists."
- `DefaultDependencies=no` -- necessary for early-boot services that run before `sysinit.target`.
- `Type=oneshot`, `RemainAfterExit=yes` -- correct for a one-shot generator; systemd considers it "active" after completion, allowing `Requires=phonehome-keygen.service` in register to work.

**phonehome-register.service:**
- `After=network-online.target phonehome-keygen.service` -- waits for both network and key generation. Correct: registration requires HTTPS connectivity and existing keys.
- `Requires=phonehome-keygen.service` -- hard dependency. If keygen fails, register won't start. Correct.
- `ConditionPathExists=!/etc/phonehome/.registration_complete` -- idempotent, only runs once.
- `ConditionPathExists=/etc/phonehome/id_ed25519` -- double-guard; won't attempt registration without a key file.
- `Restart=on-failure`, `RestartSec=60` -- note: `Restart=on-failure` on a `Type=oneshot` service means if the script exits non-zero, systemd will retry after 60s. This is appropriate for transient network failures during first-boot registration.

The dependency chain is: `keygen -> network-online -> register`. No cycles.

---

## 7. Init.d scripts: LSB headers correct, dependencies match systemd

**Verdict: PASS**

**phonehome-keygen (init.d):**
- `Required-Start:` (empty) -- no dependencies, runs early. Matches systemd's `DefaultDependencies=no`.
- `Default-Start: S` -- runs in single-user/boot runlevel. Correct for pre-network key generation.
- `Default-Stop:` (empty) -- nothing to stop for a one-shot. Correct.
- Sentinel check (`if [ -f "$SENTINEL" ]`) mirrors systemd's `ConditionPathExists=!...`.
- `stop)` case is a no-op. Correct for one-shot.
- `status)` case reports sentinel state. Correct.

**phonehome-register (init.d):**
- `Required-Start: $network phonehome-keygen` -- matches systemd's `After=network-online.target` + `Requires=phonehome-keygen.service`.
- `Default-Start: 2 3 4 5` -- standard multi-user runlevels. Correct for a service needing network.
- Sentinel check mirrors systemd `ConditionPathExists=!...`.
- Key file check (`if [ ! -f "$KEY_FILE" ]`) mirrors systemd `ConditionPathExists=/etc/phonehome/id_ed25519`.
- Exit 1 on missing key (vs exit 0 on already registered). Correct differentiation.

Both scripts have proper LSB `### BEGIN/END INIT INFO` blocks, `Provides:` names match systemd unit names, and all four standard actions (`start|stop|status|*`) are handled.

---

## 8. phonehome.conf example: all keys present with reasonable defaults

**Verdict: PASS**

Contents of `etc/phonehome/phonehome.conf`:
```
BASTION_HOST=bastion-dev.imatrixsys.com
BASTION_PORT=22
TUNNEL_TIMEOUT=3600
RETRY_ON_DISCONNECT=0
LOG_LEVEL=info
DOIP_SOURCE_ADDR=0x0001
HMAC_SECRET_FILE=/etc/phonehome/hmac_secret
CONNECT_SCRIPT=/usr/sbin/phonehome-connect.sh
LOCK_FILE=/var/run/phonehome.lock
```

Cross-referencing with `phonehome_config_load()` in `phonehome_handler.c`:
- **Required keys:** `HMAC_SECRET_FILE` (present, line 10), `CONNECT_SCRIPT` (present, line 11). Both have reasonable production paths.
- **Optional keys with defaults:** `BASTION_HOST` (present, placeholder `bastion-dev.imatrixsys.com` matches code default), `LOCK_FILE` (present, `/var/run/phonehome.lock` matches code default).
- **Shell-script-only keys:** `BASTION_PORT`, `TUNNEL_TIMEOUT`, `RETRY_ON_DISCONNECT`, `LOG_LEVEL`, `DOIP_SOURCE_ADDR` -- the C parser ignores these (line 168 comment: "Ignore other keys...used by shell scripts"). Their presence is correct for the shell scripts that source this file.

All values are reasonable:
- `BASTION_PORT=22` -- standard SSH.
- `TUNNEL_TIMEOUT=3600` -- 1 hour, reasonable for remote debug.
- `RETRY_ON_DISCONNECT=0` -- conservative default (no auto-retry).
- `HMAC_SECRET_FILE=/etc/phonehome/hmac_secret` -- under the 700-mode `/etc/phonehome/` directory.
- `CONNECT_SCRIPT=/usr/sbin/phonehome-connect.sh` -- matches install target in Makefile line 90.

---

## Overall Verdict: PASS

All 8 integration and build review items pass. The phone-home feature integrates cleanly into the existing build system, maintains backward compatibility for existing users (config commented out by default), has correct static storage lifetime for the handler configuration pointer, builds all targets with zero warnings, provides a minimal self-contained test binary, and has correctly ordered systemd and init.d service definitions with matching dependency semantics.
