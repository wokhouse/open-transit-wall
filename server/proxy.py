"""open-transit-wall mini-proxy.

Fetches the 511 SIRI VehicleMonitoring feed for SFMTA on a shared timer
(default 65s, keeping a live run under 511's default token limit of 60
requests/hour including the startup GTFS routes refresh) and serves a
compact vehicle list for the ESP32 display.

Usage:
  python proxy.py --fixture ../fixtures/sample_vehiclemonitor.json   # offline
  python proxy.py --record    # live fetch, also save raw responses to fixtures/
  python proxy.py             # live fetch

Configuration via environment (or server/.env):
  FIVE11_API_KEY   511 open-data token (required for live mode)
  POLL_SECONDS     upstream poll interval (default 65; a full minute of
                   polling plus the startup GTFS refresh stays within 511's
                   default 60 req/hour)
  HOST / PORT      bind address (default 0.0.0.0:8000)
  TYPES            vehicle classes to serve: subset of B,R,M,C (empty = all;
                   TYPES=M,C gives a metro + cable car wall)
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import threading
import time
import zipfile
import zlib
from datetime import datetime, timezone
from pathlib import Path

import httpx
import uvicorn
from dotenv import load_dotenv
from fastapi import FastAPI
from fastapi.responses import FileResponse, JSONResponse

from classify import classify, load_routes
from geo import BBOX, SF_LAND
from siri import parse_vehiclemonitor

SERVER_DIR = Path(__file__).resolve().parent
load_dotenv(SERVER_DIR / ".env")

ROOT = SERVER_DIR.parent
FIXTURES_DIR = ROOT / "fixtures"
ROUTES_JSON = SERVER_DIR / "routes.json"
UPSTREAM_URL = "https://api.511.org/transit/VehicleMonitoring"
DATAFEED_URL = "https://api.511.org/transit/datafeeds"

# Which vehicle classes to serve: subset of B(bus) R(rapid) M(metro) C(cable).
# Empty = all. Tolerant of "M,C", "MC", "m c". Set TYPES=M,C in .env for a
# metro + cable car wall.
def parse_types(value: str) -> set:
    return {c for token in value.upper().split(",")
            for c in token.strip() if c in "BRMC"}


TYPES = parse_types(os.environ.get("TYPES", ""))


class Snapshot:
    """Latest vehicle list, shared between the poll thread and handlers."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._vehicles: list[dict] = []
        self._fetched_at: float = 0.0
        self.routes: dict[str, dict] = load_routes(ROUTES_JSON)

    def update(self, vehicles: list[dict]) -> None:
        with self._lock:
            self._vehicles = vehicles
            self._fetched_at = time.time()

    def payload(self) -> dict:
        with self._lock:
            return {
                "ts": datetime.fromtimestamp(self._fetched_at, tz=timezone.utc).isoformat(),
                "age": round(time.time() - self._fetched_at, 1),
                "count": len(self._vehicles),
                "v": self._vehicles,
            }

    def age(self) -> float:
        with self._lock:
            return time.time() - self._fetched_at


state = Snapshot()


def compact_vehicles(raw_vehicles: list[dict]) -> list[dict]:
    """Map parsed SIRI entries to the cached vehicle dicts.

    Vehicles without a route reference (yards / deadheading, ~20% of the feed)
    are dropped — they'd plot as misclassified dots clustered at the depots.
    """
    out = []
    for vehicle in raw_vehicles:
        if not vehicle["line"]:
            continue
        info = classify(vehicle["line"], state.routes)
        if TYPES and info["code"] not in TYPES:
            continue
        out.append({
            "r": info["line"],
            "la": round(vehicle["lat"], 5),
            "lo": round(vehicle["lon"], 5),
            "t": info["code"],
            "c": list(info["rgb"]),
            # Stable id so the ESP32 can match vehicles across polls and
            # animate each one's movement individually. 31 bits keeps the
            # collision rate negligible at this fleet size (~2^-6 per day).
            "i": zlib.crc32(vehicle["id"].encode()) & 0x7FFFFFFF,
        })
    return out


def to_wire(vehicles: list[dict]) -> list[list]:
    """Project cached vehicles to the ESP32 wire format: flat
    [lat, lon, r, g, b, id] arrays. Keyed objects overflowed the ESP32's heap
    at rush-hour fleet sizes — every JSON key costs ~16 bytes of parse-time
    heap next to the display framebuffer, so /vehicles ships arrays by default
    and the keyed format stays available for the preview (?full=1)."""
    return [[v["la"], v["lo"], *v["c"], v["i"]] for v in vehicles]


# --- Upstream fetching -------------------------------------------------------

def fetch_live(api_key: str) -> dict:
    response = httpx.get(
        UPSTREAM_URL,
        params={"api_key": api_key, "agency": "SF", "format": "json"},
        timeout=20.0,
        follow_redirects=True,
    )
    response.raise_for_status()
    # 511 sometimes prefixes JSON responses with whitespace/BOM.
    return json.loads(response.text.lstrip("\ufeff \t\r\n"))


