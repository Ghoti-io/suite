#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# text's XML, HTML, Encoding and markup tests for win64 under wine (the m1 machinery: m1-env.sh, m1-win.sh).
#
#   suite/tools/xwin/text-run.sh [scratch-dir]            (default /tmp/xwin-text; podman on the host)
#   M1_IN_IMAGE=1 suite/tools/xwin/text-run.sh /w          inside Containerfile.wine's image (the EVO-X2)
#
# Builds gtest, cutil, unicode, chron, regex and text for win64 into a prefix in the scratch tree
# (`m1-lib.sh deps` with M1_DEPS set), then text-lib.sh builds and runs the test programs.  This imitates
# MSYS2, it is not Windows (README.md).  Exit 0 only if text-lib.sh says OK.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC=${M1_SRC:-$ROOT}
W=${1:-/tmp/xwin-text}
if [ -n "${M1_IN_IMAGE:-}" ]; then
  [ "$W" = /w ] || { echo "text-run: with M1_IN_IMAGE the scratch directory is /w" >&2; exit 2; }
fi
mkdir -p "$W/logs"
for l in cutil unicode chron regex security text; do
  rsync -a --exclude build --exclude .git "$SRC/libs/$l/" "$W/$l/" || exit 2
done
in_container() { "$HERE/m1-win.sh" "$W" "$@"; }
echo "== dependencies"
in_container 'M1_DEPS="cutil:cutil unicode:unicode chron:chron regex:regex security:security text:text" bash /tools/xwin/m1-lib.sh deps' > "$W/logs/deps.log" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/deps.log"
[ $rc -eq 0 ] || { echo "text-run: the dependencies did not build" >&2; exit 2; }
echo "== text"
in_container 'bash /tools/xwin/text-lib.sh' > "$W/logs/text.summary" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/text.summary"
[ $rc -eq 0 ] && echo "text-run: GREEN" || echo "text-run: NOT GREEN"
exit $rc
