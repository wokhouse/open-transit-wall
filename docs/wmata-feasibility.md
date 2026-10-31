# WMATA Metrorail as an agency: feasibility scope

Status: scoped 2026-10-31. Live probes ran 2026-10-31 21:08-21:14 UTC
(Saturday 14:08-14:14 PT, 17:08-17:14 ET). Static numbers come from the
rail static GTFS zip current at that date (internal file timestamps
2026-10-30 13:45).

## Verdict

Feasible, and the smallest implementation of any agency so far. The rail
feed is position-first, keyed, fresh, and carries a stable fleet number.
The fleet is 105 trains at weekday peak by static bound, a fifth of the
MTA subway. `MAX_VEHICLES 1200`, the wire contract, the render path, the
sim, and the flasher do not change.

Three things are new, none of them large:

1. The key gates the static zip too. `gtfs_rt` agencies today fetch
   `static_gtfs` with no key (MTA, Metro Transit and CTA all serve it
   keyless). WMATA answers 401 on every endpoint, feeds and static both,
   so the registry key field must reach the routes refresh as well as the
   vehicle fetch.
2. WMATA's terms of use restrict re-serving the data through your own
   API. A wall that the operator runs for herself is inside the license.
   A public multi-tenant proxy is not — see "Licensing" below. This is
   the only item on this page that can block a deployment shape.
3. Six to seven percent of the live rows are non-revenue trains on route
   `NR`, which has no entry in the static `routes.txt`. They classify as
   ordinary bus blue. One registry field, not one bug.

## Measured live feeds (2026-10-31, times UTC)

All probes used WMATA's public use key (their documented testing key,
terms section 4). A developer key is required for anything but testing.

### Rail VehiclePositions — keyed, position-first, ~10 s fresh

- `https://api.wmata.com/gtfs/rail-gtfsrt-vehiclepositions.pb?api_key=...`
  returned HTTP 200, `application/octet-stream`, 10,719-10,927 bytes
  across three probes.

- No key: HTTP 401 with a JSON body,
  `{"statusCode":401,"message":"Access denied due to missing
  subscription key..."}`. An invalid key: HTTP 401,
  `"invalid subscription key"`. Unlike the CTA portal, the failure is a
  real status code, so `raise_for_status()` sees it and no XML-body
  sniffing is needed.

- Feed freshness: header timestamp 9-10 s behind the request
  (21:12:47 header at a 21:12:56 request). Header advance was exactly
  70 s over a 70 s sleep, so the feed regenerates continuously rather
  than on a fixed batch clock. That is fresher than the MTA subway's
  30 s.

- 77 VehiclePosition entities in each of the three probes. Placement:
  77/77 carry `position.lat/lon` (100 percent). 65-68 of the 77 also
  carry a `stop_id`, and every one of those stop ids resolves against
  the static `stops.txt` platform table (`PF_G05_C` style) — zero
  misses. No station snapping is needed, but the station table exists
  anyway because the routes refresh builds it.

- Identity: `vehicle.id` is a three-digit fleet/train number (`033`,
  `207`). 76 of 77 ids survived a 70 s gap, and 60 of those moved more
  than 1 m. Glide identity works unchanged and better than the MTA's
  `trip_id` (no re-identification at terminals).

- Routes: `route_id` is the bare line name — `RED`, `BLUE`, `GREEN`,
  `YELLOW`, `ORANGE`, `SILVER` — which is exactly the static
  `routes.txt` `route_id`. Every one is `route_type 1` with an official
  `route_color`, so the existing `M:<LINE>` palette path carries the six
  real line colors with no `classify` change.

- Two row classes need a rule:
  - 4-6 of the 77 entities carry `vehicle.id` and a position but no trip
    at all, so no route. They are yard moves (`PM49`, `BV01` style
    labels). The existing no-route drop rule already removes them, the
    same way it handles TTC carhouse rows.
  - 6-7 of the routed entities carry `route_id` `NR` (non-revenue) with
    trips `NR084`, `NR089`, `NR090`, `NR093`, `NR213`, `NR223`, `NR243`,
    `NR262`. `NR` is not in the static `routes.txt` (6 routes), so they
    fall through `classify` to the generic bus-blue `B` key. See
    "Non-revenue trains" below.

