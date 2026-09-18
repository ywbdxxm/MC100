"""Read-only, independent standard-library verifier for an MC100 WAV/index pair."""
import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import sys
import wave
import zlib

MAX_PCM = 9_600_000
MAX_INDEX = 512 + 4096 * 64
UINT64_MAX = (1 << 64) - 1


def require(condition, message):
    if not condition:
        raise ValueError(message)


def bounded_read(path, maximum):
    with Path(path).open("rb") as source:
        data = source.read(maximum + 1)
    require(len(data) <= maximum, f"{path}: exceeds format size limit")
    return data


def unpack(fmt, data, offset):
    return struct.unpack_from("<" + fmt, data, offset)[0]


def check_crc(data, label):
    require(zlib.crc32(data[:-4]) == unpack("I", data, len(data) - 4), f"{label}: CRC mismatch")


def verify_pair(wav_path, index_path):
    """Validate finalized bytes; return measurements, never modify either file.

    A valid INCIDENT is reported as incomplete rather than rejected. Numeric
    waveform measurements cannot establish intelligibility or acoustic validity.
    The paths are explicit local inputs; no discovery or device access occurs.
    """
    wav_data = bounded_read(wav_path, 512 + MAX_PCM)
    index = bounded_read(index_path, MAX_INDEX)
    require(len(wav_data) >= 512, "WAV: truncated prefix")
    pcm_bytes = unpack("I", wav_data, 508)
    require(pcm_bytes <= MAX_PCM and pcm_bytes % 2 == 0, "WAV: invalid PCM size")
    require(len(wav_data) == 512 + pcm_bytes, "WAV: data length mismatch")
    require(wav_data[:4] == b"RIFF" and wav_data[8:16] == b"WAVEfmt ", "WAV: invalid signature")
    require(unpack("I", wav_data, 4) == len(wav_data) - 8, "WAV: RIFF length mismatch")
    require(struct.unpack_from("<IHHIIHH", wav_data, 16) == (16, 1, 1, 16000, 32000, 2, 16),
            "WAV: expected 16000-Hz, signed 16-bit mono PCM")
    require(wav_data[36:40] == b"JUNK" and unpack("I", wav_data, 40) == 460 and
            not any(wav_data[44:504]) and wav_data[504:508] == b"data", "WAV: invalid 512-byte prefix")
    pcm = wav_data[512:]
    try:
        with wave.open(io.BytesIO(wav_data), "rb") as reader:
            require(reader.getparams()[:4] == (1, 2, 16000, pcm_bytes // 2), "WAV: standard reader format mismatch")
            require(reader.readframes(reader.getnframes()) == pcm and reader.readframes(1) == b"",
                    "WAV: standard reader PCM mismatch")
    except (wave.Error, EOFError) as error:
        raise ValueError(f"WAV: standard reader rejected file: {error}") from error

    require(len(index) >= 576 and (len(index) - 512) % 64 == 0, "index: truncated or missing records")
    header = index[:512]
    check_crc(header, "index header")
    require(header[:8] == b"MC100IDX" and unpack("H", header, 8) == 1 and
            unpack("H", header, 10) == 512, "index: invalid signature/version")
    require(unpack("I", header, 12) == 2, "index: not CLAIMED")
    generation = unpack("Q", header, 32)
    segment = unpack("I", header, 40)
    first_sample = unpack("Q", header, 56)
    require(generation != 0 and first_sample <= UINT64_MAX - MAX_PCM // 2, "index: invalid source identity")
    require(struct.unpack_from("<IHHHH", header, 44) == (16000, 16, 1, 320, 0), "index: PCM contract mismatch")
    require(struct.unpack_from("<IHHI", header, 64) == (MAX_PCM, 64, 0, 4096) and
            not any(header[76:508]), "index: invalid limits/reserved bytes")
    offset = 0
    terminal = None
    incident_reason = 0
    record_count = (len(index) - 512) // 64
    for sequence in range(record_count):
        require(terminal is None, "index: record after terminal")
        record = index[512 + sequence * 64:576 + sequence * 64]
        check_crc(record, f"record {sequence}")
        magic, version, kind, seq, position, source, size, payload_crc, gen, flags, detail, reserved = struct.unpack_from(
            "<4sHHQQQIIQIII", record)
        require(magic == b"MCR1" and version == 1 and kind in (1, 2, 3, 4) and reserved == 0,
                f"record {sequence}: invalid schema")
        require(seq == sequence and gen == generation and position == offset and source == first_sample + offset // 2,
                f"record {sequence}: sequence/offset/source/generation discontinuity")
        if kind == 1:
            require(2 <= size <= 4096 and size % 2 == 0 and offset + size <= pcm_bytes and
                    source <= UINT64_MAX - size // 2 and flags == 0 and detail == 0,
                    f"record {sequence}: invalid BLOCK")
            require(zlib.crc32(pcm[offset:offset + size]) == payload_crc, f"record {sequence}: payload CRC mismatch")
            offset += size
        else:
            require(size == 0 and payload_crc == 0, f"record {sequence}: non-BLOCK payload")
            if kind == 4:
                require(flags == 1 and 1 <= detail <= 9, f"record {sequence}: invalid INCIDENT")
                terminal, incident_reason = "INCIDENT", detail
            else:
                require(flags == 0 and detail == 0, f"record {sequence}: invalid flags/detail")
                if kind == 3:
                    terminal = "FINAL"
    require(terminal is not None, "index: missing terminal record")
    require(offset == pcm_bytes, "index: journal does not cover exact WAV payload")

    count = pcm_bytes // 2
    total = squares = clipped = 0
    low = high = None
    for (value,) in struct.iter_unpack("<h", pcm):
        total += value
        squares += value * value
        clipped += value in (-32768, 32767)
        low = value if low is None else min(low, value)
        high = value if high is None else max(high, value)
    mean = total / count if count else 0.0
    rms = math.sqrt(squares / count) if count else 0.0
    ac_rms = math.sqrt(max(0, squares * count - total * total)) / count if count else 0.0
    return {
        "valid": True, "complete": terminal == "FINAL", "wav_path": str(Path(wav_path)),
        "index_path": str(Path(index_path)), "samples": count, "duration_seconds": count / 16000,
        "min": low, "max": high, "mean": mean, "rms": rms, "ac_rms": ac_rms,
        "clipping_samples": clipped, "clipping_fraction": clipped / count if count else 0.0,
        "wav_sha256": hashlib.sha256(wav_data).hexdigest(),
        "index_sha256": hashlib.sha256(index).hexdigest(), "pcm_sha256": hashlib.sha256(pcm).hexdigest(),
        "boot_id": header[16:32].hex(), "generation": generation, "segment": segment,
        "first_source_sample": first_sample, "next_source_sample": first_sample + count,
        "terminal": terminal, "incident_reason": incident_reason, "records": record_count,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path)
    parser.add_argument("index", type=Path)
    args = parser.parse_args(argv)
    try:
        report = verify_pair(args.wav, args.index)
    except (OSError, ValueError) as error:
        print(json.dumps({"valid": False, "error": str(error)}, ensure_ascii=False))
        return 1
    print(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
