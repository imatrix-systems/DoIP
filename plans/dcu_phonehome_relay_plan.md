# Plan: FC-1 CoAP → DoIP Phone-Home Relay

**Version:** 4.0
**Status:** COMPLETE — Implemented, all 6 code reviews PASS
**Scope:** FC-1 `remote_access.c` modifications to relay DCU phone-home triggers

---

## Problem Statement

The current `remote_access.c` handles CoAP `remote_call_home` triggers for the **FC-1's own** reverse SSH tunnel. The URI is registered as `/remote_call_home/{fc1_serial}` using `imx_get_device_serial_number()`.

The Bastion server now needs to send a phone-home trigger for the **DCU** (identified by the CAN Controller serial number). When FC-1 receives this trigger, it must:

1. Recognize the serial as a DCU serial (not the FC-1's own)
2. Construct a DoIP RoutineControl UDS PDU with HMAC authentication
3. Send it to the DCU's DoIP server over the local LAN
4. The DCU opens its own reverse SSH tunnel directly to the Bastion

### Prerequisite: CAN Bus Registration

The CAN Controller serial number (`imx_get_can_controller_sn()`) is only available after the CAN bus has been registered. The DCU CoAP URI cannot be registered until the CAN Controller SN is non-zero. This means DCU phone-home relay is only available after CAN bus initialization completes.

### Prerequisite: DoIP Client Library

The DoIP client library source exists at `Fleet-Connect-1/DoIP_Client/libdoip/` but is **not yet linked** into the FC-1 build. Must be added to CMakeLists.txt before this feature can work.

---

## Bastion Architecture (Current)

The Bastion trigger chain for FC-1 phone-home is:

```
Operator Web UI
  → POST /api/v1/devices/{device_id_str}/phone-home  (Bastion app.py)
    → POST {COAP_SERVER_URL}/api/v1/remote-access/trigger/{device_sn}  (CoAP server)
      → CoAP POST /remote_call_home/{serial} over DTLS  (to FC-1)
        → FC-1 sets phone_home_triggered flag
        → FC-1 returns CoAP 2.04 Changed
```

**Key facts from `cloud_phone_home_integration.md` and `app.py`:**
- CoAP POST to `/remote_call_home/{serial}` with **empty payload**
- Serial is in the URI path (two `Uri-Path` options: `"remote_call_home"` + `"{serial}"`)
- FC-1 uses `strcmp()` exact match — the URI must match exactly
- `{serial}` = `device_id_int` from Redis = FC-1's `imx_get_device_serial_number()`
- Transport: DTLS only (port 5684); plain CoAP is rejected

### Implication for DCU Triggers

Since the payload is empty and URI matching is exact-string, the Bastion must send a **different URI** for DCU triggers — `/remote_call_home/{can_controller_sn}`. FC-1 must register a **second CoAP URI** to receive it.

The Bastion side needs a new device registration for the DCU (with the CAN Controller SN as `device_id`), and the `trigger_phone_home()` endpoint calls the CoAP server with that SN. The CoAP server sends `POST /remote_call_home/{can_controller_sn}` over the FC-1's DTLS session. No Bastion code changes needed — just a new device registration entry.

---

## Design Decisions (Resolved)

### D1: DCU IP Address Discovery

**Decision:** DoIP UDP broadcast discovery. FC-1 sends a DoIP vehicle identification request on the LAN interface. The DCU responds with its IP address. Use `doip_client_discover()` from the existing library.

**Rationale:** Most robust — confirms DCU is reachable and running DoIP. Works regardless of DHCP lease state. No config file needed.

### D2: CoAP URI Strategy

**Decision:** Register a second CoAP URI `/remote_call_home/{can_controller_sn}` for DCU triggers. The CAN Controller SN is formatted as decimal (`%u`).

**Constraint:** The CAN Controller SN is only available after CAN bus registration. The second CoAP URI must be registered **lazily** — when the SN becomes available — not at `remote_access_init()` time.

### D3: HMAC-SHA256 Location

**Decision:** Copy `hmac_sha256.c` and `hmac_sha256.h` into `Fleet-Connect-1/remote_access/`. Self-contained, no cross-repo dependency.

### D4: Error Reporting

**Decision:** Fire and forget after validation testing. Log relay results locally for diagnostics.

### D5: Blocking Model

**Decision:** CoAP handler sets flag, returns 2.04 immediately. DoIP relay runs in `state_idle()` from the main loop. Synchronous operation — worst case ~14s (3s UDP discovery timeout + TCP connect + 10s UDS response timeout). Acceptable for an infrequent, operator-initiated action.

---

## Implementation Plan

### Step 0: Add DoIP Client Library to FC-1 Build

Add to `CMakeLists.txt`:
- Include path: `DoIP_Client/libdoip/`
- Sources: `DoIP_Client/libdoip/doip.c`, `DoIP_Client/libdoip/doip_client.c`

**Note:** The DoIP client library uses `malloc()` internally in `doip_client_send_diagnostic()` (line 551 of `doip_client.c`). This is acceptable — FC-1 already uses `malloc()` elsewhere. The library also has `printf("[DoIP Client] ...")` debug output that goes to stdout; this is acceptable for now as FC-1 captures stdout in syslog when running as a service.

Verify compilation with BusyBox toolchain.

### Step 1: Add HMAC-SHA256 to FC-1 Build

Copy from `DoIP_Server/`:
- `src/hmac_sha256.c` → `Fleet-Connect-1/remote_access/hmac_sha256.c`
- `include/hmac_sha256.h` → `Fleet-Connect-1/remote_access/hmac_sha256.h`

Add `hmac_sha256.c` to `CMakeLists.txt`. Standalone C, no dependencies.

**Note:** `hmac_sha256.c` uses `explicit_bzero()` which requires `_DEFAULT_SOURCE`. Verify this is already set in FC-1 build flags or add it.

### Step 2: Add DCU Relay State to Context

Add to `remote_access_ctx_t`:

```c
/* DCU phone-home relay */
uint8_t hmac_secret[32];            /**< HMAC shared secret (FC-1 ↔ DCU) */
bool hmac_loaded;                   /**< HMAC secret loaded from file */
_Atomic bool dcu_phonehome_triggered; /**< Flag: DCU relay pending (set by CoAP thread, read by main loop) */
bool dcu_uri_registered;            /**< DCU CoAP URI registered */
char dcu_phone_home_uri[64];        /**< /remote_call_home/{can_controller_sn} */
imx_time_t dcu_relay_cooldown;      /**< Rate limit: earliest next relay allowed */
```

### Step 3: Load HMAC Secret During Init

In `INIT_STEP_GENERATE_KEYS` (or a new sub-step), load the HMAC secret:

```c
/* Load HMAC shared secret for DCU phone-home relay */
static int init_load_hmac_secret(void)
{
    int fd = open("/etc/phonehome/hmac_secret", O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        REMOTE_LOG("HMAC secret not found — DCU relay disabled");
        return 0;  /* Non-fatal: FC-1 tunnel still works */
    }

    /* Security: verify regular file with restricted permissions */
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077)) {
        REMOTE_LOG("HMAC secret file has bad permissions or is not a regular file");
        close(fd);
        return 0;
    }

    ssize_t n = read(fd, ctx.hmac_secret, 32);
    close(fd);
    if (n != 32) {
        REMOTE_LOG("HMAC secret invalid (%zd bytes, need 32)", n);
        return 0;
    }
    ctx.hmac_loaded = true;
    REMOTE_LOG("HMAC secret loaded — DCU relay enabled");
    return 0;
}
```

### Step 4: Resize `host_coap_entries` Array and Lazy Registration

**CRITICAL:** The existing `host_coap_entries` is declared as `static CoAP_entry_t host_coap_entries[1]`. It **must** be resized to `[2]` to hold the second DCU entry. Without this, writing to index `[1]` is an out-of-bounds write causing memory corruption.

```c
/* In remote_access.c — change existing declaration from [1] to [2] */
static CoAP_entry_t host_coap_entries[2];  /* was [1] — now holds FC-1 + DCU entries */
```

The CAN Controller SN may not be available at init time. Check periodically from the main loop (e.g., in `state_idle()` or `state_connected()`):

```c
static void try_register_dcu_uri(void)
{
    if (ctx.dcu_uri_registered) return;
    if (!ctx.hmac_loaded) return;

    uint32_t can_sn = imx_get_can_controller_sn();
    if (can_sn == 0) return;  /* CAN bus not registered yet */

    snprintf(ctx.dcu_phone_home_uri, sizeof(ctx.dcu_phone_home_uri),
             "/remote_call_home/%u", can_sn);

    /*
     * Populate the second CoAP entry fully BEFORE making it visible
     * via imx_set_host_coap_interface(). This ensures the CoAP receive
     * thread never sees a partially-initialized entry.
     */
    CoAP_entry_t dcu_entry;
    memset(&dcu_entry, 0, sizeof(dcu_entry));
    dcu_entry.node.uri           = ctx.dcu_phone_home_uri;
    dcu_entry.node.att.title     = "DCU phone-home relay trigger";
    dcu_entry.node.att.rt        = "remote_call_home";
    dcu_entry.node.att.if_desc   = "POST relays phone-home to DCU via DoIP";
    dcu_entry.node.post_function = coap_post_dcu_phone_home;

    host_coap_entries[1] = dcu_entry;  /* Single struct copy */

    /* Memory barrier: ensure struct write is visible before count update */
    __sync_synchronize();

    imx_set_host_coap_interface(2, host_coap_entries);

    ctx.dcu_uri_registered = true;
    REMOTE_LOG("DCU phone-home URI registered: %s", ctx.dcu_phone_home_uri);
}
```

Call `try_register_dcu_uri()` from the main processing function on each iteration until registered.

### Step 5: Implement DCU CoAP Handler

```c
/**
 * @brief CoAP POST handler for /remote_call_home/{can_controller_sn}
 *
 * Receives DCU phone-home trigger from Bastion (via CoAP server over DTLS).
 * Sets dcu_phonehome_triggered flag for main loop processing.
 * The actual DoIP relay happens in state_idle().
 */
static uint16_t coap_post_dcu_phone_home(coap_message_t *msg,
                                          CoAP_msg_detail_t *cd,
                                          uint16_t arg)
{
    (void)cd;
    (void)arg;

    /* SECURITY: Reject triggers on plain (non-DTLS) CoAP */
    if (is_plain_coap_mode()) {
        REMOTE_LOG("SECURITY — DCU phone_home rejected (plain CoAP)");
        return COAP_NO_RESPONSE;
    }

    ctx.dcu_phonehome_triggered = true;
    REMOTE_LOG("CoAP DCU phone_home trigger received");

    /* Build 2.04 Changed response */
    uint16_t response_type = NON_CONFIRMABLE;
    if (msg->header.mode.udp.t == CONFIRMABLE) {
        response_type = ACKNOWLEDGEMENT;
    }

    if (coap_store_response_header(msg, CHANGED, response_type, NULL) != IMX_SUCCESS) {
        REMOTE_LOG("failed to build CoAP response");
        return COAP_NO_RESPONSE;
    }

    return COAP_SEND_RESPONSE;
}
```

### Step 5.5: Required `#include` Directives

Add to the top of `remote_access.c`:

```c
#include "doip_client.h"
#include "hmac_sha256.h"
#include <fcntl.h>          /* O_RDONLY, O_NOFOLLOW */
#include <arpa/inet.h>      /* inet_ntop, INET_ADDRSTRLEN */
#include <sys/stat.h>       /* fstat, S_ISREG */
#include <stdatomic.h>      /* _Atomic */
```

### Step 6: Implement `relay_dcu_phonehome()`

```c
#define DCU_RELAY_COOLDOWN_SEC  60  /* Minimum seconds between DCU relay attempts */

/**
 * @brief Relay phone-home trigger to DCU via DoIP RoutineControl
 *
 * Discovers DCU on the LAN via DoIP UDP broadcast, then sends
 * UDS RoutineControl (SID 0x31, routineId 0xF0A0) with HMAC-SHA256
 * authentication. The DCU validates the HMAC and opens its own
 * reverse SSH tunnel to the Bastion.
 *
 * Synchronous operation (worst case ~14s): runs from main loop, not CoAP thread.
 * Rate-limited to one attempt per 60 seconds.
 *
 * Note: doip_client_t (~4.1 KB) is declared static to avoid stack pressure
 * on embedded targets with small thread stacks.
 */
static void relay_dcu_phonehome(void)
{
    if (!ctx.hmac_loaded) {
        REMOTE_LOG("DCU relay failed — HMAC secret not loaded");
        return;
    }

    /* Rate limiting: prevent rapid triggers from blocking main loop */
    imx_time_t now = imx_get_current_time();
    if (now < ctx.dcu_relay_cooldown) {
        REMOTE_LOG("DCU relay skipped — cooldown (%u sec remaining)",
                   (unsigned)(ctx.dcu_relay_cooldown - now));
        return;
    }
    ctx.dcu_relay_cooldown = now + DCU_RELAY_COOLDOWN_SEC;

    /* Static to avoid ~4.1 KB on stack (recv_buf[4104] inside) */
    static doip_client_t client;

    /* 1. Discover DCU on LAN via DoIP UDP broadcast */
    doip_client_init(&client, NULL);

    doip_discovery_result_t discovery;
    int found = doip_client_discover(&client, NULL, &discovery, 1, 3000);
    if (found <= 0) {
        REMOTE_LOG("DCU relay failed — no DoIP entity found on LAN");
        doip_client_destroy(&client);
        return;
    }

    /* Extract DCU IP from discovery response */
    char dcu_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &discovery.source_addr.sin_addr, dcu_ip, sizeof(dcu_ip));
    /* Use standard DoIP port for TCP, not the UDP source port */
    uint16_t dcu_port = 13400;

    REMOTE_LOG("DCU discovered at %s:%u", dcu_ip, dcu_port);

    /* 2. Generate 8-byte nonce from /dev/urandom */
    uint8_t nonce[8];
    int urand_fd = open("/dev/urandom", O_RDONLY);
    if (urand_fd < 0 || read(urand_fd, nonce, sizeof(nonce)) != sizeof(nonce)) {
        REMOTE_LOG("DCU relay failed — cannot read /dev/urandom");
        if (urand_fd >= 0) close(urand_fd);
        doip_client_destroy(&client);
        return;
    }
    close(urand_fd);

    /* 3. Compute HMAC-SHA256(secret, nonce) */
    uint8_t hmac[32];
    hmac_sha256(ctx.hmac_secret, 32, nonce, 8, hmac);

    /* 4. Build UDS PDU: 31 01 F0 A0 <nonce:8> <hmac:32> = 44 bytes */
    uint8_t pdu[44];
    pdu[0] = 0x31;   /* SID: RoutineControl */
    pdu[1] = 0x01;   /* subFunction: startRoutine */
    pdu[2] = 0xF0;   /* routineIdentifier high */
    pdu[3] = 0xA0;   /* routineIdentifier low */
    memcpy(pdu + 4, nonce, 8);
    memcpy(pdu + 12, hmac, 32);

    /* 5. Connect to DCU via DoIP TCP */
    if (doip_client_connect(&client, dcu_ip, dcu_port) != DOIP_OK) {
        REMOTE_LOG("DCU relay failed — TCP connect to %s:%u", dcu_ip, dcu_port);
        doip_client_destroy(&client);
        return;
    }

    /* 6. Routing activation */
    if (doip_client_activate_routing(&client, NULL) != DOIP_OK) {
        REMOTE_LOG("DCU relay failed — routing activation rejected");
        doip_client_destroy(&client);
        return;
    }

    /* 7. Send RoutineControl and check response */
    uint8_t resp[64];
    int resp_len = doip_client_send_uds(&client, pdu, sizeof(pdu),
                                         resp, sizeof(resp), 10000);

    if (resp_len >= 5 && resp[0] == 0x71 && resp[4] == 0x02) {
        REMOTE_LOG("DCU phone-home relay OK — tunnel initiating");
    } else if (resp_len >= 3 && resp[0] == 0x7F) {
        REMOTE_LOG("DCU phone-home relay rejected — NRC 0x%02X", resp[2]);
    } else {
        REMOTE_LOG("DCU phone-home relay — unexpected response (len=%d)", resp_len);
    }

    doip_client_destroy(&client);
}
```

### Step 7: Wire into State Machine

In the main processing function, add two hooks:

```c
/* In the main process function, called every iteration */

/* Try to register DCU CoAP URI once CAN Controller SN is available */
try_register_dcu_uri();

/* In state_idle(): */
static void state_idle(imx_time_t current_time)
{
    (void)current_time;

    /* Existing FC-1 tunnel trigger */
    if (ctx.phone_home_triggered) {
        ctx.phone_home_triggered = false;
        REMOTE_LOG("trigger received — launching tunnel");
        transition_to(REMOTE_STATE_CONNECTING);
    }

    /* DCU phone-home relay (independent of FC-1 tunnel state) */
    if (ctx.dcu_phonehome_triggered) {
        ctx.dcu_phonehome_triggered = false;
        relay_dcu_phonehome();
    }
}
```

Also add DCU relay handling in `state_connected()` — the FC-1 may have its own tunnel active when a DCU trigger arrives. The DCU relay is independent:

```c
static void state_connected(imx_time_t current_time)
{
    /* ... existing child check, TTL, re-trigger logic ... */

    /* DCU relay works even while FC-1 tunnel is active */
    if (ctx.dcu_phonehome_triggered) {
        ctx.dcu_phonehome_triggered = false;
        relay_dcu_phonehome();
    }
}
```

### Step 8: Edge Cases

| Scenario | Behavior |
|----------|----------|
| CAN Controller SN = 0 (not registered yet) | DCU URI not registered; CoAP dispatches 4.04 Not Found |
| HMAC secret file missing | `hmac_loaded = false`; DCU URI not registered |
| DCU not on LAN (discovery timeout) | Log error, return to normal operation |
| DCU returns NRC 0x22 (not configured) | Log NRC, return |
| DCU returns NRC 0x35 (HMAC mismatch) | Log — indicates mismatched secrets |
| DCU returns NRC 0x21 (tunnel active) | Log — DCU already has a tunnel open |
| FC-1 tunnel active + DCU trigger | Both work independently |
| Rapid DCU triggers | Rate-limited to 1 per 60s — prevents main loop starvation |

---

## File Changes Summary

| File | Change |
|------|--------|
| `Fleet-Connect-1/remote_access/remote_access.c` | Add DCU relay: CoAP handler, lazy URI registration, `relay_dcu_phonehome()`, state machine hooks |
| `Fleet-Connect-1/remote_access/hmac_sha256.c` | Copy from DoIP_Server/src/ (standalone) |
| `Fleet-Connect-1/remote_access/hmac_sha256.h` | Copy from DoIP_Server/include/ (standalone) |
| `Fleet-Connect-1/CMakeLists.txt` | Add hmac_sha256.c, DoIP_Client/libdoip sources + include path |

### HMAC Secret Provisioning

The same 32-byte HMAC secret must be deployed to both FC-1 and DCU:
- **DCU:** `/etc/phonehome/hmac_secret` (already documented in DoIP_Server README)
- **FC-1:** `/etc/phonehome/hmac_secret` (same path, same content)

Generate with: `dd if=/dev/urandom bs=32 count=1 of=/etc/phonehome/hmac_secret && chmod 600 /etc/phonehome/hmac_secret`

Both files must be identical. If they mismatch, the DCU will reject the relay with NRC 0x35.

### Bastion Side (No Code Changes)

The existing Bastion architecture works as-is. To enable DCU triggers:
1. Register the DCU as a device in Redis with `device_id_int` = CAN Controller SN (decimal)
2. The FC-1's DTLS session carries both FC-1 and DCU CoAP URIs
3. `POST /api/v1/devices/{dcu_device_id}/phone-home` triggers via the same CoAP path

---

## Testing Strategy

### Unit Tests (on PC)
- HMAC computation with known test vectors (already passing in DoIP_Server)
- PDU construction: verify 44-byte layout

### Integration Test (two PCs on same LAN)
1. PC-A: Run `doip-server` with phone-home enabled (`CONNECT_SCRIPT=/bin/echo`)
2. PC-B: Simulate FC-1 relay — call `relay_dcu_phonehome()` targeting PC-A
3. Verify PC-A receives valid RoutineControl, returns `71 01 F0 A0 02`
4. Verify PC-A logs show HMAC validation passed

### On-Device Test (FC-1 + DCU)
1. Deploy updated FC-1 binary: `fc1 push -run`
2. Verify DCU URI registration in FC-1 log after CAN bus comes up
3. Trigger via Bastion (or `coap-client-openssl` with DCU serial in URI)
4. Monitor FC-1 log: `fc1 -d 192.168.7.1 log`
5. Monitor DCU DoIP server log for incoming RoutineControl
6. Verify DCU spawns connect script

---

## Review History

### Round 1 (v3.0) — 6 Reviewers

| Reviewer | Verdict | Key Findings |
|----------|---------|-------------|
| Security (risk) | FAIL | HMAC file needs O_NOFOLLOW+fstat, discovery accepts any responder, no rate limit |
| Integration (arch) | FAIL | host_coap_entries[1] OOB, missing #includes |
| Concurrency (risk) | FAIL | Array OOB, main loop blocking understated, lazy reg race, non-atomic flag |
| Protocol (correctness) | CONCERNS | Array OOB, CoAP re-registration API unverified, HMAC provisioning undocumented |
| Embedded (risk) | FAIL | Array OOB, stack overflow (doip_client_t), hidden malloc, printf lost |
| KISS (arch) | PASS | Clean |

### Fixes Applied in v4.0

| # | Finding | Fix |
|---|---------|-----|
| 1 | `host_coap_entries[1]` OOB write | Resize array to `[2]` (Step 4) |
| 2 | Stack overflow from `doip_client_t` on stack | Made `static doip_client_t` in relay function (Step 6) |
| 3 | Main loop blocking understated (~2s vs ~14s) | Updated D5 and function doc to state worst case ~14s |
| 4 | Lazy registration race condition | Build entry in local var, struct copy, `__sync_synchronize()` before registration (Step 4) |
| 5 | HMAC secret file lacks security checks | Added `O_NOFOLLOW`, `fstat()`, permission check (Step 3) |
| 6 | Non-atomic `dcu_phonehome_triggered` flag | Changed to `_Atomic bool` (Step 2) |
| 7 | Missing `#include` directives | Added Step 5.5 with all required headers |
| 8 | Hidden `malloc()` in DoIP client | Documented in Step 0 note |
| 9 | DoIP client `printf()` in production | Documented in Step 0 note (stdout captured by syslog) |
| 10 | No rate limiting on DCU relay | Added 60s cooldown with `dcu_relay_cooldown` field (Step 6) |
| 11 | HMAC secret provisioning undocumented | Added "HMAC Secret Provisioning" section |
| 12 | CoAP re-registration API | To verify during implementation — `imx_set_host_coap_interface()` behavior |

### Code Review (Round 1)

| Reviewer | Verdict | Key Findings |
|----------|---------|-------------|
| Security (risk) | PASS | No issues |
| Memory Safety (correctness) | PASS | No issues |
| Concurrency (risk) | FAIL→FIXED | `phone_home_triggered` made `_Atomic`, wrap-safe cooldown comparison |
| Protocol Correctness (correctness) | PASS | No issues |
| Error Handling (risk) | PASS | No issues |
| KISS (arch) | PASS | No issues |

### Concurrency Fixes Applied

| # | Finding | Fix |
|---|---------|-----|
| 1 | `phone_home_triggered` is plain `bool` but written by CoAP thread | Made `_Atomic bool` (consistency with `dcu_phonehome_triggered`) |
| 2 | `now < ctx.dcu_relay_cooldown` fails at uint32_t wrap (~49 days) | Changed to `(int32_t)(now - cooldown) < 0` signed-difference comparison |

### Deferred (not in scope)

| Item | Reason |
|------|--------|
| Discovery validation (VIN/EID filtering) | Single DCU on LAN assumption is valid for current deployment; would need `doip_client_discover_by_eid()` API verification |
| HMAC secret memory clearing on teardown | No teardown path exists in FC-1; `remote_access` runs for device lifetime |
