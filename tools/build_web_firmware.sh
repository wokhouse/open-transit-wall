#!/usr/bin/env bash
# Build the ESP32 firmware, merge it into a single image, and assemble the
# payload the static web flasher serves (web/firmware/).
#
# Requires PlatformIO (`pio`) on PATH. Run from anywhere:
#     tools/build_web_firmware.sh
#
# The merged image is written at offset 0 so the browser writes it in one
# shot; the `cfg` partition (written separately by the page) comes from
# firmware/partitions.csv — see tools/assemble_web.py.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/firmware/.pio/build/esp32dev"
OUT="$ROOT/build/open-transit-wall-merged.bin"

if ! command -v pio >/dev/null 2>&1; then
  echo "error: PlatformIO (pio) not found on PATH — pip install platformio" >&2
  exit 1
fi

echo "==> Building firmware"
(cd "$ROOT/firmware" && pio run -e esp32dev)

for artifact in bootloader.bin partitions.bin firmware.bin; do
  [[ -f "$BUILD_DIR/$artifact" ]] || {
    echo "error: missing $BUILD_DIR/$artifact — did the build succeed?" >&2
    exit 1
  }
done

# boot_app0.bin ships with the Arduino framework package and seeds otadata so
# the bootloader selects ota_0 on first boot.
BOOT_APP0="$(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}" \
  -name boot_app0.bin 2>/dev/null | head -n 1 || true)"
if [[ -z "$BOOT_APP0" ]]; then
  echo "error: boot_app0.bin not found under the PlatformIO packages dir" >&2
  exit 1
fi

mkdir -p "$(dirname "$OUT")"

# Offsets are the ESP32 defaults and must line up with firmware/partitions.csv:
#   0x1000 bootloader · 0x8000 partition table · 0xe000 otadata seed · 0x10000 app
echo "==> Merging flash image"
(cd "$ROOT/firmware" && pio pkg exec -p tool-esptoolpy -- esptool.py \
  --chip esp32 merge_bin \
  -o "$OUT" \
  0x1000 "$BUILD_DIR/bootloader.bin" \
  0x8000 "$BUILD_DIR/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$BUILD_DIR/firmware.bin")

echo "==> Assembling web payload"
python3 "$ROOT/tools/assemble_web.py" --bin "$OUT"

echo
echo "Open web/index.html over http:// (not file://) to flash, e.g.:"
echo "  python3 -m http.server -d \"$ROOT/web\" 8000"