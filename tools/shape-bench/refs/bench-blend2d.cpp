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
  std::vector<std::vector<uint32_t>> ids;    // per item, runs
  std::vector<std::vector<BLPoint>> places;  // per item, runs
  BLFontFace face;
  BLFont font;
};

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
  e.ids.resize(s.n_items);
  e.places.resize(s.n_items);
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    if (it.kind == SB_FILL) {
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
  for (size_t i = 0; i < s.n_items; i++) {
    const SB_Item & it = s.items[i];
    BLRgba32 color(it.color.r, it.color.g, it.color.b, it.color.a);
    switch (it.kind) {
    case SB_FILL:
      c.fill_path(e.paths[i], color);
      break;
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
  static const char * const scenes[] = {"fill-rect", "fill-circle", "paths", "text", "ui"};
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
