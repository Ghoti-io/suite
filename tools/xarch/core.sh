#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# runtime-core's (or runtime-heap's, as a second argument) own tests for a foreign target, run under
# qemu-user: a 32-bit build and a
# big-endian one.
#
#   core.sh i686         32-bit x86 (4-aligned int64_t, 32-bit time_t and uintptr_t), qemu-i386
#   core.sh s390x        s390x (64-bit, BIG-endian), qemu-s390x: the image of Containerfile.text-be
#   core.sh aarch64      AArch64 (64-bit, little-endian), qemu-aarch64
#
# In the image of Containerfile.text (or text-be for s390x), with the source tree at /work (read-only)
# and a scratch directory at /scratch:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xcore:/scratch:z \
#     ghoti-xarch-text:deb13-gxx14-gtest-main-locales bash /work/suite/tools/xarch/core.sh i686
#
# What it does: checks that a binary of the target is a binary of the target (ELF header, by two readers)
# before it runs one; builds cutil and runtime-core for the target into a prefix of its own in the scratch
# directory (cutil's float.h is made by running its generator under qemu); builds every test program of
# runtime-core and runs each under a time limit (CORE_XARCH_TIMEOUT, default 1800 s). A program that
# passes no tests is a failure, and skips are listed by name. Exit status: 0 only if everything built
# and passed. A death test that re-executes the binary cannot run under qemu-user, so
# GRHEAP_TEST_DEATH_FAST=1 is set for runtime-heap's one. The libraries are engine-independent: nothing
# here emits or runs machine code, so what a foreign target changes is word size, byte order and the C
# library.
set -u
set -o pipefail

ARCH="${1:-}"
case "$ARCH" in
  aarch64) TRIPLE=aarch64-linux-gnu; QEMU=qemu-aarch64; SYSROOT=/usr/aarch64-linux-gnu; WORD=64 ;;
  i686)    TRIPLE=i686-linux-gnu;    QEMU=qemu-i386;    SYSROOT=/usr/i686-linux-gnu;    WORD=32 ;;
  s390x)   TRIPLE=s390x-linux-gnu;   QEMU=qemu-s390x;   SYSROOT=/usr/s390x-linux-gnu;   WORD=64 ;;
  *) echo "usage: core.sh aarch64|i686|s390x" >&2; exit 2 ;;
esac
WORK="${WORK:-/work}"
S="${SCRATCH:-/scratch}/$ARCH"
JOBS="${JOBS:-$(nproc)}"
TMO="${CORE_XARCH_TIMEOUT:-1800}"
P="$S/prefix"
SRC="$S/src"
mkdir -p "$P" "$SRC" "$S/shim"
# The library under test is the last of the stack: runtime-core (the default) or runtime-heap, which
# is built on it ("core.sh i686 runtime-heap").
LIB="${2:-runtime-core}"
case "$LIB" in
  runtime-core) STACK="cutil runtime-core" ;;
  runtime-heap) STACK="cutil runtime-core runtime-heap"
    # runtime-heap is 64-bit only (its design says so, and its header stops a build where a pointer
    # is not eight bytes), so a 32-bit target is refused here rather than failed in the build.
    [ "$ARCH" = i686 ] && { echo "core.sh: SKIPPED (not passed): runtime-heap is 64-bit only by design, so there is nothing to run for i686"; exit 0; } ;;
  *) echo "usage: core.sh aarch64|i686|s390x [runtime-core|runtime-heap]" >&2; exit 2 ;;
esac

fail() { printf '\ncore.sh: FAIL: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }
heading() { printf '\n=== %s ===\n' "$*"; }

for t in ar ranlib nm objdump readelf strip objcopy; do
  ln -sf "$(command -v $TRIPLE-$t)" "$S/shim/$t"
done
export PATH="$S/shim:$PATH"
CROSS=(CC=$TRIPLE-gcc CXX=$TRIPLE-g++)
export PKG_CONFIG_PATH="$P/share/pkgconfig:/opt/$ARCH/lib/pkgconfig"

elf_ok() { # path
  local re fe
  re=$(readelf -h "$1" 2>/dev/null | sed -n 's/^ *Machine: *//p')
  fe=$(file -b "$1")
  case "$ARCH" in
    aarch64) [[ "$re" == *AArch64* && "$fe" == *"ARM aarch64"* ]] ;;
    i686)    [[ "$re" == *"Intel 80386"* && "$fe" == *"Intel i386"* ]] ;;
    s390x)   [[ "$re" == *"IBM S/390"* && "$fe" == *"IBM S/390"* && "$fe" == *"MSB"* ]] ;;
  esac
}

