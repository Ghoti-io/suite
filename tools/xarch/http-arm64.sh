#!/bin/bash
#
# Builds libghttp for AArch64 with its own Makefile and runs its unit tests
# under qemu-aarch64 (libs/http, story 3 of the Defiant milestone,
# notes/http/README.md). `suite/tools/xarch/http.sh` runs the differential's probe on
# seven targets; this is the other half, the real gtest suites, on the one
# target whose image has a C++ cross toolchain and a gtest for it.
#
# Run it through the host side, which stages what the container cannot (the
# dependencies as COMMITTED, and the host's test counts), from the workspace root:
#
#   suite/tools/xarch/http-arm64-host.sh [scratch-dir]      (default /tmp/xarch-http-arm)
#
# or by hand, in the image Containerfile.arm64-jit builds:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v DEPS:/deps:ro,z -v HOSTLIST:/hostlist:ro,z \
#     -v /tmp/xarch-http-arm:/scratch:z \
#     ghoti-xarch-arm64-jit:deb13-gxx14-gtest bash /work/suite/tools/xarch/http-arm64.sh
#
# where DEPS holds cutil/, security/ and compress/ (git archive HEAD of each:
# another session may be mid-edit in a working tree, and what is tested is http
# against the dependencies as they are) and HOSTLIST holds counts.txt, one
# "<test binary> <listed tests>" line per binary from the HOST's build.
#
# What it does, in order:
#
#   1. Checks the machinery: an aarch64 binary is aarch64 by its ELF header, it
#      does NOT run natively, and under qemu it runs.
#   2. Builds cutil, security and compress WHOLE for aarch64, each with its own
#      Makefile (CC and CXX the cross compilers, -Werror and all) and installs
#      them into one prefix, so that libghttp finds them only through
#      pkg-config, as it would anywhere. Nothing is cut down to "the
#      translation units http links". The one concession: cutil's build runs a
#      generator it has just built (float_identifier, which asks the compiler
#      what float32_t and float64_t are) and an aarch64 program does not run
#      here, so that one binary is replaced by a two-line wrapper that runs the
#      real one under qemu-aarch64. The answer it gives is the aarch64's.
#      compress's x86 files (crc32_pclmul.c, adler32_ssse3.c) compile under their
#      own architecture guards, as they do for any non-x86 target.
#   3. Builds the library and ALL its test binaries (the HTTP/1, HTTP/2 and
#      WebSocket suites) with the library's own Makefile and flags (-Werror),
#      CC and CXX the aarch64 cross compilers.
#   4. Runs every test binary under qemu-aarch64 and requires every one to pass,
#      and the count of tests it ran to equal the count the HOST's binary of the
#      same name lists (read from --gtest_list_tests of the host build by the host
#      side, not remembered), binary by binary, and the set of binaries to equal
#      the host's. The WebSocket handshake and connection suites reach the kernel
#      random source (getrandom, through gsec_random_bytes) under qemu-user.
#   5. Plants two defects in copies and requires the matching suite to FAIL under
#      qemu at the test written for each: Content-Length with Transfer-Encoding
#      accepted (the request-parser suite), and a WebSocket server that accepts
#      an unmasked client frame (the WebSocket connection suite).
#
# What it cannot show, and does not claim: the sanitizers and valgrind do not run
# under qemu-user; the fuzzers are clang's; the gates (`check-symbols`,
# `check-defects`, `check-oracle`) are the host's and are not run here; qemu-user
# is not hardware. Exit status 0 only if every step above held.

set -u
set -o pipefail

WORK="${WORK:-/work}"
S="${SCRATCH:-/scratch}"
JOBS="${JOBS:-$(nproc)}"
QEMU_SYSROOT=/usr/aarch64-linux-gnu
P="$S/prefix"
SRC="$S/http"

fail() { printf '\nhttp-arm64: FAIL: %s\n' "$*" >&2; exit 1; }
heading() { printf '\n=== %s ===\n' "$*"; }
q() { qemu-aarch64 -L "$QEMU_SYSROOT" "$@"; }

