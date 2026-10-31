"""Tests for the WMATA Metrorail agency (rail-only, keyed gtfs-rt source).

Fixtures are recorded live responses, trimmed: the rail VehiclePositions
probe (2026-10-31 21:08 UTC, 34 of 77 entities, every line present plus two
NR non-revenue rows and one trip-less yard row) and the static routes.txt /
stops.txt side of the keyed rail zip (all 98 parent stations, the platforms
the recorded rows reference). No test touches the network: the keyed static
fetch is exercised against an in-memory zip.

What these pin:
  * the registry contract (keyed source, key query parameter, daily
    allowance, non-revenue exclusion, the chosen frame);
  * position-first placement — this feed reports lat/lon on every row, so
    the station table is never needed to place a vehicle;
  * the NR drop, and the fact that NR would otherwise classify as generic
    bus blue (`B`) because the static GTFS has no such route;
  * the static zip carrying the key (the first agency in this project whose
    static feed is key-gated too);
  * the viewport, which crops exactly the six stations west of
    Wiehle-Reston East and nothing else.
"""

import csv
import io
import json
import math
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gtfs_rt
import proxy
from proxy import compact_vehicles, load_registry, parse_routes_txt, to_wire

FIXTURES = Path(__file__).resolve().parent.parent.parent / "fixtures"
VP = FIXTURES / "wmata_rail_vp.pb"
ROUTES_TXT = FIXTURES / "wmata_rail_routes.txt"
STOPS_TXT = FIXTURES / "wmata_rail_stops.txt"

LINES = {"RED", "BLUE", "GREEN", "YELLOW", "ORANGE", "SILVER"}
# The frame's west edge cuts west of Wiehle-Reston East: the Silver Line's
# Loudoun/Dulles tail. One feed carries the whole rail system, so the crop
# is viewport-only and these names are the measured clip set.
CROPPED_STATIONS = {
    "Reston Town Center", "Herndon", "Innovation Center",
    "Washington Dulles International Airport", "Loudoun Gateway", "Ashburn",
}


@pytest.fixture(scope="module")
def records():
    return gtfs_rt.parse_rt(VP.read_bytes())


@pytest.fixture(scope="module")
def routes():
    return parse_routes_txt(ROUTES_TXT.read_text())


@pytest.fixture(scope="module")
def stations():
    return gtfs_rt.parse_stops(STOPS_TXT.read_text())


@pytest.fixture(scope="module")
def stop_rows():
    return list(csv.DictReader(STOPS_TXT.read_text().splitlines()))


def test_registry_entry_shape():
    """One entry carries both modes: two RT feeds and the two static zips
    the modes ship separately (the Muni shape from one WMATA key)."""
    cfg = load_registry()["wmata"]
    assert cfg["source"] == "gtfs-rt"
    assert cfg["feeds"] == {
        "rail-vehicle-positions":
            "https://api.wmata.com/gtfs/rail-gtfsrt-vehiclepositions.pb",
        "bus-vehicle-positions":
            "https://api.wmata.com/gtfs/bus-gtfsrt-vehiclepositions.pb",
    }
    assert cfg["api_key_env"] == "WMATA_API_KEY"
    assert cfg["api_key_param"] == "api_key"
    assert cfg["requests_per_day_limit"] == 50_000
    assert cfg["static_gtfs"] == [
        "https://api.wmata.com/gtfs/rail-gtfs-static.zip",
        "https://api.wmata.com/gtfs/bus-gtfs-static.zip",
    ]
    assert cfg["drop_route_ids"] == ["NR"]


def test_feed_is_position_first(records, stations):
    """Every recorded row carries coordinates, so nothing snaps to a
    station. The stop ids that are present must still resolve — the table
    is built from the static zip on the same schedule as the routes."""
    assert len(records) > 30
    assert all(r["lat"] is not None and r["lon"] is not None for r in records)
    assert all(r["stop_id"] in stations for r in records if r["stop_id"])


