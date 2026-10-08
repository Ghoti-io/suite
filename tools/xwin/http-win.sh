#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with the scratch tree at /w. See http.sh.
set -u
P=$WPREFIX
APPS=build/win64/release/apps

die() { echo "http-win: $*" >&2; exit 2; }

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

# libghttp links cutil, security (SHA-1, BCryptGenRandom) and compress (deflate),
# in that order: each is built with its own Makefile's Windows arm and installed
# into the prefix, so that the next one, and libghttp, find it only through
# pkg-config. They are the COMMITTED trees (http.sh stages git archive HEAD).
for l in cutil security compress; do
  echo "=== $l, for win64"
  cd /w/$l || exit 2
  make -j4 PREFIX=$P all > /w/logs/$l.build.log 2>&1 \
    || { tail -20 /w/logs/$l.build.log >&2; die "$l did not build (/w/logs/$l.build.log)"; }
  make install PREFIX=$P > /w/logs/$l.install.log 2>&1 || { tail -20 /w/logs/$l.install.log >&2; die "$l did not install"; }
  dll=$(ls $P/bin/libghoti.io-$l-*.dll 2>/dev/null | head -1)
  [ -n "$dll" ] || die "$l left no DLL in $P/bin"
  file "$dll" | cut -c1-100
done
# What security's DLL asks Windows for: the random source is BCryptGenRandom, and
# it is bound through bcrypt.dll, which wine provides.
x86_64-w64-mingw32-objdump -p $P/bin/libghoti.io-security-0.dll | grep -qi 'bcrypt.dll' \
  && echo "security's DLL imports bcrypt.dll (BCryptGenRandom)" \
  || die "security's DLL does not import bcrypt.dll: the Windows entropy arm was not built"

echo "=== libghttp, for win64, with its own Makefile"
cd /w/http || exit 2
make -j4 PREFIX=$P all > /w/logs/http.build.log 2>&1 \
  || { tail -30 /w/logs/http.build.log >&2; die "libghttp did not build (/w/logs/http.build.log)"; }
make install PREFIX=$P > /w/logs/http.install.log 2>&1 || { tail -20 /w/logs/http.install.log >&2; die "libghttp did not install"; }
for f in $APPS/libghoti.io-http-0.dll $APPS/libghoti.io-http-0.a; do
  [ -f "$f" ] || die "$f was not built"
done
file $APPS/libghoti.io-http-0.dll | cut -c1-100
# The export directive is what __declspec(dllexport) writes; a Linux object
# has none, so this is the check that the Windows arm of the macros was built.
x86_64-w64-mingw32-objdump -s -j .drectve build/win64/release/objects/http/parser.o | grep -q 'export' \
  && echo "parser.o carries -export: directives (the dllexport arm was compiled)" \
  || die "parser.o has no export directives: the Windows arm of GHTTP_API was not built"
exports=$(x86_64-w64-mingw32-objdump -p $APPS/libghoti.io-http-0.dll | grep -c 'ghotiio_http_0_ghttp_')
echo "the DLL exports $exports ghttp functions"
[ "$exports" -gt 40 ] || die "the DLL exports only $exports ghttp functions"

echo
echo "=== the unit tests, under wine (make test, the gates cleared: they are Linux's)"
make PREFIX=$P test TEST_GATES= > /w/logs/http.test.log 2>&1
rc=$?
grep -E '^\[  (PASSED|FAILED)  \]|tests? ran' /w/logs/http.test.log | grep -vi fontconfig
ran=$(grep -h '^\[==========\] [0-9]* tests\? from' /w/logs/http.test.log | awk '{s += $2} END {print s + 0}')
total=$(grep -h '^\[  PASSED  \]' /w/logs/http.test.log | awk '{s += $4} END {print s + 0}')
echo "tests ran: $ran, passed: $total, make test rc=$rc"
[ $rc -eq 0 ] && [ "$total" -gt 0 ] && [ "$total" -eq "$ran" ] || { tail -30 /w/logs/http.test.log; die "the unit tests did not all pass under wine"; }

