#!/bin/bash
#
# Builds libghoti.io-tls for every target in the matrix, and checks that every
# target answers the RFC 8448 known answers exactly as the host does
# (libs/tls, planning/tls.md steps 3 and 4: the client's handshake, the record
# layer, the server's flight, and a real client against a real server).
#
# Run it in the ghoti-xarch image, from the workspace root, with the four
# libraries staged as committed at /deps (suite/tools/xarch/tls-host.sh does both):
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/tls-xarch/deps:/deps:ro,z \
#     ghoti-xarch:deb13 bash /work/suite/tools/xarch/tls.sh
#
# What it does, in order:
#
#   1. Checks the machinery: the aarch64 build is an aarch64 ELF, it does not
#      run on this host without qemu, and under qemu it does. A foreign result
#      that was quietly the host's would pass everything below.
#   2. For each target, compiles ALL the sources of cutil, security,
#      certificate and tls, each with its own library's flags and -Werror, and
#      links tls-probe.c against the objects. The probe reads the vectors
#      tools/extract-rfc8448.py wrote from the pinned RFC text and drives the
#      client through the simple 1-RTT and HelloRetryRequest traces, whole and
#      one byte at a time, checking every secret, the second ClientHello and
#      the client's Finished against the RFC's values; seals and opens the
#      RFC's records in both directions; drives the server with the RFC's
#      ClientHello and checks its flight, as messages and as records, against
#      the RFC; and runs a real client against a real server (each suite and
#      group, a retry, a key update, close_notify) with the stream split small.
#   3. Runs it under qemu-user (the host's directly) and requires every
#      target's output to be IDENTICAL to the host's, byte for byte, apart from
#      the size_t line, and every check on it to hold. That is the key schedule,
#      the transcript, the message codec and the state table on 32- and 64-bit,
#      on big-endian and strict-alignment targets.
#   4. Plants a defect in one target's copy of the key schedule (a traffic
#      secret derived under the other side's label), of the transcript restart,
#      and of the record nonce (the sequence number left out of it), and
#      requires the probe to FAIL on that target. A comparison that has never
#      been seen to fail may be comparing nothing.
#
# What it cannot show, and does not claim: the unit tests are gtest binaries
# and the image has no cross C++ library, so this is the probe and not
# `make test`; the sanitizers do not run under qemu-user; and qemu-user is not
# hardware. Exit status 0 only if every target matched and the planted build
# did not.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/targets.sh"

WORK=/work
DEPS=${DEPS:-/deps}
for l in cutil security certificate tls; do
  [ -d "$DEPS/$l/src" ] || { echo "tls.sh: no $DEPS/$l/src (tls-host.sh stages the libraries as committed)" >&2; exit 2; }
