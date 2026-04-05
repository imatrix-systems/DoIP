# DoIP Blob Server — Client Developer Reference

This document describes the capabilities, protocol behavior, and integration requirements for clients connecting to the DoIP Blob Server. It is intended for developers implementing the client-side (tester/Fleet-Connect-1) DoIP system.

## 1. Server Overview

The DoIP Blob Server is a data collection endpoint that receives binary blobs over the standard ISO 13400-2 DoIP transport and ISO 14229-1 UDS application protocol. It supports:

- **UDP vehicle discovery** (port 13400) — identify the server on the network
- **TCP diagnostic communication** (port 13400) — routing activation + UDS messaging
- **Blob transfer** via UDS RequestDownload/TransferData/RequestTransferExit
- **CRC-32 integrity verification** on received blobs
- **Persistent storage** of received blobs to disk

The server does **not** require session changes, security access, or any pre-flash sequence. Transfers are accepted immediately after routing activation.

## 2. Network Discovery (UDP)

### 2.1 Ports and Transport

| Parameter | Value |
|-----------|-------|
| UDP port | 13400 (configurable) |
| Protocol version | 0x03 (ISO 13400-2:2019) or 0xFF (default) |

### 2.2 Supported Discovery Requests

The server responds to three types of vehicle identification requests:

| Type | Payload Type Code | Payload | Behavior |
|------|-------------------|---------|----------|
| Generic | `0x0001` | (empty) | Always responds |
| By VIN | `0x0003` | 17-byte VIN | Responds only if VIN matches |
| By EID | `0x0002` | 6-byte EID | Responds only if EID matches |

For VIN and EID requests, the server silently ignores the request if the filter does not match — no NACK is sent.

### 2.3 Vehicle Announcement Response

All discovery responses use payload type `0x0004` (Vehicle Announcement) with this structure:

| Field | Size | Description |
|-------|------|-------------|
| VIN | 17 bytes | Vehicle Identification Number (ASCII) |
| Logical Address | 2 bytes | Server's DoIP entity address (big-endian) |
| EID | 6 bytes | Entity Identifier (typically MAC address) |
| GID | 6 bytes | Group Identifier |
| Further Action Required | 1 byte | `0x00` = no further action needed |
| VIN/GID Sync Status | 1 byte | `0x00` = synchronized |

**Default identity values** (when using the shipped `doip-server.conf`):

| Field | Value |
|-------|-------|
| VIN | `APTERADOIPSRV0001` |
| Logical Address | `0x0001` |
| EID | `00:1A:2B:3C:4D:5E` |
| GID | `00:1A:2B:3C:4D:5E` |

**Compiled defaults** (when no config file is loaded): VIN = `FC1BLOBSRV0000001`. The shipped `doip-server.conf` overrides this.

These are configurable — always validate against the deployed configuration.

### 2.4 Startup Announcement

The server broadcasts a vehicle announcement on UDP at startup. Clients can listen for this passively instead of actively polling.

## 3. TCP Connection

### 3.1 Connection Parameters

| Parameter | Default |
|-----------|---------|
| TCP port | 13400 |
| Max concurrent TCP connections | 4 |
| Max DoIP payload size | 4096 bytes |
| TCP Nagle algorithm | Disabled (TCP_NODELAY) |

When the maximum number of connections is reached, new connections are accepted and immediately closed. The client should retry after a delay.

### 3.2 Routing Activation

Routing activation **must** be performed before sending any diagnostic messages. Without it, diagnostic messages are silently dropped.

**Request:** Payload type `0x0005`

| Field | Size | Value |
|-------|------|-------|
| Source Address | 2 bytes | Tester logical address (e.g., `0x0E80`) |
| Activation Type | 1 byte | `0x00` (default) |
| Reserved | 4 bytes | `0x00000000` |

**Response:** Payload type `0x0006`

| Field | Size | Description |
|-------|------|-------------|
| Tester Logical Address | 2 bytes | Echoed back |
| Entity Logical Address | 2 bytes | Server's logical address |
| Response Code | 1 byte | `0x10` = success |
| Reserved | 4 bytes | `0x00000000` |

The server accepts **all** routing activation requests — no source address filtering, no authentication required, any activation type accepted. Response code is always `0x10` (success).

### 3.3 Tester Address

The tester logical address used in routing activation becomes the "registered" source address for that connection. All subsequent diagnostic messages from this connection **must** use this same source address; mismatches result in a diagnostic NACK with code `0x02` (Invalid Source Address).

