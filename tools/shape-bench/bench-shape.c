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
 * @file bench-shape.c
 *
 * The scene generator and the two engines of this suite the benchmark measures.
 *
 *     bench-shape emit DIR FONT     write the scenes into DIR (FONT is copied beside them by the caller)
 *     bench-shape run DIR OUT       measure shape-cpu on every scene, and font on the text scene
 *
 * `run` prints one JSON line a measurement and writes each engine's last frame,
 * and shape-cpu's control frame (every sixteenth drawable dropped), as PAM
 * files into OUT.
 *
 * shape-cpu is driven as an application would drive it: path resources (and,
 * for text, one path a glyph and one glyph run a line; for a gradient fill,
 * its gradient) interned once into a
 * shape-scene store; each frame records a list (the build) and renders it
 * through the CPU sink (the submit), strict, into a borrowed premultiplied
 * RGBA8 raster.
 *
 * font is driven through its installed API only: every glyph of every frame is
 * rendered by gfnt_face_render_glyph (unhinted, at its quarter-pixel origin),
 * and composited by this file's own premultiplied source-over blit, because
 * font has no compositor.  It has no glyph cache, and none is added here.
 */

#define _POSIX_C_SOURCE 200809L

#include "scene.h"

#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/font.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-geometry/shape-geometry.h>
#include <ghoti.io/shape-scene/shape-scene.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- the generator */

static uint64_t rng_state = 0x5eed15b0a7c0ffeeull;

static double rnd(void) {
  /* splitmix64: deterministic everywhere, no libc rand. */
  uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  z ^= z >> 31;
  return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static double range(double lo, double hi) {
  return lo + (hi - lo) * rnd();
}

/** Round to a multiple of 1/64, which every engine and every writer carries exactly. */
static double q64(double v) {
  return floor(v * 64.0 + 0.5) / 64.0;
}

static SB_Color color(int alpha_lo, int alpha_hi) {
  SB_Color c;
  c.r = (uint8_t)range(0, 256);
  c.g = (uint8_t)range(0, 256);
  c.b = (uint8_t)range(0, 256);
  c.a = (uint8_t)range(alpha_lo, alpha_hi + 1);
  return c;
}

static int add_fill(SB_Scene * s, SB_Color c) {
  SB_Item it;
  memset(&it, 0, sizeof it);
  it.kind = SB_FILL;
  it.color = c;
  it.first = s->n_cmds;
  return sb_scene_add_item(s, &it);
}

/** Close the fill opened last: its command count is what was added since. */
static void end_fill(SB_Scene * s) {
  SB_Item * it = &s->items[s->n_items - 1];
  it->count = s->n_cmds - it->first;
}

static int cmd(SB_Scene * s, char op, double a, double b, double c, double d, double e, double f) {
  double p[6] = {q64(a), q64(b), q64(c), q64(d), q64(e), q64(f)};
  return sb_scene_add_cmd(s, op, p);
}

static int rect(SB_Scene * s, SB_Color c, double x0, double y0, double x1, double y1) {
  if (add_fill(s, c) || cmd(s, 'M', x0, y0, 0, 0, 0, 0) || cmd(s, 'L', x1, y0, 0, 0, 0, 0) ||
      cmd(s, 'L', x1, y1, 0, 0, 0, 0) || cmd(s, 'L', x0, y1, 0, 0, 0, 0) || cmd(s, 'Z', 0, 0, 0, 0, 0, 0)) {
    return -1;
  }
  end_fill(s);
  return 0;
}

/* The cubic approximation of a quarter circle. */
#define KAPPA 0.5522847498307936

static int circle(SB_Scene * s, SB_Color c, double cx, double cy, double r) {
  double k = r * KAPPA;
  if (add_fill(s, c) || cmd(s, 'M', cx + r, cy, 0, 0, 0, 0) ||
      cmd(s, 'C', cx + r, cy + k, cx + k, cy + r, cx, cy + r) ||
      cmd(s, 'C', cx - k, cy + r, cx - r, cy + k, cx - r, cy) ||
      cmd(s, 'C', cx - r, cy - k, cx - k, cy - r, cx, cy - r) ||
      cmd(s, 'C', cx + k, cy - r, cx + r, cy - k, cx + r, cy) || cmd(s, 'Z', 0, 0, 0, 0, 0, 0)) {
    return -1;
  }
  end_fill(s);
  return 0;
}

static int round_rect(SB_Scene * s, SB_Color c, double x0, double y0, double x1, double y1, double r) {
  double k = r * KAPPA;
  if (add_fill(s, c) || cmd(s, 'M', x0 + r, y0, 0, 0, 0, 0) || cmd(s, 'L', x1 - r, y0, 0, 0, 0, 0) ||
      cmd(s, 'C', x1 - r + k, y0, x1, y0 + r - k, x1, y0 + r) || cmd(s, 'L', x1, y1 - r, 0, 0, 0, 0) ||
      cmd(s, 'C', x1, y1 - r + k, x1 - r + k, y1, x1 - r, y1) || cmd(s, 'L', x0 + r, y1, 0, 0, 0, 0) ||
      cmd(s, 'C', x0 + r - k, y1, x0, y1 - r + k, x0, y1 - r) || cmd(s, 'L', x0, y0 + r, 0, 0, 0, 0) ||
      cmd(s, 'C', x0, y0 + r - k, x0 + r - k, y0, x0 + r, y0) || cmd(s, 'Z', 0, 0, 0, 0, 0, 0)) {
    return -1;
  }
  end_fill(s);
  return 0;
}

static int polygon(SB_Scene * s, SB_Color c, const double * xy, int n) {
  if (add_fill(s, c) || cmd(s, 'M', xy[0], xy[1], 0, 0, 0, 0)) {
    return -1;
  }
  for (int i = 1; i < n; i++) {
    if (cmd(s, 'L', xy[2 * i], xy[2 * i + 1], 0, 0, 0, 0)) {
      return -1;
    }
  }
  if (cmd(s, 'Z', 0, 0, 0, 0, 0, 0)) {
    return -1;
  }
  end_fill(s);
  return 0;
}

static int clip(SB_Scene * s, double x0, double y0, double x1, double y1) {
  SB_Item it;
  memset(&it, 0, sizeof it);
  it.kind = SB_CLIP;
  it.rect[0] = q64(x0);
  it.rect[1] = q64(y0);
  it.rect[2] = q64(x1);
  it.rect[3] = q64(y1);
  return sb_scene_add_item(s, &it);
}

static int unclip(SB_Scene * s) {
  SB_Item it;
  memset(&it, 0, sizeof it);
  it.kind = SB_UNCLIP;
  return sb_scene_add_item(s, &it);
}

static void begin(SB_Scene * s, const char * name, const char * metric, int w, int h, const char * unit) {
  sb_scene_init(s);
  snprintf(s->name, sizeof s->name, "%s", name);
  snprintf(s->metric, sizeof s->metric, "%s", metric);
  snprintf(s->work_unit, sizeof s->work_unit, "%s", unit);
  s->width = w;
  s->height = h;
  s->background.r = s->background.g = s->background.b = s->background.a = 255;
}

/* Fill rate, rectangles: twelve translucent rectangles covering most of the frame, edges off the pixel grid. */
static int gen_fill_rect(SB_Scene * s) {
  begin(s, "fill-rect", "fill", 1920, 1080, "pixels");
  for (int i = 0; i < 12; i++) {
    double x0 = q64(range(0, 120)), y0 = q64(range(0, 60));
    double x1 = q64(x0 + range(1650, 1770)), y1 = q64(y0 + range(950, 1010));
    if (rect(s, color(96, 224), x0, y0, x1, y1)) {
      return -1;
    }
    s->work += (x1 - x0) * (y1 - y0);
  }
  return 0;
}

/* Fill rate, curves: twelve translucent circles of radius 300 to 500, wholly inside the frame. */
static int gen_fill_circle(SB_Scene * s) {
  begin(s, "fill-circle", "fill", 1920, 1080, "pixels");
  for (int i = 0; i < 12; i++) {
    double r = q64(range(300, 500));
    double cx = q64(range(r, 1920 - r)), cy = q64(range(r, 1080 - r));
    if (circle(s, color(96, 224), cx, cy, r)) {
      return -1;
    }
    s->work += 3.14159265358979323846 * r * r;
  }
  return 0;
}

/* Path count: 10,000 small paths of four kinds (triangle, quadratic blob, cubic circle, star), 4 to 48 pixels. */
static int gen_paths(SB_Scene * s) {
  begin(s, "paths", "paths", 1920, 1080, "paths");
  for (int i = 0; i < 10000; i++) {
    double size = range(2, 24);
    double cx = range(48, 1920 - 48), cy = range(48, 1080 - 48);
    double turn = range(0, 6.283185307179586);
    SB_Color c = color(64, 255);
    int rc = 0;
    switch (i % 4) {
    case 0: {
      double xy[6];
      for (int k = 0; k < 3; k++) {
        double a = turn + k * 2.0943951023931953;
        xy[2 * k] = cx + size * cos(a);
        xy[2 * k + 1] = cy + size * sin(a);
      }
      rc = polygon(s, c, xy, 3);
      break;
    }
    case 1:
      rc = add_fill(s, c) || cmd(s, 'M', cx + size, cy, 0, 0, 0, 0) ||
          cmd(s, 'Q', cx + size, cy + size, cx, cy + size * 0.6, 0, 0) ||
          cmd(s, 'Q', cx - size, cy + size, cx - size * 0.7, cy, 0, 0) ||
          cmd(s, 'Q', cx - size, cy - size, cx, cy - size * 0.8, 0, 0) ||
          cmd(s, 'Q', cx + size, cy - size, cx + size, cy, 0, 0) || cmd(s, 'Z', 0, 0, 0, 0, 0, 0);
      if (!rc) {
        end_fill(s);
      }
      break;
    case 2:
      rc = circle(s, c, cx, cy, size);
      break;
    default: {
      double xy[20];
      for (int k = 0; k < 10; k++) {
        double a = turn + k * 0.6283185307179586, rr = k % 2 ? size * 0.45 : size;
        xy[2 * k] = cx + rr * cos(a);
        xy[2 * k + 1] = cy + rr * sin(a);
      }
      rc = polygon(s, c, xy, 10);
      break;
    }
    }
    if (rc) {
      return -1;
    }
    s->work += 1;
  }
  return 0;
}

/* The list: a UI-like frame of 24 panels, each a shadow, a rounded body, and inside a rectangular clip a header and
 * seven rows of a rounded button, a two-piece icon, three text bars (some running past the clip) and a hairline. */
static int gen_ui(SB_Scene * s) {
  begin(s, "ui", "list", 1280, 800, "items");
  s->background.r = 236;
  s->background.g = 238;
  s->background.b = 242;
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 6; col++) {
      double x0 = 10 + col * 211.5, y0 = 10 + row * 197.25, x1 = x0 + 200.25, y1 = y0 + 186.5;
      SB_Color shadow = {0, 0, 0, 40}, body = {255, 255, 255, 255}, head = color(255, 255), line = {200, 204, 210, 255};
      if (round_rect(s, shadow, x0 + 1.5, y0 + 2.5, x1 + 1.5, y1 + 2.5, 8) || round_rect(s, body, x0, y0, x1, y1, 8) ||
          clip(s, x0 + 4, y0 + 4, x1 - 4, y1 - 4) || rect(s, head, x0, y0, x1, y0 + 22.5)) {
        return -1;
      }
      for (int k = 0; k < 7; k++) {
        double ry = y0 + 28 + k * 22.25;
        SB_Color button = color(160, 230), icon = color(200, 255), text = {40, 44, 52, 255};
        double tri[6] = {x0 + 44, ry + 15, x0 + 50, ry + 4, x0 + 56, ry + 15};
        if (round_rect(s, button, x0 + 8, ry + 2, x0 + 30, ry + 18, 4) || circle(s, icon, x0 + 19, ry + 10, 5.5) ||
            polygon(s, icon, tri, 3)) {
          return -1;
        }
        double tx = x0 + 62;
        for (int t = 0; t < 3; t++) {
          double w = range(18, t == 2 ? 140 : 50);
          if (rect(s, text, tx, ry + 7, tx + w, ry + 13)) {
            return -1;
          }
          tx += w + 5;
        }
        if (rect(s, line, x0 + 6, ry + 20.75, x1 - 6, ry + 21.5)) {
          return -1;
        }
      }
      if (unclip(s)) {
        return -1;
      }
    }
  }
  s->work = (double)sb_scene_drawables(s);
  return 0;
}

