#!/bin/bash
#
# Builds cutil's fibers for aarch64 and runs the architecture checks under
# qemu-aarch64 (story 1 of the Defiant milestone, notes/cutil/fibers.md).
#
# Run it in the ghoti-xarch image, from the workspace root:
#
#   podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:deb13 \
#     bash /work/suite/tools/xarch/fiber.sh
#
# What it does, in order:
#
#   1. Checks the machinery.  An aarch64 binary is aarch64 by its ELF header,
#      it does NOT run on this host without qemu, and under qemu it runs.  A
#      cross-architecture result is worthless if the "foreign" binary was
#      quietly the host's, and that failure is silent.
#   2. Builds fiber.c, with cutil's own flags and -Werror, and the C harness
#      suite/tools/xarch/fiber-check.c for aarch64, and runs it under qemu-user: the
#      switch, callee-saved registers, FPCR isolation, the guard page and the
#      thread pin.  It builds the same pair for the host and runs that too,
#      as the control: a harness that fails on the host fails for a reason
#      that is not the architecture.
#   3. Plants the defects -- the switch that does not save FPCR
#      (GCU_FIBER_PLANT_NO_FPCR) or one callee-saved register
#      (NO_CALLEE_SAVED) on aarch64, and MXCSR, the x87 control word or one
#      callee-saved register on the host -- and requires the harness to FAIL
#      at the named check while the unrelated checks still pass.  A gate that has never been seen to fail may be checking nothing.
#
# What it cannot show, and does not claim: the sanitizers do not run under
# qemu-user, so arm64 has no sanitizer gate (the x86-64 gates in
# `make check-fiber-defects` carry that); qemu-user is not real arm64
# hardware, so FPCR behaviour is qemu's; and the image has no C++ cross
# compiler, so this is the C harness and not test-fiber.cpp.
#
# Exit status 0 only if every build passed its checks and every planted
# build failed them.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/targets.sh"

