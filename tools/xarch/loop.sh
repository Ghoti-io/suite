#!/bin/bash
#
# Builds cutil's event loop for aarch64 and runs the architecture checks under
# qemu-aarch64 (story 2 of the Defiant milestone, notes/cutil/event-loop.md).
#
# Run it in the ghoti-xarch image, from the workspace root:
#
#   podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:deb13 \
#     bash /work/suite/tools/xarch/loop.sh
#
# What it does, in order:
#
#   1. Checks the machinery.  An aarch64 binary is aarch64 by its ELF header,
#      it does NOT run on this host without qemu, and under qemu it runs.  A
#      cross-architecture result is worthless if the "foreign" binary was
#      quietly the host's, and that failure is silent.
#   2. Builds loop.c, socket.c, fiber.c and what they need, with cutil's own
#      flags and -Werror, and the C harness suite/tools/xarch/loop-check.c for
#      aarch64, and runs it under qemu-user: the epoll arm's system calls
#      (epoll_create1, eventfd, accept4, send, recvfrom), timers, the wake from
#      another thread, the cancel rule and a fiber resumed by the loop.  It
#      builds the same harness for the host as the control: a harness that
#      fails on the host fails for a reason that is not the architecture.
#   3. Plants the defects of `make check-loop-defects` -- a post that does not
#      wake the loop (GCU_LOOP_PLANT_NO_WAKE), a cancel that reports before it
#      has let go of the buffer (CANCEL_EARLY_RELEASE) and a timer queue
#      ordered by start (TIMER_ORDER) -- on aarch64 and on the host, and
#      requires the harness to FAIL at the named check while the unrelated
#      checks still pass.  A gate that has never been seen to fail may be
#      checking nothing.
#   4. Builds the arm kqueue targets get (-DGCU_LOOP_FORCE_UNSUPPORTED) and
#      requires it to link and refuse, which is the only way to reach it on a
#      machine with epoll.
#   5. Builds and runs the harness for the other targets in the matrix, which
#      are Linux too, so that a 32-bit or big-endian ABI is looked at as well:
#      address conversion, struct layout and the 64-bit clock arithmetic.
#
# What it cannot show, and does not claim: the sanitizers do not run under
# qemu-user, so arm64 has no sanitizer gate (the x86-64 gates in `make test`
# carry that); qemu-user is not real arm64 hardware, and its epoll is a
# translation of the host's; and the image has no C++ cross compiler, so this
# is the C harness and not test-loop.cpp.
#
# Exit status 0 only if every build passed its checks and every planted build
# failed them.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/targets.sh"

