/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of the Ghoti.io suite's shape gallery.
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
 * @file gallery.c
 *
 * A gallery of eight PNGs drawn entirely by the suite's own libraries, for a
 * person to look at (README.md says what each shows).  Every pixel comes from
 * shape-cpu rendering shape-scene lists; text is font's paragraph layout and
 * shaping, its glyphs reaching the scene through font's glyph path provider;
 * the PNG encoding is image's own writer (gimg_doc_save, "png").
 *
 *     gallery FONTDIR DEJAVU OUTDIR
 *
 * FONTDIR is font's tests/data/fonts, DEJAVU the pinned DejaVuSans.ttf that
 * suite/tools/shape-success/fetch.sh fetches.  Writes shapes.png,
 * compositing.png, blend-modes.png, color.png, gradients.png, strokes.png, text.png and color-glyphs.png
 * into OUTDIR and a log on stdout: each render's result and its report entries
 * tallied by class and reason, the diagnostics of a few fixture fonts, and what
 * the color-glyph page refused.  Exits nonzero when any step failed.
 *
 * The images change as the libraries do; compare.py shows how.  The text drawn
 * into them is part of the picture, so changing a caption changes pixels.
 */

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/font/scene_color.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-geometry/shape-geometry.h>
#include <ghoti.io/shape-scene/shape-scene.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GSHSCENE_Store * g_store;
static GSHSCENE_Sink * g_cpu;
static GSHSCENE_Report * g_report;
static const char * g_out;
static int g_failed;

static void fail(const char * what) {
  fprintf(stderr, "gallery: %s\n", what);
  g_failed = 1;
}

/* --- paths ---------------------------------------------------------------------------------------------------- */

static GSHGEOM_Point P(double x, double y) {
  GSHGEOM_Point p = {x, y, 0.0};
  return p;
}

static GSHGEOM_Path * path_new(void) {
  GSHGEOM_Path * p = NULL;
  if (gshgeom_path_create(NULL, NULL, 2, &p) != GSHGEOM_OK) {
    fail("path create");
    exit(1);
  }
  return p;
}

static const GSHSCENE_Resource * intern(GSHGEOM_Path * path) {
  const GSHSCENE_Resource * r = NULL;
  if (gshscene_store_intern_path(g_store, path, &r) != GSHSCENE_OK) {
    fail("intern path");
  }
  gshgeom_path_destroy(path);
  return r;
}

static void add_rect(GSHGEOM_Path * p, double x0, double y0, double x1, double y1) {
  gshgeom_path_move_to(p, P(x0, y0));
  gshgeom_path_line_to(p, P(x1, y0));
  gshgeom_path_line_to(p, P(x1, y1));
  gshgeom_path_line_to(p, P(x0, y1));
  gshgeom_path_close(p);
}

static const GSHSCENE_Resource * rect(double x0, double y0, double x1, double y1) {
  GSHGEOM_Path * p = path_new();
  add_rect(p, x0, y0, x1, y1);
  return intern(p);
}

static void add_circle(GSHGEOM_Path * p, double cx, double cy, double r) {
  static const double w[3] = {1.0, 0.70710678118654752440, 1.0};
  gshgeom_path_move_to(p, P(cx + r, cy));
  gshgeom_path_rational_quadratic_to(p, P(cx + r, cy + r), P(cx, cy + r), w);
  gshgeom_path_rational_quadratic_to(p, P(cx - r, cy + r), P(cx - r, cy), w);
  gshgeom_path_rational_quadratic_to(p, P(cx - r, cy - r), P(cx, cy - r), w);
  gshgeom_path_rational_quadratic_to(p, P(cx + r, cy - r), P(cx + r, cy), w);
  gshgeom_path_close(p);
}

static const GSHSCENE_Resource * circle(double cx, double cy, double r) {
  GSHGEOM_Path * p = path_new();
  add_circle(p, cx, cy, r);
  return intern(p);
}

/* --- paints, fills ------------------------------------------------------------------------------------------- */

static GSHSCENE_Paint solid(double r, double g, double b, double a) {
  GSHSCENE_Paint p;
  memset(&p, 0, sizeof p);
  p.kind = GSHSCENE_PAINT_SOLID;
  p.rgba[0] = r;
  p.rgba[1] = g;
  p.rgba[2] = b;
  p.rgba[3] = a;
  return p;
}

static void fill_map(GSHSCENE_List * L, const GSHSCENE_Resource * res, const GSHGEOM_Map * map, GSHGEOM_Fill_Rule rule,
    GSHSCENE_Paint paint) {
  GSHSCENE_Result r = gshscene_list_fill(L, res, map, rule, &paint, 0, 0.0);
  if (r != GSHSCENE_OK) {
    fprintf(stderr, "gallery: fill: %s\n", gshscene_result_string(r));
    g_failed = 1;
  }
  gshscene_resource_release(res);
}

static void fill(GSHSCENE_List * L, const GSHSCENE_Resource * res, GSHSCENE_Paint paint) {
  fill_map(L, res, NULL, GSHGEOM_FILL_NONZERO, paint);
}

static void fill_rect(GSHSCENE_List * L, double x0, double y0, double x1, double y1, GSHSCENE_Paint paint) {
  fill(L, rect(x0, y0, x1, y1), paint);
}

static GSHSCENE_List * list_new(void) {
  GSHSCENE_List * L = NULL;
  if (gshscene_list_create(NULL, NULL, g_store, &L) != GSHSCENE_OK) {
    fail("list create");
    exit(1);
  }
  return L;
}

static GSHSCENE_Group group_props(double opacity, uint32_t composite) {
  GSHSCENE_Group g;
  memset(&g, 0, sizeof g);
  g.opacity = opacity;
  g.composite = composite;
  return g;
}

static void group_push(GSHSCENE_List * L, GSHSCENE_Group g) {
  if (gshscene_list_group_push(L, &g, 0, 0.0) != GSHSCENE_OK) {
    fail("group push");
  }
}

static void group_pop(GSHSCENE_List * L) {
  if (gshscene_list_group_pop(L) != GSHSCENE_OK) {
    fail("group pop");
  }
}

/* --- rendering ------------------------------------------------------------------------------------------------- */

static GIMG_Raster * raster_new(int w, int h) {
  GIMG_Raster * r = NULL;
  GIMG_Pixel_Format format = GIMG_PIXEL_RGBA8;
  if (gimg_raster_create((uint32_t)w, (uint32_t)h, &format, GIMG_RASTER_OWNED, NULL, 0, &r) != GIMG_OK) {
    fail("raster create");
    exit(1);
  }
  memset(gimg_raster_pixels(r), 0, gimg_raster_stride_bytes(r) * (size_t)h);
  return r;
}

/** Close the list, render it with shape-cpu onto the raster (over what it holds), print the report's entries
 *  grouped by class and reason, and destroy the list. */
static void render(const char * what, GSHSCENE_List * L, GIMG_Raster * raster) {
  if (gshscene_list_close(L) != GSHSCENE_OK) {
    fail("list close");
  }
  GSHSCENE_Target t;
  memset(&t, 0, sizeof t);
  t.kind = GSHSCENE_TARGET_RASTER;
  t.surface = raster;
  GSHSCENE_Render_Request q;
  memset(&q, 0, sizeof q);
  q.list = L;
  q.target = &t;
  q.report = g_report;
  GSHSCENE_Result r = gshscene_sink_render(g_cpu, &q);
  printf("render %s: %s, %zu items, %zu report entries\n", what, gshscene_result_string(r), gshscene_list_count(L),
      gshscene_report_count(g_report));
  if (r != GSHSCENE_OK) {
    g_failed = 1;
  }
  /* Tally entries by (class, reason). */
  struct {
    int c, r;
    size_t n;
  } tally[64];
  size_t nt = 0;
  for (size_t i = 0; i < gshscene_report_count(g_report); i++) {
    GSHSCENE_Report_Entry e;
    gshscene_report_entry(g_report, i, &e);
    size_t k = 0;
    while (k < nt && !(tally[k].c == (int)e.entry_class && tally[k].r == (int)e.reason)) {
      k++;
    }
    if (k == nt && nt < 64) {
      tally[nt].c = (int)e.entry_class;
      tally[nt].r = (int)e.reason;
      tally[nt].n = 0;
      nt++;
    }
    if (k < 64) {
      tally[k].n++;
    }
  }
  for (size_t k = 0; k < nt; k++) {
    printf("  entry %s %s x%zu\n", gshscene_entry_class_string((GSHSCENE_Entry_Class)tally[k].c),
        gshscene_reason_string((GSHSCENE_Reason)tally[k].r), tally[k].n);
  }
  /* A placeholder's entry names its source and code in `second`; the registry prints them without the front end. */
  for (size_t i = 0; i < gshscene_report_count(g_report); i++) {
    GSHSCENE_Report_Entry e;
    gshscene_report_entry(g_report, i, &e);
    if (e.reason == GSHSCENE_REASON_PLACEHOLDER) {
      printf("  placeholder %s %s (source %u, code %u) at item %zu\n",
          gshscene_entry_class_string(e.entry_class),
          gshscene_placeholder_string((unsigned)(e.second >> 16), (unsigned)(e.second & 0xffffu)),
          (unsigned)(e.second >> 16), (unsigned)(e.second & 0xffffu), e.chain_count ? e.chain[0] : (size_t)0);
    }
  }
  gshscene_list_destroy(L);
}

static void save_png(GIMG_Raster * raster, const char * name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", g_out, name);
  GIMG_Doc * doc = NULL;
  GIMG_Stream * stream = NULL;
  GIMG_Save_Report rep;
  memset(&rep, 0, sizeof rep);
  if (gimg_doc_from_raster(raster, &doc) != GIMG_OK || gimg_stream_create_memory_output(&stream) != GIMG_OK) {
    fail("png doc/stream");
    return;
  }
  GIMG_Result r = gimg_doc_save(doc, stream, "png", NULL, &rep);
  if (r != GIMG_OK) {
    fprintf(stderr, "gallery: png save of %s failed (%d)\n", name, (int)r);
    g_failed = 1;
  } else {
    const void * data = NULL;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &data, &size);
    FILE * f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f) != 0) {
      fail("write png");
    } else {
      printf("wrote %s (%zu bytes)\n", path, size);
    }
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
}

static const unsigned char * pixel(GIMG_Raster * r, int x, int y) {
  return (const unsigned char *)gimg_raster_pixels_const(r) + (size_t)y * gimg_raster_stride_bytes(r) + (size_t)x * 4;
}

/* --- fonts and text ------------------------------------------------------------------------------------------- */

typedef struct Font {
  char name[128];
  GFNT_Blob * blob;
  GFNT_Face * face;
  GFNT_SceneFont * scene;
  const GSHSCENE_Resource * res;
} Font;

static Font g_fonts[64];
static size_t g_nfonts;
static Font * g_dejavu;

static Font * font_load(const char * path, const char * name) {
  if (g_nfonts == 64) {
    return NULL;
  }
  Font * f = &g_fonts[g_nfonts];
  memset(f, 0, sizeof *f);
  snprintf(f->name, sizeof f->name, "%s", name);
  GFNT_Error error;
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(path, NULL, NULL, &f->blob, &error) != GFNT_OK ||
      gfnt_face_load(f->blob, 0, NULL, NULL, &f->face, &error) != GFNT_OK ||
      gfnt_scene_font_create(f->face, &f->scene, &error) != GFNT_OK ||
      gfnt_scene_intern_face(f->scene, g_store, NULL, &f->res, &error) != GFNT_OK) {
    fprintf(stderr, "gallery: cannot load %s\n", path);
    gfnt_error_dump(&error, stderr);
    return NULL;
  }
  g_nfonts++;
  return f;
}

static Font * font_of(const GFNT_Face * face) {
  for (size_t i = 0; i < g_nfonts; i++) {
    if (g_fonts[i].face == face) {
      return &g_fonts[i];
    }
  }
  return NULL;
}

