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
 * The Blend2D driver: reads the scenes bench-shape wrote and renders each with
 * Blend2D's public C++ API, inside its pinned image only (AD-28).
 *
 *     bench-blend2d DIR OUT      one JSON line a scene on stdout, the last frame as OUT/blend2d.<scene>.pam
 *
 * Paths, glyph arrays and the font are built once, before timing, as
 * shape-cpu's resources are.  A frame is a synchronous context (thread_count
 * 0) begun on the caller's buffer, every item drawn, and end(), which returns
 * when the pixels are written.  Blend2D has no recorded list, so there is no
 * build step to time.  Its only premultiplied 32-bit format is BGRA in memory
 * (PRGB32 on a little-endian host); the PAM writer swaps it back to RGBA.
 *
 * Gradients (the gradients scene) are BLGradient objects built once, before
 * timing, like the paths, and drawn with the context's gradient quality hint
 * set to BL_GRADIENT_QUALITY_NEAREST, its undithered setting (the hint also
 * offers dithering, which no engine here does).  The scene's kinds map as:
 *
 *   linear  BLLinearGradientValues(x0, y0, x1, y1): the same two points.
 *   radial  BLRadialGradientValues(x0, y0, x1, y1, r0) is a circle (x0, y0, r0),
 *           offset 1, and a focal point (x1, y1), offset 0.  The scene's cone
 *           with a start circle of radius 0 inside its end circle is exactly
 *           that: the end circle is (x0, y0, r0), the start point (x1, y1).
 *           A start radius other than 0, or a start point on or outside the
 *           end circle, is refused here rather than approximated.
 *   sweep   BLConicGradientValues(x0, y0, angle, repeat) spans whole turns: the
 *           color line runs `repeat` times a turn from `angle`.  The scene's
 *           sweep from a0 to a1 is that when 360 / (a1 - a0) is a whole number
 *           n and either the extend is repeat (the angles below a0 then wrap as
 *           Blend2D's do) or n is 1, a0 is 0 and the extend is pad (no angle is
 *           outside the line).  Anything else is refused.  BL_CONIC_SIGN below
 *           is the direction Blend2D's angle runs on this y-down raster
 *           relative to the scene's (+x toward +y), as measured.
 */

#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>
#else
#include <blend2d.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  BLImage image;
  std::vector<BLPath> paths;                 // per item
  std::vector<BLGradient> gradients;         // per item, gradient fills
  std::vector<std::vector<uint32_t>> ids;    // per item, runs
  std::vector<std::vector<BLPoint>> places;  // per item, runs
  BLFontFace face;
  BLFont font;
};

// +1: Blend2D's conic angle grows from +x toward +y on a y-down raster, as the scene's does; -1: the other way.
constexpr double BL_CONIC_SIGN = 1.0;

int make_gradient(const SB_Scene & s, const SB_Gradient & g, BLGradient & out) {
  BLExtendMode mode = g.extend == SB_EXTEND_REPEAT ? BL_EXTEND_MODE_REPEAT
      : g.extend == SB_EXTEND_REFLECT              ? BL_EXTEND_MODE_REFLECT
                                                   : BL_EXTEND_MODE_PAD;
  bool reversed = false;
  switch (g.kind) {
  case SB_GRADIENT_LINEAR:
    out = BLGradient(BLLinearGradientValues(g.g[0], g.g[1], g.g[2], g.g[3]), mode);
    break;
  case SB_GRADIENT_RADIAL: {
    double dx = g.g[0] - g.g[3], dy = g.g[1] - g.g[4];
    if (g.g[2] != 0.0 || !(g.g[5] > 0.0) || !(std::sqrt(dx * dx + dy * dy) < g.g[5])) {
      std::fprintf(stderr, "bench-blend2d: a radial gradient Blend2D cannot draw as the scene defines it\n");
      return -1;
    }
    out = BLGradient(BLRadialGradientValues(g.g[3], g.g[4], g.g[0], g.g[1], g.g[5]), mode);
    break;
  }
  case SB_GRADIENT_SWEEP: {
    double span = g.g[3] - g.g[2], n = span > 0.0 ? 360.0 / span : 0.0;
    if (!(n >= 1.0) || n != std::floor(n) ||
        !(g.extend == SB_EXTEND_REPEAT || (n == 1.0 && g.g[2] == 0.0 && g.extend == SB_EXTEND_PAD))) {
      std::fprintf(stderr, "bench-blend2d: a sweep gradient Blend2D's conic gradient cannot draw\n");
      return -1;
    }
    out = BLGradient(BLConicGradientValues(g.g[0], g.g[1], BL_CONIC_SIGN * g.g[2] * (3.14159265358979323846 / 180.0), n),
        mode);
    reversed = BL_CONIC_SIGN < 0.0;
    break;
  }
  }
  for (size_t k = 0; k < g.n_stops; k++) {
    // Reversed: Blend2D's offset u at the scene's angle is 1 - t, so the stop at t goes to 1 - t, in reverse order.
    const SB_Stop & st = s.stops[g.first_stop + (reversed ? g.n_stops - 1 - k : k)];
    if (out.add_stop(reversed ? 1.0 - st.offset : st.offset, BLRgba32(st.color.r, st.color.g, st.color.b, st.color.a)) !=
        BL_SUCCESS) {
      return -1;
    }
  }
  return 0;
}