def test_identity_is_the_fleet_number(records):
    """WMATA reports a three-digit train number; it survives a trip change,
    which is what the wall's glide identity wants."""
    assert all(r["id"].isdigit() and len(r["id"]) == 3 for r in records)


def test_routes_are_the_static_line_ids(records):
    assert {r["line"] for r in records} == LINES | {"NR"}


def test_line_colors_come_from_the_static_zip(routes):
    """route_type 1 plus route_color is the whole classify story: the six
    official line colors arrive through the existing M:<LINE> path."""
    expected = {"RED": (0xC8, 0x0F, 0x2D), "BLUE": (0x00, 0x9C, 0xDE),
                "GREEN": (0x00, 0xB1, 0x40), "YELLOW": (0xFF, 0xD1, 0x00),
                "ORANGE": (0xED, 0x8B, 0x00), "SILVER": (0x91, 0x9D, 0x9D)}
    for line, rgb in expected.items():
        info = proxy.classify(line, routes)
        assert info["code"] == "M" and info["key"] == f"M:{line}"
        assert info["rgb"] == rgb


def test_non_revenue_rows_never_reach_the_wire(records, routes):
    """NR is real trains with no passengers: it has no static route, so it
    would classify as generic bus blue. The registry drops it instead. The
    undropped comparison documents both halves of that contract."""
    kept = compact_vehicles(records, routes, None, frozenset({"NR"}))
    assert kept, "the drop must not empty the wall"
    assert all(v["r"] in LINES for v in kept)
    assert all(v["t"].startswith("M:") for v in kept)
    assert "B" not in {v["t"] for v in kept}

    undropped = compact_vehicles(records, routes, None)
    nr = [v for v in undropped if v["r"] == "NR"]
    assert nr, "the fixture must carry NR rows"
    assert {v["t"] for v in nr} == {"B"}
    assert len(undropped) - len(kept) == len(nr)


def test_viewport_crops_exactly_the_loudoun_tail(records, stop_rows):
    """Every recorded row is inside the frame except the Silver Line rows
    that sit west of it, and the only stations outside are the six named
    ones. Nothing else may clip."""
    bbox = load_registry()["wmata"]["bbox"]

    def inside(lat, lon):
        return (bbox["lat_min"] <= lat <= bbox["lat_max"]
                and bbox["lon_min"] <= lon <= bbox["lon_max"])

    outside = [r for r in records if not inside(r["lat"], r["lon"])]
    assert outside, "the fixture must exercise the west-edge crop"
    assert all(r["line"] == "SILVER" for r in outside)
    assert all(r["lon"] < bbox["lon_min"] for r in outside)

    stations = [r for r in stop_rows if r["location_type"] == "1"]
    assert len(stations) == 98
    cropped = {r["stop_name"] for r in stations
               if not inside(float(r["stop_lat"]), float(r["stop_lon"]))}
    assert cropped == CROPPED_STATIONS

    def at(name):
        row = next(r for r in stations if r["stop_name"].startswith(name))
        return float(row["stop_lat"]), float(row["stop_lon"])

    # Served extremes stay in frame, each at least one pixel inside.
    for name in ("Wiehle-Reston East", "Downtown Largo", "Shady Grove",
                 "Franconia-Springfield", "Metro Center", "L'Enfant Plaza"):
        lat, lon = at(name)
        assert inside(lat, lon), f"{name} must stay in frame"
    # And the clipped tail sits beyond the west edge, not beyond another one.
    for name in CROPPED_STATIONS:
        lat, lon = at(name)
        assert lon < bbox["lon_min"]


