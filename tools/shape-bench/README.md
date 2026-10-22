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
  default), which is what a retained scene sees; there is no speed threshold, only the cross-check below.
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
