# open-transit-wall

Live SFMTA transit map on a tiling LED matrix wall. Vehicle positions come
from the [511.org open data API](https://511.org/open-data/); a Waveshare
RGB-Matrix-Px-64x32 (HUB75) driven by an ESP32 renders every bus, rapid bus,
and metro train in the city as a colored dot on a dark SF map.

```
511.org SIRI API ──HTTPS──> mini-proxy (Python) ──compact JSON on LAN──> ESP32 ──HUB75──> LED panels
```

**Colors:** buses **blue** · rapid/express buses **red** · metro trains in
their **official Muni line color** (from GTFS `route_color`, e.g. N Judah
blue, J Church yellow) · cable cars dim amber.

**Scales to any wall:** everything derives from `PANEL_CHAIN_W ×
PANEL_CHAIN_H` in `firmware/include/config.h`. One panel (64×32) or a 2×2
tiled wall (128×64) is the same code — just regenerate the backdrop.

---

## Architecture

| Piece | What it does |
|---|---|
| `server/proxy.py` | Polls 511's SIRI VehicleMonitoring feed for SFMTA every 65 s on a shared timer, classifies vehicles, serves compact JSON at `GET /vehicles`, plus `/preview` (browser view of the wall), `/geo`, `/health`. Yard/deadhead vehicles (no route assigned) are filtered out. Set `TYPES=M` in `server/.env` to serve a metro-only wall (or any subset of `B R M C`). |
| `server/classify.py` | Route → type/color logic. Standalone-importable so it can be reused elsewhere (e.g. a Pi Zero build). |
| `firmware/` | PlatformIO + Arduino ESP32 firmware. Fetches the proxy, projects lat/lon → pixels, blits the backdrop, plots dots. Each vehicle glides to its next known position at a random moment inside the ~60s data window, so the wall reads as independent movement rather than one synchronized jump. |
| `tools/gen_backdrop.py` | Rasterizes the coarse SF shoreline to your exact matrix size into an RLE bitmap compiled into the firmware — dark land, darker water, and a white outline along the coastline. |
| `sim/` + `tools/run_sim.sh` | Native simulator: renders exactly what the ESP32 will draw (same projection + backdrop code) to a BMP — no hardware needed. |

### Why a proxy?

511's default token limit is **60 requests/hour**, and the raw SIRI JSON for
all of SFMTA is ~3 MB — too much to parse on an ESP32. The proxy makes the
single upstream request per minute and serves a ~40 KB compact payload that
the display can refresh every few seconds. It also gives you the browser
preview and fixture-based development with no API key.

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

Get a free 511 token at <https://511.org/open-data/>. Until you have one,
develop offline:

```bash
.venv/bin/python server/proxy.py --fixture fixtures/sample_vehiclemonitor.json
```

Use `--record` during live runs to save raw SIRI responses into `fixtures/`
(great for tests and future offline work).

Then open the preview: <http://localhost:8000/preview?w=64&h=32&scale=8>
(`w`/`h` = your display size, `scale` = zoom).

Run the tests: `.venv/bin/python -m pytest server/tests -q`

### 2. Firmware

```bash
pio run -e esp32dev -t upload          # build + flash
pio run -e esp32dev -t monitor         # watch it poll
```

1. Copy `firmware/include/secrets.example.h` → `firmware/include/secrets.h`
   (gitignored) and fill in WiFi + the proxy host's LAN IP.
2. Libraries (auto-installed by PlatformIO):
   [ESP32-HUB75-MatrixPanel-I2S-DMA](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA)
   and ArduinoJson.

### 3. Simulator (no hardware)

```bash
curl -s localhost:8000/vehicles | .venv/bin/python tools/vehicles_to_csv.py > /tmp/v.csv
tools/run_sim.sh /tmp/v.csv build/sim_out 64x32 128x32
```

Writes `build/sim_out_<W>x<H>.bmp` (plus an ASCII view in the terminal) using
the same projection, backdrop, and palette as the firmware — use it to sanity
check a new matrix size before committing to it.

---

## Scaling to a bigger wall

1. In `firmware/include/config.h`, set:
   ```c
   #define PANEL_CHAIN_W 2
   #define PANEL_CHAIN_H 1
   #define TILE_LAYOUT   LINE_TOP_LEFT_DOWN   // match your ribbon routing
   ```
2. Regenerate + reflash the backdrop (any size works):
   ```bash
   python3 tools/gen_backdrop.py --w 128 --h 64
   ```
3. Check it in the sim or at `/preview?w=128&h=64`.

Supported tile layouts: `LINE_*` (rows chained in one direction) and
`SERPENTINE_*` (boustrophedon) — see `firmware/src/screen.h`.

### Zooming

The default view is the SFMTA metro service area with true geographic aspect,
centered (`MAP_STRETCH_TO_FILL 0` in `config.h` — expect margins on panels
whose aspect differs from the map). Set it to 1 to stretch the bbox onto the
full display so the fleet reaches all four edges, at the cost of geographic
distortion. To change the view, edit the bounding box in **both**
`server/geo.py` (`BBOX`) and `firmware/include/config.h` (`MAP_*`), then
regenerate the backdrop and restart the proxy. Backdrops are generated with
the aspect fit by default; pass `--stretch` to `tools/gen_backdrop.py` to
match a `MAP_STRETCH_TO_FILL 1` build.

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
| 8 | E (unused here) | — | | 16 | GND | GND |

**Verify against your panel's silkscreen before powering up.** Pin layouts are
a de-facto standard and mostly agree, but pin 8 varies between panels: some
label it GND, others E/n.C. (Waveshare prints the signal names on the PCB next
to the connector.) Pins 4 and 16 are grounds on all variants.

These 64×32 panels are **1/16-scan**, so the **E address line is not used** —
leave it unconnected. Only 1/32-scan panels (64×64) need E, via
`E_PIN_OVERRIDE` in `config.h` (library default is `-1`).

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
- **Border stroke**: `GRID_BORDER_RGB` in `config.h` lights the perimeter
  cells so the wall's extent reads in the dark; `{0,0,0}` disables it. The
  preview draws the same stroke (`?grid=0` hides it).

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

Vehicle location data provided by [511](https://511.org/open-data/) · SFMTA.
Required acknowledgment for 511 open data tokens; please keep the footer in
`/preview` and this notice.

## Alternatives considered

- **Raspberry Pi Zero 2 W, no proxy** — viable; one Python program
  (fetch/classify/draw via
  [rpi-rgb-led-matrix](https://github.com/hzeller/rpi-rgb-led-matrix)).
  Simplest stack, but SD-card fragility and no browser-preview dev loop.
  `server/classify.py` is written to lift onto a Pi unchanged.
- **ESP32/Pico talking to 511 directly** — needs streaming gzip + incremental
  GTFS-RT protobuf parsing (`/transit/vehiclepositions?agency=SF`). Feasible,
  much slower to iterate; keep as a future option to remove the proxy box.
- **Per-route corridors, street overlays, GTFS-RT direct mode** — future work.
