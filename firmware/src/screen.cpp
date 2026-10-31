#include "screen.h"

bool Screen::begin() {
  HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, PANEL_COUNT);
  // Field order: r1 g1 b1 r2 g2 b2 a b c d e lat oe clk (config.h carries
  // the per-board map).
  cfg.gpio = { HUB75_PIN_R1, HUB75_PIN_G1, HUB75_PIN_B1,
               HUB75_PIN_R2, HUB75_PIN_G2, HUB75_PIN_B2,
               HUB75_PIN_A,  HUB75_PIN_B,  HUB75_PIN_C,
               HUB75_PIN_D,  HUB75_PIN_E,  HUB75_PIN_LAT,
               HUB75_PIN_OE, HUB75_PIN_CLK };
  dma_ = new MatrixPanel_I2S_DMA();
  return dma_->begin(cfg);
}

void Screen::setBrightness(uint8_t brightness) { dma_->setBrightness8(brightness); }

bool Screen::setRotation(uint16_t degrees) {
  if (degrees != 90 && degrees != 180 && degrees != 270) {
    degrees = 0;
  }
  if (degrees == rot_) {
    return false;
  }
  rot_ = degrees;
  return true;
}

// A quarter turn swaps the logical dimensions (a no-op on a square panel).
int Screen::width() const { return rotatedWidth(rot_, DISPLAY_W, DISPLAY_H); }

int Screen::height() const { return rotatedHeight(rot_, DISPLAY_W, DISPLAY_H); }

int Screen::panelIndex(int col, int row) const {
  switch (TILE_LAYOUT) {
    case LINE_TOP_RIGHT_DOWN:
      return row * PANEL_CHAIN_W + (PANEL_CHAIN_W - 1 - col);
    case LINE_BOTTOM_LEFT_UP:
      return (PANEL_CHAIN_H - 1 - row) * PANEL_CHAIN_W + col;
    case SERPENTINE_TOP_LEFT_DOWN:
      return row % 2 == 0 ? row * PANEL_CHAIN_W + col
                          : row * PANEL_CHAIN_W + (PANEL_CHAIN_W - 1 - col);
    case SERPENTINE_TOP_RIGHT_DOWN:
      return row % 2 == 0 ? row * PANEL_CHAIN_W + (PANEL_CHAIN_W - 1 - col)
                          : row * PANEL_CHAIN_W + col;
    case LINE_TOP_LEFT_DOWN:
    default:
      return row * PANEL_CHAIN_W + col;
  }
}

void Screen::drawPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  // Logical (rotated) frame -> physical panel frame (see display_rotate.h).
  int px, py;
  if (!rotatePixel(rot_, x, y, DISPLAY_W, DISPLAY_H, &px, &py)) {
    return;
  }
  int col = px / PANEL_RES_X;
  int row = py / PANEL_RES_Y;
  int panel = panelIndex(col, row);
  dma_->drawPixel(panel * PANEL_RES_X + (px % PANEL_RES_X), py % PANEL_RES_Y,
                  dma_->color565(r, g, b));
}

void Screen::clear() { dma_->clearScreen(); }
