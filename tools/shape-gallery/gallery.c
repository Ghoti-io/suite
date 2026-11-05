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
 * A gallery of twelve PNGs drawn entirely by the suite's own libraries, for a
 * person to look at (README.md says what each shows).  Every pixel comes from
 * shape-cpu rendering shape-scene lists; text is font's paragraph layout and
 * shaping, its glyphs reaching the scene through font's glyph path provider;
 * the PNG encoding is image's own writer (gimg_doc_save, "png").
 *
 *     gallery FONTDIR DEJAVU OUTDIR
 *
 * FONTDIR is font's tests/data/fonts, DEJAVU the pinned DejaVuSans.ttf that
 * suite/tools/shape-success/fetch.sh fetches.  Writes shapes.png,
 * compositing.png, blend-modes.png, color.png, gradients.png, images.png, masks.png, strokes.png, primitives.png, patterns.png, clip-unions.png, hatches.png, text.png and color-glyphs.png
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
#include <stdarg.h>
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
  /* A refused union clip names the first pair of members it could not separate in `second` (clip_union.h, CU13). */
  for (size_t i = 0; i < gshscene_report_count(g_report); i++) {
    GSHSCENE_Report_Entry e;
    gshscene_report_entry(g_report, i, &e);
    if (e.reason == GSHSCENE_REASON_UNION_OVERLAP) {
      printf("  union overlap at item %zu: members %u and %u\n", e.chain_count ? e.chain[0] : (size_t)0, (unsigned)(e.second >> 16),
          (unsigned)(e.second & 0xffffu));
    }
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
  /* Strokes story S5: the same run is also stroked.  `stroke_first` paints the stroke under the fill (SVG's paint-order),
   * `stroke_only` leaves the fill out.  One run, two items: all the fills of a chunk and then all its strokes. */
  const GSHSCENE_Stroke_Style * stroke;
  GSHSCENE_Paint stroke_paint;
  int stroke_first;
  int stroke_only;
} TextStyle;

static void stroke_map(GSHSCENE_List * L, const GSHSCENE_Resource * res, const GSHGEOM_Map * map,
    const GSHSCENE_Stroke_Style * st, GSHSCENE_Paint paint);

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
        /* Each item takes a reference of its own. */
        int items = (s->stroke ? 1 : 0) + (s->stroke_only ? 0 : 1);
        for (int extra = 1; extra < items; extra++) {
          gshscene_resource_retain(res);
        }
        if (s->stroke && s->stroke_first) {
          stroke_map(L, res, &at, s->stroke, s->stroke_paint);
        }
        if (!s->stroke_only) {
          fill_map(L, res, &at, GSHGEOM_FILL_NONZERO, s->paint);
        }
        if (s->stroke && !s->stroke_first) {
          stroke_map(L, res, &at, s->stroke, s->stroke_paint);
        }
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
  p.resource = gr;
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
   * where there is nothing beneath it to multiply; inside a non-isolated group it sees the backdrop again.  Then PDF's two
   * kinds of group (T1 to T7, drawn by shape-cpu since masks-images-patterns story M3): a knockout group, where the second of two
   * translucent shapes replaces the first against the backdrop, and the same two shapes in a plain group, where it composites
   * over the first. */
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
  blend_backdrop(L, x0 + 2 * dx, iy, tw, th);
  GSHSCENE_Group open_group = group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER);
  open_group.isolation = GSHSCENE_ISOLATION_NON_ISOLATED;
  group_push(L, open_group);
  group_push(L, group_props(1.0, GSHSCENE_COMPOSITE_MULTIPLY));
  blend_source(L, x0 + 2 * dx, iy, tw, th, kPatches, 4);
  group_pop(L);
  group_pop(L);
  for (int k = 0; k < 2; k++) {
    double x = x0 + (3 + k) * dx;
    blend_backdrop(L, x, iy, tw, th);
    GSHSCENE_Group g = group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER);
    g.isolation = GSHSCENE_ISOLATION_NON_ISOLATED;
    g.knockout = k == 0 ? 1u : 0u;
    group_push(L, g);
    fill_rect(L, x + 0.10 * tw, iy + 0.15 * th, x + 0.65 * tw, iy + 0.70 * th, solid(0.90, 0.10, 0.10, 0.6));
    fill_rect(L, x + 0.35 * tw, iy + 0.30 * th, x + 0.90 * tw, iy + 0.85 * th, solid(0.10, 0.20, 0.90, 0.6));
    group_pop(L);
  }
  render("blend modes (tiles)", L, R);

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
  label(L, "Isolation (B6): multiply at the root, in an isolated group, in a non-isolated one; a knockout group and a plain one (T1 to T7)",
      16, 30, iy - 26, 0.15);
  label_c(L, "multiply onto the backdrop", 14, x0 + tw / 2, iy + th + 4, 0.15);
  label_c(L, "inside an isolated group", 14, x0 + dx + tw / 2, iy + th + 4, 0.15);
  label_c(L, "inside a non-isolated group", 14, x0 + 2 * dx + tw / 2, iy + th + 4, 0.15);
  label_c(L, "a knockout group of two shapes", 14, x0 + 3 * dx + tw / 2, iy + th + 4, 0.15);
  label_c(L, "the same in a plain group", 14, x0 + 4 * dx + tw / 2, iy + th + 4, 0.15);
  static const char * const isolation_tiles[5] = {"multiply at the root", "in an isolated group", "in a non-isolated group",
      "a knockout group of two shapes", "the same in a plain group"};
  for (int k = 0; k < 5; k++) {
    tile_readback(L, R, x0 + k * dx, iy, tw, th);
    const unsigned char * mid = pixel(R, (int)(x0 + k * dx + tw / 2), (int)(iy + th / 2));
    printf("blend modes: isolation tile %d (%s): mid %d,%d,%d,%d\n", k, isolation_tiles[k], mid[0], mid[1], mid[2], mid[3]);
  }
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
  const int W = 1200, H = 1530;
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
      gp.resource = gr;
      GSHSCENE_Stroke_Style st = style_new(22, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
      stroke_map(L, scurve(900, 810, 250, 100), NULL, &st, gp);
      gshscene_resource_release(gr);
    }
    label_c(L, "a gradient as the stroke's paint", 18, 1030, 950, 0.2);
  }
  /* Pieces that overlap (strokes story S4b): a stroke's coverage is the area of what its pieces cover, so a translucent
   * stroke is no darker where pieces overlap than anywhere else in it: square-cap dashes whose caps reach into their
   * neighbours, the inner side of a tight zigzag, and a thin aligned stroke along the edge of a fill.  The first dashes are
   * 44 on and 10 off with a stroke 28 wide, so the caps (14 past each end) cover the gap and the dashes read as one; the
   * second are 20 on and 30 off, where the 28-unit squares show what a square cap is. */
  {
    double dash_d[2] = {44, 10};
    GSHSCENE_Stroke_Style st = style_new(28, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_SQUARE);
    st.dashes = dash_d;
    st.dash_count = 2;
    stroke_map(L, scurve(30, 1030, 300, 60), NULL, &st, solid(0.1, 0.2, 0.45, 0.5));
    label_c(L, "dashes [44, 10], square caps", 18, 180, 1140, 0.2);
    double dash_e[2] = {20, 30};
    GSHSCENE_Stroke_Style sq = style_new(28, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_SQUARE);
    sq.dashes = dash_e;
    sq.dash_count = 2;
    stroke_map(L, scurve(360, 1030, 300, 60), NULL, &sq, solid(0.1, 0.2, 0.45, 0.5));
    label_c(L, "dashes [20, 30], square caps", 18, 510, 1140, 0.2);
    GSHSCENE_Stroke_Style zz = style_new(34, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    stroke_map(L, zigzag(690, 1010, 250, 110), NULL, &zz, solid(0.85, 0.2, 0.1, 0.5));
    label_c(L, "tight joins, translucent", 18, 815, 1140, 0.2);
    fill(L, star(1070, 1065, 75), solid(0.8, 0.82, 0.86, 1));
    GSHSCENE_Stroke_Style th = style_new(5, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    th.alignment = GSHSCENE_STROKE_ALIGN_INSIDE;
    stroke_map(L, star(1070, 1065, 75), NULL, &th, solid(0.1, 0.2, 0.45, 0.6));
    label_c(L, "a thin inside stroke", 18, 1070, 1160, 0.2);
  }
  /* Stroked text (strokes story S5): a line of text is one run, and its stroke is one path's: the stroke of the glyphs'
   * outlines, united across the line, unhinted.  A wide translucent stroke is no darker where neighbouring letters' strokes
   * overlap; the stroke can be painted under the fill (paint-order); a gradient spans the line; dashes follow each contour. */
  {
    GSHSCENE_Stroke_Style outline = style_new(3, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
    TextStyle t = style(84, 0.1, 0.2, 0.45);
    t.hint_mode = GSHSCENE_HINT_NONE;
    t.stroke = &outline;
    t.stroke_paint = solid(0.85, 0.2, 0.1, 1);
    t.stroke_only = 1;
    text_draw(L, &g_dejavu, 1, "Outline", &t, 40, 1215);
    label_c(L, "a stroked line of text", 18, 210, 1345, 0.2);

    GSHSCENE_Stroke_Style wide = style_new(14, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
    TextStyle u = style(84, 0.98, 0.85, 0.35);
    u.hint_mode = GSHSCENE_HINT_NONE;
    u.stroke = &wide;
    u.stroke_paint = solid(0.1, 0.2, 0.45, 0.45);
    text_draw(L, &g_dejavu, 1, "Union", &u, 440, 1215);
    label_c(L, "translucent: one union across the line", 18, 610, 1345, 0.2);

    GSHSCENE_Stroke_Style behind = style_new(14, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
    TextStyle b = style(84, 1, 1, 1);
    b.hint_mode = GSHSCENE_HINT_NONE;
    b.stroke = &behind;
    b.stroke_paint = solid(0.15, 0.15, 0.2, 1);
    b.stroke_first = 1;
    text_draw(L, &g_dejavu, 1, "Behind", &b, 820, 1215);
    label_c(L, "stroke first, then the fill", 18, 985, 1345, 0.2);

    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_LINEAR);
    g.p0[0] = 40;
    g.p0[1] = 0;
    g.p1[0] = 420;
    g.p1[1] = 0;
    const GSHSCENE_Gradient_Stop gs[2] = {stop(0, 0.1, 0.3, 0.9, 1), stop(1, 0.95, 0.4, 0.05, 1)};
    g.stops = gs;
    g.stop_count = 2;
    const GSHSCENE_Resource * gr = NULL;
    if (gshscene_store_intern_gradient(g_store, &g, &gr) != GSHSCENE_OK) {
      fail("intern text gradient");
    } else {
      GSHSCENE_Paint gp;
      memset(&gp, 0, sizeof gp);
      gp.kind = GSHSCENE_PAINT_GRADIENT;
      gp.rgba[3] = 1.0;
      gp.resource = gr;
      GSHSCENE_Stroke_Style gstroke = style_new(6, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
      TextStyle gt = style(84, 0, 0, 0);
      gt.hint_mode = GSHSCENE_HINT_NONE;
      gt.stroke = &gstroke;
      gt.stroke_paint = gp;
      gt.stroke_only = 1;
      text_draw(L, &g_dejavu, 1, "Gradient", &gt, 40, 1370);
      gshscene_resource_release(gr);
    }
    label_c(L, "one gradient across the whole line", 18, 230, 1500, 0.2);

    double text_dashes[2] = {9, 6};
    GSHSCENE_Stroke_Style dashed = style_new(3, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
    dashed.dashes = text_dashes;
    dashed.dash_count = 2;
    TextStyle d = style(84, 0, 0, 0);
    d.hint_mode = GSHSCENE_HINT_NONE;
    d.stroke = &dashed;
    d.stroke_paint = solid(0.1, 0.2, 0.45, 1);
    d.stroke_only = 1;
    text_draw(L, &g_dejavu, 1, "Dashes", &d, 520, 1370);
    label_c(L, "dashes along each contour", 18, 650, 1500, 0.2);

    /* The fill is hinted where the font hints, and a stroke never is: hint mode NONE on the run is the request for a
     * registered pair (the line above uses it); the same text hinted shows the fill grid-fitted under an unhinted stroke. */
    GSHSCENE_Stroke_Style thin = style_new(1.5, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    TextStyle h = style(20, 0.98, 0.85, 0.35);
    h.hint_mode = GSHSCENE_HINT_FONT;
    h.stroke = &thin;
    h.stroke_paint = solid(0.1, 0.1, 0.1, 1);
    text_draw(L, &g_dejavu, 1, "hinted fill, unhinted stroke", &h, 890, 1390);
    label_c(L, "a stroke is never hinted", 18, 1030, 1500, 0.2);
  }
  render("strokes", L, R);
  save_png(R, "strokes.png");
  gimg_raster_destroy(R);
}

/* --- 3b: images (masks-images-patterns story I3) ------------------------------------------------------------- */

/** An image of @p w by @p h generated in code: @p fn gives texel (x, y)'s samples (@p channels of them, as fractions). */
typedef void (*ImageFn)(int x, int y, double * out);

static const GSHSCENE_Resource * image_make(int w, int h, int channels, int bits, GSHSCENE_Color_Space space, ImageFn fn) {
  GIMG_Pixel_Format format;
  memset(&format, 0, sizeof format);
  format.channel_model = channels == 1 ? GIMG_CHANNEL_GRAY : channels == 3 ? GIMG_CHANNEL_RGB : GIMG_CHANNEL_RGBA;
  format.channel_type = GIMG_CHANNEL_UNORM;
  format.layout = GIMG_LAYOUT_INTERLEAVED;
  format.channel_count = (uint8_t)channels;
  for (int i = 0; i < channels; i++) {
    format.bits_per_channel[i] = (uint8_t)bits;
  }
  format.alignment = 1;
  format.alpha_form = channels == 4 ? GIMG_ALPHA_STRAIGHT : GIMG_ALPHA_NONE;
  GIMG_Raster * raster = NULL;
  if (gimg_raster_create((uint32_t)w, (uint32_t)h, &format, GIMG_RASTER_OWNED, NULL, 0, &raster) != GIMG_OK) {
    fail("image raster");
    return NULL;
  }
  unsigned char * base = (unsigned char *)gimg_raster_pixels(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);
  const size_t size = bits <= 8 ? 1 : 2;
  const double top = (double)((1u << bits) - 1u);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      double v[4] = {0, 0, 0, 0};
      fn(x, y, v);
      for (int k = 0; k < channels; k++) {
        const uint32_t q = (uint32_t)(v[k] * top + 0.5);
        unsigned char * at = base + (size_t)y * stride + ((size_t)x * (size_t)channels + (size_t)k) * size;
        if (size == 1) {
          *at = (unsigned char)q;
        } else {
          const uint16_t u = (uint16_t)q;
          memcpy(at, &u, sizeof u);
        }
      }
    }
  }
  GSHSCENE_Image_Params params;
  memset(&params, 0, sizeof params);
  params.space = space;
  const GSHSCENE_Resource * r = NULL;
  if (gshscene_store_intern_image(g_store, raster, &params, &r) != GSHSCENE_OK) {
    fail("intern image");
  }
  gimg_raster_destroy(raster);
  return r;
}

/** Texel (x, y) of the 16 by 12 grid carries its coordinates: red is x, green is y, and the blue alternates, so a pixel read back names its texel. */
static void grid_texel(int x, int y, double * v) {
  v[0] = x / 15.0;
  v[1] = y / 11.0;
  v[2] = (x + y) % 2 ? 0.9 : 0.15;
  v[3] = 1.0;
}

/** A row of the hue circle, 24 texels, at 8 bits. */
static void hue_texel(int x, int y, double * v) {
  (void)y;
  const double h = x / 24.0 * 6.0;
  const int i = (int)h;
  const double f = h - i;
  const double r[6] = {1, 1 - f, 0, 0, f, 1};
  const double g[6] = {f, 1, 1, 1 - f, 0, 0};
  const double b[6] = {0, 0, f, 1, 1, 1 - f};
  v[0] = r[i % 6];
  v[1] = g[i % 6];
  v[2] = b[i % 6];
}

/** A 16-bit gray ramp, 64 texels: the 256 levels of an 8-bit one would show steps here. */
static void ramp_texel(int x, int y, double * v) {
  (void)y;
  v[0] = x / 63.0;
}

/** Six swatches in Display P3, as RGB (the same numbers read as sRGB are plainly different colors on an sRGB target). */
static void swatch_texel(int x, int y, double * v) {
  (void)y;
  static const double colors[6][3] = {{0.9, 0.2, 0.2}, {0.2, 0.9, 0.2}, {0.2, 0.2, 0.9}, {0.9, 0.9, 0.2}, {0.2, 0.9, 0.9}, {0.9, 0.2, 0.9}};
  v[0] = colors[x % 6][0];
  v[1] = colors[x % 6][1];
  v[2] = colors[x % 6][2];
}

/** The map of a unit of image space to a box @p sx by @p sy at (@p tx, @p ty). */
static GSHGEOM_Map placed(double sx, double sy, double tx, double ty) {
  const double m[4] = {sx, 0.0, 0.0, sy};
  const double t[2] = {tx, ty};
  GSHGEOM_Map out;
  if (gshgeom_map_from_matrix(2, m, t, &out) != GSHGEOM_OK) {
    fail("map");
  }
  return out;
}

/** A fill of the rectangle with an image paint of @p image under @p map. */
static void image_tile(GSHSCENE_List * L, const GSHSCENE_Resource * image, double x0, double y0, double x1, double y1,
    GSHGEOM_Map map, uint32_t ex, uint32_t ey, uint32_t magnify, uint32_t minify, GSHSCENE_Color_Space filter_space) {
  GSHSCENE_Image_Paint ip;
  gshscene_image_paint_init(&ip);
  ip.image = image;
  ip.map = map;
  ip.extend_x = ex;
  ip.extend_y = ey;
  ip.magnify = magnify;
  ip.minify = minify;
  ip.filter_space = filter_space;
  const GSHSCENE_Resource * paint = NULL;
  if (gshscene_store_intern_image_paint(g_store, &ip, &paint) != GSHSCENE_OK) {
    fail("intern image paint");
    return;
  }
  GSHSCENE_Paint p;
  memset(&p, 0, sizeof p);
  p.kind = GSHSCENE_PAINT_IMAGE;
  p.resource = paint;
  p.rgba[3] = 1.0;
  fill(L, rect(x0, y0, x1, y1), p);
  gshscene_resource_release(paint);
}

static void image_images(void) {
  const int W = 1200, H = 980;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.95);
  const GSHSCENE_Color_Space none = {0, 0, 0.0};
  const GSHSCENE_Color_Space srgb = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB);
  const GSHSCENE_Color_Space linear = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_LINEAR);
  const GSHSCENE_Color_Space p3 = space(GCOL_PRIMARIES_DISPLAY_P3, GCOL_TRANSFER_SRGB);
  const GSHSCENE_Resource * grid = image_make(16, 12, 4, 8, srgb, grid_texel);
  const GSHSCENE_Resource * hue = image_make(24, 1, 3, 8, srgb, hue_texel);
  const GSHSCENE_Resource * ramp = image_make(64, 1, 1, 16, srgb, ramp_texel);
  const GSHSCENE_Resource * swatch_p3 = image_make(6, 1, 3, 8, p3, swatch_texel);
  const GSHSCENE_Resource * swatch_srgb = image_make(6, 1, 3, 8, srgb, swatch_texel);
  static const char * filter_name[4] = {"nearest", "linear", "cubic", "area"};
  static const uint32_t filters[4] = {GSHSCENE_IMAGE_FILTER_NEAREST, GSHSCENE_IMAGE_FILTER_LINEAR, GSHSCENE_IMAGE_FILTER_CUBIC,
      GSHSCENE_IMAGE_FILTER_AREA};
  static const double scales[3] = {7.0, 1.0, 0.3};

  /* Each filter at x7, x1 and x0.3: the grid repeated over a tile, from a half pixel past the origin, so that a unit scale
   * falls between texels.  Each filter is both the magnify and the minify filter, so the map alone does not choose. */
  const double tw = 250, th = 100, dx = 285, dy = 150, x0 = 30, y0 = 90;
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 4; col++) {
      const double x = x0 + col * dx, y = y0 + row * dy;
      image_tile(L, grid, x, y, x + tw, y + th, placed(scales[row], scales[row], x + 0.5, y + 0.5), GSHSCENE_IMAGE_EXTEND_REPEAT,
          GSHSCENE_IMAGE_EXTEND_REPEAT, filters[col], filters[col], none);
    }
  }

  /* The four edge modes, the same on both axes: a small image placed in the middle of its tile, drawn past its edges. */
  const double ey0 = 590;
  static const char * edge_name[4] = {"pad", "repeat", "reflect", "none"};
  for (int k = 0; k < 4; k++) {
    const double x = x0 + k * dx, y = ey0;
    fill_rect(L, x, y, x + tw, y + 110, solid(0.88, 0.88, 0.8, 1));
    image_tile(L, grid, x, y, x + tw, y + 110, placed(5.0, 5.0, x + 85, y + 25), (uint32_t)k, (uint32_t)k,
        GSHSCENE_IMAGE_FILTER_NEAREST, GSHSCENE_IMAGE_FILTER_AREA, none);
  }

  /* Sources in other spaces and depths: a hue row at x10 with linear, a 16-bit ramp, a Display P3 swatch row beside the same
   * numbers in sRGB, and the hue row filtered in linear light. */
  const double sy0 = 830;
  image_tile(L, hue, x0, sy0, x0 + tw, sy0 + 60, placed(10.4, 60.0, x0, sy0), 0, 0, GSHSCENE_IMAGE_FILTER_LINEAR,
      GSHSCENE_IMAGE_FILTER_AREA, none);
  image_tile(L, hue, x0 + dx, sy0, x0 + dx + tw, sy0 + 60, placed(10.4, 60.0, x0 + dx, sy0), 0, 0, GSHSCENE_IMAGE_FILTER_LINEAR,
      GSHSCENE_IMAGE_FILTER_AREA, linear);
  image_tile(L, ramp, x0 + 2 * dx, sy0, x0 + 2 * dx + tw, sy0 + 60, placed(250.0 / 64.0, 60.0, x0 + 2 * dx, sy0), 0, 0,
      GSHSCENE_IMAGE_FILTER_LINEAR, GSHSCENE_IMAGE_FILTER_AREA, none);
  image_tile(L, swatch_p3, x0 + 3 * dx, sy0, x0 + 3 * dx + tw, sy0 + 28, placed(250.0 / 6.0, 28.0, x0 + 3 * dx, sy0), 0, 0,
      GSHSCENE_IMAGE_FILTER_NEAREST, GSHSCENE_IMAGE_FILTER_AREA, none);
  image_tile(L, swatch_srgb, x0 + 3 * dx, sy0 + 32, x0 + 3 * dx + tw, sy0 + 60, placed(250.0 / 6.0, 28.0, x0 + 3 * dx, sy0 + 32), 0, 0,
      GSHSCENE_IMAGE_FILTER_NEAREST, GSHSCENE_IMAGE_FILTER_AREA, none);
  render("images (tiles)", L, R);

  /* Labels, and the middle pixel of each tile read back. */
  L = list_new();
  label(L, "Images (masks-images-patterns story I3): an image resource sampled by an image paint, shape-cpu's definition", 19, 30, 12, 0.1);
  label(L, "Sources are generated in code.  Grid: 16 by 12 texels, red is x, green is y, blue alternates.  Each tile repeats it.",
      14, 30, 40, 0.35);
  label(L, "Rows: scale 7, scale 1 (half a pixel past the origin) and scale 0.3.  Columns: the filter used (as magnify and as minify).",
      14, 30, 60, 0.35);
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 4; col++) {
      const double x = x0 + col * dx, y = y0 + row * dy;
      char buf[96];
      snprintf(buf, sizeof buf, "%s, scale %g", filter_name[col], scales[row]);
      label_c(L, buf, 13, x + tw / 2, y + th + 3, 0.15);
      tile_readback(L, R, x, y, tw, th);
    }
  }
  label(L, "Edge modes (the same on x and y): the image is the 80 by 60 box, drawn past it", 16, 30, ey0 - 30, 0.15);
  for (int k = 0; k < 4; k++) {
    const double x = x0 + k * dx;
    label_c(L, edge_name[k], 14, x + tw / 2, ey0 + 112, 0.15);
    tile_readback(L, R, x, ey0, tw, 110);
  }
  label(L, "Spaces and depths", 16, 30, sy0 - 30, 0.15);
  label_c(L, "hue row, in its own space", 13, x0 + tw / 2, sy0 + 62, 0.15);
  label_c(L, "the same, filtered in linear light", 13, x0 + dx + tw / 2, sy0 + 62, 0.15);
  label_c(L, "a 16-bit ramp", 13, x0 + 2 * dx + tw / 2, sy0 + 62, 0.15);
  label_c(L, "Display P3 swatches over the same in sRGB", 13, x0 + 3 * dx + tw / 2, sy0 + 62, 0.15);
  tile_readback(L, R, x0, sy0, tw, 60);
  tile_readback(L, R, x0 + dx, sy0, tw, 60);
  tile_readback(L, R, x0 + 2 * dx, sy0, tw, 60);
  {
    /* The first swatch of each row, read back: the same numbers, in Display P3 above and in sRGB below. */
    const unsigned char * a = pixel(R, (int)(x0 + 3 * dx + 20), (int)(sy0 + 14));
    const unsigned char * b = pixel(R, (int)(x0 + 3 * dx + 20), (int)(sy0 + 46));
    char buf[96];
    snprintf(buf, sizeof buf, "first swatch: P3 %d,%d,%d  sRGB %d,%d,%d", a[0], a[1], a[2], b[0], b[1], b[2]);
    label_c(L, buf, 12, x0 + 3 * dx + tw / 2, sy0 + 82, 0.35);
  }
  render("images (labels)", L, R);
  save_png(R, "images.png");
  gimg_raster_destroy(R);
  gshscene_resource_release(grid);
  gshscene_resource_release(hue);
  gshscene_resource_release(ramp);
  gshscene_resource_release(swatch_p3);
  gshscene_resource_release(swatch_srgb);
}

