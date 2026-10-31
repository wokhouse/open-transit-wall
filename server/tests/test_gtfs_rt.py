"""Tests for the gtfs-rt upstream (MTA NYCT, Metro Transit, TriMet).

Fixtures are recorded live responses: division feeds and the Metro
vehicle-positions feed (raw protobuf), and static routes.txt/stops.txt.
Metro is placed by its reported coordinates, so no Metro stops fixture is
needed; the station-snap path is exercised through the MTA fixtures.
TriMet's fixtures are static-side (routes.txt and the rail stops.txt
subset) until the AppID probe records its realtime feed; the keyed-fetch
plumbing is exercised without network. No test touches the network.
"""

import sys
from pathlib import Path

import pytest
from google.transit import gtfs_realtime_pb2

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gtfs_rt
import proxy
from classify import COLOR_CABLE, classify
from proxy import compact_vehicles, load_registry, parse_routes_txt, to_wire

FIXTURES = Path(__file__).resolve().parent.parent.parent / "fixtures"
RT_FEEDS = ["mta_subway_17.pb", "mta_subway_ace.pb", "mta_subway_si.pb"]


@pytest.fixture(scope="module")
def stations():
    return gtfs_rt.parse_stops((FIXTURES / "mta_subway_stops.txt").read_text())


@pytest.fixture(scope="module")
def routes():
    return parse_routes_txt((FIXTURES / "mta_subway_routes.txt").read_text())


def test_registry_entry_shape():
    """The mta entry is a generic gtfs-rt agency: everything upstream-specific
    is registry data (load_registry would have raised at import otherwise)."""
    cfg = load_registry()["mta"]
    assert cfg["source"] == "gtfs-rt"
    assert cfg["feeds"], "a gtfs-rt entry needs at least one feed"
    assert all(url.startswith("https://api-endpoint") for url in cfg["feeds"].values())
    assert cfg["static_gtfs"].startswith("https://")


def test_parse_rt_fixture():
    records = gtfs_rt.parse_rt((FIXTURES / RT_FEEDS[0]).read_bytes())
    assert len(records) > 100
    assert all(r["line"] and r["stop_id"] and r["id"] for r in records)
    # The 1-7 feed must carry numbered lines.
    lines = {r["line"] for r in records}
    assert lines & {"1", "2", "3", "4", "5", "6", "7"}


def test_station_table_usable(stations):
    assert len(stations) > 300
    lat, lon = stations["142S"]  # South Ferry
    assert lat == pytest.approx(40.701, abs=0.01)
    assert lon == pytest.approx(-74.014, abs=0.01)


def test_feed_reports_resolve_into_the_viewport(stations):
    """The station-snap contract: the overwhelming majority of recorded
    reports resolve to a station, and every resolved one is either inside
    the served viewport or beyond one of its two deliberately cropped
    edges — the Riverdale crop clips the Van Cortlandt, Dyre Avenue,
    and 238 St branch tips at the north edge, and the south edge clips
    the outer-SI tail of the Staten Island Railway (records below the
    S21N/S22S pair, e.g. Tottenville). The other two edges contain the
    system: the east edge sits ~14 px beyond the Lefferts and 179 St
    branch tips, and the west edge is west of every station it serves
    (it crosses only SI land west of the S21 line, where no station
    exists). A resolved point outside any other edge would be a
    viewport bug. A small unresolved tail (two station ids missing from
    the static GTFS) is skipped and counted at runtime — see
    fetch_station_snapshot."""
    bbox = load_registry()["mta"]["bbox"]
    total = resolved = clipped_north = clipped_south = 0
    for feed in RT_FEEDS:
        for vp in gtfs_rt.parse_rt((FIXTURES / feed).read_bytes()):
            total += 1
            loc = stations.get(vp["stop_id"])
            if loc is None:
                continue
            resolved += 1
            lat, lon = loc
            if lat > bbox["lat_max"]:
                clipped_north += 1
                continue
            if lat < bbox["lat_min"]:
                clipped_south += 1
                continue
            assert bbox["lat_min"] <= lat
            assert bbox["lon_min"] <= lon <= bbox["lon_max"]
    assert total > 250
    assert resolved / total > 0.90
    # only the branch tips past the two cropped edges may clip
    assert 0 < clipped_north < 0.05 * resolved
    assert 0 < clipped_south < 0.15 * resolved