int setup(Engine & e, const SB_Scene & s, const std::string & dir) {
  e.scene = &s;
  e.stride = size_t(s.width) * 4;
  e.pixels.assign(e.stride * size_t(s.height), 0);
  sb_background_bytes(&s, 1, e.bg);
  if (e.image.create_from_data(s.width, s.height, BL_FORMAT_PRGB32, e.pixels.data(), intptr_t(e.stride)) !=
      BL_SUCCESS) {
    std::fprintf(stderr, "bench-blend2d: create_from_data failed\n");
    return -1;
  }
  if (s.font[0]) {
    std::string path = dir + "/" + s.font;
    if (e.face.create_from_file(path.c_str()) != BL_SUCCESS ||
        e.font.create_from_face(e.face, float(s.font_size)) != BL_SUCCESS) {
      std::fprintf(stderr, "bench-blend2d: cannot load %s\n", path.c_str());
      return -1;
    }
  }
  e.paths.resize(s.n_items);
  e.gradients.resize(s.n_items);
  e.ids.resize(s.n_items);
  e.places.resize(s.n_items);
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    if (it.kind == SB_FILL || it.kind == SB_STROKE) {
      BLPath & p = e.paths[i];
      for (size_t k = 0; k < it.count; k++) {
        const SB_Cmd & c = s.cmds[it.first + k];
        switch (c.op) {
        case 'M': p.move_to(c.p[0], c.p[1]); break;
        case 'L': p.line_to(c.p[0], c.p[1]); break;
        case 'Q': p.quad_to(c.p[0], c.p[1], c.p[2], c.p[3]); break;
        case 'C': p.cubic_to(c.p[0], c.p[1], c.p[2], c.p[3], c.p[4], c.p[5]); break;
        default: p.close(); break;
        }
      }
      if (it.gradient && make_gradient(s, s.gradients[it.gradient - 1], e.gradients[i])) {
        return -1;
      }
    } else if (it.kind == SB_RUN) {
      for (size_t k = 0; k < it.count; k++) {
        const SB_Glyph & g = s.glyphs[it.first + k];
        e.ids[i].push_back(g.id);
        e.places[i].push_back(BLPoint(g.x, g.y));
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

int submit(void * ctx) {
  Engine & e = *static_cast<Engine *>(ctx);
  const SB_Scene & s = *e.scene;
  BLContextCreateInfo info{};
  info.thread_count = 0;
  BLContext c;
  if (c.begin(e.image, info) != BL_SUCCESS) {
    return 1;
  }
  c.set_comp_op(BL_COMP_OP_SRC_OVER);
  c.set_fill_rule(BL_FILL_RULE_NON_ZERO);
  c.set_hint(BL_CONTEXT_HINT_GRADIENT_QUALITY, BL_GRADIENT_QUALITY_NEAREST);
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    BLRgba32 color(it.color.r, it.color.g, it.color.b, it.color.a);
    switch (it.kind) {
    case SB_FILL:
      if (it.gradient) {
        c.set_fill_alpha(it.color.a / 255.0);
        c.fill_path(e.paths[i], e.gradients[i]);
        c.set_fill_alpha(1.0);
      } else {
        c.fill_path(e.paths[i], color);
      }
      break;
    case SB_STROKE: {
      // Blend2D's stroker, through the context's stroke options: a miter join past the limit is a bevel,
      // as in SVG (BL_STROKE_JOIN_MITER_BEVEL).
      const SB_Stroke & st = s.strokes[it.stroke - 1];
      c.set_stroke_width(st.width);
      c.set_stroke_join(st.join == SB_JOIN_ROUND ? BL_STROKE_JOIN_ROUND
                            : st.join == SB_JOIN_BEVEL ? BL_STROKE_JOIN_BEVEL
                                                       : BL_STROKE_JOIN_MITER_BEVEL);
      c.set_stroke_caps(st.cap == SB_CAP_ROUND ? BL_STROKE_CAP_ROUND
                            : st.cap == SB_CAP_SQUARE ? BL_STROKE_CAP_SQUARE
                                                      : BL_STROKE_CAP_BUTT);
      c.set_stroke_miter_limit(st.miter);
      // The scene holds no dash here: this Blend2D accepts a dash array on the context and strokes solid (the benchmark's
      // `strokes-dashed` scene is not drawn by this driver), so a dashed stroke would be a different picture.
      if (st.n_dashes || c.stroke_path(e.paths[i], color) != BL_SUCCESS) {
        std::fprintf(stderr, "bench-blend2d: stroke refused or dashed for item %zu\n", i);
        return 2;
      }
      break;
    }
    case SB_CLIP:
      c.save();
      c.clip_to_rect(BLRect(it.rect[0], it.rect[1], it.rect[2] - it.rect[0], it.rect[3] - it.rect[1]));
      break;
    case SB_UNCLIP:
      c.restore();
      break;
    case SB_RUN: {
      BLGlyphRun run{};
      run.set_glyph_data(e.ids[i].data());
      run.set_placement_data(e.places[i].data());
      run.size = e.ids[i].size();
      run.placement_type = BL_GLYPH_PLACEMENT_TYPE_USER_UNITS;
      c.fill_glyph_run(BLPoint(0, 0), e.font, run, color);
      break;
    }
    }
  }
  return c.end() == BL_SUCCESS ? 0 : 2;
}

}  // namespace

int main(int argc, char ** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: bench-blend2d DIR OUT\n");
    return 2;
  }
  static const char * const scenes[] = {"fill-rect", "fill-circle", "paths", "text", "ui", "gradients", "strokes"};
  int failures = 0;
  for (const char * name : scenes) {
    std::string path = std::string(argv[1]) + "/" + name + ".scene";
    char err[256];
    SB_Scene s;
    if (sb_scene_load(path.c_str(), &s, err, sizeof err)) {
      std::fprintf(stderr, "bench-blend2d: %s: %s\n", path.c_str(), err);
      return 1;
    }
    {
      Engine e;
      SB_Measure m;
      SB_Engine eng = {&e, clear, nullptr, submit};
      int rc = setup(e, s, argv[1]);
      if (rc == 0) {
        rc = sb_measure(&eng, &m);
      }
      std::string out = std::string("blend2d.") + s.name + ".pam";
      if (rc == 0) {
        rc = sb_write_pam((std::string(argv[2]) + "/" + out).c_str(), s.width, s.height, e.pixels.data(), e.stride, 1);
      }
      if (rc == 0) {
        sb_report(stdout, "blend2d", &s, &m, out.c_str(),
            std::strcmp(s.metric, "list") == 0 ? "immediate mode: no recorded list, the build step does not exist"
                : std::strncmp(s.name, "strokes", 7) == 0 ? "stroke options on the context (width, join, caps, miter limit), Blend2D's own stroker, paths built once; no dashes (see README)"
                : s.n_gradients ? "BLGradient objects built once; gradient quality hint BL_GRADIENT_QUALITY_NEAREST (undithered)"
                                : nullptr);
      } else {
        std::fprintf(stderr, "bench-blend2d: %s failed (%d)\n", s.name, rc);
        failures++;
      }
    }
    sb_scene_free(&s);
  }
  return failures ? 1 : 0;
}
