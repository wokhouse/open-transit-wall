# NYC MTA as an agency: feasibility scope

Status: subway prototype implemented 2026-10-30 (branch `light-cobra`);
see "Prototype shipped" at the end. Originally scoped same day: assesses adding
New York City Transit (MTA NYCT subway + bus) as an agency in the
multi-agency framework (`docs/multi-agency-plan.md`). Bus is deferred.

## Verdict

Feasible. The wire contract, firmware render path, and flasher flow do not
change. Three things are genuinely new:

1. A second upstream type. MTA publishes GTFS-RT protobuf, not 511 SIRI.
   The proxy needs one per-source fetch/parse module (~150 lines + protobuf
   dependency).
2. Subway vehicle positions carry no coordinates. Dots must snap to station
   coords from the static GTFS (`stop_id` -> `stops.txt`). Bus positions
   are fully geolocated and need no such path.
3. Fleet size. MTA is 2.5-5x SFMTA. `MAX_VEHICLES 800` and the parse-heap
   budget that value encodes are the one hard engineering risk. Any MTA
   wall needs a raised cap and a measured heap test on real hardware.

## Measured live data (2026-10-30 ~20:50 ET, evening off-peak)

Subway (GTFS-RT, protobuf, no API key):

- 8 per-division feeds at `api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs*`.
  The `%2F` encoding is required; a literal `/` 403s (known trap from the
  MTA developer list, 2025-02).
- Feed header age 3 s; feeds regenerate every ~30 s.
- 584 VehiclePosition entities across the 8 feeds. Zero carry lat/lon. All
  carry `stop_id` + `current_stop_sequence` + `current_status` + timestamp.
- `route_id` is the bare line name (`1`, `6X`, `A`): drops straight into
  the existing `M:<LINE>` style-key machinery.
- No `vehicle.id`; `trip_id` is the only cross-poll identity.

Buses (GTFS-RT, protobuf, key parameter currently optional):

- One feed for the whole fleet: `gtfsrt.prod.obanyc.com/vehiclePositions`.
  Returned 200 + full data with no `key` param at probe time; docs still
  say a free BusTime key is required. Pass the free key if one is set;
  treat absence as acceptable.
- Feed header age 7 s; regenerates every ~30 s.
- 1,836 VehiclePosition entities, 100% with lat/lon + bearing, stable
  `vehicle.id` (`MTA NYCT_9771`) — glide identity works unchanged.
- 290 routes, `route_id` is the route short name (`M55`, `B1`): classifies
  through the normal path, default bus blue. No static GTFS needed to
  render buses.

Combined evening fleet observed: ~2,420 vehicles (bus + subway). Friday
peak is higher; expect ~1.5x. A bus-only wire payload measured 68.7 KB
(1,827 rows); combined evening ~90 KB, peak ~130 KB.

LIRR and Metro-North also publish GTFS-RT (incl. train positions); out of
scope — their extents dwarf any city bbox and they would stretch the map.

## Per-component changes

| Component | Change | Size |
|---|---|---|
| `agencies.json` | New entry shape: add `"source": "511" \| "mta-rt"` (default `511`); `siri_agency` only required for `511`; MTA entry carries the feed list instead | small |
| `proxy.py` | Branch `fetch`/`refresh_routes`/`check_operator_codes` on `source`; one poll thread still per agency | small |
| New `server/mta_rt.py` | Decode subway division feeds + bus VP feed -> the same intermediate vehicle dicts `siri.parse_vehiclemonitor` produces | ~150 lines |
| Subway position | Station-snapping: fetch subway static `google_transit.zip` at startup, index `stops.txt` id->coords, set dot coords from `VP.stop_id`; also harvest `routes.txt` `route_color` for official line colors | medium |
| `classify.py` | No change. GTFS types drive everything; subway `route_color` gives official line colors via the existing `M:<LINE>` path | none |
| `requirements.txt` | Add `protobuf` + `gtfs-realtime-bindings` | trivial |
| Tests | Record raw protobuf bytes as fixtures (same no-live-API rule as 511); parser + compact tests | small |
| `geo.py` / backdrop | N/A — the land/backdrop concept was removed from the whole project on 2026-10-30; the map is vehicles on black | — |
| Flasher | Nothing: agency dropdown reads `/agencies` | none |
| Firmware | No contract change. Heap work only — see below | the risk |

## The size problem, and options

