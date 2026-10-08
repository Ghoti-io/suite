#!/bin/bash
# Writes "<test binary> <tests listed>" for every test binary of libs/http, from a
# HOST build of the library's working tree, to the file named by $1. The cross
# runs (http-arm64-host.sh, suite/tools/xwin/http.sh) require their passed counts to
# equal these, binary by binary.
#
#   suite/tools/xarch/http-host-counts.sh COUNTS-FILE
#
# Environment: HOST_PREFIX (the PREFIX= of the host build, default $ROOT/.local;
# the dependencies' .pc files are found through PKG_CONFIG_PATH if set, else under
# HOST_PREFIX/share/pkgconfig), HOST_BUILD (the BUILD= name of the build tree,
# default xc, so that this does not touch a tree another session is using), JOBS.
# Nothing is installed; the prefix is only read.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${1:?usage: http-host-counts.sh COUNTS-FILE}
HOST_PREFIX=${HOST_PREFIX:-$ROOT/.local}
HOST_BUILD=${HOST_BUILD:-xc}
JOBS=${JOBS:-$(nproc)}
export PKG_CONFIG_PATH="$HOST_PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
M=(make -C "$ROOT/libs/http" PREFIX="$HOST_PREFIX" BUILD="$HOST_BUILD" -j"$JOBS")
NAMES=$(echo 'print-%: ; @echo $($*)' | "${M[@]}" -s --no-print-directory -f Makefile -f - print-TEST_NAMES 2>/dev/null)
[ -n "$NAMES" ] || { echo "http-host-counts: cannot read TEST_NAMES from libs/http/Makefile" >&2; exit 2; }
APPS=$ROOT/libs/http/build/linux/$HOST_BUILD/apps
LOG="$OUT.build.log"
"${M[@]}" all > "$LOG" 2>&1 || { tail -20 "$LOG" >&2; echo "http-host-counts: the host build failed" >&2; exit 2; }
: > "$OUT"
for n in $NAMES; do
  "${M[@]}" "build/linux/$HOST_BUILD/apps/$n" >> "$LOG" 2>&1 ||
    { tail -20 "$LOG" >&2; echo "http-host-counts: $n did not build on the host" >&2; exit 2; }
  c=$("$APPS/$n" --gtest_list_tests 2>&1 | grep -c '^  ')
  echo "$n $c" >> "$OUT"
done
echo "host: $(echo $NAMES | wc -w) test binaries, $(awk '{s += $2} END {print s}' "$OUT") tests listed"