typedef struct TextStyle {
  double px;            /* layout size, whole pixels */
  double run_size;      /* the glyph run's size; 0 = px */
  uint32_t hint_mode;   /* GSHSCENE_HINT_* */
  GFNT_Align align;
  GFNT_BidiDirection dir;
  int32_t width;        /* whole pixels; 0 = unbounded */
  GSHSCENE_Paint paint;
} TextStyle;

static TextStyle style(double px, double r, double g, double b) {
  TextStyle s;
  memset(&s, 0, sizeof s);
  s.px = px;
  s.align = GFNT_ALIGN_START;
  s.dir = GFNT_BIDI_LTR;
  s.paint = solid(r, g, b, 1.0);
  return s;
}

typedef struct TextBox {
  double width, height; /* typographic */
  int ok;
} TextBox;

/** Lay `text` out with font's paragraph API in the given faces and record one glyph run per line run, the
 *  paragraph's top-left at (x, y) (whole pixels, so a run at a whole size stays on the hinted fast tier). */
static TextBox text_draw(GSHSCENE_List * L, Font * const * fonts, size_t nf, const char * text, const TextStyle * s,
    double x, double y) {
  TextBox box = {0, 0, 0};
  const GFNT_Face * faces[8];
  for (size_t i = 0; i < nf && i < 8; i++) {
    faces[i] = fonts[i]->face;
  }
  GFNT_TextSpan span;
  memset(&span, 0, sizeof span);
  span.length = strlen(text);
  span.style.faces = faces;
  span.style.face_count = nf;
  span.style.size = (GFNT_F26Dot6)(s->px * 64.0);
  GFNT_ParagraphOptions opt;
  memset(&opt, 0, sizeof opt);
  opt.align = s->align;
  opt.direction = s->dir;
  GFNT_Paragraph * para = NULL;
  GFNT_Layout layout;
  memset(&layout, 0, sizeof layout);
  GFNT_Error error;
  gfnt_error_clear(&error);
  if (gfnt_paragraph_create(text, span.length, &span, 1, &opt, NULL, &para, &error) != GFNT_OK ||
      gfnt_paragraph_layout(para, s->width ? (GFNT_F26Dot6)(s->width * 64) : GFNT_LAYOUT_UNBOUNDED, &layout, &error) !=
          GFNT_OK) {
    fprintf(stderr, "gallery: layout of \"%s\" in %s failed\n", text, fonts[0]->name);
    gfnt_error_dump(&error, stderr);
    gfnt_paragraph_free(para);
    return box;
  }
  GSHGEOM_Map at;
  gshgeom_map_translate(2, x, y, 0.0, &at);
  for (size_t i = 0; i < layout.line_count; i++) {
    const GFNT_Line * line = &layout.lines[i];
    for (size_t k = 0; k < line->run_count; k++) {
      const GFNT_LineRun * run = &line->runs[k];
      Font * f = font_of(run->face);
      if (!f || run->glyph_count == 0) {
        if (!f) {
          fail("a line run in a face the gallery did not load");
        }
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
      gr.face = f->res;
      gr.size = s->run_size > 0 ? s->run_size : s->px;
      gr.hint_mode = s->hint_mode;
      gr.glyph_count = run->glyph_count;
      gr.glyphs = gl;
      const GSHSCENE_Resource * res = NULL;
      if (gshscene_store_intern_glyph_run(g_store, &gr, &res) != GSHSCENE_OK) {
        fail("intern glyph run");
      } else {
        fill_map(L, res, &at, GSHGEOM_FILL_NONZERO, s->paint);
      }
      free(gl);
    }
  }
  box.width = layout.width / 64.0;
  box.height = layout.height / 64.0;
  box.ok = 1;
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(para);
  return box;
}

/** A label in DejaVu Sans. */
static TextBox label(GSHSCENE_List * L, const char * text, double px, double x, double y, double grey) {
  TextStyle s = style(px, grey, grey, grey);
  return text_draw(L, &g_dejavu, 1, text, &s, x, y);
}

static TextBox label_c(GSHSCENE_List * L, const char * text, double px, double cx, double y, double grey) {
  /* Center by measuring first: lay it out in a throwaway list. */
  GSHSCENE_List * tmp = list_new();
  TextBox b = label(tmp, text, px, 0, 0, grey);
  gshscene_list_destroy(tmp);
  return label(L, text, px, floor(cx - b.width / 2), y, grey);
}

static void background(GSHSCENE_List * L, int w, int h, double v) {
  fill_rect(L, 0, 0, w, h, solid(v, v, v, 1.0));
}

/* --- 1: shapes ------------------------------------------------------------------------------------------------ */

static const GSHSCENE_Resource * star(double cx, double cy, double r) {
  GSHGEOM_Path * p = path_new();
  for (int i = 0; i < 5; i++) {
    double a = -M_PI / 2 + i * 4 * M_PI / 5;
    GSHGEOM_Point q = P(cx + r * cos(a), cy + r * sin(a));
    if (i == 0) {
      gshgeom_path_move_to(p, q);
    } else {
      gshgeom_path_line_to(p, q);
    }
  }
  gshgeom_path_close(p);
  return intern(p);
}

static const GSHSCENE_Resource * rounded_rect(double x0, double y0, double x1, double y1, double rad) {
  const double k = 0.5522847498307936 * rad;
  GSHGEOM_Path * p = path_new();
  gshgeom_path_move_to(p, P(x0 + rad, y0));
  gshgeom_path_line_to(p, P(x1 - rad, y0));
  gshgeom_path_cubic_to(p, P(x1 - rad + k, y0), P(x1, y0 + rad - k), P(x1, y0 + rad));
  gshgeom_path_line_to(p, P(x1, y1 - rad));
  gshgeom_path_cubic_to(p, P(x1, y1 - rad + k), P(x1 - rad + k, y1), P(x1 - rad, y1));
  gshgeom_path_line_to(p, P(x0 + rad, y1));
  gshgeom_path_cubic_to(p, P(x0 + rad - k, y1), P(x0, y1 - rad + k), P(x0, y1 - rad));
  gshgeom_path_line_to(p, P(x0, y0 + rad));
  gshgeom_path_cubic_to(p, P(x0, y0 + rad - k), P(x0 + rad - k, y0), P(x0 + rad, y0));
  gshgeom_path_close(p);
  return intern(p);
}

static const GSHSCENE_Resource * heart(double cx, double cy, double s) {
  GSHGEOM_Path * p = path_new();
#define H(x, y) P(cx + (x) * s, cy + (y) * s)
  gshgeom_path_move_to(p, H(0, 40));
  gshgeom_path_cubic_to(p, H(-12, 28), H(-60, 4), H(-60, -22));
  gshgeom_path_cubic_to(p, H(-60, -48), H(-28, -62), H(0, -34));
  gshgeom_path_cubic_to(p, H(28, -62), H(60, -48), H(60, -22));
  gshgeom_path_cubic_to(p, H(60, 4), H(12, 28), H(0, 40));
#undef H
  gshgeom_path_close(p);
  return intern(p);
}

/** Six petals of quadratic curves, with a round hole (a reversed circle) so nonzero leaves it open. */
static const GSHSCENE_Resource * flower(double cx, double cy, double r0, double r1) {
  GSHGEOM_Path * p = path_new();
  for (int i = 0; i <= 6; i++) {
    double a = i * M_PI / 3;
    GSHGEOM_Point q = P(cx + r0 * cos(a), cy + r0 * sin(a));
    if (i == 0) {
      gshgeom_path_move_to(p, q);
    } else {
      double c = a - M_PI / 6;
      gshgeom_path_quadratic_to(p, P(cx + r1 * cos(c), cy + r1 * sin(c)), q);
    }
  }
  gshgeom_path_close(p);
  /* the hole, wound the other way */
  double h = r0 * 0.35;
  static const double w[3] = {1.0, 0.70710678118654752440, 1.0};
  gshgeom_path_move_to(p, P(cx + h, cy));
  gshgeom_path_rational_quadratic_to(p, P(cx + h, cy - h), P(cx, cy - h), w);
  gshgeom_path_rational_quadratic_to(p, P(cx - h, cy - h), P(cx - h, cy), w);
  gshgeom_path_rational_quadratic_to(p, P(cx - h, cy + h), P(cx, cy + h), w);
  gshgeom_path_rational_quadratic_to(p, P(cx + h, cy + h), P(cx + h, cy), w);
  gshgeom_path_close(p);
  return intern(p);
}

static void image_shapes(void) {
  const int W = 1200, H = 800;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "shape-scene lists rendered by shape-cpu", 26, 30, 18, 0.1);

  /* Fill rules. */
  fill_map(L, star(150, 200, 110), NULL, GSHGEOM_FILL_NONZERO, solid(0.85, 0.2, 0.1, 1));
  fill_map(L, star(410, 200, 110), NULL, GSHGEOM_FILL_EVEN_ODD, solid(0.85, 0.2, 0.1, 1));
  label_c(L, "nonzero", 18, 150, 322, 0.2);
  label_c(L, "even-odd", 18, 410, 322, 0.2);

  /* Curves. */
  fill(L, rounded_rect(580, 110, 760, 290, 40), solid(0.2, 0.55, 0.35, 1));
  label_c(L, "cubic corners", 18, 670, 322, 0.2);
  fill(L, heart(880, 205, 1.45), solid(0.85, 0.15, 0.4, 1));
  label_c(L, "cubic heart", 18, 880, 322, 0.2);
  fill(L, flower(1070, 200, 70, 135), solid(0.95, 0.65, 0.1, 1));
  label_c(L, "quadratic petals, hole", 18, 1070, 322, 0.2);

  /* A clip: a circle clipping a pattern. */
  fill(L, circle(200, 590, 134), solid(0.85, 0.85, 0.88, 1));
  const GSHSCENE_Resource * clip = circle(200, 590, 130);
  if (gshscene_list_clip_push(L, clip, NULL, GSHGEOM_FILL_NONZERO, 0, 0.0) != GSHSCENE_OK) {
    fail("clip push");
  }
  gshscene_resource_release(clip);
  for (int i = -12; i <= 12; i++) {
    GSHGEOM_Map rot, mv, m;
    gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, 30.0, &rot);
    gshgeom_map_translate(2, 200 + i * 26.0, 590, 0, &mv);
    gshgeom_map_compose(&mv, &rot, &m);
    fill_map(L, rect(-6, -400, 6, 400), &m, GSHGEOM_FILL_NONZERO,
        i % 2 ? solid(0.1, 0.35, 0.8, 1) : solid(0.95, 0.55, 0.15, 1));
  }
  for (int gy = 0; gy < 7; gy++) {
    for (int gx = 0; gx < 7; gx++) {
      fill(L, circle(200 - 135 + gx * 45, 590 - 135 + gy * 45, 9), solid(1, 1, 1, 0.9));
    }
  }
  if (gshscene_list_clip_pop(L) != GSHSCENE_OK) {
    fail("clip pop");
  }
  label_c(L, "clip: a circle around stripes and dots", 18, 200, 735, 0.2);

  /* Group opacity versus per-fill alpha. */
  static const double rgb[3][3] = {{0.9, 0.1, 0.1}, {0.1, 0.7, 0.2}, {0.1, 0.3, 0.95}};
  static const double off[3][2] = {{0, -38}, {-42, 30}, {42, 30}};
  group_push(L, group_props(0.5, GSHSCENE_COMPOSITE_SRC_OVER));
  for (int i = 0; i < 3; i++) {
    fill(L, circle(620 + off[i][0], 590 + off[i][1], 75), solid(rgb[i][0], rgb[i][1], rgb[i][2], 1));
  }
  group_pop(L);
  for (int i = 0; i < 3; i++) {
    fill(L, circle(960 + off[i][0], 590 + off[i][1], 75), solid(rgb[i][0], rgb[i][1], rgb[i][2], 0.5));
  }
  label_c(L, "one group, opacity 0.5", 18, 620, 712, 0.2);
  label_c(L, "(opaque fills inside)", 15, 620, 738, 0.4);
  label_c(L, "no group, each fill alpha 0.5", 18, 960, 712, 0.2);
  label_c(L, "(overlaps show through)", 15, 960, 738, 0.4);
  label(L, "Gradients are on gradients.png, strokes on strokes.png.", 15, 30, 770, 0.45);
  render("shapes", L, R);
  save_png(R, "shapes.png");
  gimg_raster_destroy(R);
}

