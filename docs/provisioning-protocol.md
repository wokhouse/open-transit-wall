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
STORED ssid=<s> pass=set|empty host=<h> scheme=<s> port=<p> agency=<a>
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
| `get ssid` / `host` / `scheme` / `port` / `agency` | `VAL <key>=<value>` | current live value (`ssid` replies `VAL ssid EMPTY` when unset; `port` replies `VAL port=0`, and 0 is never a valid port) |
| `get pass` | `VAL pass SET len=<n>` or `VAL pass EMPTY len=0` | the password is never echoed back |
| `get all` | six `VAL` lines | |
| `status` | several `STATUS ...` lines | fw version, provisioned flag, proxy URL, WiFi state + IP, free heap |
| `save` | `OK saved` | persist the live values to NVS (survives power loss) |
| `erase` | `OK erased ...` | wipe the NVS copy; `ssid`/`pass` also clear in the live session (back to provisioning mode), `host`/`scheme`/`port`/`agency` stay until reboot |
| `reboot` | `OK rebooting` | `esp_restart()` |
| `help` | `CMD ...` lines | command list |

Unknown command → `ERR unknown command (try: help)`. Unknown key → `ERR
unknown key ...`. Bad value → `ERR <key> must be ...`.

## Value rules

- `ssid`: 1–32 chars. `pass`: 0–63 chars (open networks take an empty
  value). `host`: 1–127 chars, no `/`, `:`, or spaces. Line length ≤ 256.
- Values run to end of line; no quoting. A `set` takes effect immediately in
  the running session; `save` is what persists it. The tool's flow is:
  `set ssid=…`, `set pass=…`, `set host=…`, `save`, then read the next
  `WiFi: connecting…` / `connected, ip=` lines from the firmware log as the
  success signal.
- Storage: NVS namespace `otw`, keys `ssid`, `pass`, `host`, `scheme`,
  `port`, `agency`. An empty stored `ssid` means unprovisioned.

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

## Versioning

`OTW-FW <version>` is a single integer, bumped only on breaking protocol
changes. Current: 2.
