# Code Review: Protocol Correctness (Phone-Home + HMAC-SHA256)

**Reviewer:** Claude (Protocol Correctness Agent)
**Date:** 2026-03-09
**Scope:** hmac_sha256.c, phonehome_handler.c, main.c (SID 0x31 dispatch), test_phonehome.c
**Reference:** DCU_PhoneHome_Specification.md, FIPS 180-4, RFC 2104, RFC 4231, ISO 14229

---

## 1. SHA-256: Initial Hash Values (FIPS 180-4 Section 5.3.3)

**PASS**

The eight initial hash values in `sha256_init()` are correct:

| Index | Code          | FIPS 180-4    | Match |
|-------|---------------|---------------|-------|
| H0    | 0x6a09e667    | 0x6a09e667    | Y     |
| H1    | 0xbb67ae85    | 0xbb67ae85    | Y     |
| H2    | 0x3c6ef372    | 0x3c6ef372    | Y     |
| H3    | 0xa54ff53a    | 0xa54ff53a    | Y     |
| H4    | 0x510e527f    | 0x510e527f    | Y     |
| H5    | 0x9b05688c    | 0x9b05688c    | Y     |
| H6    | 0x1f83d9ab    | 0x1f83d9ab    | Y     |
| H7    | 0x5be0cd19    | 0x5be0cd19    | Y     |

---

## 2. SHA-256: K Constants (FIPS 180-4 Section 4.2.2)

**PASS**

All 64 constants verified against FIPS 180-4. Spot-checked:
- K[0] = 0x428a2f98 (correct)
- K[15] = 0xc19bf174 (correct)
- K[31] = 0x14292967 (correct)
- K[47] = 0x106aa070 (correct)
- K[63] = 0xc67178f2 (correct)

All 64 values match the published standard.

---

## 3. SHA-256: Round Function

**PASS**

Verified each component against FIPS 180-4 Section 4.1.2 and 6.2.2:

| Macro      | Code                                        | Standard                                            | Correct |
|------------|---------------------------------------------|-----------------------------------------------------|---------|
| ROTR(x,n)  | `((x)>>(n)) \| ((x)<<(32-(n)))`            | Right rotate                                        | Y       |
| CH(x,y,z)  | `((x)&(y)) ^ (~(x)&(z))`                   | Ch(x,y,z) = (x AND y) XOR (NOT x AND z)            | Y       |
| MAJ(x,y,z) | `((x)&(y)) ^ ((x)&(z)) ^ ((y)&(z))`       | Maj(x,y,z) = (x AND y) XOR (x AND z) XOR (y AND z) | Y       |
| EP0(x)     | `ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22)`     | Sigma0(x)                                           | Y       |
| EP1(x)     | `ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25)`     | Sigma1(x)                                           | Y       |
| SIG0(x)    | `ROTR(x,7) ^ ROTR(x,18) ^ ((x)>>3)`       | sigma0(x)                                           | Y       |
| SIG1(x)    | `ROTR(x,17) ^ ROTR(x,19) ^ ((x)>>10)`     | sigma1(x)                                           | Y       |

Round loop (lines 95-101): `t1 = h + EP1(e) + CH(e,f,g) + K[i] + W[i]`, `t2 = EP0(a) + MAJ(a,b,c)`, variable rotation, `e = d + t1`, `a = t1 + t2`. Matches FIPS 180-4 Section 6.2.2 step 3 exactly.

Message schedule expansion (line 85): `W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16]` matches FIPS 180-4 Section 6.2.2 step 1.

---

## 4. SHA-256: Padding and Big-Endian Output

**PASS**

- Padding byte 0x80 appended after data (line 133). Correct.
- Zero-fill up to byte 56 of block (line 142). Correct (leaves 8 bytes for length).
- If `buflen > 56` after 0x80 append, processes an additional block (lines 135-140). Correct.
- 64-bit bit count written as big-endian at bytes 56-63 (lines 145-147). Verified: `buffer[56 + (7 - i)] = (ctx->bitcount >> (i * 8))` for i=7..0, which writes MSB first. Correct.
- Output digest as big-endian 32-bit words (lines 152-157). Correct: `out[i*4] = state[i] >> 24`, etc.

---

## 5. HMAC: Key Hashing When >64 Bytes

**PASS**

Lines 184-188: When `key_len > SHA256_BLOCK_SIZE` (64), the key is hashed to 32 bytes via `sha256()`, and `key_len` is set to `SHA256_DIGEST_SIZE`. This matches RFC 2104 Section 2: "if the key is longer than B bytes, reset it to key=H(key)".

---