# Every test binary on its own, with its count required to equal the HOST's for
# the binary of the same name (from the host's --gtest_list_tests, written by
# http.sh), so that a binary that did not run, or ran fewer tests, is a failure
# and not a smaller total. The WebSocket suites call gsec_random_bytes, so this
# is also the check that BCryptGenRandom works under wine.
hosttotal=0; wintotal=0; nbin=0
for n in $(awk '{print $1}' /w/ref/counts.txt); do
  exe=$APPS/$n.exe
  [ -f "$exe" ] || die "$n.exe was not built (the host has it)"
  ( cd $APPS && ./$n.exe --gtest_brief=1 > /w/logs/$n.run.out 2>&1 ); r=$?
  out=$(tr -d '\r' < /w/logs/$n.run.out)
  passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p')
  want=$(awk -v n=$n '$1 == n {print $2}' /w/ref/counts.txt)
  printf '  %-20s passed=%s host=%s\n' $n "${passed:-0}" "$want"
  [ ${r:-1} -eq 0 ] && [ "${passed:-0}" -eq "$want" ] || { printf '%s\n' "$out" | tail -15; die "$n: not every test passed, or not the host's count"; }
  hosttotal=$((hosttotal + want)); wintotal=$((wintotal + passed)); nbin=$((nbin + 1))
done
built=$(ls $APPS/test*.exe | wc -l)
[ "$nbin" -eq "$built" ] || die "the host has $nbin test binaries and win64 built $built"
echo "every test binary passes under wine with the host's count: $wintotal of $hosttotal tests in $nbin binaries"

echo
echo "=== the probe and the writer round trip, against Linux's answers"
make PREFIX=$P $APPS/oracle/http_probe.exe > /w/logs/http.probe.build.log 2>&1 \
  || { tail -20 /w/logs/http.probe.build.log >&2; die "the probe did not build"; }
file $APPS/oracle/http_probe.exe | cut -c1-100
CFL=$(pkg-config --cflags ghoti.io-cutil-0 ghoti.io-security-0 ghoti.io-compress-0)
LFL=$(pkg-config --libs ghoti.io-cutil-0 ghoti.io-security-0 ghoti.io-compress-0)
x86_64-w64-mingw32-gcc -std=c17 -O2 -Wall -Wextra -Werror -DGHTTP_STATIC \
  -I include/ -I build/win64/release/generated/ $CFL \
  /w/http-write-probe.c -Wl,--whole-archive $APPS/libghoti.io-http-0.a -Wl,--no-whole-archive $LFL \
  -o /w/http-write-probe.exe > /w/logs/write-probe.build.log 2>&1 \
  || { tail -20 /w/logs/write-probe.build.log >&2; die "the writer probe did not build"; }

lines=$(wc -l < /w/ref/corpus.tsv)
( cd $APPS/oracle && ./http_probe.exe < /w/ref/corpus.tsv > /w/win-probe.out 2> /w/win-probe.err ); prc=$?
( cd /w && ./http-write-probe.exe > /w/win-write.out 2> /w/win-write.err ); wrc=$?
echo "probe rc=$prc ($(wc -l < /w/win-probe.out) of $lines lines), writer probe rc=$wrc"
bad=0
if cmp -s /w/ref/out/probe.out /w/win-probe.out; then
  echo "the probe's answers are IDENTICAL to Linux's over $lines lines"
else
  echo "FAIL: the probe's answers differ from Linux's"; bad=1
  diff <(cut -c1-300 /w/ref/out/probe.out) <(cut -c1-300 /w/win-probe.out) | head -6
fi
if cmp -s <(grep -v '^sizeof' /w/ref/out/write.out) <(grep -v '^sizeof' /w/win-write.out); then
  echo "the writer round trip is IDENTICAL to Linux's ($(grep -c . /w/win-write.out) lines)"
else
  echo "FAIL: the writer round trip differs from Linux's"; bad=1
  diff /w/ref/out/write.out /w/win-write.out | head -6
fi
grep -h '^sizeof' /w/win-write.out

