# DoIP Blob Server

A DoIP (Diagnostics over IP) blob server that receives data blobs from Fleet-Connect-1 devices via ISO 13400-2. Supports dual-platform builds (Ubuntu/Torizon), interactive CLI, daemon mode, self-generated operational scripts, structured logging with file rotation, phone-home reverse SSH tunnels, and an automated 53-test suite.

## Prerequisites

### Ubuntu (development/testing)

- Linux PC (tested on Ubuntu/Debian, WSL2)
- GCC with C11 support
- GNU Make
- pthreads (included with glibc)

```bash
sudo apt install build-essential
gcc --version   # GCC 7+ required for C11
```

### Torizon (embedded target)

- QConnect SDK at `/opt/qconnect_sdk_musl/` (same toolchain as Fleet-Connect-1)
- Target: Toradex Verdin iMX8MPlus SoM running Torizon Linux (not containerized)
- Toolchain: `arm-linux-gcc` (GCC 6.4.0, Buildroot, musl libc, ARM hard-float)

See [BUILD_GUIDE.md](BUILD_GUIDE.md) for detailed cross-compilation instructions.

## Build

```bash
cd DoIP_Server

# Ubuntu (default — native build)
make

# Torizon (cross-compile for Verdin iMX8MPlus)
make PLATFORM=torizon

# Clean and rebuild
make clean && make
```

Both platforms compile with `-Wall -Wextra -Wpedantic -std=c11` — zero warnings expected.

## Quick Start

```bash
# 1. Build
make

# 2. Run all tests (53 tests: 7 phone-home + 6 discovery + 40 full server)
make ci-test

# 3. Run the server interactively
./doip-server -c doip-server.conf

# 4. Use CLI commands at the doip> prompt
doip> status
doip> config
doip> help
doip> quit
```

The server listens on 127.0.0.1:13400 (TCP) by default. UDP always binds to `INADDR_ANY` (all interfaces) regardless of `bind_address`, as required for broadcast discovery reception.

## Usage

```
./doip-server [-c config_file] [-d] [-v|-vv] [-q|-qq] [-l logfile] [bind_ip] [port]
```

| Flag | Description |
|------|-------------|
| `-c config` | Configuration file (default: `doip-server.conf` in CWD) |
| `-d` | Daemon mode — double-fork, PID file, headless |
| `-v` | Verbose — show DEBUG messages |
| `-vv` | Extra verbose (same as `-v`) |
| `-q` | Quiet — ERROR + WARN only |
| `-qq` | Very quiet — ERROR only |
| `-l path` | Log file path (default: `/var/FC-1-DOIP.log`) |
| `-h` | Show help |
| `bind_ip` | Bind address (overrides config) |
| `port` | TCP/UDP port (overrides config) |

### Interactive Mode (default)

When stdin is a TTY, the server presents an interactive CLI prompt:

```bash
./doip-server -c doip-server.conf
```

```
doip> status
  Server:     running
  Uptime:     2h 15m 33s
  Platform:   Ubuntu
  Clients:    1 / 4
  Transfer:   none

doip> config
  VIN:               APTERADOIPSRV0001
  Logical address:   0x0001
  EID:               00:1A:2B:3C:4D:5E
  GID:               00:1A:2B:3C:4D:5E
  Bind address:      127.0.0.1
  TCP port:          13400
  UDP port:          13400
  Max TCP conns:     4
  Max data size:     4096
  Blob storage:      /tmp/doip_blobs
  Blob max size:     16777216 bytes (16 MB)
  Transfer timeout:  30 seconds
  Phone-home config: (disabled)
  PID file:          /var/run/doip-server.pid
  Script output dir: /etc/doip/scripts

doip> transfer
  No active transfer.

doip> generate-scripts /tmp/scripts
  Writing scripts to: /tmp/scripts
  5 of 5 scripts written.

doip> help
  Available commands:
    status              Show server status and uptime
    config              Display current configuration
    transfer            Show active transfer progress
    generate-scripts [dir]  Write operational scripts
    help                Show this help
    quit / exit         Shut down the server

doip> quit
  Shutting down...
```

#### CLI Commands Reference

