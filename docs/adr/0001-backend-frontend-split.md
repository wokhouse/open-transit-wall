# ADR 0001: Split the API from the frontend

Status: **Proposed**

Date: 2026-10-14

## Context

The wall needs a browser provisioning tool (Web Serial API). That tool needs a
home for its HTML and JavaScript.

Today `server/proxy.py` is already API-only: `GET /vehicles` and `GET /health`.
Commit c34acf0 removed the browser preview, the `/preview` route, the `/geo`
route, `server/static/preview.html`, and the `?full=1` keyed format. Nothing
but the preview consumed them.

`server/geo.py` stays in the repo as a library. `tools/gen_backdrop.py`
imports `BBOX` from it.

## Decision

1. `server/` stays API-only. Do not re-add a browser route to it.
2. Frontend assets move to a top-level `frontend/` directory. The page reads
   its API base from a `?api=` URL parameter, and the default is same-origin.
3. Add FastAPI `CORSMiddleware` when the frontend is served from another
   origin. Same-origin breaks as soon as the page moves to another host.
4. The WebSerial provisioning tool belongs in `frontend/`. Do the split
   first, then build the tool.
5. The map bbox stays single-sourced in `server/geo.py` (`BBOX`) and
   `firmware/include/config.h` (`MAP_*`). Any browser frontend mirrors the
   projection in `firmware/src/map_project.h`. The backdrop generator reads
   the same `BBOX`. A bbox change means: edit both files, regenerate the
   backdrop, restart the proxy.

The ESP32 keeps the flat wire arrays from `/vehicles`. The arrays exist to fit
the device heap. The keyed `?full=1` format existed only for the browser
preview and left the repo with it in c34acf0. If a browser frontend returns,
add a separate keyed route to the API; do not change the wire format the
firmware reads.

## Consequences

- The server and the frontend deploy and scale apart. The frontend can live on
  a static host.
- CORS becomes a real configuration item. A misconfigured origin breaks the
  page, and the failure looks like a network error.
- Two repos or two deploy targets need the `?api=` value kept correct.
- The API surface stays small, so the ESP32 payload stays small.

## Open questions

- Where does the frontend deploy: the same DigitalOcean app as the proxy, or a
  static host?
- Does the server side of the CORS change land before the frontend exists?
- Does the provisioning tool need any server support at all, or is it a pure
  client against Web Serial?

## Update (2026-10-30)

Decision 5's dual-source bbox rule is obsolete, and its last consumer is
gone: the land/backdrop concept (`server/geo.py`, `tools/gen_backdrop.py`,
`firmware/src/backdrop.h`) was removed from the project entirely — the map
is vehicles on black. The bbox is single-sourced in
`server/agencies.json` and ships on the wire; a bbox change needs a proxy
restart only, never a reflash.

## References

- `docs/provisioning-protocol.md` — WebSerial contract.
- `docs/plan.md` — the browser provisioning tool.
- `docs/provisioning-implementation.md` — the one-binary model.
