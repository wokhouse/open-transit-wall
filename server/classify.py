"""Vehicle classification: route -> type (bus/rapid/metro/cable) and LED color.

Written as a dependency-free module so it can be lifted onto a Raspberry Pi
(alternative architecture) or reused in scripts unchanged.

Route metadata comes from the agency's GTFS static feed (routes.txt), cached
to routes_<agency>.json by the proxy at startup. The built-in MUNI_FALLBACK
table below is SFMTA-specific and is only consulted when the caller passes it
as `fallback` — other agencies pass nothing.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

# --- Palette (the wire palette in proxy.py is built from these) --------------
COLOR_BUS = (0, 64, 255)        # local bus: blue
COLOR_RAPID = (255, 32, 16)     # rapid / express bus: red
COLOR_CABLE = (180, 120, 20)    # cable car: dim amber
# Metro lines reach the wire under per-line style keys (see style_key) with
# their GTFS route_color (or the MUNI_FALLBACK color below). This generic
# blue is only the last-resort color for a metro line with no color in
# either source.
METRO_DEFAULT = "0089D0"        # generic metro blue, used only as a fallback

MAX_LINE_CHARS = 32            # bound on the feed-derived line in a style key


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


def normalize_line(line_ref: str) -> str:
    """'SF:14R' / ' 14r ' -> '14R'; strips agency prefixes from GTFS route_ids."""
    ref = (line_ref or "").strip().upper()
    if ":" in ref:
        ref = ref.split(":", 1)[1]
    return ref


def style_key(code: str, line: str) -> str:
    """The palette key a vehicle's wire row carries.

    Base codes (B/R/C) stay generic: one color per type. Metro lines get an
    "M:<LINE>" key so each line can carry its own color in the payload's
    palette. Collision-free: the "M:" prefix is disjoint from the base
    codes, and normalize_line folds agency-prefixed refs to one canonical
    line, so each line maps to exactly one key. The line part is capped —
    the key is an untrusted feed string that also repeats in every row of
    that line, and it lands in the ESP32's heap-tight JSON document, so
    its length must be bounded. Aliasing two absurdly long names onto one
    key (shared dot color) is the benign failure mode. Nothing here is
    SFMTA-specific: any agency's metro lines key off their own line refs,
    and registry palette overrides may target any key ("M:J", "B", ...).
    """
    return f"M:{line[:MAX_LINE_CHARS]}" if code == "M" else code


def hex_to_rgb(hex_color: str) -> tuple[int, int, int]:
    """'0089D0' -> (0, 137, 208); '#0089d0' accepted. Anything that isn't
    six hex digits becomes COLOR_BUS."""
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


def classify(line_ref: str, routes: dict[str, dict] | None = None,
             fallback: dict[str, dict] | None = None) -> dict:
    """Classify a vehicle's line.

    `fallback` is an agency-specific table of {line: {type, color}} used when
    GTFS metadata is missing (pass MUNI_FALLBACK for SFMTA, nothing else).
    Returns {"code": "B"|"R"|"M"|"C", "key": <palette key>, "rgb": (r, g, b),
    "line": "14R"}.
    """
    line = normalize_line(line_ref)
    info = None
    if routes:
        info = routes.get(line)
    if info is None and line not in (fallback or {}):
        # GTFS route_ids sometimes carry a suffix/space differences.
        for key, value in (routes or {}).items():
            if normalize_line(key) == line:
                info = value
                break

    fallback = fallback or {}
    fallback_entry = fallback.get(line)
    route_type = None
    color_hex = None
    if info:
        route_type = info.get("type")
        color_hex = info.get("color")
    if route_type is None and fallback_entry:
        route_type = fallback_entry["type"]
    if not color_hex and fallback_entry:
        color_hex = fallback_entry["color"]

    if route_type in METRO_TYPES:
        code = "M"
        rgb = hex_to_rgb(color_hex or METRO_DEFAULT)
    elif route_type in CABLE_TYPES:
        code = "C"
        rgb = COLOR_CABLE
    else:
        long_name = str(info.get("long", "")).upper() if info else ""
        if (RAPID_PATTERN.match(line) or "RAPID" in long_name
                or "EXPRESS" in long_name):
            code = "R"
            rgb = COLOR_RAPID
        else:
            code = "B"
            rgb = COLOR_BUS
    return {"code": code, "key": style_key(code, line), "rgb": rgb, "line": line}
