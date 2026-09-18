// open-transit-wall firmware entry point.
//
// ESP32-WROOM-32E + Waveshare RGB-Matrix-Px-64x64 (HUB75). Polls the local
// mini-proxy for compact vehicle positions and plots them on the LED wall.
// Copy include/secrets.example.h to include/secrets.h first.
//
// 511 only publishes positions every ~65s, so instead of teleporting the
// wall on each poll, each vehicle that moved picks a random moment inside
// MOTION_WINDOW_MS and glides to its new pixel (see config.h) — the wall
// reads as many vehicles moving independently.
//
// Rendering is strictly incremental: the backdrop is painted once at boot
// and the loop only rewrites the individual pixels that change as dots
// glide. Full-frame redraws would tear against the DMA scan-out and show
// up as flicker at the render rate.

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "map_project.h"
#include "screen.h"
#include "secrets.h"
#include "vehicle_client.h"

static Screen screen;
static MapProjection projection;
static Vehicle vehicles[MAX_VEHICLES];  // latest snapshot from the proxy
static int vehicleCount = 0;
static uint32_t nextPollMs = 0;
static uint8_t pollFailures = 0;

// One animatable dot. Positions are display pixels (projected once at merge
// time, so the render loop is just integer lerps). ~20 B/vehicle keeps the
// whole fleet in ~16 KB of static RAM.
struct ActiveVehicle {
  uint32_t startMs;  // when the glide begins
  uint32_t id;       // stable crc31 of the SIRI VehicleRef
  uint16_t durMs;    // glide duration
  int8_t sx, sy;     // pixel the glide starts from
  int8_t tx, ty;     // target pixel
  int8_t dx, dy;     // pixel last painted (so we only erase what we drew)
  uint8_t r, g, b;
};
static ActiveVehicle actives[MAX_VEHICLES];
static int activeCount = 0;
static bool seen[MAX_VEHICLES];

static void drawStatusPixel();

// Backdrop pixel class (0 land / 1 water) decoded from the RLE once at boot,
// so a vacated pixel can be repainted without touching the rest of the map.
static uint8_t bgIndex[DISPLAY_W * DISPLAY_H];

static void paintBg(int x, int y) {
  if (x < 0 || y < 0 || x >= DISPLAY_W || y >= DISPLAY_H) {
    return;
  }
  const uint8_t *rgb = bgIndex[y * DISPLAY_W + x] ? BACKDROP_WATER : BACKDROP_LAND;
  screen.drawPixel(x, y, rgb[0], rgb[1], rgb[2]);
  if (x == 0 && y == 0) {
    drawStatusPixel();  // the status pixel lives at (0,0)
  }
}

// After a dot vacates a pixel, restore any other dot still parked there.
static void redrawOccupants(int x, int y, int skipIdx) {
  for (int i = 0; i < activeCount; i++) {
    if (i == skipIdx) {
      continue;
    }
    if (actives[i].dx == x && actives[i].dy == y) {
      screen.drawPixel(x, y, actives[i].r, actives[i].g, actives[i].b);
    }
  }
}

static void decodeBackdropIndices() {
  int x = 0, y = 0;
  for (unsigned int i = 0; i + 1 < backdrop_rle_len; i += 2) {
    for (uint8_t n = backdrop_rle[i]; n > 0; n--) {
      if (x < DISPLAY_W && y < DISPLAY_H) {
        bgIndex[y * DISPLAY_W + x] = backdrop_rle[i + 1];
      }
      if (++x >= DISPLAY_W) {
        x = 0;
        y++;
      }
    }
  }
}

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

// Animation phase 0..255 (fixed-point), eased with smoothstep so each glide
// starts and ends gently.
static uint8_t phaseOf(const ActiveVehicle &a, uint32_t now) {
  if ((int32_t)(now - a.startMs) <= 0) {
    return 0;
  }
  uint32_t elapsed = now - a.startMs;
  if (elapsed >= a.durMs) {
    return 255;
  }
  uint32_t t = (elapsed << 8) / a.durMs;  // 0..255
  return (t * t * (3 * 255 - 2 * t)) >> 16;
}

static void displayedXY(const ActiveVehicle &a, uint32_t now, int &x, int &y) {
  int ph = phaseOf(a, now);
  x = a.sx + ((a.tx - a.sx) * ph) / 255;
  y = a.sy + ((a.ty - a.sy) * ph) / 255;
}