def test_station_pixels_keep_a_margin(stop_rows):
    """The extremal served stations land on an interior column/row, so their
    dots cannot clip at the frame edge (the projection math mirrors the
    firmware's aspect-fit)."""
    bbox = load_registry()["wmata"]["bbox"]
    w = h = 64
    mid = (bbox["lat_min"] + bbox["lat_max"]) / 2
    cos = math.cos(math.radians(mid))
    scale = min((w - 1) / ((bbox["lon_max"] - bbox["lon_min"]) * cos),
                (h - 1) / (bbox["lat_max"] - bbox["lat_min"]))
    off_x = (w - 1 - (bbox["lon_max"] - bbox["lon_min"]) * cos * scale) / 2
    off_y = (h - 1 - (bbox["lat_max"] - bbox["lat_min"]) * scale) / 2

    def pixel(lat, lon):
        return (int(off_x + (lon - bbox["lon_min"]) * cos * scale),
                int(off_y + (bbox["lat_max"] - lat) * scale))

    row = {r["stop_name"]: r for r in stop_rows if r["location_type"] == "1"}
    for name in ("Wiehle-Reston East", "Downtown Largo", "Shady Grove",
                 "Franconia-Springfield"):
        x, y = pixel(float(row[name]["stop_lat"]), float(row[name]["stop_lon"]))
        assert 0 < x < w - 1 and 0 < y < h - 1, f"{name} is on the frame edge"


def test_wire_shape(records, routes):
    vehicles = compact_vehicles(records, routes, None, frozenset({"NR"}))
    wire = to_wire(vehicles)
    assert all(len(row) == 4 for row in wire)
    assert all(isinstance(row[2], str) and row[2].startswith("M:") for row in wire)
    assert {row[2] for row in wire} <= {f"M:{line}" for line in LINES}
    palette = {v["t"]: v["c"] for v in vehicles}
    assert set(palette) == {row[2] for row in wire}


# --- The keyed static zip (the one fetch path WMATA forced) ------------------

STATIC_KEY_ENV = "OTW_TEST_WMATA_API_KEY"


def _keyed_cfg():
    return {
        "name": "Test WMATA",
        "source": "gtfs-rt",
        "feeds": {"rail": "https://example.invalid/rail.pb"},
        "api_key_env": STATIC_KEY_ENV,
        "api_key_param": "api_key",
        "requests_per_day_limit": 50000,
        "drop_route_ids": ["NR"],
        "static_gtfs": "https://example.invalid/rail-static.zip",
        "bbox": {"lat_min": 0, "lat_max": 1, "lon_min": 0, "lon_max": 1},
    }


class _ZipResponse:
    def __init__(self, payload):
        self.content = payload

    def raise_for_status(self):
        pass


# The RT feeds and the static zips share one response shape here: both are
# fetched through proxy.httpx.get and only .content is read.
_EmptyFeedResponse = _ZipResponse


def _static_zip_bytes():
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr("routes.txt", ROUTES_TXT.read_text())
        archive.writestr("stops.txt", STOPS_TXT.read_text())
    return buffer.getvalue()


def test_static_zip_request_carries_the_key(monkeypatch, tmp_path):
    """WMATA 401s on its static zip as well as its feed. The registry key
    must ride that request too — the other gtfs-rt agencies serve their
    static side keyless, where no parameter is added."""
    monkeypatch.setenv(STATIC_KEY_ENV, "TESTKEY1234")
    state = proxy.AgencyState("wmata_test", _keyed_cfg())
    state.routes_path = tmp_path / "routes_wmata_test.json"
    state.stations_path = tmp_path / "stations_wmata_test.json"
    assert state.feed_params == {"api_key": "TESTKEY1234"}

    seen = []
    payload = _static_zip_bytes()

    def fake_get(url, params=None, **kwargs):
        seen.append((url, params))
        return _ZipResponse(payload)

    monkeypatch.setattr(proxy.httpx, "get", fake_get)
    proxy.refresh_routes(None, state, quiet=True)

    assert seen == [("https://example.invalid/rail-static.zip",
                     {"api_key": "TESTKEY1234"})]
    assert state.routes["RED"]["type"] == 1
    assert state.routes["RED"]["color"] == "C80F2D"
    assert len(state.stations) > 100
    assert state.routes_path.exists() and state.stations_path.exists()


