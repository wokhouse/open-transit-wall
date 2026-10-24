# Multi-agency support plan

Status: implemented. Extends the server and firmware so one codebase
serves any 511 member agency (SFMTA, Caltrain, AC Transit, VTA, ...).
Assumption: **one agency per wall**, chosen at provisioning time by the
WebSerial flasher against whatever server URL it is pointed at.

## How an agency differs from the next one

If a target agency publishes through 511, the upstream stays one API —
which agencies actually do is answered by the operator list below, not
assumed. Per agency, only these change:

| Thing | Today (SFMTA-only) | Per agency |
|---|---|---|
| SIRI `agency=` param | `SF` hardcoded in `fetch_live()` | registry id |
| GTFS `operator_id=` | `SF` hardcoded in `refresh_routes()` | registry id |
| Route metadata | `routes.json` + `MUNI_FALLBACK` | `routes_<agency>.json`; fallback table only for `sf` |
| Map bbox | `geo.py BBOX` ↔ `config.h MAP_*` (manual sync) | registry; shipped on the wire |
| Backdrop | `backdrop.h` from `SF_LAND` | generated table in `backdrop.h`, one entry per agency id |

## Server

### Agency registry

`server/agencies.json` is the single source of truth:

```json
{
  "sf": {"name": "SFMTA (Muni)", "siri_agency": "SF", "bbox": {...}},
  "ct": {"name": "Caltrain",     "siri_agency": "CT", "bbox": {...}},
  "ac": {"name": "AC Transit",   "siri_agency": "AC", "bbox": {...}},
  "sc": {"name": "VTA",          "siri_agency": "SC", "bbox": {...}}
}
```

`siri_agency` values come from 511's operator list, `GET
https://api.511.org/transit/operators?api_key=...` — the authoritative
source for which agency codes the SIRI/datafeeds endpoints accept.
Verified live 2026-10-14 against that endpoint (note: VTA is `SC`, not
`VX` — codes must never be guessed). When adding an agency, populate the
registry from the operator list; a startup check warns when a registry
code is missing from it.

`SERVED_AGENCIES=sf,ct` (env) picks which entries a deployment runs. Two
deployments of the same code differ only in this env var. Shoreline polygons
stay in `geo.py`, keyed by agency id, for backdrop generation; `BBOX` moves
into the registry per agency.

### Per-agency state

`Snapshot` becomes a dict keyed by agency id; one poll loop per served
agency; `refresh_routes()` per agency writing `routes_<agency>.json`.
`classify()` already takes a routes dict — pass the right one.

### Discovery: `GET /agencies`

Static discovery for the flasher:

```json
{"agencies": [{"id": "sf", "name": "SFMTA (Muni)"},
              {"id": "ct", "name": "Caltrain"}]}
```

### Data: `GET /vehicles?agency=ct`

- Unknown agency id → 400 naming the valid ids.
- No `agency` param → if the deployment serves exactly one agency, serve it
  (old firmware and single-agency self-hosters keep working); if it serves
  several, 400.
- Payload gains three fields:

```json
{"ts": "...", "age": 41.2, "next": 79, "count": 312,
 "bbox": [lat_min, lat_max, lon_min, lon_max],
 "palette": {"B": [0, 64, 255], "R": [255, 32, 16], "C": [180, 120, 20],
             "M:J": [169, 102, 20], "M:T": [191, 43, 69]},
 "v": [[37.77, -122.42, "B", 123456789],
       [37.76, -122.43, "M:J", 123456788], ...]}
```

- `bbox` kills the `config.h` ↔ `geo.py` sync hazard: the server that knows
  the fleet's extents tells the wall how to project it.
- `palette` maps style keys to RGB. Base codes (`B`/`R`/`C`) are generic
  one-color-per-type; metro rows carry per-line keys `M:<LINE>` colored
  from GTFS `route_color` (the first shipping version shared one generic
  metro blue — the client's palette lookup is opaque, so per-line colors
  needed no firmware change). The server classifies and owns
  all color; the client treats style keys as opaque keys into a map it
  reads fresh from each response — nothing preprogrammed client-side, and
  the palette mirroring in `config.h`/sim comments goes away. Registry
  entries may override the palette per agency at any key — base code or
  single line (`"M:J"`) — so Caltrain ships its own colors with no client
  change. Vehicle rows carry the style key instead
  of three color ints: smaller payload, less parse-time heap.
- `next` = whole seconds until this agency's next upstream fetch, computed
  from the poll thread's own schedule at response time. It lives in the data
  response, not `/agencies`, because it is live state and a client that only
  ever fetches `/vehicles` still learns the cadence.

### No type filtering

The `TYPES` env var goes away. Every active vehicle the agency reports is
served; the `palette` map is generated from the style keys actually present
in the payload, not a fixed set. The one filter that stays is dropping
vehicles with no line reference at all (yards / deadheading, ~20% of the
feed): they can't be classified and would cluster as dots at depots.

