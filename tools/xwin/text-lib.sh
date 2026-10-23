#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# Runs INSIDE ghoti-cross-mingw64:deb13 or the wine image (through m1-win.sh, which sources m1-env.sh first),
# with the scratch tree at /w and text's dependencies and text itself installed in $WPREFIX by
# `m1-lib.sh deps` (M1_DEPS without ctang).  Builds the test programs of the XML, HTML, Encoding, markup and CSS
# work for win64 with text's own Makefile and runs each under wine, from the library directory (the tests read
# tests/ and third_party/ by relative path).  One SUMMARY line; exit 0 only if every program passed and ran at
# least one test.  The skips are listed by name (this script asserts none: a Windows skip is a finding to be read).
set -u
P=$WPREFIX
FILTER=${TEXT_XWIN_TESTS:-^test(Html|Xml|Xhtml|Encoding|Markup|Quadratic|Css)}
mkdir -p /w/logs
cd /w/text || exit 2
names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -f Makefile -f - PREFIX=$P print-TEST_NAMES 2>/dev/null)
sel=$(tr ' ' '\n' <<<"$names" | grep -E "$FILTER" | sort)
[ -n "$sel" ] || { echo "no test program matches $FILTER"; exit 2; }
APPS=build/win64/release/apps
targets=""
for n in $sel; do targets="$targets $APPS/$n.exe"; done
make -k -j4 PREFIX=$P $targets > /w/logs/text.tests.build.log 2>&1
rc_build=$?
echo "built $(wc -w <<<"$sel") programs requested (make exit $rc_build)"
tot=0; pass=0; skip=0; bad=0; progs=0
: > /w/logs/text.skipped.actual
for n in $sel; do
  t=$APPS/$n.exe
  [ -e "$t" ] || { bad=$((bad + 1)); echo "NOT BUILT: $n"; continue; }
  progs=$((progs + 1))
  start=$(date +%s)
  timeout 3000 "$t" > /w/logs/text.$n.out 2>&1; rc=$?
  secs=$(( $(date +%s) - start ))
  out=$(tr -d '\r' < /w/logs/text.$n.out)
  c=$(echo "$out" | sed -n 's/^\[==========\] \([0-9]*\) tests\? from.*/\1/p' | tail -1)
  p=$(echo "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) tests\?\..*/\1/p' | tail -1)
  s=$(echo "$out" | sed -n 's/^\[  SKIPPED \] \([0-9]*\) tests\?[.,].*/\1/p' | tail -1)
  tot=$((tot + ${c:-0})); pass=$((pass + ${p:-0})); skip=$((skip + ${s:-0}))
  echo "$out" | grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$' | sed 's/^\[  SKIPPED \] //' | sed "s|^|$n |" >> /w/logs/text.skipped.actual
  if [ $rc -ne 0 ] || [ -z "$c" ] || [ "${p:-0}" -eq 0 ]; then
    bad=$((bad + 1)); echo "TEST PROGRAM FAILED: $n rc=$rc"; echo "$out" | grep -vi fontconfig | tail -n 15
  else
    printf '  %-30s %5s passed  %4ss\n' "$n" "$p" "$secs"
  fi
done
echo "skipped, by name:"; sed 's/^/  - /' /w/logs/text.skipped.actual
warn=$(grep -c 'warning:' /w/logs/text.tests.build.log)
status=OK
{ [ $rc_build -ne 0 ] || [ $bad -ne 0 ] || [ $progs -eq 0 ] || [ $warn -ne 0 ]; } && status=FAIL
echo "SUMMARY text status=$status warnings=$warn programs=$progs tests=$tot passed=$pass skipped=$skip bad_programs=$bad"
[ $status = OK ]
