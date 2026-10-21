#!/bin/bash
# The Windows arm of the shape family's cross-target hash gate (CAP-12, AD-17).
# Host side: needs podman and the host's wine.  Run from anywhere.
#
#   suite/tools/xwin/shape.sh [scratch-dir]        (default /tmp/xwin-shape)
#
# It builds suite/tools/xarch/shape-hash-probe.c natively for the reference
# hash, then cross-compiles it with mingw-w64 (-ffp-contract=off) inside
# ghoti-cross-mingw64:deb13 and runs the .exe under wine, through the same
# container and m1-win.sh as the other scripts here.  Exits 0 only when the
# Windows hash equals the native one AND the same probe built with one planted
# differing bit does not (so the comparison can fail).
#
# Since story 16 it also builds the success scene (libs/font/examples/font-scene.c on the pinned font of
# suite/tools/shape-success, fetched by its fetch.sh), whose whole output must be that directory's expected.txt
# under wine, and with one planted differing bit must not.
#
# Since story 14b it also builds font's provider probe (libs/font/tools/scene-hash.c: AD-5 and AD-1 through
# shape-cpu), which must exit 0 under wine (both equalities hold) and print the hash committed in
# libs/font/tests/data/golden/scene.hash, and with its planted bit must not.
#
# It also runs each library's corpus hash; shape-cpu's prints its coverage hash
# and, since story 13b, one hash per render of its fixture set (the goldens of
# tests/corpus/render.hash) and their fold, which must equal the committed ones.
#
# The math corpus hash (CAP-1): libs/math/tools/corpus-hash.c with
# every src/elementary/*.c, over libs/math/tests/corpus, must print the hash
# committed in libs/math/tests/corpus/trig.hash under wine, and with one
# planted differing bit it must not; built with -DGMATH_SQRT_PORTABLE it must
# print the same hash (story 4a: every variant of the square root is
# bit-identical), and a planted bit in the hardware square root alone must be
# reported as a variant mismatch.  The sources and the corpus are copied
# into the scratch tree, which is all the container sees.
#
# What a green run means: mingw-w64's x86-64 code for this fixture gives the
# bits the Linux build gives.  It does not mean the same on a Windows machine
# (wine runs the same instructions on the same CPU, so it shows the compiler
# and the C runtime agree, not the hardware); and it is x86-64 Windows only,
# not 32-bit or arm64 Windows.  See README.md.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${1:-/tmp/xwin-shape}

mkdir -p "$W/logs"
if [ -n "${M1_IN_IMAGE:-}" ]; then
  # Inside ghoti-xwin-wine (a host without podman runs it that way, with the tree at
  # /work, the scratch tree at /w and suite/tools at /tools): there is no
  # podman and no native compiler, so the native x86-64 hash of the placeholder
  # fixture is given (xarch/shape.sh prints it in section 2).
  want=${XWIN_NATIVE_HASH:?shape.sh: in the image, XWIN_NATIVE_HASH is the native x86_64 hash of shape-hash-probe.c}
else
  command -v podman >/dev/null || { echo "shape.sh: podman is needed on the host" >&2; exit 2; }
  [ -d /usr/lib/wine ] || { echo "shape.sh: the host needs wine in /usr/lib/wine" >&2; exit 2; }
  ref=$(mktemp -d "$W/ref.XXXXXX") || exit 2
  gcc -O2 -std=gnu17 -Wall -Wextra -Werror -ffp-contract=off "$ROOT/suite/tools/xarch/shape-hash-probe.c" -o "$ref/probe" -lm || exit 2
  want=$("$ref/probe" | awk '/^shape.hash/{print $2}')
  rm -rf "$ref"
fi
[ -n "$want" ] || { echo "shape.sh: the native probe printed no hash" >&2; exit 2; }
echo "native x86_64 hash: $want"

