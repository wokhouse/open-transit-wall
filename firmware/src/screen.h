#pragma once
// Display abstraction over ESP32-HUB75-MatrixPanel-I2S-DMA, including the
// tile mapping for chained panels (instead of the library's
// VirtualMatrixPanel, so chain layouts stay deterministic across versions).

#include <stdint.h>

#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "config.h"

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
  void drawPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
  void clear();
  void drawBackdrop();  // RLE water/land bitmap from backdrop.h
  void drawBorder();    // perimeter stroke from GRID_BORDER_RGB in config.h

 private:
  // Physical x on the linear DMA chain for a virtual panel column/row.
  int panelIndex(int col, int row) const;

  MatrixPanel_I2S_DMA *dma_ = nullptr;
};

// Backdrop (generated); defined in backdrop.h.
#include "backdrop.h"

static const uint8_t BACKDROP_LAND[3] = {2, 2, 2};
static const uint8_t BACKDROP_WATER[3] = {0, 0, 0};

#if BACKDROP_W != DISPLAY_W || BACKDROP_H != DISPLAY_H
#error "backdrop.h does not match DISPLAY_W/DISPLAY_H — run tools/gen_backdrop.py"
#endif