# HTTP/2 (libs/http story 3b): the same, over its own corpus.
make PREFIX=$P $APPS/oracle/h2_probe.exe > /w/logs/h2.probe.build.log 2>&1 \
  || { tail -20 /w/logs/h2.probe.build.log >&2; die "the HTTP/2 probe did not build"; }
lines2=$(wc -l < /w/ref/corpus_h2.tsv)
( cd $APPS/oracle && ./h2_probe.exe < /w/ref/corpus_h2.tsv > /w/win-h2probe.out 2> /w/win-h2probe.err ); hrc=$?
echo "HTTP/2 probe rc=$hrc ($(wc -l < /w/win-h2probe.out) of $lines2 lines)"
if cmp -s /w/ref/out/h2probe.out /w/win-h2probe.out; then
  echo "the HTTP/2 probe's answers are IDENTICAL to Linux's over $lines2 lines"
else
  echo "FAIL: the HTTP/2 probe's answers differ from Linux's"; bad=1
  diff <(cut -c1-300 /w/ref/out/h2probe.out) <(cut -c1-300 /w/win-h2probe.out) | head -6
fi
[ $hrc -eq 0 ] || bad=1

# WebSocket (libs/http story 3c): the same, over its own corpus.
[ -s /w/ref/corpus_ws.tsv ] || die "no WebSocket corpus from the Linux reference run"
make PREFIX=$P $APPS/oracle/ws_probe.exe > /w/logs/ws.probe.build.log 2>&1 \
  || { tail -20 /w/logs/ws.probe.build.log >&2; die "the WebSocket probe did not build"; }
lines3=$(wc -l < /w/ref/corpus_ws.tsv)
( cd $APPS/oracle && ./ws_probe.exe < /w/ref/corpus_ws.tsv > /w/win-wsprobe.out 2> /w/win-wsprobe.err ); src=$?
echo "WebSocket probe rc=$src ($(wc -l < /w/win-wsprobe.out) of $lines3 lines)"
if cmp -s /w/ref/out/wsprobe.out /w/win-wsprobe.out; then
  echo "the WebSocket probe's answers are IDENTICAL to Linux's over $lines3 lines"
else
  echo "FAIL: the WebSocket probe's answers differ from Linux's"; bad=1
  diff <(cut -c1-300 /w/ref/out/wsprobe.out) <(cut -c1-300 /w/win-wsprobe.out) | head -6
fi
[ $src -eq 0 ] || bad=1
[ $prc -eq 0 ] && [ $wrc -eq 0 ] && [ $bad -eq 0 ] || die "the probe comparison failed"

echo
echo "=== the control: a parser that refuses a bare LF with the wrong error"
d=/w/http-planted
rm -rf $d; mkdir -p $d && (cd /w/http && tar cf - --exclude=./build .) | (cd $d && tar xf -) || die "cannot copy the tree"
grep -q 'return GHTTP_ERR_CORRUPT; // a bare LF' $d/src/http/parser.c || die "the anchor for the planted defect is not in parser.c (stale patch)"
sed -i 's|return GHTTP_ERR_CORRUPT; // a bare LF|return GHTTP_ERR_LIMIT; // a bare LF|' $d/src/http/parser.c
cmp -s $d/src/http/parser.c /w/http/src/http/parser.c && die "the planted copy equals the original"
( cd $d && make -j4 PREFIX=$P all > /w/logs/http-planted.build.log 2>&1 \
    && make PREFIX=$P $APPS/oracle/http_probe.exe >> /w/logs/http-planted.build.log 2>&1 ) \
  || { tail -20 /w/logs/http-planted.build.log >&2; die "the planted copy did not build"; }
# The copy was edited and the probe rebuilt from it (the sed was checked above to
# change the file), so a difference below is the planted defect.
( cd $d/$APPS/oracle && ./http_probe.exe < /w/ref/corpus.tsv > /w/win-planted.out 2>/dev/null )
if cmp -s /w/ref/out/probe.out /w/win-planted.out; then
  die "the planted defect was NOT caught: the comparison cannot tell"
fi
echo "caught: the probe's answers differ from Linux's"

