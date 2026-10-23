#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The COLRv1 comparison of `font`'s lowering, drawn by shape-cpu, with Skia's renderer (color-paint story P4, L13).
#
#   run-colr.sh build  PREFIX BUILD_DIR      build colr-shape and colr-cmp against the installed suite (a build image)
#   run-colr.sh shape  BUILD_DIR FONTS OUT   draw every configuration of configs.txt with shape-cpu
#   run-colr.sh skia   FONTS OUT             draw them with Skia: run INSIDE Skia's pinned image (compiles colr-skia)
#   run-colr.sh compare BUILD_DIR OUT        compare cell by cell and apply the bound and the allow-list
#
# FONTS is a directory holding the fonts configs.txt names (found under it at any depth); OUT holds the atlases and the
# report. The bound is the shape benchmark's (README.md, "The cross-check"): the mean absolute difference over every
# channel of the cell at most 1.0 levels, and at most 0.1 % of its pixels with a channel more than 32 levels apart.
# skia-allow.txt lists the glyphs outside it, each with the reason; the gate fails when a glyph outside the bound is
# not on it, when the list names a glyph that is inside the bound now, and when the control is not seen to fail.
#
# The control (`shape` also writes it): every eighth fill of each of the first two hundred glyphs of the first
# configuration left out. It must put at least half of the glyphs it changes outside the bound or the gate reports
# itself blind and fails.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)

die() {
  echo "run-colr.sh: $*" >&2
  exit 1
}

phase=${1:-}
shift || true

find_font() {
  found=$(find "$1" -name "$2" -type f 2> /dev/null | head -n 1)
  [ -n "$found" ] || die "no font $2 under $1"
  echo "$found"
}

# A line of configs.txt without its comments.
configs() {
  grep -v '^[[:space:]]*#' "$HERE/configs.txt" | grep -v '^[[:space:]]*$'
}

case "$phase" in
  build)
    PREFIX=${1:?PREFIX} B=${2:?BUILD_DIR}
    mkdir -p "$B"
    PKGS="ghoti.io-shape-cpu-0 ghoti.io-shape-scene-0 ghoti.io-shape-geometry-0 ghoti.io-image-0 ghoti.io-font-0"
    export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
    # shellcheck disable=SC2046
    cc -std=c17 -O2 -g -Wall -Wextra -Werror $(pkg-config --cflags $PKGS) -o "$B/colr-shape" "$HERE/colr-shape.c" \
      $(pkg-config --libs $PKGS) -Wl,-rpath,"$PREFIX/lib/ghoti.io" -lm
    cc -std=c17 -O2 -g -Wall -Wextra -Werror -o "$B/colr-cmp" "$HERE/colr-cmp.c" -lm
    echo "built colr-shape and colr-cmp in $B"
    ;;
  shape)
    B=${1:?BUILD_DIR} FONTS=${2:?FONTS} OUT=${3:?OUT}
    mkdir -p "$OUT"
    configs | while read -r name font size cell every coords axes; do
      path=$(find_font "$FONTS" "$font")
      args="--size $size --cell $cell --encoded"
      [ "$every" = 1 ] || args="$args --every $every"
      [ "$coords" = - ] || args="$args --coords $coords"
      # shellcheck disable=SC2086
      "$B/colr-shape" "$path" "$OUT/$name.shape.pam" "$OUT/$name.glyphs" $args 2> "$OUT/$name.shape.log"
      echo "shape $name: $(tail -n 1 "$OUT/$name.shape.log")"
    done
    # The control: the first configuration's first two hundred glyphs with every eighth fill left out.
    configs > "$OUT/.configs"
    read -r name font size cell every coords axes < "$OUT/.configs"
    path=$(find_font "$FONTS" "$font")
    "$B/colr-shape" "$path" "$OUT/control.shape.pam" "$OUT/control.glyphs" --size "$size" --cell "$cell" --encoded \
      --drop 8 --limit 200 2> "$OUT/control.shape.log"
    echo "control: $name, $(tail -n 1 "$OUT/control.shape.log")"
    ;;
  skia)
    FONTS=${1:?FONTS} OUT=${2:?OUT}
    defs=$(grep -E '^ *[A-Z_][A-Z0-9_]*(=.*)?$' /opt/skia/share/DEFINES | grep -v SKIA_IMPLEMENTATION | sed 's/^ */-D/' | tr '\n' ' ')
    # shellcheck disable=SC2086
    clang++-19 -std=c++20 -O2 -DNDEBUG $defs -I/opt/skia -o /tmp/colr-skia "$HERE/colr-skia.cpp" \
      -Wl,--start-group /opt/skia/out/Release/*.a -Wl,--end-group -lfreetype -lpthread -ldl
    cat /opt/skia/share/PROVENANCE
    configs | while read -r name font size cell every coords axes; do
      path=$(find_font "$FONTS" "$font")
      args="--size $size --cell $cell"
      [ "$axes" = - ] || args="$args --axes $axes"
      # shellcheck disable=SC2086
      /tmp/colr-skia "$path" "$OUT/$name.skia.pam" "$OUT/$name.glyphs" $args 2>&1 | sed "s/^/$name: /"
    done
    ;;
  compare)
    B=${1:?BUILD_DIR} OUT=${2:?OUT}
    configs | while read -r name font size cell every coords axes; do
      "$B/colr-cmp" "$OUT/$name.skia.pam" "$OUT/$name.shape.pam" "$OUT/$name.glyphs" "$cell" 64 > "$OUT/$name.cmp"
    done
    configs > "$OUT/.configs"
    read -r name font size cell every coords axes < "$OUT/.configs"
    "$B/colr-cmp" "$OUT/$name.skia.pam" "$OUT/control.shape.pam" "$OUT/control.glyphs" "$cell" 64 > "$OUT/control.cmp"
    python3 "$HERE/colr-report.py" "$OUT" "$HERE/configs.txt" "$HERE/skia-allow.txt" "$@"
    ;;
  *)
    die "usage: run-colr.sh build|shape|skia|compare ..."
    ;;
esac
