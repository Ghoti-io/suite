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
 * @file scene.h
 *
 * The benchmark's scene file, its reader and writer, and the measurement loop
 * every driver shares.  Plain C17 so that the C++ drivers compile the same
 * reader: there is one reading of a scene, not one per engine.
 *
 * A scene file is text, one record a line:
 *
 *     shape-bench-scene 1
 *     name <name>
 *     metric <fill|paths|text|list>
 *     extent <width> <height>
 *     background <r> <g> <b> <a>          8-bit straight sRGB
 *     work <unit> <amount>                what one frame does (pixels, paths, glyphs, items)
 *     font <file> <size>                  text only: the font file (beside the scene) and the em size in pixels
 *     clip <x0> <y0> <x1> <y1>            push a rectangular clip
 *     unclip                              pop it
 *     fill <r> <g> <b> <a> <count>        a path filled non-zero, then <count> commands:
 *     M x y | L x y | Q x1 y1 x y | C x1 y1 x2 y2 x y | Z
 *     run <r> <g> <b> <a> <count>         a glyph run, then <count> lines "g <id> <x> <y>" (baseline origin)
 *     end
 *
 * Coordinates are pixels, y down, and multiples of 1/64 written exactly.
 * Colors are straight 8-bit sRGB; every engine composites source-over in the
 * target's own (sRGB-encoded) values.
 */

#ifndef GHOTI_IO_SHAPE_BENCH_SCENE_H
#define GHOTI_IO_SHAPE_BENCH_SCENE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SB_Color {
  uint8_t r, g, b, a;
} SB_Color;

typedef enum SB_Kind { SB_FILL = 1, SB_RUN = 2, SB_CLIP = 3, SB_UNCLIP = 4 } SB_Kind;

/** One path command: 'M', 'L', 'Q', 'C' or 'Z', with its points in p (x, y pairs). */
typedef struct SB_Cmd {
  char op;
  double p[6];
} SB_Cmd;

typedef struct SB_Glyph {
  uint32_t id;
  double x, y;
} SB_Glyph;

typedef struct SB_Item {
  SB_Kind kind;
  SB_Color color;  ///< fill, run
  size_t first;    ///< fill: the first command; run: the first glyph
  size_t count;    ///< fill: commands; run: glyphs
  double rect[4];  ///< clip: x0, y0, x1, y1
} SB_Item;

typedef struct SB_Scene {
  char name[64];
  char metric[16];
  int width, height;
  SB_Color background;
  char work_unit[16];
  double work;
  char font[256];   ///< the font file's name, beside the scene file; empty when none
  double font_size; ///< pixels to the em
  SB_Item * items;
  size_t n_items, cap_items;
  SB_Cmd * cmds;
  size_t n_cmds, cap_cmds;
  SB_Glyph * glyphs;
  size_t n_glyphs, cap_glyphs;
} SB_Scene;

/** Zero a scene. */
void sb_scene_init(SB_Scene * scene);
/** Free what a scene holds and zero it. */
void sb_scene_free(SB_Scene * scene);
/** Append; 0 on success, -1 when out of memory. */
int sb_scene_add_item(SB_Scene * scene, const SB_Item * item);
int sb_scene_add_cmd(SB_Scene * scene, char op, const double * p);
int sb_scene_add_glyph(SB_Scene * scene, uint32_t id, double x, double y);
/** Items that draw (fills and runs). */
size_t sb_scene_drawables(const SB_Scene * scene);

/** Read a scene file; 0 on success, else -1 with a message in err. */
int sb_scene_load(const char * path, SB_Scene * out, char * err, size_t errlen);
/** Write a scene file; 0 on success. */
int sb_scene_save(const char * path, const SB_Scene * scene);

/**
 * The control's rule: of n drawables, number k (counted from 0) is dropped when (n - 1 - k) % SB_CONTROL_STRIDE == 0.
 * Counted from the last so that the topmost drawable is always among them: dropping the bottom one of twelve
 * translucent layers changes the frame by less than the bound, and a control that cannot fail sees nothing.
 */
#define SB_CONTROL_STRIDE 16

/** Monotonic seconds. */
double sb_now(void);

/** Statistics of a set of samples, in seconds. */
typedef struct SB_Stats {
  double median, p25, p75, min, max;
} SB_Stats;

/** Sort @p samples in place and summarize them (quartiles by linear interpolation). */
void sb_stats(double * samples, int n, SB_Stats * out);

/** The protocol: warm-up frames and seconds, then timed frames. */
#define SB_WARMUP_FRAMES 5
#define SB_WARMUP_SECONDS 0.25
#define SB_TIMED_FRAMES 31

/**
 * One engine's frame, as callbacks.  @p clear restores the target to the
 * background (not timed); @p build records what the frame draws (NULL when the
 * engine has no such step); @p submit renders it until the output is complete.
 * Each returns 0 on success.
 */
typedef struct SB_Engine {
  void * ctx;
  int (*clear)(void * ctx);
  int (*build)(void * ctx);
  int (*submit)(void * ctx);
} SB_Engine;

typedef struct SB_Measure {
  int warmup_frames;
  int frames;
  int has_build;
  SB_Stats build, submit, frame;
} SB_Measure;

/** Run the protocol; 0 on success, else the failing callback's result. */
int sb_measure(const SB_Engine * engine, SB_Measure * out);

/**
 * Write one JSON line describing a measurement to @p out:
 * engine, scene, metric, work unit and amount, the protocol and the statistics,
 * the output file, and @p note (may be NULL).
 */
void sb_report(FILE * out, const char * engine, const SB_Scene * scene, const SB_Measure * m, const char * output,
    const char * note);

/**
 * Write a premultiplied RGBA8 buffer as a PAM file (TUPLTYPE RGB_ALPHA, the
 * bytes as they are: premultiplied).  @p bgra swaps the first and third channel
 * of each pixel on the way out.  0 on success.
 */
int sb_write_pam(const char * path, int width, int height, const uint8_t * pixels, size_t stride, int bgra);

/** The background as a premultiplied 32-bit pixel in R, G, B, A byte order (or B, G, R, A when @p bgra). */
void sb_background_bytes(const SB_Scene * scene, int bgra, uint8_t out[4]);

/** Fill a premultiplied buffer with the background. */
void sb_clear(uint8_t * pixels, int width, int height, size_t stride, const uint8_t px[4]);

#ifdef __cplusplus
}
#endif

#endif
