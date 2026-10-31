/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of the Ghoti.io suite's shape benchmark harness.
 *
 * It is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License version 3 as published by
 * the Free Software Foundation.
 *
 * It is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Which luminance coefficients Skia's luma color filter uses (the masks scene, README.md "Masks"), found by drawing and
 * reading the pixels back, through Skia's public API only, in the pinned Skia image:
 *
 *     probe-skia-luma           prints, for each set, how many probe colors Skia's output agrees with
 *
 * Built and run like the driver, in the Skia image, with the compile and link flags `run.sh` gives `refs/bench-skia.cpp` (its
 * `skia` engine's command; the probe needs neither the scene reader nor a scene).
 *
 * The filter takes a layer of an opaque 8-bit color (r, g, b), as the masks scene draws a mask's picture, and leaves
 * (0, 0, 0, y) over a transparent surface; y is read back.  The candidates are shape-scene's sets, as exact integers over
 * their denominator: W3C (2125, 7154, 721) / 10000, REC709 (2126, 7152, 722) / 10000 and LUM (30, 59, 11) / 100.  The
 * sets differ from one another by at most a twentieth of an 8-bit level for W3C against REC709, so the probe colors are
 * those where the two round to different levels with at least a hundredth of a level to spare on each side of the
 * rounding point: for such a color exactly one of the two predicts Skia's output, if Skia rounds the exact sum to the
 * nearest level.  A color that agrees with neither is counted too.  It then probes a translucent layer (the filter folds
 * the layer's alpha into the result, which is a luminance of the premultiplied color over black) and prints the primaries.
 */

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkLumaColorFilter.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct Set {
  const char * name;
  int k[3];
  int den;
};

const Set SETS[3] = {{"w3c", {2125, 7154, 721}, 10000}, {"rec709", {2126, 7152, 722}, 10000}, {"lum", {30, 59, 11}, 100}};

// The exact sum in levels (a real), and its nearest level.
double exact(const Set & s, int r, int g, int b) {
  return double(s.k[0] * r + s.k[1] * g + s.k[2] * b) / double(s.den);
}
int nearest(double v) {
  return int(std::floor(v + 0.5));
}
// How far v is from the rounding point (a half level), in levels.
double margin(double v) {
  return std::fabs(v - std::floor(v) - 0.5);
}

struct Color {
  int r, g, b, a;
};

// Draw each color as a layer of one pixel through the luma filter; the filter's output for color i is pixel i of the row.
std::vector<int> through_filter(const std::vector<Color> & colors) {
  const int n = int(colors.size());
  SkImageInfo info = SkImageInfo::Make(n, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas * canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);
  SkPaint layer;
  layer.setColorFilter(SkLumaColorFilter::Make());
  for (int i = 0; i < n; i++) {
    SkRect cell = SkRect::MakeXYWH(SkScalar(i), 0, 1, 1);
    canvas->saveLayer(&cell, &layer);
    SkPaint fill;
    fill.setAntiAlias(false);
    fill.setBlendMode(SkBlendMode::kSrc);
    fill.setColor(SkColorSetARGB(colors[i].a, colors[i].r, colors[i].g, colors[i].b));
    canvas->drawRect(cell, fill);
    canvas->restore();
  }
  std::vector<uint8_t> px(size_t(n) * 4);
  surface->readPixels(info, px.data(), size_t(n) * 4, 0, 0);
  std::vector<int> out;
  out.resize(size_t(n));
  for (int i = 0; i < n; i++) {
    out[size_t(i)] = px[size_t(i) * 4 + 3];
    if (px[size_t(i) * 4] || px[size_t(i) * 4 + 1] || px[size_t(i) * 4 + 2]) {
      std::printf("probe-skia-luma: color %d,%d,%d,%d left a color (%d,%d,%d)\n", colors[i].r, colors[i].g, colors[i].b,
          colors[i].a, px[size_t(i) * 4], px[size_t(i) * 4 + 1], px[size_t(i) * 4 + 2]);
    }
  }
  return out;
}

}  // namespace

int main() {
  // The colors where W3C and REC709 round apart, with room on both sides.
  std::vector<Color> separating;
  for (int r = 0; r < 256; r++) {
    for (int g = 0; g < 256; g++) {
      for (int b = 0; b < 256; b++) {
        double w = exact(SETS[0], r, g, b), v = exact(SETS[1], r, g, b);
        if (nearest(w) != nearest(v) && margin(w) > 0.01 && margin(v) > 0.01 && nearest(exact(SETS[2], r, g, b)) != nearest(w)) {
          separating.push_back({r, g, b, 255});
        }
      }
    }
  }
  std::printf("colors where w3c and rec709 round apart, each clear of its rounding point by 0.01 level, and lum differs: %zu\n",
      separating.size());
  // Every eighth of them, 4096 at most.
  std::vector<Color> sample;
  for (size_t i = 0; i < separating.size() && sample.size() < 4096; i += (separating.size() + 4095) / 4096) {
    sample.push_back(separating[i]);
  }
  std::vector<int> got = through_filter(sample);
  int agree[3] = {0, 0, 0}, neither = 0, off_by_one[3] = {0, 0, 0};
  for (size_t i = 0; i < sample.size(); i++) {
    bool any = false;
    for (int s = 0; s < 3; s++) {
      int want = nearest(exact(SETS[s], sample[i].r, sample[i].g, sample[i].b));
      agree[s] += got[i] == want;
      off_by_one[s] += std::abs(got[i] - want) <= 1;
      any = any || got[i] == want;
    }
    neither += !any;
  }
  std::printf("probed %zu opaque colors; Skia's output equals the nearest level of:\n", sample.size());
  for (int s = 0; s < 3; s++) {
    std::printf("  %-7s %5d of %zu exactly, %5d within one level\n", SETS[s].name, agree[s], sample.size(), off_by_one[s]);
  }
  std::printf("  neither of the three: %d\n", neither);

  // The primaries and white, and a translucent layer: the output is a * luma(straight) = luma of the premultiplied color.
  std::vector<Color> special = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255},
      {255, 0, 0, 128}, {0, 255, 0, 128}, {0, 0, 255, 128}, {255, 255, 255, 128}, {200, 100, 50, 77}, {40, 220, 180, 200}};
  std::vector<int> sp = through_filter(special);
  std::printf("%-18s %6s", "color", "skia");
  for (int s = 0; s < 3; s++) {
    std::printf(" %7s", SETS[s].name);
  }
  std::printf("   (the exact luma of the premultiplied color, in levels)\n");
  for (size_t i = 0; i < special.size(); i++) {
    const Color & c = special[i];
    std::printf("%3d,%3d,%3d a=%3d   %6d", c.r, c.g, c.b, c.a, sp[i]);
    for (int s = 0; s < 3; s++) {
      std::printf(" %7.3f", exact(SETS[s], c.r, c.g, c.b) * c.a / 255.0);
    }
    std::printf("\n");
  }
  return 0;
}
