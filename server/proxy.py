"""open-transit-wall mini-proxy.

Serves one or more transit agencies from server/agencies.json (the registry).
Each served agency is polled from its upstream on its own timer (POLL_SECONDS
per agency): 511 member agencies from the 511 SIRI VehicleMonitoring feed,
registry entries with `"source": "gtfs-rt"` (rail agencies, e.g. NYC subway)
from the public GTFS-RT feeds + static GTFS zip named in their registry
entry. Each agency is served as a compact vehicle list for the ESP32
display. Which agencies a deployment runs is set by SERVED_AGENCIES; the
payload carries the agency's bbox, refresh cadence, and vehicle-color
palette (per-line for metro) so clients are configured entirely from the
wire.

Usage:
  python proxy.py --fixture ../fixtures/sample_vehiclemonitor.json   # offline
  python proxy.py --record    # live fetch, also save raw responses to fixtures/
  python proxy.py             # live fetch

Configuration via environment (or server/.env):
  FIVE11_API_KEY    511 open-data token (required for live mode)
  SERVED_AGENCIES   comma-separated registry ids to run (default: sf)
  POLL_SECONDS      upstream poll interval per agency (default 65, minimum 1). The
                    operator picks a value that keeps
                    len(SERVED_AGENCIES) * 3600/POLL_SECONDS under the token
                    cap (511's default is 60 req/hour); the server warns at
                    startup if it would exceed that, but does not enforce.
  HOST / PORT       bind address (default 0.0.0.0:8000)
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import sys
import threading
import time
import zipfile
import zlib
from datetime import UTC, datetime
from pathlib import Path

import gtfs_rt
import httpx
import uvicorn
from classify import (
    COLOR_BUS,
    COLOR_CABLE,
    COLOR_RAPID,
    MUNI_FALLBACK,
    classify,
    load_routes,
)
from dotenv import load_dotenv
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from siri import parse_vehiclemonitor

SERVER_DIR = Path(__file__).resolve().parent
load_dotenv(SERVER_DIR / ".env")

ROOT = SERVER_DIR.parent
FIXTURES_DIR = ROOT / "fixtures"
AGENCIES_JSON = SERVER_DIR / "agencies.json"
UPSTREAM_URL = "https://api.511.org/transit/VehicleMonitoring"
DATAFEED_URL = "https://api.511.org/transit/datafeeds"
OPERATORS_URL = "https://api.511.org/transit/operators"

# 511's default token allowance; only used to warn at startup, never enforced.
DEFAULT_TOKEN_RPH = 60

# Default generic per-type colors shipped on the wire, built from classify's
# color constants so the two can't drift. Metro is NOT here: each metro line
# carries its own color under an "M:<LINE>" style key (classify.style_key),
# resolved from GTFS route_color per agency. Registry entries may override
# any key per agency ("palette": {"B": [...], "M:J": [...]} in
# agencies.json) — e.g. Caltrain ships its own colors.
DEFAULT_PALETTE = {
    "B": list(COLOR_BUS),
    "R": list(COLOR_RAPID),
    "C": list(COLOR_CABLE),
}

AGENCY_ID_RE = re.compile(r"^[a-z0-9]{1,8}$")


def redact(text: str) -> str:
    """Strip the API key from an error/log string before printing.

    httpx's HTTPStatusError embeds the full request URL, and 511 only
    accepts the key as a query param (header auth 401s), so every upstream
    4xx/5xx — including expected 429s — would otherwise log the key."""
    api_key = os.environ.get("FIVE11_API_KEY")
    if api_key:
        text = text.replace(api_key, "[api-key]")
    return text


def load_registry(path: Path = AGENCIES_JSON) -> dict[str, dict]:
    """Read agencies.json and sanity-check every entry before anything starts."""
    registry = json.loads(path.read_text())
    for agency_id, cfg in registry.items():
        if not AGENCY_ID_RE.match(agency_id):
            raise ValueError(f"registry id {agency_id!r} must match {AGENCY_ID_RE.pattern}")
        source = cfg.get("source", "511")
        if source not in ("511", "gtfs-rt"):
            raise ValueError(f"registry entry {agency_id!r} has unknown source {source!r}")
        required = ("name", "bbox") if source != "511" else ("name", "siri_agency", "bbox")
        for key in required:
            if key not in cfg:
                raise ValueError(f"registry entry {agency_id!r} missing {key!r}")
        if source == "gtfs-rt":
            feeds = cfg.get("feeds")
            if (not isinstance(feeds, dict) or not feeds
                    or not all(isinstance(url, str) and url.startswith("https://")
                              for url in feeds.values())):
                raise ValueError(f"registry entry {agency_id!r} needs a non-empty "
                                 "'feeds' object of https URLs")
            static = cfg.get("static_gtfs")
            if not isinstance(static, str) or not static.startswith("http"):
                raise ValueError(f"registry entry {agency_id!r} needs a 'static_gtfs' URL")
        for key in ("lat_min", "lat_max", "lon_min", "lon_max"):
            if not isinstance(cfg["bbox"].get(key), (int, float)):
                raise ValueError(f"registry entry {agency_id!r} bbox.{key} missing or not a number")
        palette = cfg.get("palette")
        if palette is not None:
            if not isinstance(palette, dict):
                raise ValueError(f"registry entry {agency_id!r} 'palette' must be an object")
            for style_key, rgb in palette.items():
                if (not isinstance(rgb, list) or len(rgb) != 3
                        or not all(isinstance(c, int) and 0 <= c <= 255 for c in rgb)):
                    raise ValueError(
                        f"registry entry {agency_id!r} palette[{style_key!r}] "
                        "must be a list of 3 ints in 0-255")
    return registry


def parse_served(value: str, registry: dict[str, dict]) -> list[str]:
    """SERVED_AGENCIES -> validated, deduplicated list of registry ids.

    Duplicates would start one poll thread per entry against the same
    AgencyState, silently doubling the upstream rate the operator's
    token math assumes."""
    ids = list(dict.fromkeys(t.strip() for t in value.split(",") if t.strip()))
    if not ids:
        ids = ["sf"]  # the historical single-agency deployment
    unknown = [agency_id for agency_id in ids if agency_id not in registry]
    if unknown:
        raise ValueError(f"SERVED_AGENCIES names unknown agencies {unknown}; "
                         f"registry has {sorted(registry)}")
    return ids


class AgencyState:
    """Per-agency latest vehicle list + poll schedule, shared between the
    poll thread and handlers."""

    def __init__(self, agency_id: str, cfg: dict) -> None:
        self.id = agency_id
        self.name = cfg["name"]
        self.source = cfg.get("source", "511")
        self.siri_agency = cfg.get("siri_agency", "")
        # gtfs-rt: the per-agency upstream endpoints named by the registry.
        self.feeds = dict(cfg.get("feeds") or {})
        self.static_gtfs = cfg.get("static_gtfs", "")
        self.bbox = [cfg["bbox"]["lat_min"], cfg["bbox"]["lat_max"],
                     cfg["bbox"]["lon_min"], cfg["bbox"]["lon_max"]]
        self.palette = {**DEFAULT_PALETTE, **(cfg.get("palette") or {})}
        # SFMTA keeps its offline line table; other agencies rely on GTFS.
        self.fallback = MUNI_FALLBACK if agency_id == "sf" else None
        self.routes_path = SERVER_DIR / f"routes_{agency_id}.json"
        # gtfs-rt: VehiclePositions report station ids, not coords;
        # this table (from the static GTFS stops.txt) resolves them.
        self.stations_path = SERVER_DIR / f"stations_{agency_id}.json"
        self.stations: dict[str, tuple[float, float]] = self._load_stations()
        self._lock = threading.Lock()
        self._vehicles: list[dict] = []
        self._fetched_at: float = 0.0
        self._next_fetch_at: float = 0.0
        self.routes: dict[str, dict] = load_routes(self.routes_path)

    def _load_stations(self) -> dict[str, tuple[float, float]]:
        try:
            with open(self.stations_path, encoding="utf-8") as fh:
                return {k: (lat, lon) for k, (lat, lon) in json.load(fh).items()}
        except (OSError, ValueError):
            return {}

    def update(self, vehicles: list[dict]) -> None:
        with self._lock:
            self._vehicles = vehicles
            self._fetched_at = time.time()

    def arm_next_poll(self, interval: float) -> None:
        with self._lock:
            self._next_fetch_at = time.time() + interval

    def payload(self) -> dict:
        with self._lock:
            now = time.time()
            # Palette keys are exactly the style keys the current rows
            # carry: generic type codes plus "M:<LINE>" per-line metro
            # colors. A registry override wins; otherwise the classified
            # color rides on the cached vehicle. A key with neither ships
            # no entry — the firmware then drops those rows rather than
            # inventing a color.
            palette: dict[str, list] = {}
            for v in self._vehicles:
                key = v["t"]
                if key in palette:
                    continue
                rgb = self.palette.get(key) or v.get("c")
                if rgb:
                    palette[key] = rgb
            return {
                "ts": datetime.fromtimestamp(self._fetched_at, tz=UTC).isoformat(),
                "age": round(now - self._fetched_at, 1),
                "next": max(0, round(self._next_fetch_at - now)),
                "count": len(self._vehicles),
                "bbox": self.bbox,
                "palette": palette,
                "v": self._vehicles,
            }

    def age(self) -> float:
        with self._lock:
            return time.time() - self._fetched_at

    @property
    def fetched(self) -> bool:
        """False until the first successful upstream poll."""
        with self._lock:
            return self._fetched_at > 0

    def ts(self) -> str:
        with self._lock:
            return datetime.fromtimestamp(self._fetched_at, tz=UTC).isoformat()


REGISTRY = load_registry()
SERVED = parse_served(os.environ.get("SERVED_AGENCIES", ""), REGISTRY)
STATES: dict[str, AgencyState] = {
    agency_id: AgencyState(agency_id, REGISTRY[agency_id]) for agency_id in SERVED
}


def compact_vehicles(raw_vehicles: list[dict], routes: dict[str, dict],
                     fallback: dict[str, dict] | None = None) -> list[dict]:
    """Map parsed SIRI entries to the cached vehicle dicts.

    Vehicles without a route reference (yards / deadheading, ~20% of the feed)
    are dropped — they'd plot as misclassified dots clustered at the depots.
    Refs that normalize away to nothing (e.g. 'SF:') are dropped by the same
    rule: they would produce a degenerate 'M:' palette key.
    No type filtering: every active vehicle the agency reports is served.
    """
    out = []
    for vehicle in raw_vehicles:
        if not vehicle["line"]:
            continue
        info = classify(vehicle["line"], routes, fallback)
        if not info["line"]:
            continue
        out.append({
            "r": info["line"],
            "la": round(vehicle["lat"], 5),
            "lo": round(vehicle["lon"], 5),
            # Style key, not the raw type code: metro rows carry an
            # "M:<LINE>" key so each line gets its own palette entry.
            "t": info["key"],
            # Classified color, server-side only — palette material. The
            # wire never carries per-vehicle color (see to_wire).
            "c": list(info["rgb"]),
            # Stable id so the ESP32 can match vehicles across polls and
            # animate each one's movement individually. 31 bits keeps the
            # collision rate negligible at this fleet size (~2^-6 per day).
            "i": zlib.crc32(vehicle["id"].encode()) & 0x7FFFFFFF,
        })
    return out


def to_wire(vehicles: list[dict]) -> list[list]:
    """Project cached vehicles to the ESP32 wire format: flat
    [lat, lon, style_key, id] arrays. Keyed objects overflowed the ESP32's
    heap at rush-hour fleet sizes — every JSON key costs ~16 bytes of
    parse-time heap next to the display framebuffer, so /vehicles ships
    arrays only. Colors are not per-vehicle: the payload's palette maps
    style keys (type codes, plus "M:<LINE>" per metro line) to RGB, so a
    row carries one short string instead of three ints."""
    return [[v["la"], v["lo"], v["t"], v["i"]] for v in vehicles]


# --- Upstream fetching -------------------------------------------------------

def fetch_live(api_key: str, siri_agency: str) -> dict:
    response = httpx.get(
        UPSTREAM_URL,
        params={"api_key": api_key, "agency": siri_agency, "format": "json"},
        timeout=20.0,
        follow_redirects=True,
    )
    response.raise_for_status()
    # 511 sometimes prefixes JSON responses with whitespace/BOM.
    return json.loads(response.text.lstrip("\ufeff \t\r\n"))


def parse_routes_txt(routes_txt: str) -> dict[str, dict]:
    """Parse a GTFS routes.txt into the per-agency route metadata cache.

    Parsed with the csv module on purpose: MTA route descriptions carry
    commas inside quoted fields, which breaks naive line splitting.
    """
    rows = [row for row in csv.reader(io.StringIO(routes_txt)) if row]
    header = [cell.strip() for cell in rows[0]]
    idx = {name: header.index(name) for name in
          ("route_id", "route_short_name", "route_long_name", "route_type")}
    color_idx = header.index("route_color") if "route_color" in header else None
    text_idx = header.index("route_text_color") if "route_text_color" in header else None

    routes: dict[str, dict] = {}
    for cells in rows[1:]:
        if len(cells) <= max(idx.values()):
            continue
        route_id = cells[idx["route_id"]].strip()
        short = cells[idx["route_short_name"]].strip().upper() or route_id.upper()
        entry = {
            "type": int(float(cells[idx["route_type"]].strip() or 3)),
            "long": cells[idx["route_long_name"]].strip(),
        }
        if color_idx is not None and len(cells) > color_idx and cells[color_idx].strip():
            entry["color"] = cells[color_idx].strip().lstrip("#").upper()
        if text_idx is not None and len(cells) > text_idx:
            entry["text"] = cells[text_idx].strip()
        routes[route_id] = entry
        routes[short] = entry
    return routes


def refresh_routes(api_key: str | None, state: AgencyState, quiet: bool = False) -> None:
    """Download the agency's GTFS static feed and cache route types/colors.

    The upstream differs by source: 511 datafeeds (keyed) for member
    agencies, and for gtfs-rt agencies the public static GTFS zip named
    by their registry entry — which also supplies the station coordinate
    table the RT feeds lack. quiet=True
    logs to stderr so callers that print a payload on stdout (--once)
    keep their output clean.
    """
    log = (lambda msg: print(msg, file=sys.stderr)) if quiet else print
    if state.source == "gtfs-rt":
        response = httpx.get(state.static_gtfs, timeout=120.0, follow_redirects=True)
        response.raise_for_status()
        with zipfile.ZipFile(io.BytesIO(response.content)) as archive:
            routes = parse_routes_txt(archive.read("routes.txt").decode("utf-8-sig"))
            stations = gtfs_rt.parse_stops(archive.read("stops.txt").decode("utf-8-sig"))
        state.routes_path.write_text(json.dumps(routes, indent=0))
        state.routes = routes
        state.stations_path.write_text(json.dumps({k: list(v) for k, v in stations.items()}))
        state.stations = stations
        log(f"[routes] cached {len(routes)//2} {state.name} routes + "
            f"{len(stations)} stations")
        return

    if not api_key:
        raise RuntimeError("FIVE11_API_KEY not set (use --fixture for offline dev)")
    response = httpx.get(
        DATAFEED_URL,
        params={"api_key": api_key, "operator_id": state.siri_agency,
                "status": "active"},
        timeout=60.0,
        follow_redirects=True,
    )
    response.raise_for_status()
    with zipfile.ZipFile(io.BytesIO(response.content)) as archive:
        routes_txt = archive.read("routes.txt").decode("utf-8-sig")
    routes = parse_routes_txt(routes_txt)

    state.routes_path.write_text(json.dumps(routes, indent=0))
    state.routes = routes
    print(f"[routes] cached {len(routes)//2} {state.name} routes -> {state.routes_path.name}",
          flush=True)


def check_operator_codes(api_key: str) -> None:
    """Warn (never block) when a registry siri_agency is missing from 511's
    live operator list — the authoritative source for accepted agency codes.
    Only 511-source agencies have a siri_agency to check."""
    if not any(state.source == "511" for state in STATES.values()):
        return
    try:
        response = httpx.get(OPERATORS_URL, params={"api_key": api_key},
                             timeout=30.0, follow_redirects=True)
        response.raise_for_status()
        operators = json.loads(response.content.decode("utf-8-sig"))
        known = {op.get("Id") for op in operators}
    except Exception as error:
        print(f"[registry] operator list unavailable ({redact(str(error))}); "
              "skipping check", file=sys.stderr)
        return
    for state in STATES.values():
        if state.source != "511":
            continue
        if state.siri_agency not in known:
            print(f"[registry] WARNING: {state.id} uses siri_agency "
                  f"{state.siri_agency!r}, which is not in 511's operator list",
                  file=sys.stderr)


def fetch_station_snapshot(state: AgencyState) -> list[dict]:
    """Poll one gtfs-rt agency's feeds and place every vehicle.

    Returns vehicles in the siri.parse_vehiclemonitor shape so the shared
    compact_vehicles/classify path applies. Position-first: feeds that
    report coordinates (Metro Transit) place the vehicle where it is.
    Feeds that report only a station id (MTA subway) are placed at that
    station from the static table (the wall's glide then animates the
    hop between polls). Reports whose station id is missing from the
    static table are skipped and counted — that mismatch means the
    routes cache needs refreshing.
    """
    if not state.stations:
        raise RuntimeError(f"{state.id}: no station table (first routes refresh failed?)")
    vehicles = []
    missing = 0
    for url in state.feeds.values():
        response = httpx.get(url, timeout=20.0, follow_redirects=True)
        response.raise_for_status()
        for vp in gtfs_rt.parse_rt(response.content):
            if vp["lat"] is not None:
                lat, lon = vp["lat"], vp["lon"]
            else:
                loc = state.stations.get(vp["stop_id"])
                if loc is None:
                    missing += 1
                    continue
                lat, lon = loc
            vehicles.append({"line": vp["line"], "lat": lat, "lon": lon,
                             "bearing": None, "direction": "", "id": vp["id"]})
    if missing:
        print(f"[poll:{state.id}] NOTE: skipped {missing} reports with unknown "
              "station ids (refresh the static GTFS cache)", file=sys.stderr)
    return vehicles


# Static GTFS (route types/colors, station coords) is (re)fetched on the
# first poll cycle and every ROUTES_REFRESH_SECONDS after: agency timetables
# change, and no per-agency derived table should stay pinned in git or in
# process memory for the life of the container. One zip fetch per agency per
# day is negligible against the 511 60 req/hr budget; a failed refresh keeps
# the current tables and retries after ROUTES_RETRY_SECONDS.
ROUTES_REFRESH_SECONDS = 24 * 3600
ROUTES_RETRY_SECONDS = 600


def poll_loop(state: AgencyState, api_key: str | None, fixture: Path | None,
              record: bool, interval: float, start_delay: float = 0.0) -> None:
    """Poll one agency on a fixed schedule.

    Slots are absolute (start + k*interval), so the advertised `next` and the
    operator's token math stay true even when a fetch is slow; a fetch that
    overruns its slot skips to the next one instead of shifting the whole
    schedule. start_delay staggers agencies so N threads don't stampede 511
    in the same second. The static tables refresh on the first cycle and then
    daily (see ROUTES_REFRESH_SECONDS); a failed refresh keeps the current
    tables rather than degrading a live agency."""
    if start_delay > 0:
        time.sleep(start_delay)
    next_slot = time.monotonic()
    next_routes_refresh = 0.0  # the first cycle always refreshes
    while True:
        next_slot += interval
        try:
            due = time.monotonic() >= next_routes_refresh
            if due and (api_key or state.source != "511"):
                try:
                    refresh_routes(api_key, state)
                except Exception as error:
                    print(f"[routes] refresh failed ({redact(str(error))}); "
                          "keeping current tables", file=sys.stderr)
                    next_routes_refresh = time.monotonic() + ROUTES_RETRY_SECONDS
                else:
                    next_routes_refresh = time.monotonic() + ROUTES_REFRESH_SECONDS

            if state.source == "gtfs-rt":
                raw_vehicles = fetch_station_snapshot(state)
            elif fixture is not None:
                raw_vehicles = parse_vehiclemonitor(json.loads(fixture.read_text()))
            elif api_key:
                payload = fetch_live(api_key, state.siri_agency)
                if record:
                    FIXTURES_DIR.mkdir(exist_ok=True)
                    stamp = datetime.now().astimezone().strftime("%Y%m%d_%H%M%S")
                    (FIXTURES_DIR / f"vehiclemonitor_{state.id}_{stamp}.json").write_text(
                        json.dumps(payload, indent=1))
                raw_vehicles = parse_vehiclemonitor(payload)
            else:
                raise RuntimeError("FIVE11_API_KEY not set (use --fixture for offline dev)")

            vehicles = compact_vehicles(raw_vehicles, state.routes, state.fallback)
            state.update(vehicles)
            print(f"[poll:{state.id}] {len(vehicles)} vehicles at {state.ts()}",
                  flush=True)
        except Exception as error:
            print(f"[poll:{state.id}] error: {redact(str(error))}", file=sys.stderr)
        state.arm_next_poll(interval)
        time.sleep(max(0.0, next_slot - time.monotonic()))


# --- HTTP surface ------------------------------------------------------------

app = FastAPI(title="open-transit-wall proxy")

# The GitHub-Pages web flasher (flasher/) fetches /agencies from a different
# origin. Everything the proxy serves is read-only public data, so any
# origin may GET it.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET"],
    allow_headers=["*"],
)


def resolve_agency(agency: str | None) -> AgencyState | None:
    """Map the ?agency= param to a served agency. No param is only allowed
    when the deployment serves exactly one agency — old firmware and
    single-agency self-hosters keep working."""
    if agency is None:
        if len(STATES) == 1:
            return next(iter(STATES.values()))
        return None
    return STATES.get(agency)


def wire_payload(state: AgencyState) -> dict:
    """The exact JSON /vehicles ships: state payload with vehicle rows
    projected to the wire format."""
    payload = state.payload()
    payload["v"] = to_wire(payload["v"])
    return payload


@app.get("/agencies")
def agencies() -> dict:
    """Static discovery for the WebSerial flasher: what this server runs."""
    return {"agencies": [{"id": state.id, "name": state.name}
                         for state in STATES.values()]}


@app.get("/vehicles")
def vehicles(agency: str | None = None) -> JSONResponse:
    state = resolve_agency(agency)
    if state is None:
        wanted = agency[:32] if agency else "(no agency param)"
        return JSONResponse(
            {"error": f"unknown agency {wanted!r}; this server serves "
                      f"{sorted(STATES)}"},
            status_code=400, headers={"Cache-Control": "no-store"})
    payload = wire_payload(state)
    return JSONResponse(payload, headers={"Cache-Control": "no-store"})


@app.get("/health")
def health() -> dict:
    return {
        "serving": sorted(STATES),
        "upstream_age_s": {agency_id: (None if not state.fetched
                                       else round(state.age(), 1))
                           for agency_id, state in STATES.items()},
        "routes_cached": {agency_id: bool(state.routes)
                          for agency_id, state in STATES.items()},
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=None,
                        help="serve vehicles from a recorded SIRI JSON file instead of 511")
    parser.add_argument("--record", action="store_true",
                        help="save raw live responses to fixtures/")
    parser.add_argument("--once", action="store_true",
                        help="fetch once, print the exact /vehicles wire payload, exit")
    parser.add_argument("--agency", default=None,
                        help="registry id for --once (default: the first served agency)")
    args = parser.parse_args()

    api_key = os.environ.get("FIVE11_API_KEY")
    try:
        interval = float(os.environ.get("POLL_SECONDS", "65"))
    except ValueError:
        parser.error("POLL_SECONDS must be a number")
        return
    if interval < 1:
        parser.error("POLL_SECONDS must be >= 1")
        return

    if args.once:
        agency_id = args.agency or SERVED[0]
        if agency_id not in STATES:
            parser.error(f"--agency {agency_id!r} is not served "
                         f"(SERVED_AGENCIES={','.join(SERVED)})")
            return
        state = STATES[agency_id]
        if state.source == "gtfs-rt":
            try:
                refresh_routes(None, state, quiet=True)
            except Exception as error:
                print(f"[routes] refresh failed ({redact(str(error))}); "
                      "using cached tables", file=sys.stderr)
            raw_vehicles = fetch_station_snapshot(state)
        elif args.fixture:
            payload = json.loads(args.fixture.read_text())
            raw_vehicles = parse_vehiclemonitor(payload)
        elif api_key:
            payload = fetch_live(api_key, state.siri_agency)
            raw_vehicles = parse_vehiclemonitor(payload)
        else:
            parser.error("--once needs FIVE11_API_KEY or --fixture")
            return
        state.update(compact_vehicles(raw_vehicles, state.routes, state.fallback))
        print(json.dumps(wire_payload(state), indent=1))
        return

    print(f"[poll] serving {len(STATES)} agencies ({', '.join(SERVED)}) "
          f"each every {interval:g}s", flush=True)
    total_rph = len(STATES) * 3600.0 / interval
    if total_rph > DEFAULT_TOKEN_RPH and any(state.source == "511" for state in STATES.values()):
        print(f"[poll] WARNING: {total_rph:.1f} req/h total is over the "
              f"{DEFAULT_TOKEN_RPH} req/h default token; raise POLL_SECONDS "
              "or drop an agency", file=sys.stderr)
    if api_key:
        check_operator_codes(api_key)

    for i, state in enumerate(STATES.values()):
        threading.Thread(
            target=poll_loop,
            args=(state, api_key, args.fixture, args.record, interval,
                  i * min(interval / max(1, len(STATES)), 5.0)),
            daemon=True,
        ).start()
    uvicorn.run(app, host=os.environ.get("HOST", "0.0.0.0"),
                port=int(os.environ.get("PORT", "8000")), log_level="warning")


if __name__ == "__main__":
    main()
