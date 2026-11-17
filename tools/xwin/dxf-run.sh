#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# shape-dxf's unit tests and its lowering gate for win64 under wine (the m1 machinery: m1-env.sh, m1-win.sh).
#
#   suite/tools/xwin/dxf-run.sh [scratch-dir]            (default /tmp/xwin-dxf; podman on the host)
#   M1_IN_IMAGE=1 suite/tools/xwin/dxf-run.sh /w          inside Containerfile.wine's image
#
# Builds gtest and the libraries shape-dxf depends on for win64 into a prefix in the scratch tree (`m1-lib.sh deps`
# with M1_DEPS set), then dxf-lib.sh builds and runs shape-dxf's test programs and the lowering gate against the
# goldens committed from the x86-64 build.  This imitates MSYS2, it is not Windows (README.md).  Exit 0 only if
# dxf-lib.sh says OK.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC=${M1_SRC:-$ROOT}
W=${1:-/tmp/xwin-dxf}
if [ -n "${M1_IN_IMAGE:-}" ]; then
  [ "$W" = /w ] || { echo "dxf-run: with M1_IN_IMAGE the scratch directory is /w" >&2; exit 2; }
fi
mkdir -p "$W/logs"
LIBS="cutil security compress math color image shape-geometry shape-scene shape-dxf"
for l in $LIBS; do
  rsync -a --exclude build --exclude .git "$SRC/libs/$l/" "$W/$l/" || exit 2
done
in_container() { "$HERE/m1-win.sh" "$W" "$@"; }
DEPS=""
for l in $LIBS; do [ $l = shape-dxf ] || DEPS="$DEPS $l:$l"; done
echo "== dependencies"
in_container "M1_DEPS=\"${DEPS# }\" bash /tools/xwin/m1-lib.sh deps" > "$W/logs/deps.log" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/deps.log"
[ $rc -eq 0 ] || { echo "dxf-run: the dependencies did not build" >&2; exit 2; }
echo "== shape-dxf"
in_container 'bash /tools/xwin/dxf-lib.sh' > "$W/logs/dxf.summary" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/dxf.summary"
[ $rc -eq 0 ] && echo "dxf-run: GREEN" || echo "dxf-run: NOT GREEN"
exit $rc
