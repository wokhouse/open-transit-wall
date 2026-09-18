#!/usr/bin/env bash
# Build and run the native simulator — renders what the ESP32 would draw.
# Usage: tools/run_sim.sh [vehicles.csv] [out_prefix] [WxH ...]
# Defaults use the live proxy on :8000; e.g.:
#   curl -s localhost:8000/vehicles | tools/vehicles_to_csv.py > /tmp/v.csv
#   tools/run_sim.sh /tmp/v.csv
set -euo pipefail
cd "$(dirname "$0")/.."

CSV="${1:-/tmp/otw_vehicles.csv}"
PREFIX="${2:-build/sim_out}"
shift 2 2>/dev/null || true
SIZES=("$@")
[ ${#SIZES[@]} -eq 0 ] && SIZES=(64x32)

mkdir -p build
for size in "${SIZES[@]}"; do
  w="${size%x*}"; h="${size#*x}"
  inc="build/sim_inc_${w}x${h}"
  mkdir -p "$inc"
  # backdrop for this size, placed where the sim's #include "backdrop.h" finds it
  python3 tools/gen_backdrop.py --w "$w" --h "$h" --out "$inc/backdrop.h" >/dev/null
  bin="build/sim_${w}x${h}"
  clang++ -std=c++17 -O2 -I "$inc" -I firmware/src sim/sim_main.cpp -o "$bin"
  "$bin" "$CSV" "${PREFIX}_${w}x${h}.bmp" "$w" "$h" 10 1 --ascii
done
