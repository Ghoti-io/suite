#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with the scratch tree at /w.  Builds gtest and cutil for
# win64, then runs test-socket.exe and test-loop.exe, which the container's
# binfmt entry hands to wine, and the planted-defect controls.  See loop.sh.
set -u
P=$WPREFIX

if [ ! -f $P/lib/libgtest.a ]; then
  mkdir -p /w/gt-build && cd /w/gt-build || exit 2
  G=/gt/googletest
  g++ -std=c++20 -O1 -I$G -I$G/include -c $G/src/gtest-all.cc -o gtest-all.o &&
  ar rcs libgtest.a gtest-all.o || exit 2
  mkdir -p $P/lib $P/include $P/share/pkgconfig
  cp libgtest.a $P/lib/ && cp -r $G/include/gtest $P/include/
  printf 'Name: gtest\nDescription: gtest\nVersion: 1.16.0\nLibs: -L%s/lib -lgtest\nCflags: -I%s/include\n' \
    $P $P > $P/share/pkgconfig/gtest.pc
fi

APPS=build/win64/release/apps
cd /w/cutil || exit 2
make -j4 PREFIX=$P all > /w/logs/cutil.build.log 2>&1 \
  || { echo "loop-win: cutil did not build (see /w/logs/cutil.build.log)" >&2; grep -E "error|Error" -A5 /w/logs/cutil.build.log | head -40 >&2; exit 2; }
make install PREFIX=$P > /w/logs/cutil.install.log 2>&1 \
  || { echo "loop-win: cutil did not install" >&2; exit 2; }
make PREFIX=$P $APPS/test-socket.exe $APPS/test-loop.exe > /w/logs/cutil.tests.build.log 2>&1 \
  || { echo "loop-win: the tests did not build" >&2; grep -E "error|Error" -A5 /w/logs/cutil.tests.build.log | head -40 >&2; exit 2; }
file $APPS/test-loop.exe | cut -c1-100

rc=0
for t in test-socket test-loop; do
  echo "=== $t.exe, every test"
  ( cd $APPS && timeout 300 ./$t.exe 2>&1 | grep -vi fontconfig | grep -v '^\[ RUN\|^\[       OK' ; exit ${PIPESTATUS[0]} )
  c=$?
  echo "$t rc=$c"
  [ $c -eq 0 ] || rc=1
done
real_rc=$rc

# The controls: the same library built with one defect compiled in, each in a
# tree of its own, running the one test that carries it.  Each must FAIL, at
# that test, which is what shows the tests can tell under wine.  The defects
# are the ones check-loop-defects plants on Linux.
bad=0
control() {
  local plant=$1 filter=$2 expect=$3
  local d=/w/cutil-$plant
  echo
  echo "=== the control: GCU_LOOP_PLANT_$plant; $filter must FAIL"
  rm -rf $d; mkdir -p $d && (cd /w/cutil && tar cf - --exclude=./build .) | (cd $d && tar xf -) || exit 2
  ( cd $d && make -j4 PREFIX=$P EXTRA_CFLAGS=-DGCU_LOOP_PLANT_$plant all > /w/logs/cutil-$plant.build.log 2>&1 \
      && make PREFIX=$P EXTRA_CFLAGS=-DGCU_LOOP_PLANT_$plant $APPS/test-loop.exe > /w/logs/cutil-$plant.test.log 2>&1 ) \
    || { echo "loop-win: the $plant control did not build" >&2; exit 2; }
  # The define must have reached the compile of loop.c, or this "control" is
  # the real library and a failure below would be for some other reason.
  if ! grep -E 'src/loop\.c' /w/logs/cutil-$plant.build.log | grep -q -- "-DGCU_LOOP_PLANT_$plant"; then
    echo "loop-win: -DGCU_LOOP_PLANT_$plant is not on the compile of src/loop.c" >&2
    exit 2
  fi
  ( cd $d/$APPS && timeout 120 ./test-loop.exe --gtest_filter="$filter" > /w/logs/cutil-$plant.run.log 2>&1 ; exit $? )
  local c=$?
  grep -vi fontconfig /w/logs/cutil-$plant.run.log | grep 'TIMED OUT\|FAILED  \]\|PASSED' | head -3
  # Caught means: it failed, and failed AT the named test.  A wine crash or a
  # missing DLL is also a non-zero exit and shows nothing.
  if [ "$c" -ne 0 ] && grep -qE "$expect" /w/logs/cutil-$plant.run.log; then
    echo "control $plant: caught (rc=$c)"
  else
    echo "control $plant: NOT caught (rc=$c, no line matching /$expect/)"
    bad=1
  fi
}
control NO_WAKE 'LoopTest.PostFromAnotherThreadWakesAWaitingLoop' \
  'TIMED OUT, no answer from: LoopTest\.PostFromAnotherThreadWakesAWaitingLoop'
control CANCEL_EARLY_RELEASE 'LoopNet.CancelledReadCompletesOnceAndNeverTouchesItsBuffer' \
  '\[  FAILED  \] LoopNet\.CancelledReadCompletesOnceAndNeverTouchesItsBuffer'
control TIMER_ORDER 'LoopTest.TimersFireInDeadlineOrderAndNotBeforeTheirTime' \
  '\[  FAILED  \] LoopTest\.TimersFireInDeadlineOrderAndNotBeforeTheirTime'
echo "real rc=$real_rc"
[ "$real_rc" -eq 0 ] && [ "$bad" -eq 0 ]
