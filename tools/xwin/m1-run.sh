#!/bin/bash
# Cross-builds the runtime stack for win64 and runs it under the host's wine.
#
#   suite/tools/xwin/m1-run.sh [scratch-dir]       (default /tmp/xwin-m1)
#   M1_SRC=<dir> suite/tools/xwin/m1-run.sh ...    take libs/ from <dir> instead of
#                                            this workspace: a fresh clone made
#                                            the way suite/clone.sh lays it out
#
# runtime-core, runtime-heap, runtime-jit, runtime-debug and lang-tang are built
# with their own Makefiles in ghoti-cross-mingw64:deb13 (m1-env.sh makes the
# container imitate MSYS2 well enough for the Makefiles to pick their Windows
# arm; it is not Windows), each library is installed into a prefix inside the
# scratch tree for the next to find through pkg-config, and `make test`, the
# examples, the gates and every test program are run.  m1-probe.sh then links a
# consumer against the installed DLLs, and m1-controls.sh puts each Windows fix
# back to its old self in a scratch copy and requires the suite to notice.
#
# Exit status 0 only if every library is OK, the probe passes and fails its
# planted control, and every control fails.  The sources are the working trees
# under libs/, copied into the scratch tree (build output is kept between runs,
# so a second run is incremental).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC=${M1_SRC:-$ROOT}
W=${1:-/tmp/xwin-m1}
LIBS="runtime-core runtime-heap runtime-jit runtime-debug lang-tang"
DEPS="cutil unicode chron regex text ctang"

for need in podman rsync bison flex m4 python3; do
  command -v $need >/dev/null || { echo "m1-run: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "m1-run: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "m1-run: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }
mkdir -p "$W/logs"
for l in $DEPS $LIBS; do
  rsync -a --exclude build --exclude .git "$SRC/libs/$l/" "$W/$l/" || exit 2
done

in_container() { "$HERE/m1-win.sh" "$W" "$@"; }

echo "== dependencies"
in_container 'bash /tools/xwin/m1-lib.sh deps' > "$W/logs/deps.log" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/deps.log"
[ $rc -eq 0 ] || { echo "m1-run: the dependencies did not build" >&2; exit 2; }

failed=0
for l in $LIBS; do
  echo "== $l"
  in_container "bash /tools/xwin/m1-lib.sh lib $l" > "$W/logs/$l.summary" 2>&1; rc=$?
  grep -vi fontconfig "$W/logs/$l.summary" | grep -v '^SUMMARY'
  grep -h '^SUMMARY' "$W/logs/$l.summary"
  [ $rc -eq 0 ] || failed=1
done

echo "== probe"
in_container 'bash /tools/xwin/m1-probe.sh' > "$W/logs/probe.log" 2>&1
grep -vi fontconfig "$W/logs/probe.log" | tail -n 8
grep -q 'probe rc=0 control rc=[1-9]' "$W/logs/probe.log" || { echo "m1-run: the probe did not pass with its control failing" >&2; failed=1; }

echo "== controls"
in_container 'bash /tools/xwin/m1-controls.sh' > "$W/logs/controls.log" 2>&1
grep -vi fontconfig "$W/logs/controls.log"
grep -q '^CONTROLS ok' "$W/logs/controls.log" || { echo "m1-run: a control did not fail as it must" >&2; failed=1; }

[ $failed -eq 0 ] && echo "m1-run: GREEN" || echo "m1-run: NOT GREEN"
exit $failed
