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
// ("bbox", "palette"): nothing about an agency is compiled at all. The map
// is vehicles on black — there is no backdrop or land layer.
//
// Optionally the wall also speaks MQTT on the LAN (src/ha_link.h): with a
// broker provisioned (`set mhost=...`) it announces itself to Home
// Assistant as a native light entity and keeps its brightness there. With
// no broker configured this changes nothing.
//
// Rendering is strictly incremental: the loop only rewrites the individual
// pixels that change as dots glide. Full-frame redraws would tear against
// the DMA scan-out and show up as flicker at the render rate.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>  // esp_random() for the splash animation seed
#include <freertos/timers.h>
#include <string.h>

#include "config.h"
#include "ha_link.h"
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
// time, so the render loop is just integer lerps). ~20 B/vehicle for
// actives plus the snapshot's vehicles[]: at MAX_VEHICLES 1200 the two
// arrays are ~45 KB of static RAM.
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
// Glide coordinates are int8_t: a display past 127 px needs wider fields.
// (The backdrop size guard is gone, so this is the pixel-width guard now.)
static_assert(DISPLAY_W <= 127 && DISPLAY_H <= 127,
              "ActiveVehicle int8_t pixel coords cap displays at 127 px");
static bool seen[MAX_VEHICLES];
static int16_t slotOrder[MAX_VEHICLES];  // shuffled glide-slot assignment

static void drawStatusPixel();

// Brightness setter handed to ha_link: MQTT commands land here.
// Brightness last commanded to the panel. 0 means "off": the map goes dark
// but the frame branch in loop() keeps the status pixels lit at
// STATUS_PIXEL_FLOOR, so the wall still shows it is alive and MQTT-linked.
static uint8_t panelBrightness = DISPLAY_BRIGHTNESS;
static bool mapDirty = false;  // frame was cleared while off; repaint on return

static void applyPanelBrightness(uint8_t v) {
  if (v == panelBrightness) {
    return;
  }
  if (v > 0) {
    if (panelBrightness == 0) {
      mapDirty = true;  // the off frame wiped the map; repaint before animating
    }
    screen.setBrightness(v);
  }
  // v == 0: the floor brightness is applied by the off-frame branch instead.
  panelBrightness = v;
}

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
};
// Track what the panel ACTUALLY shows. Boot draws the splash, so that's the
// initial state — initializing to ERR_NONE would make the first
// showScreen(ERR_NONE) a no-op and leave the splash under the map.
static ErrorKind shownError = ERR_SPLASH;

// Splash animation state: the roundel bounces around the panel while the
// wall loads (Wi-Fi/NTP waits, mDNS broker discovery, and the first poll all
// block inside library code, out of reach of the loop's frame scheduler).
static SplashState splashState;
static void drawSplashFrame();  // defined with the screen helpers below

// The splash pump. Every blocking step of the boot path (Wi-Fi, NTP, mDNS
// discovery's listen window, the first TLS poll) sits inside a library call
// that never returns to loop(), so no frame would get drawn and the panel
// would sit black from the moment the splash is torn down until the first
// snapshot lands. This timer draws one splash frame per tick from the timer
// service task instead, which runs whether or not the loop is blocked. It is
// started with the first splash frame and stopped by showScreen() the moment
// the panel shows something else; the guard in the tick makes a stray fire
// harmless.
static TimerHandle_t splashTimer = nullptr;
static_assert(SPLASH_FPS >= 1 && SPLASH_FPS <= 1000,
              "SPLASH_FPS must fit the 1 kHz FreeRTOS tick");
static void splashTick(TimerHandle_t) {
  if (shownError == ERR_SPLASH && panelBrightness != 0) {
    drawSplashFrame();
  }
}
static void splashPumpStart() {
  if (splashTimer == nullptr) {
    splashTimer = xTimerCreate("splash", pdMS_TO_TICKS(1000 / SPLASH_FPS),
                               pdTRUE, nullptr, splashTick);
  }
  if (splashTimer == nullptr || xTimerStart(splashTimer, 0) != pdPASS) {
    // A frozen splash is still better than black, but say so on the console:
    // without this the field symptom is indistinguishable from the bug the
    // pump fixes.
    Serial.println("splash pump failed to start");
  }
}
static void splashPumpStop() {
  if (splashTimer != nullptr) {
    xTimerStop(splashTimer, 0);
  }
}