echo
echo "=== the control: an HTTP/2 connection that lets DATA through on an idle stream"
d2=/w/h2-planted
rm -rf $d2; mkdir -p $d2 && (cd /w/http && tar cf - --exclude=./build .) | (cd $d2 && tar xf -) || die "cannot copy the tree"
f=$d2/src/http/h2conn.c
grep -q 'return proto(c, "DATA on an idle stream");' $f || die "the anchor for the planted HTTP/2 defect is not in h2conn.c (stale patch)"
sed -i 's|return proto(c, "DATA on an idle stream");|return GHTTP_OK;|' $f
cmp -s $f /w/http/src/http/h2conn.c && die "the planted HTTP/2 copy equals the original"
( cd $d2 && make -j4 PREFIX=$P all > /w/logs/h2-planted.build.log 2>&1 \
    && make PREFIX=$P $APPS/oracle/h2_probe.exe >> /w/logs/h2-planted.build.log 2>&1 ) \
  || { tail -20 /w/logs/h2-planted.build.log >&2; die "the planted HTTP/2 copy did not build"; }
( cd $d2/$APPS/oracle && ./h2_probe.exe < /w/ref/corpus_h2.tsv > /w/win-h2planted.out 2>/dev/null )
if cmp -s /w/ref/out/h2probe.out /w/win-h2planted.out; then
  die "the planted HTTP/2 defect was NOT caught: the comparison cannot tell"
fi
echo "caught: the HTTP/2 probe's answers differ from Linux's"
echo
echo "=== the control: a WebSocket server that accepts an unmasked client frame"
d3=/w/ws-planted
rm -rf $d3; mkdir -p $d3 && (cd /w/http && tar cf - --exclude=./build .) | (cd $d3 && tar xf -) || die "cannot copy the tree"
f=$d3/src/http/ws_conn.c
grep -qF 'if (c->role == GHTTP_WS_SERVER && !fh->masked) {' $f || die "the anchor for the planted WebSocket defect is not in ws_conn.c (stale patch)"
sed -i 's|if (c->role == GHTTP_WS_SERVER \&\& !fh->masked) {|if (0 \&\& c->role == GHTTP_WS_SERVER \&\& !fh->masked) {|' $f
cmp -s $f /w/http/src/http/ws_conn.c && die "the planted WebSocket copy equals the original"
( cd $d3 && make -j4 PREFIX=$P all > /w/logs/ws-planted.build.log 2>&1 \
    && make PREFIX=$P $APPS/testWs_conn.exe >> /w/logs/ws-planted.build.log 2>&1 ) \
  || { tail -20 /w/logs/ws-planted.build.log >&2; die "the planted WebSocket copy did not build"; }
# Only the test written for the defect: the suite crashes after the failed assertion
# on some builds, and then prints no FAILED summary; any "Failure" line from the one
# test that ran is that test's.
( cd $d3/$APPS && ./testWs_conn.exe --gtest_brief=1 --gtest_filter='*.AServerRefusesAnUnmaskedClientFrameWith1002' > /w/logs/ws-planted.run.out 2>&1 ); prc=$?
pout=$(tr -d '\r' < /w/logs/ws-planted.run.out)
if [ $prc -eq 0 ] || ! printf '%s\n' "$pout" | grep -qE '(^| )Failure$|FAILED'; then
  printf '%s\n' "$pout" | tail -8
  die "the planted WebSocket defect was NOT caught by the unit test written for it"
fi
echo "caught: WsConn.AServerRefusesAnUnmaskedClientFrameWith1002 fails under wine"
( cd $d3 && make PREFIX=$P $APPS/oracle/ws_probe.exe >> /w/logs/ws-planted.build.log 2>&1 ) || die "the planted ws_probe did not build"
( cd $d3/$APPS/oracle && ./ws_probe.exe < /w/ref/corpus_ws.tsv > /w/win-wsplanted.out 2>/dev/null )
if cmp -s /w/ref/out/wsprobe.out /w/win-wsplanted.out; then
  die "the planted WebSocket defect was NOT caught by the probe comparison: the corpus holds no unmasked client frame, or the comparison cannot tell"
fi
echo "caught: the WebSocket probe's answers differ from Linux's"
echo
echo "PASS"
