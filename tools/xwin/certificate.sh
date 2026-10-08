#!/bin/bash
# Cross-compiles cutil, security and certificate for win64 with their own
# Makefiles and runs certificate's unit tests under wine, with a control that
# must fail (libs/certificate, planning/tls.md).
# Host side: needs podman, rsync, wine and googletest sources.
#
#   suite/tools/xwin/certificate.sh [scratch-dir]      (default /tmp/xwin-certificate)
#
# All three libraries are taken as COMMITTED (git archive HEAD), not as the
# working tree stands: another session may be mid-edit in one of them, and what
# is being tested is certificate against security and cutil as they are.
#
# What a green run means: the Windows arms of certificate's and security's
# Makefiles build the libraries and certificate's tests with the real Windows
# headers; the DLL carries the dllexport arm of GCERT_API; every unit test
# passes under wine; and a planted defect in the conversion between the two
# result enums is caught by the same run. It does not mean the same on a
# Windows machine: wine is not Windows (see README.md). Set WITH_SECURITY=1 to
# run security's own tests under wine as well, which takes several minutes.
# Exit status is 0 only if every step held and the planted defect was caught.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${1:-/tmp/xwin-certificate}

for need in podman; do
  command -v $need >/dev/null || { echo "certificate.sh: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "certificate.sh: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "certificate.sh: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }

mkdir -p "$W/logs"
for lib in cutil security certificate; do
  rm -rf "$W/$lib" && mkdir -p "$W/$lib" &&
    git -C "$ROOT/libs/$lib" archive HEAD | tar -x -C "$W/$lib" || exit 2
done
"$HERE/m1-win.sh" "$W" "WITH_SECURITY=${WITH_SECURITY:-0} bash /tools/xwin/certificate-win.sh"