def test_route_colors_and_style_keys(routes):
    info = classify("A", routes)
    assert info["code"] == "M"
    assert info["key"] == "M:A"
    # The A train carries its GTFS route_color (0062CF blue), not the generic blue.
    assert info["rgb"] == (0x00, 0x62, 0xCF)
    assert classify("6X", routes)["key"] == "M:6X"


def test_compact_wire_rows(stations, routes):
    raw = []
    for vp in gtfs_rt.parse_rt((FIXTURES / RT_FEEDS[1]).read_bytes()):
        loc = stations.get(vp["stop_id"])
        if loc is None:
            continue  # the runtime skip path
        lat, lon = loc
        raw.append({"line": vp["line"], "lat": lat, "lon": lon,
                    "bearing": None, "direction": "", "id": vp["id"]})
    vehicles = compact_vehicles(raw, routes)
    assert len(vehicles) == len(raw)
    assert all(v["t"].startswith("M:") for v in vehicles)
    wire = to_wire(vehicles)
    assert all(len(row) == 4 and row[2].startswith("M:") for row in wire)


# --- Metro Transit (Twin Cities): a position-first feed --------------------
#
# The recorded feed differs from MTA's: every kept record carries real
# coordinates AND a stop_id AND a stable vehicle.id (see
# docs/cta-trimet-metro-feasibility.md). These tests pin that contract so
# a future feed change (or a parser regression) fails loudly.

def test_metro_registry_entry():
    cfg = load_registry()["metro"]
    assert cfg["source"] == "gtfs-rt"
    assert all(url.startswith("https://svc.metrotransit.org/") for url in cfg["feeds"].values())
    assert cfg["static_gtfs"] == "https://svc.metrotransit.org/mtgtfs/gtfs.zip"


def _metro_records():
    return gtfs_rt.parse_rt((FIXTURES / "metro_vehiclepositions.pb").read_bytes())


def test_parse_rt_metro_keeps_geolocated_fleet():
    records = _metro_records()
    assert len(records) >= 40
    assert all(r["line"] and r["id"] and r["lat"] is not None for r in records)


def test_parse_rt_metro_identity_is_the_fleet_number():
    """Output ids come from vehicle.id, never trip_id: one bus keeps one
    dot across trips. Decode the raw feed to compare the id sources."""
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.ParseFromString((FIXTURES / "metro_vehiclepositions.pb").read_bytes())
    vehicle_ids = {e.vehicle.vehicle.id for e in feed.entity
                   if e.HasField("vehicle") and e.vehicle.vehicle.id}
    trip_ids = {e.vehicle.trip.trip_id for e in feed.entity
                if e.HasField("vehicle") and e.vehicle.trip.trip_id}
    ids = {r["id"] for r in _metro_records()}
    assert vehicle_ids and ids <= vehicle_ids
    assert not ids & trip_ids


def test_parse_rt_synthetic_placement_and_identity_matrix():
    """The parse contract per record shape, on a synthetic feed:
    position+stop keeps both (the fetcher places by position), stop-only
    stays for station-snap, position-only is kept (the MTA-era parser
    dropped it), no-placement and no-route are dropped, and vehicle.id
    wins over trip_id even when trip_id is present."""
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.header.timestamp = 1_793_000_000
    feed.header.gtfs_realtime_version = "2.0"

    def add(eid):
        entity = feed.entity.add()
        entity.id = eid
        return entity.vehicle

    vp = add("a")
    vp.trip.route_id, vp.trip.trip_id, vp.stop_id, vp.vehicle.id = "3", "t-a", "S1", "1657"
    vp.position.latitude, vp.position.longitude = 44.95, -93.29
    vp = add("b")
    vp.trip.route_id, vp.trip.trip_id, vp.stop_id = "14", "t-b", "S2"
    vp = add("c")
    vp.trip.route_id, vp.trip.trip_id = "17", "t-c"
    vp.position.latitude, vp.position.longitude = 44.96, -93.28
    vp = add("d")
    vp.trip.route_id, vp.trip.trip_id = "9", "t-d"  # no position, no stop: dropped
    vp = add("e")
    vp.stop_id = "S9"
    vp.position.latitude, vp.position.longitude = 44.9, -93.2  # no route: dropped
    vp = add("f")
    vp.trip.route_id, vp.trip.trip_id, vp.stop_id, vp.vehicle.id = "6", "t-f", "S3", "2201"
    vp.position.latitude = vp.position.longitude = 0.0  # unset position decodes to 0/0

    by = {r["line"]: r for r in gtfs_rt.parse_rt(feed.SerializeToString())}
    assert set(by) == {"3", "14", "17", "6"}
    assert by["3"]["id"] == "1657"  # vehicle.id preferred over trip_id
    assert by["3"]["lat"] == pytest.approx(44.95) and by["3"]["stop_id"] == "S1"
    assert by["14"]["lat"] is None and by["14"]["stop_id"] == "S2"
    assert by["17"]["lat"] == pytest.approx(44.96) and by["17"]["stop_id"] == ""
    # zero/0 position counts as absent: the record stays via its stop_id.
    assert by["6"]["id"] == "2201" and by["6"]["lat"] is None


