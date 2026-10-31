# CTA, TriMet, and Metro Transit as agencies: feasibility scope

Status: scoped 2026-10-31. CTA implemented 2026-10-31 as an 'L'-trains-only
agency (`cta` in `server/agencies.json`); the implementation is described at
the end of the CTA section. All live probes ran 2026-10-31 06:02-06:05 UTC
(Friday 23:02-23:05 local, Chicago and Portland). Static numbers come from
the public GTFS zips current at that date (Last-Modified: CTA 2026-09-14,
TriMet 2026-10-21, Metro 2026-10-22).

## Verdict

All three agencies fit the current `gtfs-rt` source pattern. The wire
contract, the firmware render path, and the flasher do not change for any
of them.

One small parser change helps all three. `gtfs_rt.parse_rt` today requires
a `stop_id` and ignores coordinates. These feeds are position-first. The
parser must take `position.lat/lon` when present, and snap to a station
only as a fallback.

Agency-by-agency:

1. Metro Transit (Minneapolis-St Paul): ready now. Its GTFS-RT feeds are
   public, keyless, geolocated, and fresh. The fleet fits under
   `MAX_VEHICLES 1200` at every measured and bounded count.

2. TriMet (Portland): feasible. The real-time feeds need a free
   self-service AppID. The static zip is keyless. The fleet fits with
   large margin.

3. CTA (Chicago): feasible for an 'L'-trains-only wall after an API key
   issue. The old keyless CTA feed host is dead. Rail peak is bounded at
   256 train runs, far under the cap. A wall that includes CTA buses is
   an MTA-bus-class fleet (peak bound 3,026). It inherits the
   raised-cap and on-wall heap work that the MTA bus phase already owns.

Measured detail is below. The unknowns that block each agency are named at
the end.

## Measured live feeds (2026-10-31 06:02-06:05 UTC)

### Metro Transit - keyless, geolocated, fresh

- VehiclePositions: `https://svc.metrotransit.org/mtgtfs/vehiclepositions.pb`
  returned HTTP 200 with no key (5,188 bytes). Note: the content-type is
  `application/octet-stream`, not protobuf. The fetch code must decode by
  content, not by header.

- Feed header age 11 s at probe. Their page states refreshes every 5-15 s.

- 55 VehiclePosition entities at 23:03 CT Friday. All 55 carried
  `route_id`, `trip_id`, and a stable `vehicle.id` (a flat fleet number,
  for example `1657`). 49 carried coordinates and a `stop_id`. 6 carried
  neither. The current parser keeps 49 of 55. A position-first parser
  keeps 49 and can snap any future stop-only records.

- `route_id` values are bare GTFS ids (`3`, `14`, `922`). Every probed id
  resolved against the static `routes.txt`, and `parse_routes_txt` keys
  the routes cache by both `route_id` and short name. `classify` needs no
  change for this agency.

- Identity for glide: `vehicle.id` is a persistent bus number. Prefer it
  over `trip_id` when present.

- Static GTFS: `https://svc.metrotransit.org/mtgtfs/gtfs.zip`, keyless,
  27.2 MB, weekly updates. 126 routes (3 light rail, 123 bus), 112 with
  `route_color`. Blue Line `901` ships `00539F`, Green Line `902` ships
  `008144`. 8,328 stops, all with coordinates.

- The feed is multi-operator: Metro Transit, Met Council, Maple Grove,
  Plymouth Metrolink, SouthWest Transit, MAC, and the University of
  Minnesota share one feed and one static zip. One `metro` agency entry
  that serves them all fits the project's no-type-filter rule.

### TriMet - real-time gated by a free AppID, static keyless

- VehiclePositions: `https://developer.trimet.org/ws/V1/VehiclePositions`
  returned HTTP 403 with the exact body:

  ```text
  A valid appID is required to access this resource.
  ```

- Registration at developer.trimet.org/appid is self-service and free.
  Their stated default allowance is 1,000,000 requests per appID per day.

- Their GTFS page states the VehiclePositions feed takes the same request
  parameters as the Vehicle Location service. That service is geolocated
  (lat, lon, bearing) and supports type and area filters. A keyed probe
  must confirm this. A URL-side viewport filter, if it works, is a free
  server-side fleet cut.

- Static GTFS: `https://developer.trimet.org/schedule/gtfs.zip`, keyless,
  43.5 MB. All 82 routes carry `route_color`. The MAX colors are Red
  `C41F3E`, Blue `1359AE`, Green `008342`, Orange `D05F27`, and Yellow
  `FFC52F`. WES commuter rail is route_type 2. Portland Streetcar lines
  ride the same feed as type 0. The Portland Aerial Tram is route_type 6.
  `classify` maps type 6 to plain bus blue today. If you want the Tram
  cable-amber, add `6` to `CABLE_TYPES`: one line.

