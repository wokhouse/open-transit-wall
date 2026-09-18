#pragma once
// Lat/lon -> pixel projection. Pure C++ (no Arduino dependencies) so the
// native simulator compiles the exact same math as the firmware.
//
// Two modes:
//   stretch-to-fill — the bounding box maps onto the full display, so data
//                     reaches all four edges on any panel aspect.
//   aspect-fit      — equirectangular with cos(mid-latitude) correction,
//                     aspect preserved, centered (may leave margins).
// The preview page (server/static/preview.html) and tools/gen_backdrop.py
// mirror this math — keep them in sync.

#include <math.h>

struct BBox {
  double latMin, latMax, lonMin, lonMax;
};

class MapProjection {
 public:
  MapProjection() = default;

  void begin(const BBox &box, int width, int height, bool stretchToFill = false) {
    box_ = box;
    width_ = width;
    height_ = height;
    stretch_ = stretchToFill;
    if (stretch_) {
      scaleX_ = (double)width / (box.lonMax - box.lonMin);
      scaleY_ = (double)height / (box.latMax - box.latMin);
      return;
    }
    midCos_ = cos((box.latMin + box.latMax) * 0.5 * M_PI / 180.0);
    double lonSpan = (box.lonMax - box.lonMin) * midCos_;
    double latSpan = (box.latMax - box.latMin);
    scale_ = fmin((double)width / lonSpan, (double)height / latSpan);
    offsetX_ = ((double)width - lonSpan * scale_) * 0.5;
    offsetY_ = ((double)height - latSpan * scale_) * 0.5;
  }

  // Returns false when the point falls outside the display (still projects).
  bool project(double lat, double lon, int *x, int *y) const {
    if (stretch_) {
      *x = (int)floor((lon - box_.lonMin) * scaleX_);
      *y = (int)floor((box_.latMax - lat) * scaleY_);
    } else {
      *x = (int)floor(offsetX_ + (lon - box_.lonMin) * midCos_ * scale_);
      *y = (int)floor(offsetY_ + (box_.latMax - lat) * scale_);
    }
    return *x >= 0 && *y >= 0 && *x < width_ && *y < height_;
  }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  BBox box_{};
  int width_ = 0, height_ = 0;
  bool stretch_ = false;
  double midCos_ = 1.0, scale_ = 1.0, offsetX_ = 0.0, offsetY_ = 0.0;
  double scaleX_ = 1.0, scaleY_ = 1.0;
};
