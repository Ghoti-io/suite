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
 * The images scene: what an image paint costs on shape-cpu, one case per way the paint is sampled.  shape-cpu only for now: the
 * references' image paths filter by their own definitions (the area filter is shape-scene's, and neither reference defines it), so
 * a comparison needs a minimal pair per case before its figure means anything.
 *
 *   images [frames]  prints one line per case: the filter, the map, the pixels covered, the time of a frame in milliseconds, the fill
 *                    rate in Mpixel/s over the covered area, and how many items the sink refused (a refusal is a resource limit, and the
 *                    time of a frame that refused an item is not the time of drawing it, so the line says so)
 *
 * Each case is one 1920x1080 frame of 12 opaque rectangles (a 4 by 3 grid, 440 by 340 pixels each, a gap of 20) painted with a
 * 256 by 256 RGBA8 image of fixed pseudo-random texels (a texel is not a smooth function of its neighbors, so no filter can skip work).
 * The cases differ only in the paint's map, extend modes and filters.
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
#define IMAGE_SIDE 256

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
  if (gshscene_store_intern_path(g_store, p, &r) != GSHSCENE_OK) {
    fprintf(stderr, "images: intern path failed\n");
    exit(1);
  }
  gshgeom_path_destroy(p);
  return r;
}

/* The texels: a fixed xorshift stream, opaque, so that the frames of every run draw the same bytes. */
static const GSHSCENE_Resource * make_image(void) {
  GIMG_Raster * r = NULL;
  GIMG_Pixel_Format f = GIMG_PIXEL_RGBA8;
  if (gimg_raster_create(IMAGE_SIDE, IMAGE_SIDE, &f, GIMG_RASTER_OWNED, NULL, 0, &r) != GIMG_OK) {
    fprintf(stderr, "images: raster failed\n");
    exit(1);
  }
  uint32_t s = 0x9e3779b9u;
  for (uint32_t y = 0; y < IMAGE_SIDE; y++) {
    uint8_t * row = (uint8_t *)gimg_raster_pixels(r) + (size_t)y * gimg_raster_stride_bytes(r);
    for (uint32_t x = 0; x < IMAGE_SIDE; x++) {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      row[4 * x + 0] = (uint8_t)s;
      row[4 * x + 1] = (uint8_t)(s >> 8);
      row[4 * x + 2] = (uint8_t)(s >> 16);
      row[4 * x + 3] = 255;
    }
  }
  GSHSCENE_Image_Params params;
  memset(&params, 0, sizeof params);
  const GSHSCENE_Resource * img = NULL;
  if (gshscene_store_intern_image(g_store, r, &params, &img) != GSHSCENE_OK) {
    fprintf(stderr, "images: intern image failed\n");
    exit(1);
  }
  gimg_raster_destroy(r);
  return img;
}

typedef struct Case {
  const char * name;
  double scale;      /* the map's uniform scale, image to local space */
  double degrees;    /* the map's rotation */
  uint32_t extend;   /* both axes */
  uint32_t filter;   /* the magnify and the minify filter */
} Case;

static const GSHSCENE_Resource * image_paint(const GSHSCENE_Resource * img, const Case * c, double ox, double oy) {
  GSHSCENE_Image_Paint ip;
  gshscene_image_paint_init(&ip);
  ip.image = img;
  const double a = c->degrees * 3.14159265358979323846 / 180.0;
  const double m[4] = {c->scale * cos(a), c->scale * sin(a), -c->scale * sin(a), c->scale * cos(a)};
  const double move[2] = {ox, oy};
  if (gshgeom_map_from_matrix(2, m, move, &ip.map) != GSHGEOM_OK) {
    fprintf(stderr, "images: map failed\n");
    exit(1);
  }
  ip.extend_x = c->extend;
  ip.extend_y = c->extend;
  ip.magnify = c->filter;
  ip.minify = c->filter;
  const GSHSCENE_Resource * r = NULL;
  if (gshscene_store_intern_image_paint(g_store, &ip, &r) != GSHSCENE_OK) {
    fprintf(stderr, "images: intern image paint failed\n");
    exit(1);
  }
  return r;
}