## 4. Supported UDS Services

The server implements four UDS services. All other SIDs receive NRC `0x11` (serviceNotSupported).

### 4.1 TesterPresent (`0x3E`)

Keepalive service. Two sub-function modes:

| Request | Response | Notes |
|---------|----------|-------|
| `3E 00` | `7E 00` | Standard positive response |
| `3E 80` | *(none)* | Suppress positive response bit set |

TesterPresent does not affect transfer state.

### 4.2 RequestDownload (`0x34`)

Initiates a blob transfer to the server.

**Request format:**

```
Byte 0:    0x34 (SID)
Byte 1:    dataFormatIdentifier (ignored by server, typically 0x00)
Byte 2:    addressAndLengthFormatIdentifier
              High nibble = memory size byte count (1-4)
              Low nibble  = memory address byte count (1-4)
Bytes 3+:  memoryAddress (big-endian, variable length)
Bytes N+:  memorySize (big-endian, variable length)
```

**Positive response:**

```
Byte 0:    0x74 (positive response SID)
Byte 1:    0x20 (lengthFormatIdentifier: 2 bytes for maxBlockLength)
Bytes 2-3: maxBlockLength (big-endian, always 4096)
```

**maxBlockLength = 4096** means the maximum *total* UDS payload per TransferData message is 4096 bytes. Since TransferData includes a 1-byte SID + 1-byte block sequence counter, the effective maximum *data* per block is **4094 bytes**.

However, the DoIP transport adds 4 bytes for source/target addresses, so the maximum UDS payload that fits in a single DoIP diagnostic message is also 4096. The practical maximum data per TransferData block is **4094 bytes**.

**Error responses:**

| NRC | Code | Condition |
|-----|------|-----------|
| incorrectMessageLength | `0x13` | Request too short (< 4 bytes), or address+size fields don't fit |
| requestOutOfRange | `0x31` | Address length = 0, address length > 4, size length = 0, or size length > 4 |
| uploadDownloadNotAccepted | `0x70` | Transfer already active, memory size = 0, size exceeds blob_max_size (default 16 MB), or malloc failure |

### 4.3 TransferData (`0x36`)

Sends one block of blob data.

**Request format:**

```
Byte 0:    0x36 (SID)
Byte 1:    blockSequenceCounter (starts at 1, wraps 0xFF -> 0x00)
Bytes 2+:  blockData (the actual payload bytes)
```

**Positive response:**

```
Byte 0:    0x76 (positive response SID)
Byte 1:    blockSequenceCounter (echoed)
```

**Block sequence counter rules:**
- First block after RequestDownload: BSC = `0x01`
- Increments by 1 for each subsequent block
- Wraps from `0xFF` to `0x00` (not back to `0x01`)
- Server rejects out-of-sequence blocks

**Error responses:**

| NRC | Code | Condition |
|-----|------|-----------|
| requestSequenceError | `0x24` | No active transfer (RequestDownload not called) |
| incorrectMessageLength | `0x13` | Request too short (< 2 bytes, i.e., SID only, no BSC) |
| wrongBlockSequenceCounter | `0x73` | BSC doesn't match expected value |
| transferDataSuspended | `0x71` | Block data would exceed the total memory size from RequestDownload |

### 4.4 RequestTransferExit (`0x37`)

Finalizes the transfer, verifies CRC-32, and stores the blob to disk.

**Request format:**

```
Byte 0:    0x37 (SID)
(no additional payload required)
```

**Positive response:**

```
Byte 0:    0x77 (positive response SID)
```

**CRC-32 verification:**

The server treats the **last 4 bytes** of the transferred data as a little-endian CRC-32 checksum over all preceding bytes:

```
[payload_bytes ... ] [CRC-32 (4 bytes, little-endian)]
 └── data_len ────┘
```

- If `bytes_received >= 4`: server splits off the last 4 bytes as the expected CRC, computes CRC-32 over the remaining bytes, and compares
- If CRC matches: blob is saved to disk **without** the 4-byte CRC suffix (saved size = transferred size - 4)
- If CRC mismatches: NRC `0x72` (generalProgrammingFailure), blob is discarded
- If `bytes_received < 4`: blob is saved as-is without CRC verification (edge case for very small payloads)

