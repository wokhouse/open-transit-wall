#!/usr/bin/env python3
"""Convert the proxy's /vehicles JSON (stdin) to the simulator's CSV format:
lat,lon,r,g,b"""

import json
import sys

data = json.load(sys.stdin)
for v in data["v"]:
    # first five fields are lat,lon,r,g,b — the trailing id is ESP32-only
    print(",".join(str(x) for x in v[:5]))
