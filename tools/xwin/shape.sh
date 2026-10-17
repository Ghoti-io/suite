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
"$HERE/m1-win.sh" "$W" "bash /tools/xwin/shape-win.sh $want"