/* One gradient stop as the generator's table gives it: offset, then an opaque color. */
typedef struct Gen_Stop {
  double offset;
  uint8_t r, g, b;
} Gen_Stop;

typedef struct Gen_Gradient {
  int circle;         ///< the shape: 0 a rectangle, 1 a circle, both centered on the cell
  SB_Gradient_Kind kind;
  SB_Extend extend;
  double g[6];        ///< the geometry relative to the shape's center (sweep angles absolute, degrees)
  int n;
  Gen_Stop stops[5];
} Gen_Gradient;

/*
 * Fill rate, gradients (color-paint story P2, decision D9): twelve opaque shapes on a 4 by 3 grid of 480 by 360
 * cells, each overlapping its neighbors, edges off the pixel grid; rows 0 and 2 rectangles of 580.5 by 431 (clamped
 * to the frame), row 1 circles of radius 236.75.  Each is painted with its own gradient of 2 to 5 opaque stops,
 * offsets from 0 to 1, alpha multiplier 1, in the target's own encoded values, so that premultiplied and straight
 * interpolation agree and no conversion happens anywhere.
 *
 * The sub-cases are the ones every engine here draws by the same definition (shape-scene's G7 to G9); the drivers
 * state their mappings.  Linear: pad, repeat and reflect.  Radial: concentric with a start circle of radius 0, pad,
 * repeat and reflect, and one focal gradient, pad, whose start circle is a point inside the end circle (the
 * two-circle cone that SVG's focal point, Blend2D's focal point and Skia's two-point conical all are there).
 * Sweep: only what Blend2D's conic gradient, which always spans whole turns, can say: a full turn from 0 degrees
 * (pad), a full turn from 30 degrees (repeat, so that the angles below 30 wrap as Blend2D's do), and a third of a turn
 * repeated (Blend2D's repeat count 3).  Left out, because a reference cannot express them: a sweep over a part of a
 * turn under pad or reflect, a radial start circle of nonzero radius, and a focal point outside the end circle.
 */
