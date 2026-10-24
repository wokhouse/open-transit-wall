# Brightness control from Home Assistant

Status: implemented (firmware side).

## Goal

Let the operator dim the wall from Home Assistant — a real light entity
with a brightness slider, usable in scenes and automations — without
adding anything to HA's configuration beyond having an MQTT broker.

## Why the wall talks to HA directly

The proxy is one-to-many: one deployment feeds many walls, so a
brightness value held by the server is not specific to any one home.
Brightness is a property of the physical panel in a room. The wall is
already on the LAN, so it publishes its own state there. The proxy stays
transit data only.

## Design

`firmware/src/ha_link.{h,cpp}` is an optional module. It is inert unless
an MQTT username or a broker host is provisioned over WebSerial (see
`docs/provisioning-protocol.md`). Once enabled:

- Broker discovery: with no `mhost`, `firmware/src/ha_discover.{h,cpp}`
  browses mDNS — first `_mqtt._tcp` (a broker advertising itself, using
  its real SRV port), then `_home-assistant._tcp` (HA Core advertises
  itself; the Mosquitto add-on shares that host, port 1883). `set
  mhost=...` skips discovery for brokers elsewhere.

- Transport: PubSubClient over plain TCP/1883. The pinned
  `espressif32@7.1.3` platform ships arduino core 2.0.17, which has no
  built-in esp-mqtt; PubSubClient is the smallest correct option.
  Measured cost: ~1.7 KB flash, ~120 B static RAM for the library
  itself, plus a 1 KB heap buffer at runtime.
- Identity: `otw-<id>`, where `<id>` is the last 3 bytes of the WiFi
  MAC in lowercase hex. Unique per wall, zero extra provisioning.
- Home Assistant MQTT auto-discovery: one retained config on
  `homeassistant/light/otw-<id>/config` (schema `basic`, brightness
  scale 255). HA creates the light entity with no YAML.
- Topics (state-like ones retained):
  - `otw/<id>/set` — `ON` / `OFF`
  - `otw/<id>/set_brightness` — `0`..`255`
  - `otw/<id>/state` — `ON` / `OFF`
  - `otw/<id>/brightness` — `0`..`255`
  - `otw/<id>/available` — LWT `offline`; `online` on connect
- Discovery + state are (re)published on every broker connect, so a
  broker restart or re-adding the integration self-heals.
- `OFF` sets panel brightness 0 and remembers the previous level; any
  brightness command turns the panel back on.
- Persistence: the brightness value is stored in NVS (namespace `otw`,
  key `bright`) on every change. At boot the wall restores it before
  drawing anything, so it comes back at the last setting even with HA
  down. `erase` (factory reset) clears it; the fallback is then
  `DISPLAY_BRIGHTNESS` in `firmware/include/config.h`.
- `status` shows `mqtt=<host>:<port> connected|down` or
  `mqtt=(unset)`.

## Provisioning additions

Six optional keys, same `set`/`get`/`save` semantics as the existing
ones (`docs/provisioning-protocol.md` is the spec):

| key | meaning |
|---|---|
| `mhost` | manual broker override; empty = find it via mDNS |
| `mport` | broker port with a manual `mhost`; empty = 1883 |
| `muser` | MQTT username (optional) |
| `mpass` | MQTT password (never echoed back, like the WiFi password) |
| `bmin` | brightness floor, 0–255; empty = 0 |
| `bmax` | brightness ceiling, 0–255; empty = 255 |

`fw_version` is 4 from the brightness-window change (3 introduced the
`m*` keys). Walls on older firmware reject the unknown keys
(`ERR unknown key`) and keep working unchanged.

## Brightness window (floor and ceiling)

Panels differ, and a living room at 2 a.m. does not want the same floor a
kitchen gets at noon. `bmin`/`bmax` squeeze what the panel physically
does under any Home Assistant level: the slider's 0% lands on `bmin`,
its 100% on `bmax`, linearly between. The mapping lives in
`ha_link.cpp::mapBrightness()` and is applied only on the way to the
panel — the state topics and the NVS `bright` key keep the raw HA level,
so the HA slider never lies about where it is. Unset bounds (0/255) make
the mapping the identity, which is the pre-v4 behaviour.

The web flasher exposes this as "Brightness floor %"/"Brightness ceiling
%" inside the Home Assistant box and converts percent to 0–255 on the
wire, so the units match what HA shows. The web UI always offers the
full 0–100% slider; the window is a wall-side limit, not an HA limit.

## Status pixels when the panel is "off"

The status pixels (`main.cpp::drawStatusPixel()`) report state along the
top edge: (1,0) green=fresh data, amber=aging, red=error; (2,0) purple
while the MQTT link to Home Assistant is up, red when the link is
configured but down (broker not found, auth refused, WiFi lost), and
nothing when the link is not configured. Turning the light OFF — or 0%
with no floor — blanks
the map, but the frame keeps only those pixels lit at
`STATUS_PIXEL_FLOOR` (~1% panel brightness) so a dark wall still shows
it is alive and linked. Turning it back on repaints the map wholesale and
re-merges the current snapshot (`mapDirty` in `main.cpp`), so vehicles
are back immediately — no wait for the next poll. The window (`bmin`)
constrains the slider, not the OFF command: OFF always reaches the floor
frame.

## Home Assistant side

Nothing to configure beyond having an MQTT broker (the add-on is fine)
and giving the wall an MQTT username and password — the add-on requires
authentication, so create a dedicated HA user for it. The wall finds the
add-on broker itself via mDNS; no host to type. Once it connects, HA
shows "Transit Wall <id>" as a light with a brightness slider.
Availability comes from the LWT, so the entity goes unavailable the
moment the wall drops off the broker.

## Out of scope

- Color/temperature: the panel's look is the transit palette, not a
  user-selectable color.
- Schedules/automations logic: that is HA's job once the entity exists.
- TLS brokers: LAN-only plain MQTT for now; `mhost` takes a bare host.
