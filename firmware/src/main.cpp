// open-transit-wall firmware entry point.
//
// ESP32-WROOM-32E + Waveshare RGB-Matrix-Px-64x32 (HUB75). Polls the local
// mini-proxy for compact vehicle positions and plots them on the LED wall.
// Copy include/secrets.example.h to include/secrets.h first.

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "map_project.h"
#include "screen.h"
#include "secrets.h"
#include "vehicle_client.h"

static Screen screen;
static MapProjection projection;
static Vehicle vehicles[MAX_VEHICLES];
static int vehicleCount = 0;
static uint32_t nextPollMs = 0;
static uint8_t pollFailures = 0;

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi: connecting to %s ", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" connected, ip=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(" FAILED (will retry in loop)");
  }
}

static void drawStatusPixel() {
#if STATUS_PIXEL_ENABLED
  uint8_t r, g, b;
  if (pollFailures == 0) {
    r = 0; g = 160; b = 0;      // fresh
  } else if (pollFailures < STALE_AFTER_POLLS) {
    r = 200; g = 120; b = 0;    // aging
  } else {
    r = 200; g = 0; b = 0;      // error
  }
  screen.drawPixel(0, 0, r, g, b);
#endif
}

static void render() {
  screen.clear();
  screen.drawBackdrop();
  screen.drawBorder();
  for (int i = 0; i < vehicleCount; i++) {
    int x, y;
    if (!projection.project(vehicles[i].lat, vehicles[i].lon, &x, &y)) {
      continue;  // outside the configured bounding box
    }
    for (int dy = 0; dy < VEHICLE_DOT_SIZE; dy++) {
      for (int dx = 0; dx < VEHICLE_DOT_SIZE; dx++) {
        screen.drawPixel(x + dx, y + dy, vehicles[i].r, vehicles[i].g, vehicles[i].b);
      }
    }
  }
  drawStatusPixel();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!screen.begin()) {
    Serial.println("HUB75 init failed — halting");
    while (true) delay(1000);
  }
  screen.setBrightness(DISPLAY_BRIGHTNESS);
  screen.clear();

  connectWifi();

  BBox bbox = {MAP_LAT_MIN, MAP_LAT_MAX, MAP_LON_MIN, MAP_LON_MAX};
  projection.begin(bbox, DISPLAY_W, DISPLAY_H, MAP_STRETCH_TO_FILL);

  nextPollMs = millis();  // fetch immediately
}

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - nextPollMs) >= 0) {
    nextPollMs = now + POLL_INTERVAL_MS;
    if (WiFi.status() != WL_CONNECTED) {
      connectWifi();
    }
    float upstreamAge = 0.0f;
    int n = fetchVehicles(PROXY_HOST, PROXY_PORT, vehicles, MAX_VEHICLES, &upstreamAge);
    if (n >= 0) {
      vehicleCount = n;
      pollFailures = 0;
      render();
      Serial.printf("[poll] %d vehicles (upstream age %.0fs)\n", n, upstreamAge);
    } else {
      pollFailures = min<uint8_t>(pollFailures + 1, 200);
      drawStatusPixel();
      Serial.printf("[poll] fetch failed (%u consecutive)\n", pollFailures);
    }
  }
  delay(10);
}
