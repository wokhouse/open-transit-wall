import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from classify import (
    COLOR_BUS,
    COLOR_CABLE,
    COLOR_RAPID,
    MAX_LINE_CHARS,
    MUNI_FALLBACK,
    classify,
    normalize_line,
    style_key,
)

# These tests exercise SFMTA classification, so they pass the SFMTA fallback
# table explicitly — classify() has no agency default anymore.
RT = MUNI_FALLBACK


@pytest.fixture(scope="module")
def fixture_vehicles():
    path = Path(__file__).resolve().parent.parent.parent / "fixtures" / "sample_vehiclemonitor.json"
    return json.loads(path.read_text())


def test_normalize_line():
    assert normalize_line("SF:14R") == "14R"
    assert normalize_line(" 14r ") == "14R"
    assert normalize_line("N") == "N"
    assert normalize_line("") == ""


def test_local_bus_is_blue():
    assert classify("38")["code"] == "B"
    assert classify("38")["rgb"] == COLOR_BUS
    assert classify("38")["key"] == "B"  # non-metro keys stay the bare code


def test_rapid_variants_are_red():
    for line in ("14R", "38R", "9R", "8BX", "30X", "76X"):
        result = classify(line)
        assert result["code"] == "R", line
        assert result["rgb"] == COLOR_RAPID, line
        assert result["key"] == "R", line


def test_rapid_detection_uses_long_name():
    routes = {"NX": {"type": 3, "long": "N NX Express"}}
    assert classify("NX", routes)["code"] == "R"


def test_metro_uses_line_color_fallback():
    result = classify("N", fallback=RT)
    assert result["code"] == "M"
    assert result["rgb"] == (0, 87, 184)  # N Judah fallback blue
    assert result["key"] == "M:N"  # per-line key so each line gets its own color


def test_metro_prefers_gtfs_color():
    routes = {"N": {"type": 0, "long": "Judah", "color": "12ABCD"}}
    result = classify("N", routes)
    assert result["rgb"] == (0x12, 0xAB, 0xCD)
    assert result["key"] == "M:N"


def test_metro_without_any_color_uses_generic_blue():
    routes = {"ZZ": {"type": 2, "long": "Colorless Line"}}
    result = classify("ZZ", routes)
    assert result["key"] == "M:ZZ"
    assert result["rgb"] == (0x00, 0x89, 0xD0)  # METRO_DEFAULT


def test_style_keys_are_agency_safe_and_collision_free():
    assert style_key("B", "47") == "B"
    assert style_key("M", "J") == "M:J"
    # Another agency's line with the same short name is a different payload,
    # and prefixed GTFS ids normalize to the same key rather than colliding
    # with the base codes.
    assert style_key("M", normalize_line("LA:J")) == "M:J"
    assert style_key("M", "B") != "B"
    # Feed-derived LineRefs are untrusted: the key (repeated in every row
    # of that line) is length-bounded before it reaches the ESP32 heap.
    assert style_key("M", "Q" * 200) == "M:" + "Q" * MAX_LINE_CHARS


def test_kt_throughrun_is_metro():
    assert classify("KT", fallback=RT)["code"] == "M"
    assert classify("SF:KT", fallback=RT)["code"] == "M"
    assert classify("SF:KT", fallback=RT)["key"] == "M:KT"


def test_cable_car_is_amber():
    assert classify("PH", fallback=RT)["code"] == "C"
    assert classify("PH", fallback=RT)["rgb"] == COLOR_CABLE
    assert classify("PH", fallback=RT)["key"] == "C"  # cable cars stay generic


def test_unknown_line_defaults_to_bus():
    assert classify("999")["code"] == "B"
    assert classify("")["code"] == "B"
