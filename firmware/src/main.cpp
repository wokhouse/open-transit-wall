// open-transit-wall firmware entry point.
//
// ESP32-WROOM-32E + Waveshare RGB-Matrix-Px-64x64 (HUB75). Polls the mini-
// proxy for compact vehicle positions and plots them on the LED wall. One
// binary is flashed to every wall: WiFi credentials, the proxy endpoint, and
// the agency id are provisioned over WebSerial and kept in NVS
// (src/provisioner.h, docs/provisioning-protocol.md). No secrets.h, no
// per-wall build.
//
// The proxy publishes positions every POLL_SECONDS and advertises the exact
// seconds until its next upstream fetch in the payload ("next"). Instead of
// teleporting the wall on each poll, each vehicle that moved picks a random
// moment inside the remaining refresh cycle and glides to its new pixel
// (see config.h) — the wall reads as many vehicles moving independently.
//
// The map bounding box and the vehicle colors also come from the payload
// ("bbox", "palette"): nothing about the agency is compiled except the
// backdrop bitmap, which is looked up by agency id below.
//
// Rendering is strictly incremental: the backdrop is painted once at boot
// and the loop only rewrites the individual pixels that change as dots
// glide. Full-frame redraws would tear against the DMA scan-out and show
// up as flicker at the render rate.

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

#include "backdrop.h"
#include "config.h"
#include "map_project.h"
#include "provisioner.h"
#include "screen.h"
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

// The backdrop is the one per-agency artifact that cannot come over the
// wire. gen_backdrop.py --agency <id> emits one BackdropDef per agency;
// the wall picks its provisioned agency's entry at boot.
static const BackdropDef *backdropFor(const char *agency) {
  for (unsigned int i = 0; i < backdrop_count; i++) {
    if (strcmp(backdrops[i].agency, agency) == 0) {
      return &backdrops[i];
    }
  }
  return nullptr;
}

