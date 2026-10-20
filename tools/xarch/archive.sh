#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# The archive library's whole test suite, and its examples, for a foreign target under qemu-user.
#
#   archive.sh i686      32-bit x86, little-endian (32-bit size_t)
#   archive.sh s390x     64-bit, BIG-endian
#   archive.sh powerpc   32-bit, BIG-endian
#   archive.sh sparc64   64-bit, BIG-endian, strict alignment
#   archive.sh all       each of the four in turn; fails if any does
#
# In the image of Containerfile.archive, with the workspace at /work (read-only) and a scratch
# directory at /scratch:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xarchive:/scratch:z \
#     localhost/ghoti-xarch-archive:sid-gcc16-gtest bash /work/suite/tools/xarch/archive.sh all
#
# What it does, per target: proves the compiler makes a foreign binary of the right word size and
# byte order, and that the binary does not run without qemu; builds cutil, security, compress and
# archive for the target into a prefix in the scratch directory (cutil's float.h from its generator
# run under qemu); builds every archive test program and every example; runs each test program
# under qemu from the library's directory, so the fixtures are found; then writes a tar, a tar.xz,
# a zip and a 7z with the examples and lists each with the others.  Every failure is named at the
# end; a program that passes no tests is a failure; skips are listed by name.  Exit status: 0 only
# if everything built and passed on every target asked for.
set -u
set -o pipefail

WORK="${WORK:-/work}"
JOBS="${JOBS:-$(nproc)}"
TMO="${ARCHIVE_XARCH_TIMEOUT:-3600}"
STACK="cutil security compress archive"

if [ "${1:-}" = all ]; then
  rc=0
  for a in i686 s390x powerpc sparc64; do
    bash "$0" "$a" || rc=1
  done
  [ $rc -eq 0 ] && echo "archive.sh all: ok" || echo "archive.sh all: FAILED"
  exit $rc
fi

ARCH="${1:-}"
case "$ARCH" in
  i686)    TRIPLE=i686-linux-gnu;    QEMU=qemu-i386;    WORD=32; ORDER=little ;;
  s390x)   TRIPLE=s390x-linux-gnu;   QEMU=qemu-s390x;   WORD=64; ORDER=big ;;
  powerpc) TRIPLE=powerpc-linux-gnu; QEMU=qemu-ppc;     WORD=32; ORDER=big ;;
  sparc64) TRIPLE=sparc64-linux-gnu; QEMU=qemu-sparc64; WORD=64; ORDER=big ;;
  *) echo "usage: archive.sh i686|s390x|powerpc|sparc64|all" >&2; exit 2 ;;
esac
SYSROOT=/usr/$TRIPLE
S="${SCRATCH:-/scratch}/$ARCH"
P="$S/prefix"
SRC="$S/src"
mkdir -p "$P" "$SRC" "$S/shim"

fail() { printf '\narchive.sh %s: FAIL: %s\n' "$ARCH" "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }
heading() { printf '\n=== %s (%s) ===\n' "$*" "$ARCH"; }

for t in ar ranlib nm objdump readelf strip objcopy; do
  ln -sf "$(command -v $TRIPLE-$t)" "$S/shim/$t"
done
export PATH="$S/shim:$PATH"
CROSS=(CC=$TRIPLE-gcc CXX=$TRIPLE-g++)
export PKG_CONFIG_PATH="$P/share/pkgconfig:/opt/$ARCH/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$P/share/pkgconfig:/opt/$ARCH/lib/pkgconfig"

elf_ok() { # path: is it an ELF of this target?
  local m
  m=$(readelf -h "$1" 2>/dev/null | sed -n 's/^ *Machine: *//p')
  case "$ARCH" in
    i686)    [[ "$m" == *"Intel 80386"* ]] ;;
    s390x)   [[ "$m" == *"S/390"* ]] ;;
    powerpc) [[ "$m" == *"PowerPC"* && "$m" != *"64"* ]] ;;
    sparc64) [[ "$m" == *"Sparc v9"* || "$m" == *"SPARC v9"* ]] ;;
  esac
}

qrun() { timeout -k 10 "$TMO" $QEMU -L "$SYSROOT" -E LD_LIBRARY_PATH="$P/lib/ghoti.io:$APPS" "$@"; }

