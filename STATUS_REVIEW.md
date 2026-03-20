# DoIP Phone-Home System — Status Review

**Date:** 2026-03-12
**Author:** Greg Phillips / Claude Code
**For:** Developer peer review

---

## 1. Overview

The phone-home system enables remote SSH access to field-deployed gateway devices
(FC-1 + DCU) via a bastion server. An operator triggers a "call home" from the
cloud; the device opens a reverse SSH tunnel to the bastion; the operator
connects through a web terminal (xterm.js).

### Architecture

```
Cloud/CoAP Trigger
    |
    v
FC-1 Gateway (ARM, BusyBox/musl)        Bastion Server (Ubuntu)
    |  CoAP handler                          |  web-ssh-bastion (FastAPI + asyncssh)
    |  sets dcu_phonehome_triggered          |  xterm.js web terminal
    |                                        |
    v                                        |
FC-1 Main Loop                               |
    |  relays via DoIP RoutineControl        |
    v                                        |
DCU (DoIP Server, Ubuntu)                    |
    |  phonehome_handler.c                   |
    |  execl(phonehome-connect.sh)           |
    |                                        |
    └── SSH reverse tunnel ─────────────────>│
        -R <port>:localhost:<LOCAL_SSH_PORT>  |
                                             |
Operator Browser ──── WebSocket ────────────>│
    xterm.js           asyncssh proxy        |
```

### Repositories / Worktree Layout

| Path | Description |
|------|-------------|
| `~/iMatrix/DOIP/` | Git worktree from `iMatrix_Client` (branch: `Aptera_1_Clean`) |
| `~/iMatrix/DOIP/DoIP_Server/` | DCU blob server (standalone, will get own repo) |
| `~/iMatrix/DOIP/Fleet-Connect-1/` | FC-1 firmware (detached HEAD) |
| `~/iMatrix/DOIP/iMatrix/` | Shared iMatrix library (part of worktree) |
| `~/iMatrix/imatrix-bastion/web-ssh-bastion/` | Bastion web terminal (separate repo) |

---

## 2. What Was Built (Phone-Home Provisioning)

### Problem

After the FC-1 connects to the DCU via DoIP and obtains the CAN controller
serial number, it must send the HMAC shared secret and device identity to the
DCU so that phone-home triggers can be authenticated. Without provisioning, the
DCU rejects all phone-home triggers with NRC 0x22 (conditionsNotCorrect).

### Solution

FC-1 sends a one-time UDS RoutineControl (0x31) message to the DCU with
routineId 0xF0A1 after DoIP connects and CAN SN is available.

**PDU format (40 bytes, fixed):**

```
Offset  Length  Field
0       1       SID = 0x31
1       1       subFunction = 0x01 (startRoutine)
2       2       routineId = 0xF0A1 (big-endian)
4       4       CAN serial number (big-endian)
8       32      HMAC-SHA256 shared secret
```

### Files Changed

#### DCU Server (`DoIP_Server/`)

