# DCU SSH User Configuration — Developer Guide

**Date:** 2026-04-04
**Audience:** DCU firmware developer (independent DoIP server implementation)
**Scope:** How the `SSH_USER` field flows through the phone-home provisioning and tunnel chain

---

## 1. Overview

The phone-home system uses SSH to create a reverse tunnel from the DCU back to the bastion server. When the bastion's web terminal connects _through_ that tunnel to reach the DCU, it authenticates as a specific SSH user on the DCU. The `SSH_USER` configuration controls which local user account the bastion connects to.

There are two separate SSH users in the system:

| User | Where | Purpose |
|------|-------|---------|
| `tunnel` | Bastion side | The DCU connects _to_ the bastion as `tunnel@bastion` to open the reverse tunnel |
| `SSH_USER` (default: `imatrix`) | DCU side | The bastion connects _through_ the tunnel to the DCU as this user to open a terminal |

This document covers the DCU-side `SSH_USER` only.

---

## 2. How SSH_USER Is Configured

### 2.1 phonehome.conf

The DCU reads `SSH_USER` from its phone-home configuration file:

```ini
# /etc/phonehome/phonehome.conf
BASTION_HOST=bastion-dev.imatrixsys.com
HMAC_SECRET_FILE=/etc/phonehome/hmac_secret
CONNECT_SCRIPT=/usr/sbin/phonehome-connect.sh
LOCK_FILE=/var/run/phonehome.lock
SSH_USER=imatrix
```

**Default:** `imatrix` (set in `phonehome_config_load()` before parsing the file).

### 2.2 Provisioning Default

If the DCU has no `phonehome.conf` file (fresh deployment), the provisioning handler uses a static fallback config (`g_provision_cfg`) with `ssh_user = "imatrix"`.

---

## 3. What the DCU Does with SSH_USER

### 3.1 Server Startup

On startup, if `phonehome.conf` is loaded and `ssh_user` is non-empty, the server calls:

```c
phonehome_ensure_ssh_user(&phonehome_cfg);
```

This function:
1. Checks if the user exists (`getpwnam()`)
2. If not, creates it with `useradd -m -s /bin/bash <ssh_user>` (or `adduser -D` on Alpine/BusyBox)
3. Creates `~<ssh_user>/.ssh/` with mode 0700
4. If a bastion client key is configured (`BASTION_CLIENT_KEY` in phonehome.conf), appends it to `~<ssh_user>/.ssh/authorized_keys` with mode 0600
5. Sets ownership to the user's uid/gid

**Special case:** If `ssh_user` is `root`, the `.ssh` directory is `/root/.ssh` instead of `/home/root/.ssh`.

### 3.2 During Provisioning (0xF0A1)

When the FC-1 provisions the DCU with a bastion client key, the provision handler calls `phonehome_ensure_ssh_user()` twice:

```c
/* Primary user (from config, default "imatrix") */
phonehome_ensure_ssh_user(&g_provision_cfg);

/* Also install for root (belt and suspenders) */
phonehome_config_t root_cfg = g_provision_cfg;
strncpy(root_cfg.ssh_user, "root", sizeof(root_cfg.ssh_user) - 1);
phonehome_ensure_ssh_user(&root_cfg);
```

This ensures the bastion client key is in `authorized_keys` for both `imatrix` and `root`, regardless of which user the bastion tries to connect as.

---

## 4. SSH_USER in the Provision Response

### 4.1 Response PDU Format

When the DCU responds to a provision request (0xF0A1), it returns the DCU's SSH public key and optionally the SSH username:

```
Offset  Length  Field
 0      1       Response SID: 0x71
 1      1       SubFunc echo: 0x01
 2-3    2       RoutineId echo: 0xF0A1
 4      1       Status: 0x00 (success)
 5+     N+1     DCU SSH public key (null-terminated string)
 5+N+1  M+1     SSH username (null-terminated string, OPTIONAL)
```

### 4.2 When SSH_USER Is Included

The DCU only includes the `ssh_user` field in the response if it is **non-default**:

```c
const char *ssh_user = g_cfg ? g_cfg->ssh_user : "";
bool include_ssh_user = (ssh_user[0] != '\0' &&
                          strcmp(ssh_user, "imatrix") != 0 &&
                          strcmp(ssh_user, "tunnel") != 0);
```

**Omitted when:**
- Empty string
- `"imatrix"` (the default -- FC-1 and bastion already assume this)
- `"tunnel"` (reserved for the bastion-side tunnel user, would cause confusion)

**Included when:** Any other value (e.g., `"root"`, `"admin"`, a custom user).

### 4.3 Why This Matters

The FC-1 extracts the `ssh_user` from the provision response and passes it to the bastion during key registration. The bastion stores it in Redis and uses it when connecting through the reverse tunnel to open a terminal session on the DCU. If the DCU returns no `ssh_user`, the bastion defaults to `"root"`.

---

## 5. End-to-End Flow

