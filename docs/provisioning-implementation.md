# The one-binary provisioning model

One firmware binary runs on every wall. The binary holds no WiFi credentials,
no compiled defaults, and no endpoint. You provision the wall after the flash,
over WebSerial.

`docs/provisioning-protocol.md` is the wire spec. This file describes the
implementation behind it.

## Rules of the model

- WebSerial is the ONLY configuration path. There is no `secrets.h` and no
  endpoint in the binary. `firmware/include/config.h` has no `PROXY_*`
  defines.
- Flash the binary first, then set the credentials:

  ```bash
  pio run -e esp32dev -t upload
  ```

- The browser tool prefills the hosted app URL
  (`transit.fungj.net`) as a UI convenience only.
  The device itself knows no endpoint until you set one.

## Implementation

| Item | Detail |
|---|---|
| Code | `firmware/src/provisioner.cpp` (interface: `provisioner.h`) |
| Storage | NVS namespace `otw`, keys `ssid`, `pass`, `host`, `scheme`, `port`, `agency`, `mhost`, `mport`, `muser`, `mpass`, `bmin`, `bmax` (plus `bright`, the last Home Assistant brightness, owned by `ha_link.cpp` — see `docs/brightness-plan.md`) |
| Live values | RAM copies in the running session. `save` writes them to NVS. |
| Effect of `set` | Immediate. No reboot and no reflash. |
| Banner | `OTW-FW <n>` plus `UNPROVISIONED` or `STORED ...` |
| Commands | `set k=v`, `save`, `get`, `status`, `erase`, `reboot` |
| Broker host | An empty `mhost` with `muser` set makes `ha_link.cpp` ask `ha_discover.cpp` for the broker over mDNS (`_mqtt._tcp`, then `_home-assistant._tcp`). A non-empty `mhost` skips discovery. |

## Readiness

`provNeeded()` is true when the SSID is unset OR the host is unset (live value
or stored value). While `provNeeded()` is true:

- The wall shows the setup-needed screen.
- The wall does not touch WiFi.
- The wall answers serial commands, so a terminal can complete the setup.
- `loop()` returns before the poll, so no fetch starts.

Empty `scheme` and empty `port` mean `https://` and `443` at fetch time. The
`scheme`/`port` fields exist for LAN development against `server/proxy.py`
(`scheme=http://`, `port=8000`).

A wall with a complete config connects on the next poll cycle. No reboot is
needed.

## Projection start

`projection.begin()` runs in `loop()` on the first ready cycle, not in
`setup()`. This one placement covers both paths: provisioned at boot, and
provisioned live from the serial console.

## Related

- `docs/provisioning-protocol.md` — exact grammar, limits, and error replies.
- `docs/plan.md` — the browser tool that will drive this protocol.
- `README.md` — the user-facing setup steps.