**CRC-32 algorithm:**
- Polynomial: `0xEDB88320` (reflected, same as zlib/IEEE 802.3)
- Initial value: `0xFFFFFFFF`
- Final XOR: `0xFFFFFFFF`
- Byte order in transfer: **little-endian** (least significant byte first)

**Error responses:**

| NRC | Code | Condition |
|-----|------|-----------|
| requestSequenceError | `0x24` | No active transfer |
| generalProgrammingFailure | `0x72` | CRC-32 mismatch |

### 4.5 RoutineControl (`0x31`)

The server handles UDS RoutineControl (SID 0x31, subFunction 0x01) for the phone-home subsystem. Three routine identifiers are supported:

| routineId | Name | Description |
|-----------|------|-------------|
| `0xF0A0` | Phone-Home Trigger | HMAC-authenticated reverse SSH tunnel trigger |
| `0xF0A1` | Provisioning | Receive HMAC secret + CAN SN from FC-1 |
| `0xF0A2` | Status Query | Returns provisioning state, uptime, tunnel status |

See `docs/DCU_PhoneHome_Specification.md` for PDU formats and security details. These routines are only relevant to phone-home integration — blob transfer clients do not need to use them.

Unrecognized routine IDs return NRC `0x12` (subFunctionNotSupported). SubFunction values other than `0x01` also return NRC `0x12`.

### 4.6 Unsupported Services

Any SID other than `0x31`, `0x34`, `0x36`, `0x37`, `0x3E` returns:

```
7F [SID] 11
```

This includes `0x10` (DiagnosticSessionControl), `0x27` (SecurityAccess), `0x35` (RequestUpload), and all others.

## 5. Complete Blob Transfer Sequence

### 5.1 Step-by-Step Protocol Flow

```
Client                              Server
  |                                    |
  |--- TCP Connect ------------------->|
  |<-- TCP Accept ---------------------|
  |                                    |
  |--- Routing Activation (0x0005) --->|
  |<-- Routing Activation OK (0x0006) -|  response_code = 0x10
  |                                    |
  |--- RequestDownload (0x34) -------->|  addr, size
  |<-- Positive Response (0x74) -------|  maxBlockLength = 4096
  |                                    |
  |--- TransferData (0x36, BSC=1) ---->|  first block
  |<-- Positive Response (0x76) -------|
  |                                    |
  |--- TransferData (0x36, BSC=2) ---->|  second block
  |<-- Positive Response (0x76) -------|
  |                                    |
  |    ... (repeat for all blocks) ... |
  |                                    |
  |--- RequestTransferExit (0x37) ---->|
  |<-- Positive Response (0x77) -------|  CRC OK, blob saved
  |                                    |
```

### 5.2 Calculating Transfer Parameters

Given `N` bytes of application data to transfer:

```
total_transfer_size  = N + 4                    (append CRC-32)
max_block_data       = maxBlockLength - 2       (subtract SID + BSC = 4094)
num_blocks           = ceil(total_transfer_size / max_block_data)
```

### 5.3 Preparing the Transfer Payload

1. Compute CRC-32 over the `N` raw data bytes
2. Append the 4-byte CRC in **little-endian** byte order
3. Split the resulting `N + 4` bytes into blocks of at most `max_block_data` bytes each
4. Send each block with incrementing BSC starting at 1

### 5.4 Example: Transferring 100 Bytes

```c
uint8_t data[100] = { /* your data */ };
uint32_t crc = crc32(data, 100);

// Build transfer payload: 100 data bytes + 4 CRC bytes = 104 bytes
uint8_t payload[104];
memcpy(payload, data, 100);
memcpy(payload + 100, &crc, 4);  // little-endian on little-endian host

// RequestDownload with memory_size = 104
// Server returns maxBlockLength = 4096
// max_block_data = 4096 - 2 = 4094
// 104 <= 4094, so this fits in a single TransferData block

// TransferData: SID=0x36, BSC=0x01, data=payload[0..103]
// RequestTransferExit: SID=0x37
```

### 5.5 Example: Transferring 8000 Bytes (Multi-Block)

```
transfer payload  = 8000 + 4 = 8004 bytes
max_block_data    = 4094 bytes
block 1 (BSC=1):  4094 bytes
block 2 (BSC=2):  3910 bytes (8004 - 4094)
total blocks:     2
```

## 6. Transfer Constraints and Behavior

### 6.1 Single Concurrent Transfer

The server supports only **one active transfer at a time**, globally across all connections. If a transfer is in progress (from any client), a new RequestDownload from any client returns NRC `0x70`.