static GSHSCENE_List * build(const GSHSCENE_Resource * img, const Case * c) {
  GSHSCENE_List * l = NULL;
  gshscene_list_create(NULL, NULL, g_store, &l);
  for (int j = 0; j < ROWS; j++) {
    for (int i = 0; i < COLS; i++) {
      const double x = 20.0 + (double)i * PITCH_X, y = 10.0 + (double)j * PITCH_Y;
      const GSHSCENE_Resource * shape = rect(x, y, x + CELL_W, y + CELL_H);
      /* the image's origin at the cell's, shifted by a fraction of a pixel so that no case lands on the texel grid */
      const GSHSCENE_Resource * paint_res = image_paint(img, c, x + 0.37 * (double)(i + 1), y + 0.21 * (double)(j + 1));
      GSHSCENE_Paint paint;
      memset(&paint, 0, sizeof paint);
      paint.kind = GSHSCENE_PAINT_IMAGE;
      paint.rgba[3] = 1.0;
      paint.resource = paint_res;
      gshscene_list_fill(l, shape, NULL, GSHGEOM_FILL_NONZERO, &paint, 0, 0.0);
      gshscene_resource_release(shape);
      gshscene_resource_release(paint_res);
    }
  }
  gshscene_list_close(l);
  return l;
}

int main(int argc, char ** argv) {
  const int frames = argc > 1 ? atoi(argv[1]) : 5;
  GSHCPU_Limits lim;
  gshcpu_limits_default(&lim);
  if (gshscene_store_create(NULL, NULL, &g_store) != GSHSCENE_OK || gshcpu_sink_create(NULL, NULL, &lim, &g_cpu) != GSHCPU_OK) {
    fprintf(stderr, "images: setup failed\n");
    return 1;
  }
  const GSHSCENE_Resource * img = make_image();
  const Case cases[] = {
      {"1:1 nearest", 1.0, 0.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_NEAREST},
      {"1:1 linear", 1.0, 0.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_LINEAR},
      {"4x magnify nearest", 4.0, 0.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_NEAREST},
      {"4x magnify linear", 4.0, 0.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_LINEAR},
      {"4x magnify cubic", 4.0, 0.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_CUBIC},
      {"0.37x minify linear", 0.37, 0.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_LINEAR},
      {"0.37x minify area, repeat", 0.37, 0.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_AREA},
      {"0.37x minify area, pad", 0.37, 0.0, GSHSCENE_IMAGE_EXTEND_PAD, GSHSCENE_IMAGE_FILTER_AREA},
      {"1.7x rotated 30 deg linear", 1.7, 30.0, GSHSCENE_IMAGE_EXTEND_REPEAT, GSHSCENE_IMAGE_FILTER_LINEAR},
      {"1.7x rotated 30 deg cubic", 1.7, 30.0, GSHSCENE_IMAGE_EXTEND_REFLECT, GSHSCENE_IMAGE_FILTER_CUBIC},
  };
  const double covered = (double)COLS * ROWS * CELL_W * CELL_H;
  printf("%-30s %10s %12s %10s\n", "case", "frame ms", "Mpixel/s", "refused");
  for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    GSHSCENE_List * l = build(img, &cases[k]);
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
        fprintf(stderr, "images: render %s\n", gshscene_result_string(res));
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
    printf("%-30s %10.3f %12.1f %10zu%s\n", cases[k].name, best, covered / (best * 1e3), refused,
        refused ? "  (not the time of drawing it)" : "");
    gshscene_report_destroy(report);
    gimg_raster_destroy(r);
    gshscene_list_destroy(l);
  }
  gshscene_resource_release(img);
  gshscene_sink_destroy(g_cpu);
  gshscene_store_destroy(g_store);
  return 0;
}
