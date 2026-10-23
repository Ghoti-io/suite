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

/**
 * @file colr-skia.cpp
 *
 * The Skia side of the COLRv1 comparison (color-paint story P4, L13): each glyph of a list drawn by Skia's CPU
 * raster backend through its public API, inside its pinned image only, into one 8-bit RGBA premultiplied cell, and
 * the cells written as the PAM atlas `colr-cmp` compares with `colr-shape`'s. Skia draws COLRv1 through the
 * FreeType backend of the image; this driver sets nothing but the font, the size, no hinting, a black paint (the
 * foreground), palette 0 (Skia's default) and, for a variable font, the instance. It reads and ports nothing of Skia.
 *
 *     colr-skia FONT OUT.pam GLYPHS [--size S] [--cell C] [--cols N] [--axes TAG=V,TAG=V...]
 *
 * `--axes` is the instance in user coordinates (the font's own axis values). The glyph is drawn at the origin
 * (C / 8, C - C / 8) at an em of S pixels, as `colr-shape` places it.
 */

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char ** argv) {
  const char * font_path = nullptr;
  const char * out_path = nullptr;
  const char * glyph_path = nullptr;
  unsigned size = 100, cell = 128, cols = 64;
  std::vector<SkFontArguments::VariationPosition::Coordinate> axes;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
      size = (unsigned)std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--cell") && i + 1 < argc) {
      cell = (unsigned)std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--cols") && i + 1 < argc) {
      cols = (unsigned)std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--axes") && i + 1 < argc) {
      std::string spec = argv[++i];
      size_t at = 0;
      while (at < spec.size()) {
        size_t end = spec.find(',', at);
        std::string item = spec.substr(at, end == std::string::npos ? std::string::npos : end - at);
        size_t eq = item.find('=');
        if (eq == 4) {
          SkFourByteTag tag = SkSetFourByteTag(item[0], item[1], item[2], item[3]);
          axes.push_back({tag, (float)std::atof(item.c_str() + 5)});
        }
        at = end == std::string::npos ? spec.size() : end + 1;
      }
    } else if (!font_path) {
      font_path = argv[i];
    } else if (!out_path) {
      out_path = argv[i];
    } else if (!glyph_path) {
      glyph_path = argv[i];
    }
  }
  if (!font_path || !out_path || !glyph_path) {
    std::fprintf(stderr, "usage: colr-skia FONT OUT.pam GLYPHS [--size S] [--cell C] [--cols N] [--axes TAG=V,...]\n");
    return 2;
  }
  sk_sp<SkFontMgr> fm = SkFontMgr_New_Custom_Empty();
  sk_sp<SkTypeface> face = fm->makeFromFile(font_path, 0);
  if (!face) {
    std::fprintf(stderr, "colr-skia: cannot load %s\n", font_path);
    return 1;
  }
  if (!axes.empty()) {
    SkFontArguments args;
    args.setVariationDesignPosition({axes.data(), (int)axes.size()});
    sk_sp<SkTypeface> moved = face->makeClone(args);
    if (!moved) {
      std::fprintf(stderr, "colr-skia: cannot instantiate the instance\n");
      return 1;
    }
    face = moved;
  }
  SkFont font(face, SkScalar(size));
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setHinting(SkFontHinting::kNone);
  font.setSubpixel(true);
  font.setLinearMetrics(true);
  std::vector<unsigned> glyphs;
  {
    FILE * f = std::fopen(glyph_path, "r");
    unsigned g;
    while (f && std::fscanf(f, "%u", &g) == 1) {
      glyphs.push_back(g);
    }
    if (f) {
      std::fclose(f);
    }
  }
  const unsigned rows = glyphs.empty() ? 1 : (unsigned)((glyphs.size() + cols - 1) / cols);
  const size_t cell_row = (size_t)cell * 4;
  std::vector<uint8_t> cellpx((size_t)cell * cell * 4);
  SkImageInfo info = SkImageInfo::Make((int)cell, (int)cell, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  SkSurfaceProps props(0, kUnknown_SkPixelGeometry, 0.0f, 1.0f);
  sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, cellpx.data(), cell_row, &props);
  if (!surface) {
    std::fprintf(stderr, "colr-skia: no surface\n");
    return 1;
  }
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setAntiAlias(true);
  std::vector<uint8_t> atlas((size_t)cols * cell * rows * cell * 4, 0);
  const size_t atlas_row = (size_t)cols * cell * 4;
  for (size_t k = 0; k < glyphs.size(); k++) {
    std::memset(cellpx.data(), 0, cellpx.size());
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    SkGlyphID id = (SkGlyphID)glyphs[k];
    surface->getCanvas()->drawSimpleText(&id, sizeof id, SkTextEncoding::kGlyphID, SkScalar(cell / 8),
        SkScalar(cell - cell / 8), font, paint);
    unsigned cx = (unsigned)(k % cols) * cell;
    unsigned cy = (unsigned)(k / cols) * cell;
    for (unsigned y = 0; y < cell; y++) {
      std::memcpy(atlas.data() + (size_t)(cy + y) * atlas_row + (size_t)cx * 4, cellpx.data() + y * cell_row, cell_row);
    }
  }
  FILE * pam = std::fopen(out_path, "wb");
  if (!pam) {
    return 1;
  }
  std::fprintf(pam, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", cols * cell, rows * cell);
  std::fwrite(atlas.data(), 1, atlas.size(), pam);
  std::fclose(pam);
  std::fprintf(stderr, "colr-skia: %zu glyphs drawn\n", glyphs.size());
  return 0;
}
