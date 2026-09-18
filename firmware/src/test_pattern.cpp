// Standalone panel test — cycles the whole matrix red -> green -> blue so
// dead/stuck pixels are easy to spot. Shares config.h for panel size, E pin,
// and brightness. Does not touch WiFi or the proxy.
//
// Flash:   pio run -e testpattern -t upload
// Restore: pio run -e esp32dev     -t upload

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#include "config.h"

// Test brightness (0-255), independent of the wall's DISPLAY_BRIGHTNESS.
// The library applies a gamma curve, so mid values look much dimmer than
// expected — 255 is true full. Single-color fills draw ~1/3 the current of
// white; dial back if the supply sags (visible flicker).
#define TEST_BRIGHTNESS 255
static MatrixPanel_I2S_DMA *dma = nullptr;

void setup() {
  Serial.begin(115200);
  HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN_W * PANEL_CHAIN_H);
#ifdef E_PIN_OVERRIDE
  cfg.gpio.e = E_PIN_OVERRIDE;
#endif
  dma = new MatrixPanel_I2S_DMA();
  if (!dma->begin(cfg)) {
    Serial.println("[test] matrix init FAILED");
  }
  dma->setBrightness8(TEST_BRIGHTNESS);
}

void loop() {
  const uint16_t colors[3] = {dma->color565(255, 0, 0), dma->color565(0, 255, 0),
                              dma->color565(0, 0, 255)};
  const char *names[3] = {"RED", "GREEN", "BLUE"};
  for (int i = 0; i < 3; i++) {
    Serial.printf("[test] %s\n", names[i]);
    dma->fillScreen(colors[i]);
    delay(3000);
  }
}
