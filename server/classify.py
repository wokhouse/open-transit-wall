"""Vehicle classification: route -> type (bus/rapid/metro/cable) and LED color.

Written as a dependency-free module so it can be lifted onto a Raspberry Pi
(alternative architecture) or reused in scripts unchanged.

Route metadata comes from the SFMTA GTFS static feed (routes.txt), cached to
routes.json by the proxy at startup. Until that file exists (or when the API
key is missing) the built-in MUNI_FALLBACK table below is used; it is also the
fallback for lines missing from GTFS.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

# --- Palette (also mirrored in firmware/include/config.h and sim) -----------
COLOR_BUS = (0, 64, 255)        # local bus: blue
COLOR_RAPID = (255, 32, 16)     # rapid / express bus: red
COLOR_CABLE = (180, 120, 20)    # cable car: dim amber
# Metro (route_type 0/1) uses the official line color from GTFS route_color,
# falling back to MUNI_FALLBACK below.

# Approximate Muni metro/heritage line colors (hex, no #). GTFS route_color
# overrides these when available.
MUNI_FALLBACK = {
    "J":  {"type": 0, "color": "FFC72C"},  # J Church, yellow
    "K":  {"type": 0, "color": "0089D0"},  # K Ingleside, blue
    "KT": {"type": 0, "color": "00A95C"},  # K/T through-run, green (T leg)
    "T":  {"type": 0, "color": "00A95C"},  # T Third, green
    "L":  {"type": 0, "color": "A05DA5"},  # L Taraval, purple
    "M":  {"type": 0, "color": "00A94F"},  # M Ocean View, green
    "N":  {"type": 0, "color": "0057B8"},  # N Judah, blue
    "F":  {"type": 0, "color": "E51937"},  # F Market heritage streetcar
    "E":  {"type": 0, "color": "6E2639"},  # E Embarcadero heritage streetcar
    "C":  {"type": 5, "color": "B47814"},  # California St cable car
    "PH": {"type": 5, "color": "B47814"},  # Powell-Hyde cable car
    "PM": {"type": 5, "color": "B47814"},  # Powell-Mason cable car
}

METRO_TYPES = {0, 1, 2}
CABLE_TYPES = {5, 7}

# 14R, 9R, 38R, 8BX, 30X, 76X ... (digits then R/RX/BX/X)
RAPID_PATTERN = re.compile(r"^\d{1,3}(R|RX|BX|X)$", re.IGNORECASE)

_TYPE_CODES = {"B": "B", "R": "R", "M": "M", "C": "C"}


def normalize_line(line_ref: str) -> str:
    """'SF:14R' / ' 14r ' -> '14R'; strips agency prefixes from GTFS route_ids."""
    ref = (line_ref or "").strip().upper()
    if ":" in ref:
        ref = ref.split(":", 1)[1]
    return ref


def _hex_to_rgb(hex_color: str) -> tuple[int, int, int]:
    value = (hex_color or "").lstrip("#")
    if not re.fullmatch(r"[0-9A-Fa-f]{6}", value):
        return COLOR_BUS
    return (int(value[0:2], 16), int(value[2:4], 16), int(value[4:6], 16))


def load_routes(path: str | Path) -> dict[str, dict]:
    """Load routes.json written by the proxy's GTFS refresh."""
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def classify(line_ref: str, routes: dict[str, dict] | None = None) -> dict:
    """Classify a vehicle's line.

    Returns {"code": "B"|"R"|"M"|"C", "rgb": (r, g, b), "line": "14R"}.
    """
    line = normalize_line(line_ref)
    info = None
    if routes:
        info = routes.get(line) or routes.get(f"SF:{line}")
    if info is None and line not in MUNI_FALLBACK:
        # GTFS route_ids sometimes carry a suffix/space differences.
        for key, value in (routes or {}).items():
            if normalize_line(key) == line:
                info = value
                break

    fallback = MUNI_FALLBACK.get(line)
    route_type = None
    color_hex = None
    if info:
        route_type = info.get("type")
        color_hex = info.get("color")
    if route_type is None and fallback:
        route_type = fallback["type"]
    if not color_hex and fallback:
        color_hex = fallback["color"]

    if route_type in METRO_TYPES:
        return {"code": "M", "rgb": _hex_to_rgb(color_hex or "FFFFFF"), "line": line}
    if route_type in CABLE_TYPES:
        return {"code": "C", "rgb": COLOR_CABLE, "line": line}

    long_name = str(info.get("long", "")) if info else ""
    if RAPID_PATTERN.match(line) or "RAPID" in long_name.upper() or "EXPRESS" in long_name.upper():
        return {"code": "R", "rgb": COLOR_RAPID, "line": line}
    return {"code": "B", "rgb": COLOR_BUS, "line": line}
