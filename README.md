# open-transit-wall

Live transit map on a tiling LED matrix wall. Vehicle positions come from the
[511.org open data API](https://511.org/open-data/); a Waveshare
RGB-Matrix-Px-64x64 (HUB75, 1/32-scan) driven by an ESP32 renders every
active vehicle of a transit agency as a colored dot on a dark map.

```
511.org SIRI API ──HTTPS──> mini-proxy (Python) ──HTTPS JSON, pinned roots──> ESP32 ──HUB75──> LED panels
```

**One codebase, many agencies.** The server runs one or more agencies from
`server/agencies.json` (SFMTA, Caltrain, AC Transit, VTA today); each wall is
provisioned with an agency id and gets its bounding box, refresh cadence, and
vehicle colors from the wire — nothing agency-specific is compiled into the
firmware except the backdrop bitmap. See
[`docs/multi-agency-plan.md`](docs/multi-agency-plan.md).

**Colors:** the server ships a type→RGB `palette` per payload; the firmware
resolves it at parse time. One color per type code — buses **blue** ·
rapid/express **red** · metro **blue** · cable cars dim amber. Override an
agency's palette with a `"palette"` key in `server/agencies.json`.

**Scales to any wall:** everything derives from `PANEL_CHAIN_W ×
PANEL_CHAIN_H` in `firmware/include/config.h`. The project targets a single
64×64 panel; tiling more panels is the same code — just regenerate the
backdrop at the new size.

---

## Architecture

| Piece | What it does |
|---|---|
| `server/agencies.json` | The agency registry: id, display name, 511 SIRI agency code, bounding box, optional per-agency palette overrides. Agency codes come from `GET https://api.511.org/transit/operators` — never guessed. |
| `server/proxy.py` | Polls 511's SIRI VehicleMonitoring feed for each served agency (`SERVED_AGENCIES`, one shared `POLL_SECONDS`), classifies vehicles, and serves compact JSON at `GET /vehicles?agency=<id>` — plus `GET /agencies` for discovery and `/health`. The payload carries the agency's `bbox`, `next` (seconds until the server's next upstream fetch), and `palette`, so clients are configured entirely from the wire. Yard/deadhead vehicles (no route assigned) are filtered out; everything else the agency reports is served. |
| `server/classify.py` | Route → type/color logic. Standalone-importable so it can be reused elsewhere (e.g. a Pi Zero build). |
| `firmware/` | PlatformIO + Arduino ESP32 firmware. One binary for every wall: WiFi credentials, the proxy endpoint, and the agency id are provisioned at runtime over WebSerial and kept in NVS (see [Provisioning](#provisioning-over-webserial)). Fetches the proxy over HTTPS (roots pinned in `firmware/include/proxy_ca.h`), projects lat/lon → pixels using the bbox from the payload, and plots dots in the payload's palette colors. Each vehicle glides to its next known position at a random moment inside the server's advertised refresh cycle, so the wall reads as independent movement rather than one synchronized jump. |
| `tools/gen_backdrop.py` | Rasterizes the coarse shoreline for one or more agencies (`--agency sf --agency ct`) to your exact matrix size into an RLE bitmap compiled into the firmware — dark land on a black sea. The wall picks its provisioned agency's entry at boot. |
| `sim/` + `tools/run_sim.sh` | Native simulator: renders exactly what the ESP32 will draw (same projection + backdrop code) to a BMP — no hardware needed. |
| `flasher/` | The [web flasher](https://wokhouse.github.io/open-transit-wall/): a static page (no framework, vendored esptool-js) that flashes the CI-built firmware over WebSerial, then walks the provisioning protocol — discovers agencies from your server URL and writes WiFi/proxy/agency to the board. `.github/workflows/pages.yml` builds the firmware with `tools/build_firmware.py` and ships the images alongside the page, so they always match. |

### Why a proxy?

511's default token limit is **60 requests/hour**, and the raw SIRI JSON for
one agency is ~3 MB — too much to parse on an ESP32. The proxy makes one
upstream request per agency per `POLL_SECONDS` and serves a ~40 KB compact
payload that the display can refresh every few seconds. It also enables
fixture-based development with no API key.

Rate math is the operator's, not the server's: pick a `POLL_SECONDS` that
stays under your token with the agencies you serve (two agencies on a 60
req/h token → `POLL_SECONDS=125` ≈ 57.6 req/h). The proxy warns at startup
when the total would exceed the default token, but does not enforce it.

Alternatives considered (and why not, for v1) live at the bottom.

---

## Setup

### 1. Proxy

```bash
python3 -m venv .venv
.venv/bin/pip install -r server/requirements.txt
cp server/.env.example server/.env      # put your 511 API key in it
.venv/bin/python server/proxy.py        # serves on :8000
```

`server/.env` picks which agencies from `server/agencies.json` this
deployment runs (`SERVED_AGENCIES=sf,ct`) and the shared poll interval
(`POLL_SECONDS`). `GET /agencies` lists what a server supports — that is
what the web flasher reads to configure a wall.

Get a free 511 token at <https://511.org/open-data/>. Until you have one,
develop offline:

```bash
.venv/bin/python server/proxy.py --fixture fixtures/sample_vehiclemonitor.json
```

Use `--record` during live runs to save raw SIRI responses into `fixtures/`
(great for tests and future offline work).

Run the tests: `.venv/bin/python -m pytest server/tests -q`

The firmware carries no endpoint at all — the browser tool prefills the
hosted instance (`open-transit-wall-nuvez.ondigitalocean.app`) as a
convenience, and the device only learns it by provisioning. Running
`proxy.py` locally is the LAN dev path: provision `host` = this machine's
address, `scheme=http://`, `port=8000` — no rebuild.

### 2. Firmware

One binary, no build-time secrets — flash first, fill in the secrets after
over WebSerial:

```bash
pio run -e esp32dev -t upload          # build + flash (any wall, same binary)
pio run -e esp32dev -t monitor         # optional: watch the log
```

Then provision the wall: open the [web flasher](https://wokhouse.github.io/open-transit-wall/)
(Chrome/Edge over HTTPS — it also flashes the firmware over WebSerial, no
PlatformIO needed), or use any serial terminal at 115200:

```text
set ssid=MyWifi
set pass=hunter2
set host=open-transit-wall-nuvez.ondigitalocean.app
set agency=sf
save
```

`set agency=` names the wall's agency — the id from the server's
`GET /agencies`, not the 511 code. A wall only draws vehicles for its
provisioned agency, and its compiled backdrop must contain that agency's
entry (see `tools/gen_backdrop.py`).

The firmware prints a banner on connect (`OTW-FW 2` + `UNPROVISIONED` or
`STORED ...`), applies each `set` live, and `save` persists to NVS. The
full command set is specified in [`docs/provisioning-protocol.md`](docs/provisioning-protocol.md)
— the web flasher in `flasher/` is built against exactly that spec.

Libraries (auto-installed by PlatformIO):
[ESP32-HUB75-MatrixPanel-I2S-DMA](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA)
and ArduinoJson.

### Provisioning over WebSerial

The protocol is line-based text over the USB serial bridge (115200):

| you send | the wall answers |
|---|---|
| `set ssid=…` / `set pass=…` / `set host=…` / `set agency=…` | `OK …` — takes effect immediately |
| `save` | `OK saved` — persists to NVS, survives power loss |
| `get all` / `status` | current values / live health |
| `erase` / `reboot` | wipe NVS copy / restart |

A wall without an SSID **and** a host shows the backdrop and waits on the
serial console; the moment both arrive it connects on the next poll cycle —
no reflash, no reboot. `scheme`/`port` are optional extras for LAN dev
against `server/proxy.py` (empty means HTTPS/443). See
[`docs/provisioning-protocol.md`](docs/provisioning-protocol.md)
for the exact grammar, limits, and error replies.

### 3. Simulator (no hardware)

```bash
tools/run_sim.sh                                   # live: local proxy on :8000
tools/run_sim.sh fixtures/payload.json             # or a recorded /vehicles payload
AGENCY=ct tools/run_sim.sh                         # that agency's backdrop + bbox
```

Writes `/tmp/otw_wall.bmp` using the same projection, backdrop table, and
palette resolution as the firmware — use it to sanity check a new matrix size
or a new agency before committing to it. (`sim/sim_main.cpp` also takes
`--ascii` for a quick terminal view.)

---

## Scaling to a bigger wall

The project targets a single 64×64 panel (`PANEL_CHAIN_W 1`,
`PANEL_CHAIN_H 1` in `firmware/include/config.h`). The code derives
everything from those two constants, so a tiled wall is possible — it is
just untested:

1. In `firmware/include/config.h`, set `PANEL_CHAIN_W`/`PANEL_CHAIN_H` and a
   `TILE_LAYOUT` that matches your ribbon routing (`LINE_*` chains rows in one
   direction, `SERPENTINE_*` is boustrophedon — see `firmware/src/screen.h`).
2. Regenerate + reflash the backdrop at the new size:
   ```bash
   python3 tools/gen_backdrop.py --agency sf --w 128 --h 64
   ```
3. Check it in the sim (`tools/run_sim.sh`).

### Zooming

The default view is the agency's service area with true geographic aspect,
centered (`MAP_STRETCH_TO_FILL 0` in `config.h` — expect margins on panels
whose aspect differs from the map). Set it to 1 to stretch the bbox onto the
full display so the fleet reaches all four edges, at the cost of geographic
distortion. The bounding box lives in `server/agencies.json` and ships on
the wire — edit it there, regenerate the backdrop for that agency, and
restart the proxy; the wall follows the new bbox with no reflash.
Backdrops are generated with the aspect fit by default; pass `--stretch` to
`tools/gen_backdrop.py` to match a `MAP_STRETCH_TO_FILL 1` build.

---

## Wiring & power

### HUB75 pinout (ESP32-WROOM-32E, library defaults)

The 16-pin HUB75 input connector on the panel, mapped to the GPIOs this
firmware expects. GPIO assignments verified against the library's
`src/platforms/esp32/esp32-default-pins.hpp`.

| Pin | Signal | ESP32 GPIO | | Pin | Signal | ESP32 GPIO |
|----:|--------|-----------:|--|----:|--------|-----------:|
| 1 | R1 | 25 | | 9 | A | 23 |
| 2 | G1 | 26 | | 10 | B | 19 |
| 3 | B1 | 27 | | 11 | C | 5 |
| 4 | GND | GND | | 12 | D | 17 |
| 5 | R2 | 14 | | 13 | CLK | 16 |
| 6 | G2 | 12 | | 14 | LAT/STB | 4 |
| 7 | B2 | 13 | | 15 | OE | 15 |
| 8 | E (address line) | 32 | | 16 | GND | GND |

**Verify against your panel's silkscreen before powering up.** Pin layouts are
a de-facto standard and mostly agree, but pin 8 varies between panels: some
label it GND, others E/n.C. (Waveshare prints the signal names on the PCB next
to the connector.) Pins 4 and 16 are grounds on all variants.

These 64×64 panels are **1/32-scan**, so the **E address line must be wired**
to the HUB75 connector's pin 8 — `E_PIN_OVERRIDE 32` in `config.h` drives it
(the library default `-1` is only correct for 1/16-scan panels).

### Power

- **The panel needs its own 5 V supply** — Waveshare specs 5 V / 2.5 A (≤12 W)
  for the P2.5–P3 panels and up to 4 A for P4–P5, through the VH4 socket. That
  is far beyond the ESP32's regulator: never power the panel from the board's
  5 V or USB pin.
- **Common ground is required** between the panel supply and the ESP32. Connect
  ground first, disconnect it last.
- Check the VH4 terminal polarity before connecting (per Waveshare, a −5 V
  reading means the terminal is faulty). On tiled walls, inject 5 V at each
  panel rather than daisy-chaining power through the ribbon.
- The ESP32 itself can stay powered over USB while flashing — just make sure
  grounds are tied.
- Set `DISPLAY_BRIGHTNESS` in `config.h` — full-white full-brightness is far
  beyond spec; the dot map only needs modest brightness (default 48/255).

### Practical note

A bare ESP32-DevKitC has 2.54 mm headers while the panel takes a 16-pin IDC
ribbon, so you need either a HUB75 adapter/breakout board or jumper wires into
a 16-pin IDC socket. ESP32 GPIOs are 3.3 V logic; the HUB75 library is
designed for direct ESP32-to-panel connection, so no level shifting is used.

### USB serial driver

Many clone boards use a WCH CH34x bridge (vendor `0x1A86`). macOS's built-in
CH34x driver only matches product IDs `0x7523` and `0x55D4`; if your board
reports something else (e.g. `0x7522`, CH341) no `/dev/cu.*` port appears and
PlatformIO can't see the board. Install WCH's driver — `CH34XSER_MAC.ZIP` from
<https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html> — which covers
CH340/CH341 and macOS 11+. Verify you actually have a port before flashing:

```bash
ls /dev/cu.*        # expect something like /dev/cu.usbserial-XXXX or /dev/cu.wchusbserialXXXX
pio device list
```

## Attribution

Vehicle location data provided by [511](https://511.org/open-data/) and the
served transit agencies (SFMTA, Caltrain, AC Transit, VTA). Required
acknowledgment for 511 open data tokens; please keep this notice.

## Alternatives considered

- **Raspberry Pi Zero 2 W, no proxy** — viable; one Python program
  (fetch/classify/draw via
  [rpi-rgb-led-matrix](https://github.com/hzeller/rpi-rgb-led-matrix)).
  Simplest stack, but SD-card fragility.
  `server/classify.py` is written to lift onto a Pi unchanged.
- **ESP32/Pico talking to 511 directly** — needs streaming gzip + incremental
  GTFS-RT protobuf parsing (`/transit/vehiclepositions?agency=SF`). Feasible,
  much slower to iterate; keep as a future option to remove the proxy box.
- **Per-route corridors, street overlays, GTFS-RT direct mode** — future work.
