// Native simulator for open-transit-wall — renders exactly what the ESP32
// will draw (same map_project.h projection, same palette) to a BMP you can
// inspect, plus an ASCII view for quick checks. The map is vehicles on
// black — there is no backdrop.
//
// Usage: sim <vehicles.csv> <out.bmp> <W> <H> [scale] [dot] [--ascii]
//   vehicles.csv: a "#bbox,lat_min,lat_max,lon_min,lon_max" header line
//   (see tools/vehicles_to_csv.py), then rows: lat,lon,r,g,b
//   The bbox is the one the server ships on the wire — the same source of
//   truth the firmware's projection uses.
//   --screen splash|setup|wifi|server|agency renders a status screen
//   instead of the map (same status_screen.cpp the firmware runs).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "map_project.h"
#include "status_screen.h"

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
    fprintf(stderr, "usage: %s <vehicles.csv> <out.bmp> <W> <H> [scale] [dot] "
                    "[--screen splash|setup|wifi|server|agency] "
                    "[--ascii]\n", argv[0]);
    return 2;
  }
  const char *csvPath = argv[1];
  const char *bmpPath = argv[2];
  int w = atoi(argv[3]), h = atoi(argv[4]);
  int scale = argc > 5 && argv[5][0] != '-' ? atoi(argv[5]) : 8;
  int dot = argc > 6 && argv[6][0] != '-' ? atoi(argv[6]) : 1;
  const char *screen = nullptr;
  bool ascii = false;
  for (int i = 5; i < argc; i++) {
    if (strcmp(argv[i], "--ascii") == 0) ascii = true;
    if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) screen = argv[++i];
  }

  // 0) status screens: same status_screen.cpp the firmware runs, rendered
  // straight into the framebuffer (no vehicles, no background).
  if (screen) {
    fb.assign((size_t)w * h * 3, 0);
    struct SimCtx { int w, h; };
    SimCtx ctx = {w, h};
    auto putCb = +[](void *vctx, int x, int y, uint8_t r, uint8_t g,
                     uint8_t b) {
      SimCtx *c = (SimCtx *)vctx;
      if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
      uint8_t rgb[3] = {r, g, b};
      memcpy(&fb[((size_t)y * c->w + x) * 3], rgb, 3);
    };
    if (!strcmp(screen, "splash")) {
      // The splash is an animation; render one frame after N steps so the
      // BMP shows the roundel mid-bounce (same state machine as firmware).
      SplashState st = {};
      for (int frame = 0; frame < 37; frame++) {
        fb.assign((size_t)w * h * 3, 0);  // the caller clears each frame
        showSplashFrame(putCb, &ctx, &st);
      }
    } else if (!strcmp(screen, "setup")) {
      showSetupNeeded(putCb, &ctx);
    } else if (!strcmp(screen, "wifi")) {
      showWifiError(putCb, &ctx);
    } else if (!strcmp(screen, "server")) {
      showServerError(putCb, &ctx);
    } else if (!strcmp(screen, "agency")) {
      showAgencyError(putCb, &ctx);
    } else {
      fprintf(stderr, "unknown --screen '%s'\n", screen);
      return 2;
    }
    writeBmp(bmpPath, w, h, scale);
    printf("sim: screen '%s' -> %s (%dx%d, scale %d)\n", screen, bmpPath, w,
           h, scale);
    if (ascii) {
      for (int ay = 0; ay < h; ay++) {
        for (int ax = 0; ax < w; ax++) {
          const uint8_t *p = &fb[((size_t)ay * w + ax) * 3];
          if (p[0] > 128 && p[1] > 128 && p[2] > 128) putchar('#');  // QR light
          else if (p[0] | p[1] | p[2]) putchar('+');                 // text
          else putchar('.');                                          // dark
        }
        putchar('\n');
      }
    }
    return 0;
  }

  // 1) vehicles. The bbox travels in the CSV header — the same one the
  // server ships on the wire. stretch mirrors MAP_STRETCH_TO_FILL.
  fb.assign((size_t)w * h * 3, 0);
  FILE *csv = fopen(csvPath, "r");
  if (!csv) { perror("csv"); return 1; }
  MapProjection proj;
  bool haveBbox = false;
  char line[256];
  int count = 0, clipped = 0;
  while (fgets(line, sizeof(line), csv)) {
    if (strncmp(line, "#bbox", 5) == 0) {
      double b[4];
      if (sscanf(line + 5, ",%lf,%lf,%lf,%lf", &b[0], &b[1], &b[2], &b[3]) != 4) {
        fprintf(stderr, "bad #bbox header in %s\n", csvPath);
        return 1;
      }
      BBox bbox = {b[0], b[1], b[2], b[3]};
      proj.begin(bbox, w, h, /*stretchToFill=*/false);
      haveBbox = true;
      continue;
    }
    double lat, lon;
    int r, g, b;
    if (sscanf(line, "%lf,%lf,%d,%d,%d", &lat, &lon, &r, &g, &b) != 5) {
      continue;
    }
    if (!haveBbox) {
      fprintf(stderr, "%s has no #bbox header line — regenerate it with "
                      "tools/vehicles_to_csv.py\n", csvPath);
      return 1;
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
        putchar(p[0] || p[1] || p[2] ? 'o' : ' ');  // vehicle / empty
      }
      putchar('\n');
    }
  }
  return 0;
}