/* --- 7: masks ------------------------------------------------------------------------------------------------- */

/* A page of tiles.  Each tile is the same subject (a hue sweep with a white and a dark bar) in a group over a checkerboard, and
 * what differs is the group's mask, which is drawn in the tile's own coordinates and placed at its corner by the group's mask map
 * unless the tile is about the placement.  Under each tile a label says what the mask is, and the 8-bit values of chosen pixels,
 * read back from the render, are printed (and logged).  Every probe sits at the middle of a white checker square, so a value read
 * back is the tile's color over white. */

enum { MASK_TW = 180, MASK_TH = 110 };

static struct MaskProbe {
  char tag[16];
  int x, y;      /* in the raster */
  double tx, ty; /* the tile's corner */
} g_probe[160];
static size_t g_nprobe;

static GSHGEOM_Map move_by(double tx, double ty) {
  GSHGEOM_Map m;
  if (gshgeom_map_translate(2, tx, ty, 0.0, &m) != GSHGEOM_OK) {
    fail("translate map");
  }
  return m;
}

/** The matrix (a b; c d) taking the point (fx, fy) to (tx, ty): a mask placement. */
static GSHGEOM_Map map_about(double a, double b, double c, double d, double fx, double fy, double tx, double ty) {
  const double m[4] = {a, b, c, d};
  const double t[2] = {tx - (a * fx + b * fy), ty - (c * fx + d * fy)};
  GSHGEOM_Map out;
  if (gshgeom_map_from_matrix(2, m, t, &out) != GSHGEOM_OK) {
    fail("matrix map");
  }
  return out;
}

/** Close @p L into a picture resource (the caller releases it) and destroy the list. */
static const GSHSCENE_Resource * picture_of(GSHSCENE_List * L) {
  const GSHSCENE_Resource * pic = NULL;
  if (gshscene_list_close(L) != GSHSCENE_OK || gshscene_store_intern_picture(g_store, L, &pic) != GSHSCENE_OK) {
    fail("intern picture");
  }
  gshscene_list_destroy(L);
  return pic;
}

/** A mask over @p picture (retained by it; the caller still releases its own reference to the picture) in @p mode, with the
 *  identity transfer and the default spaces; the caller sets the rest of @p m before interning it with mask_intern. */
static GSHSCENE_Mask mask_new(const GSHSCENE_Resource * picture, uint32_t mode, uint32_t acts_on) {
  GSHSCENE_Mask m;
  memset(&m, 0, sizeof m);
  m.picture = picture;
  m.mode = mode;
  m.acts_on = acts_on;
  return m;
}

static const GSHSCENE_Resource * mask_intern(const GSHSCENE_Mask * m) {
  const GSHSCENE_Resource * r = NULL;
  GSHSCENE_Result e = gshscene_store_intern_mask(g_store, m, &r);
  if (e != GSHSCENE_OK) {
    fprintf(stderr, "gallery: intern mask: %s\n", gshscene_result_string(e));
    g_failed = 1;
  }
  return r;
}

static void masked_push(GSHSCENE_List * L, const GSHSCENE_Resource * mask, const GSHGEOM_Map * at, double opacity, uint32_t op) {
  GSHSCENE_Group g = group_props(opacity, op);
  g.soft_mask = mask;
  if (at && mask) {
    g.soft_mask_map = *at;
  }
  group_push(L, g);
}

/** The subject every tile masks: a hue sweep about the tile's middle, with a dark bar and a yellow bar across it. */
static void mask_subject(GSHSCENE_List * L, double x, double y) {
  GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_SWEEP);
  g.center[0] = x + MASK_TW / 2.0;
  g.center[1] = y + MASK_TH / 2.0;
  g.angle_unit = GSHGEOM_ANGLE_DEGREES;
  g.a0 = 0;
  g.a1 = 360;
  fill_gradient(L, rect(x, y, x + MASK_TW, y + MASK_TH), NULL, g, hue_stops(), 7);
  fill_rect(L, x, y + MASK_TH * 0.80, x + MASK_TW, y + MASK_TH * 0.88, solid(0.95, 0.95, 0.2, 1));
  fill_rect(L, x, y + MASK_TH * 0.30, x + MASK_TW, y + MASK_TH * 0.36, solid(0.1, 0.1, 0.1, 1));
}

/** The tile's backdrop: a thin frame round a checkerboard of 10-pixel squares. */
static void mask_tile_base(GSHSCENE_List * L, double x, double y) {
  fill_rect(L, x - 1, y - 1, x + MASK_TW + 1, y + MASK_TH + 1, solid(0.55, 0.55, 0.55, 1));
  checker(L, x, y, x + MASK_TW, y + MASK_TH, 10);
}

/** A masked tile: the base, then the subject in a group of @p opacity and @p op masked by @p mask placed by @p at (NULL: put the
 *  mask's origin at the tile's corner). */
static void masked_tile(GSHSCENE_List * L, double x, double y, const GSHSCENE_Resource * mask, const GSHGEOM_Map * at, double opacity,
    uint32_t op) {
  mask_tile_base(L, x, y);
  GSHGEOM_Map corner = move_by(x, y);
  masked_push(L, mask, at ? at : &corner, opacity, op);
  mask_subject(L, x, y);
  group_pop(L);
}

/** Register a probe at tile coordinates (px, py), moved to the middle of the nearest white checker square to the right. */
static void mask_probe(const char * tag, double tx, double ty, double px, double py) {
  int i = (int)(px / 10), j = (int)(py / 10);
  if ((i + j) % 2 == 0) {
    i += (i + 1) * 10 < MASK_TW ? 1 : -1;
  }
  if (g_nprobe < sizeof g_probe / sizeof g_probe[0]) {
    struct MaskProbe * p = &g_probe[g_nprobe++];
    snprintf(p->tag, sizeof p->tag, "%s", tag);
    p->x = (int)tx + i * 10 + 5;
    p->y = (int)ty + j * 10 + 5;
    p->tx = tx;
    p->ty = ty;
  }
}

/** The top-left corner of tile @p col of the section whose header is at @p top. */
static void mask_tile_at(double top, int col, double * x, double * y) {
  *x = 40 + col * 204;
  *y = top + 30;
}

static void image_masks(void) {
  const int W = 1280, H = 1560;
  const double SEC = 206, TOP = 84;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.95);
  const GSHSCENE_Color_Space srgb = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB);
  const GSHSCENE_Color_Space linear = space(GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_LINEAR);
  const double TW = MASK_TW, TH = MASK_TH;
  /* The labels of the tiles, in order, for the second pass: (section, column, text). */
  struct {
    int section, col;
    char text[48];
  } names[48];
  size_t nn = 0;