`MAX_VEHICLES 800` encodes SFMTA's ~676. MTA bus alone is 1,836 off-peak.
The firmware parses with ArduinoJson streaming into an adaptive pool; pool
cost runs ~50-60 bytes per row plus the static `vehicles`/`actives`/
`seen`/`slotOrder` arrays (~16-24 B each per vehicle). Bus-only needs
roughly 110-150 KB pool off-peak; combined peak needs ~2x that. This has
never been measured on a wall.

Order of work:

1. Spike first: raise `MAX_VEHICLES` to a candidate (e.g. 2,600), feed a
   recorded bus fixture to a real wall, log `ESP.getMinimumFreeHeap()`
   through connect + parse + render. Nothing else is worth committing
   before this number exists. Move static arrays to PSRAM if needed; the
   JSON pool wants fast heap and the framebuffer situation on the
   S3 targets decides how much internal RAM is spare.
2. If full-fleet parse does not fit, cut server-side rather than client-side:
   bbox crop (the payload already ships the bbox; the server can drop rows
   outside it before `to_wire`) or mode filter. The plan's "no type
   filtering" rule needs an explicit MTA carve-out or an honest cap.
3. Phasing suggestion (superseded below): bus-only wall first, subway
   second. Superseded by the subway-only section — the subway is the
   first cut.

## Subway-only as the first cut (revised phasing, 2026-10-30 follow-up)

Feedback: the subway is what people care about. Reframed first scope: an
NYC wall showing subway lines only. The station-coordinate gap turns out
to be fully covered by the glide mechanism already on the wall:

- `mergeVehicles` (in `firmware/src/main.cpp`) matches vehicles by id across polls
  and linearly interpolates each one, in pixel space, from the pixel it
  is visually at to the new snapshot's target. With subway dots placed at
  their reported station each poll, every poll's station-to-station hop
  becomes a glide segment. Dots crawl along the lines. No firmware change.
- Identity: id = crc32(trip_id) (no `vehicle.id` in subway VPs). Stable
  while a train runs its trip; changes at terminals, where the old dot is
  dropped and a new one appears at the platform — physically correct.
- Interpolation error is bounded by the track chord between stations and
  is invisible at wall scale. A realistic subway bbox (~40.56-40.90 N,
  -74.00 to -73.75 W) puts ~330-600 m on a pixel at 64x64; one local
  station hop is 1-2 px, express skips 4-6 px. Chord-vs-track error is
  under a pixel for hops, a few px on long curvy skips. Acceptable.
- Cadence helps: MTA feeds regenerate every ~30 s and carry no key rate
  cap, so a subway-only deployment can run `POLL_SECONDS` near 35 instead
  of 65. Shorter hops = smoother crawl; the `next` contract keeps every
  wall in sync without a reflash.
- Palette wins for free: subway static `routes.txt` carries official
  per-line `route_color`, so the wall ships ~30 real line colors through
  the existing `M:<LINE>` path.

Sizing, subway-only: 584 trains observed off-peak; peak not yet measured.
The 800 cap may cover off-peak but not peak. Raise `MAX_VEHICLES` to a
measured peak + margin (order 1,000-1,200) after the heap spike — payload
at 900 rows is ~35 KB, far inside the parse budget that 676 rows already
proved. The spike is now a low-risk confirmation, not a blocker.

Dropped by going subway-first: the bus feed and its optional-key
dependency, the 2.5-3x whole-fleet payload, and bus static GTFS. Buses
become the later phase if ever wanted.

Open item before committing the cap: one peak-hour fleet count. Run the
polled-feed probe (seven feeds; SIR is not polled) on a weekday at ET
morning peak and record the total.

## Ops and risk notes

- Rate limits: MTA feeds have no documented request cap (511's 60 req/h
  token does not apply). 9 feeds per 60 s is ~540 req/h; measure sustained
  polling before shipping, and keep `POLL_SECONDS` operator-chosen as today.
- The bus feed's no-key behavior is an observation, not a contract: the
  docs still ask for a free key. Support an optional env key.
- Subway glide identity: `trip_id` changes at terminals, so subway dots
  re-identify when a train turns around. Benign — trains reverse at
  terminals; bus IDs are stable.
- Subway dots snap station-to-station at the 30 s feed cadence; no smooth
  interpolation along track. Looks coarse but honest. Track-interpolation
  (trip `stop_time_updates` + shape) is a later enhancement, not a
  prerequisite.
- Select Bus Service would render as ordinary bus blue; `classify`'s
  RAPID/EXPRESS check does not match "SBS". A one-line SBS pattern is a
  nice-to-have for phase 2.
- MTA data is free to use under their terms; the wall shows no MTA
  branding, so the logo/trademark licensing question does not arise.
