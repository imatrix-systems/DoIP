# Plan: FC-1 Periodic DCU Provisioning Health Check

**Date:** 2026-03-20
**Status:** PLAN — needs review before implementation

---

## Problem

The FC-1 provisions the DCU once per power cycle (`dcu_provisioned = true`). If the DCU restarts (power cycle, crash, manual restart), it loses all provisioned state:
- HMAC secret (in memory only until written to disk)
- Bastion client key (never persisted — only installed in imatrix authorized_keys at provision time)
- SSH tunnel state

The FC-1 doesn't detect this. It continues sending TesterPresent (0x3E) keepalives which succeed even on a freshly-restarted DCU. Phone-home triggers fail because the bastion client key isn't in authorized_keys.

## Solution

Add a periodic health check that the FC-1 performs on the DCU via the existing DoIP TCP connection. The health check verifies the DCU's provisioning state and re-provisions if needed.

---

## Design

### New UDS Routine: Provisioning Status Query (0xF0A2)

**Request:** `31 01 F0 A2` (4 bytes — no payload)
**Response:** `71 01 F0 A2 <status_byte> [details...]`

Status byte values:
| Value | Meaning | FC-1 Action |
|-------|---------|-------------|
| 0x00 | Fully provisioned (HMAC loaded, bastion client key installed, SSH keys present) | No action needed |
| 0x01 | HMAC not loaded (fresh start, no provisioning received) | Re-provision (0xF0A1) |
| 0x02 | HMAC loaded but bastion client key missing (provisioned without key) | Re-provision with key |
| 0x03 | HMAC loaded, key installed, but SSH keypair missing | Re-provision (triggers keygen) |

Optional detail bytes after status:
- Bytes 1-4: DCU uptime in seconds (big-endian uint32) — lets FC-1 detect restart
- Byte 5: tunnel active (1) or not (0)

### FC-1 Health Check Timing

- **Interval:** Every 5 minutes (300 seconds) while DoIP is connected
- **Trigger:** In `state_connected()` of the DoIP process state machine, using a timer
- **Method:** Send 0xF0A2 on the existing DoIP TCP connection (same as TesterPresent)
- **On status != 0x00:** Re-provision by calling `send_dcu_provision()`
- **On transport error:** DoIP connection handler already handles disconnects

### Implementation Locations

**DoIP Server (`phonehome_handler.c`):**
1. Add `ROUTINE_ID_PHONEHOME_STATUS 0xF0A2` define
2. Add `phonehome_handle_status()` function — checks HMAC loaded, bastion key installed, SSH key present
3. Register in the UDS dispatch (alongside 0xF0A0 and 0xF0A1)

**FC-1 (`remote_access.c` or `doip_process.c`):**
1. Add health check timer in `state_connected()`
2. Send 0xF0A2 via `imx_doip_send_raw_uds()`
3. Parse response — if status != 0x00, clear `dcu_provisioned` flag and call `send_dcu_provision()`

### State Diagram

```
FC-1 boots
  → Provision DCU (0xF0A1) — sets dcu_provisioned=true
  → Register DCU key with Bastion — gets bastion_client_pubkey
  → Re-provision DCU with bastion client key

Every 5 minutes (while DoIP connected):
  → Send health check (0xF0A2)
  → Response 0x00: all good, no action
  → Response 0x01/0x02/0x03: re-provision
      → Clear dcu_provisioned flag
      → Call send_dcu_provision() (includes bastion_client_pubkey if available)
      → On success: set dcu_provisioned=true
  → Transport error: DoIP disconnect handler fires, will reconnect and re-provision
```

### What the DCU Status Check Evaluates