# The math sources, corpus and committed hash, copied beside the scratch tree.
MATH="$ROOT/libs/math"
rm -rf "$W/math"
mkdir -p "$W/math/tools" "$W/math/tests" "$W/math/src" "$W/math/gen"
cp -r "$MATH/include" "$W/math/include"
cp -r "$MATH/src/elementary" "$W/math/src/elementary"
cp "$MATH/tools/corpus-hash.c" "$W/math/tools/"
cp "$MATH/tests/corpus_hash.h" "$W/math/tests/"
cp -r "$MATH/tests/corpus" "$W/math/tests/corpus"
make -s -C "$MATH" libver-gen BUILD_DIR="$W/math/gen" > "$W/logs/math-libver.log" 2>&1 || { echo "shape.sh: libver-gen failed (see $W/logs/math-libver.log)" >&2; exit 2; }
math_hash=$(awk '$1=="hash"{print $2}' "$MATH/tests/corpus/trig.hash")
math_count=$(awk '$1=="count"{print $2}' "$MATH/tests/corpus/trig.hash")
[ -n "$math_hash" ] && [ -n "$math_count" ] || { echo "shape.sh: no committed math hash" >&2; exit 2; }
echo "committed math hash: $math_hash over $math_count results"
# The shape-geometry sources, the cutil allocator stand-in and the committed hash.
GEOM="$ROOT/libs/shape-geometry"
rm -rf "$W/geom"
mkdir -p "$W/geom/tools" "$W/geom/gen" "$W/geom/stubs"
cp -r "$GEOM/include" "$W/geom/include"
cp -r "$GEOM/src" "$W/geom/src"
cp "$GEOM/tools/corpus-hash.c" "$W/geom/tools/"
cp -r "$HERE/../xarch/shape-stubs/." "$W/geom/stubs/"
make -s -C "$GEOM" libver-gen BUILD_DIR="$W/geom/gen" > "$W/logs/geom-libver.log" 2>&1 || { echo "shape.sh: libver-gen failed (see $W/logs/geom-libver.log)" >&2; exit 2; }
geom_hash=$(awk '$1=="hash"{print $2}' "$GEOM/tests/corpus/geometry.hash")
geom_count=$(awk '$1=="count"{print $2}' "$GEOM/tests/corpus/geometry.hash")
[ -n "$geom_hash" ] && [ -n "$geom_count" ] || { echo "shape.sh: no committed shape-geometry hash" >&2; exit 2; }
echo "committed shape-geometry hash: $geom_hash over $geom_count results"
# The shape-cpu sources, the fixture set its probe renders and the committed hashes (story 9, and story 13b's
# renders); it is built with shape-scene's, shape-geometry's and math's, and cutil's, color's and image's parts.
CPU="$ROOT/libs/shape-cpu"
rm -rf "$W/cpu"
mkdir -p "$W/cpu/tools" "$W/cpu/gen" "$W/cpu/tests"
cp -r "$CPU/include" "$W/cpu/include"
cp -r "$CPU/src" "$W/cpu/src"
cp "$CPU/tools/corpus-hash.c" "$W/cpu/tools/"
cp "$CPU/tests/render_fixtures.h" "$CPU/tests/corpus/render.hash" "$W/cpu/tests/"
# The fixtures' runs from a font use shape-scene's test glyph provider, at the path shape-scene installs it (story 14a).
rm -rf "$W/scene-testing"
mkdir -p "$W/scene-testing/ghoti.io/shape-scene/testing"
cp "$ROOT/libs/shape-scene/tests/glyph_test_provider.h" "$W/scene-testing/ghoti.io/shape-scene/testing/"
make -s -C "$CPU" libver-gen BUILD_DIR="$W/cpu/gen" > "$W/logs/cpu-libver.log" 2>&1 || { echo "shape.sh: libver-gen failed (see $W/logs/cpu-libver.log)" >&2; exit 2; }
cpu_hash=$(awk '$1=="hash"{print $2}' "$CPU/tests/corpus/coverage.hash")
cpu_count=$(awk '$1=="count"{print $2}' "$CPU/tests/corpus/coverage.hash")
[ -n "$cpu_hash" ] && [ -n "$cpu_count" ] || { echo "shape.sh: no committed shape-cpu hash" >&2; exit 2; }
echo "committed shape-cpu hash: $cpu_hash over $cpu_count results"
render_hash=$(awk '$1=="hash"{print $2}' "$CPU/tests/corpus/render.hash")
render_count=$(awk '$1=="count"{print $2}' "$CPU/tests/corpus/render.hash")
[ -n "$render_hash" ] && [ -n "$render_count" ] || { echo "shape.sh: no committed shape-cpu renders" >&2; exit 2; }
echo "committed shape-cpu renders: $render_hash over $render_count"
# The shape-scene sources and the committed hash (story 10); it is built with shape-geometry's and math's too.
SCENE="$ROOT/libs/shape-scene"
# cutil's real headers, its generated libver_gen.h, allocator.c and atomic.c: the scene's mutex and counts are cutil's.
CUTIL="$ROOT/libs/cutil"
rm -rf "$W/cutil"
mkdir -p "$W/cutil/src" "$W/cutil/gen"
cp -r "$CUTIL/include" "$W/cutil/include"
cp "$CUTIL/src/allocator.c" "$CUTIL/src/atomic.c" "$W/cutil/src/"
make -s -C "$CUTIL" BUILD_DIR="$W/cutil/gen" "$W/cutil/gen/include/ghoti.io/cutil/libver_gen.h" > "$W/logs/cutil-libver.log" 2>&1 || { echo "shape.sh: cutil libver_gen.h failed (see $W/logs/cutil-libver.log)" >&2; exit 2; }
# color's headers, its generated libver_gen.h and gamut.c: the scene names its types and calls gcol_gamut_identify.
COLOR="$ROOT/libs/color"
rm -rf "$W/color"
mkdir -p "$W/color/src" "$W/color/gen"
cp -r "$COLOR/include" "$W/color/include"
cp "$COLOR/src/gamut/gamut.c" "$W/color/src/"
make -s -C "$COLOR" SKIP_DEP_CHECK=1 BUILD_DIR="$W/color/gen" "$W/color/gen/generated/ghoti.io/color/libver_gen.h" > "$W/logs/color-libver.log" 2>&1 || { echo "shape.sh: color libver-gen failed (see $W/logs/color-libver.log)" >&2; exit 2; }
# image's headers, its generated libver_gen.h, raster.c and its allocator.c: a target names a raster and the test sink
# stamps one (story 13a).
IMAGE="$ROOT/libs/image"
rm -rf "$W/image"
mkdir -p "$W/image/src" "$W/image/gen"
cp -r "$IMAGE/include" "$W/image/include"
cp "$IMAGE/src/raster/raster.c" "$IMAGE/src/raster/raster_internal.h" "$W/image/src/"
mkdir -p "$W/image/core"
cp "$IMAGE/src/core/"*.h "$W/image/core/"
cp "$IMAGE/src/core/allocator.c" "$W/image/core/"
make -s -C "$IMAGE" SKIP_DEP_CHECK=1 BUILD_DIR="$W/image/gen" "$W/image/gen/generated/ghoti.io/image/libver_gen.h" > "$W/logs/image-libver.log" 2>&1 || { echo "shape.sh: image libver-gen failed (see $W/logs/image-libver.log)" >&2; exit 2; }
rm -rf "$W/scene"
mkdir -p "$W/scene/tools" "$W/scene/gen" "$W/scene/tests"
cp -r "$SCENE/include" "$W/scene/include"
cp -r "$SCENE/src" "$W/scene/src"
cp "$SCENE/tools/corpus-hash.c" "$W/scene/tools/"
cp "$SCENE/tests/glyph_test_provider.h" "$W/scene/tests/"
make -s -C "$SCENE" libver-gen BUILD_DIR="$W/scene/gen" > "$W/logs/scene-libver.log" 2>&1 || { echo "shape.sh: libver-gen failed (see $W/logs/scene-libver.log)" >&2; exit 2; }
scene_hash=$(awk '$1=="hash"{print $2}' "$SCENE/tests/corpus/scene.hash")
scene_count=$(awk '$1=="count"{print $2}' "$SCENE/tests/corpus/scene.hash")
[ -n "$scene_hash" ] && [ -n "$scene_count" ] || { echo "shape.sh: no committed shape-scene hash" >&2; exit 2; }
echo "committed shape-scene hash: $scene_hash over $scene_count results"
# The font provider's probe (story 14b) also takes cutil's file.c: a blob's destroy frees through it.
cp "$CUTIL/src/file.c" "$CUTIL/src/file_internal.h" "$CUTIL/src/path_internal.h" "$W/cutil/src/"
# The font provider's probe (story 14b): font's sources, its tools/scene-hash.c, the fixtures it renders and the
# committed hash; the headers and generated headers of the dependencies it names and never reaches (compress,
# security, unicode), whose symbols the link drops or stubs.
FONT="$ROOT/libs/font"
rm -rf "$W/font"
mkdir -p "$W/font/tools" "$W/font/fonts" "$W/font/gen"
cp -r "$FONT/include" "$W/font/include"
cp -r "$FONT/src" "$W/font/src"
cp "$FONT/tools/scene-hash.c" "$W/font/tools/"
for f in $(sed -n 's/^fixture \([^ ]*\) .*/\1/p' "$FONT/tests/data/golden/scene.hash"); do cp "$FONT/tests/data/fonts/$f" "$W/font/fonts/"; done
for lib in font compress security unicode; do
  [ "$lib" = font ] || { rm -rf "$W/$lib"; mkdir -p "$W/$lib"; cp -r "$ROOT/libs/$lib/include" "$W/$lib/include"; }
  make -s -C "$ROOT/libs/$lib" SKIP_DEP_CHECK=1 BUILD_DIR="$W/$lib/gen" "$W/$lib/gen/generated/ghoti.io/$lib/libver_gen.h" > "$W/logs/$lib-libver.log" 2>&1 || { echo "shape.sh: $lib libver_gen.h failed (see $W/logs/$lib-libver.log)" >&2; exit 2; }