static const Gen_Gradient GRADIENTS[12] = {
    /* row 0: rectangles */
    {0, SB_GRADIENT_LINEAR, SB_EXTEND_PAD, {-200, -60, 180, 70}, 2,
        {{0, 230, 40, 40}, {1, 30, 60, 220}}},
    {0, SB_GRADIENT_LINEAR, SB_EXTEND_REPEAT, {-40, -30, 60, 50}, 3,
        {{0, 250, 220, 40}, {0.5, 40, 160, 80}, {1, 20, 20, 90}}},
    {0, SB_GRADIENT_LINEAR, SB_EXTEND_REFLECT, {-90, 0, 30, -110}, 4,
        {{0, 255, 255, 255}, {0.25, 200, 30, 120}, {0.625, 10, 120, 200}, {1, 0, 0, 0}}},
    {0, SB_GRADIENT_RADIAL, SB_EXTEND_PAD, {0, 0, 0, 0, 0, 170}, 3,
        {{0, 255, 240, 200}, {0.5, 220, 100, 20}, {1, 60, 10, 40}}},
    /* row 1: circles */
    {1, SB_GRADIENT_LINEAR, SB_EXTEND_PAD, {0, -200, 0, 180}, 5,
        {{0, 240, 20, 20}, {0.25, 240, 200, 20}, {0.5, 20, 200, 60}, {0.75, 20, 120, 240}, {1, 140, 20, 200}}},
    {1, SB_GRADIENT_RADIAL, SB_EXTEND_REPEAT, {0, 0, 0, 0, 0, 75.5}, 3,
        {{0, 10, 30, 80}, {0.4375, 120, 220, 230}, {1, 250, 250, 240}}},
    {1, SB_GRADIENT_SWEEP, SB_EXTEND_PAD, {0, 0, 0, 360}, 5,
        {{0, 255, 0, 0}, {0.25, 255, 255, 0}, {0.5, 0, 200, 80}, {0.75, 0, 80, 255}, {1, 160, 0, 200}}},
    {1, SB_GRADIENT_RADIAL, SB_EXTEND_PAD, {-90.5, -60.25, 0, 0, 0, 230}, 4,
        {{0, 255, 255, 230}, {0.375, 250, 160, 60}, {0.75, 150, 30, 30}, {1, 30, 10, 20}}},
    /* row 2: rectangles */
    {0, SB_GRADIENT_RADIAL, SB_EXTEND_REFLECT, {0, 0, 0, 0, 0, 95.25}, 2,
        {{0, 20, 80, 40}, {1, 200, 240, 120}}},
    {0, SB_GRADIENT_SWEEP, SB_EXTEND_REPEAT, {0, 0, 30, 390}, 4,
        {{0, 30, 30, 30}, {0.25, 230, 230, 230}, {0.5, 200, 40, 40}, {1, 40, 40, 200}}},
    {0, SB_GRADIENT_SWEEP, SB_EXTEND_REPEAT, {0, 0, 0, 120}, 3,
        {{0, 250, 200, 0}, {0.5, 0, 150, 150}, {1, 100, 0, 60}}},
    {0, SB_GRADIENT_LINEAR, SB_EXTEND_REPEAT, {-30, 40, 45, -20}, 2,
        {{0, 255, 128, 0}, {1, 0, 64, 128}}},
};

static int gen_gradients(SB_Scene * s) {
  begin(s, "gradients", "fill", 1920, 1080, "pixels");
  for (int k = 0; k < 12; k++) {
    const Gen_Gradient * gg = &GRADIENTS[k];
    int col = k % 4, row = k / 4;
    double cx = 240 + 480 * col + 0.40625, cy = 180 + 360 * row + 0.28125;
    SB_Gradient g;
    memset(&g, 0, sizeof g);
    g.kind = gg->kind;
    g.extend = gg->extend;
    g.first_stop = s->n_stops;
    g.n_stops = (size_t)gg->n;
    switch (gg->kind) {
    case SB_GRADIENT_LINEAR:
      g.g[0] = cx + gg->g[0];
      g.g[1] = cy + gg->g[1];
      g.g[2] = cx + gg->g[2];
      g.g[3] = cy + gg->g[3];
      break;
    case SB_GRADIENT_RADIAL:
      g.g[0] = cx + gg->g[0];
      g.g[1] = cy + gg->g[1];
      g.g[2] = gg->g[2];
      g.g[3] = cx + gg->g[3];
      g.g[4] = cy + gg->g[4];
      g.g[5] = gg->g[5];
      break;
    case SB_GRADIENT_SWEEP:
      g.g[0] = cx;
      g.g[1] = cy;
      g.g[2] = gg->g[2];
      g.g[3] = gg->g[3];
      break;
    }
    for (int j = 0; j < gg->n; j++) {
      SB_Color c = {gg->stops[j].r, gg->stops[j].g, gg->stops[j].b, 255};
      if (sb_scene_add_stop(s, gg->stops[j].offset, c)) {
        return -1;
      }
    }
    if (sb_scene_add_gradient(s, &g)) {
      return -1;
    }
    SB_Color paint = {0, 0, 0, 255};
    if (gg->circle) {
      double r = 236.75;
      if (circle(s, paint, cx, cy, r)) {
        return -1;
      }
      s->work += 3.14159265358979323846 * r * r;
    } else {
      double x0 = fmax(cx - 290.25, 2.25), x1 = fmin(cx + 290.25, 1920 - 2.375);
      double y0 = fmax(cy - 215.5, 2.25), y1 = fmin(cy + 215.5, 1080 - 2.375);
      if (rect(s, paint, x0, y0, x1, y1)) {
        return -1;
      }
      s->work += (x1 - x0) * (y1 - y0);
    }
    s->items[s->n_items - 1].gradient = s->n_gradients;
  }
  return 0;
}

/* Stroke throughput (strokes story S4, F6): 2,500 stroked paths (and 800 dashed ones in a scene of their own) of six kinds on 1920x1080, 30 to 250 pixels across, a
 * width of 1 to 9 pixels, translucent: a miter-joined polyline, a round-joined and round-capped chain of quadratics, a
 * bevel-joined closed polygon, a dashed open cubic wave, a round-joined circle, a zigzag with square caps and a miter
 * limit of 2 (whose sharp corners are bevels).  Everything here is a style all three engines state in one definition
 * (SVG's): width, join, cap, miter limit, and dashes on open subpaths restarting at the path's start.  Left out because
 * an engine cannot say it or says it differently: alignment, a width in device pixels, miter-clip and arcs joins,
 * dashes on a closed subpath (SVG 2 and the references differ at the start), degenerate subpaths. */
static int add_stroke(SB_Scene * s, SB_Color c, const SB_Stroke * st) {
  SB_Item it;
  memset(&it, 0, sizeof it);
  it.kind = SB_STROKE;
  it.color = c;
  it.first = s->n_cmds;
  if (sb_scene_add_stroke(s, st)) {
    return -1;
  }
  it.stroke = s->n_strokes;
  return sb_scene_add_item(s, &it);
}

/* Two scenes, because Blend2D's context draws a dashed stroke solid (its dash array is accepted and not applied: a minimal
 * pair is in the story's change log), so the dashed kind is a scene of its own that the Blend2D driver does not draw and
 * the others cross-check: `strokes` holds the five undashed kinds, `strokes-dashed` the dashed wave. */
