# DCU Phone-Home Relay Plan Review: KISS
## Review Date: 2026-03-09
## Plan Version: 3.0
## Reviewer: KISS (architecture-reviewer)
## Overall: PASS

---

### Findings

#### 1. Over-Engineering -- PASS

The plan does not introduce unnecessary abstractions. There is no new state machine, no new module boundary, no new configuration file format. The relay is a single function (`relay_dcu_phonehome()`) called from the existing state handlers. The CoAP handler follows the identical pattern as the existing `coap_post_remote_call_home()` -- set a flag, return 2.04. This is the minimum viable design.

One minor concern: the `dcu_phone_home_uri[64]` field in the context struct is reasonable but could be a local in `try_register_dcu_uri()` since the CoAP framework takes a pointer to persistent storage anyway (same pattern as `phone_home_uri`). However, since the existing code uses the same pattern (URI stored in ctx), this is consistent and acceptable.

#### 2. Minimal Changes -- PASS

The file change summary is tight: one modified C file, two copied files (HMAC), one build file. No new modules, no new headers for remote_access, no refactoring of existing code. The plan explicitly avoids Bastion code changes by leveraging the existing CoAP/DTLS dispatch architecture.

The `host_coap_entries` array grows from `[1]` to `[2]` -- this is the smallest possible change to support a second URI. The plan correctly re-calls `imx_set_host_coap_interface(2, host_coap_entries)` which updates the count.

#### 3. Abstraction Level -- PASS

`relay_dcu_phonehome()` is a single linear function: discover, connect, build PDU, send, log result. No callback chains, no strategy patterns, no abstract "relay engine." The function is approximately 50 lines of straightforward sequential code with early-return error handling. This matches the embedded C style throughout the codebase.

The plan correctly avoids creating a separate `dcu_relay.c` module. All new code lives in `remote_access.c` as static functions, which is appropriate given the feature is a small addition (~150 lines) to a ~1500-line file.

#### 4. Configuration -- PASS

The only configuration is the HMAC secret file at a hardcoded path (`/etc/phonehome/hmac_secret`). There is no new config file format, no new CLI command to set parameters, no runtime reconfiguration. The secret is loaded once at init. If the file is missing, the feature is silently disabled. This is exactly the right level of configurability for an embedded device.

The plan does not introduce configurable discovery timeout, DCU port, retry count, or any other tunable. All values are constants in the code. This is correct for an embedded system where these values are determined by the protocol spec (DoIP port 13400, 3s discovery timeout).

#### 5. Code Reuse -- PASS

The plan correctly reuses:
- `doip_client_t` and the full DoIP client lifecycle (`init`, `discover`, `connect`, `activate_routing`, `send_uds`, `destroy`) from the existing library
- `hmac_sha256()` copied from DoIP_Server rather than reimplemented
- The existing CoAP entry registration pattern from `remote_access_init()`
- The existing flag-based CoAP-to-main-loop handoff pattern (`phone_home_triggered` -> `dcu_phonehome_triggered`)
- The existing `is_plain_coap_mode()` security check

The HMAC copy (Decision D3) is the right call. A cross-repo include path would create a fragile build dependency between FC-1 and DoIP_Server. Two standalone C files with no dependencies is the simplest approach.

#### 6. Relay Flow Simplicity -- PASS

The relay flow is linear and clear:

1. CoAP handler sets flag, returns immediately
2. Main loop checks flag in `state_idle()` / `state_connected()`
3. `relay_dcu_phonehome()`: discover -> connect -> build PDU -> send -> log -> cleanup

There are no retries, no queuing, no async callbacks. The plan explicitly states "fire and forget after validation testing" (Decision D4). This is appropriate because:
- The Bastion can re-send the trigger if needed
- The DCU has its own replay/HMAC validation
- Adding retry logic would block the main loop for longer

One observation worth noting: the 3-second discovery timeout plus TCP connect plus routing activation plus UDS round-trip could block the main loop for up to ~5 seconds in the worst case. The plan acknowledges this ("~2s synchronous operation") but the worst case is higher. For an embedded system that runs a cooperative main loop, this is acceptable given it happens only on explicit operator trigger, and the plan correctly places it in the main loop (not the CoAP thread) where blocking is less harmful.

#### 7. Testing Strategy -- PASS

The testing plan is proportionate:
- HMAC unit tests are already passing in DoIP_Server (no new test code needed)
- PDU construction is a trivial layout check (4 magic bytes + memcpy)
- Integration test uses existing `doip-server` binary as the DCU stand-in
- On-device test is manual with log verification

The plan does not call for a test framework, mock objects, CI pipeline changes, or automated regression. For a feature that is a ~150-line addition to an embedded system, manual integration testing with log verification is appropriate.

#### 8. Lazy URI Registration Pattern -- PASS

The `try_register_dcu_uri()` pattern is the right approach for the CAN Controller SN dependency. The alternative would be to hook into the CAN registration callback chain, which would require modifying CAN subsystem code and creating a cross-module dependency. Polling from the main loop is simpler and the cost is one `imx_get_can_controller_sn()` call per loop iteration until registered (essentially zero overhead).

The guard pattern (`if (ctx.dcu_uri_registered) return;`) ensures the registration code runs exactly once.

#### 9. Edge Case Coverage -- PASS

The edge case table in Step 8 is thorough and every entry has the correct behavior:
- Missing prerequisites disable the feature gracefully (no crash, no error loop)
- DCU-side errors are logged with specific NRC codes for diagnostics
- FC-1 tunnel independence is explicitly addressed
- Rate limiting is correctly deferred to the DCU's own replay cache

The plan does not try to handle edge cases that are out of scope (Bastion-side registration, DCU firmware updates, network partitions).

#### 10. Security Model -- PASS

The security approach is minimal and correct:
- DTLS-only check on the CoAP handler (same as existing FC-1 handler)
- HMAC-SHA256 with `/dev/urandom` nonce for the DoIP relay
- Shared secret from a file on the device filesystem
- No key derivation, no key rotation, no certificate management

This matches the threat model: the CoAP link is already secured by DTLS, and the LAN link between FC-1 and DCU uses HMAC to prevent unauthenticated trigger injection. The HMAC secret file approach is the simplest provisioning model for factory-paired devices.

#### 11. Minor Nit: Port Override -- PASS (with note)

In `relay_dcu_phonehome()`, the plan discovers the DCU via UDP and extracts `source_addr.sin_port`, then immediately overrides it with `dcu_port = 13400`. The comment explains why (UDP source port != TCP DoIP port). This is correct behavior but the variable assignment + override is slightly awkward. A cleaner approach would be to simply use `13400` directly in the `doip_client_connect()` call. However, this is cosmetic and does not affect correctness. Not worth calling FAIL.

---

### Summary

This plan is a well-scoped, minimal addition to the existing `remote_access.c` module. It follows every established pattern in the codebase: flag-based CoAP-to-main-loop handoff, static context struct, sequential error handling with early returns, and `REMOTE_LOG()` diagnostics. No new abstractions, no unnecessary configurability, no scope creep.

The file change footprint is four files (one modified, two copied, one build file update). The new code is approximately 150 lines of straightforward C. The plan correctly leverages the existing DoIP client library for the heavy lifting and avoids introducing any cross-module dependencies beyond the existing `imx_get_can_controller_sn()` API.

The only area where a future iteration might be needed is if the ~5s worst-case main loop block becomes problematic in practice, which would require moving the relay to a worker thread. The plan correctly avoids this complexity now (YAGNI) and the synchronous approach is appropriate for an operator-triggered, infrequent operation.

Verdict: **PASS** -- implement as planned.
