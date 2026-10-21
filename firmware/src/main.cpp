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
// teleporting the wall on each poll, the remaining refresh cycle is split
// into one equal slot per moved vehicle, and each one glides to its new
// pixel at a random moment inside its slot (see config.h) — the wall reads
// as many vehicles moving independently, spread evenly over the cycle.
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
#include <esp_system.h>  // esp_random() for the splash animation seed
#include <string.h>

#include "backdrop.h"
#include "config.h"
#include "map_project.h"
#include "provisioner.h"
#include "screen.h"
#include "status_screen.h"
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
static int16_t slotOrder[MAX_VEHICLES];  // shuffled glide-slot assignment

static void drawStatusPixel();

// What the panel is currently showing. The map is the normal state
// (ERR_NONE); splash, setup-needed, and error screens replace it until the
// condition clears. Screens are drawn once on the transition, never per
// loop iteration — the incremental-render rule governs the map path only.
enum ErrorKind {
  ERR_NONE,       // showing the map
  ERR_SPLASH,     // boot splash (bouncing logo) — the panel's initial state
  ERR_PROVISION,  // no ssid/host configured yet
  ERR_WIFI,       // configured, but the network won't take us
  ERR_SERVER,     // network up, proxy unreachable
  ERR_AGENCY,     // proxy 400s our agency id
  ERR_NOMAP,      // no compiled backdrop for the provisioned agency
};
// Track what the panel ACTUALLY shows. Boot draws the splash, so that's the
// initial state — initializing to ERR_NONE would make the first
// showScreen(ERR_NONE) a no-op and leave the splash under the map.
static ErrorKind shownError = ERR_SPLASH;
static const BackdropDef *curBackdrop = nullptr;  // chosen at (re)provision

// Splash animation state: the roundel bounces around the panel while the
// wall loads (Wi-Fi/NTP waits below drive frames).
static SplashState splashState;
static void drawSplashFrame();  // defined with the screen helpers below

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
    if (shownError == ERR_SPLASH) {
      drawSplashFrame();  // keep the logo bouncing while we wait
    }
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
    if (shownError == ERR_SPLASH) {
      drawSplashFrame();  // keep the logo bouncing while we wait
    }
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

// True when the fresh snapshot would move this dot: a new pixel or colour.
static bool targetChanged(const ActiveVehicle &a, const Vehicle &v, int px, int py) {
  return px != a.tx || py != a.ty || v.r != a.r || v.g != a.g || v.b != a.b;
}

