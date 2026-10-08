#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with the scratch tree at /w.  Builds, installs and tests one
# of the runtime stack's libraries for win64 with its real Makefile, and prints
# one SUMMARY line the driver (m1-run.sh) reads.
#
#   m1-lib.sh deps          gtest, then cutil unicode chron regex text ctang
#   m1-lib.sh lib <name>    all, install, test, examples, then the counts
#
# Logs are /w/logs/<name>.<target>.log.  Exit status is 0 only when `make test`
# passed, every test program passed on its own, and no example failed.
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
    printf 'Name: %s\nDescription: %s\nVersion: 1.16.0\n%sLibs: -L%s/lib -l%s\nCflags: -I%s/include\n' \
      $m $m "$([ $m = gtest_main ] && echo 'Requires: gtest
')" $P $m $P > $P/share/pkgconfig/$m.pc
  done
}

if [ "${1:-}" = deps ]; then
  build_gtest || { echo "m1-lib: gtest did not build" >&2; exit 2; }
  # source directory:installed DLL (ctang installs as "tang")
  for d in cutil:cutil unicode:unicode chron:chron regex:regex text:text ctang:tang; do
    src=${d%%:*}; dll=$P/bin/libghoti.io-${d##*:}-0.dll
    [ -f $dll ] && { echo "dep $src: present"; continue; }
    ( cd /w/$src && make -j4 PREFIX=$P > /w/logs/$src.build.log 2>&1 && make install PREFIX=$P > /w/logs/$src.install.log 2>&1 ) \
      && echo "dep $src: built" || { echo "dep $src: FAILED (see /w/logs/$src.build.log)" >&2; exit 2; }
  done
  exit 0
fi

[ "${1:-}" = lib ] && l=${2:?library} || { echo "usage: m1-lib.sh deps | lib <name>" >&2; exit 2; }
cd /w/$l || exit 2
run() { # run <target>...: -k so that one failure does not hide the rest
  for t in "$@"; do
    timeout 3000 make -k -j4 $t PREFIX=$P > /w/logs/$l.$t.log 2>&1; eval "rc_$t=$?"
  done
}
rc_all=0 rc_install=0 rc_test=0 rc_examples=0
run all install test examples
APPS=/w/$l/build/win64/release/apps

# Every test program by itself, so that a program that crashed shows up as a
# program and not as a truncated `make test`.  Counts come from gtest's own
# summary; SKIPPED is reported separately from PASSED.
tot=0; pass=0; skip=0; bad=0; progs=0
for t in $APPS/test*.exe; do
  [ -e "$t" ] || continue
  progs=$((progs + 1))
  (cd $APPS && timeout 1200 ./$(basename $t) --gtest_brief=1 > /w/logs/$l.$(basename $t .exe).out 2>&1); rc=$?
  out=$(tr -d '\r' < /w/logs/$l.$(basename $t .exe).out)
  n=$(echo "$out" | sed -n 's/^\[==========\] \([0-9]*\) tests\? from.*/\1/p' | tail -1)
  p=$(echo "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) tests\?\..*/\1/p' | tail -1)
  s=$(echo "$out" | sed -n 's/^\[  SKIPPED \] \([0-9]*\) tests\?\..*/\1/p' | tail -1)
  tot=$((tot + ${n:-0})); pass=$((pass + ${p:-0})); skip=$((skip + ${s:-0}))
  if [ $rc -ne 0 ] || [ -z "$n" ]; then
    bad=$((bad + 1)); echo "TEST PROGRAM FAILED: $(basename $t) rc=$rc"; echo "$out" | grep -vi fontconfig | tail -n 12
  fi
done

# Examples and the benchmark smoke, one by one: exit 77 is "skipped".
ex_ran=0; ex_skip=0; ex_bad=0
for e in $APPS/examples/*.exe $APPS/bench/*.exe; do
  [ -e "$e" ] || continue
  args=; case $(basename $e .exe) in web_server) args=--self-test ;; bench) args=--smoke ;; esac
  (cd $APPS && timeout 300 $e $args > /w/logs/$l.$(basename $e .exe).out 2>&1); rc=$?
  case $rc in 0) ex_ran=$((ex_ran + 1)) ;; 77) ex_skip=$((ex_skip + 1)) ;;
    *) ex_bad=$((ex_bad + 1)); echo "EXAMPLE FAILED: $(basename $e) rc=$rc" ;; esac
done

warn=$(cat /w/logs/$l.all.log /w/logs/$l.test.log 2>/dev/null | grep -c 'warning:')
status=OK
{ [ $rc_all -ne 0 ] || [ $rc_install -ne 0 ] || [ $rc_test -ne 0 ] || [ $rc_examples -ne 0 ] || [ $bad -ne 0 ] || [ $ex_bad -ne 0 ] || [ $progs -eq 0 ] || [ $warn -ne 0 ]; } && status=FAIL
echo "SUMMARY $l status=$status warnings=$warn make(all=$rc_all install=$rc_install test=$rc_test examples=$rc_examples) programs=$progs tests=$tot passed=$pass skipped=$skip bad_programs=$bad examples(ran=$ex_ran skipped=$ex_skip failed=$ex_bad)"
[ $status = OK ]
