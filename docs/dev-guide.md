# open-transit-wall developer guide

Live SFMTA transit map on a tiling HUB75 LED matrix wall. A Python mini-proxy
polls the 511 SIRI feed and serves compact JSON. An ESP32 firmware renders
every bus, rapid bus, and metro train as a colored dot.

Repo: `/home/fungj/projects/open-transit-wall` (worktrees under
`~/.paseo/worktrees/0pedc9uz/<name>`).

Note: this file is the content intended for a repo-root `AGENTS.md`. A write
to `AGENTS.md` was denied by a protected-file guard, so the content lives here
until an operator moves it.

Read `README.md` and the `docs/` files listed at the end before you change
code.

## Commands

Server venv (once):

```bash
python3 -m venv .venv
.venv/bin/pip install -r server/requirements-dev.txt
```

Run the server:

```bash
# offline: recorded feed, no 511 key
.venv/bin/python server/proxy.py --fixture fixtures/sample_vehiclemonitor.json

cp server/.env.example server/.env   # add the 511 token for live data
.venv/bin/python server/proxy.py     # live; serves on :8000
```

Verify the server:

```bash
.venv/bin/python -m pytest server/tests -q   # fixture-based; no 511 key
ruff check server tools                      # CI runs this exact command
```

Firmware (one binary for every wall):

```bash
pio run -d firmware -e esp32dev            # build
pio run -d firmware -e esp32dev -t upload  # flash
pio run -d firmware -e esp32dev -t monitor # log at 115200
```

No system `pio`? Use a venv:

```bash
python3 -m venv /tmp/piovenv && /tmp/piovenv/bin/pip install platformio
/tmp/piovenv/bin/pio run -d firmware -e esp32dev
```

cppcheck (mirrors CI; the local cppcheck is newer than CI's, so it needs the
`PROGMEM` define):

```bash
cppcheck --quiet --error-exitcode=1 --suppress=unusedFunction -DPROGMEM= \
  -I firmware/include -I firmware/src firmware/src firmware/include
```

Simulator (needs `clang++`; it is absent on bingbong):

```bash
curl -s localhost:8000/vehicles | .venv/bin/python tools/vehicles_to_csv.py > /tmp/v.csv
tools/run_sim.sh /tmp/v.csv build/sim_out 64x32 128x32
```

Backdrop (run after a map bbox or matrix size change):

```bash
python3 tools/gen_backdrop.py --w 128 --h 64    # writes firmware/src/backdrop.h
```

## Structure

| Path | Contents |
|---|---|
| `server/proxy.py` | FastAPI mini-proxy. Polls 511 every 65 s. Owns the API. |
| `server/classify.py` | Route -> type/color logic. Importable on its own (e.g. a Pi build). |
| `server/siri.py` | SIRI VehicleMonitoring parser. |
| `server/geo.py` | Map bbox + land polygon. Library only: no server route uses it. `tools/gen_backdrop.py` imports it. |
| `server/routes.json` | Cached GTFS route types and colors. |
| `firmware/` | PlatformIO + Arduino ESP32. `include/config.h` holds map and panel constants. |
| `firmware/src/provisioner.cpp` | WebSerial provisioning. NVS namespace `otw`. |
| `firmware/src/map_project.h` | lat/lon -> pixel projection. |
| `sim/`, `tools/run_sim.sh` | Native simulator: same projection and backdrop, no hardware. It compiles no firmware `main`, so it is unaffected by a firmware change. |
| `tools/gen_backdrop.py` | Shoreline -> RLE bitmap at the matrix size. |
| `fixtures/` | Recorded SIRI responses for offline work. |

## Boundaries

- `server/` is API-only: `GET /vehicles` (flat wire arrays for the ESP32) and
  `GET /health`. Do not re-add `/preview`, `/geo`, or `?full=1`. Commit
  c34acf0 removed them, and nothing consumed them. See
  `docs/adr/0001-backend-frontend-split.md`.
- The firmware carries no secrets, no endpoint, and no compiled defaults.
  WebSerial is the only configuration path.
- The panel takes its own 5 V supply. Never power the panel from the board.
- Keep the upstream poll at one request per minute or slower (511 token
  limit: 60 requests/hour).
- CI runs on push to `main` and on pull requests: ruff, pytest, cppcheck, and
  `pio run -d firmware -e esp32dev`.

## MUST / MUST NOT

- MUST read `docs/provisioning-protocol.md` before you touch the firmware
  serial console. It is the spec.
- MUST keep `platform = espressif32@7.1.3` in `firmware/platformio.ini`. The
  CI cache key includes the pin, so a bump forces a full rebuild.
- MUST `chmod +x` every new script in `tools/`. Ruff rule EXE001 fails CI on a
  shebang without the execute bit (a `tools/prov_serial.py` without it broke
  main CI for 7 runs on 2026-10-15).
- MUST restart `server/proxy.py` after a change to `server/geo.py`.
- MUST keep `MAP_*` in `firmware/include/config.h`, `BBOX` in
  `server/geo.py`, and the backdrop in step. Change all three together.
- MUST NOT trust a return value from `setCACert`. On arduino core 3.x it
  returns `void` (core 2.x returned `bool`). Call `lastError(buf, size)` and
  check for `"OK"`.
- MUST NOT use `HardwareSerial::timeout()`. It is not public on core 3.x.
  Assemble lines from `Serial.read()` in a byte loop.
- MUST NOT call `projection.begin()` from `setup()`. Call it in `loop()` on the
  first ready cycle, or the provisioned-live-from-serial path misses it.
- MUST NOT add a build-time secret or a compiled endpoint to the firmware.
- MUST NOT re-add a browser preview route to the server.
- MUST NOT call the live 511 API from a test or from CI. Use `--fixture`.
- MUST NOT power the panel from the ESP32 5 V pin or from USB.

## Docs

- `docs/provisioning-protocol.md` — the WebSerial contract (the spec).
- `docs/provisioning-implementation.md` — how the one-binary model works.
- `docs/adr/0001-backend-frontend-split.md` — API-only server split, status
  Proposed.
- `docs/plan.md` — the browser provisioning tool. Not built yet.
