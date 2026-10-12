#!/bin/bash
# Cross-builds the Defiant libraries and the defiant program for win64 and
# runs them under the host's wine, the way m1-run.sh does for the runtime
# stack.
#
#   suite/tools/xwin/defiant-run.sh [scratch-dir]      (default /tmp/xwin-defiant)
#   M1_SRC=<dir> suite/tools/xwin/defiant-run.sh ...   take libs/ from <dir>
#
# Everything the Defiant libraries stand on is built and installed first, with
# its own Makefile and no tests (m1-run.sh is the gate for the runtime stack
# and the earlier suite libraries). Then each Defiant library is built,
# installed and tested, and every test program is run on its own, by
# m1-lib.sh. Then the defiant program is started under wine in its memory
# mode and asked for /alive, the sign-in page and a login. Last, controls put
# the Windows fixes in the libraries back to their old selves, and the build
# of each must fail.
#
# This is the same imitation of MSYS2 that m1-run.sh uses; it is not Windows.
# Exit status 0 only if every library is OK and the program answers.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC=${M1_SRC:-$ROOT}
W=${1:-/tmp/xwin-defiant}
# source directory:installed DLL name (ctang installs as "tang")
DEPS="cutil:cutil unicode:unicode chron:chron regex:regex text:text ctang:tang security:security compress:compress archive:archive http:http vendor-sqlite:vendor-sqlite runtime-core:runtime-core runtime-heap:runtime-heap runtime-jit:runtime-jit runtime-debug:runtime-debug lang-tang:lang-tang"
LIBS="defiant-storage defiant-storage-memory defiant-storage-sqlite defiant-core defiant-test defiant-web"

for need in podman rsync bison flex m4 python3; do
  command -v $need >/dev/null || { echo "defiant-run: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "defiant-run: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "defiant-run: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }
mkdir -p "$W/logs"
for d in $DEPS; do
  rsync -a --exclude build --exclude .git "$SRC/libs/${d%%:*}/" "$W/${d%%:*}/" || exit 2
done
for l in $LIBS; do
  rsync -a --exclude build --exclude .git "$SRC/libs/$l/" "$W/$l/" || exit 2
done
# The cross build reads suite/libraries.txt for its prefix gate.
mkdir -p "$W/suite" && cp "$SRC/suite/libraries.txt" "$W/suite/"

in_container() { "$HERE/m1-win.sh" "$W" "$@"; }

echo "== dependencies"
in_container "M1_DEPS='$DEPS' bash /tools/xwin/m1-lib.sh deps" > "$W/logs/deps.log" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/deps.log"
[ $rc -eq 0 ] || { echo "defiant-run: the dependencies did not build" >&2; exit 2; }

failed=0
for l in $LIBS; do
  echo "== $l"
  in_container "LIBRARIES_TXT=/w/suite/libraries.txt bash /tools/xwin/m1-lib.sh lib $l" > "$W/logs/$l.summary" 2>&1; rc=$?
  grep -vi fontconfig "$W/logs/$l.summary" | grep -v '^SUMMARY'
  grep -h '^SUMMARY' "$W/logs/$l.summary"
  [ $rc -eq 0 ] || failed=1
done

echo "== the defiant program"
in_container 'bash /tools/xwin/defiant-smoke.sh' > "$W/logs/smoke.log" 2>&1
grep -vi fontconfig "$W/logs/smoke.log" | tail -n 12
grep -q '^SMOKE ok' "$W/logs/smoke.log" || { echo "defiant-run: the program did not answer under wine" >&2; failed=1; }

echo "== controls"
in_container 'bash /tools/xwin/defiant-controls.sh' > "$W/logs/controls.log" 2>&1
grep -vi fontconfig "$W/logs/controls.log"
grep -q '^CONTROLS ok' "$W/logs/controls.log" || { echo "defiant-run: a control did not fail as it must" >&2; failed=1; }

[ $failed -eq 0 ] && echo "defiant-run: GREEN" || echo "defiant-run: NOT GREEN"
exit $failed
