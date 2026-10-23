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
 *
 * The pictures (README.md, "The pictures") add, by Blend2D's public API only:
 *
 *   radial     with a start circle of any radius: BLRadialGradientValues(x, y of the end circle, x, y of the start circle,
 *              the end radius, the start radius).
 *   sweep      over any span, extend and direction of angles: the scene's color line is sampled into stops over one turn
 *              of Blend2D's conic gradient (its offset 0 at the start angle, 1 a turn on), with the seam of the scene's
 *              angles, the folds of repeat and reflect, and a stop twice at every jump (sweep_stops).  The whole-turn
 *              cases above keep their direct mapping.
 *   group      Blend2D has no layers in its context: a group is a BLImage of the extent, cleared, drawn into by a context
 *              of its own, and blitted onto the enclosing context with the group's comp-op and its opacity as the global
 *              alpha; isolated, in the encoded values.  A `linear` group is drawn the same way, in encoded values, and
 *              Blend2D has no hue, saturation, color or luminosity operators: those groups are drawn source-over.  Both are
 *              differences of the picture, reported with the scene's mismatches.
 *   clippath   Blend2D clips to rectangles only: the clipped content is drawn into a layer as a group is, the path is
 *              filled opaque into a second layer, and the first is multiplied by the second's coverage (BL_COMP_OP_DST_IN)
 *              before it is blitted source-over.
 *   joins      SVG 2's miter-clip is BL_STROKE_JOIN_MITER_CLIP; arcs has no Blend2D join and is drawn BL_STROKE_JOIN_MITER_BEVEL.
 *   dashes     this Blend2D's context accepts a dash array and strokes solid (see the strokes-dashed scene), so a dashed
 *              stroke is made by BLPath::add_stroked_path with the dash array in BLStrokeOptions, in the frame, and filled.
 */

#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>
#else
#include <blend2d.h>
#endif

#include <algorithm>
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
  struct Layer {
    std::vector<uint8_t> px;
    BLImage img;
    BLContext ctx;
  };
  std::vector<Layer *> layers;  // one per nesting level of groups and path clips, then the clip mask
  size_t max_depth = 0;
};

// +1: Blend2D's conic angle grows from +x toward +y on a y-down raster, as the scene's does; -1: the other way.
constexpr double BL_CONIC_SIGN = 1.0;

// The scene's color line at t, straight, 8-bit values as doubles, under the gradient's extend mode.
void line_color(const SB_Scene & s, const SB_Gradient & g, double t, double c[4]) {
  if (g.extend == SB_EXTEND_REPEAT) {
    t -= std::floor(t);
  } else if (g.extend == SB_EXTEND_REFLECT) {
    t = std::fabs(t - 2.0 * std::floor(t / 2.0));
    t = t > 1.0 ? 2.0 - t : t;
  }
  t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
  size_t i = 0;
  while (i + 2 < g.n_stops && s.stops[g.first_stop + i + 1].offset <= t) {
    i++;
  }
  const SB_Stop & a = s.stops[g.first_stop + i], & b = s.stops[g.first_stop + i + 1];
  double d = b.offset - a.offset, u = d > 0.0 ? (t - a.offset) / d : 1.0;
  u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
  c[0] = a.color.r + (b.color.r - a.color.r) * u;
  c[1] = a.color.g + (b.color.g - a.color.g) * u;
  c[2] = a.color.b + (b.color.b - a.color.b) * u;
  c[3] = a.color.a + (b.color.a - a.color.a) * u;
}