rm -rf "$S/prefix" "$S/http" "$S/planted" "$S/planted-ws" "$S/shim" "$S/deps-build"
mkdir -p "$P" "$S/shim"

# The Makefile calls ar, nm, objdump by their plain names.
for t in ar ranlib nm objdump readelf strip objcopy; do
  ln -sf "$(command -v aarch64-linux-gnu-$t)" "$S/shim/$t"
done
export PATH="$S/shim:$PATH"
export PKG_CONFIG_PATH="$P/share/pkgconfig:/opt/aarch64/lib/pkgconfig"
CROSS=(CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++)

elf_ok() { # <path>
  [[ "$(/usr/bin/readelf -h "$1" 2>/dev/null | sed -n 's/^ *Machine: *//p')" == *AArch64* ]] &&
    [[ "$(file -b "$1")" == *"ARM aarch64"* ]]
}

heading "cutil, security and compress, whole, for aarch64, in $P"
[ -d /deps/cutil/src ] && [ -d /deps/security/src ] && [ -d /deps/compress/src ] ||
  fail "no /deps/{cutil,security,compress}: run suite/tools/xarch/http-arm64-host.sh, which stages them"
for l in cutil security compress; do
  mkdir -p "$S/deps-build/$l"
  rsync -a /deps/$l/ "$S/deps-build/$l/"
  if [ $l = cutil ]; then
    # See step 2 in the header.
    FI=build/linux/release/apps/float_identifier
    make -C "$S/deps-build/cutil" "${CROSS[@]}" PREFIX="$P" "$FI" > "$S/cutil.fi.log" 2>&1 ||
      { tail -20 "$S/cutil.fi.log" >&2; fail "cutil's float_identifier did not build"; }
    elf_ok "$S/deps-build/cutil/$FI" || fail "float_identifier is not an aarch64 binary"
    mv "$S/deps-build/cutil/$FI" "$S/deps-build/cutil/$FI.real"
    printf '#!/bin/sh\nexec qemu-aarch64 -L %s %s.real "$@"\n' "$QEMU_SYSROOT" "$S/deps-build/cutil/$FI" \
      > "$S/deps-build/cutil/$FI"
    chmod +x "$S/deps-build/cutil/$FI"
  fi
  make -C "$S/deps-build/$l" "${CROSS[@]}" PREFIX="$P" -j"$JOBS" all > "$S/$l.build.log" 2>&1 ||
    { tail -30 "$S/$l.build.log" >&2; fail "$l did not build for aarch64 (log $S/$l.build.log)"; }
  make -C "$S/deps-build/$l" "${CROSS[@]}" PREFIX="$P" install > "$S/$l.install.log" 2>&1 ||
    { tail -30 "$S/$l.install.log" >&2; fail "$l did not install"; }
  so=$(ls "$P"/lib/ghoti.io/libghoti.io-$l-0.so.*.*.* 2>/dev/null | head -1)
  [ -n "$so" ] && elf_ok "$so" || fail "$l's library is not an aarch64 object ($so)"
  for pc in "$P"/share/pkgconfig/ghoti.io-$l-0.pc; do [ -f "$pc" ] || fail "no $pc"; done
  echo "  $l: built whole with its own Makefile, installed, an aarch64 object by ELF header and file(1)"
done
# What http calls into must be in the objects that were built, not satisfied by
# something on the build host's path.
for fn in gsec_sha1 gsec_random_bytes; do
  # Not `nm | grep -q`: grep exits at the first match, nm takes SIGPIPE, and
  # pipefail turns a found symbol into a failure.
  aarch64-linux-gnu-nm -D --defined-only "$P"/lib/ghoti.io/libghoti.io-security-0.so.*.*.* > "$S/security.syms"
  grep -q "$fn" "$S/security.syms" || fail "security's aarch64 object defines no $fn"
done

heading "Building libghttp and its tests for aarch64 with its Makefile"
mkdir -p "$SRC"
rsync -a --exclude build --exclude docs --exclude .git --exclude tests/fuzz/corpus "$WORK/libs/http/" "$SRC/"
make -C "$SRC" "${CROSS[@]}" PREFIX="$P" -j"$JOBS" all > "$S/build.log" 2>&1 ||
  { tail -30 "$S/build.log" >&2; fail "libghttp did not build for aarch64 (log $S/build.log)"; }