# Bounded run of a target program under qemu with environment words before `--`.
run_env() {
  local -a q=($QEMU -L "$SYSROOT") ev=()
  while [ "$1" != "--" ]; do ev+=(-E "$1"); shift; done
  shift
  timeout -k 10 "$TMO" "${q[@]}" "${ev[@]}" "$@"
}

mk() { local lib="$1"; shift; make -C "$SRC/$lib" "${CROSS[@]}" PREFIX="$P" "$@"; }

heading "Machinery ($ARCH)"
cat > "$S/probe.c" <<'EOP'
#include <stdio.h>
#include <stdint.h>
int main(void) { printf("%u %u\n", (unsigned)sizeof(void *), (unsigned)sizeof(size_t)); return 0; }
EOP
$TRIPLE-gcc -o "$S/probe" "$S/probe.c" || fail "the cross compiler does not compile"
elf_ok "$S/probe" || fail "the probe is not a $ARCH binary"
got=$($QEMU -L "$SYSROOT" "$S/probe") || fail "qemu does not run the $ARCH probe"
[ "$got" = "$((WORD / 8)) $((WORD / 8))" ] || fail "the probe says pointer/size_t are '$got' bytes, not $((WORD / 8))"
if "$S/probe" >/dev/null 2>&1; then fail "the $ARCH probe ran without qemu: it is not foreign"; fi
say "  probe: $ARCH ELF, size_t is $((WORD / 8)) bytes under $QEMU, and does not run without it"

heading "Building the stack for $ARCH into $P"
for lib in $STACK; do
  mkdir -p "$SRC/$lib"
  rsync -a --exclude build --exclude docs --exclude .git --exclude 'tests/fuzz/corpus/*/*' \
    --include='*/' "$WORK/libs/$lib/" "$SRC/$lib/"
done
for lib in $STACK; do
  say "-- $lib"
  if [ "$lib" = cutil ]; then
    apps="$SRC/cutil/build/linux/release/apps"
    inc="$SRC/cutil/build/linux/release/include/ghoti.io/cutil"
    mk cutil build/linux/release/apps/float_identifier >/dev/null 2>&1 || fail "cutil: float_identifier did not build"
    elf_ok "$apps/float_identifier" || fail "float_identifier is not a $ARCH binary"
    f32=$($QEMU -L "$SYSROOT" "$apps/float_identifier" 32) || fail "float_identifier 32 failed"
    f64=$($QEMU -L "$SYSROOT" "$apps/float_identifier" 64) || fail "float_identifier 64 failed"
    mkdir -p "$inc"
    sed "s/FLOAT32/$f32/; s/FLOAT64/$f64/" "$SRC/cutil/src/float.h.template" > "$inc/float.h"
    say "  cutil float.h: $f32 / $f64"
  fi
  mk "$lib" -j"$JOBS" all > "$S/build-$lib.log" 2>&1 || { tail -30 "$S/build-$lib.log" >&2; fail "$lib did not build ($S/build-$lib.log)"; }
  mk "$lib" install >> "$S/build-$lib.log" 2>&1 || { tail -30 "$S/build-$lib.log" >&2; fail "$lib did not install"; }
done
n=0
for so in "$P"/lib/ghoti.io/*.so.*.*.*; do elf_ok "$so" || fail "$so is not a $ARCH object"; n=$((n + 1)); done
[ "$n" -gt 0 ] || fail "the prefix holds no libraries; the check measured nothing"
say "  $n installed libraries, each a $ARCH object"

heading "Building the stack for $ARCH into $P"
for lib in $STACK; do
  mkdir -p "$SRC/$lib"
  rsync -a --exclude build --exclude docs --exclude .git --exclude 'tests/fuzz/corpus/*/*' \
    --include='*/' "$WORK/libs/$lib/" "$SRC/$lib/"
