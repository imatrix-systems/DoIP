#!/usr/bin/env python3
"""DoIP server test suite — queries basic identity and status information
via UDP discovery and TCP diagnostics (ISO 13400-2)."""

import argparse
import socket
import struct
import sys

# =============================================================================
# Constants
# =============================================================================

DOIP_VERSION = 0x03
DOIP_VERSION_DISCOVERY = 0xFF
DOIP_HEADER_SIZE = 8
MAX_PAYLOAD = 4096

# Payload types
TYPE_VEHICLE_ID_REQUEST = 0x0001
TYPE_VEHICLE_ID_REQUEST_EID = 0x0002
TYPE_VEHICLE_ID_REQUEST_VIN = 0x0003
TYPE_VEHICLE_ANNOUNCEMENT = 0x0004
TYPE_ROUTING_ACTIVATION_REQUEST = 0x0005
TYPE_ROUTING_ACTIVATION_RESPONSE = 0x0006
TYPE_ENTITY_STATUS_REQUEST = 0x4001
TYPE_ENTITY_STATUS_RESPONSE = 0x4002
TYPE_POWER_MODE_REQUEST = 0x4003
TYPE_POWER_MODE_RESPONSE = 0x4004
TYPE_DIAGNOSTIC_MESSAGE = 0x8001
TYPE_DIAGNOSTIC_ACK = 0x8002
TYPE_DIAGNOSTIC_NACK = 0x8003

# Defaults
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 13400
DEFAULT_VIN = "APTERADOIPSRV0001"
DEFAULT_EID = "00:1A:2B:3C:4D:5E"
SOURCE_ADDR = 0x0E80
TARGET_ADDR = 0x0001

UDP_TIMEOUT = 2.0
TCP_TIMEOUT = 5.0

# =============================================================================
# Protocol Helpers
# =============================================================================

def build_doip(payload_type, payload=b"", version=DOIP_VERSION_DISCOVERY):
    """Build a DoIP message: header + payload."""
    inv = (~version) & 0xFF
    header = struct.pack("!BBHI", version, inv, payload_type, len(payload))
    return header + payload


def parse_header(data):
    """Parse DoIP header, return (version, inv_ver, ptype, length). Raises on invalid."""
    if len(data) < DOIP_HEADER_SIZE:
        raise ValueError(f"Header too short: {len(data)} bytes")
    ver, inv, ptype, length = struct.unpack("!BBHI", data[:DOIP_HEADER_SIZE])
    if (ver ^ inv) & 0xFF != 0xFF:
        raise ValueError(f"Version mismatch: ver=0x{ver:02X}, inv=0x{inv:02X}")
    if length > MAX_PAYLOAD:
        raise ValueError(f"Payload too large: {length} bytes (max {MAX_PAYLOAD})")
    return ver, inv, ptype, length


