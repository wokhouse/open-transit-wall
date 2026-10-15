import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import proxy

TEST_CFG = {"name": "Test", "siri_agency": "TS",
            "bbox": {"lat_min": 1.0, "lat_max": 2.0, "lon_min": 3.0, "lon_max": 4.0}}


def test_load_registry_accepts_shipped_file():
    registry = proxy.load_registry()
    assert "sf" in registry
    for cfg in registry.values():
        assert cfg["siri_agency"]
        assert set(cfg["bbox"]) == {"lat_min", "lat_max", "lon_min", "lon_max"}


def test_load_registry_rejects_bad_entries(tmp_path):
    path = tmp_path / "agencies.json"
    path.write_text(json.dumps({"TOOLONGID": TEST_CFG}))
    with pytest.raises(ValueError):
        proxy.load_registry(path)

    path.write_text(json.dumps({"ok": {"name": "x", "siri_agency": "X",
                                       "bbox": {"lat_min": 0, "lat_max": 1}}}))
    with pytest.raises(ValueError):
        proxy.load_registry(path)


def test_parse_served_defaults_and_validates():
    registry = {"sf": {}, "ct": {}}
    assert proxy.parse_served("", registry) == ["sf"]
    assert proxy.parse_served(" ct , sf ", registry) == ["ct", "sf"]
    with pytest.raises(ValueError):
        proxy.parse_served("sf,nope", registry)


def test_compact_drops_no_line_but_keeps_all_types():
    raw = [
        {"line": "N", "lat": 37.76, "lon": -122.47, "bearing": 90, "direction": "1", "id": "1"},
        {"line": "38", "lat": 37.78, "lon": -122.46, "bearing": 0, "direction": "0", "id": "2"},
        {"line": "PH", "lat": 37.79, "lon": -122.41, "bearing": 0, "direction": "", "id": "3"},
        {"line": "", "lat": 37.70, "lon": -122.40, "bearing": 0, "direction": "", "id": "4"},
    ]
    out = proxy.compact_vehicles(raw, routes={}, fallback=proxy.MUNI_FALLBACK)
    assert [v["r"] for v in out] == ["N", "38", "PH"]  # only no-line dropped
    assert out[0]["t"] == "M"
    assert "c" not in out[0]  # colors live in the payload palette, not per vehicle


def test_compact_without_fallback_is_agency_safe():
    # A Muni-only fallback line must not leak into another agency's classes.
    out = proxy.compact_vehicles(
        [{"line": "PH", "lat": 1.0, "lon": 2.0, "bearing": 0, "direction": "", "id": "9"}],
        routes={}, fallback=None)
    assert out[0]["t"] == "B"


def test_to_wire_carries_type_code_not_rgb():
    v = {"r": "N", "la": 37.76, "lo": -122.47, "t": "M", "i": 65501}
    assert proxy.to_wire([v]) == [[37.76, -122.47, "M", 65501]]


def test_payload_carries_bbox_next_and_only_present_palette_codes():
    state = proxy.AgencyState("sf", TEST_CFG)
    state.arm_next_poll(125)
    state.update([
        {"r": "38", "la": 1.5, "lo": 3.5, "t": "B", "i": 1},
        {"r": "N", "la": 1.5, "lo": 3.5, "t": "M", "i": 2},
    ])
    payload = state.payload()
    assert payload["bbox"] == [1.0, 2.0, 3.0, 4.0]
    assert 0 <= payload["next"] <= 125
    assert set(payload["palette"]) == {"B", "M"}
    assert payload["palette"]["B"] == proxy.DEFAULT_PALETTE["B"]


def test_registry_palette_override():
    cfg = {**TEST_CFG, "palette": {"B": [1, 2, 3]}}
    state = proxy.AgencyState("xx", cfg)
    state.update([{"r": "1", "la": 0.5, "lo": 0.5, "t": "B", "i": 1}])
    assert state.payload()["palette"] == {"B": [1, 2, 3]}


def test_resolve_agency_single_vs_multi(monkeypatch):
    single = {"sf": proxy.AgencyState("sf", TEST_CFG)}
    monkeypatch.setattr(proxy, "STATES", single)
    assert proxy.resolve_agency(None) is single["sf"]
    assert proxy.resolve_agency("ct") is None

    multi = {"sf": proxy.AgencyState("sf", TEST_CFG),
             "ct": proxy.AgencyState("ct", TEST_CFG)}
    monkeypatch.setattr(proxy, "STATES", multi)
    assert proxy.resolve_agency(None) is None  # ambiguous -> 400
    assert proxy.resolve_agency("ct") is multi["ct"]