- The zip carries three agencies (TriMet, Portland Streetcar, Aerial
  Tram) in one file. One `trimet` registry entry that includes all of
  them is the honest shape. Same reason as Metro.

### CTA - keyless host dead, new portal requires a key

- The old keyless host `embed.transitchicago.com/gtfs/rt/...` resets the
  connection. It is dead. Transitland's mirror fails with "no matching
  secret found". CTA moved its real-time feeds behind a key.

- New portal (marked Beta): `https://transitdata.transitchicago.com/`.
  The real paths are `/GtfsRealtime/VehiclePositions.pb` (one combined
  bus+rail feed), `/GtfsRealtime/TripUpdates.pb`, and
  `/GtfsRealtime/ServiceAlerts.pb`, with `.json` variants and a `?key=`
  parameter.

- Trap for the fetch code: without a key the portal returns HTTP 200 with
  an XML error body. The body says `errCd 100 "Required parameter 'key'
  is missing."` A dummy key gets `errCd 101 "Invalid API key."`.
  `raise_for_status()` cannot see the failure. The parser must treat an
  XML error document as a failed poll.

- Keys come from the CTA Developer Center application form. Review is
  manual. The Train Tracker API documents a default of 50,000 requests per
  day. The GTFS-RT portal states no quota.

- Static GTFS: `https://www.transitchicago.com/downloads/sch_data/google_transit.zip`,
  keyless, 68.7 MB. 132 routes, all colored. The 8 'L' lines are
  route_type 1 (subway), each with its official color. Rail route ids are
  `Red`, `Org`, `Brn`, and `P`. Rail short names are empty, so the routes
  cache keys land as `RED`, `ORG`, and `BRN`. The current `M:<LINE>`
  palette path works. All 124 bus route ids equal their short names, and
  the current rapid pattern already matches CTA `35R` and `20X` ids.

#### CTA as implemented (2026-10-31)

The `cta` registry entry serves the 'L' only, as the fleet bound above
requires. The new machinery it needed is generic gtfs-rt registry data,
not CTA code:

- `route_types: [1]` — `compact_vehicles` drops vehicles whose static
  route type is not listed. Without it the mixed feed's ~2,700 buses
  blow past `MAX_VEHICLES 1200`. `classify` now carries the static
  `route_type` through to that check. Unset on every other agency: the
  no-type-filter rule stands.
- `api_key_env: CTA_API_KEY` + `api_key_param: key` — the proxy reads
  that env var (see `server/.env.example`) and appends the key to the
  feed URL. The key never reaches the wire or the logs: `redact()`
  masks every configured key.
- The XML-error trap is closed in `gtfs_rt.parse_rt`: an XML document
  where a protobuf was expected raises with CTA's own `errCd`/`errNm`
  (`fixtures/cta_error_missing_key.xml`, `cta_error_invalid_key.xml`
  are the recorded portal responses).
- `allowance_note` is echoed at startup with what this deployment's
  `POLL_SECONDS` would spend against it.
- One probe correction: the static zip's stops.txt does carry
  `stop_lat`/`stop_lon` — the first routes refresh cached 11,138
  stations live on 2026-10-31.

What is not proven yet: a poll with a real Developer Center key. A key
supplied 2026-10-31 is sent correctly (`?key=` gets `errCd 101`, not
the keyless `errCd 100`) but both the portal `.pb` and `.json` routes
reject its value: "Invalid API key." Re-test after the Developer
Center registration completes.

## Fleet size against MAX_VEHICLES 1200

The late-night live count is not a fleet number for the cap. For a bound,
each static zip gets swept for the maximum simultaneous trips in service
on one date (Friday 2026-10-30). The sweep reads calendar dates and
exceptions. Each in-service trip stands in for one vehicle, so the count
over-counts: real overlaps and short turns share buses. Treat it as a hard
upper bound.

| Agency | Live at probe | Static peak bound (time) | Fits 1200? |
|---|---|---|---|
| Metro Transit | 55 vehicles (23:03 CT, Friday) | 970 (16:18 CT: 798 metro buses, 101 connector/suburban buses, 19 rail) | yes |
| TriMet | not measurable (key) | 637 (16:31 PT: 579 TriMet buses, 35 MAX trains, 21 streetcars, 2 tram cars) | yes |
| CTA rail-only | not measurable (key) | 256 rail runs (16:24 CT) | yes |
| CTA all | not measurable (key) | 3,026 (2,770 buses + 256 rail, 16:24 CT) | no - MTA-bus-class work |