def test_metro_recorded_positions_inside_the_viewport():
    """The v1 metro bbox was set to contain this whole recording; growing
    it later is a composition decision, shrinking it breaks this contract."""
    bbox = load_registry()["metro"]["bbox"]
    records = _metro_records()
    placed = 0
    for r in records:
        if r["lat"] is None:
            continue
        assert bbox["lat_min"] <= r["lat"] <= bbox["lat_max"]
        assert bbox["lon_min"] <= r["lon"] <= bbox["lon_max"]
        placed += 1
    assert placed >= 40


def test_metro_route_colors_and_style_keys():
    routes = parse_routes_txt((FIXTURES / "metro_routes.txt").read_text())
    blue = classify("901", routes)  # Metro Blue Line, GTFS type 0 (light rail)
    assert blue["code"] == "M" and blue["key"] == "M:901"
    assert blue["rgb"] == (0x00, 0x53, 0x9F)  # the line's own GTFS color
    assert classify("902", routes)["rgb"] == (0x00, 0x81, 0x44)  # Green Line
    assert classify("3", routes)["code"] == "B"  # bus routes stay bus blue


def test_metro_compact_wire_rows():
    """The fetch_station_snapshot placement path for a position-first
    feed: place by reported coordinates, no station lookup needed."""
    routes = parse_routes_txt((FIXTURES / "metro_routes.txt").read_text())
    raw = [{"line": r["line"], "lat": r["lat"], "lon": r["lon"],
            "bearing": None, "direction": "", "id": r["id"]}
           for r in _metro_records() if r["lat"] is not None]
    vehicles = compact_vehicles(raw, routes)
    assert len(vehicles) == len(raw), "every recorded route must resolve in the static routes"
    assert all(v["t"] in ("B", "R", "C") or v["t"].startswith("M:") for v in vehicles)
    assert len({v["i"] for v in vehicles}) == len(vehicles)  # one dot per fleet number
    wire = to_wire(vehicles)
    assert all(len(row) == 4 and isinstance(row[3], int) for row in wire)


# --- TriMet (Portland): a keyed gtfs-rt agency --------------------------------
#
# The realtime feed is gated behind a free self-service AppID (HTTP 403
# "A valid appID is required to access this resource." without one —
# docs/cta-trimet-metro-feasibility.md). The keyed probe records the RT
# fixture once the AppID exists; until then these tests pin the registry
# contract, the key plumbing (offline), the static-side classification,
# and the deliberate viewport crops.
#
# Viewport: the v1 frame is centered on the rail network's mass center at
# the MTA-class ~530 m/px. It clips only named far-suburb tips: the
# Hillsboro end of the Blue/Red lines (west edge), the far-Gresham end of
# the Blue Line (east edge), and the Wilsonville end of WES (south edge;
# Tualatin itself stays in frame). Nothing clips at the north edge (the
# Yellow Line's Rose Quarter terminus is in frame). The assertions below
# encode exactly that measured clip set.

def test_trimet_registry_entry():
    cfg = load_registry()["trimet"]
    assert cfg["source"] == "gtfs-rt"
    assert all(url.startswith("https://developer.trimet.org/")
               for url in cfg["feeds"].values())
    assert cfg["api_key_env"] == "TRIMET_APPID"
    assert cfg["api_key_param"] == "appID"
    assert cfg["requests_per_day_limit"] == 1_000_000
    assert cfg["static_gtfs"] == "https://developer.trimet.org/schedule/gtfs.zip"


TRIMET_KEY_ENV = "OTW_TEST_TRIMET_APPID"


