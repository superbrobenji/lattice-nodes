"""
Hardware-in-the-loop test harness.
Connects to ESP32 nodes over serial, sends protobuf frames, verifies responses.
"""
import struct
import time
import serial
from typing import Optional


BAUD_RATE = 115200

# Serial opcodes (must match planetopia-protocol/c/opcodes.h)
OP_NODE_ID_SET        = 0xC0
OP_CONFIG_SET         = 0xC1
OP_TX_POWER_SET       = 0xC2
OP_HEALTH_REQ         = 0xB0
OP_HEALTH_REPORT      = 0xB1
OP_LED_SOLID          = 0xD0
OP_LED_OFF            = 0xD1
OP_LED_BLINK          = 0xD2
OP_RELAY_SET          = 0xD8
OP_COMMAND_ACK        = 0xE0


def write_frame(port: serial.Serial, payload: bytes) -> None:
    """Write a 2-byte LE length-prefixed frame."""
    header = struct.pack('<H', len(payload))
    port.write(header + payload)
    port.flush()


def read_frame(port: serial.Serial, timeout: float = 5.0) -> Optional[bytes]:
    """Read one 2-byte LE length-prefixed frame, returns payload bytes or None on timeout."""
    port.timeout = timeout
    header = port.read(2)
    if len(header) < 2:
        return None
    length = struct.unpack('<H', header)[0]
    if length == 0 or length > 4096:
        return None
    data = port.read(length)
    return data if len(data) == length else None


def wait_for_line(port: serial.Serial, prefix: str, timeout: float = 10.0) -> Optional[str]:
    """Read lines until one starts with prefix, or timeout."""
    deadline = time.time() + timeout
    port.timeout = 0.1
    while time.time() < deadline:
        line = port.readline().decode('utf-8', errors='replace').strip()
        if line.startswith(prefix):
            return line
    return None


class Node:
    def __init__(self, serial_port: str, name: str = ''):
        self.name = name
        self.port = serial.Serial(serial_port, BAUD_RATE, timeout=1.0)
        time.sleep(2.0)  # Allow ESP32 boot

    def close(self):
        self.port.close()

    def send_opcode(self, opcode: int, payload: bytes = b'') -> None:
        write_frame(self.port, bytes([opcode]) + payload)

    def read_next_frame(self, timeout: float = 5.0) -> Optional[bytes]:
        return read_frame(self.port, timeout)

    def wait_for_log(self, substring: str, timeout: float = 10.0) -> bool:
        deadline = time.time() + timeout
        self.port.timeout = 0.1
        while time.time() < deadline:
            try:
                line = self.port.readline().decode('utf-8', errors='replace')
                if substring in line:
                    return True
            except Exception:
                pass
        return False

    def get_public_key(self, timeout: float = 5.0) -> Optional[bytes]:
        """Read PLANETOPIA_PUBKEY:... line from serial, return 32 bytes."""
        line = wait_for_line(self.port, 'PLANETOPIA_PUBKEY:', timeout)
        if not line:
            return None
        hex_str = line.split('PLANETOPIA_PUBKEY:')[1].strip()
        return bytes.fromhex(hex_str) if len(hex_str) == 64 else None

    def send_enrollment_approve(self, mac: bytes, server_url: str, admin_key: str) -> None:
        """Approve enrollment via server HTTP API (POST /api/v1/enrollments/{mac}/approve)."""
        import requests
        mac_str = ':'.join(f'{b:02X}' for b in mac)
        url = f"{server_url}/api/v1/enrollments/{mac_str}/approve"
        resp = requests.post(url, headers={"Authorization": f"Bearer {admin_key}"}, timeout=5.0)
        resp.raise_for_status()
