#pragma once
// Display abstraction over ESP32-HUB75-MatrixPanel-I2S-DMA, including the
// tile mapping for chained panels (instead of the library's
// VirtualMatrixPanel, so chain layouts stay deterministic across versions).
//
// Rotation is a provisioning setting, not a build constant: the wall can be
// mounted four ways, so every draw goes through a quarter-turn transform
// (0/90/180/270, clockwise). Callers work in the *logical* frame — the
// rotated picture — and Screen::width()/height() report its size. On a
// square panel the logical frame is the panel frame for every rotation; on a
// non-square tiled wall a 90/270 turn swaps the two dimensions.

#include <stdint.h>

#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "config.h"
#include "display_rotate.h"

// Physical panel frame, as wired (before rotation).
#define DISPLAY_W (PANEL_RES_X * PANEL_CHAIN_W)
#define DISPLAY_H (PANEL_RES_Y * PANEL_CHAIN_H)
#define PANEL_COUNT (PANEL_CHAIN_W * PANEL_CHAIN_H)

// Daisy-chain layouts. PANEL 0's HUB75 output feeds panel 1, etc.
enum TileLayout {
  LINE_TOP_LEFT_DOWN,        // rows top->bottom, each chained left->right
  LINE_TOP_RIGHT_DOWN,       // rows top->bottom, each chained right->left
  LINE_BOTTOM_LEFT_UP,       // rows bottom->top, each chained left->right
  SERPENTINE_TOP_LEFT_DOWN,  // rows top->bottom, alternate direction each row
  SERPENTINE_TOP_RIGHT_DOWN, // rows top->bottom, alternate, starting right->left
};

class Screen {
 public:
  bool begin();
  void setBrightness(uint8_t brightness);  // 0-255
  // Quarter turns are the only orientations a panel mount can take. Anything
  // outside 0/90/180/270 falls back to 0. Returns true when the value
  // changed — the caller then owns the repaint, since every pixel moved.
  bool setRotation(uint16_t degrees);
  uint16_t rotation() const { return rot_; }
  // Logical frame size: what callers draw into (see the header comment).
  int width() const;
  int height() const;
  void drawPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
  void clear();

 private:
  // Physical x on the linear DMA chain for a virtual panel column/row.
  int panelIndex(int col, int row) const;

  MatrixPanel_I2S_DMA *dma_ = nullptr;
  uint16_t rot_ = 0;  // degrees clockwise: 0, 90, 180, 270
};
