#!/bin/bash
# Cross-compiles cutil, security, certificate and tls for win64 with their own
# Makefiles and runs tls's unit tests under wine, with controls that must
# fail (libs/tls, planning/tls.md steps 3 and 4).
# Host side: needs podman, rsync, wine and googletest sources.
#
#   suite/tools/xwin/tls.sh [scratch-dir]      (default /tmp/xwin-tls)
#
# All four libraries are taken as COMMITTED (git archive HEAD), not as the
# working tree stands: another session may be mid-edit in one of them, and what
# is being tested is tls against certificate, security and cutil as they are.
#
# What a green run means: the Windows arms of the four Makefiles build the
# libraries and tls's tests with the real Windows headers; the DLL carries the
# dllexport arm of GTLS_API; every unit test, the RFC 8448 known answers and
# the loopback of a real client against a real server among them, passes under
# wine, and so do RFC 8448's resumed trace and a real client and server resuming
# a ticket; and RFC 8448's 0-RTT trace and early data through a real client and
# server pass too; and a planted defect in the key schedule, another in the record
# nonce, a third in the PSK binder and a fourth in the early secret's label are each caught by the same run. It does not mean the same on a Windows machine: wine
# is not Windows (see README.md).
# Exit status is 0 only if every step held and the planted defect was caught.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${1:-/tmp/xwin-tls}

for need in podman; do
  command -v $need >/dev/null || { echo "tls.sh: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "tls.sh: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "tls.sh: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }

mkdir -p "$W/logs"
for lib in cutil security certificate tls; do
  rm -rf "$W/$lib" && mkdir -p "$W/$lib" &&
    git -C "$ROOT/libs/$lib" archive HEAD | tar -x -C "$W/$lib" || exit 2
done
"$HERE/m1-win.sh" "$W" "bash /tools/xwin/tls-win.sh"
