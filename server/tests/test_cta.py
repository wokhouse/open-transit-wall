"""The CTA 'L'-trains-only agency (added 2026-10-31).

The probe (docs/cta-trimet-metro-feasibility.md) found two CTA-specific
facts the generic gtfs-rt path must handle:

* The portal answers keyless or wrongly-keyed GETs with HTTP 200 + an
  XML error document. raise_for_status never fires, so parse_rt must
  reject the XML and name the CTA error — fixtures are the recorded
  portal responses (errCd 100 missing key; errCd 101 invalid key).
* The one VehiclePositions feed mixes ~2,700 buses with 256 trains:
  only rail fits MAX_VEHICLES, so the registry entry carries
  route_types [1] and compact_vehicles enforces it.

No network, no CTA key.
"""

import sys
from pathlib import Path

import httpx
import pytest
from google.transit import gtfs_realtime_pb2

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gtfs_rt
import proxy
from classify import classify
from proxy import compact_vehicles, load_registry, parse_routes_txt

FIXTURES = Path(__file__).resolve().parent.parent.parent / "fixtures"

# CTA rail ids as the real routes.txt spells them (the fixture excerpt).
RAIL_ROUTES = {"Red": {"short": "", "long": "Red Line", "type": 1, "color": "C60C30"},
               "Blue": {"short": "", "long": "Blue Line", "type": 1, "color": "00A1DE"},
               "1": {"short": "1", "long": "Bronzeville/Union Station", "type": 3,
                     "color": "99999C"}}


@pytest.fixture(scope="module")
def cta_cfg():
    return load_registry()["cta"]


@pytest.fixture(scope="module")
def cta_routes():
    return parse_routes_txt((FIXTURES / "cta_routes.txt").read_text())


# ---------------------------------------------------------------- registry

def test_cta_registry_entry_shape(cta_cfg):
    assert cta_cfg["source"] == "gtfs-rt"
    assert list(cta_cfg["feeds"].values()) == [
        "https://transitdata.transitchicago.com/GtfsRealtime/VehiclePositions.pb"]
    assert cta_cfg["api_key_env"] == "CTA_API_KEY"
    assert cta_cfg["api_key_param"] == "key"
    assert cta_cfg["static_gtfs"] == (
        "https://www.transitchicago.com/downloads/sch_data/google_transit.zip")
    assert cta_cfg["route_types"] == [1], "the wall is 'L' trains only"
    assert cta_cfg["allowance_note"], "the 50k/day allowance must be discoverable"
    bbox = cta_cfg["bbox"]
    assert bbox["lat_min"] < 41.88 < bbox["lat_max"]      # the Loop
    assert bbox["lon_min"] < -87.63 < bbox["lon_max"]
    assert bbox["lat_max"] - 42.024 > 0                   # Purple Skyhorse
    assert bbox["lat_min"] < 41.788 < bbox["lat_max"]    # Red 95th


def test_invalid_route_types_rejected(tmp_path):
    """route_types is registry-validated like every other field."""
    bad = {
        "x": {
            "name": "Bad", "source": "gtfs-rt",
            "feeds": {"vp": "https://example.invalid/vp.pb"},
            "static_gtfs": "https://example.invalid/gtfs.zip",
            "route_types": "1",
            "bbox": {"lat_min": 0, "lat_max": 1, "lon_min": 0, "lon_max": 1},
        }
    }
    path = tmp_path / "agencies.json"
    path.write_text(__import__("json").dumps(bad))
    with pytest.raises(ValueError, match="route_types"):
        load_registry(path)


# ---------------------------------------------------------------- API gate

def test_keyless_xml_error_surfaces_from_parse_rt():
    """The recorded keyless response (HTTP 200, XML body) must not parse as
    zero vehicles — it must raise and name the CTA error."""
    body = (FIXTURES / "cta_error_missing_key.xml").read_bytes()
    with pytest.raises(ValueError, match="errCd 100"):
        gtfs_rt.parse_rt(body)


