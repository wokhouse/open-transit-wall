# TTC (Toronto) as an agency: feasibility scope

Status: scoped 2026-10-31. All live probes ran 2026-10-31 18:33-18:50 UTC
(Saturday 14:33-14:50 local, Toronto). Static numbers come from the merged
GTFS zip current at that date (resource last-modified 2026-10-30,
`feed_version S1000544`, service period 2026-11-01 to 2026-12-19).

## Verdict

The TTC real-time feed is the cleanest upstream this project has probed:
keyless, geolocated on 100 percent of records, stable fleet-number
identity, 30 s refresh. The position-first `gtfs_rt.parse_rt` and the
`vehicle.id` identity rule shipped for Metro Transit (PR #5) already
consume it exactly right. No parser, classifier, proxy, or firmware code
change is needed to parse it.

Two facts shape the plan:

1. Fleet size. A full-network TTC wall exceeds `MAX_VEHICLES 1200` right
   now: the live probe kept 1,212 wire rows on a Saturday afternoon, and
   the static peak bound for a weekday is 1,487 surface vehicles. The
   raised-cap and on-wall heap work that the MTA bus phase already owns
   is the first work item. The TTC is not a registry-edit-only agency.

2. No subway vehicles. The vehicle-positions feed is documented by TTC as
   "surface routes only", and the probe confirms it: subway trains appear
   nowhere in it. The subway has station-level trip updates and alerts,
   but no positions, in any public feed. A Toronto wall is a streetcar
   and bus wall. The subway dots that made the MTA agency work are not
   available.

Everything else is routine. Measured detail below.

## Measured live feeds (2026-10-31 18:33-18:50 UTC)

### Hosts and gates

- The old portal `open.ttc.ca` is gone: DNS does not resolve at all. Do
  not reference its `server/gtfs-rt/*.php` paths.
- Current portal: `https://gtfsrt.ttc.ca/` (linked from the City of
  Toronto open-data page). Feeds: `/vehicles/position?format=binary`,
  `/trips/update`, `/trips/subway`, `/alerts/*` (mode-split: all, bus,
  streetcar, subway, stops, accessibility), plus `?format=text`
  (protobuf text) variants and a `/release-notes` page.
- Mirror: `https://bustime.ttc.ca/gtfsrt/vehicles` returned identical
  bytes (same header timestamp, 101,664 bytes). One host per registry
  entry; `gtfsrt.ttc.ca` is the portal the city documents.
- No API key, no gate body, no error-inside-200 trap.
  Content-type `application/x-protobuf`.

### VehiclePositions facts

- 1,419 entities at 14:36 EDT Saturday. 1,212 carried `route_id`; all
  1,419 carried `position.lat/lon` (plus bearing, speed); 1,206 also
  carried `stop_id`. Every routed record carried a stable 4-digit
  `vehicle.id` (all 1,212 unique in-feed).
- 207 entities carried position and `vehicle.id` but no `trip` at all.
  Their ids fall in the surface fleet number series (streetcar 3xxx/4xxx,
  bus 6xxx-9xxx) and their positions sit in carhouses, streetcar loops,
  and station bus bays (River carhouse, the Don Mills 506 loop, the
  Lawrence West bus bay). These are off-trip surface vehicles. No entity
  sat in a subway tunnel segment. The current parser and
  `compact_vehicles` drop them by the same standing rule used for SIRI
  yard/deadheading records: no route means no color, and they would
  cluster at depots. Correct as-is.
- Refresh cadence measured: `header.timestamp` advanced exactly 30 s
  between fetches 30 s apart; entity age at fetch was 5-30 s.
  `POLL_SECONDS 60` stays safe.
- Feed is `incrementality: FULL`. One poll = one complete snapshot.

### No subway vehicle positions, in any feed

- TTC's own release notes state: "Added vehicle positions feed (surface
  routes only)" (v3.0) and, later, "Added Subway Trip Updates" (v3.4.1,
  2026). The `/trips/subway` feed carries per-trip station arrival
  predictions with `route_id` 1/2/4, but no `vehicle` descriptor and no
  coordinates: no join to vehicles is possible.
- The surface `/trips/update` feed carries no subway trips at all (166
  distinct routes probed, zero subway).
- The former workaround, the TTC Tracker CSV
  `feed-stops-subway-position.txt` on `ttctrackertest.ttc.ca`, is dead:
  that host no longer resolves. A 2026-10-31 search of the city open-data
  catalogue finds no other subway-position dataset (only delay stats,
  shapefiles, ridership).
- Conclusion: subway dots are impossible for now. If a future TTC release
  adds subway vehicle positions, the records would arrive route-less at
  first; that stays a watch item on `/release-notes`, not a design input.

### Static GTFS

- Use the merged feed (all modes in one file):
  `https://ckan0.cf.opendata.inter.prod-toronto.ca/dataset/b811ead4-6eaf-4adb-8408-d389fb5a069c/resource/c920e221-7a1c-488b-8c5b-6d8cd4e85eaf/download/completegtfs`
  keyless, 67.5 MB, refreshed daily (resource last-modified
  2026-10-30T17:17).
