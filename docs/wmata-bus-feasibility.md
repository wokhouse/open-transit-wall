# WMATA Metrobus as an agency: feasibility scope

Status: scoped and shipped 2026-10-31, as a combined rail+bus entry
(PR #14). Live probe ran 2026-10-31 22:05 UTC (Saturday 18:05 ET, evening
shoulder). Static numbers come from the keyed bus GTFS
zip current at that date (`feed_version S1000251`, service window
2026-09-13 to 2027-03-27; the zip's internal file timestamps are
2026-08-28).

## Verdict

Feasible, and it needs no parser work: the bus feed is the same
position-first shape the Metro Transit entry already handles, and the
keyed feed plus keyed static zip are already wired for the `wmata` rail
entry. Two registry entries against one upstream key is legitimate, and
one combined entry works too — see "One entry or two" below.

The question is not whether it works, it is whether the fleet fits. It
does, barely:

| | Value |
|---|---|
| Live at the probe (Saturday 18:05 ET) | 635 vehicles, 569 in service |
| Weekday peak scheduled | 812 concurrent trips at 17:00 |
| Live / scheduled ratio at the probe | 1.15 |
| Estimated weekday peak rows | ~934 |
| `MAX_VEHICLES` (compiled) | 1200 |
| Headroom | ~22 percent |

That puts a WMATA bus wall 1.7x the shipped MTA subway wall (~550 rows,
19.5 KB) and about half the MTA bus feed (1,836 rows, 69 KB) that the
project has never parsed on hardware. The one real engineering risk is
the on-wall heap at ~950 rows, which is a measurement, not a redesign.

Three smaller decisions are in the scope: the viewport (the bus network
is 56 km wide, against 39 km for the rail system), whether the wall uses
the four route-color families WMATA publishes, and whether the entry
ships at all given WMATA's terms of use.

## Measured live feed (2026-10-31 22:05 UTC)

`https://api.wmata.com/gtfs/bus-gtfsrt-vehiclepositions.pb?api_key=...`
returned HTTP 200, `application/octet-stream`, 61,735 bytes.

- Feed header 28.6 s behind the request, so this feed is slower than the
  rail one (9-10 s). A bus wall polls it at 35-65 s and nobody can tell.
- 635 VehiclePosition entities. All 635 carry `position.lat/lon` and a
  `vehicle.id`; 569 also carry a `stop_id` and a trip.
- 569 of the 635 carry a `trip_id`, and every one of those resolves
  against the static `trips.txt`. The other 66 are pull-ins, layovers and
  yard moves with no route at all — the existing no-route drop rule
  removes them, 10.4 percent of the feed.
- `vehicle.id` is a four-digit fleet number (`4728`), and `label` repeats
  it. Identity for glide works unchanged.
- `route_id` is the GTFS route id (`M54`, `C63`, `P93`, `D80`), 104
  distinct at the probe, every one present in the static `routes.txt`.
- `bearing` is on 293 of 635 rows only; the wall does not use it.

Wire size, measured: 569 rows serialize to 20,043 bytes of `[lat, lon,
key, id]` arrays, 35.2 bytes per row for the bus half and 36.3 bytes per
row for a measured combined payload (22,163 B / 611 rows). At the
~1,039-row peak the payload lands near 37 KB.

## Static GTFS — keyed, and the largest download in the project

`https://api.wmata.com/gtfs/bus-gtfs-static.zip?api_key=...` returned
HTTP 200, 34,961,134 bytes. Eleven files at the zip root, including two
WMATA-only extras (`timepoints.txt`, `timepoint_times.txt`). Sizes that
matter: `stop_times.txt` 177 MB uncompressed, `shapes.txt` 18 MB,
`trips.txt` 4.9 MB, `stops.txt` 468 KB, `routes.txt` 6 KB.