def recv_exact(sock, n):
    """Receive exactly n bytes from a TCP socket."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"Connection closed (got {len(buf)}/{n} bytes)")
        buf += chunk
    return buf


def recv_doip(sock):
    """Receive a complete DoIP message from TCP. Returns (ptype, payload)."""
    header = recv_exact(sock, DOIP_HEADER_SIZE)
    _ver, _inv, ptype, length = parse_header(header)
    payload = recv_exact(sock, length) if length > 0 else b""
    return ptype, payload


def parse_announcement(payload):
    """Parse vehicle announcement payload (33 bytes) into a dict."""
    if len(payload) < 33:
        raise ValueError(f"Announcement too short: {len(payload)} bytes")
    vin = payload[0:17].decode("ascii", errors="replace")
    logical_addr = struct.unpack("!H", payload[17:19])[0]
    eid = payload[19:25]
    gid = payload[25:31]
    further_action = payload[31]
    sync_status = payload[32]
    return {
        "vin": vin,
        "logical_addr": logical_addr,
        "eid": eid,
        "gid": gid,
        "further_action": further_action,
        "sync_status": sync_status,
    }


def format_bytes(data):
    """Format bytes as XX:XX:XX string."""
    return ":".join(f"{b:02X}" for b in data)


def parse_eid(eid_str):
    """Parse 'XX:XX:XX:XX:XX:XX' into 6 bytes."""
    parts = eid_str.split(":")
    if len(parts) != 6:
        raise ValueError(f"EID must be 6 hex pairs: {eid_str}")
    return bytes(int(p, 16) for p in parts)


# =============================================================================
# Verbose logging
# =============================================================================

VERBOSE = False

def log_tx(label, data):
    if VERBOSE:
        print(f"  TX [{label}]: {data.hex()}")

def log_rx(label, data):
    if VERBOSE:
        print(f"  RX [{label}]: {data.hex()}")


# =============================================================================
# UDP Discovery
# =============================================================================

def udp_send_recv(host, port, msg, timeout=UDP_TIMEOUT):
    """Send a UDP DoIP message and receive a response. Returns raw bytes or None on timeout."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        log_tx("UDP", msg)
        sock.sendto(msg, (host, port))
        try:
            data, _addr = sock.recvfrom(MAX_PAYLOAD + DOIP_HEADER_SIZE)
            log_rx("UDP", data)
            return data
        except socket.timeout:
            return None


def udp_vehicle_id_request(host, port):
    """Send Vehicle ID Request (0x0001), return announcement dict or None."""
    msg = build_doip(TYPE_VEHICLE_ID_REQUEST)
    data = udp_send_recv(host, port, msg)
    if data is None:
        return None
    _ver, _inv, ptype, length = parse_header(data)
    if ptype != TYPE_VEHICLE_ANNOUNCEMENT:
        raise ValueError(f"Expected announcement (0x0004), got 0x{ptype:04X}")
    return parse_announcement(data[DOIP_HEADER_SIZE:DOIP_HEADER_SIZE + length])


def udp_vehicle_id_by_vin(host, port, vin):
    """Send Vehicle ID by VIN (0x0003), return announcement dict or None."""
    vin_bytes = vin.encode("ascii")[:17].ljust(17, b"\x00")
    msg = build_doip(TYPE_VEHICLE_ID_REQUEST_VIN, vin_bytes)
    data = udp_send_recv(host, port, msg)
    if data is None:
        return None
    _ver, _inv, ptype, length = parse_header(data)
    if ptype != TYPE_VEHICLE_ANNOUNCEMENT:
        raise ValueError(f"Expected announcement (0x0004), got 0x{ptype:04X}")
    return parse_announcement(data[DOIP_HEADER_SIZE:DOIP_HEADER_SIZE + length])


def udp_vehicle_id_by_eid(host, port, eid_bytes):
    """Send Vehicle ID by EID (0x0002), return announcement dict or None."""
    msg = build_doip(TYPE_VEHICLE_ID_REQUEST_EID, eid_bytes)
    data = udp_send_recv(host, port, msg)
    if data is None:
        return None
    _ver, _inv, ptype, length = parse_header(data)
    if ptype != TYPE_VEHICLE_ANNOUNCEMENT:
        raise ValueError(f"Expected announcement (0x0004), got 0x{ptype:04X}")
    return parse_announcement(data[DOIP_HEADER_SIZE:DOIP_HEADER_SIZE + length])


# =============================================================================
# TCP Connection (context manager)
# =============================================================================

