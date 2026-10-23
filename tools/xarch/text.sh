#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# The text library's Phase 3 test programs (XML, HTML, Encoding Standard, markup tree) for a
# foreign target, run under qemu-user.
#
#   text.sh aarch64      AArch64 (64-bit, little-endian), qemu-aarch64
#   text.sh i686         32-bit x86 (32-bit size_t), qemu-i386
#
# In the image of Containerfile.text, with the workspace at /work (read-only) and a scratch
# directory at /scratch:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xtext:/scratch:z \
#     ghoti-xarch-text:deb13-gxx14-gtest-main bash /work/suite/tools/xarch/text.sh i686
#
# What it does: checks that a binary of the target is a binary of the target (ELF header, by two
# readers) before it runs one; builds cutil, unicode, chron, regex, security, math and text for the target into a
# prefix of its own in the scratch directory (cutil's float.h is made by running its generator under
# qemu); builds every test program whose name matches $TEXT_XARCH_TESTS (default: Html, Xml, Xhtml,
# Encoding, Markup, Quadratic, Css) and runs each with GTEXT_TEST_FULL=$TEXT_XARCH_FULL (default 0), a
# time limit each (TEXT_XARCH_TIMEOUT, default 3600 s).  Every failure is reported at the end with the
# program's name; a program that passes no tests is a failure; skips are listed by name.  On i686 it
# also requires that the 32-bit source-map limit test (HtmlSourceMapLimit) ran and did not skip, and on
# AArch64 that it skipped (64-bit size_t: the limit is unreachable).  Exit status: 0 only if everything
# built and passed.
set -u
set -o pipefail

ARCH="${1:-}"
case "$ARCH" in
  aarch64) TRIPLE=aarch64-linux-gnu; QEMU=qemu-aarch64; SYSROOT=/usr/aarch64-linux-gnu; WORD=64 ;;
  i686)    TRIPLE=i686-linux-gnu;    QEMU=qemu-i386;    SYSROOT=/usr/i686-linux-gnu;    WORD=32 ;;
  *) echo "usage: text.sh aarch64|i686" >&2; exit 2 ;;
esac
WORK="${WORK:-/work}"
S="${SCRATCH:-/scratch}/$ARCH"
JOBS="${JOBS:-$(nproc)}"
FILTER="${TEXT_XARCH_TESTS:-^test(Html|Xml|Xhtml|Encoding|Markup|Quadratic|Css)}"
FULL="${TEXT_XARCH_FULL:-0}"
TMO="${TEXT_XARCH_TIMEOUT:-3600}"
P="$S/prefix"
SRC="$S/src"
mkdir -p "$P" "$SRC" "$S/shim"
STACK="cutil unicode chron regex security math text"

fail() { printf '\ntext.sh: FAIL: %s\n' "$*" >&2; exit 1; }
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
  if [ "$lib" = text ]; then
    # the seeds the replay tests read (the exclude above keeps the machine-grown corpora out)
    (cd "$WORK/libs/text" && find tests/fuzz/corpus -name '*.seed' -print0 | tar cf - --null -T -) | tar xf - -C "$SRC/text"
  fi
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
names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -C "$SRC/text" -f Makefile -f - "${CROSS[@]}" PREFIX="$P" print-TEST_NAMES 2>/dev/null)
sel=$(tr ' ' '\n' <<<"$names" | grep -E "$FILTER" | sort)
[ -n "$sel" ] || fail "no test program matches $FILTER"
say "  $(wc -w <<<"$sel") programs: $(tr '\n' ' ' <<<"$sel")"
targets=()
for n in $sel; do targets+=("build/linux/release/apps/$n"); done
mk text -k -j"$JOBS" "${targets[@]}" > "$S/build-tests.log" 2>&1 || { grep -a -E " error|Error [0-9]" "$S/build-tests.log" | head -40 >&2; fail "the test programs did not build ($S/build-tests.log)"; }