def refresh_routes(api_key: str) -> None:
    """Download the SFMTA GTFS static feed and cache route types/colors."""
    response = httpx.get(
        DATAFEED_URL,
        params={"api_key": api_key, "operator_id": "SF", "status": "active"},
        timeout=60.0,
        follow_redirects=True,
    )
    response.raise_for_status()
    with zipfile.ZipFile(io.BytesIO(response.content)) as archive:
        routes_txt = archive.read("routes.txt").decode("utf-8-sig")
    rows = [line.split(",") for line in routes_txt.splitlines() if line.strip()]
    header = [cell.strip('"') for cell in rows[0]]
    idx = {name: header.index(name) for name in
           ("route_id", "route_short_name", "route_long_name", "route_type")}
    color_idx = header.index("route_color") if "route_color" in header else None
    text_idx = header.index("route_text_color") if "route_text_color" in header else None

    routes: dict[str, dict] = {}
    for cells in rows[1:]:
        if len(cells) <= max(idx.values()):
            continue
        route_id = cells[idx["route_id"]].strip('"')
        short = cells[idx["route_short_name"]].strip('"').upper() or route_id.upper()
        entry = {
            "type": int(float(cells[idx["route_type"]].strip('"') or 3)),
            "long": cells[idx["route_long_name"]].strip('"'),
        }
        if color_idx is not None and cells[color_idx].strip('"'):
            entry["color"] = cells[color_idx].strip('"').lstrip("#").upper()
        if text_idx is not None:
            entry["text"] = cells[text_idx].strip('"')
        routes[route_id] = entry
        routes[short] = entry

    ROUTES_JSON.write_text(json.dumps(routes, indent=0))
    state.routes = routes
    print(f"[routes] cached {len(routes)//2} SFMTA routes -> {ROUTES_JSON.name}",
          flush=True)


def poll_loop(api_key: str | None, fixture: Path | None, record: bool,
              interval: float) -> None:
    first = True
    while True:
        try:
            if first and api_key:
                try:
                    refresh_routes(api_key)
                except Exception as error:  # noqa: BLE001 - keep serving
                    print(f"[routes] refresh failed ({error}); using fallback")

            if fixture is not None:
                payload = json.loads(fixture.read_text())
            elif api_key:
                payload = fetch_live(api_key)
                if record:
                    FIXTURES_DIR.mkdir(exist_ok=True)
                    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                    (FIXTURES_DIR / f"vehiclemonitor_{stamp}.json").write_text(
                        json.dumps(payload, indent=1))
            else:
                raise RuntimeError("FIVE11_API_KEY not set (use --fixture for offline dev)")

            vehicles = compact_vehicles(parse_vehiclemonitor(payload))
            state.update(vehicles)
            print(f"[poll] {len(vehicles)} vehicles at {state.payload()['ts']}",
                  flush=True)
            first = False
        except Exception as error:  # noqa: BLE001 - the proxy must keep serving
            print(f"[poll] error: {error}", file=sys.stderr)
        time.sleep(interval)


# --- HTTP surface ------------------------------------------------------------

app = FastAPI(title="open-transit-wall proxy")


@app.get("/vehicles")
def vehicles(full: bool = False) -> JSONResponse:
    payload = state.payload()
    if not full:
        payload["v"] = to_wire(payload["v"])
    return JSONResponse(payload, headers={"Cache-Control": "no-store"})


@app.get("/geo")
def geo() -> dict:
    return {"bbox": BBOX, "land": SF_LAND}


@app.get("/health")
def health() -> dict:
    return {"upstream_age_s": None if not state.age() else round(state.age(), 1),
            "routes_cached": bool(state.routes)}


@app.get("/preview")
def preview() -> FileResponse:
    return FileResponse(SERVER_DIR / "static" / "preview.html")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=None,
                        help="serve vehicles from a recorded SIRI JSON file instead of 511")
    parser.add_argument("--record", action="store_true",
                        help="save raw live responses to fixtures/")
    parser.add_argument("--once", action="store_true",
                        help="fetch once, print the compact payload, exit")
    args = parser.parse_args()

    api_key = os.environ.get("FIVE11_API_KEY")
    interval = float(os.environ.get("POLL_SECONDS", 60))

    if args.once:
        payload = (json.loads(args.fixture.read_text()) if args.fixture
                   else fetch_live(api_key))
        print(json.dumps(compact_vehicles(parse_vehiclemonitor(payload)), indent=1))
        return

    thread = threading.Thread(
        target=poll_loop,
        args=(api_key, args.fixture, args.record, interval),
        daemon=True,
    )
    thread.start()
    uvicorn.run(app, host=os.environ.get("HOST", "0.0.0.0"),
                port=int(os.environ.get("PORT", 8000)), log_level="warning")


if __name__ == "__main__":
    main()
