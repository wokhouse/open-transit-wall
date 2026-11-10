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

- `server/agencies.json`: optional `"short"` — a 1-32 char single-line
  spoken name. `load_registry` rejects anything else; the shipped
  entries define it only where `name` carries extra prose (sf → SFMTA,
  mta → MTA, metro → Metro Transit, trimet → TriMet, wmata → WMATA).
- `GET /agencies` now returns `{id, name, short}` per served agency.
  The flasher ignores `short`; the wall uses it as its option list.

### Firmware

- `main.cpp` GETs `/agencies` once per run via
  `vehicle_client.h::fetchAgencies` (retry every 30 s until it
  succeeds). It is attempted even while the vehicle poll fails: a
  stored agency id the server stopped serving is exactly when voice
  should be able to fix the wall.
- `ha_link.cpp` publishes a second retained discovery config,
  `homeassistant/select/otw-<id>/config` — a select entity named
  "Transit Wall Agency" whose options are the roster's spoken labels,
  sharing the light entity's `device` block so HA keeps one device.
- Topics (added to the existing set in `docs/brightness-plan.md`):
  - `otw/<id>/set_agency` — command, an option label
  - `otw/<id>/agency` — retained state, the current option label
- A command is matched against the roster labels case-insensitively
  (voice pipelines drift case). On a hit, `provisioner.h::provSetAgency`
  sets the id AND persists it to NVS immediately — a voice command is a
  finished action, not a flasher session edit awaiting `save`. On a miss
  (stale card after a roster change) the wall republishes the real
  state so the card snaps back.
- A successful command re-arms the poll for immediately, so the map
  repaints within seconds instead of waiting out the poll slot.
- State edge cases: no agency set (single-agency server) leaves the
  entity unknown rather than claiming an option; a stored id the server
  no longer serves publishes the raw id, which shows as a state beyond
  the options — the visible symptom of that mismatch.
- Discovery + state are re-published on every broker connect and as
  soon as the roster arrives, whichever happens last.

## Voice path

HA exposes select entities to Assist natively; the options are ordinary
words, so "change the transit wall agency to Caltrain", or an LLM
agent offered the entity with its `options` attribute, both land on
`select.select_option`. No custom intent, no HA YAML.

## Limits

- The roster is fetched at boot. If the server starts serving a new
  agency, a powered-off wall picks it up on its next reboot; HA's
  options do not change while the wall runs.
- The select entity exists only when both links are up: MQTT
  credentials provisioned AND the wall reaching its proxy. The light
  entity works without the proxy; the agency select needs both.
- No serial protocol change: no new keys, `OTW_FW_VERSION` stays 5
  (v6 is reserved by the panel-rotation work). `save`/`get agency`
  see the HA-chosen value because `provSetAgency` writes the same NVS
  key.
