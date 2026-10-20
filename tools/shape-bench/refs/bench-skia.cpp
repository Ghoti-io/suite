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
 * The Skia driver: reads the scenes bench-shape wrote and renders each with
 * Skia's CPU raster backend through its public C++ API, inside its pinned
 * image only (AD-28).
 *
 *     bench-skia DIR OUT      one JSON line a scene on stdout, the last frame as OUT/skia.<scene>.pam
 *
 * Paths, text blobs and the typeface are built once, before timing, as
 * shape-cpu's resources are.  The surface wraps the caller's buffer, RGBA8
 * premultiplied.  For the list scene a frame records an SkPicture (the build)
 * and plays it into the surface (the submit), which is Skia's counterpart of
 * recording a list and rendering it; every other scene draws directly, as an
 * immediate-mode caller would.  Text is SkFont unhinted, anti-aliased (not
 * LCD), subpixel-positioned, with linear metrics, on a surface whose text
 * contrast is 0 and gamma 1, so that glyph coverage is linear alpha as in every
 * other engine here. Skia keeps rendered glyph masks in its strike cache, which
 * is its design and is left as it is.
 */

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../scene.h"

namespace {

struct Engine {
  const SB_Scene * scene = nullptr;
  std::vector<uint8_t> pixels;
  size_t stride = 0;
  uint8_t bg[4] = {0, 0, 0, 0};
  sk_sp<SkSurface> surface;
  std::vector<SkPath> paths;              // per item
  std::vector<SkPaint> paints;            // per item
  std::vector<sk_sp<SkTextBlob>> blobs;   // per item, runs
  sk_sp<SkFontMgr> fonts;
  sk_sp<SkTypeface> typeface;
  bool record = false;
  sk_sp<SkPicture> picture;
};

void draw(Engine & e, SkCanvas * c) {
  const SB_Scene & s = *e.scene;
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    switch (it.kind) {
    case SB_FILL:
      c->drawPath(e.paths[i], e.paints[i]);
      break;
    case SB_CLIP:
      c->save();
      c->clipRect(SkRect::MakeLTRB(SkScalar(it.rect[0]), SkScalar(it.rect[1]), SkScalar(it.rect[2]),
                      SkScalar(it.rect[3])),
          SkClipOp::kIntersect, true);
      break;
    case SB_UNCLIP:
      c->restore();
      break;
    case SB_RUN:
      c->drawTextBlob(e.blobs[i], 0, 0, e.paints[i]);
      break;
    }
  }
}

int setup(Engine & e, const SB_Scene & s, const std::string & dir) {
  e.scene = &s;
  e.stride = size_t(s.width) * 4;
  e.pixels.assign(e.stride * size_t(s.height), 0);
  sb_background_bytes(&s, 0, e.bg);
  e.record = std::strcmp(s.metric, "list") == 0;
  SkImageInfo info = SkImageInfo::Make(s.width, s.height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  // Text contrast 0 and gamma 1: glyph coverage composited as linear alpha, as every other engine here does. Skia's
  // defaults adjust A8 glyph masks by the text's luminance (about a sixth less ink for dark text on light), which is
  // a different picture rather than a different speed: the adjustment is made once per cached mask.
  SkSurfaceProps props(0, kUnknown_SkPixelGeometry, 0.0f, 1.0f);
  e.surface = SkSurfaces::WrapPixels(info, e.pixels.data(), e.stride, &props);
  if (!e.surface) {
    std::fprintf(stderr, "bench-skia: WrapPixels failed\n");
    return -1;
  }
  SkFont font;
  if (s.font[0]) {
    std::string path = dir + "/" + s.font;
    e.fonts = SkFontMgr_New_Custom_Empty();
    e.typeface = e.fonts ? e.fonts->makeFromFile(path.c_str(), 0) : nullptr;
    if (!e.typeface) {
      std::fprintf(stderr, "bench-skia: cannot load %s\n", path.c_str());
      return -1;
    }
    font = SkFont(e.typeface, SkScalar(s.font_size));
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setHinting(SkFontHinting::kNone);
    font.setSubpixel(true);
    font.setLinearMetrics(true);
  }
  e.paths.resize(s.n_items);
  e.paints.resize(s.n_items);
  e.blobs.resize(s.n_items);
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    SkPaint & p = e.paints[i];
    p.setAntiAlias(true);
    p.setColor(SkColorSetARGB(it.color.a, it.color.r, it.color.g, it.color.b));
    p.setBlendMode(SkBlendMode::kSrcOver);
    if (it.kind == SB_FILL) {
      SkPathBuilder b(SkPathFillType::kWinding);
      for (size_t k = 0; k < it.count; k++) {
        const SB_Cmd & c = s.cmds[it.first + k];
        auto x = [&](int j) { return SkScalar(c.p[j]); };
        switch (c.op) {
        case 'M': b.moveTo(x(0), x(1)); break;
        case 'L': b.lineTo(x(0), x(1)); break;
        case 'Q': b.quadTo(x(0), x(1), x(2), x(3)); break;
        case 'C': b.cubicTo(x(0), x(1), x(2), x(3), x(4), x(5)); break;
        default: b.close(); break;
        }
      }
      e.paths[i] = b.detach();
    } else if (it.kind == SB_RUN) {
      SkTextBlobBuilder tb;
      const SkTextBlobBuilder::RunBuffer & run = tb.allocRunPos(font, int(it.count));
      for (size_t k = 0; k < it.count; k++) {
        const SB_Glyph & g = s.glyphs[it.first + k];
        run.glyphs[k] = SkGlyphID(g.id);
        run.pos[2 * k] = SkScalar(g.x);
        run.pos[2 * k + 1] = SkScalar(g.y);
      }
      e.blobs[i] = tb.make();
      if (!e.blobs[i]) {
        return -1;
      }
    }
  }
  return 0;
}