static int findActive(uint32_t id) {
  for (int i = 0; i < activeCount; i++) {
    if (actives[i].id == id) {
      return i;
    }
  }
  return -1;
}

// Fold the fresh snapshot into the animation slots: matching vehicles get a
// new target (scheduled at a random moment in the remainder of the upstream
// refresh cycle), new vehicles appear in place, missing ones are erased.
static void mergeVehicles(uint32_t nowMs, float upstreamAge) {
  // Glides tile the whole refresh cycle: spread start times across whatever
  // is left of it after the snapshot's own age.
  int32_t spreadMs = (int32_t)(UPSTREAM_REFRESH_S * 1000)
                     - (int32_t)(upstreamAge * 1000.0f) - MOTION_GLIDE_MS;
  if (spreadMs < 1000) {
    spreadMs = 1000;  // stale snapshot: still stagger, just tightly
  }
  memset(seen, 0, activeCount);
  int moved = 0, fresh = 0;
  for (int i = 0; i < vehicleCount; i++) {
    const Vehicle &v = vehicles[i];
    int px, py;
    if (!projection.project(v.lat, v.lon, &px, &py)) {
      continue;  // outside the configured bounding box
    }
    int s = findActive(v.id);
    if (s >= 0) {
      seen[s] = true;
      ActiveVehicle &a = actives[s];
      if (px != a.tx || py != a.ty || v.r != a.r || v.g != a.g || v.b != a.b) {
        a.sx = a.dx;  // glide starts from the pixel it's visually at
        a.sy = a.dy;
        a.tx = px;
        a.ty = py;
        a.r = v.r;
        a.g = v.g;
        a.b = v.b;
        a.startMs = nowMs + random(spreadMs);
        a.durMs = MOTION_GLIDE_MS;
        moved++;
      }
    } else if (activeCount < MAX_VEHICLES) {
      ActiveVehicle &a = actives[activeCount];
      a.id = v.id;
      a.sx = a.tx = px;
      a.sy = a.ty = py;
      a.dx = px;
      a.dy = py;
      a.r = v.r;
      a.g = v.g;
      a.b = v.b;
      a.startMs = nowMs;  // appear in place immediately
      a.durMs = 1;
      seen[activeCount] = true;
      activeCount++;
      screen.drawPixel(px, py, a.r, a.g, a.b);
      fresh++;
    }
  }
  int w = 0;
  for (int i = 0; i < activeCount; i++) {
    if (seen[i]) {
      actives[w++] = actives[i];
    } else {
      paintBg(actives[i].dx, actives[i].dy);
      redrawOccupants(actives[i].dx, actives[i].dy, i);
    }
  }
  int gone = activeCount - w;
  activeCount = w;
  Serial.printf("[poll] %d vehicles (%d moved, %d new, %d gone, heap %u)\n",
                vehicleCount, moved, fresh, gone, ESP.getFreeHeap());
}

// Animation tick at RENDER_FPS: only rewrites pixels whose dot moved. With
// glides of a few pixels over several seconds, that's a handful of pixel
// writes per frame — no full-frame repaint, no tear, no flicker.
static void animate(uint32_t now) {
  for (int i = 0; i < activeCount; i++) {
    ActiveVehicle &a = actives[i];
    int x, y;
    displayedXY(a, now, x, y);
    if (x == a.dx && y == a.dy) {
      continue;
    }
    paintBg(a.dx, a.dy);
    int oldx = a.dx, oldy = a.dy;
    a.dx = x;
    a.dy = y;
    redrawOccupants(oldx, oldy, i);
    screen.drawPixel(x, y, a.r, a.g, a.b);
  }
  drawStatusPixel();
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

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!screen.begin()) {
    Serial.println("HUB75 init failed — halting");
    while (true) delay(1000);
  }
  screen.setBrightness(DISPLAY_BRIGHTNESS);
  screen.clear();
  decodeBackdropIndices();
  screen.drawBackdrop();  // painted once; animate() only touches dot pixels

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
      mergeVehicles(now, upstreamAge);
    } else {
      pollFailures = min<uint8_t>(pollFailures + 1, 200);
      Serial.printf("[poll] fetch failed (%u consecutive)\n", pollFailures);
    }
  }

  static uint32_t nextFrameMs = 0;
  if ((int32_t)(now - nextFrameMs) >= 0) {
    nextFrameMs = now + 1000 / RENDER_FPS;
    animate(now);
  }

  delay(2);
}