```
                        Provision (0xF0A1)
FC-1 ──────────────────────────────────────────> DCU
       HMAC secret, CAN SN, bastion host,
       bastion client pubkey

                        Provision Response
FC-1 <────────────────────────────────────────── DCU
       DCU SSH pubkey + ssh_user (optional)


                     CoAP PUT register/{sn}
FC-1 ─────────────────────────────────────────> Bastion
       {"pubkey": "<dcu_pubkey>",
        "target": "dcu",
        "ssh_user": "<ssh_user>"}              ← included if DCU returned it


                     Web Terminal Connect
Bastion ─── SSH through reverse tunnel ──────> DCU
       ssh -p <tunnel_port> <ssh_user>@localhost
       authenticates with bastion_client_key
```

### 5.1 Field Flow Summary

| Step | Source | Destination | Field |
|------|--------|-------------|-------|
| 1. Config load | `phonehome.conf` | DCU `g_cfg->ssh_user` | `SSH_USER=imatrix` |
| 2. User creation | DCU | Local system | `useradd imatrix` |
| 3. Key install | DCU | `~imatrix/.ssh/authorized_keys` | bastion client pubkey |
| 4. Provision response | DCU | FC-1 `ctx.dcu_ssh_user` | `"imatrix"` (omitted) or custom |
| 5. Bastion registration | FC-1 | Bastion Redis `device:{sn}` | `ssh_user` field |
| 6. Terminal connect | Bastion | DCU via tunnel | `ssh <ssh_user>@localhost` |

---

## 6. Implementation Requirements for DCU Developer

### 6.1 Minimum (Use Defaults)

If the DCU uses `SSH_USER=imatrix` (the default), no special handling is needed:

1. Set `SSH_USER=imatrix` in `phonehome.conf`
2. Ensure the `imatrix` user exists with a home directory and `.ssh/authorized_keys`
3. Install the bastion client key in `~imatrix/.ssh/authorized_keys` during provisioning
4. Do NOT include `ssh_user` in the provision response (omit the field entirely)
5. The bastion defaults to `"root"` when `ssh_user` is absent, but the current DCU server also installs the key for `root` as a fallback

### 6.2 Custom SSH User

If the DCU needs a different SSH user (e.g., `"admin"` or `"operator"`):

1. Set `SSH_USER=<custom_name>` in `phonehome.conf`
2. Create the user account on the DCU with a home directory
3. Install the bastion client key in `~<custom_name>/.ssh/authorized_keys`
4. **Include the `ssh_user` in the provision response** after the DCU pubkey:
   ```
   Response: 71 01 F0 A1 00 <pubkey\0> <ssh_user\0>
   ```
5. The FC-1 will extract it and register it with the bastion
6. The bastion will connect as that user through the tunnel

### 6.3 Security Considerations

- The `ssh_user` account should have a restricted shell or limited sudo if it's not `root`
- Password authentication should be disabled; only pubkey auth via the bastion client key
- The bastion client key must be in `authorized_keys` for the specified user
- `phonehome-connect.sh` always connects to the bastion as `tunnel@bastion` regardless of `SSH_USER` -- `SSH_USER` only affects inbound connections from the bastion to the DCU

---

## 7. phonehome.conf Reference

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `HMAC_SECRET_FILE` | Yes | (none) | Path to 32-byte HMAC shared secret |
| `CONNECT_SCRIPT` | Yes | (none) | Path to the SSH tunnel script |
| `BASTION_HOST` | No | `bastion-dev.imatrixsys.com` | Bastion hostname (must be FQDN, not IP) |
| `LOCK_FILE` | No | `/var/run/phonehome.lock` | Tunnel lock file path |
| `SSH_USER` | No | `imatrix` | Local user for bastion inbound SSH connections |
| `SSH_CA_PUBKEY` | No | (none) | SSH CA public key for `@cert-authority` known_hosts |
| `BASTION_CLIENT_KEY` | No | (none) | Bastion's SSH client public key (installed in authorized_keys) |

---

## 8. Debugging

### Verify SSH user setup

```bash
# Check if user exists
id imatrix

# Check authorized_keys
cat /home/imatrix/.ssh/authorized_keys

# Check permissions
ls -la /home/imatrix/.ssh/
# Expected: drwx------ (700) .ssh/
# Expected: -rw------- (600) authorized_keys

# Verify bastion can authenticate (manual test through tunnel)
# On bastion, with tunnel active on port 10017:
ssh -p 10017 -i /opt/web-ssh-bastion/bastion_key imatrix@localhost
```

### Check what the DCU returns in provision response

Run the DoIP server with verbose logging (`-v`) and look for:

```
phonehome: [PROV STEP 6] Returning DCU pubkey (94 bytes) in response
phonehome: returning ssh_user 'admin' in response     ← only if non-default
```

If you don't see the `ssh_user` line, the default (`imatrix`) is being used and the field is omitted from the response.

### Verify bastion has the correct ssh_user

```bash
# On bastion:
sudo docker exec web-ssh-bastion-redis-1 redis-cli HGETALL device:<CAN_SN>
# Look for "ssh_user" field
```

If `ssh_user` shows `"root"` and you expected `"imatrix"`, the DCU either didn't return the field (expected for default) or the bastion auto-registration defaulted to `root`. The bastion tries both `root` and the configured `ssh_user` when connecting, so this is usually not a problem as long as the authorized_keys are installed for both users.
