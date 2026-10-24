#!/usr/bin/env python3
"""Convert the mini-proxy's compact /vehicles payload to sim CSV rows.

Rows: lat,lon,r,g,b — colors resolved through the payload's "palette" map,
exactly like the firmware does. Rows whose style key is missing from the
palette are skipped, also like the firmware.

Usage: curl -s localhost:8000/vehicles | python3 tools/vehicles_to_csv.py > /tmp/v.csv
"""
import json
import sys

data = json.load(sys.stdin)
palette = data.get("palette", {})
for lat, lon, style_key, _vid in data["v"]:
    rgb = palette.get(style_key)
    if rgb is None:
        continue
    print(f"{lat},{lon},{rgb[0]},{rgb[1]},{rgb[2]}")