A transfer becomes inactive when:
- RequestTransferExit completes (success or CRC failure)
- The transfer timeout expires (default 30 seconds of inactivity)
- The server shuts down

**Important:** Client disconnection does **not** immediately cancel an active transfer. The transfer remains active until the timeout expires. A new client attempting RequestDownload during this window will be rejected.

### 6.2 Transfer Timeout

The server enforces a configurable inactivity timeout (default: **30 seconds**). The timer resets on each successful TransferData block. If no TransferData is received within the timeout period, the server aborts the transfer and frees the state.

After a timeout, the next RequestDownload will succeed.

### 6.3 Size Limits

| Limit | Default Value | Notes |
|-------|---------------|-------|
| Max blob size | 16,777,216 bytes (16 MB) | Configurable via `blob_max_size` |
| Max UDS payload per message | 4,096 bytes | `DOIP_MAX_DIAGNOSTIC_SIZE` |
| Max data per TransferData block | 4,094 bytes | maxBlockLength(4096) - SID(1) - BSC(1) |

The `memory_size` in RequestDownload is the total transfer size including the 4-byte CRC suffix. It must be:
- Greater than 0
- Less than or equal to `blob_max_size`

### 6.4 Memory Address

The memory address in RequestDownload is stored as metadata in the output filename. The server does not validate or use the address for routing — any value is accepted. Use it to tag your blobs (e.g., transfer type identifier).

### 6.5 Address and Size Field Widths

The `addressAndLengthFormatIdentifier` byte encodes field widths:

| Nibble | Valid Range | Meaning |
|--------|-------------|---------|
| High (bits 7-4) | 1-4 | Memory size field width in bytes |
| Low (bits 3-0) | 1-4 | Memory address field width in bytes |

Values of 0 or >4 for either nibble return NRC `0x31` (requestOutOfRange).

Common format identifiers:
- `0x44` — 4-byte address, 4-byte size (supports addresses and sizes up to 4 GB)
- `0x24` — 2-byte address, 4-byte size
- `0x14` — 1-byte address, 4-byte size

## 7. DoIP Transport Layer Details

### 7.1 DoIP Header Format

Every DoIP message starts with an 8-byte header:

```
Byte 0:    Protocol version (0x03 or 0xFF)
Byte 1:    Inverse version  (version XOR 0xFF)
Bytes 2-3: Payload type     (big-endian uint16)
Bytes 4-7: Payload length   (big-endian uint32)
```

The server validates `version ^ 0xFF == inverse_version`. A mismatch sends header NACK code `0x00` and **closes the connection**.

### 7.2 Diagnostic Message Wrapping

UDS requests are wrapped in DoIP diagnostic messages (type `0x8001`):

```
[DoIP Header (8 bytes)]
  Bytes 0-1: Source address (tester, big-endian uint16)
  Bytes 2-3: Target address (server, big-endian uint16)
  Bytes 4+:  UDS data (SID + service-specific data)
```

The server sends a **diagnostic ACK** (type `0x8002`, code `0x00`) before processing the UDS request, then sends the UDS response wrapped in a diagnostic message (type `0x8001`) with source/target addresses swapped.

### 7.3 Header NACK Codes

| Code | Meaning | Connection Effect |
|------|---------|-------------------|
| `0x00` | Incorrect pattern (bad version/inverse) | **Connection closed** |
| `0x01` | Unknown payload type | Connection stays open |
| `0x02` | Message too large (payload > 4096) | Connection stays open |
| `0x04` | Invalid payload length | Connection stays open |

### 7.4 Diagnostic NACK Codes

Sent as payload type `0x8003`:

| Code | Meaning | Cause |
|------|---------|-------|
| `0x02` | Invalid source address | SA doesn't match routing activation |
| `0x03` | Unknown target address | Target not registered on server |

### 7.5 Diagnostic Without Routing

If a diagnostic message is sent before routing activation, the server **silently drops** it — no NACK, no response, no connection closure.

## 8. Entity Status and Power Mode

These queries work on any connected client (routing activation not required, but the connection must be established).

### 8.1 Entity Status (type `0x4001` / `0x4002`)

**Request:** Empty payload, type `0x4001`

**Response fields:**

| Field | Size | Value |
|-------|------|-------|
| Node type | 1 byte | `0x00` (gateway) |
| Max concurrent sockets | 1 byte | Configured max (default 4) |
| Currently open sockets | 1 byte | Current active TCP connections |
| Max data size | 4 bytes | `4096` (big-endian) |

