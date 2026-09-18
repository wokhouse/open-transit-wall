#include "vehicle_client.h"

#include <Arduino.h>

int fetchVehicles(const char *host, uint16_t port, Vehicle *out, int maxVehicles,
                  float *upstreamAgeSeconds) {
  HTTPClient http;
  String url = String("http://") + host + ":" + String(port) + "/vehicles";
  if (!http.begin(url)) {
    return -1;
  }
  http.setTimeout(5000);
  int status = http.GET();
  if (status != 200) {
    Serial.printf("[http] GET status %d\n", status);
    http.end();
    return -1;
  }

  // Parse straight off the HTTP stream — no intermediate String. Buffering
  // the body as a String next to ArduinoJson's document overflowed the heap
  // at rush-hour fleet sizes (~500 vehicles, ~48 KB). The proxy serves flat
  // [lat, lon, r, g, b] arrays so the document stays small: every JSON key
  // would cost ~16 bytes of heap next to the display framebuffer.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[http] json error %s (heap %u)\n", err.c_str(),
                  ESP.getFreeHeap());
    return -1;
  }

  *upstreamAgeSeconds = doc["age"] | 0.0f;

  int count = 0;
  for (JsonArray veh : doc["v"].as<JsonArray>()) {
    if (count >= maxVehicles) {
      break;
    }
    Vehicle &v = out[count];
    v.lat = veh[0] | 0.0f;
    v.lon = veh[1] | 0.0f;
    v.r = veh[2] | 0;
    v.g = veh[3] | 0;
    v.b = veh[4] | 0;
    count++;
  }
  return count;
}