Consequences of serving the full fleet:

- `MAX_VEHICLES` stays at 800 for now: the unfiltered live SFMTA feed was
  observed at ~676 vehicles (the number `config.h` records), so the cap
  already covers whole fleets. Revisit only if a wall actually hits the
  cap.
- SF keeps the current bbox. Buses running over the Bay Bridge / East Bay
  fall outside it and clip at the edge — the firmware already clips
  out-of-bbox vehicles, so this is existing behavior, not new. Widen the
  bbox (and extend the `SF_LAND` polygon past the shoreline) only if the
  clipped dots turn out to matter.

### Rate limiting: operator-specified, not auto-split

`POLL_SECONDS` stays a single global operator setting, applied per agency.
The operator chooses a value she knows stays under her token cap given how
many agencies she serves — no server-side budget math, no auto-split. Worked
example: a 60 req/h token with two agencies → `POLL_SECONDS=125`. The server
just logs what it's doing at startup (`serving 2 agencies, polling each
every 125s`) so the cost is visible in the log. If the total exceeds the
60 req/h default token it logs a warning naming the total — a check, not
enforcement: the operator's setting still wins, the server just refuses to
be quietly wrong about it.

`next` on the wire keeps clients in sync with whatever the operator
configured — retuning `POLL_SECONDS` needs a server restart, and every wall
follows on its next poll without a reflash.

## Provisioning protocol (docs/provisioning-protocol.md, implemented)

- New key: `set agency=<id>` / `get agency`. Value: lowercase `[a-z0-9]{1,8}`
  matching registry ids. NVS key `agency` in namespace `otw`.
- Banner: `STORED ... agency=<a>`. `UNPROVISIONED` unchanged.
- Readiness stays ssid+host. Empty agency = fetch `/vehicles` with no param,
  which works against single-agency servers.
- `status` gains `STATUS agency=...`.
- **Bump `OTW-FW` to 2.** The change is additive (v1 firmware replies `ERR
  unknown key`), but the version lets the flasher gate the agency dropdown
  instead of probing.

## Firmware

1. **Provisioner** — `curAgency` buffer, `set/get agency`, NVS save/load,
   banner/help/status lines. ~30 lines in `provisioner.cpp`.
2. **vehicle_client** — append `?agency=<id>` when set; parse `next`,
   `bbox`, and `palette` from the payload. Resolve palette→RGB at parse
   time into the existing `Vehicle` struct (no render-path change; the
   palette is a handful of entries). Missing/invalid palette → poll failed,
   red pixel, same strictness as `next`. A vehicle row whose type key is
   absent from the palette → skip that row; never invent a color.
3. **Cadence** — the glide scheduler uses the advertised `next`;
   `UPSTREAM_REFRESH_S` is deleted from `config.h`. `next` is authoritative
   per-response, never sticky. Missing or unparseable `next` → treat the
   poll as failed: red status pixel, no map update (same path as an HTTP
   error). No fallback constant — a wall that doesn't know the cadence
   can't animate honestly, and red is the correct signal. `next == 0` is
   valid (the server's fetch is due now); the client floors the wait at 1s
   and clamps it to 3600s so a buggy server can't turn the wall into a
   poll hammer or overflow the uint32 schedule math.
4. **Projection** — `map_project` reads the bbox from the payload instead
   of `MAP_*` macros; re-init when the bbox changes (agency swap after
   re-provisioning). `MAP_*` deleted from `config.h`.
5. **Backdrop** — stays compiled: `gen_backdrop.py --agency ct` picks the
   bbox + polygon from the registry and emits one table entry per agency into
   `backdrop.h` (`{agency_id → rle, bbox}`; ~262 bytes per agency at 64×64 —
   four agencies is nothing in flash). Each entry carries the bbox it was
   rasterized for, so the firmware can tell when the provisioned agency's
   backdrop does not match its map. Unknown agency id → blank backdrop; the
   map still works.
6. **Sim** — `sim/sim_main.cpp` mirrors the firmware: same bbox-from-payload
   and `next` handling.

## Flasher flow (future app)

1. User enters a server URL → `GET /agencies` → dropdown ("SFMTA (Muni)",
   "Caltrain").
2. Connect WebSerial → banner → prefill the form from `STORED`.
3. `set ssid=… pass=… host=… agency=ct`, `save`, watch the WiFi-connect log.
4. `OTW-FW 1` → hide the agency field, note that the firmware needs
   reflashing.

The flasher is the only place that prevents agency typos: the firmware
can't validate the id against the server at provisioning time (may have no
WiFi creds yet), so an unknown id surfaces as a server 400 → red pixel.

## Open issues

- **Visible cost of slow polling**: at 125s the glide animation tiles a
  2-minute cycle instead of 65s. `MOTION_GLIDE_MS` stays 5s; glides just
  spread wider. The flasher could surface the server's interval before you
  commit to it.
