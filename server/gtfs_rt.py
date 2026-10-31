"""GTFS-RT upstream for the registry's `gtfs-rt` source (rail-style agencies).

The VehiclePosition records from these real-time feeds (Google GTFS-RT
protobuf) place vehicles one of two ways, by feed: some report
coordinates directly (position.lat/lon), some report only the station a
vehicle is stopped at or heading to (stop_id). This parser accepts both
and prefers coordinates. The static GTFS stops.txt named by the registry
entry supplies station coordinates for the stop-only case, and the proxy
resolves stop_id -> coords per poll. Identity prefers vehicle.id, the
stable fleet number; when a feed omits it the trip id takes over (stable
while a vehicle runs its trip; it changes at terminals and layovers,
where the wall drops and re-creates the dot). Everything agency-specific
— the feed URLs and the static zip — lives in `server/agencies.json`
(`feeds`, `static_gtfs`); this module holds only the parsing.

Facts verified live 2026-10-30 on MTA NYCT (see docs/nyc-mta-feasibility.md;
the eighth feed, SIR, verified live 2026-10-31):
- Its eight polled division feeds (seven subway + the Staten Island
  Railway) update every ~30 s and need no API key. The SIR feed is at
  nyct%2Fgtfs-si — gtfs-sir is NOT a path there: api-endpoint answers
  unknown paths with HTTP 200 and an XML NoSuchKey body, which the
  protobuf parse reports as a corrupt wire format. Its route_id is SI,
  colored from the static routes.txt (#08179C). The %2F path
  encoding is required on api-endpoint.mta.info; a
  literal slash returns 403.
- Every subway VehiclePosition carried a stop_id; none carried lat/lon,
  and none carried vehicle.id — that fleet is placed by station and
  identified by trip id.
- route_id is the bare line name ("1", "6X", "A"), which feeds the
  normal M:<LINE> palette-key path; line colors come from the static
  routes.txt via the proxy's per-agency routes cache.

Facts verified live 2026-10-31 on Metro Transit (see
docs/cta-trimet-metro-feasibility.md):
- The single public vehiclepositions feed needs no key and is fresh
  (header age 11 s). Its records carry position AND stop_id AND a stable
  numeric vehicle.id — position-first placement, fleet-number identity.
- route_id values are bare static GTFS ids ("3", "14", "922") that
  resolve through the routes cache unchanged.

Facts verified live 2026-10-31 on the CTA portal (same doc; the RT field
shape is unconfirmed until a Developer Center key lands — what IS
verified is the gate behavior and the static zip):
- A keyless request to transitdata.transitchicago.com/GtfsRealtime/
  VehiclePositions.pb answers HTTP 200 with an XML error document
  (errCd 100 missing key / 101 invalid key). raise_for_status() is
  blind to it, so parse_rt treats any XML document as a failed poll and
  reports the errCd/errNm it carries. The same guard names MTA's
  NoSuchKey body for an unknown feed path.
- CTA serves ONE VehiclePositions feed mixing buses and 'L' trains.
  The rail-only wall is the registry's `route_types` filter applied by
  the proxy (fleet-cap bound: 3,026 mixed vs 256 rail vs MAX_VEHICLES
  1200) — not parser logic.
"""

from __future__ import annotations

import csv
import io
import re

from google.transit import gtfs_realtime_pb2

_XML_ERROR_RE = re.compile(r"<errCd>(\d+)</errCd>\s*<errNm>(.*?)</errNm>", re.S)


def parse_rt(data: bytes) -> list[dict]:
    """Decode one GTFS-RT VehiclePositions feed into vehicle records.

    Returns [{line, id, lat, lon, stop_id}, ...]. lat/lon are set when
    the feed carries coordinates (an unset position field decodes to
    0/0, treated as no position); stop_id when it carries a station
    reference. A record is skipped if it has no route (it cannot be
    colored) or neither placement (it cannot be placed). lat is None
    for position-less feeds — callers then resolve stop_id through the
    static station table. Identity prefers vehicle.id (it survives trip
    changes); trip_id, then the entity id, are the fallbacks.
    """
    head = data.lstrip(b"\xef\xbb\xbf \r\n\t")
    if head[:5] == b"<?xml":
        # Key-gated portals answer an error as XML inside HTTP 200 (CTA:
        # errCd 100/101; MTA api-endpoint: NoSuchKey for unknown paths).
        # Decoding by content, not header: some feeds serve protobuf as
        # application/octet-stream, so an XML body means failed poll, period.
        match = _XML_ERROR_RE.search(head.decode("utf-8", "replace"))
        detail = f" (errCd {match.group(1)}: {match.group(2)})" if match else ""
        raise ValueError(f"upstream answered with an XML document, not a "
                        f"protobuf feed{detail}")
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.ParseFromString(data)
    out = []
    for entity in feed.entity:
        if not entity.HasField("vehicle"):
            continue
        vp = entity.vehicle
        if not (vp.HasField("trip") and vp.trip.route_id):
            continue
        lat = lon = None
        if vp.HasField("position"):
            if vp.position.latitude and vp.position.longitude:
                lat, lon = vp.position.latitude, vp.position.longitude
        stop_id = getattr(vp, "stop_id", "")
        if lat is None and not stop_id:
            continue
        out.append({
            "line": vp.trip.route_id,
            "id": vp.vehicle.id or vp.trip.trip_id or entity.id,
            "lat": lat,
            "lon": lon,
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