- `routes.txt`: 126 rows, every one `route_type 3` (bus) and every one
  colored, in four families — `0091c8` on 51 routes, `005594` on 32,
  `735fa9` on 22, `d83a1f` on 21. No route long name contains EXPRESS or
  RAPID, and no route id matches the numeric-R pattern, so `classify`'s
  rapid path never fires here.
- `stops.txt`: 7,540 rows, spanning 38.694-39.156 N (51.4 km) and
  -77.366 to -76.720 W (55.9 km). Against the rail system's 39.3 x 56.0
  km, the bus network grows the vertical axis by 12 km and is 0.1 km
  narrower east-west, which is why its square frame crops differently.
- `calendar.txt` exists (unlike the rail feed) with eight service
  patterns and day columns, plus 58 `calendar_dates.txt` exceptions that
  replace weekday service with a Saturday pattern on holidays. Any fleet
  sweep must apply both.
- 98,429 trips, 4,176,900 `stop_times` rows.

The proxy reads only `routes.txt` and `stops.txt` out of that zip. The
download is 34.9 MB once a day per boot cycle, which is the largest
static fetch in the project and worth a note in the deploy docs rather
than a code change.

## Fleet size against MAX_VEHICLES 1200

Each in-service trip stands in for one bus, so a max-concurrent-trips
sweep over the static timetable is a hard upper bound.

| Date | Service | Peak concurrent trips | Time |
|---|---|---|---|
| Sat 2026-10-31 | Saturday pattern | 527 | 17:00 |
| Sun 2026-11-01 | Sunday pattern | 502 | 17:00 |
| Tue 2026-11-03 | weekday pattern | 812 | 17:00 |
| Wed 2026-11-04 | weekday pattern | 816 | 17:00 |

The static number under-counts the live fleet, measured directly: at
22:05 UTC on a Saturday the schedule says 495 trips are in progress and
the feed reports 569 buses in service, a ratio of 1.149.

So write the peak as a function of that one measured ratio, not as a
point:  rows(k) = 105 + 812k, where 105 is the rail peak at 08:43 and 812
is the bus peak at 17:00. At the measured k = 1.15 that is ~1,039 rows;
the compiled 1200 cap holds for any k below 1.35, a 17 percent error on
the only ratio sample we have, taken on a Saturday evening. Using the
sweep's maximum (816 on Wed 2026-11-04) instead gives ~1,043 rows and a
break-even of 1.34. Summing two different peak hours is a bound, not a
forecast: the combined wall's true peak will be at or below it.

Compare the shipped walls: MTA subway 503-584 trains per poll at 19.5 KB,
proven on hardware; SFMTA Muni ~676 vehicles. A WMATA bus wall is the
largest wall this project would serve that still fits the current cap.

## One entry or two: the Muni model

SFMTA arrives as one 511 operator and one entry, so Muni Metro lines key
as `M:<LINE>` and Muni buses as `B` on one wall. WMATA can be shaped the
same way, and the feed plumbing for it already exists: a registry entry's
`feeds` is an object, `fetch_station_snapshot` loops all of them, and
`compact_vehicles` classifies each row on its own route. Measured with
both recorded probes merged through one route table:

| | Combined `wmata` entry |
|---|---|
| Rows at the probe | 634 (65 rail + 569 bus) |
| Style keys | `M:RED`, `M:BLUE`, `M:GREEN`, `M:YELLOW`, `M:ORANGE`, `M:SILVER`, `B` |
| Wire payload | 22,689 B (35.8 B/row) |
| Estimated peak rows | ~1,039 |
| Estimated peak payload | ~35.7 KB |
| Headroom to `MAX_VEHICLES` 1200 | ~13 percent |
| In the rail B box | 62/65 trains (the Loudoun clip) and 558/569 buses |

That is the Muni palette shape on one wall, with one viewport that needs
no new tuning: the shipped rail B box keeps 98 percent of the buses.

