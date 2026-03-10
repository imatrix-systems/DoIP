# DCU Phone-Home Relay Code Review: KISS
## Review Date: 2026-03-09
## Reviewer: KISS (architecture-reviewer)
## Overall: PASS

---

### Findings

#### 1. Minimal Code Changes -- PASS

The DCU relay adds approximately 200 lines to a ~1700-line file. All new code is contained within `remote_access.c` as static functions. No new modules, no new header files for remote_access, no refactoring of existing code. The only files touched are:

- `remote_access/remote_access.c` -- new functions + context fields + state hooks
- `remote_access/hmac_sha256.c` + `.h` -- standalone copy from DoIP_Server
- `CMakeLists.txt` -- sources + include path (already present, no new changes needed)

The `host_coap_entries` array grew from `[1]` to `[2]` -- the smallest possible change. No existing function signatures changed. No existing logic was altered beyond inserting DCU flag checks at three well-defined points.

#### 2. Relay Function Flow -- PASS

`relay_dcu_phonehome()` (lines 1596-1687) is a linear, sequential function with early-return error handling at every step:

1. Check HMAC loaded
2. Check cooldown
3. Discover DCU (UDP broadcast)
4. Generate nonce
5. Compute HMAC
6. Build 44-byte PDU
7. TCP connect
8. Routing activation
9. Send UDS, log result
10. Cleanup

No callbacks, no retry loops, no state machine within the relay, no abstraction layers. Each failure path logs a clear diagnostic and returns. The function reads top-to-bottom as a single procedural sequence. This is the simplest possible implementation of the relay.

#### 3. CoAP Handler Pattern Match -- PASS

`coap_post_dcu_phone_home()` (lines 1552-1580) is a near-exact copy of `coap_post_remote_call_home()` (lines 1365-1396). Both follow the identical pattern:

1. Cast unused params with `(void)`
2. Security check: `is_plain_coap_mode()` -> `COAP_NO_RESPONSE`
3. Set atomic flag
4. Log trigger received
5. Determine response type (ACK for CON, NON for NON)
6. `coap_store_response_header(msg, CHANGED, response_type, NULL)`
7. Return `COAP_SEND_RESPONSE`

The only difference is which flag gets set (`dcu_phonehome_triggered` vs `phone_home_triggered`) and the log message prefix. This is correct pattern reuse -- no unnecessary deviation or "improvement."

One minor ordering difference: the DCU handler sets the flag before building the response (line 1565), while the FC-1 handler sets the flag after determining response type (line 1385). Both are correct since the flag is only consumed by the main loop, not the CoAP thread. The ordering difference is cosmetic and does not warrant a FAIL.

#### 4. No Unnecessary Abstractions -- PASS

The implementation resists several tempting abstractions that would have been over-engineering:

- No generic "relay engine" or "relay_target_t" struct
- No abstract "phone home handler factory" despite two similar CoAP handlers
- No separate `dcu_relay.c` module for ~100 lines of relay code
- No configurable discovery timeout, port, or retry count
- No callback-based async relay (synchronous fire-and-forget is correct here)
- No retry mechanism (the Bastion can re-trigger if needed)

All constants are `#define` values, not runtime-configurable. The only configuration is the HMAC secret file at a hardcoded path. This is appropriate for an embedded system where these values are protocol-defined.

#### 5. CMakeLists.txt -- PASS (no changes needed)

The CMakeLists.txt already contained all necessary entries before this feature was implemented:
- `remote_access/hmac_sha256.c` at line 140
- `DoIP_Client/libdoip/doip.c` at line 141
- `DoIP_Client/libdoip/doip_client.c` at line 142
- `DoIP_Client/libdoip` include path at line 368

No build system changes were required for the relay feature itself. The prerequisite libraries were already integrated. This is the ideal outcome -- zero build churn.

#### 6. Comment Proportionality -- PASS

Comments are helpful without being excessive:

- **Context struct fields** (lines 192-198): One-line doxygen comments per field. Concise and descriptive. The `_Atomic` annotation on `dcu_phonehome_triggered` has a comment explaining the cross-thread nature ("CoAP thread -> main loop").

- **Function-level doxygen** on all three new functions (`init_load_hmac_secret`, `try_register_dcu_uri`, `coap_post_dcu_phone_home`, `relay_dcu_phonehome`): Each explains purpose, threading model, and failure behavior. The `relay_dcu_phonehome` doxygen notes the worst-case timing (~14s) and why `doip_client_t` is static. These are the comments a maintainer needs.

