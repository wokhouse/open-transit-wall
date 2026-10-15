#!/usr/bin/env python3
"""Stage the esp32dev firmware images for the web flasher (flasher/).

Runs `pio run -e esp32dev`, then copies the four images the ESP32 ROM
bootloader expects into flasher/firmware/ plus a manifest.json with
offsets, sizes, sha256, and a version string. The GitHub Pages artifact
is the flasher/ directory, so the page and the binaries it offers always
come from the same commit; app.js shows the manifest version and refuses
to flash over a board reporting a newer OTW-FW.

Also writes open-transit-wall-merged.bin (0x0..firmware end, 0xFF padded)
for manual `esptool.py write_flash 0x0 ...` recovery.

Usage:
    python3 tools/build_firmware.py [--version main-abc1234] [--skip-build]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "firmware" / ".pio" / "build" / "esp32dev"
OUT = ROOT / "flasher" / "firmware"

# (source in BUILD, destination name, flash offset). boot_app0.bin ships
# with the Arduino core; the other three are build outputs.
IMAGES = [
    (BUILD / "bootloader.bin", "bootloader.bin", 0x1000),
    (BUILD / "partitions.bin", "partitions.bin", 0x8000),
    (Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32"
     / "tools" / "partitions" / "boot_app0.bin", "boot_app0.bin", 0xe000),
    (BUILD / "firmware.bin", "firmware.bin", 0x10000),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def fw_protocol_version() -> int:
    """OTW_FW_VERSION from the firmware source, so the page can tell a board
    running an older provisioning protocol to flash first."""
    src = (ROOT / "firmware" / "src" / "provisioner.h").read_text()
    m = re.search(r"#define\s+OTW_FW_VERSION\s+(\d+)", src)
    if not m:
        raise SystemExit("OTW_FW_VERSION not found in provisioner.h")
    return int(m.group(1))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="dev",
                    help="version string baked into the manifest (shown in the flasher UI)")
    ap.add_argument("--skip-build", action="store_true",
                    help="stage the existing .pio build instead of rebuilding")
    args = ap.parse_args()

    if not args.skip_build:
        subprocess.run(
            ["pio", "run", "-d", str(ROOT / "firmware"), "-e", "esp32dev"],
            check=True,
        )

    missing = [str(src) for src, _, _ in IMAGES if not src.exists()]
    if missing:
        print("missing build outputs:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 1

    OUT.mkdir(parents=True, exist_ok=True)
    entries = []
    for src, name, offset in IMAGES:
        shutil.copyfile(src, OUT / name)
        staged = OUT / name
        data = staged.read_bytes()
        # Assert the staged copy is byte-identical to the build output.
        assert sha256(staged) == sha256(src), f"staged {name} differs from build"
        entries.append({
            "name": name,
            "offset": offset,
            "size": len(data),
            "sha256": sha256(staged),
            # esptool-js's calculateMD5Hash hook: the page hands this md5 to
            # the loader, which reads the region back and compares.
            "md5": md5(data),
        })

    # Merged image for manual esptool recovery: pad each image up to the
    # next image's offset with 0xFF (erased flash), start at 0x0.
    merged = bytearray()
    for e, (_, _, offset) in zip(entries, IMAGES, strict=True):
        merged.extend(b"\xff" * (offset - len(merged)))
        merged.extend((OUT / e["name"]).read_bytes())
    (OUT / "open-transit-wall-merged.bin").write_bytes(bytes(merged))

    manifest = {
        "version": args.version,
        "built": datetime.now(UTC).isoformat(timespec="seconds"),
        "chip": "esp32",
        "flash_size": "detect",
        "fw_version": fw_protocol_version(),
        "images": entries,
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")

    for e in entries:
        print(f"  0x{e['offset']:05x}  {e['name']:24s} {e['size']:>8d}  {e['sha256'][:12]}")
    print(f"staged {len(entries)} images + merged bin -> {OUT} (version {args.version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
