#include "vehicle_client.h"

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "proxy_ca.h"
#include "provisioner.h"

// The wire contract (docs/multi-agency-plan.md): a valid payload always
// carries ts, age, next, count, bbox, palette and v. A missing or invalid
// next/bbox/palette is a failed poll — the client never substitutes its own
// refresh interval or colors.

// One TLS client reused across polls; the CA bundle is parsed into mbedTLS
// trust anchors on first use and kept (~2 KB heap). The scheme comes from
// the provisioner: HTTPS against the hosted app is the default, plain HTTP
// against a LAN proxy.py is provisioned, not compiled.
static WiFiClientSecure secureClient;
static bool caLoaded = false;

static void ensureCa() {
  if (caLoaded) {
    return;
  }
  // Core 3.x (platform 7.1.3): setCACert is void; the parse result comes
  // from lastError(). "OK" means the bundle parsed into trust anchors.
  char errbuf[128] = {0};
  secureClient.setCACert(PROXY_CA_PEM);
  secureClient.lastError(errbuf, sizeof(errbuf));
  if (strcmp(errbuf, "OK") != 0) {
    Serial.printf("[tls] CA bundle rejected (%s) — check proxy_ca.h\n", errbuf);
  }
  caLoaded = true;
}

static bool readBbox(JsonObject root, float *bbox) {
  JsonArray b = root["bbox"].as<JsonArray>();
  if (b.size() != 4) {
    return false;
  }
  for (int i = 0; i < 4; i++) {
    if (!b[i].is<float>() && !b[i].is<int>()) {
      return false;
    }
    bbox[i] = b[i].as<float>();
  }
  return bbox[0] < bbox[1] && bbox[2] < bbox[3];
}

// palette: { "B": [r,g,b], ... }. Every entry must be a 3-int array of
// bytes; an empty palette is fine (no vehicle can reference it then).
static bool readPalette(JsonObject root, JsonObject &palette) {
  palette = root["palette"].as<JsonObject>();
  if (palette.isNull()) {
    return false;
  }
  for (JsonPair kv : palette) {
    JsonArray rgb = kv.value().as<JsonArray>();
    if (rgb.size() != 3) {
      return false;
    }
    for (JsonVariant v : rgb) {
      if (!v.is<int>() || v.as<int>() < 0 || v.as<int>() > 255) {
        return false;
      }
    }
  }
  return true;
}

PollResult fetchVehicles(const char *scheme, const char *host, uint16_t port,
                        const char *agency, Vehicle *out, int maxVehicles) {
  PollResult res{false, false, 0, 0.0f, 0, {0, 0, 0, 0}, out};

  String url = String(scheme) + host + ":" + String(port) + "/vehicles";
  if (agency && agency[0]) {
    url += "?agency=" + String(agency);
  }

  HTTPClient http;
  ensureCa();
  http.setTimeout(8000);
  if (!http.begin(secureClient, url)) {
    return res;
  }
  int code = http.GET();
  if (code == 400) {
    // The server does not serve our agency id: a provisioning mistake, not
    // a transient error. Surface it so the status pixel can be made steady
    // red and the operator can re-provision.
    http.end();
    res.agencyUnknown = true;
    return res;
  }
  if (code != 200) {
    http.end();
    return res;
  }

  // Parse straight off the HTTP stream — no intermediate String. Buffering
  // the body next to ArduinoJson's document overflowed the heap at
  // rush-hour fleet sizes (~500 vehicles, ~48 KB).
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    return res;
  }

  JsonObject root = doc.as<JsonObject>();
  if (!root["age"].is<float>() && !root["age"].is<int>()) {
    return res;
  }
  if (!root["next"].is<int>() || root["next"].as<int>() < 0) {
    return res;  // no advertised cadence -> failed poll, red pixel.
  }
  // next == 0 is valid: the server's next fetch is due right now. The
  // caller re-polls promptly (with a 1s floor) instead of erroring.
  if (!readBbox(root, res.bbox)) {
    return res;
  }
  JsonObject palette;
  if (!readPalette(root, palette)) {
    return res;
  }

  res.age = root["age"].as<float>();
  res.next = (uint32_t)root["next"].as<int>();

  // v rows: [lat, lon, "TYPE", id]. A row whose type key is not in the
  // palette is skipped — never invent a color.
  JsonArray vehicles = root["v"].as<JsonArray>();
  int n = 0;
  for (JsonArray row : vehicles) {
    if (row.size() < 4 || !row[0].is<float>() || !row[1].is<float>() ||
        !row[2].is<const char *>() ||
        !(row[3].is<int>() || row[3].is<float>())) {
      continue;
    }
    const char *type = row[2].as<const char *>();
    JsonArray rgb = palette[type].as<JsonArray>();
    if (rgb.size() != 3) {
      continue;
    }
    if (n >= maxVehicles) {
      break;
    }
    Vehicle &v = out[n++];
    v.lat = row[0].as<float>();
    v.lon = row[1].as<float>();
    v.id = row[3].as<uint32_t>();
    v.r = (uint8_t)rgb[0].as<int>();
    v.g = (uint8_t)rgb[1].as<int>();
    v.b = (uint8_t)rgb[2].as<int>();
  }

  res.count = n;
  res.ok = true;
  return res;
}
