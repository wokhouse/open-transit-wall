import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import proxy  # noqa: E402


def test_parse_types_tolerates_formats():
    assert proxy.parse_types("MC") == {"M", "C"}
    assert proxy.parse_types("M,C") == {"M", "C"}
    assert proxy.parse_types(" m , c ") == {"M", "C"}
    assert proxy.parse_types("M,C,X") == {"M", "C"}  # unknown codes ignored
    assert proxy.parse_types("") == set()


def test_compact_filters_by_type(monkeypatch):
    monkeypatch.setattr(proxy, "TYPES", {"M"})
    monkeypatch.setattr(proxy.state, "routes", {})
    raw = [
        {"line": "N", "lat": 37.76, "lon": -122.47, "bearing": 90, "direction": "1", "id": "1"},
        {"line": "38", "lat": 37.78, "lon": -122.46, "bearing": 0, "direction": "0", "id": "2"},
        {"line": "PH", "lat": 37.79, "lon": -122.41, "bearing": 0, "direction": "", "id": "3"},
        {"line": "", "lat": 37.70, "lon": -122.40, "bearing": 0, "direction": "", "id": "4"},
    ]
    out = proxy.compact_vehicles(raw)
    assert [v["r"] for v in out] == ["N"]
    assert out[0]["t"] == "M"


def test_compact_all_types_when_unset(monkeypatch):
    monkeypatch.setattr(proxy, "TYPES", set())
    monkeypatch.setattr(proxy.state, "routes", {})
    raw = [
        {"line": "N", "lat": 37.76, "lon": -122.47, "bearing": 90, "direction": "1", "id": "1"},
        {"line": "38", "lat": 37.78, "lon": -122.46, "bearing": 0, "direction": "0", "id": "2"},
    ]
    assert len(proxy.compact_vehicles(raw)) == 2


def test_to_wire_flattens_for_esp32():
    v = {"r": "N", "la": 37.76, "lo": -122.47, "t": "M", "c": [10, 20, 30]}
    assert proxy.to_wire([v]) == [[37.76, -122.47, 10, 20, 30]]