- **Inline comments** in `relay_dcu_phonehome()`: Numbered steps (1-7) match the plan's structure. Each step has a one-line comment. The UDS PDU construction (lines 1650-1657) has byte-level comments explaining each field. The `/* Single struct copy */` comment at line 1529 explains why the local-to-array copy pattern is used.

- No comment novels. No ASCII art. No TODO items left dangling. No commented-out alternative approaches.

#### 7. Convention Adherence -- PASS

The new code follows every existing convention in `remote_access.c`:

- **Logging**: Uses `REMOTE_LOG()` macro consistently, with module-prefixed messages
- **Error handling**: Early-return pattern with descriptive log messages at each exit point
- **Naming**: `ctx.dcu_*` fields follow the `ctx.phone_home_*` naming pattern; function names use `snake_case` throughout
- **Static functions**: All new functions are `static` (file-scoped), matching existing pattern
- **Section headers**: `/*--------------------------------------------------` separator comments match the existing `CoAP Handlers` and `CLI Commands` sections; new section is `DCU Phone-Home Relay`
- **Guard patterns**: `if (ctx.dcu_uri_registered) return;` matches the boolean-guard style used elsewhere (e.g., `if (!ctx.initialized) return;`)
- **Variable types**: Uses `imx_time_t`, `uint8_t`, `uint16_t` consistently with the rest of the file

#### 8. State Machine Integration -- PASS

The DCU relay integrates into the existing state machine at exactly three points:

1. **`remote_access_process()`** line 422: `try_register_dcu_uri()` called every iteration (no-op after registration). Placed before the state dispatch switch, which is correct -- URI registration is state-independent.

2. **`state_idle()`** lines 631-635: Check `dcu_phonehome_triggered` after the FC-1 trigger check. Clean separation -- DCU relay is independent of FC-1 tunnel state.

3. **`state_connected()`** lines 738-742: Same check, allowing DCU relay while FC-1 tunnel is active.

The DCU trigger is also cleared on DTLS reset (line 415), preventing stale triggers from firing after network recovery. This matches the existing `phone_home_triggered` reset at line 414.

No new states were added. No existing state transitions were modified. The relay operates orthogonally to the FC-1 tunnel state machine.

#### 9. Scope Discipline -- PASS

The implementation includes exactly what is needed and nothing more:

- No CLI command for DCU relay (not needed -- triggered via Bastion only)
- No status reporting for DCU relay state (not needed -- fire and forget)
- No DCU discovery caching (not needed -- relay is infrequent)
- No retry on relay failure (not needed -- Bastion can re-trigger)
- No configuration file for HMAC path or DCU port (not needed -- hardcoded values are protocol-defined)

There is no scope creep. The implementation does not touch the DTLS handling, the FC-1 tunnel logic, the CLI system, or any other subsystem.

#### 10. Implementation Improvements Over Plan -- PASS

The implementation made several small improvements over the v4.0 plan, all in the right direction:

- **Cooldown calculation**: Plan had `now + DCU_RELAY_COOLDOWN_SEC` (raw seconds). Implementation correctly uses `now + SEC_TO_MS(DCU_RELAY_COOLDOWN_SEC)` since `imx_time_t` is in milliseconds. The cooldown log message also divides by 1000 for human-readable seconds.

- **Nonce read comparison**: Plan had bare `!= sizeof(nonce)`. Implementation has `!= (ssize_t)sizeof(nonce)`, avoiding a signed/unsigned comparison warning. This is the correct C idiom.

- **Time API**: Plan used `imx_get_current_time()`. Implementation uses `imx_time_get_time(&now)`, which matches the actual API used elsewhere in the codebase.

These are correctness fixes applied during implementation, not scope additions.

---

### Summary

The DCU phone-home relay implementation is a textbook example of KISS in embedded C. It adds a focused feature (~200 lines) to an existing module without introducing new abstractions, new files, new configuration mechanisms, or new state machine complexity. Every new function follows the exact patterns established by the existing code. The CoAP handler is a clean copy of the existing pattern. The relay function is a linear sequence of steps with early-return error handling. Comments are proportionate -- explaining the "why" without restating the "what."

The implementation faithfully follows the v4.0 plan while making three small correctness improvements (cooldown units, signed cast, time API). No unnecessary refactoring was performed. The CMakeLists.txt required zero changes because the prerequisite libraries were already integrated.

The only observation worth noting for future reference: the ~14s worst-case synchronous blocking in the main loop is acceptable for an operator-triggered, infrequent operation, but if the relay frequency increases or the main loop gets tighter timing requirements, this would need to move to a worker thread. The current implementation correctly avoids that complexity (YAGNI).

Verdict: **PASS** -- implementation is minimal, focused, and follows all codebase conventions.