/* --- 2: compositing ------------------------------------------------------------------------------------------ */

static void image_compositing(void) {
  const int W = 1200, H = 800;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "Porter-Duff operators and PLUS: a group of the operator around the source", 24, 30, 16, 0.1);
  label(L, "Backdrop (dst): blue circle, alpha 0.9.  Source (src): red square, alpha 0.9.  Cells are isolated groups over a checkerboard we draw.",
      15, 30, 52, 0.35);
  const int cols = 5, cw = 220, ch = 230, x0 = 55, y0 = 90;
  const double bw = 200, bh = 180;
  /* One checkerboard path (the grey squares) for a cell, placed by a translation. */
  GSHGEOM_Path * cp = path_new();
  for (int y = 0; y < (int)(bh / 10); y++) {
    for (int x = 0; x < (int)(bw / 10); x++) {
      if ((x + y) % 2) {
        add_rect(cp, x * 10.0, y * 10.0, x * 10.0 + 10, y * 10.0 + 10);
      }
    }
  }
  const GSHSCENE_Resource * checker = intern(cp);
  /* The thirteen of N4: the blend modes have blend-modes.png. */
  for (int op = 0; op <= GSHSCENE_COMPOSITE_PLUS; op++) {
    int cx = x0 + (op % cols) * cw, cy = y0 + (op / cols) * ch;
    GSHGEOM_Map at;
    gshgeom_map_translate(2, cx, cy, 0, &at);
    fill_rect(L, cx, cy, cx + bw, cy + bh, solid(1, 1, 1, 1));
    fill_map(L, gshscene_resource_retain(checker), &at, GSHGEOM_FILL_NONZERO, solid(0.8, 0.8, 0.8, 1));
    group_push(L, group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER));
    fill(L, circle(cx + 80, cy + 80, 55), solid(0.1, 0.35, 0.9, 0.9));
    group_push(L, group_props(1.0, (uint32_t)op));
    fill_rect(L, cx + 85, cy + 75, cx + 175, cy + 165, solid(0.9, 0.2, 0.15, 0.9));
    group_pop(L);
    group_pop(L);
    label_c(L, gshscene_composite_string((GSHSCENE_Composite)op), 17, cx + bw / 2, cy + bh + 8, 0.15);
  }
  gshscene_resource_release(checker);
  render("compositing", L, R);
  save_png(R, "compositing.png");
  gimg_raster_destroy(R);
}

/* --- 3: color -------------------------------------------------------------------------------------------------- */

static GSHSCENE_Color_Space space(GCOL_Primaries p, GCOL_Transfer t) {
  GSHSCENE_Color_Space s;
  memset(&s, 0, sizeof s);
  s.primaries = p;
  s.transfer = t;
  return s;
}

static void blend_panel(GSHSCENE_List * L, double x, double y, GSHSCENE_Color_Space bs) {
  GSHSCENE_Group g = group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER);
  g.blend_space = bs;
  group_push(L, g);
  /* Three translucent fills. */
  static const double rgb[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  static const double off[3][2] = {{0, -40}, {-45, 32}, {45, 32}};
  fill_rect(L, x - 160, y - 140, x + 160, y + 120, solid(1, 1, 1, 1));
  for (int i = 0; i < 3; i++) {
    fill(L, circle(x + off[i][0], y + off[i][1], 78), solid(rgb[i][0], rgb[i][1], rgb[i][2], 0.5));
  }
  /* White at 25/50/75 % over black, and black over white. */
  fill_rect(L, x - 160, y + 140, x + 160, y + 190, solid(0, 0, 0, 1));
  fill_rect(L, x - 160, y + 190, x + 160, y + 240, solid(1, 1, 1, 1));
  for (int i = 0; i < 3; i++) {
    double a = 0.25 * (i + 1);
    fill_rect(L, x - 160 + i * 320 / 3.0, y + 140, x - 160 + (i + 1) * 320 / 3.0, y + 190, solid(1, 1, 1, a));
    fill_rect(L, x - 160 + i * 320 / 3.0, y + 190, x - 160 + (i + 1) * 320 / 3.0, y + 240, solid(0, 0, 0, a));
  }
  group_pop(L);
}

static void image_color(void) {
  const int W = 1200, H = 960;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.93);
  blend_panel(L, 300, 250, space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB));
  blend_panel(L, 900, 250, space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_LINEAR));

  /* Swatches: the same numbers in three source spaces, converted to the sRGB target. */
  static const double sw[7][3] = {
      {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.8, 0.5, 0.2}, {0.3, 0.6, 0.5}, {0.5, 0.5, 0.5}, {1, 1, 1}};
  const GSHSCENE_Color_Space src[3] = {space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB),
      space(GCOL_PRIMARIES_DISPLAY_P3, GCOL_TRANSFER_SRGB), space(GCOL_PRIMARIES_BT2020, GCOL_TRANSFER_SRGB)};
  const int sx0 = 250, sy0 = 620, sdx = 130, sdy = 100;
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 7; c++) {
      GSHSCENE_Paint p = solid(sw[c][0], sw[c][1], sw[c][2], 1);
      p.space = src[r];
      fill_rect(L, sx0 + c * sdx, sy0 + r * sdy, sx0 + c * sdx + 100, sy0 + r * sdy + 60, p);
    }
  }
  render("color (fills)", L, R);

  /* Labels, with the pixel values read back from the render. */
  L = list_new();
  label(L, "Group blend space: the same fills in an sRGB-encoded group and in a linear-light group (story 12)", 20, 30, 12,
      0.1);
  label_c(L, "blend space: sRGB (encoded)", 19, 300, 52, 0.15);
  label_c(L, "blend space: linear sRGB", 19, 900, 52, 0.15);
  char buf[256];
  for (int p = 0; p < 2; p++) {
    int x = p ? 900 : 300, y = 250;
    const unsigned char * c = pixel(R, x, y);
    const unsigned char * rg = pixel(R, x - 50, y - 15);
    snprintf(buf, sizeof buf, "centre #%02x%02x%02x   red+green #%02x%02x%02x", c[0], c[1], c[2], rg[0], rg[1], rg[2]);
    label_c(L, buf, 15, x, y - 166, 0.25);
    for (int i = 0; i < 3; i++) {
      const unsigned char * a = pixel(R, x - 160 + i * 107 + 53, y + 165);
      const unsigned char * b = pixel(R, x - 160 + i * 107 + 53, y + 215);
      snprintf(buf, sizeof buf, "%d%%", 25 * (i + 1));
      label_c(L, buf, 13, x - 160 + i * 107 + 53, y + 244, 0.35);
      snprintf(buf, sizeof buf, "%d / %d", a[0], b[0]);
      label_c(L, buf, 14, x - 160 + i * 107 + 53, y + 262, 0.2);
    }
  }
  label_c(L, "bars: white over black / black over white at 25, 50, 75 % alpha; numbers are the 8-bit values read back", 14, 600, 538, 0.35);
  label(L, "Solid paints in a source space, converted to the sRGB target (N3); the same numbers in each row", 20, 30, 568,
      0.1);
  static const char * rows[3] = {"sRGB", "Display P3", "BT.2020 (sRGB curve)"};
  static const char * cols[7] = {"1,0,0", "0,1,0", "0,0,1", ".8,.5,.2", ".3,.6,.5", ".5,.5,.5", "1,1,1"};
  for (int c = 0; c < 7; c++) {
    label_c(L, cols[c], 14, sx0 + c * sdx + 50, sy0 - 22, 0.3);
  }
  for (int r = 0; r < 3; r++) {
    label(L, rows[r], 17, 30, sy0 + r * sdy + 20, 0.15);
    for (int c = 0; c < 7; c++) {
      const unsigned char * px = pixel(R, sx0 + c * sdx + 50, sy0 + r * sdy + 30);
      snprintf(buf, sizeof buf, "#%02x%02x%02x", px[0], px[1], px[2]);
      label_c(L, buf, 13, sx0 + c * sdx + 50, sy0 + r * sdy + 64, 0.3);
    }
  }
  label(L, "Out-of-gamut results are clamped by the 8-bit write.", 14, 30, 928, 0.45);
  render("color (labels)", L, R);
  save_png(R, "color.png");
  gimg_raster_destroy(R);
}

/* --- 4: gradients --------------------------------------------------------------------------------------------- */

/** A gradient's content with the identity map, the target's interpolation space, straight, pad and no stops. */
static GSHSCENE_Gradient grad_new(GSHSCENE_Gradient_Kind kind) {
  GSHSCENE_Gradient g;
  memset(&g, 0, sizeof g);
  g.kind = (uint32_t)kind;
  gshgeom_map_identity(2, &g.map);
  return g;
}

static GSHSCENE_Gradient_Stop stop(double offset, double r, double g, double b, double a) {
  GSHSCENE_Gradient_Stop s;
  memset(&s, 0, sizeof s);
  s.offset = offset;
  s.color = solid(r, g, b, a);
  /* Stated as sRGB, so a gradient may name an interpolation space of its own (G3). */
  s.color.space = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB);
  return s;
}

/** The hue sweep a blend tile stands on: red at 0 degrees around to red again, a whole turn. */
static const GSHSCENE_Gradient_Stop * hue_stops(void) {
  static GSHSCENE_Gradient_Stop hue[7];
  static int made;
  if (!made) {
    static const double rgb[7][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}};
    for (int i = 0; i < 7; i++) {
      hue[i] = stop(i / 6.0, rgb[i][0], rgb[i][1], rgb[i][2], 1);
    }
    made = 1;
  }
  return hue;
}

/** Fill `res` (released) under `map` with the gradient `g` and its `n` stops, interned for this fill. */
static void fill_gradient(GSHSCENE_List * L, const GSHSCENE_Resource * res, const GSHGEOM_Map * map, GSHSCENE_Gradient g,
    const GSHSCENE_Gradient_Stop * stops, size_t n) {
  g.stops = stops;
  g.stop_count = n;
  const GSHSCENE_Resource * gr = NULL;
  GSHSCENE_Result r = gshscene_store_intern_gradient(g_store, &g, &gr);
  if (r != GSHSCENE_OK) {
    fprintf(stderr, "gallery: intern gradient: %s\n", gshscene_result_string(r));
    g_failed = 1;
    gshscene_resource_release(res);
    return;
  }
  GSHSCENE_Paint p;
  memset(&p, 0, sizeof p);
  p.kind = GSHSCENE_PAINT_GRADIENT;
  p.rgba[3] = 1.0;
  p.gradient = gr;
  fill_map(L, res, map, GSHGEOM_FILL_NONZERO, p);
  gshscene_resource_release(gr);
}

/** A light and dark grey checkerboard of `s`-pixel squares over the rectangle, to show what is translucent. */
static void checker(GSHSCENE_List * L, double x0, double y0, double x1, double y1, double s) {
  fill_rect(L, x0, y0, x1, y1, solid(1, 1, 1, 1));
  GSHGEOM_Path * p = path_new();
  for (int j = 0; y0 + j * s < y1; j++) {
    for (int i = j % 2; x0 + i * s < x1; i += 2) {
      add_rect(p, x0 + i * s, y0 + j * s, fmin(x0 + (i + 1) * s, x1), fmin(y0 + (j + 1) * s, y1));
    }
  }
  fill(L, intern(p), solid(0.78, 0.78, 0.78, 1));
}

/** A thin dark mark across a tile, where a gradient's geometry puts a point. */
static void tick(GSHSCENE_List * L, double x, double y0, double y1) {
  fill_rect(L, x - 1, y0, x + 1, y1, solid(0.1, 0.1, 0.1, 1));
}

/* --- 2b: blend modes (color-paint story P3) ------------------------------------------------------------------- */

/** A tile's backdrop: a hue sweep about its center, under a lightness ramp (white at the top to black at the bottom,
 *  each half transparent toward the middle). */