static int gen_strokes(SB_Scene * s, int dashed) {
  static const int undashed[5] = {0, 1, 2, 4, 5};
  begin(s, dashed ? "strokes-dashed" : "strokes", "paths", 1920, 1080, "paths");
  const int count = dashed ? 800 : 2500;
  for (int i = 0; i < count; i++) {
    double cx = range(140, 1920 - 140), cy = range(140, 1080 - 140);
    double span = range(30, 250);
    double turn = range(0, 6.283185307179586);
    SB_Color c = color(64, 255);
    SB_Stroke st;
    memset(&st, 0, sizeof st);
    st.width = q64(range(1, 9));
    st.miter = 4.0;
    int rc = 0;
    double pt[12];
    switch (dashed ? 3 : undashed[i % 5]) {
    case 0: /* a polyline of six points, miter joins, butt caps */
      st.join = SB_JOIN_MITER;
      st.cap = SB_CAP_BUTT;
      for (int k = 0; k < 6; k++) {
        double a = turn + k * 0.9 + range(-0.3, 0.3), r = span * (0.35 + 0.65 * rnd());
        pt[2 * k] = cx + r * cos(a);
        pt[2 * k + 1] = cy + r * sin(a);
      }
      rc = add_stroke(s, c, &st) || cmd(s, 'M', pt[0], pt[1], 0, 0, 0, 0);
      for (int k = 1; k < 6 && !rc; k++) {
        rc = cmd(s, 'L', pt[2 * k], pt[2 * k + 1], 0, 0, 0, 0);
      }
      break;
    case 1: /* a chain of three quadratics, round joins and caps */
      st.join = SB_JOIN_ROUND;
      st.cap = SB_CAP_ROUND;
      rc = add_stroke(s, c, &st) || cmd(s, 'M', cx - span, cy, 0, 0, 0, 0);
      for (int k = 0; k < 3 && !rc; k++) {
        double x = cx - span + (k + 1) * span * 2.0 / 3.0;
        rc = cmd(s, 'Q', x - span / 3.0, cy + (k % 2 ? span : -span) * (0.4 + 0.6 * rnd()), x,
            cy + range(-span * 0.3, span * 0.3), 0, 0);
      }
      break;
    case 2: { /* a closed polygon of five to seven corners, bevel joins */
      int n = 5 + (i / 6) % 3;
      st.join = SB_JOIN_BEVEL;
      st.cap = SB_CAP_BUTT;
      rc = add_stroke(s, c, &st);
      for (int k = 0; k < n && !rc; k++) {
        double a = turn + k * 6.283185307179586 / n, r = span * (0.55 + 0.45 * rnd());
        rc = cmd(s, k ? 'L' : 'M', cx + r * cos(a), cy + r * sin(a), 0, 0, 0, 0);
      }
      rc = rc || cmd(s, 'Z', 0, 0, 0, 0, 0, 0);
      break;
    }
    case 3: /* a dashed cubic wave, open, butt or round caps */
      st.join = SB_JOIN_ROUND;
      st.cap = (i / 6) % 2 ? SB_CAP_ROUND : SB_CAP_BUTT;
      st.n_dashes = 2;
      st.dashes[0] = q64(range(4, 14));
      st.dashes[1] = q64(range(3, 9));
      st.offset = q64(range(0, 10));
      rc = add_stroke(s, c, &st) || cmd(s, 'M', cx - span, cy, 0, 0, 0, 0) ||
          cmd(s, 'C', cx - span * 0.4, cy - span * 0.9, cx + span * 0.1, cy + span * 0.9, cx + span, cy);
      break;
    case 4: { /* a circle, round joins (a closed subpath of four cubics) */
      double r = span * 0.5, k = r * KAPPA;
      st.join = SB_JOIN_ROUND;
      st.cap = SB_CAP_BUTT;
      rc = add_stroke(s, c, &st) || cmd(s, 'M', cx + r, cy, 0, 0, 0, 0) ||
          cmd(s, 'C', cx + r, cy + k, cx + k, cy + r, cx, cy + r) ||
          cmd(s, 'C', cx - k, cy + r, cx - r, cy + k, cx - r, cy) ||
          cmd(s, 'C', cx - r, cy - k, cx - k, cy - r, cx, cy - r) ||
          cmd(s, 'C', cx + k, cy - r, cx + r, cy - k, cx + r, cy) || cmd(s, 'Z', 0, 0, 0, 0, 0, 0);
      break;
    }
    default: /* a zigzag of seven points, square caps, miter limit 2 */
      st.join = SB_JOIN_MITER;
      st.cap = SB_CAP_SQUARE;
      st.miter = 2.0;
      rc = add_stroke(s, c, &st) || cmd(s, 'M', cx - span, cy, 0, 0, 0, 0);
      for (int k = 1; k < 7 && !rc; k++) {
        rc = cmd(s, 'L', cx - span + k * span / 3.0, cy + (k % 2 ? -1 : 1) * span * (0.2 + 0.4 * rnd()), 0, 0, 0, 0);
      }
      break;
    }
    if (rc) {
      return -1;
    }
    s->items[s->n_items - 1].count = s->n_cmds - s->items[s->n_items - 1].first;
    s->work += 1;
  }
  return 0;
}

static const char TEXT[] =
    "The quick brown fox jumps over the lazy dog while a benchmark counts every glyph it draws. "
    "Speed is measured, never assumed: each engine renders the same page, at the same size, onto the same pixels. "
    "Pack my box with five dozen liquor jugs; sphinx of black quartz, judge my vow. 0123456789 (+-*/=) [x, y] {a; b} "
    "How razorback-jumping frogs can level six piqued gymnasts! Amazingly few discotheques provide jukeboxes. ";

/* Text: a 1920 by 1080 page of 16-pixel DejaVu Sans, glyphs on quarter-pixel x positions and integer baselines,
 * unhinted, advances from hmtx; spaces advance and draw nothing, so they are not in the runs. */
static int gen_text(SB_Scene * s, const char * font_path, const char * font_name) {
  begin(s, "text", "text", 1920, 1080, "glyphs");
  snprintf(s->font, sizeof s->font, "%s", font_name);
  s->font_size = 16;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  uint16_t upem = 0;
  int status = -1;
  if (gfnt_blob_create_file(font_path, NULL, NULL, &blob, NULL) != GFNT_OK ||
      gfnt_face_load(blob, 0, NULL, NULL, &face, NULL) != GFNT_OK ||
      gfnt_face_units_per_em(face, &upem, NULL) != GFNT_OK || upem == 0) {
    fprintf(stderr, "bench-shape: cannot read the font %s\n", font_path);
    goto done;
  }
  size_t pos = 0, len = strlen(TEXT);
  for (int baseline = 20; baseline <= 1080 - 6; baseline += 19) {
    SB_Item it;
    memset(&it, 0, sizeof it);
    it.kind = SB_RUN;
    it.color.r = 24;
    it.color.g = 24;
    it.color.b = 32;
    it.color.a = 255;
    it.first = s->n_glyphs;
    double pen = 6.0;
    for (;;) {
      uint32_t cp = (unsigned char)TEXT[pos % len], gid = 0;
      int32_t adv = 0;
      if (gfnt_face_glyph_for_codepoint(face, cp, &gid, NULL) != GFNT_OK || gid == 0 ||
          gfnt_face_glyph_advance(face, gid, NULL, &adv, NULL) != GFNT_OK) {
        fprintf(stderr, "bench-shape: the font has no glyph for U+%04X\n", (unsigned)cp);
        goto done;
      }
      double step = (double)adv * s->font_size / (double)upem;
      if (pen + step > 1920 - 6) {
        break;
      }
      if (cp != ' ' && sb_scene_add_glyph(s, gid, floor(pen * 4.0 + 0.5) / 4.0, (double)baseline)) {
        goto done;
      }
      pen += step;
      pos++;
    }
    it.count = s->n_glyphs - it.first;
    if (it.count && sb_scene_add_item(s, &it)) {
      goto done;
    }
  }
  s->work = (double)s->n_glyphs;
  status = 0;
done:
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return status;
}