## 6. HMAC: ipad/opad XOR and Inner/Outer Hash Sequence

**PASS**

1. Key zero-padded to block size (lines 191-192). Correct.
2. Inner key: `k_pad[i] ^ 0x36` (line 197). Correct: ipad = 0x36 per RFC 2104.
3. Inner hash: `SHA256(inner_key || data)` (lines 200-204). Correct.
4. Outer key: `k_pad[i] ^ 0x5c` (line 209). Correct: opad = 0x5c per RFC 2104.
5. Outer hash: `SHA256(outer_key || inner_hash)` (lines 211-214). Correct.

The sequence is: `HMAC(K, m) = H((K ^ opad) || H((K ^ ipad) || m))`. Implementation matches.

---

## 7. PDU Byte Offsets: Nonce at 4, HMAC at 12, Args at 44

**PASS**

Spec (Section 5.4) defines:
- Byte 0: SID (0x31)
- Byte 1: subFunction (0x01)
- Bytes 2-3: routineIdentifier (0xF0, 0xA0)
- Bytes 4-11: Nonce (8 bytes)
- Bytes 12-43: HMAC (32 bytes)
- Bytes 44+: optional args

Code defines (phonehome_handler.c lines 45-47):
- `NONCE_OFFSET = 4`
- `HMAC_OFFSET = NONCE_OFFSET + NONCE_LEN = 4 + 8 = 12`
- `ARGS_OFFSET = HMAC_OFFSET + HMAC_LEN = 12 + 32 = 44`
- `MIN_PDU_LEN = 4 + 8 + 32 = 44`

All match the specification exactly.

---

## 8. Response Format: Positive Response {0x71, 0x01, 0xF0, 0xA0, 0x02}

**PASS**

phonehome_handler.c lines 471-476:
```c
response[0] = 0x71;    /* RoutineControl positive response (SID + 0x40) */
response[1] = 0x01;    /* subFunction: startRoutine */
response[2] = 0xF0;    /* routineIdentifier high byte */
response[3] = 0xA0;    /* routineIdentifier low byte */
response[4] = 0x02;    /* routineStatus: routineRunning */
```

Matches spec Section 5.4 "Positive Response PDU" exactly:
- 0x71 = 0x31 + 0x40 (correct positive response SID echo)
- 0x01 = startRoutine subFunction
- 0xF0A0 = routineIdentifier
- 0x02 = routineRunning

Return value is 5. Correct.

---

## 9. NRC Mapping: 0x35/0x24/0x21/0x13/0x22/0x12 All Correct per ISO 14229

**PASS**

| Condition                  | Code NRC | Spec NRC | ISO 14229 Name              | Match |
|----------------------------|----------|----------|-----------------------------|-------|
| HMAC verification failed   | 0x35     | 0x35     | invalidKey                  | Y     |
| Nonce replay detected      | 0x24     | 0x24     | requestSequenceError        | Y     |
| Tunnel already active      | 0x21     | 0x21     | busyRepeatRequest           | Y     |
| PDU too short              | 0x13     | 0x13     | incorrectMessageLengthOrInvalidFormat | Y     |
| HMAC not loaded / conditions fail | 0x22 | 0x22  | conditionsNotCorrect        | Y     |
| Invalid bastion hostname   | 0x31     | N/A      | requestOutOfRange (vendor)  | Y (reasonable, not spec-mandated) |

NRC constant definitions (lines 52-56) match ISO 14229-1 Table A.1.

The NRC response format `{0x7F, 0x31, <NRC>}` (build_nrc, lines 88-92) is correct per ISO 14229: negative response SID = 0x7F, followed by the rejected service SID, followed by the NRC code.

---

## 10. Per-Case Mutex Locking: All 3 Transfer Cases Still Protected

**PASS**

In main.c `handle_diagnostic()`:

| SID  | Mutex                  | Lines     | Correct |
|------|------------------------|-----------|---------|
| 0x34 | `g_transfer_mutex`     | 381-383   | Y       |
| 0x36 | `g_transfer_mutex`     | 386-388   | Y       |
| 0x37 | `g_transfer_mutex`     | 391-393   | Y       |
| 0x3E | No mutex needed        | 399-407   | Y (stateless) |
| 0x31 | No transfer mutex needed | 410-416 | Y (phone-home has its own mutexes) |

All three transfer-related cases (0x34, 0x36, 0x37) acquire `g_transfer_mutex` before calling their handler and release after. The lock/unlock is symmetric in all paths.

The phone-home handler (0x31) correctly uses its own `phonehome_fork_mutex` and `replay_mutex` internally, and does not touch transfer state.

---

## 11. NRC 0x12 (Not 0x11) for Unknown Routines Within SID 0x31

