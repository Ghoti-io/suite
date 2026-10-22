#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# The Defiant storage libraries and defiant-core (and defiant-test and
# defiant-web where their dependencies build), every test program of each,
# for a foreign target under qemu-user.
#
#   defiant.sh i686      32-bit x86, little-endian (32-bit size_t)
#   defiant.sh s390x     64-bit, BIG-endian
#   defiant.sh powerpc   32-bit, BIG-endian
#   defiant.sh sparc64   64-bit, BIG-endian, strict alignment
#   defiant.sh all       the four at once, in parallel; fails if any does
#
# DEFIANT_XARCH_TARGETS narrows `all`; DEFIANT_XARCH_DEPS and
# DEFIANT_XARCH_LIBS replace the dependency stack and the Defiant libraries
# built; DEFIANT_XARCH_TESTS (an extended regex) chooses test programs. Each
# is for looking at one failure; a gate run leaves them unset.
#
# In the image of Containerfile.defiant, with the workspace at /work
# (read-only) and a scratch directory at /scratch:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xdefiant:/scratch:z \
#     localhost/ghoti-xarch-defiant:sid-gcc16-gtest-bison bash /work/suite/tools/xarch/defiant.sh all
#
# What it does, per target: proves the compiler makes a foreign binary of the
# right word size and byte order, and that the binary does not run without
# qemu; builds the dependency stack and the Defiant libraries for the target
# into a prefix in the scratch directory (cutil's float.h from its generator
# run under qemu; lang-tang without its debugger, and without its JIT when
# runtime-jit does not build); builds every test program of each Defiant
# library and runs each under qemu from the library's directory, so fixtures
# are found, DEFIANT_XARCH_PAR of them at a time.
#
# A library that does not build, or whose dependency did not, is named with
# the reason (the distinct error lines of its log, or the dependency) and its
# tests are not run; that is a failure of the target unless the library is in
# DEFIANT_XARCH_EXPECT_UNBUILT ("lib:reason;lib:reason"), where each named
# library must indeed fail to build, so an expectation that stops being true
# fails too.  defiant-storage-postgres is never built here: it needs libpq for
# the target and a Postgres server, which this image has neither of; that is
# printed as a reason, not run as a pass.
#
# Not run here: the libraries' check-* gates (scripts that plant defects in a
# copy of the source and rebuild; they test the tests, natively, in each
# library's `make test`), and anything that needs a server or a container.
#
# Every failure is named at the end; a program that passes no tests is a
# failure; skips are listed by name.  Exit status: 0 only if everything that
# was expected to build built and passed on every target asked for.
set -u
set -o pipefail

WORK="${WORK:-/work}"
JOBS="${JOBS:-$(nproc)}"
PAR="${DEFIANT_XARCH_PAR:-4}"
TMO="${DEFIANT_XARCH_TIMEOUT:-14400}"
DEPS="${DEFIANT_XARCH_DEPS:-cutil security compress archive unicode chron regex math text ctang runtime-core runtime-heap runtime-debug runtime-jit lang-tang http vendor-sqlite}"
DEFIANT="${DEFIANT_XARCH_LIBS:-defiant-storage defiant-storage-memory defiant-storage-sql defiant-storage-sqlite defiant-core defiant-test defiant-web}"
TARGETS="${DEFIANT_XARCH_TARGETS:-i686 s390x powerpc sparc64}"
TESTS="${DEFIANT_XARCH_TESTS:-.}" # an extended regex of test program names

if [ "${1:-}" = all ]; then
  rc=0
  pids=()
  mkdir -p "${SCRATCH:-/scratch}"
  for a in $TARGETS; do
    bash "$0" "$a" > "${SCRATCH:-/scratch}/defiant-$a.out" 2>&1 &
    pids+=("$!")
  done
  i=0
  for a in $TARGETS; do
    if wait "${pids[$i]}"; then r=ok; else r=FAILED; rc=1; fi
    printf '\n######## %s: %s ########\n' "$a" "$r"
    cat "${SCRATCH:-/scratch}/defiant-$a.out"
    i=$((i + 1))
  done
  [ $rc -eq 0 ] && echo "defiant.sh all: ok" || echo "defiant.sh all: FAILED"
  exit $rc
fi

