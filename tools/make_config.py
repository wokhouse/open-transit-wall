#!/usr/bin/env python3
"""Build a `cfg` partition blob outside the browser — handy for scripted
provisioning or when you want to write settings with esptool directly:

    tools/make_config.py --ssid HomeNet --password hunter2 \\
        --host 192.168.1.50 --port 8000 -o /tmp/config.bin
    esptool.py write_flash 0x190000 /tmp/config.bin

The layout matches firmware/include/device_config.h and web/app.js. The `cfg`
offset is looked up in firmware/partitions.csv, so don't hardcode it.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from assemble_web import parse_partitions  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PARTITIONS_CSV = ROOT / "firmware" / "partitions.csv"

MAGIC = 0x4F545743  # "OTWC"
VERSION = 1
SSID_LEN, PASS_LEN, HOST_LEN = 33, 65, 64


def fixed(value: str, length: int, field: str) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) > length - 1:
        raise SystemExit(f"{field} too long (max {length - 1} bytes as UTF-8)")
    return raw.ljust(length, b"\x00")


def build_blob(ssid: str, password: str, host: str, port: int) -> bytes:
    payload = (
        fixed(ssid, SSID_LEN, "SSID")
        + fixed(password, PASS_LEN, "password")
        + fixed(host, HOST_LEN, "host")
        + struct.pack("<HH", port, 0)
    )
    header = struct.pack(
        "<IHHII", MAGIC, VERSION, len(payload), zlib.crc32(payload), 0
    )
    return header + payload


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ssid", required=True)
    parser.add_argument("--password", default="")
    parser.add_argument("--host", required=True, help="proxy hostname or IP")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("-o", "--out", type=Path, required=True)
    args = parser.parse_args(argv)

    if not 1 <= args.port <= 65535:
        raise SystemExit("port must be 1..65535")

    blob = build_blob(args.ssid, args.password, args.host, args.port)
    args.out.write_bytes(blob)

    cfg = parse_partitions(PARTITIONS_CSV)["cfg"]
    print(f"wrote {len(blob)} bytes -> {args.out}")
    print(f"write with: esptool.py write_flash 0x{cfg[0]:x} {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))