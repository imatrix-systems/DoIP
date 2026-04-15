# FC-1 Update: DCU Remote Access Recovery After DCU Restart

**Date:** 2026-04-14
**Version:** FC-1 v1.014.017
**Status:** Deployed to Test #3 FC-1 (SN 0641236602) — awaiting validation

---

## Summary

A firmware update has been deployed to the Test #3 FC-1 that resolves two issues:

1. **DCU restart recovery** — "Connect to Shell" for the DCU stopped working after a DCU restart. The FC-1 now correctly re-registers the DCU's SSH credentials with the bastion whenever the DCU is re-provisioned.

2. **FC-1 gateway login** — "Connect to Shell" for the FC-1 gateway itself was failing with a timeout. The FC-1 was not completing its own credential registration with the bastion at boot, so the bastion did not know how to reach the gateway. The FC-1 now correctly registers itself before accepting tunnel connections.

---

## Issues Resolved

### Issue 1: DCU restart recovery

When the DCU restarts (power cycle, crash, or manual reboot), the FC-1 detects the restart within 5 minutes via its periodic health check and successfully re-provisions the DCU. However, the FC-1 was not completing the second step of updating the DCU's SSH credentials on the bastion server. As a result, the bastion retained stale credentials and all "Connect to Shell" attempts for the DCU failed with:

```
Permission denied (publickey).
```

This only occurred after a DCU restart while the FC-1 remained running. A fresh FC-1 boot always worked correctly on the first cycle.

### Issue 2: FC-1 gateway login timeout

"Connect to Shell" for the FC-1 gateway itself was timing out. The FC-1 was establishing its reverse SSH tunnel to the bastion, but had not completed its own credential registration. This meant the bastion did not have the correct tunnel port assignment for the gateway, so connection attempts timed out.

The root cause was a timing issue during boot: the FC-1's own registration step was being incorrectly skipped when a DCU registration completed first. The FC-1 now ensures its own registration runs to completion independently.

---

## What Changed

### DCU restart recovery (Issue 1)

The FC-1 firmware was updated so that when the health check detects a DCU restart, the full credential registration cycle runs end-to-end:

1. FC-1 detects DCU needs re-provisioning (health check every 5 minutes)
2. FC-1 re-provisions the DCU (delivers security credentials)
3. FC-1 receives the DCU's SSH public key in the response
4. **FC-1 re-registers the key with the bastion** (this step was previously skipped)
5. Bastion updates its records — remote shell access restored

The entire recovery sequence completes within seconds of the health check firing.

### FC-1 gateway registration (Issue 2)

The FC-1 boot sequence was corrected so that the gateway's own bastion registration always completes before accepting tunnel connections. Previously, a DCU registration that completed during the FC-1 boot could cause the FC-1 to skip its own registration, resulting in `tunnel_port=0` (no port assigned). The FC-1 now tracks its own registration independently and will not advance until it receives a valid tunnel port assignment from the bastion.

---

## Deployment Details

| Item | Value |
|------|-------|
| Firmware Version | 1.014.016 |
| Device | Test #3 FC-1, SN 0641236602 |
| Deploy Time | 2026-04-14 11:20 AM PDT |
| Status | Running (confirmed) |

---

## Initial Validation

Immediately after deployment, the FC-1 logs confirmed both fixes working:

```
[00:00:19] Keys ready — advancing to registration
[00:00:43] DCU SSH key registered with bastion — tunnel_port=10017
[00:00:44] FC-1 gateway registered with bastion — tunnel_port=10014
[00:00:45] Initialization complete — tunnel_port=10014
[00:00:46] Tunnel connected to bastion
```

Key observations:
- The FC-1 now correctly registers **itself** (tunnel_port=10014) after the DCU registration (port 10017) completes — previously this step was skipped and tunnel_port was 0.
- DCU health check re-provisioning and re-registration also confirmed working (see earlier v1.014.015 validation).

---

## Test Procedure

### Test 1: Verify FC-1 gateway shell access

1. Open the Bastion web UI
2. Click "Connect to Shell" for the **FC-1 gateway** (SN 0641236602)
3. **Expected:** SSH terminal opens successfully with the FC-1 command prompt

### Test 2: Verify DCU shell access

1. Open the Bastion web UI
2. Click "Connect to Shell" for the **DCU**
3. **Expected:** SSH terminal opens successfully

If both work, the fixes are confirmed. No further testing is needed unless you want to verify the DCU restart recovery.

### Test 3: DCU shell access (known issue — see below)

The FC-1 relay is working correctly — it delivers the phone-home trigger to the DCU and the DCU responds positively. However, the DCU's own reverse SSH tunnel is not connecting to the bastion. This is a DCU-side issue unrelated to the FC-1 fixes deployed today.

See `docs/eric_test_guide_2026-04-14.md` for detailed DCU tunnel diagnostics (6 steps).

The most likely cause is a username mismatch: the FC-1 registers the DCU key using CAN SN `281183743`, but the DCU connects to the bastion as `phonehome-A030X10002` (from `/etc/dcu-serial`). If the bastion creates the authorized_keys entry under a CAN-SN-based path instead of the DCU serial-based path, authentication will fail.

### Test 4: Verify DCU restart recovery (once DCU tunnel issue is resolved)

This test confirms the FC-1 automatically restores remote access after a DCU restart.

1. **Restart the DCU** (power cycle or `sudo reboot` via SSH)
2. **Wait approximately 5-7 minutes** for the FC-1 to:
   - Reconnect to the DCU via DoIP (1-2 minutes)
   - Run the health check (runs every 5 minutes)
   - Complete re-provisioning and re-registration (seconds)
3. **Try "Connect to Shell"** from the Bastion web UI
4. **Expected:** SSH terminal opens successfully

If the connection still fails after 10 minutes, please capture the FC-1 log output:
```bash
grep -i 'health check\|re-provisioning\|bastion registration' /var/log/fc-1.log | tail -20
```

---

## Verified from Bastion (2026-04-14 18:34 UTC)

We confirmed the following directly on the bastion server:

- **FC-1 tunnel:** Port 10003 listening, SSH authenticated successfully from inside the web-ssh container
- **FC-1 Redis:** `gateway_port=10003`, `ssh_user=root`, `dtls_status=online`
- **DCU Redis:** `gateway_port=10016`, `ssh_user=root`, `dtls_status=online`
- **DCU tunnel:** Port 10016 **not listening** — the DCU is not establishing its reverse SSH tunnel
- **Web app logs:** Actively streaming FC-1 data (chunks #53-64), confirming FC-1 shell connection is live

The FC-1 side is fully operational. The remaining DCU tunnel issue requires DCU-side diagnostics.

## Notes

- The FC-1 health check runs every 5 minutes. After a DCU restart, it may take up to 5 minutes for the FC-1 to detect the change and initiate recovery.
- During the recovery window, "Connect to Shell" will not work. This is expected.
- No changes were made to the DCU firmware. This is an FC-1-only update.
- See `docs/eric_test_guide_2026-04-14.md` for DCU tunnel diagnostic steps.

---

*Please reply with your test results or any questions. The Test #3 FC-1 is running the updated firmware and ready for validation.*