int clear(void * ctx) {
  Engine & e = *static_cast<Engine *>(ctx);
  sb_clear(e.pixels.data(), e.scene->width, e.scene->height, e.stride, e.bg);
  return 0;
}

int build(void * ctx) {
  Engine & e = *static_cast<Engine *>(ctx);
  SkPictureRecorder recorder;
  SkCanvas * c = recorder.beginRecording(SkRect::MakeWH(SkScalar(e.scene->width), SkScalar(e.scene->height)));
  draw(e, c);
  e.picture = recorder.finishRecordingAsPicture();
  return e.picture ? 0 : 1;
}

int submit(void * ctx) {
  Engine & e = *static_cast<Engine *>(ctx);
  SkCanvas * c = e.surface->getCanvas();
  if (e.record) {
    c->drawPicture(e.picture);
  } else {
    draw(e, c);
  }
  return 0;
}

}  // namespace

int main(int argc, char ** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: bench-skia DIR OUT\n");
    return 2;
  }
  static const char * const scenes[] = {"fill-rect", "fill-circle", "paths", "text", "ui"};
  int failures = 0;
  for (const char * name : scenes) {
    std::string path = std::string(argv[1]) + "/" + name + ".scene";
    char err[256];
    SB_Scene s;
    if (sb_scene_load(path.c_str(), &s, err, sizeof err)) {
      std::fprintf(stderr, "bench-skia: %s: %s\n", path.c_str(), err);
      return 1;
    }
    {
      Engine e;
      SB_Measure m;
      int rc = setup(e, s, argv[1]);
      SB_Engine eng = {&e, clear, e.record ? build : nullptr, submit};
      if (rc == 0) {
        rc = sb_measure(&eng, &m);
      }
      std::string out = std::string("skia.") + s.name + ".pam";
      if (rc == 0) {
        rc = sb_write_pam((std::string(argv[2]) + "/" + out).c_str(), s.width, s.height, e.pixels.data(), e.stride, 0);
      }
      if (rc == 0) {
        sb_report(stdout, "skia", &s, &m, out.c_str(),
            e.record                              ? "build: SkPictureRecorder; submit: drawPicture"
                : std::strcmp(s.metric, "text") == 0 ? "SkTextBlob built once; glyph masks cached by Skia's strike cache; text contrast 0 and gamma 1 (linear coverage)"
                                                     : nullptr);
      } else {
        std::fprintf(stderr, "bench-skia: %s failed (%d)\n", s.name, rc);
        failures++;
      }
    }
    sb_scene_free(&s);
  }
  return failures ? 1 : 0;
}