| File | Lines Changed | Purpose |
|------|--------------|---------|
| `include/phonehome_handler.h` | +24 | `ROUTINE_ID_PHONEHOME_PROVISION` (0xF0A1), handler declaration |
| `src/phonehome_handler.c` | +210 | Provision handler, `hmac_mutex` for `g_cfg`/`hmac_secret`, `g_provision_cfg` defaults, atomic file write |
| `src/main.c` | +14 | RoutineControl dispatch: extract routineId, route 0xF0A0→trigger, 0xF0A1→provision |
| `scripts/phonehome-connect.sh` | +1 | `LOCAL_SSH_PORT` changed 22→22222 (see Open Issue #1) |

#### FC-1 (`Fleet-Connect-1/`)

| File | Lines Changed | Purpose |
|------|--------------|---------|
| `remote_access/remote_access.c` | +426 (total diff, includes prior relay work) | `send_dcu_provision()`, retry logic in `remote_access_process()`, CoAP phone-home relay |

#### iMatrix Shared Library (`iMatrix/`)

| File | Lines Changed | Purpose |
|------|--------------|---------|
| `doip/doip_process.c` | +349 (total diff, includes DoIP client) | `imx_doip_send_raw_uds()`, `imx_doip_is_connected()` |
| `doip/doip_process.h` | +58 | API declarations for above |

### Key Design Decisions

1. **Provisioning in `remote_access.c`, not `doip_process.c`** — remote_access
   owns the HMAC secret, bastion config, and CAN SN. doip_process is a
   transport layer with no phone-home knowledge.

2. **`g_provision_cfg` static defaults** — On a fresh DCU (no phonehome.conf),
   `phonehome_init()` never runs, so `g_cfg` is NULL. After provisioning writes
   the HMAC secret, `g_provision_cfg` provides default bastion/lock-file/script
   paths so triggers work immediately without a config file.

3. **Transport vs UDS error distinction** — `send_dcu_provision()` returns:
   - `0`: success
   - `-1`: UDS rejection (counted toward 3-attempt cap)
   - `-2`: transport error (triggers DoIP reconnect, NOT counted)

   This prevents network glitches from permanently disabling provisioning.

4. **`hmac_mutex`** — Protects both `hmac_secret[]` and `g_cfg` pointer.
   `phonehome_handle_routine()` copies both to local variables inside the
   critical section, then uses locals for the rest of the function.

5. **Atomic file write** — HMAC secret written via:
   `unlink(tmp)` → `open(O_CREAT|O_EXCL|O_NOFOLLOW)` → `write` → `fsync` →
   `rename(tmp, final)`. Prevents partial writes on power loss.

---

## 3. Testing & Verification

### Unit Tests (53 total, all pass)

| Suite | Count | Description |
|-------|-------|-------------|
| Phone-home | 7 | HMAC vectors, constant-time compare, trigger accept/reject, replay, short PDU |
| Discovery (smoke) | 6 | UDP broadcast, VIN/EID filter, TCP routing, TesterPresent |
| Full server | 40 | Protocol, blob transfers, error handling, concurrency |

### End-to-End Hardware Test (2026-03-11)

Verified on real FC-1 (192.168.7.1) and DCU/CAN-Test (192.168.7.100):

1. FC-1 binary deployed with provisioning code
2. HMAC secret (`/etc/phonehome/hmac_secret`) placed on FC-1
3. DoIP server deployed to CAN-Test with `bind_address=0.0.0.0`
4. FC-1 discovered DCU via UDP broadcast
5. FC-1 connected via TCP + routing activation
6. FC-1 sent RoutineControl 0xF0A1 provision PDU
7. DCU logged: `phonehome: provisioned (CAN SN 281183743)`
8. DCU wrote HMAC to `/etc/phonehome/hmac_secret` — **secrets match**
9. `g_provision_cfg` initialized with defaults

```
DCU log:
[2026-03-11T17:48:52.700] [INFO ] phonehome: g_cfg initialized with defaults via provision
[2026-03-11T17:48:52.700] [INFO ] phonehome: provisioned (CAN SN 281183743)
```

### Code Reviews (6 agents x 2 rounds = 12 reviews, all PASS)

| Reviewer | Round 1 | Round 2 |
|----------|---------|---------|
| Concurrency | FAIL (g_cfg outside mutex) | PASS |
| Correctness | PASS | PASS |
| Error Handling | PASS | PASS |
| Security | PASS | PASS |
| Integration | PASS | PASS |
| KISS | PASS | PASS |

Review files in `~/iMatrix/DOIP/fci-review-code-*-r2.md`.

---

## 4. Open Issues

### Issue #1: `LOCAL_SSH_PORT` in `phonehome-connect.sh` — NEEDS DECISION

**Status:** Changed from 22 to 22222, but correctness depends on target device.

The connect script runs on the **DCU** and opens a reverse tunnel:
```
-R <bastion_port>:localhost:LOCAL_SSH_PORT
```

This means the tunnel endpoint is the DCU's own SSH port. The question is:
**which device does the operator want to reach through the tunnel?**

- If the tunnel should reach the **DCU** itself → depends on what SSH port the
  DCU runs (standard Ubuntu = 22, but CAN-Test may differ)
- If the tunnel should reach the **FC-1** → the DCU would need to forward to
  the FC-1's IP:22222, which requires a different `-R` target

**Current state on FC-1:** Dropbear listens on port **22222 only**.
```
dropbear -F -r /etc/dropbear_rsa/dropbear_rsa_host_key -p 22222
```

**The web terminal screenshot (b1.png) shows:** Device 0131557250 connected for
22 seconds with a completely blank terminal. This confirms the tunnel endpoint
is wrong — nothing is listening on the port being forwarded to.

**Action needed:** Clarify the intended tunnel target (DCU or FC-1) and set
`LOCAL_SSH_PORT` accordingly.

### Issue #2: Web Terminal — Blank Screen on Connect

**Status:** INVESTIGATING

Screenshot `debug/b1.png` shows the web-ssh-bastion terminal page connected to
device 0131557250 (FC-1) but displaying nothing — no banner, no prompt, no
output after 22 seconds.

**Root cause (most likely):** The reverse tunnel forwards to `localhost:22` (or
`localhost:22222`) on the DCU, but the web-ssh-bastion connects through that
tunnel expecting to reach the FC-1's shell. If the port mismatch described in
Issue #1 is the cause, fixing the port will fix the blank screen.

**Previously fixed (2026-03-11):** A separate SIGWINCH/banner corruption issue
was fixed in `app.py` and `terminal.html` (browser-driven PTY sizing, removed
`\r` injection). This fix is deployed but does not address the blank screen
since no data is reaching the terminal at all.

**Diagnostic:**
- sshd on bastion is healthy: `ClientAliveInterval 120`, `ClientAliveCountMax 3`
- Tunnel user properly restricted (`AllowTcpForwarding remote`, `PermitTTY no`,
  `ForceCommand sleep infinity`, `GatewayPorts no`)
- asyncssh keepalive disabled (Dropbear doesn't respond to
  `keepalive@openssh.com`)

### Issue #3: phonehome.conf Not Yet Created on DCU

**Status:** LOW PRIORITY

The DCU currently has no `/etc/phonehome/phonehome.conf`. The provision handler
works without it (uses `g_provision_cfg` defaults), but a proper config file
should be deployed for production to set the correct bastion hostname, connect
script path, and lock file location.

### Issue #4: FC-1 Phone-Home Relay Not Yet Triggered End-to-End

**Status:** NOT TESTED

The provisioning path (FC-1 → DCU HMAC secret) is verified. The full trigger
path (Cloud CoAP → FC-1 → DCU → reverse tunnel → bastion → web terminal) has
NOT been tested end-to-end. The blank screen issue (Issue #2) blocks this.

---

## 5. Deployment State

### CAN-Test (DCU, 192.168.7.100)

- **Binary:** `/tmp/doip-server` (new version with provisioning support)
- **Config:** `/etc/doip/doip-server.conf` (`bind_address=0.0.0.0`)
- **HMAC secret:** `/etc/phonehome/hmac_secret` (32 bytes, provisioned by FC-1)
- **phonehome.conf:** NOT present (uses `g_provision_cfg` defaults)
- **SSH keys:** NOT deployed (`/etc/phonehome/id_ed25519` missing)
- **Status:** Was running, currently offline (192.168.7.100 unreachable)

### FC-1 (192.168.7.1)

- **Binary:** `/usr/qk/bin/FC-1` (new version with provisioning code)
- **HMAC secret:** `/etc/phonehome/hmac_secret` (32 bytes, test key)
- **DoIP state:** CONNECTED to 192.168.7.100:13400
- **Provisioning:** Completed successfully (CAN SN 281183743)

### Bastion (tunnel-bastion)

- **web-ssh-bastion:** Deployed with terminal fix (browser-driven sizing)
- **sshd:** Hardened config active, tunnel user restricted
- **Status:** Running, but terminal shows blank screen (Issue #2)

---

## 6. Build Instructions

### DCU Server
```bash
cd ~/iMatrix/DOIP/DoIP_Server
make clean && make                    # Ubuntu build
make PLATFORM=torizon                 # ARM cross-compile (Toradex)
make ci-test                          # Run all 53 tests
```

### FC-1
```bash
cd ~/iMatrix/DOIP/Fleet-Connect-1
mkdir -p build && cd build
cmake .. && make -j$(nproc)           # ARM cross-compile (QConnect SDK)
```

### Deploy FC-1
```bash
~/iMatrix/iMatrix_Client/scripts/fc1 -d 192.168.7.1 -b build/FC-1 push -run
```

---

## 7. Review Artifacts

| File | Purpose |
|------|---------|
| `fci-plan.md` | Implementation plan v2.1 (3 review rounds) |
| `fci-review-code-*-r2.md` | Round 2 code reviews (6 agents, all PASS) |
| `fci-retro-provision.md` | FCI retrospective (gap analysis + process improvements) |
| `DCU_PhoneHome_Specification.md` | Phone-home protocol specification |
| `CHANGES-web-terminal-fix.md` | Web terminal SIGWINCH fix documentation |
| `debug/b1.png` | Blank terminal screenshot (current issue) |
| `debug/b2.png` | Bastion sshd config verification |

---

## 8. Next Steps (Priority Order)

1. **Resolve Issue #1** — Determine correct `LOCAL_SSH_PORT` for the reverse
   tunnel based on which device the operator needs shell access to
2. **Deploy SSH keys** to DCU (`/etc/phonehome/id_ed25519` + `known_hosts`)
   so the connect script can authenticate to the bastion
3. **Test full trigger path** — CoAP trigger → FC-1 relay → DCU tunnel →
   bastion → web terminal with shell access
4. **Create production `phonehome.conf`** on DCU with correct bastion hostname
5. **Deploy to production** — Torizon cross-compile for actual DCU hardware