WORK=/work
SRC=$WORK/libs/cutil
B=${B:-/tmp/xarch-fiber}
# $B is removed below.  Refuse the values that would make that dangerous.
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "fiber.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
case "$B" in
  /*) ;;
  *) echo "fiber.sh: B must be an absolute path, got '$B'" >&2; exit 2 ;;
esac
rm -rf "$B"
mkdir -p "$B/include/ghoti.io/cutil"

# The generated header the Makefile writes: the namespace token and version.
# Same text as the Makefile's libver_gen.h rule, for a branch named xarch.
cat > "$B/include/ghoti.io/cutil/libver_gen.h" <<'EOF'
#ifndef GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_NAME ghotiio_cutil_xarch
#define GHOTIIO_CUTIL_VERSION "0.0.0-xarch"
#define GHOTIIO_CUTIL_VERSION_MAJOR 0
#define GHOTIIO_CUTIL_VERSION_MINOR 0
#define GHOTIIO_CUTIL_VERSION_PATCH 0
#endif
EOF

# cutil's flags for a library source (CONVENTIONS.md section 6), so that a
# warning the host compiler does not raise but this one does is a failure.
LIBFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD -I $SRC/include -I $B/include"

status=0
fail() { echo "FAIL: $*"; status=1; }

# build NAME CC [PLANT]  ->  $B/NAME/fiber-check
build() {
  local name=$1 cc=$2 plant=${3:-}
  local d=$B/$name
  mkdir -p "$d"
  local def=""
  [ -n "$plant" ] && def="-DGCU_FIBER_PLANT_$plant"
  for f in fiber allocator error; do
    $cc $LIBFLAGS $def -c "$SRC/src/$f.c" -o "$d/$f.o" || return 1
  done
  $cc -std=gnu17 -O2 -g -Wall -Wextra -Werror -I "$SRC/include" -I "$B/include" \
    "$HERE/fiber-check.c" "$d"/fiber.o "$d"/allocator.o "$d"/error.o \
    -o "$d/fiber-check" -pthread -lm || return 1
}

# run NAME CC-TRIPLE QEMU  ->  exit status of the harness, output kept
run() {
  local name=$1 triple=$2 qemu=$3
  xarch_run "$triple" "$qemu" "$B/$name/fiber-check" > "$B/$name/out.txt" 2>&1
  return $?
}

echo "== machinery"
cc_a=aarch64-linux-gnu-gcc
build aarch64 $cc_a || { echo "FAIL: cannot build for aarch64"; exit 1; }
elf=$(file -b "$B/aarch64/fiber-check")
case "$elf" in
  *aarch64*|*"ARM aarch64"*) echo "   ELF: $elf" ;;
  *) fail "the aarch64 build is not an aarch64 binary: $elf" ;;
esac
if "$B/aarch64/fiber-check" > /dev/null 2>&1; then
  fail "the aarch64 binary ran WITHOUT qemu: it is not foreign, nothing here means anything"
else
  echo "   the aarch64 binary does not run natively (as it must not)"
fi
[ $status -eq 0 ] || exit 1

echo
echo "== aarch64 under qemu-aarch64"
run aarch64 aarch64-linux-gnu qemu-aarch64; rc=$?
cat "$B/aarch64/out.txt"
[ $rc -eq 0 ] || fail "aarch64: $rc check(s) failed"

echo
echo "== x86_64 on the host (the control)"
build x86_64 gcc || { echo "FAIL: cannot build for x86_64"; exit 1; }
run x86_64 x86_64-linux-gnu ""; rc=$?
cat "$B/x86_64/out.txt"
[ $rc -eq 0 ] || fail "x86_64: $rc check(s) failed"

# planted NAME CC TRIPLE QEMU PLANT EXPECT_FAIL_LINE [REQUIRED_OK_LINE...]
# The planted build must fail EXPECT_FAIL_LINE and must still pass every
# REQUIRED_OK_LINE: a planted build that fails everything (it crashed, or did
# not link) has not shown that the check can tell.
planted() {
  local name=$1 cc=$2 triple=$3 qemu=$4 plant=$5 expect=$6
  shift 6
  build "$name" "$cc" "$plant" || { fail "$name: cannot build with $plant planted"; return; }
  run "$name" "$triple" "$qemu"; local rc=$?
  if [ $rc -eq 0 ]; then
    fail "$name: $plant planted and the harness still passed"
    return
  elif ! grep -q "^FAIL $expect" "$B/$name/out.txt"; then
    echo "   (output)"; sed 's/^/   | /' "$B/$name/out.txt"
    fail "$name: $plant planted; failed, but not at '$expect'"
    return
  fi
  local ok_line
  for ok_line in "$@"; do
    if ! grep -q "^ok   $ok_line" "$B/$name/out.txt"; then
      echo "   (output)"; sed 's/^/   | /' "$B/$name/out.txt"
      fail "$name: $plant planted; '$ok_line' should still pass and did not"
      return
    fi
  done
  echo "   $name: $plant planted -> $rc check(s) fail, including '$expect'"
}

echo
echo "== planted defects, each of which must be caught"
RT="round trip: each leg runs once"
CS="callee-saved registers survive"
RND="rounding: the upward fiber keeps its mode"
planted aarch64-no-fpcr $cc_a aarch64-linux-gnu qemu-aarch64 NO_FPCR \
  "$RND" "$RT" "$CS"
planted aarch64-no-callee-saved $cc_a aarch64-linux-gnu qemu-aarch64 NO_CALLEE_SAVED \
  "$CS" "$RT" "$RND"
planted x86_64-no-mxcsr gcc x86_64-linux-gnu "" NO_MXCSR "$RND" "$RT" "$CS"
planted x86_64-no-x87cw gcc x86_64-linux-gnu "" NO_X87CW "$RND" "$RT" "$CS"
planted x86_64-no-callee-saved gcc x86_64-linux-gnu "" NO_CALLEE_SAVED \
  "$CS" "$RT" "$RND"

# The architectures with no switch routine must still build and link, which
# is the whole of the promise to every other xarch build, and must refuse
# cleanly when asked for a fiber.
echo
echo "== the targets with no switch routine: build, link, refuse"
unsupported_one() {
  local triple=$1 cc=$2 qemu=$3 desc=$4
  case "$triple" in x86_64-linux-gnu|aarch64-linux-gnu) return ;; esac
  if ! command -v "$cc" >/dev/null 2>&1; then
    fail "$triple: no compiler"; return 1
  fi
  local d=$B/unsupported-$triple
  mkdir -p "$d"
  local f
  for f in fiber allocator error; do
    $cc $LIBFLAGS -c "$SRC/src/$f.c" -o "$d/$f.o" || { fail "$triple: $f.c does not build"; return 1; }
  done
  $cc -std=gnu17 -O2 -Wall -Wextra -Werror -I "$SRC/include" -I "$B/include" \
    "$HERE/fiber-unsupported.c" "$d"/fiber.o "$d"/allocator.o "$d"/error.o \
    -o "$d/check" || { fail "$triple: does not link"; return 1; }
  local out
  out=$(xarch_run "$triple" "$qemu" "$d/check" 2>&1) || { fail "$triple: $out"; return 1; }
  printf "   %-22s %s\n" "$triple" "$out"
}
xarch_each unsupported_one

echo
if [ $status -eq 0 ]; then
  echo "fiber.sh: PASS - aarch64 passes, each planted defect is caught, the other targets refuse"
else
  echo "fiber.sh: FAIL"
fi
exit $status