The static GTFS was the blocker at scoping time: an entry carried one
`static_gtfs`, and WMATA's rail routes (`route_type 1`) and bus routes
(`route_type 3`) live in two separate zips. With only the bus zip loaded, the six rail lines
would classify as generic bus blue. Two ways out:

1. WMATA documents a Rail & Bus Combined GTFS Static API (portal
   operation `rail-bus-combined-gtfs-static`, daily updates). The gateway
   path is not in any public document reachable from here — every
   guessed `/gtfs/*.zip` variant answers 404, and the portal's API page
   is rendered client-side. One look in the signed-in API console settles
   it. If that zip exists at a stable URL, the combined entry is a
   registry edit with no code at all.
2. Let `static_gtfs` accept a list of zips and merge route tables and
   station ids across them. ~15 lines in `refresh_routes` plus tests, and
   generic to any agency whose modes ship separate static feeds.

Cost of combined: two feed requests per poll (2,658/day, still 5 percent
of the allowance) and two static downloads a day (3.0 MB + 34.9 MB)
unless the combined zip is used. The real cost is the fleet: ~1,039 rows
puts the heap gate at 35.7 KB, which is the largest payload this project
would ever parse on a wall, and leaves 13 percent to the compiled cap.

Trade-off, stated plainly:

- Combined (Muni model): one wall shows trains and buses together, one
  viewport, palette reads exactly like Muni's. Fleet math gets tight and
  downtown density goes up.
- Separate entries as scoped: the subway wall keeps its proven 19.5 KB,
  the bus wall can take a tighter, denser frame, and each fleet stays
  well inside measured territory.

Jade chose combined on 2026-10-31. Option 2 shipped (the `static_gtfs`
list), so the wall does not depend on finding WMATA's combined zip: both
zips are fetched and merged on the daily static refresh (24 h, retried
after 10 minutes on failure).

## Viewport candidates

The network is 51.4 km north-south by 55.9 km east-west, and the
aspect-fit projection scales by the larger span, so a candidate box is
sized by its east-west side. The traffic is not spread evenly: at the
probe the live mass sat at 38.9111 N, -77.0199 W, essentially downtown. Five
candidate frames were measured from one payload:

| | Box | Side | m/px | Buses in frame | Stops in frame | Dot span | Mass center |
|---|---|---|---|---|---|---|---|
| A | lat 38.694-39.156, lon -77.366--76.720 | 55.9 km | 873 | 569/569 | 7540/7540 | x 0-61, y 2-60 | 33.2, 32.8 |
| B | the rail B box | 45.9 km | 717 | 562/569 | 7303/7540 | x 0-63, y 0-60 | 39.6, 35.7 |
| **C** | lat 38.7315-39.0908, lon -77.2508--76.7890 | 40.0 km | 625 | **552/569** | 6943/7540 | x 0-63, y 0-59 | 31.6, 31.0 |
| D | lat 38.7674-39.0549, lon -77.2046--76.8352 | 32.0 km | 500 | 530/569 | 6315/7540 | x 0-62, y 0-63 | 32.4, 31.4 |
| E | lat 38.7882-39.0218, lon -77.1701--76.8699 | 26.0 km | 406 | 500/569 | 5583/7540 | x 0-63, y 0-62 | 32.8, 31.1 |

Recommendation: **C**, 40 km at 625 m/px, mass-centred by construction
(dot mass lands at 31.6, 31.0 against a panel centre of 31.5). It keeps
97 percent of the live fleet and 92 percent of the stops.

- B is the interesting alternative: it is the exact frame the rail wall
  ships, so a bus wall and a subway wall would line up pixel for pixel.
  At the 22:05 probe it keeps 562 of 569 buses (98.8 percent); on the
  combined 23:35 probe 14 of 545 bus rows were outside, 97.4 percent.
- D and E trade fleet for detail, down to 88 percent at 26 km. Downtown
  DC bus density at 406 m/px reads as a solid blob on a 64 px panel.