- Trap: the sibling `ttc-routes-and-schedules` resource (the URL
  Transitland used through mid-2026) now returns a 22-byte EMPTY zip with
  HTTP 200. Fetch-by-header is blind to it. Use the merged package.
- 2026-10-31 afternoon re-probe: the `ttc.ca/open_data` mirror of this
  resource has gone 404 (it served 67.5 MB that morning). The CKAN
  resource above did not move: 3/3 fetches at 67,548,989 bytes, ~2 s
  each. The registry points at CKAN, never the mirror.
- The city's other live surface static package (`SurfaceGTFS.zip`,
  dataset `bd4809dd`) serves real bytes (66 MB) but nests every file
  under a `GTFSExport - Copy/` directory inside the zip, so the proxy's
  flat `routes.txt`/`stops.txt` reads would find nothing. Not usable
  without new code; the merged package needs none.
- Trap: live route ids `124`, `200`, `201`, `202`, `203` appear in the
  vehicle feed at real road positions but in neither published static
  package. `classify` treats an unknown id as an ordinary bus row, so
  those vehicles stay on the wall as generic dots instead of vanishing.
  Pinned by the fixture tests; no code.
- One agency (`TTC`, `America/Toronto`). 230 routes, every one with
  `route_short_name` and `route_color`. Route types: 3 subway (Line 1
  `D5C82B`, Line 2 `008000`, Line 4 `B300B3`), 18 streetcar/LRT (Line 5
  Eglinton `FF8000` and Line 6 Finch West `808080` are in the static and
  the alert feeds but not yet in vehicle positions: not in revenue), and
  209 bus (all `ED1C24`).
- 11,970 stops, 9,826 with coordinates (entrances/pathways are the rest).
  114 parent stations. `parse_stops` and the daily routes/stations
  refresh handle this file unchanged. `stop_times.txt` is 4.2 M rows; the
  proxy never parses it.

### Classification, verified against static + live

- Every `route_id` is bare numeric and equal to the short name. The
  routes cache keys (route_id + upper short name) resolve live ids
  unchanged. `normalize_line` needs no help.
- 209 buses -> bus blue. The 29 express 9xx routes (900, 905, 939, ...)
  classify red through the `EXPRESS`/long-name path; 119 such vehicles
  were live at probe. That matches real TTC express service.
- Streetcars 501-512 -> per-line `M:5xx` style keys with the official
  `ED1C24` from routes.txt: same path as Muni metro lines. 10 line keys
  were live at probe.
- The subway line colors are present in the routes cache but never used,
  because subway vehicles never arrive.
- `classify.py`: no change. Design note for the composition pass: the
  default bus-blue vs TTC-red identity is a look, not a blocker; registry
  `palette` overrides already exist for any key.

## Fleet size against MAX_VEHICLES 1200

The live count at probe is not a fleet number, so the static zip was
swept for maximum simultaneous trips in service on the next Monday
(2026-11-02), same method as the CTA/TriMet/Metro scope doc. Each
in-service trip stands in for one vehicle: over-counts buses (short
turns share vehicles), under-counts coupled streetcar pairs (the probe
showed 2 two-unit groups at identical coordinates; peak has more).

| Scope | Live at probe (Sat 14:36) | Static peak bound (Mon 11-02) | Fits 1200? |
|---|---|---|---|
| Surface total | 1,212 rows kept | 1,487 (1,247 bus at 15:40 + 240 streetcar at 16:50) | no |
| Bus only | - | 1,247 (15:40) | no |
| Streetcar+LRT only | - | 240 (16:50) | yes |
| Subway (not drawable) | 0 | 110 (08:05) | yes, but no positions |

The full-network live wire payload was measured through the repo's own
pipeline (`parse_rt` -> `compact_vehicles` -> `wire_payload`): 1,212 rows
= 43,899 bytes, 36 bytes per row (same flat `[lat,lon,key,id]` shape as
the measured MTA payload). Scaled, a weekday-peak TTC wall is about 55 KB. That
is MTA-bus scale, not MTA-subway scale: it belongs to the raised-cap and
heap-spike phase that project already owns, and the cap arithmetic must
count coupled streetcar units as separate rows.

## Viewport sizes for later composition

Raw network extents from the static zip, for scale only (the composition
pass follows the usual preview process):

- Subway (if it ever gets positions): 22 km E-W x 17 km N-S.
- Streetcar + LRT: 27 km E-W x 19 km N-S (724 line stops; a few virtual
  stop points stretch this slightly).
- Full bus network: 35 km N-S x 45 km E-W (43.59-43.91 N,
  -79.65..-79.12 W). The bus network reaches far into Mississauga,
  Markham, Vaughan, and Scarborough.

