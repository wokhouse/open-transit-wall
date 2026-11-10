#pragma once

#include <Arduino.h>

// A vehicle as parsed from the proxy payload. Colors are resolved at parse
// time from the payload's "palette" map (style key -> RGB; a style key is a
// type code like "B" or a per-line metro key like "M:J"), so the renderer
// never sees style keys and carries no compiled color table. Keys are
// opaque: the firmware works for any agency without a reflash.
struct Vehicle {
  float lat;
  float lon;
  uint32_t id;
  uint8_t r, g, b;
};

// Result of one /vehicles fetch. ok=false means the payload failed the wire
// contract (missing/invalid age, next, bbox, or palette, or a parse error) and
// the caller must treat it as a failed poll — no partial updates.
struct PollResult {
  bool ok;
  bool agencyUnknown;  // server 400'd our agency id: re-provision
  int count;
  float age;
  uint32_t next;       // seconds until the server's next upstream fetch
  float bbox[4];       // lat_min, lat_max, lon_min, lon_max
  Vehicle *vehicles;
};

// Fetches the fleet once. When agency is non-empty it is appended as
// ?agency=<id>; empty means no parameter (single-agency servers).
// The caller owns the Vehicle array; it is filled up to maxVehicles.
PollResult fetchVehicles(const char *scheme, const char *host, uint16_t port,
                        const char *agency, Vehicle *out, int maxVehicles);

// One entry of the server's /agencies roster. `id` is the registry id the
// wall polls with; `label` is the short spoken name the server ships for
// voice control (its `short`, already fallen back to `name` server-side).
// The server caps served labels at 32 chars AND 40 UTF-8 bytes; ids are
// [a-z0-9]{1,8}. An entry that does not fit here is dropped whole — a
// truncated label could cut a multi-byte character in half and make the
// discovery JSON unparseable.
#define AGENCY_MAX 16
#define AGENCY_ID_MAX 8
#define AGENCY_LABEL_MAX 40
struct AgencyEntry {
  char id[AGENCY_ID_MAX + 1];
  char label[AGENCY_LABEL_MAX + 1];
};

// GETs /agencies into out (up to max), skipping malformed entries. Returns
// the count (0 = fetch failed or empty); the caller retries later. The
// roster feeds the Home Assistant agency select (src/ha_link.h).
int fetchAgencies(const char *scheme, const char *host, uint16_t port,
                  AgencyEntry *out, int max);
