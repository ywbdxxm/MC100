"""Offline protocol checks; never open a serial port."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
import zlib

spec = importlib.util.spec_from_file_location("mc100_serial", Path(__file__).parents[1] / "tools" / "mc100_serial.py")
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class ClientProtocol(unittest.TestCase):
    def test_buffered_reply_does_not_wait_for_an_extra_byte(self):
        connection = client.Connection("target-port")
        connection.buffer = bytearray()
        connection.serial = ScriptedTransport([b"STATUS mode=EVT_USB_BENCH\nMC100_READY\n"])
        connection.command("status", timeout=1)
        self.assertEqual(connection.serial.read_counts, [38])
    def test_delayed_boot_ready_cannot_complete_record(self):
        connection = client.Connection("target-port")
        connection.buffer = bytearray()
        connection.serial = ScriptedTransport([
            b"STATUS mode=EVT_USB_BENCH\nMC100_READY\n",
            b"RECORD result=0 reset_required=0\nMC100_READY\n",
        ])
        response = connection.command("record 3", timeout=1)
        self.assertIn("RECORD result=0 reset_required=0", response)

    def test_sync_barrier_ignores_old_boot_and_old_token(self):
        connection = client.Connection("target-port")
        connection.buffer = bytearray()
        connection.serial = ScriptedTransport([
            b"STATUS mode=EVT_USB_BENCH\nMC100_READY\nSYNC 42\nMC100_READY\n",
            b"SYNC 1234\nMC100_READY\n",
        ])
        with patch.object(client.secrets, "randbits", return_value=1234):
            connection.synchronize()
        self.assertEqual(connection.serial.writes, [b"sync 1234\n"])
        self.assertEqual(connection.serial.chunks, [])

    def test_port_is_explicit(self):
        self.assertEqual(client.Connection("target-port").port, "target-port")

    def test_transfer_identity_length_and_crc(self):
        name = "a" * 32 + "_1_0.wav"
        content = b"\x00\xff\x12\x34"
        lines = ["log", f"BEGIN {name} 512 4", content.hex(), f"END {zlib.crc32(content):08x}", "MC100_READY"]
        self.assertEqual(client.decode_transfer(lines, name, 512, 4), content)
        for offset, count in ((0, 4), (512, 3)):
            with self.assertRaises(ValueError):
                client.decode_transfer(lines, name, offset, count)
        for bad in ("END 00000000", "END 123", "END zzzzzzzz"):
            with self.assertRaises(ValueError):
                client.decode_transfer(lines[:3] + [bad], name, 512, 4)

    def test_rejects_incomplete_extra_or_wrong_file(self):
        name = "0" * 32 + "_1_0.idx"
        for lines in (
            [f"BEGIN {name} 0 1", "00"],
            [f"BEGIN {name} 0 1", "0000", "END 41d912ff"],
            ["READ result=4"],
            ["BEGIN wrong.wav 0 0", "END 00000000"],
        ):
            with self.assertRaises(ValueError):
                client.decode_transfer(lines, name, 0, 1)

    def test_remote_filename_is_finalized_and_bounded(self):
        self.assertTrue(client.valid_filename("1" * 32 + "_2_0.partial.wav"))
        for name in ("../data.wav", "target-port", "a" * 32 + "_1_0.wav.part", "a" * 140):
            self.assertFalse(client.valid_filename(name))


class ScriptedTransport:
    def __init__(self, chunks):
        self.chunks, self.writes = list(chunks), []
        self.read_counts = []

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def write(self, data):
        self.writes.append(data)

    def read(self, _count):
        self.read_counts.append(_count)
        return self.chunks.pop(0) if self.chunks else b""


if __name__ == "__main__":
    unittest.main()
