#include "vehicle_client.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "proxy_ca.h"

// One TLS client reused across polls; the CA bundle is parsed into mbedTLS
// trust anchors on first use and kept (~2 KB heap). PROXY_SCHEME/PROXY_PORT
// come from config.h: HTTPS against the hosted app is the default, plain
// HTTP against a LAN proxy.py is the dev override.
static WiFiClientSecure secureClient;
static bool caLoaded = false;

int fetchVehicles(const char *host, uint16_t port, Vehicle *out, int maxVehicles,
                  float *upstreamAgeSeconds) {
  if (!caLoaded) {
    secureClient.setCACert(PROXY_CA_PEM);
    caLoaded = true;
  }
  HTTPClient http;
  String url = String(PROXY_SCHEME) + host + ":" + String(port) + "/vehicles";
  if (!http.begin(secureClient, url)) {
    return -1;
  }
  http.setTimeout(5000);
  int status = http.GET();
  if (status != 200) {
    // Negative codes are HTTPClient transport errors: a TLS handshake
    // failure is usually OOM or an untrusted/rotated CA (see proxy_ca.h).
    Serial.printf("[http] GET status %d (heap %u)\n", status,
                  ESP.getFreeHeap());
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
    v.id = veh[5] | 0;
    count++;
  }
  return count;
}
