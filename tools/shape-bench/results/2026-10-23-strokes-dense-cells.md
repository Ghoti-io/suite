# Dense cells in the exact union step: the census (strokes story S4c, commit 0)

What the exact union step (shape-cpu `src/render/exact.c`) was given by every drawing the suite keeps, measured before the step
was changed. The step then was the one strokes story S4b made (shape-cpu `0062396`, whose step is S4b's `1c8f98f` unchanged),
so the times are S4b's. Every figure is measured by `libs/shape-cpu/tools/exact-census.c`, a preloaded object that stands
between the scan converter and the step and measures each cell by itself from the pieces it is given: the raw pieces of a
part, the fragments left once a piece and its reverse cancel (equal copies counted once), the slabs, and the crossings `K`
(the pairs of fragments that meet strictly inside a slab, counted as the inversions between the order of the fragments at the
top of each slab and at its bottom). The largest winding is the step's own (`EXACT_WINDING_MAX` raised to 32767 in the measuring
build so that the step follows the deeper windings of text instead of refusing them), and the time is the step's own.

## Provenance

| | |
| --- | --- |
| Tree | shape-cpu `0062396` (clean), suite working tree on `790a4ce` with the drivers of this commit, font `678c91e`, the other libraries at their current heads |
| Compiler | GCC 16.2.0, `ghoti-build:gcc16`, `-O2` (the libraries' own flags), shape-cpu built with `-fvisibility=default -DEXACT_WINDING_MAX=32767` |
| Host | a 32-thread x86-64 build host, the job capped at 24 GB; one process, one thread (times are the step's alone, per call) |
| Instrument | `exact-census.c` under `LD_PRELOAD`; `census.sh` builds and runs it (`tools/shape-bench`) |
| Drawings | `strokes` and `strokes-dashed`: one frame of the benchmark's scenes (`bench-shape frame`); `gallery`: `gallery.c` as committed (all eight pages; the sink is given `max_exact_fragments` 65536 there); `corpus`: shape-cpu's `corpus-hash` probe (the committed fixtures on every format); `font-fixtures`: font's `scene-hash` probe (16 ppem and the other sizes of its stroke pair, raised limit); `text-W`: `stroke-text` drawing DejaVu Sans 2.37 "Union", "Gradient" and "Outline" at 84 px, round joins and caps, width W, with the cells, points and fragments raised to the point that nothing is refused |

## Cells per drawing

| drawing | exact cells | raw pieces of a part: max / 99.9 % / 99 % | distinct fragments max | crossings `K`: max / 99.9 % | largest winding | cells over 512 raw pieces | S4b's step, total |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `strokes` | 3,693,250 | 4294 / 61 / 42 | 695 | 34,222 / 7 | 249 | 35 | 2.93 s |
| `strokes-dashed` | 1,043,032 | 118 / 72 / 54 | 35 | 17 / 7 | 7 | 0 | 0.89 s |
| gallery pages | 93,756 | 2359 / 210 / 95 | 500 | 1686 / 48 | 149 | 36 | 0.19 s |
| committed fixtures (`corpus`) | 39,684 | 359 / 292 / 164 | 161 | 534 / 177 | 10 | 0 | 0.15 s |
| font's fixtures | 43,624 | 3783 / 2063 / 609 | 925 | 52,284 / 13,430 | 173 | 564 | 5.89 s |
| text, width 14 | 31,958 | 4597 / 2039 / 385 | 776 | 49,761 / 862 | 318 | 263 | 1.86 s |
| text, width 16 | 34,596 | 6078 / 2990 / 794 | 927 | 45,213 / 2752 | 396 | 743 | 3.43 s |
| text, width 18 | 37,138 | 11,620 / 3887 / 1172 | 1331 | 197,093 / 5746 | 431 | 1315 | 10.89 s |
| text, width 20 | 40,297 | 12,397 / 5067 / 1466 | 1069 | 291,031 / 9099 | 455 | 1875 | 20.00 s |
| text, width 22 | 43,766 | 13,363 / 5852 / 2078 | 1084 | 333,496 / 17,179 | 494 | 3704 | 30.05 s |

The raw per-drawing reports (the ten densest cells by pieces and by crossings, histograms, the cells of each size) are the
`census-*.txt` files beside this one.

### What these figures say about S4c's estimates and defaults

- The largest raw cell measured anywhere is **13,363** pieces (the three words at width 22), not the 7915 of S5's "Union" alone
  (Gradient and Outline are denser). Twice that is 26,726, so a default of 16,384 is a headroom of **1.23**, not the 2.0 the
  story asks for; the smallest power of two with 2x is 32,768.
- The largest `K` measured is **333,496** (width 22), and the 4294-piece cell of `strokes` has **34,222**, not the "about 6000"
  of the story's estimate: the estimate was low by a factor of 5.7. `2^20` crossings is 3.1x the largest measured.
- The largest winding is **494** (width 22, the three words), 475 for "Union" alone: twice 494 is 988, so 1024 gives the 2x. Of
  the benchmark scenes the deepest is 249 in `strokes`, not the 19 of S4b's table.
- S4b's step on the densest `strokes` cell (4294 pieces, 600 distinct, 29 slabs, 34,222 crossings) takes 170 ms.

## Where the instructions go (callgrind, one warm frame)

`attribute.sh` runs `bench-shape frame` twice under `valgrind --tool=callgrind --instr-atstart=no` and counts the second frame.
The sink's cache holds 16 MiB of expansions and both scenes' expansions are larger, so the frame re-makes some of them
(the expansion share below); the figures are of the frame as the benchmark draws it. Inclusive shares of the frame:

| | `strokes-dashed` | `strokes` |
| --- | --- | --- |
| the frame | 30.1 G instructions | 90.6 G |
| expansion (`gshcpu_render_stroke_open`) | 35.4 % | 20.6 % |
| the cover (`gshcpu_render_cover_exact`) | 60.4 % | 73.8 % |
| &nbsp;&nbsp;the dense pass over the edges (`path_edges` less the second pass) | about 11 % | about 11 % |
| &nbsp;&nbsp;the second pass over the edges and the windows (`exact_band` less the step) | 15.1 % | 18.6 % |
| &nbsp;&nbsp;the exact step (`gshcpu_exact_cell`) | 28.6 % | 34.4 % |
| paint and composition (`paint_coverage` and the rest) | the remainder | the remainder |

So on the ordinary scene the second pass is 34.5 % of the exact step's cost (`exact_band` 43.7 % of the frame, of which the
step is 28.6 %), above the quarter the story names as the threshold for lever L1.

## Frame times (`frames.sh`, `ghoti-build:gcc16`)

One thread, 1920x1080, the median of 31 timed frames after the warm-up, pinned to one core of a 32-thread x86-64 build host
Both steps ran both steps in one run on the same host. The raw records are `2026-10-23-dense-cells/frames.txt`.

| Scene | the exact step of this story | the step of S4b (preloaded) |
|---|---|---|
| `strokes` | 4225 ms | 6192 ms |
| `strokes-dashed` | 1686 ms | 2158 ms |

S4b's own figures (5990 and 2197 ms) and S4's (2050 and 1047 ms) were taken on other runs. The step is 1.47x and 1.28x faster than
S4b's on the same run; it is not back at S4's frame time, which had no exact union step at all.