#define NAME(sec, c, str) \
  do { \
    names[nn].section = (sec); \
    names[nn].col = (c); \
    snprintf(names[nn].text, sizeof names[nn].text, "%s", (str)); \
    nn++; \
  } while (0)
  double x, y;

  /* ---- 0: SVG. Luminance by W3C's coefficients over black, alpha, and the picture rendered and read in other spaces. */
  {
    const double top = TOP;
    {
      mask_tile_at(top, 0, &x, &y);
      GSHSCENE_List * PL = list_new();
      GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_LINEAR);
      g.p0[0] = 0;
      g.p0[1] = 0;
      g.p1[0] = TW;
      g.p1[1] = 0;
      const GSHSCENE_Gradient_Stop ramp[2] = {stop(0, 1, 1, 1, 1), stop(1, 0, 0, 0, 1)};
      fill_gradient(PL, rect(0, 0, TW, TH), NULL, g, ramp, 2);
      const GSHSCENE_Resource * pic = picture_of(PL);
      GSHSCENE_Mask m = mask_new(pic, GSHSCENE_MASK_LUMINANCE, GSHSCENE_MASK_ACTS_ON_OPACITY);
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("left", x, y, 30, 20);
      mask_probe("right", x, y, 150, 20);
      NAME(0, 0, "luminance: white to black");
      gshscene_resource_release(mk);
      gshscene_resource_release(pic);
    }
    {
      mask_tile_at(top, 1, &x, &y);
      GSHSCENE_List * PL = list_new();
      fill(PL, circle(34, TH / 2, 24), solid(1, 0, 0, 1));
      fill(PL, circle(78, TH / 2, 24), solid(0, 1, 0, 1));
      fill(PL, circle(122, TH / 2, 24), solid(0, 0, 1, 1));
      fill(PL, circle(158, TH / 2, 14), solid(1, 1, 1, 0.5));
      const GSHSCENE_Resource * pic = picture_of(PL);
      GSHSCENE_Mask m = mask_new(pic, GSHSCENE_MASK_LUMINANCE, GSHSCENE_MASK_ACTS_ON_OPACITY);
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("red", x, y, 34, 55);
      mask_probe("green", x, y, 78, 55);
      NAME(0, 1, "luminance of r, g, b, and a=0.5");
      gshscene_resource_release(mk);
      gshscene_resource_release(pic);
    }
    {
      mask_tile_at(top, 2, &x, &y);
      GSHSCENE_List * PL = list_new();
      GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_RADIAL);
      g.c0[0] = g.c1[0] = TW / 2;
      g.c0[1] = g.c1[1] = TH / 2;
      g.r1 = 54;
      const GSHSCENE_Gradient_Stop spot[2] = {stop(0, 0.9, 0.2, 0.2, 1), stop(1, 0.9, 0.2, 0.2, 0)};
      fill_gradient(PL, circle(TW / 2, TH / 2, 54), NULL, g, spot, 2);
      const GSHSCENE_Resource * pic = picture_of(PL);
      GSHSCENE_Mask m = mask_new(pic, GSHSCENE_MASK_ALPHA, GSHSCENE_MASK_ACTS_ON_OPACITY);
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("middle", x, y, 90, 55);
      NAME(0, 2, "alpha: a fading spot");
      gshscene_resource_release(mk);
      gshscene_resource_release(pic);
    }
    /* Overlapping translucent discs on black, in three combinations of render space (the picture is blended there) and
     * luminance space (the luminance is taken there): linear then sRGB, sRGB and sRGB, linear and linear. */
    GSHSCENE_List * PL = list_new();
    fill(PL, circle(TW * 0.36, TH * 0.42, 36), solid(1.0, 0.85, 0.1, 0.65));
    fill(PL, circle(TW * 0.62, TH * 0.42, 36), solid(0.1, 0.8, 0.9, 0.65));
    fill(PL, circle(TW * 0.49, TH * 0.68, 36), solid(0.9, 0.2, 0.7, 0.65));
    const GSHSCENE_Resource * discs = picture_of(PL);
    static const char * names3[3] = {"discs: render linear, luma sRGB", "discs: render and luma sRGB", "discs: render and luma linear"};
    for (int k = 0; k < 3; k++) {
      mask_tile_at(top, 3 + k, &x, &y);
      GSHSCENE_Mask m = mask_new(discs, GSHSCENE_MASK_LUMINANCE, GSHSCENE_MASK_ACTS_ON_OPACITY);
      m.render_space = k == 1 ? srgb : linear;
      m.space = k == 0 ? srgb : k == 1 ? srgb : linear;
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("overlap", x, y, TW * 0.49, TH * 0.5);
      mask_probe("one disc", x, y, TW * 0.25, TH * 0.5);
      NAME(0, 3 + k, names3[k]);
      gshscene_resource_release(mk);
    }
    gshscene_resource_release(discs);
  }

  /* ---- 1: PDF. Luminosity over a backdrop color, DeviceRGB's coefficients (30, 59, 11) / 100, and a transfer function. */
  {
    const double top = TOP + SEC;
    GSHSCENE_List * PL = list_new();
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_RADIAL);
    g.c0[0] = g.c1[0] = TW / 2;
    g.c0[1] = g.c1[1] = TH / 2;
    g.r1 = 46;
    const GSHSCENE_Gradient_Stop disc[2] = {stop(0, 1, 1, 1, 1), stop(1, 0.25, 0.25, 0.25, 1)};
    fill_gradient(PL, circle(TW / 2, TH / 2, 46), NULL, g, disc, 2);
    const GSHSCENE_Resource * pic = picture_of(PL);
    static const double table[4] = {0.0, 0.1, 0.9, 1.0};
    static const double fine[3] = {0.1, 0.5, 0.9};
    GSHSCENE_Transfer_Piece pieces[3];
    memset(pieces, 0, sizeof pieces);
    pieces[0].lo = 0.0;
    pieces[0].hi = 0.3;
    pieces[0].e0 = 0.0;
    pieces[0].e1 = 1.0;
    pieces[0].leaf_kind = GSHSCENE_TRANSFER_IDENTITY;
    pieces[1].lo = 0.3;
    pieces[1].hi = 0.7;
    pieces[1].e0 = 0.0;
    pieces[1].e1 = 1.0;
    pieces[1].leaf_kind = GSHSCENE_TRANSFER_EXPONENTIAL;
    pieces[1].c0 = 0.2;
    pieces[1].c1 = 0.9;
    pieces[1].n = 2.5;
    pieces[2].lo = 0.7;
    pieces[2].hi = 1.0;
    pieces[2].e0 = 1.0;
    pieces[2].e1 = 0.0;
    pieces[2].leaf_kind = GSHSCENE_TRANSFER_TABLE;
    pieces[2].sample_count = 3;
    pieces[2].samples = fine;
    static const char * nm[5] = {"BC (.5 .35 .2), identity", "BC, exponential n = 2.2", "BC, table 0 .1 .9 1", "BC, stitching of three", "black BC, identity"};
    for (int k = 0; k < 5; k++) {
      mask_tile_at(top, k, &x, &y);
      GSHSCENE_Mask m = mask_new(pic, GSHSCENE_MASK_LUMINANCE, GSHSCENE_MASK_ACTS_ON_OPACITY);
      m.coefficients = GSHSCENE_MASK_COEFF_LUM;
      if (k < 4) {
        m.backdrop[0] = 0.5;
        m.backdrop[1] = 0.35;
        m.backdrop[2] = 0.2;
      }
      if (k == 1) {
        m.transfer.kind = GSHSCENE_TRANSFER_EXPONENTIAL;
        m.transfer.c0 = 0.0;
        m.transfer.c1 = 1.0;
        m.transfer.n = 2.2;
      } else if (k == 2) {
        m.transfer.kind = GSHSCENE_TRANSFER_TABLE;
        m.transfer.sample_count = 4;
        m.transfer.samples = table;
      } else if (k == 3) {
        m.transfer.kind = GSHSCENE_TRANSFER_STITCHING;
        m.transfer.piece_count = 3;
        m.transfer.pieces = pieces;
      }
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("middle", x, y, TW / 2, TH / 2);
      mask_probe("outside", x, y, 8, 8);
      NAME(1, k, nm[k]);
      gshscene_resource_release(mk);
    }
    gshscene_resource_release(pic);
  }

  /* ---- 2: CSS mask-composite. A stack of layers, bottom first; each layer is composited onto the ones below with its operator
   * (add SRC_OVER, subtract SRC_OUT, intersect SRC_IN, exclude XOR), as a group of the mask's picture; a luminance layer is a
   * group holding an opaque fill, masked by that layer's luminance mask, which makes its alpha the luminance. */
  {
    const double top = TOP + 2 * SEC;
    static const uint32_t ops[4] = {GSHSCENE_COMPOSITE_SRC_OVER, GSHSCENE_COMPOSITE_SRC_OUT, GSHSCENE_COMPOSITE_SRC_IN,
        GSHSCENE_COMPOSITE_XOR};
    static const char * nm[5] = {"add: circle, luminance layer", "subtract (SRC_OUT)", "intersect (SRC_IN)", "exclude (XOR)",
        "the stack, all four operators"};
    /* the luminance layer's own mask: a ramp from black at 0.3 of the width to white at 0.82, over black */
    GSHSCENE_List * R0 = list_new();
    GSHSCENE_Gradient lg = grad_new(GSHSCENE_GRADIENT_LINEAR);
    lg.p0[0] = TW * 0.30;
    lg.p0[1] = 0;
    lg.p1[0] = TW * 0.82;
    lg.p1[1] = 0;
    const GSHSCENE_Gradient_Stop lramp[2] = {stop(0, 0, 0, 0, 1), stop(1, 1, 1, 1, 1)};
    fill_gradient(R0, rect(TW * 0.30, 0, TW * 0.82, TH), NULL, lg, lramp, 2);
    const GSHSCENE_Resource * lpic = picture_of(R0);
    GSHSCENE_Mask lm = mask_new(lpic, GSHSCENE_MASK_LUMINANCE, GSHSCENE_MASK_ACTS_ON_OPACITY);
    const GSHSCENE_Resource * lmask = mask_intern(&lm);
    for (int k = 0; k < 5; k++) {
      mask_tile_at(top, k, &x, &y);
      GSHSCENE_List * PL = list_new();
      if (k < 4) {
        fill(PL, circle(TW * 0.38, TH / 2, 38), solid(1, 1, 1, 1));
        group_push(PL, group_props(1.0, ops[k]));
        masked_push(PL, lmask, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
        fill_rect(PL, 0, 0, TW, TH, solid(1, 1, 1, 1));
        group_pop(PL);
        group_pop(PL);
      } else {
        fill(PL, circle(TW * 0.40, TH / 2, 24), solid(1, 1, 1, 1));
        group_push(PL, group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER));
        fill_rect(PL, TW * 0.30, TH * 0.35, TW * 0.60, TH * 0.65, solid(1, 1, 1, 1));
        group_pop(PL);
        group_push(PL, group_props(1.0, GSHSCENE_COMPOSITE_SRC_OUT));
        fill(PL, circle(TW * 0.45, TH / 2, 46), solid(1, 1, 1, 1));
        group_pop(PL);
        group_push(PL, group_props(1.0, GSHSCENE_COMPOSITE_SRC_IN));
        fill_rect(PL, TW * 0.35, 0, TW * 0.65, TH, solid(1, 1, 1, 1));
        group_pop(PL);
        group_push(PL, group_props(1.0, GSHSCENE_COMPOSITE_XOR));
        masked_push(PL, lmask, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
        fill_rect(PL, 0, 0, TW, TH, solid(1, 1, 1, 1));
        group_pop(PL);
        group_pop(PL);
      }
      const GSHSCENE_Resource * pic = picture_of(PL);
      GSHSCENE_Mask m = mask_new(pic, GSHSCENE_MASK_ALPHA, GSHSCENE_MASK_ACTS_ON_OPACITY);
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("middle", x, y, TW * 0.55, TH / 2);
      mask_probe("circle", x, y, TW * 0.30, TH / 2);
      NAME(2, k, nm[k]);
      gshscene_resource_release(mk);
      gshscene_resource_release(pic);
    }
    gshscene_resource_release(lmask);
    gshscene_resource_release(lpic);
  }

  /* ---- 3: a mask of a mask. The outer mask's picture is a group of stripes that is itself masked by a spot. */
  {
    const double top = TOP + 3 * SEC;
    GSHSCENE_List * S = list_new();
    GSHSCENE_Gradient sg = grad_new(GSHSCENE_GRADIENT_RADIAL);
    sg.c0[0] = sg.c1[0] = TW / 2;
    sg.c0[1] = sg.c1[1] = TH / 2;
    sg.r1 = 56;
    const GSHSCENE_Gradient_Stop sstops[2] = {stop(0, 1, 1, 1, 1), stop(1, 1, 1, 1, 0)};
    fill_gradient(S, circle(TW / 2, TH / 2, 56), NULL, sg, sstops, 2);
    const GSHSCENE_Resource * spot = picture_of(S);
    GSHSCENE_Mask sm = mask_new(spot, GSHSCENE_MASK_ALPHA, GSHSCENE_MASK_ACTS_ON_OPACITY);
    const GSHSCENE_Resource * spot_mask = mask_intern(&sm);
    GSHGEOM_Path * stripes = path_new();
    for (int i = 0; i < 9; i++) {
      add_rect(stripes, 4 + i * 20, 0, 14 + i * 20, TH);
    }
    const GSHSCENE_Resource * stripe_res = intern(stripes);
    /* picture 0: the stripes alone; picture 1: the stripes in a group masked by the spot */
    GSHSCENE_List * A = list_new();
    gshscene_resource_retain(stripe_res);
    fill(A, stripe_res, solid(1, 1, 1, 1));
    const GSHSCENE_Resource * plain = picture_of(A);
    GSHSCENE_List * B = list_new();
    masked_push(B, spot_mask, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
    gshscene_resource_retain(stripe_res);
    fill(B, stripe_res, solid(1, 1, 1, 1));
    group_pop(B);
    const GSHSCENE_Resource * nested = picture_of(B);
    static const char * nm[3] = {"mask of a mask: stripes in a spot", "the stripes alone", "the spot alone"};
    const GSHSCENE_Resource * pics[3] = {nested, plain, spot};
    for (int k = 0; k < 3; k++) {
      mask_tile_at(top, k, &x, &y);
      GSHSCENE_Mask m = mask_new(pics[k], GSHSCENE_MASK_ALPHA, GSHSCENE_MASK_ACTS_ON_OPACITY);
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("middle", x, y, TW / 2, TH / 2);
      mask_probe("edge", x, y, 24, 40);
      NAME(3, k, nm[k]);
      gshscene_resource_release(mk);
    }
    gshscene_resource_release(nested);
    gshscene_resource_release(plain);
    gshscene_resource_release(stripe_res);
    gshscene_resource_release(spot_mask);
    gshscene_resource_release(spot);
  }

  /* ---- 4: what the mask multiplies. A group of a white disc, composited DEST_IN onto a layer of the subject, under an alpha ramp
   * that acts on the group's opacity or on its shape; they differ for DEST_IN (an unbounded operator) and agree for SRC_OVER. */
  {
    const double top = TOP + 4 * SEC;
    GSHSCENE_List * PL = list_new();
    GSHSCENE_Gradient g = grad_new(GSHSCENE_GRADIENT_LINEAR);
    g.p0[0] = 0;
    g.p0[1] = 0;
    g.p1[0] = TW;
    g.p1[1] = 0;
    const GSHSCENE_Gradient_Stop ramp[2] = {stop(0, 1, 1, 1, 0), stop(1, 1, 1, 1, 1)};
    fill_gradient(PL, rect(0, 0, TW, TH), NULL, g, ramp, 2);
    const GSHSCENE_Resource * pic = picture_of(PL);
    static const char * nm[5] = {"DEST_IN, no mask", "DEST_IN, mask on opacity", "DEST_IN, mask on shape", "SRC_OVER, mask on opacity",
        "SRC_OVER, mask on shape"};
    for (int k = 0; k < 5; k++) {
      mask_tile_at(top, k, &x, &y);
      mask_tile_base(L, x, y);
      GSHSCENE_Mask m = mask_new(pic, GSHSCENE_MASK_ALPHA, k % 2 == 0 ? GSHSCENE_MASK_ACTS_ON_SHAPE : GSHSCENE_MASK_ACTS_ON_OPACITY);
      const GSHSCENE_Resource * mk = k == 0 ? NULL : mask_intern(&m);
      GSHGEOM_Map corner = move_by(x, y);
      group_push(L, group_props(1.0, GSHSCENE_COMPOSITE_SRC_OVER));
      mask_subject(L, x, y);
      masked_push(L, mk, &corner, 1.0, k < 3 ? GSHSCENE_COMPOSITE_DEST_IN : GSHSCENE_COMPOSITE_SRC_OVER);
      fill(L, circle(x + TW / 2, y + TH / 2, 38), solid(0.95, 0.95, 0.95, 1));
      group_pop(L);
      group_pop(L);
      mask_probe("in disc", x, y, TW / 2, TH / 2);
      mask_probe("outside", x, y, 150, 20);
      NAME(4, k, nm[k]);
      gshscene_resource_release(mk);
    }
    gshscene_resource_release(pic);
  }

  /* ---- 5: the mask's placement. One picture, a star in a ring, in a box of 100 by 100, put in each tile by the group's mask map. */
  {
    const double top = TOP + 5 * SEC;
    GSHSCENE_List * PL = list_new();
    GSHGEOM_Path * star_path = path_new();
    for (int i = 0; i < 5; i++) {
      double a = -M_PI / 2 + i * 4 * M_PI / 5;
      GSHGEOM_Point q = P(50 + 32 * cos(a), 50 + 32 * sin(a));
      if (i == 0) {
        gshgeom_path_move_to(star_path, q);
      } else {
        gshgeom_path_line_to(star_path, q);
      }
    }
    gshgeom_path_close(star_path);
    fill_map(PL, intern(star_path), NULL, GSHGEOM_FILL_NONZERO, solid(1, 1, 1, 1));
    GSHGEOM_Path * ring = path_new();
    add_circle(ring, 50, 50, 47);
    add_circle(ring, 50, 50, 41);
    fill_map(PL, intern(ring), NULL, GSHGEOM_FILL_EVEN_ODD, solid(1, 1, 1, 0.7));
    const GSHSCENE_Resource * pic = picture_of(PL);
    GSHSCENE_Mask m = mask_new(pic, GSHSCENE_MASK_ALPHA, GSHSCENE_MASK_ACTS_ON_OPACITY);
    const GSHSCENE_Resource * mk = mask_intern(&m);
    GSHSCENE_Mask flat = m;
    flat.transfer.kind = GSHSCENE_TRANSFER_EXPONENTIAL;
    flat.transfer.c0 = 0.4;
    flat.transfer.c1 = 0.4;
    flat.transfer.n = 1.0;
    const GSHSCENE_Resource * mk_flat = mask_intern(&flat);
    const double mx = TW / 2, my = TH / 2, deg = M_PI / 180.0;
    GSHGEOM_Map placements[6] = {
        map_about(1, 0, 0, 1, 50, 50, mx, my),
        map_about(cos(30 * deg), -sin(30 * deg), sin(30 * deg), cos(30 * deg), 50, 50, mx, my),
        map_about(0.6 * cos(-20 * deg), -0.6 * sin(-20 * deg), 0.6 * sin(-20 * deg), 0.6 * cos(-20 * deg), 50, 50, mx, my),
        map_about(1.7, 0, 0, 0.7, 50, 50, mx, my),
        map_about(-1, 0, 0.35, 1, 50, 50, mx, my),
        map_about(1, 0, 0, 0, 50, 50, mx, my),
    };
    static const char * nm[6] = {"translation", "rotated by 30 degrees", "scaled 0.6, turned -20", "scaled 1.7 by 0.7", "mirrored, with a shear",
        "singular: the outside value"};
    for (int k = 0; k < 6; k++) {
      mask_tile_at(top, k, &x, &y);
      GSHGEOM_Map at = placements[k];
      /* the map is from the mask's space into the tile's: the tile's own origin is added by a leading translation */
      GSHGEOM_Map shift = move_by(x, y);
      GSHGEOM_Map chain[2] = {shift, at};
      GSHGEOM_Map placed;
      if (gshgeom_map_compose_chain(chain, 2, NULL, &placed) != GSHGEOM_OK) {
        fail("compose placement");
      }
      masked_tile(L, x, y, k == 5 ? mk_flat : mk, &placed, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("middle", x, y, mx, my);
      mask_probe("corner", x, y, 12, 12);
      NAME(5, k, nm[k]);
    }
    gshscene_resource_release(mk_flat);
    gshscene_resource_release(mk);
    gshscene_resource_release(pic);
  }

  /* ---- 6: outside the picture. Where it draws nothing the mask's value is the transfer of the backdrop's value: TR(0) for alpha,
   * TR of the backdrop's luminance for luminance, over the whole plane. */
  {
    const double top = TOP + 6 * SEC;
    const GSHSCENE_Resource * empty = picture_of(list_new());
    GSHSCENE_List * PL = list_new();
    fill(PL, circle(TW / 2, TH / 2, 32), solid(1, 1, 1, 1));
    const GSHSCENE_Resource * disc = picture_of(PL);
    static const double half_to_zero[2] = {0.5, 0.0};
    static const char * nm[5] = {"empty, TR(0) = 0.4", "disc, TR(0) = 0.4, TR(1) = 1", "empty, identity: nothing", "empty, luminance of BC .6",
        "luminance disc, table .5 to 0"};
    for (int k = 0; k < 5; k++) {
      mask_tile_at(top, k, &x, &y);
      GSHSCENE_Mask m = mask_new(k == 1 || k == 4 ? disc : empty, k >= 3 ? GSHSCENE_MASK_LUMINANCE : GSHSCENE_MASK_ALPHA,
          GSHSCENE_MASK_ACTS_ON_OPACITY);
      if (k <= 1) {
        m.transfer.kind = GSHSCENE_TRANSFER_EXPONENTIAL;
        m.transfer.c0 = 0.4;
        m.transfer.c1 = 1.0;
        m.transfer.n = 1.0;
      } else if (k == 3) {
        m.coefficients = GSHSCENE_MASK_COEFF_LUM;
        m.backdrop[0] = m.backdrop[1] = m.backdrop[2] = 0.6;
      } else if (k == 4) {
        m.transfer.kind = GSHSCENE_TRANSFER_TABLE;
        m.transfer.sample_count = 2;
        m.transfer.samples = half_to_zero;
      }
      const GSHSCENE_Resource * mk = mask_intern(&m);
      masked_tile(L, x, y, mk, NULL, 1.0, GSHSCENE_COMPOSITE_SRC_OVER);
      mask_probe("outside", x, y, 12, 12);
      mask_probe("middle", x, y, TW / 2, TH / 2 + 20);
      NAME(6, k, nm[k]);
      gshscene_resource_release(mk);
    }
    gshscene_resource_release(disc);
    gshscene_resource_release(empty);
  }
  render("masks (tiles)", L, R);
  {
    /* A refused or approximated item would leave a tile blank or wrong without failing the render; the page is for looking at what
     * the definition says, so either fails it. */
    GSHSCENE_Report_Summary sum;
    if (gshscene_report_summary(g_report, &sum) != GSHSCENE_OK || sum.by_class[GSHSCENE_ENTRY_REFUSED] ||
        sum.by_class[GSHSCENE_ENTRY_APPROXIMATED]) {
      fail("masks: an item was refused or approximated");
    }
  }

  /* Labels and the values read back. */
  L = list_new();
  label(L, "Masks: a group's layer is multiplied by a value rendered from a picture (shape-scene's mask), as shape-cpu defines it", 19, 30, 12, 0.1);
  label(L, "Each tile is one subject, a hue sweep with a dark bar and a yellow bar, in a group over a checkerboard; the label says the group's mask.", 14, 30, 40, 0.35);
  label(L, "The 8-bit values under a tile are read back from this image at the middle of a white square of the checkerboard (also in gallery.log).", 14, 30, 58, 0.35);
  static const char * title[7] = {
      "SVG: luminance (W3C coefficients) and alpha, and the picture rendered and its luminance read in linear light or in sRGB",
      "PDF soft masks: luminosity over a backdrop color BC, coefficients (30, 59, 11) / 100, then a transfer function",
      "CSS mask-composite: a stack of layers, bottom first, each composited onto those below (add SRC_OVER, subtract SRC_OUT, intersect SRC_IN, exclude XOR)",
      "A mask of a mask: the mask's picture holds a group that is itself masked",
      "What the mask multiplies: a white disc in a group composited DEST_IN onto the subject, under an alpha ramp acting on opacity or on shape",
      "Placement: one picture (a star in a ring, in a box of 100 by 100) put in the tile by the group's mask map",
      "Outside the picture: the value there is the transfer of the backdrop's value, everywhere the picture draws nothing"};
  for (int s = 0; s < 7; s++) {
    label(L, title[s], 14, 40, TOP + s * SEC, 0.1);
  }
  for (size_t i = 0; i < nn; i++) {
    mask_tile_at(TOP + names[i].section * SEC, names[i].col, &x, &y);
    label_c(L, names[i].text, 11, x + TW / 2, y + MASK_TH + 4, 0.15);
  }
  /* The probes: the printed lines of a tile stack under its label. */
  {
    double last_tx = -1, last_ty = -1;
    int line = 0;
    for (size_t i = 0; i < g_nprobe; i++) {
      const struct MaskProbe * p = &g_probe[i];
      const unsigned char * c = pixel(R, p->x, p->y);
      line = p->tx == last_tx && p->ty == last_ty ? line + 1 : 0;
      last_tx = p->tx;
      last_ty = p->ty;
      char buf[96];
      snprintf(buf, sizeof buf, "%.15s %d,%d,%d,%d", p->tag, c[0], c[1], c[2], c[3]);
      label_c(L, buf, 11, p->tx + MASK_TW / 2, p->ty + MASK_TH + 20 + 14 * line, 0.4);
      printf("masks probe (%d,%d) %s: %d,%d,%d,%d\n", p->x, p->y, p->tag, c[0], c[1], c[2], c[3]);
    }
  }
  render("masks (labels)", L, R);
  save_png(R, "masks.png");
  gimg_raster_destroy(R);
#undef NAME
}

/* --- 11: primitives: the exact arcs (shape-geometry A1) and the named primitives (A2) ------------------------- */

#define PTW 270.0 /* a tile's width; four across at a pitch of 290 */

static double g_py; /* the next section's top */
static struct PLabel {
  double cx, y;
  char text[100];
} g_plab[400];
static size_t g_nplab;
static struct PProbe {
  double cx, y;
  char tag[12];
  int px, py;
} g_ppr[80];
static size_t g_nppr;

/** A caption line (line 0, 1) under a tile whose bottom is at @p ybot, centered on @p cx. */
static void pcap(double cx, double ybot, int line, const char * fmt, ...) {
  if (g_nplab >= sizeof g_plab / sizeof g_plab[0]) {
    fail("too many captions");
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  struct PLabel * l = &g_plab[g_nplab++];
  vsnprintf(l->text, sizeof l->text, fmt, ap);
  va_end(ap);
  l->cx = cx;
  l->y = ybot + 4 + 14 * line;
}

/** A pixel to read back after the render and print on line @p line (counted after the two caption lines). */
static void pprobe(double cx, double ybot, int line, const char * tag, double px, double py) {
  if (g_nppr >= sizeof g_ppr / sizeof g_ppr[0]) {
    fail("too many probes");
    return;
  }
  struct PProbe * p = &g_ppr[g_nppr++];
  snprintf(p->tag, sizeof p->tag, "%s", tag);
  p->cx = cx;
  p->y = ybot + 4 + 14 * (2 + line);
  p->px = (int)floor(px);
  p->py = (int)floor(py);
}

/** A section: its title at the running y; returns the top of its tiles and moves the running y below the captions. */
static double psec(GSHSCENE_List * L, const char * title, double th) {
  label(L, title, 14, 40, g_py, 0.1);
  double top = g_py + 26;
  g_py = top + th + 86;
  return top;
}

/** Tile @p col of a section whose tiles start at @p top and are @p th high: a white card; returns its left edge. */
static double ptile(GSHSCENE_List * L, int col, double top, double th) {
  double ox = 40 + col * 290;
  fill_rect(L, ox - 1, top - 1, ox + PTW + 1, top + th + 1, solid(0.8, 0.8, 0.82, 1));
  fill_rect(L, ox, top, ox + PTW, top + th, solid(1, 1, 1, 1));
  return ox;
}

static void pdot(GSHSCENE_List * L, double x, double y, double r, GSHSCENE_Paint paint) {
  fill(L, circle(x, y, r), paint);
}

/** A line from (x0, y0) to (x1, y1), @p w wide. */
static void pline(GSHSCENE_List * L, double x0, double y0, double x1, double y1, double w, GSHSCENE_Paint paint) {
  GSHGEOM_Path * p = path_new();
  gshgeom_path_move_to(p, P(x0, y0));
  gshgeom_path_line_to(p, P(x1, y1));
  GSHSCENE_Stroke_Style st = style_new(w, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
  stroke_map(L, intern(p), NULL, &st, paint);
}

/** Fill @p res with @p f and stroke it with @p st in @p ink, under @p map (NULL: none). */
static void pfs(GSHSCENE_List * L, const GSHSCENE_Resource * res, const GSHGEOM_Map * map, GSHSCENE_Paint f,
    const GSHSCENE_Stroke_Style * st, GSHSCENE_Paint ink) {
  if (f.rgba[3] > 0) {
    fill_map(L, gshscene_resource_retain(res), map, GSHGEOM_FILL_NONZERO, f);
  }
  if (st) {
    stroke_map(L, gshscene_resource_retain(res), map, st, ink);
  }
  gshscene_resource_release(res);
}

static const GSHSCENE_Resource * prim_intern(const GSHSCENE_Primitive * pr) {
  const GSHSCENE_Resource * r = NULL;
  if (gshscene_store_intern_primitive(g_store, pr, &r) != GSHSCENE_OK) {
    fail("intern primitive");
  }
  return r;
}

static const GSHSCENE_Resource * prim_rect(double x, double y, double w, double h, int fx, int fy) {
  GSHSCENE_Primitive pr;
  memset(&pr, 0, sizeof pr);
  pr.kind = GSHSCENE_PRIMITIVE_RECT;
  pr.x = x, pr.y = y, pr.w = w, pr.h = h, pr.flip_x = fx, pr.flip_y = fy;
  return prim_intern(&pr);
}

static const GSHSCENE_Resource * prim_rrect(
    double x, double y, double w, double h, const GSHGEOM_Corner r[4], int fx, int fy) {
  GSHSCENE_Primitive pr;
  memset(&pr, 0, sizeof pr);
  pr.kind = GSHSCENE_PRIMITIVE_ROUNDED_RECT;
  pr.x = x, pr.y = y, pr.w = w, pr.h = h, pr.flip_x = fx, pr.flip_y = fy;
  memcpy(pr.r, r, sizeof pr.r);
  return prim_intern(&pr);
}

static const GSHSCENE_Resource * prim_ellipse(double cx, double cy, double rx, double ry, int fx, int fy) {
  GSHSCENE_Primitive pr;
  memset(&pr, 0, sizeof pr);
  pr.kind = GSHSCENE_PRIMITIVE_ELLIPSE;
  pr.cx = cx, pr.cy = cy, pr.rx = rx, pr.ry = ry, pr.flip_x = fx, pr.flip_y = fy;
  return prim_intern(&pr);
}

static GSHGEOM_Arc_Kind g_arc_kind;

/** SVG's `A rx ry phi large sweep end` (degrees) from the current point. */
static void arc_end(GSHGEOM_Path * p, GSHGEOM_Point e, double rx, double ry, double phi, int large, int sweep) {
  if (gshgeom_path_arc_endpoint_to(p, e, rx, ry, phi, GSHGEOM_ANGLE_DEGREES, large, sweep, &g_arc_kind) != GSHGEOM_OK) {
    fail("arc endpoint");
  }
}

/** The centre form about (cx, cy): radii, rotation @p phi and the start and sweep in @p unit; a subpath begins at P(t0). */
static void arc_centre(GSHGEOM_Path * p, double cx, double cy, double rx, double ry, double phi, GSHGEOM_Angle_Unit unit,
    double t0, double sweep) {
  if (gshgeom_path_arc_center_to(p, P(cx, cy), rx, ry, phi, unit, t0, sweep, NULL) != GSHGEOM_OK) {
    fail("arc centre");
  }
}

static GSHGEOM_Map map_scale_to(double sx, double sy, double tx, double ty) {
  return map_about(sx, 0, 0, sy, 0, 0, tx, ty);
}

static const char * kind_name(GSHGEOM_Arc_Kind k) {
  return k == GSHGEOM_ARC_KIND_ARC ? "an arc" : k == GSHGEOM_ARC_KIND_LINE ? "a line" : "nothing";
}

static void image_primitives(void) {
  const int W = 1200, H = 3220;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "primitives: exact arcs and named primitives, drawn by shape-cpu", 26, 30, 18, 0.1);
  g_py = 64;
  g_nplab = g_nppr = 0;
  const GSHSCENE_Paint ink = solid(0.1, 0.2, 0.45, 1);
  const GSHSCENE_Paint wash = solid(0.62, 0.76, 0.95, 0.6);
  const GSHSCENE_Paint solid_fill = solid(0.62, 0.76, 0.95, 1);
  const GSHSCENE_Paint guide = solid(0.9, 0.35, 0.1, 1);
  const GSHSCENE_Paint green = solid(0.1, 0.6, 0.2, 1);
  const GSHSCENE_Paint red = solid(0.85, 0.15, 0.15, 1);
  const GSHSCENE_Paint none = solid(0, 0, 0, 0);
  const GSHSCENE_Paint grey = solid(0.7, 0.7, 0.72, 1);
  GSHSCENE_Stroke_Style s5 = style_new(5, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_BUTT);
  GSHSCENE_Stroke_Style s6 = style_new(6, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
  GSHSCENE_Stroke_Style thin = style_new(1, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
  GSHSCENE_Stroke_Style s8 = style_new(8, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
  GSHSCENE_Stroke_Style s6r = style_new(6, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);

  /* A. Endpoint form, the four flag combinations on the same two points and radii. */
  {
    const double TH = 250;
    double top = psec(L, "SVG 2 endpoint form (A rx ry phi large sweep x y): the four flag combinations, one chord, rx = ry = 70", TH);
    for (int k = 0; k < 4; k++) {
      int large = k >> 1, sweep = k & 1;
      double ox = ptile(L, k, top, TH);
      GSHGEOM_Path * p = path_new();
      gshgeom_path_move_to(p, P(ox + 75, top + 130));
      arc_end(p, P(ox + 195, top + 130), 70, 70, 0, large, sweep);
      pfs(L, intern(p), NULL, wash, &s5, ink);
      pdot(L, ox + 75, top + 130, 6, green);
      pdot(L, ox + 195, top + 130, 6, red);
      pcap(ox + PTW / 2, top + TH, 0, "large-arc %d, sweep %d", large, sweep);
      pcap(ox + PTW / 2, top + TH, 1, "%s arc, %s on screen", large ? "the long" : "the short",
          sweep ? "clockwise" : "counter-clockwise");
    }
  }

  /* B. Endpoint form: the radius correction and the degenerate radii. */
  {
    const double TH = 210;
    double top = psec(L, "Endpoint form: radii too small are scaled up (SVG 2 F.6.6), a zero radius is a line, a rotation turns the axes", TH);
    double ox = ptile(L, 0, top, TH);
    GSHGEOM_Path * p = path_new();
    gshgeom_path_move_to(p, P(ox + 75, top + 130));
    arc_end(p, P(ox + 195, top + 130), 30, 30, 0, 0, 1);
    pfs(L, intern(p), NULL, wash, &s5, ink);
    pdot(L, ox + 75, top + 130, 6, green);
    pdot(L, ox + 195, top + 130, 6, red);
    pcap(ox + PTW / 2, top + TH, 0, "r = 30, chord 120: scaled to 60");
    pcap(ox + PTW / 2, top + TH, 1, "the half circle (%s)", kind_name(g_arc_kind));

    ox = ptile(L, 1, top, TH);
    p = path_new();
    gshgeom_path_move_to(p, P(ox + 60, top + 125));
    arc_end(p, P(ox + 210, top + 150), 110, 45, 30, 0, 1);
    pfs(L, intern(p), NULL, wash, &s5, ink);
    pdot(L, ox + 60, top + 125, 6, green);
    pdot(L, ox + 210, top + 150, 6, red);
    pcap(ox + PTW / 2, top + TH, 0, "rx 110, ry 45, phi 30 degrees");
    pcap(ox + PTW / 2, top + TH, 1, "a tilted ellipse, short arc, clockwise");

    ox = ptile(L, 2, top, TH);
    p = path_new();
    gshgeom_path_move_to(p, P(ox + 60, top + 150));
    arc_end(p, P(ox + 210, top + 70), 0, 60, 0, 0, 1);
    pfs(L, intern(p), NULL, wash, &s5, ink);
    pdot(L, ox + 60, top + 150, 6, green);
    pdot(L, ox + 210, top + 70, 6, red);
    pcap(ox + PTW / 2, top + TH, 0, "rx = 0: SVG says draw a line");
    pcap(ox + PTW / 2, top + TH, 1, "the call adds %s", kind_name(g_arc_kind));

    ox = ptile(L, 3, top, TH);
    p = path_new();
    gshgeom_path_move_to(p, P(ox + 60, top + 140));
    arc_end(p, P(ox + 210, top + 140), 60, 30, 0, 0, 1);
    pfs(L, intern(p), NULL, wash, &s5, ink);
    pdot(L, ox + 60, top + 140, 6, green);
    pdot(L, ox + 210, top + 140, 6, red);
    pcap(ox + PTW / 2, top + TH, 0, "rx 60, ry 30, chord 150: both x 1.25");
    pcap(ox + PTW / 2, top + TH, 1, "to 75 and 37.5, the ratio kept");
  }

  /* C. Centre form. */
  {
    const double TH = 230;
    double top = psec(L, "Centre form (Canvas arc and ellipse, DXF ARC): start, signed sweep, rotation, and the angle unit", TH);
    double ox = ptile(L, 0, top, TH);
    GSHGEOM_Path * p = path_new();
    arc_centre(p, ox + 135, top + 115, 75, 75, 0, GSHGEOM_ANGLE_DEGREES, 0, 360);
    gshgeom_path_close(p);
    pfs(L, intern(p), NULL, wash, &s5, ink);
    pdot(L, ox + 135 + 75, top + 115, 6, green);
    pcap(ox + PTW / 2, top + TH, 0, "t0 0, sweep 360 degrees");
    pcap(ox + PTW / 2, top + TH, 1, "one turn closes to the start, bit for bit");

    ox = ptile(L, 1, top, TH);
    p = path_new();
    arc_centre(p, ox + 135, top + 115, 75, 75, 0, GSHGEOM_ANGLE_DEGREES, -90, -270);
    pfs(L, intern(p), NULL, none, &s6r, ink);
    pdot(L, ox + 135, top + 40, 6, green);
    pdot(L, ox + 135 + 75, top + 115, 6, red);
    pcap(ox + PTW / 2, top + TH, 0, "t0 -90, sweep -270 degrees");
    pcap(ox + PTW / 2, top + TH, 1, "a negative sweep: counter-clockwise");

    ox = ptile(L, 2, top, TH);
    p = path_new();
    arc_centre(p, ox + 135, top + 115, 105, 45, 30, GSHGEOM_ANGLE_DEGREES, 0, 360);
    pfs(L, intern(p), NULL, none, &thin, grey);
    p = path_new();
    arc_centre(p, ox + 135, top + 115, 105, 45, 30, GSHGEOM_ANGLE_DEGREES, 20, 220);
    pfs(L, intern(p), NULL, none, &s6r, ink);
    pcap(ox + PTW / 2, top + TH, 0, "rx 105, ry 45, phi 30, t0 20, sweep 220");
    pcap(ox + PTW / 2, top + TH, 1, "the ellipse it lies on in grey");

    ox = ptile(L, 3, top, TH);
    static const struct {
      GSHGEOM_Angle_Unit u;
      double quarter, r;
      GSHSCENE_Paint c;
    } unit[4] = {{GSHGEOM_ANGLE_RADIANS, 1.5707963267948966, 25, {0}}, {GSHGEOM_ANGLE_DEGREES, 90, 45, {0}},
        {GSHGEOM_ANGLE_GRADIANS, 100, 65, {0}}, {GSHGEOM_ANGLE_TURNS, 0.25, 85, {0}}};
    const GSHSCENE_Paint hue[4] = {solid(0.8, 0.15, 0.15, 1), solid(0.9, 0.55, 0.05, 1), solid(0.1, 0.6, 0.2, 1), solid(0.15, 0.3, 0.8, 1)};
    pline(L, ox + 45, top + 45, ox + 45 + 110, top + 45, 1, grey);
    pline(L, ox + 45, top + 45, ox + 45, top + 45 + 110, 1, grey);
    for (int u = 0; u < 4; u++) {
      p = path_new();
      arc_centre(p, ox + 45, top + 45, unit[u].r, unit[u].r, 0, unit[u].u, 0, unit[u].quarter);
      pfs(L, intern(p), NULL, none, &s6, hue[u]);
    }
    pcap(ox + PTW / 2, top + TH, 0, "one quarter turn, four units, from the right");
    pcap(ox + PTW / 2, top + TH, 1, "radians, degrees, gradians, turns (in to out)");
  }

  /* D. Tangent form. */
  {
    const double TH = 210;
    double top = psec(L, "Tangent form (Canvas arcTo): a line to the first tangent point, then a circular arc toward the second point", TH);
    static const double radius[4] = {0, 30, 90, 40};
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH);
      GSHGEOM_Point p0 = P(ox + 30, top + 40), p1 = P(ox + 150, top + 160), p2 = P(ox + 240, top + 40);
      if (k == 3) {
        p2 = P(ox + 30 + 2 * 70, top + 40 + 2 * 70); /* on the line p0 p1, past p1 */
        p1 = P(ox + 30 + 70, top + 40 + 70);
      }
      pline(L, p0.x, p0.y, p1.x, p1.y, 1.5, guide);
      pline(L, p1.x, p1.y, p2.x, p2.y, 1.5, guide);
      GSHGEOM_Path * p = path_new();
      gshgeom_path_move_to(p, p0);
      if (gshgeom_path_arc_tangent_to(p, p1, p2, radius[k]) != GSHGEOM_OK) {
        fail("arc tangent");
      }
      gshgeom_path_line_to(p, p2);
      pfs(L, intern(p), NULL, none, &s5, ink);
      pdot(L, p0.x, p0.y, 5, green);
      pdot(L, p1.x, p1.y, 4, guide);
      pdot(L, p2.x, p2.y, 5, red);
      if (k == 3) {
        pcap(ox + PTW / 2, top + TH, 0, "r = 40 with the three points collinear");
        pcap(ox + PTW / 2, top + TH, 1, "no corner to round: a line");
      } else {
        pcap(ox + PTW / 2, top + TH, 0, "r = %g", radius[k]);
        pcap(ox + PTW / 2, top + TH, 1, k == 0 ? "radius 0: a line to the corner" : "orange: the two lines it rounds");
      }
    }
  }

  /* E. Arcs through a map and as a stroke. */
  {
    const double TH = 230;
    double top = psec(L, "An arc under a non-uniform map is an elliptical arc (the conic route); an arc as a stroke, with dashes", TH);
    /* local-space arc about the origin: a C shape, radius 55, 240 degrees from -210 */
    GSHSCENE_Stroke_Style wide = style_new(14, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    GSHSCENE_Stroke_Style dev = wide;
    dev.width_space = GSHSCENE_STROKE_WIDTH_DEVICE;
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH);
      GSHGEOM_Path * p = path_new();
      arc_centre(p, 0, 0, 55, 55, 0, GSHGEOM_ANGLE_DEGREES, k < 2 ? -210 : -120, k < 2 ? 240 : 300);
      const GSHSCENE_Resource * res = intern(p);
      if (k < 2) {
        GSHGEOM_Map m1 = k ? map_scale_to(0.5, 1.6, ox + 60, top + 115) : map_scale_to(1, 1, ox + 70, top + 115),
            m2 = k ? map_scale_to(1, 0.5, ox + 190, top + 115) : map_scale_to(0.5, 1.6, ox + 205, top + 115);
        pfs(L, gshscene_resource_retain(res), &m1, none, k ? &dev : &wide, ink);
        pfs(L, res, &m2, none, k ? &dev : &wide, ink);
        if (k == 0) {
          pcap(ox + PTW / 2, top + TH, 0, "the arc, stroked 14 wide, as it is and");
          pcap(ox + PTW / 2, top + TH, 1, "under scale (0.5, 1.6): a local-width pen");
        } else {
          pcap(ox + PTW / 2, top + TH, 0, "a device width of 14, under (0.5, 1.6) and (1, 0.5):");
          pcap(ox + PTW / 2, top + TH, 1, "uniform, whatever the map");
        }
      } else {
        double dash[2] = {22, 16};
        GSHSCENE_Stroke_Style d = style_new(9, GSHSCENE_STROKE_JOIN_ROUND, k == 2 ? GSHSCENE_STROKE_CAP_BUTT : GSHSCENE_STROKE_CAP_ROUND);
        d.dashes = dash;
        d.dash_count = 2;
        GSHGEOM_Map m = k == 2 ? map_scale_to(1.35, 1.35, ox + 135, top + 115) : map_scale_to(1.9, 0.6, ox + 135, top + 115);
        if (k == 3) {
          dash[0] = 0.01;
          dash[1] = 20;
          d.width = 11;
        }
        pfs(L, res, &m, none, &d, ink);
        double a = -120 * M_PI / 180;
        double sx = k == 2 ? 1.35 : 1.9, sy = k == 2 ? 1.35 : 0.6;
        pdot(L, ox + 135 + sx * 55 * cos(a), top + 115 + sy * 55 * sin(a), 5, green);
        if (k == 2) {
          pcap(ox + PTW / 2, top + TH, 0, "dashes 22 on 16 off along a 300 degree arc");
          pcap(ox + PTW / 2, top + TH, 1, "the pattern starts at the green dot");
        } else {
          pcap(ox + PTW / 2, top + TH, 0, "round dots every 20 along the arc,");
          pcap(ox + PTW / 2, top + TH, 1, "under scale (1.9, 0.6): dots follow the map");
        }
      }
    }
  }

  /* F. Rect and rounded rect. */
  {
    const double TH = 190;
    double top = psec(L, "rect and rounded rect (named primitives, exact lines and quarter-turn arcs), filled and stroked", TH);
    GSHGEOM_Corner round24[4] = {{24, 24}, {24, 24}, {24, 24}, {24, 24}};
    GSHGEOM_Corner ell[4] = {{50, 20}, {20, 50}, {50, 20}, {20, 50}};
    double ox = ptile(L, 0, top, TH);
    pfs(L, prim_rect(ox + 60, top + 45, 150, 100, 0, 0), NULL, solid_fill, &s6, ink);
    pcap(ox + PTW / 2, top + TH, 0, "rect 150 by 100");
    pcap(ox + PTW / 2, top + TH, 1, "four lines, no arcs");
    pprobe(ox + PTW / 2, top + TH, 0, "ctr", ox + 135, top + 95);
    pprobe(ox + PTW / 2, top + TH, 1, "corner", ox + 63, top + 48);

    ox = ptile(L, 1, top, TH);
    pfs(L, prim_rrect(ox + 60, top + 45, 150, 100, round24, 0, 0), NULL, solid_fill, &s6, ink);
    pcap(ox + PTW / 2, top + TH, 0, "rounded rect, every corner 24 by 24");
    pcap(ox + PTW / 2, top + TH, 1, "four circular quarter-turn arcs");
    pprobe(ox + PTW / 2, top + TH, 0, "ctr", ox + 135, top + 95);
    pprobe(ox + PTW / 2, top + TH, 1, "corner", ox + 63, top + 48);

    ox = ptile(L, 2, top, TH);
    pfs(L, prim_rrect(ox + 60, top + 45, 150, 100, ell, 0, 0), NULL, solid_fill, &s6, ink);
    pcap(ox + PTW / 2, top + TH, 0, "elliptical corners, in the order");
    pcap(ox + PTW / 2, top + TH, 1, "tl 50x20, tr 20x50, br 50x20, bl 20x50");
    pprobe(ox + PTW / 2, top + TH, 0, "ctr", ox + 135, top + 95);
    pprobe(ox + PTW / 2, top + TH, 1, "corner", ox + 63, top + 48);

    /* The front-end resolves radii that do not fit: the waist refuses them and never scales (CSS: by the least of side / sum). */
    ox = ptile(L, 3, top, TH);
    GSHGEOM_Corner big[4] = {{80, 80}, {80, 80}, {80, 80}, {80, 80}};
    GSHGEOM_Result unscaled = gshgeom_rounded_rect_check(150, 60, big);
    double f = fmin(150.0 / 160.0, 60.0 / 160.0);
    GSHGEOM_Corner scaled[4];
    for (int i = 0; i < 4; i++) {
      scaled[i].rx = big[i].rx * f;
      scaled[i].ry = big[i].ry * f;
    }
    GSHGEOM_Result fits = gshgeom_rounded_rect_check(150, 60, scaled);
    pfs(L, prim_rrect(ox + 60, top + 65, 150, 60, scaled, 0, 0), NULL, solid_fill, &s6, ink);
    pcap(ox + PTW / 2, top + TH, 0, "r 80 on 150 by 60: check says %s;", gshgeom_result_string(unscaled));
    pcap(ox + PTW / 2, top + TH, 1, "scaled by %g to 30 (%s): a stadium", f, gshgeom_result_string(fits));
    pprobe(ox + PTW / 2, top + TH, 0, "ctr", ox + 135, top + 95);
    pprobe(ox + PTW / 2, top + TH, 1, "corner", ox + 63, top + 68);
  }

  /* G. The flips of a rounded rect, as Chromium's roundRect with a negative size draws it. */
  {
    const double TH = 200;
    double top = psec(L, "rounded rect flips, as Chromium's roundRect draws a negative width or height: one list of radii (44, 8, 24, 0) in visiting order", TH);
    GSHGEOM_Corner r[4] = {{44, 44}, {8, 8}, {24, 24}, {0, 0}};
    static const char * const name[4] = {"no flip", "flip_x (width < 0)", "flip_y (height < 0)", "flip_x and flip_y"};
    static const int tl_index[4] = {0, 1, 3, 2};
    static const double pat[4] = {52, 12, 8, 12};
    GSHSCENE_Stroke_Style st = style_new(5, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    st.dashes = pat;
    st.dash_count = 4;
    for (int k = 0; k < 4; k++) {
      int fx = k & 1, fy = k >> 1;
      double ox = ptile(L, k, top, TH);
      double x = ox + 45, y = top + 45;
      pfs(L, prim_rrect(x, y, 180, 110, r, fx, fy), NULL, wash, &st, ink);
      pdot(L, x + r[tl_index[k]].rx, y, 6, green);
      pcap(ox + PTW / 2, top + TH, 0, "%s", name[k]);
      pcap(ox + PTW / 2, top + TH, 1, "%s, from the dot", fx == fy ? "clockwise" : "reversed");
      pprobe(ox + PTW / 2, top + TH, 0, "br", x + 178, y + 108);
    }
  }

  /* H. The flips of a rect and an ellipse. */
  {
    const double TH = 170;
    double top = psec(L, "rect and ellipse flips (Canvas's four sign cases): the dashed outline starts at the dot and runs the way the dashes lead", TH);
    static const char * const name[4] = {"no flip", "flip_x", "flip_y", "flip_x and flip_y"};
    static const double pat[4] = {40, 10, 6, 10};
    GSHSCENE_Stroke_Style st = style_new(5, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    st.dashes = pat;
    st.dash_count = 4;
    for (int k = 0; k < 4; k++) {
      int fx = k & 1, fy = k >> 1;
      double ox = ptile(L, k, top, TH);
      double x = ox + 18, y = top + 45, w = 105, h = 70;
      pfs(L, prim_rect(x, y, w, h, fx, fy), NULL, wash, &st, ink);
      pdot(L, fx ? x + w : x, fy ? y + h : y, 6, green);
      double cx = ox + 198, cy = top + 80, rx = 58, ry = 36;
      pfs(L, prim_ellipse(cx, cy, rx, ry, fx, fy), NULL, wash, &st, ink);
      pdot(L, fx ? cx - rx : cx + rx, cy, 6, green);
      pcap(ox + PTW / 2, top + TH, 0, "%s", name[k]);
      pcap(ox + PTW / 2, top + TH, 1, "%s", (fx != fy) ? "counter-clockwise" : "clockwise");
    }
  }

  /* I. Circles and ellipses. */
  {
    const double TH = 190;
    double top = psec(L, "circle and ellipse (four quarter-turn arcs each), filled and stroked", TH);
    double ox = ptile(L, 0, top, TH);
    double cx = ox + 135, cy = top + 95;
    pfs(L, prim_ellipse(cx, cy, 70, 70, 0, 0), NULL, solid_fill, &s8, ink);
    pcap(ox + PTW / 2, top + TH, 0, "circle, r 70, stroked 8 centered");
    pcap(ox + PTW / 2, top + TH, 1, "an ellipse whose radii are bit equal");
    pprobe(ox + PTW / 2, top + TH, 0, "ctr", cx, cy);
    pprobe(ox + PTW / 2, top + TH, 1, "rim", cx + 70, cy);

    ox = ptile(L, 1, top, TH);
    cx = ox + 135;
    pfs(L, prim_ellipse(cx, cy, 115, 50, 0, 0), NULL, solid_fill, NULL, ink);
    pcap(ox + PTW / 2, top + TH, 0, "ellipse, rx 115, ry 50, fill only");
    pcap(ox + PTW / 2, top + TH, 1, "the conic route: smooth, no facets");
    pprobe(ox + PTW / 2, top + TH, 0, "ctr", cx, cy);
    pprobe(ox + PTW / 2, top + TH, 1, "outside", cx + 117, cy);

    ox = ptile(L, 2, top, TH);
    cx = ox + 135;
    double dots[2] = {0.01, 17};
    GSHSCENE_Stroke_Style dot = style_new(8, GSHSCENE_STROKE_JOIN_ROUND, GSHSCENE_STROKE_CAP_ROUND);
    dot.dashes = dots;
    dot.dash_count = 2;
    pfs(L, prim_ellipse(cx, cy, 105, 60, 0, 0), NULL, none, &dot, ink);
    pcap(ox + PTW / 2, top + TH, 0, "ellipse, rx 105, ry 60, dots every 17");
    pcap(ox + PTW / 2, top + TH, 1, "dashes follow the true arc length");

    ox = ptile(L, 3, top, TH);
    cx = ox + 135;
    double pat[4] = {52, 12, 8, 12};
    GSHSCENE_Stroke_Style st = style_new(5, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
    st.dashes = pat;
    st.dash_count = 4;
    pfs(L, prim_ellipse(cx, cy, 70, 70, 0, 0), NULL, wash, &st, ink);
    pdot(L, cx + 70, cy, 6, green);
    pcap(ox + PTW / 2, top + TH, 0, "a circle's dashes start at 3 o'clock");
    pcap(ox + PTW / 2, top + TH, 1, "and run clockwise, as SVG 2's do");
  }

  /* J. Very large circles and ellipses: an edge cut from a tile. */
  {
    const double TH = 150;
    double top = psec(L, "very large circles and ellipses, each clipped to its tile: the device route keeps the edge smooth at any radius", TH);
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH);
      double mx = ox + 135, my = top + 75; /* the edge passes through here */
      const GSHSCENE_Resource * clip = rect(ox, top, ox + PTW, top + TH);
      if (gshscene_list_clip_push(L, clip, NULL, GSHGEOM_FILL_NONZERO, 0, 0.0) != GSHSCENE_OK) {
        fail("clip push");
      }
      gshscene_resource_release(clip);
      /* the unit normal, pointing into the circle: 20 degrees off the vertical for the first and third */
      double tilt = (k == 1 || k == 3 ? 0.0 : k == 2 ? -20.0 : 20.0) * M_PI / 180;
      double nx = sin(tilt), ny = cos(tilt);
      if (k == 3) {
        pfs(L, prim_ellipse(mx, my + 1500, 4000, 1500, 0, 0), NULL, solid_fill, &s8, ink);
        pcap(ox + PTW / 2, top + TH, 0, "ellipse rx 4000, ry 1500,");
        pcap(ox + PTW / 2, top + TH, 1, "filled and stroked 8");
      } else {
        double R = k == 1 ? 3000 : 100000;
        const GSHSCENE_Resource * c = prim_ellipse(mx + nx * R, my + ny * R, R, R, 0, 0);
        if (k == 2) {
          double dash[2] = {26, 14};
          GSHSCENE_Stroke_Style d = style_new(7, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
          d.dashes = dash;
          d.dash_count = 2;
          pfs(L, c, NULL, none, &d, ink);
          pcap(ox + PTW / 2, top + TH, 0, "circle r 100000 stroked 7, dashes 26 on 14");
          pcap(ox + PTW / 2, top + TH, 1, "off, on an arc 600000 long");
        } else if (k == 0) {
          pfs(L, c, NULL, solid_fill, NULL, ink);
          pcap(ox + PTW / 2, top + TH, 0, "circle r 100000, filled");
          pcap(ox + PTW / 2, top + TH, 1, "an edge 20 degrees off the horizontal");
        } else {
          pfs(L, c, NULL, solid_fill, &s8, ink);
          pcap(ox + PTW / 2, top + TH, 0, "circle r 3000, filled and stroked 8");
          pcap(ox + PTW / 2, top + TH, 1, "the edge sags 3 px over the tile");
        }
      }
      if (k == 0) {
        pprobe(ox + PTW / 2, top + TH, 0, "in", mx + 12 * nx, my + 12 * ny);
        pprobe(ox + PTW / 2, top + TH, 1, "out", mx - 12 * nx, my - 12 * ny);
      }
      if (k == 1) {
        pprobe(ox + PTW / 2, top + TH, 0, "in", mx, my + 14);
        pprobe(ox + PTW / 2, top + TH, 1, "out", mx, my - 14);
      }
      if (gshscene_list_clip_pop(L) != GSHSCENE_OK) {
        fail("clip pop");
      }
    }
  }

  render("primitives", L, R);

  /* The captions, and the pixels at the probes read back from what was drawn. */
  L = list_new();
  for (size_t i = 0; i < g_nplab; i++) {
    label_c(L, g_plab[i].text, 11, g_plab[i].cx, g_plab[i].y, 0.15);
  }
  for (size_t i = 0; i < g_nppr; i++) {
    const struct PProbe * p = &g_ppr[i];
    const unsigned char * c = pixel(R, p->px, p->py);
    char buf[64];
    snprintf(buf, sizeof buf, "%s %d,%d,%d,%d", p->tag, c[0], c[1], c[2], c[3]);
    label_c(L, buf, 11, p->cx, p->y, 0.4);
    printf("primitives probe (%d,%d) %s: %d,%d,%d,%d\n", p->px, p->py, p->tag, c[0], c[1], c[2], c[3]);
  }
  render("primitives (labels)", L, R);
  printf("primitives: page ends at y=%g of %d\n", g_py, H);
  save_png(R, "primitives.png");
  gimg_raster_destroy(R);
}

/* --- 12: patterns (masks-images-patterns story R2): a tile repeated on a lattice ------------------------------------- */

/** The map with the linear part R(deg) [[1, kx], [0, 1]] diag(sx, sy) and the translation (tx, ty): a scale, a skew along x, a
 *  rotation, in that order, then a move. */
static GSHGEOM_Map pmap(double tx, double ty, double deg, double kx, double sx, double sy) {
  const double c = cos(deg * M_PI / 180), s = sin(deg * M_PI / 180);
  const double m[4] = {c * sx, c * kx * sy - s * sy, s * sx, s * kx * sy + c * sy}, t[2] = {tx, ty};
  GSHGEOM_Map out;
  if (gshgeom_map_from_matrix(2, m, t, &out) != GSHGEOM_OK) {
    fail("pattern map");
  }
  return out;
}

/** A pattern over @p tile with the cell (0, 0, w, h), the steps @p xs and @p ys, the repeats and the map; a tint makes it uncolored. */
static const GSHSCENE_Resource * pat_make(const GSHSCENE_Resource * tile, double w, double h, double xs, double ys, int rx, int ry,
    GSHGEOM_Map map, const GSHSCENE_Paint * tint) {
  GSHSCENE_Pattern p;
  gshscene_pattern_init(&p);
  p.tile = tile;
  p.w = w;
  p.h = h;
  p.x_step = xs;
  p.y_step = ys;
  p.repeat_x = (uint32_t)rx;
  p.repeat_y = (uint32_t)ry;
  p.map = map;
  if (tint) {
    p.has_tint = 1;
    p.tint = *tint;
  }
  const GSHSCENE_Resource * r = NULL;
  GSHSCENE_Result e = gshscene_store_intern_pattern(g_store, &p, &r);
  if (e != GSHSCENE_OK) {
    fprintf(stderr, "gallery: intern pattern: %s\n", gshscene_result_string(e));
    g_failed = 1;
  }
  return r;
}

/** The paint of @p pat at the alpha multiplier @p alpha. */
static GSHSCENE_Paint pat_paint(const GSHSCENE_Resource * pat, double alpha) {
  GSHSCENE_Paint p;
  memset(&p, 0, sizeof p);
  p.kind = GSHSCENE_PAINT_PATTERN;
  p.resource = pat;
  p.rgba[3] = alpha;
  return p;
}

/** Fill @p shape with @p pat (the list keeps its own reference, so the caller's is released). */
static void pat_fill(GSHSCENE_List * L, const GSHSCENE_Resource * shape, const GSHSCENE_Resource * pat, double alpha) {
  fill(L, shape, pat_paint(pat, alpha));
  gshscene_resource_release(pat);
}

/** An asymmetric motif in a @p w by @p h cell, so that a rotation or a skew shows: a disc, a square and a bar, over a cell-filling
 *  background when @p background is set. */
static const GSHSCENE_Resource * tile_motif(double w, double h, int background) {
  GSHSCENE_List * T = list_new();
  if (background) {
    fill_rect(T, 0, 0, w, h, solid(0.99, 0.95, 0.78, 1));
  }
  fill(T, circle(0.3 * w, 0.3 * h, 0.17 * fmin(w, h)), solid(0.9, 0.35, 0.15, 1));
  fill_rect(T, 0.55 * w, 0.55 * h, 0.9 * w, 0.9 * h, solid(0.15, 0.3, 0.7, 1));
  fill_rect(T, 0.1 * w, 0.66 * h, 0.42 * w, 0.76 * h, solid(0.1, 0.6, 0.3, 1));
  return picture_of(T);
}

/** A star, a disc and four translucent dots in an @p n cell, in colors a tint discards. */
static const GSHSCENE_Resource * tile_star(double n) {
  GSHSCENE_List * T = list_new();
  fill(T, star(0.5 * n, 0.55 * n, 0.44 * n), solid(0.45, 0.2, 0.65, 1));
  fill(T, circle(0.5 * n, 0.55 * n, 0.12 * n), solid(1, 0.85, 0.1, 1));
  const double q[4][2] = {{0.12, 0.12}, {0.88, 0.12}, {0.12, 0.88}, {0.88, 0.88}};
  for (int i = 0; i < 4; i++) {
    fill(T, circle(q[i][0] * n, q[i][1] * n, 0.07 * n), solid(0.1, 0.5, 0.8, 0.5));
  }
  return picture_of(T);
}

/** Two discs that fill their cell, at @p alpha, the bigger one under the smaller. */
static const GSHSCENE_Resource * tile_disc(double n, double alpha) {
  GSHSCENE_List * T = list_new();
  fill(T, circle(0.5 * n, 0.5 * n, 0.5 * n), solid(0.2, 0.5, 0.8, alpha));
  fill(T, circle(0.5 * n, 0.5 * n, 0.25 * n), solid(0.95, 0.8, 0.2, alpha));
  return picture_of(T);
}

/** Two squares of a checker in a cell of @p n, in @p a and @p b. */
static const GSHSCENE_Resource * tile_check(double n, GSHSCENE_Paint a, GSHSCENE_Paint b) {
  GSHSCENE_List * T = list_new();
  fill_rect(T, 0, 0, n, n, a);
  fill_rect(T, 0, 0, 0.5 * n, 0.5 * n, b);
  fill_rect(T, 0.5 * n, 0.5 * n, n, n, b);
  return picture_of(T);
}

/** The frame of a positioning area, one pixel wide. */
static void pframe(GSHSCENE_List * L, double x0, double y0, double x1, double y1) {
  const GSHSCENE_Paint c = solid(0.45, 0.45, 0.5, 1);
  fill_rect(L, x0 - 1, y0 - 1, x1 + 1, y0, c);
  fill_rect(L, x0 - 1, y1, x1 + 1, y1 + 1, c);
  fill_rect(L, x0 - 1, y0, x0, y1, c);
  fill_rect(L, x1, y0, x1 + 1, y1, c);
}

static void image_patterns(void) {
  const int W = 1200, H = 2200;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "patterns: a tile repeated on a lattice, drawn by shape-cpu", 26, 30, 18, 0.1);
  g_py = 64;
  g_nplab = g_nppr = 0;
  const double TH = 190; /* every tile here is 270 by 190; its positioning area is the 250 by 170 inside a 10-pixel margin */
  const GSHSCENE_Paint steel = solid(0.1, 0.2, 0.45, 1);

  /* A. SVG: patternUnits and patternTransform. */
  {
    double top = psec(L, "SVG patterns: userSpaceOnUse, objectBoundingBox, patternTransform rotate and skewX", TH);
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH), bx = ox + 10, by = top + 10;
      const GSHSCENE_Resource * pat = NULL;
      const GSHSCENE_Resource * tile = NULL;
      const GSHSCENE_Resource * shape = rect(bx, by, bx + 250, by + 170);
      if (k == 0) {
        tile = tile_motif(40, 40, 1);
        pat = pat_make(tile, 40, 40, 40, 40, 1, 1, pmap(0, 0, 0, 0, 1, 1), NULL);
        pcap(ox + PTW / 2, top + TH, 0, "patternUnits=userSpaceOnUse, 40 x 40");
        pcap(ox + PTW / 2, top + TH, 1, "anchored at the origin, not the shape");
      } else if (k == 1) {
        tile = tile_motif(62.5, 40, 1);
        pat = pat_make(tile, 62.5, 40, 62.5, 40, 1, 1, pmap(bx, by, 0, 0, 1, 1), NULL);
        shape = rect(bx, by, bx + 250, by + 160);
        pcap(ox + PTW / 2, top + TH, 0, "objectBoundingBox, width 0.25 height 0.25");
        pcap(ox + PTW / 2, top + TH, 1, "4 x 4 cells of 62.5 x 40 fit the shape");
      } else if (k == 2) {
        tile = tile_motif(40, 40, 1);
        pat = pat_make(tile, 40, 40, 40, 40, 1, 1, pmap(ox + 135, top + 95, 20, 0, 1, 1), NULL);
        shape = circle(ox + 135, top + 95, 82);
        pcap(ox + PTW / 2, top + TH, 0, "patternTransform=rotate(20)");
        pcap(ox + PTW / 2, top + TH, 1, "about the circle's center");
      } else {
        tile = tile_motif(40, 40, 1);
        pat = pat_make(tile, 40, 40, 40, 40, 1, 1, pmap(bx, by, 0, tan(25 * M_PI / 180), 1.1, 1.1), NULL);
        shape = rounded_rect(bx, by, bx + 250, by + 170, 28);
        pcap(ox + PTW / 2, top + TH, 0, "patternTransform=skewX(25) scale(1.1)");
        pcap(ox + PTW / 2, top + TH, 1, "the cells are parallelograms");
      }
      gshscene_resource_release(tile);
      pat_fill(L, shape, pat, 1);
    }
  }

  /* B. CSS background-repeat. */
  {
    double top = psec(L, "CSS background-repeat: space, repeat, space no-repeat and round, in a 250 x 170 positioning area", TH);
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH), bx = ox + 10, by = top + 10;
      const GSHSCENE_Resource * tile = tile_motif(52, 52, 1);
      const GSHSCENE_Resource * pat = NULL;
      if (k == 0) {
        /* 4 whole tiles across 250 leave 42, shared by 3 gaps (14); 3 down 170 leave 14, shared by 2 (7) */
        pat = pat_make(tile, 52, 52, 66, 59, 1, 1, pmap(bx, by, 0, 0, 1, 1), NULL);
        pcap(ox + PTW / 2, top + TH, 0, "space space: 4 x 3 tiles of 52");
        pcap(ox + PTW / 2, top + TH, 1, "gaps of 14 and 7 between them");
      } else if (k == 1) {
        pat = pat_make(tile, 52, 52, 52, 52, 1, 1, pmap(bx, by, 0, 0, 1, 1), NULL);
        pcap(ox + PTW / 2, top + TH, 0, "repeat repeat: tiles of 52 abut");
        pcap(ox + PTW / 2, top + TH, 1, "the last ones are cut by the area");
      } else if (k == 2) {
        pat = pat_make(tile, 52, 52, 66, 52, 1, 0, pmap(bx, by, 0, 0, 1, 1), NULL);
        pcap(ox + PTW / 2, top + TH, 0, "space no-repeat: one row, gapped");
        pcap(ox + PTW / 2, top + TH, 1, "repeat_y 0 draws index 0 only");
      } else {
        pat = pat_make(tile, 52, 52, 52, 52, 1, 1, pmap(bx, by, 0, 0, 50.0 / 52, (170.0 / 3) / 52), NULL);
        pcap(ox + PTW / 2, top + TH, 0, "round round: 5 x 3 tiles stretched");
        pcap(ox + PTW / 2, top + TH, 1, "to 50 x 56.7 so that they fit");
      }
      gshscene_resource_release(tile);
      pat_fill(L, rect(bx, by, bx + 250, by + 170), pat, 1);
      pframe(L, bx, by, bx + 250, by + 170);
    }
  }

  /* C. PDF tiling patterns. */
  {
    double top = psec(L, "PDF tiling patterns: PaintType 1 (colored) and PaintType 2 (uncolored) with two tints and a translucent one", TH);
    const GSHSCENE_Resource * tile = tile_star(36);
    const GSHSCENE_Paint tint[3] = {solid(0.85, 0.1, 0.1, 1), solid(0.1, 0.35, 0.85, 1), solid(0.1, 0.5, 0.2, 0.5)};
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH), bx = ox + 10, by = top + 10;
      if (k == 3) {
        checker(L, bx, by, bx + 250, by + 170, 10);
      }
      const GSHSCENE_Resource * pat =
          pat_make(tile, 36, 36, 36, 36, 1, 1, pmap(bx, by, 0, 0, 1, 1), k ? &tint[k - 1] : NULL);
      pat_fill(L, rounded_rect(bx, by, bx + 250, by + 170, 24), pat, 1);
      if (k == 0) {
        pcap(ox + PTW / 2, top + TH, 0, "PaintType 1: the tile's own colors");
        pcap(ox + PTW / 2, top + TH, 1, "star, disc and translucent dots");
      } else if (k == 3) {
        pcap(ox + PTW / 2, top + TH, 0, "PaintType 2, tint (0.1, 0.5, 0.2) at 0.5");
        pcap(ox + PTW / 2, top + TH, 1, "the tile's alpha under the tint's");
      } else {
        pcap(ox + PTW / 2, top + TH, 0, "PaintType 2, the same tile");
        pcap(ox + PTW / 2, top + TH, 1, k == 1 ? "tint (0.85, 0.1, 0.1)" : "tint (0.1, 0.35, 0.85)");
      }
    }
    gshscene_resource_release(tile);
  }

  /* D. Rotated and skewed lattices. */
  {
    double top = psec(L, "Rotated, sheared and scaled lattices (cells of whole units, maps that are not whole pixels)", TH);
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH), bx = ox + 10, by = top + 10;
      const double n = k == 3 ? 12 : 24;
      const GSHSCENE_Resource * tile = tile_motif(n, n, 1);
      GSHGEOM_Map map = k == 0 ? pmap(bx + 125, by + 85, 30, 0, 1, 1)
          : k == 1             ? pmap(bx, by, 0, 0.5, 1, 1)
          : k == 2             ? pmap(bx + 125, by + 85, 20, 0, 1.6, 0.7)
                               : pmap(bx + 125, by + 85, 7, 0, 1, 1);
      const GSHSCENE_Resource * pat = pat_make(tile, n, n, n, n, 1, 1, map, NULL);
      gshscene_resource_release(tile);
      pat_fill(L, rect(bx, by, bx + 250, by + 170), pat, 1);
      pframe(L, bx, by, bx + 250, by + 170);
      static const char * const line0[4] = {"rotate 30", "shear x' = x + y / 2", "scale 1.6 x 0.7, then rotate 20", "rotate 7, cells of 12"};
      static const char * const line1[4] = {"cells of 24 about the middle", "steps (24, 0) and (12, 24)", "cells are rectangles, turned", "many small cells, no seam"};
      pcap(ox + PTW / 2, top + TH, 0, "%s", line0[k]);
      pcap(ox + PTW / 2, top + TH, 1, "%s", line1[k]);
    }
  }

  /* E. Overlapping cells. */
  {
    double top = psec(L, "Overlapping cells (a step smaller than the cell): the copies are composited in lattice order, later over earlier", TH);
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH), bx = ox + 10, by = top + 10;
      const GSHSCENE_Resource * tile = tile_disc(44, k == 1 ? 0.5 : k == 3 ? 0.7 : 1);
      const GSHSCENE_Resource * pat = NULL;
      if (k == 0 || k == 1) {
        pat = pat_make(tile, 44, 44, 28, 28, 1, 1, pmap(bx, by, 0, 0, 1, 1), NULL);
      } else if (k == 2) {
        pat = pat_make(tile, 44, 44, 26, 44, 1, 1, pmap(bx, by, 0, 0, 1, 1), NULL);
      } else {
        pat = pat_make(tile, 44, 44, 30, 30, 1, 1, pmap(bx + 125, by + 85, 25, 0, 1, 1), NULL);
      }
      gshscene_resource_release(tile);
      pat_fill(L, rect(bx, by, bx + 250, by + 170), pat, 1);
      pframe(L, bx, by, bx + 250, by + 170);
      static const char * const line0[4] = {"cell 44, step 28 x 28", "the same, discs at alpha 0.5", "cell 44, step 26 x 44", "step 30, rotate 25, alpha 0.7"};
      static const char * const line1[4] = {"each copy over the one before", "the overlaps are darker", "overlap along x only", "lattice order follows the map"};
      pcap(ox + PTW / 2, top + TH, 0, "%s", line0[k]);
      pcap(ox + PTW / 2, top + TH, 1, "%s", line1[k]);
    }
  }

  /* F. Nested patterns. */
  {
    double top = psec(L, "Nested patterns: a pattern painted inside a tile is evaluated for each copy of the outer one", TH);
    const GSHSCENE_Paint dark = solid(0.15, 0.25, 0.55, 1), light = solid(0.9, 0.93, 1, 1);
    const GSHSCENE_Resource * check_tile = tile_check(12, dark, light);
    const GSHSCENE_Paint blue_tint = solid(0.1, 0.4, 0.9, 1);
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH), bx = ox + 10, by = top + 10;
      /* the inner pattern, in the outer tile's space */
      const GSHSCENE_Resource * inner = pat_make(check_tile, 12, 12, 12, 12, 1, 1, pmap(0, 0, 0, 0, 1, 1), k == 3 ? &blue_tint : NULL);
      GSHSCENE_List * T = list_new();
      pat_fill(T, rounded_rect(4, 4, 56, 56, 9), inner, 1);
      fill(T, circle(30, 30, 13), solid(0.9, 0.3, 0.2, 1));
      fill(T, star(30, 30, 9), solid(1, 0.9, 0.3, 1));
      const GSHSCENE_Resource * tile = picture_of(T);
      const GSHSCENE_Resource * pat = NULL;
      if (k == 2) {
        pat = pat_make(tile, 60, 60, 76, 76, 1, 1, pmap(bx + 4, by + 4, 0, 0, 1, 1), NULL);
      } else {
        pat = pat_make(tile, 60, 60, 60, 60, 1, 1, k == 1 ? pmap(bx + 125, by + 85, 25, 0, 1, 1) : pmap(bx, by, 0, 0, 1, 1), NULL);
      }
      gshscene_resource_release(tile);
      pat_fill(L, rect(bx, by, bx + 250, by + 170), pat, 1);
      pframe(L, bx, by, bx + 250, by + 170);
      static const char * const line0[4] = {"a checker inside a cell of 60", "the outer lattice rotated 25", "the outer cells 60, step 76", "the inner pattern uncolored"};
      static const char * const line1[4] = {"under a disc and a star", "the inner one turns with it", "gaps between whole cells", "tint (0.1, 0.4, 0.9)"};
      pcap(ox + PTW / 2, top + TH, 0, "%s", line0[k]);
      pcap(ox + PTW / 2, top + TH, 1, "%s", line1[k]);
    }
    gshscene_resource_release(check_tile);
  }

  /* G. Hatch: strokes at the cell edge. */
  {
    double top = psec(L, "Hatch: a stroke that ends at the cell edge, or crosses it and is cut by it, leaves no seam", TH);
    for (int k = 0; k < 4; k++) {
      double ox = ptile(L, k, top, TH), bx = ox + 10, by = top + 10;
      static const double cell[4] = {14.3, 12.7, 16, 20};
      const double n = cell[k];
      GSHSCENE_List * T = list_new();
      if (k == 0) {
        pline(T, 0, n / 2, n, n / 2, 3, steel);
      } else if (k == 1) {
        pline(T, -4, n + 4, n + 4, -4, 2.4, solid(0.7, 0.2, 0.1, 1));
      } else if (k == 2) {
        pline(T, -4, n + 4, n + 4, -4, 2, solid(0.1, 0.5, 0.25, 1));
        pline(T, -4, -4, n + 4, n + 4, 2, solid(0.1, 0.5, 0.25, 1));
      } else {
        GSHGEOM_Path * p = path_new();
        add_rect(p, 0, 0, n, n);
        GSHSCENE_Stroke_Style st = style_new(3, GSHSCENE_STROKE_JOIN_MITER, GSHSCENE_STROKE_CAP_BUTT);
        stroke_map(T, intern(p), NULL, &st, solid(0.35, 0.35, 0.5, 1));
        fill(T, circle(n / 2, n / 2, 3), solid(0.9, 0.35, 0.15, 1));
      }
      const GSHSCENE_Resource * tile = picture_of(T);
      const GSHSCENE_Resource * pat = pat_make(tile, n, n, n, n, 1, 1, pmap(bx, by, 0, 0, 1, 1), NULL);
      gshscene_resource_release(tile);
      pat_fill(L, rect(bx, by, bx + 250, by + 170), pat, 1);
      pframe(L, bx, by, bx + 250, by + 170);
      static const char * const line0[4] = {"horizontal lines, cell 14.3", "diagonals across the edge, 12.7", "crosshatch, cell 16", "lines on the cell edges, cell 20"};
      static const char * const line1[4] = {"butt ends meet on the shared edge", "each stroke is cut by its cell", "two strokes in one tile", "half a line per cell, the halves meet"};
      pcap(ox + PTW / 2, top + TH, 0, "%s", line0[k]);
      pcap(ox + PTW / 2, top + TH, 1, "%s", line1[k]);
      if (k == 0) {
        const double seam = bx + 3 * n, row = by + 2.5 * n;
        pprobe(ox + PTW / 2, top + TH, 0, "seam left", seam - 1, row);
        pprobe(ox + PTW / 2, top + TH, 1, "seam right", seam + 1, row);
        pprobe(ox + PTW / 2, top + TH, 2, "mid cell", seam + 7, row);
      } else if (k == 3) {
        const double row = by + 2.5 * n;
        pprobe(ox + PTW / 2, top + TH, 0, "on the edge", bx + 3 * n - 1, row);
        pprobe(ox + PTW / 2, top + TH, 1, "on the edge", bx + 3 * n, row);
        pprobe(ox + PTW / 2, top + TH, 2, "inside", bx + 3 * n + 8, row);
      }
    }
  }

  render("patterns", L, R);

  /* The captions, and the pixels at the probes read back from what was drawn. */
  L = list_new();
  for (size_t i = 0; i < g_nplab; i++) {
    label_c(L, g_plab[i].text, 11, g_plab[i].cx, g_plab[i].y, 0.15);
  }
  for (size_t i = 0; i < g_nppr; i++) {
    const struct PProbe * p = &g_ppr[i];
    const unsigned char * c = pixel(R, p->px, p->py);
    char buf[64];
    snprintf(buf, sizeof buf, "%s %d,%d,%d,%d", p->tag, c[0], c[1], c[2], c[3]);
    label_c(L, buf, 11, p->cx, p->y, 0.4);
    printf("patterns probe (%d,%d) %s: %d,%d,%d,%d\n", p->px, p->py, p->tag, c[0], c[1], c[2], c[3]);
  }
  render("patterns (labels)", L, R);
  printf("patterns: page ends at y=%g of %d\n", g_py, H);
  save_png(R, "patterns.png");
  gimg_raster_destroy(R);
}