/* The gradients scene, and the strokes scene after it, come last, so that the generator's random stream, and with it every earlier scene, is as
 * it was before they existed (the gradients scene draws nothing from that stream; the strokes scene draws from it last). */
static const char * const SCENES[] = {"fill-rect", "fill-circle", "paths", "text", "ui", "gradients", "strokes", "strokes-dashed"};
#define N_SCENES (sizeof SCENES / sizeof SCENES[0])

static int emit(const char * dir, const char * font_path) {
  const char * font_name = strrchr(font_path, '/') ? strrchr(font_path, '/') + 1 : font_path;
  for (size_t i = 0; i < N_SCENES; i++) {
    SB_Scene s;
    int rc = i == 0 ? gen_fill_rect(&s)
        : i == 1    ? gen_fill_circle(&s)
        : i == 2    ? gen_paths(&s)
        : i == 3    ? gen_text(&s, font_path, font_name)
        : i == 4    ? gen_ui(&s)
        : i == 5    ? gen_gradients(&s)
        : i == 6    ? gen_strokes(&s, 0)
                    : gen_strokes(&s, 1);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.scene", dir, SCENES[i]);
    if (rc || sb_scene_save(path, &s)) {
      fprintf(stderr, "bench-shape: cannot write %s\n", path);
      sb_scene_free(&s);
      return 1;
    }
    fprintf(stderr, "bench-shape: %s: %zu items, %zu commands, %zu glyphs, work %.6g %s\n", s.name, s.n_items,
        s.n_cmds, s.n_glyphs, s.work, s.work_unit);
    sb_scene_free(&s);
  }
  return 0;
}

/* ---------------------------------------------------------------- shape-cpu */

typedef struct Shape {
  const SB_Scene * scene;
  int control; ///< drop every SB_CONTROL_STRIDE-th drawable (scene.h)
  int empty;   ///< record nothing: the fixed cost of a render on the extent
  size_t drawables;
  GSHSCENE_Store * store;
  GSHSCENE_List * list;
  GSHSCENE_Sink * sink;
  GSHSCENE_Report * report;
  GIMG_Raster * raster;
  uint8_t * pixels;
  size_t stride;
  uint8_t bg[4];
  const GSHSCENE_Resource ** res; ///< per item: the path, the run or the clip's rectangle
  const GSHSCENE_Resource ** glyph_paths;
  size_t n_glyph_paths;
  GSHSCENE_Paint * paint; ///< per item
  const GSHSCENE_Resource ** grad; ///< per item: a gradient fill's gradient
  const GSHSCENE_Resource ** style; ///< per item: a stroke's style
} Shape;

static GSHGEOM_Point pt(double x, double y) {
  GSHGEOM_Point p = {x, y, 0.0};
  return p;
}

static const GSHSCENE_Resource * intern_cmds(GSHSCENE_Store * store, const SB_Cmd * c, size_t n) {
  GSHGEOM_Path * path = NULL;
  const GSHSCENE_Resource * r = NULL;
  if (gshgeom_path_create(NULL, NULL, 2, &path) != GSHGEOM_OK) {
    return NULL;
  }
  int bad = 0;
  for (size_t i = 0; i < n && !bad; i++) {
    const double * p = c[i].p;
    switch (c[i].op) {
    case 'M':
      bad = gshgeom_path_move_to(path, pt(p[0], p[1])) != GSHGEOM_OK;
      break;
    case 'L':
      bad = gshgeom_path_line_to(path, pt(p[0], p[1])) != GSHGEOM_OK;
      break;
    case 'Q':
      bad = gshgeom_path_quadratic_to(path, pt(p[0], p[1]), pt(p[2], p[3])) != GSHGEOM_OK;
      break;
    case 'C':
      bad = gshgeom_path_cubic_to(path, pt(p[0], p[1]), pt(p[2], p[3]), pt(p[4], p[5])) != GSHGEOM_OK;
      break;
    default:
      bad = gshgeom_path_close(path) != GSHGEOM_OK;
      break;
    }
  }
  if (!bad && gshscene_store_intern_path(store, path, &r) != GSHSCENE_OK) {
    r = NULL;
  }
  gshgeom_path_destroy(path);
  return r;
}

/* A glyph's outline, scaled to pixels by font (26.6, as font renders it) and flipped to y down, as a path. */
typedef struct Outline_Ctx {
  GSHGEOM_Path * path;
  int bad;
} Outline_Ctx;

static GSHGEOM_Point fp(GFNT_Point p) {
  return pt((double)p.x / 64.0, -(double)p.y / 64.0);
}
static GFNT_Result o_move(void * u, GFNT_Point to) {
  Outline_Ctx * o = u;
  o->bad |= gshgeom_path_move_to(o->path, fp(to)) != GSHGEOM_OK;
  return GFNT_OK;
}
static GFNT_Result o_line(void * u, GFNT_Point to) {
  Outline_Ctx * o = u;
  o->bad |= gshgeom_path_line_to(o->path, fp(to)) != GSHGEOM_OK;
  return GFNT_OK;
}
static GFNT_Result o_quad(void * u, GFNT_Point c, GFNT_Point to) {
  Outline_Ctx * o = u;
  o->bad |= gshgeom_path_quadratic_to(o->path, fp(c), fp(to)) != GSHGEOM_OK;
  return GFNT_OK;
}
static GFNT_Result o_cubic(void * u, GFNT_Point c1, GFNT_Point c2, GFNT_Point to) {
  Outline_Ctx * o = u;
  o->bad |= gshgeom_path_cubic_to(o->path, fp(c1), fp(c2), fp(to)) != GSHGEOM_OK;
  return GFNT_OK;
}
static GFNT_Result o_close(void * u) {
  Outline_Ctx * o = u;
  o->bad |= gshgeom_path_close(o->path) != GSHGEOM_OK;
  return GFNT_OK;
}

static const GSHSCENE_Resource * intern_glyph(GSHSCENE_Store * store, const GFNT_Face * face, uint32_t gid,
    GFNT_F16Dot16 scale) {
  GFNT_Outline * outline = NULL;
  const GSHSCENE_Resource * r = NULL;
  Outline_Ctx o = {NULL, 0};
  GFNT_OutlineSink sink = {o_move, o_line, o_quad, o_cubic, o_close};
  if (gfnt_face_glyph_outline(face, gid, NULL, NULL, &outline, NULL) != GFNT_OK ||
      gfnt_outline_scale(outline, scale, NULL) != GFNT_OK ||
      gshgeom_path_create(NULL, NULL, 2, &o.path) != GSHGEOM_OK ||
      gfnt_outline_decompose(outline, &sink, &o, NULL) != GFNT_OK || o.bad ||
      gshscene_store_intern_path(store, o.path, &r) != GSHSCENE_OK) {
    r = NULL;
  }
  gshgeom_path_destroy(o.path);
  gfnt_outline_destroy(outline);
  return r;
}