| Command | Description |
|---------|-------------|
| `status` | Server uptime, platform, connected clients, active transfer progress |
| `config` | Full configuration display (VIN, network, storage, phone-home, daemon settings) |
| `transfer` | Detailed transfer progress: bytes received, percentage, block number, idle time |
| `generate-scripts [dir]` | Write all operational scripts to directory (default: config's `script_output_dir`) |
| `help` or `?` | List available commands |
| `quit` or `exit` | Graceful shutdown (stops server, cleans up transfers) |

All CLI commands are **read-only** — configuration changes require editing the config file and restarting.

#### Non-TTY Mode

When stdin is not a TTY (piped input, backgrounded by test harness, etc.), the CLI is disabled to prevent `SIGTTIN` signals. The server runs in a simple sleep loop, checking for transfer timeouts every second.

```bash
# Piped — no CLI, runs until signal
echo "" | ./doip-server -c doip-server.conf &

# Test harness — no CLI interference
./doip-server -c doip-server.conf &
```

### Daemon Mode

Run as a background process with `-d`:

```bash
./doip-server -d -c doip-server.conf -l /var/log/doip-server.log
```

Daemon mode:
- **Double-fork** — fully detaches from terminal (POSIX daemon pattern)
- **PID file** with `flock(LOCK_EX|LOCK_NB)` — prevents duplicate instances
- **Status pipe** — parent process waits for child to bind ports before exiting
  - Parent exits 0 only after TCP and UDP ports are successfully bound
  - Parent exits 1 if server fails to start (clear error feedback to init system)
- **stdin/stdout/stderr** redirected to `/dev/null`
- **Working directory** changed to `/`
- All logging goes to the log file (use `-l` to specify path)

#### Managing the Daemon

```bash
# Start
./doip-server -d -c /etc/doip/doip-server.conf -l /var/log/doip-server.log

# Check if running
cat /var/run/doip-server.pid
kill -0 $(cat /var/run/doip-server.pid) && echo "running" || echo "stopped"

# Stop gracefully
kill $(cat /var/run/doip-server.pid)

# The PID file is automatically cleaned up on shutdown
```

#### Daemon Configuration

These settings can be set in the config file or overridden by CLI flags:

```ini
# doip-server.conf
daemon_mode         = true          # Same as -d flag
pid_file            = /var/run/doip-server.pid
script_output_dir   = /etc/doip/scripts
```

| Setting | Default | Description |
|---------|---------|-------------|
| `daemon_mode` | `false` | Run as daemon (values: `1`, `true`, `yes`) |
| `pid_file` | `/var/run/doip-server.pid` | PID file path (rejects `..` in path) |
| `script_output_dir` | `/etc/doip/scripts` | Default directory for generated scripts (rejects `..` in path) |

### Service Integration

The server self-generates service files via the `generate-scripts` command or at build time.

#### systemd (Ubuntu)

```bash
# Generate and install
./doip-server -c doip-server.conf <<< "generate-scripts /etc/systemd/system"
# Or copy the generated file:
sudo cp /etc/doip/scripts/doip-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable doip-server
sudo systemctl start doip-server
sudo systemctl status doip-server
```

#### init.d (Torizon/embedded)

```bash
sudo cp /etc/doip/scripts/doip-server-initd /etc/init.d/doip-server
sudo chmod 755 /etc/init.d/doip-server
sudo /etc/init.d/doip-server start
sudo /etc/init.d/doip-server status
```

## Self-Generated Scripts

The server embeds all operational scripts as compiled-in string literals. Scripts are platform-specific (selected at compile time) and can be written to disk via the CLI or programmatically.

### Available Scripts

| Script | Mode | Description |
|--------|------|-------------|
| `phonehome-keygen.sh` | 0755 | SSH key generation (Torizon: Dropbear, Ubuntu: OpenSSH) |
| `phonehome-register.sh` | 0755 | Bastion HTTPS registration (Torizon: wget, Ubuntu: curl) |
| `phonehome-connect.sh` | 0755 | Reverse SSH tunnel (platform-independent) |
| `doip-server.service` | 0644 | systemd unit file |
| `doip-server-initd` | 0755 | init.d service script |

### Generating Scripts

```bash
# Via CLI (interactive mode)
doip> generate-scripts /etc/doip/scripts

# Via CLI with custom directory
doip> generate-scripts /tmp/my-scripts

# Programmatically (from C code)
#include "script_gen.h"
int n = script_gen_write_to("/etc/doip/scripts");
// Or: int n = script_gen_write_all(&app_config);  // uses config's script_output_dir
```

Scripts are written with `O_NOFOLLOW` (prevents symlink attacks) and `fchmod()` on the file descriptor (prevents TOCTOU race conditions). Files are created with mode 0 initially, then set to the target mode atomically.

## Configuration

`doip-server.conf` uses key=value format:

```ini
# Identity
vin                 = APTERADOIPSRV0001
logical_address     = 0x0001
eid                 = 00:1A:2B:3C:4D:5E
gid                 = 00:1A:2B:3C:4D:5E
further_action      = 0x00
vin_gid_sync_status = 0x00

# Network
bind_address        = 127.0.0.1
tcp_port            = 13400
udp_port            = 13400
max_tcp_connections = 4
max_data_size       = 4096

# Blob Storage
blob_storage_dir    = /tmp/doip_blobs
blob_max_size       = 16777216
transfer_timeout    = 30

# Daemon / CLI
daemon_mode         = false
pid_file            = /var/run/doip-server.pid
script_output_dir   = /etc/doip/scripts

# Phone-Home config path (uncomment to enable phone-home capability)
# phonehome_config  = /etc/phonehome/phonehome.conf
```

**Config loading behavior:**
- No `-c` flag: tries `doip-server.conf` in CWD, falls back to defaults if missing
- Explicit `-c path`: fails with error if file not found
- CLI `bind_ip` and `port` arguments override config file values
- CLI `-d` flag overrides `daemon_mode` config setting
- Unrecognized keys produce a warning; invalid values keep the default
- `pid_file` and `script_output_dir` reject paths containing `..` (path traversal protection)

### Configuration Reference

| Setting | Default | Notes |
|---------|---------|-------|
| `vin` | `FC1BLOBSRV0000001` | 17-char Vehicle Identification Number |
| `logical_address` | `0x0001` | DoIP entity logical address |
| `eid` / `gid` | `00:1A:2B:3C:4D:5E` | Entity/Group ID (MAC-like, colon-separated) |
| `bind_address` | `127.0.0.1` | TCP bind address (`127.0.0.1` in shipped config; override with CLI arg for LAN use). UDP always binds to `INADDR_ANY` for broadcast discovery. |
| `tcp_port` / `udp_port` | `13400` | Standard DoIP port (ISO 13400) |
| `max_tcp_connections` | `4` | Maximum simultaneous TCP clients |
| `max_data_size` | `4096` | DoIP payload size limit |
| `blob_storage_dir` | `/tmp/doip_blobs` | Created automatically if missing |
| `blob_max_size` | `16 MB` | Maximum accepted blob size |
| `transfer_timeout` | `30` seconds | Idle transfer abort timeout |
| `daemon_mode` | `false` | Run as background daemon |
| `pid_file` | `/var/run/doip-server.pid` | PID file for daemon mode |
| `script_output_dir` | `/etc/doip/scripts` | Default script output directory |
| `phonehome_config` | (disabled) | Path to phone-home config file |

## Logging

The server writes structured logs to both a log file and stderr. Each line includes an ISO 8601 timestamp with millisecond resolution:

```
[2026-03-10T14:53:05.766] [INFO ] Server running. Press Ctrl+C to stop.
[2026-03-10T14:53:10.305] [INFO ] Routing activation: SA=0x0E80, type=0x00 — accepted
[2026-03-10T14:53:10.400] [DEBUG] UDS request from 0x0E80: SID=0x34, len=11
```

### Log Levels

| Level | Flag | Description |
|-------|------|-------------|
| ERROR | `-qq` | Fatal or unrecoverable errors |
| WARN | `-q` | Recoverable problems (timeouts, sequence errors) |
| INFO | (default) | Normal operations (connections, transfers, config) |
| DEBUG | `-v` | Protocol-level detail (every UDS request, block transfers) |

### Log File Rotation

- Default path: `/var/FC-1-DOIP.log` (override with `-l`)
- Maximum file size: 1 MB
- Maximum files: 5 (active + 4 rotated: `.log.1` through `.log.4`)
- Oldest file is deleted when a new rotation occurs
- File permissions: 0640

If the log file cannot be opened (e.g., `/var` permission denied without root), the server continues with stderr-only logging — no data is lost.

### Logging Tips

```bash
# Log to current directory (no root needed)
./doip-server -c doip-server.conf -l ./doip.log

# Log to /tmp
./doip-server -c doip-server.conf -l /tmp/doip.log

# Verbose logging for debugging protocol issues
./doip-server -c doip-server.conf -v -l ./debug.log

# Suppress all but errors (production-like)
./doip-server -c doip-server.conf -qq

# Discard log file entirely (stderr only)
./doip-server -c doip-server.conf -l /dev/null
```

## Testing

### Automated Tests

```bash
# All tests (CI mode: phone-home + smoke + full — 53 tests total)
make ci-test

# Phone-home unit tests (HMAC + handler, no server needed — 7 tests)
make run-test-phonehome

# Smoke tests (starts server, 6 discovery + connectivity checks)
make test

# Full server test suite (transfer, CRC, concurrency — 40 tests)
make test-full

# Config parser tests
make test-config
```

### Manual Testing

Start the server in one terminal:

```bash
./doip-server -c doip-server.conf -v -l ./doip.log
```

Run the test tool in another:

```bash
./test-discovery -c doip-server.conf 127.0.0.1 13400
```

### Test Suite Summary

| Suite | Tests | Description |
|-------|-------|-------------|
| Phone-Home | 7 | HMAC-SHA256 RFC 4231 vectors, constant-time compare, handler NRC codes |
| Discovery (A) | 3 | UDP broadcast, VIN filter, EID filter |
| TCP Protocol (B) | 14 | Routing, TesterPresent, entity status, NACK codes, connection limits |
| Blob Write (C) | 5 | Single block, multi-block, BSC wrap, back-to-back, minimum size |
| Error Handling (D) | 14 | Transfer state errors, wrong BSC, CRC mismatch, truncated messages |
| Concurrent (E) | 4 | Concurrent rejection, disconnect survival, multi-client keepalive |

## UDS Services

| Service | SID | Description |
|---------|-----|-------------|
| RoutineControl | 0x31 | Phone-home trigger (routineId 0xF0A0) |
| RequestDownload | 0x34 | Initiate blob transfer |
| TransferData | 0x36 | Receive blob chunks |
| RequestTransferExit | 0x37 | Finalize, verify CRC-32, store to disk |
| TesterPresent | 0x3E | Keepalive |

## Phone-Home

The phone-home feature allows an operator to establish an on-demand reverse SSH tunnel to a DCU that has no publicly routable IP address. See `docs/DCU_PhoneHome_Specification.md` for the full specification.

### How It Works

The system has three phases: provisioning (one-time), registration (first boot), and operation (on demand).

**1. Provisioning (manufacturing)**

At the factory, each DCU is flashed with:
- Firmware containing the bastion host key and CA certificate
- A 32-byte HMAC shared secret (identical on the paired FC-1 and DCU)
- A one-time provisioning token (JWT, 24h TTL)
- The device serial number (`/etc/dcu-serial`)

**2. Registration (first boot)**

On first power-up, two services run automatically:

```
phonehome-keygen.sh    ->  Generates ED25519 SSH key pair on the device
                           (private key never leaves the DCU)
                           Guarded by sentinel file, runs once only

phonehome-register.sh  ->  Sends public key + serials to Bastion HTTPS API
                           Authenticated by the provisioning token
                           Token is shredded after successful registration
                           Guarded by sentinel file, runs once only
```

After registration, the Bastion has the DCU's public key stored in `authorized_keys` for user `phonehome-<serial>`, enabling future SSH connections.

**3. Operation (on demand)**

```
Operator -> Web UI -> Backend -> Bastion -> FC-1 -> DCU (DoIP) -> SSH tunnel back to Bastion

Detailed flow (~3-8 seconds total):
  1. Operator clicks "Connect" in web UI
  2. Backend calls Bastion signal dispatcher
  3. Bastion SSHes into FC-1 via FC-1's existing reverse tunnel
  4. FC-1 runs phonehome-relay:
     - Generates random 8-byte nonce
     - Computes HMAC-SHA256(shared_secret, nonce)
     - Sends DoIP RoutineControl (SID 0x31, routineId 0xF0A0) to DCU
  5. DCU DoIP server validates HMAC, checks replay cache, spawns phonehome-connect.sh
  6. phonehome-connect.sh opens reverse SSH tunnel to Bastion
  7. Operator connects: ssh -p <assigned_port> operator@bastion
```

### Phone-Home Configuration

The DoIP server config (`doip-server.conf`) references a separate phone-home config:

```ini
# doip-server.conf
phonehome_config = /etc/phonehome/phonehome.conf
```

The phone-home config (`/etc/phonehome/phonehome.conf`):

```ini
BASTION_HOST=bastion-dev.imatrixsys.com
HMAC_SECRET_FILE=/etc/phonehome/hmac_secret
CONNECT_SCRIPT=/usr/sbin/phonehome-connect.sh
LOCK_FILE=/var/run/phonehome.lock
```

| Key | Required | Description |
|-----|----------|-------------|
| `HMAC_SECRET_FILE` | Yes | Path to 32-byte HMAC secret (must be 0600, not world-readable) |
| `CONNECT_SCRIPT` | Yes | Path to the SSH tunnel script |
| `BASTION_HOST` | No | Default bastion hostname (default: `bastion-dev.imatrixsys.com`) |
| `LOCK_FILE` | No | Tunnel lock file path (default: `/var/run/phonehome.lock`) |

### Phone-Home UDS PDU Format

**Request** (minimum 44 bytes):

```
Byte 0:       0x31  (SID: RoutineControl)
Byte 1:       0x01  (subFunction: startRoutine)
Bytes 2-3:    0xF0 0xA0  (routineIdentifier)
Bytes 4-11:   Nonce (8 bytes, random)
Bytes 12-43:  HMAC-SHA256(shared_secret, nonce)
Bytes 44+:    [optional] bastion_host (null-terminated) + port (2 bytes, big-endian)
```

**Positive response** (5 bytes):

```
Byte 0:    0x71  (RoutineControl positive)
Byte 1:    0x01  (startRoutine)
Bytes 2-3: 0xF0 0xA0  (routineIdentifier)
Byte 4:    0x02  (routineRunning)
```

**Error responses** (3 bytes: `7F 31 <NRC>`):

| NRC | Meaning |
|-----|---------|
| 0x13 | PDU too short (< 44 bytes) |
| 0x21 | Tunnel already active |
| 0x22 | HMAC secret not loaded (phone-home not configured) |
| 0x24 | Replay detected (nonce already used within 300s) |
| 0x31 | Invalid bastion hostname characters |
| 0x35 | HMAC verification failed |

### Testing Phone-Home Locally

#### Unit Tests

```bash
make run-test-phonehome

# Expected output:
# === Phone-Home Unit Tests ===
# PASS: RFC 4231 test vectors (TC1-TC3)
# PASS: Constant-time comparison
# PASS: HMAC not loaded -> NRC {7F 31 22}
# PASS: Valid HMAC -> positive response {71 01 F0 A0 02}
# PASS: Invalid HMAC -> NRC {7F 31 35}
# PASS: Replay nonce -> NRC {7F 31 24}
# PASS: Short PDU -> NRC {7F 31 13}
# === Results: 7 passed, 0 failed ===
```

#### Testing with the Scripts

See `docs/DCU_PhoneHome_Specification.md` for complete provisioning and trigger flow testing instructions.

## FC-1 DCU Phone-Home Relay

The FC-1 gateway can relay phone-home triggers from the Bastion to the DCU via DoIP. This allows operators to establish SSH tunnels to DCUs that are only reachable via the FC-1's LAN.

### Relay Flow

```
Bastion -> CoAP POST /remote_call_home/{can_controller_sn} (over DTLS)
  -> FC-1 receives trigger, sets flag
  -> FC-1 main loop: relay_dcu_phonehome()
    -> DoIP UDP broadcast discovery (finds DCU IP)
    -> Generate 8-byte nonce from /dev/urandom
    -> Compute HMAC-SHA256(shared_secret, nonce)
    -> DoIP TCP connect -> routing activation
    -> Send UDS RoutineControl (0x31, 0xF0A0, nonce, hmac) -- 44 bytes
    -> DCU validates HMAC, spawns reverse SSH tunnel
```

### Key Details

- **Lazy CoAP URI** — second URI `/remote_call_home/{can_sn}` registered after CAN bus init
- **Rate limited** — 60-second cooldown between relay attempts
- **Fire and forget** — logs success/failure, no retries
- **Independent** — DCU relay works regardless of FC-1's own tunnel state
- **Graceful degradation** — missing HMAC secret disables relay, FC-1 tunnel unaffected

## Project Structure

```
DoIP_Server/
├── Makefile                      # Build system (PLATFORM=ubuntu|torizon)
├── README.md                     # This file
├── BUILD_GUIDE.md                # Developer build guide
├── doip-server.conf              # Server configuration file
├── src/
│   ├── main.c                    # Entry point, UDS handlers, daemon, CLI loop
│   ├── config.c                  # Key=value config parser
│   ├── doip_log.c                # Structured logging with rotation
│   ├── doip.c                    # Core DoIP protocol
│   ├── doip_server.c             # Server (TCP/UDP, threading)
│   ├── cli.c                     # Interactive CLI commands
│   ├── script_gen.c              # Embedded script generation
│   ├── phonehome_handler.c       # Phone-home RoutineControl handler
│   └── hmac_sha256.c             # Standalone SHA-256 + HMAC-SHA256
├── include/
│   ├── config.h                  # Config API (doip_app_config_t)
│   ├── config_parse.h            # Shared config parsing helpers
│   ├── doip_log.h                # Logging API (4 levels, rotation)
│   ├── doip.h                    # Protocol types & constants
│   ├── doip_server.h             # Server API
│   ├── doip_client.h             # Client API (for test tools)
│   ├── cli.h                     # CLI context and API
│   ├── script_gen.h              # Script generation API
│   ├── phonehome_handler.h       # Phone-home API
│   └── hmac_sha256.h             # HMAC-SHA256 API
├── scripts/
│   ├── phonehome-keygen.sh       # SSH key generation (first boot)
│   ├── phonehome-register.sh     # Bastion registration (first boot)
│   ├── phonehome-connect.sh      # Reverse SSH tunnel (on trigger)
│   ├── systemd/                  # systemd service units
│   └── initd/                    # init.d scripts
├── etc/
│   └── phonehome/
│       └── phonehome.conf        # Example phone-home configuration
├── test/
│   ├── test_discovery.c          # 6-test discovery suite
│   ├── test_phonehome.c          # 7-test phone-home + HMAC suite
│   ├── test_server.c             # 40-test full server suite
│   └── test_config.sh            # Config parser tests
└── docs/
    └── DCU_PhoneHome_Specification.md
```

## Troubleshooting

**UDP discovery not working (clients can't find server):**
The default config ships with `bind_address = 127.0.0.1`, which restricts TCP to localhost only. For LAN discovery, override the bind address via CLI:
```bash
./doip-server -c doip-server.conf 0.0.0.0 13400
```
UDP discovery always binds to `INADDR_ANY` regardless of the config `bind_address`, so broadcast reception works automatically. If TCP connections from remote clients are refused, the bind address is the most likely cause.

**Port already in use:**
```
[DoIP Server] TCP bind: Address already in use
```
Another instance is running, or a previous instance didn't shut down cleanly. Wait a few seconds for the OS to release the port, or use a different port:
```bash
./doip-server -c doip-server.conf 127.0.0.1 13401
```

**Log file permission denied:**
```
doip_log: cannot open /var/FC-1-DOIP.log: Permission denied
```
The default log path requires root. Use `-l` to write elsewhere:
```bash
./doip-server -c doip-server.conf -l ./doip.log
```
The server continues with stderr-only logging if the file can't be opened.

**Daemon won't start (PID file locked):**
```
Failed to daemonize
```
Another instance holds the PID file lock. Check with:
```bash
cat /var/run/doip-server.pid
kill -0 $(cat /var/run/doip-server.pid) 2>/dev/null && echo "running" || echo "stale"
```
If stale, remove the PID file: `rm /var/run/doip-server.pid`

**Storage directory issues:**
The server creates `blob_storage_dir` automatically. If it fails:
```bash
mkdir -p /tmp/doip_blobs
```

**No CLI prompt:**
The CLI only activates when stdin is a TTY. If running via pipe, nohup, or test harness, the server runs in non-interactive mode. Use `-d` for proper daemon operation.
