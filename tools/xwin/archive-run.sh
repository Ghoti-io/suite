#!/bin/bash
# Cross-builds the archive library and its dependencies for win64 and runs them under wine.
#
#   suite/tools/xwin/archive-run.sh [scratch-dir]      (default /tmp/xwin-archive)
#   M1_IN_IMAGE=1 suite/tools/xwin/archive-run.sh /w   inside Containerfile.wine's image (EVO-X2)
#
# cutil, security and compress are built with their own Makefiles in the mingw image and installed
# into a prefix in the scratch tree; then m1-lib.sh builds archive, installs it, runs `make test`
# (the gates included), runs every test program on its own, and runs the examples in the order
# m1-lib.sh gives for archive: the writers first, then every reader over what they wrote.  Every skip
# must be named in m1-skips.txt.  This is wine, not Windows: it shows the _WIN32 arms compile
# under -Werror and behave under wine's implementation of the API, and nothing about a real
# Windows machine.  Exit status 0 only if archive's SUMMARY says OK.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC=${M1_SRC:-$ROOT}
W=${1:-/tmp/xwin-archive}
if [ -n "${M1_IN_IMAGE:-}" ]; then
  [ "$W" = /w ] || { echo "archive-run: with M1_IN_IMAGE the scratch directory is /w" >&2; exit 2; }
fi
mkdir -p "$W/logs"
for l in cutil security compress archive; do
  rsync -a --exclude build --exclude .git "$SRC/libs/$l/" "$W/$l/" || exit 2
done
in_container() { "$HERE/m1-win.sh" "$W" "$@"; }
echo "== dependencies"
in_container 'M1_DEPS="cutil:cutil security:security compress:compress" bash /tools/xwin/m1-lib.sh deps' > "$W/logs/deps.log" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/deps.log"
[ $rc -eq 0 ] || { echo "archive-run: the dependencies did not build" >&2; exit 2; }
echo "== archive"
in_container 'bash /tools/xwin/m1-lib.sh lib archive' > "$W/logs/archive.summary" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/archive.summary"
[ $rc -eq 0 ] && echo "archive-run: GREEN" || echo "archive-run: NOT GREEN"
exit $rc