/* --- 13: union clips (shapes story A3): the region of several shapes, summed in exact area ------------------------------------ */

/** A union of @p n members (each released here: the union holds its own references), under the rules @p rules. */
static const GSHSCENE_Resource * union_of(const GSHSCENE_Resource ** res, const int * rules, size_t n) {
  GSHSCENE_Clip_Member m[16];
  memset(m, 0, sizeof m);
  for (size_t i = 0; i < n && i < 16; i++) {
    m[i].resource = res[i];
    m[i].rule = rules ? (GSHGEOM_Fill_Rule)rules[i] : GSHGEOM_FILL_NONZERO;
  }
  const GSHSCENE_Resource * u = NULL;
  if (gshscene_store_intern_clip_union(g_store, m, n, &u) != GSHSCENE_OK) {
    fail("intern clip union");
    exit(1);
  }
  for (size_t i = 0; i < n; i++) {
    gshscene_resource_release(res[i]);
  }
  return u;
}

/** A fill of @p what under the clip @p u (the union keeps its reference for the next use). */
static void clipped(GSHSCENE_List * L, const GSHSCENE_Resource * u, const GSHSCENE_Resource * what, GSHSCENE_Paint paint) {
  if (gshscene_list_clip_push(L, u, NULL, GSHGEOM_FILL_NONZERO, 0, 0.0) != GSHSCENE_OK) {
    fail("clip push of a union");
    return;
  }
  fill(L, what, paint);
  if (gshscene_list_clip_pop(L) != GSHSCENE_OK) {
    fail("clip pop");
  }
}

