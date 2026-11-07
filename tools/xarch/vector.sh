#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# The vector library's cross-target gate: its unit tests, and the lowering gate against its committed goldens, for a
# foreign target, run under qemu-user.
#
#   vector.sh aarch64      AArch64 (64-bit, little-endian), qemu-aarch64
#   vector.sh i686         32-bit x86 (32-bit size_t), qemu-i386
#   vector.sh s390x        s390x (64-bit, BIG-endian), qemu-s390x: the image of Containerfile.text-be
#
# In the image of Containerfile.text (aarch64, i686) or Containerfile.text-be (s390x), with the checkout mounted at /work
# (read-only) and a scratch directory at /scratch:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xvector:/scratch:z \
#     ghoti-xarch-text:deb13-gxx14-gtest-main-locales bash /work/suite/tools/xarch/vector.sh i686
#
# What it does: checks that a binary of the target is a binary of the target (ELF header, by two readers) before it
# runs one; builds the libraries vector depends on, and vector, for the target into a prefix of its own in the scratch
# directory (cutil's float.h is made by running its generator under qemu); runs every unit test program of vector
# (VECTOR_XARCH_TESTS selects by name, default all) and the lowering gate (tests/lower/lower_gate.c) over every
# fixture, which compares the list's hash, the raster's hash, the diagnostics and the list's dump with the goldens
# committed from the x86-64 build: a number that came out differently on this target is a failing golden.  The gate is
# then shown to fail: a golden with one hex digit changed must be refused by the same binary.  A program that passes no
# tests is a failure.  Exit status: 0 only if everything built and passed.
set -u
set -o pipefail

ARCH="${1:-}"
case "$ARCH" in
  aarch64) TRIPLE=aarch64-linux-gnu; QEMU=qemu-aarch64; SYSROOT=/usr/aarch64-linux-gnu; WORD=64; ENDIAN=little ;;
  i686)    TRIPLE=i686-linux-gnu;    QEMU=qemu-i386;    SYSROOT=/usr/i686-linux-gnu;    WORD=32; ENDIAN=little ;;
  s390x)   TRIPLE=s390x-linux-gnu;   QEMU=qemu-s390x;   SYSROOT=/usr/s390x-linux-gnu;   WORD=64; ENDIAN=big ;;
  *) echo "usage: vector.sh aarch64|i686|s390x" >&2; exit 2 ;;
esac
WORK="${WORK:-/work}"
S="${SCRATCH:-/scratch}/vector-$ARCH"
JOBS="${JOBS:-$(nproc)}"
FILTER="${VECTOR_XARCH_TESTS:-^test}"
TMO="${VECTOR_XARCH_TIMEOUT:-3600}"
P="$S/prefix"
SRC="$S/src"
mkdir -p "$P" "$SRC" "$S/shim"
# suite/libraries.txt order, up to vector's own dependencies.
STACK="cutil security unicode chron regex math text compress color image shape-geometry shape-scene shape-cpu vector"

fail() { printf '\nvector.sh: FAIL: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }
heading() { printf '\n=== %s ===\n' "$*"; }

for t in ar ranlib nm objdump readelf strip objcopy; do
  ln -sf "$(command -v $TRIPLE-$t)" "$S/shim/$t"
done
export PATH="$S/shim:$PATH"
# A 32-bit x86 target computes a double in SSE, not in the x87 stack, whose 80-bit registers round a sum twice (the flags the
# other cross gates take from xarch_fp_cflags in targets.sh); the makefile adds them only for a native 32-bit build.
FPFLAGS=""
[ "$ARCH" = i686 ] && FPFLAGS="-msse2 -mfpmath=sse"
CROSS=(CC=$TRIPLE-gcc CXX=$TRIPLE-g++ "ARCH_FP_FLAGS=$FPFLAGS")
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

# Bounded run of a target program under qemu.
run_q() { timeout -k 10 "$TMO" "$QEMU" -L "$SYSROOT" -E "LD_LIBRARY_PATH=$P/lib/ghoti.io:$LIBDIR" "$@"; }

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
say "  probe: $ARCH ELF, $ENDIAN-endian, size_t is $((WORD / 8)) bytes under $QEMU, and does not run without it"

heading "Building the stack for $ARCH into $P"
for lib in $STACK; do
  mkdir -p "$SRC/$lib"
  rsync -a --exclude build --exclude docs --exclude .git --exclude 'tests/fuzz/corpus/*/*' \
    --include='*/' "$WORK/libs/$lib/" "$SRC/$lib/"
  if [ "$lib" = text ]; then
    (cd "$WORK/libs/text" && find tests/fuzz/corpus -name '*.seed' -print0 | tar cf - --null -T -) | tar xf - -C "$SRC/text"
  fi
done
LIBDIR=""
for lib in $STACK; do
  [ "$lib" = vector ] && break
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

heading "Building vector's test programs and the lowering gate"
names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -C "$SRC/vector" -f Makefile -f - "${CROSS[@]}" PREFIX="$P" print-TEST_NAMES 2>/dev/null)
sel=$(tr ' ' '\n' <<<"$names" | grep -E "$FILTER" | sort)
[ -n "$sel" ] || fail "no test program matches $FILTER"
say "  $(wc -w <<<"$sel") programs: $(tr '\n' ' ' <<<"$sel")"
APPS="build/linux/release/apps"
targets=("$APPS/lower/lower_gate")
for n in $sel; do targets+=("$APPS/$n"); done
mk vector -k -j"$JOBS" "${targets[@]}" > "$S/build-tests.log" 2>&1 || { grep -a -E " error|Error [0-9]" "$S/build-tests.log" | head -40 >&2; fail "the test programs did not build ($S/build-tests.log)"; }
LIBDIR="$SRC/vector/$APPS"

