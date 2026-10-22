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
 *     stroke <r> <g> <b> <a> <width> <join> <cap> <miter> <offset> <ndash> <dash>... <count>
 *                                         a path stroked (strokes story S4): then <count> commands as a fill's.
 *                                         <join> is miter, round or bevel; <cap> butt, round or square; the width is in
 *                                         pixels under the identity map (every engine's local width); <miter> the miter
 *                                         limit; <ndash> lengths in pixels (an even count, 0 for solid) restarting at each
 *                                         subpath, from the phase <offset>.  Non-zero winding, a union of what the
 *                                         stroke covers, one blend: each engine's own stroker.
 *     gradient <kind> <extend> <geometry> <count>
 *                                         the paint of the fill that follows it, then <count> stops
 *                                         "s <offset> <r> <g> <b> <a>"
 *     end
 *
 * Coordinates are pixels, y down, and multiples of 1/64 written exactly.
 * Colors are straight 8-bit sRGB; every engine composites source-over in the
 * target's own (sRGB-encoded) values.
 *
 * A gradient record paints the fill record that comes next, and nothing else:
 * any other record after it is an error.  That fill's color is then `0 0 0 a`,
 * `a` the alpha multiplier.  `<kind>` and its `<geometry>`:
 *
 *     linear x0 y0 x1 y1                  offset 0 at (x0, y0), 1 at (x1, y1), isolines perpendicular
 *     radial x0 y0 r0 x1 y1 r1            the two-circle cone: offset 0 on the circle (x0, y0, r0), 1 on
 *                                         (x1, y1, r1), the largest offset whose radius is positive wins
 *     sweep cx cy a0 a1                   angles in degrees from +x toward +y (clockwise on this y-down
 *                                         raster), offset 0 at a0 and 1 at a1
 *
 * `<extend>` is `pad`, `repeat` or `reflect`.  There are 2 to SB_GRADIENT_STOPS_MAX
 * stops, their offsets non-decreasing from exactly 0 to exactly 1: a first
 * offset above 0 or a last below 1 means different periods to different
 * engines under repeat and reflect, so the format does not carry one.  Every
 * engine interpolates the stops in the target's own encoded values, unpremultiplied
 * and undithered.  These are shape-scene's G7 to G9 (its paint.h) restricted to
 * what the references can also say; a scene states in its generator which of
 * them it uses and why.
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

typedef enum SB_Kind { SB_FILL = 1, SB_RUN = 2, SB_CLIP = 3, SB_UNCLIP = 4, SB_STROKE = 5 } SB_Kind;

typedef enum SB_Join { SB_JOIN_MITER = 0, SB_JOIN_ROUND = 1, SB_JOIN_BEVEL = 2 } SB_Join;
typedef enum SB_Cap { SB_CAP_BUTT = 0, SB_CAP_ROUND = 1, SB_CAP_SQUARE = 2 } SB_Cap;

/** The most dash lengths a stroke record carries. */
#define SB_DASHES_MAX 8

/** A stroke's style (the path is the item's commands). */
typedef struct SB_Stroke {
  double width;
  SB_Join join;
  SB_Cap cap;
  double miter;
  double offset;
  size_t n_dashes;
  double dashes[SB_DASHES_MAX];
} SB_Stroke;

/** One path command: 'M', 'L', 'Q', 'C' or 'Z', with its points in p (x, y pairs). */
typedef struct SB_Cmd {
  char op;
  double p[6];
} SB_Cmd;

typedef struct SB_Glyph {
  uint32_t id;
  double x, y;
} SB_Glyph;

typedef enum SB_Gradient_Kind { SB_GRADIENT_LINEAR = 1, SB_GRADIENT_RADIAL = 2, SB_GRADIENT_SWEEP = 3 } SB_Gradient_Kind;
typedef enum SB_Extend { SB_EXTEND_PAD = 0, SB_EXTEND_REPEAT = 1, SB_EXTEND_REFLECT = 2 } SB_Extend;

/** The most stops a gradient record carries. */
#define SB_GRADIENT_STOPS_MAX 16

typedef struct SB_Stop {
  double offset;
  SB_Color color;
} SB_Stop;

typedef struct SB_Gradient {
  SB_Gradient_Kind kind;
  SB_Extend extend;
  /** linear: x0 y0 x1 y1; radial: x0 y0 r0 x1 y1 r1; sweep: cx cy a0 a1 (degrees). */
  double g[6];
  size_t first_stop; ///< into SB_Scene.stops
  size_t n_stops;
} SB_Gradient;

typedef struct SB_Item {
  SB_Kind kind;
  SB_Color color;  ///< fill, run; a gradient fill's is 0 0 0 and its alpha multiplier
  size_t first;    ///< fill: the first command; run: the first glyph
  size_t count;    ///< fill: commands; run: glyphs
  double rect[4];  ///< clip: x0, y0, x1, y1
  size_t gradient; ///< fill: 1 + the index of its paint in SB_Scene.gradients; 0 for a solid color
  size_t stroke;   ///< stroke: 1 + the index of its style in SB_Scene.strokes
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
  SB_Gradient * gradients;
  size_t n_gradients, cap_gradients;
  SB_Stop * stops;
  size_t n_stops, cap_stops;
  SB_Stroke * strokes;
  size_t n_strokes, cap_strokes;
} SB_Scene;

/** Zero a scene. */
void sb_scene_init(SB_Scene * scene);
/** Free what a scene holds and zero it. */
void sb_scene_free(SB_Scene * scene);
/** Append; 0 on success, -1 when out of memory. */
int sb_scene_add_item(SB_Scene * scene, const SB_Item * item);
int sb_scene_add_cmd(SB_Scene * scene, char op, const double * p);
int sb_scene_add_glyph(SB_Scene * scene, uint32_t id, double x, double y);
/** Append a gradient (its stops already appended, from first_stop); 0 on success. */
int sb_scene_add_gradient(SB_Scene * scene, const SB_Gradient * gradient);
int sb_scene_add_stop(SB_Scene * scene, double offset, SB_Color color);
/** Append a stroke style; 0 on success. */
int sb_scene_add_stroke(SB_Scene * scene, const SB_Stroke * stroke);
/** Items that draw (fills, runs and strokes). */
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