/*
 * A scene gradient as shape-scene's resource (paint.h, G1 to G9): the geometry as the scene gives it, the identity
 * map, the target's space (zero) for the interpolation and for every stop, straight interpolation; the sweep's angles
 * in degrees, which is the scene's unit.  Interned once, before timing, like the paths.
 */
static const GSHSCENE_Resource * intern_gradient(GSHSCENE_Store * store, const SB_Scene * s, const SB_Gradient * sg) {
  GSHSCENE_Gradient_Stop stops[SB_GRADIENT_STOPS_MAX];
  GSHSCENE_Gradient g;
  const GSHSCENE_Resource * r = NULL;
  memset(&g, 0, sizeof g);
  memset(stops, 0, sizeof stops);
  if (sg->n_stops > SB_GRADIENT_STOPS_MAX || gshgeom_map_identity(2, &g.map) != GSHGEOM_OK) {
    return NULL;
  }
  switch (sg->kind) {
  case SB_GRADIENT_LINEAR:
    g.kind = GSHSCENE_GRADIENT_LINEAR;
    g.p0[0] = sg->g[0];
    g.p0[1] = sg->g[1];
    g.p1[0] = sg->g[2];
    g.p1[1] = sg->g[3];
    break;
  case SB_GRADIENT_RADIAL:
    g.kind = GSHSCENE_GRADIENT_RADIAL;
    g.c0[0] = sg->g[0];
    g.c0[1] = sg->g[1];
    g.r0 = sg->g[2];
    g.c1[0] = sg->g[3];
    g.c1[1] = sg->g[4];
    g.r1 = sg->g[5];
    break;
  case SB_GRADIENT_SWEEP:
    g.kind = GSHSCENE_GRADIENT_SWEEP;
    g.center[0] = sg->g[0];
    g.center[1] = sg->g[1];
    g.angle_unit = GSHGEOM_ANGLE_DEGREES;
    g.a0 = sg->g[2];
    g.a1 = sg->g[3];
    break;
  }
  g.extend = sg->extend == SB_EXTEND_REPEAT ? GSHSCENE_EXTEND_REPEAT
      : sg->extend == SB_EXTEND_REFLECT     ? GSHSCENE_EXTEND_REFLECT
                                            : GSHSCENE_EXTEND_PAD;
  g.premultiplied = 0;
  for (size_t k = 0; k < sg->n_stops; k++) {
    const SB_Stop * st = &s->stops[sg->first_stop + k];
    stops[k].offset = st->offset;
    stops[k].color.kind = GSHSCENE_PAINT_SOLID;
    stops[k].color.rgba[0] = st->color.r / 255.0;
    stops[k].color.rgba[1] = st->color.g / 255.0;
    stops[k].color.rgba[2] = st->color.b / 255.0;
    stops[k].color.rgba[3] = st->color.a / 255.0;
  }
  g.stop_count = sg->n_stops;
  g.stops = stops;
  if (gshscene_store_intern_gradient(store, &g, &r) != GSHSCENE_OK) {
    r = NULL;
  }
  return r;
}

static int shape_setup(Shape * e, const SB_Scene * s, const char * dir, int control) {
  memset(e, 0, sizeof *e);
  e->scene = s;
  e->control = control;
  e->drawables = sb_scene_drawables(s);
  e->stride = (size_t)s->width * 4;
  e->pixels = malloc(e->stride * (size_t)s->height);
  e->res = calloc(s->n_items ? s->n_items : 1, sizeof *e->res);
  e->paint = calloc(s->n_items ? s->n_items : 1, sizeof *e->paint);
  e->grad = calloc(s->n_items ? s->n_items : 1, sizeof *e->grad);
  e->style = calloc(s->n_items ? s->n_items : 1, sizeof *e->style);
  sb_background_bytes(s, 0, e->bg);
  GIMG_Pixel_Format format = GIMG_PIXEL_RGBA8;
  format.alpha_form = GIMG_ALPHA_PREMULTIPLIED;
  if (!e->pixels || !e->res || !e->paint || !e->grad || !e->style || gshscene_store_create(NULL, NULL, &e->store) != GSHSCENE_OK ||
      gshscene_list_create(NULL, NULL, e->store, &e->list) != GSHSCENE_OK ||
      gshcpu_sink_create(NULL, NULL, NULL, &e->sink) != GSHCPU_OK ||
      gshscene_report_create(NULL, NULL, &e->report) != GSHSCENE_OK ||
      gimg_raster_create((uint32_t)s->width, (uint32_t)s->height, &format, GIMG_RASTER_BORROWED, e->pixels, e->stride,
          &e->raster) != GIMG_OK) {
    fprintf(stderr, "bench-shape: shape-cpu setup failed for %s\n", s->name);
    return -1;
  }
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_F16Dot16 scale = 0;
  uint32_t max_gid = 0;
  if (s->font[0]) {
    char path[1024];
    uint16_t upem = 0;
    snprintf(path, sizeof path, "%s/%s", dir, s->font);
    if (gfnt_blob_create_file(path, NULL, NULL, &blob, NULL) != GFNT_OK ||
        gfnt_face_load(blob, 0, NULL, NULL, &face, NULL) != GFNT_OK ||
        gfnt_face_units_per_em(face, &upem, NULL) != GFNT_OK) {
      fprintf(stderr, "bench-shape: cannot read %s\n", path);
      return -1;
    }
    scale = gfnt_scale_for_ppem(upem, (uint32_t)s->font_size);
    for (size_t i = 0; i < s->n_glyphs; i++) {
      max_gid = s->glyphs[i].id > max_gid ? s->glyphs[i].id : max_gid;
    }
    e->n_glyph_paths = (size_t)max_gid + 1;
    e->glyph_paths = calloc(e->n_glyph_paths, sizeof *e->glyph_paths);
    if (!e->glyph_paths) {
      return -1;
    }
  }
  int status = 0;
  for (size_t i = 0; i < s->n_items && status == 0; i++) {
    const SB_Item * it = &s->items[i];
    if (it->kind == SB_FILL || it->kind == SB_RUN || it->kind == SB_STROKE) {
      GSHSCENE_Paint * p = &e->paint[i];
      p->kind = GSHSCENE_PAINT_SOLID;
      p->rgba[0] = it->color.r / 255.0;
      p->rgba[1] = it->color.g / 255.0;
      p->rgba[2] = it->color.b / 255.0;
      p->rgba[3] = it->color.a / 255.0;
      if (it->gradient) {
        e->grad[i] = intern_gradient(e->store, s, &s->gradients[it->gradient - 1]);
        if (!e->grad[i]) {
          fprintf(stderr, "bench-shape: cannot intern the gradient of item %zu of %s\n", i, s->name);
          status = -1;
          break;
        }
        p->kind = GSHSCENE_PAINT_GRADIENT;
        p->rgba[0] = p->rgba[1] = p->rgba[2] = 0.0;
        p->gradient = e->grad[i];
      }
    }
    if (it->kind == SB_STROKE) {
      /* The scene's style is shape-scene's, field for field: local width, the pen's join and cap, the miter limit, the
       * dashes restarting at each subpath from the offset; a stroke is centered. */
      const SB_Stroke * sk = &s->strokes[it->stroke - 1];
      GSHSCENE_Stroke_Style st;
      gshscene_stroke_style_init(&st);
      st.width = sk->width;
      st.join = sk->join == SB_JOIN_ROUND ? GSHSCENE_STROKE_JOIN_ROUND
          : sk->join == SB_JOIN_BEVEL     ? GSHSCENE_STROKE_JOIN_BEVEL
                                          : GSHSCENE_STROKE_JOIN_MITER;
      st.cap = sk->cap == SB_CAP_ROUND ? GSHSCENE_STROKE_CAP_ROUND
          : sk->cap == SB_CAP_SQUARE   ? GSHSCENE_STROKE_CAP_SQUARE
                                       : GSHSCENE_STROKE_CAP_BUTT;
      st.miter_limit = sk->miter;
      st.dash_count = sk->n_dashes;
      st.dashes = sk->n_dashes ? sk->dashes : NULL;
      st.dash_offset = sk->offset;
      if (gshscene_store_intern_stroke_style(e->store, &st, &e->style[i]) != GSHSCENE_OK) {
        fprintf(stderr, "bench-shape: cannot intern the style of item %zu of %s\n", i, s->name);
        status = -1;
        break;
      }
    }
    if (it->kind == SB_FILL || it->kind == SB_STROKE) {
      e->res[i] = intern_cmds(e->store, s->cmds + it->first, it->count);
    } else if (it->kind == SB_CLIP) {
      SB_Cmd c[5] = {{'M', {it->rect[0], it->rect[1]}}, {'L', {it->rect[2], it->rect[1]}},
          {'L', {it->rect[2], it->rect[3]}}, {'L', {it->rect[0], it->rect[3]}}, {'Z', {0}}};
      e->res[i] = intern_cmds(e->store, c, 5);
    } else if (it->kind == SB_RUN) {
      GSHSCENE_Glyph * g = calloc(it->count, sizeof *g);
      if (!g) {
        status = -1;
        break;
      }
      for (size_t k = 0; k < it->count; k++) {
        const SB_Glyph * sg = &s->glyphs[it->first + k];
        if (!e->glyph_paths[sg->id]) {
          e->glyph_paths[sg->id] = intern_glyph(e->store, face, sg->id, scale);
        }
        g[k].id = sg->id;
        g[k].x = sg->x;
        g[k].y = sg->y;
        g[k].outline = e->glyph_paths[sg->id];
        if (!g[k].outline) {
          status = -1;
        }
      }
      GSHSCENE_Glyph_Run run;
      memset(&run, 0, sizeof run);
      run.source = GSHSCENE_GLYPHS_FROM_OUTLINES;
      run.glyph_count = it->count;
      run.glyphs = g;
      if (status == 0 && gshscene_store_intern_glyph_run(e->store, &run, &e->res[i]) != GSHSCENE_OK) {
        e->res[i] = NULL;
      }
      free(g);
    } else {
      continue;
    }
    if (!e->res[i]) {
      fprintf(stderr, "bench-shape: cannot intern item %zu of %s\n", i, s->name);
      status = -1;
    }
  }
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
  return status;
}

