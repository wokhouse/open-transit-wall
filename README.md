<p align="center"><img src="assets/logo.svg" width="160" alt="open transit wall logo"></p>

# Open Transit Wall

Live transit map on a tiling LED matrix wall. Vehicle positions come from the
[511.org open data API](https://511.org/open-data/); a Waveshare
RGB-Matrix-Px-64x64 (HUB75, 1/32-scan) driven by an ESP32-S3 board renders
every active vehicle of a transit agency as a colored dot on a dark map.
Supported boards: Waveshare ESP32-S3-RGB-Matrix (default) and Adafruit
MatrixPortal S3.

```
511.org SIRI API ──HTTPS──> mini-proxy (Python) ──HTTPS JSON, pinned roots──> ESP32-S3 ──HUB75──> LED panels
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
| `firmware/` | PlatformIO + Arduino ESP32 firmware. One binary for every wall: WiFi credentials, the proxy endpoint, and the agency id are provisioned at runtime over WebSerial and kept in NVS (see [Provisioning](#provisioning-over-webserial)). Fetches the proxy over HTTPS (roots pinned in `firmware/include/proxy_ca.h`), projects lat/lon → pixels using the bbox from the payload, and plots dots in the payload's palette colors. Each vehicle glides to its next known position at a random moment inside an equal slot of the server's advertised refresh cycle, so the wall reads as independent movement spread evenly over the cycle rather than one synchronized jump or chance clumps. |
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

One binary per board, no build-time secrets — flash first, fill in the
secrets after over WebSerial:

```bash
pio run -e s3rgbmatrix -t upload       # Waveshare ESP32-S3-RGB-Matrix
pio run -e matrixportal-s3 -t upload   # Adafruit MatrixPortal S3
pio run -e <env> -t monitor            # optional: watch the log
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

The firmware prints a banner on connect (`OTW-FW 4` + `UNPROVISIONED` or
`STORED ...`), applies each `set` live, and `save` persists to NVS. The
full command set is specified in [`docs/provisioning-protocol.md`](docs/provisioning-protocol.md)
— the web flasher in `flasher/` is built against exactly that spec.

Libraries (auto-installed by PlatformIO):
[ESP32-HUB75-MatrixPanel-I2S-DMA](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA),
ArduinoJson, and PubSubClient (only used when an MQTT broker is provisioned).

### Provisioning over WebSerial

The protocol is line-based text over the USB serial bridge (115200):

| you send | the wall answers |
|---|---|
| `set ssid=…` / `set pass=…` / `set host=…` / `set agency=…` | `OK …` — takes effect immediately |
| `set muser=…` / `set mpass=…` (optional) | `OK …` — Home Assistant MQTT credentials; the wall finds the broker itself; see [Brightness from Home Assistant](#brightness-from-home-assistant) |
| `set bmin=…` / `set bmax=…` (optional) | `OK …` — brightness floor/ceiling: the HA slider's 0% dims only to `bmin`, its 100% reaches only `bmax` |
| `save` | `OK saved` — persists to NVS, survives power loss |
| `get all` / `status` | current values / live health |
| `erase` / `reboot` | wipe NVS copy / restart |

A wall without an SSID **and** a host shows the backdrop and waits on the
serial console; the moment both arrive it connects on the next poll cycle —
no reflash, no reboot. `scheme`/`port` are optional extras for LAN dev
against `server/proxy.py` (empty means HTTPS/443). See
[`docs/provisioning-protocol.md`](docs/provisioning-protocol.md)
for the exact grammar, limits, and error replies.

### Brightness from Home Assistant

If your home runs Home Assistant with its Mosquitto add-on, just give the
wall an MQTT username and password (`set muser=…` / `set mpass=…` — create
a dedicated HA user). The wall finds the broker itself by browsing mDNS for
the Home Assistant host, connects over the LAN it is already on, and
announces itself through Home Assistant MQTT auto-discovery: a real
**light entity** with a brightness slider, usable in scenes and
automations, with no HA YAML. A broker elsewhere still works: `set mhost=`
/ `mport=` override the discovery. The wall keeps its own brightness in
NVS, so it comes back at the last setting after a power cut. With no
credentials provisioned this whole path stays off — nothing changes. The
proxy is not involved: it serves many walls, and brightness belongs to the
room.
`set bmin=<0-255>` / `set bmax=<0-255>` (the web flasher shows them as
percent) squeeze what the panel physically does: the slider's 0% lands on
the floor, its 100% on the ceiling, so the wall never goes fully dark or
blazing bright no matter what an automation asks for. Turning the light
off blanks the map, but the status pixels — green/amber/red for
data freshness, purple while the Home Assistant link is up — stay lit at
about 1% so a dark wall still shows it is alive.
Design and topics: [`docs/brightness-plan.md`](docs/brightness-plan.md).

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

### Boards

Both supported boards have an onboard HUB75 connector — no adapter board or
jumper wires. The PlatformIO env and the web flasher's board dropdown pick
the same pin map (`firmware/include/config.h`).

- **Waveshare ESP32-S3-RGB-Matrix** (default, env `s3rgbmatrix`): the HUB75
  connector sits behind the panel mounting area. 64×64 panels are 1/32-scan,
  so the E address line must be driven; the board's connector pin 8 is E and
  `config.h` drives it on GPIO9.
- **Adafruit MatrixPortal S3** (env `matrixportal-s3`): front-facing HUB75
  output. For 64×64 (1/32-scan) panels, close the board's E-line solder
  jumper toward the pad marked **8** (it ships set for 64×32 panels). This
  target builds with PSRAM off on purpose: the panel lines include
  GPIO35-37, which the module's octal PSRAM would claim.

### Power

- **The panel needs its own 5 V supply** — Waveshare specs 5 V / 2.5 A (≤12 W)
  for the P2.5–P3 panels and up to 4 A for P4–P5. Feed it through the board's
  5 V power input, which carries panel current to the HUB75 connector; never
  power the panel from USB.
- **Common ground is required** between the panel supply and the board.
  Connect ground first, disconnect it last.
- Check the power terminal polarity before connecting (per Waveshare, a −5 V
  reading means the terminal is faulty). On tiled walls, inject 5 V at each
  panel rather than daisy-chaining power through the ribbon.
- The board itself can stay powered over USB while flashing — just make sure
  grounds are tied.
- Set `DISPLAY_BRIGHTNESS` in `config.h` — full-white full-brightness is far
  beyond spec; the dot map only needs modest brightness (default 48/255).
  With an MQTT broker provisioned, the live value comes from Home Assistant
  instead and this constant is only the boot fallback (see
  [Brightness from Home Assistant](#brightness-from-home-assistant)).

### USB serial

Both boards expose the ESP32-S3's native USB (no USB-UART bridge), so no
driver install is needed — the port shows up as vendor `303A` (Espressif) or
`239A` (Adafruit). The web flasher talks to it over WebSerial. If a flash
attempt can't connect, hold the board's BOOT button, reset or replug, and
release once the ROM bootloader takes the port. Verify you actually have a
port before flashing:

```bash
ls /dev/cu.*        # expect something like /dev/cu.usbmodemXXXX
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
