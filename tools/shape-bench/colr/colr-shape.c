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
 * @file colr-shape.c
 *
 * The shape-cpu side of the COLRv1 comparison with Skia (color-paint story P4, L13): every color glyph of a font
 * lowered by `font` and drawn by `shape-cpu`, unhinted, palette 0, a black foreground, into one 8-bit RGBA
 * premultiplied cell each, and the cells written as one PAM atlas that `colr-cmp` compares with the one
 * `colr-skia.cpp` writes.
 *
 *     colr-shape FONT OUT.pam OUT.glyphs [--size S] [--cell C] [--cols N] [--encoded] [--drop N]
 *                [--glyphs FILE] [--coords c0,c1,...] [--limit N] [--every N]
 *
 * A glyph is drawn at an em of S pixels with its origin at (C / 8, C - C / 8) - the same place `colr-skia`
 * puts it - in a cell of C by C pixels. OUT.glyphs lists the glyph ids drawn, one a line, in the atlas's order:
 * all the glyphs with a color description, or those of --glyphs. `--encoded` lowers in the encoded space (the one
 * Skia draws in); the default is the standard's. `--coords` is the instance, normalised 2.14 integers in axis
 * order. `--drop N` is the comparison's control: every Nth fill of each glyph's list is left out, which must put
 * a glyph outside the bound. `--limit` keeps the first N glyphs, `--every N` every Nth.
 */

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/scene.h>
#include <ghoti.io/font/scene_color.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/shape-cpu/render.h>
#include <ghoti.io/shape-scene/list.h>
#include <ghoti.io/shape-scene/resource.h>
#include <ghoti.io/shape-scene/sink.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char * what) {
  fprintf(stderr, "colr-shape: %s\n", what);
  exit(1);
}

/** The list without every Nth fill (the comparison's control). */
static GSHSCENE_List * without_fills(GSHSCENE_Store * store, const GSHSCENE_List * list, unsigned every) {
  GSHSCENE_List * out = NULL;
  unsigned seen = 0;
  if (gshscene_list_create(NULL, NULL, store, &out) != GSHSCENE_OK) {
    die("list");
  }
  for (size_t i = 0; i < gshscene_list_count(list); i++) {
    const GSHSCENE_Item * item = NULL;
    gshscene_list_item(list, i, &item);
    GSHSCENE_Result r = GSHSCENE_OK;
    switch (item->kind) {
      case GSHSCENE_ITEM_FILL:
        if (++seen % every == 0) {
          break;
        }
        r = gshscene_list_fill(out, item->resource, &item->map, item->fill_rule, &item->paint, item->layer, item->depth);
        break;
      case GSHSCENE_ITEM_CLIP_PUSH:
        r = gshscene_list_clip_push(out, item->resource, &item->map, item->fill_rule, item->layer, item->depth);
        break;
      case GSHSCENE_ITEM_CLIP_POP:
        r = gshscene_list_clip_pop(out);
        break;
      case GSHSCENE_ITEM_GROUP_PUSH:
        r = gshscene_list_group_push(out, &item->group, item->layer, item->depth);
        break;
      case GSHSCENE_ITEM_GROUP_POP:
        r = gshscene_list_group_pop(out);
        break;
      default:
        die("an item kind the control does not copy");
    }
    if (r != GSHSCENE_OK) {
      die("copying a list for the control");
    }
  }
  return out;
}

