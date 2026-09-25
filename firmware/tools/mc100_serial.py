"""Bounded MC100 EVT client for an explicitly selected serial endpoint.

No enumeration, alternate-port fallback, format, deletion, or arbitrary commands.
Run with the project's IDF Python (pyserial), not an ambient Python installation.
"""
import argparse
import hashlib
from pathlib import Path
import re
import secrets
import time
import zlib

READY = "MC100_READY"


def valid_filename(name):
    return len(name) < 128 and re.fullmatch(
        r"[0-9a-f]{32}_[0-9]+_[0-9]+(?:\.wav|\.idx|\.partial\.wav|\.recovered\.wav)", name
    ) is not None


def decode_transfer(lines, name, offset, requested):
    markers = [i for i, line in enumerate(lines) if line.startswith("BEGIN ")]
    if len(markers) != 1:
        raise ValueError("Missing or duplicate BEGIN")
    start = markers[0]
    fields = lines[start].split()
    if len(fields) != 4 or fields[1] != name or int(fields[2]) != offset:
        raise ValueError("Read identity mismatch")
    actual = int(fields[3])
    if actual < 0 or actual > requested:
        raise ValueError("Read length exceeds request")
    data = bytearray()
    ended = False
    for line in lines[start + 1:]:
        if line.startswith("END "):
            if not re.fullmatch(r"END [0-9a-fA-F]{8}", line):
                raise ValueError("Malformed CRC")
            if len(data) != actual or int(line[4:], 16) != zlib.crc32(data):
                raise ValueError("Read length or CRC mismatch")
            ended = True
            break
        if not re.fullmatch(r"(?:[0-9a-fA-F]{2}){1,64}", line):
            raise ValueError("Unexpected line inside transfer")
        data.extend(bytes.fromhex(line))
        if len(data) > actual:
            raise ValueError("Excess payload")
    if not ended:
        raise ValueError("Missing END")
    return bytes(data)


class Connection:
    def __init__(self, port):
        self.port = port

    def __enter__(self):
        import serial
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=3)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = self.port
        self.serial.open()
        self.buffer = bytearray()
        # Drain bounded startup/stale output without toggling reset or other ports.
        end = time.monotonic() + 1
        while time.monotonic() < end:
            self.serial.read(min(self.serial.in_waiting + 1, 4096))
        return self

    def __exit__(self, *_):
        self.serial.close()

    def synchronize(self):
        # An echoed nonce is a barrier behind all boot/prior-command output.
        self.command(f"sync {secrets.randbits(32)}", timeout=30)

    def command(self, command, timeout=15):
        operation = command.split()[0]
        terminals = {"status": ("STATUS mode=EVT_USB_BENCH",),
                     "list": ("LIST result=",), "record": ("RECORD ",),
                     "capture": ("CAPTURE ",), "read": ("END ", "READ result=")}
        if operation != "sync" and operation not in terminals:
            raise ValueError("Unsupported command")
        self.serial.write((command + "\n").encode("ascii"))
        deadline = time.monotonic() + timeout
        lines = []
        completed = False
        while time.monotonic() < deadline:
            chunk = self.serial.read(min(max(self.serial.in_waiting, 1), 4096))
            self.buffer.extend(chunk)
            if len(self.buffer) > 16384:
                raise ValueError("Unbounded serial line")
            while b"\n" in self.buffer:
                raw, _, tail = self.buffer.partition(b"\n")
                self.buffer = bytearray(tail)
                line = raw.decode("ascii", errors="replace").rstrip("\r")
                if line:
                    lines.append(line)
                if len(lines) > 10000:
                    raise ValueError("Unbounded serial response")
                if operation == "sync":
                    completed |= line == "SYNC " + command.split()[1]
                else:
                    completed |= line.startswith(terminals[operation]) or line.startswith("ERROR ")
                if line == READY:
                    if completed:
                        return lines
                    lines = []  # Unsolicited boot/stale output is not our reply.
        raise TimeoutError(
            f"Target serial endpoint did not complete {command.split()[0]}: {lines[-5:]}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Explicit target serial endpoint")
    commands = parser.add_subparsers(dest="operation", required=True)
    commands.add_parser("status")
    commands.add_parser("list")
    record = commands.add_parser("record")
    record.add_argument("seconds", type=int, choices=range(3, 601), metavar="3..600")
    capture = commands.add_parser("capture")
    capture.add_argument("seconds", type=int, choices=range(3, 61), metavar="3..60")
    read = commands.add_parser("read")
    read.add_argument("name")
    read.add_argument("offset", type=int)
    read.add_argument("count", type=int)
    read.add_argument("output", type=Path)
    download = commands.add_parser("download")
    download.add_argument("name")
    download.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.operation in ("read", "download"):
        if not valid_filename(args.name):
            parser.error("Only finalized MC100 filenames are accepted")
        root = (Path(__file__).resolve().parents[1] / "out").resolve()
        args.output = args.output.resolve()
        if root not in args.output.parents or args.output.exists():
            parser.error("Output must be a new file below firmware/out")
        if args.operation == "read" and (args.offset < 0 or args.offset > 0xFFFFFFFF or not 1 <= args.count <= 1024):
            parser.error("Read offset must fit FAT32 and count must be 1..1024")
    with Connection(args.port) as connection:
        connection.synchronize()
        # Status handshake also rejects a different or non-EVT firmware.
        status = connection.command("status")
        if not any("STATUS mode=EVT_USB_BENCH" in line for line in status):
            raise RuntimeError("Selected endpoint is not running MC100 EVT bench firmware")
        if args.operation == "status":
            print("\n".join(status))
        elif args.operation == "list":
            print("\n".join(connection.command("list")))
        elif args.operation in ("record", "capture"):
            result = connection.command(f"{args.operation} {args.seconds}", timeout=args.seconds + 90)
            print("\n".join(result), flush=True)
            records = [line for line in result if line.startswith(args.operation.upper() + " ")]
            if len(records) != 1 or "result=0 " not in records[0] or "reset_required=0" not in records[0]:
                raise RuntimeError("Recording did not report clean completion")
        else:
            if args.operation == "download":
                listing = connection.command("list")
                matches = [line.split() for line in listing if line.startswith(f"FILE {args.name} ")]
                if len(matches) != 1 or len(matches[0]) != 3:
                    raise ValueError("File missing or ambiguous")
                remaining = int(matches[0][2])
                if remaining < 0 or remaining > 9600512:
                    raise ValueError("Invalid MC100 file size")
                offset = 0
            else:
                remaining, offset = args.count, args.offset
            args.output.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            total = 0
            # Exclusive creation; a failed transfer remains clearly incomplete.
            partial = args.output.with_name(args.output.name + ".incomplete")
            with partial.open("xb") as file:
                while remaining:
                    count = min(1024, remaining)
                    data = decode_transfer(connection.command(f"read {args.name} {offset} {count}"), args.name, offset, count)
                    if len(data) != count:
                        raise ValueError("Premature EOF; incomplete output retained")
                    file.write(data)
                    digest.update(data)
                    offset += count
                    total += count
                    remaining -= count
                file.flush()
            # Windows rename is no-replace; check also for platforms with replace rename.
            if args.output.exists():
                raise FileExistsError(args.output)
            partial.rename(args.output)
            print(f"VERIFIED bytes={total} sha256={digest.hexdigest()} output={args.output}")


if __name__ == "__main__":
    main()
