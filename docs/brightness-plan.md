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
an MQTT broker is provisioned (`set mhost=...` over WebSerial — see
`docs/provisioning-protocol.md`). With a broker configured:

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

Four optional keys, same `set`/`get`/`save` semantics as the existing
ones (`docs/provisioning-protocol.md` is the spec):

| key | meaning |
|---|---|
| `mhost` | broker host; empty = HA link off |
| `mport` | broker port; empty = 1883 |
| `muser` | MQTT username (optional) |
| `mpass` | MQTT password (never echoed back, like the WiFi password) |

`fw_version` is 3 from this change. Walls on older firmware reject the
unknown keys (`ERR unknown key`) and keep working unchanged.

## Home Assistant side

Nothing to configure beyond having an MQTT broker (the add-on is fine).
Once the wall connects, HA shows "Transit Wall <id>" as a light with a
brightness slider. Availability comes from the LWT, so the entity goes
unavailable the moment the wall drops off the broker.

## Out of scope

- Color/temperature: the panel's look is the transit palette, not a
  user-selectable color.
- Schedules/automations logic: that is HA's job once the entity exists.
- TLS brokers: LAN-only plain MQTT for now; `mhost` takes a bare host.
