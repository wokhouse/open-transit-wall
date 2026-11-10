#pragma once
// Optional Home Assistant link: the wall speaks MQTT on the LAN it is
// already connected to and registers itself as a native light entity via
// Home Assistant MQTT auto-discovery (brightness slider, scenes,
// automations — no HA YAML). It also registers a select entity listing
// the agencies the proxy serves, so "change the transit wall to SFMTA"
// works in HA voice (docs/agency-select.md). The broker is found by
// itself: with no `mhost` provisioned the wall browses mDNS for the Home
// Assistant host (ha_discover.h) and uses its MQTT port; `set mhost=...`
// overrides that for brokers elsewhere. Credentials (`muser`/`mpass`)
// stay required by the HA Mosquitto add-on. With neither a broker hint
// nor credentials set, this module does nothing at all.
//
// The wall publishes retained state (ON/OFF + brightness 0-255, the
// selected agency), an LWT availability topic, and retained discovery
// configs. Commands arrive on the set topics. The brightness value is
// persisted in NVS (namespace `otw`, key `bright`) so the wall comes
// back at the last setting without HA being up. The full design is
// docs/brightness-plan.md.

#include <Arduino.h>

struct AgencyEntry;  // full definition in vehicle_client.h

// Load the stored brightness and wire up the MQTT callback. `apply` is
// called with the effective brightness whenever it changes (and may be
// called from haService() in the render loop — it must be cheap).
void haBegin(void (*apply)(uint8_t));

// Brightness to boot with: the NVS value if one was ever set, else the
// DISPLAY_BRIGHTNESS fallback from config.h. Valid after haBegin().
uint8_t haBrightness();

// Call every loop() iteration. No-ops when no broker is provisioned or
// WiFi is down; otherwise keeps the connection alive (rate-limited
// reconnects) and services commands.
void haService();

// True while the broker connection is up (for `status`).
bool haConnected();
// True when the link is configured (broker host or MQTT credentials
// provisioned). Combined with haConnected() a caller can tell "up" from
// "configured but broken" from "not configured".
bool haEnabled();

// The broker this module is using or last tried: the provisioned host, or
// the mDNS-discovered one when none was provisioned. Empty = unknown yet.
const char *haBroker();

// Hands the proxy's /agencies roster (vehicle_client.h's fetchAgencies) to
// the link: it announces the agency select entity with those options. Safe
// before the broker connects — the connect path publishes then. Re-calling
// replaces the roster and republishes.
void haSetAgencies(const AgencyEntry *list, int count);

// True exactly once after Home Assistant selected a different agency than
// the one polled. Lets the main loop fetch the new fleet immediately
// instead of waiting out POLL_SECONDS.
bool haTakeAgencyRepoll();