static void blend_backdrop(GSHSCENE_List * L, double x, double y, double w, double h) {
  GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_SWEEP);
  g.center[0] = x + w / 2;
  g.center[1] = y + h / 2;
  g.angle_unit = GSHGEOM_ANGLE_DEGREES;
  g.a0 = 0;
  g.a1 = 360;
  fill_gradient(L, rect(x, y, x + w, y + h), NULL, g, hue_stops(), 7);
  GSHSCENE_Gradient ramp = grad_new(GSHSCENE_GRADIENT_LINEAR);
  ramp.p0[0] = x;
  ramp.p0[1] = y;
  ramp.p1[0] = x;
  ramp.p1[1] = y + h;
  const GSHSCENE_Gradient_Stop light[3] = {stop(0, 1, 1, 1, 0.85), stop(0.5, 0.5, 0.5, 0.5, 0), stop(1, 0, 0, 0, 0.85)};
  fill_gradient(L, rect(x, y, x + w, y + h), NULL, ramp, light, 3);
}

/** A tile's source: saturated red, green and blue and a grey, at full alpha (top row) and half (bottom row). */
static void blend_source(GSHSCENE_List * L, double x, double y, double w, double h, const double (*colors)[3], int n) {
  double pw = (w - 40) / n, ph = (h - 30) / 2;
  for (int row = 0; row < 2; row++) {
    for (int i = 0; i < n; i++) {
      double px = x + 30 + i * pw, py = y + 15 + row * ph;
      fill_rect(L, px, py, px + pw - 3, py + ph - 3, solid(colors[i][0], colors[i][1], colors[i][2], row ? 0.5 : 1.0));
    }
  }
}

static const double kPatches[4][3] = {{1, 0, 0}, {0, 0.8, 0.1}, {0.1, 0.2, 1}, {0.5, 0.5, 0.5}};

/** One tile: the backdrop, then a group of blend mode `op` holding the source; `parent` the group the tile lives
 *  in (zero: the target's space). */
static void blend_tile(GSHSCENE_List * L, uint32_t op, double x, double y, double w, double h,
    const double (*colors)[3], int n, GSHSCENE_Color_Space parent) {
  GSHSCENE_Group outer = group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER);
  outer.blend_space = parent;
  group_push(L, outer);
  blend_backdrop(L, x, y, w, h);
  group_push(L, group_props(1.0, op));
  blend_source(L, x, y, w, h, colors, n);
  group_pop(L);
  group_pop(L);
}

/** The 8-bit values of the tile's middle pixel, read back, under it. */
static void tile_readback(GSHSCENE_List * L, GIMG_Raster * R, double x, double y, double w, double h) {
  const unsigned char * c = pixel(R, (int)(x + w / 2), (int)(y + h / 2));
  char buf[64];
  snprintf(buf, sizeof buf, "mid %d,%d,%d,%d", c[0], c[1], c[2], c[3]);
  label_c(L, buf, 12, x + w / 2, y + h + 22, 0.35);
}

static void image_blend_modes(void) {
  const int W = 1240, H = 1640;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.95);
  const double tw = 200, th = 110, dx = 240, dy = 158, x0 = 30;
  const GSHSCENE_Color_Space encoded = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB);
  const GSHSCENE_Color_Space linear = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_LINEAR);
  /* Two sections of the fifteen modes: blended in sRGB-encoded values, then in linear light (B5: the space of the
   * layer the group composites onto). */
  for (int sec = 0; sec < 2; sec++) {
    double y0 = 92 + sec * 520;
    for (int i = 0; i < 15; i++) {
      double x = x0 + (i % 5) * dx, y = y0 + (i / 5) * dy;
      blend_tile(L, (uint32_t)(GSHSCENE_COMPOSITE_SCREEN + i), x, y, tw, th, kPatches, 4, sec ? linear : encoded);
    }
  }
  /* Out of range (B10): sources with negative components and components above 1, never clamped before blending. */
  static const double wide[4][3] = {{1.5, -0.25, 0.2}, {-0.5, 1.25, 0.5}, {2.0, 2.0, 2.0}, {-0.5, -0.5, -0.5}};
  static const uint32_t oor[5] = {GSHSCENE_COMPOSITE_MULTIPLY, GSHSCENE_COMPOSITE_SCREEN, GSHSCENE_COMPOSITE_COLOR_DODGE,
      GSHSCENE_COMPOSITE_SOFT_LIGHT, GSHSCENE_COMPOSITE_LUMINOSITY};
  for (int i = 0; i < 5; i++) {
    blend_tile(L, oor[i], x0 + i * dx, 1158, tw, th, wide, 4, encoded);
  }
  /* Isolation (B6): the same multiply group drawn straight onto the backdrop, and inside an isolated group of its own,
   * where there is nothing beneath it to multiply; then PDF's two other kinds of group, which shape-cpu refuses. */
  const double iy = 1400;
  blend_backdrop(L, x0, iy, tw, th);
  group_push(L, group_props(1.0, GSHSCENE_COMPOSITE_MULTIPLY));
  blend_source(L, x0, iy, tw, th, kPatches, 4);
  group_pop(L);
  blend_backdrop(L, x0 + dx, iy, tw, th);
  group_push(L, group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER));
  group_push(L, group_props(1.0, GSHSCENE_COMPOSITE_MULTIPLY));
  blend_source(L, x0 + dx, iy, tw, th, kPatches, 4);
  group_pop(L);
  group_pop(L);
  for (int k = 0; k < 2; k++) {
    double x = x0 + (2 + k) * dx;
    fill_rect(L, x, iy, x + tw, iy + th, solid(0.82, 0.82, 0.82, 1));
    fill_rect(L, x + 3, iy + 3, x + tw - 3, iy + th - 3, solid(0.9, 0.9, 0.9, 1));
  }
  render("blend modes (tiles)", L, R);

  /* PDF's non-isolated and knockout groups (T1 to T7): drawn by no sink yet.  Each is recorded and rendered over its
   * placeholder, so the log shows shape-cpu's refusal (S19) and the placeholder shows it drew nothing. */
  for (int k = 0; k < 2; k++) {
    L = list_new();
    double x = x0 + (2 + k) * dx;
    GSHSCENE_Group g = group_props(1.0, GSHSCENE_COMPOSITE_MULTIPLY);
    if (k == 0) {
      g.isolation = GSHSCENE_ISOLATION_NON_ISOLATED;
    } else {
      g.knockout = 1;
    }
    group_push(L, g);
    blend_source(L, x, iy, tw, th, kPatches, 4);
    group_pop(L);
    render(k == 0 ? "blend modes (a non-isolated group, refused)" : "blend modes (a knockout group, refused)", L, R);
  }

  /* Labels, and each tile's middle pixel read back. */
  L = list_new();
  static const char * names[15] = {"screen", "overlay", "darken", "lighten", "color-dodge", "color-burn", "hard-light",
      "soft-light", "difference", "exclusion", "multiply", "hue", "saturation", "color", "luminosity"};
  label(L, "Blend modes 13 to 27 (W3C Compositing and Blending Level 1, numbered as COLRv1's): each a group around the source",
      19, 30, 12, 0.1);
  label(L, "Backdrop: a hue sweep under a lightness ramp.  Source: red, green, blue and grey, full alpha (top) and half (bottom).",
      14, 30, 40, 0.35);
  for (int sec = 0; sec < 2; sec++) {
    double y0 = 92 + sec * 520;
    label(L, sec ? "Blended in linear light (the parent group is linear sRGB)" : "Blended in sRGB-encoded values (the parent is the sRGB target)",
        16, 30, y0 - 26, 0.15);
    for (int i = 0; i < 15; i++) {
      double x = x0 + (i % 5) * dx, y = y0 + (i / 5) * dy;
      char buf[64];
      snprintf(buf, sizeof buf, "%d %s", 13 + i, names[i]);
      label_c(L, buf, 14, x + tw / 2, y + th + 4, 0.15);
      tile_readback(L, R, x, y, tw, th);
    }
  }
  label(L, "Out of range (B10): sources (1.5, -0.25, 0.2), (-0.5, 1.25, 0.5), (2, 2, 2) and (-0.5, -0.5, -0.5), blended literally",
      16, 30, 1132, 0.15);
  for (int i = 0; i < 5; i++) {
    double x = x0 + i * dx;
    label_c(L, gshscene_composite_string((GSHSCENE_Composite)oor[i]), 14, x + tw / 2, 1158 + th + 4, 0.15);
    tile_readback(L, R, x, 1158, tw, th);
  }
  label(L, "Isolation (B6): multiply at the root, and inside an isolated group of its own; PDF's other groups are refused (S19)",
      16, 30, iy - 26, 0.15);
  label_c(L, "multiply onto the backdrop", 14, x0 + tw / 2, iy + th + 4, 0.15);
  label_c(L, "inside an isolated group", 14, x0 + dx + tw / 2, iy + th + 4, 0.15);
  tile_readback(L, R, x0, iy, tw, th);
  tile_readback(L, R, x0 + dx, iy, tw, th);
  label_c(L, "non-isolated group:", 14, x0 + 2 * dx + tw / 2, iy + 34, 0.3);
  label_c(L, "refused by shape-cpu", 14, x0 + 2 * dx + tw / 2, iy + 56, 0.3);
  label_c(L, "knockout group:", 14, x0 + 3 * dx + tw / 2, iy + 34, 0.3);
  label_c(L, "refused by shape-cpu", 14, x0 + 3 * dx + tw / 2, iy + 56, 0.3);
  render("blend modes (labels)", L, R);
  save_png(R, "blend-modes.png");
  gimg_raster_destroy(R);
}

