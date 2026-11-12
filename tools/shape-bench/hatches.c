/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Shape CPU.
 *
 * Ghoti.io Shape CPU is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 */

/*
 * The hatches scene: what a hatch fill costs on shape-cpu, one case per family set.  shape-cpu only: no reference draws a hatch (a hatch is
 * the DXF and PDF pattern of lines trimmed to a region, which neither Blend2D nor Skia has as a paint).
 *
 *   hatches [frames [case]]  prints one line per case: the time of a frame in milliseconds, the fill rate in Mpixel/s over the area the shapes
 *                    cover, how many items it refused (a refusal is a resource limit, and the time of a
 *                    frame that refused an item is not the time of drawing it, so the line says so), and a hash of the frame's bytes
 *
 * Each case is one 1920x1080 frame of 12 opaque rectangles (a 4 by 3 grid, 440 by 340 pixels each, a gap of 20), every one with a rectangular
 * hole in its middle (DXF's outermost-ring nesting, so the hole stays empty), hatched in ink at a pen width and a line spacing the case names.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-geometry/shape-geometry.h>
#include <ghoti.io/shape-scene/shape-scene.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 1920
#define H 1080
#define COLS 4
#define ROWS 3
#define CELL_W 440.0
#define CELL_H 340.0
#define PITCH_X 480.0
#define PITCH_Y 360.0

static GSHSCENE_Store * g_store;
static GSHSCENE_Sink * g_cpu;

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/** A rectangle with a rectangular hole in its middle, as one path. */
static const GSHSCENE_Resource * ring(double x0, double y0, double x1, double y1) {
  GSHGEOM_Path * p = NULL;
  if (gshgeom_path_create(NULL, NULL, 2, &p) != GSHGEOM_OK || gshgeom_path_add_rect(p, x0, y0, x1 - x0, y1 - y0, 0, 0) != GSHGEOM_OK ||
      gshgeom_path_add_rect(p, x0 + 0.3 * (x1 - x0), y0 + 0.3 * (y1 - y0), 0.4 * (x1 - x0), 0.4 * (y1 - y0), 1, 0) != GSHGEOM_OK) {
    fprintf(stderr, "hatches: path failed\n");
    exit(1);
  }
  const GSHSCENE_Resource * r = NULL;
  if (gshscene_store_intern_path(g_store, p, &r) != GSHSCENE_OK) {
    fprintf(stderr, "hatches: intern path failed\n");
    exit(1);
  }
  gshgeom_path_destroy(p);
  return r;
}

typedef struct Case {
  const char * name;
  double width;     /* the pen's width */
  size_t families;  /* 1 or 2 */
  double angle[2];
  double spacing[2];
  int dashed[2];    /* a dash pattern of 5 on, 2 off for this family */
  uint32_t cap;
} Case;

static const GSHSCENE_Resource * make_hatch(const Case * c) {
  static const double dashes[2] = {5.0, -2.0};
  GSHSCENE_Stroke_Style pen;
  gshscene_stroke_style_init(&pen);
  pen.width = c->width;
  pen.cap = c->cap;
  const GSHSCENE_Resource * style = NULL;
  if (gshscene_store_intern_stroke_style(g_store, &pen, &style) != GSHSCENE_OK) {
    fprintf(stderr, "hatches: intern style failed\n");
    exit(1);
  }
  GSHSCENE_Hatch_Family fam[2];
  memset(fam, 0, sizeof fam);
  for (size_t i = 0; i < c->families; i++) {
    fam[i].angle = c->angle[i];
    fam[i].offset_across = c->spacing[i];
    fam[i].dash_count = c->dashed[i] ? 2 : 0;
    fam[i].dashes = c->dashed[i] ? dashes : NULL;
  }
  GSHSCENE_Hatch spec;
  gshscene_hatch_init(&spec);
  spec.style = style;
  spec.nesting = GSHSCENE_HATCH_NESTING_OUTER;
  spec.family_count = c->families;
  spec.families = fam;
  spec.ink.rgba[0] = 0.1;
  spec.ink.rgba[1] = 0.2;
  spec.ink.rgba[2] = 0.6;
  spec.ink.rgba[3] = 1.0;
  const GSHSCENE_Resource * hatch = NULL;
  if (gshscene_store_intern_hatch(g_store, &spec, &hatch) != GSHSCENE_OK) {
    fprintf(stderr, "hatches: intern hatch failed\n");
    exit(1);
  }
  gshscene_resource_release(style);
  return hatch;
}

static GSHSCENE_List * build(const Case * c) {
  GSHSCENE_List * l = NULL;
  gshscene_list_create(NULL, NULL, g_store, &l);
  const GSHSCENE_Resource * hatch = make_hatch(c);
  for (int j = 0; j < ROWS; j++) {
    for (int i = 0; i < COLS; i++) {
      const double x = 20.0 + (double)i * PITCH_X, y = 10.0 + (double)j * PITCH_Y;
      const GSHSCENE_Resource * shape = ring(x, y, x + CELL_W, y + CELL_H);
      GSHSCENE_Paint paint;
      memset(&paint, 0, sizeof paint);
      paint.kind = GSHSCENE_PAINT_HATCH;
      paint.rgba[3] = 1.0;
      paint.resource = hatch;
      gshscene_list_fill(l, shape, NULL, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0);
      gshscene_resource_release(shape);
    }
  }
  gshscene_resource_release(hatch);
  gshscene_list_close(l);
  return l;
}

