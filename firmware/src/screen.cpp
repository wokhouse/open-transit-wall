#include "screen.h"

bool Screen::begin() {
  HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, PANEL_COUNT);
#ifdef E_PIN_OVERRIDE
  cfg.gpio.e = E_PIN_OVERRIDE;
#endif
  dma_ = new MatrixPanel_I2S_DMA();
  return dma_->begin(cfg);
}

void Screen::setBrightness(uint8_t brightness) { dma_->setBrightness8(brightness); }

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
  if (x < 0 || y < 0 || x >= DISPLAY_W || y >= DISPLAY_H) {
    return;
  }
  int col = x / PANEL_RES_X;
  int row = y / PANEL_RES_Y;
  int panel = panelIndex(col, row);
  dma_->drawPixel(panel * PANEL_RES_X + (x % PANEL_RES_X), y % PANEL_RES_Y,
                  dma_->color565(r, g, b));
}

void Screen::clear() { dma_->clearScreen(); }

void Screen::drawBackdrop() {
  // backdrop_rle is row-major (count, colorIndex) pairs; index 0 = land,
  // 1 = water. Total pixels == DISPLAY_W * DISPLAY_H by construction.
  int x = 0, y = 0;
  for (unsigned int i = 0; i + 1 < backdrop_rle_len; i += 2) {
    const uint8_t *rgb = backdrop_rle[i + 1] == 1 ? BACKDROP_WATER : BACKDROP_LAND;
    for (uint8_t n = backdrop_rle[i]; n > 0; n--) {
      drawPixel(x, y, rgb[0], rgb[1], rgb[2]);
      if (++x >= DISPLAY_W) {
        x = 0;
        y++;
      }
    }
  }
}
