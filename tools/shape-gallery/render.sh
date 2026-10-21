#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The gallery's build-and-render step, run inside ghoti-build:gcc16 (README.md
# says how):
#
#   render.sh WS PREFIX OUT JOBS LIB...
#
# WS holds libs/<lib> and suite/ (the staged committed trees), PREFIX is an
# empty install prefix, OUT the run's directory.  Installs LIB... into PREFIX
# in the order given (suite/libraries.txt order), builds gallery.c against
# them, renders the seven PNGs into OUT with gallery.log beside them, and
# decode-checks every PNG with compare.py.  Build logs go to OUT/logs/.
#
#   render.sh --host-info ENGINE OUT
#
# is the step outside the container: the build image's id and the host's name,
# into OUT/host.txt, for the manifest.

set -eu

if [ "${1:-}" = --host-info ]; then
  {
    echo "host: $(hostname)"
    echo "image: ghoti-build:gcc16"
    echo "image_id: $("$2" image inspect --format '{{.Id}}' ghoti-build:gcc16 2> /dev/null || echo unknown)"
  } > "$3/host.txt"
  exit 0
fi

[ $# -ge 5 ] || { echo "usage: render.sh WS PREFIX OUT JOBS LIB..." >&2; exit 2; }
WS=$1 PREFIX=$2 OUT=$3 JOBS=$4
shift 4
T=$WS/suite/tools/shape-gallery
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
mkdir -p "$PREFIX" "$OUT/logs"

for l in "$@"; do
  echo "== install $l"
  # PREFIX on every make: the rpath is added only when it is set.
  make -C "$WS/libs/$l" -j"$JOBS" install PREFIX="$PREFIX" > "$OUT/logs/install-$l.log" 2>&1 ||
    { tail -n 40 "$OUT/logs/install-$l.log"; echo "render.sh: installing $l failed" >&2; exit 1; }
done

echo "== build the gallery"
PC="ghoti.io-font-0 ghoti.io-shape-cpu-0 ghoti.io-shape-scene-0 ghoti.io-shape-geometry-0 ghoti.io-image-0
    ghoti.io-color-0 ghoti.io-math-0 ghoti.io-cutil-0"
# The rpath comes from each -L directory: the .pc files define no libdir
# variable, and without an rpath the program finds its libraries only where
# the install could run ldconfig, which needs root.
RPATH=
# shellcheck disable=SC2086 # PC is a word list
for d in $(pkg-config --libs-only-L $PC); do
  RPATH="$RPATH -Wl,-rpath,${d#-L}"
done
CFLAGS="-std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra"
BIN=${TMPDIR:-/tmp}/shape-gallery-$$
# shellcheck disable=SC2086 # pkg-config's output and RPATH are word lists
gcc $CFLAGS "$T/gallery.c" $(pkg-config --cflags --libs $PC) $RPATH -lm -o "$BIN" > "$OUT/logs/gallery-build.log" 2>&1 ||
  { cat "$OUT/logs/gallery-build.log"; echo "render.sh: building gallery.c failed" >&2; exit 1; }
{
  echo "compiler: $(gcc --version | head -n 1)"
  echo "gallery_flags: $CFLAGS"
} > "$OUT/build.txt"

echo "== the pinned font"
sh "$WS/suite/tools/shape-success/fetch.sh" "$WS/suite/tools/shape-success/data"
echo "font_file: DejaVuSans.ttf 2.37 sha256 $(sha256sum "$WS/suite/tools/shape-success/data/DejaVuSans.ttf" | cut -d' ' -f1)" \
  >> "$OUT/build.txt"

echo "== render"
rc=0
"$BIN" "$WS/libs/font/tests/data/fonts" "$WS/suite/tools/shape-success/data/DejaVuSans.ttf" "$OUT" > "$OUT/gallery.log" 2>&1 ||
  rc=$?
rm -f "$BIN"
echo "$rc" > "$OUT/gallery.exit"
tail -n 3 "$OUT/gallery.log"

echo "== decode check"
drc=0
python3 -I "$T/compare.py" check "$OUT"/*.png > "$OUT/decode-check.txt" 2>&1 || drc=$?
cat "$OUT/decode-check.txt"
[ "$rc" -eq 0 ] || { echo "render.sh: the gallery exited $rc" >&2; exit 1; }
[ "$drc" -eq 0 ] || { echo "render.sh: a PNG failed the decode check" >&2; exit 1; }
