// Native simulator for open-transit-wall — renders exactly what the ESP32
// will draw (same map_project.h projection, same backdrop.h RLE blit, same
// palette) to a BMP you can inspect, plus an ASCII view for quick checks.
//
// Usage: sim <vehicles.csv> <out.bmp> <W> <H> [scale] [dot] [--ascii]
//   vehicles.csv rows: lat,lon,r,g,b   (see tools/vehicles_to_csv.py)
//   W/H must match the compiled backdrop.h (BACKDROP_W/BACKDROP_H).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "backdrop.h"
#include "map_project.h"

static const uint8_t LAND[3] = {2, 2, 2};
static const uint8_t WATER[3] = {0, 0, 36};

// Mirror firmware/include/config.h GRID_BORDER_RGB so the sim shows exactly
// what the wall will show: perimeter stroke only, no interior grid.
static const uint8_t GRID_BORDER[3] = {60, 60, 70};

static void put(int x, int y, const uint8_t *rgb, int w, int h);

static void drawBorder(int w, int h) {
  const uint8_t *c = GRID_BORDER;
  if (!(c[0] | c[1] | c[2])) {
    return;
  }
  for (int x = 0; x < w; x++) {
    put(x, 0, c, w, h);
    put(x, h - 1, c, w, h);
  }
  for (int y = 1; y < h - 1; y++) {
    put(0, y, c, w, h);
    put(w - 1, y, c, w, h);
  }
}

struct Veh {
  double lat, lon;
  uint8_t r, g, b;
};

static std::vector<uint8_t> fb;  // W*H*3, row-major RGB

static void put(int x, int y, const uint8_t *rgb, int w, int h) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  memcpy(&fb[(y * w + x) * 3], rgb, 3);
}

static void writeBmp(const char *path, int w, int h, int scale) {
  int W = w * scale, H = h * scale;
  int rowBytes = (W * 3 + 3) & ~3;
  uint32_t fileSize = 54 + (uint32_t)rowBytes * H;
  FILE *f = fopen(path, "wb");
  if (!f) { perror("bmp"); exit(1); }
  uint8_t header[54] = {0};
  header[0] = 'B'; header[1] = 'M';
  memcpy(header + 2, &fileSize, 4);
  header[10] = 54;                       // pixel data offset
  header[14] = 40;                       // DIB header size
  memcpy(header + 18, &W, 4);
  memcpy(header + 22, &H, 4);
  header[26] = 1;                        // planes
  header[28] = 24;                       // bpp
  fwrite(header, 1, 54, f);
  std::vector<uint8_t> row(rowBytes, 0);
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) {
      const uint8_t *src = &fb[(((y / scale) * w) + (x / scale)) * 3];
      row[x * 3 + 0] = src[2];  // BMP is BGR
      row[x * 3 + 1] = src[1];
      row[x * 3 + 2] = src[0];
    }
    fwrite(row.data(), 1, rowBytes, f);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <vehicles.csv> <out.bmp> <W> <H> [scale] [dot] [--ascii]\n",
            argv[0]);
    return 2;
  }
  const char *csvPath = argv[1];
  const char *bmpPath = argv[2];
  int w = atoi(argv[3]), h = atoi(argv[4]);
  int scale = argc > 5 && argv[5][0] != '-' ? atoi(argv[5]) : 8;
  int dot = argc > 6 && argv[6][0] != '-' ? atoi(argv[6]) : 1;
  bool ascii = false;
  for (int i = 5; i < argc; i++) {
    if (strcmp(argv[i], "--ascii") == 0) ascii = true;
  }

  if (w != BACKDROP_W || h != BACKDROP_H) {
    fprintf(stderr, "backdrop.h is %dx%d but sim asked for %dx%d — regenerate with "
                    "tools/gen_backdrop.py\n", BACKDROP_W, BACKDROP_H, w, h);
    return 1;
  }

  // 1) backdrop via the same RLE decode the firmware runs
  fb.assign((size_t)w * h * 3, 0);
  int x = 0, y = 0;
  for (unsigned int i = 0; i + 1 < backdrop_rle_len; i += 2) {
    const uint8_t *rgb = backdrop_rle[i + 1] == 1 ? WATER : LAND;
    for (uint8_t n = backdrop_rle[i]; n > 0; n--) {
      put(x, y, rgb, w, h);
      if (++x >= w) { x = 0; y++; }
    }
  }
  drawBorder(w, h);

  // 2) vehicles — bbox mirrors server/geo.py BBOX; stretch mirrors
  // MAP_STRETCH_TO_FILL in config.h
  BBox bbox = {37.705, 37.810, -122.512, -122.385};
  MapProjection proj;
  proj.begin(bbox, w, h, /*stretchToFill=*/false);

  FILE *csv = fopen(csvPath, "r");
  if (!csv) { perror("csv"); return 1; }
  char line[256];
  int count = 0, clipped = 0;
  while (fgets(line, sizeof(line), csv)) {
    double lat, lon;
    int r, g, b;
    if (sscanf(line, "%lf,%lf,%d,%d,%d", &lat, &lon, &r, &g, &b) != 5) {
      continue;
    }
    int px, py;
    if (!proj.project(lat, lon, &px, &py)) { clipped++; continue; }
    const uint8_t rgb[3] = {(uint8_t)r, (uint8_t)g, (uint8_t)b};
    for (int dy = 0; dy < dot; dy++) {
      for (int dx = 0; dx < dot; dx++) {
        put(px + dx, py + dy, rgb, w, h);
      }
    }
    count++;
  }
  fclose(csv);

  writeBmp(bmpPath, w, h, scale);
  printf("sim: %d vehicles drawn, %d outside bbox -> %s (%dx%d, scale %d)\n",
         count, clipped, bmpPath, w, h, scale);

  if (ascii) {
    for (int ay = 0; ay < h; ay++) {
      for (int ax = 0; ax < w; ax++) {
        const uint8_t *p = &fb[(ay * w + ax) * 3];
        if (p[0] > 40 || p[1] > 40 || p[2] > 80) {
          putchar('o');  // vehicle (bright)
        } else if (p[2] > 16) {
          putchar('.');  // water
        } else if (p[0] | p[1] | p[2]) {
          putchar('#');  // land
        } else {
          putchar(' ');
        }
      }
      putchar('\n');
    }
  }
  return 0;
}