### Non-revenue trains (`route_id` `NR`)

`NR` is WMATA's non-revenue class: trains on the network that carry no
passengers. The RT feed reports them like any other train — position,
fleet number, a `label` that is the internal run designator — but their
trip ids are `NR<number>`, they carry no `stop_id` and no
`current_stop_sequence`, and no `NR` route exists in the static feed.
They split into two measurable kinds at the probe:

- Moving pull-outs. Train `084` (label `831`) travelled Naylor Rd ->
  Anacostia -> Navy Yard-Ballpark across the three probes, and `089`
  (label `830`) came off the Branch Av end. These run along passenger
  track from a yard into the line.
- Parked units. Four trains — `213` (`pm50`), `223` (`T006`), `243`
  (`P47`), `262` (`TR06`) — held the same coordinates for the whole six
  minutes, at bearing 294, 0.5-1.1 km past East Falls Church. Stale or
  stabled positions, not traffic.

Recommendation: drop them. They carry no riders, four of the six draw
frozen dots on a single pixel, and the fleet cap does not need the
headroom. The clean way is a registry `drop_route_ids: ["NR"]` list
(~8 lines, mirroring the `route_types` filter the CTA entry already
carries). The zero-code alternative is a `palette` override on the `B`
key to dim gray, which recolors them but still draws them.

### Rail static GTFS — keyed, one zip, flat files

- `https://api.wmata.com/gtfs/rail-gtfs-static.zip?api_key=...` returned
  HTTP 200, 3,055,425 bytes. Ten files, all at the zip root:
  `agency, calendar_dates, routes, shapes, stop_times, stops, trips,
  feed_info, levels, pathways`.

- No `calendar.txt`. Service is resolved only through
  `calendar_dates.txt`, whose 92 rows are all `exception_type 1`
  (added). A date's active service ids are simply the entries dated that
  day, one or two of the twelve.

- `routes.txt`: 6 rows, all `route_type 1`, all colored —
  Red `C80F2D`, Blue `009CDE`, Green `00B140`, Yellow `FFD100`,
  Orange `ED8B00`, Silver `919D9D`.

- `stops.txt`: 2,176 rows. 98 `location_type 1` parent stations, 125
  type 0 platforms, 240 entrances, 1,713 generic nodes. `parse_stops`
  keeps every row with coordinates, so the station table is larger than
  the MTA's, and harmless.

- Bus static and the combined rail+bus static exist on the same portal.
  Not needed for a rail wall.

### Bus VehiclePositions — measured, out of scope

`https://api.wmata.com/gtfs/bus-gtfsrt-vehiclepositions.pb` returned
200, 63,251 bytes, 652 vehicles over 104 routes at 17:14 ET on a
Saturday, every row with coordinates and a `vehicle.id`. A WMATA bus
wall is a separate fleet question: 652 is a weekend midday count, and a
weekday peak count has not been taken. It is not part of this scope.

## Fleet size against MAX_VEHICLES 1200

| Measure | Value | Source |
|---|---|---|
| Live, Saturday 17:08 ET | 77 entities, 73 placed after the no-route drop | probe |
| Peak concurrent trips, weekday | 105 at 08:43 | static sweep |
| Peak concurrent trips, Saturday | 61 at 06:56, 81 on 2026-11-07 | static sweep |
| Trips in service, weekday | 1,699 | static sweep |

The weekday sweep resolves each in-service trip's first and last
`stop_time` and counts overlaps. Each trip stands in for one train, so
105 over-counts (short turns and overlaps share train sets). It is a
hard upper bound, and it is 8 percent of the compiled cap. Compare the
MTA subway at 503-584 trains observed.

Wire size, measured not estimated: the 71 rows of the 21:12 probe
serialize to 2,857 bytes of rows and 3,128 bytes for the whole payload
with bbox and palette. At 105 rows a peak wall is about 4 KB. That is
13 percent of the shipped MTA payload.

