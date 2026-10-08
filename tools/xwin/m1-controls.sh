#!/bin/bash
# Runs INSIDE the container after m1-run.sh has built the libraries (through
# m1-win.sh).  A Windows fix whose test has never been seen to fail may be
# testing nothing, so each of these puts one fix back to the way it was in a
# COPY of the built tree (cp -a keeps the mtimes, so only what the change
# touches is rebuilt; the real trees are never edited, which is the only safe
# way to restore) and requires the suite to notice.
#
#   clock   runtime-core waits on CLOCK_MONOTONIC on Windows again: winpthreads
#           refuses it (EINVAL), port_create reads that as out of memory, and
#           testRequest and testProfile must fail.
#   stack   tang.exe and testCompile.exe linked without --stack: they get the
#           linker's default reserve, and a 10,000-deep tree must kill them.
#   crlf    tang.exe without the _setmode calls: stdout is a text stream, a
#           printed newline is CR LF, and the byte checks of cli-test.sh must
#           fail.
#   win64-5, win64-6, win64-7, win64-30 to win64-37   runtime-jit's Windows
#           backend with one of its planted defects built into a scratch copy
#           (GRJIT_TEST_PLANT_BUG; the list is runtime-jit's tools/win64-plants.txt):
#           a callee-saved register used, no outgoing area (shadow space), the
#           unwind table never registered (5 to 7), and the calls, tail calls and
#           natives of story 7b: a pair read from rax:rdx, a native's area without
#           shadow space, a tail copy through rdi, an adapter whose unwind
#           information has a frame register, a callee that does not pop, only
#           one of the two RUNTIME_FUNCTIONs registered, the walk start stored
#           after a native call, a callable frame without its outgoing area. The
#           test that RUNS the code must fail on the planted executable and pass
#           on the real one (CONTROLS_ONLY="30 33" runs only those).
#
# Prints one CONTROL line each, then `CONTROLS ok` only if every control FAILED
# the suite as it must.  A control that passes is the bad outcome.
set -u
P=$WPREFIX
C=/w/ctl
rm -rf $C; mkdir -p $C
bad=0

verdict() { # verdict <name> <expected: fail> <rc> <detail>
  if [ "$3" -ne 0 ]; then echo "CONTROL $1: caught (rc=$3) $4"
  else echo "CONTROL $1: NOT CAUGHT, the suite passed with the fix reverted"; bad=1; fi
}

if [ -z "${CONTROLS_ONLY:-}" ]; then
# ---- clock ---------------------------------------------------------------
cp -a /w/runtime-core $C/runtime-core && cd $C/runtime-core || exit 2
python3 - <<'E'
p = 'src/b/cond_clock_internal.h'
s = open(p).read()
a = s.replace('#ifdef _WIN32\n#define GRCORE_COND_CLOCK CLOCK_REALTIME', '#if 0\n#define GRCORE_COND_CLOCK CLOCK_REALTIME')
b = a.replace('#ifndef _WIN32\n  rc =', '#if 1\n  rc =')
assert a != s and b != a, 'the clock control matched nothing'
open(p, 'w').write(b)
E
touch src/b/cond_clock_internal.h
make -j4 build/win64/release/apps/testRequest.exe build/win64/release/apps/testProfile.exe PREFIX=$P > $C/clock.build.log 2>&1 \
  || { echo "CONTROL clock: the planted build did not build"; bad=1; }
r=0
for t in testRequest testProfile; do
  (cd build/win64/release/apps && ./$t.exe --gtest_brief=1 2>&1 | tr -d '\r' | grep -E '^\[  (PASSED|FAILED)  \] [0-9]+ tests?' | head -n 2 | tr '\n' ' ' > $C/clock.$t.txt; exit ${PIPESTATUS[0]}) || r=1
done
verdict clock fail $r "$(cat $C/clock.testRequest.txt) | $(cat $C/clock.testProfile.txt)"

# ---- stack and crlf share a copy of the built lang-tang ----------------------
cp -a /w/lang-tang $C/lang-tang && cd $C/lang-tang || exit 2
A=build/win64/release/apps
relink_without_stack() { # <exe>: its real link line, with the --stack flag taken out
  rm -f $A/$1
  cmd=$(make -n $A/$1 PREFIX=$P 2>/dev/null | grep -- "-o $A/$1" | tail -n 1)
  [ -n "$cmd" ] && case "$cmd" in *--stack,16777216*) ;; *) cmd= ;; esac
  [ -n "$cmd" ] || { echo "CONTROL stack: could not find the link line of $1 with --stack in it"; bad=1; return 1; }
  eval "${cmd//-Wl,--stack,16777216/}" > $C/stack.$1.log 2>&1 || { echo "CONTROL stack: $1 did not relink"; bad=1; return 1; }
}
relink_without_stack tang.exe && {
  (sh tests/cli-test.sh $A/tang.exe 2>&1 | tr -d '\r' > $C/stack.cli.txt)
  if grep -q 'CLI check(s) failed' $C/stack.cli.txt; then r=1; else r=0; fi   # the failure line is the catch
  verdict stack fail $r "tang.exe without --stack: $(grep -c '^  FAIL' $C/stack.cli.txt) CLI checks failed, e.g. $(grep -m1 '^  FAIL' $C/stack.cli.txt | cut -c1-70)"
}
relink_without_stack testCompile.exe && {
  (cd $A && ./testCompile.exe --gtest_brief=1 > $C/stack.testCompile.txt 2>&1); rc=$?
  verdict stack-unit fail $rc "testCompile.exe without --stack exited $rc"
}