def _keyed_agency_cfg():
    return {
        "name": "Test Keyed Agency",
        "source": "gtfs-rt",
        "feeds": {"vp": "https://example.invalid/VehiclePositions"},
        "api_key_env": TRIMET_KEY_ENV,
        "api_key_param": "appID",
        "requests_per_day_limit": 1000000,
        "static_gtfs": "https://example.invalid/gtfs.zip",
        "bbox": {"lat_min": 0, "lat_max": 1, "lon_min": 0, "lon_max": 1},
    }


class _EmptyFeedResponse:
    content = b""  # parses to a zero-entity FeedMessage

    def raise_for_status(self):
        pass


def test_keyed_feed_appends_the_registered_param(monkeypatch):
    """The whole keyed-fetch contract, offline: the env var is resolved at
    startup, its value rides every feed request under the registry's query
    parameter, and the key is redacted from error strings."""
    monkeypatch.setenv(TRIMET_KEY_ENV, "TESTAPPID777")
    state = proxy.AgencyState("keyed_test", _keyed_agency_cfg())
    assert state.api_key == "TESTAPPID777"
    assert state.feed_params == {"appID": "TESTAPPID777"}

    seen = []

    def fake_get(url, params=None, **kwargs):
        seen.append((url, params))
        return _EmptyFeedResponse()

    monkeypatch.setattr(proxy.httpx, "get", fake_get)
    state.stations = {"S1": (0.5, 0.5)}
    assert proxy.fetch_station_snapshot(state) == []
    assert seen == [("https://example.invalid/VehiclePositions",
                     {"appID": "TESTAPPID777"})]
    # redact() strips the resolved keys of served agencies from any error
    # string (httpx embeds the full URL in HTTPStatusError).
    monkeypatch.setitem(proxy.STATES, "keyed_test", state)
    redacted = proxy.redact(
        "HTTP error '403' ... url='https://example.invalid/VehiclePositions"
        "?appID=TESTAPPID777'")
    assert "TESTAPPID777" not in redacted


def test_keyed_agency_without_its_key_raises_at_the_fetch(monkeypatch):
    """Construction is key-agnostic (a deployment may not serve this
    agency), so the missing key surfaces at the fetch — and main()
    refuses to start a served keyless agency before any thread runs."""
    monkeypatch.delenv(TRIMET_KEY_ENV, raising=False)
    state = proxy.AgencyState("keyed_test", _keyed_agency_cfg())
    assert state.api_key == "" and state.feed_params == {}
    state.stations = {"S1": (0.5, 0.5)}
    with pytest.raises(RuntimeError, match=TRIMET_KEY_ENV):
        proxy.fetch_station_snapshot(state)


def test_unkeyed_feed_sends_no_params(monkeypatch):
    cfg = _keyed_agency_cfg()
    for key in ("api_key_env", "api_key_param", "requests_per_day_limit"):
        cfg.pop(key)
    state = proxy.AgencyState("unkeyed_test", cfg)
    assert state.api_key == "" and state.feed_params == {}

    seen = []

    def fake_get(url, params=None, **kwargs):
        seen.append(params)
        return _EmptyFeedResponse()

    monkeypatch.setattr(proxy.httpx, "get", fake_get)
    state.stations = {"S1": (0.5, 0.5)}
    proxy.fetch_station_snapshot(state)
    assert seen == [None]


def test_registry_rejects_bad_key_fields(tmp_path):
    """load_registry's validation of the new optional fields."""
    import json as _json

    def load(mutate):
        reg = load_registry()
        mutate(reg["trimet"])
        path = tmp_path / "agencies.json"
        path.write_text(_json.dumps(reg))
        with pytest.raises(ValueError):
            load_registry(path)

    def bad_env(cfg):
        cfg["api_key_env"] = "lowercase-name"
    load(bad_env)

    def no_param(cfg):
        cfg.pop("api_key_param")
    load(no_param)

    def bad_limit(cfg):
        cfg["requests_per_day_limit"] = 0
    load(bad_limit)

    def five11_key(cfg):
        cfg2 = load_registry()["sf"] | {"api_key_env": "X_KEY", "api_key_param": "k"}
        reg = load_registry()
        reg["sf"] = cfg2
        path2 = tmp_path / "agencies2.json"
        path2.write_text(_json.dumps(reg))
        with pytest.raises(ValueError):
            load_registry(path2)
    five11_key(None)