NAMES=$(echo 'print-%: ; @echo $($*)' |
  make -s --no-print-directory -C "$SRC" -f Makefile -f - "${CROSS[@]}" PREFIX="$P" print-TEST_NAMES 2>/dev/null)
[ -n "$NAMES" ] || fail "could not read the test names from the Makefile"
APPS="$SRC/build/linux/release/apps"
for n in $NAMES; do
  make -C "$SRC" "${CROSS[@]}" PREFIX="$P" -j"$JOBS" "build/linux/release/apps/$n" >> "$S/build.log" 2>&1 ||
    { tail -30 "$S/build.log" >&2; fail "$n did not build"; }
  elf_ok "$APPS/$n" || fail "$n is not an aarch64 binary: refusing to run it as one"
done
elf_ok "$APPS/libghoti.io-http-0.so.0.0.0" || fail "the library is not an aarch64 object"
echo "  library and $(echo $NAMES | wc -w) test binaries, each an aarch64 object by ELF header and file(1)"

heading "Machinery"
if "$APPS/testCore" >/dev/null 2>&1; then
  fail "the aarch64 testCore ran WITHOUT qemu: it is not foreign, nothing here means anything"
fi
echo "  an aarch64 binary does not run natively (as it must not)"

declare -A PASSED_OF
run_all() { # <apps dir> -> sets TOTAL, returns non-zero on any failure
  local dir="$1" rc=0 n
  TOTAL=0
  for n in $NAMES; do
    local out
    out=$(cd "$SRC" && q -E LD_LIBRARY_PATH="$dir:$P/lib/ghoti.io" "$dir/$n" --gtest_brief=1 2>&1)
    local r=$?
    local passed
    passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p')
    printf '  %-20s rc=%s passed=%s\n' "$n" "$r" "${passed:-0}"
    TOTAL=$((TOTAL + ${passed:-0}))
    PASSED_OF[$n]=${passed:-0}
    [ $r -eq 0 ] || { printf '%s\n' "$out" | tail -15; rc=1; }
  done
  return $rc
}

heading "Every test binary, under qemu-aarch64"
run_all "$APPS" || fail "a test binary failed under qemu"
LISTED=0
for n in $NAMES; do
  c=$(cd "$SRC" && q -E LD_LIBRARY_PATH="$APPS:$P/lib/ghoti.io" "$APPS/$n" --gtest_list_tests 2>&1 | grep -c '^  ')
  LISTED=$((LISTED + c))
  [ "${PASSED_OF[$n]:-0}" -eq "$c" ] || fail "$n passed ${PASSED_OF[$n]:-0} but lists $c: a test did not run"
done
[ "$TOTAL" -eq "$LISTED" ] || fail "$TOTAL tests passed but the binaries list $LISTED: a test did not run"
echo "  $TOTAL passed of $LISTED listed (by the aarch64 binaries themselves)"

# The same count from the HOST's build of the same sources, binary by binary.
if [ -s /hostlist/counts.txt ]; then
  HOSTTOTAL=0
  for n in $NAMES; do
    h=$(awk -v n="$n" '$1 == n {print $2}' /hostlist/counts.txt)
    [ -n "$h" ] || fail "the host's listing has no $n: the host and aarch64 do not build the same set of tests"
    [ "${PASSED_OF[$n]}" -eq "$h" ] || fail "$n: $h tests on the host, ${PASSED_OF[$n]} passed on aarch64"
    HOSTTOTAL=$((HOSTTOTAL + h))
  done
  [ "$(awk 'END {print NR}' /hostlist/counts.txt)" -eq "$(echo $NAMES | wc -w)" ] ||
    fail "the host lists a different number of test binaries than aarch64 built"
  echo "  and every binary's count equals the host's ($HOSTTOTAL tests in $(echo $NAMES | wc -w) binaries)"
elif [ "${ALLOW_NO_HOSTLIST:-0}" = 1 ]; then
  echo "  WARNING: no /hostlist/counts.txt: the host's counts were NOT compared (ALLOW_NO_HOSTLIST=1)"