**PASS**

main.c lines 414-415:
```c
} else {
    result = build_negative_response(sid, 0x12, response, resp_size);
}
```

This returns NRC 0x12 (`subFunctionNotSupported`) when SID is 0x31 but the routineIdentifier/subFunction combination is not recognized. This is correct per ISO 14229:

- 0x12 = `subFunctionNotSupported` -- used when the SID is supported but the sub-function or routine is not
- 0x11 = `serviceNotSupported` -- used in the `default` case (line 420) for completely unknown SIDs

The distinction is correct: the server supports SID 0x31, but only the specific routine 0xF0A0 with subFunction 0x01. Any other combination within 0x31 gets 0x12, and unknown SIDs get 0x11.

---

## 12. Test Vectors: RFC 4231 TC1-TC3 Expected Values Correct

**PASS**

Verified each test case against RFC 4231 Section 4:

**TC1:** Key = 20 bytes of 0x0b, Data = "Hi There" (8 bytes)
- Expected: `b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7`
- RFC 4231 TC1 HMAC-SHA-256: `b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7`
- Match: **YES**

**TC2:** Key = "Jefe" (4 bytes), Data = "what do ya want for nothing?" (28 bytes)
- Expected: `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`
- RFC 4231 TC2 HMAC-SHA-256: `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`
- Match: **YES**

**TC3:** Key = 20 bytes of 0xaa, Data = 50 bytes of 0xdd
- Expected: `773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe`
- RFC 4231 TC3 HMAC-SHA-256: `773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe`
- Match: **YES**

All three test vectors are byte-for-byte correct.

---

## Additional Observations

### Constant-Time Comparison

**PASS** -- `hmac_sha256_compare()` uses `volatile uint8_t result` with accumulating OR of XOR differences. The `volatile` prevents the compiler from short-circuiting. This is the standard pattern for timing-safe comparison.

### Test Coverage of NRC Codes

**PASS** -- test_phonehome.c tests:
- UT-01: Valid HMAC -> checks for `{0x71, 0x01, 0xF0, 0xA0, 0x02}` (all 5 bytes)
- UT-02: Invalid HMAC -> checks for `{0x7F, 0x31, 0x35}`
- UT-03: Replay nonce -> checks for `{0x7F, 0x31, 0x24}`
- UT-04: Short PDU -> checks for `{0x7F, 0x31, 0x13}`
- UT-05: Not initialized -> checks for `{0x7F, 0x31, 0x22}`

All NRC values match the specification. NRC 0x21 (busyRepeatRequest) is not directly tested in the unit tests -- this would require a lock-file race scenario. Acceptable for unit test scope; integration tests should cover this.

### Test PDU Construction

**PASS** -- `build_valid_pdu()` places SID at byte 0, subFunction at byte 1, routineId at bytes 2-3, nonce at byte 4, and HMAC at byte 12. All offsets match the specification.

---

## Summary

| # | Check Item                                                   | Verdict |
|---|--------------------------------------------------------------|---------|
| 1 | SHA-256 initial hash values (H0-H7)                         | PASS    |
| 2 | SHA-256 K constants (64 values)                              | PASS    |
| 3 | SHA-256 round function (CH, MAJ, Sigma, sigma)              | PASS    |
| 4 | SHA-256 padding and big-endian output                        | PASS    |
| 5 | HMAC key hashing when >64 bytes                             | PASS    |
| 6 | HMAC ipad/opad XOR and inner/outer hash sequence             | PASS    |
| 7 | PDU byte offsets: nonce at 4, HMAC at 12, args at 44        | PASS    |
| 8 | Response format {0x71, 0x01, 0xF0, 0xA0, 0x02}              | PASS    |
| 9 | NRC 0x35 / 0x24 / 0x21 / 0x13 / 0x22 correct per ISO 14229 | PASS    |
| 10 | Per-case mutex locking on all 3 transfer cases              | PASS    |
| 11 | NRC 0x12 (not 0x11) for unknown routines within SID 0x31   | PASS    |
| 12 | Test vectors RFC 4231 TC1-TC3                                | PASS    |
| -- | Constant-time comparison                                     | PASS    |
| -- | Test coverage of NRC codes                                   | PASS    |
| -- | Test PDU construction offsets                                 | PASS    |

**Result: 15/15 PASS, 0 FAIL**

All protocol correctness checks pass. The implementation faithfully follows FIPS 180-4 (SHA-256), RFC 2104 (HMAC), RFC 4231 (test vectors), ISO 14229 (UDS NRC codes), and the DCU_PhoneHome_Specification.md PDU format.
