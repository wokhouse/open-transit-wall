#pragma once

#include <Arduino.h>

// A vehicle as parsed from the proxy payload. Colors are resolved at parse
// time from the payload's "palette" map (type code -> RGB), so the renderer
// never sees type codes and carries no compiled color table.
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