## Viewport

The network is wide: 98 parent stations span 39.3 km north-south
(38.7665-39.1199) and 56.0 km east-west (-77.4914 to -76.8447). A true
square at 64x64 therefore forces a choice. Two candidates were
measured, not eyeballed:

| | Box | Side | m/px | Stations in frame | Live rows clipped | In-frame dot span | Dot-mass center |
|---|---|---|---|---|---|---|---|
| **B (chosen)** | lat 38.7501-39.1364, lon -77.3403--76.8435 | 43.0 km | 672 | 92/98 | 3 (Silver, west edge) | x 12-63, y 2-60 | x 34.5, y 35.2 |
| A (rejected) | lat 38.6895-39.1969, lon -77.4940--76.8435 | 56.5 km | 883 | 98/98 | 0 | x 0-62, y 9-53 | x 41.0, y 34.2 |

Jade picked B on 2026-10-31. It crops west of Wiehle-Reston East,
dropping six stations (Reston Town Center, Herndon, Innovation Center,
Dulles Airport, Loudoun Gateway, Ashburn) and clipping the three Silver
trains that happen to be on that tail, at the west edge. It buys 24
percent more detail and lands the mass center 3 px from panel center.
Its east edge is as tight as A's (column 63), so the gain is the
vertical bands, not the horizontal margin; the westernmost in-frame
column moves to 12. The tail cannot be un-polled the way the Staten
Island Railway was: one feed carries the whole rail system, so the
crop is viewport-only and a later widening is a bbox edit.

A was rejected for the 9-10 px of black band at the top and bottom and
a dot mass 9.5 px right of panel center, with no detail gained for the
space. Recorded here because it is the box to restore if the Loudoun
tail is ever wanted back.

Both frames were rendered from one 21:08 payload, with the static-GTFS
network skeleton beside the live dots, and the pixel stats above come
from those rasters. At implementation time the box must be re-verified
on live data: every served row inside, and any row past the west edge
named as the documented Loudoun clip.

## Per-component changes

Sized against the framework on `main` plus the key-field work already
written on the open CTA and TriMet PRs.

