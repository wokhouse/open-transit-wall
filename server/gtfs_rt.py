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

Facts probed 2026-10-31 for TriMet (Portland):
- VehiclePositions at developer.trimet.org/ws/V1/VehiclePositions is
  gated behind a free self-service AppID: without one the service answers
  HTTP 403 with the body "A valid appID is required to access this
  resource." The static zip (developer.trimet.org/schedule/gtfs.zip) is
  keyless. The registry entry names the env var ("api_key_env":
  TRIMET_APPID) and the query parameter the key rides under
  ("api_key_param": appID); the proxy appends it per request.
- Static routes.txt verified from the recorded zip: the five MAX lines
  are type 0 with empty route_short_name and bare ids ("90", "100",
  "190", "200", "290") carrying their official route_color; Portland
  Streetcar rides the same feed as type 0; WES is type 2; the Portland
  Aerial Tram is type 6 (classify: cable amber).
- Verified live 2026-10-31 with a registered self-service AppID:
  /ws/V1/VehiclePositions returns GTFS-RT protobuf, so the generic
  parser works unchanged. The deployed poll runs fresh (<20 s age);
  the 34 km viewport carried 392 vehicles (310 bus, 82 across the five
  MAX lines) at 15:40 PT — far under MAX_VEHICLES. Line colors resolve
  through the static routes cache with TriMet's own route_color values.
  An invalid or unregistered AppID gets HTTP 403 with a plain-text body
  on EVERY TriMet web service — including the /ws/gtfs/ and /ws/v2/
  aliases Transitland and the current docs use; neither alias is needed.
"""

from __future__ import annotations

import csv
import io

from google.transit import gtfs_realtime_pb2


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
