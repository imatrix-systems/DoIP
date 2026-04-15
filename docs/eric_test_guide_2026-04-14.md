# FC-1 v1.014.016 — Test Guide for Eric

**Date:** 2026-04-14
**Device:** Test #3 FC-1, SN 0131557250
**DCU:** CAN SN 281183743, serial A030X10002

---

## Current Status (verified from bastion)

| Component | Status | Details |
|-----------|--------|---------|
| FC-1 tunnel | Connected | Port 10003, streaming data |
| FC-1 gateway shell | Working | Bastion web app authenticated and connected |
| FC-1 registration | OK | `tunnel_port=10003` in Redis |
| DCU registration | OK | `tunnel_port=10016` in Redis |
| DCU relay trigger | Sent | FC-1 relayed at 00:02:55, "relay OK" |
| DCU reverse tunnel | **Not connected** | Port 10016 not listening on bastion |

**FC-1 gateway "Connect to Shell" should work now.** The bastion is already connected and receiving data.

**DCU "Connect to Shell" will time out** because the DCU is not establishing its reverse SSH tunnel to the bastion. The FC-1 successfully relayed the phone-home trigger to the DCU, but the DCU's tunnel script is not connecting.

---

## What to Test

### Test 1: FC-1 Gateway Shell (should work now)

1. Open the web UI: `cloud-dev.imatrixsys.com`
2. Navigate to device **0131557250** (FC-1 gateway)
3. Click "Connect to Shell"
4. **Expected:** Terminal opens with FC-1 output or command prompt

### Test 2: DCU Shell (expected to fail — see diagnostics below)

1. Navigate to device **0281183743** (DCU)
2. Click "Connect to Shell"
3. **Expected:** Timeout — the DCU tunnel is not connected

---

## DCU Tunnel Diagnostics

The FC-1 relay is working correctly — the trigger reaches the DCU and the DCU responds positively. The problem is the DCU's own SSH tunnel to the bastion. Please run these checks **on the DCU** (SSH to it directly or via the FC-1 LAN):

### Step 1: Check if the tunnel script ran

```bash
# On the DCU:
ps aux | grep ssh | grep -v grep
```

Look for a process like:
```
ssh -R 10016:localhost:22 phonehome-A030X10002@bastion-dev.imatrixsys.com
```

If no SSH tunnel process exists, the tunnel script failed to launch or exited immediately.

### Step 2: Check the tunnel lock file

```bash
ls -la /var/run/phonehome.lock 2>/dev/null || echo "no lock file"
```

If the lock file exists but no tunnel process is running, a previous tunnel didn't clean up. Remove it:
```bash
sudo rm -f /var/run/phonehome.lock
```

Then trigger another phone-home from the web UI to retry.

### Step 3: Check the connect script exists and is executable

```bash
ls -la /usr/sbin/phonehome-connect.sh
```

If missing, the DoIP server's `generate-scripts` command can create it, or it may need to be deployed manually.

### Step 4: Test manual SSH from the DCU to the bastion

```bash
ssh -v -i /etc/phonehome/id_ed25519 \
    -o UserKnownHostsFile=/etc/phonehome/known_hosts \
    -o StrictHostKeyChecking=yes \
    -o BatchMode=yes \
    -o ConnectTimeout=15 \
    phonehome-A030X10002@bastion-dev.imatrixsys.com echo ok
```

This was run previously and showed:
```
Permission denied (publickey).
```

Key being offered: `SHA256:p+gJ42KP/DJsReArzF5P4oUdgI3Tu9fqU46Ltnr+mF4`

This means the bastion does not have this key in the `phonehome-A030X10002` user's `authorized_keys`. The FC-1 registers the DCU key via CoAP (which succeeded), but the bastion may be storing it under a different username or path.

### Step 5: Check what user the bastion expects

The FC-1 registers the DCU key with:
- **CAN SN:** 281183743
- **SSH user:** (sent in CoAP payload if DCU provided it in provision response)

The DCU connects as:
- **Username:** `phonehome-A030X10002` (from `/etc/dcu-serial`)

If the bastion creates the authorized_keys entry under a CAN-SN-based path (e.g., `/home/phonehome-0281183743/`) instead of the DCU serial-based path (`/home/phonehome-A030X10002/`), authentication will fail.

**To check on the bastion:**
```bash
# Does the user exist?
id phonehome-A030X10002

# What keys are authorized?
cat /home/phonehome-A030X10002/.ssh/authorized_keys 2>/dev/null

# Or check the CAN SN-based user:
id phonehome-0281183743
cat /home/phonehome-0281183743/.ssh/authorized_keys 2>/dev/null
```

### Step 6: Check the DoIP server log on the DCU

```bash
# If the DoIP server logs to a file:
grep -i 'phonehome\|connect\|tunnel\|ssh' /var/log/doip-server.log 2>/dev/null | tail -20

# Or check the server's console output:
grep -i 'phonehome\|connect\|tunnel' ~/DoIP/log/server.log 2>/dev/null | tail -20
```

Look for:
- `phonehome: trigger received` — confirms the relay arrived
- `phonehome: spawning connect script` — script was launched
- Any errors about missing files, permissions, or SSH failures

---

## Summary of What Was Fixed Today

| Version | Fix | Status |
|---------|-----|--------|
| v1.014.015 | DCU key re-registration after DCU restart | Verified working |
| v1.014.016 | FC-1 gateway own registration at boot | Verified working |

Both fixes are deployed and confirmed working on Test #3 FC-1. The remaining DCU tunnel issue is on the DCU/bastion side — the FC-1 relay is delivering the trigger correctly.

---

*Please send back the output from Steps 1-6 and we can pinpoint exactly where the DCU tunnel is failing.*
