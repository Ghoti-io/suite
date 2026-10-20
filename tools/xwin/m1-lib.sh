#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with the scratch tree at /w.  Builds, installs and tests one
# of the runtime stack's libraries for win64 with its real Makefile, and prints
# one SUMMARY line the driver (m1-run.sh) reads.
#
#   m1-lib.sh deps          gtest, then cutil security unicode chron regex text ctang, or
#                           the space-separated source:dll pairs in $M1_DEPS
#   m1-lib.sh lib <name>    all, install, test, examples, then the counts
#
# Logs are /w/logs/<name>.<target>.log.  Exit status is 0 only when `make test`
# passed, every test program passed on its own, no example failed, and the
# tests that were skipped are exactly the ones m1-skips.txt names for the
# library (an unlisted skip fails; so does a listed test that no longer skips,
# because the count is asserted both ways).  runtime-heap also gets a second
# build with RELOCATE=yes in a prefix of its own: every test program run with
# GRHEAP_RELOCATE=1 (what `make test-relocate` runs) and the relocation gates.
set -u
mkdir -p /w/logs
P=$WPREFIX

build_gtest() {
  [ -f $P/lib/libgtest.a ] && return 0
  mkdir -p /w/gt-build && cd /w/gt-build || return 1
  local G=/gt/googletest
  g++ -std=c++20 -O1 -I$G -I$G/include -c $G/src/gtest-all.cc -o gtest-all.o &&
  g++ -std=c++20 -O1 -I$G -I$G/include -c $G/src/gtest_main.cc -o gtest_main.o &&
  ar rcs libgtest.a gtest-all.o && ar rcs libgtest_main.a gtest_main.o || return 1
  mkdir -p $P/lib $P/include $P/share/pkgconfig
  cp libgtest*.a $P/lib/ && cp -r $G/include/gtest $P/include/
  for m in gtest gtest_main; do
    # (The Requires line is its own line: a newline inside $( ) is stripped, which glued "Libs:" onto "gtest" and made a
    # gtest_main.pc that no Makefile asking for both could read - found by text, the first library whose tests link gtest_main.)
    printf 'Name: %s\nDescription: %s\nVersion: 1.16.0\n%s\nLibs: -L%s/lib -l%s\nCflags: -I%s/include\n' \
      $m $m "$([ $m = gtest_main ] && echo 'Requires: gtest')" $P $m $P > $P/share/pkgconfig/$m.pc
  done
}

