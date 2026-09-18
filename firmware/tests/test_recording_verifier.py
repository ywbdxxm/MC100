"""Hand-built format fixtures, without importing production encoders."""
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

SPEC = importlib.util.spec_from_file_location(
    "verify_recording", Path(__file__).resolve().parents[1] / "tools" / "verify_recording.py"
)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


def crc_fix(data):
    struct.pack_into("<I", data, len(data) - 4, zlib.crc32(data[:-4]))
    return data


def fixture():
    # Four independently known samples: mean 0, RMS sqrt(5), no clipping.
    pcm = bytes.fromhex("fdff ffff 0100 0300")
    wav = bytearray(512)
    wav[:36] = struct.pack("<4sI4s4sIHHIIHH", b"RIFF", 512, b"WAVE", b"fmt ",
                           16, 1, 1, 16000, 32000, 2, 16)
    wav[36:44] = b"JUNK" + struct.pack("<I", 460)
    wav[504:512] = b"data" + struct.pack("<I", 8)
    wav.extend(pcm)
    header = bytearray(512)
    header[:16] = struct.pack("<8sHHI", b"MC100IDX", 1, 512, 2)
    header[16:32] = bytes(range(16))
    struct.pack_into("<QIIHHHHQ IHHI", header, 32, 7, 0, 16000, 16, 1, 320, 0,
                     960, 9600000, 64, 0, 4096)
    crc_fix(header)
    block = bytearray(64)
    block[:56] = struct.pack("<4sHHQQQIIQII", b"MCR1", 1, 1, 0, 0, 960,
                             8, zlib.crc32(pcm), 7, 0, 0)
    crc_fix(block)
    final = bytearray(64)
    final[:56] = struct.pack("<4sHHQQQIIQII", b"MCR1", 1, 3, 1, 8, 964,
                             0, 0, 7, 0, 0)
    crc_fix(final)
    return wav, header + block + final


