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
 *     clippath <count>                    push the clip of a path (anti-aliased, non-zero), then <count> commands as a fill's
 *     unclip                              pop the innermost clip
 *     group <composite> <opacity> [linear] [mask <n> <dx> <dy>]
 *                                         begin an isolated group: what follows, up to the matching `endgroup`, is drawn on
 *                                         a transparent layer of the extent, and the layer is then composited onto what is
 *                                         beneath with the named operator after its result is scaled by <opacity> (0 to 1);
 *                                         groups nest, and nest well with the clips.  `linear` says the group composites in
 *                                         linear light (its layer and the backdrop converted from the target's encoded
 *                                         values, blended there, and converted back), which is what shape-scene's
 *                                         `blend_space` says; a reference whose layers blend in the encoded values only
 *                                         draws it encoded, and its driver says so.  `mask` names the n-th mask defined
 *                                         above (counted from 1), placed by the translation (<dx>, <dy>) in pixels, so
 *                                         that one definition serves many places: the group's layer is multiplied, in all
 *                                         four premultiplied channels, by the mask's value at each pixel before it is
 *                                         scaled by <opacity> and composited (see below)
 *     endgroup                            end it
 *     mask alpha                          define a mask whose value is its picture's alpha, with the records up to the
 *     mask luminance <coeff> <r> <g> <b>  matching `endmask`: the picture.  `luminance` defines one whose value is the
 *                                         luminance of the picture composited over the opaque backdrop <r> <g> <b> (8-bit
 *                                         straight sRGB, the target's encoded values, like every other color here), by
 *                                         the coefficient set <coeff> (`w3c`, `rec709` or `lum`, scene.c's
 *                                         sb_coefficients) applied to the premultiplied components: shape-scene's mask
 *                                         (mask.h) with the identity transfer, the render and luminance spaces left to
 *                                         the target's, and the mask acting on the opacity.  A definition stands at the
 *                                         top level, outside every clip and group, and holds only `fill` and `fill-eo`
 *                                         records (each with its gradient where it has one): the picture, in the mask's
 *                                         own space, which a group's translation then places.  Nothing in it is drawn
 *                                         where it stands
 *     endmask                             end it
 *     pattern <x> <y> <w> <h> <xstep> <ystep> <rx> <ry> <a> <b> <c> <d> <e> <f> [tint <r> <g> <b> <a>]
 *                                         define a pattern (shape-scene's pattern.h, PT1) with the records up to the matching
 *                                         `endpattern`: its tile, a picture of fills in pattern space.  The cell is the rectangle
 *                                         [x, x + w] x [y, y + h] of that space, the copy (i, j) is the tile moved by (i xstep,
 *                                         j ystep), <rx> and <ry> (0 or 1) say whether the lattice repeats along each axis, and the
 *                                         map takes pattern space to the device by x' = a x + c y + e, y' = b x + d y + f (SVG's
 *                                         matrix order).  With `tint` the pattern is uncolored (PDF's PaintType 2): its colors are
 *                                         the tint's and its alpha the tile's, times the tint's alpha.  A definition stands at the
 *                                         top level, outside every clip and group, and holds only `fill` and `fill-eo` records of
 *                                         solid colors: nothing in it is drawn where it stands
 *     endpattern                          end it
 *     patternpaint <n>                    the paint of the fill that follows it, and nothing else: the n-th pattern defined above
 *                                         (counted from 1).  That fill's color is then `0 0 0 a`, `a` the alpha multiplier
 *     fill <r> <g> <b> <a> <count>        a path filled non-zero, then <count> commands:
 *     M x y | L x y | Q x1 y1 x y | C x1 y1 x2 y2 x y | Z
 *     fill-eo <r> <g> <b> <a> <count>     as `fill`, by the even-odd rule
 *     run <r> <g> <b> <a> <count>         a glyph run, then <count> lines "g <id> <x> <y>" (baseline origin)
 *     stroke <r> <g> <b> <a> <width> <join> <cap> <miter> <offset> <ndash> <dash>... <count>
 *                                         a path stroked (strokes story S4): then <count> commands as a fill's.
 *                                         <join> is miter, round or bevel; <cap> butt, round or square; the width is in
 *                                         pixels under the identity map (every engine's local width); <miter> the miter
 *                                         limit (`miterclip` and `arcs` are SVG 2's joins, which a reference may not have:
 *                                         its driver then names the nearest it has); <ndash> lengths in pixels (an even count, 0 for solid) restarting at each
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
 * A gradient record paints the fill (or stroke) record that comes next, and nothing else:
 * any other record after it is an error.  That item's color is then `0 0 0 a`,
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
 *
 * A mask's value at a pixel is shape-scene's (mask.h, MK2) for the mask's picture, translated by the group's placement and
 * drawn into a transparent isolated layer of the extent in the target's encoded values: `alpha` is the layer's alpha a;
 * `luminance` is y = k . (c + (1 - a) B), with c the layer's premultiplied color, B the backdrop and k the coefficients,
 * all as values in [0, 1], clamped to [0, 1].  Where the picture draws nothing the layer is (0, 0, 0, 0), so the value
 * is 0 for `alpha` and k . B for `luminance`, at every pixel of the extent.  A reference with no such mask builds it
 * from layers, and its driver says how.
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

typedef enum SB_Kind {
  SB_FILL = 1,
  SB_RUN = 2,
  SB_CLIP = 3,
  SB_UNCLIP = 4,
  SB_STROKE = 5,
  SB_GROUP = 6,
  SB_ENDGROUP = 7,
  SB_CLIPPATH = 8,
  SB_MASK_BEGIN = 9, ///< a mask definition's start; `count` is the index of its matching SB_MASK_END, `mask` its index
  SB_MASK_END = 10,
  SB_PATTERN_BEGIN = 11, ///< a pattern definition's start; `count` is the index of its matching SB_PATTERN_END, `pattern` its index
  SB_PATTERN_END = 12
} SB_Kind;

typedef enum SB_Join {
  SB_JOIN_MITER = 0,
  SB_JOIN_ROUND = 1,
  SB_JOIN_BEVEL = 2,
  SB_JOIN_MITER_CLIP = 3,
  SB_JOIN_ARCS = 4
} SB_Join;

/** The composite operators of a group, in shape-scene's order (the names are those the file spells, lower case). */
typedef enum SB_Composite {
  SB_COMPOSITE_SRC_OVER = 0,
  SB_COMPOSITE_CLEAR,
  SB_COMPOSITE_SRC,
  SB_COMPOSITE_DEST,
  SB_COMPOSITE_DEST_OVER,
  SB_COMPOSITE_SRC_IN,
  SB_COMPOSITE_DEST_IN,
  SB_COMPOSITE_SRC_OUT,
  SB_COMPOSITE_DEST_OUT,
  SB_COMPOSITE_SRC_ATOP,
  SB_COMPOSITE_DEST_ATOP,
  SB_COMPOSITE_XOR,
  SB_COMPOSITE_PLUS,
  SB_COMPOSITE_SCREEN,
  SB_COMPOSITE_OVERLAY,
  SB_COMPOSITE_DARKEN,
  SB_COMPOSITE_LIGHTEN,
  SB_COMPOSITE_COLOR_DODGE,
  SB_COMPOSITE_COLOR_BURN,
  SB_COMPOSITE_HARD_LIGHT,
  SB_COMPOSITE_SOFT_LIGHT,
  SB_COMPOSITE_DIFFERENCE,
  SB_COMPOSITE_EXCLUSION,
  SB_COMPOSITE_MULTIPLY,
  SB_COMPOSITE_HUE,
  SB_COMPOSITE_SATURATION,
  SB_COMPOSITE_COLOR,
  SB_COMPOSITE_LUMINOSITY,
  SB_COMPOSITE_COUNT
} SB_Composite;

/** The name a composite is spelled with in a scene file. */
const char * sb_composite_name(SB_Composite op);
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

/** What a mask takes from its picture. */
typedef enum SB_Mask_Mode { SB_MASK_ALPHA = 1, SB_MASK_LUMINANCE = 2 } SB_Mask_Mode;

/** The luminance coefficient sets, in shape-scene's order (mask.h, MK3). */
typedef enum SB_Coefficients { SB_COEFF_W3C = 0, SB_COEFF_REC709 = 1, SB_COEFF_LUM = 2, SB_COEFF_COUNT } SB_Coefficients;

/** The name a coefficient set is spelled with in a scene file. */
const char * sb_coefficients_name(SB_Coefficients c);
/** The set as exact integers: k[i] / @p den are the coefficients of red, green and blue (their sum is @p den). */
void sb_coefficients(SB_Coefficients c, int k[3], int * den);

/** A mask's definition; its picture is the items [first_item, first_item + n_items) of SB_Scene.items, all fills. */
typedef struct SB_Mask {
  SB_Mask_Mode mode;
  SB_Coefficients coefficients; ///< luminance only
  SB_Color backdrop;            ///< luminance only: opaque
  size_t first_item, n_items;
} SB_Mask;

/** A pattern's definition (pattern.h): its tile is the items [first_item, first_item + n_items) of SB_Scene.items, all solid fills. */
typedef struct SB_Pattern {
  double cell[4];  ///< x, y, w, h of the cell in pattern space
  double step[2];  ///< x_step, y_step
  int repeat[2];   ///< repeat_x, repeat_y
  double map[6];   ///< a, b, c, d, e, f: x' = a x + c y + e, y' = b x + d y + f
  int has_tint;    ///< uncolored: the tile's colors are replaced by the tint
  SB_Color tint;
  size_t first_item, n_items;
} SB_Pattern;

typedef struct SB_Item {
  SB_Kind kind;
  SB_Color color;  ///< fill, run; a gradient fill's is 0 0 0 and its alpha multiplier
  size_t first;    ///< fill: the first command; run: the first glyph
  size_t count;    ///< fill: commands; run: glyphs
  double rect[4];  ///< clip: x0, y0, x1, y1
  size_t gradient; ///< fill: 1 + the index of its paint in SB_Scene.gradients; 0 for a solid color
  size_t stroke;   ///< stroke: 1 + the index of its style in SB_Scene.strokes
  size_t pattern;  ///< fill: 1 + the index of its pattern in SB_Scene.patterns; 0 for none.  SB_PATTERN_BEGIN: 1 + the index it defines
  int evenodd;     ///< fill: the even-odd rule (else non-zero)
  int composite;   ///< group: an SB_Composite
  double opacity;  ///< group: 0 to 1
  int linear;      ///< group: composites in linear light (sRGB primaries)
  size_t mask;     ///< group: 1 + the index of its mask in SB_Scene.masks; 0 for none.  SB_MASK_BEGIN: 1 + the index it defines
  double mask_dx, mask_dy; ///< group: the mask's placement, a translation in pixels
  int nonisolated;         ///< group: PDF's non-isolated group (its content composites with what was painted before it)
  int knockout;            ///< group: PDF's knockout group (each element composites with the group's initial backdrop)
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
  SB_Mask * masks;
  size_t n_masks, cap_masks;
  SB_Pattern * patterns;
  size_t n_patterns, cap_patterns;
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
/** Append a mask definition; 0 on success. */
int sb_scene_add_mask(SB_Scene * scene, const SB_Mask * mask);
/** Append a pattern definition; 0 on success. */
int sb_scene_add_pattern(SB_Scene * scene, const SB_Pattern * pattern);
/** Items that draw (fills, runs and strokes) and are not the picture of a mask or the tile of a pattern: the control drops these. */
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