heading "Running ($ARCH, GTEXT_TEST_FULL=${FULL}, GTEXT_TEST_SLOWDOWN=${TEXT_XARCH_SLOWDOWN:-40})"
dir="$SRC/text/build/linux/release/apps"
# A target under emulation cannot run localedef (the shell a test starts is not of the target), and the CSS number tests need a locale whose
# decimal separator is a comma and fail, not skip, without one (tests/test-css-oom.cpp). The locale is made here, with the host's own
# localedef, and handed to the tests as LOCPATH: the files are the same for any target of the same byte order.
LOC="$S/locale"
mkdir -p "$LOC"
if command -v localedef >/dev/null 2>&1 && [ ! -e "$LOC/de_DE.UTF-8" ]; then
  env -u LD_PRELOAD -u LD_LIBRARY_PATH localedef -i de_DE -f UTF-8 "$LOC/de_DE.UTF-8" >/dev/null 2>&1 || say "text.sh: localedef could not make de_DE.UTF-8; the tests that need a comma locale will fail"
fi
fails=(); total=0; ran=0; skips=()
limit_ran=0; limit_skipped=0
for n in $sel; do
  bin="$dir/$n"
  [ -x "$bin" ] || { fails+=("$n: not built"); continue; }
  elf_ok "$bin" || fail "$bin is not a $ARCH binary: refusing to run it as one"
  start=$(date +%s)
  # GTEXT_TEST_FULL is read by its presence, so it is set only when asked for; the emulator's slowness is declared to the one
  # test that reads a clock (testEncodingFuzzSeeds).
  envs=(LD_LIBRARY_PATH="$P/lib/ghoti.io:$dir" GTEXT_TEST_SLOWDOWN="${TEXT_XARCH_SLOWDOWN:-40}" LOCPATH="$LOC")
  [ "$FULL" = 1 ] && envs+=(GTEXT_TEST_FULL=1)
  out=$(cd "$SRC/text" && run_env "${envs[@]}" -- "$bin" 2>&1)
  rc=$?
  secs=$(( $(date +%s) - start ))
  passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1); passed=${passed:-0}
  if [ $rc -ne 0 ]; then
    printf '%s\n' "$out" | tail -30 >&2
    fails+=("$n: exit $rc after ${secs}s")
    printf '  %-30s FAILED (exit %s, %ss)\n' "$n" "$rc" "$secs"
    continue
  fi
  if [ "$passed" -eq 0 ]; then fails+=("$n: exit 0 having passed no tests"); printf '  %-30s PASSED NOTHING\n' "$n"; continue; fi
  total=$((total + passed)); ran=$((ran + 1))
  while IFS= read -r line; do skips+=("$n: ${line#\[  SKIPPED \] }"); done < <(printf '%s\n' "$out" | grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$')
  printf '%s\n' "$out" | grep -qE '^\[       OK \] HtmlSourceMapLimit\.' && limit_ran=1
  printf '  %-30s %5s passed  %5ss\n' "$n" "$passed" "$secs"
done
say ""
say "$ran programs, $total tests passed on $ARCH; ${#skips[@]} skipped:"
for s in "${skips[@]+"${skips[@]}"}"; do say "  - $s"; done
printf '%s\n' "${skips[@]+"${skips[@]}"}" | grep -q 'HtmlSourceMapLimit\.' && limit_skipped=1
if [ "$WORD" = 32 ]; then
  [ "$limit_ran" = 1 ] && [ "$limit_skipped" = 0 ] || fails+=("HtmlSourceMapLimit did not run (and pass) on a 32-bit size_t")
else
  [ "$limit_skipped" = 1 ] || fails+=("HtmlSourceMapLimit did not skip on a 64-bit size_t")
fi
if [ ${#fails[@]} -gt 0 ]; then
  say ""; say "FAILURES (${#fails[@]}):"
  for f in "${fails[@]}"; do say "  $f"; done
  exit 1
fi
say "text.sh $ARCH: ok"