// Fold the fresh snapshot into the animation slots: matching vehicles get a
// new target (scheduled inside an equal slot of the remaining upstream
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

  // Count this snapshot's glides first, then give each one an equal slot
  // inside the cycle and a random moment inside that slot (stratified
  // sampling). Independent randoms clump: some moments get several glides
  // at once while others stay dark.
  int willMove = 0;
  for (int i = 0; i < vehicleCount; i++) {
    const Vehicle &v = vehicles[i];
    int px, py;
    if (!projection.project(v.lat, v.lon, &px, &py)) {
      continue;  // outside the agency's advertised bounding box
    }
    int s = findActive(v.id);
    if (s >= 0 && targetChanged(actives[s], v, px, py)) {
      willMove++;
    }
  }
  int32_t strideMs = willMove > 0 ? spreadMs / willMove : spreadMs;
  if (strideMs < 1) {
    strideMs = 1;
  }

  // Shuffle which vehicle gets which slot. Payload order is stable across
  // polls, so unshuffled slots would make the same vehicles glide first
  // every cycle.
  for (int i = 0; i < willMove; i++) {
    slotOrder[i] = i;
  }
  for (int i = willMove - 1; i > 0; i--) {
    int j = random(i + 1);
    int16_t t = slotOrder[i];
    slotOrder[i] = slotOrder[j];
    slotOrder[j] = t;
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
      if (targetChanged(a, v, px, py)) {
        a.sx = a.dx;  // glide starts from the pixel it's visually at
        a.sy = a.dy;
        a.tx = px;
        a.ty = py;
        a.r = v.r;
        a.g = v.g;
        a.b = v.b;
        // One glide per slot; which vehicle takes which slot is shuffled,
        // and the start is jittered inside the slot.
        a.startMs = nowMs + (int32_t)slotOrder[moved] * strideMs + random(strideMs);
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

// Paint the panel for a (possibly new) error state. No-op when the panel
// already shows it, so the retry loop can call this every cycle without
// repainting. Clearing back to the map repaints the backdrop (dots are
// dropped — they were drawn under the error screen).
static void putScreenPixel(void *, int x, int y, uint8_t r, uint8_t g,
                           uint8_t b) {
  screen.drawPixel(x, y, r, g, b);
}

// One frame of the bouncing splash. Called from setup's blocking waits so
// the logo moves while the wall loads.
static void drawSplashFrame() {
  screen.clear();
  showSplashFrame(putScreenPixel, nullptr, &splashState);
}

static void showScreen(ErrorKind kind) {
  if (kind == shownError) {
    return;
  }
  shownError = kind;
  // Every status screen replaces whatever was on the panel wholesale:
  // wipe first so no pixels from the previous screen bleed through.
  screen.clear();
  switch (kind) {
    case ERR_SPLASH:
      showSplashFrame(putScreenPixel, nullptr, &splashState);
      break;
    case ERR_PROVISION:
      showSetupNeeded(putScreenPixel, nullptr);
      break;
    case ERR_WIFI:
      showWifiError(putScreenPixel, nullptr);
      break;
    case ERR_SERVER:
      showServerError(putScreenPixel, nullptr);
      break;
    case ERR_AGENCY:
      showAgencyError(putScreenPixel, nullptr);
      break;
    case ERR_NOMAP:
      showNoMapError(putScreenPixel, nullptr, provAgency());
      break;
    case ERR_NONE:
    default:
      if (curBackdrop) {
        screen.drawBackdrop(*curBackdrop);
      } else {
        screen.clear();
      }
      // Dots from before the error screen are gone; forget them so the
      // next merge re-adds vehicles at their current positions.
      activeCount = 0;
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!screen.begin()) {
    Serial.println("HUB75 init failed — halting");
    while (true) delay(1000);
  }
  screen.setBrightness(DISPLAY_BRIGHTNESS);

  // Splash while we figure out what to show: the roundel bouncing DVD-style
  // around the panel while the wall loads. Replaced by the map or an error
  // screen below. Seed from the SoC RNG so every boot takes a different
  // path (initial diagonal and corner bounces both derive from this).
  screen.clear();
  splashState.seed = esp_random();
  drawSplashFrame();

  provLoad();
  if (provNeeded()) {
    // No complete config in NVS yet (needs ssid + host). Stay up on the
    // serial console: the browser tool (or any terminal) can `set` values;
    // the loop picks them up live — no reflash, no reboot needed.
    Serial.println("UNPROVISIONED: open the browser tool and set "
                   "ssid/pass/host, then save.");
    showScreen(ERR_PROVISION);
    return;
  }

  // The backdrop is chosen once at boot from the provisioned agency id.
  // Changing agency is a re-provision + reboot, not a live edit.
  curBackdrop = backdropFor(provAgency());
  if (curBackdrop) {
    decodeBackdropIndices(*curBackdrop);
  } else {
    Serial.printf("BACKDROP none for agency '%s' — blank map; regenerate with "
                  "tools/gen_backdrop.py --agency %s and reflash\n",
                  provAgency(), provAgency());
  }

  connectWifi();
  syncTime();
  if (WiFi.status() != WL_CONNECTED) {
    showScreen(ERR_WIFI);
  } else if (!curBackdrop) {
    showScreen(ERR_NOMAP);
  } else {
    showScreen(ERR_NONE);  // paint the map
  }
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
      showScreen(ERR_PROVISION);
      return;  // still waiting for set ssid/pass
    }
    if (!curBackdrop) {
      // Either boot found no backdrop for this agency, or the agency was
      // (re)provisioned live since then: pick the backdrop for the current
      // id before polling. A null result keeps the NO MAP screen up.
      curBackdrop = backdropFor(provAgency());
      if (curBackdrop) {
        decodeBackdropIndices(*curBackdrop);
      }
    }
    if (WiFi.status() != WL_CONNECTED) {
      connectWifi();
      if (WiFi.status() != WL_CONNECTED) {
        showScreen(ERR_WIFI);
        return;
      }
      syncTime();  // TLS needs the wall clock; NTP only works over WiFi
    }
    if (!curBackdrop) {
      showScreen(ERR_NOMAP);
      return;
    }
    PollResult poll = fetchVehicles(provScheme(), provPollHost(), provPort(),
                                    provAgency(), vehicles, MAX_VEHICLES);
    if (!poll.ok) {
      pollFailures = min<uint8_t>(pollFailures + 1, 200);
      if (poll.agencyUnknown) {
        Serial.println("[poll] server 400: agency id not served — "
                       "check `get agency` against the server's /agencies");
        showScreen(ERR_AGENCY);
      } else {
        Serial.printf("[poll] fetch failed (%u consecutive)\n", pollFailures);
        showScreen(ERR_SERVER);
      }
      return;
    }
    pollFailures = 0;
    showScreen(ERR_NONE);  // no-op while the map is up; repaints after an error
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