static void shape_teardown(Shape * e) {
  for (size_t i = 0; e->res && i < e->scene->n_items; i++) {
    gshscene_resource_release(e->res[i]);
    gshscene_resource_release(e->grad ? e->grad[i] : NULL);
    gshscene_resource_release(e->style ? e->style[i] : NULL);
  }
  for (size_t i = 0; i < e->n_glyph_paths; i++) {
    gshscene_resource_release(e->glyph_paths[i]);
  }
  gshscene_report_destroy(e->report);
  gshscene_sink_destroy(e->sink);
  gimg_raster_destroy(e->raster);
  gshscene_list_destroy(e->list);
  gshscene_store_destroy(e->store);
  free(e->glyph_paths);
  free(e->res);
  free(e->paint);
  free(e->grad);
  free(e->style);
  free(e->pixels);
  memset(e, 0, sizeof *e);
}

static int shape_clear(void * ctx) {
  Shape * e = ctx;
  sb_clear(e->pixels, e->scene->width, e->scene->height, e->stride, e->bg);
  return 0;
}

static int shape_build(void * ctx) {
  Shape * e = ctx;
  const SB_Scene * s = e->scene;
  if (gshscene_list_reset(e->list) != GSHSCENE_OK) {
    return 1;
  }
  size_t drawable = 0;
  for (size_t i = 0; i < s->n_items && !e->empty; i++) {
    const SB_Item * it = &s->items[i];
    GSHSCENE_Result r = GSHSCENE_OK;
    switch (it->kind) {
    case SB_FILL:
    case SB_RUN:
      if (!(e->control && (e->drawables - 1 - drawable) % SB_CONTROL_STRIDE == 0)) {
        r = gshscene_list_fill(e->list, e->res[i], NULL, GSHGEOM_FILL_NONZERO, &e->paint[i], 0, 0.0);
      }
      drawable++;
      break;
    case SB_STROKE:
      if (!(e->control && (e->drawables - 1 - drawable) % SB_CONTROL_STRIDE == 0)) {
        r = gshscene_list_stroke(e->list, e->res[i], e->style[i], NULL, &e->paint[i], 0, 0.0);
      }
      drawable++;
      break;
    case SB_CLIP:
      r = gshscene_list_clip_push(e->list, e->res[i], NULL, GSHGEOM_FILL_NONZERO, 0, 0.0);
      break;
    case SB_UNCLIP:
      r = gshscene_list_clip_pop(e->list);
      break;
    }
    if (r != GSHSCENE_OK) {
      fprintf(stderr, "bench-shape: recording item %zu: %s\n", i, gshscene_result_string(r));
      return 2;
    }
  }
  return gshscene_list_close(e->list) == GSHSCENE_OK ? 0 : 3;
}

static int shape_submit(void * ctx) {
  Shape * e = ctx;
  GSHSCENE_Target target;
  memset(&target, 0, sizeof target);
  target.kind = GSHSCENE_TARGET_RASTER;
  target.surface = e->raster;
  GSHSCENE_Render_Request request;
  memset(&request, 0, sizeof request);
  request.list = e->list;
  request.target = &target;
  request.report = e->report;
  request.flags = GSHSCENE_RENDER_STRICT;
  GSHSCENE_Result r = gshscene_sink_render(e->sink, &request);
  if (r != GSHSCENE_OK) {
    fprintf(stderr, "bench-shape: render: %s\n", gshscene_result_string(r));
    gshscene_report_dump(e->report, stderr);
    return 4;
  }
  return 0;
}

/* ---------------------------------------------------------------- font */

typedef struct Font {
  const SB_Scene * scene;
  GFNT_Blob * blob;
  GFNT_Face * face;
  uint8_t * pixels;
  size_t stride;
  uint8_t bg[4];
} Font;

static int font_setup(Font * e, const SB_Scene * s, const char * dir) {
  memset(e, 0, sizeof *e);
  e->scene = s;
  e->stride = (size_t)s->width * 4;
  e->pixels = malloc(e->stride * (size_t)s->height);
  sb_background_bytes(s, 0, e->bg);
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, s->font);
  if (!e->pixels || gfnt_blob_create_file(path, NULL, NULL, &e->blob, NULL) != GFNT_OK ||
      gfnt_face_load(e->blob, 0, NULL, NULL, &e->face, NULL) != GFNT_OK) {
    fprintf(stderr, "bench-shape: font setup failed\n");
    return -1;
  }
  return 0;
}

