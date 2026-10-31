"""GTFS-RT upstream for the registry's `gtfs-rt` source (rail-style agencies).

The VehiclePosition records from these real-time feeds (Google GTFS-RT
protobuf) report the station each vehicle is stopped at or heading to
(stop_id) rather than coordinates: the static GTFS stops.txt named by the
registry entry supplies station coordinates, and the proxy resolves
stop_id -> coords per poll. Trip identity rides the vehicle id so the
wall's glide animation interpolates station-to-station. Everything
agency-specific — the feed URLs and the static zip — lives in
`server/agencies.json` (`feeds`, `static_gtfs`); this module holds only
the parsing.

Facts verified live 2026-10-30 on MTA NYCT (see docs/nyc-mta-feasibility.md):
- Its seven polled division feeds update every ~30 s and need no API key.
  (SIR is deliberately unpolled there: the mta viewport excludes Staten
  Island.) The %2F path encoding is required on api-endpoint.mta.info; a
  literal slash returns 403.
- Every subway VehiclePosition carried a stop_id; none carried lat/lon.
- route_id is the bare line name ("1", "6X", "A"), which feeds the
  normal M:<LINE> palette-key path; line colors come from the static
  routes.txt via the proxy's per-agency routes cache.
"""

from __future__ import annotations

import csv
import io

from google.transit import gtfs_realtime_pb2


def parse_rt(data: bytes) -> list[dict]:
    """Decode one GTFS-RT feed into subway vehicle records.

    Returns [{line, id, stop_id}, ...]. Records without a route or a
    station reference are skipped: they cannot be colored or placed.
    The id is the trip id (stable while a train runs its trip; it
    changes at terminals, where the wall drops and re-creates the dot).
    """
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.ParseFromString(data)
    out = []
    for entity in feed.entity:
        if not entity.HasField("vehicle"):
            continue
        vp = entity.vehicle
        if not (vp.HasField("trip") and vp.trip.route_id):
            continue
        stop_id = getattr(vp, "stop_id", "")
        if not stop_id:
            continue
        out.append({
            "line": vp.trip.route_id,
            "id": (vp.trip.trip_id if vp.trip.trip_id else entity.id),
            "stop_id": stop_id,
        })
    return out


def parse_stops(stops_txt: str) -> dict[str, tuple[float, float]]:
    """stop_id -> (lat, lon) from a GTFS stops.txt."""
    stops: dict[str, tuple[float, float]] = {}
    for row in csv.DictReader(io.StringIO(stops_txt)):
        try:
            stops[row["stop_id"]] = (float(row["stop_lat"]), float(row["stop_lon"]))
        except (KeyError, TypeError, ValueError):
            continue
    return stops
