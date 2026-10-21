#!/bin/sh
# The shape family's success scene on one host (story 16 of the shape milestone;
# the SPEC's success signal): run font's example program on the pinned font and
# compare everything it prints with expected.txt, the output committed from the
# x86-64 build in ghoti-build:gcc16.  The cross targets are suite/tools/xarch/
# shape.sh section 10 and suite/tools/xwin/shape.sh; this is the native arm, for a
# real arm64 machine.
#
#   suite/tools/shape-success/check.sh FONT_SCENE [FONT]
#
# FONT_SCENE is libs/font's examples/font-scene, built; FONT defaults to data/
# DejaVuSans.ttf (fetch.sh).  The comparison is first seen to fail: the output
# with one bit of its fold flipped must not match.  Exit 0 only when the output
# matches and the planted copy does not.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${1:?usage: check.sh FONT_SCENE [FONT]}
FONT=${2:-$HERE/data/DejaVuSans.ttf}
[ -f "$FONT" ] || { echo "check.sh: no $FONT (run $HERE/fetch.sh)" >&2; exit 2; }
out=$(mktemp)
trap 'rm -f "$out" "$out.planted"' EXIT INT TERM
"$BIN" "$FONT" AB > "$out" || { echo "check.sh: FAIL: the program exited non-zero" >&2; cat "$out" >&2; exit 1; }
# The plant: the last hex digit of the fold, xor 1.
awk '$1=="font-scene.hash"{d=substr($2,16,1); n=index("0123456789abcdef",d)-1; n=(n%2==0)?n+1:n-1;
  $2=substr($2,1,15) substr("0123456789abcdef",n+1,1)} {print}' "$out" > "$out.planted"
if cmp -s "$out.planted" "$HERE/expected.txt"; then
  echo "check.sh: FAIL: the planted output matched expected.txt; the comparison sees nothing" >&2
  exit 1
fi
if diff -u "$HERE/expected.txt" "$out"; then
  echo "check.sh: PASS: the success scene printed expected.txt ($(awk '$1=="font-scene.hash"{print $2}' "$out")), and a planted bit did not"
  exit 0
fi
echo "check.sh: FAIL: the output differs from expected.txt" >&2
exit 1
