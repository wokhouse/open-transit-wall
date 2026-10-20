#!/usr/bin/env python3
"""Stage the firmware images for every supported board into the web flasher.

Runs `pio run -e <env>` for each board in BOARDS, then copies the four
images the ESP32-S3 ROM bootloader expects into
flasher/firmware/<board-id>/ plus a manifest.json listing, per board, the
offsets, sizes, sha256, and a version string. The GitHub Pages artifact is
the flasher/ directory, so the page and the binaries it offers always come
from the same commit; app.js shows the manifest version and picks the
image set for the board the user selects.

Also writes open-transit-wall-merged.bin (0x0..firmware end, 0xFF padded)
per board for manual `esptool.py write_flash 0x0 ...` recovery.

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
FW = ROOT / "firmware"
OUT = ROOT / "flasher" / "firmware"

# One entry per flasher board preset. `id` is the flasher dropdown value
# and the staging directory; it must match the <select> in flasher/index.html.
# `variant` marks UF2-style boards (Adafruit): the Arduino core ships a
# prebuilt bootloader for the variant instead of building one into the
# build dir, and the tinyuf2 app at 0x410000 keeps drag-and-drop working.
BOARDS = [
    {
        "id": "s3rgbmatrix",
        "env": "s3rgbmatrix",
        "name": "Waveshare ESP32-S3-RGB-Matrix",
        # ESP32-S3-WROOM-1-N32R16
        "flash_bytes": 32 * 1024 * 1024,
    },
    {
        "id": "matrixportal-s3",
        "env": "matrixportal-s3",
        "name": "Adafruit MatrixPortal S3",
        # ESP32-S3-PICO-1-N8R8
        "flash_bytes": 8 * 1024 * 1024,
        "variant": "adafruit_matrixportal_esp32s3",
    },
]

# The Arduino core package the pinned platform installed (same path CI uses).
CORE = Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32"

# boot_app0.bin ships with the Arduino core; the other three are build
# outputs. ESP32-S3 maps the bootloader at 0x0 (classic ESP32 used 0x1000).
BOOT_APP0 = CORE / "tools" / "partitions" / "boot_app0.bin"


def image_plan(build_dir: Path, board: dict):
    """The image set, mirroring what `pio run -t upload` flashes for this
    board (verified against `pio run -t envdump` FLASH_EXTRA_IMAGES)."""
    if "variant" in board:
        bootloader = CORE / "variants" / board["variant"] / "bootloader-tinyuf2.bin"
    else:
        bootloader = build_dir / "bootloader.bin"
    plan = [
        (bootloader, "bootloader.bin", 0x0),
        (build_dir / "partitions.bin", "partitions.bin", 0x8000),
        (BOOT_APP0, "boot_app0.bin", 0xe000),
        (build_dir / "firmware.bin", "firmware.bin", 0x10000),
    ]
    if "variant" in board:
        plan.append((CORE / "variants" / board["variant"] / "tinyuf2.bin",
                     "tinyuf2.bin", 0x410000))
    return plan


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def fw_protocol_version() -> int:
    """OTW_FW_VERSION from the firmware source, so the page can tell a board
    running an older provisioning protocol to flash first."""
    src = (FW / "src" / "provisioner.h").read_text()
    m = re.search(r"#define\s+OTW_FW_VERSION\s+(\d+)", src)
    if not m:
        raise SystemExit("OTW_FW_VERSION not found in provisioner.h")
    return int(m.group(1))


def stage_board(board: dict, version: str) -> dict:
    """Stage one board's images under OUT/<id>/ and return its manifest entry."""
    build_dir = FW / ".pio" / "build" / board["env"]
    out = OUT / board["id"]

    plan = image_plan(build_dir, board)
    missing = [str(src) for src, _, _ in plan if not src.exists()]
    if missing:
        print(f"{board['id']}: missing build outputs:\n  " + "\n  ".join(missing),
              file=sys.stderr)
        raise SystemExit(1)

    out.mkdir(parents=True, exist_ok=True)
    entries = []
    for src, name, offset in plan:
        shutil.copyfile(src, out / name)
        staged = out / name
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
    for e, (_, _, offset) in zip(entries, plan, strict=True):
        merged.extend(b"\xff" * (offset - len(merged)))
        merged.extend((out / e["name"]).read_bytes())
    (out / "open-transit-wall-merged.bin").write_bytes(bytes(merged))

    return {
        "id": board["id"],
        "name": board["name"],
        "chip": "esp32s3",
        # 'keep': the images come from PlatformIO with a valid flash-size
        # header for this board, so esptool-js must not probe the flash RDID
        # nor rewrite the header. The page does its own fit check against
        # flash_bytes.
        "flash_size": "keep",
        "flash_bytes": board["flash_bytes"],
        "images": entries,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="dev",
                    help="version string baked into the manifest (shown in the flasher UI)")
    ap.add_argument("--skip-build", action="store_true",
                    help="stage the existing .pio build instead of rebuilding")
    args = ap.parse_args()

    if not args.skip_build:
        for board in BOARDS:
            subprocess.run(
                ["pio", "run", "-d", str(FW), "-e", board["env"]],
                check=True,
            )

    manifest = {
        "version": args.version,
        "built": datetime.now(UTC).isoformat(timespec="seconds"),
        "fw_version": fw_protocol_version(),
        "boards": [stage_board(b, args.version) for b in BOARDS],
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")

    for b in manifest["boards"]:
        print(f"\n{b['id']} ({b['name']}):")
        for e in b["images"]:
            print(f"  0x{e['offset']:05x}  {e['name']:24s} {e['size']:>8d}  {e['sha256'][:12]}")
    print(f"\nstaged {len(BOARDS)} boards -> {OUT} (version {args.version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
