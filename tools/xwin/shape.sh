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
# It also runs the math corpus hash (CAP-1): libs/math/tools/corpus-hash.c with
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

command -v podman >/dev/null || { echo "shape.sh: podman is needed on the host" >&2; exit 2; }
[ -d /usr/lib/wine ] || { echo "shape.sh: the host needs wine in /usr/lib/wine" >&2; exit 2; }

mkdir -p "$W/logs"
ref=$(mktemp -d "$W/ref.XXXXXX") || exit 2
gcc -O2 -std=gnu17 -Wall -Wextra -Werror -ffp-contract=off "$ROOT/suite/tools/xarch/shape-hash-probe.c" -o "$ref/probe" -lm || exit 2
want=$("$ref/probe" | awk '/^shape.hash/{print $2}')
rm -rf "$ref"
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
"$HERE/m1-win.sh" "$W" "bash /tools/xwin/shape-win.sh $want $math_hash $math_count"