static void image_clip_unions(void) {
  const int W = 1200, H = 760;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "union clips: the exact area of the union, quantized once", 26, 30, 18, 0.1);
  const GSHSCENE_Paint ink = solid(0.1, 0.2, 0.45, 1);
  const GSHSCENE_Paint hot = solid(0.85, 0.2, 0.15, 1);
  const GSHSCENE_Paint green = solid(0.1, 0.55, 0.25, 1);
  /* A. The seam: two shapes share an edge at x = 100.5.  As the union of one clip the shared pixel column is whole; as two
   * separate clips (the way an alpha mask of the two would compose them) the column carries a seam of 3/4. */
  {
    label(L, "A. abutting at x = 100.5: one union clip (left), two clips (right)", 14, 30, 60, 0.2);
    const GSHSCENE_Resource * halves[2] = {rect(30, 90, 100.5, 190), rect(100.5, 90, 170, 190)};
    const GSHSCENE_Resource * u = union_of(halves, NULL, 2);
    clipped(L, u, rect(20, 80, 180, 200), ink);
    gshscene_resource_release(u);
    for (int k = 0; k < 2; k++) {
      const GSHSCENE_Resource * one[1] = {k ? rect(100.5 + 250, 90, 170 + 250, 190) : rect(30 + 250, 90, 100.5 + 250, 190)};
      const GSHSCENE_Resource * su = union_of(one, NULL, 1);
      clipped(L, su, rect(270, 80, 430, 200), ink);
      gshscene_resource_release(su);
    }
    label(L, "no seam", 12, 100, 215, 0.3);
    label(L, "a 3/4 seam at the join", 12, 350, 215, 0.3);
  }
  /* B. An icon sheet: each icon is clipped to a union of a circle, a rectangle and a rounded rectangle that do not overlap. */
  {
    label(L, "B. icons: a circle, a bar and a rounded square, apart, clipped as one", 14, 560, 60, 0.2);
    for (int i = 0; i < 12; i++) {
      const double ox = 545 + 105 * (i % 6), oy = 85 + 105 * (i / 6);
      const GSHSCENE_Resource * parts[3] = {circle(ox + 20, oy + 20, 15), rect(ox + 45, oy + 8, ox + 90, oy + 22), rounded_rect(ox + 8, oy + 52, ox + 70, oy + 92, 9)};
      const GSHSCENE_Resource * u = union_of(parts, NULL, 3);
      clipped(L, u, rect(ox, oy, ox + 100, oy + 100), solid(0.2 + 0.06 * i, 0.35, 0.8 - 0.05 * i, 1));
      gshscene_resource_release(u);
    }
  }
  /* C. Nested: a union inside a circle's clip block. */
  {
    label(L, "C. a union inside a clip block: the product of the coverages", 14, 30, 270, 0.2);
    const GSHSCENE_Resource * parts[3] = {rect(30, 300, 130, 400), rect(130, 300, 230, 400), rect(230, 300, 330, 400)};
    const GSHSCENE_Resource * u = union_of(parts, NULL, 3);
    const GSHSCENE_Resource * ring = circle(180, 350, 78);
    if (gshscene_list_clip_push(L, ring, NULL, GSHGEOM_FILL_NONZERO, 0, 0.0) != GSHSCENE_OK) {
      fail("outer clip");
    }
    gshscene_resource_release(ring);
    clipped(L, u, rect(20, 290, 340, 410), green);
    gshscene_list_clip_pop(L);
    gshscene_resource_release(u);
  }
  /* D. Each member under its own rule: a nonzero star and an even-odd ring, apart. */
  {
    label(L, "D. each member by its own rule: a ring (even-odd) beside a plain square", 14, 30, 450, 0.2);
    GSHGEOM_Path * ringp = path_new();
    add_circle(ringp, 110, 540, 60);
    add_circle(ringp, 110, 540, 32);
    const GSHSCENE_Resource * parts[2] = {intern(ringp), rect(200, 480, 300, 600)};
    const int rules[2] = {GSHGEOM_FILL_EVEN_ODD, GSHGEOM_FILL_NONZERO};
    const GSHSCENE_Resource * u = union_of(parts, rules, 2);
    clipped(L, u, rect(30, 470, 320, 610), hot);
    gshscene_resource_release(u);
  }
  /* E. An overlapping union: the sink draws only disjoint unions, so it is refused and nothing inside is drawn. */
  {
    label(L, "E. members that overlap: refused (union-overlap), nothing drawn inside the clip", 14, 540, 330, 0.2);
    fill_rect(L, 550, 360, 700, 480, solid(0.5, 0.5, 0.55, 0.25)); /* the two members, drawn plainly, so that what is refused can be seen */
    fill_rect(L, 650, 420, 800, 540, solid(0.5, 0.5, 0.55, 0.25));
    const GSHSCENE_Resource * parts[2] = {rect(550, 360, 700, 480), rect(650, 420, 800, 540)};
    const GSHSCENE_Resource * u = union_of(parts, NULL, 2);
    clipped(L, u, rect(540, 350, 820, 560), hot);
    gshscene_resource_release(u);
  }
  render("clip unions", L, R);
  save_png(R, "clip-unions.png");
  gimg_raster_destroy(R);
}

