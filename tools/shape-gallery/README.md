# Shape gallery

Five PNGs drawn entirely by the suite's own libraries, for a person to look at,
and a way to see what changed in them since the last time. Every pixel comes
from `shape-cpu` rendering `shape-scene` lists; text is `font`'s paragraph
layout and shaping, its glyphs reaching the scene through `font`'s glyph path
provider; the PNG encoding is `image`'s own writer. It lives in the suite, beside
`shape-success` and `shape-bench`, because it is a consumer of the whole stack:
it includes and links `color`, `math`, `image`, the three shape libraries and
`font` together, and under the shape spine's AD-2 a library's manifest line
lists every library whose header it includes or links, so hosting it in any one
library would add edges to that library.

| Image | What it shows |
| --- | --- |
| `shapes.png` | fill rules (nonzero, even-odd), cubic and quadratic curves, a path with a hole, a clip, group opacity against per-fill alpha |
| `compositing.png` | every Porter-Duff operator and PLUS, each a group around the source over a backdrop, on a checkerboard |
| `color.png` | the same translucent fills in an sRGB-encoded and a linear-light group, and solid paints in sRGB, Display P3 and BT.2020 converted to the sRGB target, with the 8-bit values read back and printed |
| `text.png` | DejaVu Sans 2.37 at eight sizes, a justified paragraph, a 6x zoom of hinted, unhinted and general-route glyphs, nine scripts, and `font`'s committed test fixtures |
| `color-glyphs.png` | the COLR glyphs of `font`'s fixtures lowered into scene lists, with what is refused and why |

`gallery.log` beside them has each render's result and its report entries
tallied by class and reason, diagnostics of a few fixture fonts, and the
refused color glyphs.

## Running it

From the workspace root. Both build from **committed trees only**: the `HEAD`
of suite and of every library the gallery needs (the closure of `font`'s
dependencies in `suite/libraries.txt`, installed in that file's order), cloned
into a scratch stage under `runs/`. An uncommitted edit in any checkout never
reaches the pictures; the script says when it left one out. Commit a change to
`gallery.c` before running it.

On the EVO-X2, the usual way:

```bash
suite/tools/shape-gallery/run-evo.sh
```

It ships the stage as one `tools/evo` job, builds and renders in
`ghoti-build:gcc16` (8 GB cap, one hour limit), records the box's image id,
waits for the job, fetches it and cleans it off the box. A few minutes.

When the EVO-X2 cannot be reached:

```bash
suite/tools/shape-gallery/run-local.sh
```

The same in the same image under podman here. This workstation has 15 GB
shared with Claude Desktop and several sessions and has been taken down by
heavy runs, so the container is held to 3 GB and two make jobs, the whole run
is niced under `tools/memcap -m 3G`, and it refuses to start below 6000 MB
available (`SHAPE_GALLERY_MIN_FREE_MB`). It is slower.

Each run lands in `runs/<UTC time>_<shape-geometry>-<shape-scene>-<shape-cpu>-<font>/`
(the short commits), which the script prints last:

- the five PNGs and `gallery.log`;
- `manifest.txt`: the date, where it ran, host, image and image id, compiler,
  the pinned font's digest, suite's commit, and every library's full commit;
- `decode-check.txt`: every PNG's chunks, CRCs and image data checked by an
  independent reader;
- `logs/`: the install and build logs, and the job's log on the EVO-X2.

`runs/` is gitignored. The first run in it, `20261021T134110Z_ce39283-…`, is
the one-off probe that preceded this tool, recorded afterwards; its manifest
says what was and was not known about it.

## Seeing what changed

```bash
python3 -I suite/tools/shape-gallery/compare.py            # the two most recent runs
python3 -I suite/tools/shape-gallery/compare.py OLD NEW    # any two run directories
```

For each image it says identical, or changed with the number of changed pixels,
the largest difference in any channel and the region they fall in, or that the
size changed. It writes `NEW/diff-vs-<OLD>/`: per changed image a diff PNG (the
new image dimmed to grey, changed pixels in magenta, a cyan frame around them),
`gallery.log.diff` when the logs differ, and `summary.txt`. It also lists the
libraries whose commit differs between the two manifests, which is where to
look for the cause. Standard library only; its PNG reader is its own and checks
the gallery's PNGs, it does not draw them.

**This is for seeing changes, not a gate.** Pixels change legitimately as
features land: gradients (story P2) and strokes will fill in parts that are
blank today, and the captions that say what is not drawn yet will change with
them. A difference is a reason to look at the diff image, not a failure; an
unexpected one, between runs whose changed commits should not touch rendering,
is a finding. Equally, identical pictures say nothing about what the gallery
does not draw.