static void image_gradients(void) {
  const int W = 1200, H = 1320;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.95);
  static const char * ext_name[3] = {"pad", "repeat", "reflect"};
  static const GSHSCENE_Extend ext[3] = {GSHSCENE_EXTEND_PAD, GSHSCENE_EXTEND_REPEAT, GSHSCENE_EXTEND_REFLECT};

  /* Linear: the color line runs from p0 to p1, the middle third of each tile (the marks). */
  const GSHSCENE_Gradient_Stop lin[3] = {
      stop(0, 0.10, 0.25, 0.75, 1), stop(0.5, 0.98, 0.98, 0.95, 1), stop(1, 0.95, 0.45, 0.05, 1)};
  for (int k = 0; k < 3; k++) {
    double x0 = 40 + k * 385, y0 = 110, x1 = x0 + 345, y1 = 230;
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_LINEAR);
    g.p0[0] = x0 + 115;
    g.p0[1] = y0;
    g.p1[0] = x0 + 230;
    g.p1[1] = y0;
    g.extend = (uint32_t)ext[k];
    fill_gradient(L, rect(x0, y0, x1, y1), NULL, g, lin, 3);
    tick(L, x0 + 115, y1, y1 + 10);
    tick(L, x0 + 230, y1, y1 + 10);
  }

  /* Radial: concentric pad and repeat, a focal point inside the end circle, and the cone when neither circle holds
   * the other. */
  const GSHSCENE_Gradient_Stop rad[3] = {
      stop(0, 1.0, 0.95, 0.55, 1), stop(0.6, 0.90, 0.30, 0.15, 1), stop(1, 0.30, 0.05, 0.35, 1)};
  for (int k = 0; k < 4; k++) {
    double cx = 140 + k * 307, cy = 400, h = 115;
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_RADIAL);
    g.c0[0] = g.c1[0] = cx;
    g.c0[1] = g.c1[1] = cy;
    g.r1 = 100;
    if (k == 1) {
      g.r1 = 34;
      g.extend = GSHSCENE_EXTEND_REPEAT;
    } else if (k == 2) {
      g.c0[0] = cx + 45;
      g.c0[1] = cy - 35;
    } else if (k == 3) {
      checker(L, cx - h, cy - h, cx + h, cy + h, 15);
      g.c0[0] = cx - 75;
      g.r0 = 12;
      g.c1[0] = cx + 30;
      g.r1 = 45;
    }
    fill_gradient(L, rect(cx - h, cy - h, cx + h, cy + h), NULL, g, rad, 3);
  }

  /* Sweep: angles from +x toward +y, which on this y-down page is clockwise. */
  const GSHSCENE_Gradient_Stop hue[7] = {stop(0, 1, 0, 0, 1), stop(1 / 6.0, 1, 1, 0, 1), stop(2 / 6.0, 0, 1, 0, 1),
      stop(3 / 6.0, 0, 1, 1, 1), stop(4 / 6.0, 0, 0, 1, 1), stop(5 / 6.0, 1, 0, 1, 1), stop(1, 1, 0, 0, 1)};
  const GSHSCENE_Gradient_Stop two[2] = {stop(0, 0.15, 0.15, 0.2, 1), stop(1, 0.55, 0.85, 1.0, 1)};
  for (int k = 0; k < 3; k++) {
    double cx = 200 + k * 400, cy = 690;
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_SWEEP);
    g.center[0] = cx;
    g.center[1] = cy;
    g.angle_unit = GSHGEOM_ANGLE_DEGREES;
    g.a0 = 0;
    g.a1 = 360;
    if (k == 0) {
      fill_gradient(L, circle(cx, cy, 105), NULL, g, hue, 7);
    } else {
      g.a0 = 30;
      g.a1 = 120;
      g.extend = (uint32_t)(k == 1 ? GSHSCENE_EXTEND_PAD : GSHSCENE_EXTEND_REPEAT);
      fill_gradient(L, circle(cx, cy, 105), NULL, g, two, 2);
    }
  }

  /* Interpolation spaces and alpha: red to green in sRGB-encoded values and in linear light; red to a nearly
   * transparent blue, straight and premultiplied, over a checkerboard. */
  const GSHSCENE_Gradient_Stop rg[2] = {stop(0, 1, 0, 0, 1), stop(1, 0, 1, 0, 1)};
  const GSHSCENE_Gradient_Stop rb[2] = {stop(0, 1, 0, 0, 1), stop(1, 0, 0, 1, 0.1)};
  for (int k = 0; k < 4; k++) {
    double x0 = 300, x1 = 1160, y0 = 875 + k * 52, y1 = y0 + 40;
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_LINEAR);
    g.p0[0] = x0;
    g.p0[1] = y0;
    g.p1[0] = x1;
    g.p1[1] = y0;
    if (k < 2) {
      g.space = space(GCOL_PRIMARIES_SRGB, k == 0 ? GCOL_TRANSFER_SRGB : GCOL_TRANSFER_LINEAR);
      fill_gradient(L, rect(x0, y0, x1, y1), NULL, g, rg, 2);
    } else {
      checker(L, x0, y0, x1, y1, 10);
      g.space = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB);
      g.premultiplied = k == 3;
      fill_gradient(L, rect(x0, y0, x1, y1), NULL, g, rb, 2);
    }
  }

  /* Under a transform: the item's map turns the path and its gradient together; the gradient's own map skews or
   * stretches only the gradient. */
  {
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_LINEAR);
    g.p0[0] = -20;
    g.p1[0] = 20;
    g.extend = GSHSCENE_EXTEND_REFLECT;
    GSHGEOM_Map rot, mv, m;
    gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, 30.0, &rot);
    gshgeom_map_translate(2, 200, 1200, 0, &mv);
    gshgeom_map_compose(&mv, &rot, &m);
    fill_gradient(L, rect(-110, -40, 110, 40), &m, g, lin, 3);

    GSHSCENE_Gradient s = grad_new(GSHSCENE_GRADIENT_RADIAL);
    s.c0[0] = s.c1[0] = 0;
    s.r1 = 30;
    s.extend = GSHSCENE_EXTEND_REPEAT;
    const double skew[4] = {1.0, 0.7, 0.0, 1.0}, at[2] = {600, 1200};
    gshgeom_map_from_matrix(2, skew, at, &s.map);
    fill_gradient(L, rect(470, 1110, 730, 1290), NULL, s, rad, 3);

    GSHSCENE_Gradient w = grad_new(GSHSCENE_GRADIENT_SWEEP);
    w.angle_unit = GSHGEOM_ANGLE_DEGREES;
    w.a1 = 360;
    const double stretch[4] = {2.2, 0.0, 0.0, 1.0}, at2[2] = {1000, 1200};
    gshgeom_map_from_matrix(2, stretch, at2, &w.map);
    fill_gradient(L, rect(870, 1110, 1130, 1290), NULL, w, hue, 7);
  }
  render("gradients (fills)", L, R);

  /* Labels, with the interpolation bars' middle pixels read back. */
  L = list_new();
  label(L, "Gradients drawn by shape-cpu: linear, two-circle radial and sweep", 26, 30, 18, 0.1);
  label(L, "Linear: blue, white, orange from p0 to p1 (the marks below each bar); outside them the extend decides", 17, 30, 70,
      0.2);
  for (int k = 0; k < 3; k++) {
    label_c(L, ext_name[k], 17, 40 + k * 385 + 172, 246, 0.2);
  }
  label(L, "Radial: yellow, red, purple from the start circle to the end circle", 17, 30, 262, 0.2);
  static const char * rad_name[4] = {
      "concentric, pad", "concentric, repeat", "focal point, pad", "cone: neither circle holds the other"};
  for (int k = 0; k < 4; k++) {
    label_c(L, rad_name[k], 15, 140 + k * 307, 522, 0.2);
  }
  label(L, "Sweep: angles from +x toward +y, clockwise on this y-down page", 17, 30, 552, 0.2);
  label(L, "0\xC2\xB0", 14, 312, 680, 0.2);
  label_c(L, "90\xC2\xB0", 14, 200, 798, 0.2);
  static const char * sweep_name[3] = {"red to red, a whole turn", "30\xC2\xB0 to 120\xC2\xB0, pad", "30\xC2\xB0 to 120\xC2\xB0, repeat"};
  for (int k = 0; k < 3; k++) {
    label_c(L, sweep_name[k], 15, 200 + k * 400, 820, 0.2);
  }
  label(L, "Interpolation: the same two stops, in other spaces and alpha forms; the middle pixel read back", 17, 30, 845, 0.2);
  static const char * interp_name[4] = {"red to green, sRGB", "red to green, linear sRGB", "to blue at alpha 0.1, straight",
      "to blue at alpha 0.1, premultiplied"};
  char buf[128];
  for (int k = 0; k < 4; k++) {
    int y = 875 + k * 52;
    label(L, interp_name[k], 13, 30, y + 2, 0.15);
    const unsigned char * px = pixel(R, 730, y + 20);
    snprintf(buf, sizeof buf, "middle #%02x%02x%02x", px[0], px[1], px[2]);
    label(L, buf, 13, 30, y + 21, 0.4);
  }
  label(L, "Under a transform", 17, 30, 1082, 0.2);
  label_c(L, "item map: rotated 30\xC2\xB0 (reflect)", 14, 200, 1295, 0.2);
  label_c(L, "gradient map: skewed rings", 14, 600, 1295, 0.2);
  label_c(L, "gradient map: sweep stretched 2.2x", 14, 1000, 1295, 0.2);
  render("gradients (labels)", L, R);
  save_png(R, "gradients.png");
  gimg_raster_destroy(R);
}

/* --- 5: text --------------------------------------------------------------------------------------------------- */

static uint32_t utf8_next(const char ** s) {
  const unsigned char * p = (const unsigned char *)*s;
  uint32_t c = *p++;
  int n = 0;
  if (c >= 0xF0) {
    c &= 0x07;
    n = 3;
  } else if (c >= 0xE0) {
    c &= 0x0F;
    n = 2;
  } else if (c >= 0xC0) {
    c &= 0x1F;
    n = 1;
  }
  while (n-- > 0 && (*p & 0xC0) == 0x80) {
    c = (c << 6) | (*p++ & 0x3F);
  }
  *s = (const char *)p;
  return c;
}

static void utf8_put(char ** s, uint32_t c) {
  unsigned char * p = (unsigned char *)*s;
  if (c < 0x80) {
    *p++ = (unsigned char)c;
  } else if (c < 0x800) {
    *p++ = (unsigned char)(0xC0 | (c >> 6));
    *p++ = (unsigned char)(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    *p++ = (unsigned char)(0xE0 | (c >> 12));
    *p++ = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
    *p++ = (unsigned char)(0x80 | (c & 0x3F));
  } else {
    *p++ = (unsigned char)(0xF0 | (c >> 18));
    *p++ = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
    *p++ = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
    *p++ = (unsigned char)(0x80 | (c & 0x3F));
  }
  *s = (char *)p;
}

/** How many letters of `text` (not spaces or ASCII punctuation) the face has no glyph for. */
static int missing(const Font * f, const char * text, int * total) {
  int miss = 0;
  *total = 0;
  while (*text) {
    uint32_t c = utf8_next(&text);
    if (c <= 0x40 || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7F) || c == 0x060C) {
      continue;
    }
    uint32_t g = 0;
    (*total)++;
    if (gfnt_face_glyph_for_codepoint(f->face, c, &g, NULL) != GFNT_OK || g == 0) {
      miss++;
    }
  }
  return miss;
}

/** The last zoomed word's source pixels, so two routes' renders can be compared byte for byte. */
static unsigned char g_zoom_last[120 * 20 * 4];

/** Render `word` small into its own raster with the given glyph-run size and hint mode, then draw every pixel as a
 *  `zoom`-pixel square into `L` (nearest neighbor, drawn as fills).  Prints the route the planning pass chose for the
 *  first glyph run and writes it to `route_out`; keeps the small render in g_zoom_last. */
static void zoomed_word(GSHSCENE_List * L, const char * word, double px, double run_size, uint32_t hint, double x, double y,
    int zoom, char * route_out, size_t route_len) {
  const int w = 120, h = 20;
  GIMG_Raster * small = raster_new(w, h);
  GSHSCENE_List * sl = list_new();
  background(sl, w, h, 1.0);
  TextStyle s = style(px, 0, 0, 0);
  s.run_size = run_size;
  s.hint_mode = hint;
  text_draw(sl, &g_dejavu, 1, word, &s, 3, 1);
  /* Route of the first glyph run, as the planning pass decides it. */
  const GSHSCENE_Item * item = NULL;
  snprintf(route_out, route_len, "?");
  for (size_t i = 0; i < gshscene_list_count(sl); i++) {
    gshscene_list_item(sl, i, &item);
    GSHSCENE_Glyph_Run gr;
    if (item->resource && gshscene_resource_glyph_run(item->resource, &gr) == GSHSCENE_OK) {
      GSHSCENE_Glyph_Route rt;
      if (gshscene_glyph_route(item->resource, &item->map, GSHSCENE_TARGET_RASTER, &rt) == GSHSCENE_OK) {
        snprintf(route_out, route_len, "%s%s%s", rt.route == GSHSCENE_GLYPH_GENERAL ? "general route" : "fast tier",
            rt.reason ? ", " : "", rt.reason ? gshscene_reason_string((GSHSCENE_Reason)rt.reason) : "");
      }
      break;
    }
  }
  printf("zoom: size %.12g hint %u -> %s\n", run_size, hint, route_out);
  render("zoom source", sl, small);
  fill_rect(L, x - 1, y - 1, x + w * zoom + 1, y + h * zoom + 1, solid(0.6, 0.6, 0.6, 1));
  fill_rect(L, x, y, x + w * zoom, y + h * zoom, solid(1, 1, 1, 1));
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      const unsigned char * p = pixel(small, xx, yy);
      if (p[0] == 255 && p[1] == 255 && p[2] == 255) {
        continue;
      }
      fill_rect(L, x + xx * zoom, y + yy * zoom, x + (xx + 1) * zoom, y + (yy + 1) * zoom,
          solid(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, 1));
    }
  }
  for (int yy = 0; yy < h; yy++) {
    memcpy(g_zoom_last + (size_t)yy * w * 4, pixel(small, 0, yy), (size_t)w * 4);
  }
  gimg_raster_destroy(small);
}

