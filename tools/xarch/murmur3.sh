#!/bin/bash
# Run suite/tools/xarch/murmur3-probe.c against every target in the matrix, using the
# src/string.c given as $1 (default: the working tree's).  Writes one output
# file per target under /tmp/xarch-out so they can be diffed against each other.
#
# Caveat worth knowing: the generated float.h used here is the *host's*, from
# build/linux/release/include.  Every target in the matrix is IEEE-754 with
# 32-bit float and 64-bit double, and string.c touches neither, so it is inert
# -- but it is not a cross-generated header and should not be trusted for
# anything that reads those types.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/targets.sh"
SRC="${1:-$ROOT/libs/cutil/src/string.c}"
OUT=/tmp/xarch-out
mkdir -p $OUT
INC="-I $ROOT/libs/cutil/include -I $ROOT/libs/cutil/build/linux/release/include"

one() {
  local triple="$1" cc="$2" qemu="$3" desc="$4"
  local bin="/tmp/probe-$triple"
  if ! $cc -std=c17 -O2 $INC "$HERE/murmur3-probe.c" "$SRC" \
        -o "$bin" 2>/tmp/err-$triple; then
    printf "%-22s BUILD FAILED: %s\n" "$triple" "$(head -2 /tmp/err-$triple | tr '\n' ' ')"
    return
  fi
  local rc
  xarch_run "$triple" "$qemu" "$bin" > "$OUT/$triple.txt" 2>"$OUT/$triple.err"
  rc=$?
  if [ $rc -ne 0 ]; then
    printf "%-22s EXIT %-3s %s | last line: %s\n" "$triple" "$rc" "$desc" \
      "$(tail -1 "$OUT/$triple.txt" 2>/dev/null)"
    head -2 "$OUT/$triple.err" | sed 's/^/                       /'
  else
    printf "%-22s ok        %s\n" "$triple" "$desc"
  fi
}
echo "probing with: $SRC"
xarch_each one