- Every cropped candidate cuts the network at the border, and buses move
  continuously, so those buses pop in and out at the edge as they cross.
  That is unavoidable with a crop, and it is the reason the MTA subway
  wall can clip branch tips and still look right: a subway dot snaps
  station to station, a bus dot slides.
- The server does not filter rows by bbox today; off-frame rows still
  ship and the wall simply does not draw them. At 3 percent that is
  ~1 KB of wasted payload, not worth a code change.

## Colors: four families the wall cannot show

All 569 rows classify to the single generic `B` key, so a bus wall paints
every route the same blue. WMATA publishes four route-color families
(51/32/22/21 routes). Three ways to handle it:

1. Ship all-blue. Zero code, honest about the data the pipeline uses
   today, and consistent with how every 511 agency already renders.
2. Use the published colors. `classify` ignores `route_color` for bus
   routes because one color per type is the project's standing default.
   A registry flag (`bus_route_colors: true`) that keys buses as
   `B:<ROUTE>` would light the four families, at the cost of a palette
   with 126 more entries on the wire (~2.5 KB measured) and a lookup per
   row.
   ~15 lines in `classify`/`proxy` plus tests.
3. A registry `palette` override on `B` — one color, no code, no value.

Recommendation: ship all-blue first; take option 2 only if the wall looks
flat once it is running.

## Per-component changes

| Component | Change as scoped (two entries) | Shipped instead (one entry) |
|---|---|---|
| `agencies.json` | A second entry with id `wmatabus` — `AGENCY_ID_RE` is `^[a-z0-9]{1,8}$`, so a dash is not allowed — its own feed, its own zip | The existing `wmata` entry gained the bus feed and a second `static_gtfs` URL. No new id |
| `proxy.py` | None: keyed feeds and keyed zips already worked | `static_gtfs` accepts a URL or a list and merges route/station tables; per-feed isolation on the RT side; per-zip failure reporting |
| `classify.py` | None for the all-blue cut | None. Optional `bus_route_colors` for the four colour families remains open (~15 lines) |
| Fixtures + tests | Bus VP subset + `routes.txt` + `stops.txt` subset | Shipped: `wmata_bus_vp.pb` (11 KB, 104 of 126 routes), the whole bus `routes.txt`, a 118-row `stops.txt` subset, 12 tests across the combined entry |
| Firmware | None to commit, heap measurement at the bus fleet | None to the cap; one log line added so the heap gate is measurable (`ESP.getMinFreeHeap()` beside the free figure) |
| Flasher | None: `/agencies` lists whatever the proxy serves | None. `wmata` is already in the list |
| Deploy | `SERVED_AGENCIES` gains `wmatabus` | Nothing new: `wmata` is already served; the app env keeps `WMATA_API_KEY`. One extra upstream request per poll, 34.9 MB more static per cold start and per day |

## Ops notes

- Allowance: the same 50,000/day Default-tier key covers the bus feed.
  Two entries at `POLL_SECONDS=65` is 2,658 requests/day, still 5 percent
  of the allowance.
- The bus feed is 28.6 s stale at the probe and the rail feed 9-10 s, so
  one shared `POLL_SECONDS` suits both at 35-65.
- The static bus zip is 10x the rail zip. A cold start does both
  downloads before the first dot appears; on the hosted proxy the first
  poll cycle took about a minute to fill in when the two agencies were
  added together.
- The 66 trip-less buses are dropped by the existing no-route rule. If a
  wall ever looks sparse at a terminus, that rule is where the buses went,
  not a feed failure.
- No firmware secret, no compiled endpoint: the key stays in the proxy's
  environment, as with the rail entry.

## Licensing