else
  fail "no /hostlist/counts.txt: the count was not compared with the host's (suite/tools/xarch/http-arm64-host.sh writes it; ALLOW_NO_HOSTLIST=1 to run without)"
fi

# plant <label> <dir> <file-under-src/http> <anchor> <replacement> <suite binary> <failing test regex>
# A copy of the built tree with one line changed, rebuilt, and the suite required
# to fail at the test written for it. The anchor must occur exactly once: a patch
# that did not apply would leave a copy equal to the original, and the run would
# then "show" nothing.
plant() {
  local label="$1" dir="$2" file="$3" anchor="$4" repl="$5" suite="$6" want="$7"
  rm -rf "$dir"; mkdir -p "$dir"
  rsync -a "$SRC/" "$dir/"
  python3 - "$dir/src/http/$file" "$anchor" "$repl" <<'PY' || fail "$label: the planted patch did not apply (stale anchor)"
import sys
p, old, new = sys.argv[1:4]
s = open(p).read()
if s.count(old) != 1:
    sys.exit(3)
open(p, "w").write(s.replace(old, new))
PY
  cmp -s "$dir/src/http/$file" "$SRC/src/http/$file" && fail "$label: the planted copy equals the original"
  touch "$dir/src/http/$file"
  rm -f "$dir/build/linux/release/apps/"*.a "$dir/build/linux/release/apps/"*.so* \
        "$dir/build/linux/release/apps/test"*
  make -C "$dir" "${CROSS[@]}" PREFIX="$P" -j"$JOBS" all "build/linux/release/apps/$suite" \
    > "$dir.build.log" 2>&1 || { tail -20 "$dir.build.log" >&2; fail "$label: the planted copy did not build"; }
  local out rc outf rcf
  out=$(cd "$dir" && q -E LD_LIBRARY_PATH="$dir/build/linux/release/apps:$P/lib/ghoti.io" \
    "$dir/build/linux/release/apps/$suite" --gtest_brief=1 2>&1)
  rc=$?
  # The test written for the defect, alone: a suite that crashes after the
  # first failed assertion (as the WebSocket one does when a frame it expected
  # refused is accepted) prints no FAILED summary line, and the named test's own
  # verdict is what shows the run can tell. Only that one test runs, so any
  # "Failure" line in its output is its own.
  outf=$(cd "$dir" && q -E LD_LIBRARY_PATH="$dir/build/linux/release/apps:$P/lib/ghoti.io" \
    "$dir/build/linux/release/apps/$suite" --gtest_brief=1 --gtest_filter="*.$want" 2>&1)
  rcf=$?
  if [ $rc -eq 0 ]; then
    fail "$label: the $suite suite PASSED with the defect planted: the run cannot tell"
  elif [ $rcf -ne 0 ] && printf '%s\n' "$outf" | grep -qE "(^| )Failure$|FAILED.*$want"; then
    echo "  caught: the $suite suite fails (rc=$rc) and $want fails on its own, under qemu"
  else
    printf '%s\n' "$outf" | tail -10
    fail "$label: the suite failed with the defect planted, but not at the test written for it"
  fi
}

heading "The control: Content-Length with Transfer-Encoding accepted"
plant "request smuggling" "$S/planted" util.c \
'  if (have_te && have_cl) {
    return GHTTP_ERR_CORRUPT;
  }' \
'  if (have_te && have_cl) {
    have_cl = false;
  }' testParser_request ContentLengthWithTransferEncodingIsCorrupt

heading "The control: a WebSocket server that accepts an unmasked client frame"
plant "websocket unmasked" "$S/planted-ws" ws_conn.c \
'if (c->role == GHTTP_WS_SERVER && !fh->masked) {' \
'if (0 && c->role == GHTTP_WS_SERVER && !fh->masked) {' testWs_conn AServerRefusesAnUnmaskedClientFrameWith1002

printf '\nPASS: %s tests pass on aarch64 under qemu-user, equal to the host'"'"'s count, and both planted defects are caught\n' "$TOTAL"
