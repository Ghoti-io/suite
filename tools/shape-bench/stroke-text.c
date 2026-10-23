/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of the Ghoti.io suite's shape benchmark.
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
 * @file stroke-text.c
 *
 * Stroked text through shape-cpu, one line of real text at a time: the densest everyday input of the exact union step
 * (every counter and curve of a line of text stroked wide piles hundreds of outline pieces into one pixel).
 *
 *     stroke-text FONT [--cells N] [--points N] [--fragments N] [--crossings N] [--pam DIR] [WORD WIDTH]...
 *
 * FONT is DejaVu Sans (suite/tools/shape-success/fetch.sh fetches the pinned file).  Each WORD is laid out at 84 pixels
 * and stroked WIDTH units wide with round joins and caps, unhinted, as an item of its own on a white 1280 by 160 target;
 * with no pair given, "Union", "Gradient" and "Outline" at widths 14, 16, 18, 20 and 22.  A limit given on the command line
 * is the sink's; one not given is the library's default.  For each case it prints the result of the render, the entries of
 * its report and the FNV-1a 64 hash of the pixels, and with --pam writes the pixels (netpbm, 8-bit RGBA) so that two
 * builds can be compared byte for byte.  Exits nonzero when a case is refused.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-geometry/shape-geometry.h>
#include <ghoti.io/shape-scene/shape-scene.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIDTH 1280
#define HEIGHT 160
#define SIZE_PX 84.0