// Repaint a pixel a dot has vacated: the empty map is black, so restoring
// is one write. (1,0) belongs to the data status pixel — a dot landing
// there is overpainted by the per-frame drawStatusPixel() anyway.
static void clearMapPixel(int x, int y) {
  if (x < 0 || y < 0 || x >= DISPLAY_W || y >= DISPLAY_H) {
    return;
  }
  screen.drawPixel(x, y, 0, 0, 0);
  if (x == 1 && y == 0) {
    drawStatusPixel();  // the data status pixel lives at (1,0)
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
      clearMapPixel(actives[i].dx, actives[i].dy);
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
    clearMapPixel(a.dx, a.dy);
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
  static bool mqttPixelOn = false;
  uint8_t r, g, b;
  if (pollFailures == 0) {
    r = 0; g = 160; b = 0;      // fresh
  } else if (pollFailures < STALE_AFTER_POLLS) {
    r = 200; g = 120; b = 0;    // aging
  } else {
    r = 200; g = 0; b = 0;      // error
  }
  screen.drawPixel(1, 0, r, g, b);
  // Second status pixel: purple while the Home Assistant link is up, red
  // when the link is configured but down (broker not found, auth refused,
  // WiFi lost), and absent when the link is not configured at all. When
  // the pixel goes dark, restore what was under it so no color lingers.
  if (haEnabled()) {
    mqttPixelOn = true;
    if (haConnected()) {
      screen.drawPixel(2, 0, 110, 0, 150);  // deep violet, kept below the data pixel's punch
    } else {
      screen.drawPixel(2, 0, 200, 0, 0);  // same red as the data pixel's error state
    }
  } else if (mqttPixelOn) {
    mqttPixelOn = false;
    if (panelBrightness == 0 || shownError != ERR_NONE) {
      screen.drawPixel(2, 0, 0, 0, 0);  // off-frame / status screen bg is black
    } else {
      screen.drawPixel(2, 0, 0, 0, 0);  // empty map is black
      redrawOccupants(2, 0, -1);  // a dot may have been parked under the pixel
    }
  }
#endif
}

// Paint the panel for a (possibly new) error state. No-op when the panel
// already shows it, so the retry loop can call this every cycle without
// repainting. Clearing back to the map starts from a black panel (dots are
// dropped — they were drawn under the error screen).
static void putScreenPixel(void *, int x, int y, uint8_t r, uint8_t g,
                           uint8_t b) {
  screen.drawPixel(x, y, r, g, b);
}

// One frame of the bouncing splash. Driven at SPLASH_FPS by the splashTick
// timer callback; called directly only for the first frame (setup/showScreen).
static void drawSplashFrame() {
  screen.clear();
  showSplashFrame(putScreenPixel, nullptr, &splashState);
}

static void showScreen(ErrorKind kind) {
  if (kind == shownError) {
    return;
  }
  shownError = kind;
  // The splash pump owns the panel while the splash is up; anything else
  // takes it over, so the pump must not draw over it. A tick already in
  // flight when stop() is queued still finishes, so wait it out: one tick
  // guarantees nothing can repaint over the screen drawn below. showScreen
  // only runs from setup()/loop(), where 100 ms at a screen transition is
  // invisible.
  if (kind != ERR_SPLASH) {
    splashPumpStop();
    delay(1000 / SPLASH_FPS + 1);
  }
  // Every status screen replaces whatever was on the panel wholesale:
  // wipe first so no pixels from the previous screen bleed through.
  screen.clear();
  switch (kind) {
    case ERR_SPLASH:
      showSplashFrame(putScreenPixel, nullptr, &splashState);
      splashPumpStart();
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
    case ERR_NONE:
    default:
      // (The panel is already cleared above; no extra paint needed.)
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
  screen.setBrightness(DISPLAY_BRIGHTNESS);  // boot default; NVS value applied after provLoad

  // Splash while we figure out what to show: the roundel bouncing DVD-style
  // around the panel while the wall loads. Replaced by an error screen below,
  // or by the map once the first poll lands in loop() (its showScreen
  // stops the pump and takes the panel). Seed from the SoC RNG so every boot
  // takes a different path (initial diagonal and corner bounces both derive
  // from this).
  screen.clear();
  splashState.seed = esp_random();
  drawSplashFrame();
  splashPumpStart();  // ticks from the timer task: the waits below block

  provLoad();
  // The panel brightness the user last set from Home Assistant (if ever)
  // lives in NVS; restore it before anything is drawn. With no stored
  // value this is DISPLAY_BRIGHTNESS again — no behavior change for walls
  // that never see an MQTT broker.
  haBegin(applyPanelBrightness);
  applyPanelBrightness(haBrightness());
  if (provNeeded()) {
    // No complete config in NVS yet (needs ssid + host). Stay up on the
    // serial console: the browser tool (or any terminal) can `set` values;
    // the loop picks them up live — no reflash, no reboot needed.
    Serial.println("UNPROVISIONED: open the browser tool and set "
                   "ssid/pass/host, then save.");
    showScreen(ERR_PROVISION);
    return;
  }

  connectWifi();
  syncTime();
  if (WiFi.status() != WL_CONNECTED) {
    showScreen(ERR_WIFI);
  }
  // Wi-Fi is up, but the wall has nothing to show yet: leave the splash
  // running. Two more blocking steps stand between here and the first dots
  // — haService()'s mDNS broker discovery (a 2.5 s listen window per service
  // query, seconds long on a miss) and the first TLS poll — and clearing to
  // a blank map before them is exactly the black screen this avoids. The
  // first successful poll calls showScreen(ERR_NONE), which clears the panel
  // and hands it to the map.
}

void loop() {
  uint32_t now = millis();

  // Serial provisioning commands work at any time: unprovisioned, or live
  // while the wall runs (a corrected password takes effect on the next
  // connect attempt without a reboot).
  provPumpSerial();

  // Home Assistant link: no-op unless a broker was provisioned. Runs every
  // loop so keepalive and brightness commands land between polls.
  haService();

  if ((int32_t)(now - nextPollMs) >= 0) {
    // Fallback cadence only for failed polls; a successful poll re-arms
    // from the server's advertised "next".
    nextPollMs = now + POLL_INTERVAL_MS;
    if (provNeeded()) {
      showScreen(ERR_PROVISION);
      return;  // still waiting for set ssid/pass
    }
    if (WiFi.status() != WL_CONNECTED) {
      connectWifi();
      if (WiFi.status() != WL_CONNECTED) {
        showScreen(ERR_WIFI);
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
          clearMapPixel(actives[i].dx, actives[i].dy);
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
    if (panelBrightness == 0) {
      // "Off" is not fully dark: the map is out, but the status pixels
      // stay lit at ~1% so the wall still reports alive/MQTT state.
      screen.setBrightness(STATUS_PIXEL_FLOOR);
      screen.clear();
      drawStatusPixel();
    } else if (shownError == ERR_SPLASH) {
      // The splash pump owns the panel: it ticks from the timer task, which
      // keeps drawing while this loop is blocked in discovery or a TLS poll.
      // Nothing here may paint over it (the status pixels would land on the
      // roundel). Consume any mapDirty raised by a brightness change during
      // the splash window: the splash covers the whole panel anyway and the
      // first showScreen(ERR_NONE) wipes and re-merges the map wholesale, so
      // a deferred flag would only make that first map flash and re-draw.
      mapDirty = false;
    } else {
      if (mapDirty) {
        // Back from the off frame: the frame was wiped, so repaint the map
        // wholesale (same as showScreen(ERR_NONE), but forced) and re-merge
        // the current snapshot so vehicles are back at once, not next poll.
        // With activeCount reset to 0 every vehicle re-adds in place, so
        // the glide-stagger arguments never apply.
        mapDirty = false;
        shownError = ERR_NONE;
        screen.clear();  // the map is vehicles on black — nothing to repaint under them
        activeCount = 0;
        mergeVehicles(now, 0.0f, 60);
      }
      animate(now);
    }
  }

  delay(2);
}