int main(int argc, char ** argv) {
  const int frames = argc > 1 ? atoi(argv[1]) : 5;
  GSHCPU_Limits lim;
  gshcpu_limits_default(&lim);
  if (getenv("HATCHES_CACHE_MIB")) {
    lim.max_cache_bytes = (uint64_t)atoll(getenv("HATCHES_CACHE_MIB")) << 20; /* the stroke cache's bound, to see what a cold frame costs against a warm one */
  }
  if (gshscene_store_create(NULL, NULL, &g_store) != GSHSCENE_OK || gshcpu_sink_create(NULL, NULL, &lim, &g_cpu) != GSHCPU_OK) {
    fprintf(stderr, "hatches: setup failed\n");
    return 1;
  }
  const uint32_t butt = GSHSCENE_STROKE_CAP_BUTT, round_cap = GSHSCENE_STROKE_CAP_ROUND;
  const Case cases[] = {
      {"45 deg, spacing 4, width 1", 1.0, 1, {45.0, 0}, {4.0, 0}, {0, 0}, butt},
      {"45 deg, spacing 12, width 1", 1.0, 1, {45.0, 0}, {12.0, 0}, {0, 0}, butt},
      {"45 deg, spacing 1.5, width 0.6", 0.6, 1, {45.0, 0}, {1.5, 0}, {0, 0}, butt},
      {"crosshatch 45 and 135, spacing 6", 1.0, 2, {45.0, 135.0}, {6.0, 6.0}, {0, 0}, butt},
      {"dashed 45 deg, round caps, spacing 6", 1.5, 1, {45.0, 0}, {6.0, 0}, {1, 0}, round_cap},
      {"dashed 45 and solid 135, round caps", 1.5, 2, {45.0, 135.0}, {4.0, 6.0}, {1, 0}, round_cap},
      {"axis-aligned, spacing 8, width 1", 1.0, 1, {0.0, 0}, {8.0, 0}, {0, 0}, butt},
      {"30 deg, spacing 5, width 2", 2.0, 1, {30.0, 0}, {5.0, 0}, {0, 0}, butt},
  };
  const double covered = (double)COLS * ROWS * CELL_W * CELL_H * (1.0 - 0.4 * 0.4);
  printf("%-40s %10s %12s %10s  %s\n", "case", "frame ms", "Mpixel/s", "refused", "frame hash");
  for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    if (argc > 2 && strstr(cases[k].name, argv[2]) == NULL) {
      continue; /* a case whose name holds the text: for a profiler, which wants one */
    }
    GSHSCENE_List * l = build(&cases[k]);
    GIMG_Raster * r = NULL;
    GIMG_Pixel_Format f = GIMG_PIXEL_RGBA8;
    gimg_raster_create(W, H, &f, GIMG_RASTER_OWNED, NULL, 0, &r);
    GSHSCENE_Target t;
    memset(&t, 0, sizeof t);
    t.kind = GSHSCENE_TARGET_RASTER;
    t.surface = r;
    GSHSCENE_Report * report = NULL;
    gshscene_report_create(NULL, NULL, &report);
    double best = 1e30;
    size_t refused = 0;
    for (int n = -1; n < frames; n++) { /* the first frame warms the caches and is not timed */
      memset(gimg_raster_pixels(r), 0, gimg_raster_stride_bytes(r) * (size_t)H);
      GSHSCENE_Render_Request q;
      memset(&q, 0, sizeof q);
      q.list = l;
      q.target = &t;
      q.report = report;
      const double t0 = now_ms();
      const GSHSCENE_Result res = gshscene_sink_render(g_cpu, &q);
      const double t1 = now_ms();
      if (res != GSHSCENE_OK) {
        fprintf(stderr, "hatches: render %s\n", gshscene_result_string(res));
        exit(1);
      }
      GSHSCENE_Report_Summary s;
      memset(&s, 0, sizeof s);
      gshscene_report_summary(report, &s);
      refused = (size_t)s.by_class[GSHSCENE_ENTRY_REFUSED];
      if (n >= 0 && t1 - t0 < best) {
        best = t1 - t0;
      }
    }
    uint64_t hash = 14695981039346656037ull;
    const uint8_t * px = (const uint8_t *)gimg_raster_pixels_const(r);
    const size_t stride = gimg_raster_stride_bytes(r);
    for (size_t y = 0; y < (size_t)H; y++) {
      for (size_t i = 0; i < (size_t)W * 4u; i++) {
        hash = (hash ^ px[y * stride + i]) * 1099511628211ull;
      }
    }
    printf("%-40s %10.3f %12.1f %10zu  %016llx%s\n", cases[k].name, best, covered / (best * 1e3), refused, (unsigned long long)hash, refused ? "  (not the time of drawing it)" : "");
    gshscene_report_destroy(report);
    gimg_raster_destroy(r);
    gshscene_list_destroy(l);
  }
  gshscene_sink_destroy(g_cpu);
  gshscene_store_destroy(g_store);
  return 0;
}