- Deployment: same image, `SERVED_AGENCIES` adds the MTA id. A 511-key
  failure then cannot take down an MTA wall, and vice versa.

## Effort (rough)

Superseded by the subway-only section for phasing; totals unchanged:

- Subway-only end-to-end (feed parser — 8 feeds at estimate time, 7
  shipped with SIR dropped — static zip + station index,
  colors, fixture tests, cap/heap confirmation): 1.5-2.5 days.
- Bus phase, if ever wanted: 1-2 days.
- NYC backdrop polygon: half a day, optional. (Moot: the backdrop concept
  was dropped project-wide on 2026-10-30.)
- Blocking unknown: the on-wall heap measurement. Everything else is
  routine.

## Prototype shipped (2026-10-30)

What landed (subway-only, per the revised phasing):

- `server/gtfs_rt.py` (renamed from `mta_rt.py` right after the prototype:
  the feed URLs and static-zip URL moved into the registry entry, so the
  module is now a generic `gtfs-rt` source — a new GTFS-RT rail agency is
  a registry edit, not new Python): parses the seven polled division
  GTFS-RT feeds and
  the static `stops.txt` station table (SIR is deliberately not polled —
  the viewport excludes Staten Island). `proxy.py` gains a `source`
  branch; a `gtfs-rt` poll fetches the entry's feeds, resolves `stop_id`
  to
  coords, and
  feeds the shared compact/classify path. `routes_mta.json` and
  `stations_mta.json` are cached in-repo (same offline pattern as the
  other agencies).
- Identity: `id = crc32(trip_id)` as scoped. Positions snap to the
  reported station; the existing glide animates the hops.
- Viewport: a 34.5 km true-square (539 m/px at 64x64) with the
  composition Jade asked for: Manhattan and Brooklyn centered at ~30%
  from the left margin, Coney Island on the bottom edge, north edge at
  Riverdale to squeeze detail out of the square. Staten Island is
  excluded the way the sf viewport sits south of Treasure Island: the SIR
  GTFS-RT feed is not polled at all. The crop
  clips nine branch-tip stations — the 1 above 238 St, the 2/5 White
  Plains Rd el north of 238 St, the 5's Dyre Av — whose trains drop at
  the top edge; every other station is inside by construction.
  Iteration notes: the Coney-Island-anchored first box clipped 4 Staten
  Island Railway dots; a west edge at -74.045 pulled the Verrazano
  approach into the frame; the system's real east end is Far Rockaway
  (the network spans 23.3 km E-W, so the square's extra width is
  deliberate margin). Verified on live data after each change.
- Backdrop: built for the plan at the time — Natural Earth land (110m and
  50m) failed NYC (it fills the East River, Jamaica Bay, and drops
  Manhattan), so the polygons came from OpenStreetMap island/borough
  polygons via Nominatim, minus hand-traced Jamaica Bay and Upper New York
  Bay cutouts, simplified at 0.0004 (© OpenStreetMap contributors, ODbL).
  This was later superseded: the land/backdrop concept was removed from the
  whole project on 2026-10-30 (Jade: the map is just the transit
  vehicles). `server/geo.py` and `tools/gen_backdrop.py` are deleted, and
  the panel draws vehicles on black with no backdrop at all.
- `MAX_VEHICLES` raised 800 -> 1200 (static arrays only, ~38 KB RAM on a
  512 KB SRAM board). The on-wall heap check still wants a first boot on
  real hardware at weekday peak.

Measured live: 503-508 trains, 25 line keys with official `route_color`s,
19.5 KB wire payload, 4/517 reports with station ids missing from the
static GTFS (H19x) — skipped and counted in the poll log, by design.
Server suite: 32 tests pass; the new `test_mta_rt.py` checks fixture
parse -> station resolve -> viewport containment -> palette keys -> wire
shape, all offline from recorded fixtures.

Deployment: flash firmware built from this branch (the raised
`MAX_VEHICLES` cap is compiled), start the proxy with `SERVED_AGENCIES=mta`,
provision host + agency `mta` with the WebSerial tool. Switching a wall
between agencies after that is WebSerial-only; no reflash. Reflashes
return only for cap/panel-size changes — the bbox and palette ride the
wire.

## Viewport update (2026-10-30, second crop)

Composition re-ask from Jade: top edge in Inwood, bottom edge at Jacob
Riis Park, left edge barely clipping the SIRT north end so Manhattan and
Brooklyn sit near center. Chosen from three rendered candidate frames: the
right-at-Jamaica-179-St variant forced the left edge 14 km west of Bay
Ridge and cropped the Far Rockaway branch, so it was rejected; the
left-at-SIRT-tip frame (chosen) forces 8 km of deliberate empty margin
east of Far Rockaway — the square's forced slack.