if [ "${1:-}" = deps ]; then
  build_gtest || { echo "m1-lib: gtest did not build" >&2; exit 2; }
  # source directory:installed DLL (ctang installs as "tang")
  for d in ${M1_DEPS:-cutil:cutil security:security unicode:unicode chron:chron regex:regex text:text ctang:tang}; do
    src=${d%%:*}; dll=$P/bin/libghoti.io-${d##*:}-0.dll
    [ -f $dll ] && { echo "dep $src: present"; continue; }
    ( cd /w/$src && make -j4 PREFIX=$P > /w/logs/$src.build.log 2>&1 && make install PREFIX=$P > /w/logs/$src.install.log 2>&1 ) \
      && echo "dep $src: built" || { echo "dep $src: FAILED (see /w/logs/$src.build.log)" >&2; exit 2; }
  done
  exit 0
fi

[ "${1:-}" = lib ] && l=${2:?library} || { echo "usage: m1-lib.sh deps | lib <name>" >&2; exit 2; }
cd /w/$l || exit 2

# The WAST gate is also run directly below, outside its Makefile. Keep both
# invocations on the same pinned corpus and pass paths through Wine's Z: mapping
# of the container filesystem, which the Windows binary can open.
wast_env=()
if [ "$l" = lang-wasm ]; then
  spec_commit=$(cat /w/runtime-jit/tools/wasm/WASM_SPEC_COMMIT) || exit 2
  spec_dir="/w/runtime-jit/third_party/wasm-spec/$spec_commit"
  sh /w/runtime-jit/tools/wasm/fetch.sh > /w/logs/lang-wasm.wast-fetch.log 2>&1 || {
    tail -n 30 /w/logs/lang-wasm.wast-fetch.log >&2
    echo "m1-lib: pinned WebAssembly spec fetch failed" >&2
    exit 2
  }
  (cd "$spec_dir" && sha256sum -c /w/runtime-jit/tools/wasm/SHA256SUMS) \
    > /w/logs/lang-wasm.wast-verify.log 2>&1 || {
    tail -n 30 /w/logs/lang-wasm.wast-verify.log >&2
    echo "m1-lib: pinned WebAssembly spec verification failed" >&2
    exit 2
  }
  [ -f "$spec_dir/test/LICENSE" ] || {
    echo "m1-lib: pinned WebAssembly spec corpus is incomplete" >&2
    exit 2
  }
  for data_file in data-wast-classification.tsv data-wast-assertion-skips.tsv \
    data-wast-jit-rules.tsv; do
    [ -f "/w/lang-wasm/tests/$data_file" ] || {
      echo "m1-lib: lang-wasm WAST input $data_file is missing" >&2
      exit 2
    }
  done
  wine_path() { printf 'Z:%s\n' "$1" | sed 's#/#\\#g'; }
  wast_env=(
    GLWASM_WAST_CORPUS_ROOT="$(wine_path "$spec_dir/test")"
    GLWASM_WAST_CLASSIFICATION="$(wine_path /w/lang-wasm/tests/data-wast-classification.tsv)"
    GLWASM_WAST_ASSERTION_SKIPS="$(wine_path /w/lang-wasm/tests/data-wast-assertion-skips.tsv)"
    GLWASM_WAST_JIT_RULES="$(wine_path /w/lang-wasm/tests/data-wast-jit-rules.tsv)"
    GLWASM_WAST_SPEC_COMMIT="$spec_commit"
  )
fi
run() { # run <target>...: -k so that one failure does not hide the rest
  for t in "$@"; do
    if [ "$l/$t" = lang-wasm/test ]; then
      timeout 3000 make -k -j4 "$t" PREFIX="$P" "${wast_env[@]}" > /w/logs/$l.$t.log 2>&1
    else
      timeout 3000 make -k -j4 "$t" PREFIX="$P" > /w/logs/$l.$t.log 2>&1
    fi
    eval "rc_$t=$?"
  done
}
rc_all=0 rc_install=0 rc_test=0 rc_examples=0
run all install test examples
APPS=/w/$l/build/win64/release/apps

# Every test program by itself, so that a program that crashed shows up as a
# program and not as a truncated `make test`.  Counts come from gtest's own
# summary; SKIPPED is reported separately from PASSED.
tot=0; pass=0; skip=0; bad=0; progs=0
: > /w/logs/$l.skipped.actual
for t in $APPS/test*.exe; do
  [ -e "$t" ] || continue
  progs=$((progs + 1))
  if [ "$l/$(basename "$t")" = lang-wasm/testSpec_suite.exe ]; then
    (cd "$APPS" && env "${wast_env[@]}" timeout 1200 "./$(basename "$t")" \
      > "/w/logs/$l.$(basename "$t" .exe).out" 2>&1)
  else
    (cd "$APPS" && timeout 1200 "./$(basename "$t")" \
      > "/w/logs/$l.$(basename "$t" .exe).out" 2>&1)
  fi
  rc=$?
  out=$(tr -d '\r' < /w/logs/$l.$(basename $t .exe).out)
  n=$(echo "$out" | sed -n 's/^\[==========\] \([0-9]*\) tests\? from.*/\1/p' | tail -1)
  p=$(echo "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) tests\?\..*/\1/p' | tail -1)
  s=$(echo "$out" | sed -n 's/^\[  SKIPPED \] \([0-9]*\) tests\?[.,].*/\1/p' | tail -1)
  tot=$((tot + ${n:-0})); pass=$((pass + ${p:-0})); skip=$((skip + ${s:-0}))
  # The tests this program skipped, by name (the summary at the end of gtest's output lists them).
  echo "$out" | grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$' | sed 's/^\[  SKIPPED \] //' \
    | sed "s|^|$(basename $t .exe) |" >> /w/logs/$l.skipped.actual
  if [ $rc -ne 0 ] || [ -z "$n" ]; then
    bad=$((bad + 1)); echo "TEST PROGRAM FAILED: $(basename $t) rc=$rc"; echo "$out" | grep -vi fontconfig | tail -n 12
  fi
done

# Examples and the benchmark smoke, one by one: exit 77 is "skipped".
ex_ran=0; ex_skip=0; ex_bad=0
# archive's examples take paths, so they cannot run bare: each writer runs first, and every reader
# then lists what the writers produced.  arcpio_list reads standard input.
ax() { # ax <log tag> <example> [args...]
  local tag=$1 name=$2; shift 2
  (cd $APPS && timeout 300 ./examples/$name.exe "$@" > /w/logs/$l.ex-$tag.out 2>&1); local rc=$?
  [ $rc -eq 0 ] && ex_ran=$((ex_ran + 1)) || { ex_bad=$((ex_bad + 1)); echo "EXAMPLE FAILED: $name $* rc=$rc"; tail -n 5 /w/logs/$l.ex-$tag.out; }
}
if [ "$l" = archive ]; then
  rm -f $APPS/ex-*
  ax w-tar tar_write ex-t.tar
  ax w-txz tar_write --xz ex-t.tar.xz
  ax w-zip zip_write ex-z.zip --deflate
  ax w-zips zip_write ex-zs.zip --stream --xz
  ax w-7z sevenz_write ex-s.7z
  for f in ex-t.tar ex-t.tar.xz ex-z.zip ex-zs.zip ex-s.7z; do ax "l-$f" tar_list $f; done
  ax l-zip zip_list ex-z.zip
  ax l-7z sevenz_list ex-s.7z
  ax l-stream stream_from_file ex-t.tar
  ax l-names name_findings ex-t.tar
  (cd $APPS && timeout 300 ./examples/arcpio_list.exe < /w/archive/tests/data/arcpio/newc.cpio > /w/logs/$l.ex-cpio.out 2>&1) \
    && ex_ran=$((ex_ran + 1)) || { ex_bad=$((ex_bad + 1)); echo "EXAMPLE FAILED: arcpio_list"; }
fi
for e in $APPS/examples/*.exe $APPS/bench/*.exe; do
  [ "$l" = archive ] && break
  [ -e "$e" ] || continue
  args=; case $(basename $e .exe) in web_server) args=--self-test ;; bench) args=--smoke ;; esac
  (cd $APPS && timeout 300 $e $args > /w/logs/$l.$(basename $e .exe).out 2>&1); rc=$?
  case $rc in 0) ex_ran=$((ex_ran + 1)) ;; 77) ex_skip=$((ex_skip + 1)) ;;
    *) ex_bad=$((ex_bad + 1)); echo "EXAMPLE FAILED: $(basename $e) rc=$rc" ;; esac
done

# Every skip is named and counted.  m1-skips.txt: "<library> <program> <Suite.Name> <reason...>".
skip_bad=0
grep -E "^$l " /tools/xwin/m1-skips.txt | awk '{print $2, $3}' | sort > /w/logs/$l.skipped.expected
sort /w/logs/$l.skipped.actual > /w/logs/$l.skipped.sorted
while read -r line; do
  [ -n "$line" ] || continue
  echo "SKIPPED NOT LISTED in m1-skips.txt: $l $line"; skip_bad=1
done < <(comm -13 /w/logs/$l.skipped.expected /w/logs/$l.skipped.sorted)
while read -r line; do
  [ -n "$line" ] || continue
  echo "LISTED AS SKIPPED but it ran (or its program is gone): $l $line"; skip_bad=1
done < <(comm -23 /w/logs/$l.skipped.expected /w/logs/$l.skipped.sorted)
skip_named=$(wc -l < /w/logs/$l.skipped.sorted)
[ "$skip_named" -eq "$skip" ] || { echo "the skips counted ($skip) are not the skips named ($skip_named)"; skip_bad=1; }

# runtime-heap again with RELOCATE=yes, in a prefix of its own.
reloc_bad=0; reloc_progs=0; reloc_tests=0; reloc_gates=skipped
if [ "$l" = runtime-heap ]; then
  RP=/w/prefix-reloc
  rm -rf $RP; mkdir -p $RP; cp -a $P/. $RP/
  sed -i "s#$P#$RP#g" $RP/share/pkgconfig/*.pc
  ( export PKG_CONFIG_PATH=$RP/share/pkgconfig
    MK="make -j4 PREFIX=$RP RELOCATE=yes"
    $MK all install > /w/logs/$l.reloc.build.log 2>&1 && $MK test-relocate > /w/logs/$l.reloc.test.log 2>&1 ) \
    && echo "runtime-heap RELOCATE=yes: built, test-relocate passed" \
    || { echo "runtime-heap RELOCATE=yes: build or test-relocate FAILED (see /w/logs/$l.reloc.*.log)"; tail -n 12 /w/logs/$l.reloc.test.log 2>/dev/null; reloc_bad=1; }
  reloc_tests=$(tr -d '\r' < /w/logs/$l.reloc.test.log | sed -n 's/^\[  PASSED  \] \([0-9]*\) tests\?\..*/\1/p' | awk '{s+=$1} END {print s+0}')
  reloc_progs=$(grep -c '^### Relocating ' /w/logs/$l.reloc.test.log)
  [ "$reloc_progs" -gt 0 ] || { echo "no test program ran relocating"; reloc_bad=1; }
  ( export PKG_CONFIG_PATH=$RP/share/pkgconfig
    make -j4 PREFIX=$RP RELOCATE=yes check-relocation-present check-relocation-gates > /w/logs/$l.reloc.gates.log 2>&1 )
  if grep -q 'check-relocation-gates: all [0-9]* checks behaved' /w/logs/$l.reloc.gates.log; then
    reloc_gates=$(grep -o 'all [0-9]* checks behaved' /w/logs/$l.reloc.gates.log | head -1 | tr ' ' '_')
  else
    echo "check-relocation-gates did not report that every check behaved"; tail -n 12 /w/logs/$l.reloc.gates.log; reloc_bad=1; reloc_gates=FAILED
  fi
fi

# The Win64 Lang-Wasm gate also runs its debugger differential against the
# RELOCATE=yes heap built in the runtime-heap phase. Keep that prefix first in
# pkg-config lookup and retain the regular prefix for runtime-jit/lang-wasm.
debug_relocate=skipped
if [ "$l" = lang-wasm ]; then
  RP=/w/prefix-reloc
  ( export PKG_CONFIG_PATH=$RP/share/pkgconfig:$P/share/pkgconfig
    export WINEPATH='Z:\w\prefix-reloc\bin;Z:\w\prefix\bin'
    make -j4 PREFIX=$P RELOCATE=yes test-debug-relocate \
      > /w/logs/$l.debug-relocate.log 2>&1 ) \
    && debug_relocate=passed \
    || { echo "lang-wasm RELOCATE=yes debugger differential FAILED (see /w/logs/$l.debug-relocate.log)"; tail -n 16 /w/logs/$l.debug-relocate.log; reloc_bad=1; debug_relocate=FAILED; }
fi

warn=$(cat /w/logs/$l.all.log /w/logs/$l.test.log 2>/dev/null | grep -c 'warning:')
status=OK
{ [ $rc_all -ne 0 ] || [ $rc_install -ne 0 ] || [ $rc_test -ne 0 ] || [ $rc_examples -ne 0 ] || [ $bad -ne 0 ] || [ $ex_bad -ne 0 ] || [ $progs -eq 0 ] || [ $warn -ne 0 ] || [ $skip_bad -ne 0 ] || [ $reloc_bad -ne 0 ]; } && status=FAIL
echo "SUMMARY $l status=$status warnings=$warn make(all=$rc_all install=$rc_install test=$rc_test examples=$rc_examples) programs=$progs tests=$tot passed=$pass skipped=$skip(named=$skip_named) bad_programs=$bad examples(ran=$ex_ran skipped=$ex_skip failed=$ex_bad)$([ "$l" = runtime-heap ] && echo " relocating(programs=$reloc_progs tests=$reloc_tests gates=$reloc_gates)")$([ "$l" = lang-wasm ] && echo " debugger_relocation=$debug_relocate")"
[ $status = OK ]
