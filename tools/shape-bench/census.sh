#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# A census of the cells shape-cpu's exact union step is made to handle, over the drawings the suite keeps: the benchmark's
# stroke scenes, the gallery, shape-cpu's committed fixtures, font's fixtures at small sizes and stroked lines of real text.
#
#   census.sh WS PREFIX OUT JOBS
#
# WS holds libs/<lib> and suite/ (laid out as suite/clone.sh lays them out), PREFIX is an empty install prefix, OUT the
# results directory; run it inside ghoti-build:gcc16.  It installs the libraries in suite/libraries.txt order, shape-cpu from
# a copy of the tree built with its symbols visible (libs/shape-cpu/tools/exact-census.c explains why), builds the drivers,
# and runs each of them once under the census, which writes OUT/census-<drawing>.txt.  Pass CENSUS_WINDING=N to name the
# winding the step of S4b may follow (EXACT_WINDING_MAX, 32767 at most; a later step ignores it).  Every drawing is drawn at
# limits high enough to draw it, so that the figures are of what the step was given and not of what it refused.

set -eu

[ $# -eq 4 ] || { echo "usage: census.sh WS PREFIX OUT JOBS" >&2; exit 2; }
WS=$1 PREFIX=$2 OUT=$3 JOBS=$4
WINDING=${CENSUS_WINDING:-32767}
T=$WS/suite/tools/shape-bench
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
mkdir -p "$PREFIX" "$OUT/logs" "$OUT/frames"
WORK=${TMPDIR:-/tmp}/census-$$
mkdir -p "$WORK"

install_lib() {
  echo "== install $1"
  make -C "$2" -j"$JOBS" install PREFIX="$PREFIX" ${3:+"$3"} > "$OUT/logs/install-$1.log" 2>&1 ||
    { tail -n 40 "$OUT/logs/install-$1.log"; echo "census.sh: installing $1 failed" >&2; exit 1; }
}

for l in cutil security compress unicode math color image shape-geometry shape-scene; do
  install_lib "$l" "$WS/libs/$l"
done

# shape-cpu with its symbols visible, so that the preloaded step is the one the scan converter calls.
mkdir -p "$WORK/shape-cpu"
(cd "$WS/libs/shape-cpu" && tar cf - --exclude=./build --exclude=./.git .) | (cd "$WORK/shape-cpu" && tar xf -)
install_lib shape-cpu "$WORK/shape-cpu" "EXTRA_CFLAGS=-fvisibility=default -DEXACT_WINDING_MAX=$WINDING"
install_lib font "$WS/libs/font"

PCS="ghoti.io-shape-cpu-0"
# shellcheck disable=SC2046 # pkg-config's output is a word list
cc -std=c17 -O2 -fPIC -shared -Wall -Wextra -I "$WS/libs/shape-cpu/src/render" $(pkg-config --cflags $PCS) \
  -o "$OUT/exact-census.so" "$WS/libs/shape-cpu/tools/exact-census.c" -ldl -lpthread > "$OUT/logs/census-build.log" 2>&1 ||
  { cat "$OUT/logs/census-build.log"; echo "census.sh: building the census failed" >&2; exit 1; }

echo "== the drivers"
make -C "$T" -j"$JOBS" PREFIX="$PREFIX" BUILD_DIR="$WORK/bench-build" > "$OUT/logs/bench-build.log" 2>&1 ||
  { tail -n 40 "$OUT/logs/bench-build.log"; echo "census.sh: building the bench failed" >&2; exit 1; }
FETCH=$WS/suite/tools/shape-success
sh "$FETCH/fetch.sh" "$FETCH/data"
DEJAVU=$FETCH/data/DejaVuSans.ttf
PC="ghoti.io-font-0 ghoti.io-shape-cpu-0 ghoti.io-shape-scene-0 ghoti.io-shape-geometry-0 ghoti.io-image-0
    ghoti.io-color-0 ghoti.io-math-0 ghoti.io-cutil-0"
RPATH=
# shellcheck disable=SC2086 # PC is a word list
for d in $(pkg-config --libs-only-L $PC); do
  RPATH="$RPATH -Wl,-rpath,${d#-L}"
done
# shellcheck disable=SC2086,SC2046 # pkg-config's output and RPATH are word lists
cc -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra "$WS/suite/tools/shape-gallery/gallery.c" $(pkg-config --cflags --libs $PC) \
  $RPATH -lm -o "$WORK/gallery" > "$OUT/logs/gallery-build.log" 2>&1 ||
  { cat "$OUT/logs/gallery-build.log"; echo "census.sh: building the gallery failed" >&2; exit 1; }
# shellcheck disable=SC2086,SC2046
cc -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra $(pkg-config --cflags ghoti.io-shape-cpu-0 ghoti.io-shape-geometry-0) \
  -o "$WORK/corpus-hash" "$WS/libs/shape-cpu/tools/corpus-hash.c" $(pkg-config --libs ghoti.io-shape-cpu-0) $RPATH -lm \
  > "$OUT/logs/corpus-build.log" 2>&1 || { cat "$OUT/logs/corpus-build.log"; echo "census.sh: building corpus-hash failed" >&2; exit 1; }
# shellcheck disable=SC2086,SC2046
cc -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra $(pkg-config --cflags $PC) -o "$WORK/scene-hash" \
  "$WS/libs/font/tools/scene-hash.c" $(pkg-config --libs $PC) $RPATH -lm > "$OUT/logs/scene-hash-build.log" 2>&1 ||
  { cat "$OUT/logs/scene-hash-build.log"; echo "census.sh: building scene-hash failed" >&2; exit 1; }

census() {
  name=$1
  shift
  echo "== census: $name"
  rm -f "$OUT/census-$name.txt"
  rc=0
  GSHCPU_CENSUS_OUT="$OUT/census-$name.txt" LD_PRELOAD="$OUT/exact-census.so" "$@" > "$OUT/logs/run-$name.log" 2>&1 || rc=$?
  tail -n 3 "$OUT/logs/run-$name.log"
  [ "$rc" -eq 0 ] || echo "census.sh: $name exited $rc" >&2
  [ -f "$OUT/census-$name.txt" ] || { echo "census.sh: $name left no census" >&2; exit 1; }
}

mkdir -p "$WORK/scenes"
"$WORK/bench-build/bench-shape" emit "$WORK/scenes" "$DEJAVU" > "$OUT/logs/emit.log" 2>&1 ||
  { tail "$OUT/logs/emit.log"; echo "census.sh: bench-shape emit failed" >&2; exit 1; }
GSHCPU_CENSUS_DUMP="$OUT/cells-strokes.bin"
GSHCPU_CENSUS_EVERY=50
export GSHCPU_CENSUS_DUMP GSHCPU_CENSUS_EVERY
census strokes "$WORK/bench-build/bench-shape" frame "$WORK/scenes" "$OUT/frames" strokes
unset GSHCPU_CENSUS_DUMP GSHCPU_CENSUS_EVERY
GSHCPU_CENSUS_DUMP="$OUT/cells-strokes-dashed.bin"
GSHCPU_CENSUS_EVERY=20
export GSHCPU_CENSUS_DUMP GSHCPU_CENSUS_EVERY
census strokes-dashed "$WORK/bench-build/bench-shape" frame "$WORK/scenes" "$OUT/frames" strokes-dashed
unset GSHCPU_CENSUS_DUMP GSHCPU_CENSUS_EVERY
mkdir -p "$OUT/gallery"
census gallery "$WORK/gallery" "$WS/libs/font/tests/data/fonts" "$DEJAVU" "$OUT/gallery"
census corpus "$WORK/corpus-hash"
census font-fixtures "$WORK/scene-hash" "$WS/libs/font/tests/data/fonts"
for w in 14 16 18 20 22; do
  census "text-$w" "$WORK/bench-build/stroke-text" "$DEJAVU" --cells 67108864 --points 16777216 --fragments 65536 \
    Union $w Gradient $w Outline $w
done
# The same lines of text at the sink's default limits, none of them raised: each must draw.
echo "== text at the default limits"
rc=0
"$WORK/bench-build/stroke-text" "$DEJAVU" > "$OUT/text-defaults.txt" 2>&1 || rc=$?
cat "$OUT/text-defaults.txt"
[ "$rc" -eq 0 ] || echo "census.sh: a line of text was refused at the default limits" >&2
rm -rf "$WORK"
echo "census.sh: done"