/* --- 14: hatches (masks-images-patterns story H1): families of lines trimmed to a region, dashed and stroked ---------------- */

static const GSHSCENE_Resource * hpen(double width, int device, uint32_t cap) {
  GSHSCENE_Stroke_Style st;
  gshscene_stroke_style_init(&st);
  st.width = width;
  st.width_space = device ? GSHSCENE_STROKE_WIDTH_DEVICE : GSHSCENE_STROKE_WIDTH_LOCAL;
  st.cap = cap;
  const GSHSCENE_Resource * r = NULL;
  if (gshscene_store_intern_stroke_style(g_store, &st, &r) != GSHSCENE_OK) {
    fail("hatch pen");
  }
  return r;
}

/** A hatch of the families @p f over the pen, with the nesting and the ink; the families' elements are copied by the store. */
static const GSHSCENE_Resource * hmake(const GSHSCENE_Resource * pen, uint32_t nesting, const GSHSCENE_Hatch_Family * f, size_t n,
    GSHGEOM_Map map, GSHSCENE_Paint ink) {
  GSHSCENE_Hatch h;
  gshscene_hatch_init(&h);
  h.style = pen;
  h.nesting = nesting;
  h.family_count = n;
  h.families = f;
  h.map = map;
  h.ink = ink;
  const GSHSCENE_Resource * r = NULL;
  GSHSCENE_Result e = gshscene_store_intern_hatch(g_store, &h, &r);
  if (e != GSHSCENE_OK) {
    fprintf(stderr, "gallery: intern hatch: %s\n", gshscene_result_string(e));
    g_failed = 1;
  }
  return r;
}

