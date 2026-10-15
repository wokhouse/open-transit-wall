# Web flasher plan

Status: implemented. A static page on GitHub Pages that flashes the
ESP32 firmware over Web Serial, then provisions the board (WiFi, proxy URL,
agency) by speaking the protocol in `docs/provisioning-protocol.md`. No
build toolchain on the user's machine: open the URL, plug in the board.
Live at https://wokhouse.github.io/open-transit-wall/ (Pages build type
`workflow`, `.github/workflows/pages.yml`).

Supersedes the earlier flasher WIP (OTWCFG-framed protocol, stashed on
`main` as `stash@{0}`): the protocol it spoke no longer exists. The
firmware speaks the plain line protocol (`set`/`get`/`save`/`status`,
banner `OTW-FW 2`), and that is what this page must speak.

## Where it lives

All new files (the ones the old WIP had — `tools/build_firmware.py`,
`.github/workflows/pages.yml` — went into the stash with it):

- `flasher/` — static site: `index.html`, `app.js`, `style.css`,
  `vendor/esptool-js/` (vendored bundle, no npm at runtime).
- `tools/build_firmware.py` — new. Runs `pio run -e esp32dev`, copies the
  four images into `flasher/firmware/` with a `manifest.json` (offsets,
  sizes, sha256, version string `main-<sha7>`). Also writes
  `open-transit-wall-merged.bin` for manual `esptool write_flash 0x0`
  recovery.
- `.gitignore`: add `flasher/firmware/` (generated, never committed).
- Web Serial needs Chromium (Chrome/Edge) on HTTPS; Pages gives us both.
  The page shows a clear "open in Chrome" message elsewhere.

## Hosting and publishing

- GitHub Pages, deployed by Actions from the `flasher/` directory. One-time
  repo setting: Pages source = "GitHub Actions".
- The firmware images ship **inside the Pages artifact**, so the page and
  the binaries it offers are always the same commit. `manifest.json`
  carries the version; the page shows it and compares against the board's
  `OTW-FW` banner.
- **No GitHub Release.** A release would duplicate the same binaries and
  invite drift against the page. Anyone flashing manually with esptool can
  grab `open-transit-wall-merged.bin` straight from the Pages URL
  (`/open-transit-wall/firmware/...`). Revisit only if we ever need
  versioned permalinks for other reasons.

## Server prerequisite (small patch, do first)

The page runs on `wokhouse.github.io` and fetches `GET /agencies` from
whatever server URL the user types — cross-origin. `server/proxy.py`
currently sends no CORS headers, so the fetch fails. Add
`CORSMiddleware` (allow all origins, GET only) to the FastAPI app:
`/agencies`, `/vehicles`, `/health` are read-only public data. One test
that the preflight succeeds.

## Phase 1 — flash

1. "Connect" button → `navigator.serial.requestPort()`.
2. esptool-js: enter bootloader via DTR/RTS, detect chip (expect ESP32),
   write at 460800:
   `0x1000 bootloader.bin · 0x8000 partitions.bin · 0xe000 boot_app0.bin ·
   0x10000 firmware.bin` (paths and sizes from `manifest.json`).
3. Verify with the manifest's sha256 where esptool-js supports it; show a
   progress bar per image.
4. Hard reset out of the bootloader; the app starts printing at 115200.

## Phase 2 — provision

The page speaks the line protocol exactly as specified — send one command
per line, read until a reply line starting with `OK`/`ERR`/`VAL`/`STATUS`
(`get all`/`status`/`help` are multi-line; there is no per-command
terminator, so read with an idle timeout).

1. Read the boot banner: `OTW-FW <n>` + `STORED ...`. If `n < 2`, warn
   "board runs old firmware — flash it first" and offer to go back to
   phase 1.
2. Server URL field (default: the hosted proxy). Fetch `GET /agencies`
   from it; populate the agency dropdown with `id` + `name`. If the fetch
   fails, allow typing an agency id by hand.
3. WiFi network/pass, host, scheme, port — prefilled from the `STORED`
   line. Client-side validation mirrors the spec (lengths, `set agency`
   `[a-z0-9]{1,8}`) so errors show before hitting the board.
4. Send `set ...` for each changed field, then `save`, then read `status`
   back and show it. The password is typed into the form and sent over
   the wire only — never written to localStorage.
5. `reboot`, then tail the serial console so the user watches the board
   connect and start polling. A "console" toggle stays available for
   debugging (raw line passthrough, same framing rules).

`erase` gets a confirm dialog: it wipes NVS and drops the board back to
provisioning mode.

## CI

`.github/workflows/pages.yml`:
- Triggers: push to `main` touching `firmware/**`, `flasher/**`,
  `tools/build_firmware.py`, or the workflow itself; plus `workflow_dispatch`.
- Steps: cache `~/.platformio` keyed on `firmware/platformio.ini` (same key
  as `ci.yml`), `pip install platformio`, `python3 tools/build_firmware.py
  --version "$GITHUB_REF_NAME-$(git rev-parse --short=7 HEAD)"`, then
  `upload-pages-artifact` with `path: flasher` + `deploy-pages`.
- `ci.yml` stays as-is (lint/tests/cppcheck/build). The pages workflow
  reuses the build step; no firmware is flashed in CI — Web Serial can't
  be tested headless, so the page itself is verified manually.

## Page structure

Plain HTML + one ES module, no framework:
- `index.html` — three stacked steps (flash → configure → done), each
  disabled until the previous completes; a status log panel.
- `app.js` — `flash()` and `provision()` as above; a tiny line reader
  (TextDecoder + buffer split on `\n`, idle-timeout resolve).
- `style.css` — matches the repo README's plain-document look.

## Edge cases

- Board already provisioned: phase 2 prefills and can skip phase 1
  ("configure only" path — connect, read banner, edit, save).
- Flaky boards: flash baud falls back to 115200 on failure, with a hint.
- Server URL is user input: validate `http(s)://` before fetching; the
  board's own `set host` validation is the authority.
- Multi-line replies: never assume one command = one line (the spec says
  so; the old WIP's app.js did — do not copy that part).

## Test plan

- Manual, one 64×64 wall: fresh flash → provision `sf` against the hosted
  proxy → vehicles appear; then agency swap `sf`→`ct` → backdrop mismatch
  warning path; then `erase` → re-provision.
- CI: pages workflow must produce a manifest whose sha256s match the
  staged files (build_firmware.py asserts this before staging).