The box is a 35.8 km true square (568 m/px at 64x64): lat 40.559-40.8805,
the edges pinned to OpenStreetMap land polygons (Inwood Hill Park north
edge, Jacob Riis Park south shore) plus a ~1 px margin, lon span derived
from the lat span at mid-latitude. The left edge at -74.0849 sits ~1 px
outside St. George/Tompkinsville; still no SIRT dots — the SIR GTFS-RT
feed remains unpolled, so the left is black margin only. Verified on live
data: 234 trains, 21 line keys, 1 report outside the north edge (the 1
north of 238 St — deliberate crop, unchanged), 0 outside every other edge.
The bottom edge now fully contains both Rockaway branches.

## Viewport update (2026-10-31, third crop: traffic centered)

Weekday/weekend daytime traffic was now visible on a live wall, and Jade
asked to move the picture on the wall ~20% to the right so the traffic
sits centered; Staten Island appearing was explicitly not a priority,
just a tell. Note the direction: moving the *picture* right = moving the
*bbox* west. The first candidate (frame east 20%) moves the picture
left, jams the Manhattan core on the left border, and drops 53 live
rows — measured, not inspected.

The chosen box keeps the same 35.8 km square (lat edges unchanged) and
shifts lon west by 20% of the span: lon -74.1697/-73.7455. The picture
on the wall moves +12.6 px right; the live traffic pattern spans px
20-61 with its mass center at px 33.9 (panel center 31.5). West edge at
-74.1697: the lower-left now covers northeastern Staten Island — 14 of
the 24 SIR stations (Tompkinsville to Prince's Bay) fall inside — still
no dots, SIR remains unpolled. East edge at -73.7455: the east-most
served subway stop (Lefferts Blvd, -73.7554) keeps a ~1.5 px margin;
the 179 St tip is ~5 px inside. Verified on live data (weekend, 423
rows): 0 rows outside any edge except the north crop, unchanged at 1.
No served station drops at the west edge — the west edge crosses only
unpolled land.

Trade-off accepted: a ~24 px black band at the lower-left where Staten
Island is (no feed), and the Lefferts branch tip sits 1.5 px from the
east border. Polling SIR was the obvious follow-up; it shipped the same
day — see the next section.

## SIR feed added (2026-10-31)

The lower-left black band asked for it. The feed is
`nyct%2Fgtfs-si`, keyless, same update cadence. The guessed
`mta/gtfs-sir` path does not exist: api-endpoint answers unknown
paths with HTTP 200 + an XML `NoSuchKey` body, which surfaces as a
corrupt protobuf wire format — a trap worth remembering for any
MTA feed URL edit.

Measured on a Sunday morning: 6 SIR vehicles, route_id `SI`, colored
from the static routes.txt (#08179C) like every other subway route —
zero code. SIR station ids in RT carry direction suffixes (`S21N`),
same as the subway's, and all resolve against the station table. 14 of
24 unique SIR stations fall inside the box; the outer tail
(Tottenville..Prince's Bay side, S20 and south) clips at the bottom
edge like the Riverdale tips clip at the top. Recorded as
`fixtures/mta_subway_si.pb`; the viewport test now asserts the south
clip as a documented edge.

Live `--once` after wiring it in: 439 rows, 6 SIR, 3 drawing pixels
in the old black band (rows px (11,57) and (14,46)); the rest clip
south as designed. The frame's only off-frame rows are the documented
north and south crops. The band is filled — on a weekend the SIR
fleet is small, so it is sparse dots, not a mass.

## Viewport update (2026-10-31, fourth crop: picture 5% left)

Jade: "move it to the left like 5%" after seeing the SIR-inclusive
frame. Picture left = bbox east: lon shifts +0.0212° (5% of span) to
-74.1485/-73.7243; lat edges unchanged. Measured on one live payload
(439 rows): dot-mass center moves px 34.2 → 31.0 (panel center 31.5),
traffic span 13–62 → 10–58, left margin grows to 10 px of SI land,
right margin 1 → 5 px. Off-frame rows unchanged at 6: nothing drops
at the east edge (the edge simply extends the slack beyond the
Lefferts/179 St tips; their margin grows 1.5/5 px → ~14/17 px). The
south and north crop behavior is untouched.

## Still open

Weekday ET peak fleet count (next weekday morning), the heap
spike on first real boot, and the bus phase (deferred).
