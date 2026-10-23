#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The dual render of the drawings the suite keeps: shape-cpu's exact union step against the earlier step kept in its tests
# (libs/shape-cpu/tests/support/exact_s4b.c), cell by cell and byte for byte.
#
#   dual.sh WS PREFIX OUT JOBS
#
# WS holds libs/<lib> and suite/ (laid out as suite/clone.sh lays them out), PREFIX is an empty install prefix, OUT the results
# directory; run it inside ghoti-build:gcc16.  shape-cpu is installed with its symbols visible so that the preloaded objects
# (libs/shape-cpu/tools/exact-dual.c: both steps on every cell, the differing cells recorded; exact-frozen.c: the earlier step
# alone) stand between the scan converter and the step.  Each drawing is drawn three ways: as it is, with the earlier step in
# its place, and with both steps compared cell by cell.  Writes OUT/summary.txt: for every drawing the pixels that differ between
# the first two (the targets compared byte for byte, and for an image the pixels that differ and by how many levels at most,
# which is one at most where the two steps round an exact tie differently), the cells that differ, and tools/check-dual-cells.py's verdict on those
# cells against the exact area in rational arithmetic.  The earlier step's winding bound is raised to 32767 so that it follows
# the text, which is deeper than its 255.

set -eu

[ $# -eq 4 ] || { echo "usage: dual.sh WS PREFIX OUT JOBS" >&2; exit 2; }
WS=$1 PREFIX=$2 OUT=$3 JOBS=$4
T=$WS/suite/tools/shape-bench
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
mkdir -p "$PREFIX" "$OUT/logs" "$OUT/new" "$OUT/old" "$OUT/cells"
WORK=${TMPDIR:-/tmp}/dual-$$
mkdir -p "$WORK/scenes"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"

install_lib() {
  echo "== install $1"
  make -C "$2" -j"$JOBS" install PREFIX="$PREFIX" ${3:+"$3"} > "$OUT/logs/install-$1.log" 2>&1 ||
    { tail -n 40 "$OUT/logs/install-$1.log"; echo "dual.sh: installing $1 failed" >&2; exit 1; }
}

for l in cutil security compress unicode math color image shape-geometry shape-scene; do
  install_lib "$l" "$WS/libs/$l"
done
install_lib shape-cpu "$WS/libs/shape-cpu" "EXTRA_CFLAGS=-fvisibility=default"
install_lib font "$WS/libs/font"

PCS="ghoti.io-shape-cpu-0"
CPU=$WS/libs/shape-cpu
# shellcheck disable=SC2046 # pkg-config's output is a word list
for t in exact-dual exact-frozen; do
  cc -std=c17 -O2 -fPIC -shared -Wall -Wextra -DEXACT_WINDING_MAX=32767 -I "$CPU/src/render" $(pkg-config --cflags $PCS) \
    -o "$OUT/$t.so" "$CPU/tools/$t.c" -ldl -lpthread > "$OUT/logs/$t-build.log" 2>&1 ||
    { cat "$OUT/logs/$t-build.log"; echo "dual.sh: building $t failed" >&2; exit 1; }
done

echo "== the drivers"
make -C "$T" -j"$JOBS" PREFIX="$PREFIX" BUILD_DIR="$WORK/bench-build" > "$OUT/logs/bench-build.log" 2>&1 ||
  { tail -n 40 "$OUT/logs/bench-build.log"; echo "dual.sh: building the bench failed" >&2; exit 1; }
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
# shellcheck disable=SC2086,SC2046
cc -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra "$WS/suite/tools/shape-gallery/gallery.c" $(pkg-config --cflags --libs $PC) \
  $RPATH -lm -o "$WORK/gallery" > "$OUT/logs/gallery-build.log" 2>&1 ||
  { cat "$OUT/logs/gallery-build.log"; echo "dual.sh: building gallery.c failed" >&2; exit 1; }
# shellcheck disable=SC2086,SC2046
cc -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra $(pkg-config --cflags ghoti.io-shape-cpu-0 ghoti.io-shape-geometry-0) \
  -o "$WORK/corpus-hash" "$CPU/tools/corpus-hash.c" $(pkg-config --libs ghoti.io-shape-cpu-0) $RPATH -lm \
  > "$OUT/logs/corpus-build.log" 2>&1 || { cat "$OUT/logs/corpus-build.log"; echo "dual.sh: building corpus-hash failed" >&2; exit 1; }
# shellcheck disable=SC2086,SC2046
cc -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra $(pkg-config --cflags $PC) -o "$WORK/scene-hash" \
  "$WS/libs/font/tools/scene-hash.c" $(pkg-config --libs $PC) $RPATH -lm > "$OUT/logs/scene-hash-build.log" 2>&1 ||
  { cat "$OUT/logs/scene-hash-build.log"; echo "dual.sh: building scene-hash failed" >&2; exit 1; }
mkdir -p "$WORK/scenes"
"$WORK/bench-build/bench-shape" emit "$WORK/scenes" "$DEJAVU" > "$OUT/logs/emit.log" 2>&1 ||
  { tail "$OUT/logs/emit.log"; echo "dual.sh: bench-shape emit failed" >&2; exit 1; }

# run NAME SHIM OUTDIR cmd...: the command with the shim preloaded (none: as it is), its output in OUTDIR.
run() {
  name=$1 shim=$2 dir=$3
  shift 3
  mkdir -p "$dir/$name"
  rc=0
  if [ -n "$shim" ]; then
    GSHCPU_DUAL_OUT="$OUT/cells/$name.txt" GSHCPU_DUAL_SUMMARY="$OUT/cells/$name.summary" LD_PRELOAD="$shim" "$@" > "$dir/$name/stdout.log" 2>&1 || rc=$?
  else
    "$@" > "$dir/$name/stdout.log" 2>&1 || rc=$?
  fi
  [ "$rc" -eq 0 ] || echo "dual.sh: $name exited $rc" >&2
}

drawing() {
  name=$1
  shift
  # the command takes the output directory as its last argument, "OUT" in the words below
  echo "== $name"
  rm -f "$OUT/cells/$name.txt" "$OUT/cells/$name.summary"
  for way in new old dual; do
    case $way in
      new) shim= dir=$OUT/new ;;
      old) shim=$OUT/exact-frozen.so dir=$OUT/old ;;
      dual) shim=$OUT/exact-dual.so dir=$OUT/new ;;
    esac
    mkdir -p "$dir/$name/files"
    cmd=
    for w in "$@"; do
      case $w in OUT) cmd="$cmd $dir/$name/files" ;; *) cmd="$cmd $w" ;; esac
    done
    # shellcheck disable=SC2086
    if [ "$way" = dual ]; then
      mkdir -p "$WORK/dual-ignored"
      cmd=$(echo "$cmd" | sed "s#$dir/$name/files#$WORK/dual-ignored#")
    fi
    # shellcheck disable=SC2086
    run "$name" "$shim" "$dir" $cmd
  done
  {
    echo "== $name"
    differing=0
    for f in "$OUT/new/$name/files"/*; do
      [ -e "$f" ] || continue
      b=$(basename "$f")
      if cmp -s "$f" "$OUT/old/$name/files/$b"; then :; else
        differing=$((differing + 1))
        echo "  differs between the steps: $b ($(cmp -l "$f" "$OUT/old/$name/files/$b" 2> /dev/null | wc -l) bytes)"
        case $b in
          *.png | *.pam) python3 -I "$T/pixel-diff.py" "$f" "$OUT/old/$name/files/$b" 1 || echo "  pixel-diff: MORE THAN ONE LEVEL (or not comparable)" ;;
        esac
      fi
    done
    if [ "${STDOUT_CMP:-0}" = 1 ]; then
      if cmp -s "$OUT/new/$name/stdout.log" "$OUT/old/$name/stdout.log"; then echo "  the hashes it prints are the same"; else
        differing=$((differing + 1))
        echo "  the hashes it prints differ:"
        diff "$OUT/new/$name/stdout.log" "$OUT/old/$name/stdout.log" | head -n 12
      fi
    fi
    echo "  targets that differ byte for byte: $differing"
    if [ -f "$OUT/cells/$name.summary" ]; then cat "$OUT/cells/$name.summary"; fi
    if [ -f "$OUT/cells/$name.txt" ]; then
      python3 -I "$CPU/tools/check-dual-cells.py" "$OUT/cells/$name.txt" || true
    fi
  } >> "$SUMMARY"
}

STDOUT_CMP=1
drawing corpus "$WORK/corpus-hash"
drawing font-fixtures "$WORK/scene-hash" "$WS/libs/font/tests/data/fonts"
STDOUT_CMP=0
BENCH=$WORK/bench-build/bench-shape
drawing strokes "$BENCH" frame "$WORK/scenes" OUT strokes
drawing strokes-dashed "$BENCH" frame "$WORK/scenes" OUT strokes-dashed
drawing gallery "$WORK/gallery" "$WS/libs/font/tests/data/fonts" "$DEJAVU" OUT
for w in 14 16 18 20 22; do
  drawing "text-$w" "$WORK/bench-build/stroke-text" "$DEJAVU" --cells 67108864 --points 16777216 --fragments 65536 --pam OUT \
    Union $w Gradient $w Outline $w
done
cat "$SUMMARY"
rm -rf "$WORK"
echo "dual.sh: done"
