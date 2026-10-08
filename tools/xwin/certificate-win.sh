#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with the scratch tree at /w. See certificate.sh.
set -u
P=$WPREFIX
APPS=build/win64/release/apps

die() { echo "certificate-win: $*" >&2; exit 2; }

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

build() {              # $1 = library
  echo "=== $1, for win64, with its own Makefile"
  cd /w/$1 || exit 2
  make -j4 PREFIX=$P all > /w/logs/$1.build.log 2>&1 \
    || { tail -30 /w/logs/$1.build.log >&2; die "$1 did not build (/w/logs/$1.build.log)"; }
  make install PREFIX=$P > /w/logs/$1.install.log 2>&1 \
    || { tail -20 /w/logs/$1.install.log >&2; die "$1 did not install"; }
}
build cutil
build security
build certificate

cd /w/certificate || exit 2
for f in $APPS/libghoti.io-certificate-0.dll $APPS/libghoti.io-certificate-0.a; do
  [ -f "$f" ] || die "$f was not built"
done
file $APPS/libghoti.io-certificate-0.dll | cut -c1-100
# The export directive is what __declspec(dllexport) writes; a Linux object
# has none, so this is the check that the Windows arm of the macros was built.
x86_64-w64-mingw32-objdump -s -j .drectve build/win64/release/objects/der/der.o | grep -q 'export' \
  && echo "der.o carries -export: directives (the dllexport arm was compiled)" \
  || die "der.o has no export directives: the Windows arm of GCERT_API was not built"
exports=$(x86_64-w64-mingw32-objdump -p $APPS/libghoti.io-certificate-0.dll | grep -c 'ghotiio_certificate_0_gcert_')
echo "the DLL exports $exports gcert functions"
[ "$exports" -ge 20 ] || die "the DLL exports only $exports gcert functions"

run_tests() {          # $1 = library, $2 = the count it must be
  cd /w/$1 || exit 2
  make PREFIX=$P test TEST_GATES= > /w/logs/$1.test.log 2>&1
  local rc=$?
  grep -E '^\[  (PASSED|FAILED)  \]|tests? ran' /w/logs/$1.test.log | grep -vi fontconfig
  local ran total
  ran=$(grep -h '^\[==========\] [0-9]* tests\? from' /w/logs/$1.test.log | awk '{s += $2} END {print s + 0}')
  total=$(grep -h '^\[  PASSED  \]' /w/logs/$1.test.log | awk '{s += $4} END {print s + 0}')
  echo "$1: tests ran: $ran, passed: $total, make test rc=$rc"
  [ $rc -eq 0 ] && [ "$total" -gt 0 ] && [ "$total" -eq "$ran" ] \
    || { tail -30 /w/logs/$1.test.log; die "$1's unit tests did not all pass under wine"; }
  [ "$ran" -eq "$2" ] || die "$1 ran $ran tests, expected $2: a binary was skipped or added"
}

echo
echo "=== certificate's unit tests, under wine (make test, the gates cleared: they are Linux's)"
run_tests certificate 161
if [ "${WITH_SECURITY:-0}" = 1 ]; then
  echo
  echo "=== security's unit tests, under wine"
  run_tests security 120
fi

echo
echo "=== the control: a conversion that turns a failed signature into success"
d=/w/certificate-planted
rm -rf $d; mkdir -p $d && (cd /w/certificate && tar cf - --exclude=./build .) | (cd $d && tar xf -) || die "cannot copy the tree"
f=$d/src/core/sec_result.h
grep -q 'return GCERT_ERR_MISMATCH;' $f || die "the anchor for the planted defect is not in sec_result.h (stale patch)"
sed -i 's|return GCERT_ERR_MISMATCH;|return GCERT_OK;|' $f
cmp -s $f /w/certificate/src/core/sec_result.h && die "the planted copy equals the original"
( cd $d && make -j4 PREFIX=$P all > /w/logs/certificate-planted.build.log 2>&1 ) \
  || { tail -20 /w/logs/certificate-planted.build.log >&2; die "the planted copy did not build"; }
( cd $d && make PREFIX=$P test TEST_GATES= > /w/logs/certificate-planted.test.log 2>&1 )
if [ $? -eq 0 ]; then
  die "the planted defect was NOT caught: the tests pass over a conversion that accepts a bad signature"
fi
grep -E '^\[  FAILED  \] [A-Za-z]' /w/logs/certificate-planted.test.log | head -3
echo "caught: the same run fails over the planted copy"
echo
echo "PASS"
