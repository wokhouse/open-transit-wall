#pragma once
// Fetch + parse the compact vehicle payload served by the proxy
// (server/proxy.py  GET /vehicles). Plain HTTP over the LAN — the proxy owns
// the HTTPS call to 511 and the rate-limit discipline.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

struct Vehicle {
  float lat, lon;
  uint8_t r, g, b;
  uint32_t id;  // stable across polls (crc31 of the SIRI VehicleRef)
};

// Fetches /vehicles and fills `out` (up to maxVehicles). Returns the number
// of vehicles parsed, or -1 on any network/parse error.
int fetchVehicles(const char *host, uint16_t port, Vehicle *out, int maxVehicles,
                  float *upstreamAgeSeconds);