// A sweep of any span: Blend2D's conic gradient runs one turn from its start angle (a0) with u = (theta - a0) / 360 wrapped.
// The scene's t = (theta - a0) / (a1 - a0) for theta in [0, 360) (no wrap), then the extend mode.  Both are piecewise linear in
// theta, so the stops at every breakpoint of either, with the colors on either side of a jump, reproduce the scene's line.
int make_sweep(const SB_Scene & s, const SB_Gradient & g, BLGradient & out) {
  const double a0 = g.g[2], span = g.g[3] - g.g[2];
  if (!(span != 0.0)) {
    std::fprintf(stderr, "bench-blend2d: a sweep gradient with no span\n");
    return -1;
  }
  const double eps = 1e-7;
  std::vector<double> th = {0.0, 360.0, a0 - 360.0 * std::floor(a0 / 360.0)};
  double t_lo = std::fmin(-a0 / span, (360.0 - a0) / span), t_hi = std::fmax(-a0 / span, (360.0 - a0) / span);
  for (double m = std::floor(t_lo) - 1; m <= std::ceil(t_hi) + 1; m += 1.0) {
    for (size_t k = 0; k < g.n_stops; k++) {
      double o = s.stops[g.first_stop + k].offset;
      for (double t : {m + o, m - o, m}) {
        double a = a0 + t * span;
        if (a > 0.0 && a < 360.0) {
          th.push_back(a);
        }
      }
    }
  }
  std::sort(th.begin(), th.end());
  th.erase(std::unique(th.begin(), th.end(), [](double x, double y) { return y - x < 1e-9; }), th.end());
  struct Stop {
    double u;
    int order;
    BLRgba32 c;
  };
  std::vector<Stop> stops;
  double seam = th.empty() ? 0.0 : a0 - 360.0 * std::floor(a0 / 360.0);
  for (size_t k = 0; k + 1 < th.size(); k++) {
    double c0[4], c1[4];
    line_color(s, g, (th[k] + eps - a0) / span, c0);
    line_color(s, g, (th[k + 1] - eps - a0) / span, c1);
    double u0 = (th[k] - seam) / 360.0;
    if (u0 < -1e-12) {
      u0 += 1.0;
    }
    double u1 = u0 + (th[k + 1] - th[k]) / 360.0;
    auto q = [](double v) { return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); };
    stops.push_back({u0, 1, BLRgba32(q(c0[0]), q(c0[1]), q(c0[2]), q(c0[3]))});
    stops.push_back({u1, 0, BLRgba32(q(c1[0]), q(c1[1]), q(c1[2]), q(c1[3]))});
  }
  std::stable_sort(stops.begin(), stops.end(), [](const Stop & x, const Stop & y) {
    return x.u < y.u - 1e-12 || (std::fabs(x.u - y.u) <= 1e-12 && x.order < y.order);
  });
  out = BLGradient(BLConicGradientValues(g.g[0], g.g[1], BL_CONIC_SIGN * a0 * (3.14159265358979323846 / 180.0), 1.0),
      BL_EXTEND_MODE_PAD);
  for (const Stop & st : stops) {
    double u = st.u < 0.0 ? 0.0 : st.u > 1.0 ? 1.0 : st.u;
    if (out.add_stop(BL_CONIC_SIGN > 0 ? u : 1.0 - u, st.c) != BL_SUCCESS) {
      return -1;
    }
  }
  return 0;
}

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
    // The scene's circle 0 is Blend2D's start circle (the focal point and r1), circle 1 its end circle (x0, y0, r0).
    out = BLGradient(BLRadialGradientValues(g.g[3], g.g[4], g.g[0], g.g[1], g.g[5], g.g[2]), mode);
    break;
  }
  case SB_GRADIENT_SWEEP: {
    double span = g.g[3] - g.g[2], n = span > 0.0 ? 360.0 / span : 0.0;
    if (!(n >= 1.0) || n != std::floor(n) ||
        !(g.extend == SB_EXTEND_REPEAT || (n == 1.0 && g.g[2] == 0.0 && g.extend == SB_EXTEND_PAD))) {
      return make_sweep(s, g, out);  // the stops are its own
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
  // Layers for the groups and path clips: one a nesting level, and a spare for a clip's coverage.
  e.max_depth = 0;
  {
    std::vector<char> st;
    for (size_t i = 0; i < s.n_items; i++) {
      const SB_Kind k = s.items[i].kind;
      if (k == SB_CLIP || k == SB_CLIPPATH || k == SB_GROUP) {
        st.push_back(k == SB_CLIP ? 'r' : 'l');
      } else if (k == SB_UNCLIP || k == SB_ENDGROUP) {
        st.pop_back();
      }
      size_t layers = size_t(std::count(st.begin(), st.end(), 'l'));
      e.max_depth = std::max(e.max_depth, layers);
    }
  }
  if (e.max_depth) {
    for (size_t k = 0; k <= e.max_depth; k++) {
      auto * L = new Engine::Layer();
      L->px.assign(e.stride * size_t(s.height), 0);
      if (L->img.create_from_data(s.width, s.height, BL_FORMAT_PRGB32, L->px.data(), intptr_t(e.stride)) != BL_SUCCESS) {
        std::fprintf(stderr, "bench-blend2d: layer create_from_data failed\n");
        return -1;
      }
      e.layers.push_back(L);
    }
  }
  e.paths.resize(s.n_items);
  e.gradients.resize(s.n_items);
  e.ids.resize(s.n_items);
  e.places.resize(s.n_items);
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    if (it.kind == SB_FILL || it.kind == SB_STROKE || it.kind == SB_CLIPPATH) {
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

BLCompOp comp_op(int op) {
  switch (op) {
  case SB_COMPOSITE_CLEAR: return BL_COMP_OP_CLEAR;
  case SB_COMPOSITE_SRC: return BL_COMP_OP_SRC_COPY;
  case SB_COMPOSITE_DEST: return BL_COMP_OP_DST_COPY;
  case SB_COMPOSITE_DEST_OVER: return BL_COMP_OP_DST_OVER;
  case SB_COMPOSITE_SRC_IN: return BL_COMP_OP_SRC_IN;
  case SB_COMPOSITE_DEST_IN: return BL_COMP_OP_DST_IN;
  case SB_COMPOSITE_SRC_OUT: return BL_COMP_OP_SRC_OUT;
  case SB_COMPOSITE_DEST_OUT: return BL_COMP_OP_DST_OUT;
  case SB_COMPOSITE_SRC_ATOP: return BL_COMP_OP_SRC_ATOP;
  case SB_COMPOSITE_DEST_ATOP: return BL_COMP_OP_DST_ATOP;
  case SB_COMPOSITE_XOR: return BL_COMP_OP_XOR;
  case SB_COMPOSITE_PLUS: return BL_COMP_OP_PLUS;
  case SB_COMPOSITE_SCREEN: return BL_COMP_OP_SCREEN;
  case SB_COMPOSITE_OVERLAY: return BL_COMP_OP_OVERLAY;
  case SB_COMPOSITE_DARKEN: return BL_COMP_OP_DARKEN;
  case SB_COMPOSITE_LIGHTEN: return BL_COMP_OP_LIGHTEN;
  case SB_COMPOSITE_COLOR_DODGE: return BL_COMP_OP_COLOR_DODGE;
  case SB_COMPOSITE_COLOR_BURN: return BL_COMP_OP_COLOR_BURN;
  case SB_COMPOSITE_HARD_LIGHT: return BL_COMP_OP_HARD_LIGHT;
  case SB_COMPOSITE_SOFT_LIGHT: return BL_COMP_OP_SOFT_LIGHT;
  case SB_COMPOSITE_DIFFERENCE: return BL_COMP_OP_DIFFERENCE;
  case SB_COMPOSITE_EXCLUSION: return BL_COMP_OP_EXCLUSION;
  case SB_COMPOSITE_MULTIPLY: return BL_COMP_OP_MULTIPLY;
  default: return BL_COMP_OP_SRC_OVER;  // including hue, saturation, color and luminosity, which Blend2D does not have
  }
}

int begin_ctx(BLContext & c, BLImage & img) {
  BLContextCreateInfo info{};
  info.thread_count = 0;
  if (c.begin(img, info) != BL_SUCCESS) {
    return 1;
  }
  c.set_comp_op(BL_COMP_OP_SRC_OVER);
  c.set_fill_rule(BL_FILL_RULE_NON_ZERO);
  c.set_hint(BL_CONTEXT_HINT_GRADIENT_QUALITY, BL_GRADIENT_QUALITY_NEAREST);
  return 0;
}

int draw_item(Engine & e, BLContext & c, size_t i) {
  const SB_Scene & s = *e.scene;
  const SB_Item & it = s.items[i];
  BLRgba32 color(it.color.r, it.color.g, it.color.b, it.color.a);
  switch (it.kind) {
  case SB_FILL:
    if (it.evenodd) {
      c.set_fill_rule(BL_FILL_RULE_EVEN_ODD);
    }
    if (it.gradient) {
      c.set_fill_alpha(it.color.a / 255.0);
      c.fill_path(e.paths[i], e.gradients[i]);
      c.set_fill_alpha(1.0);
    } else {
      c.fill_path(e.paths[i], color);
    }
    if (it.evenodd) {
      c.set_fill_rule(BL_FILL_RULE_NON_ZERO);
    }
    break;
  case SB_STROKE: {
    // Blend2D's stroker, through the context's stroke options: a miter join past the limit is a bevel, as in SVG
    // (BL_STROKE_JOIN_MITER_BEVEL); miter-clip is its own join; arcs has none and is the miter-bevel.
    const SB_Stroke & st = s.strokes[it.stroke - 1];
    BLStrokeJoin join = st.join == SB_JOIN_ROUND ? BL_STROKE_JOIN_ROUND
        : st.join == SB_JOIN_BEVEL               ? BL_STROKE_JOIN_BEVEL
        : st.join == SB_JOIN_MITER_CLIP          ? BL_STROKE_JOIN_MITER_CLIP
                                                 : BL_STROKE_JOIN_MITER_BEVEL;
    BLStrokeCap cap = st.cap == SB_CAP_ROUND ? BL_STROKE_CAP_ROUND
        : st.cap == SB_CAP_SQUARE            ? BL_STROKE_CAP_SQUARE
                                             : BL_STROKE_CAP_BUTT;
    if (it.gradient) {
      c.set_stroke_alpha(it.color.a / 255.0);
    }
    if (st.n_dashes) {
      // Context dashes are accepted and not applied by this Blend2D: stroke the path with the dash array through the
      // path API and fill the result.
      BLStrokeOptions opt;
      opt.width = st.width;
      opt.miter_limit = st.miter;
      opt.join = join;
      opt.start_cap = opt.end_cap = cap;
      opt.dash_offset = st.offset;
      for (size_t d = 0; d < st.n_dashes; d++) {
        opt.dash_array.append(st.dashes[d]);
      }
      BLPath out;
      if (out.add_stroked_path(e.paths[i], opt, bl_default_approximation_options) != BL_SUCCESS) {
        std::fprintf(stderr, "bench-blend2d: dashed stroke refused for item %zu\n", i);
        return 2;
      }
      if (it.gradient) {
        c.set_fill_alpha(it.color.a / 255.0);
        c.fill_path(out, e.gradients[i]);
        c.set_fill_alpha(1.0);
      } else {
        c.fill_path(out, color);
      }
    } else {
      c.set_stroke_width(st.width);
      c.set_stroke_join(join);
      c.set_stroke_caps(cap);
      c.set_stroke_miter_limit(st.miter);
      if ((it.gradient ? c.stroke_path(e.paths[i], e.gradients[i]) : c.stroke_path(e.paths[i], color)) != BL_SUCCESS) {
        std::fprintf(stderr, "bench-blend2d: stroke refused for item %zu\n", i);
        return 2;
      }
    }
    if (it.gradient) {
      c.set_stroke_alpha(1.0);
    }
    break;
  }
  case SB_RUN: {
    BLGlyphRun run{};
    run.set_glyph_data(e.ids[i].data());
    run.set_placement_data(e.places[i].data());
    run.size = e.ids[i].size();
    run.placement_type = BL_GLYPH_PLACEMENT_TYPE_USER_UNITS;
    c.fill_glyph_run(BLPoint(0, 0), e.font, run, color);
    break;
  }
  default:
    break;
  }
  return 0;
}

int submit(void * ctx) {
  Engine & e = *static_cast<Engine *>(ctx);
  const SB_Scene & s = *e.scene;
  BLContext main_ctx;
  if (begin_ctx(main_ctx, e.image)) {
    return 1;
  }
  // The stack of what is open: 'r' a rectangular clip (a save on the current context), 'g' a group, 'p' a path clip; the
  // last two have a layer each, whose context is the current one while it is open.
  struct Open {
    char kind;
    size_t item;
    size_t layer;
  };
  std::vector<Open> open;
  std::vector<BLContext *> cur = {&main_ctx};
  size_t used = 0;
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    BLContext & c = *cur.back();
    switch (it.kind) {
    case SB_CLIP:
      c.save();
      c.clip_to_rect(BLRect(it.rect[0], it.rect[1], it.rect[2] - it.rect[0], it.rect[3] - it.rect[1]));
      open.push_back({'r', i, 0});
      break;
    case SB_GROUP:
    case SB_CLIPPATH: {
      Engine::Layer & L = *e.layers[used];
      std::memset(L.px.data(), 0, L.px.size());
      if (begin_ctx(L.ctx, L.img)) {
        return 1;
      }
      open.push_back({it.kind == SB_GROUP ? 'g' : 'p', i, used});
      cur.push_back(&L.ctx);
      used++;
      break;
    }
    case SB_UNCLIP:
    case SB_ENDGROUP: {
      Open o = open.back();
      open.pop_back();
      if (o.kind == 'r') {
        c.restore();
        break;
      }
      Engine::Layer & L = *e.layers[o.layer];
      if (L.ctx.end() != BL_SUCCESS) {
        return 2;
      }
      cur.pop_back();
      used--;
      BLContext & parent = *cur.back();
      if (o.kind == 'p') {
        // The clip's coverage: the path filled opaque into the spare layer, multiplied into the content.
        Engine::Layer & M = *e.layers[e.max_depth];
        std::memset(M.px.data(), 0, M.px.size());
        if (begin_ctx(M.ctx, M.img)) {
          return 1;
        }
        M.ctx.fill_path(e.paths[o.item], BLRgba32(255, 255, 255, 255));
        if (M.ctx.end() != BL_SUCCESS || begin_ctx(L.ctx, L.img)) {
          return 2;
        }
        L.ctx.set_comp_op(BL_COMP_OP_DST_IN);
        L.ctx.blit_image(BLPointI(0, 0), M.img);
        if (L.ctx.end() != BL_SUCCESS) {
          return 2;
        }
        parent.blit_image(BLPointI(0, 0), L.img);
      } else {
        const SB_Item & g = s.items[o.item];
        parent.set_comp_op(comp_op(g.composite));
        parent.set_global_alpha(g.opacity);
        parent.blit_image(BLPointI(0, 0), L.img);
        parent.set_global_alpha(1.0);
        parent.set_comp_op(BL_COMP_OP_SRC_OVER);
      }
      break;
    }
    default:
      if (draw_item(e, c, i)) {
        return 2;
      }
      break;
    }
  }
  return main_ctx.end() == BL_SUCCESS ? 0 : 2;
}

}  // namespace

int main(int argc, char ** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: bench-blend2d DIR OUT [SCENE...]\n");
    return 2;
  }
  static const char * const scenes[] = {"fill-rect", "fill-circle", "paths", "text", "ui", "gradients", "strokes"};
  std::vector<const char *> names(scenes, scenes + sizeof scenes / sizeof scenes[0]);
  if (argc > 3) {
    names.assign(argv + 3, argv + argc);
  }
  int failures = 0;
  for (const char * name : names) {
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
                : std::strcmp(s.metric, "picture") == 0 ? "immediate mode, no build step; BLContext fill_path and stroke_path, paths and gradients built once; groups by BLImage layers blitted with the comp-op and alpha, path clips by a coverage layer (DST_IN); dashes by BLPath::add_stroked_path in the frame"
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