static void decodeBackdropIndices(const BackdropDef &bd) {
  int x = 0, y = 0;
  for (unsigned int i = 0; i + 1 < bd.rle_len; i += 2) {
    for (uint8_t n = bd.rle[i]; n > 0; n--) {
      if (x < DISPLAY_W && y < DISPLAY_H) {
        bgIndex[y * DISPLAY_W + x] = bd.rle[i + 1];
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
  WiFi.disconnect();  // drop any stale config from a previous set
  WiFi.begin(provSsid(), provPass());
  Serial.printf("WiFi: connecting to %s ", provSsid());
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

// HTTPS certificate validation needs the wall clock — the ESP32 has no RTC.
// Sync once after WiFi comes up; SNTP keeps refreshing in the background.
// Without a synced clock every TLS handshake fails as "not yet valid".
static void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("NTP: syncing ");
  uint32_t start = millis();
  while (time(nullptr) < 1735689600 && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  if (time(nullptr) >= 1735689600) {
    Serial.println(" ok");
  } else {
    Serial.println(" FAILED (TLS will fail until the clock syncs)");
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
// nextS/age come from the payload: the server's own cadence, not a compiled
// constant.
static void mergeVehicles(uint32_t nowMs, float upstreamAge, uint32_t nextS) {
  // Glides tile the whole refresh cycle: spread start times across whatever
  // is left of it after the snapshot's own age.
  int32_t spreadMs = (int32_t)(nextS * 1000)
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
      continue;  // outside the agency's advertised bounding box
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

  provLoad();
  if (provNeeded()) {
    // No complete config in NVS yet (needs ssid + host). Stay up on the
    // serial console: the browser tool (or any terminal) can `set` values;
    // the loop picks them up live — no reflash, no reboot needed.
    Serial.println("UNPROVISIONED: open the browser tool and set "
                   "ssid/pass/host, then save.");
    return;
  }

  // The backdrop is chosen once at boot from the provisioned agency id.
  // Changing agency is a re-provision + reboot, not a live edit.
  const BackdropDef *bd = backdropFor(provAgency());
  if (bd) {
    decodeBackdropIndices(*bd);
    screen.drawBackdrop(*bd);  // painted once; animate() only touches dot pixels
  } else {
    Serial.printf("BACKDROP none for agency '%s' — blank map; regenerate with "
                  "tools/gen_backdrop.py --agency %s and reflash\n",
                  provAgency(), provAgency());
  }

  connectWifi();
  syncTime();
}

void loop() {
  uint32_t now = millis();

  // Serial provisioning commands work at any time: unprovisioned, or live
  // while the wall runs (a corrected password takes effect on the next
  // connect attempt without a reboot).
  provPumpSerial();

  if ((int32_t)(now - nextPollMs) >= 0) {
    // Fallback cadence only for failed polls; a successful poll re-arms
    // from the server's advertised "next".
    nextPollMs = now + POLL_INTERVAL_MS;
    if (provNeeded()) {
      return;  // still waiting for set ssid/pass — backdrop stays up
    }
    if (WiFi.status() != WL_CONNECTED) {
      connectWifi();
      if (WiFi.status() != WL_CONNECTED) {
        return;
      }
      syncTime();  // TLS needs the wall clock; NTP only works over WiFi
    }
    PollResult poll = fetchVehicles(provScheme(), provPollHost(), provPort(),
                                    provAgency(), vehicles, MAX_VEHICLES);
    if (!poll.ok) {
      pollFailures = min<uint8_t>(pollFailures + 1, 200);
      if (poll.agencyUnknown) {
        Serial.println("[poll] server 400: agency id not served — "
                       "check `get agency` against the server's /agencies");
      } else {
        Serial.printf("[poll] fetch failed (%u consecutive)\n", pollFailures);
      }
      return;
    }
    pollFailures = 0;
    vehicleCount = poll.count;
    // next == 0 means the server's refresh is due now: re-poll promptly.
    // Clamp the top end so a misconfigured server can't overflow the
    // uint32 math (and wrap into a poll hammer) or set an absurd wait.
    uint32_t waitS = poll.next > 0 ? poll.next : 1;
    if (waitS > 3600) {
      waitS = 3600;
    }
    // Re-arm from the post-fetch clock: `now` predates a fetch that can
    // take seconds, and scheduling from it would make the wall re-poll
    // faster than the server refreshes.
    nextPollMs = millis() + waitS * 1000UL;

    // The payload's bbox is the projection's source of truth. Re-init when
    // it changes (agency re-provision, or a registry edit server-side);
    // pixel positions under the old bbox are no longer meaningful.
    static float curBbox[4] = {0, 0, 0, 0};
    bool bboxChanged = memcmp(curBbox, poll.bbox, sizeof(curBbox)) != 0;
    if (bboxChanged) {
      memcpy(curBbox, poll.bbox, sizeof(curBbox));
      BBox bbox = {poll.bbox[0], poll.bbox[1], poll.bbox[2], poll.bbox[3]};
      projection.begin(bbox, DISPLAY_W, DISPLAY_H, MAP_STRETCH_TO_FILL);
      if (activeCount > 0) {
        // Old dots sit at pixels from the previous bbox: wipe them all.
        for (int i = 0; i < activeCount; i++) {
          paintBg(actives[i].dx, actives[i].dy);
        }
        activeCount = 0;
      }
      Serial.printf("[map] bbox %.4f,%.4f,%.4f,%.4f\n", poll.bbox[0],
                    poll.bbox[1], poll.bbox[2], poll.bbox[3]);
    }
    mergeVehicles(now, poll.age, poll.next);
  }

  static uint32_t nextFrameMs = 0;
  if ((int32_t)(now - nextFrameMs) >= 0) {
    nextFrameMs = now + 1000 / RENDER_FPS;
    animate(now);
  }

  delay(2);
}
