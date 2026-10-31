# Provisioning protocol (WebSerial contract)

See also `docs/provisioning-implementation.md` (the implementation behind this
spec) and `docs/plan.md` (the browser tool that will drive it).

One firmware binary is flashed to every wall (`pio run -e s3rgbmatrix -t
upload`). The WiFi network name, WiFi password, and proxy host are written to
the device afterwards, from the browser tool, over WebSerial — this is the
ONLY provisioning path: the binary carries no secrets, no endpoint, no
compiled defaults. The firmware implementation is
`firmware/src/provisioner.cpp`; this file is the spec.

## Transport

- USB serial, 115200 baud, 8N1 (the board's native USB-serial bridge).
- Commands are ASCII text lines terminated with `\n`. The firmware tolerates
  `\r\n` (it trims).
- The browser tool uses the Web Serial API: `navigator.serial.requestPort()`
  on a user click, `readable`/`writable` streams, one `TextEncoder` line
  writer. Web Serial needs Chrome/Edge over HTTPS or localhost.
- Commands produce one or more reply lines on stdout (`get all`, `status`,
  and `help` are multi-line). Replies start with
  `OK`, `ERR`, `VAL`, `STATUS`, `CMD`, or the banner prefixes below.

## Banner (firmware → tool, first thing after any serial activity)

```
OTW-FW <version>
STORED ssid=<s> pass=set|empty host=<h> scheme=<s> port=<p> agency=<a> mhost=<m> mport=<p> muser=<u> mpass=set|empty bmin=<i> bmax=<i>
```

or, when no SSID is stored in NVS:

```
OTW-FW <version>
UNPROVISIONED
```

The tool uses `OTW-FW` to recognize the device and `UNPROVISIONED` /
`STORED` to decide whether to show the setup form prefilled or empty.

## Commands

| Command | Reply | Effect |
|---|---|---|
| `set ssid=<network name>` | `OK ssid=<s> ...` | live value; WiFi reconnects on the next attempt |
| `set pass=<password>` | `OK pass set ...` | live value |
| `set host=<proxy host>` | `OK host=<h> ...` | host only — no scheme, no port, no spaces |
| `set scheme=https://` \| `http://` | `OK scheme=...` | optional; empty means `https://` |
| `set port=<1-65535>` | `OK port=...` | optional; empty means `443` |
| `set agency=<registry id>` | `OK agency=<a> ...` | lowercase `[a-z0-9]{1,8}`; the id from the server's `/agencies` list. Empty is rejected — clearing requires `erase` |
| `set mhost=<broker host>` | `OK mhost=<h> ...` | optional manual MQTT broker override (see `docs/brightness-plan.md`); same host rules as `host`. Empty (the default) means find the broker via mDNS when credentials are set |
| `set mport=<1-65535>` | `OK mport=...` | optional; only used with a manual `mhost`; empty means `1883` |
| `set muser=<username>` | `OK muser=<u> ...` | optional; 0–63 chars |
| `set mpass=<password>` | `OK mpass set ...` | optional; 0–63 chars, never echoed back |
| `set bmin=<0-255>` | `OK bmin=<i> ...` | optional brightness floor; empty means 0. See the brightness window below |
| `set bmax=<0-255>` | `OK bmax=<i> ...` | optional brightness ceiling; empty means 255 |
| `get ssid` / `host` / `scheme` / `port` / `agency` / `mhost` / `mport` / `muser` / `bmin` / `bmax` | `VAL <key>=<value>` | current live value (`ssid` replies `VAL ssid EMPTY` when unset; `port` replies `VAL port=0`, and 0 is never a valid port; `mport` replies `VAL mport=0` when unset — 0 means the 1883 default; `bmin`/`bmax` reply the effective 0–255 values, defaults 0/255) |
| `get pass` / `get mpass` | `VAL pass SET len=<n>` or `VAL pass EMPTY len=0` | the password is never echoed back |
| `get all` | twelve `VAL` lines | |
| `status` | several `STATUS ...` lines | fw version, provisioned flag, proxy URL, MQTT broker + link state, WiFi state + IP, free heap |
| `save` | `OK saved` | persist the live values to NVS (survives power loss) |
| `erase` | `OK erased ...` | wipe the NVS copy; `ssid`/`pass` also clear in the live session (back to provisioning mode), `host`/`scheme`/`port`/`agency`/`mhost`/`mport`/`muser`/`mpass`/`bmin`/`bmax` stay until reboot |
| `reboot` | `OK rebooting` | `esp_restart()` |
| `help` | `CMD ...` lines | command list |

Unknown command → `ERR unknown command (try: help)`. Unknown key → `ERR
unknown key ...`. Bad value → `ERR <key> must be ...`.

## Value rules

- `ssid`: 1–32 chars. `pass`: 0–63 chars (open networks take an empty
  value). `host`: 1–127 chars, no `/`, `:`, or spaces. `mhost`: same
  rules as `host`, but empty is allowed and turns the HA link off.
  `muser`/`mpass`: 0–63 chars. `bmin`/`bmax`: integers 0–255, empty
  restores the default (0 and 255); `bmin` may not exceed the current
  `bmax` and vice versa. Line length ≤ 256.
- Values run to end of line; no quoting. A `set` takes effect immediately in
  the running session; `save` is what persists it. The tool's flow is:
  `set ssid=…`, `set pass=…`, `set host=…`, `save`, then read the next
  `WiFi: connecting…` / `connected, ip=` lines from the firmware log as the
  success signal.
- Storage: NVS namespace `otw`, keys `ssid`, `pass`, `host`, `scheme`,
  `port`, `agency`, `mhost`, `mport`, `muser`, `mpass`, `bmin`, `bmax`.
  An empty stored `ssid` means unprovisioned. The brightness the wall last
  applied from HA is also in namespace `otw` (key `bright`) — see
  `docs/brightness-plan.md`; `erase` clears it too.

## Readiness — no compiled defaults

The device is ready only when an SSID **and** a host are set (live or
stored). `scheme`/`port` may stay empty — empty means the HTTPS/443 default
at fetch time. Until ready, the wall shows the setup-needed screen and
waits on the serial console. The tool should prefill the hosted proxy URL
(`transit.fungj.net`) as a UI convenience; the
device itself knows no endpoints.

## Firmware behavior around provisioning

- Incomplete at boot (no SSID or no host in NVS): the wall shows the
  setup-needed screen, does not touch WiFi, and answers serial commands. Once an SSID
  and a host arrive, the next poll cycle connects — no reboot needed.
- Provisioned at boot: normal operation; the serial console still works, so
  a wrong password can be fixed live and `save`d.
- `scheme`/`port` exist for LAN dev against `server/proxy.py`
  (`scheme=http://`, `port=8000`). The tool UI only needs the three core
  fields; leave the extras at their defaults.
- `mhost`/`mport`/`muser`/`mpass` are never required for readiness. With
  no credentials and no host the wall behaves exactly as before. With an
  MQTT username (or a host) it also keeps an MQTT connection for Home
  Assistant (see `docs/brightness-plan.md`). The broker host is found by
  itself: the wall browses mDNS for `_mqtt._tcp` (a broker that
  advertises itself, real port) and falls back to `_home-assistant._tcp`
  (the Mosquitto add-on shares the HA host; port 1883). `set mhost=...`
  overrides discovery for brokers elsewhere. A live `set muser=...`
  connects without a reboot; clearing both disconnects.
- `bmin`/`bmax` bound the brightness the panel actually applies to any
  Home Assistant level: HA 0% lands on `bmin`, HA 100% on `bmax`, linearly
  between. The HA slider and the state topics keep the raw 0–255 level;
  only the panel output is squeezed. Unset (0/255) is the identity. A live
  `set bmin=...`/`set bmax=...` takes effect on the next HA brightness or
  on/off command without a reboot.

## Versioning

`OTW-FW <version>` is a single integer, bumped only on breaking protocol
changes. Current: 5.

Version 5 changes the meaning of an empty `mhost`: with credentials set
the wall now finds the broker via mDNS instead of leaving the link off,
and `status` reports `mqtt=auto(<found host>)`. The wire commands and
fields are unchanged from v4; the tool should present credentials-only
setup when the banner reports `OTW-FW >= 5` and keep the manual broker
fields as an advanced override.

Version 4 added the optional `bmin`/`bmax` keys and the matching
`STORED`/`get all` fields. Older firmware (v3) answers `ERR unknown key`
for them, so the tool should only show the brightness-window fields when
the banner reports `OTW-FW >= 4`.

Version 3 added the optional `mhost`/`mport`/`muser`/`mpass` keys and the
matching `STORED`/`get all`/`status` fields. Older firmware (v2) answers
`ERR unknown key` for them, so the tool should only show the MQTT fields
when the banner reports `OTW-FW >= 3`.