def test_invalid_key_error_surfaces_from_parse_rt():
    body = (FIXTURES / "cta_error_invalid_key.xml").read_bytes()
    with pytest.raises(ValueError, match="errCd 101"):
        gtfs_rt.parse_rt(body)


def test_protobuf_content_mentioning_xml_is_not_an_error():
    """Regression for the XML sniff: a real protobuf whose payload bytes
    happen to contain the literal '<' 'x' 'm' 'l' must parse fine."""
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.header.gtfs_realtime_version = "2.0"
    entity = feed.entity.add()
    entity.id = "v1"
    entity.vehicle.trip.route_id = "Red<?xml version=\"1.0\""
    entity.vehicle.vehicle.id = "v1"
    entity.vehicle.position.latitude = 41.87
    entity.vehicle.position.longitude = -87.63
    records = gtfs_rt.parse_rt(feed.SerializeToString())
    assert len(records) == 1
    assert records[0]["line"] == "Red<?xml version=\"1.0\""


# ---------------------------------------------------------------- filter

def test_route_type_filter_keeps_only_rail(cta_routes):
    mixed = [
        {"id": "v-red", "lat": 41.87, "lon": -87.63, "line": "Red"},
        {"id": "v-bus", "lat": 41.87, "lon": -87.63, "line": "1"},
        {"id": "v-unknown", "lat": 41.87, "lon": -87.63, "line": "ZZZ"},
    ]
    trains = compact_vehicles(mixed, cta_routes, None, route_types=(1,))
    assert [v["r"] for v in trains] == ["RED"]
    # Unfiltered (the standing rule for every other agency): everything
    # with a route ref survives, unknown routes keep the old fallback.
    everything = compact_vehicles(mixed, cta_routes, None)
    assert sorted(v["r"] for v in everything) == ["1", "RED", "ZZZ"]


def test_classify_reports_the_static_route_type(cta_routes):
    assert classify("Red", cta_routes)["type"] == 1
    assert classify("1", cta_routes)["type"] == 3
    assert classify("ZZZ", cta_routes)["type"] is None


# ---------------------------------------------------------------- key wiring

def _vp_feed() -> bytes:
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.header.gtfs_realtime_version = "2.0"
    entity = feed.entity.add()
    entity.id = "v1"
    entity.vehicle.trip.route_id = "Red"
    entity.vehicle.vehicle.id = "v1"
    entity.vehicle.position.latitude = 41.87
    entity.vehicle.position.longitude = -87.63
    return feed.SerializeToString()


def test_key_gated_fetch_needs_the_env_var(cta_cfg, monkeypatch):
    monkeypatch.delenv("CTA_API_KEY", raising=False)
    state = proxy.AgencyState("cta", cta_cfg)
    state.stations = {"anything": (41.87, -87.63)}
    with pytest.raises(RuntimeError, match="CTA_API_KEY"):
        proxy.fetch_station_snapshot(state)


def test_key_travels_as_the_query_param_and_stays_redacted(cta_cfg, monkeypatch):
    monkeypatch.setenv("CTA_API_KEY", "SECRET-CTA-KEY")
    state = proxy.AgencyState("cta", cta_cfg)
    state.stations = {"anything": (41.87, -87.63)}
    seen = {}

    class Response:
        content = _vp_feed()

        def raise_for_status(self):
            pass

    def fake_get(url, params=None, **kwargs):
        seen["url"] = url
        seen["params"] = params
        return Response()

    monkeypatch.setattr(httpx, "get", fake_get)
    vehicles = proxy.fetch_station_snapshot(state)
    assert len(vehicles) == 1
    assert seen["params"] == {"key": "SECRET-CTA-KEY"}
    assert "SECRET-CTA-KEY" not in proxy.redact(
        f"client error for {seen['url']}?key=SECRET-CTA-KEY")