ARCH="${1:-}"
case "$ARCH" in
  i686)    TRIPLE=i686-linux-gnu;    QEMU=qemu-i386;    WORD=32; ORDER=little ;;
  s390x)   TRIPLE=s390x-linux-gnu;   QEMU=qemu-s390x;   WORD=64; ORDER=big ;;
  powerpc) TRIPLE=powerpc-linux-gnu; QEMU=qemu-ppc;     WORD=32; ORDER=big ;;
  sparc64) TRIPLE=sparc64-linux-gnu; QEMU=qemu-sparc64; WORD=64; ORDER=big ;;
  *) echo "usage: defiant.sh i686|s390x|powerpc|sparc64|all" >&2; exit 2 ;;
esac
SYSROOT=/usr/$TRIPLE
S="${SCRATCH:-/scratch}/$ARCH"
P="$S/prefix"
SRC="$S/src"
mkdir -p "$P" "$SRC" "$S/shim" "$S/logs"

fail() { printf '\ndefiant.sh %s: FAIL: %s\n' "$ARCH" "$*" >&2; exit 1; }
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

mk() { local lib="$1"; shift; make -C "$SRC/$lib" "${CROSS[@]}" PREFIX="$P" "$@"; }

# The dependencies of a library, from the manifest (optional ones dropped).
deps_of() {
  awk -v r="$1" '!/^[[:space:]]*#/ && $1 == r { print $4 }' "$WORK/suite/libraries.txt" \
    | tr ',' '\n' | grep -v '^?' | grep -v '^-$'
}

[ -f "$WORK/suite/libraries.txt" ] || fail "no $WORK/suite/libraries.txt: the manifest says what each library needs"

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
$TRIPLE-g++ --version | head -1
say "  probe: $want under $QEMU, and does not run without it"

heading "Building the stack into $P"
declare -A BUILT WHY
for lib in $DEPS $DEFIANT; do
  mkdir -p "$SRC/$lib"
  rsync -a --delete --exclude build --exclude docs --exclude .git \
    --exclude 'tests/fuzz/corpus/*/*' "$WORK/libs/$lib/" "$SRC/$lib/" \
    || fail "could not copy $lib"
done
LTANG_EXTRA=(WITH_DEBUG=no)
for lib in $DEPS $DEFIANT; do
  missing=
  for d in $(deps_of "$lib"); do
    case "${BUILT[$d]:-}" in
      1) ;;
      0) missing="$missing $d" ;;
      *) missing="$missing $d(not in this run)" ;;
    esac
  done
  if [ "$lib" = lang-tang ]; then
    # ctang is the oracle of lang-tang's tests, not a dependency of the
    # library that core links.
    missing=$(printf '%s\n' $missing | grep -v -E '^ctang(\(.*\))?$' | tr '\n' ' ')
  fi
  if [ "$lib" = lang-tang ] && [ "${BUILT[runtime-jit]:-0}" != 1 ]; then
    # The interpreter-only engine links nothing of runtime-jit.
    missing=$(printf '%s\n' $missing | grep -v -E '^runtime-jit(\(.*\))?$' | tr '\n' ' ')
    LTANG_EXTRA+=(JIT=no)
  fi
  if [ -n "$(printf '%s' "$missing" | tr -d ' ')" ]; then
    BUILT[$lib]=0
    WHY[$lib]="a dependency did not build:$missing"
    say "-- $lib: not built (${WHY[$lib]})"
    continue
  fi
  extra=()
  [ "$lib" = lang-tang ] && extra=("${LTANG_EXTRA[@]}")
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
  pc=$(ls "$P/share/pkgconfig" 2>/dev/null | grep -o 'ghoti.io-lang-tang-[0-9]*' | head -1)
  [ -n "$pc" ] && export LTANG_PC="$pc"
  log="$S/logs/build-$lib.log"
  # -k, and every distinct error rather than the last lines: with
  # -Wfatal-errors each file stops at its first, and one failing file must not
  # hide the others.
  if mk "$lib" -k -j"$JOBS" all "${extra[@]}" > "$log" 2>&1 &&
     mk "$lib" install "${extra[@]}" >> "$log" 2>&1; then
    BUILT[$lib]=1
    say "-- $lib: built ${extra[*]}"
  else
    BUILT[$lib]=0
    errs=$(grep -a -E 'error:|undefined reference|\*\*\* .*Stop\.' "$log" | grep -v '^make.*: \*\*\* \[' | sed 's/^[^ ]*\/src\//src\//' | sort -u)
    WHY[$lib]="did not build ($(printf '%s\n' "$errs" | grep -c .) distinct error lines)"
    say "-- $lib: NOT BUILT: ${WHY[$lib]}"
    printf '%s\n' "$errs" | head -40 | sed 's/^/     /'
  fi
