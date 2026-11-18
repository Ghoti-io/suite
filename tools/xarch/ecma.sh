#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# The lang-ecmascript library's unit tests, its test262, differential, re-parse and real-world-corpus gates,
# its fuzz-seed replay and its benchmark smoke, for a foreign target, run under qemu-user: a 32-bit build
# and a big-endian one, and AArch64.
#
#   ecma.sh i686         32-bit x86 (32-bit size_t and uintptr_t), qemu-i386
#   ecma.sh s390x        s390x (64-bit, BIG-endian), qemu-s390x: the image of Containerfile.text-be
#   ecma.sh aarch64      AArch64 (64-bit, little-endian), qemu-aarch64
#
# In the image of Containerfile.text (or text-be for s390x), with the checkouts laid out as the suite's clone.sh makes them at /work (read-only; its
# libs/lang-ecmascript must have its third_party corpora fetched: `make fetch-test262 fetch-corpus`) and a
# scratch directory at /scratch:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xecma:/scratch:z \
#     ghoti-xarch-text:deb13-gxx14-gtest-main-locales bash /work/suite/tools/xarch/ecma.sh i686
#
# What it does: checks that a binary of the target is a binary of the target (ELF header, by two readers)
# before it runs one; builds cutil, unicode, regex, runtime-core and lang-ecmascript for the target into a
# prefix of its own in the scratch directory (cutil's float.h is made by running its generator under qemu);
# builds every unit-test program, the test262 driver, the fuzz replay programs and the benchmarks, and runs
# each test program under a time limit (ECMA_XARCH_TIMEOUT, default 3600 s; GLECMA_TEST_SLOW=1 makes the
# doubling sweeps use their smaller sizes, as under Valgrind); runs the fuzz replay over every seed; runs the
# python gates (check-test262, check-differential, check-reparse, check-corpus) with the target's driver
# under qemu as their program, so the answers are the target's; and runs each benchmark once on its smoke
# workload. A program that passes no tests is a failure, and skips are listed by name. Exit status: 0 only if
# everything built and passed. What the target changes here is word size (size_t, uintptr_t, and the hash
# multiplier of the name sets), byte order (UTF-16 in host order, the layout of structs the tests pin) and
# the C library.
set -u
set -o pipefail

ARCH="${1:-}"
case "$ARCH" in
  aarch64) TRIPLE=aarch64-linux-gnu; QEMU=qemu-aarch64; SYSROOT=/usr/aarch64-linux-gnu; WORD=64 ;;
  i686)    TRIPLE=i686-linux-gnu;    QEMU=qemu-i386;    SYSROOT=/usr/i686-linux-gnu;    WORD=32 ;;
  s390x)   TRIPLE=s390x-linux-gnu;   QEMU=qemu-s390x;   SYSROOT=/usr/s390x-linux-gnu;   WORD=64 ;;
  *) echo "usage: ecma.sh aarch64|i686|s390x" >&2; exit 2 ;;
esac
WORK="${WORK:-/work}"
S="${SCRATCH:-/scratch}/$ARCH"
JOBS="${JOBS:-$(nproc)}"
TMO="${ECMA_XARCH_TIMEOUT:-3600}"
P="$S/prefix"
SRC="$S/src"
mkdir -p "$P" "$SRC" "$S/shim"
STACK="cutil unicode regex runtime-core lang-ecmascript"
LIB=lang-ecmascript

fail() { printf '\necma.sh: FAIL: %s\n' "$*" >&2; exit 1; }
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
[ -d "$WORK/libs/$LIB/third_party/test262" ] && [ -d "$WORK/libs/$LIB/third_party/corpus" ] ||
  fail "$WORK/libs/$LIB/third_party has no test262 or corpus: run 'make fetch-test262 fetch-corpus' in the library"

heading "Building the stack for $ARCH into $P"
for lib in $STACK; do
  mkdir -p "$SRC/$lib"
  # The sources, the seeds and the data; the fetched corpora are linked, not copied (they are large and read-only).
  rsync -a --exclude build --exclude docs --exclude .git --exclude third_party \
    --include='*/' "$WORK/libs/$lib/" "$SRC/$lib/"
done
ln -sfn "$WORK/libs/$LIB/third_party" "$SRC/$LIB/third_party"
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
names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -C "$SRC/$LIB" -f Makefile -f - "${CROSS[@]}" PREFIX="$P" print-TEST_NAMES 2>/dev/null)
sel=$(tr ' ' '\n' <<<"$names" | grep -E '^test' | sort)
[ -n "$sel" ] || fail "no test program found"
say "  $(wc -w <<<"$sel") programs: $(tr '\n' ' ' <<<"$sel")"
apps_rel=build/linux/release/apps
targets=()
for n in $sel; do targets+=("$apps_rel/$n"); done
targets+=("$apps_rel/t262/t262_driver")
for f in tokenizer source parser goals alloc; do targets+=("$apps_rel/fuzz/replay_$f"); done
targets+=("$apps_rel/bench/bench" "$apps_rel/bench/bench_corpus")
mk "$LIB" -k -j"$JOBS" "${targets[@]}" > "$S/build-tests.log" 2>&1 || { grep -a -E " error|Error [0-9]" "$S/build-tests.log" | head -40 >&2; fail "the test programs did not build ($S/build-tests.log)"; }
dir="$SRC/$LIB/$apps_rel"

