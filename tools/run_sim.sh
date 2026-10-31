#!/usr/bin/env bash
# Build and run the native wall simulator against a live or recorded
# /vehicles payload.
#
#   tools/run_sim.sh                 # live: fetch from the local mini-proxy
#   tools/run_sim.sh fixtures/x.json # recorded payload
#   AGENCY=sc tools/run_sim.sh       # that agency's payload (proxy ?agency=)
#
# The sim draws the same vehicles, palette, and bbox the server ships on
# the wire (bbox travels in the CSV header — see tools/vehicles_to_csv.py).
# The map is vehicles on black; there is no backdrop.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=sim/sim_main.cpp
OUT=/tmp/otw_sim
CSV=/tmp/otw_vehicles.csv
BMP=/tmp/otw_wall.bmp
AGENCY="${AGENCY:-}"   # live-fetch only; ignored when a payload file is given

if [ ! -f "$SRC" ]; then
  echo "no simulator source at $SRC" >&2
  exit 1
fi

if [ $# -ge 1 ]; then
  python3 tools/vehicles_to_csv.py < "$1" > "$CSV"
else
  Q=""
  [ -n "$AGENCY" ] && Q="?agency=$AGENCY"
  curl -s "http://localhost:8000/vehicles$Q" | python3 tools/vehicles_to_csv.py > "$CSV"
fi
echo "vehicles: $(($(wc -l < "$CSV") - 1)) rows -> $CSV"

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O1 -I firmware/src -o "$OUT" "$SRC" firmware/src/status_screen.cpp
"$OUT" "$CSV" "$BMP" 64 64 8 1
echo "rendered: $BMP"
command -v open >/dev/null 2>&1 && open "$BMP" || true