def test_trimet_route_colors_and_style_keys():
    routes = parse_routes_txt((FIXTURES / "trimet_routes.txt").read_text())
    red = classify("90", routes)  # MAX Red Line: bare id, empty short name
    assert red["code"] == "M" and red["key"] == "M:90"
    assert red["rgb"] == (0xC4, 0x1F, 0x3E)  # its own GTFS color
    for rid, rgb in [("100", (0x13, 0x59, 0xAE)),  # Blue
                     ("190", (0xFF, 0xC5, 0x2F)),  # Yellow
                     ("200", (0x00, 0x83, 0x42)),  # Green
                     ("290", (0xD0, 0x5F, 0x27))]:  # Orange
        assert classify(rid, routes)["rgb"] == rgb
    wes = classify("203", routes)  # WES commuter rail: GTFS type 2
    assert wes["code"] == "M" and wes["rgb"] == (0x6E, 0x6E, 0x6E)
    tram = classify("208", routes)  # Portland Aerial Tram: type 6 -> cable
    assert tram["code"] == "C" and tram["key"] == "C" and tram["rgb"] == COLOR_CABLE
    streetcar = classify("193", routes)  # PSC NS Line rides as metro type 0
    assert streetcar["code"] == "M" and streetcar["rgb"] == (0x72, 0xA1, 0x30)
    assert classify("12", routes)["code"] == "B"  # plain bus stays bus blue
    assert classify("64", routes)["code"] == "B"  # no '64' express pattern here


def test_trimet_viewport_encodes_only_the_named_crops():
    """Every recorded rail stop is inside the frame or beyond a named
    cropped edge: west = Hillsboro, east = Gresham, south = WES beyond
    Tualatin. The north edge crops nothing."""
    import csv as _csv

    bbox = load_registry()["trimet"]["bbox"]
    rows = list(_csv.DictReader((FIXTURES / "trimet_rail_stops.txt").read_text().splitlines()))
    assert len(rows) >= 200
    west = east = south = north = inside = 0
    for row in rows:
        lat, lon = float(row["stop_lat"]), float(row["stop_lon"])
        if lat > bbox["lat_max"]:
            north += 1
        elif lat < bbox["lat_min"]:
            south += 1
        elif lon < bbox["lon_min"]:
            west += 1
        elif lon > bbox["lon_max"]:
            east += 1
        else:
            inside += 1
    assert north == 0, "the north edge must not clip any rail stop"
    assert inside >= 200
    clipped = {row["stop_name"] for row in rows
               if not (bbox["lat_min"] <= float(row["stop_lat"]) <= bbox["lat_max"]
                       and bbox["lon_min"] <= float(row["stop_lon"]) <= bbox["lon_max"])}
    assert clipped == {  # the measured clip set, exactly
        # west edge: the Hillsboro end of the Blue/Red lines
        "Hatfield Government Center MAX Station", "Hillsboro Central/SE 3rd TC MAX Station",
        "Hillsboro Health District MAX Station", "Hillsboro Airport/Fairgrounds MAX Stn",
        "Orenco MAX Station", "Washington/SE 12th Ave MAX Station",
        "Hawthorn Farm MAX Station",
        # east edge: the far-Gresham end of the Blue Line
        "Gresham City Hall MAX Station", "Gresham Central TC MAX Station",
        "Cleveland Ave MAX Station", "Civic Drive MAX Station",
        # south edge: the Wilsonville end of WES
        "Wilsonville WES Station",
    }, "only the named suburb tips may clip"

    def at(name):
        row = next(r for r in rows if r["stop_name"] == name)
        return float(row["stop_lat"]), float(row["stop_lon"])

    # Downtown anchors: the whole core service must sit inside the frame.
    for name in ("Pioneer Square South MAX Station", "Pioneer Square North MAX Station",
                 "PSU Urban Center/SW 5th & Mill MAX Station",
                 "Union Station/NW 5th & Glisan MAX Stn", "Rose Quarter TC MAX Station",
                 "Portland Int'l Airport MAX Station"):
        lat, lon = at(name)
        assert bbox["lat_min"] <= lat <= bbox["lat_max"]
        assert bbox["lon_min"] <= lon <= bbox["lon_max"]
    # And every clip sits beyond its named edge.
    assert at("Hatfield Government Center MAX Station")[1] < bbox["lon_min"]
    assert at("Gresham City Hall MAX Station")[1] > bbox["lon_max"]
    assert at("Wilsonville WES Station")[0] < bbox["lat_min"]
