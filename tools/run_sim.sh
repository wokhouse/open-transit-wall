#!/usr/bin/env bash
# Build and run the native wall simulator against a live or recorded
# /vehicles payload.
#
#   tools/run_sim.sh                 # live: fetch from the local mini-proxy
#   tools/run_sim.sh fixtures/x.json # recorded payload
#   AGENCY=sc tools/run_sim.sh       # pick that agency's backdrop + bbox
#
# The sim renders the compiled backdrop.h entry for AGENCY (default: the
# first entry) using the bbox baked into it — the same bbox the server
# ships on the wire. Regenerate backdrop.h for other agencies with
# tools/gen_backdrop.py --agency <id>.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=sim/sim_main.cpp
OUT=/tmp/otw_sim
CSV=/tmp/otw_vehicles.csv
BMP=/tmp/otw_wall.bmp
AGENCY="${AGENCY:-}"
AGENCY_ARGS=()
[ -n "$AGENCY" ] && AGENCY_ARGS=(--agency "$AGENCY")

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
echo "vehicles: $(wc -l < "$CSV") rows -> $CSV"

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O1 -I firmware/src -o "$OUT" "$SRC"
"$OUT" "$CSV" "$BMP" 64 64 8 1 "${AGENCY_ARGS[@]+"${AGENCY_ARGS[@]}"}"
echo "rendered: $BMP"
command -v open >/dev/null 2>&1 && open "$BMP" || true