| Component | Change | Size |
|---|---|---|
| `agencies.json` | One `gtfs-rt` entry `wmata`: `feeds` = the rail VehiclePositions URL, `api_key_env` = `WMATA_API_KEY`, `api_key_param` = `api_key`, `static_gtfs` = the rail static zip, `allowance_note` = the Default-tier limits, `bbox` = the chosen B frame (with `drop_route_ids` once decided) | small |
| `proxy.py` | Apply `api_key_env`/`api_key_param` to the `static_gtfs` fetch in `refresh_routes`, not only to the feeds (today's CTA work keys the feeds only). Optional palette override for `NR`, or a `drop_route_ids` field | ~10 lines + one test |
| `server/.env.example` | `WMATA_API_KEY` line and its role | trivial |
| `gtfs_rt.py` | None. Position-first and `vehicle.id`-first already shipped with Metro Transit | none |
| `classify.py` | None. `route_type 1` plus `route_color` gives the six official colors through the existing `M:<LINE>` path | none |
| Routes / stations cache | Built from the keyed zip at first poll, refreshed daily, gitignored. The 3 MB fetch is the largest static download in the project | automatic |
| Fixtures + tests | Record a rail VP response and re-encode a subset: header preserved, every line present, one `NR` row, one no-trip row, two rows beyond each cropping edge. Static side: `routes.txt` and a `stops.txt` subset. Test asserts parse -> classify -> bbox containment -> palette keys -> wire shape, offline | small |
| Firmware | None. Peak 105 against a compiled 1200 | none |
| Flasher | None. The dropdown reads `/agencies` | none |
| Deploy | `SERVED_AGENCIES` gains `wmata` and the app env gains `WMATA_API_KEY` in the DigitalOcean console. A public multi-tenant wall shape needs WMATA's written authorization first | ops only |

### Ordering dependency

`api_key_env` / `api_key_param` / `allowance_note` / `route_types` exist
only on the open PRs today: PR #7 (`cta-implementation`) adds the
registry key fields, PR #10 (`trimet-implementation`) sits on top of it.
Nothing on `main` can key a gtfs-rt agency. Land WMATA after #7, or
branch it off `trimet-implementation`. The static-zip key change is the
one delta neither PR carries.

## Licensing — the one deployment gate

WMATA's Transit Data Terms of Use (revised 2019-11-19) differ from every
agency in the registry so far.

- Section 7 grants a revocable license to "download, use, reproduce, and
  redistribute WMATA's Transit Data within your Application", where an
  Application is the developer's own software or service. A proxy that
  feeds the operator's own walls is inside that grant, and the API
  license explicitly covers letting your end users use your integration.

- Section 6.6 and 8(a) prohibit creating an API that "enables access to
  the Transit Data outside of the WMATA API", and prohibit sharing data
  through "multiple tiers of distribution or your own application
  programming interface" to any other person.

- Section 8(b) is the escape hatch: re-serving to third parties needs
  prior written authorization from WMATA, the operator's own key only,
  the data "inseparably commingled with or supplemented by additional
  data that you have provided", an ability to block third parties on
  request, and user data on request. Contact `api-support@wmata.com`.

Read plainly: a WMATA wall that Jade hosts for herself is fine. Exposing
`wmata` on the public hosted proxy so other people's walls consume it is
not, until WMATA says yes in writing. The `sf`, `mta`, `metro` entries
have no such restriction, so the registry can carry `wmata` while the
hosted deployment leaves it out of `SERVED_AGENCIES`.

Two smaller obligations: the GTFS license is CC-BY 4.0, so credit WMATA
in the README and the docs, and the wall shows no WMATA marks (section
7 restricts WMATA intellectual property, and the project ships no
branding anyway).

The public use key is for testing only (section 4). Fixtures may be
recorded with it; a live deployment needs the free personal key.

## Ops notes

- Allowance: the free Default tier is documented at 10 calls/second and
  50,000 calls/day. One rail feed at `POLL_SECONDS=65` is 1,329
  requests/day, plus one static zip. That is 2.7 percent of the daily
  allowance. There is no 511-style hourly token trap.
- At a 10 s feed cadence, `POLL_SECONDS` near 35 gives smoother glide,
  as on the MTA. The value is shared across agencies, so it stays an
  operator decision.
- The key rides a query string, and `httpx` puts the full URL into every
  `HTTPError`. `redact()` on the CTA branch already masks any
  `api_key_env` value; the WMATA change inherits it.
- The static zip is 3 MB and contains 16 MB of `stop_times`; only
  `routes.txt` and `stops.txt` are read. One fetch per day.
- No firmware secret and no compiled endpoint, unchanged: the key lives
  in the proxy's environment.

## Phasing

1. Register a developer account and subscribe to the free Default tier
   (it lists the GTFS API). Take one keyed probe with the personal key
   to confirm the same field shapes. Minutes, self-service.
2. Land PR #7 (registry key fields) or branch off PR #10. Add the
   static-zip key change, the `wmata` registry entry, and the fixture
   test. Half a day.
3. Verify the chosen B box on live data: every served row inside, and
   any row past the west edge named as the documented Loudoun clip.
   Quarter day.
4. Apply the `NR` rule. Recommend `drop_route_ids: ["NR"]`; a palette
   override on `B` is the zero-code fallback. Ten minutes plus a test
   asserting an `NR` row never reaches the wire.
5. Deploy to Jade's own wall. Skip the hosted multi-tenant proxy until
   WMATA authorizes it in writing.

## Blocking unknowns

1. A weekday peak live count. The static bound is 105; one probe between
   08:00 and 09:00 ET settles it. It cannot change the cap verdict.
2. Whether the free Default subscription includes the GTFS product for a
   personal key. The public use key reaches every GTFS endpoint, which
   proves the endpoints; the product list on the signup page is the
   place to confirm the subscription covers them, and the first keyed
   probe is the proof.
3. WMATA's answer on section 8(b), if a public wall is wanted.

None of the three blocks a wall for the operator's own panel.