/** Diagnose one fixture font: per mapped glyph, font's own rasteriser and the scene at 40 px by each route. */
static void diag_font(const char * fontdir, const char * file) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", fontdir, file);
  Font * f = font_load(path, file);
  if (!f) {
    return;
  }
  for (uint32_t c = 'A'; c <= 'E'; c++) {
    uint32_t g = 0;
    if (gfnt_face_glyph_for_codepoint(f->face, c, &g, NULL) != GFNT_OK || g == 0) {
      continue;
    }
    for (int mode = 0; mode < 2; mode++) {
      GFNT_RasterOptions ro;
      memset(&ro, 0, sizeof ro);
      ro.hint_mode = mode ? GFNT_HINT_MODE_NONE : 0;
      GFNT_Coverage cov;
      memset(&cov, 0, sizeof cov);
      GFNT_Error err;
      gfnt_error_clear(&err);
      GFNT_Result r = gfnt_face_render_glyph(f->face, g, 40, &ro, NULL, &cov, &err);
      long ink = 0;
      if (r == GFNT_OK) {
        for (uint32_t row = 0; row < cov.height; row++) {
          for (uint32_t col = 0; col < cov.width; col++) {
            ink += cov.data[row * cov.stride + col];
          }
        }
        gfnt_coverage_destroy(&cov);
      } else {
        printf("diag %s '%c' gid %u: gfnt_face_render_glyph(%s) failed %d\n", file, c, g, mode ? "NONE" : "font", (int)r);
        gfnt_error_dump(&err, stdout);
      }
      printf("diag %s '%c' gid %u: font raster hint=%s result %d ink %ld\n", file, c, g, mode ? "NONE" : "font", (int)r,
          ink);
    }
    static const struct {
      const char * name;
      double size;
      uint32_t hint;
    } routes[3] = {{"fast FONT", 40, GSHSCENE_HINT_FONT}, {"fast NONE", 40, GSHSCENE_HINT_NONE},
        {"general", 40.000000001, GSHSCENE_HINT_FONT}};
    for (int k = 0; k < 3; k++) {
      GIMG_Raster * R = raster_new(80, 80);
      GSHSCENE_List * L = list_new();
      GSHSCENE_Glyph gl = {g, 20, 60, NULL};
      GSHSCENE_Glyph_Run gr;
      memset(&gr, 0, sizeof gr);
      gr.source = GSHSCENE_GLYPHS_FROM_FONT;
      gr.face = f->res;
      gr.size = routes[k].size;
      gr.hint_mode = routes[k].hint;
      gr.glyph_count = 1;
      gr.glyphs = &gl;
      const GSHSCENE_Resource * res = NULL;
      if (gshscene_store_intern_glyph_run(g_store, &gr, &res) == GSHSCENE_OK) {
        fill(L, res, solid(0, 0, 0, 1));
      }
      char what[160];
      snprintf(what, sizeof what, "diag %s '%c' gid %u scene %s", file, c, g, routes[k].name);
      render(what, L, R);
      long ink = 0;
      for (int y = 0; y < 80; y++) {
        for (int x = 0; x < 80; x++) {
          ink += pixel(R, x, y)[3];
        }
      }
      printf("  ink(alpha sum) %ld\n", ink);
      gimg_raster_destroy(R);
    }
  }
}

static void image_text(const char * fontdir) {
  diag_font(fontdir, "outline-cubic.ttf");
  diag_font(fontdir, "outline-cubic-flag.ttf");
  diag_font(fontdir, "outline-simple.ttf");
  diag_font(fontdir, "cff2-fds.otf");
  diag_font(fontdir, "variable-gvar.ttf");
  const int W = 1800, H = 1800;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.98);
  label(L, "Text: font's paragraph layout and shaping, glyphs through font's glyph path provider, drawn by shape-cpu", 24,
      30, 16, 0.1);
  label(L, "DejaVu Sans 2.37 (the success scene's pinned font), hinted where the run is on the fast tier", 16, 30, 50, 0.4);

  /* Pangram sizes. */
  static const int sizes[8] = {8, 10, 12, 16, 24, 36, 48, 72};
  double y = 90;
  for (int i = 0; i < 8; i++) {
    char b[16];
    snprintf(b, sizeof b, "%d px", sizes[i]);
    TextStyle s = style(sizes[i], 0.05, 0.05, 0.1);
    TextBox tb = text_draw(L, &g_dejavu, 1, "Sphinx of black quartz, judge my vow.", &s, 110, y);
    label(L, b, 14, 30, y + floor(tb.height / 2) - 8, 0.45);
    y += floor(tb.height) + 6;
  }

  /* A justified paragraph. */
  y += 30;
  const double py = y;
  label(L, "A paragraph broken into lines and justified", 17, 30, y, 0.15);
  label(L, "gfnt_paragraph_layout, align JUSTIFY, 17 px, 700 px column (grey guides)", 14, 30, y + 22, 0.4);
  y += 34;
  static const char * para =
      "Every pixel in this gallery was produced by the suite's own libraries. The text is shaped by the font library, "
      "which also breaks the paragraph into lines and widens the spaces of each line but the last so that it ends on the "
      "right edge of its column. Each line becomes one glyph run in a shape-scene list, named by a face resource whose "
      "glyph path provider is the font library itself; shape-cpu asks it for hinted device paths on the fast tier and for "
      "unhinted outlines on the general route, and the image library writes the PNG. Office affluent fjords waffle "
      "efficiently.";
  TextStyle ps = style(17, 0.08, 0.08, 0.12);
  ps.align = GFNT_ALIGN_JUSTIFY;
  ps.width = 700;
  y += 16;
  TextBox pb = text_draw(L, &g_dejavu, 1, para, &ps, 30, y);
  fill_rect(L, 29, y - 4, 30, y + pb.height + 4, solid(0.75, 0.75, 0.75, 1));
  fill_rect(L, 730, y - 4, 731, y + pb.height + 4, solid(0.75, 0.75, 0.75, 1));
  y += pb.height + 30;

  /* Zoomed comparison. */
  double zx = 800, zy = py;
  label(L, "Zoom x6 of \"Hamburgefonts\" at 12 px", 17, zx, zy, 0.15);
  label(L, "nearest neighbour: each source pixel drawn as a filled square", 14, zx, zy + 22, 0.4);
  zy += 50;
  char route[128], cap[256];
  zoomed_word(L, "Hamburgefonts", 12, 12, GSHSCENE_HINT_FONT, zx, zy + 22, 6, route, sizeof route);
  snprintf(cap, sizeof cap, "hint FONT, size 12: %s", route);
  label(L, cap, 15, zx, zy, 0.25);
  zy += 22 + 120 + 18;
  zoomed_word(L, "Hamburgefonts", 12, 12, GSHSCENE_HINT_NONE, zx, zy + 22, 6, route, sizeof route);
  static unsigned char none_px[sizeof g_zoom_last];
  memcpy(none_px, g_zoom_last, sizeof none_px);
  snprintf(cap, sizeof cap, "hint NONE, size 12: %s", route);
  label(L, cap, 15, zx, zy, 0.25);
  zy += 22 + 120 + 18;
  zoomed_word(L, "Hamburgefonts", 12, 12.000000001, GSHSCENE_HINT_FONT, zx, zy + 22, 6, route, sizeof route);
  {
    int diff = 0, maxd = 0;
    for (size_t k = 0; k < sizeof none_px; k++) {
      int d = abs((int)none_px[k] - (int)g_zoom_last[k]);
      diff += d != 0;
      maxd = d > maxd ? d : maxd;
    }
    printf("zoom: fast-unhinted vs general: %d bytes differ, max difference %d\n", diff, maxd);
  }
  snprintf(cap, sizeof cap, "hint FONT, size 12.000000001 (not a whole ppem): %s", route);
  label(L, cap, 15, zx, zy, 0.25);
  zy += 22 + 120 + 18;
  if (zy > y) {
    y = zy;
  }

  /* Scripts. */
  y += 10;
  label(L, "Other scripts in DejaVu Sans (bidi AUTO; Arabic is shaped by the font's GSUB/GPOS)", 17, 30, y, 0.15);
  y += 34;
  static const char * scripts[][2] = {
      {"Greek", "Ταχίστη αλώπηξ βαφής ψημένη γη, δρασκελίζει υπέρ νωθρού κυνός"},
      {"Cyrillic", "Съешь же ещё этих мягких французских булок, да выпей чаю"},
      {"Arabic", "نص حكيم له سر قاطع وذو شأن عظيم مكتوب على ثوب أخضر"},
      {"Hebrew", "דג סקרן שט בים מאוכזב ולפתע מצא חברה"},
      {"Armenian", "Բարեւ աշխարհ, ես սիրում եմ գրքեր"},
      {"Georgian", "გამარჯობა მსოფლიო"},
      {"Devanagari", "ऋषियों को सताने वाले दुष्ट राक्षसों के राजा"},
      {"Thai", "เป็นมนุษย์สุดประเสริฐเลิศคุณค่า"},
      {"Han", "天地玄黄宇宙洪荒"},
  };
  char notcov[512] = "";
  for (size_t i = 0; i < sizeof scripts / sizeof scripts[0]; i++) {
    int total = 0, miss = missing(g_dejavu, scripts[i][1], &total);
    printf("script %s: %d of %d letters missing\n", scripts[i][0], miss, total);
    if (miss * 2 > total) {
      size_t n = strlen(notcov);
      snprintf(notcov + n, sizeof notcov - n, "%s%s (%d/%d missing)", n ? ", " : "", scripts[i][0], miss, total);
      continue;
    }
    label(L, scripts[i][0], 14, 30, y + 10, 0.45);
    TextStyle s = style(28, 0.05, 0.05, 0.1);
    s.dir = GFNT_BIDI_AUTO;
    TextBox tb = text_draw(L, &g_dejavu, 1, scripts[i][1], &s, 140, y);
    y += floor(tb.height) + 6;
  }
  if (notcov[0]) {
    char b[700];
    snprintf(b, sizeof b, "Not drawn, DejaVu Sans has no glyphs for: %s", notcov);
    label(L, b, 15, 30, y + 4, 0.45);
    y += 30;
  }

  /* Fixture fonts: whatever each maps from a few ranges. */
  y += 16;
  label(L, "font's committed test fixtures (synthetic fonts): the characters each maps, at 40 px", 17, 30, y, 0.15);
  y += 34;
  static const char * fixtures[] = {"basic.ttf", "outline-simple.ttf", "outline-cubic.ttf", "outline-composite.ttf",
      "cff.otf", "cff-curves.otf", "cff2-fds.otf", "variable-gvar.ttf", "layout-gsub.ttf", "layout-gpos.ttf",
      "arabic-join.ttf", "vertical-text.ttf"};
  double fx = 30, fy = y;
  for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", fontdir, fixtures[i]);
    Font * f = font_load(path, fixtures[i]);
    char sample[256];
    char * o = sample;
    int count = 0;
    if (f) {
      static const uint32_t ranges[][2] = {{0x0621, 0x064A}, {0x21, 0x7E}, {0x391, 0x3C9}, {0x3041, 0x30FF}};
      for (size_t rr = 0; rr < 4 && count < 18; rr++) {
        for (uint32_t c = ranges[rr][0]; c <= ranges[rr][1] && count < 18; c++) {
          uint32_t g = 0;
          if (gfnt_face_glyph_for_codepoint(f->face, c, &g, NULL) == GFNT_OK && g != 0) {
            utf8_put(&o, c);
            count++;
          }
        }
        if (count && rr == 0) {
          break; /* an Arabic font: Arabic only, so it joins */
        }
      }
    }
    *o = 0;
    label(L, fixtures[i], 14, fx, fy + 12, 0.4);
    if (!f) {
      label(L, "(load failed)", 15, fx + 190, fy + 10, 0.6);
    } else if (!count) {
      label(L, "(maps nothing in the sampled ranges)", 15, fx + 190, fy + 10, 0.6);
    } else {
      uint32_t g0 = 0;
      const char * sp = sample;
      gfnt_face_glyph_for_codepoint(f->face, utf8_next(&sp), &g0, NULL);
      GFNT_RasterOptions ro;
      memset(&ro, 0, sizeof ro);
      GFNT_Coverage cov;
      memset(&cov, 0, sizeof cov);
      if (gfnt_face_render_glyph(f->face, g0, 40, &ro, NULL, &cov, NULL) == GFNT_ERR_UNSUPPORTED) {
        label(L, "(refused by font: glyphDataFormat 1, cubic glyf)", 14, fx + 190, fy + 12, 0.5);
        fx = i % 2 == 0 ? 920 : 30;
        fy += i % 2 == 0 ? 0 : 62;
        continue;
      }
      gfnt_coverage_destroy(&cov);
      TextStyle s = style(40, 0.1, 0.1, 0.25);
      s.dir = GFNT_BIDI_AUTO;
      TextBox tb = text_draw(L, &f, 1, sample, &s, fx + 190, fy);
      if (!tb.ok) {
        label(L, "(layout failed)", 15, fx + 190, fy + 10, 0.6);
      }
      printf("fixture %s: %d chars \"%s\"\n", fixtures[i], count, sample);
    }
    if (i % 2 == 0) {
      fx = 920;
    } else {
      fx = 30;
      fy += 62;
    }
  }
  render("text", L, R);
  save_png(R, "text.png");
  gimg_raster_destroy(R);
}