WORK=/work
SRC=$WORK/libs/cutil
B=${B:-/tmp/xarch-loop}
# $B is removed below.  Refuse the values that would make that dangerous.
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "loop.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
case "$B" in
  /*) ;;
  *) echo "loop.sh: B must be an absolute path, got '$B'" >&2; exit 2 ;;
esac
rm -rf "$B"
mkdir -p "$B/include/ghoti.io/cutil"

# The generated header the Makefile writes: the namespace token and version.
# Same text as the Makefile's libver_gen.h rule, for a branch named xarch.
cat > "$B/include/ghoti.io/cutil/libver_gen.h" <<'EOT'
#ifndef GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_NAME ghotiio_cutil_xarch
#define GHOTIIO_CUTIL_VERSION "0.0.0-xarch"
#define GHOTIIO_CUTIL_VERSION_MAJOR 0
#define GHOTIIO_CUTIL_VERSION_MINOR 0
#define GHOTIIO_CUTIL_VERSION_PATCH 0
#endif
EOT

# cutil's flags for a library source (CONVENTIONS.md section 6), so that a
# warning the host compiler does not raise but this one does is a failure.
LIBFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD -I $SRC/include -I $B/include"
LIBSRC="loop socket fiber allocator error"

status=0
fail() { echo "FAIL: $*"; status=1; }

# build NAME CC [DEFINE...]  ->  $B/NAME/loop-check
build() {
  local name=$1 cc=$2
  shift 2
  local d=$B/$name
  mkdir -p "$d"
  local f
  for f in $LIBSRC; do
    $cc $LIBFLAGS "$@" -c "$SRC/src/$f.c" -o "$d/$f.o" || return 1
  done
  local objs=""
  for f in $LIBSRC; do objs="$objs $d/$f.o"; done
  $cc -std=gnu17 -O2 -g -Wall -Wextra -Werror -I "$SRC/include" -I "$B/include" \
    "$HERE/loop-check.c" $objs -o "$d/loop-check" -pthread -lm || return 1
}

# run NAME CC-TRIPLE QEMU  ->  exit status of the harness, output kept
run() {
  local name=$1 triple=$2 qemu=$3
  xarch_run "$triple" "$qemu" "$B/$name/loop-check" > "$B/$name/out.txt" 2>&1
  return $?
}

echo "== machinery"
cc_a=aarch64-linux-gnu-gcc
build aarch64 $cc_a || { echo "FAIL: cannot build for aarch64"; exit 1; }
elf=$(file -b "$B/aarch64/loop-check")
case "$elf" in
  *aarch64*|*"ARM aarch64"*) echo "   ELF: $elf" ;;
  *) fail "the aarch64 build is not an aarch64 binary: $elf" ;;
esac
if "$B/aarch64/loop-check" > /dev/null 2>&1; then
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
grep -q '^ok ' "$B/aarch64/out.txt" || fail "aarch64: no check printed ok"

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
  build "$name" "$cc" "-DGCU_LOOP_PLANT_$plant" || { fail "$name: cannot build with $plant planted"; return; }
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
ECHO="tcp: write and read complete once, on the loop thread"
TIMERS="timers of 30, 10 and 20 ms fire in the order 10, 20, 30"
POST="a post from another thread wakes a waiting loop"
CANCEL1="cancel: the completion does not arrive before cancel returns"
CANCEL2="cancel: one completion, CANCELLED"
planted aarch64-no-wake $cc_a aarch64-linux-gnu qemu-aarch64 NO_WAKE \
  "$POST" "$ECHO" "$TIMERS" "$CANCEL1"
planted aarch64-cancel-early $cc_a aarch64-linux-gnu qemu-aarch64 CANCEL_EARLY_RELEASE \
  "$CANCEL1" "$ECHO" "$TIMERS"
planted aarch64-timer-order $cc_a aarch64-linux-gnu qemu-aarch64 TIMER_ORDER \
  "$TIMERS" "$ECHO" "$POST" "$CANCEL1"
planted x86_64-no-wake gcc x86_64-linux-gnu "" NO_WAKE "$POST" "$ECHO" "$TIMERS"
planted x86_64-cancel-early gcc x86_64-linux-gnu "" CANCEL_EARLY_RELEASE \
  "$CANCEL1" "$ECHO" "$TIMERS"
planted x86_64-timer-order gcc x86_64-linux-gnu "" TIMER_ORDER \
  "$TIMERS" "$ECHO" "$POST"

echo
echo "== the arm kqueue targets get: build, link, refuse"
d=$B/unsupported
mkdir -p "$d"
objs=""
for f in $LIBSRC; do
  gcc $LIBFLAGS -DGCU_LOOP_FORCE_UNSUPPORTED -c "$SRC/src/$f.c" -o "$d/$f.o" \
    || { fail "unsupported: $f.c does not build"; }
  objs="$objs $d/$f.o"
done
if gcc -std=gnu17 -O2 -Wall -Wextra -Werror -DGCU_LOOP_FORCE_UNSUPPORTED \
     -I "$SRC/include" -I "$B/include" \
     "$HERE/loop-unsupported.c" $objs -o "$d/check" -pthread; then
  out=$("$d/check" 2>&1) && echo "   $out" || { echo "$out"; fail "unsupported arm: $out"; }
else
  fail "unsupported arm does not link"
fi

echo
echo "== the other targets in the matrix: build and run"
other_one() {
  local triple=$1 cc=$2 qemu=$3 desc=$4
  case "$triple" in x86_64-linux-gnu|aarch64-linux-gnu) return ;; esac
  if ! command -v "$cc" >/dev/null 2>&1; then
    fail "$triple: no compiler"; return
  fi
  local name=other-$triple
  if ! build "$name" "$cc"; then
    fail "$triple: does not build"; return
  fi
  run "$name" "$triple" "$qemu"; local rc=$?
  local oks fails skips
  oks=$(grep -c '^ok ' "$B/$name/out.txt")
  fails=$(grep -c '^FAIL ' "$B/$name/out.txt")
  skips=$(grep -c '^skip ' "$B/$name/out.txt")
  printf "   %-22s %s ok, %s fail, %s skipped (%s)\n" "$triple" "$oks" "$fails" "$skips" "$desc"
  if [ $rc -ne 0 ]; then
    sed 's/^/   | /' "$B/$name/out.txt"
    fail "$triple: $rc check(s) failed"
  fi
}
# The tee is a pipeline, so the callback's status does not reach this shell.
# The failures are in the transcript.
xarch_each other_one 2>&1 | tee "$B/other.txt"
grep -q '^FAIL' "$B/other.txt" && status=1

echo
if [ $status -eq 0 ]; then
  echo "loop.sh: PASS - aarch64 passes, each planted defect is caught, the other targets agree"
else
  echo "loop.sh: FAIL"
fi
exit $status