heading "Running the unit tests ($ARCH)"
fails=(); total=0; ran=0; skips=()
for n in $sel; do
  bin="$LIBDIR/$n"
  [ -x "$bin" ] || { fails+=("$n: not built"); continue; }
  elf_ok "$bin" || fail "$bin is not a $ARCH binary: refusing to run it as one"
  start=$(date +%s)
  out=$(cd "$SRC/vector" && run_q "$bin" 2>&1)
  rc=$?
  secs=$(( $(date +%s) - start ))
  passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1); passed=${passed:-0}
  if [ $rc -ne 0 ]; then
    printf '%s\n' "$out" | tail -30 >&2
    fails+=("$n: exit $rc after ${secs}s")
    printf '  %-28s FAILED (exit %s, %ss)\n' "$n" "$rc" "$secs"
    continue
  fi
  if [ "$passed" -eq 0 ]; then fails+=("$n: exit 0 having passed no tests"); printf '  %-28s PASSED NOTHING\n' "$n"; continue; fi
  total=$((total + passed)); ran=$((ran + 1))
  while IFS= read -r line; do skips+=("$n: ${line#\[  SKIPPED \] }"); done < <(printf '%s\n' "$out" | grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$')
  printf '  %-28s %5s passed  %5ss\n' "$n" "$passed" "$secs"
done
say ""
say "$ran programs, $total tests passed on $ARCH; ${#skips[@]} skipped:"
for s in "${skips[@]+"${skips[@]}"}"; do say "  - $s"; done

heading "The lowering gate against the committed goldens ($ARCH)"
gate="$LIBDIR/lower/lower_gate"
[ -x "$gate" ] || fail "lower_gate was not built"
elf_ok "$gate" || fail "lower_gate is not a $ARCH binary"
fixtures=$(cd "$SRC/vector" && ls tests/data/svg-lower/*.svg | sort)
nfix=$(wc -w <<<"$fixtures")
[ "$nfix" -gt 0 ] || fail "no fixtures"
if out=$(cd "$SRC/vector" && run_q "$gate" --golden tests/golden/lower $fixtures 2>&1); then
  say "  ok    $nfix fixtures: lists equal, rasters equal, goldens match on $ARCH"
  printf '%s\n' "$out" | tail -2 | sed 's/^/        /'
else
  printf '%s\n' "$out" | tail -20 >&2
  fails+=("lower_gate: a golden differs on $ARCH")
fi
# The gate shown to fail: one hex digit of a golden hash changed, in a copy, must be refused by the same binary.
rm -rf "$S/planted"; mkdir -p "$S/planted"; cp -r "$SRC/vector/tests/golden/lower" "$S/planted/lower"
victim=$(ls "$S/planted/lower"/*.txt | head -1)
python3 - "$victim" <<'EOP' || fail "could not plant a changed golden"
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r'[0-9a-f]{16}', s)
assert m, "no hash in the golden"
h = m.group(0)
s = s[:m.start()] + ('0' if h[0] != '0' else '1') + h[1:] + s[m.end():]
open(p, 'w').write(s)
EOP
if (cd "$SRC/vector" && run_q "$gate" --golden "$S/planted/lower" $fixtures > "$S/planted.out" 2>&1); then
  fails+=("lower_gate passed with a changed golden ($(basename "$victim")): it checks nothing on $ARCH")
else
  say "  ok    a golden with one digit changed ($(basename "$victim")) is refused"
fi

if [ ${#fails[@]} -gt 0 ]; then
  say ""; say "FAILURES (${#fails[@]}):"
  for f in "${fails[@]}"; do say "  $f"; done
  exit 1
fi
say "vector.sh $ARCH: ok"