def test_static_zip_sends_no_params_without_a_key(monkeypatch, tmp_path):
    monkeypatch.delenv(STATIC_KEY_ENV, raising=False)
    state = proxy.AgencyState("wmata_test", _keyed_cfg())
    state.routes_path = tmp_path / "routes_wmata_test.json"
    state.stations_path = tmp_path / "stations_wmata_test.json"
    assert state.feed_params == {}

    seen = []
    payload = _static_zip_bytes()

    def fake_get(url, params=None, **kwargs):
        seen.append(params)
        return _ZipResponse(payload)

    monkeypatch.setattr(proxy.httpx, "get", fake_get)
    proxy.refresh_routes(None, state, quiet=True)
    assert seen == [None]


def test_registry_rejects_a_bad_drop_list(tmp_path):
    def load(mutate):
        registry = load_registry()
        mutate(registry["wmata"])
        path = tmp_path / "agencies.json"
        path.write_text(json.dumps(registry))
        with pytest.raises(ValueError):
            load_registry(path)

    load(lambda cfg: cfg.__setitem__("drop_route_ids", "NR"))
    load(lambda cfg: cfg.__setitem__("drop_route_ids", [""]))
    load(lambda cfg: cfg.__setitem__("drop_route_ids", [7]))


# --- The combined entry: rail + bus through one registry entry ---------------
#
# WMATA ships rail and bus as separate real-time feeds and separate static
# zips, so one entry lists two of each. This is the SFMTA/Muni shape: the six
# subway lines key as M:<LINE> and every bus route keys as B on one wall.

BUS_VP = FIXTURES / "wmata_bus_vp.pb"
BUS_ROUTES_TXT = FIXTURES / "wmata_bus_routes.txt"
BUS_STOPS_TXT = FIXTURES / "wmata_bus_stops.txt"

RAIL_STATIC_URL = "https://example.invalid/rail-static.zip"
BUS_STATIC_URL = "https://example.invalid/bus-static.zip"


def _zip_bytes(routes_path, stops_path):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr("routes.txt", Path(routes_path).read_text())
        archive.writestr("stops.txt", Path(stops_path).read_text())
    return buffer.getvalue()


def _combined_cfg():
    cfg = _keyed_cfg()
    cfg["feeds"] = {"rail": "https://example.invalid/rail.pb",
                    "bus": "https://example.invalid/bus.pb"}
    cfg["static_gtfs"] = [RAIL_STATIC_URL, BUS_STATIC_URL]
    return cfg


def test_multi_zip_static_refresh_merges_both_modes(monkeypatch, tmp_path):
    """Both zips ride one refresh, both carry the key, and the merged table
    keeps each mode's route type: RED stays metro (M:RED), A11 stays bus (B).
    Without the rail zip the subway would classify as generic bus blue."""
    monkeypatch.setenv(STATIC_KEY_ENV, "TESTKEY1234")
    state = proxy.AgencyState("wmata_test", _combined_cfg())
    assert state.static_gtfs == (RAIL_STATIC_URL, BUS_STATIC_URL)
    state.routes_path = tmp_path / "routes_wmata_test.json"
    state.stations_path = tmp_path / "stations_wmata_test.json"

    payloads = {RAIL_STATIC_URL: _zip_bytes(ROUTES_TXT, STOPS_TXT),
                BUS_STATIC_URL: _zip_bytes(BUS_ROUTES_TXT, BUS_STOPS_TXT)}
    seen = []

    def fake_get(url, params=None, **kwargs):
        seen.append((url, params))
        return _ZipResponse(payloads[url])

    monkeypatch.setattr(proxy.httpx, "get", fake_get)
    proxy.refresh_routes(None, state, quiet=True)

    assert seen == [(RAIL_STATIC_URL, {"api_key": "TESTKEY1234"}),
                    (BUS_STATIC_URL, {"api_key": "TESTKEY1234"})]
    assert state.routes["RED"]["type"] == 1
    assert state.routes["A11"]["type"] == 3
    assert proxy.classify("RED", state.routes)["key"] == "M:RED"
    assert proxy.classify("A11", state.routes)["key"] == "B"
    # Route ids do not collide across the two feeds, and neither do stops.
    assert set(state.routes) >= {"RED", "BLUE", "SILVER", "A11", "C53"}
    assert {"PF_G05_C", "783"} <= set(state.stations)


