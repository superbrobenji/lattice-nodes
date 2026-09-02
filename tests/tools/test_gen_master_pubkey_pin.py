#!/usr/bin/env python3
"""Host tests for tools/gen_master_pubkey_pin.py (#126).

The pin every leaf compiles in (lattice::mesh::pin::MASTER_PUBKEY) must be the
MASTER BOARD's own on-device public key — the value it prints at boot as
``LATTICE_PUBKEY:<64 hex>`` and stamps into every JOIN_ACK — not the hub's
``masterkey.json``. These tests pin down the tool's input contract (bare hex,
prefixed hex, serial-monitor capture, stdin), its refusal of ``masterkey.json``,
and that the bytes it writes are exactly the bytes the board printed, in order.

They also cross-check the two committed test fixtures that the C++ suites rely
on agreeing byte-for-byte: tests/mocks/master_pubkey_pin.h (the host-test pin)
and tests/e2e/harness/MasterKeypairFixture.h (the sim master's on-device key).

Run directly (``python3 tests/tools/test_gen_master_pubkey_pin.py``) or via
ctest (registered in tests/CMakeLists.txt).
"""
import io
import re
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import gen_master_pubkey_pin as gen  # noqa: E402

# == sim::fixture::MASTER_PUBLIC_KEY == lattice::mesh::pin::MASTER_PUBKEY (test builds)
FIXTURE_HEX = "19BB3572A22F52493A0957B1562323246636675E3342A4BE41ACBA36F0701132"
FIXTURE_BYTES = bytes.fromhex(FIXTURE_HEX)
OTHER_HEX = "00" * 31 + "01"
MAC_STR = "aa:bb:cc:dd:ee:01"
MAC_BYTES = bytes.fromhex("aabbccddee01")


