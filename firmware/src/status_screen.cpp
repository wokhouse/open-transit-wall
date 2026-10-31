// On-panel text and QR rendering for the startup splash and error screens.
// See status_screen.h for the contract. Everything goes through the
// PutPixel callback so the firmware (Screen's tile-mapped drawPixel) and
// the native simulator render byte-identical screens.

#include "status_screen.h"

#include <stdio.h>
#include <string.h>

#include "qr_flasher.h"
#include "font5x7.h"
#include "logo_bounce.h"
#include "logo_badge.h"

#define PANEL_W 64
#define PANEL_H 64

#define GLYPH_PITCH 6  // 5px glyph + 1px gap
#define LINE_PITCH 8   // 7px glyph + 1px gap

// 64px wide panel at 6px/char: 10 chars per line.
#define MAX_CHARS 10

// Bottom row of error screens: badge and QR share one row. The QR is 1px
// from the right edge and 1px from the badge (ROW_EDGE/ROW_GAP); the badge
// is vertically centered against the QR. The badge bitmap is square (the
// generator pads its crop to a square), so its top/bottom margins are equal
// by construction.
#define ROW_EDGE 1
#define ROW_GAP 1
#define QR_SIDE (QR_FLASHER_SIZE + 2)  // symbol + 1-module quiet zone

static const uint8_t C_TEXT[3] = {255, 255, 255};  // all non-error text
static const uint8_t C_ERROR[3] = {255, 0, 0};     // error screens, all of them
static const uint8_t C_QR[3] = {255, 255, 255};    // QR light color

// Logo palette — the roundel's colors from the source SVG (CTA-mark style).
// Index order matches logo_bounce.h/logo_badge.h: 1 = red, 2 = blue,
// 3 = white.
static const uint8_t C_LOGO[3][3] = {
    {0xD3, 0x11, 0x45},  // 1: red ring
    {0x00, 0x78, 0xC1},  // 2: blue disc
    {0xFF, 0xFF, 0xFF},  // 3: white ring + wordmark
};

int drawText(PutPixel put, void *ctx, int x, int y, const char *text,
             uint8_t r, uint8_t g, uint8_t b) {
  int cx = x;
  for (const char *p = text; *p; p++) {
    char c = *p;
    if (c < FONT5X7_FIRST || c > FONT5X7_LAST) {
      c = '?';
    }
    const uint8_t *glyph = &font5x7[(c - FONT5X7_FIRST) * 5];
    for (int col = 0; col < 5; col++) {
      uint8_t bits = glyph[col];
      for (int row = 0; row < 7; row++) {
        if (bits & (1u << row)) {
          put(ctx, cx + col, y + row, r, g, b);
        }
      }
    }
    cx += GLYPH_PITCH;
  }
  return cx - x;
}

// Draw one line centered on the panel. Longer than MAX_CHARS: truncated.
static int drawCentered(PutPixel put, void *ctx, int y, const char *text,
                        uint8_t r, uint8_t g, uint8_t b) {
  char line[MAX_CHARS + 1];
  size_t n = strlen(text);
  if (n > MAX_CHARS) {
    n = MAX_CHARS;
  }
  memcpy(line, text, n);
  line[n] = '\0';
  int width = (int)n * GLYPH_PITCH - 1;  // trailing gap doesn't count
  int x = (PANEL_W - width) / 2;
  return drawText(put, ctx, x, y, line, r, g, b);
}

// Greedy word-wrap into centered lines; words longer than MAX_CHARS
// hard-break. Stops after maxLines. Returns the number of lines drawn.
int drawWrappedCentered(PutPixel put, void *ctx, int y, const char *text,
                        int maxLines, uint8_t r, uint8_t g, uint8_t b) {
  int lines = 0;
  const char *p = text;
  while (*p && lines < maxLines) {
    while (*p == ' ') {
      p++;
    }
    if (!*p) {
      break;
    }
    char line[MAX_CHARS + 1];
    int n = 0;
    const char *lastSpace = nullptr;
    const char *q = p;
    while (*q && n < MAX_CHARS) {
      if (*q == ' ') {
        lastSpace = q;
      }
      line[n++] = *q++;
    }
    // Break at the last word boundary unless the first word itself is too
    // long (then hard-break at MAX_CHARS).
    if (*q && lastSpace && lastSpace > p) {
      n = (int)(lastSpace - p);
    }
    line[n] = '\0';
    drawCentered(put, ctx, y + lines * LINE_PITCH, line, r, g, b);
    lines++;
    p += n;
  }
  return lines;
}

// Blit a palette-indexed logo bitmap (see logo_splash.h) with its top-left
// at (x, y). Rows are padded to a byte boundary: stride is the padded row
// width in pixels. Index 0 = off: leave the (already cleared) panel dark.
static void blitLogo(PutPixel put, void *ctx, int x, int y,
                     const uint8_t *bits, int w, int h, int stride) {
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      uint8_t byte = bits[((size_t)j * stride + i) >> 2];
      uint8_t idx = (byte >> (6 - 2 * (i & 3))) & 3;
      if (idx == 0) {
        continue;
      }
      const uint8_t *c = C_LOGO[idx - 1];
      put(ctx, x + i, y + j, c[0], c[1], c[2]);
    }
  }
}