int main(int argc, char ** argv) {
  const char * font_path = NULL;
  const char * out_path = NULL;
  const char * glyph_out = NULL;
  const char * glyph_in = NULL;
  unsigned size = 100;
  unsigned cell = 128;
  unsigned cols = 64;
  unsigned drop = 0;
  size_t limit = (size_t)-1;
  unsigned every = 1;
  uint32_t space = GFNT_COLOR_SPACE_STANDARD;
  GFNT_F2Dot14 coords[32];
  size_t coord_count = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--size") && i + 1 < argc) {
      size = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--cell") && i + 1 < argc) {
      cell = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--cols") && i + 1 < argc) {
      cols = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--drop") && i + 1 < argc) {
      drop = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
      limit = (size_t)atol(argv[++i]);
    } else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
      every = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--glyphs") && i + 1 < argc) {
      glyph_in = argv[++i];
    } else if (!strcmp(argv[i], "--encoded")) {
      space = GFNT_COLOR_SPACE_ENCODED;
    } else if (!strcmp(argv[i], "--coords") && i + 1 < argc) {
      for (char * p = strtok(argv[++i], ","); p && coord_count < 32; p = strtok(NULL, ",")) {
        coords[coord_count++] = (GFNT_F2Dot14)atoi(p);
      }
    } else if (!font_path) {
      font_path = argv[i];
    } else if (!out_path) {
      out_path = argv[i];
    } else if (!glyph_out) {
      glyph_out = argv[i];
    } else {
      die("too many arguments");
    }
  }
  if (!font_path || !out_path || !glyph_out || cell == 0 || cols == 0) {
    die("usage: colr-shape FONT OUT.pam OUT.glyphs [--size S] [--cell C] [--cols N] [--encoded] [--drop N] "
        "[--glyphs FILE] [--coords c0,c1,...] [--limit N] [--every N]");
  }
  GFNT_Blob * blob = NULL;
  GFNT_Face * face = NULL;
  GFNT_SceneFont * scene = NULL;
  GSHSCENE_Store * store = NULL;
  const GSHSCENE_Resource * face_resource = NULL;
  const GSHSCENE_Resource * palette = NULL;
  GFNT_Error error;
  size_t glyph_count = 0;
  GFNT_Variation variation = {coords, coord_count, GFNT_DELTA_ROUND_HALF_UP};
  gfnt_error_clear(&error);
  if (gfnt_blob_create_file(font_path, NULL, NULL, &blob, &error) != GFNT_OK ||
      gfnt_face_load(blob, 0, NULL, NULL, &face, &error) != GFNT_OK ||
      gfnt_scene_font_create(face, &scene, &error) != GFNT_OK || gshscene_store_create(NULL, NULL, &store) != GSHSCENE_OK ||
      gfnt_scene_intern_face(scene, store, coord_count ? &variation : NULL, &face_resource, &error) != GFNT_OK ||
      gfnt_face_num_glyphs(face, &glyph_count, &error) != GFNT_OK) {
    gfnt_error_dump(&error, stderr);
    return 1;
  }
  if (gfnt_scene_intern_palette(face_resource, store, 0, &palette, &error) != GFNT_OK) {
    palette = NULL; /* no CPAL: version 0 and 1 alike need one, so a font without it has no color glyph */
  }

  uint32_t * wanted = malloc((glyph_count + 1) * sizeof *wanted);
  size_t wanted_count = 0;
  if (!wanted) {
    die("memory");
  }
  if (glyph_in) {
    FILE * f = fopen(glyph_in, "r");
    unsigned g;
    if (!f) {
      die("cannot read the glyph list");
    }
    while (fscanf(f, "%u", &g) == 1 && wanted_count < glyph_count) {
      wanted[wanted_count++] = g;
    }
    fclose(f);
  }

  const unsigned origin_x = cell / 8;
  const unsigned origin_y = cell - cell / 8;
  const size_t cell_bytes = (size_t)cell * cell * 4;
  size_t total = glyph_in ? wanted_count : glyph_count;
  /* The glyph ids are not known before the lowering when none is given: gather them first. */
  size_t drawn = 0;
  uint8_t * atlas = NULL;
  size_t atlas_cells = 0;
  unsigned rows = 0;
  GIMG_Raster * raster = NULL;
  GSHSCENE_Sink * sink = NULL;
  GSHSCENE_Report * report = NULL;
  GIMG_Pixel_Format pf;
  memset(&pf, 0, sizeof pf);
  pf.channel_model = GIMG_CHANNEL_RGBA;
  pf.layout = GIMG_LAYOUT_INTERLEAVED;
  pf.channel_count = 4;
  pf.channel_type = GIMG_CHANNEL_UNORM;
  for (int i = 0; i < 4; i++) {
    pf.bits_per_channel[i] = 8;
  }
  pf.alpha_form = GIMG_ALPHA_PREMULTIPLIED;
  if (gimg_raster_create(cell, cell, &pf, GIMG_RASTER_OWNED, NULL, 0, &raster) != GIMG_OK ||
      gshcpu_sink_create(NULL, NULL, NULL, &sink) != GSHCPU_OK || gshscene_report_create(NULL, NULL, &report) != GSHSCENE_OK) {
    die("raster, sink or report");
  }
  FILE * glyph_file = fopen(glyph_out, "w");
  if (!glyph_file) {
    die("cannot write the glyph list");
  }
  size_t refused = 0;
  size_t capacity = 0;
  size_t seen = 0;
  for (size_t k = 0; k < total && drawn < limit; k++) {
    uint32_t g = glyph_in ? wanted[k] : (uint32_t)k;
    GSHSCENE_List * list = NULL;
    GFNT_ColorLowering out;
    GFNT_ColorRun run;
    memset(&run, 0, sizeof run);
    run.size = (int32_t)size;
    run.x = (int32_t)origin_x;
    run.y = (int32_t)origin_y;
    run.bits = 0;
    run.hint_mode = GSHSCENE_HINT_NONE;
    run.space = space;
    if (gshscene_list_create(NULL, NULL, store, &list) != GSHSCENE_OK) {
      die("list");
    }
    GFNT_Result r = gfnt_scene_record_color_glyph(face_resource, store, list, g, &run, &out, &error);
    if (r != GFNT_OK || out.source == GFNT_COLOR_SOURCE_NONE) {
      if (r != GFNT_OK) {
        refused++;
        fprintf(stderr, "colr-shape: glyph %u not lowered (result %d)\n", g, (int)r);
      }
      gshscene_list_destroy(list);
      continue;
    }
    if (every > 1 && (seen++ % every) != 0) {
      gshscene_list_destroy(list);
      continue;
    }
    GSHSCENE_Color black = {{0.0, 0.0, 0.0, 1.0}, {GCOL_PRIMARIES_SRGB, GCOL_TRANSFER_SRGB, 0.0}};
    gshscene_list_set_foreground(list, &black);
    if (palette) {
      gshscene_list_set_palette(list, palette);
    }
    GSHSCENE_List * drawn_list = list;
    if (drop) {
      drawn_list = without_fills(store, list, drop);
      gshscene_list_set_foreground(drawn_list, &black);
      if (palette) {
        gshscene_list_set_palette(drawn_list, palette);
      }
    }
    gshscene_list_close(drawn_list);
    uint8_t * px = gimg_raster_pixels(raster);
    for (unsigned y = 0; y < cell; y++) {
      memset(px + (size_t)y * gimg_raster_stride_bytes(raster), 0, (size_t)cell * 4);
    }
    GSHSCENE_Target target;
    memset(&target, 0, sizeof target);
    target.kind = GSHSCENE_TARGET_RASTER;
    target.surface = raster;
    GSHSCENE_Render_Request request;
    memset(&request, 0, sizeof request);
    request.list = drawn_list;
    request.target = &target;
    request.report = report;
    GSHSCENE_Result sr = gshscene_sink_render(sink, &request);
    if (sr != GSHSCENE_OK) {
      fprintf(stderr, "colr-shape: glyph %u: render result %d\n", g, (int)sr);
    }
    if (drawn_list != list) {
      gshscene_list_destroy(drawn_list);
    }
    gshscene_list_destroy(list);
    if (atlas_cells == capacity) {
      capacity = capacity ? capacity * 2 : 256;
      atlas = realloc(atlas, capacity * cell_bytes);
      if (!atlas) {
        die("memory for the atlas");
      }
    }
    for (unsigned y = 0; y < cell; y++) {
      memcpy(atlas + atlas_cells * cell_bytes + (size_t)y * cell * 4, px + (size_t)y * gimg_raster_stride_bytes(raster),
          (size_t)cell * 4);
    }
    atlas_cells++;
    drawn++;
    fprintf(glyph_file, "%u\n", g);
  }
  fclose(glyph_file);
  rows = (unsigned)((atlas_cells + cols - 1) / cols);
  if (rows == 0) {
    rows = 1;
  }
  FILE * pam = fopen(out_path, "wb");
  if (!pam) {
    die("cannot write the atlas");
  }
  fprintf(pam, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", cols * cell, rows * cell);
  uint8_t * row = calloc((size_t)cols * cell, 4);
  for (unsigned r = 0; r < rows; r++) {
    for (unsigned y = 0; y < cell; y++) {
      memset(row, 0, (size_t)cols * cell * 4);
      for (unsigned c = 0; c < cols; c++) {
        size_t index = (size_t)r * cols + c;
        if (index < atlas_cells) {
          memcpy(row + (size_t)c * cell * 4, atlas + index * cell_bytes + (size_t)y * cell * 4, (size_t)cell * 4);
        }
      }
      fwrite(row, 4, (size_t)cols * cell, pam);
    }
  }
  fclose(pam);
  free(row);
  free(atlas);
  free(wanted);
  fprintf(stderr, "colr-shape: %zu glyphs drawn, %zu not lowered\n", drawn, refused);
  return 0;
}