A core-city frame near 20 km keeps every streetcar; it keeps a measured
59% of Saturday peak buses, not "most" — the outer branches (Vaughan,
Scarborough, Pearson, Mississauga) are a third of the live fleet.
The cap, not the frame, is what counts rows: the proxy serves every
vehicle the agency reports, and the firmware counts payload rows, not
in-frame rows. A deliberately cropped TTC configuration cannot dodge the
cap without new code.

### The shipped v1 frame

The registry bbox is a 23 km true square centered on the recorded
streetcar span (live 2026-10-31 14:36: 10.5 km N-S x 20.4 km E-W,
185 cars): `43.53828..43.74489 N, -79.55065..-79.26514 W`. The
streetcar network sizes the frame; it must stay wholly inside
(3.8 px minimum margin), and the test pins that.

Measured against the same full recording: 712/1,212 rows in frame
(59%), streetcars 185/185. Clipping is intentional at three edges —
north 392 rows (max 51 px beyond), west 65 (21 px), east 43 (8 px) —
and zero rows sit south of the south edge; the test encodes exactly
that shape. A live `--once --agency ttc` at 16:35 served 1,209 wire
rows (48.6 KB); the simulator drew 707 in-frame and reported 502
outside, matching the sweep. The rasterized grid check passed: px rows
30-37 carry the King/Queen/Dundas streetcar bands and px cols 32-35
carry the Yonge/University corridor. At 359 m/px the streetcar lines
resolve one pixel per corridor and the surface fleet reads sparse; a
full-network frame (41.4 km) was rejected — it would put every corridor
inside one pixel and cost nothing on the cap.

## Per-component changes

| Component | Change | Size |
|---|---|---|
| `agencies.json` | Landed: one `ttc` `gtfs-rt` entry: `feeds` = the single vehicle-positions URL, `static_gtfs` = the merged CKAN zip, bbox = the shipped v1 frame above | small |
| `gtfs_rt.py` | None. Position-first and `vehicle.id`-first (PR #5) already fit this feed exactly | none |
| `proxy.py` | None. Keyless, `application/x-protobuf`, decode-by-content already handled | none |
| `classify.py` | None; verified against the static routes.txt and live ids above | none |
| Fixtures + tests | Landed: `fixtures/ttc_vehiclepositions.pb` (subset re-encode of one recorded feed response, 2026-10-31) + `fixtures/ttc_routes.txt` slice; offline rule same as MTA/Metro | small |
| Cached tables | `routes_ttc.json` and `stations_ttc.json` generate from the merged zip at runtime | automatic |
| Firmware | Raised `MAX_VEHICLES` + on-wall heap measurement at ~1,600 rows. Shared with the MTA bus and CTA bus phases; that work is the gate, and this doc adds a third stakeholder | shared risk |
| Flasher | Nothing; the dropdown reads `/agencies` | none |
| Deploy | `SERVED_AGENCIES` adds `ttc` in the DigitalOcean console. No key | ops only |

## Phasing

1. The registry entry, fixtures, tests, and v1 frame landed on this
   branch; the parser needed nothing. What still waits on the shared
   cap/heap spike (the MTA bus phase owns it) is the DEPLOY step: until
   the cap moves, an enabled TTC wall silently loses rows past the
   compiled cap at peak (Saturday already exceeds it: 1,212 live rows).
2. Deploy after the cap lands: add `ttc` to `SERVED_AGENCIES` in the
   DigitalOcean console. No key, no other config. The frame was set and
   verified before deploy; composition changes (a tighter core, per-mode
   filters) are post-cap decisions behind the usual preview pass.
3. Subway: watch `gtfsrt.ttc.ca/release-notes` for a future subway
   vehicle-positions release. Nothing to build today.
4. Line 5 Eglinton and Line 6 Finch West LRT: already classified
   correctly (type 0, official colors); they light up on the wall on
   their own when they enter revenue service. No task.

## Ops and license notes

- Poll budget: 1 feed per agency per `POLL_SECONDS` = 1,440 requests/day.
  The portal documents no cap; it serves a public open-data audience.
  Same standing rule as MTA: measure sustained polling before shipping a
  wall.
- The upstream refreshes every 30 s; entity age runs 5-30 s. Nothing to
  configure.
- Static GTFS refreshes daily (the "approx. every 6 weeks" text on the
  city page is stale; the CKAN resource timestamp moves daily). The proxy
  already refreshes its routes/stations caches daily.
- Data is under the Open Government Licence - Toronto (attribution). No
  logos on the wall, so trademark use is not in play.

## Unknowns that block

1. The cap itself. Weekend live rows already exceed 1200; the weekday
   bound exceeds it by ~24 percent. Nothing else about the TTC is
   uncertain; the cap/heap spike is the whole blocker.
2. A weekday-peak live count (one probe, 16:30-17:15 EDT) and the real
   count of coupled streetcar units at peak. Both refine the number the
   raised cap must carry; neither changes the "over the cap" conclusion.
3. No documented request limit on `gtfsrt.ttc.ca`. The sustained-poll
   check before shipping settles it.