static void font_teardown(Font * e) {
  gfnt_face_free(e->face);
  gfnt_blob_destroy(e->blob);
  free(e->pixels);
  memset(e, 0, sizeof *e);
}

static int font_clear(void * ctx) {
  Font * e = ctx;
  sb_clear(e->pixels, e->scene->width, e->scene->height, e->stride, e->bg);
  return 0;
}

/* The harness's blit: premultiplied source-over of a straight color under 8-bit coverage, rounded once a channel. */
static void blit(Font * e, const GFNT_Coverage * cov, int x0, int y0, SB_Color c) {
  int w = e->scene->width, h = e->scene->height;
  for (uint32_t r = 0; r < cov->height; r++) {
    int y = y0 + (int)r;
    if (y < 0 || y >= h) {
      continue;
    }
    const uint8_t * src = cov->data + (size_t)r * cov->stride;
    uint8_t * dst = e->pixels + (size_t)y * e->stride;
    for (uint32_t k = 0; k < cov->width; k++) {
      int x = x0 + (int)k;
      if (x < 0 || x >= w || src[k] == 0) {
        continue;
      }
      unsigned ca = (src[k] * (unsigned)c.a + 127u) / 255u, inv = 255u - ca;
      uint8_t * d = dst + 4 * x;
      d[0] = (uint8_t)((c.r * ca + d[0] * inv + 127u) / 255u);
      d[1] = (uint8_t)((c.g * ca + d[1] * inv + 127u) / 255u);
      d[2] = (uint8_t)((c.b * ca + d[2] * inv + 127u) / 255u);
      d[3] = (uint8_t)((255u * ca + d[3] * inv + 127u) / 255u);
    }
  }
}

static int font_submit(void * ctx) {
  Font * e = ctx;
  const SB_Scene * s = e->scene;
  GFNT_RasterOptions opt;
  memset(&opt, 0, sizeof opt);
  opt.hint_mode = GFNT_HINT_MODE_NONE;
  for (size_t i = 0; i < s->n_items; i++) {
    const SB_Item * it = &s->items[i];
    if (it->kind != SB_RUN) {
      continue;
    }
    for (size_t k = 0; k < it->count; k++) {
      const SB_Glyph * g = &s->glyphs[it->first + k];
      double ix = floor(g->x);
      opt.origin_x = (GFNT_F26Dot6)((g->x - ix) * 64.0);
      GFNT_Coverage cov;
      if (gfnt_face_render_glyph(e->face, g->id, (uint32_t)s->font_size, &opt, NULL, &cov, NULL) != GFNT_OK) {
        fprintf(stderr, "bench-shape: font cannot render glyph %u\n", (unsigned)g->id);
        return 5;
      }
      if (cov.data) {
        blit(e, &cov, (int)ix + cov.left, (int)g->y - cov.top, it->color);
      }
      gfnt_coverage_destroy(&cov);
    }
  }
  return 0;
}

/* ---------------------------------------------------------------- run */

static int run(const char * dir, const char * out) {
  int failures = 0;
  for (size_t i = 0; i < N_SCENES; i++) {
    char path[1024], err[256], pam[1024], name[256];
    SB_Scene s;
    snprintf(path, sizeof path, "%s/%s.scene", dir, SCENES[i]);
    if (sb_scene_load(path, &s, err, sizeof err)) {
      fprintf(stderr, "bench-shape: %s: %s\n", path, err);
      return 1;
    }
    /* shape-cpu: the measurement, then the control frame. */
    Shape e;
    SB_Measure m;
    SB_Engine eng = {&e, shape_clear, shape_build, shape_submit};
    int rc = shape_setup(&e, &s, dir, 0);
    if (rc == 0) {
      rc = sb_measure(&eng, &m);
    }
    if (rc == 0) {
      snprintf(name, sizeof name, "shape-cpu.%s.pam", s.name);
      snprintf(pam, sizeof pam, "%s/%s", out, name);
      rc = sb_write_pam(pam, s.width, s.height, e.pixels, e.stride, 0);
    }
    if (rc == 0) {
      sb_report(stdout, "shape-cpu", &s, &m, name,
          !strcmp(s.metric, "text") ? "glyph outlines from font, decoded and interned once; one outline glyph run a line"
              : !strncmp(s.name, "strokes", 7) ? "styles and paths interned once; the expansion of each stroke is cached by the sink "
                                             "across frames (warm after the first), its coverage composed per frame"
              : s.n_gradients       ? "gradients interned once with the paths; evaluated per pixel in double at the pixel "
                                      "center, straight, in the target's space, undithered"
                                    : NULL);
    }
    shape_teardown(&e);
    /* The fixed cost of a render on this extent: the same sink and target, an empty list. */
    if (rc == 0) {
      rc = shape_setup(&e, &s, dir, 0);
      e.empty = 1;
      if (rc == 0) {
        rc = sb_measure(&eng, &m);
      }
      if (rc == 0) {
        sb_report(stdout, "shape-cpu-empty", &s, &m, "none", "an empty list on the same extent and target");
      }
      shape_teardown(&e);
    }
    if (rc == 0) {
      rc = shape_setup(&e, &s, dir, 1);
      if (rc == 0 && !(rc = shape_clear(&e)) && !(rc = shape_build(&e)) && !(rc = shape_submit(&e))) {
        snprintf(pam, sizeof pam, "%s/shape-cpu.%s.control.pam", out, s.name);
        rc = sb_write_pam(pam, s.width, s.height, e.pixels, e.stride, 0);
      }
      shape_teardown(&e);
    }
    if (rc) {
      fprintf(stderr, "bench-shape: shape-cpu failed on %s (%d)\n", s.name, rc);
      failures++;
    }
    /* font, on the text scene. */
    if (!strcmp(s.metric, "text")) {
      Font f;
      SB_Engine feng = {&f, font_clear, NULL, font_submit};
      rc = font_setup(&f, &s, dir);
      if (rc == 0) {
        rc = sb_measure(&feng, &m);
      }
      if (rc == 0) {
        snprintf(name, sizeof name, "font.%s.pam", s.name);
        snprintf(pam, sizeof pam, "%s/%s", out, name);
        rc = sb_write_pam(pam, s.width, s.height, f.pixels, f.stride, 0);
      }
      if (rc == 0) {
        sb_report(stdout, "font", &s, &m, name,
            "gfnt_face_render_glyph per glyph per frame, unhinted, no cache; the composite is the harness's blit");
      } else {
        fprintf(stderr, "bench-shape: font failed on %s (%d)\n", s.name, rc);
        failures++;
      }
      font_teardown(&f);
    }
    sb_scene_free(&s);
  }
  return failures ? 1 : 0;
}

int main(int argc, char ** argv) {
  if (argc == 4 && !strcmp(argv[1], "emit")) {
    return emit(argv[2], argv[3]);
  }
  if (argc == 4 && !strcmp(argv[1], "run")) {
    return run(argv[2], argv[3]);
  }
  fprintf(stderr, "usage: bench-shape emit DIR FONT | bench-shape run DIR OUT\n");
  return 2;
}