def c_array_bytes(header_text: str, symbol: str) -> bytes:
    """Parse `constexpr uint8_t <symbol>[N] = { 0x.., ... };` back into bytes."""
    m = re.search(symbol + r"\[\d+\]\s*=\s*\{([^}]*)\}", header_text)
    if not m:
        raise AssertionError(f"{symbol} initializer not found")
    return bytes(int(tok, 16) for tok in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


class ParsePubkeyTest(unittest.TestCase):
    def test_bare_hex(self):
        self.assertEqual(gen.resolve_pubkey(FIXTURE_HEX), FIXTURE_BYTES)

    def test_prefixed_hex_lowercase_and_whitespace(self):
        self.assertEqual(gen.resolve_pubkey(f"  LATTICE_PUBKEY:{FIXTURE_HEX.lower()} \n"), FIXTURE_BYTES)

    def test_wrong_length_hex_is_rejected(self):
        with self.assertRaises(gen.PinError):
            gen.resolve_pubkey(FIXTURE_HEX[:-2])

    def test_serial_capture_file_uses_last_pubkey_line(self):
        # A real `idf.py monitor` capture: boot banner noise, CR line endings,
        # an older key from before a factory reset, then the current key.
        log = (
            "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\r\n"
            f"LATTICE_PUBKEY:{OTHER_HEX}\r\n"
            "[MAIN] Booted as: MASTER\r\n"
            f"\x1b[0;32mLATTICE_PUBKEY:{FIXTURE_HEX}\x1b[0m\r\n"
            "[MESH] Mesh initialized\r\n"
        )
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "monitor.log"
            p.write_text(log)
            self.assertEqual(gen.resolve_pubkey(str(p)), FIXTURE_BYTES)

    def test_serial_capture_from_stdin(self):
        self.assertEqual(
            gen.resolve_pubkey("-", stdin=io.StringIO(f"noise\nLATTICE_PUBKEY:{FIXTURE_HEX}\n")),
            FIXTURE_BYTES,
        )

    def test_capture_without_pubkey_line_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "monitor.log"
            p.write_text("[MAIN] Booted as: NODE\n")
            with self.assertRaises(gen.PinError) as cm:
                gen.resolve_pubkey(str(p))
        self.assertIn("LATTICE_PUBKEY", str(cm.exception))

    def test_masterkey_json_is_refused_with_explanation(self):
        # The hub's own identity file — never the pin source (#126).
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "masterkey.json"
            p.write_text('{"public_key": [1, 2, 3], "private_key": [4, 5, 6]}')
            with self.assertRaises(gen.PinError) as cm:
                gen.resolve_pubkey(str(p))
        msg = str(cm.exception)
        self.assertIn("masterkey.json", msg)
        self.assertIn("LATTICE_PUBKEY", msg)

    def test_json_shaped_file_with_other_name_is_refused_too(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "key.txt"
            p.write_text('{"publicKey": "abc="}')
            with self.assertRaises(gen.PinError):
                gen.resolve_pubkey(str(p))

    def test_missing_path_is_an_error(self):
        with self.assertRaises(gen.PinError):
            gen.resolve_pubkey("/nonexistent/monitor.log")


class ParseMacTest(unittest.TestCase):
    def test_accepts_common_separators(self):
        for s in ("aa:bb:cc:dd:ee:01", "AA-BB-CC-DD-EE-01", "aabbccddee01", "MAC: aa:bb:cc:dd:ee:01"):
            self.assertEqual(gen.parse_mac(s), MAC_BYTES, s)

    def test_rejects_wrong_length(self):
        with self.assertRaises(gen.PinError):
            gen.parse_mac("aa:bb:cc:dd:ee")


class HeaderTest(unittest.TestCase):
    def test_header_bytes_roundtrip_in_printed_order(self):
        # main.cpp prints pubKey[0..31] as %02X in order; the pin must be the
        # same bytes in the same order (memcmp'd raw in Enrollment::processJoinAck).
        text = gen.render_header(FIXTURE_BYTES, MAC_BYTES)
        self.assertEqual(c_array_bytes(text, "MASTER_PUBKEY"), FIXTURE_BYTES)
        self.assertEqual(c_array_bytes(text, "MASTER_MAC"), MAC_BYTES)
        self.assertIn("namespace lattice { namespace mesh { namespace pin {", text)
        self.assertIn(FIXTURE_HEX, text)  # human-checkable against the board's line

    def test_main_writes_requested_output_path(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "sub" / "master_pubkey_pin.h"
            rc = gen.main(["gen", f"LATTICE_PUBKEY:{FIXTURE_HEX}", MAC_STR, "--output", str(out)])
            self.assertEqual(rc, 0)
            self.assertEqual(c_array_bytes(out.read_text(), "MASTER_PUBKEY"), FIXTURE_BYTES)

    def test_main_reports_masterkey_json_misuse(self):
        with tempfile.TemporaryDirectory() as d:
            key = Path(d) / "masterkey.json"
            key.write_text('{"public_key": [0]}')
            out = Path(d) / "pin.h"
            err = io.StringIO()
            rc = gen.main(["gen", str(key), MAC_STR, "--output", str(out)], stderr=err)
            self.assertNotEqual(rc, 0)
            self.assertFalse(out.exists())
            self.assertIn("LATTICE_PUBKEY", err.getvalue())


class CommittedFixturesAgreeTest(unittest.TestCase):
    """tests/mocks/master_pubkey_pin.h (the pin host tests compile against) and
    MasterKeypairFixture.h (the keypair the sim master boots with) must be the
    same key — that identity is exactly what #126 requires of a real deployment."""

    def test_mock_pin_equals_sim_master_on_device_key(self):
        mock = (REPO_ROOT / "tests/mocks/master_pubkey_pin.h").read_text()
        fixture = (REPO_ROOT / "tests/e2e/harness/MasterKeypairFixture.h").read_text()
        self.assertEqual(c_array_bytes(mock, "MASTER_PUBKEY"), FIXTURE_BYTES)
        self.assertEqual(c_array_bytes(fixture, "MASTER_PUBLIC_KEY"), FIXTURE_BYTES)

    def test_tool_reproduces_mock_pin_from_its_lattice_pubkey_line(self):
        mock = (REPO_ROOT / "tests/mocks/master_pubkey_pin.h").read_text()
        mac = c_array_bytes(mock, "MASTER_MAC")
        text = gen.render_header(gen.resolve_pubkey(f"LATTICE_PUBKEY:{FIXTURE_HEX}"), mac)
        self.assertEqual(c_array_bytes(text, "MASTER_PUBKEY"), c_array_bytes(mock, "MASTER_PUBKEY"))
        self.assertEqual(c_array_bytes(text, "MASTER_MAC"), mac)


if __name__ == "__main__":
    unittest.main()
