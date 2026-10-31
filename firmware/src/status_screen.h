#pragma once
// On-panel text and QR rendering for the startup splash and error screens.
// The splash is a small roundel logo (logo_bounce.h) bouncing DVD-screensaver
// style around the panel while the wall loads; error screens use the vendored
// 5x7 font (font5x7.h) and the pre-baked web-flasher QR (qr_flasher.h).
// Error screens are drawn once per state change; the splash is animated —
// the incremental-render rule only governs the per-frame map path.
//
// Rendering goes through a PutPixel callback so the firmware (Screen's
// tile-mapped drawPixel) and the native simulator share this code exactly.
//
// House style for these screens: all text horizontally centered; errors in
// red, everything else white; the QR is centered with a few rows of
// padding below it. Each composed screen owns its wording so the firmware
// and the sim can never drift apart.

#include <stdint.h>

typedef void (*PutPixel)(void *ctx, int x, int y, uint8_t r, uint8_t g,
                         uint8_t b);

// Draw one line of ASCII at (x, y) = top-left of the first glyph.
int drawText(PutPixel put, void *ctx, int x, int y, const char *text,
             uint8_t r, uint8_t g, uint8_t b);

// Word-wrap text into centered lines of at most MAX_CHARS characters,
// starting at row y with 8px line pitch; stops after maxLines. Returns
// lines drawn.
int drawWrappedCentered(PutPixel put, void *ctx, int y, const char *text,
                        int maxLines, uint8_t r, uint8_t g, uint8_t b);

// Composed screens (the caller clears the panel first). Layout budget on
// 64x64: headline at y 0-6, at most TWO detail lines at y 8 and 16
// (10 chars each — the bottom row starts at y 30, so nothing fits below
// that), then the bottom row: OTW badge on the left half, web-flasher QR
// on the right half, both 3 rows above the bottom edge. Headlines carry
// the diagnosis; detail lines carry the action ("scan qr to fix X").
// Splash animation state (the caller owns an instance; zero-init is a valid
// start: centered-ish position, diagonal down-right, seed 0 → first corner
// hit picks a fresh direction via the LCG).
struct SplashState {
  int x, y;       // top-left of the bouncing roundel
  int vx, vy;     // px per frame, always ±1
  unsigned seed;  // xorshift seed for random diagonals at corner hits
};

// Draw one frame of the DVD-screensaver splash: advance the roundel,
// bounce off the edges (a corner hit picks a new random diagonal), blit.
// The caller must clear the panel before each frame (house rule: erase
// before every new thing displayed). Call at ~10 fps while the wall loads.
void showSplashFrame(PutPixel put, void *ctx, SplashState *st);
void showSetupNeeded(PutPixel put, void *ctx);   // unprovisioned prompt
void showWifiError(PutPixel put, void *ctx);     // saved wifi won't connect
void showServerError(PutPixel put, void *ctx);   // proxy unreachable
void showAgencyError(PutPixel put, void *ctx);   // proxy 400s the agency id
