#!/bin/bash
# Cross-compiles cutil and libghttp for win64 with their own Makefiles, runs the
# unit tests under wine, and runs the differential's probe and a writer round
# trip over the same corpus the Linux gate uses, requiring byte-identical
# answers (libs/http, story 3 of the Defiant milestone, notes/http/README.md).
# Host side: needs podman, rsync, wine, googletest sources and python3.
#
#   suite/tools/xwin/http.sh [scratch-dir]            (default /tmp/xwin-http)
#
# It first runs suite/tools/xarch/http-host.sh into <scratch-dir>/ref, which writes the
# corpus and the answers a Linux x86-64 build gives (and checks them against the
# Makefile's own probe), so the comparison is with the build the oracle gate
# judges, not with whatever this script happens to compile.
#
# libghttp now links security (SHA-1 and BCryptGenRandom, for the WebSocket
# handshake and the client's masking keys) and compress (deflate, for
# permessage-deflate), so both are cross-built here with their own Makefiles'
# Windows arms, in dependency order, ahead of libghttp (http-win.sh).
#
# What a green run means: the Windows arms of http's Makefile build the library,
# its tests and the probe with the real Windows headers; every unit test passes
# under wine; and the parse of ~5,000 messages, whole and a byte at a time, and
# the writer's bytes are the same as on Linux. It does not mean the same on a
# Windows machine: wine is not Windows (see README.md). Exit status is 0 only if
# every step held and the planted defect was caught.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${1:-/tmp/xwin-http}

for need in podman rsync python3; do
  command -v $need >/dev/null || { echo "http.sh: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "http.sh: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "http.sh: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }

mkdir -p "$W/logs" "$W/ref"
"$HERE/../xarch/http-host.sh" "$W/ref" > "$W/logs/xarch-reference.log" 2>&1
rc=$?
# Exit 3 from the reference run is "everything held except that the WebSocket
# differential could not run" (no ws_probe.c / corpus yet). That is carried through
# and reported at the end, loudly; anything else non-zero is fatal.
if [ $rc -ne 0 ] && [ $rc -ne 3 ]; then
  tail -15 "$W/logs/xarch-reference.log" >&2; echo "http.sh: the Linux reference run failed" >&2; exit 2
fi
for f in corpus.tsv corpus_h2.tsv out/probe.out out/write.out out/h2probe.out; do
  [ -s "$W/ref/$f" ] || { echo "http.sh: the reference run left no $f" >&2; exit 2; }
done
tail -2 "$W/logs/xarch-reference.log"
# The WebSocket differential on Windows needs the corpus and the host's answers
# the reference run leaves; their absence is an error.
for f in corpus_ws.tsv out/wsprobe.out; do
  [ -s "$W/ref/$f" ] || { echo "http.sh: the reference run left no $f" >&2; exit 2; }
done

# The host's test counts, from a host build of the same tree: the run under wine
# must pass exactly this many tests in each test binary.
"$HERE/../xarch/http-host-counts.sh" "$W/ref/counts.txt" > "$W/logs/host-counts.log" 2>&1 ||
  { tail -15 "$W/logs/host-counts.log" >&2; echo "http.sh: the host build for the test counts failed" >&2; exit 2; }
tail -1 "$W/logs/host-counts.log"

# cutil, security and compress as COMMITTED, not as the working trees stand:
# another session may be mid-edit in one of them, and what is being tested here is
# http against them as they are, not against someone's half-written file.
for l in cutil security compress; do
  rm -rf "$W/$l" && mkdir -p "$W/$l" && git -C "$ROOT/libs/$l" archive HEAD | tar -x -C "$W/$l" || exit 2
done
rsync -a --exclude build --exclude .git --exclude tests/fuzz/corpus "$ROOT/libs/http/" "$W/http/" || exit 2
cp "$HERE/../xarch/http-write-probe.c" "$W/http-write-probe.c" || exit 2
"$HERE/m1-win.sh" "$W" 'bash /tools/xwin/http-win.sh'
exit $?