done
CUTIL=$DEPS/cutil
SECURITY=$DEPS/security
CERT=$DEPS/certificate
TLS=$DEPS/tls
B=${B:-/tmp/xarch-tls}
case "$B" in
  /*) ;;
  *) echo "tls.sh: B must be an absolute path, got '$B'" >&2; exit 2 ;;
esac
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "tls.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
rm -rf "$B"
mkdir -p "$B/include/ghoti.io/cutil" "$B/include/ghoti.io/security" "$B/include/ghoti.io/certificate" "$B/include/ghoti.io/tls"

gen() {   # NAME GUARD UPPER
  cat > "$B/include/ghoti.io/$1/libver_gen.h" <<H
#ifndef $2
#define $2
#define GHOTIIO_$3_NAME ghotiio_$1_xarch
#define GHOTIIO_$3_VERSION "0.0.0-xarch"
#define GHOTIIO_$3_VERSION_MAJOR 0
#define GHOTIIO_$3_VERSION_MINOR 0
#define GHOTIIO_$3_VERSION_PATCH 0
#endif
H
}
gen cutil GHOTIIO_CUTIL_LIBVER_GEN_H CUTIL
gen security GHOTI_IO_GSEC_LIBVER_GEN_H SECURITY
gen certificate GHOTI_IO_GCERT_LIBVER_GEN_H CERTIFICATE
gen tls GHOTI_IO_GTLS_LIBVER_GEN_H TLS

DEPINC="-I $CUTIL/include -I $SECURITY/include -I $CERT/include -I $TLS/include"
# Each library with the flags its own Makefile uses (CONVENTIONS.md section 6),
# so a warning the host compiler does not raise but this one does is a failure.
# security's and certificate's 32-bit -Wtype-limits findings and cutil's
# -Wmaybe-uninitialized on i686 are demoted for those libraries alone, as in
# http.sh, and for the same reasons.
CUFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wno-error=maybe-uninitialized -Wno-error=unused-function -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD"
SEFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wno-error=type-limits -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wno-error=unused-function -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGSEC_BUILD"
CEFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wno-error=type-limits -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wno-error=unused-function -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGCERT_BUILD"
TLFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wno-error=unused-function -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGTLS_BUILD"
JOBS=${JOBS:-$(nproc)}

# compile_tree LIB FLAGS INCLUDES OBJDIR CC ROOT: every .c under ROOT/src, in
# parallel, each object named LIB-<path>.o. Non-zero if any failed.
compile_tree() {
  local lib=$1 fl=$2 inc=$3 od=$4 cc=$5 root=$6 f rel o bad=0
  mkdir -p "$od"
  for f in $(find "$root/src" -name '*.c' ! -name '*.template.c' ! -name float_identifier.c | sort); do
    rel=${f#$root/src/}
    o=$od/$lib-$(echo "$rel" | tr / _ | sed 's/\.c$/.o/')
    while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
    ( $cc $fl $inc -I "$root/src" -c "$f" -o "$o" > "$o.log" 2>&1 || { echo "FAIL $lib $rel"; head -8 "$o.log"; touch "$o.failed"; } ) &
  done
  wait
  ls "$od"/$lib-*.failed >/dev/null 2>&1 && bad=1
  return $bad
}

# build_deps NAME CC [QEMU]: cutil, security and certificate, whole, for one
# target -> $B/NAME/deps/*.o and the target's cutil float.h.
build_deps() {
  local name=$1 cc=$2 qemu=${3:-} d=$B/$1
  mkdir -p "$d/include/ghoti.io/cutil" "$d/deps"
  $cc -std=c17 -O2 "$CUTIL/src/float_identifier.c" -o "$d/float_identifier" || return 1
  local f32 f64
  f32=$(xarch_run "${name}" "$qemu" "$d/float_identifier" 32) || return 1
  f64=$(xarch_run "${name}" "$qemu" "$d/float_identifier" 64) || return 1
  [ -n "$f32" ] && [ -n "$f64" ] || { echo "float_identifier answered nothing on $name" >&2; return 1; }
  sed "s/FLOAT32/$f32/; s/FLOAT64/$f64/" "$CUTIL/src/float.h.template" > "$d/include/ghoti.io/cutil/float.h"
  local inc="$DEPINC -I $d/include -I $B/include"
  compile_tree cutil "$CUFLAGS" "$inc" "$d/deps" "$cc" "$CUTIL" &&
  compile_tree security "$SEFLAGS" "$inc" "$d/deps" "$cc" "$SECURITY" &&
  compile_tree certificate "$CEFLAGS" "$inc" "$d/deps" "$cc" "$CERT"
}

# build NAME CC [TLS-ROOT] [DEPS-FROM]  ->  $B/NAME/tls-probe
# The dependency objects are those of target DEPS-FROM (default NAME): a planted
# build changes the library and nothing else, so it reuses its target's.
build() {
  local name=$1 cc=$2 root=${3:-$TLS} depsfrom=${4:-$1}
  local d=$B/$name
  mkdir -p "$d/tlsobj"
  rm -f "$d"/tlsobj/*
  local inc="$DEPINC -I $root/include -I $B/$depsfrom/include -I $B/include"
  compile_tree tls "$TLFLAGS" "$inc" "$d/tlsobj" "$cc" "$root" || return 1
  local pf="-std=c17 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I $root/src $inc"
  $cc $pf "$HERE/tls-probe.c" "$d"/tlsobj/*.o "$B/$depsfrom"/deps/*.o -pthread -lm -o "$d/tls-probe" || return 1
}

run() {   # NAME TRIPLE QEMU -> $B/NAME/probe.out
  xarch_run "$2" "$3" "$B/$1/tls-probe" "$TLS/tests/data/rfc8448" > "$B/$1/probe.out" 2> "$B/$1/probe.err"
}

status=0
fail() { echo "FAIL: $*"; status=1; : > "$B/.failed"; }

echo "== machinery"
build_deps aarch64-linux-gnu aarch64-linux-gnu-gcc qemu-aarch64 && build aarch64-linux-gnu aarch64-linux-gnu-gcc ||
  { echo "FAIL: cannot build for aarch64"; exit 1; }
elf=$(file -b "$B/aarch64-linux-gnu/tls-probe")
case "$elf" in
  *aarch64*|*"ARM aarch64"*) echo "   ELF: $(echo "$elf" | cut -c1-70)" ;;
  *) fail "the aarch64 build is not an aarch64 binary: $elf" ;;
esac
if "$B/aarch64-linux-gnu/tls-probe" "$TLS/tests/data/rfc8448" > /dev/null 2>&1; then
  fail "the aarch64 binary ran WITHOUT qemu: it is not foreign, nothing here means anything"
else
  echo "   the aarch64 binary does not run natively (as it must not)"
fi
[ $status -eq 0 ] || exit 1
echo "   dependencies built whole for aarch64: $(ls "$B"/aarch64-linux-gnu/deps/*.o | wc -l) objects (cutil, security, certificate)"

echo
echo "== the host (the control)"
build_deps x86_64-linux-gnu gcc && build x86_64-linux-gnu gcc || { echo "FAIL: cannot build for x86_64"; exit 1; }
run x86_64-linux-gnu x86_64-linux-gnu "" || { fail "the host's own run failed"; tail -n 5 "$B/x86_64-linux-gnu/probe.out"; tail -n 5 "$B/x86_64-linux-gnu/probe.err"; exit 1; }
checks=$(grep -c '^ok' "$B/x86_64-linux-gnu/probe.out")
echo "   $checks checks hold on the host; $(tail -1 "$B/x86_64-linux-gnu/probe.out")"
[ "$checks" -ge 203 ] || { fail "only $checks checks ran on the host: the probe measured less than it says"; exit 1; }
grep -q '^FAIL' "$B/x86_64-linux-gnu/probe.out" && { fail "a check failed on the host"; exit 1; }

one() {
  local triple="$1" cc="$2" qemu="$3" desc="$4"
  [ "$triple" = x86_64-linux-gnu ] && return
  printf "\n== %s (%s)\n" "$triple" "$desc"
  if ! { build_deps "$triple" "$cc" "$qemu" && build "$triple" "$cc"; }; then fail "$triple: cannot build"; return; fi
  if ! run "$triple" "$triple" "$qemu"; then
    fail "$triple: the run failed"; grep -m3 '^FAIL' "$B/$triple/probe.out"; head -3 "$B/$triple/probe.err"; return
  fi
  if cmp -s <(grep -v '^sizeof' "$B/x86_64-linux-gnu/probe.out") <(grep -v '^sizeof' "$B/$triple/probe.out"); then
    echo "   $(grep -c '^ok' "$B/$triple/probe.out") checks hold and the output is identical to the host's"
    grep '^sizeof' "$B/$triple/probe.out"
  else
    fail "$triple: the output differs from the host's"
    diff <(grep -v '^sizeof' "$B/x86_64-linux-gnu/probe.out") <(grep -v '^sizeof' "$B/$triple/probe.out") | head -6
  fi
}
xarch_each one
[ -e "$B/.failed" ] && status=1

# plant NAME LABEL FILE ANCHOR REPLACEMENT: a copy of tls's sources with one
# line changed (the anchor must occur exactly once), built for aarch64 against
# the dependency objects already built, and the probe required to FAIL.
plant() {
  local name=$1 label=$2 file=$3 anchor=$4 repl=$5
  local P=$B/$name-src
  rm -rf "$P"; mkdir -p "$P"; cp -a "$TLS/." "$P/"
  [ "$(grep -cF -- "$anchor" "$P/$file")" = 1 ] ||
    { fail "$label: the anchor is not in $file exactly once (stale patch): nothing was tested"; return; }
  local text
  text=$(cat "$P/$file"; echo x); text=${text%x}
  printf '%s' "${text/"$anchor"/"$repl"}" > "$P/$file"
  if cmp -s "$P/$file" "$TLS/$file"; then
    fail "$label: the planted copy equals the original"
  elif ! build "$name" aarch64-linux-gnu-gcc "$P" aarch64-linux-gnu; then
    fail "$label: the planted build does not compile"
  elif run "$name" aarch64-linux-gnu qemu-aarch64; then
    fail "$label: the planted defect was NOT caught: the probe passes over it"
  else
    echo "   caught: $(grep -c '^FAIL' "$B/$name/probe.out") checks fail on the planted build"
  fi
}

echo
echo "== the control: a client handshake traffic secret derived under the server's label, on aarch64"
plant planted "swapped label" src/schedule/schedule.c '"c hs traffic"' '"s hs traffic"'

echo
echo "== the control: a transcript that is not restarted after HelloRetryRequest, on aarch64"
plant planted-hrr "no restart" src/client/client.c 'result = gtls_transcript_restart(&c->sec.transcript, hash);' 'result = GTLS_OK;'

echo
echo "== the control: a record nonce built without the sequence number, on aarch64"
plant planted-nonce "no sequence in the nonce" src/record/record.c 'nonce[GTLS_IV_LEN - 1u - i] ^= (unsigned char)(k->seq >> (8u * i));' '(void)i;'

echo
[ $status -eq 0 ] && echo "PASS: every target answers the RFC 8448 traces as the host does, and the planted defects are caught" || echo "FAIL"
exit $status
