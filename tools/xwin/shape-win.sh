#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with suite/tools at /tools and the scratch tree at /w.
# Argument: the native x86_64 hash.  See shape.sh.
set -u
want=${1:?usage: shape-win.sh NATIVE_HASH}
P=/tools/xarch/shape-hash-probe.c
cd /w || exit 2

x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -Wextra -Werror -ffp-contract=off "$P" -o shape-probe.exe -lm || { echo "shape-win: the probe did not build" >&2; exit 2; }
x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -Wextra -Werror -ffp-contract=off -DSHAPE_PLANT_BIT "$P" -o shape-plant.exe -lm || { echo "shape-win: the planted probe did not build" >&2; exit 2; }
file shape-probe.exe | cut -c1-100

got=$(./shape-probe.exe 2>/dev/null | tr -d '\r' | awk '/^shape.hash/{print $2}')
planted=$(./shape-plant.exe 2>/dev/null | tr -d '\r' | awk '/^shape.hash/{print $2}')
echo "windows (wine) hash: ${got:-none}   planted: ${planted:-none}   native: $want"

rc=0
if [ -n "$got" ] && [ "$got" = "$want" ]; then
  echo "   ok    win64 prints the native hash"
else
  echo "   FAIL  win64 does not print the native hash"; rc=1
fi
if [ -n "$planted" ] && [ "$planted" != "$want" ]; then
  echo "   ok    planted: one differing bit on win64 is not the native hash, so the gate can fail"
else
  echo "   FAIL  planted: the win64 probe with a flipped bit still matches (or did not run)"; rc=1
fi
[ $rc -eq 0 ] && echo "shape-win: PASS" || echo "shape-win: FAIL"
exit $rc