class DoIPConnection:
    """TCP connection to a DoIP server with routing activation and diagnostics."""

    def __init__(self, host, port, timeout=TCP_TIMEOUT):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.close()

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.host, self.port))

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def send_doip(self, payload_type, payload=b"", version=DOIP_VERSION):
        msg = build_doip(payload_type, payload, version)
        log_tx("TCP", msg)
        self.sock.sendall(msg)

    def recv_doip(self):
        header = recv_exact(self.sock, DOIP_HEADER_SIZE)
        _ver, _inv, ptype, length = parse_header(header)
        payload = recv_exact(self.sock, length) if length > 0 else b""
        log_rx("TCP", header + payload)
        return ptype, payload

    def routing_activation(self, source_addr=SOURCE_ADDR):
        """Send routing activation request. Returns (response_code, entity_addr)."""
        payload = struct.pack("!HB4s", source_addr, 0x00, b"\x00" * 4)
        self.send_doip(TYPE_ROUTING_ACTIVATION_REQUEST, payload)
        ptype, resp = self.recv_doip()
        if ptype != TYPE_ROUTING_ACTIVATION_RESPONSE:
            raise ValueError(f"Expected routing response (0x0006), got 0x{ptype:04X}")
        if len(resp) < 5:
            raise ValueError(f"Routing response too short: {len(resp)} bytes")
        _tester_addr, entity_addr, code = struct.unpack("!HHB", resp[:5])
        return code, entity_addr

    def entity_status(self):
        """Send Entity Status Request (0x4001). Returns dict."""
        self.send_doip(TYPE_ENTITY_STATUS_REQUEST)
        ptype, resp = self.recv_doip()
        if ptype != TYPE_ENTITY_STATUS_RESPONSE:
            raise ValueError(f"Expected entity status (0x4002), got 0x{ptype:04X}")
        if len(resp) < 3:
            raise ValueError(f"Entity status too short: {len(resp)} bytes")
        node_type = resp[0]
        max_sockets = resp[1]
        open_sockets = resp[2]
        max_data_size = struct.unpack("!I", resp[3:7])[0] if len(resp) >= 7 else None
        return {
            "node_type": node_type,
            "max_sockets": max_sockets,
            "open_sockets": open_sockets,
            "max_data_size": max_data_size,
        }

    def power_mode(self):
        """Send Diagnostic Power Mode Request (0x4003). Returns power mode byte."""
        self.send_doip(TYPE_POWER_MODE_REQUEST)
        ptype, resp = self.recv_doip()
        if ptype != TYPE_POWER_MODE_RESPONSE:
            raise ValueError(f"Expected power mode (0x4004), got 0x{ptype:04X}")
        if len(resp) < 1:
            raise ValueError("Power mode response empty")
        return resp[0]

    def send_diagnostic(self, uds_data, source=SOURCE_ADDR, target=TARGET_ADDR):
        """Send diagnostic message. Returns (ack_code, uds_response_bytes).
        Handles the two-phase response: ACK (0x8002) then response (0x8001)."""
        payload = struct.pack("!HH", source, target) + uds_data
        self.send_doip(TYPE_DIAGNOSTIC_MESSAGE, payload)

        # Phase 1: Diagnostic ACK
        ptype, ack_payload = self.recv_doip()
        if ptype == TYPE_DIAGNOSTIC_NACK:
            nack_code = ack_payload[4] if len(ack_payload) >= 5 else 0xFF
            return nack_code, None
        if ptype != TYPE_DIAGNOSTIC_ACK:
            raise ValueError(f"Expected diagnostic ACK (0x8002), got 0x{ptype:04X}")
        ack_code = ack_payload[4] if len(ack_payload) >= 5 else 0x00

        # Phase 2: Diagnostic response
        ptype, resp_payload = self.recv_doip()
        if ptype != TYPE_DIAGNOSTIC_MESSAGE:
            raise ValueError(f"Expected diagnostic response (0x8001), got 0x{ptype:04X}")
        # Skip SA(2) + TA(2) to get UDS data
        uds_response = resp_payload[4:] if len(resp_payload) > 4 else b""
        return ack_code, uds_response

    def tester_present(self):
        """Send TesterPresent [0x3E, 0x00]. Returns True if positive response."""
        ack_code, uds = self.send_diagnostic(bytes([0x3E, 0x00]))
        if uds is None:
            return False
        return len(uds) >= 1 and uds[0] == 0x7E


