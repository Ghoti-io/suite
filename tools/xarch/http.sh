#!/bin/bash
#
# Builds libghttp for every target in the matrix and checks that every target
# parses and writes exactly as the host does (libs/http, story 3 of the Defiant
# milestone, notes/http/README.md).
#
# Run it in the ghoti-xarch image, from the workspace root, with the corpus the
# differential uses mounted at /corpus (suite/tools/xarch/http-host.sh does both):
#
#   python3 libs/http/tools/oracle/corpus.py --dump /tmp/http-xarch/corpus.tsv --count 20000
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/http-xarch:/corpus:ro,z -v /tmp/http-xarch/deps:/deps:ro,z \
#     ghoti-xarch:deb13 bash /work/suite/tools/xarch/http.sh
#
# What it does, in order:
#
#   1. Checks the machinery: the aarch64 build is an aarch64 ELF, it does not
#      run on this host without qemu, and under qemu it does. A foreign result
#      that was quietly the host's would pass everything below.
#   2. For each target, compiles the library's sources (src/core, src/http) and
#      the three libraries it links - ALL the sources of cutil, security and
#      compress, each with its own library's flags and -Werror (see build_deps) -
#      and links the probes against the objects: the HTTP/1.1 differential's
#      probe (libs/http/tools/oracle/http_probe.c), which parses every case of
#      the corpus whole and one byte at a time and prints the answer as JSON, the
#      HTTP/2 probe (h2_probe.c), the WebSocket probe (ws_probe.c, see "The
#      WebSocket hook" below) and http-write-probe.c, which writes a chunked
#      message through the writer and reads it back at four piece sizes. The
#      dependency objects are linked directly and not through an archive:
#      compress registers its methods from constructors, and an archive link
#      would drop them (the Makefiles use --whole-archive for the same reason).
#      The image has no cross C++ compiler, so the libraries' own Makefiles
#      (whose shared-library link is g++'s) cannot run here; suite/tools/xarch/
#      http-arm64.sh builds all three with their Makefiles for aarch64.
#      security and compress need from the OS only getrandom (sys/random.h) and
#      pthreads, which glibc provides on every target here; compress's x86
#      files (crc32_pclmul.c, adler32_ssse3.c) compile under their own
#      architecture guards on the other targets.
#   3. Runs both under qemu-user (the host's directly) and requires each
#      target's output to be IDENTICAL to the host's, byte for byte, apart from
#      the size_t line. The probe's output covers every message shape the
#      differential holds: it is the parse, header by header and body byte by
#      body byte, on 32- and 64-bit and on big-endian and strict-alignment
#      targets.
#   4. Plants a defect in one target's copy of the parser (a bare LF refused
#      with the wrong error) and requires the comparison to FAIL for that target
#      and only for it. A comparison that has never been seen to fail may be
#      comparing nothing.
#
# The WebSocket probe (ws_probe.c, libs/http/tools/oracle/) runs on every target
# over the corpus the host side dumps with diff_ws.py --dump (http-host.sh), with
# the same byte-for-byte identity with the host as the others, and a planted
# defect (a server that accepts an unmasked client frame) that the comparison
# must catch. A missing ws_probe.c or corpus is a hard error, not a skipped part.
#
# What it cannot show, and does not claim: the unit tests are gtest binaries and
# the image has no cross C++ library, so this is the probe and not `make test`;
# the sanitizers do not run under qemu-user; and qemu-user is not hardware.
# Exit status 0 only if every target matched and the planted build did not.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/targets.sh"

WORK=/work
HTTP=$WORK/libs/http
# The dependencies as COMMITTED (http-host.sh stages `git archive HEAD` of each at
# /deps): another session may be mid-edit in a working tree, and what is tested is
# libghttp against the dependencies as they are. Without /deps the working trees are
# used, and the run says so.
if [ -d /deps/cutil/src ] && [ -d /deps/security/src ] && [ -d /deps/compress/src ]; then
  DEPS=/deps