static GSHSCENE_Paint hpaint(const GSHSCENE_Resource * hatch, double alpha) {
  GSHSCENE_Paint p;
  memset(&p, 0, sizeof p);
  p.kind = GSHSCENE_PAINT_HATCH;
  p.resource = hatch;
  p.rgba[3] = alpha;
  return p;
}

/** The region of a hatch tile: a frame, and the shape moved to the tile. */
static void hpanel(GSHSCENE_List * L, double x, double y, const char * caption, const GSHSCENE_Resource * region, GSHGEOM_Fill_Rule rule,
    const GSHSCENE_Resource * hatch, double alpha, const GSHGEOM_Map * extra) {
  pframe(L, x, y, x + 270, y + 190);
  GSHGEOM_Map at;
  gshgeom_map_translate(2, x + 10, y + 10, 0.0, &at);
  if (extra) {
    GSHGEOM_Map both;
    gshgeom_map_compose(&at, extra, &both);
    at = both;
  }
  gshscene_resource_retain(region); /* fill_map releases what it is given, and a region is used by several panels */
  fill_map(L, region, &at, rule, hpaint(hatch, alpha));
  label(L, caption, 11, x, y + 206, 0.15);
}

static const GSHSCENE_Resource * hrings(double w, double h, int levels) {
  GSHGEOM_Path * p = path_new();
  double d = 0.0;
  for (int i = 0; i < levels; i++) {
    const double a = d, b = d, e = w - d, f = h - d;
    gshgeom_path_move_to(p, P(a, b));
    if (i == 1) {
      gshgeom_path_line_to(p, P(a, f));
      gshgeom_path_line_to(p, P(e, f));
      gshgeom_path_line_to(p, P(e, b));
    } else {
      gshgeom_path_line_to(p, P(e, b));
      gshgeom_path_line_to(p, P(e, f));
      gshgeom_path_line_to(p, P(a, f));
    }
    gshgeom_path_close(p);
    d += 34.0;
  }
  return intern(p);
}

