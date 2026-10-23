#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Where the instructions of a stroke frame go, by function: one frame of a benchmark scene under callgrind, annotated with the
# inclusive cost of each function, so that the share of the exact union step in a frame and what that share is made of (the
# second pass over the edges that keeps the pieces of the cells, the sort, the walk of the slabs) are figures and not guesses.
#
#   attribute.sh WS PREFIX OUT JOBS [SCENE...]
#
# WS holds libs/<lib> and suite/ (laid out as suite/clone.sh lays them out), PREFIX is an empty install prefix, OUT the results
# directory; run it inside ghoti-build:gcc16, which carries valgrind.  The scenes default to strokes-dashed and strokes.  For
# each scene it writes OUT/callgrind-<scene>.out and OUT/attribution-<scene>.txt (callgrind_annotate --inclusive=yes, the
# hundred costliest functions).  The libraries are the release build with its symbols; callgrind counts instructions, so
# the figures do not depend on the machine's load, only on the compiler and the tree.  Two frames are drawn and the second,
# the warm one (the sink has cached the expansions), is what is counted.

set -eu

[ $# -ge 4 ] || { echo "usage: attribute.sh WS PREFIX OUT JOBS [SCENE...]" >&2; exit 2; }
WS=$1 PREFIX=$2 OUT=$3 JOBS=$4
shift 4
SCENES=${*:-strokes-dashed strokes}
T=$WS/suite/tools/shape-bench
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
mkdir -p "$PREFIX" "$OUT/logs" "$OUT/frames"
WORK=${TMPDIR:-/tmp}/attribute-$$
mkdir -p "$WORK/scenes"

for l in cutil security compress unicode math color image shape-geometry shape-scene shape-cpu font; do
  echo "== install $l"
  make -C "$WS/libs/$l" -j"$JOBS" install PREFIX="$PREFIX" > "$OUT/logs/install-$l.log" 2>&1 ||
    { tail -n 40 "$OUT/logs/install-$l.log"; echo "attribute.sh: installing $l failed" >&2; exit 1; }
done
make -C "$T" -j"$JOBS" PREFIX="$PREFIX" BUILD_DIR="$WORK/bench-build" > "$OUT/logs/bench-build.log" 2>&1 ||
  { tail -n 40 "$OUT/logs/bench-build.log"; echo "attribute.sh: building the bench failed" >&2; exit 1; }
FETCH=$WS/suite/tools/shape-success
sh "$FETCH/fetch.sh" "$FETCH/data"
"$WORK/bench-build/bench-shape" emit "$WORK/scenes" "$FETCH/data/DejaVuSans.ttf" > "$OUT/logs/emit.log" 2>&1 ||
  { tail "$OUT/logs/emit.log"; echo "attribute.sh: bench-shape emit failed" >&2; exit 1; }
for s in $SCENES; do
  echo "== callgrind: $s"
  SHAPE_BENCH_FRAMES=2 valgrind --tool=callgrind --instr-atstart=no --callgrind-out-file="$OUT/callgrind-$s.out" \
    "$WORK/bench-build/bench-shape" frame "$WORK/scenes" "$OUT/frames" "$s" > "$OUT/logs/callgrind-$s.log" 2>&1 ||
    { tail "$OUT/logs/callgrind-$s.log"; echo "attribute.sh: callgrind failed on $s" >&2; exit 1; }
  callgrind_annotate --inclusive=yes "$OUT/callgrind-$s.out" 2> /dev/null | head -n 140 > "$OUT/attribution-$s.txt"
  callgrind_annotate --inclusive=no "$OUT/callgrind-$s.out" 2> /dev/null | head -n 60 > "$OUT/attribution-self-$s.txt"
  sed -n 1,30p "$OUT/attribution-$s.txt"
done
rm -rf "$WORK"
echo "attribute.sh: done"