```c
int phonehome_handle_status(const uint8_t *uds_data, uint32_t uds_len,
                             uint8_t *response, uint32_t resp_size)
{
    uint8_t status = 0x00;  /* Assume fully provisioned */

    if (!hmac_loaded) {
        status = 0x01;  /* No HMAC — needs full provisioning */
    }
    else if (g_cfg->bastion_client_key[0] == '\0') {
        status = 0x02;  /* HMAC OK but no bastion client key */
    }
    else {
        struct stat st;
        if (stat("/etc/phonehome/id_ed25519", &st) != 0) {
            status = 0x03;  /* Keys missing */
        }
    }

    /* Also check if imatrix user exists and authorized_keys has content */
    if (status == 0x00) {
        struct stat ak;
        if (stat("/home/imatrix/.ssh/authorized_keys", &ak) != 0 || ak.st_size == 0) {
            status = 0x02;  /* Key not installed */
        }
    }

    /* Build response: 71 01 F0 A2 <status> <uptime:4> <tunnel:1> */
    response[0] = 0x71;
    response[1] = 0x01;
    response[2] = 0xF0;
    response[3] = 0xA2;
    response[4] = status;

    /* Uptime */
    time_t up = time(NULL) - server_start_time;  /* Need to expose this */
    response[5] = (uint8_t)(up >> 24);
    response[6] = (uint8_t)(up >> 16);
    response[7] = (uint8_t)(up >>  8);
    response[8] = (uint8_t)(up);

    /* Tunnel active */
    struct stat lock;
    response[9] = (stat("/etc/phonehome/phonehome.lock", &lock) == 0) ? 1 : 0;

    return 10;
}
```

### FC-1 Health Check Logic

```c
/* In state_connected() or doip_process periodic handler */
#define DCU_HEALTH_CHECK_INTERVAL_MS  (300 * 1000)  /* 5 minutes */

static imx_time_t dcu_health_check_timer = 0;

/* Check if health check is due */
if (imx_is_later(current_time, dcu_health_check_timer)) {
    dcu_health_check_timer = current_time + DCU_HEALTH_CHECK_INTERVAL_MS;

    uint8_t req[4] = {0x31, 0x01, 0xF0, 0xA2};
    uint8_t resp[16];
    int resp_len = imx_doip_send_raw_uds(req, 4, resp, sizeof(resp), 3000);

    if (resp_len >= 5 && resp[0] == 0x71 && resp[4] != 0x00) {
        DCU_LOG("DCU health check: status=0x%02X — re-provisioning", resp[4]);
        ctx.dcu_provisioned = false;
        /* Will re-provision on next state_connected() iteration */
    }
    else if (resp_len >= 9) {
        uint32_t dcu_uptime = (resp[5] << 24) | (resp[6] << 16) |
                              (resp[7] << 8) | resp[8];
        /* If DCU uptime < FC-1 uptime since last provision, DCU restarted */
        if (dcu_uptime < 60) {  /* Less than 60s — just started */
            DCU_LOG("DCU health check: DCU uptime=%us (recently restarted)", dcu_uptime);
            ctx.dcu_provisioned = false;
        }
    }
}
```

---

## Files Changed

| File | Component | Changes |
|------|-----------|---------|
| `DoIP_Server/include/phonehome_handler.h` | DCU | Add `ROUTINE_ID_PHONEHOME_STATUS 0xF0A2`, declare `phonehome_handle_status()` |
| `DoIP_Server/src/phonehome_handler.c` | DCU | Implement `phonehome_handle_status()` |
| `DoIP_Server/src/doip_server.c` | DCU | Dispatch 0xF0A2 to `phonehome_handle_status()` in UDS handler |
| `Fleet-Connect-1/remote_access/remote_access.c` | FC-1 | Add health check timer + logic in `state_connected()` |

## What NOT Changed

- TesterPresent (0x3E) keepalive — continues as-is
- Phone-home trigger (0xF0A0) — unchanged
- Provisioning (0xF0A1) — unchanged, just called more often when health check detects issues
- DoIP connection management — unchanged

## Acceptance Criteria

1. DCU responds to 0xF0A2 with correct provisioning status
2. FC-1 sends 0xF0A2 every 5 minutes while DoIP connected
3. If DCU reports not fully provisioned, FC-1 re-provisions within the same 5-minute cycle
4. After DCU restart, the first health check triggers re-provisioning
5. Re-provisioning includes bastion client key if available
6. Health check does not interfere with active phone-home tunnels
7. Health check response includes DCU uptime for restart detection

## Risks

- **Extra DoIP traffic:** One 4-byte request + 10-byte response every 5 minutes — negligible
- **Re-provision during active tunnel:** Provisioning writes HMAC and keys to disk — safe even if tunnel is active. The tunnel uses the in-memory HMAC and existing SSH keys, not the files being written.
- **False re-provision:** If the health check sees status 0x02 because the imatrix user setup failed (permissions), it will re-provision repeatedly. Mitigate by capping re-provision attempts per health check cycle (max 1 per interval).