# =============================================================================
# Test Runner
# =============================================================================

class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def run(self, name, func):
        """Run a test function. Catches exceptions, prints PASS/FAIL."""
        try:
            func()
            self.passed += 1
            return True
        except AssertionError as e:
            print(f"[FAIL] {name}")
            print(f"  {e}")
            self.failed += 1
            return False
        except Exception as e:
            print(f"[FAIL] {name}")
            print(f"  Exception: {type(e).__name__}: {e}")
            self.failed += 1
            return False

    def summary(self):
        total = self.passed + self.failed
        print(f"\n=== Results: {self.passed} passed, {self.failed} failed ===")
        return self.failed == 0


# =============================================================================
# Tests
# =============================================================================

def make_tests(host, port, expected_vin, expected_eid):
    """Create test functions bound to the given parameters."""

    eid_bytes = parse_eid(expected_eid)
    tests = []

    # --- UDP Discovery ---

    def test_udp_discovery():
        ann = udp_vehicle_id_request(host, port)
        assert ann is not None, "No response to vehicle ID request"
        assert ann["vin"] == expected_vin, f"VIN mismatch: {ann['vin']}"
        assert ann["eid"] == eid_bytes, f"EID mismatch: {format_bytes(ann['eid'])}"
        print(f"[PASS] Vehicle ID Request")
        print(f"  VIN:     {ann['vin']}")
        print(f"  Address: 0x{ann['logical_addr']:04X}")
        print(f"  EID:     {format_bytes(ann['eid'])}")
        print(f"  GID:     {format_bytes(ann['gid'])}")
    tests.append(("Vehicle ID Request", test_udp_discovery))

    def test_vin_positive():
        ann = udp_vehicle_id_by_vin(host, port, expected_vin)
        assert ann is not None, "No response for correct VIN"
        assert ann["vin"] == expected_vin, f"VIN mismatch: {ann['vin']}"
        print(f"[PASS] VIN filter (positive match)")
    tests.append(("VIN filter (positive)", test_vin_positive))

    def test_vin_negative():
        ann = udp_vehicle_id_by_vin(host, port, "WRONGVIN000000000")
        assert ann is None, "Got response for wrong VIN (expected silence)"
        print(f"[PASS] VIN filter (negative — no response)")
    tests.append(("VIN filter (negative)", test_vin_negative))

    def test_eid_positive():
        ann = udp_vehicle_id_by_eid(host, port, eid_bytes)
        assert ann is not None, "No response for correct EID"
        assert ann["eid"] == eid_bytes, f"EID mismatch: {format_bytes(ann['eid'])}"
        print(f"[PASS] EID filter (positive match)")
    tests.append(("EID filter (positive)", test_eid_positive))

    # --- TCP Queries ---

    def test_routing_activation():
        with DoIPConnection(host, port) as conn:
            code, entity = conn.routing_activation()
            assert code == 0x10, f"Routing failed: code=0x{code:02X}"
            assert entity == TARGET_ADDR, f"Entity mismatch: 0x{entity:04X}"
            print(f"[PASS] Routing activation (code=0x{code:02X}, entity=0x{entity:04X})")
    tests.append(("Routing activation", test_routing_activation))

    def test_entity_status():
        with DoIPConnection(host, port) as conn:
            conn.routing_activation()
            status = conn.entity_status()
            assert status["node_type"] == 0, f"Unexpected node type: {status['node_type']}"
            assert status["max_sockets"] > 0, "max_sockets is 0"
            print(f"[PASS] Entity status (node=gateway, max_sockets={status['max_sockets']}, "
                  f"open={status['open_sockets']}, max_data={status['max_data_size']})")
    tests.append(("Entity status", test_entity_status))

    def test_power_mode():
        with DoIPConnection(host, port) as conn:
            conn.routing_activation()
            mode = conn.power_mode()
            assert mode == 0x01, f"Unexpected power mode: 0x{mode:02X}"
            print(f"[PASS] Power mode (ready)")
    tests.append(("Power mode", test_power_mode))

    def test_tester_present():
        with DoIPConnection(host, port) as conn:
            conn.routing_activation()
            ok = conn.tester_present()
            assert ok, "TesterPresent did not get positive response"
            print(f"[PASS] TesterPresent (0x7E response)")
    tests.append(("TesterPresent", test_tester_present))

    def test_unsupported_sid():
        with DoIPConnection(host, port) as conn:
            conn.routing_activation()
            _ack, uds = conn.send_diagnostic(bytes([0x22, 0xF1, 0x90]))
            assert uds is not None, "No UDS response"
            assert len(uds) >= 3, f"Response too short: {len(uds)} bytes"
            assert uds[0] == 0x7F, f"Expected negative response (0x7F), got 0x{uds[0]:02X}"
            assert uds[1] == 0x22, f"SID echo mismatch: 0x{uds[1]:02X}"
            assert uds[2] == 0x11, f"Expected NRC 0x11 (serviceNotSupported), got 0x{uds[2]:02X}"
            print(f"[PASS] Unsupported SID (NRC=0x{uds[2]:02X})")
    tests.append(("Unsupported SID", test_unsupported_sid))

    def test_diagnostic_without_routing():
        with DoIPConnection(host, port) as conn:
            # Send diagnostic WITHOUT routing activation — server should drop silently
            payload = struct.pack("!HH", SOURCE_ADDR, TARGET_ADDR) + bytes([0x3E, 0x00])
            conn.send_doip(TYPE_DIAGNOSTIC_MESSAGE, payload)
            try:
                conn.sock.settimeout(2.0)
                _ptype, _resp = conn.recv_doip()
                # If we get here, the server responded (unexpected)
                assert False, "Server responded to diagnostic without routing (expected silence)"
            except (socket.timeout, ConnectionError):
                print(f"[PASS] Diagnostic without routing (silence — timeout)")
    tests.append(("Diagnostic without routing", test_diagnostic_without_routing))

    return tests


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="DoIP server test suite — queries basic identity and status "
                    "information via UDP discovery and TCP diagnostics.")
    parser.add_argument("-H", "--host", default=DEFAULT_HOST,
                        help=f"Server IP address (default: {DEFAULT_HOST})")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT,
                        help=f"Server port (default: {DEFAULT_PORT})")
    parser.add_argument("--vin", default=DEFAULT_VIN,
                        help=f"Expected VIN for validation (default: {DEFAULT_VIN})")
    parser.add_argument("--eid", default=DEFAULT_EID,
                        help=f"Expected EID for validation (default: {DEFAULT_EID})")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Print raw TX/RX hex dumps")
    args = parser.parse_args()

    # Validate port
    if not 1 <= args.port <= 65535:
        print(f"Error: port must be 1-65535, got {args.port}", file=sys.stderr)
        sys.exit(1)

    # Validate EID format
    try:
        parse_eid(args.eid)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    global VERBOSE
    VERBOSE = args.verbose

    print("=== DoIP Server Test Suite ===")
    print(f"Server: {args.host}:{args.port}")
    print()

    # Pre-flight: check server reachable via UDP
    print("Pre-flight: checking server reachability...")
    probe = udp_send_recv(args.host, args.port, build_doip(TYPE_VEHICLE_ID_REQUEST))
    if probe is None:
        print(f"Error: no UDP response from {args.host}:{args.port} — is the server running?",
              file=sys.stderr)
        sys.exit(1)
    print("Server responding.\n")

    # Build and run tests
    tests = make_tests(args.host, args.port, args.vin, args.eid)
    runner = TestRunner()

    print("--- UDP Discovery ---")
    for name, func in tests[:4]:
        runner.run(name, func)

    print("\n--- TCP Queries ---")
    for name, func in tests[4:]:
        runner.run(name, func)

    print()
    success = runner.summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
