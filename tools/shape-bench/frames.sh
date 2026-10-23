#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The frame time of the stroke scenes with shape-cpu's exact union step and with the earlier one, on one machine:
#
#   frames.sh WS PREFIX OUT JOBS [CPU]
#
# WS holds libs/<lib> and suite/ (laid out as suite/clone.sh lays them out), PREFIX is an empty install prefix, OUT the results
# directory; run it inside ghoti-build:gcc16.  The libraries are built as the suite builds them (release flags), shape-cpu with
# its symbols visible so that the earlier step (libs/shape-cpu/tests/support/exact_s4b.c, preloaded as exact-frozen.so) can
# stand in for the library's.  bench-shape runs `strokes` and `strokes-dashed` the way a benchmark run does (31 timed frames
# after the warm-up, one thread, 1920x1080, the median), pinned to CPU (default 4) when taskset is there, once with the
# library's step and once with the earlier one.  OUT/frames.txt has the lines bench-shape prints (one JSON record each) and the
# host's CPU model, the compiler and the commits, which are the provenance of the figures.  No threshold (AD-28).

set -eu

[ $# -ge 4 ] || { echo "usage: frames.sh WS PREFIX OUT JOBS [CPU]" >&2; exit 2; }
WS=$1 PREFIX=$2 OUT=$3 JOBS=$4 CPU=${5:-4}
T=$WS/suite/tools/shape-bench
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
mkdir -p "$PREFIX" "$OUT/logs"
WORK=${TMPDIR:-/tmp}/frames-$$
mkdir -p "$WORK/scenes"

install_lib() {
  echo "== install $1"
  make -C "$2" -j"$JOBS" install PREFIX="$PREFIX" ${3:+"$3"} > "$OUT/logs/install-$1.log" 2>&1 ||
    { tail -n 40 "$OUT/logs/install-$1.log"; echo "frames.sh: installing $1 failed" >&2; exit 1; }
}
for l in cutil security compress unicode math color image shape-geometry shape-scene; do
  install_lib "$l" "$WS/libs/$l"
done
install_lib shape-cpu "$WS/libs/shape-cpu" "EXTRA_CFLAGS=-fvisibility=default"
install_lib font "$WS/libs/font"
PCS="ghoti.io-shape-cpu-0"
# shellcheck disable=SC2046
cc -std=c17 -O2 -fPIC -shared -DEXACT_WINDING_MAX=32767 -I "$WS/libs/shape-cpu/src/render" $(pkg-config --cflags $PCS) \
  -o "$OUT/exact-frozen.so" "$WS/libs/shape-cpu/tools/exact-frozen.c" -ldl > "$OUT/logs/frozen-build.log" 2>&1 ||
  { cat "$OUT/logs/frozen-build.log"; echo "frames.sh: building exact-frozen failed" >&2; exit 1; }
make -C "$T" -j"$JOBS" PREFIX="$PREFIX" BUILD_DIR="$WORK/bench-build" > "$OUT/logs/bench-build.log" 2>&1 ||
  { tail -n 40 "$OUT/logs/bench-build.log"; echo "frames.sh: building the bench failed" >&2; exit 1; }
FETCH=$WS/suite/tools/shape-success
sh "$FETCH/fetch.sh" "$FETCH/data"
"$WORK/bench-build/bench-shape" emit "$WORK/scenes" "$FETCH/data/DejaVuSans.ttf" > "$OUT/logs/emit.log" 2>&1 ||
  { tail "$OUT/logs/emit.log"; echo "frames.sh: bench-shape emit failed" >&2; exit 1; }

PIN=
if command -v taskset > /dev/null 2>&1; then
  PIN="taskset -c $CPU"
fi
{
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"
  echo "compiler: $(gcc --version | head -n 1)"
  echo "pinned: ${PIN:-no}"
  for l in shape-geometry shape-scene shape-cpu; do echo "commit $l: $(git -C "$WS/libs/$l" rev-parse --short HEAD 2> /dev/null || echo unknown)"; done
  echo "uname: $(uname -srm)"
} > "$OUT/frames.txt"
for way in new old; do
  shim=
  [ "$way" = old ] && shim=$OUT/exact-frozen.so
  mkdir -p "$WORK/out-$way"
  echo "== frames: $way"
  {
    echo "## the step: $way (new is the library's, old is the earlier one preloaded)"
    # shellcheck disable=SC2086
    SHAPE_BENCH_ONLY=strokes,strokes-dashed LD_PRELOAD=$shim $PIN "$WORK/bench-build/bench-shape" run "$WORK/scenes" "$WORK/out-$way" 2> "$OUT/logs/frames-$way.err"
  } >> "$OUT/frames.txt"
done
cat "$OUT/frames.txt"
rm -rf "$WORK"
echo "frames.sh: done"