# tang.exe back to the real flags, then without the binary-mode calls.
rm -f $A/tang.exe
python3 - <<'E'
p = 'src/tang.c'
s = open(p).read()
t = s.replace('_setmode(_fileno(stdin), _O_BINARY);', '').replace('_setmode(_fileno(stdout), _O_BINARY);', '').replace('_setmode(_fileno(stderr), _O_BINARY);', '')
assert t != s, 'the crlf control matched nothing'
open(p, 'w').write(t)
E
touch src/tang.c
make $A/tang.exe PREFIX=$P > $C/crlf.build.log 2>&1 || { echo "CONTROL crlf: the planted build did not build"; bad=1; }
(sh tests/cli-test.sh $A/tang.exe 2>&1 | tr -d '\r' > $C/crlf.cli.txt)
if grep -q 'CLI check(s) failed' $C/crlf.cli.txt; then r=1; else r=0; fi
verdict crlf fail $r "tang.exe in text mode: $(grep -c '^  FAIL' $C/crlf.cli.txt) CLI checks failed, e.g. $(grep -m1 '^  FAIL' $C/crlf.cli.txt | cut -c1-70)"

fi

# ---- the Windows backend's planted defects --------------------------------------
# The list is runtime-jit's own, tools/win64-plants.txt: id | GRJIT_TEST_PLANT_BUG | test program | gtest filter |
# what is planted.  Each runs on the real executable first (the control), then on a tree of its own built with
# the macro, and must fail by an assertion that names what it saw.
cp -a /w/runtime-jit $C/runtime-jit && cd $C/runtime-jit || exit 2
CA=build/win64/release/apps
planted() { # planted <id> <macro> <exe> <filter> <what>
  local n=$1 macro=$2 exe=$3 filter=$4 what=$5 tree=build/win64/release-plant-$1
  (cd $CA && ./$exe.exe --gtest_brief=1 --gtest_filter="$filter" 2>&1 | tr -d '\r' > $C/win64-$n.control.txt; exit ${PIPESTATUS[0]}); local crc=$?
  local ran; ran=$(grep -c '^\[       OK \]\|^\[  PASSED  \]' $C/win64-$n.control.txt)
  if [ $crc -ne 0 ] || [ "$ran" -eq 0 ]; then
    echo "CONTROL win64-$n: the control (the real executable, $exe $filter) did not pass: rc=$crc"; bad=1; return
  fi
  if ! make $tree/apps/$exe.exe BUILD_DIR=$tree EXTRA_CFLAGS="-DGRJIT_TEST_PLANT_BUG=$macro" PREFIX=$P > $C/win64-$n.build.log 2>&1; then
    echo "CONTROL win64-$n: the planted build did not build"; bad=1; return
  fi
  (cd $tree/apps && ./$exe.exe --gtest_brief=1 --gtest_filter="$filter" 2>&1 | tr -d '\r' > $C/win64-$n.txt; exit ${PIPESTATUS[0]}); local rc=$?
  if ! grep -q 'FAILED' $C/win64-$n.txt; then rc=0; fi    # a crash that reports no failure is not a verdict
  verdict win64-$n fail $rc "$what: $(grep -m1 -E '^(\[  FAILED  \]|.*Failure)' $C/win64-$n.txt | cut -c1-90)"
  rm -rf $tree
}
ONLY=${CONTROLS_ONLY:-}
ran=0
while IFS='|' read -r id macro exe filter what; do
  case "$id" in ''|'#'*) continue ;; esac
  [ -n "$ONLY" ] && case " $ONLY " in *" $id "*) ;; *) continue ;; esac
  planted "$id" "$macro" "$exe" "$filter" "$what" </dev/null
  ran=$((ran + 1))
done < tools/win64-plants.txt
if [ -n "$ONLY" ] && [ "$ran" -eq 0 ]; then
  echo "CONTROL none: CONTROLS_ONLY='$ONLY' names no planted defect of tools/win64-plants.txt, so no control ran"; bad=1
fi

rm -rf $C
[ $bad -eq 0 ] && echo "CONTROLS ok" || echo "CONTROLS NOT ok"
exit $bad