class RecordingVerifierTests(unittest.TestCase):
    def verify(self, wav, idx):
        with tempfile.TemporaryDirectory(prefix="mc100-verifier-") as tmp:
            wp, ip = Path(tmp) / "tiny.wav", Path(tmp) / "tiny.idx"
            wp.write_bytes(wav)
            ip.write_bytes(idx)
            before = wp.read_bytes(), ip.read_bytes()
            result = VERIFY.verify_pair(wp, ip)
            self.assertEqual((wp.read_bytes(), ip.read_bytes()), before)
            return result

    def test_known_pcm_and_final(self):
        result = self.verify(*fixture())
        self.assertEqual(result["samples"], 4)
        self.assertEqual(result["duration_seconds"], 0.00025)
        self.assertEqual((result["min"], result["max"], result["mean"]), (-3, 3, 0))
        self.assertAlmostEqual(result["rms"], math.sqrt(5))
        self.assertAlmostEqual(result["ac_rms"], math.sqrt(5))
        self.assertEqual(result["clipping_samples"], 0)
        self.assertEqual((result["generation"], result["segment"], result["first_source_sample"]), (7, 0, 960))
        self.assertEqual(result["terminal"], "FINAL")
        self.assertEqual(len(result["wav_sha256"]), 64)

    def test_detects_bit_flips(self):
        for file, position in ((0, 512), (1, 16), (1, 550)):
            with self.subTest(file=file, position=position):
                pair = fixture()
                pair[file][position] ^= 1
                with self.assertRaises(ValueError):
                    self.verify(*pair)

    def test_semantic_record_mutations_with_valid_crc(self):
        # seq, offset, sample, generation, reserved, flags, payload CRC.
        for offset, fmt, value in ((8, "Q", 3), (16, "Q", 2), (24, "Q", 961),
                                    (40, "Q", 8), (56, "I", 1), (48, "I", 1),
                                    (36, "I", 0)):
            with self.subTest(offset=offset):
                wav, idx = fixture()
                record = idx[512:576]
                struct.pack_into("<" + fmt, record, offset, value)
                idx[512:576] = crc_fix(record)
                with self.assertRaises(ValueError):
                    self.verify(wav, idx)

    def test_incident_and_terminal_rules(self):
        wav, idx = fixture()
        incident = idx[576:640]
        struct.pack_into("<H", incident, 6, 4)
        struct.pack_into("<II", incident, 48, 1, 4)
        idx[576:640] = crc_fix(incident)
        result = self.verify(wav, idx)
        self.assertEqual((result["terminal"], result["incident_reason"]), ("INCIDENT", 4))
        for bad in (idx[:576], idx + idx[576:640], idx + b"\0"):
            with self.assertRaises(ValueError):
                self.verify(wav, bad)

    def test_wav_lengths_and_format(self):
        for change in ("truncated", "trailing", "rate", "prefix", "riff"):
            wav, idx = fixture()
            if change == "truncated": wav = wav[:-2]
            elif change == "trailing": wav += b"\0\0"
            elif change == "rate": struct.pack_into("<I", wav, 24, 8000)
            elif change == "prefix": wav[36:40] = b"LIST"
            elif change == "riff": struct.pack_into("<I", wav, 4, 1)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.verify(wav, idx)

    def test_header_semantic_mutations_with_valid_crc(self):
        for offset, fmt, value in ((12, "I", 1), (32, "Q", 0), (44, "I", 8000),
                                    (54, "H", 1), (56, "Q", (1 << 64) - 1),
                                    (64, "I", 9600002), (70, "H", 1), (76, "I", 1)):
            wav, idx = fixture()
            header = idx[:512]
            struct.pack_into("<" + fmt, header, offset, value)
            idx[:512] = crc_fix(header)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.verify(wav, idx)

    def test_checkpoint_does_not_end_stream(self):
        wav, idx = fixture()
        checkpoint = idx[576:640]
        struct.pack_into("<H", checkpoint, 6, 2)
        final = idx[576:640]
        struct.pack_into("<Q", final, 8, 2)
        idx[576:] = crc_fix(checkpoint) + crc_fix(final)
        self.assertEqual(self.verify(wav, idx)["records"], 3)

    def test_dc_and_clipping_statistics(self):
        for values, expected_mean, expected_clipped in (((3, 3, 3, 3), 3, 0),
                                                       ((-32768, 32767, 0, 1), 0, 2)):
            wav, idx = fixture()
            wav[512:] = struct.pack("<4h", *values)
            block = idx[512:576]
            struct.pack_into("<I", block, 36, zlib.crc32(wav[512:]))
            idx[512:576] = crc_fix(block)
            result = self.verify(wav, idx)
            self.assertEqual(result["mean"], expected_mean)
            self.assertEqual(result["clipping_samples"], expected_clipped)
            if values == (3, 3, 3, 3):
                self.assertEqual((result["rms"], result["ac_rms"]), (3, 0))

    def test_exact_payload_coverage(self):
        wav, idx = fixture()
        block = idx[512:576]
        struct.pack_into("<II", block, 32, 6, zlib.crc32(wav[512:518]))
        idx[512:576] = crc_fix(block)
        final = idx[576:640]
        struct.pack_into("<QQ", final, 16, 6, 963)
        idx[576:640] = crc_fix(final)
        with self.assertRaisesRegex(ValueError, "cover exact"):
            self.verify(wav, idx)

    def test_cli_and_bounded_input(self):
        with tempfile.TemporaryDirectory(prefix="mc100-verifier-cli-") as tmp:
            wp, ip = Path(tmp) / "tiny.wav", Path(tmp) / "tiny.idx"
            wav, idx = fixture()
            wp.write_bytes(wav)
            ip.write_bytes(idx)
            command = [sys.executable, str(Path(VERIFY.__file__)), str(wp), str(ip)]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(json.loads(result.stdout)["valid"])
            with wp.open("wb") as oversized:
                oversized.seek(9_600_512)
                oversized.write(b"\0")
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(json.loads(result.stdout)["valid"])
            self.assertIn("size limit", json.loads(result.stdout)["error"])


if __name__ == "__main__":
    unittest.main()