/* --- 6: color glyphs --------------------------------------------------------------------------------------------- */

/* A refused glyph is a placeholder item in the list, which the render report names with its (source, code) pair; the
 * page draws what an application would draw for one, from the item's own box (the library draws nothing). */

/** The item's box under its map in list space: the corners' bounds, or the cell when the item states none. */
static void placeholder_box(const GSHSCENE_Item * it, double cell[4], double out[4]) {
  if (!it->placeholder.has_box) {
    memcpy(out, cell, 4 * sizeof out[0]);
    return;
  }
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (int k = 0; k < 4; k++) {
    GSHGEOM_Point p = {it->placeholder.box[(k & 1) ? 2 : 0], it->placeholder.box[(k & 2) ? 3 : 1], 0.0}, q;
    if (gshgeom_map_apply_point(&it->map, p, &q) != GSHGEOM_OK) {
      memcpy(out, cell, 4 * sizeof out[0]);
      return;
    }
    x0 = q.x < x0 ? q.x : x0;
    x1 = q.x > x1 ? q.x : x1;
    y0 = q.y < y0 ? q.y : y0;
    y1 = q.y > y1 ? q.y : y1;
  }
  out[0] = x0;
  out[1] = y0;
  out[2] = x1;
  out[3] = y1;
}

static void image_color_glyphs(const char * fontdir) {
  static const char * files[] = {"color-v0.ttf", "color-v0-edges.ttf", "color-cpal0.ttf", "color-v1.ttf",
      "color-v1-scene.ttf", "color-v1-var.ttf", "color-v1-varmap.ttf", "color-v1-varnone.ttf"};
  enum { NF = sizeof files / sizeof files[0] };
  Font * fonts[NF];
  size_t nglyphs[NF];
  int ncolor = 0;
  /* First pass: count the glyphs with a color description, to size the image. */
  for (int i = 0; i < NF; i++) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", fontdir, files[i]);
    fonts[i] = font_load(path, files[i]);
    nglyphs[i] = 0;
    if (fonts[i]) {
      gfnt_face_num_glyphs(fonts[i]->face, &nglyphs[i], NULL);
      GSHSCENE_List * tmp = list_new();
      for (size_t g = 0; g < nglyphs[i]; g++) {
        GFNT_ColorRun run = {.size = 100, .x = 0, .y = 100, .hint_mode = GSHSCENE_HINT_NONE};
        GFNT_ColorLowering out;
        memset(&out, 0, sizeof out);
        gfnt_scene_record_color_glyph(fonts[i]->res, g_store, tmp, (uint32_t)g, &run, &out, NULL);
        if (out.source != GFNT_COLOR_SOURCE_NONE) {
          ncolor++;
        }
      }
      gshscene_list_destroy(tmp);
    }
  }
  const int cols = 8, cw = 145, ch = 190, x0 = 30;
  /* Each font starts a new row, so allow one partial row per font; the image is cropped to what was used. */
  const int rows_est = ncolor / cols + NF + 1;
  const int W = 1200, H = 120 + rows_est * ch + 40 * NF + 120;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "COLR color glyphs from font's test fixtures, lowered into shape-scene lists (all of COLRv1), CPAL palette 0", 22,
      30, 16, 0.1);
  render("color-glyphs background", L, R);

  double y = 60;
  char skipped[2048] = "";
  int drawn = 0, refused = 0;
  for (int i = 0; i < NF; i++) {
    L = list_new();
    if (!fonts[i]) {
      label(L, files[i], 18, 30, y, 0.1);
      label(L, "(could not be loaded)", 16, 260, y, 0.5);
      y += 40;
      render("color-glyphs font", L, R);
      continue;
    }
    const GSHSCENE_Resource * pal = NULL;
    GFNT_Error err;
    gfnt_error_clear(&err);
    GFNT_Result pr = gfnt_scene_intern_palette(fonts[i]->res, g_store, 0, &pal, &err);
    char head[256];
    snprintf(head, sizeof head, "%s  (%zu glyphs%s)", files[i], nglyphs[i], pr == GFNT_OK ? ", palette 0" : ", no CPAL");
    label(L, head, 18, 30, y, 0.1);
    if (!strcmp(files[i], "color-v0-edges.ttf")) {
      label(L, "gid 3 is 'B', whose layers are all empty glyphs by design (tools/fixtures/color_fixtures.py): it draws nothing",
          14, 420, y + 3, 0.45);
    }
    if (pal) {
      gshscene_list_set_palette(L, pal);
    }
    y += 32;
    int col = 0, any = 0;
    char reasons[512] = "";
    for (size_t g = 0; g < nglyphs[i]; g++) {
      double cx = x0 + col * cw, cy = y;
      GFNT_ColorRun run = {.size = 100, .x = (int32_t)(cx + 20), .y = (int32_t)(cy + 110),
          .hint_mode = GSHSCENE_HINT_NONE};
      GFNT_ColorLowering out;
      memset(&out, 0, sizeof out);
      /* Cell background first, so the glyph lands on it: record the glyph into a scratch list to learn its fate. */
      GSHSCENE_List * probe = list_new();
      GFNT_Result r = gfnt_scene_record_color_glyph(fonts[i]->res, g_store, probe, (uint32_t)g, &run, &out, NULL);
      gshscene_list_destroy(probe);
      if (r == GFNT_OK && out.source == GFNT_COLOR_SOURCE_NONE) {
        continue;
      }
      any = 1;
      fill_rect(L, cx, cy, cx + cw - 10, cy + ch - 40, solid(1, 1, 1, 1));
      char cap[128];
      snprintf(cap, sizeof cap, "gid %zu  COLRv%d", g, out.source == GFNT_COLOR_SOURCE_V1 ? 1 : 0);
      if (r != GFNT_OK) {
        /* A failure of the call, not a refusal of the glyph (a refusal is a placeholder, below): say so and fail the run. */
        GFNT_Error e2;
        gfnt_error_clear(&e2);
        GSHSCENE_List * p2 = list_new();
        gfnt_scene_record_color_glyph(fonts[i]->res, g_store, p2, (uint32_t)g, &run, &out, &e2);
        printf("color %s gid %zu: result %d\n", files[i], g, (int)r);
        gfnt_error_dump(&e2, stdout);
        gshscene_list_destroy(p2);
        label_c(L, "failed", 14, cx + (cw - 10) / 2.0, cy + 60, 0.3);
        g_failed = 1;
      } else {
        const size_t first = gshscene_list_count(L);
        r = gfnt_scene_record_color_glyph(fonts[i]->res, g_store, L, (uint32_t)g, &run, &out, &err);
        if (r != GFNT_OK) {
          fail("color glyph record");
        } else if (out.outcome == GFNT_COLOR_OUTCOME_LOWERED) {
          drawn++;
          printf("color %s gid %zu: %zu items:", files[i], g, out.items);
          for (size_t k = gshscene_list_count(L) - out.items; k < gshscene_list_count(L); k++) {
            const GSHSCENE_Item * it = NULL;
            gshscene_list_item(L, k, &it);
            GSHSCENE_Glyph_Run gr;
            long gid = -1;
            if (it->resource && gshscene_resource_glyph_run(it->resource, &gr) == GSHSCENE_OK && gr.glyph_count == 1) {
              gid = (long)gr.glyphs[0].id;
            }
            printf(" [kind %d glyph %ld paint %d entry %u alpha %g op %u]", (int)it->kind, gid, (int)it->paint.kind,
                it->paint.entry, it->paint.rgba[3], it->group.composite);
          }
          printf("\n");
        } else {
          /* The glyph is refused: the list holds a placeholder (an item that draws nothing, reported by the render), and
           * this page draws the box an application would, from the item, with the registry's name for the reason. */
          const GSHSCENE_Item * it = NULL;
          gshscene_list_item(L, first, &it);
          const char * why = gshscene_placeholder_string(it->placeholder.source, it->placeholder.code);
          double cell[4] = {cx + 10, cy + 10, cx + cw - 20, cy + ch - 50}, box[4];
          placeholder_box(it, cell, box);
          fill_rect(L, box[0], box[1], box[2], box[3], solid(0.85, 0.85, 0.85, 1));
          for (int k = 0; k < 8; k++) {
            double sy = box[1] + (box[3] - box[1]) * k / 8.0;
            fill_rect(L, box[0], sy, box[2], sy + (box[3] - box[1]) / 24.0, solid(0.75, 0.75, 0.75, 1));
          }
          label_c(L, "placeholder:", 14, cx + (cw - 10) / 2.0, cy + 50, 0.3);
          label_c(L, why, 14, cx + (cw - 10) / 2.0, cy + 70, 0.3);
          if (out.paint_format) {
            char pf[64];
            snprintf(pf, sizeof pf, "paint format %u", out.paint_format);
            label_c(L, pf, 12, cx + (cw - 10) / 2.0, cy + 92, 0.35);
          }
          printf("color %s gid %zu: placeholder item %zu, source %u code %u (%s)%s, paint format %u, box %s\n", files[i], g,
              first, (unsigned)it->placeholder.source, (unsigned)it->placeholder.code, why,
              it->placeholder.approximated ? ", approximated" : ", refused", out.paint_format,
              it->placeholder.has_box ? "from the item" : "none");
          size_t n = strlen(reasons);
          if (out.paint_format) {
            snprintf(reasons + n, sizeof reasons - n, "%sgid %zu: %s (format %u)", n ? "; " : "", g, why,
                out.paint_format);
          } else {
            snprintf(reasons + n, sizeof reasons - n, "%sgid %zu: %s", n ? "; " : "", g, why);
          }
          refused++;
        }
      }
      label_c(L, cap, 13, cx + (cw - 10) / 2.0, cy + ch - 34, 0.3);
      col++;
      if (col == cols) {
        col = 0;
        y += ch;
      }
    }
    if (!any) {
      label(L, "(no glyph with a COLR description)", 15, 30, y, 0.5);
      y += 30;
    } else if (col) {
      y += ch;
    }
    if (reasons[0]) {
      char b[700];
      snprintf(b, sizeof b, "Placeholders: %s", reasons);
      label(L, b, 14, 30, y - 12, 0.4);
      y += 22;
      size_t n = strlen(skipped);
      snprintf(skipped + n, sizeof skipped - n, "%s%s: %s", n ? " | " : "", files[i], reasons);
    }
    y += 14;
    char what[160];
    snprintf(what, sizeof what, "color-glyphs %s", files[i]);
    render(what, L, R);
    gshscene_resource_release(pal);
  }
  L = list_new();
  label(L, "Not drawn here: sbix, CBDT and SVG glyphs (bitmap/SVG sources; the scene's image resource is reserved).", 15,
      30, y + 6, 0.45);
  render("color-glyphs footer", L, R);
  printf("color glyphs: %d drawn, %d placeholders; placeholders: %s\n", drawn, refused, skipped);
  /* Crop to what was used. */
  int used = (int)y + 40;
  if (used < H) {
    GIMG_Raster * C = raster_new(W, used);
    for (int yy = 0; yy < used; yy++) {
      memcpy((unsigned char *)gimg_raster_pixels(C) + (size_t)yy * gimg_raster_stride_bytes(C), pixel(R, 0, yy),
          (size_t)W * 4);
    }
    save_png(C, "color-glyphs.png");
    gimg_raster_destroy(C);
  } else {
    save_png(R, "color-glyphs.png");
  }
  gimg_raster_destroy(R);
}

