#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with the scratch tree at /w. See tls.sh.
set -u
P=$WPREFIX
APPS=build/win64/release/apps

die() { echo "tls-win: $*" >&2; exit 2; }

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
build tls

cd /w/tls || exit 2
for f in $APPS/libghoti.io-tls-0.dll $APPS/libghoti.io-tls-0.a; do
  [ -f "$f" ] || die "$f was not built"
done
file $APPS/libghoti.io-tls-0.dll | cut -c1-100
# The export directive is what __declspec(dllexport) writes; a Linux object
# has none, so this is the check that the Windows arm of the macros was built.
x86_64-w64-mingw32-objdump -s -j .drectve build/win64/release/objects/schedule/schedule.o | grep -q 'export' \
  && echo "schedule.o carries -export: directives (the dllexport arm was compiled)" \
  || die "schedule.o has no export directives: the Windows arm of GTLS_API was not built"
exports=$(x86_64-w64-mingw32-objdump -p $APPS/libghoti.io-tls-0.dll | grep -c 'ghotiio_tls_0_gtls_')
echo "the DLL exports $exports gtls functions"
[ "$exports" -ge 30 ] || die "the DLL exports only $exports gtls functions"

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
echo "=== tls's unit tests, under wine (make test, the gates cleared: they are Linux's)"
run_tests tls 624

echo
echo "=== the control: a client handshake traffic secret derived under the server's label"
d=/w/tls-planted
rm -rf $d; mkdir -p $d && (cd /w/tls && tar cf - --exclude=./build .) | (cd $d && tar xf -) || die "cannot copy the tree"
f=$d/src/schedule/schedule.c
grep -q '"c hs traffic"' $f || die "the anchor for the planted defect is not in schedule.c (stale patch)"
sed -i 's|"c hs traffic"|"s hs traffic"|' $f
cmp -s $f /w/tls/src/schedule/schedule.c && die "the planted copy equals the original"
( cd $d && make -j4 PREFIX=$P all > /w/logs/tls-planted.build.log 2>&1 ) \
  || { tail -20 /w/logs/tls-planted.build.log >&2; die "the planted copy did not build"; }
( cd $d && make PREFIX=$P test TEST_GATES= > /w/logs/tls-planted.test.log 2>&1 )
if [ $? -eq 0 ]; then
  die "the planted defect was NOT caught: the tests pass over a key schedule that derives the wrong secret"
fi
grep -E '^\[  FAILED  \] [A-Za-z]' /w/logs/tls-planted.test.log | head -3
echo "caught: the same run fails over the planted copy"

echo
echo "=== the control: a record nonce built without the sequence number"
d=/w/tls-planted-nonce
rm -rf $d; mkdir -p $d && (cd /w/tls && tar cf - --exclude=./build .) | (cd $d && tar xf -) || die "cannot copy the tree"
f=$d/src/record/record.c
grep -q 'nonce\[GTLS_IV_LEN - 1u - i\] ^= (unsigned char)(k->seq >> (8u \* i));' $f || die "the anchor for the planted nonce defect is not in record.c (stale patch)"
sed -i 's|nonce\[GTLS_IV_LEN - 1u - i\] ^= (unsigned char)(k->seq >> (8u \* i));|(void)i;|' $f
cmp -s $f /w/tls/src/record/record.c && die "the planted copy equals the original"
( cd $d && make -j4 PREFIX=$P all > /w/logs/tls-planted-nonce.build.log 2>&1 ) \
  || { tail -20 /w/logs/tls-planted-nonce.build.log >&2; die "the planted nonce copy did not build"; }
( cd $d && make PREFIX=$P test TEST_GATES= > /w/logs/tls-planted-nonce.test.log 2>&1 )
if [ $? -eq 0 ]; then
  die "the planted nonce defect was NOT caught: the tests pass over records sealed with a constant nonce"
fi
grep -E '^\[  FAILED  \] [A-Za-z]' /w/logs/tls-planted-nonce.test.log | head -3
echo "caught: the record known answers and the loopback fail over the planted copy"
echo
echo "=== the control: a PSK binder derived under the wrong label"
d=/w/tls-planted-binder
rm -rf $d; mkdir -p $d && (cd /w/tls && tar cf - --exclude=./build .) | (cd $d && tar xf -) || die "cannot copy the tree"
f=$d/src/schedule/schedule.c
grep -q '"res binder"' $f || die "the anchor for the planted binder defect is not in schedule.c (stale patch)"
sed -i 's|"res binder"|"res binderx"|' $f
cmp -s $f /w/tls/src/schedule/schedule.c && die "the planted copy equals the original"
( cd $d && make -j4 PREFIX=$P all > /w/logs/tls-planted-binder.build.log 2>&1 ) \
  || { tail -20 /w/logs/tls-planted-binder.build.log >&2; die "the planted binder copy did not build"; }
( cd $d && make PREFIX=$P test TEST_GATES= > /w/logs/tls-planted-binder.test.log 2>&1 )
if [ $? -eq 0 ]; then
  die "the planted binder defect was NOT caught: the tests pass over a binder keyed under the wrong label"
fi
grep -E '^\[  FAILED  \] [A-Za-z]' /w/logs/tls-planted-binder.test.log | head -3
echo "caught: RFC 8448's resumed trace and the resumption loopbacks fail over the planted copy"
echo
echo "=== the control: the early traffic secret derived under another label"
d=/w/tls-planted-early
rm -rf $d; mkdir -p $d && (cd /w/tls && tar cf - --exclude=./build .) | (cd $d && tar xf -) || die "cannot copy the tree"
f=$d/src/schedule/schedule.c
grep -q '"c e traffic"' $f || die "the anchor for the planted early defect is not in schedule.c (stale patch)"
sed -i 's|"c e traffic"|"c e traffiq"|' $f
cmp -s $f /w/tls/src/schedule/schedule.c && die "the planted copy equals the original"
( cd $d && make -j4 PREFIX=$P all > /w/logs/tls-planted-early.build.log 2>&1 ) \
  || { tail -20 /w/logs/tls-planted-early.build.log >&2; die "the planted early copy did not build"; }
( cd $d && make PREFIX=$P test TEST_GATES= > /w/logs/tls-planted-early.test.log 2>&1 )
if [ $? -eq 0 ]; then
  die "the planted early defect was NOT caught: the tests pass over an early secret derived under the wrong label"
fi
grep -E '^\[  FAILED  \] [A-Za-z]' /w/logs/tls-planted-early.test.log | head -3
echo "caught: RFC 8448's 0-RTT trace and the early-data loopbacks fail over the planted copy"
echo
echo "PASS"
