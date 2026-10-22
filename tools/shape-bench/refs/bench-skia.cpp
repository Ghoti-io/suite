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
 *
 * Gradients (the gradients scene) are SkShaders built once, before timing,
 * like the paths, through m156's SkShaders gradient factories and an
 * SkGradient (include/effects/SkGradient.h), on paints whose dithering is
 * turned off explicitly.  Each stop is the 8-bit sRGB color as an SkColor4f
 * (SkColor4f::FromColor) at its offset; the interpolation is
 * Interpolation{InPremul::kNo, ColorSpace::kDestination}, the destination
 * being a surface with no color space, so no color is converted and the stops
 * are interpolated in the target's encoded values, unpremultiplied (for
 * opaque stops the same as premultiplied).  The scene's kinds map as:
 *
 *   linear  SkShaders::LinearGradient with the same two points.
 *   radial  SkShaders::TwoPointConicalGradient(start, r0, end, r1): the
 *           scene's two-circle cone (offset 0 on the start circle, 1 on the
 *           end circle, the largest offset whose radius is positive), which is
 *           the HTML canvas's radial gradient, as Skia documents it.
 *   sweep   SkShaders::SweepGradient(center, startAngle a0, endAngle a1):
 *           degrees from the +x axis, clockwise on this y-down canvas, offset 0
 *           at a0 and 1 at a1, the angles sampled in [0, 360) and the tile mode
 *           applied outside [a0, a1], as the scene's.
 *
 * pad, repeat and reflect are SkTileMode kClamp, kRepeat and kMirror.
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
#include "include/effects/SkDashPathEffect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkGradient.h"
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
    case SB_STROKE:
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

sk_sp<SkShader> make_gradient(const SB_Scene & s, const SB_Gradient & g) {
  SkColor4f colors[SB_GRADIENT_STOPS_MAX];
  float pos[SB_GRADIENT_STOPS_MAX];
  size_t n = g.n_stops;
  for (size_t k = 0; k < n; k++) {
    const SB_Stop & st = s.stops[g.first_stop + k];
    colors[k] = SkColor4f::FromColor(SkColorSetARGB(st.color.a, st.color.r, st.color.g, st.color.b));
    pos[k] = float(st.offset);
  }
  SkTileMode mode = g.extend == SB_EXTEND_REPEAT ? SkTileMode::kRepeat
      : g.extend == SB_EXTEND_REFLECT            ? SkTileMode::kMirror
                                                 : SkTileMode::kClamp;
  SkGradient::Interpolation interp;
  interp.fInPremul = SkGradient::Interpolation::InPremul::kNo;
  interp.fColorSpace = SkGradient::Interpolation::ColorSpace::kDestination;
  SkGradient grad(SkGradient::Colors(SkSpan<const SkColor4f>(colors, n), SkSpan<const float>(pos, n), mode), interp);
  auto v = [&](int j) { return SkScalar(g.g[j]); };
  switch (g.kind) {
  case SB_GRADIENT_LINEAR: {
    SkPoint pts[2] = {SkPoint::Make(v(0), v(1)), SkPoint::Make(v(2), v(3))};
    return SkShaders::LinearGradient(pts, grad, nullptr);
  }
  case SB_GRADIENT_RADIAL:
    return SkShaders::TwoPointConicalGradient(SkPoint::Make(v(0), v(1)), v(2), SkPoint::Make(v(3), v(4)), v(5), grad,
        nullptr);
  case SB_GRADIENT_SWEEP:
    return SkShaders::SweepGradient(SkPoint::Make(v(0), v(1)), v(2), v(3), grad, nullptr);
  }
  return nullptr;
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
    p.setDither(false);
    if (it.kind == SB_FILL && it.gradient) {
      // The color's alpha is the gradient's multiplier; its r, g, b are 0 and unused under a shader.
      p.setShader(make_gradient(s, s.gradients[it.gradient - 1]));
      if (!p.getShader()) {
        std::fprintf(stderr, "bench-skia: cannot make the gradient of item %zu\n", i);
        return -1;
      }
    }
    if (it.kind == SB_STROKE) {
      // The scene's stroke, in Skia's words: a stroking paint (width, join, cap, miter limit) and, with dashes, an
      // SkDashPathEffect from the phase; Skia's own stroker and its own dasher, restarting at each contour.
      const SB_Stroke & st = s.strokes[it.stroke - 1];
      p.setStyle(SkPaint::kStroke_Style);
      p.setStrokeWidth(SkScalar(st.width));
      p.setStrokeJoin(st.join == SB_JOIN_ROUND ? SkPaint::kRound_Join
                          : st.join == SB_JOIN_BEVEL ? SkPaint::kBevel_Join
                                                     : SkPaint::kMiter_Join);
      p.setStrokeCap(st.cap == SB_CAP_ROUND ? SkPaint::kRound_Cap
                         : st.cap == SB_CAP_SQUARE ? SkPaint::kSquare_Cap
                                                   : SkPaint::kButt_Cap);
      p.setStrokeMiter(SkScalar(st.miter));
      if (st.n_dashes) {
        SkScalar iv[SB_DASHES_MAX];
        for (size_t d = 0; d < st.n_dashes; d++) {
          iv[d] = SkScalar(st.dashes[d]);
        }
        p.setPathEffect(SkDashPathEffect::Make(SkSpan<const SkScalar>(iv, st.n_dashes), SkScalar(st.offset)));
      }
    }
    if (it.kind == SB_FILL || it.kind == SB_STROKE) {
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
  static const char * const scenes[] = {"fill-rect", "fill-circle", "paths", "text", "ui", "gradients", "strokes", "strokes-dashed"};
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
                : std::strncmp(s.name, "strokes", 7) == 0 ? "SkPaint stroke style (width, join, cap, miter limit) and SkDashPathEffect, Skia's own stroker and dasher, paths built once"
                : s.n_gradients ? "SkShaders gradient shaders built once; paint dither off; 8-bit sRGB stops, destination interpolation on a surface with no color space, unpremultiplied"
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
