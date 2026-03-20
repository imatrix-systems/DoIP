# DoIP Phone-Home — Final Diagnosis Plan

**Date:** 2026-03-19
**Goal:** Get Bastion web-ssh terminal working through the DCU reverse tunnel

---

## Current State

The reverse SSH tunnel from CAN-Test to Bastion:10008 is **established and working**. The remaining failure is the Bastion web-ssh app authenticating through the tunnel to CAN-Test's SSH server.

### How the Bastion Authenticates (from app.py line 765)

```python
asyncssh.connect(
    "localhost",
    port=ssh_port,           # 10008 (from Redis device:0911934657 gateway_port)
    username=ssh_user,       # "greg" (from Redis device:0911934657 ssh_user)
    known_hosts=None,        # Skip host key check
    client_keys=[SSH_KEY_PATH],  # /opt/web-ssh-bastion/bastion_key
    password=SSH_PASSWORD,   # env SSH_PASSWORD, default "PasswordQConnect"
)
```

It tries **two auth methods** in order:
1. **Public key** using the file at `SSH_KEY_PATH` (`/opt/web-ssh-bastion/bastion_key`)
2. **Password** using `SSH_PASSWORD` env var (default: `PasswordQConnect`)

Both fail on CAN-Test because:
- CAN-Test user `greg` doesn't have the Bastion's pubkey in `authorized_keys`
- CAN-Test password for `greg` is `Sierra007!`, not `PasswordQConnect`

---

## Step-by-Step Diagnosis & Fix

### Step 1: Verify the tunnel is alive

**On CAN-Test:**
```bash
ps aux | grep ssh | grep tunnel
```
Expected: SSH process with `-R 10008:localhost:22 tunnel@bastion-dev.imatrixsys.com`

If not running:
```bash
sudo rm -f /etc/phonehome/phonehome.lock
# Trigger from Bastion, or restart DoIP server
```

### Step 2: Verify Bastion port 10008 is listening

**On Bastion:**
```bash
ss -tlnp | grep 10008
```
Expected: `LISTEN 0 128 127.0.0.1:10008 0.0.0.0:*`

### Step 3: Check Redis device config

**On Bastion:**
```bash
sudo docker exec web-ssh-bastion-redis-1 redis-cli HGETALL device:0911934657
```
Verify:
- `ssh_user` = `greg` (for CAN-Test) or `root` (for embedded)
- `gateway_port` = `10008`

### Step 4: Check Bastion SSH_PASSWORD environment variable

**On Bastion:**
```bash
# If running via Docker:
sudo docker exec web-ssh-bastion-web-1 env | grep SSH_PASSWORD

# If running via systemd:
grep SSH_PASSWORD /opt/web-ssh-bastion/.env
```
- Current default: `PasswordQConnect` (wrong for CAN-Test `greg` user)
- CAN-Test greg password: `Sierra007!`

### Step 5: Fix the authentication (choose ONE)

#### Option A: Set correct password in Bastion (quickest for CAN-Test)

```bash
# On Bastion — set password for this specific test device
# Option 1: Set global SSH_PASSWORD to CAN-Test's password
sudo docker exec web-ssh-bastion-web-1 sh -c 'echo "SSH_PASSWORD=Sierra007!" >> /app/.env'

# Option 2: Or edit docker-compose.yml environment section:
# SSH_PASSWORD=Sierra007!

# Then restart the web app:
sudo docker compose restart web
```

**Note:** This changes the global password for ALL devices. For production, the Bastion should read per-device credentials from Redis.

#### Option B: Add Bastion's SSH key to CAN-Test (better for testing)

```bash
# On Bastion — get the key the web app uses for SSH:
sudo docker exec web-ssh-bastion-web-1 cat /opt/web-ssh-bastion/bastion_key.pub

# If that file doesn't exist, check what SSH_KEY_PATH is set to:
sudo docker exec web-ssh-bastion-web-1 env | grep SSH_KEY_PATH

# On CAN-Test — add the key to greg's authorized_keys:
mkdir -p ~/.ssh && chmod 700 ~/.ssh
echo "<paste bastion pubkey here>" >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
```

#### Option C: Set per-device password in Redis (production approach)

This requires a code change in `app.py` to read `ssh_password` from the device's Redis hash instead of the global `SSH_PASSWORD` env var. Not needed for this test.

### Step 6: Verify CAN-Test SSH accepts the auth method

**On CAN-Test:**
```bash
# Check SSH config allows password auth:
grep -i "PasswordAuthentication" /etc/ssh/sshd_config
# Should be: PasswordAuthentication yes (or not present, defaults to yes)

# Check SSH config allows pubkey auth:
grep -i "PubkeyAuthentication" /etc/ssh/sshd_config
# Should be: PubkeyAuthentication yes (or not present, defaults to yes)
```

### Step 7: Test auth manually through the tunnel

**On Bastion:**
```bash
# Test password auth through the tunnel:
ssh -p 10008 -o StrictHostKeyChecking=no greg@localhost
# Enter: Sierra007!

# Test key auth through the tunnel (if Option B was done):
ssh -p 10008 -i /home/tunnel/.ssh/id_ed25519 -o StrictHostKeyChecking=no greg@localhost
```

### Step 8: Trigger from web UI and verify

1. Open the Bastion web UI
2. Connect to device 911934657
3. Watch Bastion logs:
   ```bash
   sudo docker logs -f web-ssh-bastion-web-1 2>&1 | grep -i "ssh\|auth\|device=0911934657"
   ```

Expected success:
```
Connecting SSH to localhost:10008 as greg device=0911934657
SSH connected to device=0911934657
```

### Step 9: Verify Bastion tunnel_monitor detects the tunnel

**On Bastion:**
```bash
# Check if tunnel_monitor is running:
sudo docker ps | grep monitor

# Check Redis tunnel status:
sudo docker exec web-ssh-bastion-redis-1 redis-cli HGETALL device:0911934657:tunnel
```
Expected: `status: connected`

If not, check `tunnel_monitor.py` is scanning the correct port range.

---

## Summary of What Needs to Happen

| Step | Who | Action | Time |
|------|-----|--------|------|
| 1 | CAN-Test | Verify tunnel is alive (`ps aux | grep ssh`) | 10s |
| 2 | Bastion | Verify port 10008 listening (`ss -tlnp`) | 10s |
| 3 | Bastion | Check Redis `ssh_user` = `greg` | 10s |
| 4 | Bastion | Check `SSH_PASSWORD` env var | 10s |
| 5 | Bastion | Set `SSH_PASSWORD=Sierra007!` OR add bastion key to CAN-Test | 2min |
| 6 | CAN-Test | Verify sshd allows password/pubkey auth | 10s |
| 7 | Bastion | Test `ssh -p 10008 greg@localhost` manually | 30s |
| 8 | Browser | Trigger web terminal connection | 30s |
| 9 | Bastion | Verify tunnel_monitor detects connection | 10s |

**Total estimated time to resolve: 5-10 minutes**

The DoIP server code, FC-1 relay, and SSH tunnel are all working correctly. The only gap is Bastion → DCU SSH authentication configuration.