This is useful for monitoring connection capacity before opening additional connections.

### 8.2 Diagnostic Power Mode (type `0x4003` / `0x4004`)

**Request:** Empty payload, type `0x4003`

**Response:**

| Field | Size | Value |
|-------|------|-------|
| Power mode | 1 byte | `0x01` (ready) |

The server always reports ready.

## 9. Blob Storage

### 9.1 Output Location

Blobs are saved to the configured `blob_storage_dir` (default: `/tmp/doip_blobs/`). The directory is created automatically if it doesn't exist.

### 9.2 Filename Format

```
YYYY-MM-DD_HHMMSS_addr_XXXXXXXX_Nbytes.bin
```

Example: `2026-03-05_143022_addr_00001000_8000bytes.bin`

| Component | Description |
|-----------|-------------|
| `YYYY-MM-DD_HHMMSS` | Server-local timestamp at save time |
| `addr_XXXXXXXX` | Memory address from RequestDownload (hex, zero-padded to 8 digits) |
| `Nbytes` | Saved data size in bytes (decimal) — excludes CRC suffix |

### 9.3 Saved Content

- If transfer size >= 4 bytes and CRC matches: raw data **without** the 4-byte CRC suffix
- If transfer size < 4 bytes: data saved as-is (no CRC verification possible)

## 10. Error Recovery

### 10.1 Transfer Failure Recovery

| Scenario | Server State | Client Action |
|----------|-------------|---------------|
| CRC mismatch at TransferExit | Transfer cleaned up | Retry RequestDownload immediately |
| Wrong block sequence counter | Transfer still active | Can retry the correct block |
| Data exceeds declared size | Transfer still active | Send RequestTransferExit to clean up, then retry |
| Transfer timeout (30s idle) | Transfer cleaned up | Retry RequestDownload |
| Client disconnect mid-transfer | Transfer remains active until timeout | Wait up to 30s, then retry |
| Server at max connections | Connection refused | Retry after delay |

### 10.2 Cleaning Up a Failed Transfer

If a transfer is stuck (wrong BSC sent, data overflow, etc.), send `RequestTransferExit (0x37)`. The server cleans up the transfer state regardless of whether the CRC check passes or fails. After cleanup, a new RequestDownload will be accepted.

### 10.3 After Client Disconnect

Because transfer state is global and not tied to a specific connection, disconnecting does **not** free the transfer slot. If the client crashes mid-transfer, the transfer state remains locked for up to `transfer_timeout` seconds (default 30). A new client must wait for the timeout before starting a new transfer.

## 11. Configuration Reference

All values are configurable via the server's `doip-server.conf` file. The client must be aware of these as they affect protocol behavior:

| Key | Default | Client Impact |
|-----|---------|---------------|
| `logical_address` | `0x0001` | Target address for diagnostic messages |
| `tcp_port` | `13400` | TCP connection port |
| `udp_port` | `13400` | UDP discovery port |
| `max_tcp_connections` | `4` | Max simultaneous clients |
| `max_data_size` | `4096` | Max DoIP diagnostic payload |
| `blob_max_size` | `16777216` | Max bytes in a single RequestDownload |
| `transfer_timeout` | `30` | Seconds of idle before transfer abort |
| `vin` | `FC1BLOBSRV0000001` | Expected in discovery responses |
| `eid` | `00:1A:2B:3C:4D:5E` | Expected in discovery responses |
| `gid` | `00:1A:2B:3C:4D:5E` | Expected in discovery responses |

## 12. Quick Integration Checklist

1. **Discover** the server via UDP `0x0001` request to port 13400, validate VIN/EID in response
2. **Connect** via TCP to the server's IP on port 13400
3. **Activate routing** with your tester address (e.g., `0x0E80`), activation type `0x00`
4. **Prepare payload**: compute CRC-32 over your data, append it as 4 little-endian bytes
5. **RequestDownload** (`0x34`): specify total size = data + 4, use target address `0x0001`
6. **TransferData** (`0x36`): send blocks with BSC starting at 1, max 4094 data bytes per block
7. **RequestTransferExit** (`0x37`): server verifies CRC and saves blob
8. **Verify**: check for positive response `0x77` — blob is on disk
9. **Keepalive**: send TesterPresent (`0x3E 0x00`) periodically if idle between transfers
10. **Disconnect** when done — or reuse the connection for additional transfers
