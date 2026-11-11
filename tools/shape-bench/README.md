# The shape benchmark

How fast the shape family draws, measured against Blend2D and Skia. The engines measured are `shape-cpu`
(through `shape-scene`'s lists and its sink) and, for text, `font`'s glyph
rendering; the references are Blend2D and Skia's CPU raster backend, each
built from a pinned upstream commit inside a pinned image and used only as a
black box through its public API.

**This file holds no numbers.** The results are in `results/`, one JSON
record per run with the full provenance and a Markdown table generated from
it. A figure quoted anywhere else is read from there, with its date and CPU.

## What is measured

| Scene | Metric | One frame |
| --- | --- | --- |
| `fill-rect` | fill rate (Mpixel/s over the shapes' area) | 12 translucent rectangles, edges off the pixel grid, 1920x1080 |
| `fill-circle` | fill rate | 12 translucent circles of radius 300 to 500 (cubic arcs) |
| `paths` | path count (paths/s) | 10,000 small paths: triangles, quadratic blobs, cubic circles, stars |
| `text` | text throughput (glyphs/s) | a page of 16-pixel DejaVu Sans 2.37, unhinted, quarter-pixel positions |
| `ui` | list build and submit time per frame | 24 panels of rounded rectangles, icons, bars and rectangular clips, 1280x800 |
| `strokes` | path count (paths/s) | 2,500 stroked paths of 30 to 250 pixels and a width of 1 to 9, translucent, 1920x1080: a miter-joined polyline, round-joined and round-capped quadratics, a bevel-joined closed polygon, a round-joined circle, a zigzag with square caps and a miter limit of 2 |
| `strokes-dashed` | path count | 800 dashed open cubic waves (butt or round caps), as `strokes`; drawn by shape-cpu and Skia only: this Blend2D (58ca946) accepts a dash array on its context and strokes solid, so its arm omits the scene (the minimal pair is in the strokes story's change log) |
| `gradients` | fill rate | 12 opaque rectangles and circles overlapping on a 4x3 grid, each a gradient of 2 to 5 opaque stops: linear pad, repeat and reflect; radial concentric pad, repeat and reflect, and one focal pad; sweep over a whole turn (pad, and repeat from 30 degrees) and a third of a turn repeated, 1920x1080 |
| `groups` | groups per second (shape-cpu only: no reference draws it) | fifteen groups of three shapes on 1600x1000, three each of a non-isolated multiply group, a knockout group, a knockout group around a non-isolated group around a knockout one, the multiply group isolated (the control) and a non-isolated knockout screen group |
| `masks` | masked groups per second | 15 groups of three or four fills on 1600x1000, each multiplied by a mask the group places by a translation: four alpha masks (a ring with a translucent core, a linear fade, a radial spot, three translucent circles) and four luminance masks by Rec. 709's coefficients (a ramp, the three primaries, translucent shapes over a gray backdrop, black spots over a white one), source-over, multiply and screen at opacities of 0.6 to 1, over six large translucent shapes; Blend2D and Skia build each mask from layers ([Masks](#masks)) |
| `patterns` | fill rate | 24 fills painted with a pattern on 1600x1000: twelve large shapes on a 4 by 3 grid and a smaller translucent one over each, every one with a pattern of its own (a checker, a hatch, dots, and an uncolored hatch under a tint; a cell of 8 or 16 pixels), every lattice axis aligned with steps of the cell's whole-pixel size at an integer translation ([Patterns](#patterns)) |
| `patterns-rotated` | fill rate | the same 24 under rotations of 0 to 135 degrees, uniform scales of 1 to 2 and translations that are not whole pixels; one is a quarter turn at an integer translation |
| `images` | fill rate (Mpixel/s over the covered area; shape-cpu only: `build/images [frames]`) | 12 opaque rectangles on 1920x1080, each painted with a 256x256 RGBA8 image of fixed pseudo-random texels: nearest and linear at 1:1, a 4x magnification (nearest, linear, cubic), a 0.37x minification (linear, and the area filter under repeat and pad), and a 1.7x rotation of 30 degrees (linear, cubic). No reference is run yet: the area filter is shape-scene's own definition, and a comparison needs a minimal pair per case first |
| `clip-unions` | path count (fills of the cells per second) | 720 cells on 1600x1000, each clipped to a union of 3 to 6 rectangles, circles and triangles placed at random in the cell (some overlap, some stand apart) around a translucent rectangle and a translucent circle; the union is one path of same-wound subpaths, the clip of a reference, and a clip union of its members for shape-cpu |

`bench-shape emit` writes the scenes once (`scene.h` gives the format); every
driver reads the same files through the same reader (`scene.c`, compiled into
the C++ drivers too).

## Fairness, and what cannot be made equal

- One thread everywhere (Blend2D `thread_count = 0`; Skia's raster backend and
  `shape-cpu` are single-threaded), every engine pinned to the same CPU.
- The same target: caller-owned 8-bit RGBA, premultiplied (Blend2D's only
  premultiplied 32-bit format is BGRA in memory; the comparison converts it),
  cleared to the background outside the timed region by the same loop.
- Source-over of straight 8-bit sRGB colors in the target's own encoded values,
  non-zero winding, anti-aliasing on as each engine does it.
- Gradients are interpolated in the same encoded values, straight, with no
  dithering anywhere: Skia's paint has dithering turned off, and Blend2D's
  gradient quality hint is set to its undithered `BL_GRADIENT_QUALITY_NEAREST`.
  The `gradients` scene holds only the sub-cases every engine draws by one
  definition (shape-scene's G7 to G9); the generator (`bench-shape.c`) names
  what is left out and why, and each driver states its mapping. Its stops are
  opaque, so premultiplied and straight interpolation agree.
- Resources are built before timing in every engine: `shape-cpu`'s interned
  paths, glyph runs and gradients, Blend2D's `BLPath`s, glyph arrays and
  `BLGradient`s, Skia's `SkPath`s, `SkTextBlob`s and gradient `SkShader`s.
- A frame is timed from the engine's first call until its output is complete.
  For `shape-cpu` that is recording the list (the build) and rendering it (the
  submit); for Skia on the `ui` scene, recording an `SkPicture` and playing it
  back, and elsewhere drawing directly; Blend2D is immediate and has no build.
- Warm-up of at least 5 frames and 0.25 s, then 31 timed frames: the median,
  the quartiles, the minimum and maximum. Throughput is derived from the median.
- Strokes are each engine's own: shape-cpu's expansion in `shape-geometry`, Blend2D's stroker and dasher through the
  context's stroke options (a miter join past the limit is a bevel: `BL_STROKE_JOIN_MITER_BEVEL`), Skia's stroking
  `SkPaint` and `SkDashPathEffect`. The scene states only what the three say by one definition (SVG's width, join, cap,
  miter limit, and dashes on open subpaths restarting at the path's start); the generator names what is left out.
  The `strokes` frames of `shape-cpu` run with the sink's expansion cache warm after the first frame (`GSHCPU_Limits`
  `max_cache_bytes` of 256 MiB; the 16 MiB default holds about an eighth of the scenes' expansions and would evict them each frame), which is what a retained scene sees; there is no speed threshold, only the cross-check below.
- What differs by design is stated, not hidden: Skia caches rendered glyph
  masks and nothing else here does; Blend2D JIT-compiles its pipelines and Skia
  dispatches SIMD at run time, while `shape-cpu` is the scalar `double`
  reference (SIMD tiers, tiles and threads are build step 5); `font` has no
  compositor, so its text frame includes this harness's own blit.

## The cross-check

A fast wrong answer must not win. Each engine's last frame is compared with
`shape-cpu`'s (`pamdiff`): the mean absolute difference over every channel at
most 1.0 (8-bit levels) and pixels with a channel more than 32 levels apart at
most 0.1 %. An engine outside the bound on a scene is marked invalid there and
not ranked. The control, `shape-cpu` with every sixteenth drawable dropped,
must exceed the bound on every scene, or that scene's check is reported blind
and the run fails. `report.py` holds the bound.

**The stroke scenes (`strokes`, `strokes-dashed`) are judged by corroboration**
(Corey, 2026-10-22): two references' antialiasing and curve approximation differ
from each other as much as either differs from `shape-cpu`, so a pixel where
`shape-cpu` differs from a reference by more than 32 levels counts against it
only when a second, independent source also differs from `shape-cpu` there by
more than 32. The second source is the other reference engine where it draws
the scene (`strokes`: Blend2D and Skia check each other), and otherwise the
geometric oracle of the stroke (`strokes-dashed`, which Blend2D does not draw):
the exact region of each stroke from the definition alone, sampled sixteen by
sixteen in the pixel and composited in the scene's order with its paints,
evaluated only at the pixels in question (`corroborate.cpp`, using the oracle
header `shape-geometry` installs for the tests of a sink). The bounds do not
change and are measured on the counted pixels, with the corroborated pixels'
differences taken as zero in the mean. Two controls keep the rule honest: the
dropped-drawable frame, judged by the same rule against each reference, must
exceed the bound against every one, and a frame with 3000 of its drawn pixels
inverted must still have those pixels counted and exceed the bound, or the rule
would be hiding wrong frames and the scene is reported blind.

## Masks

The `masks` scene draws groups whose layer is multiplied by a mask: shape-scene's mask is a picture and a value taken from it at
each pixel, and a group names one with a placement (`scene.h` has the record: `mask alpha` or `mask luminance <coefficients>
<r> <g> <b>`, its picture of fills up to `endmask`, and `mask <n> <dx> <dy>` on a `group`). An alpha mask's value is the picture's
alpha; a luminance mask's value is the weighted sum of the picture's premultiplied color over an opaque backdrop, in the target's
encoded values, by a set of coefficients. The scene's masks use the identity transfer, the target's spaces, and act on the
group's opacity. Neither reference has a mask on a group, so each driver builds one from layers when the group ends, before its
layer is composited:

- **Skia** (`refs/bench-skia.cpp`): the mask's picture is drawn into a `saveLayer` whose paint has the blend mode `kDstIn`, which
  multiplies the group's layer by the mask layer's alpha when the mask layer is restored; for a luminance mask that paint carries
  `SkLumaColorFilter`, and the backdrop is painted into the layer first.
- **Blend2D** (`refs/bench-blend2d.cpp`): the picture is drawn into a spare layer (over the backdrop first for a luminance mask), the
  layer is made the mask's value (a luminance mask is converted by hand, a pixel at a time, to `(0, 0, 0, y)` with `y` the nearest
  level of the premultiplied color's weighted sum), and the group's layer is multiplied by it with `BL_COMP_OP_DST_IN`, as a path clip is.

**What is compared against what.** Alpha-mask groups are checked against both references, each by a layer and `DST_IN`.
Luminance masks are checked against Skia, whose luma filter is an independent implementation of the value. Blend2D has no luma
filter, so its driver converts by hand with the same numbers, and for luminance masks it checks the layer code only, not a luminance:
the record says so beside the scene's table.

**The coefficient set is Skia's, found by probe.** `refs/probe-skia-luma.cpp` draws opaque colors through `SkLumaColorFilter` into a
transparent surface and reads the alpha back, in the pinned Skia image. It compares the result with the nearest level of each of
shape-scene's coefficient sets, `W3C` (2125, 7154, 721) / 10000, `REC709` (2126, 7152, 722) / 10000 and `LUM` (30, 59, 11) / 100,
over 3,921 colors sampled from the 39,203 where `W3C` and `REC709` round to different levels (and `LUM` rounds to neither), with at
least a hundredth of a level to spare on either side of the rounding point: Skia's alpha equals the `REC709` level for all 3,921, and the `W3C` level and the `LUM` level for
none. A translucent layer gives the luma of the premultiplied color (the filter folds the layer's alpha in), within one level of
the exact value, which is what the mask's value is over a black backdrop. The scene's luminance masks therefore name `rec709`,
and Blend2D's hand conversion uses the same integers. The frame-level bound cannot tell `REC709` from `W3C` (they differ by at
most a twentieth of a level), so the probe is what says which set Skia has; the cross-check says the layer code and the
luminance agree.

**What the scene leaves out.** The scene holds only the sub-cases every engine draws by one definition, as the others do. Left out, because a reference
cannot say it or says it differently: a transfer other than the identity (neither reference has shape-scene's table, exponential and
stitching functions on a mask); a mask acting on the shape rather than the opacity (the two differ only for the unbounded
operators, which a masked group here does not use); luminance in a space other than the target's, and a picture rendered in
another (neither reference converts a layer's space); a placement other than a translation; a mask of a mask; a mask on a
non-isolated or knockout group; any coefficient set but Rec. 709's, which is Skia's only one; a picture of anything but fills (a
stroke or glyph run would add stroker and rasterizer differences to what the scene checks); and a group in linear light. Where
the picture draws nothing a mask is `(0, 0, 0, 0)` and its value is 0 for an alpha mask and the backdrop's luminance for a
luminance one; the scene covers this, because each group's content runs past its mask's picture.

**The control** is `shape-cpu` with every sixteenth drawable dropped, counted from the topmost, as for every scene; the drawables
are the groups' contents, never a mask's picture. It exceeds the bound on this scene against both references, so the check can see a wrong frame; a frame with the masks left
off the groups, or the luminance masks read as alpha masks, exceeds it by far more.

**The cross-check without a timing run.** `SHAPE_BENCH_FRAMES_ONLY=1` makes each reference driver draw one frame, write it and print no
measurement, and `SHAPE_BENCH_CONTROL=1 bench-shape frame DIR OUT masks` writes shape-cpu's frame and its control frame; `report.py`'s
`pamdiff` and bound are then applied to those frames as they are to the last frames of a run.

**The timing run** is the documented two phases, limited to this scene; all three engines run it, because no earlier run has
reference figures for it:

```bash
SHAPE_BENCH_CPU=8 SHAPE_BENCH_SCENES=masks SHAPE_BENCH_TAG=masks \
    sh "$WORK/ws/suite/tools/shape-bench/run.sh" time "$WORK"
cp "$WORK"/results/report/<date>-masks.json "$WORK"/results/report/<date>-masks.md tools/shape-bench/results/
```

The record is `results/<date>-masks.json`, with the Markdown generated from it beside it, holding the scene's table of three engines
(frame time, throughput in masked groups per second, the cross-check figures and the control) and the provenance this file states:
the images and their IDs, the compilers and flags, the CPU model, architecture and CPUs online, the governor and boost, the pinned
CPU and the load before and after each engine; it does not name the host.

## Patterns

The two pattern scenes fill shapes with shape-scene's pattern paint: a tile (a picture of fills in pattern space) repeated on a lattice
(`scene.h` has the records: `pattern <cell> <steps> <repeats> <map> [tint r g b a]` up to `endpattern`, and `patternpaint <n>` before the
fill that uses the n-th). The definition is the lattice of vector copies of the picture under the map, the area average over each pixel;
shape-cpu draws that (a lattice of whole-pixel steps is the case its period tier is built for; the harness reads only the public API
and so does not observe which tier drew a lattice, only the frame). Neither reference has a pattern of that kind, only an image that
repeats, so each driver does what a caller of that library would: it renders the tile once, before timing, into a bitmap of the cell in
device pixels with its own rasterizer (the fills of the definition, or in the tint's color with the tile's alpha for an uncolored
pattern) and paints the fill with that bitmap repeated under the map, bilinearly sampled: Blend2D's `BLPattern` with
`BL_EXTEND_MODE_REPEAT`, Skia's image shader with `kRepeat` and linear filtering. The map's linear part is divided by the bitmap's scale
(a similarity, whose scale makes the cell a whole number of pixels, which the scenes arrange) and its translation is moved to the bitmap's
origin, so the bitmap is resampled only by the rotation and the fraction.

**`patterns`: the aligned frame, judged by the standard bound.** Every step is the cell's size, 8 or 16 pixels, and every map is a
translation by whole pixels, so the picture a pattern paints is the tile rendered into a bitmap of the cell, repeated, which is also
what a reference draws: no pixel lies across two cells' pictures and no resampling happens. What remains is each rasterizer's antialiasing
of the edges inside the tile (the dots' circles, the hatch's diagonals) and of the filled shape's own edge, which every scene has. The bound
is the standard one (mean at most 1.0, pixels more than 32 levels apart at most 0.1 %), measured on the first run (`results/2026-11-05-patterns.md`):
Blend2D's mean is 0.155 with no such pixel, Skia's 0.109 with 12 (0.0008 %). The two references differ from each other by a mean of 0.154 and
0.19 % of the pixels past 32, more than the bound's fraction, so the bound is tighter on shape-cpu than the references are of each other on
this scene; each reference is inside it. Where the figures come from: the circles of the dots tile carry the largest mean per cell (about
0.4 to 0.6 levels, the other kinds 0.00 to 0.14; read from the first run's frames, not part of the record). The control, every sixteenth drawable dropped counted from the topmost (two of the 24
fills, both pattern fills), has a mean of 4.44 and 4.5 % of the pixels past 32 and exceeds the bound, so the check can see a wrong frame; a
lattice error is also seen: one of the 24 patterns (the checker of the top-left shape, 7 % of the frame) moved by one pixel gives means
of 1.22 and 1.18 and 0.9 % past 32 against the two references, outside the bound (this was run once for this record, from a scene file edited
by hand, and is not part of the run).

**`patterns-rotated`: a bound of its own, measured.** Here a reference cannot draw the definition: it resamples a bitmap of the tile
bilinearly under a rotation or a fraction, where the definition draws the vector tile under the map and averages it over each pixel. A
resampled hatch line a few pixels wide is blurred (lower at its center, wider at its edge), so the references differ from shape-cpu by
more than the standard bound by construction, and most on the thin hatches (the cells with the highest means are the 8-pixel hatches
under 22.5 and 75 degrees, about 5.5 levels, 6 % of their pixels past 32, read from the first run's frames and not part of the record). The two references agree with each other (mean 0.21,
0.097 % past 32), which is what says the difference is the definition against a resampled bitmap and not either engine. The bound is the
measured figures of the first run with half again as much room: mean at most 3.0 and pixels more than 32 levels apart at most 2.5 %
(measured: Blend2D 2.06 and 1.57 %, Skia 2.05 and 1.62 %; maxima 62 and 68 levels). It is calibrated on that run, so it says that the
frames are the same pictures to within what resampling costs, not that they are equal; the control (4.16 and 5.02 %) exceeds it, and so
does one pattern of the 24 (an uncolored hatch of 8-pixel cells, at 37.5 degrees) turned by half a degree about the origin (mean 3.05, 3.9 % past 32; run once for this record, as above). It
would not see an error smaller than that. `report.py` holds the bound (`SCENE_BOUNDS`) and the record says it beside the scene's table,
with the references' agreement.

**What the scenes leave out**, because a reference cannot say it by one definition: a step other than the cell's size (CSS `space`,
PDF's steps smaller than the cell), a repeat along one axis alone, a cell that is not a whole number of pixels once scaled, a map that
is not a similarity (a skew or a mirror), a tile holding a stroke, a gradient, a group or a pattern, and a tint with an alpha other
than 1. (Blend2D and Skia could each be driven to draw some of these by a different construction, a bitmap tiled by hand; that would
be the benchmark's own lattice code and not the reference's.) The tile's colors are straight 8-bit sRGB, composited in the target's
encoded values as everywhere else here.

**The control** is the usual one: shape-cpu with every sixteenth drawable dropped, the drawables being the 24 pattern fills, never a
tile's fills (`sb_scene_drawables`).

**Timing** is the documented two phases, limited to the two scenes, all three engines running them:

```bash
SHAPE_BENCH_CPU=8 SHAPE_BENCH_SCENES="patterns patterns-rotated" SHAPE_BENCH_TAG=patterns \
    sh "$WORK/ws/suite/tools/shape-bench/run.sh" time "$WORK"
cp "$WORK"/results/report/<date>-patterns.json "$WORK"/results/report/<date>-patterns.md tools/shape-bench/results/
```

What the figures compare: shape-cpu draws the lattice of vector copies from the picture on every frame (the scalar double reference
sink); Blend2D and Skia draw a bitmap that was rendered before timing, whose rendering is not in their frame, so they are timed doing
less work than the definition asks and the frame-time ratio is not a like-for-like speed of the same computation. The rotated scene is
the general tier of shape-cpu for all but one lattice and a resampling of a bitmap for the references.

**Where shape-cpu's time goes in `patterns`** (the aligned scene, one frame, measured as instructions under callgrind because the figures of a
shared machine move by tens of percent from run to run; the count includes the one-time scene setup and the harness's own
PAM write, 2.6 %). Two things were avoidable and are fixed. A scan for the sticky bit in `gshgeom_exact_sum_to_double` tested up to 25,000 bits
one at a time for every lattice constraint of every use: 44 % of the frame; it is now read a word at a time (`shape-geometry`). The period tier
reduced every pixel of a span to its window with three 64-bit divisions; it now steps the column along the row (`shape-cpu`), 6 % of the frame at that point. The frame was 1.26 G instructions before the two changes and is 0.67 G after them (the commit before the period tier: 2.50 G). What
remains is the cost of the definition's arithmetic, and it is per pixel, not per copy: the span receiver that composes each covered pixel in
`double` (26 %), the rounding of each pixel to the 8-bit target (16 %), the read of the lattice layer's pixel and its tint (12 %), the clearing of
the layers (6 %), the layer's load and the write to the target (9 %), and the exact constraints that enumerate each use's copies (13 %, paid
once for each of the 24 fills). **This is a different workload from the references' frame**: the reference drivers read a bitmap that was
rendered before timing (`refs/bench-blend2d.cpp`, `refs/bench-skia.cpp`: each pattern's tile is drawn once at setup) and copy it through SIMD paths with an 8-bit
pipeline, so what is timed there is a blit of 1.46 M pixels; shape-cpu is the scalar reference sink, which composes in `double` and is
bit-identical on every host, and draws the lattice from the vector picture's period on every frame. A faster path for the same output would
be a lean 8-bit tier for a pattern (the plain fill has one), which is a design of its own and not part of this scene.

## Union clips: what each tier costs

`build/clip-unions [frames [plain]]` (shape-cpu only: no reference draws a union clip in one call) times an icon sheet of 1500 icons each
clipped to a union of 2 to 12 shapes, as a union of members apart (the fast tier), as nested clips, and with the members overlapping (the
general tier, the exact step in the pixels the members share), then one union of n members in a chain, apart and overlapping, which is the
cost of a member of each tier. A run is one process on whatever machine it is on; the numbers are only comparable with the same program's
on the same machine and load.

## Non-isolated and knockout groups: a scene no reference draws

The `groups` scene draws groups of PDF's two kinds: fifteen groups of three shapes on 1600x1000, three of each of five kinds in the
cells of a 5 by 3 grid. A non-isolated multiply group, whose content multiplies with what was painted before it; a knockout group of
three overlapping translucent shapes, each composited with the group's initial backdrop and not with the shapes before it; a knockout
group holding a non-isolated one holding a knockout one; the same multiply group isolated, as the control, which takes none of the
code the others take; and a non-isolated knockout screen group. A scene line spells the flags as `nonisolated` and `knockout` after the
group's operator and opacity.

**Neither Blend2D nor Skia has either flag, so no reference draws this scene and no cross-check applies**: the default lists of the
two reference drivers do not name it, and `report.py` does not table it. It records shape-cpu's figure with its provenance, and one
claim that figures of other scenes can carry: a list that sets neither flag takes none of the code these groups take, so the
default-group scenes already here (`ui`, `masks`) are no slower than before the flags were composited. That is measured, not asserted:
shape-cpu is built from the commit before the flags and from the one after, `bench-shape run` draws the three scenes (interleaved,
pinned to one CPU, on the same machine), and the results file `<date>-groups.md` holds both columns and the spread of each. The
spread is the noise the claim is judged against.

## COLRv1 against Skia

`colr/` holds the comparison of `font`'s lowering of COLRv1 with Skia's renderer (color-paint story P4). Each glyph of a font
with a color description is drawn twice, unhinted, palette 0, a black foreground, at the same size and origin into an 8-bit
RGBA premultiplied cell: by `shape-cpu` from `font`'s lowered list (`colr-shape.c`, lowered in the encoded space, the one
Skia draws in) and by Skia's CPU raster backend in the pinned image (`colr-skia.cpp`, public API only). `colr-cmp.c`
compares the two atlases cell by cell with the measure above: the mean absolute difference over every channel of the
cell, and the fraction of the cell's pixels that differ by more than 32 levels, counted as `tbig`: a pixel whose value is
also more than 32 levels outside the range of the other picture's values within one pixel, either way round. That is the
bound's second figure for glyphs. Two engines that antialias an edge differently put different values on an edge pixel (a
straight edge a sixth of a pixel apart is 38 levels apart along its whole length), and over a 128-pixel cell the raw count
fails for four glyphs in five of Noto Color Emoji with nothing wrong: the bound's figure was set for whole frames of large
shapes. A wrong color, a gradient running the wrong way or a layer in the wrong place is out of range. The bound is the mean at
most 1.0 and `tbig` at most 0.1 % of the cell (`colr-report.py`); `skia-allow.txt` lists the glyphs outside it, each with
the reason, and the gate fails when a glyph outside is not listed and when the list names one that is inside. The control
leaves out every eighth fill of each of the first two hundred glyphs of the first configuration and must put at least half of
the glyphs it changes outside the bound, or the gate reports itself blind.

`configs.txt` names the runs: Noto Color Emoji's COLR version 1 build (every glyph at 128 pixels, a sample of two hundred at
24), Nabla at four instances and a sample at 24, Google's test glyphs and Twemoji, and `font`'s committed fixtures (version 0 and
version 1, the variable ones at the default and at a corner). The fixtures state a left side bearing of zero for outlines that
start at 40 to 100 units, so they are the runs that hold `font`'s placement of an outline to a renderer's pixels. `run-colr.sh` has the phases (`build`,
`shape`, `skia` inside Skia's image, `compare`). The fonts are fetched by digest (`libs/font/tools/oracle/colr_real_fonts.py`)
and are never committed.

## The pictures

Two large pictures, each a whole composition of gradients, strokes, clips, groups and glyph outlines at 2400x1500, are drawn
by every engine from one description, and timed. They are not generated here: a program that records a picture as a
`shape-scene` list walks the closed list and writes it as a scene of this directory's format, named `picture-<name>`
(`scene.h`; the walker puts every path in device space, a glyph run as the outlines of `font`'s own outline provider, a
stroke with its width, join, cap, miter limit and dashes, a gradient with its geometry and its stops as 8-bit encoded values,
and every group and clip). So that every engine rasterizes the same curves, text is **glyph outlines drawn as paths**: shaping,
outline loading and glyph caches are not what is timed.

The scene format carries what these pictures need beyond the benchmark's scenes: `group <composite> <opacity> [linear]` and
`endgroup` (an isolated layer of the extent, composited with the named operator after its result is scaled by the opacity),
`clippath`, `fill-eo`, the `miterclip` and `arcs` joins, and a gradient before a stroke as well as a fill.

Three drivers draw a picture's scene, by their public API only, and two further arms time `shape-cpu`:

- `shape-cpu` (scene arm): `bench-shape pictures` reads the scene file as it reads any scene. This is the like-for-like arm.
- `shape-cpu` (own-list arm): the program that records the picture re-records its list each frame from resources interned once
  and renders it, strict; it has the glyph runs, the palette and the linear-light gradient as they are, so it is the picture as
  `shape-cpu` draws it. `run.sh pictures` runs it through `SHAPE_BENCH_NATIVE_CMD`.
- Blend2D (`refs/bench-blend2d.cpp`): groups are `BLImage` layers of the extent blitted with the group's comp-op and its opacity
  as the global alpha; a path clip is a layer multiplied by the path's coverage (`BL_COMP_OP_DST_IN`); a sweep over part of a
  turn is the color line sampled into stops over one turn of the conic gradient; a dashed stroke is made by
  `BLPath::add_stroked_path` with the dash array, which this version also draws solid, so Blend2D's pictures have solid
  strokes where the others have dashes.
- Skia (`refs/bench-skia.cpp`): `saveLayer` with a paint holding the blend mode and the opacity, `clipPath`, `SkShaders` for every
  gradient.

**What the references cannot say by the same definition** is listed in each record, found by reading the scene
(`report-pictures.py`): a group `shape-cpu` composites in linear light (the color-glyph lowering's choice) is composited in the
target's encoded values by both references, which differs by construction wherever translucent layers overlap; Blend2D has no
hue, saturation, color or luminosity operator; Skia has no miter-clip or arcs join and Blend2D has no arcs join (drawn as the
miter, a bevel past the limit); Blend2D clips to rectangles only; a gradient `shape-cpu` interpolates in linear light is sampled
into encoded stops in the scene. A speed for a picture a reference draws differently is for that picture.

The comparison is with `shape-cpu`'s own picture (the PNG of its list), not with a reference: the share of the pixels whose
largest colour-channel difference is past 1, 4 and 16 levels, the mean absolute difference, the largest difference and the
tiles where the large differences are; `report-pictures.py --images DIR` also writes a side-by-side image of the four pictures
and a heatmap of each reference's difference (it needs numpy and Pillow). The protocol is the benchmark's (one thread, one
pinned CPU, warm-up, 31 timed frames and the median), the whole run repeated `SHAPE_BENCH_REPS` times (three) with the median
of the repetitions' medians as the figure and their spread beside it, and, for `shape-cpu`, the build (recording the list) and
the submit (rendering it) apart and together, with the first frame, whose expansion cache is cold, timed apart from the warm ones.

```bash
# on the machine, after `run.sh build` and the program that wrote results/scenes/picture-*.scene and results/pictures/native/*.png:
SHAPE_BENCH_CPU=8 SHAPE_BENCH_NATIVE_CMD='...' sh "$WORK/ws/suite/tools/shape-bench/run.sh" pictures "$WORK"
python3 tools/shape-bench/report-pictures.py --results "$WORK"/results --shipped "$WORK"/SHIPPED --images DIR
```

## Provenance

Every result carries the image reference and its image ID, the base image's
digest, the reference's version and commit (`containers/IMAGES` and the image's
own `PROVENANCE`), the compiler and its flags (the library's flag stamp for
`shape-cpu`, Blend2D's compile command, Skia's gn arguments and defines), the
CPU's model name, its architecture and the number of CPUs online, the governor
and boost, the pinned CPU, and the machine's load average, how many other
containers ran and the CPU shares of the busiest processes before and after
each engine ran. The record does not name the machine: `run.sh` does not
record its host name, its kernel string or the names and owners of other
processes, and `report.py` drops them from a record that has them.

## Running it

Everything runs in containers: the libraries and the harness are built in the
suite's build image, `ghoti-build:gcc16` (`suite/Containerfile.build`), and each
engine is timed in its own image. It wants a machine with a container engine
(docker, or podman through `SHAPE_BENCH_CLI=podman`), a few GB of memory for
the build, and a CPU that nothing else is using for the timing.

The references' images, once, from the suite's checkout (Skia's build is heavy:
up to 24 GB of memory and two hours):

```bash
docker build -t localhost/ghoti-shape-bench-blend2d:0.21.3-2 \
    -f tools/shape-bench/containers/blend2d/Containerfile tools/shape-bench/containers/blend2d
docker build -t localhost/ghoti-shape-bench-skia:m156-2 \
    -f tools/shape-bench/containers/skia/Containerfile tools/shape-bench/containers/skia
```

A run lives in one directory, `WORK`, which holds the sources under `ws/`
(`libs/<lib>` beside `suite/`, as `suite/clone.sh` lays them out; clones of the
commits you want measured), the install prefix, the build and the results.
`run.sh` gives the layout. Its two phases:

```bash
# Build: install the libraries, build the harness, write the scenes, and run
# shape-cpu's make test before anything is timed. Inside the build image, with
# WORK mounted:
docker run --rm --user "$(id -u):$(id -g)" -v "$WORK:/work" -w /work ghoti-build:gcc16 \
    sh ws/suite/tools/shape-bench/run.sh build /work 8

# Time: on the machine itself, every engine in its own image, pinned to one CPU,
# then the cross-check and the results files:
SHAPE_BENCH_CPU=8 sh "$WORK/ws/suite/tools/shape-bench/run.sh" time "$WORK"
```

The results files land in `WORK/results/report/`, named `<date>.json` and
`<date>.md` (or `<date>-<tag>.*`, from `report.py --tag`); a run worth keeping
is copied into `results/` here:

```bash
cp "$WORK"/results/report/<date>*.json "$WORK"/results/report/<date>*.md tools/shape-bench/results/
```

Run it on a quiet machine: the results record what else was running, and a
loaded machine is a reason to run again, not a footnote.

### Re-measuring shape-cpu alone

A change to `shape-cpu` does not move the references, so their numbers can stay
and only `shape-cpu` (and `font`, which runs in its process) is timed again:

```bash
SHAPE_BENCH_ENGINES=shape-cpu sh "$WORK/ws/suite/tools/shape-bench/run.sh" time "$WORK"
cc -O2 -o pamdiff tools/shape-bench/pamdiff.c     # it needs no library
python3 tools/shape-bench/report.py --results "$WORK"/results --pamdiff ./pamdiff \
    --shipped "$WORK"/SHIPPED --reuse EARLIER/results --tag TAG
```

`--reuse` names the results directory of the run whose Blend2D and Skia figures
are kept (raw, prov, logs and frames); their frames are cross-checked again,
against the new `shape-cpu` frames, and the record names the run they came
from. A run from before a scene existed has no reference figures for it, so
that scene's table then shows `shape-cpu` alone. `--tag` keeps a second run on
one date from overwriting the first. `report.py --markdown RECORD.json` writes
a record's Markdown again from the record alone.

## Clip unions

The `clip-unions` scene (shape shown in the table above) writes each union as one `clippath ... union` record whose closed subpaths all wind one way, so
that non-zero winding is their union. shape-cpu interns each subpath as a member of a clip union (the general tier draws the cells whose members overlap, exactly);
Skia clips to the path (`clipPath`, intersect, anti-aliased) and Blend2D fills the path into a layer whose coverage multiplies the cell's content. The three agree
on the set and differ at its edge by each engine's rounding of a coverage. The first run (2026-11-06, one machine, all three offline in the pinned images) measured
shape-cpu against Skia at 0.27 levels mean and 0.026 % of the pixels past 32, and against Blend2D at 0.50 and 0.263 %: Blend2D's layer is multiplied by a second
layer's coverage, one more rounding at every edge, so this scene's bound for the pixels past 32 is its own (`report.py`, `SCENE_BOUNDS`, 0.4 %), measured and not
the standard one. Drawn as a plain non-zero clip, shape-cpu differs from its own union by 0.03 levels and 0.046 % (the pixels where two members overlap inside a
pixel); that is what the union adds to the exact answer.
