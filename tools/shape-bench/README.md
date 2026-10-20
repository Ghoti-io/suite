# The shape benchmark

How fast the shape family draws, measured against Blend2D and Skia (spine
AD-28, CAP-17 of the shape milestone). The engines measured are `shape-cpu`
(through `shape-scene`'s lists and its sink) and, for text, `font`'s glyph
rendering; the references are Blend2D and Skia's CPU raster backend, each
built from a pinned upstream commit inside a pinned image and used only as a
black box through its public API.

**This file holds no numbers.** The results are in `results/`, one JSON
record per run with the full provenance and a Markdown table generated from
it. A figure quoted anywhere else is read from there, with its date and host.

## What is measured

| Scene | Metric | One frame |
| --- | --- | --- |
| `fill-rect` | fill rate (Mpixel/s over the shapes' area) | 12 translucent rectangles, edges off the pixel grid, 1920x1080 |
| `fill-circle` | fill rate | 12 translucent circles of radius 300 to 500 (cubic arcs) |
| `paths` | path count (paths/s) | 10,000 small paths: triangles, quadratic blobs, cubic circles, stars |
| `text` | text throughput (glyphs/s) | a page of 16-pixel DejaVu Sans 2.37, unhinted, quarter-pixel positions |
| `ui` | list build and submit time per frame | 24 panels of rounded rectangles, icons, bars and rectangular clips, 1280x800 |

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
- Resources are built before timing in every engine: `shape-cpu`'s interned
  paths and glyph runs, Blend2D's `BLPath`s and glyph arrays, Skia's `SkPath`s
  and `SkTextBlob`s.
- A frame is timed from the engine's first call until its output is complete.
  For `shape-cpu` that is recording the list (the build) and rendering it (the
  submit); for Skia on the `ui` scene, recording an `SkPicture` and playing it
  back, and elsewhere drawing directly; Blend2D is immediate and has no build.
- Warm-up of at least 5 frames and 0.25 s, then 31 timed frames: the median,
  the quartiles, the minimum and maximum. Throughput is derived from the median.
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
host, the CPU model, the kernel, the governor and boost, the pinned CPU, and the
box's load and busiest processes before and after each engine ran.

## Running it

On the EVO-X2 only, through `tools/evo`; nothing here is built or run on the
workstation. From the workspace root:

```bash
# The references' images, once (each is a capped, time-limited job of its own):
tools/evo image build localhost/ghoti-shape-bench-blend2d:0.21.3-2 \
    suite/tools/shape-bench/containers/blend2d suite/tools/shape-bench/containers/blend2d/Containerfile -m 8G -t 1h
tools/evo image build localhost/ghoti-shape-bench-skia:m156-2 \
    suite/tools/shape-bench/containers/skia suite/tools/shape-bench/containers/skia/Containerfile -m 24G -t 2h

# The run: the libraries and this directory shipped, built in ghoti-build:gcc16,
# then every engine timed in its own image on the box:
job=$(tools/evo new --ws shape-bench cutil security compress unicode color image math \
      shape-geometry shape-scene shape-cpu font)
tools/evo run "$job" -m 24G -t 2h \
    --host-after 'sh ws/suite/tools/shape-bench/run-evo.sh host' \
    -- 'sh ws/suite/tools/shape-bench/run-evo.sh container'
tools/evo status "$job"          # until it has exited
tools/evo fetch "$job"           # results land in .evo/<job>/
cp .evo/"$job"/results/report/*-evo.* suite/tools/shape-bench/results/
tools/evo clean "$job"
```

The container phase also runs `shape-cpu`'s `make test`, before anything is
timed. Run it on a quiet box: the results record what else was running, and a
loaded box is a reason to run again, not a footnote.
