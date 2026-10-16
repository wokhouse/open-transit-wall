# Plan: the browser provisioning tool

Status: **Not built.** The firmware side is done and spec'd. The tool does not
exist yet.

## Why

A wall has no keyboard and no screen. The user must set the WiFi network name,
the WiFi password, and the proxy host after the flash. A browser page with the
Web Serial API is the chosen path. See
`docs/adr/0001-backend-frontend-split.md`: the tool lives in `frontend/`, and
the split lands first.

Until the tool exists, any serial terminal at 115200 baud works. The README
shows the `set` / `save` sequence.

## Design

- Use the Web Serial API. Call `navigator.serial.requestPort()` inside a click
  handler. A page cannot open a port without a user gesture.
- Write commands with one `TextEncoder` line writer. Terminate every line with
  `\n`.
- Web Serial needs Chrome or Edge, and it needs HTTPS or localhost.
- Read the banner first. `OTW-FW <n>` identifies the device; `UNPROVISIONED`
  and `STORED ...` decide whether the form starts empty or prefilled.
- Prefill the hosted proxy URL as a UI convenience. The device holds no
  endpoint of its own.
- Send three core fields: `ssid`, `pass`, `host`. Then `save`. Leave `scheme`
  and `port` at their defaults; they exist for LAN development only.
- Report success from the firmware log lines: `WiFi: connecting…`, then
  `connected, ip=`.

The exact grammar, the length limits, and the error replies are in
`docs/provisioning-protocol.md`. Build against that file.

## Work items

1. Land the frontend split (ADR 0001). Create `frontend/`.
2. Build the page: port picker, three fields, Connect, Save, and a log pane.
3. Handle the error replies (`ERR unknown key ...`, `ERR <key> must be ...`).
4. Test against a real board on Chrome: unprovisioned boot, live `set`,
   `save`, and a reboot that keeps the config.
5. Optionally serve `frontend/` from the hosted app beside the API.