def test_combined_snapshot_serves_both_modes(monkeypatch, tmp_path):
    """Two feeds in one entry produce one snapshot: trains and buses together,
    a palette of the six line keys plus the single bus key, and the NR drop
    still applying to the rail half."""
    monkeypatch.setenv(STATIC_KEY_ENV, "TESTKEY1234")
    state = proxy.AgencyState("wmata_test", _combined_cfg())
    state.routes = {**parse_routes_txt(ROUTES_TXT.read_text()),
                    **parse_routes_txt(BUS_ROUTES_TXT.read_text())}
    state.stations = {**gtfs_rt.parse_stops(STOPS_TXT.read_text()),
                      **gtfs_rt.parse_stops(BUS_STOPS_TXT.read_text())}

    payloads = {"https://example.invalid/rail.pb": VP.read_bytes(),
                "https://example.invalid/bus.pb": BUS_VP.read_bytes()}

    def fake_get(url, params=None, **kwargs):
        return _EmptyFeedResponse(payloads[url])

    monkeypatch.setattr(proxy.httpx, "get", fake_get)
    raw = proxy.fetch_station_snapshot(state)

    lines = {r["line"] for r in raw}
    assert {"RED", "BLUE", "GREEN", "YELLOW", "ORANGE", "SILVER"} <= lines
    assert {"A11", "C53"} & lines
    vehicles = compact_vehicles(raw, state.routes, None, frozenset({"NR"}))
    keys = {v["t"] for v in vehicles}
    assert keys == {"M:RED", "M:BLUE", "M:GREEN", "M:YELLOW", "M:ORANGE",
                    "M:SILVER", "B"}
    assert all(v["r"] != "NR" for v in vehicles)


def test_bus_fixture_parses_and_clips_only_named_edges():
    """The bus half of the fixture: every row is position-first and classifies
    to the single bus key, and the only rows outside the shared frame are
    beyond the north and south edges (M22 north of the frame, F1X at the
    south-west corner)."""
    records = gtfs_rt.parse_rt(BUS_VP.read_bytes())
    assert len(records) > 100
    assert all(r["lat"] is not None for r in records)
    routes = parse_routes_txt(BUS_ROUTES_TXT.read_text())
    vehicles = compact_vehicles(records, routes, None)
    assert {v["t"] for v in vehicles} == {"B"}

    bbox = load_registry()["wmata"]["bbox"]

    def inside(v):
        return (bbox["lat_min"] <= v["la"] <= bbox["lat_max"]
                and bbox["lon_min"] <= v["lo"] <= bbox["lon_max"])

    outside = [v for v in vehicles if not inside(v)]
    assert len(outside) == 3
    north = [v for v in outside if v["la"] > bbox["lat_max"]]
    south = [v for v in outside if v["la"] < bbox["lat_min"]]
    assert len(north) == 2 and len(south) == 1
    assert {v["r"] for v in north} == {"M22"}
    assert {v["r"] for v in south} == {"F1X"}


def test_registry_accepts_a_single_static_url(tmp_path):
    """The list form is new; every existing agency still ships one URL and
    the loader normalises it, so nothing else had to change."""
    registry = load_registry()
    registry["wmata"]["static_gtfs"] = RAIL_STATIC_URL
    path = tmp_path / "agencies.json"
    path.write_text(json.dumps(registry))
    assert load_registry(path)["wmata"]["static_gtfs"] == RAIL_STATIC_URL
    state = proxy.AgencyState("one_zip", {**registry["wmata"],
                                          "static_gtfs": RAIL_STATIC_URL})
    assert state.static_gtfs == (RAIL_STATIC_URL,)


def test_registry_rejects_bad_static_lists(tmp_path):
    for bad in ("", [], [""], [3], "example.invalid/x.zip"):
        registry = load_registry()
        registry["wmata"]["static_gtfs"] = bad
        path = tmp_path / "agencies.json"
        path.write_text(json.dumps(registry))
        with pytest.raises(ValueError):
            load_registry(path)
