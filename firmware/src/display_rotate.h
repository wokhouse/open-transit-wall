#pragma once
// The pixel transform behind Screen's provisioning-time rotation, kept free
// of Arduino and DMA dependencies so a host build can check it (the native
// simulator and a throwaway test both can).
//
// Callers draw into the *logical* frame — the picture as the wall hangs. A
// clockwise quarter turn maps logical -> physical panel coordinates. On a
// square panel the logical frame is the panel frame for every rotation; a
// 90/270 turn on a non-square tiled wall swaps width and height.

#include <stdint.h>

inline bool rotateSwapsAxes(uint16_t degrees) {
  return degrees == 90 || degrees == 270;
}

inline int rotatedWidth(uint16_t degrees, int physW, int physH) {
  return rotateSwapsAxes(degrees) ? physH : physW;
}

inline int rotatedHeight(uint16_t degrees, int physW, int physH) {
  return rotateSwapsAxes(degrees) ? physW : physH;
}

// Logical (x, y) -> physical (px, py). Returns false when the point falls
// outside the logical frame. Anything that is not a quarter turn is 0.
inline bool rotatePixel(uint16_t degrees, int x, int y, int physW, int physH,
                        int *px, int *py) {
  if (x < 0 || y < 0 || x >= rotatedWidth(degrees, physW, physH) ||
      y >= rotatedHeight(degrees, physW, physH)) {
    return false;
  }
  switch (degrees) {
    // A clockwise quarter turn sends the logical top edge to the panel's
    // right edge: logical (0,0) lands on the panel's top-right corner.
    case 90:  *px = physW - 1 - y; *py = x; break;
    case 180: *px = physW - 1 - x; *py = physH - 1 - y; break;
    case 270: *px = y; *py = physH - 1 - x; break;
    case 0:
    default:  *px = x; *py = y; break;
  }
  return true;
}