heading "Running the unit tests ($ARCH)"
fails=(); total=0; ran=0; skips=()
for n in $sel; do
  bin="$dir/$n"
  [ -x "$bin" ] || { fails+=("$n: not built"); continue; }
  elf_ok "$bin" || fail "$bin is not a $ARCH binary: refusing to run it as one"
  start=$(date +%s)
  out=$(cd "$SRC/$LIB" && run_env LD_LIBRARY_PATH="$P/lib/ghoti.io:$dir" GLECMA_TEST_SLOW=1 -- "$bin" 2>&1)
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

heading "Fuzz replay, the benchmark smoke and the gates ($ARCH)"
mapfile -t inputs < <(cd "$SRC/$LIB" && find tests/fuzz/corpus -type f | sort)
[ ${#inputs[@]} -gt 0 ] || fail "no fuzz seed to replay"
for f in tokenizer source parser goals alloc; do
  bin="$dir/fuzz/replay_$f"
  [ -x "$bin" ] || { fails+=("replay_$f: not built"); continue; }
  elf_ok "$bin" || fail "$bin is not a $ARCH binary"
  out=$(cd "$SRC/$LIB" && run_env LD_LIBRARY_PATH="$P/lib/ghoti.io:$dir" -- "$bin" "${inputs[@]}" 2>&1)
  rc=$?
  last=$(printf '%s\n' "$out" | tail -1)
  if [ $rc -ne 0 ]; then printf '%s\n' "$out" | tail -8 >&2; fails+=("replay_$f: exit $rc"); fi
  printf '  replay_%-10s %s\n' "$f" "$last"
done
for b in bench bench_corpus; do
  bin="$dir/bench/$b"
  out=$(cd "$SRC/$LIB" && run_env LD_LIBRARY_PATH="$P/lib/ghoti.io:$dir" -- "$bin" --smoke 2>&1)
  rc=$?
  [ $rc -eq 0 ] || { printf '%s\n' "$out" | tail -8 >&2; fails+=("$b --smoke: exit $rc"); }
  printf '  %-18s smoke exit %s\n' "$b" "$rc"
done
# The python gates take the target's driver, which they run as a program: a wrapper that runs it under qemu.
cat > "$S/driver" <<EOW
#!/bin/sh
exec $QEMU -L $SYSROOT -E LD_LIBRARY_PATH=$P/lib/ghoti.io:$dir $dir/t262/t262_driver "\$@"
EOW
chmod +x "$S/driver"
elf_ok "$dir/t262/t262_driver" || fail "the driver is not a $ARCH binary"
T262=$(cat "$SRC/$LIB/tools/test262/TEST262_COMMIT")
cd "$SRC/$LIB" || fail "no library directory"
common=(--root "third_party/test262/$T262" --sums tools/test262/SHA256SUMS
        --classification tests/data-test262-classification.tsv --tags tests/data-test262-tags.tsv --driver "$S/driver")
gate() { # name command...
  local name=$1; shift
  local start=$(date +%s) out rc
  out=$("$@" 2>&1); rc=$?
  printf '%s\n' "$out" | tail -3 | sed "s/^/    /"
  [ $rc -eq 0 ] || fails+=("$name: exit $rc")
  printf '  %-22s exit %s, %ss\n' "$name" "$rc" "$(( $(date +%s) - start ))"
}
gate check-test262 python3 tools/check-test262.py check --root "third_party/test262/$T262" --sums tools/test262/SHA256SUMS \
  --classification tests/data-test262-classification.tsv --tags tests/data-test262-tags.tsv \
  --pending tests/data-test262-pending.tsv --driver "$S/driver" --features ""
gate check-differential python3 tools/check-differential.py "${common[@]}"
gate check-reparse python3 tools/check-reparse.py "${common[@]}"
gate check-corpus python3 tools/check-corpus.py --driver "$S/driver"

# The native stack one level of nesting costs on this target, found by the library's own measurement tool.
heading "Stack per level of nesting ($ARCH)"
$TRIPLE-gcc -std=c17 -O2 -I include -I src -I build/linux/release/generated \
  $(pkg-config --cflags ghoti.io-cutil-0 ghoti.io-runtime-core-0 ghoti.io-unicode-0 ghoti.io-regex-0) tools/measure-stack.c \
  -Wl,--whole-archive "$dir/libghoti.io-lang-ecmascript-0.a" -Wl,--no-whole-archive \
  $(pkg-config --libs ghoti.io-runtime-core-0 ghoti.io-unicode-0 ghoti.io-regex-0 ghoti.io-cutil-0) -lm -pthread \
  -o "$S/measure-stack" > "$S/measure-stack.build.log" 2>&1 || { tail -20 "$S/measure-stack.build.log" >&2; fails+=("measure-stack did not build"); }
if [ -x "$S/measure-stack" ]; then
  elf_ok "$S/measure-stack" || fail "measure-stack is not a $ARCH binary"
  run_env LD_LIBRARY_PATH="$P/lib/ghoti.io" -- "$S/measure-stack" > "$S/measure-stack.txt" 2>&1 || fails+=("measure-stack failed")
  cat "$S/measure-stack.txt"
fi

if [ ${#fails[@]} -gt 0 ]; then
  say ""; say "FAILURES (${#fails[@]}):"
  for f in "${fails[@]}"; do say "  $f"; done
  exit 1
fi
say "ecma.sh $ARCH: ok"