mk() { local lib="$1"; shift; make -C "$SRC/$lib" "${CROSS[@]}" PREFIX="$P" "$@"; }

heading "Machinery"
cat > "$S/probe.c" <<'EOP'
#include <stdio.h>
#include <stdint.h>
int main(void) {
  const uint16_t one = 1;
  printf("%u %u %s\n", (unsigned)sizeof(void *), (unsigned)sizeof(size_t),
      *(const uint8_t *)&one ? "little" : "big");
  return 0;
}
EOP
$TRIPLE-gcc -o "$S/probe" "$S/probe.c" || fail "the cross compiler does not compile"
elf_ok "$S/probe" || fail "the probe is not a $ARCH binary"
got=$($QEMU -L "$SYSROOT" "$S/probe") || fail "qemu does not run the probe"
want="$((WORD / 8)) $((WORD / 8)) $ORDER"
[ "$got" = "$want" ] || fail "the probe says '$got', not '$want'"
if "$S/probe" >/dev/null 2>&1; then fail "the probe ran without qemu: it is not foreign"; fi
$TRIPLE-gcc --version | head -1
say "  probe: $want under $QEMU, and does not run without it"

heading "Building the stack into $P"
for lib in $STACK; do
  mkdir -p "$SRC/$lib"
  rsync -a --delete --exclude build --exclude docs --exclude .git \
    --exclude 'tests/fuzz/corpus/*/*' "$WORK/libs/$lib/" "$SRC/$lib/" \
    || fail "could not copy $lib"
done
for lib in $STACK; do
  say "-- $lib"
  if [ "$lib" = cutil ]; then
    capps="$SRC/cutil/build/linux/release/apps"
    inc="$SRC/cutil/build/linux/release/include/ghoti.io/cutil"
    mk cutil build/linux/release/apps/float_identifier >/dev/null 2>&1 || fail "cutil: float_identifier did not build"
    elf_ok "$capps/float_identifier" || fail "float_identifier is not a $ARCH binary"
    f32=$($QEMU -L "$SYSROOT" "$capps/float_identifier" 32) || fail "float_identifier 32 failed"
    f64=$($QEMU -L "$SYSROOT" "$capps/float_identifier" 64) || fail "float_identifier 64 failed"
    mkdir -p "$inc"
    sed "s/FLOAT32/$f32/; s/FLOAT64/$f64/" "$SRC/cutil/src/float.h.template" > "$inc/float.h"
  fi
  # -k, and every distinct error rather than the last thirty lines: with -Wfatal-errors each file stops at
  # its first, and one failing file must not hide the others.
  mk "$lib" -k -j"$JOBS" all > "$S/build-$lib.log" 2>&1 || {
    grep -a -E 'error:' "$S/build-$lib.log" | sort -u >&2
    fail "$lib did not build ($(grep -a -c -E 'error:' "$S/build-$lib.log") error lines)"; }
  mk "$lib" install >> "$S/build-$lib.log" 2>&1 || { tail -30 "$S/build-$lib.log" >&2; fail "$lib did not install"; }
