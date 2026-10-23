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
 * @file corroborate.cpp
 *
 * The cross-check of a stroke scene by corroboration: a pixel counts against a candidate frame only when no second,
 * independent source agrees with it there.
 *
 *     corroborate SCENE CANDIDATE REFERENCE (--frame SECOND | --oracle) [--perturb N]
 *
 * Reads the scene file, the candidate's frame and one reference's frame (RGBA PAM, premultiplied, like pamdiff's), and
 * prints one JSON object.  A pixel where the candidate differs from the reference by more than 32 levels in any channel
 * is a *disagreement*.  It is *corroborated* when the second source is within 32 levels of the candidate there, and
 * *counted* otherwise.  The second source is either another engine's frame (`--frame`), where that engine draws the
 * scene, or the geometric oracle of the stroke (`--oracle`): the exact region of each stroke, from the definition alone
 * (shape-geometry's installed `testing/stroke_oracle.h`, which shares no code with any stroker), sampled sixteen by
 * sixteen inside the pixel, and composited in the scene's order with the scene's paints over its background.  The oracle
 * is evaluated only at the disagreements.
 *
 * The bound is measured on the counted pixels: `counted_bad_fraction` is the counted pixels over all pixels, and
 * `counted_mean_abs` is the mean absolute difference over every channel of every pixel with the corroborated pixels'
 * differences taken as zero (what they differ by is the reference's, not the candidate's).  Corroboration only moves
 * blame between engines; it removes nothing that two sources do not both contradict.
 *
 * `--perturb N` is the control that shows the rule cannot hide a wrong frame: before the comparison, N pixels the
 * scene draws on (the candidate differs from the background there) are made wrong by inverting their color channels.
 * The output then also says how many of them were counted, and how many the second frame and the oracle each
 * contradict.
 */

#include <ghoti.io/shape-geometry/testing/stroke_oracle.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "scene.h"
}

namespace so = stroke_oracle;

namespace {

const int kBad = 32;
const int kGrid = 16;

struct Pam {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};

bool read_pam(const char * path, Pam * out) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  char line[128];
  int w = -1, h = -1, depth = -1, maxval = -1;
  bool ok = false;
  if (!fgets(line, sizeof line, f) || strcmp(line, "P7\n") != 0) {
    fclose(f);
    return false;
  }
  while (fgets(line, sizeof line, f)) {
    if (!strcmp(line, "ENDHDR\n")) {
      ok = true;
      break;
    }
    sscanf(line, "WIDTH %d", &w);
    sscanf(line, "HEIGHT %d", &h);
    sscanf(line, "DEPTH %d", &depth);
    sscanf(line, "MAXVAL %d", &maxval);
  }
  if (!ok || w <= 0 || h <= 0 || depth != 4 || maxval != 255) {
    fclose(f);
    return false;
  }
  out->w = w;
  out->h = h;
  out->px.resize((size_t)w * (size_t)h * 4);
  bool got = fread(out->px.data(), 1, out->px.size(), f) == out->px.size();
  fclose(f);
  return got;
}

int worst(const uint8_t * a, const uint8_t * b) {
  int m = 0;
  for (int c = 0; c < 4; c++) {
    int d = (int)a[c] - (int)b[c];
    d = d < 0 ? -d : d;
    m = d > m ? d : m;
  }
  return m;
}

/** The stroke of one scene item as the oracle reads it, with a box the stroke stays inside. */
struct Item {
  so::Stroked stroked;
  double box[4] = {0, 0, 0, 0};
  SB_Color color;
};

so::Seg make_seg(int n, const double * p) {
  so::Seg s;
  s.n = n;
  for (int i = 0; i <= n; i++) {
    s.p[i] = {p[2 * i], p[2 * i + 1]};
  }
  return s;
}

/** The centerline of a scene path, read the way the oracle reads a path: every subpath, the segments with extent. */
so::Centerline centerline_of(const SB_Scene * sc, const SB_Item & it) {
  so::Centerline cl;
  bool open = false;
  so::V2 cur = {0, 0}, start = {0, 0};
  for (size_t k = 0; k < it.count; k++) {
    const SB_Cmd & c = sc->cmds[it.first + k];
    if (c.op == 'M') {
      cl.subs.push_back(so::Sub{});
      cl.subs.back().start = {c.p[0], c.p[1]};
      cur = start = {c.p[0], c.p[1]};
      open = true;
      continue;
    }
    if (!open) {
      cl.subs.push_back(so::Sub{});
      cl.subs.back().start = start;
      cur = start;
      open = true;
    }
    so::Sub & sub = cl.subs.back();
    sub.draws = true;
    double q[8] = {(double)cur.x, (double)cur.y};
    int n = 1;
    if (c.op == 'L') {
      q[2] = c.p[0];
      q[3] = c.p[1];
    } else if (c.op == 'Q') {
      n = 2;
      memcpy(q + 2, c.p, 4 * sizeof(double));
    } else if (c.op == 'C') {
      n = 3;
      memcpy(q + 2, c.p, 6 * sizeof(double));
    } else { // 'Z'
      q[2] = (double)start.x;
      q[3] = (double)start.y;
    }
    so::Seg s = make_seg(n, q);
    if (so::has_extent(s)) {
      sub.segs.push_back(s);
    }
    cur = {(double)q[2 * n], (double)q[2 * n + 1]};
    if (c.op == 'Z') {
      sub.closed = true;
      open = false;
      cur = start;
    }
  }
  return cl;
}

struct Oracle {
  const SB_Scene * scene;
  std::vector<Item> items;
  std::vector<bool> built;
  std::vector<size_t> strokes; // the scene's drawables, in order

  explicit Oracle(const SB_Scene * s) : scene(s) {
    items.resize(s->n_items);
    built.assign(s->n_items, false);
    for (size_t i = 0; i < s->n_items; i++) {
      if (s->items[i].kind == SB_STROKE) {
        strokes.push_back(i);
      } else {
        fprintf(stderr, "corroborate: item %zu is not a stroke; the oracle composites strokes only\n", i);
        exit(2);
      }
    }
  }

  Item & item(size_t i) {
    if (built[i]) {
      return items[i];
    }
    built[i] = true;
    const SB_Item & it = scene->items[i];
    const SB_Stroke & sk = scene->strokes[it.stroke - 1];
    Item & out = items[i];
    out.color = it.color;
    so::Style st;
    st.width = sk.width;
    st.space = 0;
    st.join = sk.join == SB_JOIN_ROUND ? so::JOIN_ROUND : sk.join == SB_JOIN_BEVEL ? so::JOIN_BEVEL : so::JOIN_MITER;
    st.limit = sk.miter;
    st.cap = sk.cap == SB_CAP_ROUND ? so::CAP_ROUND : sk.cap == SB_CAP_SQUARE ? so::CAP_SQUARE : so::CAP_BUTT;
    st.degenerate = 0;
    for (size_t d = 0; d < sk.n_dashes; d++) {
      st.dashes.push_back(sk.dashes[d]);
    }
    st.dash_offset = sk.offset;
    st.dash_scope = 0;
    so::Centerline cl = centerline_of(scene, it);
    out.stroked = so::build_stroke(cl, st, so::Affine{}, 0);
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (size_t k = 0; k < it.count; k++) {
      const SB_Cmd & c = scene->cmds[it.first + k];
      int pts = c.op == 'C' ? 3 : c.op == 'Q' ? 2 : (c.op == 'M' || c.op == 'L') ? 1 : 0;
      for (int j = 0; j < pts; j++) {
        x0 = std::min(x0, c.p[2 * j]);
        x1 = std::max(x1, c.p[2 * j]);
        y0 = std::min(y0, c.p[2 * j + 1]);
        y1 = std::max(y1, c.p[2 * j + 1]);
      }
    }
    double reach = sk.width / 2 * std::max(sk.miter, 2.0) + 1;
    out.box[0] = x0 - reach;
    out.box[1] = y0 - reach;
    out.box[2] = x1 + reach;
    out.box[3] = y1 + reach;
    return out;
  }

  /** The coverage of one stroke at a pixel: the share of a grid inside its region, the grid refined only where a coarse one
   * disagrees with itself. */
  double coverage(Item & it, int px, int py) {
    auto at = [&](int i, int j, int n) {
      so::V2 x = {px + ((so::Real)i + 0.5L) / n, py + ((so::Real)j + 0.5L) / n};
      return so::region_contains(it.stroked.body, x).inside;
    };
    int inside = 0;
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 4; i++) {
        inside += at(i, j, 4);
      }
    }
    if (inside == 0 || inside == 16) {
      return inside ? 1.0 : 0.0;
    }
    inside = 0;
    for (int j = 0; j < kGrid; j++) {
      for (int i = 0; i < kGrid; i++) {
        inside += at(i, j, kGrid);
      }
    }
    return (double)inside / (kGrid * kGrid);
  }

  /** The pixel the scene draws, premultiplied 8-bit RGBA, by the oracle's coverage and the scene's paints. */
  void pixel(int px, int py, double out[4]) {
    const SB_Color & bg = scene->background;
    double a = bg.a / 255.0;
    out[0] = bg.r * a;
    out[1] = bg.g * a;
    out[2] = bg.b * a;
    out[3] = bg.a;
    for (size_t i : strokes) {
      Item & it = item(i);
      if (px + 1 < it.box[0] || px > it.box[2] || py + 1 < it.box[1] || py > it.box[3]) {
        continue;
      }
      double cov = coverage(it, px, py);
      if (cov == 0) {
        continue;
      }
      double s = it.color.a / 255.0 * cov;
      out[0] = it.color.r * s + out[0] * (1 - s);
      out[1] = it.color.g * s + out[1] * (1 - s);
      out[2] = it.color.b * s + out[2] * (1 - s);
      out[3] = 255.0 * s + out[3] * (1 - s);
    }
  }
};