// QR at the right of the bottom row (badge owns the left).
// Standard polarity: light up the whole symbol (incl. 1-module quiet
// zone), then blank the dark modules. Phone scanners expect dark-on-light.
static void drawQrRight(PutPixel put, void *ctx) {
  const int x = PANEL_W - ROW_EDGE - QR_SIDE;
  const int y = PANEL_H - ROW_EDGE - QR_SIDE;
  for (int j = 0; j < QR_SIDE; j++) {
    for (int i = 0; i < QR_SIDE; i++) {
      put(ctx, x + i, y + j, C_QR[0], C_QR[1], C_QR[2]);
    }
  }
  for (int my = 0; my < QR_FLASHER_SIZE; my++) {
    for (int mx = 0; mx < QR_FLASHER_SIZE; mx++) {
      if (qr_flasher_rows[my][mx >> 3] & (0x80u >> (mx & 7))) {
        put(ctx, x + 1 + mx, y + 1 + my, 0, 0, 0);
      }
    }
  }
}

// Error screens share this skeleton: red headline at y=0, red wrapped detail
// lines that stop above the bottom row, then that row: OTW badge left of
// the QR. detail2 may be null.
static void showErrorScreen(PutPixel put, void *ctx, const char *headline,
                            const char *detail1, const char *detail2) {
  drawCentered(put, ctx, 0, headline, C_ERROR[0], C_ERROR[1], C_ERROR[2]);
  int y = LINE_PITCH;
  // Lines of text that still fit above the badge/QR row — detail must not
  // run into it or the disc overwrites glyphs.
  int budget = (PANEL_H - ROW_EDGE - QR_SIDE - y) / LINE_PITCH;
  if (detail1 && *detail1) {
    int used = drawWrappedCentered(put, ctx, y, detail1, budget,
                                   C_ERROR[0], C_ERROR[1], C_ERROR[2]);
    y += used * LINE_PITCH;
    budget -= used;
  }
  if (detail2 && *detail2 && budget > 0) {
    drawWrappedCentered(put, ctx, y, detail2, budget, C_ERROR[0], C_ERROR[1],
                        C_ERROR[2]);
  }
  // QR pinned 1px from the right/bottom edges; badge 1px left of the QR,
  // vertically centered against it.
  const int qrX = PANEL_W - ROW_EDGE - QR_SIDE;
  const int qrY = PANEL_H - ROW_EDGE - QR_SIDE;
  const int badgeX = qrX - ROW_GAP - LOGO_BADGE_W;
  const int badgeY = qrY + (QR_SIDE - LOGO_BADGE_H) / 2;
  blitLogo(put, ctx, badgeX, badgeY, logo_badge, LOGO_BADGE_W,
           LOGO_BADGE_H, LOGO_BADGE_STRIDE);
  drawQrRight(put, ctx);
}

// DVD-screensaver splash: a small roundel bouncing around the panel while
// the wall loads. Position/velocity live in the caller's SplashState so the
// animation survives across the blocking boot waits; zero-init is a valid
// start (treated as "fresh": centered, random initial diagonal — seed the
// state from a real RNG, e.g. esp_random(), for per-boot variety).
void showSplashFrame(PutPixel put, void *ctx, SplashState *st) {
  if (st->vx == 0) {
    st->x = (PANEL_W - LOGO_BOUNCE_W) / 2;
    st->y = (PANEL_H - LOGO_BOUNCE_H) / 2;
    st->seed = st->seed * 1103515245u + 12345u;
    st->vx = (st->seed & 1u) ? 1 : -1;
    st->vy = (st->seed & 2u) ? 1 : -1;
  }
  st->x += st->vx;
  st->y += st->vy;
  bool hitX = false, hitY = false;
  if (st->x <= 0) {
    st->x = 0;
    hitX = true;
  } else if (st->x + LOGO_BOUNCE_W >= PANEL_W) {
    st->x = PANEL_W - LOGO_BOUNCE_W;
    hitX = true;
  }
  if (st->y <= 0) {
    st->y = 0;
    hitY = true;
  } else if (st->y + LOGO_BOUNCE_H >= PANEL_H) {
    st->y = PANEL_H - LOGO_BOUNCE_H;
    hitY = true;
  }
  if (hitX) {
    st->vx = -st->vx;
  }
  if (hitY) {
    st->vy = -st->vy;
  }
  if (hitX && hitY) {
    // Corner hit: the classic screensaver picks a fresh random diagonal.
    // Plain LCG — no degenerate zero state, no RNG dependency.
    st->seed = st->seed * 1103515245u + 12345u;
    st->vx = (st->seed & 1u) ? 1 : -1;
    st->vy = (st->seed & 2u) ? 1 : -1;
  }
  blitLogo(put, ctx, st->x, st->y, logo_bounce, LOGO_BOUNCE_W,
           LOGO_BOUNCE_H, LOGO_BOUNCE_STRIDE);
}

void showSetupNeeded(PutPixel put, void *ctx) {
  showErrorScreen(put, ctx, "NOT SET UP", "scan qr to configure", nullptr);
}

void showWifiError(PutPixel put, void *ctx) {
  showErrorScreen(put, ctx, "NO WIFI", "bad wifi. scan qr to fix",
                  nullptr);
}

void showServerError(PutPixel put, void *ctx) {
  showErrorScreen(put, ctx, "NO SERVER", "proxy down. scan qr to fix",
                  nullptr);
}

void showAgencyError(PutPixel put, void *ctx) {
  showErrorScreen(put, ctx, "BAD AGENCY", "wrong agency id. scan qr",
                  nullptr);
}
