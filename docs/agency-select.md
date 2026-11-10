# Agency selection from Home Assistant

Status: implemented (server + firmware side).

## Goal

"Change the transit wall to SFMTA." A voice command in Home Assistant
switches which agency a wall displays, with no re-provisioning and no
custom HA integration. HA knows the choices because the wall advertises
them.

## Why these pieces

- Which agencies exist is the server's data (`server/agencies.json` →
  `GET /agencies`). The wall re-publishes that list; it never keeps its
  own copy of agency names.
- Which agency this wall shows is the wall's data (NVS key `agency`).
  A brightness analogy holds: the selection lives in the panel, so the
  MQTT endpoint is the wall, same as the light entity
  (`docs/brightness-plan.md`). The proxy stays transit data only.
- Voice matching wants plain, spoken names as the select *options*:
  "SFMTA", not "SFMTA (Muni)". The registry therefore carries an
  optional `short` per entry, and `/agencies` ships `short` for every
  served entry (falling back to `name`). `name` stays the descriptive
  label the flasher dropdown shows.

## Design

### Server

- `server/agencies.json`: optional `"short"` — the spoken name (sf →
  SFMTA, mta → MTA, metro → Metro Transit, trimet → TriMet, wmata →
  WMATA); entries whose `name` is already clean (Caltrain, VTA) carry
  none and fall back to it. `load_registry` validates whatever is
  served as the label: 1-32 characters, at most 40 UTF-8 bytes (the
  firmware buffer counts bytes, not code points), single line, no
  control or invisible characters, and unique across entries
  case-insensitively — two entries sharing a label would make one
  agency unreachable. A long `name` is fine once the entry carries a
  `short`.
- `GET /agencies` returns `{id, name, short}` per served agency.
  The flasher ignores `short`; the wall uses it as its option list.

### Firmware

- `main.cpp` keeps the retry schedule (fetch once per run, retry every
  30 s until it succeeds) and calls `ha_link.h::haFetchRoster`, which
  GETs `/agencies` via `vehicle_client.h::fetchAgencies` and owns the
  roster itself. The fetch is attempted even while the vehicle poll
  fails: a stored agency id the server stopped serving is exactly when
  voice should be able to fix the wall.
- `ha_link.cpp` publishes a second retained discovery config,
  `homeassistant/select/otw-<id>/config` — a select entity named
  "Agency <wall-id>" (HA shows it device-prefixed, "Open Transit Wall
  Agency <wall-id>", and per-wall ids keep two walls distinguishable
  on one broker), whose options are the roster's spoken labels,
  sharing the light entity's `device` block so HA keeps one device.
- Topics (added to the existing set in `docs/brightness-plan.md`):
  - `otw/<id>/set_agency` — command, an option label
  - `otw/<id>/agency` — retained state, the current option label,
    or `None` for "no valid selection" (HA resets the entity to unknown)
- A command is matched against the roster labels exactly first (HA
  hands back the option string unchanged), case-insensitively only as
  a fallback for a voice pipeline that drifted case. On a hit,
  `provisioner.h::provSetAgency` sets the id AND persists it to NVS
  immediately — a voice command is a finished action, not a flasher
  session edit awaiting `save`. On a miss (stale card after a roster
  change) the wall republishes the real state (its label, or `None`
  when none is set) so the card snaps back.
- A successful command re-arms the poll for immediately, so the map
  repaints within seconds instead of waiting out the poll slot.
- State edge cases: no agency set (single-agency server), or a stored
  id the server no longer serves, publishes `None` — HA's documented
  "reset to unknown" payload. A raw id outside `options` would be
  rejected by HA's select and leave the card showing the last valid
  option, a quieter symptom; the `[ha]` serial log names the cause.
- Discovery + state are re-published on every broker connect and as
  soon as the roster arrives, whichever happens last. If the roster
  never arrived this run, the connect path clears the retained config
  (an empty retained payload removes the entity) instead of leaving a
  select whose commands would all fall.

## Voice path

Stock HA sentence intents have no sentence for select entities, so
voice control needs an LLM-based conversation agent in Assist (OpenAI,
Ollama, etc.): the agent is offered the entity with its `options`, and
"change the transit wall agency to Caltrain" lands on
`select.select_option`. Dashboards and automations call
`select.select_option` directly and are unaffected. No custom intent,
no HA YAML.

## Limits

- The roster is fetched at boot. If the server starts serving a new
  agency, a powered-off wall picks it up on its next reboot; HA's
  options do not change while the wall runs.
- The select entity exists only when both links are up: a broker host
  or MQTT credentials provisioned AND the wall reaching its proxy. The
  light entity works without the proxy; the agency select needs both.
- No serial protocol change: no new keys, `OTW_FW_VERSION` stays 5
  (v6 is reserved by the panel-rotation work). `save`/`get agency`
  see the HA-chosen value because `provSetAgency` writes the same NVS
  key.
