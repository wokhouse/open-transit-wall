// Native simulator for open-transit-wall — renders exactly what the ESP32
// will draw (same map_project.h projection, same backdrop.h RLE blit, same
// palette) to a BMP you can inspect, plus an ASCII view for quick checks.
//
// Usage: sim <vehicles.csv> <out.bmp> <W> <H> [scale] [dot] [--agency id] [--ascii]
//   vehicles.csv rows: lat,lon,r,g,b   (see tools/vehicles_to_csv.py)
//   W/H must match the compiled backdrop.h (BACKDROP_W/BACKDROP_H).
//   --agency picks the backdrop entry and its bbox (default: the first one).
//   --screen splash|setup|wifi|server|agency|nomap renders a status screen
//   instead of the map (same status_screen.cpp the firmware runs).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "backdrop.h"
#include "map_project.h"
#include "status_screen.h"

static const uint8_t LAND[3] = {2, 2, 2};
static const uint8_t WATER[3] = {0, 0, 0};

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
                    "[--agency id] [--screen splash|setup|wifi|server|agency|nomap] "
                    "[--ascii]\n", argv[0]);
    return 2;
  }
  const char *csvPath = argv[1];
  const char *bmpPath = argv[2];
  int w = atoi(argv[3]), h = atoi(argv[4]);
  int scale = argc > 5 && argv[5][0] != '-' ? atoi(argv[5]) : 8;
  int dot = argc > 6 && argv[6][0] != '-' ? atoi(argv[6]) : 1;
  const char *agency = nullptr;
  const char *screen = nullptr;
  bool ascii = false;
  for (int i = 5; i < argc; i++) {
    if (strcmp(argv[i], "--ascii") == 0) ascii = true;
    if (strcmp(argv[i], "--agency") == 0 && i + 1 < argc) agency = argv[++i];
    if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) screen = argv[++i];
  }

  if (w != BACKDROP_W || h != BACKDROP_H) {
    fprintf(stderr, "backdrop.h is %dx%d but sim asked for %dx%d — regenerate with "
                    "tools/gen_backdrop.py\n", BACKDROP_W, BACKDROP_H, w, h);
    return 1;
  }

  const BackdropDef *bd = nullptr;
  for (unsigned int i = 0; i < backdrop_count; i++) {
    if (!agency || strcmp(backdrops[i].agency, agency) == 0) { bd = &backdrops[i]; break; }
  }
  if (!bd) {
    fprintf(stderr, "backdrop.h has no entry for agency '%s' — generate it with "
                    "tools/gen_backdrop.py --agency %s\n", agency, agency);
    return 1;
  }

  // 0) status screens: same status_screen.cpp the firmware runs, rendered
  // straight into the framebuffer (no vehicles, no backdrop).
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
    } else if (!strcmp(screen, "nomap")) {
      showNoMapError(putCb, &ctx, "ct");
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

  // 1) backdrop via the same RLE decode the firmware runs
  fb.assign((size_t)w * h * 3, 0);
  int x = 0, y = 0;
  for (unsigned int i = 0; i + 1 < bd->rle_len; i += 2) {
    const uint8_t *rgb = bd->rle[i + 1] == 1 ? WATER : LAND;
    for (uint8_t n = bd->rle[i]; n > 0; n--) {
      put(x, y, rgb, w, h);
      if (++x >= w) { x = 0; y++; }
    }
  }

  // 2) vehicles — the bbox baked into the backdrop entry is the same one
  // the server ships on the wire; stretch mirrors MAP_STRETCH_TO_FILL
  BBox bbox = {bd->bbox[0], bd->bbox[1], bd->bbox[2], bd->bbox[3]};
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
  printf("sim: agency %s, %d vehicles drawn, %d outside bbox -> %s (%dx%d, scale %d)\n",
         bd->agency, count, clipped, bmpPath, w, h, scale);

  if (ascii) {
    for (int ay = 0; ay < h; ay++) {
      for (int ax = 0; ax < w; ax++) {
        const uint8_t *p = &fb[(ay * w + ax) * 3];
        if (p[0] > 40 || p[1] > 40 || p[2] > 80) {
          putchar('o');  // vehicle (bright)
        } else if (p[0] | p[1] | p[2]) {
          putchar('#');  // land
        } else {
          putchar(' ');  // water / background (black)
        }
      }
      putchar('\n');
    }
  }
  return 0;
}
