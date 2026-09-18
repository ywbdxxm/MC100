"""Validate C-produced WAV using an independent standard-library reader."""

import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import wave


class WaveInterop(unittest.TestCase):
    def test_standard_reader_recovers_exact_pcm(self):
        executable = pathlib.Path(sys.argv[1]).resolve()
        with tempfile.TemporaryDirectory(prefix="mc100-wav-") as directory:
            path = pathlib.Path(directory) / "fixture.wav"
            subprocess.run([str(executable), str(path)], check=True)
            self.assertEqual(path.stat().st_size, 1152)
            with wave.open(str(path), "rb") as recording:
                self.assertEqual(recording.getparams()[:4], (1, 2, 16000, 320))
                self.assertEqual(recording.readframes(320), struct.pack("<320h", *range(320)))
                self.assertEqual(recording.readframes(1), b"")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
