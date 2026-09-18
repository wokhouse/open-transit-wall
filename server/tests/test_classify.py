import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from classify import COLOR_BUS, COLOR_CABLE, COLOR_RAPID, classify, normalize_line


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


def test_rapid_variants_are_red():
    for line in ("14R", "38R", "9R", "8BX", "30X", "76X"):
        result = classify(line)
        assert result["code"] == "R", line
        assert result["rgb"] == COLOR_RAPID, line


def test_rapid_detection_uses_long_name():
    routes = {"NX": {"type": 3, "long": "N NX Express"}}
    assert classify("NX", routes)["code"] == "R"


def test_metro_uses_line_color_fallback():
    result = classify("N")
    assert result["code"] == "M"
    assert result["rgb"] == (0, 87, 184)  # N Judah fallback blue


def test_metro_prefers_gtfs_color():
    routes = {"N": {"type": 0, "long": "Judah", "color": "12ABCD"}}
    assert classify("N", routes)["rgb"] == (0x12, 0xAB, 0xCD)


def test_kt_throughrun_is_metro():
    assert classify("KT")["code"] == "M"
    assert classify("SF:KT")["code"] == "M"


def test_cable_car_is_amber():
    assert classify("PH")["code"] == "C"
    assert classify("PH")["rgb"] == COLOR_CABLE


def test_unknown_line_defaults_to_bus():
    assert classify("999")["code"] == "B"
    assert classify("")["code"] == "B"
