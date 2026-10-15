#include "vehicle_client.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "proxy_ca.h"
#include "provisioner.h"

// One TLS client reused across polls; the CA bundle is parsed into mbedTLS
// trust anchors on first use and kept (~2 KB heap). The scheme comes from
// the provisioner: HTTPS against the hosted app is the default, plain HTTP
// against a LAN proxy.py is provisioned, not compiled.
static WiFiClientSecure secureClient;
static bool caLoaded = false;

int fetchVehicles(const char *host, uint16_t port, Vehicle *out, int maxVehicles,
                  float *upstreamAgeSeconds) {
  if (!caLoaded) {
    // Core 3.x (platform 7.1.3): setCACert is void; the parse result comes
    // from lastError(). "OK" means the bundle parsed into trust anchors.
    char errbuf[128] = {0};
    secureClient.setCACert(PROXY_CA_PEM);
    secureClient.lastError(errbuf, sizeof(errbuf));
    if (strcmp(errbuf, "OK") != 0) {
      Serial.printf("[tls] CA bundle rejected (%s) — check proxy_ca.h\n",
                    errbuf);
    }
    caLoaded = true;
  }
  HTTPClient http;
  String url =
      String(provScheme()) + host + ":" + String(port) + "/vehicles";
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
