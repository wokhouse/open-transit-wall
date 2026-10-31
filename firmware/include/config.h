#pragma once
// ============================================================================
// open-transit-wall configuration — this file is the only thing you should
// need to touch to retarget the display (size, area, look).
//
// The project targets a single 64x64 panel (PANEL_RES_X/Y below, chain 1x1).
// The code derives everything from those numbers, so a tiled wall is
// possible but untested. Nothing per-agency is compiled: the panel size is
// the only display constant, and adding an agency is a server-side change.
// ============================================================================

// --- Panel hardware ---------------------------------------------------------
#define PANEL_RES_X 64            // pixels per panel, X
#define PANEL_RES_Y 64            // pixels per panel, Y
#define PANEL_CHAIN_W 1           // panels tiled horizontally
#define PANEL_CHAIN_H 1           // panels tiled vertically

// How the chained panels are wired with HUB75 ribbons (constants defined in
// screen.h; for a single panel this is ignored):
//   LINE_TOP_LEFT_DOWN        rows, each chained left->right, top row first
//   LINE_TOP_RIGHT_DOWN       rows, each chained right->left
//   LINE_BOTTOM_LEFT_UP       rows starting from the bottom, chaining upward
//   SERPENTINE_TOP_LEFT_DOWN  rows alternate direction (boustrophedon)
//   SERPENTINE_TOP_RIGHT_DOWN ... starting right->left
#define TILE_LAYOUT LINE_TOP_LEFT_DOWN

// --- Board pinout -----------------------------------------------------------
// Full HUB75 GPIO map per board; the platformio.ini env selects the board
// with -DOTW_BOARD_MATRIXPORTAL_S3 (default: Waveshare). Every line is set
// explicitly — do not rely on the library's per-chip defaults.
#if defined(OTW_BOARD_MATRIXPORTAL_S3)
// Adafruit MatrixPortal S3 (https://www.adafruit.com/product/5778).
// Map from Adafruit's CircuitPython board module (pins.c). The panel lines
// include GPIO35-37, which the module's octal PSRAM would claim: this
// target must build with PSRAM off (the board def's qio_qspi memory type
// does that). 64x64 panels also need the board's E-line solder jumper
// closed (toward the pad marked 8).
#define HUB75_PIN_R1 42
#define HUB75_PIN_G1 41
#define HUB75_PIN_B1 40
#define HUB75_PIN_R2 38
#define HUB75_PIN_G2 39
#define HUB75_PIN_B2 37
#define HUB75_PIN_A  45
#define HUB75_PIN_B  36
#define HUB75_PIN_C  48
#define HUB75_PIN_D  35
#define HUB75_PIN_E  21
#define HUB75_PIN_CLK 2
#define HUB75_PIN_LAT 47
#define HUB75_PIN_OE 14
#else
// Waveshare ESP32-S3-RGB-Matrix (https://www.waveshare.com/esp32-s3-rgb-matrix.htm).
// Matches the library's ESP32-S3 defaults (verified against
// esp32s3-default-pins.hpp) plus E on GPIO9 — 64x64 panels are 1/32-scan
// and need E wired to the HUB75 connector's pin 8 (the library default
// -1 is only correct for 1/16-scan 64x32 panels).
#define HUB75_PIN_R1 4
#define HUB75_PIN_G1 5
#define HUB75_PIN_B1 6
#define HUB75_PIN_R2 7
#define HUB75_PIN_G2 15
#define HUB75_PIN_B2 16
#define HUB75_PIN_A  18
#define HUB75_PIN_B  8
#define HUB75_PIN_C  3
#define HUB75_PIN_D  42
#define HUB75_PIN_E  9
#define HUB75_PIN_CLK 41
#define HUB75_PIN_LAT 40
#define HUB75_PIN_OE 2
#endif

// --- Display look -----------------------------------------------------------
// Boot brightness (0-255; panels are very bright indoors). If a Home
// Assistant MQTT broker is provisioned, the wall's real brightness is the
// last value HA set — persisted in NVS (see src/ha_link.h) — and this is
// only the fallback for walls that never had one.
#define DISPLAY_BRIGHTNESS 48
// Status pixels along the top edge: (1,0) green=fresh, amber=aging,
// red=error; (2,0) purple while the Home Assistant MQTT link is up, red
// when the link is configured but down, absent when it is not configured.
// Even when the panel is "off" (HA OFF or 0%), the frame keeps only these
// pixels lit at STATUS_PIXEL_FLOOR (~1%) so the wall still reports state.
#define STATUS_PIXEL_ENABLED 1
#define STATUS_PIXEL_FLOOR   3

// --- Map area ---------------------------------------------------------------
// Nothing per-agency is compiled at all: the proxy ships the bounding box
// per agency in the /vehicles payload ("bbox", from server/agencies.json),
// and the projection initializes from it. Vehicles outside are clipped. The
// map is vehicles on black — there is no backdrop or land layer, so adding
// an agency never needs a reflash.

// 0 = preserve true geographic aspect, centered (leaves margins on panels
// wider/narrower than the map's aspect). 1 = stretch the bounding box to fill
// the whole display: data reaches all four edges on any panel aspect (the map
// is distorted to the panel shape).
#define MAP_STRETCH_TO_FILL 0

// --- Data source (the mini-proxy, not 511 directly) -------------------------
// NO compiled defaults: host, scheme and port are provisioned over WebSerial
// and stored in NVS — see src/provisioner.h and docs/provisioning-protocol.md.
// The wall does not touch the network until an SSID and a host are both
// provisioned. HTTPS chains are verified against the roots pinned in
// include/proxy_ca.h; plain HTTP (scheme=http://, port=8000) is the LAN dev
// path against server/proxy.py — provisioned, never compiled.
#define POLL_INTERVAL_MS 12000UL  // ESP32-side refresh; proxy rate-limits 511 itself
#define STALE_AFTER_POLLS 3       // status pixel goes amber after this many failed polls
#define SPLASH_FPS 10             // boot splash tick rate (see the splash pump)

#define MAX_VEHICLES 1200         // sized for the largest served fleet: MTA NYC subway
                                  // observed 584 trains off-peak, weekday peak order ~1k
                                  // (SFMTA, the other reference fleet, runs ~676)

// --- Per-vehicle motion ------------------------------------------------------
// The proxy publishes positions every POLL_SECONDS and advertises the exact
// seconds until its next upstream fetch in the payload ("next"). The
// *remaining* refresh cycle is split into one equal slot per vehicle whose
// position changed; each glides to its new pixel over MOTION_GLIDE_MS at a
// random moment inside its slot — so glides tile the whole cycle evenly
// instead of bursting after each snapshot or clumping by chance. There is
// no compiled refresh constant: `next` is authoritative per response.
#define MOTION_GLIDE_MS  5000     // how long a single glide takes
#define RENDER_FPS       20       // continuous re-render rate
