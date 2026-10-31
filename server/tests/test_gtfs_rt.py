"""Tests for the gtfs-rt upstream (MTA NYCT: GTFS-RT + static GTFS).

Fixtures are recorded live responses: two division feeds (raw protobuf),
the static feed's routes.txt and stops.txt. No test touches the network.
"""

import sys
from pathlib import Path

import pytest

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
    system: the east edge sits ~2 px beyond the Lefferts and 179 St
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
