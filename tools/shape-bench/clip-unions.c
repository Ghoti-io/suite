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
 * The clip-unions scene (shape-exact-primitives story A3): what a union clip costs on shape-cpu, against the same regions drawn as n
 * nested single clips (not the same picture: nested clips are an intersection, so each of the n fills is drawn under one clip of one
 * member), and what a list with no union costs.  shape-cpu only: no reference draws a union clip through its API in one call.
 *
 *   clip-unions [frames [plain]]  prints one line per case (with `plain`, the default path alone): members, the time of a frame in milliseconds, and the cost per member
 *
 * The cases: an icon sheet (1500 icons, each clipped to a union of 2 to 12 shapes), a text-like clip of 64 small rectangles in a row,
 * and one large union of 256 members.  The default path (the "plain" case, a list of the sheet's fills with no clip at all) is the
 * number to compare across commits.
 *
 * Shape-exact-primitives story A3b adds the general tier (members whose boxes overlap, which the exact step makes exact in the
 * pixels they share): the icon sheet again with its members overlapping ("overlap"), and one union of n members in a chain of
 * overlapping rectangles against the same n members apart, which is the cost of a member of each tier.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/shape-cpu.h>
#include <ghoti.io/shape-geometry/shape-geometry.h>
#include <ghoti.io/shape-scene/shape-scene.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static GSHSCENE_Store * g_store;
static GSHSCENE_Sink * g_cpu;

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static const GSHSCENE_Resource * rect(double x0, double y0, double x1, double y1) {
  GSHGEOM_Path * p = NULL;
  gshgeom_path_create(NULL, NULL, 2, &p);
  GSHGEOM_Point a = {x0, y0, 0}, b = {x1, y0, 0}, c = {x1, y1, 0}, d = {x0, y1, 0};
  gshgeom_path_move_to(p, a);
  gshgeom_path_line_to(p, b);
  gshgeom_path_line_to(p, c);
  gshgeom_path_line_to(p, d);
  gshgeom_path_close(p);
  const GSHSCENE_Resource * r = NULL;
  gshscene_store_intern_path(g_store, p, &r);
  gshgeom_path_destroy(p);
  return r;
}

static const GSHSCENE_Resource * make_union(const GSHSCENE_Resource ** parts, size_t n) {
  GSHSCENE_Clip_Member * m = calloc(n, sizeof *m);
  for (size_t i = 0; i < n; i++) {
    m[i].resource = parts[i];
  }
  const GSHSCENE_Resource * u = NULL;
  if (gshscene_store_intern_clip_union(g_store, m, n, &u) != GSHSCENE_OK) {
    fprintf(stderr, "clip-unions: intern failed\n");
    exit(1);
  }
  free(m);
  for (size_t i = 0; i < n; i++) {
    gshscene_resource_release(parts[i]);
  }
  return u;
}

/* Which structure of list a case is: 0 no clip, 1 one union clip per icon, 2 a nest of single clips per icon. */
static GSHSCENE_List * build(int mode, size_t icons, size_t members, size_t cols, double pitch, size_t * total_members) {
  GSHSCENE_List * l = NULL;
  gshscene_list_create(NULL, NULL, g_store, &l);
  GSHSCENE_Paint paint;
  memset(&paint, 0, sizeof paint);
  paint.kind = GSHSCENE_PAINT_SOLID;
  paint.rgba[0] = 0.2;
  paint.rgba[2] = 0.7;
  paint.rgba[3] = 1.0;
  for (size_t i = 0; i < icons; i++) {
    const double ox = (double)(i % cols) * pitch + 2.25, oy = (double)(i / cols) * pitch + 2.5;
    const size_t k = 2 + i % (members - 1);
    const double step = mode == 3 ? 2.5 : 4.0; /* mode 3: each member overlaps the next by 0.9 */
    const GSHSCENE_Resource * parts[16];
    *total_members += k;
    for (size_t j = 0; j < k && j < 16; j++) {
      parts[j] = rect(ox + (double)j * step, oy, ox + (double)j * step + 3.4, oy + 9.0);
    }
    const GSHSCENE_Resource * big = rect(ox - 1, oy - 1, ox + 4.0 * (double)k + 1, oy + 10);
    if (mode == 1 || mode == 3) {
      const GSHSCENE_Resource * u = make_union(parts, k);
      gshscene_list_clip_push(l, u, NULL, GSHGEOM_FILL_NONZERO, 0, 0.0);
      gshscene_list_fill(l, big, NULL, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0);
      gshscene_list_clip_pop(l);
      gshscene_resource_release(u);
    } else if (mode == 2) {
      for (size_t j = 0; j < k; j++) {
        gshscene_list_clip_push(l, parts[j], NULL, GSHGEOM_FILL_NONZERO, 0, 0.0);
        gshscene_list_fill(l, big, NULL, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0);
        gshscene_list_clip_pop(l);
        gshscene_resource_release(parts[j]);
      }
    } else {
      for (size_t j = 0; j < k; j++) {
        gshscene_resource_release(parts[j]);
      }
      gshscene_list_fill(l, big, NULL, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0);
    }
    gshscene_resource_release(big);
  }
  gshscene_list_close(l);
  return l;
}

static double time_frame(GSHSCENE_List * l, int w, int h, int frames, uint64_t * union_fast, uint64_t * tests) {
  GIMG_Raster * r = NULL;
  GIMG_Pixel_Format f = GIMG_PIXEL_RGBA8;
  gimg_raster_create((uint32_t)w, (uint32_t)h, &f, GIMG_RASTER_OWNED, NULL, 0, &r);
  GSHSCENE_Target t;
  memset(&t, 0, sizeof t);
  t.kind = GSHSCENE_TARGET_RASTER;
  t.surface = r;
  GSHSCENE_Report * report = NULL;
  gshscene_report_create(NULL, NULL, &report);
  double best = 1e30;
  for (int k = 0; k < frames; k++) {
    memset(gimg_raster_pixels(r), 0, gimg_raster_stride_bytes(r) * (size_t)h);
    GSHSCENE_Render_Request q;
    memset(&q, 0, sizeof q);
    q.list = l;
    q.target = &t;
    q.report = report;
    const double t0 = now_ms();
    GSHSCENE_Result res = gshscene_sink_render(g_cpu, &q);
    const double t1 = now_ms();
    if (res != GSHSCENE_OK || gshscene_report_count(report) != 0) {
      fprintf(stderr, "clip-unions: render %s, %zu entries\n", gshscene_result_string(res), gshscene_report_count(report));
      exit(1);
    }
    if (t1 - t0 < best) {
      best = t1 - t0;
    }
  }
  (void)union_fast;
  (void)tests;
  gshscene_report_destroy(report);
  gimg_raster_destroy(r);
  return best;
}

int main(int argc, char ** argv) {
  const int frames = argc > 1 ? atoi(argv[1]) : 5;
  GSHCPU_Limits lim;
  gshcpu_limits_default(&lim);
  GSHSCENE_Limits sl;
  memset(&sl, 0, sizeof sl);
  sl.max_clip_members = 1024;
  if (gshscene_store_create(NULL, &sl, &g_store) != GSHSCENE_OK || gshcpu_sink_create(NULL, NULL, &lim, &g_cpu) != GSHCPU_OK) {
    fprintf(stderr, "clip-unions: setup failed\n");
    return 1;
  }
  struct Case {
    const char * name;
    size_t icons, members, cols;
    double pitch;
    int w, h;
  } cases[] = {
      {"icon-sheet (1500 icons, 2 to 12 members)", 1500, 12, 50, 28.0, 1400, 900},
      {"text-like (64 small rectangles, one clip)", 1, 0, 1, 0, 0, 0},
      {"one-large-union (256 members)", 1, 0, 1, 0, 0, 0},
  };
  for (int c = 0; c < 1; c++) {
    const struct Case * k = &cases[c];
    const char * modes[4] = {"plain  ", "union  ", "nested ", "overlap"};
    for (int mode = 0; mode < (argc > 2 ? 1 : 4); mode++) {
      size_t members = 0;
      GSHSCENE_List * l = build(mode, k->icons, k->members, k->cols, k->pitch, &members);
      const double ms = time_frame(l, k->w, k->h, frames, NULL, NULL);
      const double per = ms * 1e3 / (double)k->icons;
      printf("%-42s %s frame %9.3f ms  %8.2f us per icon  %6.2f us per member\n", k->name, modes[mode], ms, per,
          members ? ms * 1e3 / (double)members : 0.0);
      gshscene_list_destroy(l);
    }
  }
  if (argc > 2) {
    gshscene_sink_destroy(g_cpu);
    gshscene_store_destroy(g_store);
    return 0; /* `plain` only: the default path, for a build that has no union clips */
  }
  /* A text-like clip: 64 small rectangles in a row, and one union of 256 members apart: the cost of one push against n. */
  for (size_t n = 8; n <= 256; n *= 4) {
    GSHSCENE_List * l = NULL;
    gshscene_list_create(NULL, NULL, g_store, &l);
    const GSHSCENE_Resource * parts[256];
    for (size_t j = 0; j < n; j++) {
      parts[j] = rect(2.0 + (double)(j % 64) * 20.0, 2.0 + (double)(j / 64) * 20.0, 2.0 + (double)(j % 64) * 20.0 + 15.5, 2.0 + (double)(j / 64) * 20.0 + 15.5);
    }
    const GSHSCENE_Resource * u = make_union(parts, n);
    const GSHSCENE_Resource * big = rect(0, 0, 1300, 100);
    GSHSCENE_Paint paint;
    memset(&paint, 0, sizeof paint);
    paint.kind = GSHSCENE_PAINT_SOLID;
    paint.rgba[3] = 1.0;
    gshscene_list_clip_push(l, u, NULL, GSHGEOM_FILL_NONZERO, 0, 0.0);
    gshscene_list_fill(l, big, NULL, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0);
    gshscene_list_clip_pop(l);
    gshscene_list_close(l);
    const double ms = time_frame(l, 1300, 100, frames, NULL, NULL);
    printf("one union of %-4zu members, 1300x100          frame %9.3f ms  %8.2f us per member\n", n, ms, ms * 1e3 / (double)n);
    gshscene_list_destroy(l);
    gshscene_resource_release(u);
    gshscene_resource_release(big);
  }
  /* The general tier: n rectangles in a chain, each overlapping the next by 1.5 pixels, against the same n apart. */
  for (size_t n = 8; n <= 256; n *= 4) {
    for (int overlapping = 0; overlapping < 2; overlapping++) {
      GSHSCENE_List * l = NULL;
      gshscene_list_create(NULL, NULL, g_store, &l);
      const GSHSCENE_Resource * parts[256];
      const double pitch = overlapping ? 14.0 : 17.0;
      for (size_t j = 0; j < n; j++) {
        parts[j] = rect(2.0 + (double)j * pitch, 2.0, 2.0 + (double)j * pitch + 15.5, 17.5);
      }
      const int w = (int)(2.0 + (double)n * pitch + 16.0);
      const GSHSCENE_Resource * u = make_union(parts, n);
      const GSHSCENE_Resource * big = rect(0, 0, (double)w, 20);
      GSHSCENE_Paint paint;
      memset(&paint, 0, sizeof paint);
      paint.kind = GSHSCENE_PAINT_SOLID;
      paint.rgba[3] = 1.0;
      gshscene_list_clip_push(l, u, NULL, GSHGEOM_FILL_NONZERO, 0, 0.0);
      gshscene_list_fill(l, big, NULL, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0);
      gshscene_list_clip_pop(l);
      gshscene_list_close(l);
      const double ms = time_frame(l, w, 20, frames, NULL, NULL);
      printf("a chain of %-4zu members %-12s %5dx20        frame %9.3f ms  %8.2f us per member\n", n, overlapping ? "overlapping" : "apart", w, ms,
          ms * 1e3 / (double)n);
      gshscene_list_destroy(l);
      gshscene_resource_release(u);
      gshscene_resource_release(big);
    }
  }
  gshscene_sink_destroy(g_cpu);
  gshscene_store_destroy(g_store);
  return 0;
}
