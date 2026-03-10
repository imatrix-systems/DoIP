# Error Handling Review — doip_test.py

**Reviewer:** Error Handling Agent
**Date:** 2026-03-09
**File:** `/home/greg/iMatrix/DOIP/DoIP_Server/test/scripts/doip_test.py`

---

## 1. recv_exact(): handles partial reads, raises on connection close

**PASS**

Lines 69-77: The loop accumulates chunks until exactly `n` bytes are received. An empty `recv()` return (peer closed) raises `ConnectionError` with a clear message showing bytes received vs expected. This correctly handles both partial reads and mid-stream disconnects.

---

## 2. Header validation: checks version/inverse, rejects oversized payloads

**PASS**

Lines 57-66: `parse_header()` validates three conditions:
- Header length >= 8 bytes (line 60)
- Version XOR inverse == 0xFF (line 62)
- Payload length <= MAX_PAYLOAD (4096) (line 64)

All raise `ValueError` with descriptive messages. The XOR check `(ver ^ inv) & 0xFF != 0xFF` is the correct complement validation per ISO 13400-2.

---

## 3. Context manager: DoIPConnection __enter__/__exit__ ensures socket cleanup

**PASS**

Lines 204-209: `__enter__` calls `connect()` and returns self; `__exit__` calls `close()`. The `close()` method (lines 216-222) guards against `self.sock` being None, catches `OSError` on close, and sets `self.sock = None` to prevent double-close. If `connect()` raises inside `__enter__`, `__exit__` is not called by the context manager protocol, but `self.sock` is only assigned after `socket.socket()` succeeds (line 212), and the OS will reclaim it. All test functions using TCP consistently use the `with DoIPConnection(...)` pattern.

---

## 4. Socket timeouts: UDP 2s, TCP 5s set correctly

**PASS**

Constants defined at lines 43-44: `UDP_TIMEOUT = 2.0`, `TCP_TIMEOUT = 5.0`. UDP timeout is applied in `udp_send_recv()` (line 143). TCP timeout is applied in `DoIPConnection.connect()` (line 213) via the constructor default (line 198). The `test_diagnostic_without_routing` test overrides to 2.0s (line 438) which is appropriate for a silence-expected test.

---

## 5. Pre-flight: clean error message when server not reachable

**PASS**

Lines 489-495: Before running any tests, a UDP probe is sent. On timeout (None return), a clear message is printed to stderr naming the host:port and suggesting the server may not be running, then exits with code 1. This prevents a cascade of confusing test failures when the server is simply down.

---

## 6. Test isolation: one test failure doesn't crash remaining tests

**PASS**

Lines 317-332: `TestRunner.run()` wraps each test function in a try/except that catches both `AssertionError` and the general `Exception` base class. Each failure increments the counter and prints details, then control returns to the loop. The test loop (lines 502-507) iterates unconditionally, so a failure in one test does not skip subsequent tests.

---

## 7. Exception types: socket.timeout, ConnectionError, ConnectionRefusedError all handled

**PASS**

- `socket.timeout`: caught in `udp_send_recv()` (line 150), returns None gracefully; caught in `test_diagnostic_without_routing` (line 442) as expected behavior.
- `ConnectionError` (parent of `ConnectionRefusedError`): caught in `test_diagnostic_without_routing` (line 442); also raised by `recv_exact()` (line 75).
- `ConnectionRefusedError`: would be caught by TestRunner's general `Exception` handler (line 328) for TCP tests, producing a clear FAIL message with the exception type and details.

All three exception types result in either graceful handling or clean test failure reporting.

---

## 8. Port validation: rejects out-of-range ports

**PASS**

Lines 469-472: After argparse parses the integer, the range check `1 <= args.port <= 65535` rejects invalid ports with a message to stderr and exit code 1. Note: argparse's `type=int` (line 459) handles non-integer input, so both non-numeric and out-of-range cases are covered.

---

## 9. EID format validation: rejects malformed --eid

**PASS**

Lines 474-479: The `parse_eid()` function (lines 113-118) validates exactly 6 colon-separated hex pairs. It raises `ValueError` for wrong part count (line 117) and `int(p, 16)` raises `ValueError` for non-hex characters. The main function catches this and prints a clean error to stderr before exiting with code 1.

---

## 10. No resource leaks: all sockets closed in all paths

**PASS**

- UDP: `udp_send_recv()` uses `with socket.socket(...)` (line 142), guaranteeing close on all paths including exceptions.
- TCP tests: All use `with DoIPConnection(...)` (e.g., lines 387, 395, 405, 412, 421, 433), guaranteeing `__exit__` -> `close()`.
- TCP `close()`: guards against None socket, catches OSError, sets socket to None (lines 216-222).
- Pre-flight probe: uses `udp_send_recv()` which has the `with` guard.

No socket is created outside a context manager or without a corresponding close path.

---

## Overall Verdict: PASS

All 10 review items pass. The script demonstrates solid error handling throughout:
- Robust partial-read handling and connection-close detection in `recv_exact()`
- Protocol-level validation of headers and payload sizes
- Clean resource management via context managers on all socket paths
- Test isolation via exception-catching test runner
- Input validation for both port and EID before any network operations
- Clear, actionable error messages on pre-flight failure
