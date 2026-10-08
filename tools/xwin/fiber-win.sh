#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with the scratch tree at /w.  Builds gtest and cutil for
# win64, then runs test-fiber.exe, which the container's binfmt entry hands to
# wine.  See fiber.sh.
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

cd /w/cutil || exit 2
make -j4 PREFIX=$P all > /w/logs/cutil.build.log 2>&1 \
  || { echo "fiber-win: cutil did not build (see /w/logs/cutil.build.log)" >&2; tail -20 /w/logs/cutil.build.log >&2; exit 2; }
make install PREFIX=$P > /w/logs/cutil.install.log 2>&1 \
  || { echo "fiber-win: cutil did not install" >&2; exit 2; }
APPS=build/win64/release/apps
make PREFIX=$P $APPS/test-fiber.exe > /w/logs/cutil.test-fiber.build.log 2>&1 \
  || { echo "fiber-win: test-fiber did not build" >&2; tail -30 /w/logs/cutil.test-fiber.build.log >&2; exit 2; }

file $APPS/test-fiber.exe | cut -c1-100
echo "=== test-fiber.exe, every test"
( cd $APPS && ./test-fiber.exe 2>&1 | grep -vi fontconfig | grep -v '^\[ RUN\|^\[       OK' ; exit ${PIPESTATUS[0]} )
real=$?

# The controls: the same library built with the switch skipping the MXCSR
# control bits, and with it skipping the x87 control word, each in a tree of
# its own.  The rounding test must FAIL on both, which is what shows the test
# can tell under wine.
real_rc=$real
bad=0
for plant in NO_MXCSR NO_X87CW; do
  d=/w/cutil-$plant
  echo
  echo "=== the control: GCU_FIBER_PLANT_$plant; the rounding test must FAIL"
  rm -rf $d; mkdir -p $d && (cd /w/cutil && tar cf - --exclude=./build .) | (cd $d && tar xf -) || exit 2
  ( cd $d && make -j4 PREFIX=$P EXTRA_CFLAGS=-DGCU_FIBER_PLANT_$plant all > /w/logs/cutil-$plant.build.log 2>&1 \
      && make PREFIX=$P EXTRA_CFLAGS=-DGCU_FIBER_PLANT_$plant $APPS/test-fiber.exe > /w/logs/cutil-$plant.test.log 2>&1 ) \
    || { echo "fiber-win: the $plant control did not build" >&2; exit 2; }
  # The define must have reached the compile of fiber.c, or this "control" is
  # the real library and a failure below would be for some other reason.
  if ! grep -E 'src/fiber\.c' /w/logs/cutil-$plant.build.log | grep -q -- "-DGCU_FIBER_PLANT_$plant"; then
    echo "fiber-win: -DGCU_FIBER_PLANT_$plant is not on the compile of src/fiber.c" >&2
    exit 2
  fi
  ( cd $d/$APPS && ./test-fiber.exe --gtest_filter='*EachFiberKeepsItsOwnRoundingMode*' > /w/logs/cutil-$plant.run.log 2>&1 ; exit $? )
  c=$?
  grep -vi fontconfig /w/logs/cutil-$plant.run.log | grep 'fegetround\|FAILED  \] Fiber\|PASSED'
  # Caught means: it failed, and failed AT the rounding test.  A wine crash or
  # a missing DLL is also a non-zero exit and shows nothing.
  if [ "$c" -ne 0 ] && grep -q '\[  FAILED  \] Fiber\.EachFiberKeepsItsOwnRoundingModeAcrossSwitches' /w/logs/cutil-$plant.run.log; then
    echo "control $plant: caught (rc=$c)"
  else
    echo "control $plant: NOT caught (rc=$c, no FAILED line for the rounding test)"
    bad=1
  fi
done
echo "real rc=$real_rc"
[ "$real_rc" -eq 0 ] && [ "$bad" -eq 0 ]