/* --- 8: strokes ----------------------------------------------------------------------------------------------- */

/** Stroke `res` (released) under `map` with `st`, which is interned for this stroke. */
static void stroke_map(GSHSCENE_List * L, const GSHSCENE_Resource * res, const GSHGEOM_Map * map,
    const GSHSCENE_Stroke_Style * st, GSHSCENE_Paint paint) {
  const GSHSCENE_Resource * sr = NULL;
  GSHSCENE_Result r = gshscene_store_intern_stroke_style(g_store, st, &sr);
  if (r == GSHSCENE_OK) {
    r = gshscene_list_stroke(L, res, sr, map, &paint, 0, 0.0);
  }
  if (r != GSHSCENE_OK) {
    fprintf(stderr, "gallery: stroke: %s\n", gshscene_result_string(r));
    g_failed = 1;
  }
  gshscene_resource_release(sr);
  gshscene_resource_release(res);
}

static GSHSCENE_Stroke_Style style_new(double width, uint32_t join, uint32_t cap) {
  GSHSCENE_Stroke_Style st;
  gshscene_stroke_style_init(&st);
  st.width = width;
  st.join = join;
  st.cap = cap;
  return st;
}

/** An open zigzag of three segments with a sharp turn each way, from (x, y) across `w`. */
static const GSHSCENE_Resource * zigzag(double x, double y, double w, double h) {
  GSHGEOM_Path * p = path_new();
  gshgeom_path_move_to(p, P(x, y + h));
  gshgeom_path_line_to(p, P(x + w * 0.3, y));
  gshgeom_path_line_to(p, P(x + w * 0.55, y + h));
  gshgeom_path_line_to(p, P(x + w, y + h * 0.15));
  return intern(p);
}

/** A cubic S-curve across the box. */
static const GSHSCENE_Resource * scurve(double x, double y, double w, double h) {
  GSHGEOM_Path * p = path_new();
  gshgeom_path_move_to(p, P(x, y + h));
  gshgeom_path_cubic_to(p, P(x + w * 0.1, y - h * 0.3), P(x + w * 0.5, y - h * 0.3), P(x + w * 0.5, y + h * 0.5));
  gshgeom_path_cubic_to(p, P(x + w * 0.5, y + h * 1.3), P(x + w * 0.9, y + h * 1.3), P(x + w, y));
  return intern(p);
}

static void image_strokes(void) {
  const int W = 1200, H = 1210;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "strokes: one expansion, drawn by shape-cpu", 26, 30, 18, 0.1);
  const GSHSCENE_Paint ink = solid(0.1, 0.2, 0.45, 1);
  const GSHSCENE_Paint guide = solid(0.9, 0.35, 0.1, 1);

  /* Joins: miter, round, bevel on the same zigzag, with the centerline over it. */
  static const char * join_name[3] = {"miter", "round", "bevel"};
  static const uint32_t join_id[3] = {GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_JOIN_BEVEL};
  for (int k = 0; k < 3; k++) {
    double x = 40 + k * 390;
    GSHSCENE_Stroke_Style st = style_new(26, join_id[k], GSHSCENE_STROKE_CAP_BUTT);
    stroke_map(L, zigzag(x, 90, 330, 120), NULL, &st, ink);
    GSHSCENE_Stroke_Style th = style_new(1.5, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    stroke_map(L, zigzag(x, 90, 330, 120), NULL, &th, solid(1, 1, 1, 1));
    label_c(L, join_name[k], 18, x + 165, 235, 0.2);
  }

  /* Caps: butt, round, square on a line, the endpoints marked. */
  static const char * cap_name[3] = {"butt", "round", "square"};
  static const uint32_t cap_id[3] = {GSHSCENE_STROKE_CAP_BUTT, GSHSCENE_STROKE_CAP_ROUND, GSHSCENE_STROKE_CAP_SQUARE};
  for (int k = 0; k < 3; k++) {
    double x = 60 + k * 390, y = 300;
    GSHGEOM_Path * p = path_new();
    gshgeom_path_move_to(p, P(x, y));
    gshgeom_path_line_to(p, P(x + 290, y));
    GSHSCENE_Stroke_Style st = style_new(34, GSHSCENE_STROKE_JOIN_MITER, cap_id[k]);
    stroke_map(L, intern(p), NULL, &st, ink);
    fill(L, circle(x, y, 3), guide);
    fill(L, circle(x + 290, y, 3), guide);
    label_c(L, cap_name[k], 18, x + 145, y + 36, 0.2);
  }

  /* Dashes: a pattern on a curve, with round caps making dots, and an offset. */
  {
    double dash_a[2] = {28, 16};
    GSHSCENE_Stroke_Style st = style_new(10, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_BUTT);
    st.dashes = dash_a;
    st.dash_count = 2;
    stroke_map(L, scurve(40, 420, 340, 70), NULL, &st, ink);
    label_c(L, "dashes 28 on, 16 off", 18, 210, 520, 0.2);
    double dash_b[2] = {0.01, 22};
    GSHSCENE_Stroke_Style dots = style_new(12, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
    dots.dashes = dash_b;
    dots.dash_count = 2;
    stroke_map(L, scurve(430, 420, 340, 70), NULL, &dots, guide);
    label_c(L, "round dots", 18, 600, 520, 0.2);
    double dash_c[4] = {30, 10, 6, 10};
    GSHSCENE_Stroke_Style dd = style_new(8, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_SQUARE);
    dd.dashes = dash_c;
    dd.dash_count = 4;
    dd.dash_offset = 20;
    stroke_map(L, scurve(820, 420, 340, 70), NULL, &dd, ink);
    label_c(L, "dash-dot, square caps, offset", 18, 990, 520, 0.2);
  }

  /* Widths in both spaces under a non-uniform map: the local pen is an ellipse; the device pen stays round. */
  {
    GSHGEOM_Map sc;
    gshgeom_map_scale(2, 2.4, 0.9, 1.0, &sc);
    GSHGEOM_Map mv, m;
    for (int k = 0; k < 2; k++) {
      gshgeom_map_translate(2, 60 + k * 400, 590, 0, &mv);
      gshgeom_map_compose(&mv, &sc, &m);
      GSHGEOM_Path * p = path_new();
      add_circle(p, 50, 60, 42);
      GSHSCENE_Stroke_Style st = style_new(k ? 14 : 8, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_BUTT);
      st.width_space = k ? GSHSCENE_STROKE_WIDTH_DEVICE : GSHSCENE_STROKE_WIDTH_LOCAL;
      stroke_map(L, intern(p), &m, &st, ink);
    }
    label_c(L, "local width: an elliptical pen", 18, 60 + 120, 745, 0.2);
    label_c(L, "device width: 14 pixels all round", 18, 460 + 120, 745, 0.2);
    /* A hairline: 1 device pixel at any scale. */
    for (int k = 0; k < 8; k++) {
      GSHGEOM_Path * p = path_new();
      gshgeom_path_move_to(p, P(850, 600 + k * 14));
      gshgeom_path_line_to(p, P(1150, 604 + k * 14 + k * 3));
      GSHSCENE_Stroke_Style st = style_new(1, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
      st.width_space = GSHSCENE_STROKE_WIDTH_DEVICE;
      stroke_map(L, intern(p), NULL, &st, solid(0.1, 0.1, 0.1, 1));
    }
    label_c(L, "device-width hairlines", 18, 1000, 745, 0.2);
  }

  /* Alignment and a gradient: inside and outside strokes of a star, and a gradient stroke. */
  {
    static const char * al_name[3] = {"centered", "inside", "outside"};
    static const uint32_t al_id[3] = {GSHSCENE_STROKE_ALIGN_CENTER, GSHSCENE_STROKE_ALIGN_INSIDE,
        GSHSCENE_STROKE_ALIGN_OUTSIDE};
    for (int k = 0; k < 3; k++) {
      double cx = 130 + k * 260, cy = 870;
      fill(L, star(cx, cy, 70), solid(0.8, 0.82, 0.86, 1));
      GSHSCENE_Stroke_Style st = style_new(18, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
      st.alignment = al_id[k];
      stroke_map(L, star(cx, cy, 70), NULL, &st, solid(0.85, 0.2, 0.1, 0.8));
      label_c(L, al_name[k], 18, cx, 950, 0.2);
    }
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_LINEAR);
    g.p0[0] = 900;
    g.p0[1] = 800;
    g.p1[0] = 1160;
    g.p1[1] = 940;
    const GSHSCENE_Gradient_Stop gs[2] = {stop(0, 0.1, 0.3, 0.9, 1), stop(1, 0.95, 0.4, 0.05, 1)};
    GSHSCENE_Gradient gg = g;
    gg.stops = gs;
    gg.stop_count = 2;
    const GSHSCENE_Resource * gr = NULL;
    if (gshscene_store_intern_gradient(g_store, &gg, &gr) != GSHSCENE_OK) {
      fail("intern gradient");
    } else {
      GSHSCENE_Paint gp;
      memset(&gp, 0, sizeof gp);
      gp.kind = GSHSCENE_PAINT_GRADIENT;
      gp.rgba[3] = 1.0;
      gp.gradient = gr;
      GSHSCENE_Stroke_Style st = style_new(22, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
      stroke_map(L, scurve(900, 810, 250, 100), NULL, &st, gp);
      gshscene_resource_release(gr);
    }
    label_c(L, "a gradient as the stroke's paint", 18, 1030, 950, 0.2);
  }
  /* Pieces that overlap (strokes story S4b): a stroke's coverage is the area of what its pieces cover, so a translucent
   * stroke is no darker where pieces overlap than anywhere else in it: square-cap dashes whose caps reach into their
   * neighbours, the inner side of a tight zigzag, and a thin aligned stroke along the edge of a fill. */
  {
    double dash_d[2] = {44, 10};
    GSHSCENE_Stroke_Style st = style_new(28, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_SQUARE);
    st.dashes = dash_d;
    st.dash_count = 2;
    stroke_map(L, scurve(40, 1030, 340, 60), NULL, &st, solid(0.1, 0.2, 0.45, 0.5));
    label_c(L, "dashes, square caps, 10 apart", 18, 210, 1140, 0.2);
    GSHSCENE_Stroke_Style zz = style_new(34, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    stroke_map(L, zigzag(440, 1010, 320, 110), NULL, &zz, solid(0.85, 0.2, 0.1, 0.5));
    label_c(L, "tight joins, translucent", 18, 600, 1140, 0.2);
    fill(L, star(960, 1065, 75), solid(0.8, 0.82, 0.86, 1));
    GSHSCENE_Stroke_Style th = style_new(5, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    th.alignment = GSHSCENE_STROKE_ALIGN_INSIDE;
    stroke_map(L, star(960, 1065, 75), NULL, &th, solid(0.1, 0.2, 0.45, 0.6));
    label_c(L, "a thin inside stroke", 18, 960, 1160, 0.2);
  }
  render("strokes", L, R);
  save_png(R, "strokes.png");
  gimg_raster_destroy(R);
}

int main(int argc, char ** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: gallery FONTDIR DEJAVU OUTDIR\n");
    return 2;
  }
  g_out = argv[3];
  if (gshscene_store_create(NULL, NULL, &g_store) != GSHSCENE_OK ||
      gshcpu_sink_create(NULL, NULL, NULL, &g_cpu) != GSHCPU_OK ||
      gshscene_report_create(NULL, NULL, &g_report) != GSHSCENE_OK) {
    fprintf(stderr, "gallery: setup failed\n");
    return 1;
  }
  g_dejavu = font_load(argv[2], "DejaVuSans.ttf");
  if (!g_dejavu) {
    return 1;
  }
  image_shapes();
  image_compositing();
  image_blend_modes();
  image_color();
  image_gradients();
  image_strokes();
  image_text(argv[1]);
  image_color_glyphs(argv[1]);
  printf("gallery: %s\n", g_failed ? "FAILED" : "ok");
  return g_failed;
}