done
for lib in $STACK; do
  say "-- $lib"
  if [ "$lib" = cutil ]; then
    apps="$SRC/cutil/build/linux/release/apps"
    inc="$SRC/cutil/build/linux/release/include/ghoti.io/cutil"
    mk cutil build/linux/release/apps/float_identifier >/dev/null 2>&1 || fail "cutil: float_identifier did not build"
    elf_ok "$apps/float_identifier" || fail "float_identifier is not a $ARCH binary"
    f32=$($QEMU -L "$SYSROOT" "$apps/float_identifier" 32) || fail "float_identifier 32 failed"
    f64=$($QEMU -L "$SYSROOT" "$apps/float_identifier" 64) || fail "float_identifier 64 failed"
    mkdir -p "$inc"
    sed "s/FLOAT32/$f32/; s/FLOAT64/$f64/" "$SRC/cutil/src/float.h.template" > "$inc/float.h"
    say "  cutil float.h: $f32 / $f64"
  fi
  mk "$lib" -j"$JOBS" all > "$S/build-$lib.log" 2>&1 || { tail -30 "$S/build-$lib.log" >&2; fail "$lib did not build ($S/build-$lib.log)"; }
  mk "$lib" install >> "$S/build-$lib.log" 2>&1 || { tail -30 "$S/build-$lib.log" >&2; fail "$lib did not install"; }
done
n=0
for so in "$P"/lib/ghoti.io/*.so.*.*.*; do elf_ok "$so" || fail "$so is not a $ARCH object"; n=$((n + 1)); done
[ "$n" -gt 0 ] || fail "the prefix holds no libraries; the check measured nothing"
say "  $n installed libraries, each a $ARCH object"

heading "Building the test programs"
sel=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -C "$SRC/$LIB" -f Makefile -f - "${CROSS[@]}" PREFIX="$P" print-TEST_NAMES 2>/dev/null | tr ' ' '\n' | sort)
[ -n "$sel" ] || fail "$LIB names no test program"
say "  $(wc -w <<<"$sel") programs"
targets=()
for n in $sel; do targets+=("build/linux/release/apps/$n"); done
mk "$LIB" -k -j"$JOBS" "${targets[@]}" > "$S/build-tests.log" 2>&1 || { grep -a -E " error|Error [0-9]" "$S/build-tests.log" | head -40 >&2; fail "the test programs did not build ($S/build-tests.log)"; }

heading "Running ($ARCH)"
dir="$SRC/$LIB/build/linux/release/apps"
fails=(); total=0; ran=0; skips=()
for n in $sel; do
  bin="$dir/$n"
  [ -x "$bin" ] || { fails+=("$n: not built"); continue; }
  elf_ok "$bin" || fail "$bin is not a $ARCH binary: refusing to run it as one"
  start=$(date +%s)
  out=$(cd "$SRC/$LIB" && run_env LD_LIBRARY_PATH="$P/lib/ghoti.io:$dir" GRHEAP_TEST_DEATH_FAST=1 -- "$bin" 2>&1)
  rc=$?
  secs=$(( $(date +%s) - start ))
  passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1); passed=${passed:-0}
  if [ $rc -ne 0 ]; then
    printf '%s\n' "$out" | tail -30 >&2
    fails+=("$n: exit $rc after ${secs}s")
    printf '  %-30s FAILED (exit %s, %ss)\n' "$n" "$rc" "$secs"
    continue
  fi
  nskip=0
  while IFS= read -r line; do skips+=("$n: ${line#\[  SKIPPED \] }"); nskip=$((nskip + 1)); done < <(printf '%s\n' "$out" | grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$')
  # A program whose every test skips (the hand-built compiled frames are 64-bit native frames, so
  # their tests skip on a 32-bit word size) passed nothing and is not a failure; one that ran
  # nothing at all, skipped or not, is.
  if [ "$passed" -eq 0 ] && [ "$nskip" -eq 0 ]; then fails+=("$n: exit 0 having passed no tests"); printf '  %-30s PASSED NOTHING\n' "$n"; continue; fi
  total=$((total + passed)); ran=$((ran + 1))
  printf '  %-30s %5s passed  %5ss\n' "$n" "$passed" "$secs"
done
say ""
say "$ran programs, $total tests passed on $ARCH; ${#skips[@]} skipped:"
for s in "${skips[@]+"${skips[@]}"}"; do say "  - $s"; done
if [ ${#fails[@]} -gt 0 ]; then
  say ""; say "FAILURES (${#fails[@]}):"
  for f in "${fails[@]}"; do say "  $f"; done
  exit 1
fi
say "core.sh $ARCH: ok"