int oracle_worst(Oracle & o, int px, int py, const uint8_t * ours) {
  double v[4];
  o.pixel(px, py, v);
  int m = 0;
  for (int c = 0; c < 4; c++) {
    int d = (int)std::lround(std::fabs(v[c] - ours[c]));
    m = d > m ? d : m;
  }
  return m;
}

} // namespace

int main(int argc, char ** argv) {
  const char * second_path = nullptr;
  bool use_oracle = false;
  long perturb = 0;
  if (argc < 5) {
    fprintf(stderr, "usage: corroborate SCENE CANDIDATE REFERENCE (--frame SECOND | --oracle) [--perturb N]\n");
    return 2;
  }
  for (int i = 4; i < argc; i++) {
    if (!strcmp(argv[i], "--frame") && i + 1 < argc) {
      second_path = argv[++i];
    } else if (!strcmp(argv[i], "--oracle")) {
      use_oracle = true;
    } else if (!strcmp(argv[i], "--perturb") && i + 1 < argc) {
      perturb = atol(argv[++i]);
    } else {
      fprintf(stderr, "corroborate: unknown argument %s\n", argv[i]);
      return 2;
    }
  }
  if ((second_path != nullptr) == use_oracle) {
    fprintf(stderr, "corroborate: exactly one of --frame and --oracle\n");
    return 2;
  }
  SB_Scene scene;
  sb_scene_init(&scene);
  char err[256];
  if (sb_scene_load(argv[1], &scene, err, sizeof err)) {
    fprintf(stderr, "corroborate: %s: %s\n", argv[1], err);
    return 2;
  }
  Pam cand, ref, second;
  if (!read_pam(argv[2], &cand) || !read_pam(argv[3], &ref) || cand.w != ref.w || cand.h != ref.h ||
      cand.w != scene.width || cand.h != scene.height) {
    fprintf(stderr, "corroborate: cannot compare %s and %s\n", argv[2], argv[3]);
    return 2;
  }
  if (second_path && (!read_pam(second_path, &second) || second.w != cand.w || second.h != cand.h)) {
    fprintf(stderr, "corroborate: cannot read %s\n", second_path);
    return 2;
  }
  Oracle oracle(&scene);
  const size_t pixels = (size_t)cand.w * (size_t)cand.h;

  /* The perturbation: N pixels the scene draws on, inverted, spread evenly over them. */
  std::vector<size_t> perturbed;
  if (perturb > 0) {
    std::vector<size_t> drawn;
    uint8_t bg[4] = {(uint8_t)(scene.background.r * scene.background.a / 255), (uint8_t)(scene.background.g * scene.background.a / 255),
        (uint8_t)(scene.background.b * scene.background.a / 255), scene.background.a};
    for (size_t i = 0; i < pixels; i++) {
      if (worst(&cand.px[4 * i], bg) > 8) {
        drawn.push_back(i);
      }
    }
    size_t n = std::min((size_t)perturb, drawn.size());
    for (size_t k = 0; k < n; k++) {
      size_t i = drawn[k * drawn.size() / n];
      for (int c = 0; c < 3; c++) {
        cand.px[4 * i + c] = (uint8_t)(255 - cand.px[4 * i + c]);
      }
      /* an inverted color can be within the threshold of the reference (a mid gray); keep only the pixels it moved */
      if (worst(&cand.px[4 * i], &ref.px[4 * i]) > kBad) {
        perturbed.push_back(i);
      }
    }
  }

  size_t disagree = 0, corroborated = 0, counted = 0, differing = 0;
  uint64_t sum_raw = 0, sum_counted = 0;
  std::map<size_t, int> verdicts; // pixel -> 1 counted
  for (size_t i = 0; i < pixels; i++) {
    int w = worst(&cand.px[4 * i], &ref.px[4 * i]);
    uint64_t d = 0;
    for (int c = 0; c < 4; c++) {
      int e = (int)cand.px[4 * i + c] - (int)ref.px[4 * i + c];
      d += (uint64_t)(e < 0 ? -e : e);
    }
    sum_raw += d;
    differing += w > 0;
    if (w <= kBad) {
      sum_counted += d;
      continue;
    }
    disagree++;
    int px = (int)(i % (size_t)cand.w), py = (int)(i / (size_t)cand.w);
    int s = second_path ? worst(&cand.px[4 * i], &second.px[4 * i]) : oracle_worst(oracle, px, py, &cand.px[4 * i]);
    if (s <= kBad) {
      corroborated++;
    } else {
      counted++;
      sum_counted += d;
      if (perturb > 0) {
        verdicts[i] = 1;
      }
    }
  }
  printf("{\"pixels\":%zu,\"mean_abs_raw\":%.6f,\"differing_pixels\":%zu,\"disagreements\":%zu,\"corroborated\":%zu,"
         "\"counted\":%zu,\"counted_bad_fraction\":%.8f,\"counted_mean_abs\":%.6f,\"bad_threshold\":%d,\"second\":\"%s\"",
      pixels, (double)sum_raw / (double)(pixels * 4), differing, disagree, corroborated, counted,
      (double)counted / (double)pixels, (double)sum_counted / (double)(pixels * 4), kBad,
      second_path ? "frame" : "oracle");
  if (perturb > 0) {
    size_t counted_p = 0, second_disagrees = 0, oracle_disagrees = 0;
    for (size_t i : perturbed) {
      counted_p += verdicts.count(i);
      int px = (int)(i % (size_t)cand.w), py = (int)(i / (size_t)cand.w);
      if (second_path && worst(&cand.px[4 * i], &second.px[4 * i]) > kBad) {
        second_disagrees++;
      }
      if (oracle_worst(oracle, px, py, &cand.px[4 * i]) > kBad) {
        oracle_disagrees++;
      }
    }
    printf(",\"perturbed\":%zu,\"perturbed_counted\":%zu,\"perturbed_second_frame_disagrees\":%zu,"
           "\"perturbed_oracle_disagrees\":%zu",
        perturbed.size(), counted_p, second_disagrees, oracle_disagrees);
  }
  printf("}\n");
  sb_scene_free(&scene);
  return 0;
}
