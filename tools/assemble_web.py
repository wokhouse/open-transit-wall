#!/usr/bin/env python3
"""Assemble the static web-flasher payload: copy the merged firmware binary
into web/firmware/ and write the manifest the page reads at load time.

The `cfg` partition offset is parsed out of firmware/partitions.csv so the
table stays the single source of truth — device_config.cpp finds the partition
by label, the flasher writes by offset, and both come from that one file.

    tools/assemble_web.py --bin build/merged.bin
    tools/assemble_web.py --bin build/merged.bin --version v0.2.0
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PARTITIONS_CSV = ROOT / "firmware" / "partitions.csv"
DEFAULT_OUT_DIR = ROOT / "web" / "firmware"
FIRMWARE_FILENAME = "open-transit-wall-merged.bin"

# Path as seen by the browser (index.html lives at the site root, the manifest
# in ./firmware/).
FIRMWARE_URL_PATH = f"./firmware/{FIRMWARE_FILENAME}"


def parse_partitions(path: Path) -> dict[str, tuple[int, int]]:
    """Return {name: (offset, size)} for each row in a PlatformIO CSV table."""
    partitions: dict[str, tuple[int, int]] = {}
    for lineno, raw in enumerate(path.read_text().splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 5:
            raise SystemExit(f"{path}:{lineno}: expected at least 5 columns")
        name = fields[0]
        try:
            offset = int(fields[3], 0)
            size = int(fields[4], 0)
        except ValueError as exc:
            raise SystemExit(f"{path}:{lineno}: bad offset/size: {exc}") from exc
        partitions[name] = (offset, size)
    return partitions


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True, type=Path,
                        help="merged firmware image (from build_web_firmware.sh)")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR,
                        help=f"destination directory (default: {DEFAULT_OUT_DIR})")
    parser.add_argument("--version", default=os.environ.get("GIT_VERSION", "dev"),
                        help="version string for the manifest (default: $GIT_VERSION)")
    parser.add_argument("--partitions", type=Path, default=PARTITIONS_CSV,
                        help=f"partition table to read (default: {PARTITIONS_CSV})")
    args = parser.parse_args(argv)

    merged = args.bin.resolve()
    if not merged.is_file():
        raise SystemExit(f"merged firmware not found: {merged}")

    partitions = parse_partitions(args.partitions)
    if "cfg" not in partitions:
        raise SystemExit(f"{args.partitions}: no `cfg` partition defined")
    cfg_offset, cfg_size = partitions["cfg"]

    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / FIRMWARE_FILENAME
    shutil.copyfile(merged, dest)

    manifest = {
        "name": "open-transit-wall",
        "version": args.version,
        "builtAt": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
        "chipFamily": "ESP32",
        "firmware": {
            "path": FIRMWARE_URL_PATH,
            "flashOffset": 0,
            "size": dest.stat().st_size,
            "sha256": sha256_file(dest),
        },
        "config": {
            "flashOffset": cfg_offset,
            "size": cfg_size,
        },
    }

    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"firmware {dest.stat().st_size} bytes -> {dest}")
    print(f"cfg partition @ 0x{cfg_offset:x} ({cfg_size} bytes)")
    print(f"manifest -> {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))