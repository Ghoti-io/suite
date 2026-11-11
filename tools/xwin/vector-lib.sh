#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# Runs INSIDE ghoti-cross-mingw64:deb13 or the wine image (through m1-win.sh), with the scratch tree at /w and
# vector's dependencies installed in $WPREFIX by `m1-lib.sh deps`.  Builds vector's unit test programs and its
# lowering gate for win64 with vector's own Makefile and runs each under wine from the library directory (the tests
# read tests/ by relative path).  The gate must match the goldens committed from the x86-64 build, and with one hex
# digit of a golden changed it must refuse.  One SUMMARY line; exit 0 only if every program passed and ran at least
# one test.  A skip is listed by name, never asserted away.
set -u
P=$WPREFIX
FILTER=${VECTOR_XWIN_TESTS:-^test}
mkdir -p /w/logs
cd /w/vector || exit 2
names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -f Makefile -f - PREFIX=$P print-TEST_NAMES 2>/dev/null)
sel=$(tr ' ' '\n' <<<"$names" | grep -E "$FILTER" | sort)
[ -n "$sel" ] || { echo "no test program matches $FILTER"; exit 2; }
APPS=build/win64/release/apps
targets=""
for n in $sel; do targets="$targets $APPS/$n.exe"; done
make -k -j4 PREFIX=$P $targets $APPS/lower/lower_gate.exe > /w/logs/vector.tests.build.log 2>&1
rc_build=$?
echo "built $(wc -w <<<"$sel") programs requested (make exit $rc_build)"
tot=0; pass=0; skip=0; bad=0; progs=0
: > /w/logs/vector.skipped.actual
for n in $sel; do
  t=$APPS/$n.exe
  [ -e "$t" ] || { bad=$((bad + 1)); echo "NOT BUILT: $n"; continue; }
  progs=$((progs + 1))
  start=$(date +%s)
  timeout 3000 "$t" > /w/logs/vector.$n.out 2>&1; rc=$?
  out=$(tr -d '\r' < /w/logs/vector.$n.out)
  secs=$(( $(date +%s) - start ))
  c=$(echo "$out" | sed -n 's/^\[==========\] \([0-9]*\) tests\? from.*/\1/p' | tail -1)
  p=$(echo "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) tests\?\..*/\1/p' | tail -1)
  s=$(echo "$out" | sed -n 's/^\[  SKIPPED \] \([0-9]*\) tests\?[.,].*/\1/p' | tail -1)
  tot=$((tot + ${c:-0})); pass=$((pass + ${p:-0})); skip=$((skip + ${s:-0}))
  echo "$out" | grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$' | sed 's/^\[  SKIPPED \] //' | sed "s|^|$n |" >> /w/logs/vector.skipped.actual
  if [ $rc -ne 0 ] || [ -z "$c" ] || [ "${p:-0}" -eq 0 ]; then
    bad=$((bad + 1)); echo "TEST PROGRAM FAILED: $n rc=$rc"; echo "$out" | grep -vi fontconfig | tail -n 15
  else
    printf '  %-30s %5s passed  %4ss\n' "$n" "$p" "$secs"
  fi
done
echo "skipped, by name:"; sed 's/^/  - /' /w/logs/vector.skipped.actual
# The lowering gate against the goldens, then shown to fail on a golden with one digit changed.
gate=$APPS/lower/lower_gate.exe
fixtures=$(ls tests/data/svg-lower/*.svg)
gate_status=OK
if [ -e "$gate" ]; then
  timeout 3000 "$gate" --golden tests/golden/lower $fixtures > /w/logs/vector.gate.out 2>&1 || { gate_status=FAIL; echo "LOWERING GATE FAILED"; tail -n 15 /w/logs/vector.gate.out; }
  g=$(ls tests/golden/lower/* | head -1)
  rm -rf /w/golden-planted && cp -r tests/golden/lower /w/golden-planted
  pg=/w/golden-planted/$(basename "$g")
  sed -i 's/^\(list-hash \)./\1Z/' "$pg"  # one digit of the first golden's list hash
  cmp -s "$g" "$pg" && { echo 'the planted golden equals the real one'; gate_status=FAIL; }
  if timeout 3000 "$gate" --golden /w/golden-planted $fixtures > /w/logs/vector.gate-planted.out 2>&1; then
    gate_status=FAIL; echo "THE LOWERING GATE ACCEPTED A CHANGED GOLDEN ($(basename "$g"))"
  else
    echo "  the lowering gate refuses a golden with a digit changed ($(basename "$g")): ok"
  fi
else
  gate_status=FAIL; echo "NOT BUILT: lower_gate"
fi
warn=$(grep -c 'warning:' /w/logs/vector.tests.build.log)
status=OK
{ [ $rc_build -ne 0 ] || [ $bad -ne 0 ] || [ $progs -eq 0 ] || [ $warn -ne 0 ] || [ $gate_status != OK ]; } && status=FAIL
echo "SUMMARY vector status=$status warnings=$warn programs=$progs tests=$tot passed=$pass skipped=$skip bad_programs=$bad gate=$gate_status"
[ $status = OK ]