static void image_hatches(void) {
  const int W = 1200, H = 960;
  GIMG_Raster * R = raster_new(W, H);
  GSHSCENE_List * L = list_new();
  background(L, W, H, 0.97);
  label(L, "hatches: families of lines trimmed to a region, dashed and stroked", 26, 30, 16, 0.1);
  GSHGEOM_Map id;
  gshgeom_map_identity(2, &id);
  const GSHSCENE_Resource *box = rect(0, 0, 250, 170), *rings = hrings(250, 170, 3);
  const GSHSCENE_Resource *thin = hpen(2.0, 0, GSHSCENE_STROKE_CAP_BUTT), *round = hpen(3.5, 0, GSHSCENE_STROKE_CAP_ROUND);
  const GSHSCENE_Resource *square = hpen(3.0, 0, GSHSCENE_STROKE_CAP_SQUARE), *hairline = hpen(1.0, 1, GSHSCENE_STROKE_CAP_BUTT);
  static const double dash[2] = {18.0, -8.0}, dots[2] = {0.0, -12.0}, brick[2] = {26.0, -6.0};
  const GSHSCENE_Paint blue = solid(0.1, 0.25, 0.6, 1), red = solid(0.75, 0.12, 0.1, 1), green = solid(0.1, 0.5, 0.2, 1),
                       ink = solid(0.1, 0.1, 0.1, 1), purple = solid(0.45, 0.15, 0.55, 1), amber = solid(0.85, 0.45, 0.0, 1);
  const double x0 = 20, y0 = 56, dx = 295, dy = 218;
  GSHSCENE_Hatch_Family one[1] = {{45.0, 0.0, 0.0, 0.0, 14.0, 0, NULL}};
  GSHSCENE_Hatch_Family cross[2] = {{45.0, 0.0, 0.0, 0.0, 16.0, 0, NULL}, {135.0, 0.0, 0.0, 0.0, 16.0, 0, NULL}};
  GSHSCENE_Hatch_Family dashed[1] = {{0.0, 0.0, 4.0, 0.0, 14.0, 2, dash}};
  GSHSCENE_Hatch_Family dotted[1] = {{0.0, 0.0, 4.0, 0.0, 14.0, 2, dots}};
  GSHSCENE_Hatch_Family bricks[1] = {{0.0, 0.0, 4.0, 16.0, 14.0, 2, brick}};
  GSHSCENE_Hatch_Family flat[1] = {{0.0, 0.0, 0.0, 0.0, 17.0, 0, NULL}};
  /* row 0: one family, a translucent crosshatch (one union: the crossings are not darker), dashes, dots */
  hpanel(L, x0, y0, "one 45 degree family", box, GSHGEOM_FILL_NONZERO, hmake(thin, 0, one, 1, id, blue), 1.0, NULL);
  hpanel(L, x0 + dx, y0, "a crosshatch at alpha 0.5: one union, no darker crossings", box, GSHGEOM_FILL_NONZERO,
      hmake(thin, 0, cross, 2, id, red), 0.5, NULL);
  hpanel(L, x0 + 2 * dx, y0, "dashes 18, -8 (the pattern runs without a break)", box, GSHGEOM_FILL_NONZERO, hmake(square, 0, dashed, 1, id, green), 1.0, NULL);
  hpanel(L, x0 + 3 * dx, y0, "dots 0, -12 under a round cap", box, GSHGEOM_FILL_NONZERO, hmake(round, 0, dotted, 1, id, purple), 1.0, NULL);
  /* row 1: a brick stagger, and the three nesting styles over three nested squares */
  hpanel(L, x0, y0 + dy, "delta-x 16: a brick stagger", box, GSHGEOM_FILL_NONZERO, hmake(thin, 0, bricks, 1, id, ink), 1.0, NULL);
  hpanel(L, x0 + dx, y0 + dy, "three nested squares, nonzero (a ring, a gap, a center)", rings, GSHGEOM_FILL_NONZERO, hmake(thin, 0, flat, 1, id, blue), 1.0, NULL);
  hpanel(L, x0 + 2 * dx, y0 + dy, "the outermost ring only (DXF Outer)", rings, GSHGEOM_FILL_NONZERO, hmake(thin, 1, flat, 1, id, red), 1.0, NULL);
  hpanel(L, x0 + 3 * dx, y0 + dy, "everything inside the outer loop (DXF Ignore)", rings, GSHGEOM_FILL_NONZERO, hmake(thin, 2, flat, 1, id, green), 1.0, NULL);
  /* row 2: a curved boundary with an island and each pen */
  GSHGEOM_Path * disc = path_new();
  add_circle(disc, 125, 85, 80);
  add_circle(disc, 125, 85, 30);
  const GSHSCENE_Resource * ring = intern(disc);
  GSHSCENE_Hatch_Family curved[1] = {{60.0, 0.0, 0.0, 0.0, 12.0, 0, NULL}};
  const GSHSCENE_Resource * pens[4] = {hairline, hpen(5.0, 0, GSHSCENE_STROKE_CAP_BUTT), hpen(5.0, 0, GSHSCENE_STROKE_CAP_ROUND), hpen(5.0, 0, GSHSCENE_STROKE_CAP_SQUARE)};
  const char * names[4] = {"a hairline (device width 1)", "thick, butt caps", "thick, round caps", "thick, square caps"};
  for (int i = 0; i < 4; i++) {
    hpanel(L, x0 + dx * i, y0 + 2 * dy, names[i], ring, GSHGEOM_FILL_EVEN_ODD, hmake(pens[i], 0, curved, 1, id, i == 0 ? ink : amber), 1.0, NULL);
  }
  /* row 3: a hatch under a rotation and under a non-uniform scale */
  GSHGEOM_Map rot, wide;
  gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, 30.0, &rot);
  gshgeom_map_scale(2, 1.0, 2.2, 1.0, &wide);
  GSHSCENE_Hatch_Family fam2[2] = {{0.0, 0.0, 0.0, 0.0, 12.0, 2, dash}, {90.0, 0.0, 0.0, 0.0, 30.0, 0, NULL}};
  hpanel(L, x0, y0 + 3 * dy, "the hatch map rotated 30 degrees", box, GSHGEOM_FILL_NONZERO, hmake(thin, 0, fam2, 2, rot, blue), 1.0, NULL);
  hpanel(L, x0 + dx, y0 + 3 * dy, "the hatch map stretched 2.2 in y", box, GSHGEOM_FILL_NONZERO, hmake(thin, 0, fam2, 2, wide, red), 1.0, NULL);
  GSHGEOM_Map item_rot, item_move;
  gshgeom_map_rotate(GSHGEOM_ANGLE_DEGREES, 12.0, &item_rot);
  gshgeom_map_translate(2, 20.0, 5.0, 0.0, &item_move);
  GSHGEOM_Map item;
  gshgeom_map_compose(&item_move, &item_rot, &item);
  hpanel(L, x0 + 2 * dx, y0 + 3 * dy, "the item turned 12 degrees: the lines turn with it", box, GSHGEOM_FILL_NONZERO, hmake(thin, 0, one, 1, id, green), 1.0, &item);
  hpanel(L, x0 + 3 * dx, y0 + 3 * dy, "a round-capped crosshatch at alpha 0.5 over the nested squares", rings, GSHGEOM_FILL_NONZERO, hmake(round, 0, cross, 2, id, purple), 0.5, NULL);
  gshscene_resource_release(box);
  gshscene_resource_release(rings);
  gshscene_resource_release(ring);
  render("hatches", L, R);
  const unsigned char * hole = pixel(R, (int)(x0 + dx * 2 + 10 + 125), (int)(y0 + dy + 10 + 85));
  printf("hatches: the center of the outermost-ring tile reads %d,%d,%d,%d\n", hole[0], hole[1], hole[2], hole[3]);
  save_png(R, "hatches.png");
  gimg_raster_destroy(R);
}

int main(int argc, char ** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: gallery FONTDIR DEJAVU OUTDIR\n");
    return 2;
  }
  g_out = argv[3];
  /* The sink's limits are its defaults: a line of text stroked wide is a dense stroke (the inner sides of every curve and counter
   * overlap in a pixel), which the default of `max_exact_fragments` and `max_exact_crossings` draws with room to spare. */
  GSHCPU_Limits cpu_limits;
  gshcpu_limits_default(&cpu_limits);
  if (gshscene_store_create(NULL, NULL, &g_store) != GSHSCENE_OK ||
      gshcpu_sink_create(NULL, NULL, &cpu_limits, &g_cpu) != GSHCPU_OK ||
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
  image_images();
  image_masks();
  image_strokes();
  image_primitives();
  image_patterns();
  image_clip_unions();
  image_hatches();
  image_text(argv[1]);
  image_color_glyphs(argv[1]);
  printf("gallery: %s\n", g_failed ? "FAILED" : "ok");
  return g_failed;
}
