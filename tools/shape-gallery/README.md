# Shape gallery

Seven PNGs drawn entirely by the suite's own libraries, for a person to look at,
and a way to see what changed in them since the last time. Every pixel comes
from `shape-cpu` rendering `shape-scene` lists; text is `font`'s paragraph
layout and shaping, its glyphs reaching the scene through `font`'s glyph path
provider; the PNG encoding is `image`'s own writer. It lives in the suite, beside
`shape-success` and `shape-bench`, because it is a consumer of the whole stack:
it includes and links `color`, `math`, `image`, the three shape libraries and
`font` together, and a library's line in `suite/libraries.txt` lists every
library whose header it includes or links, so hosting it in any one library
would add edges to that library.

| Image | What it shows |
| --- | --- |
| `shapes.png` | fill rules (nonzero, even-odd), cubic and quadratic curves, a path with a hole, a clip, group opacity against per-fill alpha |
| `compositing.png` | every Porter-Duff operator and PLUS, each a group around the source over a backdrop, on a checkerboard |
| `blend-modes.png` | the fifteen blend modes (W3C's, numbered as COLRv1 numbers them), each a group around red, green, blue and grey patches at full and half alpha over a hue sweep under a lightness ramp, blended in sRGB-encoded values and in linear light; a row of sources out of range; the same multiply at the root and in an isolated group of its own; PDF's non-isolated and knockout groups as labelled placeholders, which shape-cpu refuses; each tile's middle pixel read back and printed |
| `color.png` | the same translucent fills in an sRGB-encoded and a linear-light group, and solid paints in sRGB, Display P3 and BT.2020 converted to the sRGB target, with the 8-bit values read back and printed |
| `gradients.png` | linear gradients with pad, repeat and reflect; radial (concentric, a focal point, and the cone when neither circle holds the other); sweep over a whole turn and over part of one with pad and repeat, showing its direction; the same stops interpolated in sRGB and linear light, and straight and premultiplied with a translucent stop, the middle pixels read back; and gradients under an item's map and under their own map |
| `text.png` | DejaVu Sans 2.37 at eight sizes, a justified paragraph, a 6x zoom of hinted, unhinted and general-route glyphs, nine scripts, and `font`'s committed test fixtures |
| `color-glyphs.png` | the COLR glyphs of `font`'s fixtures lowered into scene lists, with what is refused and why |

`gallery.log` beside them has each render's result and its report entries
tallied by class and reason, diagnostics of a few fixture fonts, and the
refused color glyphs.

## Running it

`render.sh` is the whole build-and-render step. It wants a directory laid out
as `suite/clone.sh` lays one out (`libs/<lib>` beside `suite/`), an empty
install prefix and an output directory:

```bash
sh suite/tools/shape-gallery/render.sh WS PREFIX OUT JOBS LIB...
```

`LIB...` is every library the gallery needs, in `suite/libraries.txt` order: the
closure of `font`'s and `shape-cpu`'s dependencies (cutil, security, unicode,
compress, color, image, math, shape-geometry, shape-scene, shape-cpu and font,
today). It installs each into `PREFIX` (`PREFIX=` on
every make, so the rpath is set), builds `gallery.c` against them, fetches the
pinned font by digest (`suite/tools/shape-success/fetch.sh`), renders the seven
PNGs into `OUT` with `gallery.log`, and decode-checks every PNG. Build logs go to
`OUT/logs/`. The result is the suite's only when it is built by the suite's
compiler, so run it inside `ghoti-build:gcc16` (`suite/Containerfile.build`),
for example:

```bash
podman run --rm --userns=keep-id -v "$WS:$WS:z" -v "$OUT:$OUT:z" -w "$WS" \
    localhost/ghoti-build:gcc16 \
    sh suite/tools/shape-gallery/render.sh "$WS" "$WS/.gallery-prefix" "$OUT" 4 $LIBS
```

Build it from committed trees (a clone of each library at the commit you want)
rather than from working trees with edits in them, so that the commits a run
names are what drew it. `render.sh --host-info ENGINE OUT` writes the image id
and the host's name into `OUT/host.txt`, for a record of where it ran.

A run directory holds:

- the seven PNGs and `gallery.log`;
- `decode-check.txt`: every PNG's chunks, CRCs and image data checked by an
  independent reader;
- `build.txt` (compiler, flags, the font's digest) and `gallery.exit`;
- `logs/`: the install and build logs.

A caller that keeps runs side by side can add `manifest.txt` with a
`lib: <name> <commit>` line per library; `compare.py` reads those lines to say
which libraries changed between two runs.

## Seeing what changed

```bash
python3 -I suite/tools/shape-gallery/compare.py            # the two most recent runs in $SHAPE_GALLERY_RUNS (default runs/ here)
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
features land: strokes, and the color glyphs' gradients once `font` lowers
them, will fill in parts that are blank today, and the captions that say what
is not drawn yet will change with them. A difference is a reason to look at the diff image, not a failure; an
unexpected one, between runs whose changed commits should not touch rendering,
is a finding. Equally, identical pictures say nothing about what the gallery
does not draw.