The wire payload was measured, not estimated: the real Metro payload built
from the live probe (49 rows, full envelope with bbox and palette) is
1,904 bytes. That is 35.3 bytes per row. Scaled: a peak Metro wall is
about 35 KB, a peak TriMet wall about 23 KB, and a peak CTA rail wall
about 11 KB. The shipped MTA subway payload (503 rows, 19.5 KB) already
proved that range on a wall. The fleet-cap logic holds: anything at CTA
bus scale belongs to the MTA bus project, not a registry edit.

## Viewport sizes for later composition

The viewport crop is its own task (see the viewport history on `mta`). Raw
network extents from the static zips, for scale only:

- Metro: all stops span 44.71-45.33 N, -93.73..-92.81 W (72 km E-W with
  the suburban operators). The light-rail network alone spans 15 km E-W
  with 75 stations. A city-core frame near 25-30 km captures most peak
  buses.

- TriMet: all stops span 45.29-45.64 N, 61 km E-W. Rail spans 44.6 km
  E-W with 240 stations. The lines reach far into the suburbs.

- CTA: the 'L' spans 41.72-42.07 N, -87.90..-87.61 W. It is taller than
  wide: about 39 km N-S and 23 km E-W. O'Hare sits northwest, 95th
  Street south, and Howard north. A true square near 40 km loses nothing.

## Per-component changes

| Component | Change | Size |
|---|---|---|
| `agencies.json` | Three `gtfs-rt` entries (Metro keyless; TriMet and CTA keyed). A new optional field `api_key_env` names the env var whose value the proxy appends to each feed URL | small |
| `gtfs_rt.py` | Position-first placement: use `position.lat/lon` when present, else resolve `stop_id` through the station table. Prefer `vehicle.id` over `trip_id` for identity. Decode any content-type. Treat an XML error body inside HTTP 200 as a failed poll (CTA) | ~30 lines |
| `proxy.py` | Pass the optional key into the gtfs-rt fetch. At startup, state the per-agency request allowances where documented (TriMet 1M/day, CTA 50k/day) | small |
| `classify.py` | None for all three; verified against each static routes.txt above. Optional one line only if you want the Aerial Tram to render cable-amber | none |
| Fixtures + tests | Record the Metro feed bytes now (already probed). Record TriMet and CTA after their keys. Same offline rule as MTA | small |
| Cached tables | `routes_<id>.json` and `stations_<id>.json` generate from each static zip at runtime (first poll, refreshed daily); gitignored | automatic |
| Firmware | No change for any TriMet, Metro, or CTA-rail wall: all bounds sit under the compiled 1200 cap. A CTA bus wall joins the MTA bus phase (raise the cap, measure heap on hardware) | none / shared risk |
| Flasher | Nothing; the dropdown reads `/agencies` | none |
| Deploy | `SERVED_AGENCIES` adds the ids in the DigitalOcean console. TriMet and CTA also need their key set in the app env | ops only |

## Phasing

1. Metro Transit first. No key, everything measured, the fleet fits, and
   the work exercises the position-first parser change. Registry entry,
   the parser change, and a fixture test: about half a day, plus the
   viewport pass.

2. TriMet second. Register a free AppID (minutes). Run one keyed probe to
   confirm the position fields and the `vehicle.id` shape. Then repeat
   step 1. Half a day.

3. CTA rail-only third. Apply for the Developer Center key (manual
   review). Probe once to confirm the combined feed's fields. Then repeat
   step 1, because step 1 already added the parser safety for the CTA
   error document. Half a day.

4. CTA bus and MTA bus together, last. One heap spike gates both. Raise
   `MAX_VEHICLES` once, for both at the same time.

## Ops and license notes

- Poll budget: these sources have no 511-style 60 req/h trap. One feed
  per agency per `POLL_SECONDS` is 1,440 requests/day. That is
  negligible against TriMet's 1M/day and CTA's 50k/day. Metro states no
  cap. `POLL_SECONDS` stays operator-chosen and shared.

- All three agencies publish their data for public use under their own
  terms. The wall shows no agency logos, so trademark use is not in play.

- CTA's portal is labeled "Beta Testing". Watch it during the first month
  of any CTA wall.

## Unknowns that block

1. The CTA and TriMet real-time field shapes (coordinate coverage,
   `vehicle.id` stability across polls). The key issue blocks both. Each
   one closes with one 10-minute probe after the key arrives.

2. A weekday-daylight live fleet count for Metro. The 970 static bound
   over-counts, and the 55 late-night count under-counts. One probe on a
   weekday at 07:00-08:00 or 16:00-17:00 CT settles it. It will not
   change the cap conclusion, because even the bound fits.

3. Whether the TriMet GTFS-RT endpoints take the area and type request
   parameters that their XML service documents. A keyed probe must
   confirm this.