else
  DEPS=$WORK/libs
  echo "http.sh: WARNING: no /deps: using the dependencies' WORKING TREES, not their commits" >&2
fi
CUTIL=$DEPS/cutil
B=${B:-/tmp/xarch-http}
CORPUS=${CORPUS:-/corpus/corpus.tsv}
CORPUS2=${CORPUS2:-/corpus/corpus_h2.tsv}   # the HTTP/2 corpus (libs/http story 3b)
CORPUS3=${CORPUS3:-/corpus/corpus_ws.tsv}   # the WebSocket corpus (libs/http story 3c)
CORPUS4=${CORPUS4:-/corpus/corpus_wsh2.tsv} # WebSocket over HTTP/2 (libs/http story 3d), see ws_h2_probe.c
SECURITY=$DEPS/security
COMPRESS=$DEPS/compress
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "http.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
case "$B" in
  /*) ;;
  *) echo "http.sh: B must be an absolute path, got '$B'" >&2; exit 2 ;;
esac
if [ ! -s "$CORPUS" ]; then
  echo "http.sh: no corpus at $CORPUS; write one with" >&2
  echo "  python3 libs/http/tools/oracle/corpus.py --dump DIR/corpus.tsv --count 20000" >&2
  echo "and mount DIR at /corpus (suite/tools/xarch/http-host.sh does both)" >&2
  exit 2
fi
rm -rf "$B"
mkdir -p "$B/include/ghoti.io/cutil" "$B/include/ghoti.io/http" "$B/include/ghoti.io/security" "$B/include/ghoti.io/compress"

# The generated headers the Makefiles write, with the same text.
cat > "$B/include/ghoti.io/cutil/libver_gen.h" <<'H'
#ifndef GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_NAME ghotiio_cutil_xarch
#define GHOTIIO_CUTIL_VERSION "0.0.0-xarch"
#define GHOTIIO_CUTIL_VERSION_MAJOR 0
#define GHOTIIO_CUTIL_VERSION_MINOR 0
#define GHOTIIO_CUTIL_VERSION_PATCH 0
#endif
H
cat > "$B/include/ghoti.io/http/libver_gen.h" <<'H'
#ifndef GHOTI_IO_GHTTP_LIBVER_GEN_H
#define GHOTI_IO_GHTTP_LIBVER_GEN_H
#define GHOTIIO_HTTP_NAME ghotiio_http_xarch
#define GHOTIIO_HTTP_VERSION "0.0.0-xarch"
#define GHOTIIO_HTTP_VERSION_MAJOR 0
#define GHOTIIO_HTTP_VERSION_MINOR 0
#define GHOTIIO_HTTP_VERSION_PATCH 0
#endif
H

cat > "$B/include/ghoti.io/security/libver_gen.h" <<'H'
#ifndef GHOTI_IO_GSEC_LIBVER_GEN_H
#define GHOTI_IO_GSEC_LIBVER_GEN_H
#define GHOTIIO_SECURITY_NAME ghotiio_security_xarch
#define GHOTIIO_SECURITY_VERSION "0.0.0-xarch"
#define GHOTIIO_SECURITY_VERSION_MAJOR 0
#define GHOTIIO_SECURITY_VERSION_MINOR 0
#define GHOTIIO_SECURITY_VERSION_PATCH 0
#endif
H
cat > "$B/include/ghoti.io/compress/libver_gen.h" <<'H'
#ifndef GHOTI_IO_GCOMP_LIBVER_GEN_H
#define GHOTI_IO_GCOMP_LIBVER_GEN_H
#define GHOTIIO_COMPRESS_NAME ghotiio_compress_xarch
#define GHOTIIO_COMPRESS_VERSION "0.0.0-xarch"
#define GHOTIIO_COMPRESS_VERSION_MAJOR 0
#define GHOTIIO_COMPRESS_VERSION_MINOR 0
#define GHOTIIO_COMPRESS_VERSION_PATCH 0
#endif
H

# The library's flags (CONVENTIONS.md section 6), so that a warning the host
# compiler does not raise but this one does is a failure.
# Headers of the dependencies come first-hand from their own checkouts; the
# generated ones (libver_gen.h, and cutil's float.h, which is target-specific and
# so lives in each target's own include directory) from $B/include and $d/include.
DEPINC="-I $CUTIL/include -I $SECURITY/include -I $COMPRESS/include"
LIBFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGHTTP_BUILD -I $HTTP/include $DEPINC -I $B/include"
# Each dependency is compiled with the flags ITS Makefile uses (cutil's: CFLAGS,
# security's: CFLAGS with -Wfloat-conversion and the aliasing warning, compress's:
# -O3 and -Wstrict-aliasing=2), so that a warning the library's own build would
# raise fails here too. HARDEN_CFLAGS (security) is not repeated: it is hardening
# for the shipped object, not diagnostics.
# cutil's gcu_file_is_directory (src/file.c:976) draws -Wmaybe-uninitialized from
# gcc 14 on the 32-bit targets (i686, powerpc) at -O2, a false positive on a stat
# out-parameter that cutil's own -Werror build would refuse there too. It is a
# cutil finding, not http's, so it is demoted to a warning for cutil's sources
# alone (printed in the log, which is kept) and nothing else is relaxed.
CUFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wno-error=maybe-uninitialized -Wno-error=unused-function -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD"
# security's sha1.c:155, sha256.c:174, md5.c:187, aes/gcm.c:52 and
# chacha20/chacha20_poly1305.c:326 compare a size_t with a 64-bit bound
# (`n > UINT64_MAX >> 3`), which gcc 14 under -Wextra reports as -Wtype-limits when
# size_t is 32 bits wide: security's own -Werror build does not compile on i686 and
# powerpc. The comparisons are correct and dead there; the warning is a library
# portability finding (reported, with a reproducer, to the parent), demoted to a
# warning here for security's sources alone so that the rest of the run can say
# what http does on a 32-bit target. Remove the demotion when security is fixed.
SEFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wno-error=type-limits -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wno-error=unused-function -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGSEC_BUILD"
# compress's lz4_frame.c:252 draws the same -Wtype-limits on 32-bit size_t (see SEFLAGS); demoted for compress alone, reported to the parent.
# compress: on s390x gcc 14 reports lzma_decode.c's `s.byte` as maybe-uninitialized
# (compress's own, unrelated to the deflate http uses): a warning in a dependency
# that this run does not gate on, as cutil's above.
COFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wno-error=maybe-uninitialized -Wno-error=type-limits -Wfatal-errors -std=c17 -O3 -g -Wstrict-aliasing=2 -fvisibility=hidden -DGCOMP_BUILD"
JOBS=${JOBS:-$(nproc)}
# The programs that read a corpus on stdin: name, corpus, output file.
probe_stdin() { case $1 in http_probe) echo "$CORPUS";; h2_probe) echo "$CORPUS2";; ws_probe) echo "$CORPUS3";; ws_h2_probe) echo "$CORPUS4";; esac; }
probe_out() { case $1 in http_probe) echo probe.out;; h2_probe) echo h2probe.out;; ws_probe) echo wsprobe.out;; ws_h2_probe) echo wsh2probe.out;; esac; }
WS_PROBE_SRC=$HTTP/tools/oracle/ws_probe.c
[ -f "$WS_PROBE_SRC" ] || { echo "http.sh: $WS_PROBE_SRC does not exist" >&2; exit 2; }
[ -s "$CORPUS3" ] || { echo "http.sh: no WebSocket corpus at $CORPUS3 (suite/tools/xarch/http-host.sh dumps it with diff_ws.py --dump)" >&2; exit 2; }
[ -s "$CORPUS4" ] || { echo "http.sh: no WebSocket-over-HTTP/2 corpus at $CORPUS4 (suite/tools/xarch/http-host.sh writes it)" >&2; exit 2; }
[ -f "$HTTP/tools/oracle/ws_h2_probe.c" ] || { echo "http.sh: ws_h2_probe.c does not exist" >&2; exit 2; }
PROBES="http_probe h2_probe ws_probe ws_h2_probe"

# compile_tree LIB FLAGS INCLUDES OBJDIR CC: every .c under the library's src/
# (the .template.c files are #included by others and not compiled alone), in
# parallel, each object named LIB-<path>.o. Returns non-zero if any failed.
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

# build_deps NAME CC [QEMU]: cutil, security and compress, whole, for one target
# -> $B/NAME/deps/*.o and $B/NAME/include/ghoti.io/cutil/float.h. cutil's float.h
# is generated by running the TARGET's float_identifier (under qemu when foreign),
# as cutil's Makefile does with the host's.
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
  compile_tree compress "$COFLAGS" "$inc" "$d/deps" "$cc" "$COMPRESS"
}

# build NAME CC [SRC-DIR] [DEPS-FROM]  ->  $B/NAME/{http-probe,h2-probe,ws-probe,http-write-probe}
# The dependency objects are those of target DEPS-FROM (default NAME): a planted
# build changes the library and nothing else, so it reuses its target's.
build() {
  local name=$1 cc=$2 srcdir=${3:-$HTTP/src} depsfrom=${4:-$1}
  local d=$B/$name
  mkdir -p "$d"
  local objs=""
  for f in "$srcdir"/core/*.c "$srcdir"/http/*.c; do
    local o=$d/$(basename "$(dirname "$f")")-$(basename "$f" .c).o
    $cc $LIBFLAGS -I "$srcdir/http" -I "$B/$depsfrom/include" -c "$f" -o "$o" || return 1
    objs="$objs $o"
  done
  objs="$objs $(ls "$B/$depsfrom"/deps/*.o)"
  # The probes are programs, not library sources: no -fvisibility, no BUILD.
  local pf="-std=c17 -O2 -g -Wall -Wextra -Werror -I $HTTP/include $DEPINC -I $B/$depsfrom/include -I $B/include"
  local p
  for p in $PROBES; do
    $cc $pf "$HTTP/tools/oracle/$p.c" $objs -pthread -lm -o "$d/$(echo $p | tr _ -)" || return 1
  done
  $cc $pf "$HERE/http-write-probe.c" $objs -pthread -lm -o "$d/http-write-probe" || return 1
}

# run NAME TRIPLE QEMU  ->  $B/NAME/{probe.out,h2probe.out,wsprobe.out,write.out}
run() {
  local name=$1 triple=$2 qemu=$3 rc1=0 p
  for p in $PROBES; do
    xarch_run "$triple" "$qemu" "$B/$name/$(echo $p | tr _ -)" < "$(probe_stdin $p)" \
      > "$B/$name/$(probe_out $p)" 2> "$B/$name/$p.err"
    rc1=$((rc1 + $?))
  done
  xarch_run "$triple" "$qemu" "$B/$name/http-write-probe" > "$B/$name/write.out" 2> "$B/$name/write.err"
  local rc2=$?
  [ $rc1 -eq 0 ] && [ $rc2 -eq 0 ]
}

# fail records itself in a file as well. The exit below reads that file, so a
# target that failed cannot end the run with PASS.
status=0
fail() { echo "FAIL: $*"; status=1; : > "$B/.failed"; }

lines=$(wc -l < "$CORPUS")
[ -s "$CORPUS2" ] || { echo "http.sh: no HTTP/2 corpus at $CORPUS2 (suite/tools/xarch/http-host.sh writes it)" >&2; exit 2; }
lines2=$(wc -l < "$CORPUS2")
lines3=0; [ -s "$CORPUS3" ] && lines3=$(wc -l < "$CORPUS3")
lines4=$(wc -l < "$CORPUS4")
corpus_lines() { case $1 in http_probe) echo $lines;; h2_probe) echo $lines2;; ws_probe) echo $lines3;; ws_h2_probe) echo $lines4;; esac; }
echo "== machinery"
build_deps aarch64-linux-gnu aarch64-linux-gnu-gcc qemu-aarch64 && build aarch64-linux-gnu aarch64-linux-gnu-gcc ||
  { echo "FAIL: cannot build for aarch64"; exit 1; }
elf=$(file -b "$B/aarch64-linux-gnu/http-probe")
case "$elf" in
  *aarch64*|*"ARM aarch64"*) echo "   ELF: $(echo "$elf" | cut -c1-70)" ;;
  *) fail "the aarch64 build is not an aarch64 binary: $elf" ;;
esac
if "$B/aarch64-linux-gnu/http-probe" < /dev/null > /dev/null 2>&1; then
  fail "the aarch64 binary ran WITHOUT qemu: it is not foreign, nothing here means anything"
else
  echo "   the aarch64 binary does not run natively (as it must not)"
fi
[ $status -eq 0 ] || exit 1
echo "   dependencies built whole for aarch64: $(ls "$B"/aarch64-linux-gnu/deps/*.o | wc -l) objects (cutil, security, compress)"

echo
echo "== the host (the control): $lines probe lines, $lines2 HTTP/2 lines, $lines3 WebSocket lines, $lines4 WebSocket-over-HTTP/2 lines"
build_deps x86_64-linux-gnu gcc && build x86_64-linux-gnu gcc || { echo "FAIL: cannot build for x86_64"; exit 1; }
run x86_64-linux-gnu x86_64-linux-gnu "" || { fail "the host's own run failed"; cat "$B/x86_64-linux-gnu/http_probe.err" "$B/x86_64-linux-gnu/write.out" | head; exit 1; }
for p in $PROBES; do
  want=$(corpus_lines $p); got=$(wc -l < "$B/x86_64-linux-gnu/$(probe_out $p)")
  [ "$got" -eq "$want" ] || { fail "the host's $p answered $got of $want lines"; exit 1; }
done
tail -3 "$B/x86_64-linux-gnu/write.out"

# same TARGET -> 0 when every output equals the host's; DIFFERS lists the ones that do not.
DIFFERS=""
same() {
  local t=$1 p o
  DIFFERS=""
  for p in $PROBES; do
    o=$(probe_out $p)
    cmp -s "$B/x86_64-linux-gnu/$o" "$B/$t/$o" || DIFFERS="$DIFFERS $o"
  done
  cmp -s <(grep -v '^sizeof' "$B/x86_64-linux-gnu/write.out") <(grep -v '^sizeof' "$B/$t/write.out") || DIFFERS="$DIFFERS write.out"
  [ -z "$DIFFERS" ]
}

one() {
  local triple="$1" cc="$2" qemu="$3" desc="$4"
  [ "$triple" = x86_64-linux-gnu ] && return
  printf "\n== %s (%s)\n" "$triple" "$desc"
  if ! { build_deps "$triple" "$cc" "$qemu" && build "$triple" "$cc"; }; then fail "$triple: cannot build"; return 1; fi
  if ! run "$triple" "$triple" "$qemu"; then
    fail "$triple: the run failed"; head -3 "$B/$triple/http_probe.err" "$B/$triple/write.out"; return 1
  fi
  if same "$triple"; then
    echo "   $lines probe lines, $lines2 HTTP/2 probe lines, $lines3 WebSocket probe lines, $lines4 WebSocket-over-HTTP/2 lines and the writer round trip are identical to the host's"
    grep '^sizeof' "$B/$triple/write.out"
  else
    local o
    for o in $DIFFERS; do
      fail "$triple: $o differs from the host's"
      diff <(cut -c1-300 "$B/x86_64-linux-gnu/$o") <(cut -c1-300 "$B/$triple/$o") | head -6
    done
  fi
}
xarch_each one
[ -e "$B/.failed" ] && status=1

# plant NAME LABEL FILE ANCHOR REPLACEMENT PROBE-OUTPUT: a copy of the library's
# sources with one line changed (the anchor must occur exactly once), built for
# aarch64 against the dependency objects already built, and the named probe
# output required to DIFFER from the host's.
plant() {
  local name=$1 label=$2 file=$3 anchor=$4 repl=$5 out=$6
  local P=$B/$name-src
  rm -rf "$P"; mkdir -p "$P"; cp -a "$HTTP/src/." "$P/"
  # The anchor is one line and must occur exactly once; the replacement is literal
  # (a quoted pattern in ${//}), the image having no python3.
  [ "$(grep -cF -- "$anchor" "$P/http/$file")" = 1 ] ||
    { fail "$label: the anchor is not in $file exactly once (stale patch): nothing was tested"; return; }
  local text
  text=$(cat "$P/http/$file"; echo x); text=${text%x}
  printf '%s' "${text/"$anchor"/"$repl"}" > "$P/http/$file"
  if cmp -s "$P/http/$file" "$HTTP/src/http/$file"; then
    fail "$label: the planted copy equals the original"
  elif ! build "$name" aarch64-linux-gnu-gcc "$P" aarch64-linux-gnu; then
    fail "$label: the planted build does not compile"
  elif ! run "$name" aarch64-linux-gnu qemu-aarch64; then
    fail "$label: the planted build did not run"
  elif cmp -s "$B/x86_64-linux-gnu/$out" "$B/$name/$out"; then
    fail "$label: the planted defect was NOT caught: the comparison cannot tell"
  else
    echo "   caught: $out differs from the host's"
  fi
}

echo
echo "== the control: a parser that refuses a bare LF with the wrong error, on aarch64"
plant planted "bare LF" parser.c 'return GHTTP_ERR_CORRUPT; // a bare LF' 'return GHTTP_ERR_LIMIT; // a bare LF' probe.out

echo
echo "== the control: an HTTP/2 connection that lets DATA through on an idle stream, on aarch64"
plant planted-h2 "idle DATA" h2conn.c 'return proto(c, "DATA on an idle stream");' 'return GHTTP_OK;' h2probe.out

# The WebSocket control: the corpus holds a client frame that arrives unmasked
# at a server, which is what the comparison has to see.
if true; then
  echo
  echo "== the control: a WebSocket server that accepts an unmasked client frame, on aarch64"
  plant planted-ws "unmasked" ws_conn.c 'if (c->role == GHTTP_WS_SERVER && !fh->masked) {' \
    'if (0 && c->role == GHTTP_WS_SERVER && !fh->masked) {' wsprobe.out
fi

# The WebSocket-over-HTTP/2 control: an adapter that never ends its side of the
# stream after the closing handshake changes the HTTP/2 bytes (no END_STREAM) and
# so the digest the probe prints, though every message still arrives.
echo
echo "== the control: a WebSocket-over-HTTP/2 adapter that never ends the stream after a Close, on aarch64"
plant planted-wsh2 "no END_STREAM" ws_h2.c 'if (st == GHTTP_WS_STATE_CLOSED || st == GHTTP_WS_STATE_FAILED || a->remote_ended) {' \
  'if (st == GHTTP_WS_STATE_FAILED || a->remote_ended) {' wsh2probe.out
[ -e "$B/.failed" ] && status=1

# For the host script to compare with the probes `make check-oracle` runs.
[ -d /out ] && cp "$B/x86_64-linux-gnu/probe.out" "$B/x86_64-linux-gnu/write.out" "$B/x86_64-linux-gnu/h2probe.out" /out/
[ -d /out ] && [ -f "$B/x86_64-linux-gnu/wsprobe.out" ] && cp "$B/x86_64-linux-gnu/wsprobe.out" /out/
[ -d /out ] && [ -f "$B/x86_64-linux-gnu/wsh2probe.out" ] && cp "$B/x86_64-linux-gnu/wsh2probe.out" /out/

echo
[ $status -eq 0 ] && echo "PASS: every target matches the host, and the planted defects are caught" || echo "FAIL"
exit $status
