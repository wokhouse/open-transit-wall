#pragma once
// ============================================================================
// open-transit-wall configuration — this file is the only thing you should
// need to touch to retarget the display (size, area, look).
//
// The project targets a single 64x64 panel (PANEL_RES_X/Y below, chain 1x1).
// The code derives everything from those numbers, so a tiled wall is
// possible but untested; if you try one, regenerate the backdrop:
//     python3 tools/gen_backdrop.py --agency <id> --w <DISPLAY_W> --h <DISPLAY_H>
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

// Display data pins follow the library defaults (R1=25 G1=26 B1=27 R2=14
// G2=12 B2=13 A=23 B=19 C=5 D=17 CLK=16 LAT=4 OE=15; verified against
// esp32-default-pins.hpp in the library). The E address line is only needed
// for 1/32-scan panels (64x64); the library default is -1 (unused), which is
// correct for 1/16-scan 64x32 panels. 64x64 panels are 1/32-scan and need E
// wired to the HUB75 connector's pin 8:
#define E_PIN_OVERRIDE 32

// --- Display look -----------------------------------------------------------
#define DISPLAY_BRIGHTNESS 48     // 0-255; panels are very bright indoors
#define STATUS_PIXEL_ENABLED 1    // corner pixel: green=fresh, amber=aging, red=error

// --- Map area ---------------------------------------------------------------
// The bounding box is NOT compiled: the proxy ships it per agency in the
// /vehicles payload ("bbox", from server/agencies.json), and the projection
// initializes from it. Vehicles outside are clipped. The backdrop bitmap is
// the one per-agency artifact that stays compiled — regenerate it with
// tools/gen_backdrop.py --agency <id> and reflash to add an agency.

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

#define MAX_VEHICLES 800          // live SFMTA feed observed ~676 vehicles

// --- Per-vehicle motion ------------------------------------------------------
// The proxy publishes positions every POLL_SECONDS and advertises the exact
// seconds until its next upstream fetch in the payload ("next"). Each vehicle
// whose position changed picks a random moment inside the *remaining* refresh
// cycle and glides to its new pixel over MOTION_GLIDE_MS — so glides tile
// the whole cycle instead of bursting right after each snapshot. There is no
// compiled refresh constant: `next` is authoritative per response.
#define MOTION_GLIDE_MS  5000     // how long a single glide takes
#define RENDER_FPS       20       // continuous re-render rate