done
n=0
for so in "$P"/lib/ghoti.io/*.so.*.*.*; do elf_ok "$so" || fail "$so is not a $ARCH object"; n=$((n + 1)); done
[ "$n" -ge 4 ] || fail "the prefix holds $n libraries, not the four of the stack"
say "  $n installed libraries, each a $ARCH object"

heading "Building the test programs and examples"
APPS="$SRC/archive/build/linux/release/apps"
names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -C "$SRC/archive" -f Makefile -f - "${CROSS[@]}" PREFIX="$P" print-TEST_NAMES 2>/dev/null)
[ -n "$names" ] || fail "the Makefile names no test programs"
targets=()
for t in $names; do targets+=("build/linux/release/apps/$t"); done
mk archive -k -j"$JOBS" "${targets[@]}" examples > "$S/build-tests.log" 2>&1 \
  || { grep -a -E "error:" "$S/build-tests.log" | sort -u >&2; fail "the test programs or examples did not build"; }
say "  $(wc -w <<<"$names") test programs"

heading "Running the tests"
fails=(); total=0; ran=0; skips=()
for t in $names; do
  bin="$APPS/$t"
  [ -x "$bin" ] || { fails+=("$t: not built"); continue; }
  elf_ok "$bin" || fail "$bin is not a $ARCH binary: refusing to run it as one"
  start=$(date +%s)
  out=$(cd "$SRC/archive" && qrun "$bin" 2>&1)
  rc=$?
  secs=$(( $(date +%s) - start ))
  passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1); passed=${passed:-0}
  if [ $rc -ne 0 ]; then
    printf '%s\n' "$out" | grep -E '^\[  FAILED  \]|Failure|Expected|Which is|actual' | head -40 >&2
    fails+=("$t: exit $rc after ${secs}s")
    printf '  %-24s FAILED (exit %s, %ss)\n' "$t" "$rc" "$secs"
    continue
  fi
  if [ "$passed" -eq 0 ]; then fails+=("$t: exit 0 having passed no tests"); continue; fi
  total=$((total + passed)); ran=$((ran + 1))
  while IFS= read -r line; do skips+=("$t: ${line#\[  SKIPPED \] }"); done \
    < <(printf '%s\n' "$out" | grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$')
  printf '  %-24s %5s passed  %5ss\n' "$t" "$passed" "$secs"
done

heading "Running the examples"
E="$APPS/examples"
X="$S/examples-out"
rm -rf "$X"; mkdir -p "$X"
ex() { # description, then the command; a non-zero exit is a failure
  local what="$1"; shift
  if out=$("$@" 2>&1); then say "  ok   $what"; else fails+=("example: $what"); say "  FAIL $what"; printf '%s\n' "$out" | tail -5; fi
}
ex "tar_write t.tar"          qrun "$E/tar_write" "$X/t.tar"
ex "tar_write --xz t.tar.xz"  qrun "$E/tar_write" --xz "$X/t.tar.xz"
ex "zip_write --deflate"      qrun "$E/zip_write" "$X/z.zip" --deflate
ex "zip_write --stream --xz"  qrun "$E/zip_write" "$X/zs.zip" --stream --xz
ex "sevenz_write"             qrun "$E/sevenz_write" "$X/s.7z"
ex "sevenz_write --non-solid --bzip2" qrun "$E/sevenz_write" "$X/s2.7z" --non-solid --bzip2
for f in t.tar t.tar.xz z.zip zs.zip s.7z s2.7z; do
  ex "tar_list $f" qrun "$E/tar_list" "$X/$f"
done
ex "zip_list z.zip"           qrun "$E/zip_list" "$X/z.zip"
ex "sevenz_list s.7z"         qrun "$E/sevenz_list" "$X/s.7z"
ex "stream_from_file t.tar"   qrun "$E/stream_from_file" "$X/t.tar"
ex "name_findings t.tar"      qrun "$E/name_findings" "$X/t.tar"
ex "archive_update z.zip"     qrun "$E/archive_update" "$X/z.zip" -d notes/run.sh -a extra.txt=hello
ex "archive_update --in-place t.tar" qrun "$E/archive_update" "$X/t.tar" --in-place -a log.txt=line
ex "tar_list t.tar after"     qrun "$E/tar_list" "$X/t.tar"
# newc, not bin.cpio, which the reader refuses by name.
cpio="$SRC/archive/tests/data/arcpio/newc.cpio"
[ -f "$cpio" ] || { fails+=("example: no newc.cpio for arcpio_list"); cpio=; }
qrun_stdin() { local in="$1"; shift; qrun "$@" < "$in"; }
[ -n "$cpio" ] && ex "arcpio_list < $(basename "$cpio")" qrun_stdin "$cpio" "$E/arcpio_list"

say ""
say "$ran programs, $total tests passed on $ARCH; ${#skips[@]} skipped:"
for s in "${skips[@]+"${skips[@]}"}"; do say "  - $s"; done
if [ ${#fails[@]} -gt 0 ]; then
  say ""; say "FAILURES on $ARCH (${#fails[@]}):"
  for f in "${fails[@]}"; do say "  $f"; done
  exit 1
fi
say "archive.sh $ARCH: ok"