The rail scope's finding carries over unchanged: WMATA's Transit Data
Terms of Use (2019-11-19, sections 6.6 and 8) forbid serving their data
through your own API to third parties without prior written authorization
(`api-support@wmata.com`), while section 7's grant covers the operator's
own application. Bus data is the same data family under the same terms.
A wall for Jade's own panel is inside the grant. Verified 2026-10-31:
the shared hosted proxy (`transit.fungj.net`) does serve `wmata` — its
`/agencies` lists it and `/vehicles?agency=wmata` returns rows — and that
is legitimate only while the endpoint backs her own walls. It needs
WMATA's written yes before other people's walls read it.

Attribution: the GTFS license is CC-BY 4.0, so credit WMATA in the README
and docs, and ship no WMATA marks.

## What shipped (2026-10-31)

PR #14, folded into the existing `wmata` entry rather than a second id:

- `server/agencies.json`: `wmata` gained the bus VehiclePositions feed and
  a two-entry `static_gtfs` list (rail zip, bus zip); name now reads
  "WMATA (DC Metrorail + Metrobus)". Frame unchanged.
- `server/proxy.py`: `static_gtfs` accepts one URL or a list
  (`load_registry` normalises, `AgencyState` stores a tuple) and
  `refresh_routes` downloads each zip, merging route and station tables.
  The shared key rides every zip request. The route-count log line now
  counts distinct routes instead of half the dict length, which
  under-reported for the 126 bus routes whose id equals their short name.
- `fixtures/wmata_bus_vp.pb` (11 KB, 108 entities covering 104 of the 126
  routes plus three out-of-frame rows and one trip-less row),
  `fixtures/wmata_bus_routes.txt` (the whole 126-route file),
  `fixtures/wmata_bus_stops.txt` (118 rows: referenced stops plus the
  network's coordinate edges).
- `server/tests/test_wmata.py` grew 5 tests (17 in the file, 64 in the
  suite): the combined registry shape, the two-zip merge with each mode
  keeping its route type, a two-feed snapshot producing the seven-key
  palette, the bus fixture's exact edge clips, and acceptance of a single
  `static_gtfs` URL plus rejection of bad list shapes.

Verified live on 2026-10-31 23:35 UTC with the public use key:
`[routes] cached 132 WMATA (DC Metrorail + Metrobus) routes + 9716
stations from 2 zips`, 611 rows (545 bus, 66 rail), 22,163-byte payload,
seven palette keys, and 16 rows outside the frame (14 bus, 2 Silver) —
the documented edge crops. Native sim draws 595 of 611.

Open: the weekday peak probe and the on-wall heap log at ~1,040 rows.

## Phasing (as scoped)

1. Registry entry with the chosen frame, plus the fixture test. Half a
   day, and it is a registry edit plus fixtures: no Python changes.
2. Verify on live data: every served row inside or named as an edge clip,
   and the palette carries exactly one key. Quarter day.
3. Optional: the `bus_route_colors` flag if the all-blue wall reads flat.
   Half a day.
4. Weekday peak probe at 16:45-17:30 ET — the bus peak in the sweep is
   17:00, not the rail's 08:43 — plus the on-wall heap log at that fleet
   size. Take an 08:45 reading too if the combined wall is what matters,
   so the sum-of-peaks bound can be replaced by two real hours.

## Blocking unknowns

1. The weekday peak live bus count. The static bound is 812 (816 on the
   sweep's best day) and the measured live/scheduled ratio is 1.15, so the
   estimate is ~1,039 rows with a break-even at k = 1.35. One weekday
   probe at 17:00 ET settles it. Nothing in the pipeline changes if the
   answer is higher — only whether the compiled cap holds.
2. The on-wall heap at the combined peak: ~1,040 rows and ~37 KB. Read
   the minimum free heap through connect + parse + render, not the free
   heap after merge (the JSON document is already freed by then). The
   firmware now logs both (`heap` and `min`), so one weekday boot settles
   it. The largest payload proven on hardware is the MTA subway's 19.5 KB.
3. Whether the four published route-color families are wanted. A taste
   call, not a risk.