done
# The success scene (story 16): font's example program, the pinned font and the committed output.
SUCCESS="$ROOT/suite/tools/shape-success"
SUCCESS_FONT=${SUCCESS_FONT:-$SUCCESS/data/DejaVuSans.ttf}
printf '%s  %s\n' 7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954 "$SUCCESS_FONT" | sha256sum -c - > /dev/null 2>&1 \
  || { echo "shape.sh: no pinned font at $SUCCESS_FONT (run suite/tools/shape-success/fetch.sh)" >&2; exit 2; }
mkdir -p "$W/font/examples"
rm -rf "$W/success"
mkdir -p "$W/success"
cp "$FONT/examples/font-scene.c" "$W/font/examples/"
cp "$SUCCESS_FONT" "$W/success/DejaVuSans.ttf"
cp "$SUCCESS/expected.txt" "$W/success/"
font_hash=$(awk '$1=="hash"{print $2}' "$FONT/tests/data/golden/scene.hash")
font_count=$(awk '$1=="count"{print $2}' "$FONT/tests/data/golden/scene.hash")
[ -n "$font_hash" ] && [ -n "$font_count" ] || { echo "shape.sh: no committed font provider hash" >&2; exit 2; }
echo "committed font provider hash: $font_hash over $font_count renders"
"$HERE/m1-win.sh" "$W" "bash /tools/xwin/shape-win.sh $want $math_hash $math_count $geom_hash $geom_count $cpu_hash $cpu_count $scene_hash $scene_count $render_hash $render_count $font_hash $font_count"
