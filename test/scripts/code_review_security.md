# Security Review: doip_test.py

**File:** `/home/greg/iMatrix/DOIP/DoIP_Server/test/scripts/doip_test.py`
**Reviewer:** Security Review Agent
**Date:** 2026-03-09

---

## 1. Port validation: range 1-65535 enforced?

**PASS**

Lines 470-472 validate the port immediately after argument parsing:

```python
if not 1 <= args.port <= 65535:
    print(f"Error: port must be 1-65535, got {args.port}", file=sys.stderr)
    sys.exit(1)
```

argparse `type=int` handles non-integer rejection. The range check covers both bounds.

---

## 2. recv buffer cap: MAX_PAYLOAD=4096, enforced in parse_header?

**PASS**

Lines 64-65 enforce the cap in `parse_header()`:

```python
if length > MAX_PAYLOAD:
    raise ValueError(f"Payload too large: {length} bytes (max {MAX_PAYLOAD})")
```

This is called before any payload `recv_exact()` call in both `recv_doip()` (line 83) and `DoIPConnection.recv_doip()` (line 231). The UDP `recvfrom` on line 147 also caps the buffer at `MAX_PAYLOAD + DOIP_HEADER_SIZE`. No unbounded reads exist.

---

## 3. No eval/exec/shell commands?

**PASS**

No usage of `eval()`, `exec()`, `os.system()`, `subprocess`, `os.popen()`, or any shell invocation. The script does not import `os` or `subprocess`.

---

## 4. No arbitrary file access (config parser was removed)?

**PASS**

No file I/O anywhere in the script. No `open()`, no `os.path`, no filesystem operations. The config parser was correctly removed; all parameters come from CLI arguments with hardcoded defaults.

---

## 5. Socket timeouts enforced on all sockets?

**PASS**

- UDP sockets: `sock.settimeout(timeout)` on line 143, default `UDP_TIMEOUT = 2.0`.
- TCP sockets: `self.sock.settimeout(self.timeout)` on line 213, default `TCP_TIMEOUT = 5.0`.
- The "diagnostic without routing" test overrides to 2.0s on line 438, which is still bounded.
- No socket is ever left in blocking mode (timeout=None).

---

## 6. No broadcast storms: UDP sends single targeted messages?

**PASS**

`udp_send_recv()` sends a single `sendto()` to the specified `(host, port)` and waits for one response. No broadcast address is used (default is `127.0.0.1`). No loops or retries on the UDP send path. The pre-flight check (line 490) is also a single send/recv.

---

## 7. No sensitive data in output?

**PASS**

Output contains only protocol-level data: VIN, EID, GID, logical addresses, response codes, and power mode values. These are diagnostic identifiers, not secrets. No passwords, tokens, keys, or credentials appear. Verbose mode (`-v`) logs raw hex of DoIP protocol frames only.

---

## 8. Input validation: --vin length, --eid format?

**PASS (with note)**

- **EID:** Validated on lines 475-479 via `parse_eid()`, which enforces exactly 6 colon-separated hex pairs. Invalid format causes immediate exit.
- **VIN:** Not length-validated at parse time, but `udp_vehicle_id_by_vin()` truncates/pads to exactly 17 bytes on line 168: `vin.encode("ascii")[:17].ljust(17, b"\x00")`. This prevents buffer overflow in the protocol payload. A wrong-length VIN will simply fail to match, which is the correct test behavior (the VIN is an expected-value for assertion, not a security-sensitive input).

No risk: the VIN is only used as a comparison value and as a 17-byte protocol field that is safely truncated/padded.

---

## 9. No denial-of-service: limited retries, bounded operations?

**PASS**

- No retry loops anywhere in the code.
- All socket operations are timeout-bounded (2s UDP, 5s TCP).
- The test suite runs a fixed set of 10 tests with no repetition.
- `recv_exact()` is bounded by the `parse_header()` length cap (4096 bytes max).
- No infinite loops; the only while loop is in `recv_exact()` (line 71) which is bounded by `n` (max `MAX_PAYLOAD`).

---

## 10. No external dependencies: pure stdlib?

**PASS**

Imports are exclusively Python standard library:

```python
import argparse
import socket
import struct
import sys
import time
```

No third-party packages. `time` is imported but unused (harmless; likely kept for future debug timing). No `pip install` or `requirements.txt` needed.

---

## Overall Verdict: PASS

All 10 security review items pass. The script is a well-contained test tool with proper input validation, bounded network operations, no file I/O, no code injection vectors, and no external dependencies.
