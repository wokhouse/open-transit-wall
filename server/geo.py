"""Shared geography for the transit wall: SF bounding box and a coarse land polygon.

Used by the proxy (served at /geo for the browser preview) and by
tools/gen_backdrop.py (rasterized into the firmware backdrop). One source of
truth so the preview, simulator, and LED matrix all show the same map.

The polygon is deliberately coarse (~170 m per pixel at 64x32, so precision
beyond this is invisible). Coordinates are (lat, lon).
"""

# Bounding box around the SFMTA metro service area (live fleet extents from
# 2026-09: N Ocean Beach .. T Sunnydale / Wharves), padded ~1-2 px so the data
# sits just inside the edges. Must match firmware/include/config.h MAP_*.
BBOX = {
    "lat_min": 37.705,
    "lat_max": 37.810,
    "lon_min": -122.512,
    "lon_max": -122.385,
}

# Coarse SF peninsula shoreline, traced clockwise starting at the Golden Gate.
SF_LAND = [
    (37.810, -122.477),  # Golden Gate / Fort Point
    (37.807, -122.481),
    (37.805, -122.490),  # Baker Beach
    (37.796, -122.501),
    (37.786, -122.506),  # Lands End
    (37.775, -122.510),
    (37.760, -122.511),  # Ocean Beach
    (37.745, -122.510),
    (37.730, -122.511),
    (37.718, -122.508),  # Fort Funston
    (37.708, -122.503),
    (37.706, -122.497),  # southern edge (Daly City line)
    (37.708, -122.483),
    (37.709, -122.470),  # SE corner of city
    (37.712, -122.453),
    (37.712, -122.443),  # Brisbane line
    (37.718, -122.433),  # Candlestick Point
    (37.722, -122.421),
    (37.727, -122.406),  # Hunters Point
    (37.732, -122.394),
    (37.738, -122.390),  # India Basin
    (37.744, -122.391),
    (37.752, -122.392),  # Mission Bay
    (37.762, -122.391),
    (37.770, -122.390),  # China Basin / McCovey
    (37.778, -122.392),
    (37.786, -122.392),  # Embarcadero
    (37.794, -122.394),
    (37.800, -122.397),
    (37.806, -122.404),  # Fisherman's Wharf
    (37.808, -122.412),
    (37.810, -122.422),  # Marina
    (37.810, -122.437),
    (37.807, -122.446),
    (37.806, -122.454),  # Crissy Field
    (37.808, -122.465),
]

# Palette indices shared with the firmware backdrop (see gen_backdrop.py).
LAND_RGB = (2, 2, 2)
WATER_RGB = (0, 0, 0)