static uint64_t fnv(const unsigned char * p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
}

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int draw(GSHSCENE_Sink * sink, GSHSCENE_Store * store, GSHSCENE_Report * report, const GFNT_Face * face,
    const GSHSCENE_Resource * face_res, const char * word, double width, const char * pam_dir) {
  const GFNT_Face * faces[1] = {face};
  GFNT_TextSpan span;
  memset(&span, 0, sizeof span);
  span.length = strlen(word);
  span.style.faces = faces;
  span.style.face_count = 1;
  span.style.size = (GFNT_F26Dot6)(SIZE_PX * 64.0);
  GFNT_ParagraphOptions opt;
  memset(&opt, 0, sizeof opt);
  opt.align = GFNT_ALIGN_START;
  opt.direction = GFNT_BIDI_LTR;
  GFNT_Paragraph * para = NULL;
  GFNT_Layout layout;
  memset(&layout, 0, sizeof layout);
  GFNT_Error error;
  gfnt_error_clear(&error);
  if (gfnt_paragraph_create(word, span.length, &span, 1, &opt, NULL, &para, &error) != GFNT_OK ||
      gfnt_paragraph_layout(para, GFNT_LAYOUT_UNBOUNDED, &layout, &error) != GFNT_OK) {
    fprintf(stderr, "stroke-text: layout of \"%s\" failed\n", word);
    gfnt_paragraph_free(para);
    return 1;
  }
  GSHSCENE_List * list = NULL;
  if (gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
    return 1;
  }
  GSHGEOM_Map at;
  gshgeom_map_translate(2, 20.0, 20.0, 0.0, &at);
  GSHSCENE_Stroke_Style st;
  gshscene_stroke_style_init(&st);
  st.width = width;
  st.join = GSHSCENE_STROKE_JOIN_ROUND;
  st.cap = GSHSCENE_STROKE_CAP_ROUND;
  const GSHSCENE_Resource * style_res = NULL;
  int bad = gshscene_store_intern_stroke_style(store, &st, &style_res) != GSHSCENE_OK;
  GSHSCENE_Paint paint;
  memset(&paint, 0, sizeof paint);
  paint.kind = GSHSCENE_PAINT_SOLID;
  paint.rgba[0] = 0.1;
  paint.rgba[1] = 0.2;
  paint.rgba[2] = 0.45;
  paint.rgba[3] = 0.45;
  for (size_t i = 0; i < layout.line_count && !bad; i++) {
    const GFNT_Line * line = &layout.lines[i];
    for (size_t k = 0; k < line->run_count && !bad; k++) {
      const GFNT_LineRun * run = &line->runs[k];
      if (run->glyph_count == 0) {
        continue;
      }
      GSHSCENE_Glyph * gl = calloc(run->glyph_count, sizeof *gl);
      for (size_t g = 0; g < run->glyph_count; g++) {
        gl[g].id = run->glyphs[g].glyph;
        gl[g].x = run->glyphs[g].x / 64.0;
        gl[g].y = run->glyphs[g].y / 64.0;
      }
      GSHSCENE_Glyph_Run gr;
      memset(&gr, 0, sizeof gr);
      gr.source = GSHSCENE_GLYPHS_FROM_FONT;
      gr.face = face_res;
      gr.size = SIZE_PX;
      gr.hint_mode = GSHSCENE_HINT_NONE;
      gr.glyph_count = run->glyph_count;
      gr.glyphs = gl;
      const GSHSCENE_Resource * res = NULL;
      bad = gshscene_store_intern_glyph_run(store, &gr, &res) != GSHSCENE_OK ||
            gshscene_list_stroke(list, res, style_res, &at, &paint, 0, 0.0) != GSHSCENE_OK;
      gshscene_resource_release(res);
      free(gl);
    }
  }
  gshscene_resource_release(style_res);
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(para);
  if (bad || gshscene_list_close(list) != GSHSCENE_OK) {
    fprintf(stderr, "stroke-text: could not build the list for \"%s\"\n", word);
    gshscene_list_destroy(list);
    return 1;
  }
  GIMG_Raster * raster = NULL;
  GIMG_Pixel_Format format = GIMG_PIXEL_RGBA8;
  if (gimg_raster_create(WIDTH, HEIGHT, &format, GIMG_RASTER_OWNED, NULL, 0, &raster) != GIMG_OK) {
    return 1;
  }
  memset(gimg_raster_pixels(raster), 255, gimg_raster_stride_bytes(raster) * HEIGHT);
  GSHSCENE_Target t;
  memset(&t, 0, sizeof t);
  t.kind = GSHSCENE_TARGET_RASTER;
  t.surface = raster;
  GSHSCENE_Render_Request q;
  memset(&q, 0, sizeof q);
  q.list = list;
  q.target = &t;
  q.report = report;
  gshscene_report_reset(report);
  double t0 = now_s();
  GSHSCENE_Result r = gshscene_sink_render(sink, &q);
  double dt = now_s() - t0;
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * px = gimg_raster_pixels_const(raster);
  uint64_t h = 1469598103934665603ull;
  for (int y = 0; y < HEIGHT; y++) {
    h ^= fnv(px + (size_t)y * stride, (size_t)WIDTH * 4);
    h *= 1099511628211ull;
  }
  printf("%-9s width %-4g %-14s entries %zu hash %016llx  %.3f s\n", word, width, gshscene_result_string(r),
      gshscene_report_count(report), (unsigned long long)h, dt);
  if (pam_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/stroke-text-%s-%g.pam", pam_dir, word, width);
    FILE * f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", WIDTH, HEIGHT);
      for (int y = 0; y < HEIGHT; y++) {
        fwrite(px + (size_t)y * stride, 1, (size_t)WIDTH * 4, f);
      }
      fclose(f);
    }
  }
  gimg_raster_destroy(raster);
  gshscene_list_destroy(list);
  return r == GSHSCENE_OK ? 0 : 1;
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: stroke-text FONT [--cells N] [--points N] [--fragments N] [--crossings N] [--pam DIR] [WORD WIDTH]...\n");
    return 2;
  }
  GSHCPU_Limits limits;
  gshcpu_limits_default(&limits);
  const char * pam_dir = NULL;
  int i = 2;
  for (; i + 1 < argc && !strncmp(argv[i], "--", 2); i += 2) {
    if (!strcmp(argv[i], "--cells")) {
      limits.max_cells = (size_t)strtoull(argv[i + 1], NULL, 0);
    } else if (!strcmp(argv[i], "--points")) {
      limits.max_points = (size_t)strtoull(argv[i + 1], NULL, 0);
    } else if (!strcmp(argv[i], "--fragments")) {
      limits.max_exact_fragments = strtoull(argv[i + 1], NULL, 0);
    } else if (!strcmp(argv[i], "--crossings")) {
      limits.max_exact_crossings = strtoull(argv[i + 1], NULL, 0);
    } else if (!strcmp(argv[i], "--pam")) {
      pam_dir = argv[i + 1];
    } else {
      fprintf(stderr, "stroke-text: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  GSHSCENE_Store * store = NULL;
  GSHSCENE_Sink * sink = NULL;
  GSHSCENE_Report * report = NULL;
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_SceneFont * scene_font = NULL;
  const GSHSCENE_Resource * face_res = NULL;
  GFNT_Error error;
  gfnt_error_clear(&error);
  if (gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK || gshcpu_sink_create(NULL, NULL, &limits, &sink) != GSHCPU_OK ||
      gshscene_report_create(NULL, NULL, &report) != GSHSCENE_OK ||
      gfnt_blob_create_file(argv[1], NULL, NULL, &blob, &error) != GFNT_OK ||
      gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK || gfnt_scene_font_create(face, &scene_font, &error) != GFNT_OK ||
      gfnt_scene_intern_face(scene_font, store, NULL, &face_res, &error) != GFNT_OK) {
    fprintf(stderr, "stroke-text: setup failed\n");
    return 1;
  }
  int failures = 0;
  if (i + 1 < argc) {
    for (; i + 1 < argc; i += 2) {
      failures += draw(sink, store, report, face, face_res, argv[i], strtod(argv[i + 1], NULL), pam_dir);
    }
  } else {
    static const char * const words[] = {"Union", "Gradient", "Outline"};
    static const double widths[] = {14, 16, 18, 20, 22};
    for (size_t w = 0; w < 3; w++) {
      for (size_t k = 0; k < 5; k++) {
        failures += draw(sink, store, report, face, face_res, words[w], widths[k], pam_dir);
      }
    }
  }
  return failures ? 1 : 0;
}
