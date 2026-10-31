#!/usr/bin/env python3
"""Convert the mini-proxy's compact /vehicles payload to sim CSV rows.

First line: the payload's bounding box, so the CSV is self-contained the
same way the wire payload is:  #bbox,lat_min,lat_max,lon_min,lon_max
Rows: lat,lon,r,g,b — colors resolved through the payload's "palette" map,
exactly like the firmware does. Rows whose style key is missing from the
palette are skipped, also like the firmware.

Usage: curl -s localhost:8000/vehicles | python3 tools/vehicles_to_csv.py > /tmp/v.csv
"""
import json
import sys

data = json.load(sys.stdin)
bbox = data.get("bbox")
if not isinstance(bbox, list) or len(bbox) != 4 or not all(
        isinstance(x, (int, float)) for x in bbox):
    sys.exit('payload has no numeric 4-element "bbox" — cannot write the header')
print(f"#bbox,{bbox[0]},{bbox[1]},{bbox[2]},{bbox[3]}")
palette = data.get("palette", {})
for lat, lon, style_key, _vid in data["v"]:
    rgb = palette.get(style_key)
    if rgb is None:
        continue
    print(f"{lat},{lon},{rgb[0]},{rgb[1]},{rgb[2]}")