done
for so in "$P"/lib/ghoti.io/*.so.*.*.*; do
  [ -e "$so" ] || continue
  elf_ok "$so" || fail "$so is not a $ARCH object"
done

heading "Building and running the Defiant test programs"
fails=(); skips=(); unbuilt=(); total=0; ran=0
# What is known not to build, per target, with the reason (story S10's run,
# 2026-10-21). Each must still fail to build, or the run fails: an
# expectation that stops being true is news. DEFIANT_XARCH_EXPECT_UNBUILT
# replaces the table.
case "$ARCH" in
  i686) default_expect="defiant-core:runtime-core does not build for 32-bit (src/a/codemeta.c: static assertion GRCORE_CodeLocation's layout is part of the format), so lang-tang does not;defiant-test:needs defiant-core;defiant-web:needs defiant-core" ;;
  powerpc) default_expect="defiant-core:runtime-heap does not build for 32-bit PowerPC (src/heap/heap_internal.h: static assertion weak and verify code assume 8-byte slots), so lang-tang does not;defiant-test:needs defiant-core;defiant-web:needs defiant-core" ;;
  *) default_expect= ;;
esac
expect="${DEFIANT_XARCH_EXPECT_UNBUILT-$default_expect}"
expected_reason() { # lib: the reason it is expected not to build, or nothing
  printf '%s' "$expect" | tr ';' '\n' | awk -F: -v l="$1" '$1 == l { sub(/^[^:]*:/, ""); print; exit }'
}
# Tests that cannot run under qemu-user, each named with its reason, run with
# a filter that names exactly them; the x86-64 control (each library's own
# `make test`) runs them. "lib|program|test|reason".
EXCLUDED=(
  "defiant-core|testInst|Inst.Effective|runs the built examples/effective-config as a child process, and qemu-user cannot exec a foreign binary from a guest without binfmt"
  "defiant-test|testHarness|Site.Readable|execs a separately built snapshot reader (GDEFTEST_READZIP, which make test sets) and reads its ELF header; qemu-user cannot exec a foreign binary from a guest without binfmt"
)
excluded_filter() { # lib test: a gtest negative filter, or nothing
  local e f=
  for e in "${EXCLUDED[@]}"; do
    IFS='|' read -r el et en _ <<<"$e"
    [ "$el" = "$1" ] && [ "$et" = "$2" ] && f="$f:$en"
  done
  [ -n "$f" ] && printf -- '--gtest_filter=-%s' "${f#:}"
}
run_one() { # lib test: run one program, write its verdict file
  local lib="$1" t="$2" apps="$SRC/$1/build/linux/release/apps" out rc secs start passed
  local v="$S/logs/verdict-$lib-$t" filt
  filt=$(excluded_filter "$lib" "$t")
  start=$(date +%s)
  out=$(cd "$SRC/$lib" && env -u GTEST_FILTER timeout -k 10 "$TMO" $QEMU -L "$SYSROOT" \
      -E LD_LIBRARY_PATH="$P/lib/ghoti.io:$apps" "$apps/$t" $filt 2>&1)
  rc=$?
  secs=$(( $(date +%s) - start ))
  printf '%s\n' "$out" > "$S/logs/run-$lib-$t.log"
  passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1)
  printf '%s %s %s %s\n' "$rc" "${passed:-0}" "$secs" "$t" > "$v"
}
# Libraries whose code runs on cutil fibers throughout, which cutil has only
# on x86-64 and arm64 ELF: on these targets their test programs are built but
# not run, and say so. "lib|reason".
FIBER_ONLY=(
  "defiant-web|its server runs each connection on a cutil fiber and its edge fixture drives clients on fibers; every target here lacks them (GCU_FIBER_SUPPORTED is 0)"
)
fiber_only() { local e; for e in "${FIBER_ONLY[@]}"; do [ "${e%%|*}" = "$1" ] && { printf '%s' "${e#*|}"; return 0; }; done; return 1; }
notrun=()
for lib in $DEFIANT; do
  if [ "${BUILT[$lib]:-0}" != 1 ]; then
    r=$(expected_reason "$lib")
    if [ -n "$r" ]; then
      unbuilt+=("$lib: ${WHY[$lib]} (expected: $r)")
    else
      unbuilt+=("$lib: ${WHY[$lib]}")
      fails+=("$lib: not built: ${WHY[$lib]}")
    fi
    continue
  fi
  r=$(expected_reason "$lib")
  [ -n "$r" ] && fails+=("$lib: built, but DEFIANT_XARCH_EXPECT_UNBUILT says it should not ($r)")
  names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -C "$SRC/$lib" -f Makefile -f - "${CROSS[@]}" PREFIX="$P" print-TEST_NAMES 2>/dev/null | tr ' ' '\n' | grep -E "$TESTS" | tr '\n' ' ')
  [ -n "$names" ] || { fails+=("$lib: the Makefile names no test programs"); continue; }
  targets=()
  for t in $names; do targets+=("build/linux/release/apps/$t"); done
  if ! mk "$lib" -k -j"$JOBS" "${targets[@]}" > "$S/logs/build-tests-$lib.log" 2>&1; then
    grep -a -E 'error:|undefined reference' "$S/logs/build-tests-$lib.log" | sed 's/^[^ ]*\/tests\//tests\//' | sort -u | head -40 >&2
  fi
  if r=$(fiber_only "$lib"); then
    built=0
    for t in $names; do [ -x "$SRC/$lib/build/linux/release/apps/$t" ] && built=$((built + 1)) || fails+=("$lib/$t: test program not built"); done
    notrun+=("$lib: $built test programs built, not run: $r")
    say "-- $lib: $built test programs built, not run"
    continue
  fi
  n=0
  for t in $names; do
    bin="$SRC/$lib/build/linux/release/apps/$t"
    if [ ! -x "$bin" ]; then fails+=("$lib/$t: test program not built"); continue; fi
    elf_ok "$bin" || fail "$bin is not a $ARCH binary: refusing to run it as one"
    while [ "$(jobs -rp | wc -l)" -ge "$PAR" ]; do wait -n; done
    run_one "$lib" "$t" &
    n=$((n + 1))
  done
  wait
  say "-- $lib ($n programs)"
  for t in $names; do
    v="$S/logs/verdict-$lib-$t"
    [ -f "$v" ] || continue
    read -r rc passed secs _ < "$v"
    log="$S/logs/run-$lib-$t.log"
    if [ "$rc" -ne 0 ]; then
      grep -E '^\[  FAILED  \]|Failure|Expected|Which is|actual|SKIPPED: |Segmentation|Bus error|Illegal' "$log" | head -30 | sed 's/^/     /' >&2
      fails+=("$lib/$t: exit $rc after ${secs}s")
      printf '  %-28s FAILED (exit %s, %ss)\n' "$t" "$rc" "$secs"
      continue
    fi
    if [ "$passed" -eq 0 ]; then fails+=("$lib/$t: exit 0 having passed no tests"); continue; fi
    total=$((total + passed)); ran=$((ran + 1))
    while IFS= read -r line; do skips+=("$lib/$t: ${line#\[  SKIPPED \] }"); done \
      < <(grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$' "$log")
    printf '  %-28s %5s passed  %6ss\n' "$t" "$passed" "$secs"
  done
done
say "-- defiant-storage-postgres: not built here: it needs libpq built for $TRIPLE and a Postgres server; neither is in this image"

say ""
say "Excluded by name under qemu-user (${#EXCLUDED[@]}):"
for e in "${EXCLUDED[@]}"; do IFS='|' read -r el et en er <<<"$e"; say "  - $el/$et: $en ($er)"; done
say "$ran programs, $total tests passed on $ARCH; ${#skips[@]} skipped:"
for s in "${skips[@]+"${skips[@]}"}"; do say "  - $s"; done
if [ ${#notrun[@]} -gt 0 ]; then
  say ""; say "Built and not run on $ARCH (${#notrun[@]}):"
  for u in "${notrun[@]}"; do say "  $u"; done
fi
if [ ${#unbuilt[@]} -gt 0 ]; then
  say ""; say "Not built on $ARCH (${#unbuilt[@]}):"
  for u in "${unbuilt[@]}"; do say "  $u"; done
fi
if [ ${#fails[@]} -gt 0 ]; then
  say ""; say "FAILURES on $ARCH (${#fails[@]}):"
  for f in "${fails[@]}"; do say "  $f"; done
  exit 1
fi
say "defiant.sh $ARCH: ok"
