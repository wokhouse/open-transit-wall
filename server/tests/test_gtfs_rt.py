"""Tests for the gtfs-rt upstream (MTA NYCT, Metro Transit, and the TTC).

Fixtures are recorded live responses: division feeds and the Metro
vehicle-positions feed (raw protobuf), and static routes.txt/stops.txt.
Metro is placed by its reported coordinates, so no Metro stops fixture is
needed; the station-snap path is exercised through the MTA fixtures.
The TTC fixture re-encodes a subset of one recorded feed response
(header preserved; all 185 then-live streetcar rows, 30 sampled buses,
two rows beyond each cropping edge, and the one route-less entity the
probe contained) — see docs/ttc-toronto-feasibility.md.
No test touches the network.
"""

import sys
from pathlib import Path

import pytest
from google.transit import gtfs_realtime_pb2

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gtfs_rt
from classify import classify
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


# --- TTC (Toronto): surface-only and position-first, with no API key at
#     any endpoint (docs/ttc-toronto-feasibility.md) ---

def test_ttc_registry_entry():
    cfg = load_registry()["ttc"]
    assert cfg["source"] == "gtfs-rt"
    assert all(url.startswith("https://gtfsrt.ttc.ca/") for url in cfg["feeds"].values())
    # the city's CKAN: the ttc.ca/open_data mirror of this resource 404s
    assert cfg["static_gtfs"].startswith("https://ckan0.cf.opendata.inter.prod-toronto.ca/")


def _ttc_records():
    return gtfs_rt.parse_rt((FIXTURES / "ttc_vehiclepositions.pb").read_bytes())


def test_parse_rt_ttc_keeps_geolocated_fleet():
    records = _ttc_records()
    assert len(records) >= 200
    assert all(r["line"] and r["id"] and r["lat"] is not None for r in records)
    # the recorded response carried exactly one entity with no trip and no
    # route: a surface vehicle outside any recorded trip. parse_rt drops it.
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.ParseFromString((FIXTURES / "ttc_vehiclepositions.pb").read_bytes())
    routeless = [e for e in feed.entity
                 if e.HasField("vehicle")
                 and not (e.vehicle.trip.trip_id or e.vehicle.trip.route_id)]
    assert len(routeless) == 1
    assert len(records) == len(feed.entity) - len(routeless)


def test_parse_rt_ttc_identity_is_the_fleet_number():
    """Output ids come from vehicle.id, never trip_id: one TTC car keeps
    one dot across trips. Decode the raw feed to compare the id sources.
    The ids are plain car numbers (<= 6 digits)."""
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.ParseFromString((FIXTURES / "ttc_vehiclepositions.pb").read_bytes())
    vehicle_ids = {e.vehicle.vehicle.id for e in feed.entity
                   if e.HasField("vehicle") and e.vehicle.vehicle.id}
    trip_ids = {e.vehicle.trip.trip_id for e in feed.entity
                if e.HasField("vehicle") and e.vehicle.trip.trip_id}
    ids = {r["id"] for r in _ttc_records()}
    assert len(ids) >= 200
    assert ids <= vehicle_ids
    assert not (ids & trip_ids)
    assert all(i.isdigit() and len(i) <= 6 for i in ids)


def test_ttc_frame_holds_the_streetcar_network_inside():
    """The shipped bbox is a 23 km true square centered on the recorded
    streetcar span: the streetcar network sizes the frame, and it must
    stay wholly inside. The bus network reaches past three named edges
    (over the full 1,212-row recording: north 392 rows up to 51 px
    beyond, west 65 up to 21 px, east 43 up to 8 px); no recorded row
    is south of the south edge at all. Those rows still count against
    MAX_VEHICLES on the wall; the frame only hides them (see the doc)."""
    bbox = load_registry()["ttc"]["bbox"]
    routes = parse_routes_txt((FIXTURES / "ttc_routes.txt").read_text())
    km_per_px = 23.0 / 64
    out = {"N": 0, "S": 0, "W": 0, "E": 0}
    far = {"N": 0.0, "W": 0.0, "E": 0.0}
    streetcars = kept = 0
    for r in _ttc_records():
        kept += 1
        if routes.get(r["line"], {}).get("type") == 0:  # streetcar
            streetcars += 1
            assert bbox["lat_min"] < r["lat"] < bbox["lat_max"]
            assert bbox["lon_min"] < r["lon"] < bbox["lon_max"]
            continue
        if r["lat"] > bbox["lat_max"]:
            out["N"] += 1
            far["N"] = max(far["N"], (r["lat"] - bbox["lat_max"]) * 111.32)
        elif r["lat"] < bbox["lat_min"]:
            out["S"] += 1
        elif r["lon"] < bbox["lon_min"]:
            out["W"] += 1
            far["W"] = max(far["W"], (bbox["lon_min"] - r["lon"]) * 80.6)
        elif r["lon"] > bbox["lon_max"]:
            out["E"] += 1
            far["E"] = max(far["E"], (r["lon"] - bbox["lon_max"]) * 80.6)
    assert kept >= 200
    assert streetcars >= 180
    assert out["S"] == 0
    assert out["N"] and out["W"] and out["E"]
    assert far["N"] <= 55 * km_per_px
    assert far["W"] <= 25 * km_per_px
    assert far["E"] <= 12 * km_per_px


def test_ttc_route_colors_and_style_keys():
    routes = parse_routes_txt((FIXTURES / "ttc_routes.txt").read_text())
    # 505: a GTFS type 0 route with its own published color, so it takes
    # the metro-style code like the subway and the Twin Cities lines.
    q = classify("505", routes)
    assert q["code"] == "M" and q["key"] == "M:505"
    assert q["rgb"] == (0xED, 0x1C, 0x24)
    # 905/927: buses whose static long names carry "EXPRESS" -> red
    assert classify("905", routes)["key"] == "R"
    assert classify("927", routes)["key"] == "R"
    # 29: an ordinary bus
    assert classify("29", routes)["code"] == "B"


def test_ttc_compact_wire_rows_including_rt_only_ids():
    """Full compact pass: position-first placement, no station snap.
    Route ids 124 and 200-203 appear in the live vehicle feed but in
    neither published static package (measured 2026-10-31); classify
    treats an unknown id as an ordinary bus, so a positionally known
    vehicle stays on the wall as a generic row instead of vanishing."""
    routes = parse_routes_txt((FIXTURES / "ttc_routes.txt").read_text())
    raw = [{"line": r["line"], "lat": r["lat"], "lon": r["lon"],
            "bearing": None, "direction": "", "id": r["id"]}
           for r in _ttc_records()]
    vehicles = compact_vehicles(raw, routes)
    assert len(vehicles) == len(raw)
    by = {v["r"]: v for v in vehicles}
    assert by["200"]["t"] == "B" and by["201"]["t"] == "B"
    assert by["505"]["t"] == "M:505" and by["501"]["t"] == "M:501"
    wire = to_wire(vehicles)
    assert all(len(row) == 4 and row[3] >= 0 for row in wire)
