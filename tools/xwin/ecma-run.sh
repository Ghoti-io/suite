#!/bin/bash
# Cross-builds lang-ecmascript for win64 and runs it under the host's wine.
#
#   suite/tools/xwin/ecma-run.sh [scratch-dir]       (default /tmp/xwin-ecma)
#
# The same machinery as m1-run.sh (m1-win.sh, m1-env.sh, m1-lib.sh: the library's own Makefile in
# ghoti-cross-mingw64:deb13, made to imitate MSYS2 well enough to take its Windows arm; it is not Windows),
# for the one library and the four it needs: cutil, unicode, regex and runtime-core are built and installed
# into a prefix in the scratch tree, then lang-ecmascript's `make all install test examples` run, with every
# test program, example and the benchmark smoke run by itself. The libraries' fetched corpora (test262, the
# real-world corpus) must be present under libs/lang-ecmascript/third_party (`make fetch-test262 fetch-corpus`),
# because `make test` reads them. The gates that run Python run the host's.
#
# Exit status 0 only if the library is OK: every make target, every program and every example passed, with no
# compiler warning, and the skips are exactly the ones m1-skips.txt names for it (none).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC=${M1_SRC:-$ROOT}
W=${1:-/tmp/xwin-ecma}
LIB=lang-ecmascript
DEPS="cutil:cutil unicode:unicode regex:regex runtime-core:runtime-core"

for need in podman rsync bison flex m4 python3; do
  command -v $need >/dev/null || { echo "ecma-run: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "ecma-run: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "ecma-run: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }
[ -d "$SRC/libs/$LIB/third_party/test262" ] && [ -d "$SRC/libs/$LIB/third_party/corpus" ] ||
  { echo "ecma-run: $SRC/libs/$LIB/third_party has no test262 or corpus; fetch them in the library first" >&2; exit 2; }
mkdir -p "$W/logs"
for d in $DEPS $LIB:$LIB; do
  l=${d%%:*}
  rsync -a --exclude /build --exclude .git "$SRC/libs/$l/" "$W/$l/" || exit 2
done

in_container() { "$HERE/m1-win.sh" "$W" "$@"; }

echo "== dependencies"
in_container "M1_DEPS='$DEPS' bash /tools/xwin/m1-lib.sh deps" > "$W/logs/deps.log" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/deps.log"
[ $rc -eq 0 ] || { echo "ecma-run: the dependencies did not build" >&2; exit 2; }

echo "== $LIB"
# Every gate of `make test` but check-rule-codes, which compares the registry with the repository's history and
# so needs git, which the container lacks; the host's own `make test` runs it. GLECMA_TEST_SLOW is for the
# emulated runs (smaller sweeps).
GATES="check-symbols check-aliasing check-stamps check-fp-contract check-labels check-edges check-bench check-limit-names check-data-versions check-test262 check-differential check-reparse check-corpus check-node-selftest check-unsupported check-clauses check-gtest-full fuzz-replay"
in_container "GLECMA_TEST_SLOW=1 TEST_GATES='$GATES' bash /tools/xwin/m1-lib.sh lib $LIB" > "$W/logs/$LIB.summary" 2>&1; rc=$?
grep -vi fontconfig "$W/logs/$LIB.summary" | grep -v '^SUMMARY'
grep -h '^SUMMARY' "$W/logs/$LIB.summary"
[ $rc -eq 0 ] && echo "ecma-run: GREEN" || echo "ecma-run: NOT GREEN"
exit $rc
